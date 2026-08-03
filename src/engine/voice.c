/* voice.c -- voice pool (48 primary + 6 reserve, SPEC.adoc S5.2/S5.5), stealing, key-group choke, per-voice pitch/envelope/gain computation (SPEC.adoc Parts 3 & 5). Known simplifications vs. SPEC.adoc are tracked in SPEC_LOG.adoc items 7 and 8. */
#include "voice.h"
#include "synth.h"
#include "tables.h"
#include "rt.h"

Voice g_voices[NUM_VOICES];
uint32_t g_voice_age_counter;

/* TOPUP_RESERVE_COUNT: SPEC.adoc S5.5's 48+6=54 split; -D0 collapses to a flat pool, see SPEC_LOG entry11 */
#ifndef TOPUP_RESERVE_COUNT
#define TOPUP_RESERVE_COUNT 6
#endif
#define NUM_RESERVE TOPUP_RESERVE_COUNT
#define NUM_PRIMARY (NUM_VOICES - NUM_RESERVE)

static uint32_t g_topup_accum = 0; /* top-up tick clock, see voice_topup_tick */

void voice_pool_reset(void) {
    g_topup_accum = 0;
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voices[i];
        v->active = 0;
        v->channel = -1;
        v->note = -1;
        v->locale = 0;
        v->key_group = 0;
        v->wave = 0;
        v->artic = 0;
        v->phase_pos = 0;
        v->phase_step = 0;
        v->phase_step_target = 0;
        v->phase_step_ramp_step = 0;
        v->phase_step_ramp_acc = 0;
        v->ramp_left = 0;
        v->base_cents = 0;
        v->base_ratio_q12 = 4096; /* unity */
        v->loop_start_s = v->loop_len_s = v->sample_end_s = 0;
        v->gain_l = v->gain_r = 0.0;
        v->gain_l_target = v->gain_r_target = 0.0;
        v->amp_left = 0;
        v->amp_l = v->amp_r = 0.0;
        v->amp_step_l = v->amp_step_r = 0.0;
        v->amp_retire = 0;
        v->start_delay = 0;
        v->atten_const_hdb = 0;
        v->env_stage = ENV_IDLE;
        v->env_level = 0.0;
        v->env_decay_samples_left = 0;
        v->eg2_stage = ENV_IDLE;
        v->eg2_level = 0.0;
        v->eg2_depth_cents = 0.0;
        v->held = 0;
        v->sustain_deferred = 0;
        v->age = 0;
        v->lfo_phase = 0.0;
        v->lfo_freq_hz = 0.0;
        v->lfo_delay_s = 0.0;
        v->lfo_elapsed_s = 0.0;
        /* Reserve-tier starting index is cosmetic/[O]; nothing downstream distinguishes tier once active -- SPEC_LOG item54 */
        v->in_reserve = (i >= NUM_PRIMARY) ? 1 : 0;
        v->fast_release_committed = 0;
    }
    g_voice_age_counter = 0;
}

/* +-4800 clamp left as-is after RPN1/RPN2 added; also applied to EG2's pitch contribution -- SPEC_LOG item54 */
#define CENTS_CLAMP 4800

static int32_t cents_to_ratio_q12(int32_t cents) {
    if (cents > CENTS_CLAMP) cents = CENTS_CLAMP;
    if (cents < -CENTS_CLAMP) cents = -CENTS_CLAMP;
    if (cents >= -100 && cents <= 100) {
        return g_table_cents[cents + 100];
    }
    int32_t whole = cents / 100;
    int32_t rem_cents = cents % 100;
    int32_t octaves = whole / 12;
    int32_t rem_semi = whole % 12;
    int64_t t2 = g_table_cents[rem_cents + 100];
    int64_t t3 = g_table_semi[rem_semi + 48];
    int64_t scaled_t2 = (octaves >= 0) ? (t2 << octaves) : (t2 >> (-octaves));
    return (int32_t)((t3 * scaled_t2) >> 12);
}

/* SPEC.adoc S3.4.1: tc = lScale/65536.0; duration = 2^(tc/1200), trunc toward zero (kept as a plain double here, feeding a continuous-time coefficient, not an integer table). */
/* Where inside the current render call the event being dispatched actually falls; smf.c sets it per event, direct msgs_midi injection leaves it at 0 ("now"). Only note-on reads it -- see Voice::start_delay. */
static uint32_t g_event_offset = 0;

void voice_set_event_offset(uint32_t frames) { g_event_offset = frames; }

static double timecents_to_seconds(int32_t tc) {
    if (tc == (int32_t)0x80000000) return 0.0;
    double t = (double)tc / 65536.0;
    return rt_pow(2.0, t / 1200.0);
}

