/* voice.c -- voice pool, allocation, stealing, key-group choke, per-voice
 * parameter computation (pitch/envelopes/volume law/pan), SPEC.adoc Parts 3 & 5.
 *
 * Pool: SPEC.adoc S5.2/S5.5's explicit "48 primary + 6 reserve = 54" split is
 * implemented as bookkeeping (Voice.in_reserve) on top of one flat NUM_VOICES
 * array, not a second data structure -- see NUM_PRIMARY/NUM_RESERVE below,
 * voice_topup_reserve() (S5.4), and the two distinct steal comparators
 * (S5.7: find_steal_candidate_symmetric / find_steal_candidate_asymmetric).
 *
 * Simplifications relative to SPEC.adoc still open (recorded in SPEC_LOG.adoc):
 *  - Envelope is a per-sample linear-attack / exponential-decay-and-release
 *    state machine rather than a reproduction of the original's unrecovered
 *    ([O] per SPEC.adoc S3.4.2/S6.6) block-cadenced ramp mechanism.
 *  - The fast/choke release uses a fixed 70 ms time-constant clamp
 *    (SPEC.adoc S5.6's measured 70.0 ms figure) rather than the exact
 *    `region_field / 70` divisor whose base quantity SPEC.adoc itself marks
 *    [O].
 *  - voice_topup_reserve()'s call CADENCE (once per render.c render_frames()
 *    call, TOPUP_PER_SUBCHUNK==0 default) is an `[O]` stand-in for SPEC.adoc
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

/* SPEC.adoc S5.5's explicit "Implementation requirement": 48 primary + 6
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
        /* SPEC.adoc S5.2/S5.5 48/6 split. Which physical index starts primary
         * vs. reserve is cosmetic/`[O]` -- nothing downstream distinguishes a
         * voice's tier once it becomes active, and voice_topup_reserve's own
         * Branch A retags nodes over time anyway. */
        v->in_reserve = (i >= NUM_PRIMARY) ? 1 : 0;
        v->fast_release_committed = 0;
    }
    g_voice_age_counter = 0;
}

/* SPEC.adoc S3.3.3: cents-to-Q12-ratio via the T2/T3 table decomposition.
 * The +-CENTS_CLAMP clamp below is NOT a reimplementation shortcut -- SPEC.adoc
 * S3.3.3 confirms it byte-for-byte in the real driver's CentsToRatio
 * (0x18e2c-0x18e6c, "clamp(cents, -4800, 4800) ... two-sided, sign-aware
 * clamp", matching T3's +-48-semitone table domain exactly). Left as-is
 * even after adding RPN1/RPN2 into the cents sum: the reference file's
 * actual RPN2 usage (+-1200/+-2400 cents) plus ordinary bend/base_cents
 * stays far under 4800, and raising this clamp would deviate from a
 * confirmed hardware behaviour, not fix a bug. Also applied to EG2's pitch
 * contribution in voice_step_eg2 -- same hardware bound, same table domain. */
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

/* SPEC.adoc S3.4.1: tc = lScale/65536.0; duration = 2^(tc/1200), trunc toward
 * zero (we keep this as a plain double, not truncated, since it feeds a
 * continuous-time envelope coefficient rather than an integer table). */
/* Where inside the current render call the event being dispatched actually
 * falls. smf.c sets it per event while draining a service block; anything
 * dispatched outside that path (direct msgs_midi injection) leaves it at 0,
 * i.e. "now". Only note-on reads it -- see Voice::start_delay. */
static uint32_t g_event_offset = 0;

void voice_set_event_offset(uint32_t frames) { g_event_offset = frames; }

static double timecents_to_seconds(int32_t tc) {
    if (tc == (int32_t)0x80000000) return 0.0;
    double t = (double)tc / 65536.0;
    return rt_pow(2.0, t / 1200.0);
}

/* The one duration-scaling rule the note-setup segment configurator applies,
 * SPEC.adoc S2.4.3 / S5.1.2 `[A:0x198e1]`,`[A:0x1991a]`. Two art1 rows use
 * it: usSource==3 KEYNUMBER -> EG decay (higher notes decay faster, 169 of
 * gm.dls's 235 instruments) and usSource==2 KEYONVELOCITY -> EG attack
 * (louder notes attack faster, 27 instruments). `depth` is the stored high
 * word of lScale -- the full-scale offset in cents -- and `src` is the raw
 * key or velocity. A sentinel duration (nothing authored) stays a sentinel.
 *
 * /127, not the DLS-1 spec's /128: `0x198b6` computes the term with an
 * `imul` by the source byte and an `idiv` by 0x7f, truncating toward zero.
 * That divisor was `[I]` and swept as /128 before the configurator was
 * disassembled; probe 35 measures 126.5 +/- 0.8 and cannot separate the two.
 * The term ALONE is then clamped to +/-4800 cents by the 2^(x/1200)
 * evaluator 0x18e1c that consumes it (`[A:0x18e2c]`,`[A:0x18e63]`) -- the
 * clamp is on the offset, not on the summed duration, which is why it is
 * applied here and not after the sum. The driver multiplies the duration by
 * 2^(term/1200) rather than summing timecents; identical quantity. */
static int32_t scale_tc_by_source(int32_t tc, int16_t depth, int src) {
    if (tc == (int32_t)0x80000000 || depth == 0) return tc;
    int32_t cents = (int32_t)depth * (int32_t)src / 127;
    if (cents > 4800) cents = 4800;
    if (cents < -4800) cents = -4800;
    return tc + cents * 65536;
}

/* LFO rate, SPEC.adoc LFO section `[M: probe 06]` -- derived, not fit: the
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
 * (SPEC.adoc S3.4.2: release shape confirmed exponential). SPEC.adoc S3.4.1
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
 * reference. SPEC.adoc S5.6 independently corroborates the same "reaches full
 * silence AT the nominal duration" shape from a wholly separate measurement
 * (probe 18_key_groups fast/choke release: measured 70.0ms to full silence,
 * matching the 70ms rate-clamp constant directly, not ~7.6x that).
 *
 * ponytail: SPEC.adoc does not state the reference dB span the original
 * driver's real consumption code (S3.4.2/Part 5 +0x13c, outside every PAGE
 * range this project examined) calibrates against -- that exact formula is
 * [O]. This uses a "decays 100dB over exactly `seconds` seconds" calibration
 * (the widely-used DLS-1/SF2 convention for this exact parameter), which
 * matches the S5.6 measurement almost exactly. Ceiling: best-effort
 * external-convention fit, not a byte-confirmed formula -- upgrade path is
 * locating the real consumption code if it ever becomes available. (The
 * "~2.9x still off for the EG1 decay segment" caveat that used to sit here
 * was decay key-follow, now supplied per-key by scale_tc_by_source above.) */
