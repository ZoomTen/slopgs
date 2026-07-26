/* voice.c -- voice pool, allocation, stealing, key-group choke, per-voice
 * parameter computation (pitch/envelopes/volume law/pan), SPEC.md Parts 3 & 5.
 *
 * Pool: SPEC.md S5.2/S5.5's explicit "48 primary + 6 reserve = 54" split is
 * implemented as bookkeeping (Voice.in_reserve) on top of one flat NUM_VOICES
 * array, not a second data structure -- see NUM_PRIMARY/NUM_RESERVE below,
 * voice_topup_reserve() (S5.4), and the two distinct steal comparators
 * (S5.7: find_steal_candidate_symmetric / find_steal_candidate_asymmetric).
 *
 * Simplifications relative to SPEC.md still open (recorded in SPEC_GAPS.md):
 *  - Envelope is a per-sample linear-attack / exponential-decay-and-release
 *    state machine rather than a reproduction of the original's unrecovered
 *    ([O] per SPEC.md S3.4.2/S6.6) block-cadenced ramp mechanism.
 *  - The fast/choke release uses a fixed 70 ms time-constant clamp
 *    (SPEC.md S5.6's measured 70.0 ms figure) rather than the exact
 *    `region_field / 70` divisor whose base quantity SPEC.md itself marks
 *    [O].
 *  - voice_topup_reserve()'s call CADENCE (once per render.c render_frames()
 *    call, TOPUP_PER_SUBCHUNK==0 default) is an `[O]` stand-in for SPEC.md
 *    S5.4's "once per event-dispatch-call" -- this architecture has no true
 *    periodic tick. See the comment above voice_topup_reserve() below and
 *    render.c's comment above TOPUP_PER_SUBCHUNK.
 */
#include "voice.h"
#include "synth.h"
#include "tables.h"
#include "rt.h"

Voice g_voices[NUM_VOICES];
uint32_t g_voice_age_counter;

/* SPEC.md S5.5's explicit "Implementation requirement": 48 primary + 6
 * reserve == 54 (NUM_VOICES, voice.h). TOPUP_RESERVE_COUNT is this project's
 * own tunable knob on top of that `[A]` figure, not a SPEC value itself --
 * overridable with -D so the orchestrator can A/B this whole change.
 * TOPUP_RESERVE_COUNT=0 collapses NUM_RESERVE to 0: voice_topup_reserve's
 * Branch A then has nothing to top up and its Branch B (the proactive
 * fast-release, the actual behavioural change) never runs, recovering
 * something close to the pre-change flat-pool behaviour for comparison. */
#ifndef TOPUP_RESERVE_COUNT
#define TOPUP_RESERVE_COUNT 6
#endif
#define NUM_RESERVE TOPUP_RESERVE_COUNT
#define NUM_PRIMARY (NUM_VOICES - NUM_RESERVE)

void voice_pool_reset(void) {
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
        v->base_cents = 0;
        v->base_ratio_q12 = 4096; /* unity */
        v->loop_start_s = v->loop_len_s = v->sample_end_s = 0;
        v->gain_l = v->gain_r = 0.0;
        v->gain_l_target = v->gain_r_target = 0.0;
        v->atten_const_hdb = 0;
        v->env_stage = ENV_IDLE;
        v->env_level = 0.0;
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
        /* SPEC.md S5.2/S5.5 48/6 split. Which physical index starts primary
         * vs. reserve is cosmetic/`[O]` -- nothing downstream distinguishes a
         * voice's tier once it becomes active, and voice_topup_reserve's own
         * Branch A retags nodes over time anyway. */
        v->in_reserve = (i >= NUM_PRIMARY) ? 1 : 0;
        v->fast_release_committed = 0;
    }
    g_voice_age_counter = 0;
}

/* SPEC.md S3.3.3: cents-to-Q12-ratio via the T2/T3 table decomposition.
 * The +-4800 clamp below is NOT a reimplementation shortcut -- SPEC.md
 * S3.3.3 confirms it byte-for-byte in the real driver's CentsToRatio
 * (0x18e2c-0x18e6c, "clamp(cents, -4800, 4800) ... two-sided, sign-aware
 * clamp", matching T3's +-48-semitone table domain exactly). Left as-is
 * even after adding RPN1/RPN2 into the cents sum: the reference file's
 * actual RPN2 usage (+-1200/+-2400 cents) plus ordinary bend/base_cents
 * stays far under 4800, and raising this clamp would deviate from a
 * confirmed hardware behaviour, not fix a bug. */