/* SPEC.adoc S2.4.3/S5.1.2 [A:0x198e1],[A:0x1991a]: /127 divisor confirmed, not /128 — see SPEC_LOG item27 */
static int32_t scale_tc_by_source(int32_t tc, int16_t depth, int src) {
    if (tc == (int32_t)0x80000000 || depth == 0) return tc;
    int32_t cents = (int32_t)depth * (int32_t)src / 127;
    if (cents > 4800) cents = 4800;
    if (cents < -4800) cents = -4800;
    return tc + cents * 65536;
}

/* LFO rate, SPEC.adoc LFO section [M: probe 06]: freq = 8.176 * 2^(tc/1200), per-instrument via lfo_freq_tc, see SPEC_LOG entry6 */
static double lfo_freq_from_tc(int32_t tc) {
    if (tc == (int32_t)0x80000000) return 8.176;
    double t = (double)tc / 65536.0;
    return 8.176 * rt_pow(2.0, t / 1200.0);
}

/* exp_coef: "100dB-over-seconds" release/decay calibration, see SPEC_LOG (line 5320). ponytail: external-convention fit, not byte-confirmed; upgrade path = locate real EG consumption code (+0x13c/0x194da/0x19644, still [O]). */
/* DECAY_RATE_MULT removed (not merely unused), SPEC.adoc S5.1.2.1 pinned-endpoint geometric ramp -- SPEC_LOG item54 (addendum to entry15) */
#ifndef RELEASE_RATE_MULT
#define RELEASE_RATE_MULT 1.0
#endif
/* RELEASE_FLOOR_S=0.060: fitted floor on ordinary note-off release, corpus + isolate-capture sweep, see SPEC_LOG entry10 */
#ifndef RELEASE_FLOOR_S
#define RELEASE_FLOOR_S 0.060
#endif

/* FAST_RELEASE_S = 1/70: `0x19834` divides the SAMPLE RATE, not milliseconds (315 samples = 14.29 ms), see SPEC_LOG item29 (derivation) and item21-reap (-24.00 vs -17.25 topup effect). */
#ifndef FAST_RELEASE_S
#define FAST_RELEASE_S (1.0 / 70.0)
#endif

/* AUDIBLE_FLOOR=1e-4, -74dB below full scale; [I] reading of +0x13c as gain-inclusive, see SPEC_LOG entry16 */
#ifndef AUDIBLE_FLOOR
#define AUDIBLE_FLOOR 0.0001
#endif

/* ENV_SPAN_DB=96.0, SPEC.adoc S3.4.2 [A:0x1685e]: EG runs on a 96dB scale, not 100; governs decay and release alike, see SPEC_LOG item26 */
#define ENV_SPAN_DB 96.0

/* ENV_ATTACK_TABLE_C: quantized Table-C attack A/B measured, not shipped (default OFF), see SPEC_LOG item46 */
#ifndef ENV_ATTACK_TABLE_C
#define ENV_ATTACK_TABLE_C 0
#endif

/* exp_coef_scaled mult=1.0 re-verified 2026-07-25 under the corrected harness aligner -- SPEC_LOG item54 (addendum to entry1/entry15) */
static double exp_coef_scaled(double seconds, double mult) {
    if (seconds <= 0.0) return 0.0;
    double samples = seconds * (double)RENDER_RATE;
    if (samples < 1.0) samples = 1.0;
    return rt_pow(10.0, (-(ENV_SPAN_DB / 20.0) * mult) / samples);
}


#define EG2_BLOCK_FRAMES LFO_UPDATE_FRAMES
#define EG2_BLOCK_RATE ((double)RENDER_RATE / (double)EG2_BLOCK_FRAMES)

/* EG2_LINEAR_SEGMENTS: pitch EG is linear-in-cents not exponential (spotted on probe 34), see SPEC_LOG item20 */
#ifndef EG2_LINEAR_SEGMENTS
#define EG2_LINEAR_SEGMENTS 1
#endif

/* "Close enough" threshold for snapping a decaying/releasing envelope level to its target (sustain level or zero) instead of asymptoting forever. */
#define ENV_SNAP_EPSILON 0.0005

static double eg2_coef(double seconds) {
    if (seconds <= 0.0) return 0.0;
    double blocks = seconds * EG2_BLOCK_RATE;
    if (blocks < 1.0) blocks = 1.0;
#if EG2_LINEAR_SEGMENTS
    return 1.0 / blocks;                  /* linear: fraction of full span per block */
#else
    return rt_pow(10.0, -5.0 / blocks);   /* EG1's exponential calibration */
#endif
}

