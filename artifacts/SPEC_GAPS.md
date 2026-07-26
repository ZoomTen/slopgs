# SPEC_GAPS.md

## AUDIT 2026-07-25: three of our largest remaining defects are DOCUMENTED mechanisms we did not implement, not unknowns

Prompted by discovering that #19's ramp machinery is `[A]` in SPEC.md when this
file had called it `[O]`, every entry was re-checked against the spec. Three
mechanisms are fully recovered and cited in SPEC.md, were skipped or simplified
here, and each maps onto a defect independently measured against reference
audio. This reframes the project's remaining work: the dominant errors are not
things nobody knows, they are things we chose not to build.

| # | Mechanism, `[A]` in SPEC.md | Status | Measured defect it explains |
|---|---|---|---|
| 19 | S6.6 / S6.4.1: gain and **phase step** are each held in a ramp accumulator, re-derived from a caller-supplied **linear step** every `ramp_period` samples, held constant between refreshes | **PARTLY BUILT.** A phase-step ramp ships (glide 17.7 ms vs the reference's ~16.4 ms, linear in Hz). A second attempt to make it interpolate across the full message interval was **rejected 2026-07-26**: it fixed ISOLATE-2a but cost `05_pitchbend` 1.1-1.2 dB, because probe 05's reference glides *then holds flat* and one fixed-duration ramp cannot fit both. `render.c`'s gain smoother is still a one-pole, i.e. still the wrong shape. | Bend staircase (probe 31, r=0.84 at 23 ms); +11.1 dB excess near-Nyquist energy in fast-bend windows (HueArme isolate); squares instead of arcs on content that composes against the ramp |
| 14 | S1.2 / S4.2.1 / S4.7.3: controller writes go into per-controller **timestamp-keyed queues**, becoming current on periodic promotion or earlier via a **look-ahead read** | **PARTLY BUILT.** The Bank/Program locale is now latched at Program Change (`scheduled_locale`), which is the one queue whose current value is derived rather than raw. The other five are argued inert for our synchronous, sorted `smf.c` dispatch loop -- an argument from code review, never measured. | SPEC.md states the failure mode outright: "will diverge on any file that changes a controller and triggers a note within the same processing buffer". HueArme and CrystalOscillator do this at 118k and 161k CC11 events |
| 7 | S5.2-S5.5: 48 primary + 6 reserve pool, `TopUpReserve` (`0x12b6a`) once per dispatcher call; Branch B calls **`ScheduleFastRelease`** (`0x19aa4`, rate-clamped release) on up to 6 active voices | **RESOLVED 2026-07-26.** 48+6 two-tier pool, `voice_topup_reserve()` with Branch A/B, Branch B issuing a fast *release* rather than a cut, and the two steal comparators split apart again. Probe 21 -7.805 -> **-9.031 dB**, `20_voice_count` -7.113 -> -7.946. **Cadence corrected 2026-07-26 (later, same day)** -- see the cadence note below; the 47/33 residual this row previously recorded is now **48/32, exactly matching SPEC's `[M]`**. | Probe 21: the reference's oldest partials **fade** where ours terminate as flat vertical hard cuts, starting ~57 notes into the descending run |

**Update 2026-07-26:** #7 is now built and #20 (EG2) resolved separately; #19
and #14 are partly built with their remaining halves measured-and-rejected, not
merely undone. The generalisation below still holds and is still the most useful
lens on what is left.

**The common shape of the error.** In each case the original applies a change
*gradually and pre-emptively* -- ramping a control toward its target, promoting a
queued controller at a scheduled time, releasing a voice before the pool runs
dry -- and in each case we apply it *instantly and reactively*. That is one
design instinct repeated three times, not three unrelated bugs.

**Also clarified by the same pass:**

- **#10 (tremolo)**: SPEC.md Part 7 reports the `(usSource=1 LFO, usControl=0x0081
  CC1, usDestination=0x0001 ATTENUATION)` triple occurs **0 times in 7,451**
  `art1` blocks in `gm.dls`. Mod-wheel tremolo is therefore unreachable for this
  collection and needs no implementation; only the ungated (`ctrl=0`) depth
  connections remain relevant. That half of #10 can be closed as not-applicable.
- **#8 (envelope cadence)**: S6.6 is explicit that `ramp_period` is a smoothing
  mechanism, NOT an envelope generator, and warns against inferring an envelope
  cadence from it. So #19 being answered does not answer #8; the envelope
  generator's own cadence remains genuinely `[O]`.

**Method note.** This audit only happened because a claim in this file was
checked against the spec rather than trusted. The entries here are written by
implementers under time pressure and at least one confidently mis-stated what
the spec covers. Re-verify before treating any "the spec is silent" claim in
this document as fact.

**UPDATE 2026-07-26 -- rows 19 and 7 have since been acted on; read this
table as history, not current status.**

- **Row 7 (voice-stealing top-up) is RESOLVED** -- see #7 below. The
  48-primary + 6-reserve pool and both split steal comparators are
  implemented and shipped. What the table calls "no top-up" no longer
  applies; what remains genuinely open is narrower than the row implies --
  only the top-up's exact CADENCE (SPEC.md's own dispatcher-call timing has
  no recovered counterpart here) and a small 47-vs-48-voice discrepancy
  against SPEC.md's own S5.5 figure. See `FITTED.md` Entry 11.
- **Row 19 is PARTIALLY resolved, and was never as clean a case as this
  table's framing suggests.** The phase-step ramp is implemented and shipped
  (see the correction blocks under #19 below, and `FITTED.md` Entry 9) --
  "`voice_update_pitch` writes `phase_step` directly (no ramp at all)" is no
  longer true. Gain is still a one-pole exponential, unchanged, and a
  same-session attempt to fix that (a linear ramp, `FITTED.md` Entry 13) was
  measured and rejected, not merely left undone. More importantly: this
  table's framing -- "a documented mechanism we did not implement" -- is only
  half the story even for the pitch half. SPEC.md's mechanism (the ramp
  itself) is `[A]`; the caller's own slope/duration rule was, and remains,
  genuinely `[O]`, not merely unimplemented. A same-session attempt to guess
  that rule differently (a geometric ramp, `FITTED.md` Entry 12) was measured
  and rejected against the corpus, and in the process showed that the
  existing shipped ramp already closely matches at least one of this
  project's own probes (`05_pitchbend`). Do not read row 19 as "we just need
  to build the documented thing"; the documented thing (the ramp shape) is
  built, and the undocumented thing (the caller's rule) is still open and
  resisted a second attempt to close it.


Ambiguities, self-contradictions, and simplifications hit while implementing
`src/engine/` from `SPEC.md`. Each entry: where, what was unclear, what was
assumed/implemented, and (where applicable) how it was verified empirically
against `gm.dls`'s own bytes rather than guessed.

## 1. The public ABI has no way to inject a SysEx message

**Path:** `SPEC.md` Part 1 S1.5.3 (the fixed ABI) vs. Part 4 (SysEx: GM
Reset, GS Reset, Master Volume, RCV CHANNEL, USE RHYTHM PART, tuning grid).

`msgs_midi(status, d1, d2)` can only carry a 3-byte short message. There is
no ABI entry point that accepts an arbitrary-length buffer, so a live/real-
time SysEx message (GS Reset, Master Volume, etc.) cannot be delivered
through the public ABI at all, despite SPEC.md Part 4 specifying SysEx as a
required part of the control plane.

**What I did:** implemented SysEx handling as an internal (non-ABI) function
`synth_sysex(buf, len)` in `synth.c`/`synth.h`, invoked only from `smf.c`
when it encounters an `0xF0` event embedded in a loaded Standard MIDI File.
This means SysEx-driven behavior (GS Reset gating of bank select, Master
Volume, GM Reset) works for a *loaded song* but cannot be triggered by a
host calling `msgs_midi` directly. This is a structural consequence of the
fixed ABI, not a choice I could design around without changing the ABI
signature (which the assignment says not to do).

## 2. Section 2.9.2 ("Wave-object fields") contradicts Section 3.2's own model

**Path:** `SPEC.md` S2.9.2 vs. S3.2/S2.5.

S2.9.2's table, titled "Wave-object fields", lists `art1`-destination fields
(EG1/EG2 attack/decay/release/sustain, LFO frequency/delay, pan, velocity
depth) at offsets on what it calls the wave object. But S3.2 explicitly
describes a *separate* 0x68-byte "resolved articulation block" object
(`region+0x20`, distinct from the wave pointer at `region+0x04`), built by
`art1`/default-init and only adopted per-region or shared per-instrument.
S2.5's own wave-object default-initializer (`0x145a0`, 0x34 bytes) contains
no EG fields at all, which corroborates the S3.2 model and contradicts
S2.9.2's own header. I treated this as a mislabeling in S2.9.2 (its field
offsets actually describe the articulation block, not the wave object) and
implemented two separate structs (`Wave` and `Artic` in `dls.h`), matching
S3.2/S2.5. Recorded here because a naive reader of S2.9.2 alone would build
one merged struct and get this wrong.

## 3. Section 2.5 calls the velocity-depth sentinel "disabled"; the S3.5 formula says it means "full effect"

**Path:** `SPEC.md` S2.5 ("default velocity->attenuation curve is disabled")
vs. S3.5's own formula `scaled = (velAtten * depth) / -9600` with the stated
default `depth = -9600`.

Substituting the stated default into the stated formula:
`scaled = velAtten * (-9600) / (-9600) = velAtten` -- i.e. *full* velocity
sensitivity, not "disabled". A depth of `0` would give `scaled = 0`
(disabled), which is the opposite of S2.5's own default value. I implemented
per the S3.5 *formula* (default depth = -9600 -> full velocity effect),
since it is the more mechanical, directly-cited-instruction source, and
because "velocity clearly audibly affects volume" is uncontroversial for
this class of synth. Verified this is moot for `gm.dls` specifically: S7.A.5
(and my own independent read of the file) confirms `gm.dls` has zero
`(usSource=2, usDestination=0x0001)` connections, so `depth` is the default
sentinel for all 235 instruments regardless of which reading is correct.

## 4. S3.10's consolidated pseudocode mixes hundredths-of-dB and tenths-of-dB units with no shown conversion -- now SETTLED empirically (`[M: probe 07]`), against the literal (no x10) reading, previously only plausibility-argued

**Path:** `SPEC.md` S3.10 (consolidated note-on pseudocode) vs. S1.4.4 (units)
vs. S2.3.4 (region wsmp attenuation unit).

**Update, now measured rather than merely plausibility-argued:**
`probes/07_pan_volume.mid` vs `probe-results/07.flac`'s CC7 (Main Volume)
and CC11 (Expression) ladders both track the squared-law volume curve
(`g_table_vel`) already implemented, with this synth's render sitting a
flat 1.5-2.4 dB below the reference across the whole ladder -- consistent
with the already-shipped literal (no `x10`) S3.10 reading, not with the
unit-consistent (`x10`) alternative: the `x10` reading would place the
Acoustic Grand Piano's key-60 region attenuation (`-670` raw) roughly
600 dB down instead of 6.7 dB, which is not reconcilable with a probe that
plainly renders an audible, correctly-shaped volume ladder at all. The
original reasoning below (plausibility of output level, not a probe
measurement) is kept for the record; probe 07 is the first probe to give
this a real audio-based confirmation.

S3.10's pseudocode sums `velAtten`/`chanVol`/`expr` (all confirmed
hundredths-of-a-dB, from `table_1c9d0`, per S3.5/T.2) with
`region.attenuation` (confirmed *tenths*-of-a-dB, per S2.3.4/S1.4.4's own
formula `(lAttenuation*10)>>16`), with no `x10` conversion shown anywhere in
the pseudocode. S1.4.4 explicitly warns about exactly this class of mistake
("dividing that other table's values by 10 instead of 100 silently produces
attenuations 10x too large") but does not resolve which of the two readings
S3.10's own sum actually implements.

I implemented **both** readings and tested them against a real region from
`gm.dls` (the Acoustic Grand Piano's key-60-67 region, wsmp
`lAttenuation = -4390912` raw, i.e. `-670` tenths-of-dB = -67.0 dB exactly,
confirmed against on-disk bytes) whose wave's own raw PCM peaks near
full-scale (31783/32767, confirmed against on-disk bytes):

- unit-consistent (`x10`) reading: total attenuation ~-75 dB -> output peaks
  at a handful of LSBs out of 32767 at maximum velocity. This is not
  plausibly how a widely-deployed default OS MIDI synth actually sounds.
- literal S3.10 reading (no conversion): total attenuation ~-15 dB ->
  output peaks at roughly 17% of full scale at moderate velocity/volume,
  a plausible level leaving headroom for polyphony.

I implemented the **literal S3.10 reading** (no `x10` conversion) on this
empirical basis. This is a real, testable divergence in behavior depending
on which reading is chosen -- flagged prominently because it is exactly the
kind of thing S1.4.4 warns readers to get right, and the spec's own two
sections disagree on how to apply that warning to S3.10's specific sum.

## 5. Section 3.3.5's own pseudocode and its own prose disagree about which
   offset convention `region[N]` uses

**Path:** `SPEC.md` S3.3.5.

S3.3.5's code block writes `voice->0x50 = region[0x10]<<12` and calls it
"total sample end", using the same `region[N]` notation S3.3.2 established
as `(&region+4)+N` (i.e. `region+4+N`). But S3.3.5's own prose in the same
paragraph says these three fields come "from region+0x08/0x0c/0x10" --
direct, unshifted offsets, matching S2.9.1's field table instead. These two
conventions cannot both be correct simultaneously for the same code. Rather
than adopt an offset that could point at either of two different fields
depending on which convention is "right", I derived the two quantities the
render algorithm (S6.4.8) actually needs directly from what both sections
agree on: `SampleEnd = region.loop_end` and `LoopLength = loop_end -
loop_start` when the region loops, or `SampleEnd = wave.sample_count` and
`LoopLength = 0` for a one-shot region. This sidesteps the disputed third
field entirely and matches the *effect* both offset conventions were
presumably trying to describe.

## 6. `smf.c`'s design is unconstrained by SPEC.md (by its own S1.5.4 statement) -- documented design choices

- Running-status expansion happens inside `smf.c`'s SMF byte-stream walker,
  not inside `synth_midi`/`msgs_midi`. `msgs_midi` always requires an
  explicit, already-expanded status byte. This matches the ABI's own
  phrasing ("inject one short message immediately") and is a reasonable
  reading given SPEC.md places running-status parsing inside a component
  (the original driver's own byte-stream parser) that has no counterpart in
  the fixed ABI's surface.
- `msgs_load_smf` calls a full synth/voice reset before loading, so a newly
  loaded song always starts from documented default channel state. Not
  specified either way by SPEC.md (S1.5.4 disclaims any original-driver
  behavior for `smf.c`); this is simply the more predictable choice for a
  standalone test harness.
- SMPTE-based SMF division (division word's high bit set) is supported with
  a direct tick/second conversion (ignoring tempo meta events, standard
  practice); tempo-map-based (ticks-per-quarter) division is the fully
  exercised, tested path.

## 7. Voice pool: single flat 54-voice pool instead of the 48-primary + 6-reserve two-tier design

> **RESOLVED 2026-07-26.** Implemented the 48-primary + 6-reserve two-tier
> pool this entry originally described as skipped. `src/engine/voice.c` now
> splits the existing 54-voice array (`NUM_VOICES`, unchanged) into
> `NUM_PRIMARY`/`NUM_RESERVE` via `TOPUP_RESERVE_COUNT` (default 6), adds
> `voice_topup_reserve()` with Branch A (retag already-free primary voices
> into reserve) and Branch B (only when primary is ALSO empty: fast-release
> up to `NUM_RESERVE` active voices via the existing `start_release(v, 1)`
> path -- the same fast-release call already used by exclusive-key-group
> choke and same-note retrigger, no second release mechanism added), and
> splits SPEC.md S5.7's two steal comparators back apart:
> `find_steal_candidate_symmetric` (used only by top-up's own Branch B,
> gated on `fast_release_committed`) and `find_steal_candidate_asymmetric`
> (used only by note-on's forced-steal fallback, deliberately NOT gated, so
> an all-voices-committed state can never dead-end note-on -- checked with a
> targeted test that forces exactly that state).
>
> **What is fitted, not recovered:** the top-up CADENCE. SPEC.md S5.4 states
> `TopUpReserve` (`0x12b6a`) runs "exactly once per call to the event
> dispatcher `0x12bd6` ... before any queued event in that call's batch is
> processed," whose only caller is the per-tick service routine `0x13054`.
> This project has no recovered true tick, so the cadence is `[O]` and was
> fit against the corpus -- two cadences were tried and measured via
> spectral-residual analysis (`compare_spectral_22050.overall_db` on
> 32 graded probes, post-release-floor baseline mean -9.7693):
>
> - Hooked to `render.c`'s existing 64-frame (~2.9 ms) `LFO_UPDATE_FRAMES`
>   sub-chunk: fired Branch B roughly an order of magnitude too often and
>   REGRESSED exactly the probes it targets -- `21_steal_policy` -7.797 ->
>   -6.292 (+1.505 worse), `10_polyphony` -4.576 -> -3.627 (+0.950 worse),
>   `20_voice_count` -7.120 -> -6.787 (+0.333 worse), mean -9.7693 ->
>   -9.6822. Corroborated by the defect metric overshooting: broadband
>   hard-cut click count fell to 6 against the reference's 54 -- i.e. this
>   cadence faded voices the reference leaves sounding.
> - Hooked once per `render_frames()` call (the nearest analogue to a
>   dispatcher batch, since `smf.c` drains a batch of due events then calls
>   `render_frames` once for the gap to the next event) -- shipped:
>   `21_steal_policy` -7.797 -> **-9.031** (-1.234 better), `20_voice_count`
>   -7.120 -> -7.946 (-0.826 better), `10_polyphony` -4.576 -> -4.716
>   (-0.139 better), mean -9.7693 -> **-9.8380**, no probe regressed. Click
>   count 44 against the reference's 54; settle-time ratio ours/reference
>   1.85 -> 1.16.
>
> `TOPUP_PER_SUBCHUNK` (default 0) retains the rejected cadence as a
> compile-time A/B switch; `-DTOPUP_RESERVE_COUNT=0` reproduces the
> pre-feature behaviour byte-identically on probe 21. Full fit details,
> including the reserve-size rationale and the deadlock-safety check, are in
> `FITTED.md` Entry 11.
>
> **Remaining discrepancy, not papered over:** against SPEC.md S5.5's own
> `[M]` figure (80 simultaneous note-ons should leave 48 sounding and cut
> 32), this build measures 47 surviving / 33 cut -- off by one, unchanged by
> the cadence fix. Recorded as open.
>
> ---
>
> **CADENCE CORRECTED 2026-07-26 (later the same day), after a listener
> reported voices cutting out in `field/HueArme-Weekend.mid` around 23 s.**
> Both cadences A/B'd above were wrong in the same way, and the probe corpus
> could not see it: **both were tied to render.c's call structure, not to
> wall-clock time.** `smf.c` splits a `render_frames()` call at *every*
> dispatched MIDI event, so "once per `render_frames()` call" runs at MIDI
> **event density**, not at a fixed period. Instrumented on
> HueArme-Weekend: ~800 top-up calls/second at t=23 s (the probes, whose
> events are 100 ms apart, run it ~10/second -- which is why they never
> exposed it).
>
> That is fatal because Branch B is a feedback loop. A marked voice keeps
> rendering for its full ~70 ms fast release before it can be reaped and
> recycled, so any cadence faster than that drain time marks another 6
> voices before the previous batch has freed anything. Measured: Branch B
> fired **33 times in 100 ms** and the active-voice count reached **zero** --
> two total-silence dropouts (t=23.09-23.15 s and t=23.44-23.51 s) that the
> reference does not have.
>
> SPEC.md S5.5's own `[M]` figure bounds the period from the other side and
> was the missed clue: 32 cut = 26 pigeonhole + **6**, i.e. Branch B
> contributes exactly ONE batch over an entire saturated 8-second run. That
> only holds if a top-up cannot fire again until the batch it marked has
> drained and been recycled -- so the real period is at least the ~70 ms
> fast-release time.
>
> Fixed by making the cadence a real tick clock: `voice_topup_tick(frames)`
> (voice.c) accumulates rendered frames and runs one top-up every
> `TOPUP_INTERVAL_FRAMES` (default 2048, ~92.9 ms @ 22050 Hz).
> `TOPUP_PER_SUBCHUNK` is deleted -- both of its cadences were the same
> mistake, so there is nothing left to A/B against.
>
> Measured with `artifacts/score.py` (CLI port of `dist/compare.js`'s
> envelope-r and level-normalized spectral residual), 13-item field+tests
> corpus:
>
> | item | r before | r after | residual before | residual after |
> |---|---|---|---|---|
> | HueArme-Weekend | 0.602 | **0.772** | -17.38 | **-19.16** |
> | CrystalOscillator | 0.779 | **0.862** | -23.68 | **-25.01** |
> | Kot_and_A64-GENERAL_SERUM | 0.866 | 0.868 | -19.37 | -19.38 |
> | corpus MEAN | 0.780 | **0.800** | -20.37 | **-20.61** |
>
> The other 10 items are bit-identical before and after -- they never
> saturate the pool, so the top-up's Branch B never fires in them at all.
> Invented digital silence (render at its own -60 dB where the reference is
> not) falls 880 ms -> 210 ms on HueArme-Weekend and 1580 ms -> 150 ms on
> CrystalOscillator; the remainder in both is lead-in/tail, not mid-song.
> Interval swept over 64/512/1024/2048/4096 frames: the spectral residual is
> flat above 512 frames (0.03 dB spread) and cannot pick a value, but the
> probe-20/21 survivor count can -- 64 frames leaves 1 voice, 512 leaves 43,
> 1024 and above leave 48. Floor is therefore between 512 and 1024 frames
> (23-46 ms); 2048 sits above it. Full fit record in `FITTED.md` Entry 14.
>
> **The 47/33 residual recorded above is CLOSED by this fix:** probes
> `20_voice_count` and `21_steal_policy` now both settle at exactly **48
> surviving / 32 cut**, matching SPEC.md S5.5's `[M]` figure. The off-by-one
> was the too-fast cadence marking one extra voice, not a pool-size error.
>
> The original entry follows as history.

**Path:** `SPEC.md` Part 5 S5.2-S5.5.

Implemented: one pool of 54 voices (`NUM_VOICES` in `voice.h`), a linear
scan for a free slot, and (only once all 54 are active) a steal of the best
candidate by a single merged priority (prefer an already-released voice,
then oldest allocation age, then lowest current envelope level). This
reproduces the *behaviorally significant, audible* part of S5.7 (oldest-
first stealing under sustained overshoot) without the reserve top-up's
once-per-dispatch-call transient-burst mechanic (S5.4/S5.5), which only
changes the *exact number* of extra voices evicted during a brief transient
when demand suddenly exceeds 54 -- not whether 54 voices are available or
whether stealing is oldest-first. Also merged SPEC.md's two distinct
steal-priority comparators (`0x12426` symmetric vs. `0x124a8` asymmetric,
S5.7) into one comparator, since the asymmetry only matters when notes are
actively being released while stealing is also happening -- a narrower case
than the measured probes SPEC.md itself uses to validate oldest-first
stealing.

## 8. Envelope model: per-sample linear-attack / exponential-decay-and-release, not a reproduction of the original's own (unrecovered) update mechanism

**Path:** `SPEC.md` S3.4.2, S6.6 (both explicitly mark the original's own
envelope/ramp update cadence and shape as `[O]` beyond "release is
exponential" and "block-cadenced, exact frame count unrecovered").

Implemented a straightforward ADSR-style state machine per voice: linear
attack (0->1 over the DLS-authored attack time), exponential approach to the
sustain level over the authored decay time, hold at sustain, exponential
decay to zero over the release time (or a clamped 70 ms time constant for
choke/steal fast release, matching SPEC.md's own *measured* 70.0 ms choke
cut-time figure from S5.6/S5.8, rather than the exact-but-unrecovered
`region_field/70` divisor SPEC.md itself flags `[O]`). Updated once per
output sample (finer-grained than the original's own block cadence, which
SPEC.md never pins to a specific number anyway) rather than block-cadenced.

## 9. Pan law L/R channel assignment -- now SETTLED by measurement (`[M: probe 07]`)

**Path:** `SPEC.md` S3.6, explicitly marked `[O]` for which physical output
channel receives `gainA` (sqrt-law, reverse-indexed) vs. `gainB`
(squared-law, direct-indexed).

**Formerly an inference** (see the original reasoning kept below); now
**measured and settled**: `probes/07_pan_volume.mid` vs
`probe-results/07.flac` (a 9-step CC10 sweep, Acoustic Grand Piano, new
reference), CC10=0 (hard left) gives reference **L = -26.11 dBFS vs
R = -51.15 dBFS** -- left is loud, right is far down, at a hard-left pan.
Left is loud at hard left, directly confirming **Left = `gainA`** (the
reverse-indexed sqrt-law table), matching the boundary-value inference
below exactly. (The same conclusion also falls out of the per-channel-
normalised sweep table in `FITTED.md` Entry 7: L is at its own maximum,
0.00 dB, at CC10=0, while R is -26.14 dB down from its own maximum at that
same point.) Implemented unchanged: Left = `gainA`, Right = `gainB` -- see
`src/engine/voice.c`'s pan-law comment and `FITTED.md` Entry 7 for the
current gain formula (`g_table_lin`, both channels, re-centered).

**Also settled by the same probe:** the *other* half of SPEC.md S3.6's
disassembly reading -- `gainB` being the squared table `g_table_vel`,
direct-indexed -- is **REFUTED**. That formula predicts -11.90 dB of
attenuation at centre pan; probe 07 measures -3.72 dB at centre pan on the
R channel. Both channels are in fact the SAME linear/sqrt table
(`g_table_lin`), reverse-indexed for L and direct-indexed for R -- see
`FITTED.md` Entry 7.

**Original reasoning (kept for the record, boundary-value argument that
predated probe 07 and is now independently confirmed):** at `pan=0` (hard
left, standard MIDI CC10 convention), `gainB=table_vel[0]=-9600` (silent)
and `gainA=table_lin[127]=0` (unattenuated) -- i.e. whichever channel gets
`gainA` is fully present at `pan=0`. That is exactly the expected behavior
for the *left* channel at a hard-left pan position, and the symmetric
argument holds at `pan=127` for `gainB`/right. This was consistent with the
boundary values but was an inference, not a proof, at the time it was
written, since SPEC.md itself states the assignment was not traced past the
point both values are computed.

## 10. LFO->pitch (vibrato) is now applied (`[M: probe 06]`, FITTED.md Entry 6); LFO->attenuation, real-time pitch-bend-to-EG2 routing, key-follow, and the GS per-part tuning grid remain parsed but not applied

**Update:** LFO->pitch (vibrato) -- previously the highest-effort item left
out of this gap, and twice fitted-then-reverted (`FITTED.md` Entries 2/3) --
is now implemented and shipped (`dls.c`/`voice.c`/`render.c`,
`FITTED.md` Entry 6, tagged `[M: probe 06]`). Per-instrument rate (from
`lfo_freq_tc` via the absolute-pitch-cents convention, ~6.0 Hz on `gm.dls`'s
own program 48/73 bytes, matching `probes/06_modwheel.mid`'s reference),
per-instrument start delay (`lfo_delay_tc`), and the CC1-modwheel-gated
depth connection (`usSource=1,usControl=0x81,usDestination=0x0003`) are all
applied; `render.c`'s `render_frames` sub-chunks every call into 64-frame
slices so a held note's LFO actually oscillates rather than freezing at one
per-block offset. This was shipped on direct rate/depth measurement against
the reference audio (`[M]`), NOT gated on `probes/06_modwheel.mid`'s own
`compare_spectral_22050.overall_db`, because that probe's cross-correlation
alignment search is independently fragile on its ten-repeated-identical-
notes structure (flagged `IMPLAUSIBLE_OFFSET` even on the untouched
pre-vibrato baseline). The sibling ungated ("inherent", `ctrl=0`)
LFO->pitch connection is parsed (`Artic.lfo_pitch_inherent_cents`) but
deliberately NOT applied -- own fresh corpus-wide measurement found summing
it in regresses `probes/04_envelope.mid` (a probe wholly unrelated to
CC1/vibrato) by +10.7dB, a confirmed real regression on a non-06 probe; see
`FITTED.md` Entry 6 section 3 for the full measurement and root cause (a
correctly-parsed, real, but audibly-negligible 1-cent `gm.dls` connection on
Acoustic Grand Piano that nonetheless desyncs that probe's phase-sensitive
alignment). One direct, measured consequence of this scoping:
a test MIDI sends zero CC1 events anywhere, so its render is byte-identical
before/after this change -- its instruments' own gm.dls
inherent depths (1-6 cents where present at all) are real but too small to
constitute audible vibrato regardless, so there is no available design,
given `gm.dls`'s own actual data, that gives that specific file audible
vibrato without reintroducing the confirmed regression (`FITTED.md` Entry 6
section 4).

**`usSource=3` (key-follow) is now applied too — RESOLVED 2026-07-26.** Both
decay rows of SPEC.md S2.4.3's own "Source = 3" table (`0x0207` EG1 decay,
`0x030b` EG2 decay) are stored and consumed at note-on
(`voice.c`'s `decay_tc_keyfollow`); see SPEC.md S2.4.3.2 for the measurement.
This was the single largest outstanding defect in the engine: 169 of
`gm.dls`'s 235 instruments carry the `0x0207` row — every acoustic patch in
the field corpus — so dropping it made those notes decay 3–5× too slowly
(mean spectral residual −24.55 → −28.18 dB on the fix, −28.85 dB with the shape constant probe 35 then
measured). `probes/35_decay_keyfollow.mid` has since been captured: it settles
the source as the *absolute* key rather than 60-relative (the reading this
document used to rule key-follow out), but the measured divisor 126.5 ± 0.8
still cannot separate `/127` from `/128`. That much remains `[I]`.

**Still not applied, unchanged from before:** `art1` connections for
`usSource=1` (LFO depth to ATTENUATION/tremolo, `usDestination=0x0001`, 46
blocks in `gm.dls`), `usSource=3` to any destination outside the two decay
rows (`gm.dls` authors none), and the GS SysEx 12-entry-per-part Scale
Tuning grid are all parsed into their respective struct fields (or silently
accepted in the SysEx dispatcher) but have no runtime effect: there is no
tremolo oscillator and no per-semitone-class tuning offset applied anywhere
in `render.c`/`voice.c`. Priority order in the assignment (numeric-detail
signal path, voice model, control plane) was followed; these remain
lower-priority, still-unattempted follow-ups with no measured probe
reference yet.

## 11. RCV CHANNEL "Part" indirection is not modeled

**Path:** `SPEC.md` S4.2.2/S4.6.1/S4.8/T.8 (the `USE RHYTHM PART`/`RCV
CHANNEL` per-"Part" tables, with a channel<->Part remap layer, and the
static default-RCV-CHANNEL table at VMA `0x1a600`).

Implemented: channel index doubles directly as "Part" index for all *other*
per-part state (no remap table) -- but `USE RHYTHM PART` is the one
exception that does honor the *default* Part->channel mapping, because it
has to: `synth_sysex`'s GS branch now parses the 3-byte GS part-parameter
address correctly (`a0=0x40` fixed, `a1=0x1<block>` in `0x10..0x1F`,
`a2=<param>`; previously `a0`'s low nibble was misread as the part and
`a1` was misread as the param, so this address family never matched and
`USE RHYTHM PART` was silently a no-op) and, for `a2==0x15` (`USE RHYTHM
PART`), maps the address's block nibble to a channel index via the
default `RCV CHANNEL` table `SPEC.md` T.8 documents byte-for-byte
(`[9,0,1,2,3,4,5,6,7,8,10,11,12,13,14,15]`, block 0 -> channel 9, blocks
1-9 -> channels 0-8, blocks 10-15 -> channels 10-15 identically) before
writing `is_rhythm`. This is *not* full RCV CHANNEL support: an actual
`RCV CHANNEL` SysEx message (`a2==0x02`) is still parsed (address
recognized) but has no effect, so a file that reassigns a Part to a
non-default physical channel via `RCV CHANNEL` and *then* sends `USE
RHYTHM PART` for that Part will still update the default-mapped channel,
not the reassigned one. This only matters for content that explicitly
reassigns Parts via GS SysEx `RCV CHANNEL`, which is uncommon in ordinary
GM/GS content (confirmed absent from `field/Kot_and_A64-GENERAL_SERUM.mid`,
which sends `USE RHYTHM PART` for every block at time 0 but never sends
`RCV CHANNEL`). The per-part 12-entry tuning grid remains parsed-address
but not applied (§10 above).

## 12. Wave-level 8-bit PCM storage path deviates from "reference in place"

**Path:** `SPEC.md` S1.5.5 ("sample data is referenced, not copied").

`gm.dls` is 495/495 16-bit mono PCM (confirmed, S2.7.3/S2.11), so this path
is never exercised for `gm.dls` itself. For a hypothetical 8-bit-source
wave, `dls.c` converts to a small int16 copy at load time (`(sample-128)*
256`) rather than referencing in place, because `render.c` only implements
the 16-bit fetch tap. The log-companded 8-bit storage path (S2.7.2,
`flagByte` bit 0) and the two 8-bit MMX mixer variants (functions A/B,
S6.2.2) are not implemented at all, matching SPEC.md's own note that these
are real but structurally unreachable for this specific sample collection.

## 13. Numeric tables built but not wired into the signal path

`tables.c` builds all five runtime tables SPEC.md's Appendix T documents
(T1 dB->linear-amplitude, T2 cents, T3 semitone, the 201-entry envelope/
time-progress curve, the 256-entry sine LFO, and the 2048-entry log-
companding curve), matching the module map's requirement that `tables.c`
build "the runtime lookup tables this project's numeric-data appendix fully
documents". T2, T3, the two velocity/pan tables (squared-law and sqrt-law), and now
(since `FITTED.md` Entry 6, item 10 above) the 256-entry sine LFO table
(`g_table_sine`, sampled by `voice.c`'s `voice_lfo_cents` for pitch
vibrato) are actually consumed by the gain/pitch/pan computations in
`voice.c`; T1, the envelope-shape curve, and the companding table remain
built (with the exact formulas and truncating conversions specified) but
have no runtime consumer, because their own consumers in the original
driver are themselves marked `[O]`/unrecovered by SPEC.md itself (S2.7.2's
`flagByte` origin, S6.6's ramp/envelope cadence).

## 14. Event scheduling: no timestamp-keyed pending-controller queues

**Path:** `SPEC.md` S4.2.1/S4.7 (six per-controller scheduled queues with
look-ahead promotion).

`msgs_midi` is documented in the ABI itself as injecting a message
"immediately". Implemented: every controller write in `synth.c` takes
effect immediately on the call that produces it; there is no
scheduled/promoted queue and no look-ahead read. For `smf.c`-driven
playback this is equivalent in observable effect (events are already
dispatched at their own correct sample time by the sequencer's own event
loop in `smf_render`), but a host injecting out-of-order or batched
`msgs_midi` calls with its own timestamps (not supported by the ABI at all,
since `msgs_midi` carries no timestamp parameter) cannot rely on the
look-ahead-visibility behavior S4.7.3 describes.

## 15. `exp_coef`'s per-sample coefficient: retuned from a 1/e time constant to a "100dB-over-`seconds`" calibration -- SPEC.md leaves the exact number `[O]`

> **The "~2.9x still too slow" residual below is RESOLVED 2026-07-26: it was
> decay-time key-follow.** This entry ruled `usSource=3` out on the strength
> of a `scale*(60-keynum)` reading, which is zero at note 60 and therefore
> could not explain a note-60 measurement. That reading was wrong: DLS-1
> normalizes the KEYNUMBER source as `key/128`, un-shifted, so Piano 1's own
> `-3979` contributes `-1865` timecents at note 60 -- 40.02s -> 13.63s, i.e.
> **7.34 dB/s against the 7.14 dB/s measured below**, from `gm.dls`'s own
> data with no fudge factor. Shipped; see SPEC.md S2.4.3.2 and item 10 above.
> The "100dB-over-`seconds`" calibration this entry describes is unchanged
> and still `[O]` as a *mechanism* -- key-follow supplies the *duration*, this
> supplies the *shape*, and the two were being conflated. Its *value* is no
> longer open: `probes/35_decay_keyfollow.mid` measures the reference decaying
> at 0.965x this project's rate, uniformly across 17 notes / 3 instruments /
> a 7x rate range (sd 0.005, no trend vs. key), so the decay segment is
> **96.5 dB over `seconds`**, shipped as `DECAY_RATE_MULT = 0.965`. Release
> keeps 100 dB -- probe 35 measures decay only and S5.6's 70 ms fast-release
> still matches. See FITTED.md Entry 15 section 4b.

**Path:** `SPEC.md` S3.4.1 (timecent->duration formula, confirmed `[A]`) vs.
S3.4.2 (release *shape* confirmed exponential, but exact per-sample
consumption of the resulting duration explicitly marked `[O]`) vs. Part 5
S5.6 (a wholly separate, already-`[M]`-measured data point: fast/choke
release reaches full silence in 70.0 ms, matching the 70ms rate-clamp
constant directly -- not ~7.6x that, which is what an asymptotic-tau reading
of "duration" would require to reach even a -66dB floor).

Measured directly (own fresh measurement, this pass): rendering
`probes/04_envelope.mid` (a single-note attack/decay/sustain/release sweep)
against its reference capture `probe-results/04.flac`, a held note's
release segment decays *linearly in dB* at a near-constant ~100 dB/s
(R²=0.9994 log-linear fit over a 4s window) for a patch whose own
timecent-derived release duration is 0.99s -- i.e. it reaches full silence
right around that 0.99s mark, not after ~7.6 time constants of an
asymptotic decay-to-zero (which the previous implementation computed,
making release/decay ring ~11x-33x longer than the reference and leaving
`corridor.mid` never latching `msgs_is_finished` within a 20s-slack render
cap -- 82.8s actual vs. a 52.4s reference).

**What I did:** changed `exp_coef(seconds)` in `voice.c` from a 1/e-time-
constant model to a "decays 100dB over exactly `seconds` seconds" model
(`c = 10^(-5/samples)`), applied uniformly to both EG1 decay and release
(the same shared helper). This matches the S5.6 fast-release measurement
almost exactly and the probe-04 release measurement well. It does **not**
fully explain the EG1 *decay*-segment rate for one measured case (Acoustic
Grand Piano, sustain=0, note 60): predicted ~2.5 dB/s vs. measured ~7.14
dB/s, roughly 2.9x still too slow. I looked for (and ruled out) an
`usSource=3` (KEYNUMBER) key-scaling connection targeting EG1 decay as the
explanation -- `gm.dls` does carry one for this exact instrument
(confirmed directly against the file's raw bytes, matching SPEC.md S2.4.3's
own confirmed `usSource=3`/`0x0207` table), but its contribution is
`scale*(60-keynum)`, which is exactly zero at note 60 (the only note
probe 04 exercises), so it cannot be the cause of the residual 2.9x this
measurement shows. The precise remaining discrepancy is unresolved; the
real consumption code (SPEC.md Part 5 `+0x13c`/`0x194da`/`0x19644`) lives
outside every PAGE range this project examined and is marked `[O]`.
Measured net effect (spectral-residual graded-probe mean): -2.64dB ->
-3.08dB; `field/corridor.mid` render duration: 82.8s (truncated, never finished)
-> 52.57s (finishes, vs. a 52.43s reference). Kept.

I also tried replacing the release-segment finish threshold (`env_level <
0.0005`, an arbitrary -66dB) with SPEC.md Part 5's own confirmed
finish-detection floor constant (`0xffffe0c0` = -8000 hundredths-dB =
-80dB, `[A:0x19733]`). Measured: byte-identical `spec_ovr` on every graded
probe, and a very slightly worse (further from reference) `corridor.mid`
duration (53.13s vs. 52.57s against a 52.43s reference). Reverted --
no measured benefit.

## Verified-absent risk, not a gap: gain law double-counting (fixed during implementation, not shipped)

An earlier draft of `voice.c` summed *both* the region's own wsmp
attenuation and the wave object's own wsmp attenuation into the gain
formula. Testing against real `gm.dls` bytes (a wave whose own
`lAttenuation` decodes to -104.2 dB, stacked with a -67 dB region-level
term) showed this produces silence-adjacent output (single-digit sample
values at full velocity). SPEC.md's own S3.5/S3.10 pseudocode sums only the
region's term, matching "region overrides wave" (S2.6/S3.3.2 -- every
`gm.dls` region carries its own wsmp). Fixed before delivery; recorded here
because it is exactly the kind of error the "verify before flag" / warning-
budget discipline is meant to catch, and because item 4 above (the
tenths-vs-hundredths question) was investigated and resolved using the same
empirical method (real `gm.dls` bytes plus the actual raw PCM peak of the
affected wave) at the same time.

## 16. Where latched RPN1/RPN2 tuning enters the pitch chain -- SETTLED by measurement (`[M: probe 30]`), the spec was silent

> **RESOLVED 2026-07-25. Latched RPN1/RPN2 tuning IS summed inside the ±4800
> clamp; the shipped code was right all along and needed no change.**
> `probes/30_tune_clamp_bend.mid` section B (RPN2 = +24 semitones, no bend)
> drives keys 105-127 past the clamp on program 86. In the captured reference
> `probe-results/30.flac`, keys 119 and 127 -- two different keys -- both read
> **3028.8 Hz**, i.e. the real driver collapses them onto one pitch exactly as
> a summed-into-the-clamp implementation does. All 24 notes of sections A, B
> and D match our render to ratio 1.000. Variant A below (tuning applied
> outside the clamp) is therefore **refuted by direct measurement**, not merely
> rejected on a corpus regression -- the corpus regression it caused was
> pointing at a real error. The investigation and its numbers are kept below
> as recorded.

### Original entry (SPEC underspecification, still accurate as a description of the spec)

**Path:** `SPEC.md` S3.3.2 (the note-on cents sum) vs. S3.3.3 (the clamp) vs.
S4.4 / Part 7 open item #10 (the RPN1/RPN2 consumer).

S3.3.3 confirms `CentsToRatio` clamps its argument to ±4800 cents
byte-for-byte (`[A:0x18e2c]`-`0x18e6c`, matching T3's ±48-semitone domain).
S3.3.2's disassembled note-on sum is
`cents = fineTune + (key - unityNote)*100 + pitchBendCentsParam`
(`0x18f53`-`0x18f62`) — it contains **no RPN1/RPN2 term at all**. Part 7 open
item #10 states plainly that the RPN1/RPN2 consumer was never located in any
traced range (`[O]`), while probe 23 measures only that the values ARE applied
and are note-on latched. So the spec establishes that latched tuning reaches
the pitch somehow, and is silent on where — specifically on whether it is
summed into the clamped quantity or applied outside it.

**Why this is not academic.** Below the clamp the two readings are
algebraically identical (`2^((a+b)/1200) = 2^(a/1200)·2^(b/1200)`), so no
existing probe can tell them apart — probe 23 exercises RPN2 at keys nowhere
near ±4800. Above the clamp they diverge completely. `gm.dls` program 86
(5th Saw Wave) has unity note 81/91; GENERAL SERUM measure 226 plays keys
105-127 on it with +2400 cents of RPN2 coarse tune, which puts **every note
except one past +4800**:

```
key 105: +2406 base, +4806 with RPN2      key 119: +2801 base, +5201 with RPN2
key 116: +2501 base, +4901 with RPN2      key 127: +3601 base, +6001 with RPN2
```

Summed into the clamp, all of them pin at +4800 and live pitch bend cannot
move them at all. Measured: our render of that passage is audibly and
visibly frozen (long horizontal partials in a spectrogram where the reference
shows clean diagonal sweeps; spectral-centroid spread 348 Hz vs the
reference's 966 Hz).

**What I did: nothing — the current code sums RPN1/RPN2 into the clamped
cents, unchanged.** Two alternatives were implemented and measured against
the reference capture, and **both were rejected**:

| variant | isolate residual | GENERAL SERUM |
|---|---|---|
| shipped (RPN inside clamp) | +2.33 dB | **−4.24 dB** |
| A: RPN as a separate factor outside the clamp | **+0.24 dB** | −1.10 dB (regressed 3.1 dB) |
| B: base+RPN clamped at note-on, live bend/LFO as a separate factor | +1.48 dB | not run (A's result made the class suspect) |

Variant A fixes the passage and regresses the acceptance north star by 3.1 dB
(envelope r 0.831 → 0.743, alignment unchanged and unflagged, so the
regression is real). Since the two readings can only differ where clamping is
active, that regression says other saturating notes elsewhere in the song
*should* stay clamped — i.e. neither "always inside" nor "always outside" is
right, and the truth is not derivable from the audio we have.

**What would settle it:** `probes/30_tune_clamp_bend.mid` (authored, awaiting
a reference capture). Section B holds keys straddling the clamp at RPN2 = +24
semitones — if tuning is inside the clamp, keys 105+ collapse to one pitch;
if outside, each stays distinct. Section C repeats it with a full-range bend
sweep, which separates the bend question (#17) on the same material.

## 17. Whether a live pitch bend re-enters the clamped sum -- SETTLED by measurement (`[M: probe 30]`), SHIPPED

> **RESOLVED 2026-07-25. Live modulation (pitch bend and LFO) is applied as a
> SEPARATE ratio factor OUTSIDE the ±4800 clamp. Implemented and shipped in
> `src/engine/voice.c`.**
> Same probe, section C: the same saturated base as section B plus a
> full-range bend sweep. The reference's clamped keys 119/127 **move**
> (1685.0 / 1687.7 Hz) where a single-clamped-sum implementation stays pinned
> at section B's 3028.8 Hz. So the base clamps but bend still sweeps it --
> the two facts together (#16 inside, #17 outside) are only consistent with a
> two-factor phase increment.
>
> `voice_note_on` now latches `v->base_ratio_q12 = CentsToRatio(base_cents)`
> once, and `voice_update_pitch` multiplies it by `CentsToRatio(bend + LFO)`.
> Corpus effect, corrected aligner both sides: **1 probe better, 0 worse** --
> `06_modwheel` -6.49 -> **-13.12 dB** (the LFO is live modulation too, which
> is why that probe defeated FITTED.md Entries 2/3/6), corpus mean -10.113 ->
> **-10.391**, GENERAL SERUM -4.239 -> **-4.704** with envelope r 0.8313 ->
> **0.8645**, corridor render length unchanged, self-tests pass. This is
> `[M]`, measured against a real capture -- not a fit.
>
> **Residual: CLOSED.** Probe 30's clamped keys showed our peak at 1779.9 Hz
> against the reference's 1685.0/1687.7 -- a 5.6% gap that could not be
> attributed, because program 86 is a saw whose fundamental is far above
> Nyquist at those keys, so the measured peak is an aliased image and argmax
> can land on a different partial in each render.
> `probes/31_tune_clamp_bend_sine.mid` / `probe-results/31.flac` repeats the
> identical structure on bank 8 program 80 (Sine Wave), the only
> single-partial patch in `gm.dls`, where whatever appears IS the folded
> fundamental. Result: **sections A, B and D match the reference at ratio
> 1.000 on all 18 notes, and section C's bend sweeps fall within 0.994-1.014
> with no outlier and no systematic bias** -- the clamped keys 119/127 land at
> 1.014 and 0.996. The 5.6% was an artifact of the measuring instrument, not a
> pitch error. The ±1% left on section C is a 0.5 s analysis window averaging
> a tone that is sweeping throughout it.
>
> Probe 31 is worth keeping for any future pitch question: it is the project's
> only clean single-partial instrument.

### Original entry (SPEC underspecification, still accurate as a description of the spec)

**Path:** `SPEC.md` S3.3.2/S3.3.4 (note-on only) vs. S4.4 (bend is
continuous, RPN1/RPN2 are latched, `[M: probe 23]`).

Every `CentsToRatio` call SPEC.md exhibits is on the **note-trigger** path
(`0x18f65`, reached from `0x19b54`). S4.4 establishes that pitch bend, unlike
RPN1/RPN2, retunes an already-sounding voice — but the code that does that
re-tuning is not shown anywhere in the spec, so whether it recomputes the
whole clamped sum or multiplies a stored base ratio by a bend ratio is
undetermined. The two are indistinguishable except when the base is
saturated, where the first freezes the note and the second lets it sweep.

`src/engine/voice.c` implements the first (recomputes the full sum every block,
clamp included). Variant B above implements the second; it improved the
measure-226 isolate (+2.33 → +1.48 dB) but was not adopted, because it is a
structural change that also perturbs every non-saturated note by an extra Q12
truncation and it did not resolve the larger discrepancy. Same probe as #16
(section C) settles it.

## 18. Expression (CC11) behaviour under fast gating is unmeasured, and the reference does something our smoothing model cannot produce

**Path:** `FITTED.md` Entry 4 (`GAIN_SMOOTH_ALPHA`, `[F:fitted]`) vs.
`SPEC.md` S6.6 (ramp/envelope update cadence, `[O]`).

Entry 4's 12 ms gain-smoothing time constant was fit against slow CC7/CC11
glides. GENERAL SERUM measure 226 gates CC11 between 127 and 0 every 12-25
ms — the same order as the constant itself, so the fit is being extrapolated
into a regime it was never measured in.

Measured, and not explicable by any single-pole smoothing: the reference
contains **hard near-silent spans of 25-90 ms** (at 0.923-1.013 s and
2.260-2.285 s) during a stretch where CC11 alternates 0/127 every 25 ms.
A symmetric one-pole filter
tracking a 50%-duty square cannot go silent for 90 ms; it settles around a
mid-level. Our render shows no gap at all there. Sweeping
`GAIN_SMOOTH_ALPHA` across 0.05-30 ms moves the isolate residual by only
0.19 dB (+2.49 → +2.30), so the constant is not the free parameter — the
*shape* of the response is wrong, not its rate. Candidates the audio cannot
separate: an asymmetric attack/release, expression sampled on a slow control
cadence rather than per-block, or a gate that latches at note-on.

**What I did: nothing.** `GAIN_SMOOTH_ALPHA` is unchanged; only an
`#ifndef` guard was added around it in `render.c` so the constant can be
overridden with `-D` for sweeps without editing the file.

