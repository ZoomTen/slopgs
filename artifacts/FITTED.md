# FITTED.md -- fitted-value ledger

## What `[F:fitted]` means

A value tagged `[F:fitted]` in this project's source or docs was tuned
**empirically against reference audio renders** (probe MIDI files vs. their
captured reference audio in `probe-results/`), not recovered from the
original binary's disassembly. It exists because the real consumption code
for that value is `[O]` (outside every disassembled/examined range in
`SPEC.md`) -- empirical fitting against reference audio is the sanctioned
fallback for exactly that situation, but it must always be visibly flagged
as a fit, never presented or graded as `[A:recovered]`.

Rules for anything tagged `[F:fitted]`:

- It must cite the probe/reference it was fit against and the exact error
  metric used.
- It must cite where a future RE pass should look to recover the real
  formula (a `SPEC.md` pointer, an address, a function), so the fit can be
  retired once real recovery happens.
- It must never be cited as evidence of what the original binary does --
  only as an evidence-based placeholder standing in for it.
- If a fit is measured and found to make things *worse* against the
  project's own regression harness, it is not shipped, but the attempt and
  its measurements are still recorded here (see entry 1) -- an honestly
  reported negative result is not a discarded one.

---

## 2026-07-25 -- HARNESS RE-BASELINE: dB figures recorded before this date are not comparable to ones recorded after

The project's Python fitting/comparison harness (a corpus-comparison driver
plus per-feature fit-check scripts) is not retained in this tree; every
mention of it below (and of its individual scripts and recorded JSON
results by name) is historical narration of a check that was run at the
time, not a pointer to a file that exists now. Its rendering role is now
filled by the native `dist/msgs-render` binary. This harness's alignment was replaced (see
`envelope_align_offset` and the two-stage alignment in `align_and_grade`). Every
`compare_spectral_22050.overall_db` figure in this file that was recorded
before this change was computed on a **misaligned** reference/render pair
for a large part of the corpus, and is therefore not comparable with any
figure recorded after it.

**What was wrong.** The old aligner cross-correlated raw waveforms over the
whole file. The probe corpus is mostly ladders of *identical repeated notes*
at fixed spacing, so the waveform correlates nearly as well one whole note-
slot off as at the true offset, and the FFT peak routinely locked onto the
wrong slot. Confirmed on the gain/pan probes, where every disagreement with
a hand-measured offset was an exact integer multiple of that probe's own
note spacing: 03_velocity off by 8.07 s, 24_gain_staging by 8.11 s,
25_pan_law by exactly two notes.

**The fix.** Two stages: a coarse pass correlating 10 ms **RMS envelopes**
(the envelope keeps the per-step *level* that distinguishes one rung of a
ladder from the next, which is exactly what waveform correlation discards),
then a sample-domain refine within ±50 ms of the coarse result, so the final
offset is still sample-accurate. Validated against seven independently
hand-measured offsets: max error **29 ms**, five of seven within 6 ms, versus
errors up to 8.11 s before. All seven `IMPLAUSIBLE_OFFSET` flags in the
corpus cleared (7 → 0); envelope correlation r = 0.86–0.98.

**Scale of the correction.** 14 of 24 graded probes were misaligned. Corpus
mean −8.025 → **−10.113 dB**; GENERAL_SERUM −2.27 → **−4.24 dB** with its
envelope correlation r 0.642 → **0.831**.

**These are measurement corrections, not synth improvements.** Not one line
of `src/engine/` changed between those two numbers. The synth was always this
close; the harness was mis-reporting it.

**What this does and does not invalidate.** Conclusions drawn from *direct
per-step measurement at a hand-recovered alignment* stand unaffected —
notably Entry 7's per-channel pan table and the `[M: probe 07]` settlements
in `SPEC_GAPS.md` §4 and §9, all of which used RMS-envelope alignment
precisely because the harness number was visibly untrustworthy. What is
invalidated is any *corpus-wide comparison table* computed by the old
harness, including Entry 7 §4 (re-measured — see the note there) and the
before/after tables in Entries 1–6. Those tables are kept as recorded; they
are history, not current measurements.

---

## Entry 1: EG1 decay-segment rate multiplier (Acoustic Grand Piano probe) -- FIT ATTEMPTED, NOT SHIPPED, SUPERSEDED BY ENTRY 15

> **SUPERSEDED 2026-07-26 -- do not re-attempt this fit.** The 2.85x was
> decay-time KEY-FOLLOW at note 60 mistaken for a constant. `gm.dls` gives
> Piano 1 a `usSource=3`(KEYNUMBER)->`0x0207`(EG1 decay) connection of
> `-3979` timecents full-scale, which at note 60 is `-1865` tc =
> **2.945x faster decay** -- within 3% of the 2.85 fit below, and available
> per-key from the file instead of frozen into a constant. That is exactly
> why this fit repaired probe 04 (note 60 only) and regressed monotonically
> everywhere else. Now shipped properly as Entry 15 / SPEC.md S2.4.3.2.

**Status: `[F:fitted, NOT SHIPPED]`.** Measured and characterized below, but
**not applied** in `src/engine/voice.c` -- see "Residual delta" below for why.
`src/engine/voice.c`'s decay segment still uses the original shared `exp_coef` (the
same function used for release), unchanged from before this investigation.

### 1. Value chosen (would-be)

A decay-segment-only rate multiplier of **2.85** applied to `exp_coef`'s
`-5.0` dB-decade exponent, i.e. a hypothetical `decay_coef(seconds)`:

```c
/* NOT present in src/engine/voice.c -- shown here for the record only. */
static double decay_coef(double seconds) {
    if (seconds <= 0.0) return 0.0;
    double samples = seconds * (double)RENDER_RATE;
    if (samples < 1.0) samples = 1.0;
    return rt_pow(10.0, (-5.0 * 2.85) / samples);
}
```

used in place of `exp_coef(decay_s)` at the `v->env_decay_coef = ...`
assignment in `voice_note_on` (`src/engine/voice.c`), decoupled from the
`v->env_release_coef = exp_coef(rel_s)` assignment in `start_release` (which
is untouched, both in this attempt and in the shipped code).

### 2. How it was fit

- **Probe/reference:** `probes/04_envelope.mid` vs. `probe-results/04.flac`,
  the longest-held note in the first (Acoustic Grand Piano, bank 0 program
  0) block: note-on at tick 35588, note-off at tick 40388 (120 BPM, 480
  ticks/quarter -> 1/960 s/tick), i.e. onset ~37.075s, a 5.0s hold, note-off
  ~42.075s. `gm.dls`'s own art1 connections for this instrument confirm
  `eg1_decay_tc` raw scale = 6387 (timecent -> ~39.97s per `src/engine/voice.c`'s
  `timecents_to_seconds`), `eg1_sustain_permille` = 0, and a `usSource=3`
  (KEYNUMBER) connection on EG1_DECAY whose contribution is exactly zero at
  note 60 (ruled out as a contributing factor, matches the finding already
  recorded in `SPEC_GAPS.md` item 15).
- **Error metric:** decoded both synth output (rendered via
  `dist/msgs-render`) and the reference FLAC to a common mono
  RMS envelope (10ms window/5ms hop), converted to dB, and fit a log-linear
  (dB vs. time) least-squares line over a decay window. Window selection
  was itself measured: scanning the window start from t=36.9s to t=38.2s
  (end fixed at 41.8s) shows the fitted slope destabilized by attack-onset
  transients before ~37.6s and stable (both R^2 > 0.995) for any start
  >=37.6s -- used **[37.6s, 41.8s]** as the reported window.
- **Before:** synth decay measured at **-2.505 dB/s** (R^2=0.996, matching
  the closed-form prediction of `-100dB/39.97s = -2.50 dB/s` almost
  exactly). Reference measured at **-7.14 dB/s** (R^2=0.9995). Ratio =
  2.850 (stable to 3 significant figures across the whole stable-window
  range above).
- **Fitted value:** multiplier = ref_slope / synth_slope = **2.850**,
  applied as a scale on `exp_coef`'s exponent for the decay segment only.
- **After (isolated target metric, own measurement):** re-rendering with
  the 2.85x decay-only multiplier applied gives synth decay = **-7.20
  dB/s** vs. reference's -7.14 dB/s -- residual **0.06 dB/s** (0.8%), an
  excellent match on the exact metric it was fit to.
- **Release, confirmed unchanged:** measured the same-instrument's fast
  release segment (first, 10-tick-held note, effectively an immediate
  release from near env_level=1.0) before and after: **-114.47 dB/s**
  before vs. **-114.45 dB/s** after (R^2=0.9993 both) -- release is
  untouched by this scoping, as required.

### 3. Where a future RE pass should look

`SPEC.md` Part 5 `+0x13c`, functions **`0x194da`** / **`0x19644`** -- the
real EG1 decay-rate consumption code, explicitly marked `[O]` in `SPEC.md`
because it lies outside every disassembled/PAGE range this project
examined. This is the same pointer already on record in `SPEC_GAPS.md` item
15. A future pass should start there specifically for the *decay*-segment
rate constant/formula (as distinct from the release-segment rate, which
`SPEC_GAPS.md` item 15 already resolved well via S5.6's independent 70ms
measurement) -- likely a per-instrument or per-decay_tc-range table/divisor
rather than a single global multiplier, given what section 4 below shows.

### 4. Residual delta the fit bought -- **this is why it was not shipped**

Ran the harness (--skip-smoke mode) (`compare_spectral_22050.overall_db`,
more-negative = better) with the decay-only multiplier at several
strengths, holding everything else fixed:

| decay-rate multiplier | probe 04 spectral (dB) | 18-probe mean (dB) |
|---|---|---|
| 1.00 (baseline, no fit) | -4.235 | -3.335 |
| 1.50 | -4.222 | -3.257 |
| 2.00 | -4.211 | -3.175 |
| 2.85 (the fitted value) | -4.194 | -3.053 |

Both the probe-04 metric and the 18-probe mean get **monotonically worse**
(less negative) as the decay-rate multiplier increases -- every step in the
direction that improves the isolated dB/s target metric (see section 2)
makes the harness's own broader phase-insensitive spectral-residual gate
worse, on essentially every probe (17 of 18 graded probes regressed at the
2.85x setting; only `16_drum_parts`, uninvolved in EG1 decay timing,
improved marginally). This is consistent with the multiplier being fit
against a single instrument/patch (Acoustic Grand Piano, one decay_tc
value) and not generalizing to the corpus's other ~234 GM instruments,
whose own decay-rate discrepancy (if any) may differ in magnitude or sign
from this one measured case.

**Per this project's own pass condition for this kind of fit** ("keep the
fit only if probe 04's spectral residual improves AND the 18-probe mean
does not regress... if the decay fit makes 04 worse or drags the mean down,
the fit or its scoping is wrong -- revert and report"), this fit is
**recorded but not shipped**. `src/engine/voice.c` is unchanged from before
this investigation (single shared `exp_coef` for both decay and release).

### Falsifiability

The project's decay fit check (a harness script, not retained -- see the
top of this file) re-derives the synth-vs-reference decay dB/s measurement
above from any built wasm, comparing synth vs. reference decay dB/s as
described in section 2. Run against the current `dist/msgs.wasm` it is
*expected* to report a mismatch (documenting this still-open gap, not a bug
in the check).

If a future attempt re-scopes this fit (e.g. per-instrument rather than
global) or a real RE recovery of `0x194da`/`0x19644` lands, an equivalent check
should be re-run (and the harness's full spectral gate re-checked
before shipping) to confirm the new code actually closes this specific gap
without the regression documented above.

---

## Entry 2: pitch-LFO (vibrato) rate + depth-application scale (CC1/mod-wheel probe) -- FIT ATTEMPTED, NOT SHIPPED

**SUPERSEDED (see Entry 6): pitch-LFO (vibrato) has since been SHIPPED**,
tagged `[M: probe 06]` -- decided on direct rate/depth measurement against
`probes/06_modwheel.mid`/`probe-results/06.flac` rather than gated on this
probe's own (independently fragile) `compare_spectral_22050` alignment
score. This entry (a hardcoded global rate + CC1-only depth-scale fit) and
Entry 3 (the per-instrument-rate retry) are kept below for the investigation
history; Entry 4 records what actually shipped, which is closest to Entry
3's design (per-instrument rate/delay, both depth connections *parsed* but
only the CC1-gated one *applied* -- see Entry 6 for why the inherent term
was dropped, a corpus-wide-measured decision, not the "not shipped" verdict
below, which predates that measurement).

**Status (as originally written, now historical): `[F:fitted, NOT SHIPPED]`.**
Measured and characterized below, but **not applied** in
`src/engine/voice.c`/`src/engine/dls.c`/`src/engine/dls.h` -- confirmed reverted byte-for-byte
(rendered PCM of `probes/06_modwheel.mid` is identical to the
pre-investigation build). `voice_update_pitch`'s LFO hook is still the bare
`+ 0 /* + LFO (step 3) */` placeholder it was before this investigation.
`g_table_sine` (Table D) remains built but unused (SPEC_GAPS.md #13,
unchanged).

### 1. Value chosen (would-be)

Two constants in `src/engine/voice.c`, plus a two-field parse addition in
`src/engine/dls.c`/`src/engine/dls.h`:

- `LFO_RATE_HZ = 5.99` -- a single global vibrato rate applied to every
  voice's LFO phase accumulator (`v->lfo_phase`, advanced each render block
  by `LFO_RATE_HZ * frames / RENDER_RATE` cycles, wrapped to `[0,1)`,
  indexed into `g_table_sine` as the waveform).
- `LFO_DEPTH_SCALE = 0.80` -- an overall multiplier applied to the DLS
  `art1` **mod-wheel-scaled** LFO-to-pitch connection depth
  (`usSource=1(LFO), usControl=0x0081(CC1), usDestination=0x0003(PITCH)`,
  parsed into a new `Artic.lfo_to_pitch_cc1_cents` field with the exact
  same `high word of lScale, clamped [-1200,1200]` formula SPEC.md already
  confirms `[A]` for `eg2_to_pitch_cents`), scaled linearly by
  `g_channels[ch].modulation / 127.0` (CC1 is already plumbed into
  `g_channels[ch].modulation` by `src/engine/synth.c`'s existing CC1 dispatch -- no
  plumbing gap there, contrary to what the briefing worried might be
  missing). The sibling **always-on, uncontrolled** LFO-to-pitch connection
  (`usControl=0`, same destination) is intentionally NOT read/used --
  see section 2.

Would-be code (for the record only -- not present in `src/engine/voice.c`):

```c
/* NOT present in src/engine/voice.c -- shown here for the record only. */
#define LFO_RATE_HZ 5.99
#define LFO_DEPTH_SCALE 0.80

int32_t lfo_cents = 0;
if (v->artic && v->artic->lfo_to_pitch_cc1_cents != 0) {
    double depth_cents = LFO_DEPTH_SCALE *
        ((double)g_channels[v->channel].modulation / 127.0) *
        (double)v->artic->lfo_to_pitch_cc1_cents;
    int idx = ((int)(v->lfo_phase * 256.0)) & 255;
    double lfo_unit = (double)g_table_sine[idx] / 100.0;
    lfo_cents = (int32_t)(depth_cents * lfo_unit);
}
```

with `v->lfo_phase` a per-voice `double` (0..1 cycles, reset to 0 at
note-on), advanced once per render block in `voices_update_modulation`
(which would need a `frames` parameter -- see section 4 for why even that
plumbing detail mattered).

### 2. How it was fit

- **Probe/reference:** `probes/06_modwheel.mid` vs `probe-results/06.flac`.
  The probe holds MIDI note 60 ten times, ~4s each, sweeping CC1 through
  `{0, 32, 64, 96, 127}` on GM program 48 (String Ensemble 1) then again on
  program 73 (0-indexed GM program), both bank 0.
- **Rate measurement:** for each of the ten held notes, band-passed the
  rendered/reference audio around the note's fundamental (261.6256 Hz
  ±6%), computed the instantaneous frequency via a Hilbert transform, then
  fit a sinusoid (grid search + linear least squares at each candidate
  frequency) to the frequency-deviation signal. Every segment with
  measurable depth (CC1 >= 32, both instruments) converged to
  **5.98-5.99 Hz** with very low scatter (std across 9 clean
  measurements: 5.9884 Hz mean, std ~0.004 Hz) -- confirms the reference
  **does** have a real, measurable, consistent-rate vibrato; this directly
  refutes the "maybe mod wheel does nothing" branch of the briefing's
  standing instruction. Rounded to `LFO_RATE_HZ = 5.99`.
- **Depth measurement:** converted each segment's peak frequency deviation
  to cents (`1200*log2((mean_f+dev_hz)/mean_f)`). Program 73 (flute-ish,
  clean single-layer tone, no natural chorus) gave a very clean linear fit
  of cents-vs-CC1 (intercept ~3.64 cents at CC1=0, slope ~0.30 cents/CC1
  unit, R² effectively 1). `gm.dls`'s own `art1` data for both programs 48
  and 73 (dumped and cross-checked directly against the file, not
  inferred) carries **identical** connections:
  `(src=1,ctrl=0,dest=3)` "static" depth = 1 cent (prog 48) / 5 cents (prog
  73), and `(src=1,ctrl=0x81,dest=3)` "CC1-scaled" depth = 47 cents for
  *both* programs -- and this `~47` cents CC1-scale value is essentially a
  corpus-wide constant: scanning all 235 `gm.dls` instruments' `art1` data
  directly, **every one of them** carries a `src=1,dest=3` LFO->pitch
  connection, and the CC1-scaled depth value clusters overwhelmingly at
  `{45, 47, 49}` cents (761 connections total, instrument- and
  region-level combined) -- i.e. this specific connection is corpus-wide
  content, not a one-off. Least-squares fitting a single multiplier `M`
  against program 73's 5 measured points, modeling
  `depth_cents = M * (static_cents + (CC1/127)*cc1_scale_cents)`, gives
  `M ≈ 0.805`; rounded to `LFO_DEPTH_SCALE = 0.80`.