static int32_t voice_step_eg2(Voice *v) {
    if (v->eg2_depth_cents == 0.0) return 0;
    switch (v->eg2_stage) {
        case ENV_ATTACK:
            v->eg2_level += v->eg2_attack_step;
            if (v->eg2_level >= 1.0) {
                v->eg2_level = 1.0;
                v->eg2_stage = (v->eg2_decay_coef > 0.0 && v->eg2_decay_coef < 1.0) ? ENV_DECAY : ENV_SUSTAIN;
            }
            break;
        case ENV_DECAY:
#if EG2_LINEAR_SEGMENTS
            v->eg2_level -= v->eg2_decay_coef * (1.0 - v->eg2_sustain_level);
#else
            v->eg2_level = v->eg2_sustain_level + (v->eg2_level - v->eg2_sustain_level) * v->eg2_decay_coef;
#endif
            if (v->eg2_level - v->eg2_sustain_level < ENV_SNAP_EPSILON) {
                v->eg2_level = v->eg2_sustain_level;
                v->eg2_stage = ENV_SUSTAIN;
            }
            break;
        case ENV_SUSTAIN:
            v->eg2_level = v->eg2_sustain_level;
            break;
        case ENV_RELEASE:
#if EG2_LINEAR_SEGMENTS
            v->eg2_level -= v->eg2_release_coef;
#else
            v->eg2_level *= v->eg2_release_coef;
#endif
            if (v->eg2_level < ENV_SNAP_EPSILON) { v->eg2_level = 0.0; v->eg2_stage = ENV_IDLE; }
            break;
        case ENV_IDLE:
        default:
            return 0;
    }
    double c = v->eg2_depth_cents * v->eg2_level;
    if (c > (double)CENTS_CLAMP) c = (double)CENTS_CLAMP;
    if (c < -(double)CENTS_CLAMP) c = -(double)CENTS_CLAMP;
    return (int32_t)c;
}

static int32_t voice_lfo_cents(Voice *v) {
    if (!v->artic) return 0;
    int32_t cc1_depth = v->artic->lfo_pitch_cc1_cents;
    if (cc1_depth == 0) return 0;
    double cc1 = (double)g_channels[v->channel].modulation / 127.0;
    double depth_cents = (double)cc1_depth * cc1;
    int idx = ((int)(v->lfo_phase * 256.0)) & 255;
    double lfo_unit = (double)g_table_sine[idx] / 100.0;
    return (int32_t)(depth_cents * lfo_unit);
}

/* [F:fitted] RAMP_HORIZON_FRAMES=320 (14.5ms); probe 32/33 sweep + open rise/fall question -- SPEC_LOG entry17. ponytail: one constant, no ceiling, no magnitude keying, no per-source exemption -- the measurement says duration does not depend on step size, so nothing here should either. */
#ifndef RAMP_HORIZON_FRAMES
#define RAMP_HORIZON_FRAMES (320 * RESAMPLE_FACTOR)
#endif

/* Sizes the held slope so phase_step reaches phase_step_target in exactly `frames` samples; truncation residue is bounded, not corrected, since a zero slope snaps immediately instead of crawling. */
static void ramp_reaim(Voice *v, uint32_t frames) {
    int64_t d = (int64_t)v->phase_step_target - (int64_t)v->phase_step;
    int32_t slope = (int32_t)((d << 8) / (int64_t)frames); /* Q8 per sample */
    if (slope == 0) {
        v->phase_step = v->phase_step_target;
        v->phase_step_ramp_acc = 0;
        v->ramp_left = 0;
        v->phase_step_ramp_step = 0;
        return;
    }
    v->phase_step_ramp_step = slope;
    v->ramp_left = frames;
}

/* render.c's accumulator has no arrival test (SPEC.adoc S6.6); this counts every active voice's ramp down and snaps it exactly on expiry, called AFTER render_voice so the expiring chunk is still rendered with it first. */
void voice_ramp_tick(uint32_t frames) {
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voices[i];
        if (!v->active || !v->ramp_left) continue;
        if (v->ramp_left > frames) { v->ramp_left -= frames; continue; }
        v->ramp_left = 0;
        v->phase_step = v->phase_step_target;
        v->phase_step_ramp_step = 0;
        v->phase_step_ramp_acc = 0;
    }
}