/* Rate multipliers, overridable with -D for sweeps.
 *
 * DECAY_RATE_MULT is `[M: probe 35]`, not a fit. Once key-follow supplied the
 * per-key decay DURATION, what was left between this project and the
 * reference was a single scale factor on the decay SHAPE, uniform across
 * every key and instrument: probe 35's reference decays at 0.965x this
 * project's rate, mean over all 17 notes, sd 0.005 (three instruments, keys
 * 24-96, rates spanning 3.8-27 dB/s -- a 7x range, with no trend against
 * either key or instrument). That is 29 standard errors from 1.0, so the
 * decay segment is "96.5dB over `seconds`", not the 100dB the shared
 * exp_coef comment above assumes.
 *
 * NOT swept to this value -- the corpus cannot resolve it (0.9633/0.965/0.97
 * all land within 0.05dB of each other on the mean). It is the direct
 * measurement, shipped over the marginally better corpus number on purpose.
 * Probe 35 itself: -36.82 -> -43.58dB; corpus mean -28.36 -> -28.85dB.
 *
 * Note 96.5 ~= 96.33 = a 16-bit floor (20*log10(2^16)). That would be a
 * tidier constant than a measured 0.965 and sits 1.4 sd from it, but this
 * project has NOT confirmed it -- do not "clean it up" to 0.9633 without a
 * measurement that separates the two. RELEASE_RATE_MULT is untouched: probe
 * 35 measures decay only, and S5.6's 70ms fast-release still matches 100dB.
 * See FITTED.md Entry 15 and SPEC_LOG.adoc #15.
 *
 * SUPERSEDED 2026-07-26 by SPEC.adoc S5.1.2.1: this was a rate-multiplier on
 * the OLD asymptotic approach-to-target decay shape, entangled with that
 * shape's own "100dB over `seconds`" calibration. The decay segment is now a
 * geometric ramp whose coefficient is derived directly from
 * env_sustain_level and the already-rescaled decay-to-sustain duration (see
 * voice_note_on) -- there is no free rate parameter left to scale, both
 * endpoints are pinned by the target/duration formulas. Macro removed rather
 * than left dead; probe 35's 0.965 measurement above stays as history. */
#ifndef RELEASE_RATE_MULT
#define RELEASE_RATE_MULT 1.0
#endif
/* Floor (seconds) on the ordinary note-off release duration only -- never
 * the fast/choke path (see start_release() below). [F:fitted] SPEC.adoc
 * S5.6/S3.8.2 state the only documented minimum-release mechanism (the 70ms
 * rate clamp at 0x19834) is reachable only from fast-release/choke path
 * 0x19aa4 and never from ordinary note-off 0x19a2c; there is no [A] floor on
 * the ordinary path. Value 0.060 (60 ms) fitted to corpus-wide sweep: floors
 * of 20/40/60/90 ms gave mean spectral residuals -9.7388 / -9.7569 / -9.7693
 * / -9.6624 dB vs baseline -9.7206 dB (more negative better); 60ms optimal.
 * 90ms contaminated by alignment flip on probe 31, multiple probes turned
 * around by 90ms. GENERAL SERUM envelope r: 0.8673 (no floor) -> 0.8684 at
 * 60ms. field/corridor.mid: 1,159,168 frames (untruncated) at every floor
 * tested, confirming SPEC_LOG.adoc #15 (release truncation) does not recur.
 * Corroborated: reference release measurement on a hand-cut excerpt shows
 * its authored 5-6ms releases need
 * ~6-11x stretch; 60ms sits inside. artifacts/probes/04_envelope.mid piano patch (990ms
 * authored release): byte-identical renders, untouched. Overridable -D. */
#ifndef RELEASE_FLOOR_S
#define RELEASE_FLOOR_S 0.060
#endif

/* The fast/choke-release duration clamp, `[A:0x1987f]`. Read as 70 ms from
 * SPEC.adoc S5.6, which is what the 0x46 in `0x19834` looks like at a glance.
 * It is not milliseconds: `0x19aa4` passes the SAMPLE RATE as the argument
 * (`[A:0x19acd]`, device+0x0 = 22050 from `[A:0x12b3e]`), and `0x19834` does
 * `div $0x46` on it, clamping the segment duration to rate/70 SAMPLES = 315 at
 * 22050 = 14.29 ms. A fifth of what we had.
 *
 * That mattered well beyond the choke itself. voice_topup_reserve's Branch B
 * marks voices for this release and does NOT decrement the shortfall it is
 * working against (`[A:0x12b6a]`: only Branch A touches +0x1b8), so the next
 * call recomputes the same shortfall -- and if a marked batch cannot drain and
 * be recycled before that call, every call marks another batch until the pool
 * is gone. At 70 ms it could not, which is why the top-up cadence had to be
 * fitted to a slow ~93 ms period instead of the driver's own once-per-buffer.
 * At 14.29 ms it drains inside one block: probe 20_voice_count at the true
 * cadence scores -24.00 with this value against -17.25 with 70 ms, restoring
 * S5.5's measured "exactly one batch per saturation".
 *
 * This is a bound on TIME TO SILENCE, not on rate: `0x19834` clamps a
 * duration that `0x197dc`'s formula already scaled by the voice's current
 * level, so a choked voice reaches -96dB within 14.29 ms of the choke from
 * wherever it happened to be. start_release() below turns that back into a
 * rate. Probe 18's reference choke falls (90%->10%) measure 30-52 ms against
 * our 67-96 ms at 70 ms; that measurement is contaminated by the closed hat
 * sounding through it, so it bounds the direction rather than the value.
 * Overridable -D. */
#ifndef FAST_RELEASE_S
#define FAST_RELEASE_S (1.0 / 70.0)
#endif

/* Audible-level floor for release finish detection, linear. GAIN_CEILING
 * (~0.5) is a voice's structural maximum gain, so 1e-4 here is -74dB below
 * a full-scale voice -- SPEC.adoc's -80dB `[A:0x19733]` constant read against
 * a gain-inclusive envelope. See voice_step_envelope's ENV_RELEASE case for
 * why the bare-EG floor alone is not enough. Overridable -D. */
#ifndef AUDIBLE_FLOOR
#define AUDIBLE_FLOOR 0.0001
#endif

/* The full span of the driver's envelope scale, SPEC.adoc S3.4.2
 * `[A:0x1685e]`. Both EGs carry their level as a position on a normalized dB
 * scale whose ends are fixed by Table C (0x1a9d8): 1000 == 0dB, 0 == -96dB.
 * Every segment is a straight line on that scale, so an authored duration
 * always means "traverse 96dB", and a release from a partway-down level
 * covers proportionally less of it in proportionally less time -- which is
 * exactly what a fixed-rate exponential does, so nothing here has to track
 * the level explicitly (SPEC.adoc S5.6).
 *
 * Was 100dB, which is where the 4.2%-too-fast release tails came from.
 * Probe 35 had already measured the decay segment at 96.5dB and left it [O]
 * against the 96.33dB 16-bit floor; it is neither, it is 96 by construction
 * and it governs the release as well as the decay. */
#define ENV_SPAN_DB 96.0

/* SPEC.adoc T.4/S3.4.2: the original reads the ATTACK segment's level from
 * Table C (`g_table_envshape`, 0x1a9d8), a 201-entry int16 LUT indexed by
 * trunc(elapsed*1000/attackDuration)/5 -- i.e. quantized to at most 201
 * distinct steps. That is algebraically the SAME curve the continuous ramp
 * below produces (both are ratio <-> 1000*(1+20*log10(ratio)/96), one
 * direction and its inverse) -- not a shape difference, only quantization.
 * Default OFF: SPEC_LOG.adoc item46 measured this against the corpus
 * (04_envelope, 32_ramp_shape, 41_sustain_decay_curve, 44_release_shape,
 * and the 68-item MEAN) -- none of the targeted items improved and the
 * mean moved marginally worse, so the continuous ramp ships as the
 * default. Overridable -D for A/B (artifacts/score.py MSGS_RENDER=...). */
#ifndef ENV_ATTACK_TABLE_C
#define ENV_ATTACK_TABLE_C 0
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
    return rt_pow(10.0, (-(ENV_SPAN_DB / 20.0) * mult) / samples);
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
 * here -- see FITTED.md before re-attempting this.
 *
 * SUPERSEDED 2026-07-26: do not re-attempt it. The 2.85x was decay-time
 * KEY-FOLLOW at note 60 mistaken for a constant -- see scale_tc_by_source
 * above, which now supplies it per-key from gm.dls's own data. That is why
 * the constant fixed note 60 and regressed everything else. */