- **Why the always-on/`ctrl=0` term was dropped from the design entirely:**
  including it (`depth_cents = SCALE*(static + (CC1/127)*cc1_scale)`, the
  literal reading of both `art1` rows) was tried first and measured against
  the full 18-probe corpus (not just probe 06): it regressed
  `04_envelope`'s `compare_spectral_22050.overall_db` by **+2.61 dB**
  (`-4.235` -> `-1.626`, a large regression) and `22_no_gs_reset` moved
  **-2.235 dB** (an improvement, but on an unrelated probe, for the wrong
  reason -- see below). Root cause, confirmed directly: `probes/04_envelope.mid`
  and `probes/22_no_gs_reset.mid` never send CC1 at all (checked: zero CC1
  events in either file), yet Acoustic Grand Piano (program 0, used by
  both probes) carries a nonzero static (`ctrl=0`) LFO->pitch depth of 1
  cent in `gm.dls` -- a `+-1` cent constant vibrato with **no clean probe
  reference to validate against** (probe 06 only ever exercises the CC1
  path) turned out to be enough to measurably worsen an unrelated,
  sustained-tone probe's phase-insensitive spectral match. This is
  precisely the Entry 1 failure shape (a value that looks locally
  supported turns out not to generalize) caught *before* it reached the
  ship-gate stage, by testing the isolated design choice against the full
  corpus first. The final design therefore reads and uses **only** the
  `ctrl=0x81` (CC1) connection.
- **A real implementation bug found and fixed mid-investigation:**
  `render_frames`'s block size is dictated entirely by `src/engine/smf.c`'s
  MIDI-event-boundary chunking (`smf_render`) -- for a held note with no
  MIDI events for ~4 seconds, `render_frames` is called *once* for the
  entire span. The first working version of this fit only recomputed the
  LFO phase/`phase_step` once per `render_frames` call (matching the
  existing `voices_update_modulation()` call site exactly as briefed),
  which meant the "vibrato" was actually a single frozen pitch offset per
  note, not an oscillation -- confirmed directly by FFT/instantaneous-
  frequency analysis of the rendered output (multiple spurious low-
  frequency peaks at 2-3 Hz instead of a clean 6 Hz tone). Fixed by having
  `render_frames` internally sub-chunk its own loop every 64 frames
  (`LFO_UPDATE_FRAMES`) and call `voices_update_modulation(chunk)` at that
  cadence -- confirmed by the same FFT method afterward that this produces
  a clean, consistent 5.98-5.99 Hz oscillation matching the fitted rate,
  with depth scaling linearly with CC1 as designed. **This fix mattered a
  great deal to the final ship decision -- see section 4.**

### 3. Where a future RE pass should look

`SPEC.md` Part 2 §2.4.2/§2.4.3 -- the **depth VALUES** this reads
(`lfo_to_pitch_cc1_cents`, via the same `high word of lScale, clamped
[-1200,1200]` formula already used for `eg2_to_pitch_cents`) are already
`[A:recovered]`, not fitted; what remains genuinely `[O]` and is what
`LFO_RATE_HZ`/`LFO_DEPTH_SCALE` stand in for:

- **The LFO rate/frequency formula**: `usDestination=0x0104` (LFO_FREQUENCY)
  routes through helper `0x153aa` (§2.4.2), a float-domain helper outside
  every disassembled/PAGE range this project examined -- the exact
  timecent(lScale)->Hz conversion is unrecovered. A future pass should
  start there; note that `gm.dls`'s own dest-0x0104 `lScale` values are
  *not* a single global constant across the corpus (24 distinct raw values
  found across 235 instruments' connections, dominant one used 568 times,
  a different one -- shared identically by both programs 48 and 73 tested
  here -- used 74 times), so a real per-instrument rate is plausible and
  the single global `5.99 Hz` used here is knowingly an approximation that
  could not be cross-validated against any *other* instrument's true rate
  (probe 06 is the only probe that isolates a single held note with a
  clean vibrato reference, and it only exercises these two programs).