void voice_update_pitch(Voice *v) {
    if (!v->active || !v->wave) return;
    /* Two-factor pitch: base (note-on, clamped) vs live bend/LFO/EG2 (clamped separately), SPEC.adoc S3.3.2/S3.3.3, settled by probe 30, see SPEC_LOG item16/item17 */
    int32_t bend_cents = synth_pitch_bend_cents(v->channel); /* SPEC.adoc S4.4 */
    int32_t mod_cents = bend_cents
                      + voice_lfo_cents(v) /* SPEC.adoc LFO section, `[M: probe 06]` */
                      + voice_step_eg2(v); /* SPEC.adoc S2.4.3 `[A:0x15838]`, SPEC_LOG.adoc #20 */
    uint64_t raw = (uint64_t)v->wave->sample_rate * (uint64_t)v->base_ratio_q12;
    raw = (raw * (uint64_t)(uint32_t)cents_to_ratio_q12(mod_cents)) >> 12;
    uint32_t new_target = (uint32_t)(raw / RENDER_RATE);

    /* Only bend is ramped; routing LFO/EG2 through it regressed 06_modwheel/26_other_gains -- SPEC_LOG item52 */
    int bend_moved = (bend_cents != v->bend_cents_applied);
    if (!bend_moved) {
        int64_t moved = (int64_t)v->phase_step
                      + ((int64_t)new_target - (int64_t)v->phase_step_target);
        v->phase_step = (uint32_t)(moved < 0 ? 0 : moved);
    }
    v->bend_cents_applied = bend_cents;
    v->phase_step_target = new_target;

    /* Bend ramp re-aimed instantly on target move, over a fixed horizon, not a shared clock's remainder -- SPEC_LOG item52 */
    if (bend_moved) ramp_reaim(v, RAMP_HORIZON_FRAMES);
}

/* Per-voice LFO phase advance, SPEC.adoc LFO section [M: probe 06]; sub-chunked so a held note with no MIDI traffic still oscillates, see SPEC_LOG entry6 */
void voices_advance_lfo(uint32_t frames) {
    double dt = (double)frames / (double)RENDER_RATE;
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voices[i];
        if (!v->active) continue;
        v->lfo_elapsed_s += dt;
        if (v->lfo_elapsed_s <= v->lfo_delay_s) continue; /* still in start-delay: phase frozen at 0 */
        v->lfo_phase += v->lfo_freq_hz * dt;
        v->lfo_phase -= (double)(int64_t)v->lfo_phase; /* wrap to [0,1); phase is always >= 0 */
    }
}

/* GAIN_CEILING = 4095/8192, SPEC.adoc S6.4.5: the table's own zero point IS the per-voice ceiling, see SPEC_LOG item47 */
#define GAIN_CEILING (4095.0 / 8192.0)

/* Gain law, SPEC.adoc S3.5/S3.10/S3.6: re-read live every block (mirrors voice_update_pitch) so CC7/CC10/CC11/master-volume changes reach a held note; atten_const_hdb (velocity+region attenuation) is fixed at note-on. */
/* Pan law, SPEC.adoc S3.6 [A:0x19c12-0x19c2a]: one g_table_lin read forward/reversed, summed into attenuation before the S6.4.5 clamp, see SPEC_LOG item50/item49/item47 */
static double atten_to_gain(int32_t atten_hdb) {
    int32_t i = atten_hdb / 10;          /* 0x18e0b: signed index, 0.1dB steps */
    if (i > 0) i = 0;                    /* 0x18dea: clamp(.., -1000, 0) */
    if (i < -1000) i = -1000;
    return (double)g_table_dbamp[i + 1000] / 8192.0;
}

void voice_update_gain(Voice *v) {
    if (!v->active) return;
    int32_t chan_vol = g_table_vel[g_channels[v->channel].volume];
    int32_t expr = g_table_vel[g_channels[v->channel].expression];
    int32_t atten_hdb = g_master_vol_hdb + chan_vol + expr + v->atten_const_hdb;

    int pan = (int)g_channels[v->channel].pan + v->artic->pan_cb;
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;
    v->gain_l_target = atten_to_gain(atten_hdb + g_table_lin[127 - pan]);
    v->gain_r_target = atten_to_gain(atten_hdb + g_table_lin[pan]);
}

void voices_update_modulation(void) {
    for (int i = 0; i < NUM_VOICES; i++) {
        if (g_voices[i].active) {
            voice_update_pitch(&g_voices[i]);
            voice_update_gain(&g_voices[i]);
        }
    }
}

static Voice *find_free_primary(void) {
    for (int i = 0; i < NUM_VOICES; i++) {
        if (!g_voices[i].active && !g_voices[i].in_reserve) return &g_voices[i];
    }
    return 0;
}

static Voice *find_free_reserve(void) {
    for (int i = 0; i < NUM_VOICES; i++) {
        if (!g_voices[i].active && g_voices[i].in_reserve) return &g_voices[i];
    }
    return 0;
}

/* SPEC.adoc S5.7 0x12426 analogue -- top-up's own Branch B comparator only, see SPEC_LOG item7 */
static Voice *find_steal_candidate_symmetric(void) {
    Voice *best = 0;
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *cand = &g_voices[i];
        if (!cand->active || cand->fast_release_committed) continue;
        if (!best) { best = cand; continue; }
        int cand_released = !cand->held;
        int best_released = !best->held;
        if (cand_released != best_released) {
            if (cand_released) best = cand;
            continue;
        }
        if (cand_released) {
            if (cand->env_level < best->env_level) best = cand;
            continue;
        }
        if (cand->age < best->age) best = cand;
    }
    return best;
}