/* SPEC.adoc S4.4: pitch bend -> cents is synth_pitch_bend_cents(), re-read
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
 * into base_cents in voice_note_on instead (SPEC.adoc S4.4, `[M: probe 23]`). */
/* Pitch-LFO (vibrato) depth, SPEC.adoc LFO section `[M: probe 06]`: CC1
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
 * changing any rendered sample. SPEC.adoc S6.6 explicitly marks the envelope
 * generator's own cadence `[O]` and warns against inferring it from the
 * mixer's ramp_period, so this is a documented choice, not a recovered one.
 * Currently tied to render.c's LFO_UPDATE_FRAMES (voice.h) via this alias --
 * kept as its own name since EG2's cadence has been a live open question
 * (see above) and may need to diverge from the LFO's again. */
#define EG2_BLOCK_FRAMES LFO_UPDATE_FRAMES
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
 * SPEC.adoc S6.6 marks the envelope generator's own behaviour `[O]`. */
#ifndef EG2_LINEAR_SEGMENTS
#define EG2_LINEAR_SEGMENTS 1
#endif

/* "Close enough" threshold for snapping a decaying/releasing envelope level
 * to its target (sustain level or zero) instead of asymptoting forever. */
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

/* SPEC.adoc S6.6/S6.4.1 `[A]`: the mixer holds the phase step in a "ramp
 * accumulator" re-derived from a caller-supplied LINEAR step every
 * `ramp_period` samples, held constant between refreshes -- not written
 * directly (this project's prior defect, SPEC_LOG.adoc #19) and not a
 * one-pole exponential (that shape is a separate, already-fitted stand-in
 * used only for GAIN, see render.c's GAIN_SMOOTH_ALPHA / FITTED.md Entry 4;
 * probe 28/32 already showed a one-pole is the wrong shape there too, but
 * this pass changes pitch only, per the assignment's own sequencing).
 *
 * What determines the caller's slope between calls is genuinely `[O]`
 * (S6.6), so the DURATION is measured, not derived. `[M: probe 33]`, every
 * condition the probe offers, read off artifacts/probe-results/33.flac
 * alone (the reference is the ground truth here; the render is not
 * consulted). 10-90% transition time, ensemble-averaged over the 8 reps:
 *
 *   step      +-200c   +-600c   +-2400c  +-4000c   6000c (section C)
 *   ref 10-90  23-24ms  24ms     21-22ms  20-22ms   29-31ms
 *
 * FLAT across a 30x range of step sizes -- so the ramp is fixed-DURATION,
 * not the fixed-RATE slew this project shipped before (SPEC_LOG.adoc #19,
 * FITTED.md Entry 9). A rate law predicts ~2ms for the 200c step and
 * ~400ms for the 6000c one; neither is observed. Section C's uptick to
 * ~29ms is confounded (bigger step AND a mid-note re-aim rather than a
 * fresh bend on a settled note), so it does not carry the fit.
 *
 * SHAPE is linear, same measurement: the 50% crossing lands at
 * (t50-t10)/(t90-t10) = 0.43..0.54 in every condition, against 0.50 for a
 * linear ramp and 0.27 for a first-order exponential approach.
 *
 * 10-90% covers 80% of a linear ramp, so 22ms of 10-90% is ~27.5ms total;
 * the value below is the 10-90% figure the corpus also prefers, and the
 * two agree to well inside the ensemble spread. Estimators: heterodyne
 * (carrier at the key's centre frequency, low-pass, unwrap+differentiate)
 * for the +-200c/+-600c steps, where FFT bin resolution is far too coarse;
 * per-cycle zero-crossing for the big steps, where a heterodyne's target
 * leaves the passband. FFT peak tracking, the obvious third choice, is
 * resolution-bound on both ends and under-reads a fast edge by ~50%.
 *
 * Two things this replaces, both now known to be wrong rather than merely
 * unconstrained:
 *
 *  - Entry 9's single -24-semitone fit point (16.51ms, sd 3.90ms) came from
 *    an FFT tracker and is the under-read above; the same condition
 *    measures 21.3/21.7ms here.
 *  - The old rate scaled off the voice's phase_step AT RAMP START, which
 *    made upward moves slow and downward moves fast. Probe 33's
 *    C_square_17ms shows the consequence directly: the render's swing sat
 *    entirely on one side (+396..+2645c) instead of straddling +-3000c. A
 *    duration law is symmetric by construction.
 *
 * WHAT THE DURATION IS ATTACHED TO matters more than its value, and this is
 * where an earlier version of this code was wrong. S6.6 describes a slope
 * REFRESHED ON A FIXED GRID -- "re-derived from a caller-supplied LINEAR
 * step every `ramp_period` samples, held constant between refreshes" -- not
 * a countdown started by each arriving message. The two agree on isolated
 * steps (which is all probe 33 sends) and diverge completely under dense
 * automation:
 *
 *   - Restarting a fixed duration per message never converges when messages
 *     arrive faster than the duration. It degenerates into a first-order
 *     lag whose error GROWS with message density. Measured: field/
 *     Kot_and_A64-GENERAL_SERUM.mid carries 36,763 bend events at a 6.7ms
 *     median spacing and tests/radio.mid 371 events at 8.0ms, both far
 *     inside a ~27ms duration -- the reference's kick pitch-drop is visibly
 *     and audibly quicker than a restarting ramp can render, and radio's
 *     sweeps come out curved where the reference's are straight.
 *   - A slope held on a grid lags by at most one period regardless of
 *     density, and the trajectory stays piecewise linear.
 *
 * tests/lazers.mid is the same gesture as GENERAL_SERUM's, slowed down: its
 * bends are 41.7ms apart, WIDER than the period, so a restarting ramp very
 * nearly completes and the file looks fine. It cannot distinguish the two
 * models -- do not fit against it. Fit against the dense files.
 *
 * The grid also removes the need to exempt LFO and EG2 from the ramp. They
 * retarget every LFO_UPDATE_FRAMES sub-chunk; a restarting ramp could never
 * converge on them (vibrato collapsed 10.6dB on probe 06 and had to be
 * special-cased out), whereas a held slope converges on a moving target
 * fine, so they go back through the one pitch path the driver has.
 *
 * ponytail: one constant, no ceiling, no magnitude keying, no per-source
 * exemption -- the measurement says duration does not depend on step size,
 * so nothing here should either.
 *
 * PERIOD, `[M: probe 32 + probe 33, per-rep]`: 512 frames, 23.22ms. Two
 * independent readings land on it.
 *
 *  - Duration. Both probes' references glide a fixed 10-90% of 16.4-16.8ms
 *    for every step from 100 to 600 cents, 50% crossing at 0.46-0.51
 *    (linear), i.e. a full ramp of ~20.5-21ms. Measured PER REP, not
 *    ensemble-averaged: the reference's bend-application time jitters +-18ms
 *    rep to rep, and averaging edges that are each ~16.5ms wide but mutually
 *    offset by up to 18ms manufactures a ~23ms composite that is the JITTER,
 *    not the ramp. An earlier pass of this project reported exactly that
 *    artifact (23.2/24.3ms) and it is what the 27.5ms this replaces was fit
 *    to.
 *  - Grid. Those same transition times concentrate on a period of 23.22ms
 *    (circular concentration R=0.83) == 1024 samples at the 44.1kHz the
 *    references were captured at == 512 at this driver's own 22050Hz. The
 *    reference also renders a minority of reps (18 of 48 in probe 32) as
 *    near-instant steps at the estimator's floor, clustered at one end of
 *    that period -- which is what a block grid does and a per-message
 *    countdown cannot do at all.
 *
 * The bimodality itself is `[O]`: a plain "reach the target by the end of the
 * block" model predicts a continuum of durations, and the measured ones are
 * tightly bimodal at 4.2 and 16.5ms with nothing between. Modelled here as
 * the uniform case; the instant minority is not reproduced.
 *
 * SHAPE: fixed horizon, not a grid. An earlier version of this file re-aimed
 * every voice on a free-running global clock -- one refresh per period, and
 * a mid-period message got whatever was left of that period. That made two
 * identical gestures traverse at different speeds depending only on where
 * the clock happened to be when the message arrived -- measured directly on
 * field/town.mid (two near-identical 200-cent bend gestures, ~13ms message
 * spacing each, landing at different phases of that clock: per-message
 * slopes differed by up to 2x between the two gestures with no musical
 * cause) and on the corpus as a spectral ripple at exactly the clock's
 * period (tests/radio.mid, 8.0 cents peak-to-peak at 14.50ms -- the period
 * to the resolution of the estimator). A held slope re-derived from the
 * CURRENT value every period was also tried (SPEC.adoc's own "re-derived ...
 * every ramp_period samples" read as literally as possible); at any period
 * other than the one chosen to size THIS message's ramp, it either
 * overshoots (short period, held past the point it should have stopped) or
 * degenerates into a never-settling exponential (FITTED.md Entry 12's
 * geometric ramp, shown to reproduce here at horizon==period). Every voice
 * instead now gets its own fixed HORIZON, sized once when a bend message
 * moves its target and left alone until it arrives -- no shared clock, no
 * periodic re-derivation, nothing to walk in and out of phase with the
 * music. voice_ramp_tick is a per-voice countdown that retires an arrived
 * ramp (render.c has no arrival test of its own); it no longer re-aims
 * anything.
 *
 * VALUE SHIPPED, and the one honest wart in this file: the corpus prefers
 * 320 frames (14.5ms), not the 448-512 the per-rep edge measurement implies,
 * and the shipped value is the corpus one. Swept against the dedicated bend
 * probes:
 *
 *   frames    192    256    320    384    448    512   (baseline: rate law)
 *   33_ramp -27.86 -31.45 -34.04 -32.75 -32.29 -30.39      -30.04
 *   05_bend -31.68 -28.95 -28.12 -28.33 -25.08 -27.14      -27.78
 *
 * 320 is a clean optimum on probe 33 -- the probe built for this question --
 * and beats the law it replaces by 4.0dB there, the largest improvement any
 * variant of this ramp has produced. It disagrees with the edge measurement
 * by ~40%, which is not resolved. Reasons to distrust the corpus side: the
 * two items driving the corpus-mean regression, 32_ramp_shape and
 * field/HueArme-Weekend, both have references whose capture alignment is
 * known bad -- 32.flac drifts 422ms over 158s NON-linearly (43ms residual to
 * a straight line, 55ms of spread within one 12-rep section), so no single
 * lag aligns it and score.py uses exactly one. Reasons to distrust the
 * measurement side: none found, but it rests on one analysis pass.
 * Re-derive both before treating 320 as recovered rather than fitted.
 *
 * NOT explained by any shape tried, this one included: field/town.mid's two
 * bend gestures glide at measurably different rates depending on direction
 * (~2.2 vs ~4.24 cents/ms) at nearly the same message spacing. That may be a
 * real rise/fall asymmetry or may be confounded with note age (the two
 * gestures sit at different offsets from note-on) -- not separated. A probe
 * pairing a rise and a fall at equal note age would settle it; until then
 * this constant is being asked to fit one number for what may be two. */