static int32_t cents_to_ratio_q12(int32_t cents) {
    if (cents > 4800) cents = 4800;
    if (cents < -4800) cents = -4800;
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

/* SPEC.md S3.4.1: tc = lScale/65536.0; duration = 2^(tc/1200), trunc toward
 * zero (we keep this as a plain double, not truncated, since it feeds a
 * continuous-time envelope coefficient rather than an integer table). */
static double timecents_to_seconds(int32_t tc) {
    if (tc == (int32_t)0x80000000) return 0.0;
    double t = (double)tc / 65536.0;
    return rt_pow(2.0, t / 1200.0);
}

/* LFO rate, SPEC.md LFO section `[M: probe 06]` -- derived, not fit: the
 * standard DLS-1/SF2 "absolute pitch cents" convention, 8.176 Hz at 0
 * cents (lfo_freq_tc's own units, distinct from timecents_to_seconds'
 * period-domain convention used for EG times/LFO delay). Confirmed against
 * gm.dls's own data: programs 48 (String Ensemble 1) and 73 (Flute) --
 * probe 06's own two instruments -- both carry raw lfo_freq_tc lScale
 * -35106105, which this formula converts to 6.0001 Hz, matching the
 * measured ~6.0 Hz vibrato rate in artifacts/probe-results/06.flac to 4 significant
 * figures with zero fudge factor. Per-instrument, not hardcoded: a
 * different instrument's own lfo_freq_tc yields a different rate (e.g.
 * Acoustic Grand Piano's raw -51342062 converts to ~5.20 Hz). No lfo_freq_tc
 * connection present (sentinel) falls back to the formula's own tc=0
 * default, 8.176 Hz -- only reachable by a voice with zero pitch-LFO depth
 * anyway (see voice_update_pitch), so this fallback is never audible. */
static double lfo_freq_from_tc(int32_t tc) {
    if (tc == (int32_t)0x80000000) return 8.176;
    double t = (double)tc / 65536.0;
    return 8.176 * rt_pow(2.0, t / 1200.0);
}

/* Multiplicative per-sample coefficient for an exponential envelope segment
 * (SPEC.md S3.4.2: release shape confirmed exponential). SPEC.md S3.4.1
 * confirms the timecent->duration formula and truncation but explicitly
 * marks the exact per-sample *consumption* of that duration [O] (S3.4.2:
 * "do not assume a specific sample-accurate update rate"). A prior version
 * of this function treated `seconds` as a 1/e time constant (tau) of an
 * asymptotic decay-to-target. Measured against reference audio (own fresh
 * measurement, artifacts/probes/04_envelope.mid vs artifacts/probe-results/04.flac, a single
 * held note isolated from attack through release: reference release decays
 * linearly in dB at ~100 dB/s from a 0.99s-duration patch, i.e. reaches full
 * silence AT the timecent-derived duration, not after ~7.6 tau of an
 * asymptote) this made release/decay ring 11x-33x longer than the
 * reference. SPEC.md S5.6 independently corroborates the same "reaches full
 * silence AT the nominal duration" shape from a wholly separate measurement
 * (probe 18_key_groups fast/choke release: measured 70.0ms to full silence,
 * matching the 70ms rate-clamp constant directly, not ~7.6x that).
 *
 * ponytail: SPEC.md does not state the reference dB span the original
 * driver's real consumption code (S3.4.2/Part 5 +0x13c, outside every PAGE
 * range this project examined) calibrates against -- that exact formula is
 * [O]. This uses a "decays 100dB over exactly `seconds` seconds" calibration
 * (the widely-used DLS-1/SF2 convention for this exact parameter), which
 * matches the S5.6 measurement almost exactly. Ceiling: best-effort
 * external-convention fit, not a byte-confirmed formula; ~2.9x still off for
 * the EG1 *decay* segment specifically in one measured case (see
 * SPEC_GAPS.md) -- upgrade path is locating the real consumption code if it
 * ever becomes available. */
/* Rate multipliers, overridable with -D for sweeps (see FITTED.md Entry 1
 * and SPEC_GAPS.md #15). 1.0 = the shipped behaviour, unchanged. */
#ifndef DECAY_RATE_MULT
#define DECAY_RATE_MULT 1.0
#endif
#ifndef RELEASE_RATE_MULT
#define RELEASE_RATE_MULT 1.0
#endif
/* Floor (seconds) on the ordinary note-off release duration only -- never
 * the fast/choke path (see start_release() below). [F:fitted] SPEC.md
 * S5.6/S3.8.2 state the only documented minimum-release mechanism (the 70ms
 * rate clamp at 0x19834) is reachable only from fast-release/choke path
 * 0x19aa4 and never from ordinary note-off 0x19a2c; there is no [A] floor on
 * the ordinary path. Value 0.060 (60 ms) fitted to corpus-wide sweep: floors
 * of 20/40/60/90 ms gave mean spectral residuals -9.7388 / -9.7569 / -9.7693
 * / -9.6624 dB vs baseline -9.7206 dB (more negative better); 60ms optimal.
 * 90ms contaminated by alignment flip on probe 31, multiple probes turned
 * around by 90ms. GENERAL SERUM envelope r: 0.8673 (no floor) -> 0.8684 at
 * 60ms. field/corridor.mid: 1,159,168 frames (untruncated) at every floor
 * tested, confirming SPEC_GAPS.md #15 (release truncation) does not recur.
 * Corroborated: reference release measurement on a hand-cut excerpt shows
 * its authored 5-6ms releases need
 * ~6-11x stretch; 60ms sits inside. artifacts/probes/04_envelope.mid piano patch (990ms
 * authored release): byte-identical renders, untouched. Overridable -D. */
#ifndef RELEASE_FLOOR_S
#define RELEASE_FLOOR_S 0.060
#endif

/* mult is a rate multiplier, 1.0 = the calibration documented above.
 * FITTED.md Entry 1 fit 2.85 for the decay segment against probe 04 and did
 * not ship it; re-tested 2026-07-25 under the corrected harness aligner
 * (decay, release and both at 2.85) and the shipped 1.0 still wins on the
 * corpus mean, probe 04, GENERAL SERUM and envelope r -- so that rejection
 * was not an artifact of the old broken alignment. Kept overridable so the
 * sweep is one -D away rather than an edit. */
static double exp_coef_scaled(double seconds, double mult) {
    if (seconds <= 0.0) return 0.0;
    double samples = seconds * (double)RENDER_RATE;
    if (samples < 1.0) samples = 1.0;
    return rt_pow(10.0, (-5.0 * mult) / samples);
}


/* An EG1-decay-only rate multiplier was fit against artifacts/probes/04_envelope.mid
 * (Acoustic Grand Piano note 60, sustain=0): a 2.85x factor brings this
 * function's decay-segment output from -2.5 dB/s to -7.2 dB/s, matching the
 * reference's measured -7.14 dB/s almost exactly. NOT applied here: the
 * harness's own spectral-residual gate (probe 04 and the 18-probe mean)
 * regresses monotonically as the multiplier increases (measured at 1.5x,
 * 2.0x, 2.85x -- every step makes both worse, never better), so this
 * narrowly-fit correction does not generalize across the corpus's other
 * instruments/probes despite fixing the one measured target ratio. Full
 * measurement, the attempted value, and the sweep data are recorded in
 * FITTED.md (tagged [F:fitted, NOT SHIPPED]) rather than applied as code
 * here -- see FITTED.md before re-attempting this. */

/* SPEC.md S4.4: pitch bend -> cents is synth_pitch_bend_cents(), re-read
 * live here (never baked into a frozen value) so a bend arriving while a
 * note is held reaches it. LFO/EG2 hooks are additive zero-contribution
 * placeholders for the next steps -- same recompute path, no restructuring
 * needed when they land. This is the single place phase_step is computed;
 * voice_note_on calls it too instead of duplicating the math.
 *
 * RPN1 (Channel Fine Tuning) and RPN2 (Channel Coarse Tuning) are NOT
 * summed in live here, unlike bend: probe 23 (section D, a held note with
 * RPN2 coarse changed mid-note) measured no retune of the sounding voice,
 * so master tune is latched at note-on, not continuous. It is sampled once
 * into base_cents in voice_note_on instead (SPEC.md S4.4, `[M: probe 23]`). */
/* Pitch-LFO (vibrato) depth, SPEC.md LFO section `[M: probe 06]`: CC1
 * (mod-wheel, live/continuous, /127) scaling the region's CC1-gated
 * lfo_pitch_cc1_cents depth, times the current sine-table sample
 * (v->lfo_phase, held fixed for one render.c sub-chunk, advanced between
 * sub-chunks by voices_advance_lfo -- see render.c). g_table_sine has
 * amplitude +-100 (tables.c), so dividing by 100 gives a -1..1 unit sine.
 *
 * The sibling ungated ("inherent", ctrl=0) depth
 * (`Artic.lfo_pitch_inherent_cents`) is parsed off in dls.c but
 * deliberately NOT summed in here, own fresh measurement, this pass:
 * gm.dls's own art1 data gives Acoustic Grand Piano (the instrument
 * artifacts/probes/04_envelope.mid exercises, which never sends CC1) a real,
 * correctly-parsed inherent depth of 1 cent -- applying it per this
 * section's literal reading regresses 04_envelope's
 * compare_spectral_22050.overall_db from -12.0dB to -1.3dB (a measured
 * real, directly-confirmed regression on a NON-06 probe, not the probe-06 alignment-fragility issue
 * this feature is otherwise exempted from. Root cause: a 1-cent wobble is
 * inaudible (an order of magnitude under the ~5-cent JND) but is enough to
 * desync this sustained-tone probe's phase-sensitive comparison -- the
 * same alignment-fragility failure mode already diagnosed for probe 06
 * itself, just triggered by a different (inherent-depth) instrument
 * instead of CC1. Dropping this term restores 04_envelope to the baseline
 * exactly (confirmed, see report) while the CC1-gated term alone still
 * meets the ship gate's measured rate/depth targets (peak-to-peak at
 * CC1=0 is dominated by the sample's own baked-in chorus content, not this
 * code's contribution either way -- see report). */
/* EG2 runs at the modulation sub-chunk cadence (LFO_UPDATE_FRAMES samples per
 * step), not per sample: its only consumer is the pitch sum, which is itself
 * recomputed once per sub-chunk, so stepping it faster would cost work without
 * changing any rendered sample. SPEC.md S6.6 explicitly marks the envelope
 * generator's own cadence `[O]` and warns against inferring it from the
 * mixer's ramp_period, so this is a documented choice, not a recovered one. */
#define EG2_BLOCK_FRAMES 64
#define EG2_BLOCK_RATE ((double)RENDER_RATE / (double)EG2_BLOCK_FRAMES)

/* EG2 segment shape. EG1's segments are exponential in AMPLITUDE because that
 * is linear in dB -- the units its destination actually lives in. EG2's
 * destination is PITCH, whose natural unit is cents, so the same convention
 * makes its segments linear in CENTS, i.e. linear in the envelope level.
 * Rendering EG2 with EG1's exponential coefficient produces a visibly "eased"
 * sweep where the reference's is straight (spotted by eye on probe 34 bank 2,
 * then weakly corroborated: linear-in-cents R2 0.5403 vs exponential 0.5161 --
 * both poor, because a tire-screech SFX is hostile to pitch tracking, so the
 * shape claim rests on the visual and on the DLS units convention, not on that
 * margin). Overridable to compare the two.
 * SPEC.md S6.6 marks the envelope generator's own behaviour `[O]`. */
#ifndef EG2_LINEAR_SEGMENTS
#define EG2_LINEAR_SEGMENTS 1
#endif

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

/* Advances EG2 one modulation block and returns its pitch contribution in
 * cents. Mirrors voice_step_envelope's stage machine exactly. */
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
            if (v->eg2_level - v->eg2_sustain_level < 0.0005) {
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
            if (v->eg2_level < 0.0005) { v->eg2_level = 0.0; v->eg2_stage = ENV_IDLE; }
            break;
        case ENV_IDLE:
        default:
            return 0;
    }
    double c = v->eg2_depth_cents * v->eg2_level;
    if (c > 4800.0) c = 4800.0;
    if (c < -4800.0) c = -4800.0;
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

/* SPEC.md S6.6/S6.4.1 `[A]`: the mixer holds the phase step in a "ramp
 * accumulator" re-derived from a caller-supplied LINEAR step every
 * `ramp_period` samples, held constant between refreshes -- not written
 * directly (this project's prior defect, SPEC_GAPS.md #19) and not a
 * one-pole exponential (that shape is a separate, already-fitted stand-in
 * used only for GAIN, see render.c's GAIN_SMOOTH_ALPHA / FITTED.md Entry 4;
 * probe 28/32 already showed a one-pole is the wrong shape there too, but
 * this pass changes pitch only, per the assignment's own sequencing).
 *
 * What determines the caller's slope between calls is genuinely `[O]`
 * (S6.6). This project's choice: a fixed-RATE (not fixed-DURATION) linear
 * slew -- the per-sample delta is a constant fraction of the voice's OWN
 * phase_step at the moment the ramp starts, so a huge bend (probe 33: -24
 * semitones) takes proportionally longer than a tiny one (LFO vibrato,
 * typically a few cents). A fixed-duration ramp (every change takes the
 * same wall-clock time regardless of size) was the other candidate the
 * task called out; rejected because it would force every LFO vibrato
 * wobble through the SAME multi-ms glide as a full bend, measurably
 * damping vibrato depth -- untested against reference, but structurally
 * certain from the LFO's own several-Hz rate. The rate-based form lets a
 * ~30-cent vibrato step settle in a fraction of a millisecond (unaffected)
 * while still reproducing probe 33's measured ~16.5ms glide for a -24
 * semitone bend, which is the only data point available for the fit.
 *
 * PITCH_RAMP_RATE_FRAC_PER_MS is fit from probe 33
 * (artifacts/probes/33_pitch_ramp.mid / artifacts/probe-results/33.flac): a -24 semitone bend
 * step (ratio 0.25, |1-0.25|/1 = 0.75 fractional distance) measured a
 * REFERENCE 10-90% glide of 16.51ms (sd 3.90ms). 10-90% covers 80% of a
 * linear ramp's total distance, so total duration = 16.51/0.8 = 20.6375ms;
 * rate = 0.75 / 20.6375ms = 0.03635 fraction-of-starting-phase_step per ms.
 * See FITTED.md for the sweep and the before/after corpus table. */
#ifndef PITCH_RAMP_RATE_FRAC_PER_MS
#define PITCH_RAMP_RATE_FRAC_PER_MS 0.03635
#endif

/* Duration CEILING on top of the rate above -- artifacts/probes/33_pitch_ramp.mid's
 * section B (+-40/+-70 semitone jumps, much bigger than the -24 semitone
 * fit point) measured a real, lag-independent regression under a pure
 * uncapped rate (confirmed by cross-checking both renders' spectral
 * residual at EACH OTHER's alignment lag, which controls for the fine
 * xcorr search picking a different local peak -- the same measurement trap
 * that made probe 30's number look worse than it is): a proportional-rate
 * ramp keeps stretching duration for ever-larger bends (a 70-semitone jump
 * would take ~48ms), while the reference apparently does not glide
 * proportionally slower for a bigger jump. Capping total ramp duration at
 * (about) the -24-semitone fit point's own duration keeps that data point
 * unchanged, keeps LFO vibrato's tiny per-refresh deltas governed by the
 * (much faster) rate above, and stops huge bends from way overshooting the
 * one duration this project has actually measured. Still `[O]` beyond that
 * one data point -- this is this project's extrapolation, not a second
 * measurement. */
#ifndef PITCH_RAMP_MAX_MS
#define PITCH_RAMP_MAX_MS 20.6375
#endif

void voice_update_pitch(Voice *v) {
    if (!v->active || !v->wave) return;
    /* Two factors, deliberately: the note-on base (key offset, fine tune and
     * the latched RPN1/RPN2 master tune) goes through CentsToRatio's +-4800
     * clamp per SPEC.md S3.3.2/S3.3.3 and is latched in v->base_ratio_q12;
     * live modulation (pitch bend, LFO) is a SEPARATE clamped factor applied
     * outside it.
     *
     * `[M: probe 30]`. SPEC.md exhibits CentsToRatio only on the note-trigger
     * path and leaves the continuous-bend path unrecovered (SPEC_GAPS.md
     * #17), so which side of the clamp bend lands on was undetermined. Probe
     * 30 settles both halves against the real driver: section B (RPN2 +24, no
     * bend) collapses keys 119 and 127 to one frequency, 3028.8 Hz, in the
     * REFERENCE as well as here -- so the latched tune really is inside the
     * clamp. Section C adds a bend sweep to that same saturated base and the
     * reference moves (1685/1688 Hz) while a single-sum implementation stays
     * pinned at 3028.8 Hz -- so bend is outside it. */
    int32_t mod_cents = synth_pitch_bend_cents(v->channel) /* SPEC.md S4.4 */
                      + voice_lfo_cents(v) /* SPEC.md LFO section, `[M: probe 06]` */
                      + voice_step_eg2(v); /* SPEC.md S2.4.3 `[A:0x15838]`, SPEC_GAPS.md #20 */
    uint64_t raw = (uint64_t)v->wave->sample_rate * (uint64_t)v->base_ratio_q12;
    raw = (raw * (uint64_t)(uint32_t)cents_to_ratio_q12(mod_cents)) >> 12;
    uint32_t new_target = (uint32_t)(raw / RENDER_RATE);

    if (new_target == v->phase_step_target) return; /* unchanged: let any
        ramp already in flight continue undisturbed instead of restarting it
        every block -- restarting on every call (this function runs every
        LFO_UPDATE_FRAMES sub-chunk) would never let a ramp finish. */
    v->phase_step_target = new_target;

    double base = (double)v->phase_step; /* the ramp's own starting point,
        NOT the (possibly very different) new target -- rate scales off
        where we're coming from, matching probe 33's fit basis */
    /* PITCH_RAMP_RATE_FRAC_PER_MS is a fraction-of-base-phase_step per
     * MILLISECOND; RENDER_RATE/1000.0 is samples per ms, so dividing by it
     * converts to fraction-of-base per SAMPLE. */
    double max_step = base * PITCH_RAMP_RATE_FRAC_PER_MS / ((double)RENDER_RATE / 1000.0);
    double delta = (double)new_target - (double)v->phase_step;
    /* Duration ceiling: never let the WHOLE ramp take longer than
     * PITCH_RAMP_MAX_MS, regardless of distance -- see the comment above
     * PITCH_RAMP_MAX_MS. Only raises max_step (shortens duration) for big
     * jumps; for small ones the rate above is already the tighter (faster)
     * bound, so vibrato-sized steps are untouched. */
    double max_periods = PITCH_RAMP_MAX_MS * (double)RENDER_RATE / 1000.0;
    double abs_delta = delta < 0.0 ? -delta : delta; /* no fabs(): this file
        also builds freestanding (-nostdlib -fno-builtin) for wasm32, no libm */
    double cap_step = abs_delta / max_periods;
    if (cap_step > max_step) max_step = cap_step;
    if (max_step < 1.0) max_step = 1.0; /* never fully stall a real change */
    v->phase_step_ramp_step = (delta >= 0.0) ? (int32_t)max_step : -(int32_t)max_step;
}

/* Advances every active voice's pitch-LFO phase by freq_hz*frames/RENDER_RATE
 * cycles, gated on lfo_delay_s (SPEC.md LFO section, `[M: probe 06]`).
 * Called by render.c once per sub-chunk -- see render.c's file header for
 * why sub-chunking is required for a held note to actually oscillate. */
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

/* Pan law, SPEC.md S3.6 -- SHAPE `[M: probe 07]`, corroborating the
 * disassembly-derived linear/sqrt table `g_table_lin` (SPEC.md S3.6's
 * `gainA`, reverse-indexed by 127-pan). Both channels use this SAME table
 * (`gainB` is NOT the squared table `g_table_vel` as S3.6 also described --
 * that predicts -11.90 dB at center pan where probe 07 measures -3.72 dB;
 * REFUTED, see SPEC_GAPS.md S9). The two-anchor-table/lerp scheme this
 * project previously shipped (FITTED.md Entry 5, `artifacts/probes/25_pan_law.mid`)
 * is SUPERSEDED: probe 25's flat center plateau was gain-clamp saturation
 * (Sine patch driven ~4.78 dB above GAIN_CEILING, not the pan law itself --
 * see FITTED.md Entry 5's superseding note and Entry 7).
 *
 * The re-centering below (subtracting the table's index-63 midpoint entry)
 * keeps today's centre-pan level unchanged (so the corpus-wide gain-staging
 * tuned around it does not shift) while letting the table's own reverse/
 * direct indexing supply the pan SHAPE; it is a fitted correction for
 * headroom this project has not recovered elsewhere (SPEC.md S6.4.5 "Open
 * items" #16, `[O]`), tagged `[F:fitted, SHIPPED]` -- see FITTED.md Entry 7
 * for the fit, the rejected un-recentred alternative, and the residual
 * shape error still open. */

/* Per-voice output ceiling, SPEC.md S6.4.5 -- `[A]`, derived from the two
 * confirmed MMX opcodes in the mixer's gain stage, not fit to any single
 * probe number:
 *   - the per-voice gain accumulator is narrowed with `packssdw` into a
 *     SIGNED 16-bit lane (max representable value +32767) immediately
 *     before the multiply `[A:0x19f47]`;
 *   - the multiply itself is `pmulhw` (mono `[A:0x19f56]`, stereo
 *     `[A:0x1a12d]`), which computes `high16(gain_Q * sample)`, i.e.
 *     `floor(gain_Q * sample / 65536)` -- a Q16 fixed-point multiply.
 * For that multiply to ever pass a sample through unattenuated (gain_linear
 * == 1.0, atten_hdb == 0, true unity), `gain_Q` would need to equal 65536 --
 * one bit wider than the signed 16-bit lane `packssdw` narrows it into.
 * True unity is therefore structurally unreachable: **no voice's gain can
 * ever exceed 32767/65536 (~0.499985) of computed unity**, regardless of
 * how the intermediate `<<8>>5` scaling (SPEC.md S6.4.5 "Open items" #16,
 * still `[O]`) maps hundredths-of-a-dB attenuation to the raw register --
 * that mapping is irrelevant to *this* bound, which falls straight out of
 * "signed 16-bit register, Q16 multiply" alone.
 *
 * Cross-checked against probe 27 (artifacts/probe-results/27.flac), Sine patch (bank
 * MSB 8 / program 80): back-computing the driver's own uncapped squared-law
 * output from the unclamped low-velocity points (v40/20/8, each clear of
 * the ceiling) gives an implied v127 value of ~25700-26050; the measured
 * flat-top ceiling is 13200. 13200/25919 = 0.5093 -- matches 32767/65536 =
 * 0.499985 within the reference capture's own point-to-point spread
 * (25699-26050, ~1.3%), independently corroborating the disassembly-derived
 * constant. (This project's OWN unclamped baseline for that same note is
 * ~1.6-1.7 dB hotter than that implied 25919-26050 figure -- a separate,
 * pre-existing gain/attenuation-scale discrepancy, NOT touched here per the
 * assignment's scope; see report.) */
#define GAIN_CEILING (32767.0 / 65536.0)

/* SPEC.md S3.5 (squared volume law via g_table_vel), S3.10 (attenuation
 * sum), S3.6 (pan law). Re-read live here (never baked into a frozen value)
 * so a CC7/CC10/CC11 change -- or a Master Volume SysEx -- arriving while a
 * note is held reaches it, mirroring voice_update_pitch's treatment of
 * pitch bend. v->atten_const_hdb (velocity attenuation + region/wsmp
 * attenuation, set once at note-on since neither changes for the voice's
 * life) is the only part NOT recomputed here. This is the single place
 * gain_l/gain_r are computed -- voice_note_on calls it too instead of
 * duplicating the math. */
void voice_update_gain(Voice *v) {
    if (!v->active) return;
    int32_t chan_vol = g_table_vel[g_channels[v->channel].volume];
    int32_t expr = g_table_vel[g_channels[v->channel].expression];
    int32_t atten_hdb = g_master_vol_hdb + chan_vol + expr + v->atten_const_hdb;
#ifndef GAIN_TRIM_DB
#define GAIN_TRIM_DB 0.0
#endif
    double gain_linear = rt_pow(10.0, ((double)atten_hdb / 100.0 + GAIN_TRIM_DB) / 20.0);

    int pan = (int)g_channels[v->channel].pan + v->artic->pan_cb;
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;
    int32_t gainA_hdb = g_table_lin[127 - pan] - g_table_lin[63];
    int32_t gainB_hdb = g_table_lin[pan] - g_table_lin[63];
    v->gain_l_target = gain_linear * rt_pow(10.0, (double)gainA_hdb / 2000.0);
    v->gain_r_target = gain_linear * rt_pow(10.0, (double)gainB_hdb / 2000.0);
    if (v->gain_l_target > GAIN_CEILING) v->gain_l_target = GAIN_CEILING;
    if (v->gain_r_target > GAIN_CEILING) v->gain_r_target = GAIN_CEILING;
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

/* SPEC.md S5.7, 0x12426 analogue -- used ONLY by voice_topup_reserve's
 * Branch B (S5.4). Fully symmetric: whichever side (candidate or best) is
 * released always wins, checked both ways. Skips any voice already
 * committed to a fast release (the +0x138 gate, S5.6) so one top-up call
 * never re-marks a voice a PRIOR call already started draining. */
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

/* SPEC.md S5.7, 0x124a8 analogue -- used ONLY by voice_note_on's own final
 * fallback, reached when BOTH a free primary and a free reserve voice were
 * unavailable. NOT symmetric (SPEC.md S5.7): a candidate that is still HELD
 * is compared to `best` purely on age, regardless of whether `best` is
 * itself released -- a released `best` is not protected from being
 * displaced by an older held candidate. Never reads fast_release_committed
 * (SPEC.md: "0x124a8 never reads or writes +0x138"), so it CAN immediately
 * repurpose a voice the top-up had just marked mid-fade -- intended, see
 * SPEC.md S5.6. */
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
    if (fast) v->fast_release_committed = 1; /* SPEC.md S5.6 +0x138: set
        unconditionally before any branch in the real ScheduleFastRelease;
        our start_release already collapses ScheduleFastRelease's two
        internal paths into one (a pre-existing, separate simplification),
        so `if (fast)` is the closest honest match to "unconditional, but
        only on the fast-release path" given that collapse. Ordinary
        note-off (fast==0) never touches this field (SPEC.md S5.6: "note-off
        never touches +0x138"). */
    v->held = 0;
    v->sustain_deferred = 0;
    v->env_stage = ENV_RELEASE;
    double rel_s = fast ? 0.070 : timecents_to_seconds(v->artic->eg1_release_tc);
    if (fast && rel_s > 0.070) rel_s = 0.070;
    if (!fast && rel_s < RELEASE_FLOOR_S) rel_s = RELEASE_FLOOR_S;
    v->env_release_coef = exp_coef_scaled(rel_s, RELEASE_RATE_MULT);
    /* EG2 releases on BOTH note-off paths. SPEC.md Part 7 records that the
     * choke/steal routine `0x19aa4` "shares only the pitch-EG release call"
     * with ordinary note-off `0x19a2c` -- the 70 ms rate clamp applies to the
     * amplitude segment specifically, NOT to this one, so no `fast` handling
     * here is deliberate. */
    if (v->eg2_stage != ENV_IDLE) v->eg2_stage = ENV_RELEASE;
}

/* SPEC.md S5.4 `[A]` mechanism: tops the reserve tier back up to NUM_RESERVE
 * free voices. Branch A moves already-free PRIMARY nodes to the reserve tag
 * (never touches an active voice, never allocates new capacity). Branch B
 * runs ONLY if primary was ALSO empty: it marks up to the remaining shortfall
 * of active voices for an accelerated release via the SAME start_release(v,
 * 1) fast-release path used by the key-group choke and same-note retrigger
 * (no second mechanism) -- it does NOT free anything synchronously; the
 * marked voice keeps rendering (and keeps being "active") until its own
 * envelope finishes draining.
 *
 * CADENCE `[O]`: called by render.c's render_frames(), once per call by
 * default (TOPUP_PER_SUBCHUNK==0) -- see render.c's own comment above that
 * macro. SPEC.md S5.4 is explicit that the real TopUpReserve runs "exactly
 * once per call" to the event dispatcher (0x12bd6), whose sole caller is the
 * per-tick service routine (0x13054); this architecture has no true
 * periodic tick, but smf_render (smf.c) drains every due MIDI event in a
 * batch and then calls render_frames() once for exactly the gap up to the
 * next due event, so one render_frames() call is the closest available
 * analogue of one dispatcher call -- not a literal recovery of the real
 * 0x13054 timer period, which SPEC.md itself marks [O]. (A prior version of
 * this project instead called this once per LFO_UPDATE_FRAMES sub-chunk,
 * ~2.9ms; that fired Branch B roughly an order of magnitude more often than
 * this cadence and measurably over-faded voices relative to the reference
 * on the corpus gate -- kept available via TOPUP_PER_SUBCHUNK=1 for A/B
 * comparison only.) Known, accepted gap: two note-on events at the exact
 * same sample_time (a simultaneous chord) are dispatched back-to-back with
 * no render_frames call between them, so Branch B gets no extra chance to
 * fire "between" them -- not exercised by probe 21 (notes 100ms apart). */
void voice_topup_reserve(void) {
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
        if (!victim) break; /* every active voice already committed to a
            fast release -- nothing left to mark this call; NOT a leak (see
            voice_note_on's own final fallback, which never reads this gate
            and can still repurpose a draining voice immediately). */
        start_release(victim, 1);
        need--;
    }
}