/* SPEC.adoc S5.7 0x124a8 analogue -- note-on's forced-steal fallback only, deliberately asymmetric/ungated, see SPEC_LOG item7 */
static Voice *find_steal_candidate_asymmetric(void) {
    Voice *best = 0;
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *cand = &g_voices[i];
        if (!cand->active) continue;
        if (!best) { best = cand; continue; }
        if (cand->held) {
            if (cand->age < best->age) best = cand;
        } else if (best->held) {
            best = cand;
        } else if (cand->env_level < best->env_level) {
            best = cand;
        }
    }
    return best;
}

static void start_release(Voice *v, int fast) {
    if (fast) v->fast_release_committed = 1; /* start_release: +0x138 set unconditionally per SPEC.adoc S5.6/item30 (0x19ab6); this engine already collapses ScheduleFastRelease's two internal paths into one */
    v->held = 0;
    v->sustain_deferred = 0;
    v->env_stage = ENV_RELEASE;
    double authored_s = timecents_to_seconds(v->artic->eg1_release_tc);
    double rel_s = authored_s;
    if (fast) {
        double lvl = 1.0 + 20.0 * rt_log10(v->env_level > 1e-12 ? v->env_level : 1e-12) / ENV_SPAN_DB;
        if (lvl > 1.0) lvl = 1.0;
        rel_s = (lvl > 1e-6) ? FAST_RELEASE_S / lvl : authored_s;
        if (rel_s > authored_s) rel_s = authored_s;
    }
    if (!fast && rel_s < RELEASE_FLOOR_S) rel_s = RELEASE_FLOOR_S;
    v->env_release_coef = exp_coef_scaled(rel_s, RELEASE_RATE_MULT);
    /* EG2 releases on both note-off paths -- the 70ms clamp is amplitude-only (SPEC.adoc Part 7), see SPEC_LOG item20 */
    if (v->eg2_stage != ENV_IDLE) v->eg2_stage = ENV_RELEASE;
}

/* topup_reserve cadence: SPEC.adoc S5.4 [A] mechanism (Branch A/B); period is [F:fitted] TOPUP_INTERVAL_FRAMES, a real wall-clock tick -- see SPEC_LOG item7/entry14/item21-reap for the derivation and open residual. */
#ifndef TOPUP_INTERVAL_FRAMES
#define TOPUP_INTERVAL_FRAMES (2048 * RESAMPLE_FACTOR) /* ~92.9ms */
#endif

static void topup_reserve(void) {
    int reserve_free = 0;
    for (int i = 0; i < NUM_VOICES; i++) {
        if (!g_voices[i].active && g_voices[i].in_reserve) reserve_free++;
    }
    int need = NUM_RESERVE - reserve_free;
    if (need <= 0) return;

    for (int i = 0; i < NUM_VOICES && need > 0; i++) {
        Voice *v = &g_voices[i];
        if (!v->active && !v->in_reserve) { v->in_reserve = 1; need--; }
    }

    while (need > 0) {
        Voice *victim = find_steal_candidate_symmetric();
        if (!victim) break; /* every active voice already committed to a fast release -- nothing left to mark this call; NOT a leak (see voice_note_on's own final fallback, which never reads this gate and can still repurpose a draining voice immediately). */
        start_release(victim, 1);
        need--;
    }
}

void voice_topup_tick(uint32_t frames) {
    g_topup_accum += frames;
    if (g_topup_accum < TOPUP_INTERVAL_FRAMES) return;
    g_topup_accum %= TOPUP_INTERVAL_FRAMES; /* no catch-up burst: one top-up
        per tick however coarsely render.c happens to slice its chunks. */
    topup_reserve();
}