#ifndef RAMP_HORIZON_FRAMES
#define RAMP_HORIZON_FRAMES (320 * RESAMPLE_FACTOR)
#endif

/* Size the held slope to land on the target `frames` from now, and arm the
 * per-voice countdown that retires it. Integer truncation of the slope
 * leaves a residual; this voice will not be re-aimed again until its OWN
 * next bend message, so residue is not corrected before arrival the way a
 * shared periodic refresh would -- bounded by construction, since a slope
 * that truncates to zero snaps directly instead of crawling forever. */
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

/* render.c's per-sample accumulator has no arrival test (SPEC.adoc S6.6: the
 * mixer only ever consumes a caller-supplied slope, it has no notion of a
 * target) -- without this, a held slope keeps walking phase_step past the
 * target forever. Counts every active voice's ramp down and snaps it exactly
 * on expiry. Called AFTER render_voice for this sub-chunk, so a ramp expiring
 * partway through it still rendered every frame it owns first. */
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
    /* Two factors, deliberately: the note-on base (key offset, fine tune and
     * the latched RPN1/RPN2 master tune) goes through CentsToRatio's +-4800
     * clamp per SPEC.adoc S3.3.2/S3.3.3 and is latched in v->base_ratio_q12;
     * live modulation (pitch bend, LFO) is a SEPARATE clamped factor applied
     * outside it.
     *
     * `[M: probe 30]`. SPEC.adoc exhibits CentsToRatio only on the note-trigger
     * path and leaves the continuous-bend path unrecovered (SPEC_LOG.adoc
     * #17), so which side of the clamp bend lands on was undetermined. Probe
     * 30 settles both halves against the real driver: section B (RPN2 +24, no
     * bend) collapses keys 119 and 127 to one frequency, 3028.8 Hz, in the
     * REFERENCE as well as here -- so the latched tune really is inside the
     * clamp. Section C adds a bend sweep to that same saturated base and the
     * reference moves (1685/1688 Hz) while a single-sum implementation stays
     * pinned at 3028.8 Hz -- so bend is outside it. */
    int32_t bend_cents = synth_pitch_bend_cents(v->channel); /* SPEC.adoc S4.4 */
    int32_t mod_cents = bend_cents
                      + voice_lfo_cents(v) /* SPEC.adoc LFO section, `[M: probe 06]` */
                      + voice_step_eg2(v); /* SPEC.adoc S2.4.3 `[A:0x15838]`, SPEC_LOG.adoc #20 */
    uint64_t raw = (uint64_t)v->wave->sample_rate * (uint64_t)v->base_ratio_q12;
    raw = (raw * (uint64_t)(uint32_t)cents_to_ratio_q12(mod_cents)) >> 12;
    uint32_t new_target = (uint32_t)(raw / RENDER_RATE);

    /* Only BEND is ramped. The period above is measured on bend steps and
     * nothing else -- probe 33 and probe 32 send no LFO and no pitch envelope,
     * so what the driver does with those is `[O]`, and routing them through
     * this ramp is a guess that measures badly (06_modwheel -3.2dB,
     * 26_other_gains -1.3dB, both files containing no bend at all).
     *
     * Carried through by shifting phase_step and its target by the SAME
     * amount: the LFO/EG2 delta lands immediately while whatever error the
     * in-flight bend ramp is still carrying is preserved, so the two do not
     * fight over one accumulator. */
    int bend_moved = (bend_cents != v->bend_cents_applied);
    if (!bend_moved) {
        int64_t moved = (int64_t)v->phase_step
                      + ((int64_t)new_target - (int64_t)v->phase_step_target);
        v->phase_step = (uint32_t)(moved < 0 ? 0 : moved);
    }
    v->bend_cents_applied = bend_cents;
    v->phase_step_target = new_target;

    /* Re-aim NOW, the moment the bend moves, over a FIXED horizon -- not the
     * remainder of some shared clock's current period. Every bend gesture of
     * a given size therefore traverses at the same speed regardless of when
     * it happens to land, which a shared periodic clock cannot promise (see
     * RAMP_HORIZON_FRAMES above). No dead time either: a fresh voice's first
     * bend is re-aimed here on the same call that moves its target, not left
     * for a tick to notice later. */
    if (bend_moved) ramp_reaim(v, RAMP_HORIZON_FRAMES);
}