- **The runtime depth-combination/application formula**: how the driver
  actually turns the two stored per-connection cents constants
  (`ctrl=0` "static" and `ctrl=0x81` "CC1-scaled", stored to two different
  fields per §2.4.3's table) plus a live CC1 value into an actual per-
  block pitch contribution is not shown anywhere in the parsing-focused
  sections of `SPEC.md` examined for this project -- this is the real LFO
  oscillator/mixer code, likely adjacent to wherever `0x153aa` lives,
  never located. `LFO_DEPTH_SCALE=0.80` and the decision to drop the
  `ctrl=0` term stand in for that unrecovered combination logic.

### 4. Residual delta the fit bought -- **this is why it was not shipped**

This entry has a more layered story than Entry 1's monotonic-regression
sweep, because a real bug (section 2, the render-granularity issue)
temporarily made the fit look like a clean win. All three measurements
below are `compare_spectral_22050.overall_db` from the harness (`--skip-smoke`
mode; more-negative = better), computed against a freshly-
reverted, byte-identical-to-pre-investigation baseline build (recorded at the
time in a JSON results file not retained in this tree, "step_lfo_before"):

| build | 06_modwheel (dB) | 18-probe mean (dB) | 06 alignment flag |
|---|---|---|---|
| baseline (no LFO at all) | -3.1791 | -3.3354 | `IMPLAUSIBLE_OFFSET` (pre-existing, see below) |
| static+CC1 depth, granularity bug present | -3.4760 | -3.3275 | none |
| CC1-only depth, granularity bug present (recorded at the time as "step_lfo_cc1only_BUGGY_GRANULARITY", JSON not retained) | -3.5465 | -3.3506 | none |
| CC1-only depth, granularity bug **fixed**, correct 5.99 Hz oscillation confirmed by direct FFT (recorded at the time as "step_lfo_cc1only_CORRECTED_NOT_SHIPPED", JSON not retained) | **-2.8882** | **-3.3150** | `IMPLAUSIBLE_OFFSET` (different lag than baseline's) |

Reading this top to bottom: the first two (buggy) builds look like clean
wins against the ship gate (06 improves, mean holds or improves, no other
probe moves >0.3dB). But rendering the exact same probe's synth PCM output
and directly comparing bytes shows *why*: the granularity bug gave each of
the ten repeated ~4-second held notes a distinct, CC1-correlated frozen
pitch offset baked in for its whole duration. `probes/06_modwheel.mid`
repeats the same note+instrument ten times with otherwise near-identical
underlying waveform content, and the harness's cross-correlation
alignment search (`align_and_grade`, ±15s window) is independently
confirmed to be **already borderline/wrong on the untouched baseline
build**: the baseline itself reports `offset_seconds=5.585` and an
`IMPLAUSIBLE_OFFSET` flag for probe 06 (recorded at the time as
"step_lfo_before", `compare_22050`), i.e. roughly one repeat-period off from the true ~0.09s
alignment. The granularity bug's per-note frozen pitch offsets happened to
make each repeat acoustically distinct enough to *accidentally* resolve
this pre-existing alignment ambiguity in the correlation search's favor
(`offset_seconds` dropped to ~0.09-0.10s, flag cleared) -- which is what
actually drove the "improvement," not a better-sounding vibrato. This was
confirmed directly, not inferred: rendering probe 06 through the buggy and
the fixed builds and diff'ing raw PCM bytes shows the two are substantially
different (1.55M of 2.43M samples differ, starting exactly at the second
note's onset), and setting `LFO_DEPTH_SCALE=0.0` on the *fixed*
(non-buggy) build reproduces the baseline PCM byte-for-byte, confirming
the fixed implementation's own code path is correct and the divergence
really is coming from the vibrato's audible content, not some other
change.

Once the render-granularity bug was fixed (confirmed via direct FFT that
the LFO now oscillates cleanly at 5.98-5.99 Hz with depth scaling linearly
with CC1, exactly as fitted), the corrected, honestly-implemented vibrato:

- Does **not** improve `06_modwheel` -- it regresses, `-3.1791` ->
  `-2.8882` (+0.29 dB, worse), and lands on a *different* wrong alignment
  lag (`11.10s` vs baseline's `5.585s`) rather than resolving the
  ambiguity the way the buggy version accidentally did.
- Does **not** improve the 18-probe mean -- `-3.3354` -> `-3.3150`
  (+0.0204 dB, a very small but real regression).
- Every other probe is **exactly** unchanged (`0.0000` dB delta to full
  float precision) except `13_edge` (+0.0765 dB, incidental, well under
  the 0.3 dB bar) -- confirming the CC1-gating design successfully
  contained the blast radius to only content that actually moves the mod
  wheel, exactly as intended. ("megalovania" -- an external test MIDI
  supplied by the user, not part of this tree -- sends zero CC1 events, so
  its rendered PCM is provably byte-identical between shipped and
  candidate builds -- confirmed directly, not just architecturally
  reasoned -- terminates at 156.05s/3,440,640 frames, matching the ~156s
  nominal duration, not truncated, no garbage.)
- A depth-scale sweep (0.0, 0.2, 0.4, 0.8, 1.2) on the corrected build
  shows `06_modwheel`'s spectral score moving *monotonically worse* as
  depth increases past 0 (`-3.179` @ 0.0 -> `-3.280` @ 0.2 -> `-3.372` @
  0.4 -> `-2.888` @ 0.8, non-monotonic between 0.4 and 0.8 specifically
  because that is where the alignment search flips from the baseline's
  wrong lag to a *different* wrong lag) -- there is no depth setting in
  the tested range where the corrected implementation clearly improves the
  target probe's own gate metric.

**Per this project's own ship gate** ("probe 06 improves AND the 18-probe
mean does not regress AND no more than incidental movement elsewhere"):
criterion 1 fails outright (06 regresses) and criterion 2 fails narrowly
(mean regresses by 0.02 dB) once the implementation is correct. This fit
is **recorded but not shipped**. `src/engine/voice.c`, `src/engine/dls.c`, `src/engine/dls.h`, and
`src/engine/render.c` are confirmed reverted to their pre-investigation state (byte-
identical rendered PCM output on `probes/06_modwheel.mid`, verified via
`cmp`, not just a source diff).

**A note on what this result does and doesn't mean:** the reference audio
*does* have a real, measurable, consistent vibrato (5.99 Hz, CC1-scaled
depth up to ~40-53 cents) -- this is not a "no vibrato" refutation of the
briefing. What the measurement shows instead is that (a) the harness's
cross-correlation alignment is independently fragile for this specific
probe's repetitive ten-repeats-of-one-note structure (flagged
`IMPLAUSIBLE_OFFSET` even on the pristine baseline, before any of this
work), which makes `compare_spectral_22050.overall_db` for probe 06
specifically a noisier signal than for other probes -- a fact worth fixing
in the harness independently of this fit, but out of this task's scope
(the harness's alignment algorithm was not modified, per the briefing's
file-scope restriction) -- and (b) once that noise is controlled for by
actually fixing the implementation to be a real, correct oscillator (not
by relying on an accidental side effect of a bug), the correct
implementation does not clear the gate. Both are real, directly-measured
findings, not speculation.

### Falsifiability

The LFO fit check (a harness script, not retained -- per-segment
Hilbert-transform instantaneous-frequency + sinusoid fit, at the default
5.0-cent tolerance) re-derives the rate/depth measurement above from any
built wasm. Run against the current (reverted) `dist/msgs.wasm` it is
*expected* to report a mismatch on every CC1>0 segment (synth depth stays
at its natural, near-zero or chorus-baked-in-sample value regardless of
CC1, since no LFO code is present).

If a future attempt re-scopes this fit (a harness alignment fix for probe
06's repeat-ambiguity, a per-instrument rate via real recovery of
`0x153aa`, or a different depth-combination model) it should be re-run
(and the harness's full spectral gate re-checked corpus-wide before
shipping, not just on probe 06) to confirm the new code actually clears
this gate without the regression documented above.

---

## Entry 3: pitch-LFO retry -- per-instrument rate/delay via `lfo_freq_tc`/`lfo_delay_tc`, both depth connections -- FIT ATTEMPTED, STILL NOT SHIPPED

**SUPERSEDED (see Entry 6): SHIPPED as `[M: probe 06]`**, with one change
from this entry's own design -- the inherent (`ctrl=0`) depth term is
summed/parsed here but NOT applied at runtime, dropped after this entry's
own section 3 found it regresses `04_envelope` by +2.70dB; Entry 4 re-
confirms that exact finding fresh against the current codebase (which had
since gained the pan-law/gain-smoother/GAIN_CEILING work) and ships the
CC1-only variant instead. The rate/delay derivation and sub-block
granularity fix below are unchanged and are what shipped.

**Status (as originally written, now historical): `[F:fitted, NOT SHIPPED]`.**
This retries Entry 2 with the two
gaps that entry's own "where a future pass should look" section named:
a real per-instrument LFO rate (not a single hardcoded `5.99`) derived from
each instrument's own `lfo_freq_tc`, and a real per-instrument start delay
from `lfo_delay_tc`. Both were implemented and measured. **The outcome is
the same as Entry 2: probe 06 itself regresses once the vibrato is
correctly implemented, for the identical, already-diagnosed reason (the
harness's cross-correlation alignment search on this specific
ten-repeated-notes probe is fragile and lands on a different wrong lag once
the audio actually changes).** `src/engine/voice.c`, `src/engine/voice.h`, `src/engine/dls.c`,
`src/engine/dls.h`, `src/engine/render.c`, and `src/engine/synth.h` are confirmed reverted -- byte-identical
to their pre-investigation state (`src/engine/voice.c` diffed line-for-line back to
the version read at the start of this pass; full corpus mean and every
per-probe `compare_spectral_22050.overall_db` reproduce the pre-
investigation baseline exactly, see table below).

### 1. What changed vs. Entry 2 (the two named gaps, both closed)

- **`src/engine/dls.c`/`src/engine/dls.h`**: `art1` `usSource==1(LFO)`, `usDestination==0x0003
  (PITCH)` connections are now parsed into two fields, `Artic.
  lfo_pitch_inherent_cents` (`usControl==0`) and `Artic.lfo_pitch_cc1_cents`
  (`usControl==0x0081`), via the same "high word of `lScale`, clamped
  `[-1200,1200]`" formula SPEC.md S2.4.2/S2.4.3 already confirms `[A]` for
  `eg2_to_pitch_cents` (reused verbatim, not a new formula). `usDestination
  ==0x0001` (tremolo) is explicitly left unparsed into any applied field --
  out of scope, noted in code and SPEC_GAPS.md.
- **LFO rate, per-instrument, not hardcoded**: `lfo_freq_tc` (already parsed,
  previously unused) is converted to Hz via `freq = 8.176 * 2^(tc/1200.0)`,
  `tc = lfo_freq_tc/65536.0` -- the standard DLS-1/SF2 "absolute-pitch-cents,
  8.176Hz at 0 cents" convention, applied per-voice from that voice's own
  region's `artic->lfo_freq_tc` (cached once at note-on, `v->lfo_freq_hz`).
  **Derivation, not a fit against a single global number:** GM program 48
  (String Ensemble 1) and 73 (Flute) -- probe 06's own two instruments, both
  independently measured at a dead-steady ~6.0 Hz vibrato rate regardless of
  CC1 -- carry the identical raw `lfo_freq_tc` lScale **-35106105**
  `[D:gm.dls]` (dumped directly from the file's `art1` bytes, both
  instrument-level, no region-level override for either instrument). Two
  candidate conversions were tried against this one raw value:
  - period-in-timecents (same domain as the EG-time duration formula,
    `timecents_to_seconds`): `freq = 2^(-tc/1200)` -> **1.363 Hz**. Rejected,
    does not match.
  - absolute-pitch-cents: `freq = 8.176 * 2^(tc/1200)` -> **6.0001 Hz**.
    Matches the measured ~6.0 Hz target to 4 significant figures, with zero
    fudge factor. **Shipped formula** (in the sense of "used in the retry
    build" -- the retry as a whole is still not shipped, see below).
  This is genuinely per-instrument, not a global constant standing in for
  one: a different instrument's own `lfo_freq_tc` yields a different rate
  (e.g. Acoustic Grand Piano's own raw value, `-51342062`, converts to
  **5.20 Hz** under the same formula -- confirmed by direct computation,
  not asserted).
- **LFO start delay, per-instrument**: `lfo_delay_tc` (already parsed,
  previously unused) reuses the existing, already-`[A]` `timecents_to_seconds`
  helper directly (SPEC.md S2.4.3: destination `0x0105` is routed through
  the same helper, `0x15364`, as the four EG times). Strings' raw delay
  lScale `-124646522` -> 0.333s; Flute's `-83274788` -> 0.480s (both
  computed directly, `[D:gm.dls]`+`[A]`). Both are real per-instrument
  values, not the "~immediate, <=0.1s" the briefing guessed might be
  typical -- that guess does not hold for these two instruments specifically,
  and the real values were used regardless.
- **Sub-block modulation granularity** (the bug Entry 2 itself found and
  fixed mid-investigation, re-applied here from scratch): `render_frames`
  now sub-chunks its loop every 64 frames (~2.9ms @ 22050Hz, >150Hz update
  rate), calling `voices_update_modulation()` once per sub-chunk and
  advancing each voice's LFO phase by `freq_hz * (frames/22050)` cycles
  between sub-chunks (a new `voices_advance_lfo(frames)`, gated on each
  voice's own cached `lfo_delay_s` so the oscillator's phase=0 start lines
  up with the end of the delay, not with note-on).
- **Both depth connections applied together** (Entry 2 shipped-candidate
  design only read `usControl==0x81`; this retry reads and sums both,
  per the briefing's explicit request): `depth_cents = inherent_cents +
  cc1_cents * (CC1/127)`, CC1 read live from `g_channels[ch].modulation`
  (already stored by the existing CC1 dispatch in `src/engine/synth.c` -- confirmed,
  no plumbing gap).

### 2. Measured against probe 06 (own fresh measurement, the LFO fit check, unmodified)

Rate: **5.996 Hz** measured on every CC1>=32 segment on both instruments
(reference: 5.976-5.986 Hz on the same segments) -- matches the ~6 Hz
target and the reference's own rate to within measurement noise.

Depth (peak-to-peak, `2 x` the script's own amplitude-cents measurement),
synth vs. reference, across the probe's swept CC1 values:

| segment | synth pp (cents) | ref pp (cents) |
|---|---|---|
| prog48 (strings) cc1=0   | 19.86 | 19.82 |
| prog48 cc1=32             | 23.40 | 19.50 |
| prog48 cc1=64             | 45.62 | 40.46 |
| prog48 cc1=96             | 67.52 | 61.12 |
| prog48 cc1=127            | 87.96 | 80.90 |
| prog73 (flute) cc1=0      |  7.78 |  7.28 |
| prog73 cc1=32             | 29.08 | 25.90 |
| prog73 cc1=64             | 50.00 | 45.82 |
| prog73 cc1=96             | 70.86 | 65.24 |
| prog73 cc1=127            | 90.04 | 84.22 |

This matches the briefing's measured targets closely (strings ~20pp at
CC1=0, flute ~10pp, rising to ~90pp at CC1=127) and tracks the reference
`.flac`'s own measured depth curve within a few cents at every point --
the LFO fit check, run against this build, reports "Overall: MATCH"
(all ten segments, including the one the check's own docstring already
flags as excused for an unrelated reason -- prog48 cc1=0's reference rate
measures 5.145 Hz, not ~6 Hz, because that segment's ~20pp wobble is
dominated by the String Ensemble sample's own baked-in chorus/detune
content, present identically in both the reference capture and this
synth's rendered output because both play the same underlying gm.dls PCM
-- confirmed by checking that this synth's `lfo_pitch_inherent_cents` for
program 48 is only 1 cent (pp=2), an order of magnitude too small to be
the source of a 20-cent wobble on its own).

**Both the rate derivation and the depth model are correct and validated
against gm.dls's own data plus the reference audio.** The reason this is
still not shipped is entirely in section 3 below.

### 3. Corpus-wide grading -- why this is still not shipped

`compare_spectral_22050.overall_db` (the harness (--skip-smoke mode),
more-negative = better), true current-codebase baseline (LFO code fully
absent) vs. two tested configurations of the retry:

| probe | baseline (no LFO) | full (inherent+CC1) | CC1-only |
|---|---|---|---|
| 03_velocity | -3.4121 | -3.4121 | -3.4121 |
| 04_envelope | **-4.2346** | **-1.5361** | -4.2346 |
| 05_pitchbend | -4.3376 | -4.3376 | -4.3376 |
| 06_modwheel (target) | **-3.1791** | -2.9439 | -2.8943 |
| 08_reverb | -3.7605 | -3.7605 | -3.7605 |
| 09_chorus | -4.7318 | -4.7318 | -4.7318 |
| 10_polyphony | -2.8332 | -2.8332 | -2.8332 |
| 12_gs_sysex | -4.1515 | -4.1515 | -4.1515 |
| 13_edge | -4.0006 | -4.0354 | -4.0355 |
| 14_running_status | -4.4054 | -4.4054 | -4.4054 |
| 15_banks | -4.1878 | -4.2079 | -4.1878 |
| 16_drum_parts | 0.1848 | 0.1848 | 0.1848 |
| 17_master_volume | -4.1396 | -4.1396 | -4.1396 |
| 18_key_groups | -3.6866 | -3.6866 | -3.6866 |
| 19_prior_art | -1.4771 | -1.4771 | -1.4771 |
| 20_voice_count | -3.2262 | -3.2262 | -3.2262 |
| 21_steal_policy | -2.6945 | -2.6945 | -2.6945 |
| 23_rpn_tune | -3.3361 | -3.3361 | -3.3361 |
| **18-probe mean** | **-3.4227** | -3.2628 | -3.4089 |

Two configurations were isolated and measured separately (both with the
correct per-instrument rate/delay and correct sub-block granularity):

- **Full (`inherent_cents + cc1_cents`, as the briefing's depth model
  literally specifies)**: regresses `04_envelope` by **+2.70 dB**
  (`-4.235` -> `-1.536`), the single largest movement of any probe in this
  entire project's history. Root cause confirmed directly, not inferred:
  Acoustic Grand Piano (program 0, the instrument `04_envelope` exercises,
  which never sends CC1) carries `lfo_pitch_inherent_cents = 1` in
  `gm.dls`'s own `art1` data -- a real, data-authored `usControl==0`
  connection, correctly parsed and correctly applied per the briefing's own
  model, that turns out to measurably worsen an unrelated sustained-tone
  probe's phase-insensitive spectral score even at a ±1-cent amplitude.
  This is the exact same failure this project's Entry 2 already diagnosed
  for the *identical* connection/value on the *identical* instrument --
  confirmed reproduced again here with a materially different (per-
  instrument, not global-hardcoded) rate/delay implementation, which rules
  out "the old global 5.99 Hz guess was the problem" as an explanation.
- **CC1-only (`cc1_cents` alone, `inherent_cents` read but not summed --
  Entry 2's shipped-candidate design)**: confirmed this restores
  `04_envelope` to the baseline exactly (`-4.2346`, byte-for-byte same
  value), i.e. the gating logic is correct and no vibrato leaks onto a
  voice via any path other than the `usControl==0x81` connection's own
  correctly-computed depth. But **`06_modwheel` -- the one probe this
  entire feature is being built for -- still regresses**, `-3.1791` ->
  `-2.8943` (+0.285 dB, worse), landing on a different alignment lag
  (`offset_seconds` 11.107s vs baseline's 5.585s, both independently
  flagged `IMPLAUSIBLE_OFFSET`) for the identical reason Entry 2 already
  root-caused: `probes/06_modwheel.mid` repeats the same note+instrument
  ten times, so the harness's cross-correlation alignment search is
  already fragile/ambiguous on the *unmodified baseline itself* (flagged
  `IMPLAUSIBLE_OFFSET` even with zero vibrato code present), and once the
  rendered audio actually changes (a real, correctly-oscillating vibrato,
  not a frozen per-block approximation), the search settles on a
  *different* wrong lag that happens to score worse. This was verified
  fresh this pass (not assumed from Entry 2): the offset only moves because
  the audio content changes, and it moves to a different value than the
  granularity-bug build in Entry 2 did (11.10s here vs. that build's own
  11.10s coincidentally very close, but for the -0.80-vs-0.80-scale
  different depth model), consistent with an alignment search that is
  sensitive to any real change in the ten repeats' waveform content, not
  specifically sensitive to this feature's correctness.

Per this project's own ship gate ("ship if... probe 06's spectral residual
improves AND the 18-probe mean does not regress"): criterion 1 fails in
both tested configurations (06 gets worse in each, never better), so
neither is shipped, independent of the mean question. megalovania
(zero CC1 events, uses instruments other than 48/73 for its leads) was
rendered under the full-LFO build and terminates correctly at 156.05s /
3,440,640 frames (unchanged from baseline -- expected, since none of its
notes send CC1 and its own instruments' `lfo_pitch_inherent_cents` values
were not individually audited, but the render completing on-length with no
truncation confirms no crash/hang/runaway growth from the added per-voice
LFO state or the sub-chunked render loop). `GENERAL SERUM`'s spectral
residual and envelope-r are unchanged to 4 decimal places between baseline
and the full-LFO build (`-2.4599` -> `-2.4599` dB spectral,
`r=0.59058` -> `r=0.59063`) -- this dense, highly polyphonic reference is
not measurably affected either way, consistent with the CC1-gating design
successfully containing the change's blast radius to the handful of
probes that actually exercise CC1 or the specific inherent-depth
instrument (piano) held in isolation.

### 4. Where a future RE pass should look

Same as Entry 2 section 3 for the depth-combination/mixer code itself
(never located, `[O]`). The rate/delay half of that gap is now closed by
this entry (see section 1) and should not be treated as open in any future
attempt. What remains genuinely blocking a ship decision is **not** the
DSP implementation (rate, delay, and depth are all now measured-correct
against gm.dls's own data and the reference capture) but
the harness's cross-correlation alignment search's own fragility on
`probes/06_modwheel.mid`'s ten-repeated-identical-notes structure --
already flagged `IMPLAUSIBLE_OFFSET` on the untouched baseline before any
of this work, in both Entry 2 and this entry. A future attempt should
either fix that alignment search (out of this project's file-scope
restriction on the harness for this pass) or obtain/author a
non-repetitive probe reference for CC1-modwheel vibrato before re-
attempting a ship decision on this feature.

### Falsifiability

The LFO fit check, run against the current (reverted) `dist/msgs.wasm`
build, is expected to report a mismatch on every CC1>0 segment (no LFO code
present, same as after Entry 2). To reproduce this entry's own
measured-correct numbers, re-apply the diff recorded in this entry (section
1) and re-run the same check -- it should report "Overall: MATCH" as shown
in section 2.

---

## Entry 4: gain-smoothing time constant for CC7/CC11 Expression gliding (`GAIN_SMOOTH_ALPHA` in src/engine/render.c and src/engine/voice.c) -- SHIPPED

**Status: `[F:fitted, SHIPPED]`.** Implemented and shipped in `src/engine/render.c` and `src/engine/voice.c` -- the per-sample one-pole gain smoother applied to `gain_l`/`gain_r` variables, eliminating empirically-observed 162/244-sample exact-zero gaps caused by instantaneous CC11 Expression=0 steps that truncate to zero-valued samples.

### 1. Value chosen

**`GAIN_SMOOTH_ALPHA = 0.003780968318281238`** in `src/engine/render.c` (line 49).

A per-sample one-pole gain smoothing coefficient with an effective time constant of ~12 ms at the render rate (22050 Hz), computed as `GAIN_SMOOTH_ALPHA = 1 - exp(-1/(0.012 * RENDER_RATE))`.

Applied in both `render_frames()` (inside voice-level per-block rendering) and `src/engine/voice.c`'s gain-update code:

```c
v->gain_l += (v->gain_l_target - v->gain_l) * GAIN_SMOOTH_ALPHA;
v->gain_r += (v->gain_r_target - v->gain_r) * GAIN_SMOOTH_ALPHA;
```

each sample (via a per-sample iteration in the render loop), so gain glides exponentially toward its target value (set by MIDI CC7 Main Volume and CC11 Expression) instead of snapping instantaneously. Note-on events snap gain directly to the target (no glide during initial envelope attack) so the envelope's own attack envelope is not double-shaped by the gain ramp.

### 2. How it was fit

- **Probe/reference:** a captured render of GENERAL_SERUM (GM program 32) channels 1–2 (a polyphonic orchestration with string and pad sections), recorded at the time as "gs_ch12" and not retained in this tree (see the correction below). The probe MIDI behind it contained authored discrete-step CC11 (Expression) automation that includes ~7–11 ms windows where Expression snaps to value 0.
- **The bug:** Before the fix, `voice_update_gain()` reads CC11 live and applies it instantly each block. When Expression=0, the gain is set to `gain = 10^(g_table_vel[0] / 200.0)` with `g_table_vel[0] = -9600` (−96 dB), yielding `gain ≈ 1.6e-5`. Multiplying a ±32767 audio sample by `1.6e-5` gives a value ~±0.5, which when cast to `int16_t` truncates to exactly 0. A 7–11 ms Expression=0 window at 22050 Hz = 154–243 samples all outputting exactly zero -- measured observation: runs of 162–244 samples of exact zero, chopping what should be smooth, sustained chords into fragments.
- **Error metric:** Count of contiguous runs of exact-zero samples ≥50 samples long in the rendered channels 1–2 stereo output (measured by counting every consecutive run of samples where both `left == 0 && right == 0`).
- **Before fit:** channels-1&2 render contained **187 occurrence of the 162/244-sample zero-run signature** (the specific size corresponding to the ~7–11 ms CC11 dips), plus an additional 285 runs of other lengths (≥50 samples), totaling 472 runs ≥50 samples. Spectral residual on the 18-probe corpus: `compare_spectral_22050.overall_db` mean = −3.335 dB (unsmoothed baseline). GENERAL_SERUM envelope correlation `r = 0.5913`.
- **Fitted value:** Set `GAIN_SMOOTH_ALPHA = 1 - exp(-1/(0.012 * RENDER_RATE))` = 0.003780968318281238, tuned so the 12 ms exponential time constant is long enough to prevent the gain from reaching the far end of any single 7–11 ms Expression blip (so the 1.6e-5 extreme is never fully engaged) but short enough (12 ms << note envelope's attack/decay/release timescales, seconds to tens of ms per instrument) not to noticeably soften real dynamics.
- **After fit:** Zero occurrences of the 162/244-sample signature; total ≥50-sample zero runs dropped to **89** (the remaining 89 are legitimate release tails and inter-note silences in a program-81 staccato section, confirmed present in both before and after renders). Spectral residual: `compare_spectral_22050.overall_db` mean = −3.3335 dB (no regression, stable within float precision). GENERAL_SERUM envelope correlation `r = 0.5941` (+0.0028 improvement, within measurement noise but consistent direction).

### 3. Where a future RE pass should look

**`SPEC.md` Part 6, Section 6.6** ("reverse engineering confirms gain is NOT applied as an instant per-block jump ... a linear-ramp smoothing mechanism operating on [gain]"). The real hardware ramp mechanism's exact `ramp_period` and waveform shape (the specification describes it as *linear*, but this fit uses a *one-pole exponential* approximation) are marked `[O]` (the consumption code lies outside every disassembled/PAGE-range boundary examined). A future reverse-engineering pass should locate the real gain-ramp code, recover its exact period and mathematical form, and replace both the 12 ms constant and the exponential approximation with the true formula.

### 4. Residual delta the fit bought

- **Gap elimination (the primary goal):** 187 → 0 occurrences of the 162/244-sample exact-zero signature; 472 → 89 total ≥50-sample runs (the 89 are confirmed legitimate release/silence content, not audible truncation artifacts).
- **Spectral mean unchanged:** −3.335 → −3.3335 dB (stable within rounding; no measurable regression).
- **GENERAL_SERUM envelope correlation improved marginally:** `r = 0.5913` → `r = 0.5941` (+0.003, within measurement noise).
- **Fidelity trade-off:** This is an **exponential approximation** to SPEC's stated linear ramp. It is "good enough" to eliminate the audible gap artifacts present in the reference capture and maintains backward compat with the existing 18-probe corpus. Once the real ramp period and shape are recovered via future RE, both should be updated and this entry's status changed to `[A:recovered]`.

### Falsifiability

Gaps in the channels-1&2 render are objectively countable: rerun a render and count ≥50-sample exact-zero runs in the output PCM for program=GENERAL_SERUM, channels 1–2 (the specific MIDI/channels that triggered the bug). Must stay at ~0 for the 162/244-sample signature (the 89 remaining runs of other lengths are baked into the staccato/release content of the reference itself, present identically before and after the fit).

Once the real gain-ramp mechanism is recovered (ramp period and linear vs. exponential shape), replacing the current one-pole form with the true formula should reproduce the same gap elimination while potentially improving the spectral residual further (if the real ramp is meaningfully different from exponential).

Real-hardware reference: the original MSGS hardware synth (if available for re-testing) should also show zero such gap artifacts at the same CC11 step timing, confirming the hardware's own ramp mechanism silences them naturally.

### Correction (2026-07-26): this entry's own supporting probe/reference are confirmed GONE

The "gs_ch12" probe MIDI and its reference capture -- the probe/reference this
entire entry (rate, fit, and the 187/89 zero-run counts in section 4) is
measured against -- are both confirmed absent from the current tree, at the
paths they would have lived at:

```
$ ls probes/gs_ch12.mid probe_results/gs_ch12.flac
ls: cannot access 'probes/gs_ch12.mid': No such file or directory
ls: cannot access 'probe_results/gs_ch12.flac': No such file or directory
```

So the 12 ms `GAIN_SMOOTH_ALPHA` calibration above is **no longer backed by
any surviving capture** -- it cannot be re-measured or re-falsified against
its own cited evidence today. This does not mean the value is wrong; it means
it is currently unverifiable by the method this entry itself specifies. The
value is left unchanged in `src/engine/render.c` (still `[F:fitted, SHIPPED]`,
still the best available stand-in for SPEC.md S6.6's `[O]` real ramp), but a
future pass should either recover/re-author an equivalent probe+reference pair
before relying on this entry's specific numbers again, or treat section 4's
187/89 counts as historical only. See Entry 13 below for a separate, freshly-
run attempt (this session) to replace the mechanism itself (one-pole ->
linear ramp), which used `probes/28_expression_gate.mid` instead and did not
depend on the missing `gs_ch12` pair.

---

## Entry 5: pan-law interpolation shape between probe-25 anchors -- SUPERSEDED

> **SUPERSEDED by Entry 7, do not re-ship.** The nine anchor values below
> reproduce exactly on re-measurement (the *measurement* was sound), but the
> *interpretation* was wrong: probe 25's flat center-pan plateau (unity gain,
> both channels, CC10=48..80) is **GAIN_CEILING saturation**, not an absence
> of pan attenuation. Model `measured = min(GAIN_CEILING_dB, A + 10*log10((127-pan)/127))`
> fits all six unsaturated probe-25 points (both channels, Sine patch bank 8
> program 80, CC7=127 -- driven ~4.78 dB above the ceiling) to within 0.01 dB;
> probe 25 was never a clean read of the pan law by itself, it was reading a
> clamp. Probe 07 (`probes/07_pan_volume.mid`, new reference, unsaturated
> throughout) gives the real shape and supersedes both the anchor/lerp table
> below and its "constant per-patch offset" residual story in section 4. See
> `FITTED.md` Entry 7 for the replacement, the saturation evidence, and the
> corpus-wide re-measurement. This entry's body is kept below, unmodified,
> for the investigation history (this project keeps negative/superseded
> results, not just shipped ones).

**Status: `[F:fitted, SUPERSEDED -- see Entry 7]`.** (Originally shipped as
`[F:fitted, SHIPPED]`.) The nine anchor attenuation values are
`[M: probe 25]` (measured directly, not fit, and reproduce exactly on
re-measurement); only the piecewise-linear **shape between anchors** was a
fitted engineering choice, because probe 25 only samples CC10 at 16-unit
intervals (31 at the outermost gap) and no finer-grained reference existed
at the time to pin the true intra-gap curve. Both the anchors *and* the
interpolation are now superseded -- the anchors themselves were reading a
saturation plateau, not the underlying law (see note above).

### 1. Value chosen

`src/engine/voice.c`, replacing the two-line disassembly-derived pan gain
computation in `voice_update_gain`:

```c
static const int PAN_ANCHOR[9] = {0, 16, 32, 48, 64, 80, 96, 112, 127};
static const int32_t PAN_ATTEN_L_HDB[9] = {0, 0, 0, 0, 0, -3, -141, -452, -2021};
static const int32_t PAN_ATTEN_R_HDB[9] = {-2021, -420, -120, 0, 0, 0, 0, 0, 0};

static int32_t pan_lerp_hdb(const int32_t *tab, int pan) {
    int i = 0;
    while (i < 8 && pan > PAN_ANCHOR[i + 1]) i++;
    int p0 = PAN_ANCHOR[i], p1 = PAN_ANCHOR[i + 1];
    int32_t v0 = tab[i], v1 = tab[i + 1];
    if (p1 == p0) return v0;
    return v0 + (int32_t)((int64_t)(v1 - v0) * (pan - p0) / (p1 - p0));
}
```

`PAN_ANCHOR`/`PAN_ATTEN_L_HDB`/`PAN_ATTEN_R_HDB`'s nine values are `[M:
probe 25]`, read directly off the reference (see SPEC.md 3.6 for the
dB table and derivation). `pan_lerp_hdb`'s linear-in-hundredths-of-a-dB
interpolation *between* those nine points is the `[F:fitted]` part.

### 2. How it was fit

- **Probe/reference:** `probes/25_pan_law.mid` vs `probe-results/25.flac` --
  a CC10 sweep {0,16,32,48,64,80,96,112,127} on a held Sine Wave (bank 8
  program 80) note.
- **Anchor measurement:** for each of the 9 swept notes, RMS over four
  independent early sub-windows (onset+[0.00,0.02], [0.02,0.05], [0.05,0.10],
  [0.10,0.20] s -- before the patch's own decay and before any reverb
  buildup; all four windows agree to within 0.03 dB per point, confirming
  the measurement isn't attack-transient or reverb-bleed noise) gives the
  per-channel dB values in SPEC.md 3.6's table. Small in-plateau noise
  (<0.1 dB, e.g. CC10=48/80 read -0.01 to -0.03 dB rather than exactly 0)
  was rounded to 0 since it is well within measurement noise and the
  design's own unity-gain ceiling.
- **Why linear interpolation (not a closed-form curve):** several standard
  and disassembly-adjacent candidates were tried and rejected against the
  9 measured points before settling on plain interpolation: constant-power
  sin/cos (predicts a mid-pan dip that measured data does not show, rejected
  outright by the center-plateau finding alone); the original disassembly
  formula (`g_table_vel`/`g_table_lin` reused, see SPEC.md 3.6, same
  rejection); a single power-law fit to the outer-quarter dropoff (exponent
  derived from CC10=96/112 does not extrapolate to CC10=127's measured
  -20.2 dB, under-predicting by >10 dB); an additive constant "floor" atop
  the disassembly formula's dip (cannot simultaneously satisfy the flat
  center *and* the correct outer-quarter dropoff magnitude). None fit
  cleanly; piecewise-linear-in-dB through the actual measured points is the
  smallest correct thing that reproduces probe 25 exactly at all 9 tested
  values and interpolates monotonically and reasonably in between.

### 3. Where a future RE pass should look

The real pan-gain consumption code (whatever it is -- confirmed *not* to be
`0x19bfe`-`0x19c2a` as previously read, see SPEC.md 3.6 and the open-items
ledger entry 4) was not relocated in this pass. A future RE attempt with a
finer-grained pan-sweep reference (CC10 at every integer step, not every 16)
would let the outer-quarter accelerating dropoff be characterized precisely
enough to either identify the real closed-form law or justify the current
interpolation with tighter anchors.

### 4. Residual delta the fit bought

The harness (--skip-smoke mode), `compare_spectral_22050.overall_db`
(more-negative = better), 18-probe core set (03-06,08-10,12-21):

| build | probe 24 baseline RMS vs ref 8556 | probe 25 L/R shape match | 18-probe mean (dB) |
|---|---|---|---|
| before (disassembly formula) | 5995 (-3.09 dB) | dL/dR vary +4.8 to -27 dB across sweep (formula's own shape, doesn't track reference) | -3.471 |
| after (probe-25 anchors + lerp) | 8508 (-0.05 dB) | dL/dR constant at -1.29 dB (+/-0.03) across all 9 CC10 points -- shape matches reference exactly, residual is a flat per-patch offset unrelated to pan | -8.338 |

Every one of the 18 core probes improved (more negative) after the fix,
consistent with a constant per-voice gain term being corrected everywhere,
not just at center pan. `GENERAL_SERUM` (informational, not gate-relevant):
spectral residual moved -2.5 dB -> -1.2 dB (worse on this specific
phase-insensitive integration metric) and envelope-correlation r moved
0.5941 -> 0.5898 (~flat, within noise) -- both informational only, per this
project's own instructions, and not a ship blocker given the 18-probe gate
and both targeted probes (24, 25) improved decisively. megalovania
still terminates correctly at 156.05s / 3,440,640 frames, not truncated,
after the fix.

### Falsifiability

Re-render `probes/25_pan_law.mid`, decode both channels, and confirm the
per-CC10-point dB deltas between synth and `probe-results/25.flac` are
constant (a single flat offset, not a pan-dependent shape) -- a non-constant
delta across the sweep would mean the pan law itself, not just an unrelated
patch-level attenuation, regressed. Re-render `probes/24_gain_staging.mid`'s
baseline note and confirm its RMS is within ~0.2 dB of 8556.
---

## Entry 6: pitch-LFO (vibrato) -- SHIPPED, `[M: probe 06]`, gated on direct rate/depth measurement not on probe 06's own spectral score

**Status: `[M: probe 06]`, SHIPPED.** `src/engine/dls.h`/`src/engine/dls.c`/`src/engine/voice.h`/
`src/engine/voice.c`/`src/engine/render.c` all changed. `voice_update_pitch`'s LFO hook is no
longer the bare `+ 0 /* + LFO (step 3) */` placeholder -- it now sums a
real, per-instrument, CC1-gated vibrato. This supersedes Entries 2 and 3
(both "NOT SHIPPED"): the decision this time is to ship on the direct
rate/depth measurement against `probes/06_modwheel.mid`, per explicit
standing instruction, because that probe's own `compare_spectral_22050`
alignment search is independently fragile on its ten-repeated-identical-
notes structure (flagged `IMPLAUSIBLE_OFFSET` on the untouched baseline
before any of this work -- unchanged from Entries 2/3's own finding,
reconfirmed fresh this pass) and is therefore not used as the gate.

### 1. What changed vs. Entry 3 (the design that was re-derived and shipped, with one change)

- `src/engine/dls.c`/`src/engine/dls.h`: `art1` `usSource==1(LFO)`, `usDestination==0x0003(PITCH)`
  parsed into `Artic.lfo_pitch_inherent_cents` (`usControl==0`) and
  `Artic.lfo_pitch_cc1_cents` (`usControl==0x0081`), both via the existing
  `high word of lScale, clamped [-1200,1200]` formula (reused from
  `eg2_to_pitch_cents`, unchanged formula, `[A]`).
- `src/engine/voice.c`: per-voice `lfo_phase`/`lfo_freq_hz`/`lfo_delay_s`/`lfo_elapsed_s`,
  cached at note-on from the voice's own region's `artic->lfo_freq_tc`/
  `lfo_delay_tc`. Rate: `freq_Hz = 8.176 * 2^((lfo_freq_tc/65536)/1200)` (the
  DLS-1/SF2 absolute-pitch-cents convention). Delay: reuses the existing
  `timecents_to_seconds` helper (already `[A]` for destination `0x0105`).
- `src/engine/render.c`: `render_frames` now sub-chunks every call into 64-frame
  (~2.9ms) slices, calling `voices_update_modulation()` (pitch+gain) then
  `voices_advance_lfo()` (new, in `src/engine/voice.c`) once per slice, so a held note
  with no intervening MIDI events still gets its LFO phase advanced
  frequently enough to actually oscillate (`render_voice`'s own state,
  `v->phase_pos`, is unaffected by the slicing -- N one-shot calls of length
  k render identically to one call of length N*k except for the modulation
  refresh cadence).
- **Depth model -- the one change from Entry 3's literal design**: only the
  CC1-gated depth is applied, `depth_cents = lfo_pitch_cc1_cents * (live
  CC1/127)`. The ungated ("inherent", `ctrl=0`) depth is parsed into
  `Artic.lfo_pitch_inherent_cents` but **deliberately not summed in** --
  see section 3, this is a corpus-wide-measured decision, re-confirmed
  fresh against the *current* codebase (which has since gained the pan-law
  rewrite, `GAIN_CEILING`, and the gain smoother since Entries 2/3 were
  written -- the regression below was re-verified against that current
  state, not assumed from the older entries).

### 2. Measured against probe 06 (own fresh measurement, the LFO fit check, unmodified, this is the `[M]` ship evidence)

Rate: **5.996 Hz** on every CC1>=32 segment, both instruments (reference:
5.976-5.986 Hz on the same segments) -- matches the ~6 Hz target.

Depth (peak-to-peak, `2 x` the script's own per-segment amplitude-cents
measurement), synth vs. reference:

| segment | synth pp (cents) | ref pp (cents) |
|---|---|---|
| prog48 (strings) cc1=0   | 19.86 | 19.82 |
| prog48 cc1=32             | 21.60 | 19.50 |
| prog48 cc1=64             | 43.80 | 40.46 |
| prog48 cc1=96             | 65.52 | 61.12 |
| prog48 cc1=127            | 86.10 | 80.90 |
| prog73 (flute) cc1=0      |  0.02 |  7.28 |
| prog73 cc1=32             | 19.96 | 25.90 |
| prog73 cc1=64             | 41.22 | 45.82 |
| prog73 cc1=96             | 61.98 | 65.24 |
| prog73 cc1=127            | 81.88 | 84.22 |

The LFO fit check, run against `dist/msgs.wasm` (default 5.0-cent
tolerance, single-amplitude not pp), reports **"Overall: MATCH"** (all ten
segments, including the one its own docstring already excuses -- prog48
cc1=0's reference rate measures 5.145 Hz, not ~6 Hz, dominated by the
String Ensemble sample's own baked-in chorus content, present identically
in both captures since both play the same underlying `gm.dls` PCM).

**Known, accepted gap**: flute's own cc1=0 point (0.02 pp synth vs. 7.28 pp
reference) is the one segment furthest off-target, entirely attributable to
dropping the inherent term (program 73's own `gm.dls` data carries no
`ctrl=0` connection at all in the strings case, but a real, if small, one
elsewhere in the corpus -- see section 3). This is a known, deliberate
trade-off, not a rate/scaling bug: the LFO fit check still reports this
segment as `match` (3.62 cents below its 5.0-cent tolerance).

### 3. Why the inherent term was dropped -- confirmed regression on a NON-06 probe

Built and measured the "full" model (`inherent_cents + cc1_cents*(CC1/127)`,
the literal briefing formula) against the current codebase first. A fresh
run of the harness (--skip-smoke mode), `compare_spectral_22050.overall_db`:

| probe | baseline (no LFO) | full (inherent+CC1) | CC1-only (shipped) |
|---|---|---|---|
| 04_envelope | **-12.025** | **-1.325** | **-12.025** |
| 06_modwheel (not gated) | -5.849 | -5.848 | -5.301 |
| 13_edge | -10.395 | -10.729 | -10.729 |
| every other probe (20 of 23) | unchanged | unchanged | unchanged |

`04_envelope` (Acoustic Grand Piano, program 0, zero CC1 events in that
probe's own MIDI) regresses **+10.7 dB** under the full model -- confirmed
directly, not inferred: `gm.dls`'s own `art1` data gives that instrument a
real, correctly-parsed `ctrl=0` inherent depth of exactly 1 cent, which is
inaudible on its own (an order of magnitude under the ~5-cent JND for a slow
vibrato) but is enough to desync this sustained-tone probe's phase-sensitive
alignment/comparison -- the identical failure mode Entries 2/3 already
diagnosed for the CC1 term on probe 06 itself, here triggered by a different
(inherent-depth) instrument instead. This is a **real, confirmed regression
on a non-06 probe**, which per this task's own standing instruction ("if it
regresses a NON-06 probe ... report that and fix or hold") is treated as
disqualifying for the full model, not as harness noise to route around.

Dropping the inherent term (CC1-only) restores `04_envelope` to the baseline
**exactly** (`-12.025` dB both before and after, to full float precision --
confirmed via the harness's own JSON output, not just the printed
table). `13_edge` moves `-10.395` -> `-10.729` dB (-0.334 dB) under BOTH the
full and CC1-only models identically -- **verified this is legitimate, not
a leak**: `probes/13_edge.mid` contains exactly one real CC1 event (checked
directly by parsing the SMF), so an instrument on that channel with a real
`lfo_pitch_cc1_cents` connection correctly receiving vibrato from that one
real message is expected, working behavior, not a bug -- the same small
magnitude and direction under both models confirms it's driven by the CC1
term specifically, not the inherent one. `06_modwheel` moves `-5.849` ->
`-5.301` dB under CC1-only (an *improvement* on its own score, though this
probe is explicitly not the gate here either way, per its alignment-search
fragility).

**17(+5)-probe / 22-non-06-probe regression check**: of the 23 currently-
graded probes (excluding `01/02/07/11`, which have no valid reference audio
to compare against at all -- `NO_REFERENCE_AUDIO`/`STALE_REFERENCE`, not
part of any grade), 22 are non-06. 21 of those 22 are **byte-identical**
(0.0000 dB delta to full float precision) between baseline and the shipped
build; only `13_edge` moves, by the small, verified-legitimate amount above.
Non-06 mean: baseline **-8.2108 dB** -> shipped **-8.2260 dB** (a net
-0.0152 dB, i.e. very slightly *improved*, driven entirely by 13_edge's own
correctly-applied vibrato). All-23-probe mean (including 06, informational
only): **-8.1081 dB** -> **-8.0989 dB**.

**No-LFO-connection voices get exactly zero vibrato**, confirmed two ways:
(1) structurally -- `voice_lfo_cents()` returns `0` immediately whenever
`v->artic->lfo_pitch_cc1_cents == 0`, which is the `artic_defaults()`
default for any instrument/region that authors no `usSource=1,
usControl=0x81, usDestination=0x0003` connection at all; (2) empirically --
21 of 22 non-06 probes render byte-for-byte identical PCM to the pre-change
baseline (verified via the harness's own spectral comparison at full float
precision, a metric sensitive enough to have flagged even a 1-cent
difference in the `04_envelope` case above).

### 4. GENERAL SERUM (north star, informational) and megalovania

The harness (--skip-smoke mode), `field/Kot_and_A64-GENERAL_SERUM.mid`
(5,382,144 frames rendered, terminated, not truncated, both before and
after):

| metric | baseline | shipped (CC1-only) |
|---|---|---|
| spectral residual (PRIMARY) | -2.2731 dB | -2.2720 dB |
| envelope correlation r | 0.64776 | 0.64775 |

Effectively unchanged (both deltas are noise-level) on this blunt,
whole-mix, 244s-long aggregate metric -- but the vibrato is directly
confirmed active and correctly gated within it: channel 13 (GM program 81,
"Lead 2 (sawtooth)", which `gm.dls` gives a real 47-cent CC1-scaled
LFO->pitch connection and no inherent one) sends live CC1 automation
(0<->127) from t~=125.6s to t~=144.9s in that MIDI file. A direct PCM diff
(own fresh A/B render, LFO code force-disabled vs. shipped) shows the
rendered audio differs in exactly that window (first differing sample at
t=124.25s, last at t=135.76s, 71,657 of 10,764,288 samples differ) and is
byte-identical everywhere else -- the vibrato is real, audible, and
correctly confined to the one channel/time-range that actually has both a
CC1-gated connection and live CC1 automation, in a piece this project's own
briefing calls out as its north star.

megalovania (the user's external test MIDI, not part of this tree) still terminates correctly at 156.05s /
3,440,640 frames (unchanged, confirmed via direct render). **Correction of
the briefing's own expectation, confirmed by direct measurement, not
inferred**: megalovania contains **zero CC1 events anywhere** (checked
directly, all 14 tracks) -- every one of its channels' `modulation` stays at
its default 0 for the whole piece, so `depth_cents = cc1_cents * (0/127) =
0` exactly for every voice, every sample. A direct PCM diff (LFO-disabled
vs. shipped build) confirms the render is **byte-for-byte identical**
before and after this change. This is a real, measured, content-driven fact
about that specific MIDI file (its author never moves the mod wheel), not a
driver limitation -- `gm.dls`'s own data gives every one of megalovania's 11
distinct instruments a real ~47-cent CC1-gated vibrato connection (checked
directly), it simply has nothing to gate on in this file. The instruments'
own *inherent* depths (also checked directly: 1-6 cents, `{-1,1,2,2,2,6}`
across the 6 of 11 that have one at all) are too small to constitute
audible vibrato even if applied, and applying them is exactly what caused
the confirmed `04_envelope` regression in section 3 -- so there is no
available design, given `gm.dls`'s own actual data, that gives
megalovania audible vibrato without either fabricating depth `gm.dls`
doesn't author or reintroducing the confirmed non-06 regression. This is
reported as a direct refutation of that one clause of the shipping
briefing, not glossed over.

### 5. Where a future RE pass should look

Unchanged from Entry 3 section 4: the real depth-combination/mixer code
(how the original driver itself turns the two stored connections plus a
live CC1 value into a pitch contribution) was never located in this
project's disassembly work and remains `[O]`; a future pass should also
revisit the harness's cross-correlation alignment search for
`probes/06_modwheel.mid`'s repeat-ambiguity (out of this pass's file-scope
restriction) if a tighter probe-06-native ship gate is ever wanted instead
of the direct-measurement approach used here.

### Falsifiability

The LFO fit check (rate/depth vs. reference, described in section 2) should
report "Overall: MATCH" against the shipped build (reproduced above). The
harness (`--skip-smoke` mode) should reproduce the non-06 byte-identical/13_edge-only-moves
pattern in section 3 against any future change touching this code path --
any other probe moving would indicate the CC1-gating has stopped containing
the feature's blast radius to voices that actually have the connection and
receive live CC1.

---

## Entry 7: pan law replaced with centre-normalised `g_table_lin` sqrt-table (probe 07) -- SHIPPED

**Status: SHAPE `[M: probe 07]`, re-centering `[F:fitted, SHIPPED]`.**
`src/engine/voice.c`'s `voice_update_gain()` now computes both channels'
pan attenuation from the single disassembly-recovered linear/sqrt table
`g_table_lin` (SPEC.md S3.6's `gainA`), reverse- and direct-indexed, instead
of the probe-25-anchored two-table lerp this project previously shipped
(Entry 5, now superseded -- see the note at the top of that entry). The
lookup itself (which table, which indexing direction, per channel) is
measured against a new reference, `probes/07_pan_volume.mid` /
`probe-results/07.flac`; the additive `- g_table_lin[63]` centre-recentering
term is a fitted correction, not itself measured.

### 1. Value chosen

`src/engine/voice.c`, `voice_update_gain()`:

```c
int32_t gainA_hdb = g_table_lin[127 - pan] - g_table_lin[63];
int32_t gainB_hdb = g_table_lin[pan] - g_table_lin[63];
```

replacing the two-table anchor/lerp scheme (`PAN_ANCHOR`, `PAN_ATTEN_L_HDB`,
`PAN_ATTEN_R_HDB`, `pan_lerp_hdb()`), all four deleted (grepped first to
confirm nothing else in the tree referenced them; nothing did).
`g_table_lin` is unchanged, already built in `src/engine/tables.c`
(`g_table_lin[0] = -2500`; `g_table_lin[v] = trunc(1000*log10(v/127))` for
`v=1..127`, hundredths of a dB; `g_table_lin[63] = -304`). No new table.
`GAIN_CEILING` and everything else in `voice_update_gain()` is unchanged.

### 2. How it was fit

- **SHAPE, `[M: probe 07]`:** `probes/07_pan_volume.mid` vs
  `probe-results/07.flac` (new reference, first-ever measurement of this
  probe) -- Acoustic Grand Piano, note 60 vel 100, CC7=100, CC11=127, a
  9-step CC10 sweep {0,16,...,127}. Alignment recovered via RMS-envelope
  cross-correlation (+4180 samples @22050, r=0.893) after confirming the
  harness's own whole-file sample-domain xcorr fails on this probe
  (r=-0.245, IMPLAUSIBLE_OFFSET-class result) -- that whole-file number is
  therefore NOT used as evidence here; the RMS-envelope-recovered alignment
  is. Reference per-channel dB, each channel normalised to its own max,
  stable within 0.3 dB across three independent windows
  (`[0.02,0.15]`,`[0.20,0.60]`,`[1.00,1.40]` s post-onset):

  ```
  CC10:      0      16      32      48      64      80      96     112     127
  L:      0.00   -0.41   -1.06   -1.68   -2.69   -3.85   -5.15   -8.22  -23.93
  R:    -26.14   -9.93   -6.90   -4.95   -3.72   -2.65   -1.34   -0.58    0.00
  ```

  L loud at CC10=0 (hard left) confirms Left = `gainA` (SPEC_GAPS.md S9, now
  settled by this same probe). `P_L + P_R` (each channel's own power,
  normalised to its own extreme) = 1.00 +/- 0.04 across the whole sweep --
  the underlying law is constant-power, corroborating a sqrt (amplitude)
  table read reverse/direct per channel rather than the squared table on
  either side. The table's hardcoded index-0 floor of -25.00 dB is
  corroborated twice independently: probe 07's hard-pan opposite channel
  reads -26.14 / -23.93 dB (mean -25.04), and probe 25's own -20.21 dB
  anchor (Entry 5) is -24.99 dB once that probe's separately-measured 4.78
  dB saturation offset (see the note atop Entry 5) is removed.
  `gainB` = the squared table `g_table_vel` direct-indexed (the other half
  of SPEC.md S3.6's disassembly reading) is REFUTED by the same probe: it
  predicts -11.90 dB at centre pan where probe 07 measures -3.72 dB.
- **Why Entry 5's anchors were not the pan law:** probe 25's reference
  plateau (L: 0,0,0,0,0,0,-1.40,-4.50,-20.21) is gain-ceiling saturation, not
  the pan law. `measured = min(GAIN_CEILING_dB, A + 10*log10((127-pan)/127))`
  with the Sine patch (bank 8 program 80, CC7=127) driven `A - GAIN_CEILING_dB
  = 4.78` dB above the ceiling fits all six unsaturated points on both
  channels independently: 4.78, 4.78, 4.79, 4.79, 4.79, 4.79 dB. Directly
  checked and ruled out: this is NOT ±32767 sample clipping (probe 25
  reference peak is 15880/32767, zero samples >=32000). Probe 25's own
  anchor *measurements* reproduce exactly on re-measurement -- Entry 5's
  measurement was sound, only its interpretation (reading a saturation
  plateau as the pan law itself) was wrong.
- **The `- g_table_lin[63]` re-centering, `[F:fitted, SHIPPED]`:** the
  literal law with no re-centering (candidate C1, `gainA_hdb =
  g_table_lin[127-pan]`, `gainB_hdb = g_table_lin[pan]`) puts -2.97 dB of
  attenuation on BOTH channels at centre pan, where the current corpus's
  overall gain staging was tuned assuming ~0 dB at centre. Subtracting
  `g_table_lin[63]` (-3.04 dB) from both channels restores centre-pan level
  to what the corpus is tuned around while preserving the table's own
  reverse/direct-indexed shape -- an engineering choice to preserve level,
  not itself a measured quantity. Error metric: the harness's
  `--skip-smoke` `compare_spectral_22050.overall_db` (more negative =
  better) across the full probe corpus, plus probe 07's own per-channel
  shape RMS error (each channel normalised to its own max, against the
  measured table above).

### 3. Where a future RE pass should look

The missing ~3 dB the re-centering (`- g_table_lin[63]`) stands in for is
presumed to live in the unrecovered gain-register scaling, SPEC.md S6.4.5
"Open items" #16 (`[O]`, the same `<<8>>5`-class intermediate-scaling gap
already on record for `GAIN_CEILING`'s own comment in `src/engine/voice.c`) -- a future
RE pass should look there to retire this fit with a real recovered constant
instead of a level-preserving offset chosen to match current behaviour.
Separately, probe 07's own residual shape error (section 4) suggests the
real exponent may not be exactly 0.5 (sqrt); a future attempt with a
finer-grained CC10 sweep could pin that down without resorting to a fresh
curve fit.

### 4. Residual delta the fit bought

> **RE-MEASURED 2026-07-25 under the corrected aligner — the table below is
> superseded but kept as recorded.** Every figure in it was computed by the
> old whole-file waveform aligner, which mis-locked on 14 of 24 probes (see
> the re-baseline note at the top of this file). The BEFORE-vs-C2 comparison
> was re-run with both pan laws built and graded under the corrected
> two-stage aligner:
>
> | | Entry 5 law (BEFORE) | Entry 7 law (C2, shipped) | delta |
> |---|---|---|---|
> | 07_pan_volume | −8.51 | **−8.79** | −0.28 |
> | 25_pan_law | −16.45 | **−17.02** | −0.57 |
> | 18_key_groups | −6.85 | **−7.38** | −0.53 |
> | 26_other_gains | −10.44 | **−10.51** | −0.07 |
> | GENERAL_SERUM | −4.172 | **−4.239** | −0.07 |
> | corpus mean | −10.015 | **−10.113** | −0.099 |
>
> **18 probes better, 0 worse** (>0.02 dB), so the ship decision holds and is
> strengthened. Note specifically that **26_other_gains, recorded below as
> the single regression this change cost (+0.12 dB), was a misalignment
> artifact** — under correct alignment it improves like everything else, and
> the caveat about GS part pan that was offered to excuse it was not needed.
> The C1 (no re-centering) column was not re-run; C1 was rejected by margins
> up to +8.20 dB, far outside anything the alignment error could account for.

The harness (--skip-smoke mode), `compare_spectral_22050.overall_db`
(more-negative = better), self-tests PASS on all three builds, corridor
render 52.57 s unchanged on all three:

| probe | BEFORE (Entry 5 anchors/lerp) | C1 (no re-centering) | C2 (`- g_table_lin[63]`, shipped) | C1-BEFORE | C2-BEFORE |
|---|---|---|---|---|---|
| 03_velocity | -4.02 | -4.42 | -4.00 | -0.40 | +0.02 |
| 04_envelope | -12.02 | -7.15 | -12.09 | +4.88 | -0.07 |
| 05_pitchbend | -8.32 | -6.74 | -8.31 | +1.58 | +0.01 |
| 06_modwheel | -5.30 | -4.22 | -5.31 | +1.08 | -0.01 |
| 07_pan_volume | -4.47 | -4.12 | -4.58 | +0.35 | -0.11 |
| 08_reverb | -11.39 | -6.34 | -11.48 | +5.04 | -0.09 |
| 09_chorus | -16.54 | -8.34 | -16.69 | +8.20 | -0.16 |
| 10_polyphony | -4.58 | -3.97 | -4.58 | +0.60 | +0.00 |
| 12_gs_sysex | -12.78 | -7.08 | -12.88 | +5.70 | -0.10 |
| 13_edge | -10.73 | -6.69 | -10.78 | +4.04 | -0.05 |
| 14_running_status | -11.81 | -7.40 | -11.86 | +4.41 | -0.05 |
| 15_banks | -9.40 | -6.73 | -9.42 | +2.67 | -0.02 |
| 16_drum_parts | -9.61 | -6.03 | -9.66 | +3.58 | -0.05 |
| 17_master_volume | -12.63 | -7.05 | -12.73 | +5.58 | -0.10 |
| 18_key_groups | -6.85 | -5.18 | -7.38 | +1.67 | -0.53 |
| 19_prior_art | -1.54 | -1.75 | -1.53 | -0.21 | +0.01 |
| 20_voice_count | -7.05 | -5.00 | -7.07 | +2.05 | -0.02 |
| 21_steal_policy | -4.87 | -3.90 | -4.87 | +0.97 | -0.01 |
| 22_no_gs_reset | -0.43 | -0.41 | -0.43 | +0.02 | +0.00 |
| 23_rpn_tune | -6.33 | -4.98 | -6.34 | +1.35 | -0.01 |
| 24_gain_staging | -9.15 | -7.45 | -9.14 | +1.70 | +0.01 |
| 25_pan_law | -10.90 | -7.97 | -11.57 | +2.93 | -0.67 |
| 26_other_gains | -4.80 | -4.52 | -4.68 | +0.28 | +0.12 |
| 27_gain_curves | -5.22 | -5.25 | -5.21 | -0.03 | +0.01 |
| GENERAL_SERUM | -2.27 | -2.56 | -2.27 | -0.28 | +0.01 |

**C1 (no re-centering) -- measured and rejected, recorded per this file's
own rule on honestly-reported negative results:** 3 improved / 20 regressed,
several by multiple dB (09_chorus +8.20, 08_reverb +5.04, 17_master_volume
+5.58, 12_gs_sysex +5.70). The uniform -2.97 dB centre-pan attenuation this
candidate adds on every voice, every channel, everywhere, regresses the
large majority of the corpus -- not shipped.

**C2 (`- g_table_lin[63]` re-centering, SHIPPED):** 9 improved / 1 regressed.
The sole regression is `26_other_gains` (+0.12 dB) -- confirmed independent
of this change: probe 26's own section A exercises a GS-part pan SysEx that
this implementation parses but does not apply (SPEC_GAPS.md #10/#11's scope,
untouched by this pass), so that probe's pan content is wrong for an
unrelated, pre-existing reason, not because of this fit.

**Probe 07 pan-SHAPE RMS error** (each channel normalised to its own max,
against the reference table in section 2): BEFORE (Entry 5 anchors/lerp)
**3.454 dB** -> C2 (shipped) **0.679 dB**. C1 is identical to C2 at 0.679 dB
on this metric, since per-channel normalisation cancels the constant offset
that is the only difference between C1 and C2 -- this metric alone cannot
distinguish the two candidates; the corpus-wide `overall_db` table above is
what separates them.

**Residual still open (not pursued):** C2's 0.679 dB shape error is
systematic, not noise -- the reference is consistently *less* attenuated
than the sqrt table predicts across the mid-sweep (e.g. measured -2.69 dB
vs predicted -2.97 dB at CC10=64; -5.15 dB vs -6.12 dB at CC10=96). An
amplitude exponent of ~0.43 fits probe 07 better than the table's 0.5, but
adopting it would mean replacing a disassembly-recovered table with a fresh
curve fit, which was NOT pursued in this pass.

### Falsifiability

Re-render `probes/07_pan_volume.mid`, recover alignment via the same
RMS-envelope cross-correlation method (whole-file sample-domain xcorr is
independently known to fail on this probe, see section 2), decode both
channels, normalise each to its own max, and confirm the per-CC10-point dB
values track the reference table in section 2 to within roughly the 0.679
dB RMS this entry measured -- a materially larger error would mean the
lookup/re-centering regressed. the harness (--skip-smoke mode) should
reproduce the C2 column above (9 improved / 1 regressed vs. the prior
anchored-lerp build, 26_other_gains the sole, independently-explained
regression) against any future change touching this code path.

---

## Entry 8: pitch bend / LFO applied outside the CentsToRatio clamp -- SHIPPED, `[M: probe 30]`, not a fit

**Status: `[M: probe 30]` -- measured, not fitted.** Recorded here rather than
only in `SPEC_GAPS.md` because it retires two open items and because the route
to it went through a *rejected* candidate worth keeping on the record.

`src/engine/voice.c` now computes the phase increment from two clamped factors
instead of one clamped sum: `voice_note_on` latches
`base_ratio_q12 = CentsToRatio(fineTune + (key-unity)*100 + RPN1 + RPN2)`, and
`voice_update_pitch` multiplies that by `CentsToRatio(bend + LFO)`.

**What settled it.** `probes/30_tune_clamp_bend.mid` / `probe-results/30.flac`
(new capture) puts keys 105-127 of program 86 past the ±4800 clamp with +24
semitones of RPN2, with and without a bend sweep:

- **Section B (no bend):** reference keys 119 and 127 both read 3028.8 Hz --
  collapsed onto one pitch. Latched tune is therefore INSIDE the clamp.
- **Section C (same base + bend sweep):** reference keys 119/127 read
  1685.0/1687.7 Hz -- they moved. Bend is therefore OUTSIDE it.
- Sections A and D, and keys 60-105 throughout, match our render at ratio
  1.000 both before and after, so the change is confined to the saturated case.

**Rejected candidate, recorded per this file's negative-result rule.** Before
probe 30 existed, an earlier attempt moved RPN1/RPN2 *outside* the clamp
instead. It scored better on the GENERAL SERUM measure-226 isolate than the
shipped fix does (+0.24 dB vs +1.48 dB, against +2.33 dB before either) and was
still wrong: it regressed GENERAL SERUM as a whole by 3.1 dB, and section B
above then refuted it outright. **A candidate that wins on the one passage that
motivated it can still be measurably wrong**; the corpus gate caught it and the
probe explained why.

**Residual delta:**

| metric | before | after |
|---|---|---|
| corpus mean | -10.113 | **-10.391** |
| 06_modwheel | -6.49 | **-13.12** |
| GENERAL SERUM spectral | -4.239 | **-4.704** |
| GENERAL SERUM envelope r | 0.8313 | **0.8645** |
| measure-226 isolate | +2.33 | **+1.48** |
| probes regressed | -- | **0** |

`06_modwheel` moving 6.63 dB is the informative one: the pitch LFO is live
modulation, so it escapes the clamp under this change too. That probe defeated
Entries 2, 3 and 6, each of which treated it as an LFO *depth/rate* problem;
part of it was never about depth or rate at all.

**Residual closed by `[M: probe 31]`.** The 5.6% gap on probe 30's clamped
keys was an artifact of measuring a saw above Nyquist (argmax landing on
different aliased partials), not a pitch error.
`probes/31_tune_clamp_bend_sine.mid` / `probe-results/31.flac` repeats the same
structure on bank 8 program 80 (Sine Wave, the only single-partial patch in
gm.dls): **sections A/B/D match at ratio 1.000 on all 18 notes; section C's
bend sweeps fall within 0.994-1.014 with no outlier**, clamped keys 119/127
included. Nothing from this entry remains open.

---

## Entry 9: phase-step ramp (`PITCH_RAMP_RATE_FRAC_PER_MS` / `PITCH_RAMP_MAX_MS` in src/engine/voice.c) -- SHIPPED, SPEC_GAPS.md #19

**Status: `[F:fitted, SHIPPED]`.** Implements SPEC.md S6.6/S6.4.1's `[A]`
mechanism: the mixer holds the phase step in a ramp accumulator, re-derived
from a caller-supplied linear step every `ramp_period` samples, held
constant between refreshes. This project's prior code (`voice_update_pitch`)
wrote `v->phase_step` directly every block -- no ramp at all. What SPEC.md
leaves `[O]` is the caller's own slope-selection rule and the envelope
generator's cadence; both constants below are this project's fit for that
open half, not a recovered value.

### 1. Values chosen

`src/engine/voice.c`, `voice_update_pitch()`:

```c
#define PITCH_RAMP_RATE_FRAC_PER_MS 0.03635
#define PITCH_RAMP_MAX_MS 20.6375
```

On a target change, `src/engine/voice.c` computes a fixed per-sample delta
(`phase_step_ramp_step`) once, sized so the ramp is the FASTER of two rules:
- **rate-limited**: move at most `PITCH_RAMP_RATE_FRAC_PER_MS` percent of the
  voice's OWN phase_step (at the moment the ramp starts) per millisecond --
  so a huge bend takes proportionally longer than a tiny one;
- **duration-capped**: never let the total ramp exceed `PITCH_RAMP_MAX_MS`,
  regardless of distance.
`src/engine/render.c`'s `render_voice` then applies that fixed step once per sample
until `phase_step` reaches `phase_step_target`, snapping exactly on arrival
(no overshoot, no separate "ramp_period" countdown -- see the design note
below for why per-sample was chosen over a coarser refresh cadence).
New notes (`voice_note_on`) snap `phase_step` straight to target, bypassing
the ramp entirely -- the ramp is for modulation reaching an
ALREADY-sounding voice, not for note-on itself.

### 2. How it was fit

- **Probe/reference:** `probes/33_pitch_ramp.mid` / `probe-results/33.flac`,
  section `A_step_-24` (8 reps, sine patch bank 8 program 80, key 72,
  RPN0=24, a -24 semitone bend step on an already-sounding note).
- **Error metric:** carrier instantaneous frequency via an FFT-based Hilbert
  transform (no scipy available in the harness's Python environment) on a bandpassed
  (0.5x-1.5x the expected f0/f1 span) segment around each step; 10-90%
  crossing time between the pre-step and post-step median frequency, and
  R² of a linear fit to frequency (Hz) vs. to cents over that 10-90% window.
- **Before fit** (direct write, no ramp): glide = 0.50ms (effectively
  instantaneous; the task's own probe measured 0.73ms on an earlier build,
  both read as "instant" at this resolution).
- **REFERENCE**: mean glide over 7 clean reps (the 8th rep's window runs off
  the end of the render) = **14.83ms** (individual reps 16.34-16.39ms,
  pulled down by one truncated 4.06ms outlier -- the 7 untruncated reps
  average 16.37ms), R²(Hz) = 0.9974, R²(cents) = 0.9780. This independently
  reproduces the task's own -24-semitone measurement (16.51ms, sd 3.90,
  R²(Hz) 0.9986 vs. R²(cents) 0.9782) closely enough to confirm the same
  underlying phenomenon and confirm SPEC's phase-step (not cents) domain.
- **Rate derivation:** distance_frac = |1 - ratio| = |1 - 2^(-24/12)| = 0.75.
  10-90% covers 80% of a linear ramp's total span, so total duration =
  16.51/0.8 = 20.6375ms; rate = 0.75 / 20.6375ms = 0.03635
  fraction-of-starting-phase_step per ms. `PITCH_RAMP_MAX_MS` was set to
  that same 20.6375ms total-duration figure (see "duration cap" below for
  why it exists).
- **After fit (rate only, no cap):** glide = 17.73ms, R²(Hz) = 0.9993,
  R²(cents) = 0.9794 -- matches the reference's shape and magnitude within
  the reference's own rep-to-rep spread.
- **Why a rate, not a fixed duration:** a fixed-duration ramp (every target
  change takes the same wall-clock time regardless of size) was the other
  candidate raised for this fit. Rejected because the pitch-LFO (vibrato)
  recomputes its target every `LFO_UPDATE_FRAMES` sub-chunk (~2.9ms); a
  fixed ~20ms glide restarted that often would lag/damp real vibrato
  noticeably. A rate scaled to the voice's own current phase_step lets a
  ~30-cent vibrato wobble settle in a fraction of a millisecond
  (unaffected) while still reproducing the measured -24-semitone glide.
  Confirmed cheap to check either way: `06_modwheel`'s corpus delta is
  +0.0007dB (noise), i.e. genuinely unaffected.
- **Why the duration cap was added on top (this took an extra pass):** the
  rate-only version, run against the FULL corpus (isolated via direct
  `-D`-toggled binaries built to unique paths, not through
  the harness's shared `dist/msgs-render`, per this task's own
  contamination warning), regressed `33_pitch_ramp`'s own section B
  (+-40/+-70 semitone jumps, all on the same note/patch as the -24 fit
  point) by +2.5 to +4.5dB depending on which of the two renders' fine
  alignment lag was used for comparison -- confirmed to be a REAL, not an
  alignment-search artifact, because the regression persisted when BOTH
  renders were compared at the SAME shared lag (either render's own lag),
  ruling out the fine-xcorr-picks-a-different-local-peak trap that WAS
  responsible for probe 30's apparent regression (see below). A pure rate
  keeps stretching duration for ever-bigger jumps (a 70-semitone jump would
  take ~48ms); the reference apparently does not glide proportionally
  slower for a bigger jump. Capping total duration at the -24-semitone fit
  point's own duration (20.6375ms) leaves that data point and vibrato
  unchanged (both already resolve faster than the cap) while stopping huge
  bends from badly overshooting the one duration actually measured.
  **After adding the cap**, section B's regression shrank to -0.12 to
  +0.44dB (both shared-lag directions), and the probe-33-whole-file
  spectral delta shrank from +1.486dB to +0.297dB. This ceiling is this
  project's extrapolation beyond the single measured data point, not a
  second measurement -- a future pass with more bend-size reference
  captures (small step sizes especially, per the task's own note that
  ±2/±6/+24 "did not track reliably") should revisit both constants
  together.

### 3. Corpus-wide result (isolated A/B, both builds from the SAME tree)

Compiled directly to unique paths (`/tmp/.../uniq_control_v2` with
`-DPITCH_RAMP_RATE_FRAC_PER_MS=1e12 -DPITCH_RAMP_MAX_MS=1e12`, i.e. ramp
completes in under one sample = the old instant-write behaviour, vs.
`uniq_test_v2` at the defaults above), never through `dist/msgs-render`
while any other agent could also be rebuilding it. `compare_spectral_22050.overall_db`,
control vs. test:

| probe | control | test | delta |
|---|---|---|---|
| 01-04, 07-29 (25 probes untouched by pitch) | unchanged | unchanged | 0.000 (to 3 decimals) |
| 05_pitchbend | -8.333 | -8.228 | +0.106 (noise) |
| 06_modwheel | -13.124 | -13.123 | +0.001 (noise) |
| 30_tune_clamp_bend | -8.577 | -5.374 | +3.203 (see below -- artifact) |
| 31_tune_clamp_bend_sine | -9.284 | -9.305 | -0.021 (tiny improvement) |
| 32_ramp_shape | -6.531 | -6.534 | -0.003 (tiny improvement) |
| 33_pitch_ramp | -7.114 | -6.817 | +0.297 (see Entry text above) |
| 34_sfx_bank_identity | -2.768 | -2.768 | 0.000 |

**30_tune_clamp_bend's +3.2dB is a measurement artifact, not a regression,
confirmed by direct check.** the harness's alignment is two-stage:
an envelope-domain coarse lag, then a raw-waveform xcorr refine restricted
to +-50ms around it. The TRUE global-best raw-waveform lag (searched over
the full range, not just the refine window) is **identical for control and
test: -12557 samples, correlation 0.2548 vs 0.2543** -- i.e. the underlying
audio content match is essentially unchanged. The refine window doesn't
reach that true optimum (it's ~10.7ms outside it), so it picks two
DIFFERENT local peaks for control (-11228, r=0.226) and test (-10472,
r=-0.222) instead -- and cross-checking, the correlation AT EITHER specific
candidate lag is nearly identical between the two renders (0.226 vs 0.211
at control's lag; -0.225 vs -0.222 at test's lag). This is the harness's own
documented failure mode for quasi-periodic probe content (`envelope_align_offset`'s
docstring describes the same class of ambiguity for note-ladder probes);
it is not something this change caused, only something a small fine-waveform
difference (of any kind) can flip.

### 4. HueArme isolate (SPEC_GAPS.md #19's own acceptance test), bend-rate-conditioned

A hand-cut isolate pair of HueArme-Weekend (MIDI + capture, not retained in
this tree), 10,756 bend events, split into
551 windows of 0.25s and ranked by summed bend movement per window (matching
the entry's own methodology); per-window 9.5-11kHz energy relative to
per-window total energy, in dB, ours minus reference:

| | control (instant) | test (ramped) |
|---|---|---|
| low-bend-rate (bottom third) | +4.60 dB | +3.89 dB |
| high-bend-rate (top third) | +13.24 dB | +12.72 dB |
| no-bend windows | +6.06 dB | +3.26 dB |
| whole-track mean | +10.08 dB | +9.41 dB |

Moves in the right direction everywhere, does not regress the low-bend
regime, but the improvement on the acceptance test's own headline number
(high-bend excess) is modest (0.5dB) with the duration cap in place -- an
UNCAPPED rate-only build measured a much bigger swing here (13.27dB ->
6.05dB) but that version is the one that regressed probe 33 section B
above. This project chose the smaller, non-regressing improvement over the
larger, regressing one; a future pass with a genuine second bend-size
reference point could possibly recover more of the isolate's improvement
without the section-B cost, but that requires a second measured data point,
not more tuning of these two constants against the one currently available.
Whole-track (unconditioned) balance is essentially unchanged (9.5-11kHz
diff +12.4dB control vs. +12.3dB test), exactly as the entry's own note
warns it would be -- this is why the tercile split above, not the
whole-track table, is the number that matters.

### 5. Where a future RE pass should look

Same as Entry 4's gain smoother: SPEC.md S6.6 says the caller-side slope
selection and the envelope generator's own cadence are `[O]`, with the
consumption code outside every disassembled PAGE range this project
examined. A future pass that recovers the real `ramp_period` and the real
slope-selection rule should replace both constants above with the true
values; in particular it should explain why huge (+-40/+-70 semitone) bends
apparently do NOT glide proportionally slower than a -24 semitone one, and
whether the true rule looks anything like this project's ad hoc
rate-with-ceiling hybrid.

---

## Entry 10: release-duration floor (`RELEASE_FLOOR_S` in src/engine/voice.c) -- SHIPPED

**Status: `[F:fitted, SHIPPED]`.** `src/engine/voice.c`'s `start_release()`
floors the release SEGMENT duration, in seconds, at `RELEASE_FLOOR_S` before
that duration is converted to a per-sample coefficient by `exp_coef_scaled`.
Applied only on the ordinary note-off path (`!fast`); the fast-release/choke
path (`fast==1`, `RELEASE_RATE_MULT`) is untouched, still 1.0.

### 1. Value chosen

```c
#ifndef RELEASE_FLOOR_S
#define RELEASE_FLOOR_S 0.060
#endif
```

`src/engine/voice.c`, `start_release()`:

```c
if (!fast && rel_s < RELEASE_FLOOR_S) rel_s = RELEASE_FLOOR_S;
v->env_release_coef = exp_coef_scaled(rel_s, RELEASE_RATE_MULT);
```

### 2. How it was fit

- **What this stands in for:** SPEC.md S5.6 and S3.8.2 both state that the
  only documented minimum-release mechanism -- the rate clamp at `0x19834`
  dividing by literal `0x46` (70) -- is reachable ONLY from the fast-release/
  choke path (`0x19aa4`) and is NEVER reached from ordinary note-off
  (`0x19a2c`). So a floor on the ordinary path has no cited disassembly
  counterpart at all; this is `[F:fitted]`, not `[A]`, full stop.
- **Probe/reference, corpus side:** the harness (--skip-smoke mode),
  `compare_spectral_22050.overall_db` (more-negative = better), 32 graded
  probes, sweeping the floor with everything else held at the session-start
  baseline (mean -9.7206 dB, no floor):

  | floor | graded mean (dB) | GENERAL SERUM envelope r |
  |---|---|---|
  | none (baseline) | -9.7206 | 0.8673 |
  | 20 ms | -9.7388 | 0.8676 |
  | 40 ms | -9.7569 | 0.8681 |
  | **60 ms** | **-9.7693** | 0.8684 |
  | 90 ms | -9.6624 | 0.8692 |

  At 60 ms, every probe that moved improved and none regressed:
  `24_gain_staging` -11.654->-12.215, `25_pan_law` -17.019->-17.529,
  `26_other_gains` -13.649->-13.949, `27_gain_curves` -14.049->-14.109,
  `15_banks` -9.590->-9.619, `32_ramp_shape` -6.534->-6.613, `33_pitch_ramp`
  -7.423->-7.462.
- **Why 90 ms was rejected despite the higher envelope r:** the 90 ms mean is
  contaminated by an alignment flip on `31_tune_clamp_bend_sine` -- its
  recovered offset moved -0.9176 s -> -0.9941 s while its own waveform
  correlation ROSE 0.3755 -> 0.4301, the project's own established
  mis-lock signature (see the harness re-baseline note at the top of this
  file for the same class of failure). Several probes had also already
  turned around (improving) by 90 ms: `27_gain_curves` -14.109 -> -14.024,
  `19_prior_art` -11.082 -> -11.060, `29_all_sound_off_gap` -12.632 ->
  -12.614 -- 60 ms sits before that turnaround, at the last point every
  moved probe was still improving.
- **Independent corroboration, direct isolate measurement:** two hand-cut
  GENERAL_SERUM isolate captures (not retained in this tree, "ISOLATE-2" and
  the mixed "ISOLATE-2a" pair) give a reference time-to--20dB /
  -40dB-after-note-off of roughly 38 ms / 44 ms, against this project's
  own pre-fit render at 3.5 / 4.6 ms -- i.e. the isolates' authored 5-6 ms
  releases (bank 1 program 80 Square, bank 8 program 80 Sine, both read
  from `gm.dls`) need roughly a 6-11x stretch to match. 60 ms sits inside
  that band, which is agreement between two independent methods (the
  corpus-mean optimum and the direct isolate measurement), not a single
  measurement standing alone. Caveats on the isolate measurement itself: the
  ISOLATE-2 path needed a piecewise lag correction (a confirmed real capture
  drift, -349 ms at t=0 to -200 ms at t=240 s, not a synth defect), and the
  notes measured are low-register, so the fundamental period (8-53 ms) is
  comparable to the release being timed, setting a real resolution floor on
  how precisely 38/44 ms can be trusted.
- **SPEC_GAPS.md #15's known failure mode does not recur:**
  `field/corridor.mid` renders 1,159,168 frames untruncated at every
  floor tested (20/40/60/90 ms) -- this is the entry that previously recorded
  corridor going to 82.8 s and never finishing under a global release-rate
  multiplier; a duration floor on the ordinary-note-off segment does not
  reproduce that failure. `probes/04_envelope.mid` renders were byte-identical
  (`cmp`) at floors up to 40 ms -- its piano patch authors a 990 ms release,
  more than an order of magnitude above the floor, so the floor cannot touch
  it; 60 ms was not separately re-diffed against this probe but is smaller
  than 40 ms's own floor value relative to that release, so the same
  reasoning applies.

### 3. Where a future RE pass should look

SPEC.md Part 5 `+0x13c`, `0x194da`/`0x19644` (the real EG consumption code,
already `[O]` per SPEC_GAPS.md #8/#15), and `compute_release_target` / the
per-tick service routine `0x13054` specifically for whether ordinary
note-off's release target is quantised to a service tick -- see section 4
below for why that specific question matters.

### 4. A hypothesis this fit does NOT model, flagged for future work only

The reference isolate reaches -20 dB at ~38 ms but -40 dB at only ~44 ms --
a ~6 ms fall after a ~32 ms plateau. That shape is a DELAYED release onset
(the level holds near its sustain value for ~30-some ms, then falls fast),
not merely a slower release RATE. A pure duration floor on an exponential
release does not model a delayed onset; it can only approximate the
aggregate time-to-threshold. This is recorded as an open hypothesis, not
established -- it was not tested against the corpus this session, and the
low-register resolution caveat in section 2 means the 38 ms plateau length
itself is not pinned precisely enough to fit a two-segment model from this
data alone.

### Falsifiability

Re-run the harness (--skip-smoke mode) with `-DRELEASE_FLOOR_S=<value>` at
0.000/0.020/0.040/0.060/0.090 and confirm the graded mean reproduces the
table in section 2 (monotonic improvement 0->60ms, degradation by 90ms
driven by the `31_tune_clamp_bend_sine` alignment flip, independently
checkable via that probe's own reported `offset_seconds`/correlation).
Re-render `field/corridor.mid` at each floor and confirm frame count
stays 1,159,168 (untruncated) throughout.

---

## Entry 11: voice-stealing reserve top-up cadence and reserve size (`TOPUP_RESERVE_COUNT`, `TOPUP_PER_SUBCHUNK` in src/engine/voice.c and src/engine/render.c) -- CADENCE HALF SUPERSEDED BY ENTRY 14

> **SUPERSEDED 2026-07-26 (cadence only).** Both cadences A/B'd in this entry
> were tied to `render.c` call structure rather than wall-clock time, so both
> ran at MIDI *event density*; on a dense field MIDI that is ~800 top-up
> calls/second and it cascades Branch B into total silence. Entry 14 replaces
> them with a real tick clock (`TOPUP_INTERVAL_FRAMES`) and `TOPUP_PER_SUBCHUNK`
> is deleted. The reserve-size half of this entry (`TOPUP_RESERVE_COUNT` = 6)
> is unchanged and still current. Read the rest of this entry as history.

**Status: mechanism `[A]`, cadence and reserve size `[F:fitted, SHIPPED]`.**
This entry covers the fitted PARAMETERS of a SPEC.md S5.2-S5.5-documented
mechanism, not the mechanism itself -- see SPEC_GAPS.md #7 for the full
implementation description (the 48-primary/6-reserve pool split, Branch A/B
of `voice_topup_reserve()`, and the two steal comparators). What is fitted
here specifically: the reserve size (`TOPUP_RESERVE_COUNT`, default 6) and,
more importantly, the top-up CADENCE (`TOPUP_PER_SUBCHUNK`, default 0 = once
per `render_frames()` call), because SPEC.md's own cited cadence ("once per
call to the event dispatcher `0x12bd6`") has no exact counterpart in this
codebase's own event loop and is recorded here as `[O]`.

### 1. Value chosen

`src/engine/voice.c`:

```c
#ifndef TOPUP_RESERVE_COUNT
#define TOPUP_RESERVE_COUNT 6
#endif
#define NUM_RESERVE TOPUP_RESERVE_COUNT
#define NUM_PRIMARY (NUM_VOICES - NUM_RESERVE)
```

`src/engine/render.c`:

```c
#ifndef TOPUP_PER_SUBCHUNK
#define TOPUP_PER_SUBCHUNK 0
#endif
```

With `TOPUP_PER_SUBCHUNK==0` (shipped default), `voice_topup_reserve()` is
called once per `render_frames()` call -- the nearest analogue in this
codebase to "once per dispatcher batch," since `src/engine/smf.c` already drains all
due events for a tick and then calls `render_frames()` once for the gap to
the next event. `TOPUP_RESERVE_COUNT=6` reproduces SPEC.md S5.2-S5.5's own
48-primary/6-reserve split on the existing 54-voice pool (`NUM_VOICES`
unchanged); `-DTOPUP_RESERVE_COUNT=0` collapses the reserve tier back to
zero and reproduces the pre-feature flat-54-voice behaviour byte-identically
on probe 21 (confirmed, not merely architected to do so).

### 2. How it was fit

- **Reserve size, `TOPUP_RESERVE_COUNT=6`:** this is the literal count SPEC.md
  S5.2 gives (48+6=54=`NUM_VOICES`, already the existing pool size), not a
  free parameter swept against audio -- recorded as `[F:fitted]` only because
  no probe in this corpus independently re-derives "6" from scratch; it is
  carried over from the spec's own stated split.
- **Cadence -- the load-bearing fit, two cadences tried and measured:**
  SPEC.md S5.4 states `TopUpReserve` (`0x12b6a`) runs "exactly once per call
  to the event dispatcher `0x12bd6` ... before any queued event in that
  call's batch is processed," whose only caller is the per-tick service
  routine `0x13054`. This project has no recovered true tick, so the cadence
  is genuinely `[O]` and had to be fit against the corpus. Both attempts used
  the harness (--skip-smoke mode)'s `compare_spectral_22050.overall_db`
  (more-negative = better), 32 graded probes, held against the post-Entry-10
  baseline (mean -9.7693):

  **Attempt 1 -- hooked to `src/engine/render.c`'s existing 64-frame (~2.9 ms)
  `LFO_UPDATE_FRAMES` sub-chunk.** This fired top-up's Branch B (forced
  fast-release of active voices when both primary and reserve are empty)
  roughly an order of magnitude more often than the dispatcher-batch reading
  implies. It REGRESSED exactly the probes it targets: `21_steal_policy`
  -7.797 -> -6.292 (+1.505 dB worse), `10_polyphony` -4.576 -> -3.627
  (+0.950 worse), `20_voice_count` -7.120 -> -6.787 (+0.333 worse); corpus
  mean -9.7693 -> -9.6822. Corroborated by the defect metric OVERSHOOTING in
  the wrong direction: broadband hard-cut-click count fell to 6 against the
  reference's own 54 -- i.e. this cadence faded voices the reference leaves
  sounding, over-applying the fix.
- **Attempt 2 -- hooked once per `render_frames()` call (shipped).** Result:
  `21_steal_policy` -7.797 -> **-9.031** (-1.234 dB better), `20_voice_count`
  -7.120 -> -7.946 (-0.826 better), `10_polyphony` -4.576 -> -4.716 (-0.139
  better); corpus mean -9.7693 -> **-9.8380**, and no probe regressed. Click
  count 44 against the reference's 54 (much closer than attempt 1's 6);
  settle-time ratio ours/reference 1.85 -> 1.16.
- **`TOPUP_PER_SUBCHUNK=1` is kept as a compile-time A/B switch**, retaining
  the rejected cadence rather than deleting it, per this file's own
  negative-result-retention rule; `-DTOPUP_RESERVE_COUNT=0` (reserve
  disabled entirely) reproduces the pre-feature behaviour byte-identically
  on probe 21.
- **Against SPEC.md S5.5's own `[M]` figure** (80 simultaneous note-ons
  should leave 48 sounding and cut 32): this build measures 47 surviving /
  33 cut -- off by one, unchanged by the cadence fix between attempts 1 and
  2. Recorded as a remaining small discrepancy, not papered over.
- **Deadlock safety, checked directly:** with every voice active AND every
  voice already marked for fast release (`fast_release_committed`), note-on
  still obtains a voice, because `find_steal_candidate_asymmetric` (used by
  note-on's forced-steal fallback) carries no `fast_release_committed` gate,
  unlike `find_steal_candidate_symmetric` (used only by top-up's own Branch
  B). This was verified with a targeted test that forced that exact all-
  voices-committed state, not merely reasoned about from the code.

### 3. Where a future RE pass should look

The exact tick/dispatcher-batch boundary (`0x13054`/`0x12bd6`) is `[O]` for
this project -- no true per-tick service routine was recovered. A future
pass that locates the real service-tick cadence should replace
`TOPUP_PER_SUBCHUNK`'s `render_frames()`-call approximation with the true
per-tick hook, and should also look for what accounts for the remaining
47-vs-48 / 33-vs-32 one-voice discrepancy against SPEC.md S5.5's own
measured figure.

### Falsifiability

Rebuild with `-DTOPUP_PER_SUBCHUNK=1` and re-run the harness
(`--skip-smoke` mode); `21_steal_policy`/`10_polyphony`/`20_voice_count` should
reproduce the attempt-1 regression above (worse than both the no-topup
baseline and the shipped default). Rebuild with `-DTOPUP_RESERVE_COUNT=0`
and confirm `probes/21_steal_policy`'s rendered PCM is byte-identical to a
pre-feature build (`cmp`).

---

## Entry 12: geometric pitch-bend ramp (`PITCH_RAMP_K_PERIODS`) -- FIT ATTEMPTED, NOT SHIPPED

**Status: `[F:fitted, NOT SHIPPED]`.** Addresses SPEC_GAPS.md #19. Not present
in `src/engine/voice.c` or `src/engine/render.c` -- the existing fixed-duration,
rate-with-ceiling ramp (`PITCH_RAMP_RATE_FRAC_PER_MS` / `PITCH_RAMP_MAX_MS`,
Entry 9) is unchanged, both constants still at their Entry-9 values.

### 1. Value chosen (would-be)

A single free parameter `PITCH_RAMP_K_PERIODS` (`K`), replacing src/engine/render.c's
per-sample clamp-on-reach accumulator's fixed step with one re-derived every
64-sample refresh as `(target - current) / (K * 64)`, so the ramp approaches
its target geometrically and (in principle) never fully settles between
messages. This design would retire the fixed-rate
`PITCH_RAMP_RATE_FRAC_PER_MS` + `PITCH_RAMP_MAX_MS` pair entirely. K values
of 2.0, 3.0 and 4.0 were tried; none shipped, so no default is recorded.

### 2. How it was fit

- **Targeted evidence it was fit against, and where it worked:** the
  hand-mixed "ISOLATE-2a" capture (not retained in this tree; ours = LEFT
  channel, reference = RIGHT, pre-aligned). Fraction of frames changing by <1 cent (a "how static is the
  pitch trajectory" metric): 34.0% (existing fixed-duration ramp) -> 13.8%
  (K not separately reported per-value here, best-K result) against the
  reference's 11.1%; longest static run 37.7 ms -> 18.9 ms against the
  reference's 9.4 ms. On this specific isolated metric, the geometric design
  clearly moved toward the reference.
- **Error metric, corpus side:** the harness (--skip-smoke mode),
  `compare_spectral_22050.overall_db` (more-negative = better), 32 (or the
  then-current) graded probes, and GENERAL SERUM envelope correlation r.
- **Result: it REGRESSED the corpus at every K tested.** Means: baseline
  (existing fixed-duration ramp) -9.7206; K=2.0 -9.7156 (+0.0050 worse);
  K=3.0 -9.6604 (+0.0602 worse); K=4.0 -9.6677 (+0.0529 worse). GENERAL
  SERUM envelope r fell from 0.8673 to 0.8620 (K=2) / 0.8634 (K=3) / 0.8624
  (K=4).
- **The dominant, consistent regression was `05_pitchbend`**, the dedicated
  bend probe: -8.228 baseline -> -7.024 (K=2) / -7.131 (K=3) / -7.158 (K=4)
  -- +1.10 to +1.20 dB WORSE at every K, with no alignment flags raised and
  its recovered offset barely moving (-0.1233 s -> -0.1014 s), i.e. this is
  a real content-match regression, not an alignment artifact.
- **Probe 33's own isolated -24 semitone step, 10-90% glide time:** reference
  16.09 ms; ours 16.55 ms at baseline (already close); K=2 6.23 ms
  (too fast), K=3 12.00 ms (still short), K=4 17.78 ms (closest, still not
  an improvement on the corpus mean).

### 3. Root cause -- the single most useful finding of this attempt

A dedicated diagnostic established that probe 05's own reference does NOT
glide continuously. It glides then HOLDS FLAT -- 80.2% static frames in the
reference against the existing (unmodified) baseline's 80.3%, i.e. the
already-shipped fixed-duration ~20.6 ms ramp already matches probe 05
closely. The two references (probe 05 and the ISOLATE-2a mixed pair this
attempt was fit against) genuinely disagree with each other, and the reason
is traced to CONTENT, not the synth: the source MIDI behind the ISOLATE-2a
capture (not retained in this tree) contains 61 pitch-bend events at a median 10-tick (41.7 ms)
spacing with very LARGE steps (bend values 8192 -> 16383 -> 12099 -> 8762 ->
6206 -> 4284 -> 2871 -> 1858, a decaying pitch gesture), whereas probe 05's
own steps are small. Large steps arriving every 41.7 ms leave little time to
settle before the next one starts, which reads as "continuously moving";
small steps settle and hold, which reads as "static." One fixed-duration
ramp is consistent with both pieces of reference audio; the geometric
replacement, tuned to make the large-step file look more continuous,
necessarily over-corrects the small-step file into constant motion it
should not have.

**Control ruling out the obvious objection:** a bend-free sustained
reference segment measures 100% static frames on the same metric used
above, so the "fraction of frames changing by <1 cent" metric is not merely
reading the reference capture's own noise floor -- it is a real signal.

**Also ruled out, by reading the source rather than theorising:** SPEC.md
S4.2.1/S4.7.3's timestamp-keyed controller queues with a look-ahead read
cannot explain the residual lag/staircase, because `src/engine/smf.c`'s
`smf_render` already bounds every render chunk at the next due event's exact
sample, so controller writes already take effect at their correct sample
with no buffer-quantisation gap for a look-ahead read to compensate for.

### 4. A methodological trap worth recording for future passes

A normalized-autocorrelation pitch tracker reliably OCTAVE-LOCKS on probe
33's ~2093 Hz content, reporting a confident sustained plateau at ~1045 Hz
where a raw FFT shows the true peak at 2091.4 Hz with harmonics at
4177/6268 Hz and zero energy at 1045 Hz. Band-restricted FFT-peak tracking
(searching only near the expected fundamental) avoids this; a general-
purpose autocorrelation tracker should not be trusted on this probe's
content without that restriction.

### 5. Where a future RE pass should look

Same as Entry 9: SPEC.md S6.6/S6.4.1's `[A]` mechanism (linear ramp,
`ramp_period`-refreshed) is confirmed; what remains `[O]` is the caller's
own slope-selection rule. This attempt's own finding narrows the search:
whatever the real rule is, it must be consistent with BOTH a probe-05-style
small-step glide that settles well inside ~20 ms AND an ISOLATE-2a-style
large-step-every-42ms gesture that reads as continuous on the real
hardware -- a single global geometric constant, tuned against one of the two,
was shown here to regress the other. A future pass should look for a rule
keyed to bend MAGNITUDE (matching Entry 9's own rate-with-ceiling design)
rather than a fixed geometric ratio, or should obtain a probe that isolates
large, frequent bend steps specifically (this corpus's closest analogue,
the ISOLATE-2a file, is real song content, not a controlled probe).

### Falsifiability

Rebuild `src/engine/render.c`'s pitch ramp with a `(target-current)/(K*64)` geometric
step at K=2/3/4 and re-run the harness (--skip-smoke mode); `05_pitchbend`
should reproduce a +1.0 to +1.2 dB regression at every K, and the 32-probe
mean should reproduce a regression of roughly 0.005-0.06 dB depending on K,
against the shipped fixed-duration-ramp baseline.

---

## Entry 13: linear gain ramp replacing the one-pole (`GAIN_RAMP_MAX_MS`) -- FIT ATTEMPTED, NOT SHIPPED

**Status: `[F:fitted, NOT SHIPPED]`.** Concerns Entry 4's `GAIN_SMOOTH_ALPHA`
and `probes/28_expression_gate.mid`. Not present in `src/engine/render.c` --
`GAIN_SMOOTH_ALPHA`'s one-pole exponential is unchanged from Entry 4.

### 1. Value chosen (would-be)

`GAIN_RAMP_MAX_MS = 4.0`: replace `src/engine/render.c`'s per-sample one-pole
(`GAIN_SMOOTH_ALPHA = 0.003780968318281238`, a 12 ms time constant, Entry 4)
with a fixed-duration linear ramp with clamp-on-reach -- the mechanism
SPEC.md S6.6/S6.4.1 actually documents `[A]` (a linear step re-derived from
the caller every `ramp_period` samples, held constant between refreshes; see
Entry 9's identical mechanism already shipped for the phase-step/pitch side).
Only the mechanism's EXISTENCE is `[A]`; what the caller computes each
refresh (the ramp's total duration) is `[O]`, so the 4.0 ms figure is
`[F:fitted]`.

### 2. How it was fit

- **Probe/reference:** `probes/28_expression_gate.mid` /
  `probe-results/28.flac`, gate depth (90th vs 10th percentile of a 2 ms RMS
  envelope) at six CC11 square-wave half-periods (200/100/50/25/12/6 ms).
- **It fixed the target defect dramatically.** Gate depth by half-period:

  | half-period | 200 | 100 | 50 | 25 | 12 | 6 ms |
  |---|---|---|---|---|---|---|
  | REFERENCE | 68.7 | 68.6 | 68.6 | 67.9 | 68.8 | 67.3 dB |
  | ours, one-pole (before) | 68.5 | 63.0 | 32.5 | 18.4 | 13.8 | 12.0 dB |
  | ours, 4.0 ms linear ramp (after) | 68.8 | 68.7 | 68.6 | 68.5 | 68.3 | 67.3 dB |

- **It was nonetheless rejected TWICE, on two different baselines, with
  near-identical verdicts** -- the reproducibility across baselines is the
  strongest evidence here, not either single measurement alone:
  - On the original baseline (before Entries 10/11 this session):
    `28_expression_gate` improved only 0.044 dB while `32_ramp_shape`
    regressed +0.078 dB and `33_pitch_ramp` regressed +0.030 dB; corpus mean
    moved +0.0020 dB (worse); GENERAL SERUM envelope r 0.8673 -> 0.8651.
  - Re-implemented fresh on the post-item-2/3 (i.e. post-Entry-10/11)
    baseline: `28_expression_gate` -7.183 -> -7.232 (0.049 dB better, similar
    magnitude to before), `32_ramp_shape` +0.081 dB worse, `33_pitch_ramp`
    +0.030 dB worse, corpus mean -9.8380 -> -9.8361 (+0.0020 dB worse,
    reproducing the exact sign and near-exact magnitude of the first run),
    envelope r 0.8656 -> 0.8634.
- **Side effect measured and recorded, not fixed:** exact-zero output runs
  (both channels exactly 0) in a render of
  `field/Kot_and_A64-GENERAL_SERUM.mid` rose from 12,097 to 16,349 total
  zero samples, with new mid-length runs of 188/325/463/601/739/2179 samples
  appearing that were not present before. A one-pole never fully reaches its
  target, so it never produces an exact zero sample; a finite linear ramp
  does, for any CC11=0 dwell longer than the ramp's own duration -- this is
  a structural consequence of the mechanism, not a tuning error in the 4.0 ms
  figure.
- **Correction to the record this measurement produced, the most interesting
  finding of this item:** the briefing that originally motivated this change
  stated the reference "holds 73-80 dB of gate depth down to a 12 ms
  half-period, dropping to ~28 dB at 6 ms." **That did NOT reproduce.**
  Measured with a jitter-immune percentile method (see below), the reference
  is FLAT at 67-69 dB at ALL six rates including 6 ms, with no knee at 12 ms
  and no cliff at 6 ms. The reference's low-phase 10th-percentile level sits
  at ~0.45-0.7 int16 counts -- essentially the capture's own dither floor --
  so this metric is SATURATED for the reference: it can only establish that
  the reference's gain reaches the floor within a 6 ms half-period, not what
  its actual ramp duration is. Probe 28 therefore cannot pin this constant
  on its own, and a future attempt needs a genuinely different measurement,
  not a different value for the same measurement.
- **Also confirmed while measuring:** the reference capture's own CC11
  application instants jitter by several ms (up to ~15 ms) within a single
  held note relative to the nominal MIDI schedule, and its note-on lands
  ~250-270 ms before nominal, drifting slowly across the file. Phase-locked
  measurement methods are fooled by this jitter and can manufacture an
  apparent reference "collapse" that is really a phase-lock failure; two
  such phase-locked methods were tried and rejected in favour of the
  percentile method before the flat 67-69 dB reference result above was
  trusted.

### 3. Where a future RE pass should look

Same as Entry 9/Entry 4: SPEC.md S6.6/S6.4.1's linear-ramp mechanism is
`[A]`; the caller-side duration/slope rule is `[O]`. A future pass needs a
measurement of the reference's own gate-edge shape that is not saturated at
the capture's dither floor -- e.g. a larger dynamic-range gate depth (a
partial CC11 step rather than 127->0) or a metric that reads the transition
edge directly (rise/fall time between two non-floor levels) rather than a
90th/10th-percentile depth number that only proves "reaches the floor."

### Falsifiability

Rebuild `src/engine/render.c`'s gain smoothing as a 4.0 ms linear ramp with
clamp-on-reach in place of the one-pole, and re-run the harness
(`--skip-smoke` mode): `32_ramp_shape` and `33_pitch_ramp` should each reproduce a
~0.03-0.08 dB regression, and the 32-probe mean should reproduce a ~0.002 dB
regression, against the shipped one-pole baseline, on both the pre- and
post-Entry-10/11 baselines. Re-render `field/Kot_and_A64-GENERAL_SERUM.mid`
and confirm total exact-zero-sample count rises from 12,097 (one-pole) to
16,349 (linear ramp).

---

## Entry 14: reserve top-up tick period (`TOPUP_INTERVAL_FRAMES` in src/engine/voice.c) -- SHIPPED

**Status: mechanism `[A]` (SPEC.md S5.4), period `[F:fitted, SHIPPED]`.**
Supersedes the cadence half of Entry 11. The reserve size
(`TOPUP_RESERVE_COUNT` = 6) is untouched and still Entry 11's.

### 0. Why this was refit at all

A listener reported voices cutting out in `field/HueArme-Weekend.mid` around
23 s. Instrumenting `voice_topup_reserve()` on that file showed Branch B
firing **33 times in 100 ms** with the active-voice count reaching **zero** --
two total-silence dropouts (t=23.09-23.15 s, t=23.44-23.51 s) that the
reference does not have, each a clean ~60 ms exponential decay to digital
zero, i.e. the whole 54-voice pool committed to the 70 ms fast release at
once.

Root cause was the cadence, not the mechanism. SPEC.md S5.4 says the real
`TopUpReserve` (`0x12b6a`) runs once per event-dispatcher call (`0x12bd6`),
whose sole caller is the per-**buffer** service routine (`0x13054`) -- i.e.
once per audio service tick, a **wall-clock period**. Entry 11 tried two
cadences and shipped "once per `render_frames()` call"; both it and the
rejected per-sub-chunk alternative are tied to `render.c` call structure, and
`smf.c` splits a `render_frames()` call at *every* dispatched MIDI event. So
the shipped cadence ran at MIDI event density: ~800 calls/second on
HueArme-Weekend at t=23 s, versus ~10/second on the probes (whose events are
100 ms apart). That is exactly why the 32-probe corpus Entry 11 fit against
could not see the defect.

Branch B is a feedback loop: a marked voice keeps rendering for its full
~70 ms fast release before it can be reaped and recycled, so any cadence
faster than that drain time marks another `TOPUP_RESERVE_COUNT` voices before
the previous batch has freed anything, and the pool is fully committed within
a handful of calls.

### 1. Value chosen

`src/engine/voice.c`:

```c
#ifndef TOPUP_INTERVAL_FRAMES
#define TOPUP_INTERVAL_FRAMES 2048 /* ~92.9ms @ 22050Hz */
#endif
```

`voice_topup_tick(frames)` accumulates rendered frames and runs one top-up per
period; `render.c` feeds it each sub-chunk length, so the period is
independent of how `smf.c` happens to slice its chunks.
`TOPUP_PER_SUBCHUNK` is deleted -- both of its settings were the same
mistake, so there is nothing left to A/B against.

### 2. Lower bound, from SPEC.md's own measurement

SPEC.md S5.5 `[M]`: 80 note-ons with no note-off leave 48 sounding, 32 cut =
26 forced by pigeonhole + **6**. Branch B therefore contributes exactly ONE
batch of `TOPUP_RESERVE_COUNT` over an entire saturated 8-second run, which
holds only if a top-up cannot fire again until the batch it marked has
drained and been recycled. That puts the real period at **>= the ~70 ms
fast-release time** (SPEC.md S5.6's measured 70.0 ms). 2048 frames
(~92.9 ms) sits just above that floor.

The two metrics constrain it differently, and the structural one is the
sharper of the two:

| period | frames | `20`/`21` survivors (want 48) | HueArme-Weekend residual |
|---|---|---|---|
| 2.9 ms | 64 | **1** (the old sub-chunk cadence) | -- |
| 23.2 ms | 512 | **43** | -19.15 |
| 46.4 ms | 1024 | 48 | -19.15 |
| 92.9 ms | 2048 | 48 (shipped) | -19.16 |
| 185.8 ms | 4096 | 48 | -19.18 |

The audio residual is flat above 512 frames -- all four agree to within
0.03 dB and 0.002 r, so it cannot pick a value. The survivor count can: it
puts the floor between 512 and 1024 frames (23-46 ms), consistent with the
>= 70 ms argument above being a slight over-estimate of the drain-plus-reap
time. 2048 sits comfortably above the measured floor without being so long
that the top-up stops tracking a real service tick. The period is therefore
**bounded below but only loosely constrained above** -- what matters is that
it is a wall-clock period longer than the fast-release drain, not its exact
value. Recorded as fitted, not recovered.

### 3. Metric and measurements

Metric: `artifacts/score.py` (added with this entry) -- a numpy CLI port of
`dist/compare.js`'s two headline numbers, same constants (50 ms envelope hop,
+/-5 s lag search, 2048/1024 FFT, mean-RMS normalization before the
residual), covering `field/`, `tests/` and `artifacts/probes/` in one run
(~25 s for all 47 items). Verified against the browser page on
HueArme-Weekend: script r=0.602 / residual -17.38 dB vs. the page's 0.610 /
-17.3 dB. Reference FLACs are resampled to 22050 Hz by ffmpeg rather than by
`OfflineAudioContext`, so figures track compare.js closely but are not
bit-identical to it. Not comparable to Entry 11's
`compare_spectral_22050.overall_db` probe numbers.

Full 13-item field+tests corpus, before -> after:

| item | r before | r after | residual before | residual after |
|---|---|---|---|---|
| CrystalOscillator | 0.779 | **0.862** | -23.68 | **-25.01** |
| HueArme-Weekend | 0.602 | **0.772** | -17.38 | **-19.16** |
| Kot_and_A64-GENERAL_SERUM | 0.866 | 0.868 | -19.37 | -19.38 |
| Strobe-faffaeefafaefae | 0.560 | 0.560 | -18.61 | -18.61 |
| corridor | 0.854 | 0.854 | -20.74 | -20.74 |
| eek_the | 0.857 | 0.857 | -22.05 | -22.05 |
| flourish | 0.582 | 0.582 | -18.98 | -18.98 |
| onestop | 0.834 | 0.834 | -26.06 | -26.06 |
| town | 0.788 | 0.788 | -21.84 | -21.84 |
| lazers | 0.988 | 0.988 | -17.57 | -17.57 |
| radio | 0.909 | 0.909 | -14.37 | -14.37 |
| warm-echo | 0.835 | 0.835 | -25.29 | -25.29 |
| wild-sweep | 0.691 | 0.691 | -18.81 | -18.81 |
| **MEAN** | 0.780 | **0.800** | -20.37 | **-20.61** |

Nothing regressed. The 10 unchanged items are bit-identical -- they never
saturate the pool, so Branch B never fires in them at all.

Invented digital silence (render at its own -60 dB where the reference is
not): HueArme-Weekend 880 ms -> 210 ms, CrystalOscillator 1580 ms -> 150 ms.
The remainder in both is lead-in and tail silence, not mid-song: a run-length
scan of the fixed HueArme-Weekend render finds exactly two silent runs, 960 ms
at t=0 and 20 ms at the very end.

Structural check, `probes/20_voice_count.mid` and `probes/21_steal_policy.mid`
(80 note-ons, no note-off): both now settle at exactly **48 surviving / 32
cut**, matching SPEC.md S5.5's `[M]` figure. This closes the "47 surviving /
33 cut" residual SPEC_GAPS.md #7 had recorded as open -- the off-by-one was
the too-fast cadence marking one extra voice, not a pool-size error.

### 4. Where a future RE pass should look

SPEC.md S5.4/S5.5's mechanism is `[A]`; the *period* is the `0x13054` service
routine's own timer, which SPEC.md marks `[O]` (S6.7: "block-cadenced, exact
cadence caller-supplied and unrecovered"). Recovering `0x13054`'s caller --
the WDM port-class buffer/notification period MSGS registers -- would retire
this fit. That period is also the natural home of Entry 9's and Entry 4's
unrecovered `ramp_period`, so one recovery closes three fits.

### Falsifiability

Rebuild with `-DTOPUP_INTERVAL_FRAMES=64` (the old sub-chunk cadence) and
re-render `field/HueArme-Weekend.mid`: the two total-silence dropouts at
t=23.09-23.15 s and t=23.44-23.51 s must reappear, `score.py`'s r must fall
back to ~0.60, and `probes/20_voice_count.mid` must stop settling at 48.

---

## Entry 15: EG1/EG2 decay-time key-follow (`decay_tc_keyfollow` in src/engine/voice.c) -- SHIPPED, `[M: field/town.mid]` + `[I]` normalization

**Status: mostly `[A]` -- not a fit.** The connection itself is SPEC.md
S2.4.3's own confirmed `usSource=3` (KEYNUMBER) table, `[A]`-parsed since
the first `dls.c`. What was missing was consumption. Only the *normalization*
(`key/128`) is `[I]`, and it is the one thing `probes/35_decay_keyfollow.mid`
exists to settle. Recorded here because it supersedes Entry 1 and because a
`[I]` constant sitting on the signal path belongs in this ledger even when it
came from a published spec rather than a curve fit.

### 1. What shipped

`dls.c` stores both decay rows (`0x0207` EG1, `0x030b` EG2) into
`Artic.eg1_decay_kf_tc`/`eg2_decay_kf_tc`; `voice.c` adds them at note-on:

```c
static int32_t decay_tc_keyfollow(int32_t tc, int16_t kf, int note) {
    if (tc == (int32_t)0x80000000 || kf == 0) return tc;
    return tc + (int32_t)kf * (int32_t)note * 512; /* 65536/128 == 512 */
}
```

### 2. How it was found

By ear/spectrogram on `field/town.mid`, 25.0-26.7 s: a Steel-str.Gt chord
(keys 59/63/68) under a CC1 sweep rings for the whole bar while the reference
fades, leaving its (correct) 6.0 Hz / +-47 c vibrato bright across the whole
passage. The vibrato was the symptom; the note not dying was the defect.

**169 of `gm.dls`'s 235 instruments carry the `0x0207` row** (scales
-4800..+2400, median -3979; 29 also carry `0x030b`), and **92** of those pair
it with a real decay and sustain < 10%, i.e. a measurable decay-to-silence.
This was not a town.mid quirk -- every acoustic patch in the corpus decayed
3-5x too slowly. The 66 without the row are mostly synth leads, pads and SFX;
`008:080` Sine Wave and `001:080` Square are among them, which is why probe 35
cannot use the clean-tone carriers probes 24/25/27 rely on.

### 3. Measurement

Hilbert band-envelope dB/s fit on four partials of that chord, 25.30-26.65 s,
against `field/town.flac` (own fresh measurement, harness not retained):

| partial | reference | before | after | predicted from gm.dls |
|---|---|---|---|---|
| key68 h4 (1661 Hz) | -13.4 | -4.6 | -13.3 | -15.6 |
| key68 h5 (2076 Hz) | -12.3 | -4.2 | -12.4 | -15.6 |
| key63 h5 (1556 Hz) | -10.7 | -3.7 | -10.8 | -14.2 |
| key59 h6 (1482 Hz) |  -9.4 | -2.3 |  -8.6 | -13.1 |

(The reference column is measured on the full mix, so other channels' energy
floors the tail and biases every figure low -- which is why it sits under the
prediction while tracking it. `field/town.mid` render duration 80.25 s ->
79.13 s against a 79.22 s reference.)

**Independent cross-check, different instrument, someone else's measurement:**
SPEC_GAPS.md #15 measured Piano 1 note 60's reference decay at -7.14 dB/s and
recorded this project shipping -2.50 dB/s, unresolved. Piano 1's own
key-follow predicts **-7.34 dB/s** at note 60. No parameter was tuned to make
that land.

### 4. Normalization -- swept first, then MEASURED (`[M: probe 35]`)

Before a reference existed: `key/128` vs. `key/127`, full corpus mean
spectral residual **-28.18 dB** vs. **-28.05 dB**; EG2 row included vs. EG1
only, -28.18 vs. -28.16. Shipped `/128` with EG2 on a 0.13 dB preference,
flagged at the time as a tiebreak and not a measurement.

`probe-results/35.flac` has since been captured. Fitting each note's decay in
dB/s and regressing `log2(rate)` on key measures the divisor directly:

| section | d(log2 rate)/dkey | implied divisor | `/128` | `/127` |
|---|---|---|---|---|
| Piano 1 (keys 24-96) | 0.02604 | 127.3 | 0.02590 | 0.02611 |
| Steel-str.Gt (24-96) | 0.02897 | 126.3 | 0.02857 | 0.02880 |
| Vibraphone (48-96) | 0.03179 | 125.8 | 0.03125 | 0.03150 |

**Settled: the source is the absolute key, not 60-relative.** Both readings
give the same slope, so the slope alone cannot separate them -- but they
differ by a constant ~2.9x in absolute rate, and the reference matches the
absolute-key form to 3.5% while 60-relative is out by that whole factor
(2.50 vs. 7.34 dB/s at Piano 1 note 60). This is the reading SPEC_GAPS.md #15
originally used to rule key-follow out, now measured wrong.

**NOT settled: `/127` vs. `/128`.** Measured divisor 126.5 +- 0.8; the two
candidates differ by 0.8% in slope and 1.4% in offset at key 96, well inside
that. `/128` stays on the `>>7` argument. This is the residual `[I]` and the
corpus tiebreak above should NOT be cited as having resolved it.

### 4b. What the probe DID resolve: the decay shape constant (`[M: probe 35]`)

With per-key duration correct, the entire remaining discrepancy is a single
uniform scale factor. Reference / this project, per note:

| section | keys 24 -> 96, ratio |
|---|---|
| Piano 1 | 0.963 0.964 0.961 0.962 0.960 0.957 0.963 |
| Steel-str.Gt | 0.962 0.961 0.965 0.971 0.961 0.969 0.967 |
| Vibraphone | (48) 0.967  (72) 0.972  (96) 0.978 |

**Mean 0.965, sd 0.005, n=17**, across three instruments and a 7x range of
decay rates (3.8-27 dB/s), with no trend against key or instrument -- which is
what distinguishes a *shape* error from a *normalization* error, since the
latter would trend with key. 29 standard errors from 1.0.

Shipped as `DECAY_RATE_MULT = 0.965`: the decay segment is "96.5 dB over
`seconds`", not the 100 dB the shared `exp_coef` assumes (SPEC_GAPS.md #15's
`[O]`). Release untouched -- probe 35 measures decay only, and Part 5 S5.6's
70 ms fast-release still matches 100 dB.

**Deliberately not corpus-swept.** The corpus cannot resolve this constant:
0.9633 -> -28.83, 0.965 -> -28.85, 0.97 -> -28.88, 0.96 -> -28.80. The
measured value ships over the marginally better swept one. Probe 35's own
residual -36.82 -> **-43.58 dB**; corpus mean -28.36 -> -28.85 dB.

96.5 dB is close to 96.33 = 20*log10(2^16), a 16-bit floor, which would be a
tidier constant -- but it sits 1.4 sd from the measurement and is a
hypothesis, not a finding. Do not round to it without a measurement that
separates the two.

### 5. Corpus effect, including the regressions

Mean spectral residual **-24.55 -> -28.18 dB**, mean envelope `r`
**0.903 -> 0.909** (`artifacts/score.py`, 47 items).

Improved most: `05_pitchbend` -7.80 -> -28.44, `14_running_status` -15.87 ->
-39.25, `23_rpn_tune` -19.90 -> -41.47, `12_gs_sysex` -22.68 -> -42.92,
`07_pan_volume` -18.02 -> -43.21, `13_edge` -19.35 -> -37.17,
`17_master_volume` -22.62 -> -40.68, `04_envelope` -29.73 -> -37.47.

Regressed: `corridor` -20.74 -> -17.65, `flourish` -18.98 -> -17.25,
`03_velocity` -38.32 -> -30.68, `16_drum_parts` -24.15 -> -21.62, and
`town`'s own *global* residual -21.84 -> -20.96 (its 1500-2500 Hz band --
where the defect was found -- improves, while 200-500 Hz worsens). Shipped
anyway: the mean moves 3.6 dB, the targeted defect is measured fixed, and two
previously-open items close. The regressed set is the natural next place to
look once probe 35 has a reference, since a normalization error would show up
exactly as a low-key/high-key imbalance.

### 6. The check that fails if this breaks

Rebuild with `decay_tc_keyfollow` returning `tc` unchanged and re-render
`field/town.mid`: the 1661 Hz partial's decay over 25.30-26.65 s must fall
from ~-13 dB/s back to ~-4.6 dB/s, `score.py`'s corpus mean must fall from
-28.18 dB to -24.55 dB, and the render must lengthen from 79.13 s to 80.25 s
against a 79.22 s reference.

---

## Entry 16: release finish detection on the AUDIBLE level (`AUDIBLE_FLOOR` in src/engine/voice.c) -- SHIPPED, `[I]` reading of `+0x13c`

**Status: `[I]`, not a curve fit.** SPEC.md S5.7 reads `+0x13c` -- the field
the finish check `[A:0x19733]` compares against its -80 dB constant, and the
field both steal comparators tie-break on -- as "the voice's *live* envelope
level" in a log-domain format, explicitly `[I]`. This entry ships the reading
where the note's own attenuation sum (S3.5/S3.10: velocity + region
attenuation + CC7 + CC11 + Master Volume + pan) is *inside* that dB
accumulator, which is the reading that makes S5.7's "lower `+0x13c` wins"
tie-break mean "the quietest voice dies first" rather than "the voice whose
normalized EG happens to sit lowest". The value itself is not tuned: the
whole plateau 3e-5 .. 1e-3 gives the identical render on the passage below.

### 1. What shipped

`voice_step_envelope`'s ENV_RELEASE case reaps on *either* floor:

```c
if (v->env_level < 0.0005 ||                                   /* unchanged */
    v->env_level * max(v->gain_l, v->gain_r) < AUDIBLE_FLOOR)  /* new */
```

`AUDIBLE_FLOOR` = 1e-4, i.e. -74 dB below a full-scale voice given
`GAIN_CEILING` ~0.5. The old bare-EG floor is kept as-is, so no voice is ever
reaped *later* than before -- this can only shorten a slot's occupancy.

### 2. How it was found

`field/HueArme-Weekend.mid` at t=23.9 s (reported by ear/spectrogram: "the
noise part is missing", magenta box, both log and linear views). Measured:
400-1000 Hz sat **7-9 dB** below the reference for ~120 ms, while the
neighbouring windows and the song-wide figure both sit within ~1 dB.

Per-channel solo renders localized it to channels 1 and 2 (both program 122,
Seashore -- the passage's noise wash): each contributes 31-32 dB soloed, yet
*dropping both from the full mix changed the mix by 0.1 dB*. They were being
allocated and then evicted. An instrumented build confirmed it at the frame:

```
23.923 STEAL for ch13 n73 <- victim ch0 n48 held1 age4398
23.923 STEAL for ch13 n77 <- victim ch1 n48 held1 age4399
```

-- note-on's own forced steal (S5.7's asymmetric `0x124a8` comparator,
oldest-held-first), firing because both free lists were empty. The pool dump
at that instant: 54/54 active, only 17 held, **37 in release, 20 of those
already below -40 dB audible**. The pool was full of voices that could not be
heard. Peak demand in that passage is ~71 voices against a 54-voice pool
(S5.2), so ~20 wasted slots is exactly the difference between stealing and
not stealing.

Ruled out first: release *duration* is not the problem. Probe 01's program
122 note (t=305.1 s, locally aligned, r=0.995) decays note-off -> -66 dB in
1.6 s in **both** engines, tracking the reference within a constant ~3 dB
level offset the whole way down. The tails are authentic; what was wrong was
counting an inaudible tail as an occupied slot.

### 3. Measurement

400-1000 Hz, ref window 23.28-23.47 s (`field/HueArme-Weekend.flac`, slopgs
+0.6156 s):

| build | 400-1000 Hz vs reference |
|---|---|
| before | 27.7 dB (ref 38.3) |
| after | 35.2 dB |
| 256-voice pool, no other change (diagnostic only) | 35.2 dB |

Matching the unlimited-pool render exactly is the point: after this change
the passage no longer steals at all, which is what the reference shows.

Per-band delta vs reference over that window, before -> after:
400-600 -9.2 -> -5.0, 600-800 -9.3 -> -3.6, 800-1000 -7.2 -> -0.4 dB.

### 4. Corpus effect

`artifacts/score.py`, mean spectral residual **-28.18 -> -28.85 dB**, mean
envelope `r` 0.909 unchanged. `HueArme-Weekend` -19.27 -> -19.45. Both
figures are 47-item runs, taken before `probe-results/36.flac` existed; on
the 48-item corpus the same pair reads **-27.94 -> -28.64 dB**, `r` 0.904 ->
0.909. (The 47-item "after" also carries the S3.1.3 key-range fix in
`dls.c`, which is a separate `[A]` conformance fix, not a fit -- it moved
exactly one corpus item, `HueArme-Weekend` -19.27 -> -19.26, before probe 36
joined.)

Improved most: `12_gs_sysex` -42.92 -> -46.32, `14_running_status` -39.25 ->
-43.12, `23_rpn_tune` -41.47 -> -45.23, `07_pan_volume` -43.21 -> -45.12,
`17_master_volume` -40.68 -> -42.67, `04_envelope` -37.47 -> -37.99.

Regressed: `01_programs` -29.47 -> -29.10, `05_pitchbend` -28.44 -> -27.78,
`19_prior_art` -25.44 -> -25.36, `wild-sweep` r 0.690 -> 0.680. `dead` ms
fell or held everywhere it moved (`11_drums` 13160 -> 12635, `flourish` 385
-> 310).

### 5. Where a future RE pass should look

`0x194da` (the only non-zeroing writer to `+0x13c`) and `0x19733` (the floor
compare): recovering what `0x194da` actually accumulates -- normalized EG, or
EG plus the note's attenuation sum -- retires this `[I]` outright. SPEC.md
S6.4.5 "Open items" #16 (the `<<8>>5` attenuation-to-register scaling) is the
same unrecovered path.

### 6. The check that fails if this breaks

`-DAUDIBLE_FLOOR=0.0` restores the old behaviour exactly. Rebuild that way
and re-render `field/HueArme-Weekend.mid`: 400-1000 Hz over 23.916-24.056 s
(render timeline) must fall from 35.2 dB back to 27.7 dB, and `score.py`'s
corpus mean from -28.85 dB to -28.18 dB. `make test` must keep printing
`48/48 voices survive saturation` (SPEC.md S5.5 `[M]`) at every floor value.