**What would settle it:** `probes/28_expression_gate.mid` (authored, awaiting
capture) square-waves CC11 at half-periods of 200/100/50/25/12/6 ms on a held
note, then isolates one clean fall and rise edge 300 ms apart. That reads the
response as a function of rate and separates the two edges, which is exactly
what the mixed-rate content in GENERAL SERUM cannot do.

Related: `probes/29_all_sound_off_gap.mid` measures how fast the driver
actually reaches silence after CC120 versus after an ordinary note-off, at
gaps of 400 down to 12 ms, on two patches. The reference's measure-226
silences may be post-CC120 decay rather than expression behaviour at all, and
these two probes together separate those explanations. SPEC.md marks the
choke/fast-release divisor's base quantity `[O]` (Part 5 S5.6/S5.8), so this
is not recoverable from the spec either.

## 19. Control changes are RAMPED -- and SPEC.md S6.6 documents the mechanism as `[A]`; we simply never implemented it

> **RESOLVED 2026-07-25 (pitch half only).** Implemented the phase-step half
> of this entry: `voice_update_pitch` (`src/engine/voice.c`) now writes a
> `phase_step_target` and lets `phase_step` glide toward it linearly, one
> sample at a time (`render.c`'s `render_voice`), instead of writing
> `phase_step` directly. Two `-D`-overridable constants,
> `PITCH_RAMP_RATE_FRAC_PER_MS` (0.03635) and `PITCH_RAMP_MAX_MS` (20.6375),
> stand in for the caller-side slope rule S6.6 leaves `[O]` -- see
> `FITTED.md` Entry 9 for the fit, the full corpus A/B (isolated via
> direct-compiled unique-path binaries per this task's own contamination
> warning, not through the shared `dist/msgs-render`), and the
> bend-rate-conditioned HueArme isolate result.
>
> **What moved:** probe 33's own `-24` semitone bend step (the fit point):
> 10-90% glide 0.50ms -> 17.73ms, matching the reference's ~16.37ms and its
> R²(Hz) > R²(cents) ordering (0.9993/0.9794 vs. reference's 0.9974/0.9780).
> HueArme isolate, bend-rate-conditioned 9.5-11kHz excess: high-bend-rate
> tercile +13.24dB -> +12.72dB, low-bend-rate tercile +4.60dB -> +3.89dB (no
> regression), no-bend windows +6.06dB -> +3.26dB. Corpus: 25 of 29 graded
> probes unchanged to 3 decimals; `05_pitchbend`/`06_modwheel` moved by
> <0.11dB (noise); `31_tune_clamp_bend_sine`/`32_ramp_shape` improved
> slightly; `33_pitch_ramp`'s own whole-file number moved +0.297dB (a
> real but small residual cost from the duration cap, see FITTED.md Entry
> 9); `30_tune_clamp_bend`'s apparent +3.2dB move was checked directly and
> is a pre-existing harness alignment-search artifact (the fine-refine
> window doesn't reach the true global-best lag, which is IDENTICAL for
> both the old and new code), not a real content change.
>
> **What was NOT done:** gain is still smoothed with `GAIN_SMOOTH_ALPHA`'s
> one-pole exponential (Entry 4), per this task's own explicit instruction
> to change pitch first and treat gain as a separate experiment. The wrong
> SHAPE for gain (exponential vs. SPEC's linear) is therefore still open.
> The caller-side slope/duration rule remains this project's fit, not a
> recovered value -- in particular, why `+-40`/`+-70` semitone bends
> apparently do NOT glide proportionally slower than a `-24` semitone one
> (which is why `PITCH_RAMP_MAX_MS` exists at all) is unexplained by
> anything in SPEC.md and needs a real RE pass or more reference captures
> at more bend sizes to settle.

> **CORRECTION 2026-07-25.** Earlier revisions of this entry (and of FITTED.md
> Entry 8's surrounding notes) said the ramp machinery was `[O]`/unrecovered in
> SPEC.md. **That is wrong and was my error, not the spec's.** SPEC.md S6.6
> marks the mechanism `[A]`, cited to `0x19e26` / `0x19ec2` / `0x19ee9`:
>
> > the active gain value and the active phase step are each held in a coarse
> > "ramp accumulator" that is re-derived from a **linear step** supplied by the
> > caller once every `ramp_period` samples ... Between refreshes, the value
> > used for interpolation/gain is held constant; this is a **linear-ramp
> > smoothing mechanism**.
>
> S6.4.1 gives the mixer's own call arguments confirming it: `+0x14` ramp
> period (samples between refreshes), `+0x18`/`+0x1c` gain ramp step per
> channel, `+0x20` **phase-step ramp step** -- each added to an internal `<<8`
> accumulator every ramp period.
>
> So the ramp is **linear on the phase step** (the frequency ratio), refreshed
> on a fixed sample cadence, held constant in between. Not exponential, and not
> in the cents domain.
>
> What is genuinely `[O]` is much narrower: **what determines the slope values
> the caller writes into those arguments between calls**, and the envelope
> generator's own update cadence. S6.6 says so explicitly and warns not to
> infer an envelope cadence from `ramp_period`.
>
> **Consequences for this implementation:**
> 1. `src/engine/voice.c` has no phase-step ramp whatsoever -- `voice_update_pitch`
>    writes `v->phase_step` directly. That is the defect behind every
>    measurement in this entry and both addenda.
> 2. `render.c`'s `GAIN_SMOOTH_ALPHA` is a one-pole exponential, i.e. the wrong
>    SHAPE for a mechanism the spec says is linear. This explains why probe 28
>    rejected both a 12 ms one-pole and an instant gate, and why probe 32 found
>    both engines ramping but with mismatched laws.
> 3. `probes/33_pitch_ramp.mid` (authored, awaiting capture) targets exactly the
>    remaining `[O]`: whether the slope is fixed-duration or rate-limited, its
>    shape at large excursions, its up/down symmetry, and whether it aims at the
>    raw or the clamped target.
>
> The original entry text follows, still accurate as a record of the
> measurements; only its claim about SPEC.md's coverage was wrong.

> **UPDATE 2026-07-26 -- this is NOT simply "a documented mechanism we
> skipped," and a follow-up attempt to close the remaining `[O]` half was
> tried and rejected.** The audit block at the top of this file (and the
> framing above) present #19 as a case where SPEC.md fully documents the
> mechanism and this project merely never built it. That is only half true:
> the phase-step RAMP mechanism is `[A]` (confirmed, Entry 9 above ships it),
> but the caller's own SLOPE/DURATION rule -- the actual number a future RE
> pass would need to recover -- remains genuinely `[O]`, and this session's
> attempt to guess it via a different functional form did not improve on the
> fit already shipped.
>
> **Attempted this session: a geometric ramp** (`PITCH_RAMP_K_PERIODS`,
> re-deriving the step every 64-sample refresh as `(target-current)/(K*64)`
> instead of Entry 9's fixed rate-with-ceiling design). It clearly moved
> toward the reference on a specific hand-cut isolate excerpt (fraction of
> frames changing by <1 cent: 34.0% -> 13.8% against the excerpt's 11.1%),
> but it REGRESSED the corpus
> at every K tried (2.0/3.0/4.0): mean -9.7206 -> -9.7156/-9.6604/-9.6677,
> `05_pitchbend` +1.10 to +1.20 dB worse at every K with no alignment flag.
> **Not shipped.** Full measurements and the falsifiability check are in
> `FITTED.md` Entry 12.
>
> **The root cause matters for how this gap should be read going forward:**
> probe 05's own reference does NOT glide continuously -- it glides then
> holds flat (80.2% static frames in the reference vs. 80.3% in our existing,
> unmodified, fixed-duration ramp -- i.e. **the existing ramp already
> matches probe 05 closely**). The ISOLATE-2a file that motivated the
> geometric redesign contains large bend steps (multi-thousand-cent swings)
> arriving every ~42 ms, which is genuinely different bend CONTENT from
> probe 05's small steps, not evidence that the existing ramp's shape is
> wrong. One fixed-duration ramp is consistent with both references; a
> single global geometric constant tuned against the large-step content
> necessarily over-corrects the small-step content.
>
> **Where this leaves the entry:** `PITCH_RAMP_RATE_FRAC_PER_MS` (0.03635)
> and `PITCH_RAMP_MAX_MS` (20.6375) are UNCHANGED in `src/engine/voice.c` --
> they were never touched by this attempt, remain the shipped fit (Entry 9),
> and should not be read as retired or superseded by this update. What is
> newly established is that the caller-side slope rule's remaining `[O]`
> is not closed by a geometric reading, and a future pass should look for a
> rule keyed to bend magnitude (as Entry 9's own rate-with-ceiling design
> already is) rather than a fixed ratio -- see `FITTED.md` Entry 12 section 5.

### Original entry

**Path:** `SPEC.md` S6.6 (ramp/envelope update cadence, `[O]`) vs.
`FITTED.md` Entry 4 (`GAIN_SMOOTH_ALPHA`, a fitted 12 ms one-pole).

Two independent measurements against new captures say the same thing, and
neither of the two obvious implementations satisfies both.

**Expression (`probes/28_expression_gate.mid` / `probe-results/28.flac`).**
CC11 square-waved on a held note at half-periods 200/100/50/25/12/6 ms.
Modulation depth, 90th vs 10th percentile of the 2 ms RMS envelope:

| half-period | 200 | 100 | 50 | 25 | 12 | 6 ms |
|---|---|---|---|---|---|---|
| REFERENCE | 80.1 | 79.9 | 79.4 | 61.9 | 79.8 | 77.9 dB |
| ours, 12 ms one-pole | (zero) | 58.5 | 29.3 | 14.8 | 7.4 | 4.3 dB |

The reference holds ~78-80 dB at *every* rate -- it reaches silence on every
CC11=0 no matter how fast. That 78-80 dB is the capture's own dither floor,
not a smoothing signature, so the real gate is effectively complete. Our
shipped 12 ms constant collapses to 4.3 dB by 6 ms.

**But removing the smoothing is also wrong.** Rebuilt with
`GAIN_SMOOTH_ALPHA = 1.0` (instant, per-sample): probe 28's own spectral
residual got *worse*, -7.19 -> -6.35 dB, and the corpus mean -10.288 ->
-10.257, GENERAL SERUM envelope r 0.8645 -> 0.8619. An instant per-sample
gate produces a broadband click the reference does not have. So the answer is
bounded on both sides: much faster than 12 ms, but not a per-sample step.
**Nothing was shipped; `GAIN_SMOOTH_ALPHA` is unchanged.**

**Pitch bend (`probes/31_tune_clamp_bend_sine.mid` / `probe-results/31.flac`).**
Section C sweeps bend with one message every 18 ms. Tracking the carrier's
instantaneous frequency (2048-pt STFT, 64-sample hop, parabolic peak
interpolation) and autocorrelating its derivative to detect step structure:

| | span | staircase |
|---|---|---|
| REFERENCE | 3163.7 -> 3852.2 Hz | none (peak 220 ms, r=0.32) |
| ours | 3169.1 -> 3854.7 Hz | **23 ms, r=0.84** |

Endpoints agree, so the bend *scaling* is right (that is Entry 8's result).
The trajectory between them is not: we step at roughly the message rate, the
reference is smooth. So the driver is interpolating pitch between bend
messages rather than latching each one.

**The hypothesis both measurements support:** control changes are applied via
a *ramp* -- a short linear glide to the new value, completed within a control
block -- rather than instantly or via a slow one-pole. That shape would give
smooth bend trajectories, full-depth expression gating at 6 ms, and no click,
simultaneously, which neither implementation tried so far does.
`SPEC.md` S6.6 marks precisely this machinery `[O]`, and Part 5's `+0x13c`
consumption code lives outside every PAGE range this project examined.

**What would settle the ramp length:** probe 28's final section already
isolates one clean CC11 fall and one rise 300 ms apart, at a rate where the
edges cannot interact -- measuring the reference's edge shape there directly
gives the ramp duration and whether it is linear in amplitude or in dB. That
analysis has not been done yet; it needs no new capture.

### #19 addendum -- the bend staircase has an AUDIBLE consequence, not just a trajectory mismatch (`[M: HueArme-Weekend_ISOLATE]`)

The staircase documented above was measured as a *shape* difference (our carrier steps at
~23 ms, the reference's is smooth). New material shows it also injects broadband energy
near Nyquist, which is audible as grit the reference does not have.

A hand-cut excerpt (two-channel, 76-note) carries **10,756 pitch bends -- 141 per note**,
with peak polyphony 4 and keys only reaching 84. Sparse voicing plus extreme bend makes it
the cleanest bend test in the corpus, and any aliasing in it is driven by bend pushing pitch
past Nyquist rather than by high keys.

Spectral balance over the full 137.8 s, each render normalised to its own total (so this
is balance, not level -- overall RMS matches at -24.03 vs -23.82 dBFS):

| band | REFERENCE | OURS | diff |
|---|---|---|---|
| 0-500 Hz | -2.7 | -2.2 | +0.6 |
| 500 Hz-8 kHz | -5.7 .. -14.8 | -6.2 .. -16.8 | -0.5 .. -2.0 |
| 8-9.5 kHz | -18.3 | -19.6 | -1.3 |
| **9.5-11 kHz** | **-30.1** | **-20.3** | **+9.9** |

Everything below 9.5 kHz matches within 2 dB; the top octave is ~10 dB hot. The same
signature appears independently on `CrystalOscillator` measure 72 (+12.3 dB at 9.5-11 kHz,
all other bands within 0.2 dB) -- two unrelated tracks.

**Ruled out: a capture artifact.** kmixer's 22050->44100 anti-imaging filter would roll off
the top octave in EVERY reference, and decimating back would not restore it. It does not:
`probe-results/31.flac` (sine) reads -8.8 dB in 9.5-11 kHz against our -8.8, and probe 03
reads -53.9 against our -52.6. The excess is ours and is specific to this material class.

**Established: it tracks bend activity.** Splitting the isolate into 551 windows of 0.25 s
and ranking them by summed bend movement per window:

```
low-bend-rate windows   (bottom third):   -5.2 dB   <- we are QUIETER in the top octave
high-bend-rate windows  (top third):     +11.1 dB   <- we are much HOTTER
windows with no bend at all (n=21):       +3.9 dB
correlation(log bend rate, excess):       +0.162
```

A 16.3 dB swing between slow-bend and fast-bend windows. (The linear correlation is weak
because the relationship is not linear in log-rate and the per-window estimate is noisy;
the tercile split is the robust statement, not the r value.)

**Interpretation, inferred not proven:** stepping the phase increment in discrete ~23 ms
jumps introduces a small frequency discontinuity at each step. In a sampled oscillator read
with 2-tap linear interpolation those discontinuities scatter energy broadband, and the
interpolator's own response concentrates what survives near Nyquist. Ramping the increment
between bend messages -- the fix #19 already proposes -- should remove it. This entry
records the audible consequence so a future implementer knows the ramp is not cosmetic.

**Note the sign flip:** during LOW bend activity we are 5.2 dB *short* in the same band,
consistent with the -1.3 to -2.0 dB deficit across 4-9.5 kHz. Whatever is fixed here must
not be evaluated on a whole-track average, which cancels the two effects against each other.

### #19 addendum 2 -- the pitch ramp is LOAD-BEARING: real content composes against it (`[M: HueArme-Weekend_ISOLATE]`)

Reading the isolate's actual MIDI events, rather than comparing the two renders to each
other, changes what this gap means. In the window 6.923-9.323 s:

- **RPN0 pitch-bend range is set to 127 semitones on channel 0 and 64 on channel 1** --
  over ten octaves, not the default 2.
- The bend value **alternates** every ~34 ms (median gap 34.0 ms, max 68.0 ms), swinging
  between roughly **-57 and +36 semitones** on ch0: 4479, 10496, 4351, 10624, 4223, 10752,
  ... continuously, ~15 Hz.

So the composer is not writing a vibrato. They are feeding the synth a large-amplitude
pitch **square wave** and relying on the driver's own ramp to integrate it into a smooth
oscillation. On the reference this renders as a clean sinusoidal wobble of moderate depth,
because a ramp heading toward a target ~13-22 ms away, re-aimed in the opposite direction
every 34 ms, never arrives at either extreme.

Our implementation latches each bend value on receipt, so it jumps the full excursion 30
times a second. Three consequences, all measured elsewhere in this entry:

1. The musical gesture is destroyed -- an abrupt alternation instead of a wave.
2. Instantaneous large jumps in the phase increment splatter broadband energy; the
   +11.1 dB fast-bend-window excess near Nyquist (addendum 1) is this.
3. **The clamp becomes reachable when it should not be.** A -57 semitone request is
   -6840 cents, past `CentsToRatio`'s confirmed +-4800 clamp, so we clamp to -48
   semitones -- we do not even jump to the requested pitch. If the original ramps, its
   instantaneous value may never approach +-4800 here at all, making the clamp inert for
   this content while it is a constant distortion for us.

**This is the strongest argument for implementing the ramp.** It is not a fidelity
refinement on this material; content exists that is unreproducible without it, and no
amount of tuning the clamp, the interpolator or the envelope can substitute.

**Method note worth keeping:** this was only visible by reading the MIDI's own events.
Comparing our render against the reference showed *that* they differed; only the event list
showed that the reference's smooth output is the *intended* result of a deliberately
un-smooth input. When a divergence is hard to characterise, check what the score actually
asks for before theorising about the engine.

## 20. EG2 (the pitch envelope) -- RESOLVED 2026-07-26, implemented and shipped

> **RESOLVED. EG2->PITCH is now applied.** `src/engine/voice.c` gains a full EG2
> stage machine (`voice_step_eg2`), configured at note-on alongside EG1,
> released on BOTH note-off paths, and summed into `voice_update_pitch`'s cents
> total where the `+ 0 /* + EG2 (step 3) */` placeholder used to sit.
>
> **Segment shape: LINEAR IN CENTS, not exponential.** EG1's segments are
> exponential in amplitude because that is linear in dB -- the unit its
> destination lives in. EG2's destination is PITCH, whose unit is cents, so the
> same convention makes its segments linear in the envelope level. Implemented
> first with EG1's exponential coefficient, which produced a visibly "eased"
> sweep where the reference's is straight; the user spotted it by eye on probe
> 34's spectrogram. Switching to linear is worth **-2.73 dB on 26_other_gains
> and -2.59 dB on 34_sfx_bank_identity**. Kept overridable as
> `EG2_LINEAR_SEGMENTS` (default 1).
>
> **Release is NOT rate-clamped.** SPEC.md Part 7 records that the choke/steal
> routine `0x19aa4` "shares only the pitch-EG release call" with ordinary
> note-off `0x19a2c` -- the 70 ms clamp applies to the amplitude segment
> specifically. So `start_release`'s `fast` flag deliberately does not touch
> EG2.
>
> **Cadence is a documented choice, not recovered.** EG2 steps once per
> modulation sub-chunk (`EG2_BLOCK_FRAMES` = 64 frames, ~2.9 ms) rather than
> per sample, because its only consumer is the pitch sum, which is itself
> recomputed at exactly that cadence. SPEC.md S6.6 marks the envelope
> generator's own cadence `[O]` and warns against inferring it from the mixer's
> `ramp_period`, so this is an implementation decision.
>
> **Measured, corpus-wide, isolated (only change since the prior baseline):**
>
> | probe | before | after |
> |---|---|---|
> | 34_sfx_bank_identity | -2.77 | **-5.58** |
> | 26_other_gains | -10.69 | **-13.65** |
> | 01_programs | -5.35 | **-5.90** |
> | 15_banks | -9.42 | **-9.61** |
> | corpus mean | -9.517 | **-9.721** |
> | probes regressed | -- | **0** |
>
> `01_programs` improving confirms this is **not confined to SFX** -- melodic
> instruments author EG2->PITCH connections too, which the original entry
> flagged as unknown.
>
> Acceptance test met: probe 34 section B now shows the reference's curved
> glide on banks 2, 3 and 8, with banks 0/1/4/5/6/7/9 unchanged.
>
> The original entry follows as history.

## 20 (original). EG2 (the pitch envelope) is parsed and then discarded -- `[A]` in SPEC.md, never applied

**Path:** `SPEC.md` §2.4.3 "Source = 5 (EG2)" vs. `src/engine/voice.c`
`voice_update_pitch`.

SPEC.md documents the connection as recovered from disassembly:

| usSource | usDestination | action | VMA |
|---|---|---|---|
| 5 (EG2) | 0x0003 (PITCH) | high word of `lScale` -> WORD wave+0x1e | `[A:0x15838]` |

§2.4.4 confirms the dispatch is real and specific: `usSource==5` to any
destination *other* than PITCH is ignored, so PITCH is the one EG2 modulation
this driver implements -- and it does implement it.

`src/engine/dls.h` parses it correctly into `Artic.eg2_to_pitch_cents`, alongside
`eg2_attack_tc` / `eg2_decay_tc` / `eg2_sustain_permille` / `eg2_release_tc`.
`voice.c`'s `voice_update_pitch` then sums:

```c
int32_t mod_cents = synth_pitch_bend_cents(v->channel)
                  + voice_lfo_cents(v)
                  + 0 /* + EG2 (step 3) */;
```

The EG2 term is a literal zero. **Every pitch envelope in `gm.dls` is silently
dropped.**

### How it was found

Found by ear on real content, then localised with a purpose-built probe: the
user reported that our render of a hand-cut excerpt (first 14 s of bank 2
program 125, `Car-Stop`) sounded higher-pitched than the reference. Automatic
pitch metrics on that noise-like, loop-based SFX were worthless -- three
separate measurements gave "+8 semitones", "patch-specific centroid offsets",
and "exact match", each retracted; it has no stable fundamental to track, and
one metric was silently pinned at the bottom of its own search range.

`probes/34_sfx_bank_identity.mid` settled it by sweeping program 125 across all
ten banks it exists in, then comparing spectrograms rather than scalars. The
reference shows a clear **curved pitch glide** on banks 2, 3 and 8; ours shows a
**flat horizontal line**. The glide is the missing EG2.

### The correlation is exact

| bank | patch | EG2->PITCH | EG2 attack/decay tc | probe 34 |
|---|---|---|---|---|
| 2 | Car-Stop | **-1000 ct** | -8722 / -677 | **glide missing** |
| 3 | Car-Pass | **+530 ct** | -- / -503 | **glide missing** |
| 8 | Starship | **-268 ct** | -- / +1868 | **glide missing** |
| 5 | Siren | +94 ct | -32768 / -32768 (sentinel) | matches |
| 9 | Burst Noise | +1200 ct | -32768 / -32768 (sentinel) | matches |
| 7 | Jetplane | +1200 ct | +5271 (~21 s attack) | matches |
| 0, 1, 4, 6 | Helicopter, Car-Engine, Car-Crash, Train | none | -- | matches |

Every patch that differs has an EG2->PITCH connection with a real,
non-sentinel envelope time. Every patch with a sentinel time (instant, so no
audible glide) or no connection at all matches. Bank 7's attack is ~21 s, so a
2 s probe note moves only a fraction of its 1200 cents -- consistent with it
appearing to match at this note length.

### Why this matters beyond SFX

The affected patches here are sound effects, but the mechanism is general: any
`gm.dls` instrument authoring an EG2->PITCH connection gets a pitch envelope
we do not render. A corpus-wide count of `(usSource=5, usDestination=0x0003)`
connections should be run before estimating the blast radius -- this entry does
not claim it is confined to program 125.

### What an implementer needs

- Depth: `Artic.eg2_to_pitch_cents`, already parsed (high word of `lScale`,
  per `[A:0x15838]`).
- Envelope: `eg2_attack_tc` / `eg2_decay_tc` / `eg2_sustain_permille` /
  `eg2_release_tc`, already parsed, with the same timecent->duration
  conversion EG1 uses (§3.4.1, confirmed `[A]`).
- Application point: the `+ 0` slot in `voice_update_pitch`, which already
  runs per sub-chunk, so an EG2 value updated on the same cadence as the LFO
  will land correctly.
- **Caution:** SPEC.md S6.6 warns the envelope generator's own update cadence
  is `[O]` and must not be inferred from the mixer's `ramp_period`. Treat the
  EG2 update rate as a free parameter, and note that SPEC_GAPS #8 (our
  envelope model) is still open -- an EG2 built on the existing EG1 machinery
  inherits whatever is wrong there.
- Acceptance test: `probes/34_sfx_bank_identity.mid` / `probe-results/34.flac`,
  section B. Banks 2, 3 and 8 must show a curved glide matching the reference;
  banks 0, 1, 4, 5, 6, 7, 9 must not change.

### Method note

The scalar metrics actively misled here, three times running. What worked was
(a) sweeping a controlled probe across every candidate rather than comparing
two, (b) looking at a spectrogram instead of computing a number, and (c)
checking the finding against `gm.dls`'s own connection data, which turned a
visual impression into an exact correlation.

## 21. Ordinary-note-off release is too short against isolate references -- fitted floor SHIPPED, delayed-onset shape genuinely open

**Path:** `SPEC.md` S5.6/S3.8.2 (the only documented minimum-release
mechanism -- the rate clamp at `0x19834`, dividing by literal `0x46`/70 --
is reachable only from the fast-release/choke path `0x19aa4`, never from
ordinary note-off `0x19a2c`) vs. this project's own direct measurement
against hand-cut excerpts.

**What I did:** added `RELEASE_FLOOR_S = 0.060` in `src/engine/voice.c`,
flooring the ordinary-note-off release SEGMENT DURATION (in seconds, before
conversion to a per-sample coefficient) at 60 ms. Gated `!fast`, so it never
touches the fast-release/choke path or `RELEASE_RATE_MULT` (left at 1.0).
Because SPEC.md's own rate-clamp mechanism cannot reach this path by its own
account, this floor has no cited disassembly counterpart and is `[F:fitted]`,
not `[A]`.

**Corpus sweep** (spectral-residual analysis, `compare_spectral_22050.overall_db`,
32 graded probes): 20 ms -9.7388, 40 ms
-9.7569, **60 ms -9.7693**, 90 ms -9.6624, against the no-floor baseline
-9.7206. 90 ms's apparent mean was contaminated by an alignment flip on
`31_tune_clamp_bend_sine` (a confirmed mis-lock, not a real regression) and
several probes had already turned around (started re-worsening) by that
point; 60 ms is the last point where every probe that moved was still
improving. Direct isolate measurement (reference time-to--20dB/-40dB after
note-off ~38/44 ms vs. our pre-fit 3.5/4.6 ms) independently corroborates a
value in the same 6-11x-stretch band. Full detail, including the isolate
measurement's own caveats (a confirmed piecewise capture-lag drift, and a
low-register resolution floor), is in `FITTED.md` Entry 10.

**Confirmed not to reproduce SPEC_GAPS.md #15's prior failure mode:**
`field/corridor.mid` renders 1,159,168 frames untruncated at every floor
tested; `probes/04_envelope.mid` is byte-identical (`cmp`) up to a 40 ms
floor, since its own authored release (990 ms) sits far above any floor
tested.

**Genuinely open, flagged as a hypothesis only, not established:** the
reference isolate reaches -20 dB at ~38 ms but -40 dB at only ~44 ms -- a
~6 ms fall after a ~32 ms plateau. That shape is a DELAYED release onset,
not merely a slower release rate, and a pure duration floor on an
exponential release does not model it. A future RE pass should look at
SPEC.md Part 5 `+0x13c`, `0x194da`/`0x19644`, and `compute_release_target` /
the per-tick service routine `0x13054` for whether ordinary note-off's
release target is quantised to a service tick, which would explain a
plateau-then-fall shape; this was not tested against the corpus and the
isolate's own resolution limits (low-register fundamental period 8-53 ms)
mean the 38 ms plateau length is not pinned precisely enough to fit a
two-segment model from the data gathered this session alone.

## 22. CC121 (Reset All Controllers) does not reset Channel Volume -- RESOLVED 2026-07-26 by `[M: probe 37]`, SHIPPED

**Path:** `SPEC.md` S4.3's CC121 row, `[A:0x1351f]`: "re-schedules Volume=100,
Pan=64, Expression=127, Pitch Bend=8192, Modulation=0".

The Volume term is not observable in the reference. `probes/37_rac_volume_order.mid`
puts one sine note per case behind a CC121 arranged four ways, with CC7=40 and
CC7=100 controls (reference RMS -29.29 and -13.39 dB, 15.90 dB apart, a spread
this project's own render reproduces to 0.02 dB):

| case | reference | reads as |
|---|---|---|
| CC7=40 then CC121, same tick | -29.29 | 40 |
| CC121 then CC7=40, same tick | -29.29 | 40 |
| CC7=40, CC121 +50 ms | -29.29 | 40 |
| CC7=40, CC121 +500 ms | -29.29 | 40 |
| CC11=40 then CC121, same tick | -10.99 | reset (+18.30 dB over a surviving 40) |

All four volume cases are exact to 0.00 dB. The +500 ms case is what settles
it: this is **not** #14's queue-ordering question, because no same-timestamp
tie-break can leave a value standing against a reset half a second later. The
handler simply does not write Channel Volume -- either `0x1351f` re-schedules
the channel's *current* volume rather than the constant `100`, or the constant
was mis-attributed when the row was read. Expression is reset as documented,
so the exemption is Volume's alone and the guard belongs on that one field.

**What I did:** `CC121_RESETS_VOLUME` in `src/engine/synth.c` defaults to `0`;
`reset_all_channel_controllers` leaves `c->volume` alone and still resets
modulation, pitch bend, pan and expression.

**Corpus, before -> after** (`artifacts/score.py`, 50 items): `tests/warm-echo`
r 0.833 -> **0.994**, residual -25.25 -> **-34.68** dB; probe 37 itself 0.699 ->
**0.977**, -9.66 -> **-37.49**; mean r 0.905 -> **0.914**, mean residual -28.21
-> **-28.90**. 46 of 50 items are bit-identical -- every CC121 in the corpus
that is not preceded by a CC7 on its own channel renders unchanged. The two
others are the ones that authored CC7 before CC121 at tick 0: `field/flourish`
(warm-echo's parent) r 0.581 -> 0.610, `field/town` mean level error -2.00 ->
-0.91 dB. Both of those improve on every envelope/level metric and lose ~1.6 dB
of spectral residual (flourish -17.08 -> -15.57, town -21.26 -> -19.71):
restoring 11-15 channels to their authored balance redistributes spectral
weight onto this project's remaining per-instrument errors, which the
level-normalized residual then reads as worse. Per-frame level-error spread is
the metric that tracks the fix directly -- warm-echo's falls from sd 3.42 dB /
median |err| 2.02 dB to **sd 0.66 / 0.29**.

**Still open, small:** case G lands 1.7 dB below where a full restore to
Expression=127 extrapolates from the CC7 controls. Probe 37 has no measured
full-scale control to pin it, and `probes/07_pan_volume.mid` rules out level
compression (the reference-vs-render offset there is flat to 0.15 dB from CC7=16
to CC7=127). Worth one more case if Expression's reset value ever matters.