/* Advances every active voice's pitch-LFO phase by freq_hz*frames/RENDER_RATE
 * cycles, gated on lfo_delay_s (SPEC.adoc LFO section, `[M: probe 06]`).
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

/* Pan law, SPEC.adoc S3.6. SHAPE and FLOOR are both `[M: probe 25]`'s own
 * 9-anchor measured table, consumed here directly (PAN_L_HDB/PAN_R_HDB
 * below); between anchors the curve is `[F]` per SPEC.adoc S3.6's own text
 * ("linear interpolation in the hundredths-of-a-dB domain reproduces the
 * anchors exactly"). SPEC_LOG.adoc item45 has the measured error this
 * replaced: up to 3.04 dB on shape (the near channel rising above unity
 * instead of holding at 0 dB) and ~1.8 dB on the hard-pan floor.
 *
 * This is NOT the disassembly-derived linear/sqrt table `g_table_lin`
 * (SPEC.adoc S3.6's `gainA`, reverse-indexed by 127-pan) this function used
 * until item45 -- that table corroborated the pan law's overall SHAPE
 * against probe 07 well enough to ship (SPEC_LOG.adoc Entry 7), but its own
 * `- g_table_lin[63]` re-centering term let the near channel rise up to
 * +3.04 dB above unity instead of holding exactly 0 dB at every anchor, as
 * SPEC.adoc S3.6 requires ("true unity on both sides simultaneously").
 * g_table_lin is no longer consumed by src/engine -- its disassembly
 * reading stays in SPEC.adoc and its byte-exact contents are still
 * asserted by unit.c's S T.3 test, so it is kept, just unused here.
 * `gainB` is NOT the squared table `g_table_vel` either, per SPEC.adoc
 * S3.6's disassembly reading: that predicts -11.90 dB at center pan where
 * probe 07 measures -3.72 dB; REFUTED, see SPEC_LOG.adoc S9. (Neither
 * branch of the law below touches g_table_vel, so this refutation no
 * longer has a "which table" question to resolve -- it is kept only
 * because it is still why SPEC.adoc's own `[O]` two-table asymmetric-law
 * reading, `0x19c12`-`0x19c2a`, is not implemented as written; see the
 * standing `[O]` at SPEC.adoc S3.6.)
 *
 * The anchor-table/lerp scheme below resembles one this project shipped
 * once before and then superseded (SPEC_LOG.adoc Entry 5,
 * `artifacts/probes/25_pan_law.mid`) -- it is NOT that same mistake
 * recurring. Entry 5 was rejected because, at the level it was fit
 * against (Sine patch, CC7=127), probe 25's flat centre plateau was
 * GAIN_CEILING saturation (~4.78 dB above the 32767/65536 ceiling), not
 * the pan law itself (SPEC_LOG.adoc Entry 7). item45's 9-anchor sweep uses
 * CC7=40/CC11=127 instead (src/unit.c's own S3.6 tests) and reaches a
 * centre gain of only 0.099312 -- well clear of GAIN_CEILING (0.499985),
 * confirmed by the test's own diagnostic before it asserts anything
 * (src/unit.c ~1602-1630). This time the plateau IS the pan law. */

/* Per-voice output ceiling, SPEC.adoc S6.4.5 -- `[A]`, derived from the two
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
 * how the intermediate `<<8>>5` scaling maps hundredths-of-a-dB attenuation
 * to the raw register (SPEC.adoc S6.4.5's hdB->raw-gain conversion table,
 * `[A:0x18dea]`, now resolved -- its own ceiling independently lands on this
 * same bound to ~0.002dB) -- that mapping is irrelevant to *this* bound,
 * which falls straight out of "signed 16-bit register, Q16 multiply" alone.
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

/* SPEC.adoc S3.5 (squared volume law via g_table_vel), S3.10 (attenuation
 * sum), S3.6 (pan law). Re-read live here (never baked into a frozen value)
 * so a CC7/CC10/CC11 change -- or a Master Volume SysEx -- arriving while a
 * note is held reaches it, mirroring voice_update_pitch's treatment of
 * pitch bend. v->atten_const_hdb (velocity attenuation + region/wsmp
 * attenuation, set once at note-on since neither changes for the voice's
 * life) is the only part NOT recomputed here. This is the single place
 * gain_l/gain_r are computed -- voice_note_on calls it too instead of
 * duplicating the math. */

/* Pan law breakpoints, SPEC.adoc S3.6 [M: probe 25]: dB (hundredths of a
 * dB) relative to centre, at the nine measured CC10 anchors, taken
 * verbatim from SPEC's own table. The near channel is 0 (true unity) at
 * every anchor; only the far channel's column is non-zero. See the pan-law
 * comment above for why this replaces g_table_lin here. */
static const uint8_t PAN_CC10[9]  = { 0, 16, 32, 48, 64, 80, 96, 112, 127 };
static const int16_t PAN_L_HDB[9] = { 0, 0, 0, 0, 0, 0, -141, -452, -2021 };
static const int16_t PAN_R_HDB[9] = { -2020, -420, -120, 0, 0, 0, 0, 0, 0 };

/* Linear interpolation between the anchors above, in the hundredths-of-a-dB
 * domain, per SPEC.adoc S3.6's own stated treatment of the unmeasured span
 * between them ("linear interpolation ... reproduces the anchors
 * exactly"). */
static int32_t pan_lerp_hdb(int pan, const int16_t *hdb) {
    for (int i = 0; i < 8; i++) {
        if (pan <= PAN_CC10[i + 1]) {
            int32_t x0 = PAN_CC10[i], x1 = PAN_CC10[i + 1];
            int32_t y0 = hdb[i], y1 = hdb[i + 1];
            return y0 + (int32_t)(((int64_t)(y1 - y0) * (pan - x0)) / (x1 - x0));
        }
    }
    return hdb[8];
}

/* SPEC.adoc S3.5 [A:0x19bcd]/S6.4.5 [A:0x18dea]: the driver folds a literal
 * +1200 (12.00dB) into atten_const_hdb (voice_note_on, below) and its
 * hdB->raw-gain conversion table's zero point (atten_hdb==0) is its own
 * ceiling, TABLE[0] == 4095 == GAIN_CEILING's numerator to within ~0.002dB
 * -- not a literal 1.0. Anchoring gain_linear on GAIN_CEILING instead of
 * 1.0, paired with the +1200, was meant to reproduce that. Measured against
 * the corpus and REJECTED (SPEC_LOG.adoc item47: mean residual moved
 * +2.90dB worse, 33 items regressed >1dB, and the pair's own falsifiable
 * v=96 knee prediction overshot to v=80). This engine's atten_hdb
 * composition is not yet on the same footing as the driver's, so the pair
 * does not currently reproduce it; kept behind ATTEN_PLUS_1200 as a single
 * -D away for a future pass rather than lost. The pair is a pure uniform
 * level shift of +5.98dB (+1200 less GAIN_CEILING's -6.02dB) and nothing
 * else -- cancelling that net reproduces the default build exactly, and
 * the corpus optimum sits near +3dB, not +6dB (SPEC_LOG.adoc item48). Default build uses the
 * pre-item47 form with the GAIN_TRIM_DB hook (always 0.0, SPEC_LOG.adoc
 * items 39/40). */