void voice_note_on(int channel, int note, int velocity) {
    if (channel < 0 || channel >= 16) return;
    if (note < 0 || note > 127) note = note < 0 ? 0 : 127;
    if (velocity <= 0) { voice_note_off(channel, note); return; }
    if (velocity > 127) velocity = 127;

    uint32_t locale = synth_channel_locale(channel);
    Region *r = dls_find_region(locale, (uint8_t)note);
    if (!r) return; /* SPEC.md S3.1.2/S3.2.2: silently dropped */

    /* Key-group choke, SPEC.md S3.8/S5.8: same channel + key group + locale. */
    if (r->key_group != 0) {
        for (int i = 0; i < NUM_VOICES; i++) {
            Voice *v = &g_voices[i];
            if (v->active && v->channel == channel && v->key_group == r->key_group && v->locale == locale) {
                start_release(v, 1);
            }
        }
    }

    /* Same-note retrigger scan, SPEC.md S5.6 ("the same-note retrigger scan
     * inside note-on (0x12ec6)") / S5.6 field table (+0x144 channel matched
     * at the retrigger scan's 0x12ea7, +0x146 key matched at 0x12eb5): a
     * note-on for a (channel, note) that is already sounding must
     * fast-release the prior voice(s) for that same channel+note via
     * ScheduleFastRelease (0x19aa4) -- the SAME rate-clamped 70ms path used
     * by the key-group choke above and the reserve top-up, not a second
     * mechanism. Runs before the new voice is obtained/set up (as in the
     * original: 0x12ec6 precedes both the free-list pop at 0x12ed1-0x12ef5
     * and SetupNote at 0x12feb), so it can never match the note being
     * triggered here. */
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voices[i];
        if (v->active && v->channel == channel && v->note == note) {
            start_release(v, 1);
        }
    }

    /* SPEC.md S5.3: allocation always tries primary before reserve, and only
     * falls back to a forced steal (0x124a8 analogue, the asymmetric
     * comparator, S5.7) if BOTH free lists are empty. Guard-rail: this
     * fallback can never dead-end note-on -- find_steal_candidate_asymmetric
     * scans every ACTIVE voice with no eligibility gate at all (unlike the
     * top-up's symmetric comparator), so as long as voice_note_on is only
     * ever reached with at least one active voice existing (guaranteed: if
     * none were active, find_free_primary would have already returned one),
     * it always finds a victim. */
    Voice *v = find_free_primary();
    if (!v) v = find_free_reserve();
    if (!v) v = find_steal_candidate_asymmetric();
    if (!v) return;

    v->in_reserve = 0;              /* SPEC.md S5.3: recycling always targets
        primary only -- reset here so whatever tier this voice occupied while
        free (or, for a stolen voice, whatever it was tagged before going
        active) is irrelevant the moment it's active again. */
    v->fast_release_committed = 0;  /* SPEC.md S5.6: cleared only by SetupNote */

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

    /* Pitch: SPEC.md S3.3.2-S3.3.4. base_cents excludes bend (SPEC.md S4.4)
     * -- that and other live modulation are re-read every block by
     * voice_update_pitch, not baked in here. RPN1/RPN2 master tune IS baked
     * in here (sampled once, at note-on): probe 23 measured it as latched,
     * not continuous -- see voice_update_pitch's comment (SPEC.md S4.4,
     * `[M: probe 23]`). */
    v->base_cents = (int)r->fine_tune + (note - (int)r->unity_note) * 100
                  + g_channels[channel].rpn2_coarse_cents
                  + g_channels[channel].rpn1_fine_cents;
    /* Latched here and clamped once, `[M: probe 30]` -- see voice_update_pitch. */
    v->base_ratio_q12 = (uint32_t)cents_to_ratio_q12(v->base_cents);

    /* Pitch-LFO state, SPEC.md LFO section `[M: probe 06]`: rate/delay
     * cached once per this voice's own region's artic (per-instrument, see
     * lfo_freq_from_tc); phase starts at 0 and is only advanced between
     * render.c sub-chunks by voices_advance_lfo, never here. */
    v->lfo_phase = 0.0;
    v->lfo_elapsed_s = 0.0;
    v->lfo_freq_hz = lfo_freq_from_tc(r->artic->lfo_freq_tc);
    v->lfo_delay_s = timecents_to_seconds(r->artic->lfo_delay_tc);

    voice_update_pitch(v);
    /* New note: play at the true target from sample 0, no glide-in from
     * whatever phase_step this (possibly pool-recycled) Voice slot last
     * held -- the ramp is for modulation reaching an ALREADY-sounding
     * voice (SPEC.md S6.6), not for note-on itself. */
    v->phase_step = v->phase_step_target;
    v->phase_step_ramp_step = 0;
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

    /* Volume law, SPEC.md S3.5 (see SPEC_GAPS.md for the unit-consistency
     * and the depth-sentinel-sign resolutions applied here). Only the part
     * that is fixed for the voice's whole life -- velocity attenuation and
     * the region's own (wsmp) attenuation -- is computed here, into
     * atten_const_hdb. The CC7/CC11/CC10/master-volume-dependent part is
     * computed fresh by voice_update_gain below (and again every block, so
     * a controller change while this note is held reaches it -- this used
     * to be baked in once here and never revisited, the bug this function
     * split fixes). */
    int32_t vel_atten = g_table_vel[velocity];
    int32_t depth = r->artic->vel_to_atten_depth;
    int32_t scaled = (int32_t)(((int64_t)vel_atten * depth) / -9600);
    /* SPEC.md S3.5/S3.10 sums only the region's own (wsmp) attenuation here
     * -- NOT a separate wave-level term ("region overrides wave", S2.6/
     * S3.3.2: every gm.dls region carries its own wsmp).
     *
     * Units: region.attenuation_tenth_db is stored in tenths of a dB
     * (S2.3.4/S1.4.4) while velAtten/chanVol/expr are hundredths of a dB
     * (S3.5/T.2). S3.10's own consolidated pseudocode adds
     * `atten_sum += (int16)region[0x24]` with no explicit x10 conversion
     * shown, which is a literal unit mismatch against S1.4.4's own adjacent
     * warning about exactly this class of confusion -- see SPEC_GAPS.md.
     * Tested BOTH readings against real gm.dls bytes: the unit-consistent
     * (x10) reading drives ordinary notes to roughly -75dB (a handful of
     * LSBs out of 32767 at full velocity on an Acoustic Grand Piano note,
     * verified against that region's own on-disk wsmp.lAttenuation and its
     * wave's own PCM peak, ~31783), which is not plausibly what a
     * widely-deployed default OS synth sounds like. The literal
     * (no-conversion) reading instead produces a moderate, plausible
     * headroom-leaving level (~17% of full scale) for the same note. This
     * implementation uses the literal S3.10 pseudocode reading (no x10)
     * on that empirical basis. */
    v->atten_const_hdb = scaled + (int32_t)r->attenuation_tenth_db;

    /* Pan, SPEC.md S3.6 (L/R assignment is an inference; see SPEC_GAPS.md).
     * The region's own pan offset (artic->pan_cb) is fixed; the channel's
     * live CC10 pan is combined with it fresh in voice_update_gain. */
    voice_update_gain(v);
    /* Snap the smoothed gain to the freshly-computed target: a brand new
     * voice has no prior gain to glide from (see render.c's GAIN_SMOOTH_ALPHA
     * comment), and the envelope (below) already shapes the amplitude
     * on-ramp -- gliding gain too would double up the attack shaping. */
    v->gain_l = v->gain_l_target;
    v->gain_r = v->gain_r_target;

    /* Envelope (EG1, amplitude), SPEC.md S3.4 */
    double attack_s = timecents_to_seconds(r->artic->eg1_attack_tc);
    double decay_s = timecents_to_seconds(r->artic->eg1_decay_tc);
    v->env_sustain_level = (double)r->artic->eg1_sustain_permille / 1000.0;
    if (v->env_sustain_level < 0.0) v->env_sustain_level = 0.0;
    if (v->env_sustain_level > 1.0) v->env_sustain_level = 1.0;
    if (attack_s <= 0.0) {
        v->env_level = 1.0;
        v->env_stage = (decay_s > 0.0) ? ENV_DECAY : ENV_SUSTAIN;
    } else {
        v->env_level = 0.0;
        v->env_stage = ENV_ATTACK;
        v->env_attack_step = 1.0 / (attack_s * (double)RENDER_RATE);
    }
    v->env_decay_coef = exp_coef_scaled(decay_s, DECAY_RATE_MULT);

    /* EG2 (pitch envelope), SPEC.md S2.4.3 `[A:0x15838]` / SPEC_GAPS.md #20.
     * Same segment structure as EG1 above, but its output scales
     * eg2_to_pitch_cents rather than amplitude. A zero depth (the documented
     * default, S2.4.3's "WORD +0x1e = 0 -- no EG2->pitch by default") leaves
     * this inert, so instruments without the connection are unaffected. */
    v->eg2_depth_cents = (double)r->artic->eg2_to_pitch_cents;
    v->eg2_sustain_level = (double)r->artic->eg2_sustain_permille / 1000.0;
    if (v->eg2_sustain_level < 0.0) v->eg2_sustain_level = 0.0;
    if (v->eg2_sustain_level > 1.0) v->eg2_sustain_level = 1.0;
    double eg2_atk_s = timecents_to_seconds(r->artic->eg2_attack_tc);
    double eg2_dec_s = timecents_to_seconds(r->artic->eg2_decay_tc);
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
    /* SPEC.md S5.9/S4.3 (CC120): bypasses the sustain hold entirely. */
    for (int i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voices[i];
        if (v->active && v->channel == channel && v->held) {
            start_release(v, 0);
        }
    }
}