void voice_note_on(int channel, int note, int velocity) {
    if (channel < 0 || channel >= 16) return;
    if (note < 0 || note > 127) note = note < 0 ? 0 : 127;
    if (velocity <= 0) { voice_note_off(channel, note); return; }
    if (velocity > 127) velocity = 127;

    uint32_t locale = synth_channel_locale(channel);
    Region *r = dls_find_region(locale, (uint8_t)note);
    if (!r) return; /* SPEC.adoc S3.1.2/S3.2.2: silently dropped */

    /* Key-group choke, SPEC.adoc S3.8/S5.8: same channel + key group + locale. */
    if (r->key_group != 0) {
        for (int i = 0; i < NUM_VOICES; i++) {
            Voice *v = &g_voices[i];
            if (v->active && v->channel == channel && v->key_group == r->key_group && v->locale == locale) {
                start_release(v, 1);
            }
        }
    }

    /* Same-note retrigger, SPEC.adoc S5.6 (0x12ec6): fast-releases a prior voice on the same (channel, note) before the new voice is set up, so it can never match the note being triggered here. */
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voices[i];
        if (v->active && v->channel == channel && v->note == note) {
            start_release(v, 1);
        }
    }

    /* SPEC.adoc S5.3: primary before reserve, forced steal (asymmetric comparator, S5.7) only if both free lists are empty; always finds a victim since note_on is never reached with zero active voices. */
    Voice *v = find_free_primary();
    if (!v) v = find_free_reserve();
    if (!v) v = find_steal_candidate_asymmetric();
    if (!v) return;

    v->in_reserve = 0;              /* SPEC.adoc S5.3: recycling always targets primary only -- reset here so whatever tier this voice occupied while free (or, for a stolen voice, whatever it was tagged before going active) is irrelevant the moment it's active again. */
    v->fast_release_committed = 0;  /* SPEC.adoc S5.6: cleared only by SetupNote */

    v->active = 1;
    v->channel = channel;
    v->note = note;
    v->locale = locale;
    v->key_group = r->key_group;
    v->wave = r->wave;
    v->artic = r->artic;
    v->age = ++g_voice_age_counter;
    v->held = 1;
    v->sustain_deferred = 0;

    /* SPEC.adoc S3.3.2/S4.4: base_cents excludes bend (re-read live); RPN2 skipped for rhythm parts, RPN1 not gated, see SPEC_LOG s3-5-pitch-cents-correction */
    v->base_cents = (int)r->fine_tune + (note - (int)r->unity_note) * 100
                  + (g_channels[channel].is_rhythm ? 0 : g_channels[channel].rpn2_coarse_cents)
                  + g_channels[channel].rpn1_fine_cents;
    /* Latched here and clamped once, `[M: probe 30]` -- see voice_update_pitch. */
    v->base_ratio_q12 = (uint32_t)cents_to_ratio_q12(v->base_cents);

    /* Pitch-LFO state, SPEC.adoc LFO section `[M: probe 06]`: rate/delay cached once from this voice's own artic; phase starts at 0, advanced only by voices_advance_lfo. */
    v->lfo_phase = 0.0;
    v->lfo_elapsed_s = 0.0;
    v->lfo_freq_hz = lfo_freq_from_tc(r->artic->lfo_freq_tc);
    v->lfo_delay_s = timecents_to_seconds(r->artic->lfo_delay_tc);

    voice_update_pitch(v);
    /* New note: plays at the true target from sample 0, no glide-in from whatever phase_step this recycled Voice slot last held -- the ramp is for modulation on an already-sounding voice (SPEC.adoc S6.6), not note-on. */
    v->phase_step = v->phase_step_target;
    v->phase_step_ramp_step = 0;
    v->phase_step_ramp_acc = 0;
    v->ramp_left = 0;
    v->bend_cents_applied = synth_pitch_bend_cents(channel);
    v->phase_pos = 0;

    int no_loop = r->no_loop;
    if (no_loop) {
        v->loop_start_s = 0;
        v->loop_len_s = 0;
        v->sample_end_s = v->wave->sample_count;
    } else {
        v->loop_start_s = r->loop_start;
        v->sample_end_s = r->loop_end;
        v->loop_len_s = r->loop_end - r->loop_start;
        if (v->loop_len_s <= 0) { v->loop_len_s = 0; v->sample_end_s = v->wave->sample_count; }
    }

    /* Volume law, SPEC.adoc S3.5: velocity/region attenuation fixed at note-on into atten_const_hdb; CC7/CC11/CC10/master-vol recomputed live by voice_update_gain, see SPEC_LOG (velocity-depth sentinel item, line 4838) */
    int32_t vel_atten = g_table_vel[velocity];
    int32_t depth = r->artic->vel_to_atten_depth;
    int32_t scaled = (int32_t)(((int64_t)vel_atten * depth) / -9600);
    /* Region's own wsmp attenuation only, not a separate wave-level term -- "region overrides wave", see SPEC_LOG (gain double-counting item, line 5395) */
    /* SPEC.adoc S3.5 [A:0x19bcd]: +1200 folded into the sum moves the S6.4.5 clamp knee 12dB earlier, not a flat boost, see SPEC_LOG item47 */
    v->atten_const_hdb = scaled + (int32_t)r->attenuation_hdb + 1200;

    /* Pan, SPEC.adoc S3.6: region's pan_cb fixed, channel CC10 combined live in voice_update_gain; L/R assignment settled, see SPEC_LOG item9 */
    voice_update_gain(v);
    v->gain_l = v->gain_l_target;
    v->gain_r = v->gain_r_target;
    v->amp_left = 0;
    v->amp_step_l = v->amp_step_r = 0.0;
    v->amp_retire = 0;
    v->start_delay = g_event_offset;
    /* amp_l/amp_r primed from envelope start level, matches driver's nsamples==0 mixer priming [A:0x18fba] -- SPEC_LOG item54 */

    /* Envelope (EG1, amplitude), SPEC.adoc S3.4 */
    double attack_s = timecents_to_seconds(
        scale_tc_by_source(r->artic->eg1_attack_tc, r->artic->eg1_attack_vel_tc, velocity));
    double decay_s = timecents_to_seconds(
        scale_tc_by_source(r->artic->eg1_decay_tc, r->artic->eg1_decay_kf_tc, note));
    /* SPEC.adoc S5.1.2 [A:0x18d3d-0x18d49/0x18b4a-0x18b8b]: sustain permille is a progress-domain marker on the 96dB scale at consumption time, not the on-disk linear-amplitude storage reading. */
    int32_t sustain_permille = r->artic->eg1_sustain_permille;
    if (sustain_permille < 0) sustain_permille = 0;
    if (sustain_permille > 1000) sustain_permille = 1000;
    double sustain_hundredths_db = (double)sustain_permille * 9.6 - 9600.0;
    v->env_sustain_level = rt_pow(10.0, sustain_hundredths_db / 100.0 / 20.0);
    if (v->env_sustain_level < 0.0) v->env_sustain_level = 0.0;
    if (v->env_sustain_level > 1.0) v->env_sustain_level = 1.0;
    /* SPEC.adoc S5.1.2 [A:0x19968-0x1997e]: note-setup rescales decay duration to decay*(1000-sustainPermille)/1000 before the decay segment ever runs. */
    double decay_to_sustain_s = decay_s * (double)(1000 - sustain_permille) / 1000.0;
    /* SPEC.adoc S5.1.2.1: decay is a geometric ramp (env_level *= coef) from 1.0, not an asymptotic approach; env_decay_samples_left is the exact countdown that snaps it to env_sustain_level. */
    int32_t decay_samples =
        (int32_t)(decay_to_sustain_s * (double)RENDER_RATE + 0.5);
    if (decay_samples > 0) {
        v->env_decay_samples_left = decay_samples;
        v->env_decay_coef = rt_pow(v->env_sustain_level, 1.0 / (double)decay_samples);
    } else {
        /* Rescaled duration is ~0 samples (e.g. sustain_permille at/near 1000) -- go straight to sustain below, matching how decay_s<=0 already worked pre-fix. */
        v->env_decay_samples_left = 0;
        v->env_decay_coef = 1.0;
    }
    if (attack_s <= 0.0) {
        v->env_level = 1.0;
        v->env_stage = (decay_samples > 0) ? ENV_DECAY : ENV_SUSTAIN;
    } else {
        v->env_level = 0.0;
        v->env_stage = ENV_ATTACK;
        v->env_attack_step = 1.0 / (attack_s * (double)RENDER_RATE);
        v->env_attack_elapsed = 0;
        v->env_attack_samples = (int32_t)(attack_s * (double)RENDER_RATE + 0.5);
        if (v->env_attack_samples < 1) v->env_attack_samples = 1;
    }

    /* EG2, SPEC.adoc S2.4.3 [A:0x15838]: same segment structure as EG1 but scales eg2_to_pitch_cents; zero depth (documented default) leaves it inert, see SPEC_LOG item20 */
    v->eg2_depth_cents = (double)r->artic->eg2_to_pitch_cents;
    v->eg2_sustain_level = (double)r->artic->eg2_sustain_permille / 1000.0;
    if (v->eg2_sustain_level < 0.0) v->eg2_sustain_level = 0.0;
    if (v->eg2_sustain_level > 1.0) v->eg2_sustain_level = 1.0;
    double eg2_atk_s = timecents_to_seconds(r->artic->eg2_attack_tc);
    double eg2_dec_s = timecents_to_seconds(
        scale_tc_by_source(r->artic->eg2_decay_tc, r->artic->eg2_decay_kf_tc, note));
    if (eg2_atk_s <= 0.0) {
        v->eg2_level = 1.0;
        v->eg2_stage = (eg2_dec_s > 0.0) ? ENV_DECAY : ENV_SUSTAIN;
    } else {
        v->eg2_level = 0.0;
        v->eg2_stage = ENV_ATTACK;
        v->eg2_attack_step = 1.0 / (eg2_atk_s * EG2_BLOCK_RATE);
    }
    v->eg2_decay_coef = eg2_coef(eg2_dec_s);
    v->eg2_release_coef = eg2_coef(timecents_to_seconds(r->artic->eg2_release_tc));

    /* Prime the amplitude ramp (see the amp_left reset above). env_level is final only now: 1.0 for an instant attack, 0.0 for a real one. */
    v->amp_l = v->env_level * v->gain_l;
    v->amp_r = v->env_level * v->gain_r;
}