#ifndef ATTEN_PLUS_1200
#define ATTEN_PLUS_1200 0   /* SPEC S3.5's [A:0x19bcd] +1200 paired with the GAIN_CEILING
                               re-anchor; measured and rejected by the corpus gate, see
                               SPEC_LOG item47. Kept so the pair is one -D away for the
                               follow-up pass, not an edit. */
#endif

void voice_update_gain(Voice *v) {
    if (!v->active) return;
    int32_t chan_vol = g_table_vel[g_channels[v->channel].volume];
    int32_t expr = g_table_vel[g_channels[v->channel].expression];
    int32_t atten_hdb = g_master_vol_hdb + chan_vol + expr + v->atten_const_hdb;
#if ATTEN_PLUS_1200
    double gain_linear = GAIN_CEILING * rt_pow(10.0, (double)atten_hdb / 2000.0);
#else
#ifndef GAIN_TRIM_DB
#define GAIN_TRIM_DB 0.0
#endif
    double gain_linear = rt_pow(10.0, ((double)atten_hdb / 100.0 + GAIN_TRIM_DB) / 20.0);
#endif

    int pan = (int)g_channels[v->channel].pan + v->artic->pan_cb;
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;
    int32_t gainA_hdb = pan_lerp_hdb(pan, PAN_L_HDB);
    int32_t gainB_hdb = pan_lerp_hdb(pan, PAN_R_HDB);
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

/* SPEC.adoc S5.7, 0x12426 analogue -- used ONLY by voice_topup_reserve's
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

/* SPEC.adoc S5.7, 0x124a8 analogue -- used ONLY by voice_note_on's own final
 * fallback, reached when BOTH a free primary and a free reserve voice were
 * unavailable. NOT symmetric (SPEC.adoc S5.7): a candidate that is still HELD
 * is compared to `best` purely on age, regardless of whether `best` is
 * itself released -- a released `best` is not protected from being
 * displaced by an older held candidate. Never reads fast_release_committed
 * (SPEC.adoc: "0x124a8 never reads or writes +0x138"), so it CAN immediately
 * repurpose a voice the top-up had just marked mid-fade -- intended, see
 * SPEC.adoc S5.6. */
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
    if (fast) v->fast_release_committed = 1; /* SPEC.adoc S5.6 +0x138: set
        unconditionally before any branch in the real ScheduleFastRelease;
        our start_release already collapses ScheduleFastRelease's two
        internal paths into one (a pre-existing, separate simplification),
        so `if (fast)` is the closest honest match to "unconditional, but
        only on the fast-release path" given that collapse. Ordinary
        note-off (fast==0) never touches this field (SPEC.adoc S5.6: "note-off
        never touches +0x138"). */
    v->held = 0;
    v->sustain_deferred = 0;
    v->env_stage = ENV_RELEASE;
    double authored_s = timecents_to_seconds(v->artic->eg1_release_tc);
    double rel_s = authored_s;
    if (fast) {
        /* SPEC.adoc S5.6 `[A:0x197dc]`,`[A:0x19834]`: both configurators set
         * relDur = authored * level/1000 with the level on the 96dB scale,
         * and the fast one then caps relDur at FAST_RELEASE_S. The scaling
         * alone is a no-op for us -- it is what makes the driver's release a
         * fixed dB RATE, which exp_coef_scaled already is -- so the whole
         * mechanism reduces to what the cap becomes once the scaling is
         * undone: a voice `lvl` of the way up the scale has only 96*lvl dB to
         * fall, so covering it in FAST_RELEASE_S means a rate of
         * FAST_RELEASE_S/lvl. Quiet voices get a gentler ramp over the same
         * short window, never a steeper one, and the patch's own release is
         * the floor (min(authored, ...) is the cap failing to bind). */
        double lvl = 1.0 + 20.0 * rt_log10(v->env_level > 1e-12 ? v->env_level : 1e-12) / ENV_SPAN_DB;
        if (lvl > 1.0) lvl = 1.0;
        rel_s = (lvl > 1e-6) ? FAST_RELEASE_S / lvl : authored_s;
        if (rel_s > authored_s) rel_s = authored_s;
    }
    if (!fast && rel_s < RELEASE_FLOOR_S) rel_s = RELEASE_FLOOR_S;
    v->env_release_coef = exp_coef_scaled(rel_s, RELEASE_RATE_MULT);
    /* EG2 releases on BOTH note-off paths. SPEC.adoc Part 7 records that the
     * choke/steal routine `0x19aa4` "shares only the pitch-EG release call"
     * with ordinary note-off `0x19a2c` -- the 70 ms rate clamp applies to the
     * amplitude segment specifically, NOT to this one, so no `fast` handling
     * here is deliberate. */
    if (v->eg2_stage != ENV_IDLE) v->eg2_stage = ENV_RELEASE;
}