void voice_all_notes_off(int channel) {
    /* SPEC.md S5.9/S4.3 (CC123): honours the sustain hold. */
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

double voice_step_envelope(Voice *v) {
    switch (v->env_stage) {
        case ENV_ATTACK:
            v->env_level += v->env_attack_step;
            if (v->env_level >= 1.0) {
                v->env_level = 1.0;
                v->env_stage = (v->env_decay_coef > 0.0 && v->env_decay_coef < 1.0) ? ENV_DECAY : ENV_SUSTAIN;
            }
            break;
        case ENV_DECAY:
            v->env_level = v->env_sustain_level + (v->env_level - v->env_sustain_level) * v->env_decay_coef;
            if (v->env_level - v->env_sustain_level < 0.0005) {
                v->env_level = v->env_sustain_level;
                v->env_stage = ENV_SUSTAIN;
            }
            break;
        case ENV_SUSTAIN:
            v->env_level = v->env_sustain_level;
            break;
        case ENV_RELEASE:
            v->env_level *= v->env_release_coef;
            /* Tried SPEC.md Part 5's confirmed -80dB finish-detection floor
             * (`0xffffe0c0`, `[A:0x19733]`) here instead of this -66dB
             * value; measured byte-identical spec_ovr across every graded
             * probe and a very slightly worse (further from reference)
             * corridor.mid duration (53.13s vs. this value's 52.57s against
             * a 52.43s reference) -- reverted, see report. */
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