void voice_note_off(int channel, int note) {
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voices[i];
        if (v->active && v->channel == channel && v->note == note && v->held) {
            if (g_channels[channel].sustain != 0) {
                v->sustain_deferred = 1;
            } else {
                start_release(v, 0);
            }
        }
    }
}

void voice_all_sound_off(int channel) {
    /* SPEC.adoc S5.9/S4.3 (CC120): bypasses the sustain hold entirely. */
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voices[i];
        if (v->active && v->channel == channel && v->held) {
            start_release(v, 0);
        }
    }
}

void voice_all_notes_off(int channel) {
    /* SPEC.adoc S5.9/S4.3 (CC123): honours the sustain hold. */
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voices[i];
        if (v->active && v->channel == channel && v->held) {
            if (g_channels[channel].sustain != 0) v->sustain_deferred = 1;
            else start_release(v, 0);
        }
    }
}

void voice_sustain_lift(int channel) {
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voices[i];
        if (v->active && v->channel == channel && v->sustain_deferred) {
            start_release(v, 0);
        }
    }
}

int voice_any_active(void) {
    for (int i = 0; i < NUM_VOICES; i++) {
        if (g_voices[i].active) return 1;
    }
    return 0;
}

/* Frames until EG1's current segment ends (the driver's "next change" term, [A:0x19490]); sustain/release return ENV_NO_CHANGE since neither has a scheduled end. */
uint32_t voice_env_frames_to_change(const Voice *v) {
    switch (v->env_stage) {
        case ENV_ATTACK: {
            if (v->env_attack_step <= 0.0) return 1;
            double n = (1.0 - v->env_level) / v->env_attack_step;
            if (n < 1.0) return 1;
            if (n >= (double)ENV_NO_CHANGE) return ENV_NO_CHANGE;
            return (uint32_t)n + 1;
        }
        case ENV_DECAY:
            return v->env_decay_samples_left > 0 ? (uint32_t)v->env_decay_samples_left : 1;
        default:
            return ENV_NO_CHANGE;
    }
}