/* SPEC.adoc S5.4 `[A]` mechanism: tops the reserve tier back up to NUM_RESERVE
 * free voices. Branch A moves already-free PRIMARY nodes to the reserve tag
 * (never touches an active voice, never allocates new capacity). Branch B
 * runs ONLY if primary was ALSO empty: it marks up to the remaining shortfall
 * of active voices for an accelerated release via the SAME start_release(v,
 * 1) fast-release path used by the key-group choke and same-note retrigger
 * (no second mechanism) -- it does NOT free anything synchronously; the
 * marked voice keeps rendering (and keeps being "active") until its own
 * envelope finishes draining.
 *
 * CADENCE `[F:fitted]`: a fixed wall-clock period, TOPUP_INTERVAL_FRAMES.
 * SPEC.adoc S5.4 is explicit that the real TopUpReserve runs "exactly once per
 * call" to the event dispatcher (0x12bd6), whose sole caller is the
 * per-BUFFER service routine (0x13054) -- i.e. once per audio service tick,
 * a wall-clock period. Earlier versions of this project instead tied the
 * cadence to render.c call structure (once per render_frames() call, or once
 * per LFO_UPDATE_FRAMES sub-chunk). Both are event-density-dependent, not
 * time-dependent: smf.c splits a render_frames() call at every dispatched
 * MIDI event, so on a dense sequence the top-up ran up to once per FRAME.
 * That is fatal, because Branch B is a feedback loop -- a marked voice keeps
 * rendering for its full ~70ms fast release before it can be reaped and
 * recycled, so any cadence faster than that drain time marks another
 * TOPUP_RESERVE_COUNT voices before the previous batch has freed anything,
 * and the whole pool is committed to release within a handful of calls.
 * Measured on field/HueArme-Weekend.mid: ~800 top-up calls/second at t=23s,
 * Branch B firing 33 times in 100ms, active voice count reaching ZERO --
 * two audible total-silence dropouts (t=23.09-23.15s and t=23.44-23.51s)
 * that the reference does not have.
 *
 * SPEC.adoc S5.5's own `[M]` measurement bounds the period from the other
 * side: probes 20/21 (80 note-ons, no note-off) cut exactly 32 voices = 26
 * forced by pigeonhole + 6, i.e. Branch B contributes exactly ONE batch of
 * TOPUP_RESERVE_COUNT over an entire saturated 8-second run. That only holds
 * if a top-up cannot fire again until the batch it marked has drained and
 * been recycled -- so the period is at least the ~70ms fast-release time.
 * The real period IS now recovered, and it refutes itself. TopUpReserve runs
 * at `[A:0x12be7]`, once per 0x12bd6, which runs once per 0x13054
 * `[A:0x130af]`, which renders one KS buffer -- so the driver's true cadence
 * is once per service block, and the driver allocates 1024-frame buffers
 * `[A:0x184c0]`. Wired up at that cadence this model breaks the very probe
 * S5.5 bounds it with. Measured, corpus mean / 20_voice_count residual:
 * 256 frames (our block) -31.0087 / -17.25, 1024 (the driver's buffer)
 * -31.1163 / -22.95, this value -31.1440 / -24.14, against -31.1483 / -24.13
 * for the pre-segment build. So the recovered cadence is not usable AS a
 * cadence, which means something else in Branch B is wrong -- the likeliest
 * suspect is `need`: a voice Branch B marks stays `active` while it drains,
 * so it never reduces reserve_free, and the next call recomputes the same
 * shortfall and marks another batch. The driver's own reserveCount may be
 * incremented at MARK time rather than at recycle time, which would make one
 * batch per saturation event fall out naturally at any cadence. Not tested.
 * The `need` suspect above was WRONG: `[A:0x12b6a]` shows Branch B never
 * touching +0x1b8 either, so the driver recomputes the same shortfall exactly
 * as we do. What differs is the drain time -- FAST_RELEASE_S was 70 ms where
 * the driver's clamp is rate/70 samples = 14.29 ms (see that macro), so our
 * marked batch could not clear before the next call and cascaded.
 *
 * Correcting the clamp gets the true once-per-block cadence most of the way
 * back: probe 20_voice_count goes -17.25 -> -24.00 and the corpus mean
 * -31.0087 -> -31.1497. It is still not right. cli.c's --selftest, which
 * asserts S5.5's `[M]` invariant directly, reports 43 of 48 voices surviving
 * 80 held note-ons instead of 48: one batch too many is still being marked,
 * because 14.29 ms of drain does not fit inside an 11.6 ms block.
 *
 * That explanation was tried and is WRONG. The level scaling is implemented
 * now (start_release, `[A:0x197dc]`) on the theory that Branch B's comparator
 * picks the QUIETEST released voice first, so its victims would drain in a
 * fraction of the clamp. At the true 256-frame cadence the selftest still
 * reports 43/48, byte-for-byte the same as before: the saturation test holds
 * all 80 notes, so Branch B never has a released voice to prefer and every
 * victim it marks is at full level, where the clamp binds and the scaling is
 * a no-op. Whatever the cadence gap is, it is not drain time. Until it is
 * found this stays at the fitted period, which scores -31.1584 / -24.14, the
 * best of anything measured. */
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
        if (!victim) break; /* every active voice already committed to a
            fast release -- nothing left to mark this call; NOT a leak (see
            voice_note_on's own final fallback, which never reads this gate
            and can still repurpose a draining voice immediately). */
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

    /* Same-note retrigger scan, SPEC.adoc S5.6 ("the same-note retrigger scan
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

    /* SPEC.adoc S5.3: allocation always tries primary before reserve, and only
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

    v->in_reserve = 0;              /* SPEC.adoc S5.3: recycling always targets
        primary only -- reset here so whatever tier this voice occupied while
        free (or, for a stolen voice, whatever it was tagged before going
        active) is irrelevant the moment it's active again. */
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

    /* Pitch: SPEC.adoc S3.3.2-S3.3.4. base_cents excludes bend (SPEC.adoc S4.4)
     * -- that and other live modulation are re-read every block by
     * voice_update_pitch, not baked in here. RPN1/RPN2 master tune IS baked
     * in here (sampled once, at note-on): probe 23 measured it as latched,
     * not continuous -- see voice_update_pitch's comment (SPEC.adoc S4.4,
     * `[M: probe 23]`). RPN2 (Channel Coarse Tuning) is skipped for rhythm
     * parts -- RPN1 is not gated (SPEC.adoc S3.3.2). */
    v->base_cents = (int)r->fine_tune + (note - (int)r->unity_note) * 100
                  + (g_channels[channel].is_rhythm ? 0 : g_channels[channel].rpn2_coarse_cents)
                  + g_channels[channel].rpn1_fine_cents;
    /* Latched here and clamped once, `[M: probe 30]` -- see voice_update_pitch. */
    v->base_ratio_q12 = (uint32_t)cents_to_ratio_q12(v->base_cents);

    /* Pitch-LFO state, SPEC.adoc LFO section `[M: probe 06]`: rate/delay
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
     * voice (SPEC.adoc S6.6), not for note-on itself. */
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

    /* Volume law, SPEC.adoc S3.5 (see SPEC_LOG.adoc for the depth-sentinel-sign
     * resolution applied here; the attenuation unit is hundredths of a dB
     * throughout, S1.4.4/S3.10). Only the part
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
    /* SPEC.adoc S3.5/S3.10 sums only the region's own (wsmp) attenuation here
     * -- NOT a separate wave-level term ("region overrides wave", S2.6/
     * S3.3.2: every gm.dls region carries its own wsmp).
     *
     * Units: region.attenuation_hdb is already hundredths of a dB, the
     * same unit as velAtten/chanVol/expr (S3.5/T.2). The on-disk wsmp
     * lAttenuation/art1 lScale is 16.16 fixed point at 0.1 dB per integer
     * step; dls.c's `(lScale*10)>>16` conversion scales that to
     * hundredths, so it is summed in directly here with no further
     * conversion, per S1.4.4/S3.10. */
    /* SPEC.adoc S3.5 [A:0x19bcd]: the driver folds a literal +1200 (12.00 dB)
     * into this same sum, paired with voice_update_gain's GAIN_CEILING
     * re-anchor above -- see the ATTEN_PLUS_1200 comment there. Measured
     * and rejected by the corpus gate (SPEC_LOG.adoc item47); default
     * build omits it. */
#if ATTEN_PLUS_1200
    v->atten_const_hdb = scaled + (int32_t)r->attenuation_hdb + 1200;
#else
    v->atten_const_hdb = scaled + (int32_t)r->attenuation_hdb;