double voice_step_envelope(Voice *v) {
    switch (v->env_stage) {
        case ENV_ATTACK:
#if ENV_ATTACK_TABLE_C
            v->env_attack_elapsed++;
            if (v->env_attack_elapsed >= v->env_attack_samples) {
                /* SPEC.adoc S3.4.2 [A:0x18ac0-0x18ad1]: elapsed>=attackDuration leaves Table C entirely, so land on exactly 1.0 rather than t[200]=999, see SPEC_LOG item46 */
                v->env_level = 1.0;
                v->env_stage = (v->env_decay_samples_left > 0) ? ENV_DECAY : ENV_SUSTAIN;
            } else {
                int32_t permille = (int32_t)((int64_t)v->env_attack_elapsed * 1000 / v->env_attack_samples);
                int32_t idx = permille / 5; /* trunc, 0x18b0a-0x18b10 */
                if (idx > 200) idx = 200;
                v->env_level = rt_pow(10.0, (((double)g_table_envshape[idx] / 1000.0) - 1.0) * ENV_SPAN_DB / 20.0);
            }
#else
            v->env_level += v->env_attack_step;
            if (v->env_level >= 1.0) {
                v->env_level = 1.0;
                v->env_stage = (v->env_decay_samples_left > 0) ? ENV_DECAY : ENV_SUSTAIN;
            }
#endif
            break;
        case ENV_DECAY:
            /* SPEC.adoc S5.1.2.1: geometric ramp, same mechanism as ENV_RELEASE; env_decay_samples_left is the exact countdown, avoiding both float drift and a threshold test that doesn't generalize to a nonzero target. */
            v->env_level *= v->env_decay_coef;
            if (--v->env_decay_samples_left <= 0) {
                v->env_level = v->env_sustain_level;
                v->env_decay_samples_left = 0;
                v->env_stage = ENV_SUSTAIN;
            }
            break;
        case ENV_SUSTAIN:
            v->env_level = v->env_sustain_level;
            break;
        case ENV_RELEASE:
            v->env_level *= v->env_release_coef;
            /* SPEC.adoc Part 5's confirmed -80dB finish floor [A:0x19733] was tried here instead of -66dB and measured no benefit (reverted), see SPEC_LOG (exp_coef entry, line 5320) */
            /* +0x13c is the bare EG, not gain-inclusive -- [A:0x194da] corrects SPEC.adoc S5.7's [I] reading. Reaping on it alone dropped audible release tails (fixed by AUDIBLE_FLOOR); see SPEC_LOG item21-reap/entry16. */
            if (v->env_level < 0.0005) {
                v->env_level = 0.0;
                v->active = 0;
                v->env_stage = ENV_IDLE;
            }
            break;
        default:
            v->env_level = 0.0;
            v->active = 0;
            break;
    }
    return v->env_level;
}