#endif

    /* Pan, SPEC.adoc S3.6 (L/R assignment is an inference; see SPEC_LOG.adoc).
     * The region's own pan offset (artic->pan_cb) is fixed; the channel's
     * live CC10 pan is combined with it fresh in voice_update_gain. */
    voice_update_gain(v);
    v->gain_l = v->gain_l_target;
    v->gain_r = v->gain_r_target;
    v->amp_left = 0;
    v->amp_step_l = v->amp_step_r = 0.0;
    v->amp_retire = 0;
    v->start_delay = g_event_offset;
    /* amp_l/amp_r are PRIMED from the envelope's own starting level at the end
     * of this function, once that level is known -- not left at zero. The
     * driver primes its mixer the same way and for the same reason: `0x18fba`
     * called with nsamples==0 writes the gains straight into the ramp state
     * without rendering `[A:0x18fd9]`. Leaving them at zero instead makes every
     * instant-attack patch (attack_s <= 0 sets env_level = 1.0 below) fade in
     * across a whole segment -- measured as a 23 ms ramp-up on every note of
     * probe 14, 5x too quiet at 4 ms in. */

    /* Envelope (EG1, amplitude), SPEC.adoc S3.4 */
    double attack_s = timecents_to_seconds(
        scale_tc_by_source(r->artic->eg1_attack_tc, r->artic->eg1_attack_vel_tc, velocity));
    double decay_s = timecents_to_seconds(
        scale_tc_by_source(r->artic->eg1_decay_tc, r->artic->eg1_decay_kf_tc, note));
    /* SPEC.adoc S5.1.2 [A:0x18d3d-0x18d49 / 0x18b4a-0x18b8b]: at CONSUMPTION
     * time the real driver does not read the raw sustain permille as a
     * linear-amplitude fraction (that reading is only the on-disk STORAGE
     * format, S5.1 Part 2). The decay segment's advance routine treats it as
     * a progress-domain marker on the same 96dB linear-dB scale used for
     * attack/release: sustainLevel_hundredthsDb = sustainPermille*9.6-9600.
     * Clamp the raw permille first (defends against malformed art1 data,
     * same intent as the old post-division clamp below). */
    int32_t sustain_permille = r->artic->eg1_sustain_permille;
    if (sustain_permille < 0) sustain_permille = 0;
    if (sustain_permille > 1000) sustain_permille = 1000;
    double sustain_hundredths_db = (double)sustain_permille * 9.6 - 9600.0;
    v->env_sustain_level = rt_pow(10.0, sustain_hundredths_db / 100.0 / 20.0);
    if (v->env_sustain_level < 0.0) v->env_sustain_level = 0.0;
    if (v->env_sustain_level > 1.0) v->env_sustain_level = 1.0;
    /* SPEC.adoc S5.1.2 [A:0x19968-0x1997e]: note-on setup overwrites the
     * (velocity-scaled) decay duration with decay*(1000-sustainPermille)/1000
     * before the decay segment's advance routine ever consumes it -- the
     * decay segment only has to travel down to sustainPermille, not to 0,
     * so its real duration is shorter than the full authored decay time by
     * exactly that fraction. */
    double decay_to_sustain_s = decay_s * (double)(1000 - sustain_permille) / 1000.0;
    /* SPEC.adoc S5.1.2.1: the decay segment's SHAPE is a plain geometric ramp
     * (env_level *= env_decay_coef every sample), the same per-sample
     * mechanism ENV_RELEASE below already uses -- NOT the asymptotic
     * approach-to-target shape this project used pre-fix. S5.1.2.1 derives
     * progressPermille as linear in elapsed samples (a countdown), not a
     * ratio against a shrinking gap, and shows the geometric ramp reaching
     * env_sustain_level exactly at the rescaled duration matches the
     * reference's gradual settle where the asymptotic shape flattened out
     * too early. The ramp always starts from env_level==1.0 (attack lands
     * there exactly), so env_decay_coef = sustain_level^(1/N) reaches
     * env_sustain_level exactly N samples later; env_decay_samples_left is
     * the exact countdown that snaps it there instead of relying on a
     * threshold test the way the old asymptotic code did (a nonzero-target
     * ramp never crosses a small-delta threshold the way approach-to-zero
     * does). */
    int32_t decay_samples =
        (int32_t)(decay_to_sustain_s * (double)RENDER_RATE + 0.5);
    if (decay_samples > 0) {
        v->env_decay_samples_left = decay_samples;
        v->env_decay_coef = rt_pow(v->env_sustain_level, 1.0 / (double)decay_samples);
    } else {
        /* Rescaled duration is ~0 samples -- e.g. sustain_permille at/near
         * 1000, no real decay to perform -- so there is nothing for
         * ENV_DECAY to do; go straight to sustain below, matching how
         * decay_s<=0 already worked pre-fix. */
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

    /* EG2 (pitch envelope), SPEC.adoc S2.4.3 `[A:0x15838]` / SPEC_LOG.adoc #20.
     * Same segment structure as EG1 above, but its output scales
     * eg2_to_pitch_cents rather than amplitude. A zero depth (the documented
     * default, S2.4.3's "WORD +0x1e = 0 -- no EG2->pitch by default") leaves
     * this inert, so instruments without the connection are unaffected. */
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

    /* Prime the amplitude ramp (see the amp_left reset above). env_level is
     * final only now: 1.0 for an instant attack, 0.0 for a real one. */
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

/* Frames until EG1's current segment ends, or ENV_NO_CHANGE if it has no
 * scheduled end. This is the driver's "next change" term: `0x19490` mins the
 * envelope's own next boundary into the voice's segment length `[A:0x19490]`,
 * which is what keeps a 5 ms attack from being smeared across a 23 ms
 * amplitude segment. Sustain and release deliberately return ENV_NO_CHANGE --
 * sustain has no end, and release running uncut to the finish floor is exactly
 * why a short release renders as one straight line into silence. */
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
                /* SPEC.adoc S3.4.2 `[A:0x18ac0-0x18ad1]`: elapsed>=attackDuration
                 * leaves the attack branch (and Table C) entirely, it never asks
                 * the table for ratio==1.0 -- so land on exactly 1.0 here rather
                 * than at whatever g_table_envshape[200]==999 converts back to. */
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
            /* SPEC.adoc S5.1.2.1: geometric ramp, same mechanism as ENV_RELEASE
             * below (env_level *= coef), not an approach-to-target -- see the
             * note-on setup comment above for the coefficient's derivation.
             * env_decay_samples_left is the exact countdown that snaps
             * env_level to env_sustain_level AT the rescaled duration,
             * avoiding both float drift and the old threshold test (which
             * doesn't generalize to a nonzero target). */
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
            /* Tried SPEC.adoc Part 5's confirmed -80dB finish-detection floor
             * (`0xffffe0c0`, `[A:0x19733]`) here instead of this -66dB
             * value; measured byte-identical spec_ovr across every graded
             * probe and a very slightly worse (further from reference)
             * corridor.mid duration (53.13s vs. this value's 52.57s against
             * a 52.43s reference) -- reverted, see report. */
            /* The bare EG, NOT the audible level. This used to also reap on
             * `env_level * gain < AUDIBLE_FLOOR`, on SPEC.adoc S5.7's `[I]`
             * reading of `+0x13c` (the field 0x19733 compares against its
             * -80dB constant) as a gain-inclusive level. That reading is
             * WRONG, and `0x19490` shows it directly: the amplitude EG is
             * evaluated at `0x194aa`, stored to `+0x13c` at `[A:0x194da]`,
             * and only THEN are the LFO term (`[A:0x194f0]`) and the
             * CC7/expression term (`[A:0x19525]`) summed into the running
             * total. `+0x13c` never sees them. The driver therefore cannot
             * reap a voice for being momentarily quiet.
             *
             * Ours could, and did. On field/Kot_and_A64-GENERAL_SERUM.mid at
             * t=3.23s, four voices at env 0.95 -- fully sounding, just
             * released -- were reaped the instant their note-off landed,
             * because that segment's sampled gain was 7e-6 (CC11 passing
             * through 0 on its way up from a gate). The whole release tail
             * vanished and the mix went to digital silence for 75 ms, which
             * is a score gap where the reference rings. The old one-pole gain
             * smoother hid this by never letting gain reach 0; per-segment
             * gain does not, so the latent bug became audible.
             *
             * What this loses: the reap used to also evict voices that are
             * inaudible from their OWN attenuation (a velocity-46 note holds
             * a slot for the ~66 dB its normalized EG still has to fall
             * through). That was measured to matter on
             * field/HueArme-Weekend.mid at t=23.9s -- 41 released voices, 20
             * below -40dB audible, and note-on's forced steal evicting the
             * two held Seashore voices carrying the passage's noise wash,
             * 8 dB missing in 400-1000Hz. Measured after the fact at the
             * shipped 256-frame segment, that hole does NOT return: HueArme
             * scores -19.35 against -19.10 for the one-pole build, i.e.
             * better. (An earlier -17.93 reading was taken at a 512-frame
             * segment and wrongly blamed on this change.) If it ever does
             * return, the fix is in the reserve top-up, not in a reap
             * condition the driver does not have. */
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
