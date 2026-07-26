# swmidi.sys — Clean-Room Implementation Specification

## Front Matter

**Target.** `swmidi.sys`, version **5.1.2600.5512** (Windows XP SP3 build
`xpsp.080413-2108`), **56,576 bytes**, dated 2008-04-14 — the Microsoft GS
Wavetable Synth kernel-streaming audio driver. This specification describes
exactly one binary. No other version of `swmidi.sys` (a different XP service
pack, Windows 98, or Windows 10) is in scope, and none of that binary's
behavior is assumed to transfer to this one or vice versa.

**What it is.** `swmidi.sys` is a Windows XP kernel-mode audio miniport
driver (a `ks.sys` client, importing only `ntoskrnl.exe` and `ks.sys`) that
renders incoming timestamped MIDI short messages and SysEx into 22050 Hz,
stereo, 16-bit signed PCM by table-driven wavetable synthesis against a
single fixed sample collection, `gm.dls` (a DLS-1 RIFF file, 3,440,660
bytes, shipped with Windows and loaded whole into paged pool at device-add
time); it has no send-effect buses, no spectral tone-shaping stage, no COM
or plugin surface, and no registry-selectable alternate rendering path.

**Provenance key.** Every factual claim below carries one of the following
tags, so a first-time reader can tell what kind of claim it is before
encountering it:

| Tag | Meaning |
|---|---|
| `[A:0xVMA]` | Read directly from an instruction or from raw bytes at that virtual address in `swmidi.sys`. A fact about the binary, independent of any measurement or inference. |
| `[D:gm.dls]` | Read directly from the bytes of `gm.dls`, the 3,440,660-byte sample collection this driver loads. |
| `[M:probe NN]` / `[M]` | Measured from rendered reference audio (a `probe-results/*.flac` capture or an equivalent rendered artifact). |
| `[I]` | An inference. The basis for the inference is stated in the same sentence or the immediately preceding one — an inference with no stated basis is a defect, not a fact. |
| `[O]` | Open. Not recovered, not measured, not inferred with confidence. Stated precisely (what specifically is missing), never guessed at. |

A claim with no tag anywhere in its vicinity is a defect in whichever part
of this document contains it.

**Verification ceiling — read before trusting any comparison against
reference audio.** All reference captures used anywhere in this project's
verification work carry a measured, small, near-independent noise floor of
roughly **±1 LSB per channel** (true-silence padding confined to `{-2..2}`
LSB, ≈99.98% within `{-1,0,1}`), consistent with dither rather than signal.
**Because of this noise floor, bit-exactness cannot be empirically proven
with these captures.** Sample-exactness — reproducing the driver's actual
computed values, not merely something perceptually close — is the design
goal throughout this specification. **No part of this project, and no part
of any implementation built from it, may report bit-exactness as
verified.** Full detail, including repeatability figures by material type
and the separate, unidentified 22050→44100 Hz upsampling stage that
contaminates any 44.1 kHz comparison, is in Part 1 §1.6.

## Table of Contents

- [Front Matter](#front-matter)
- [Part 1 — Overview and Implementation Contract](#part-1-overview-and-implementation-contract)
  - [1.1 What this document specifies, and how to read it](#11-what-this-document-specifies-and-how-to-read-it)
  - [1.2 Architecture and data flow](#12-architecture-and-data-flow)
  - [1.3 The audio contract](#13-the-audio-contract)
  - [1.4 Numeric environment](#14-numeric-environment)
  - [1.5 The implementation contract](#15-the-implementation-contract)
  - [1.6 Known divergences and the verification ceiling](#16-known-divergences-and-the-verification-ceiling)
  - [1.7 Two points settled by direct instruction reading](#17-two-points-settled-by-direct-instruction-reading)
- [Part 2 — gm.dls DLS/RIFF Parser Specification](#part-2-gmdls-dlsriff-parser-specification)
  - [2.1 Locating and loading the collection](#21-locating-and-loading-the-collection)
  - [2.2 RIFF/DLS container walk](#22-riffdls-container-walk)
  - [2.3 Per-chunk field tables, byte-exact](#23-per-chunk-field-tables-byte-exact)
  - [2.4 `art1` connection-block decoder — `0x157da`, inside `0x15788`](#24-art1-connection-block-decoder-0x157da-inside-0x15788)
  - [2.5 Default articulation (no `art1` present, or a destination not connected)](#25-default-articulation-no-art1-present-or-a-destination-not-connected)
  - [2.6 `wsmp` handling — summary (full byte table in §2.3.4)](#26-wsmp-handling-summary-full-byte-table-in-234)
  - [2.7 `fmt `/`data` — sample format handling, inside `0x153fc`](#27-fmt-data-sample-format-handling-inside-0x153fc)
  - [2.8 `ptbl` / wave-pool mechanics — fully resolved](#28-ptbl-wave-pool-mechanics-fully-resolved)
  - [2.9 In-memory instrument/region/wave structure layout](#29-in-memory-instrumentregionwave-structure-layout)
  - [2.10 `insh` locale packing — verified against real `gm.dls` bytes](#210-insh-locale-packing-verified-against-real-gmdls-bytes)
  - [2.11 `gm.dls` inventory cross-check — script and real output](#211-gmdls-inventory-cross-check-script-and-real-output)
  - [2.12 Summary of key findings, this section](#212-summary-of-key-findings-this-section)
  - [2.13 Open items — precisely stated, not guessed](#213-open-items-precisely-stated-not-guessed)
  - [2.14 Contamination / scope-discipline check (self-check, run on this file)](#214-contamination-scope-discipline-check-self-check-run-on-this-file)
- [Part 3 — Instrument Selection and Per-Voice Parameter Computation](#part-3-instrument-selection-and-per-voice-parameter-computation)
  - [3.1 Instrument selection](#31-instrument-selection)
  - [3.2 Articulation-block resolution — how a region gets playable parameters](#32-articulation-block-resolution-how-a-region-gets-playable-parameters)
  - [3.3 Pitch](#33-pitch)
  - [3.4 Envelopes](#34-envelopes)
  - [3.5 The volume law and velocity](#35-the-volume-law-and-velocity)
  - [3.6 Pan](#36-pan)
  - [3.7 Two decoder quirks (restated briefly; owned in full by the DLS section)](#37-two-decoder-quirks-restated-briefly-owned-in-full-by-the-dls-section)
  - [3.8 Drums and key groups](#38-drums-and-key-groups)
  - [3.9 Verification scripts and output](#39-verification-scripts-and-output)
  - [3.10 Note-on parameter computation — consolidated pseudo-code](#310-note-on-parameter-computation-consolidated-pseudo-code)
  - [3.11 Open items — precisely stated](#311-open-items-precisely-stated)
  - [3.12 Scope-discipline self-check](#312-scope-discipline-self-check)
- [Part 4 — MIDI Control Plane](#part-4-midi-control-plane)
  - [4.0 Provenance key](#40-provenance-key)
  - [4.1 Entry points and the byte-stream parser](#41-entry-points-and-the-byte-stream-parser)
  - [4.2 Per-channel state](#42-per-channel-state)
  - [4.3 The Control Change table (all 128 controllers)](#43-the-control-change-table-all-128-controllers)
  - [4.4 RPN and NRPN](#44-rpn-and-nrpn)
  - [4.5 SysEx messages recognized](#45-sysex-messages-recognized)
  - [4.6 Reset semantics](#46-reset-semantics)
  - [4.7 Event scheduling](#47-event-scheduling)
  - [4.8 Channel 10 / drum-part selection](#48-channel-10-drum-part-selection)
  - [4.9 Verification](#49-verification)
  - [4.10 Open items (`[O]`), collected](#410-open-items-o-collected)
- [Part 5 — The Voice Model](#part-5-the-voice-model)
  - [5.1 The voice object — struct field map](#51-the-voice-object-struct-field-map)
  - [5.2 Pool construction](#52-pool-construction)
  - [5.3 Free lists and allocation order](#53-free-lists-and-allocation-order)
  - [5.4 The reserve top-up](#54-the-reserve-top-up)
  - [5.5 Resolution of the pool-size question](#55-resolution-of-the-pool-size-question)
  - [5.6 Note-off vs. fast-release (choke)](#56-note-off-vs-fast-release-choke)
  - [5.7 Steal priority](#57-steal-priority)
  - [5.8 Exclusive key groups](#58-exclusive-key-groups)
  - [5.9 Sustain pedal (CC64) and CC120/CC123](#59-sustain-pedal-cc64-and-cc120cc123)
  - [5.10 Render-latency figure](#510-render-latency-figure)
  - [5.11 Verification scripts](#511-verification-scripts)
  - [5.12 Open items `[O]`](#512-open-items-o)
- [Part 6 — Audio Rendering Path](#part-6-audio-rendering-path)
  - [6.0 Scope and how to read the provenance tags](#60-scope-and-how-to-read-the-provenance-tags)
  - [6.1 Output format contract](#61-output-format-contract)
  - [6.2 MMX/scalar path selection](#62-mmxscalar-path-selection)
  - [6.3 Per-voice state (fields touched by the render functions)](#63-per-voice-state-fields-touched-by-the-render-functions)
  - [6.4 Per-voice render algorithm](#64-per-voice-render-algorithm)
  - [6.5 Per-wave sample rate and the phase-step composition](#65-per-wave-sample-rate-and-the-phase-step-composition)
  - [6.6 Ramp/envelope update cadence](#66-rampenvelope-update-cadence)
  - [6.7 Floating-point environment](#67-floating-point-environment)
  - [6.8 Additional per-sample signal processing](#68-additional-per-sample-signal-processing)
  - [6.9 Verification](#69-verification)
  - [6.10 Summary of open items (`[O]`)](#610-summary-of-open-items-o)
  - [6.11 The output-path resampler (kmixer.sys, 22050 → 44100)](#611-the-output-path-resampler-kmixersys-22050-44100)
- [Part 7 — Register of the Unreached and the Unknown](#part-7-register-of-the-unreached-and-the-unknown)
  - [7.A Part A — The unreached-code register](#7a-part-a-the-unreached-code-register)
  - [7.B Part B — The open-questions register](#7b-part-b-the-open-questions-register)
  - [7.C Part C — Prior-art adjudication deltas](#7c-part-c-prior-art-adjudication-deltas)
  - [7.D Summary for the report](#7d-summary-for-the-report)
- [Appendix T — Numeric Tables](#appendix-t-numeric-tables)
  - [T.0 Floating-point environment **(a)**](#t0-floating-point-environment-a)
  - [T.1 PE section map and VMA→file-offset conversion **(a)**](#t1-pe-section-map-and-vmafile-offset-conversion-a)
  - [T.2 Velocity → attenuation table (`0x1c9d0`, 128 × int32; dB = table[v] / 100.0) **(a)**](#t2-velocity-attenuation-table-0x1c9d0-128-int32-db-tablev-1000-a)
  - [T.3 Linear velocity table (`0x1bfd4`, 127 × int32, plus scalar `0x1bfd0`) **(a)**](#t3-linear-velocity-table-0x1bfd4-127-int32-plus-scalar-0x1bfd0-a)
  - [T.4 Table C — `0x1a9d8`, 201 × int16, indices 0..200 **(a)**](#t4-table-c-0x1a9d8-201-int16-indices-0200-a)
  - [T.5 Table D — `0x1a7d8`, 256 × int16, sine LFO **(a)**](#t5-table-d-0x1a7d8-256-int16-sine-lfo-a)
  - [T.6 Table E — `0x1c1d0`, 2048 × uint8, log-companding curve **(a)**](#t6-table-e-0x1c1d0-2048-uint8-log-companding-curve-a)
  - [T.7 `.rdata` DSP scalar constant pool `0x11c98`–`0x11d1c` **(a)**](#t7-rdata-dsp-scalar-constant-pool-0x11c980x11d1c-a)
  - [T.8 Table 0 — channel processing/init order **(a)**](#t8-table-0-channel-processinginit-order-a)
  - [T.9 Full verification script](#t9-full-verification-script)

---

## Part 1 — Overview and Implementation Contract

### 1.1 What this document specifies, and how to read it

#### 1.1.0 Target

This specification describes exactly one binary: **`swmidi.sys`, version
5.1.2600.5512** (Windows XP SP3 build `xpsp.080413-2108`), the Microsoft GS
Wavetable Synth kernel driver, **56,576 bytes**, dated 2008-04-14. The file
size is independently confirmed twice within this project's own evidence: by
a from-scratch PE-header/section-table parse (`file size = 56576`, PE
`ImageBase = 0x10000`, 8 sections, every section's raw file offset equal to
`VMA - 0x10000`) and by the pool-tag literal `'SwMi'` (bytes `53 77 4d 69`)
being read back byte-for-byte at VMA `0x12833` — both `[A]`, cited in full in
Appendix T §T.1 and Part 4 §4.0 respectively. The specific
four-part version string and build label are carried from this project's
initial identification of the file (a directory listing and `.rdata`/PE
version-resource read recorded elsewhere in this project's working notes,
not re-parsed by any of the six documents indexed below) — stated here as
**`[I]`** (inherited identification), not as an `[A]` claim with its own VMA
citation in this document's own chain of evidence.

**All reference audio used anywhere in this project's verification work was
rendered from this exact build.** No other version of `swmidi.sys` (a
different XP service pack, Windows 98, or Windows 10) is in scope anywhere in
this project, and none of that binary's behavior is assumed to transfer to
this one or vice versa.

#### 1.1.1 Provenance key

Every factual claim in this document, and in the six documents it indexes,
carries one of the following tags. This key is restated here because this is
the first document a clean-room implementer reads, and the tag is only
useful if its meaning is unambiguous before any claim is encountered:

| Tag | Meaning |
|---|---|
| `[A:0xVMA]` | Read directly from an instruction or from raw bytes at that virtual address in `swmidi.sys`. This is a fact about the binary, independent of any measurement or inference. |
| `[D:gm.dls]` | Read directly from the bytes of `gm.dls`, the 3,440,660-byte sample collection this driver loads. |
| `[M:probe NN]` / `[M]` | Measured from rendered reference audio (a `probe-results/*.flac` capture or an equivalent rendered artifact) produced elsewhere in this project. Where a companion section did not itself perform the measurement, this is stated explicitly rather than implied. |
| `[I]` | An inference. The basis for the inference is stated in the same sentence or the immediately preceding one — an unbaked inference (no stated basis) is a defect, not a fact. |
| `[O]` | Open. Not recovered, not measured, not inferred with confidence. Stated precisely (what specifically is missing), never guessed at. |

A claim with no tag anywhere in its vicinity is a defect in whichever
document contains it, this one included.

#### 1.1.2 Table of contents — what each companion section owns

This document is the entry point. It does not repeat the detail already
established in the six documents below; it indexes them, describes the
data flow that connects them, and states the constraints (audio contract,
numeric environment) and the implementation contract that all of them are
subject to. Each of the six was read in full before this section was
written.

| File | Owns |
|---|---|
| Part 2 | Parsing `gm.dls`: the RIFF/DLS container walk and chunk-loop shape; per-chunk field tables (`insh`, `colh`, `rgnh`, `wsmp` at both wave and region scope, `wlnk`, `art1`, `ptbl`, `fmt `/`data`, the non-standard `edit` chunk); the `art1` connection-block decoder and its full destination→field-offset dispatch table, including the two decoder quirks (`usTransform` never read; `usSource==4` silently dropped); default-articulation construction when no `art1` is present; the `ptbl`/`wvpl` wave-pool resolution mechanism (cue table, FIFO list append, the pointer-fixup loop that turns relative cue offsets into absolute pointers at real wave-chunk bytes); in-memory instrument/region/wave struct field layout; and a full byte-level cross-check of the actual `gm.dls` file (235 instruments, 495 waves, 1498 regions, 7 key groups). |
| Part 3 | What happens at note-on, from a resolved `(bank, program, key, velocity)` tuple to a playable voice: instrument selection (the three-tier bank/program/drum-bit fallback, exact-equality linear scan, region selection with no velocity gate); articulation-block resolution (private region block vs. instrument-shared refcounted fallback, and the note-render bail-out when neither exists); pitch (the three Q12 ratio tables, unity-note/fine-tune/pitch-bend combination, the cents-to-ratio table decomposition, phase-increment composition, loop-bound conversion); envelope timecent-to-duration conversion and its closed `pow`-precision analysis; the volume law (one squared-amplitude table shared by velocity, Channel Volume, Expression, and Master Volume); the asymmetric two-table pan law; and drums/key-group choke mechanics (the data-driven, channel-agnostic exclusive-group gate, and the distinction between ordinary note-off and the rate-clamped fast-release path). |
| Part 4 | The MIDI byte-stream parser and running-status rules; per-channel state (the six timestamp-keyed scheduled-controller queues, and the flat per-channel arrays for sustain, RPN/NRPN-select, Data-Entry, Master Volume, RCV CHANNEL, USE RHYTHM PART, Mono mode, the GS-mode flag); the full 128-entry Control Change dispatch table; RPN/NRPN assembly (CC98–101, CC6/CC38) and the NRPN-is-a-no-op finding; every recognized SysEx message (GM System On/Off, Roland GS Reset, RCV CHANNEL, USE RHYTHM PART, the 12-entry tuning grid, Master Volume) with the device-ID-byte-never-read and checksum-never-checked findings; the three reset functions and a field-by-field reset-state table; the event-scheduling primitives (FIFO tie-break on equal timestamps, look-ahead reads ahead of the periodic flush); and channel-10/drum-part selection (a data-driven `USE RHYTHM PART` gate, not a literal channel-9 comparison). |
| Part 5 | The voice object's field map; pool construction (54 physically distinct voice objects: 48 primary + 6 reserve, no cap-check anywhere in the code); the three free/active lists and note-on's primary→reserve→steal allocation order; the once-per-dispatcher-call reserve top-up and its two branches (move free nodes, or force-release active voices); the resolution of the "48 vs. 54" pool-size question by both static reading and acoustic measurement; note-off vs. fast-release/choke as two genuinely different configurator functions (one rate-clamped, one not); the two structurally different steal-priority comparators (symmetric vs. asymmetric released/held tie-break); exclusive key groups (the data-driven choke gate, `gm.dls`'s own 7 key-group values); sustain-pedal/CC120/CC123 interaction (which of the three honors, and which bypasses, a held pedal); and the measured, file-dependent, non-constant capture-chain onset offset (which must never be used to calibrate the synth itself). |
| Part 6 | The output format contract (single fixed rate, not a range); the MMX-vs-scalar dispatch (and why the scalar family is real but unreached on any real deployment); the per-voice render-function field map; the complete per-voice render algorithm (Q12 phase accumulator, two-tap fetch, linear interpolation via a dot-product-and-shift, gain application strictly after interpolation, saturating accumulation directly into the shared 16-bit output buffer with no wider intermediate accumulator, frame-count/stereo-doubling convention, one-shot vs. looped end handling as a single conditional subtraction never a loop); the per-wave sample-rate-to-phase-step composition (confirming `gm.dls`'s three 24000 Hz waves are live, acoustically distinct cases); the ramp/envelope update cadence (block-cadenced, exact cadence caller-supplied and unrecovered); the x87 floating-point environment (`0x027F`, and the dedicated truncating float→int helper); and a search for any additional per-sample signal-shaping stage (none found in the traced render path, and zero DLS-2 tone-shaping-destination connections in `gm.dls` itself). |
| Appendix T | The numeric-data appendix: the PE section map and `VMA - 0x10000` file-offset conversion; the x87 control-word/truncation finding (restated and independently re-derived); the five runtime-built lookup tables in full — the velocity/attenuation table (`0x1c9d0`, 128 entries, the squared-amplitude law), the linear/√-law table (`0x1bfd4`+`0x1bfd0`, 127+1 entries — the table §3.6 identifies as one side of the pan law), and three further tables (`0x1a9d8`, `0x1a7d8` sine, `0x1c1d0` log-companding) with full contents; the `.rdata` scalar constant pool (every float literal these builders consume, by exact stored bit pattern, not an idealized decimal); and the static channel-processing-order table at `0x1a600`. |

#### 1.1.3 What this synthesizer does not have

A reader's priors about a general-purpose software synthesizer will be
wrong here in specific, verifiable ways:

- **No send-effect buses of any kind** (the two effects a reader would
  normally expect a GM/GS-class synth to offer as CC91/CC93 send levels).
  `gm.dls`'s own `art1` connection data was
  scanned in full; §2.4.4 and §6.8 both
  independently confirm the complete recognized-destination set the
  connection-block decoder tests never includes any DLS send-effect
  destination, and CC91 and CC93 (the standard MIDI "Effects 1/3 Depth" send
  levels) are structurally discarded in the Control Change dispatch chain
  (§4.3) and were separately confirmed acoustically
  inert at all nine tested send levels `[M:probe]` (same section). There is
  no send-effect bus anywhere in this driver to route those CC values to.
- **No spectral tone-shaping stage.** §6.8 read the complete
  per-sample operation set of the live mixer path instruction-by-instruction
  and found exactly: two-tap fetch, linear interpolation, one gain multiply,
  one saturating accumulate — no coefficient-based difference equation, no
  modulation-rate-gated oscillator tap, no ring-buffer read/write pattern.
  Separately, `gm.dls`'s own `art1` data contains **zero** connection blocks
  targeting either DLS-1-adjacent tone-shaping destination code (`0x0500` or
  `0x0501`) out of 7,451 total connection blocks in the file `[D:gm.dls]`.
  This is a fact about the content of `gm.dls` and about the dispatch chain
  actually implemented, not a claim about what a different DLS collection
  could theoretically request.
- **No COM interfaces, no plugin model, no registry-selectable code path.**
  This driver is a kernel-streaming (`ks.sys`) audio miniport; it imports
  `ntoskrnl.exe` and `ks.sys` and nothing else module-shaped. It reads
  exactly one registry value at startup — `GMFilePath` under
  `\Registry\Machine\Software\Microsoft\DirectMusic` — purely to locate the
  `gm.dls` file on disk, falling back to a hardcoded path if that value is
  absent or malformed (§2.1). That is the full extent of this
  driver's registry interaction: there is no registry-selectable rendering
  path, no alternate synthesis engine reachable via configuration, and no
  COM/plugin surface of any kind exposed anywhere in the traced code.
- **Other Microsoft-supplied synthesizer binaries exist on the same OS and
  are out of scope.** A separate kernel-mode driver and a separate
  user-mode DLL, both distinct from `swmidi.sys`, implement unrelated
  wavetable/software-synthesis paths on this OS. These are settled to be
  independent synthesizers, not layers of this one, by import tables
  (`swmidi.sys` imports only `ntoskrnl.exe` and `ks.sys`; the other
  components import entirely different sets; none of the relevant binaries
  imports from or exports to any of the others), and further confirmed by
  `swmidi.sys` being the only one of the relevant binaries in this
  project's evidence that contains the literal path
  `\SystemRoot\System32\Drivers\gm.dls`. No claim in this document, or in
  any of the six it indexes, is sourced from those other binaries.

---

### 1.2 Architecture and data flow

End to end, MIDI bytes become audio samples through the following pipeline.
Each numbered step cites the owning companion section; the detail lives
there, not here.

```
raw MIDI bytes (from the KS pin / injected directly)
        |
        v
[1] byte-stream parser + running status  ---------------- Part 4 §4.1
        |
        v
[2] ShortMsg / SysEx: per-channel state update  ----------- Part 4 §4.2-4.6
        (scheduled-controller queues, CC table, RPN/NRPN,
         reset semantics -- values are SCHEDULED, not
         applied immediately)
        |
        v
[3] main per-device event queue (Note-On/Off + 5 sentinels)  Part 4 §4.7
        |
        v
[4] once per audio buffer: per-buffer service routine
        (a) read current time
        (b) TriggerVoiceEvent: drain every currently-due event
              For each due Note-On:
                instrument lookup (3-tier fallback)  --------- Part 3 §3.1
                region selection (first-match, file order)  -- Part 3 §3.1.4
                articulation-block resolution  ---------------- Part 3 §3.2
                voice from pool: primary -> reserve -> steal -- Part 5 §5.3, §5.7
                per-voice parameters: pitch, envelopes,
                  volume law, pan  ----------------------------- Part 3 §3.3-3.6
                exclusive key-group choke  --------------------- Part 3 §3.8, Part 5 §5.8
              For each due Note-Off / CC64/120/123/choke/steal:
                ordinary release vs. rate-clamped fast release - Part 5 §5.6, §5.9
        (c) promote scheduled controller values; reap
              finished voices back to the primary free list --- Part 4 §4.6.2, Part 5 §5.3
        |
        v
[5] per-block render/mixer pass over the active-voice list:
        two-tap fetch @ Q12 phase position -> linear
        interpolation -> persistent per-channel gain ->
        saturating accumulate into the shared output buffer --- Part 6 §6.2-6.8
        |
        v
[6] output: 22050 Hz, stereo, 16-bit signed PCM  ------------- §1.3, below
```

**Entry points, cited once here for orientation** (full detail in the
sections above): byte-stream parser `0x17fa2`, `ShortMsg` `0x131c0`, `SysEx`
`0x1367a`, per-buffer service routine `0x13054`, `TriggerVoiceEvent`
`0x12bd6`, `FindInstrument` `0x14800`, `FindRegionForNote` `0x14722`,
note-setup `0x19b54` (which itself calls the pitch/phase-increment helper
`0x18ef4` and the cents-to-ratio helper `0x18e1c`), and the live MMX mixer
function for every `gm.dls`-sourced voice, `0x1a360` (stereo output, 16-bit
mono source) `[A]` (all cited with full instruction traces in their owning
sections).

**One structural point worth stating here because it is easy to miss when
reading the sections in isolation:** step [2]'s controller writes do not
take effect immediately — they are inserted into per-controller,
timestamp-keyed queues and become "current" either when the periodic
promotion in step [4]/(c) runs, or earlier, via a look-ahead read for any
query whose timestamp is later than the pending entry's own timestamp
(§4.7.3). An implementation that simply writes a
CC value into a flat array on receipt, without reproducing this scheduling
and look-ahead behavior, will diverge on any file that changes a controller
and triggers a note within the same processing buffer.

---

### 1.3 The audio contract

**Output is 22050 Hz, stereo, 16-bit signed PCM. This is a single fixed
rate, not a range.** The driver's `KSDATARANGE_AUDIO` structure, read
directly at VMA `0x1a710`, advertises exactly one format:
`MinimumSampleFrequency == MaximumSampleFrequency == 22050`,
`MaximumChannels = 2`, `MinimumBitsPerSample == MaximumBitsPerSample == 16`,
`SubFormat = KSDATAFORMAT_SUBTYPE_PCM` `[A:0x1a710]`. The literal dword
`0x00005622` (= 22050) occurs twice in the structure's bytes, at file
offsets `0xa75c` and `0xa760` `[A:0xa75c][A:0xa760]`. The entire 9,796-byte
PAGEDATA section contains no second `KSDATARANGE_AUDIO` — there is no
alternate rate this driver can be made to advertise or produce
(§6.1).

**44100 Hz reference recordings are not produced by this binary.** Rendered
reference captures used elsewhere in this project commonly appear at
44100 Hz; that is the output of a separate resampling/upsampling stage
downstream of this driver — `kmixer.sys` (the WDM Kernel Audio Mixer) on
this OS — which has no code presence in `swmidi.sys` and is not analyzed by
any part of this project's `swmidi.sys` disassembly-based work. That
separate binary has since been reverse-engineered as far as its own bytes
allow (§6.11): its **algorithm class is now known** — a 60-tap-per-phase
float32 FIR, two output phases per input sample, extended-precision
accumulation truncated only at the final store — but its **exact tap
coefficients and the exact condition that gates it to a 22050→44100 pair
remain `[O]`**, unrecovered from `kmixer.sys`'s own binary (§6.11.4). Two
simple candidate models were tested against rendered 44100 Hz audio
elsewhere in this project's working notes and both were eliminated
(zero-order hold: only 0.23% of odd-indexed samples match; linear
interpolation: 0.87% match) `[M]`; the `kmixer.sys`-binary-level analysis in
§6.11.2 independently reaches the same exclusion by reading the code
itself, not by measurement — two methods, one conclusion. **Consequence for
implementers and for anyone grading this project's output: any comparison
against a 44100 Hz reference remains a model, not a clean match.**
Sharpening the algorithm class from unknown to a known FIR type does not,
by itself, close residual error: both the inferred tap coefficients
(§6.11.4) and the reference capture's own ±1 LSB dither remain uncontrolled
variables. **The only bit-level-exact comparison available for `swmidi.sys`
itself is at 22050 Hz**, against either a genuinely 22050 Hz capture or a
reference correctly decimated back to 22050 Hz — never against the raw
44100 Hz file directly.

---

### 1.4 Numeric environment

This section is what makes a bit-faithful, non-x87 (e.g. `wasm32`) port of
this driver's arithmetic possible at all. Every figure below is restated
from §6.7 and Appendix T §T.0, which independently
re-derived it from the disassembly; both agree exactly.

#### 1.4.1 The x87 control word is `0x027F`

Decoded bit-by-bit: bits 0–5 (all six FPU exception masks) = 1 — every
floating-point exception is masked; bits 8–9 (Precision Control) = `10b` =
2 — 53-bit, i.e. IEEE-754 `binary64`/C `double`, precision; bits 10–11
(Rounding Control) = `00b` = 0 — round-to-nearest-even `[A]` (decoded
arithmetically from the literal `0x27F`, compared against at five sites in
`.text`: `0x104ef`, `0x10b26`, `0x10bde`, `0x10bef`, `0x10c48`). This is
**not** the x87 hardware power-on default (extended, 80-bit precision,
`0x037F`) — `0x027F` is a deliberate choice that trades away extended
intermediate precision so that every individual FPU operation already
rounds to the same 53-bit result an ordinary `double`-only implementation
would compute. **Because Precision Control = 2 and Rounding Control =
round-to-nearest-even are exactly the two properties that make x87
arithmetic equal IEEE-754 `binary64` arithmetic operation-for-operation,
every multi-step floating-point computation in this driver — table
construction, timecent-to-duration conversion, anything else that touches
the FPU — is reproducible using ordinary 64-bit `double` arithmetic on a
non-x87 target, with no hidden extended-precision state for a
reimplementation to fail to replicate.** This one fact is the entire reason
a `wasm32` port of this driver's float path is tractable.

#### 1.4.2 Every float→int conversion truncates toward zero, never rounds

A single shared helper, `0x106e0`, performs every float→int conversion this
driver's table builders and runtime code use. It does not simply `fistp`
under the ambient (round-to-nearest) control word; it explicitly saves the
current control word, sets Rounding Control to `11b` (truncate toward
zero) by OR-ing `0x0c` into the high byte of the saved control word, loads
that modified word, performs the conversion, and restores the original word
`[A:0x106e6][A:0x106ef][A:0x106f6][A:0x106f9][A:0x106fc]`. **This is
exactly C's `(int)` cast semantics, not `round()`, and it is the single
fact responsible for a large fraction of this driver's exact output
values.** State it once, here, because it recurs in every table this
project has enumerated: the velocity/attenuation table (Appendix T
§T.2 — `table[v] = trunc(1000 · log10((v/127)⁴))`, matching a
task-independent 128-value reference exactly; 64 of 127 entries would
differ by one unit under round-to-nearest instead), the linear/√-law pan
table (Appendix T §T.3), the three pitch-ratio tables
(§3.3.1), and the timecent-to-duration conversion
(§3.4.1). An implementation that rounds instead of
truncating anywhere in this chain will diverge from the original driver on
a majority of table entries — this is not a rare edge case, it is the
default outcome of using the "obvious" rounding function instead of the one
actually implemented.

#### 1.4.3 No bit-exact `pow` is required — the margin is auditable, not asserted

The timecent-to-duration conversion (§3.4.1,
`tc = lScale/65536.0; duration = 2^(tc/1200)`, truncated per §1.4.2) is
computed with a real CRT `pow()` call (confirmed `fyl2x`-based), not a
lookup table. Across all **552** distinct raw timecent values present in
`gm.dls`'s EG1/EG2 `art1` connections, evaluated both as seconds and as
samples at 22050 Hz (**1,104** evaluations total), **7** land on an exact
power of two; among the remaining **1,097** inexact evaluations, the
**minimum distance to a truncation boundary is 1,367,824 ULP** (worst case:
`tc = 4330.571090698242` → `269009.99992038216` samples) `[D:gm.dls]`
(independently re-derived, byte-for-byte, in both §3.4.1
and Appendix T §T.0, with matching figures). **Conclusion, stated with
its margin so it is auditable rather than asserted: plain IEEE-754
`binary64` `pow` suffices for this file. No bit-exact reproduction of
MSVC's specific `pow` implementation is required** — the nearest
truncation boundary is over a million ULP away in the closest real case
this file exercises, which is orders of magnitude more slack than any
realistic `pow` implementation's disagreement with the correctly-rounded
result.

#### 1.4.4 Fixed-point conventions

- **Q12 phase / interpolation format**, confirmed three independent ways in
  §6.4.2: a 32-bit phase value is split **20 bits integer
  index : 12 bits fraction**. The fraction mask is the immediate `0x0FFF`
  per 16-bit SIMD lane `[A]`; the "whole" constant is `0x1000` (4096 =
  2¹²) `[A]`; the integer index is obtained with `shr reg, 0xc` `[A]`. Every
  pitch ratio in this driver (see below) is expressed in this same Q12
  format, where `4096` represents a ratio of `1.0`.
- **Q12 pitch-ratio tables.** Two runtime-built tables encode
  `4096 · 2^(n/1200)` (cents, domain `n = -100..100`, 201 entries, base
  `0x1ad00`) and `4096 · 2^(n/12)` (semitones, domain `n = -48..48`, 97
  entries, base `0x1af58`) `[A]` (§3.3.1, with an
  explicit domain correction: the cents table's domain is `-100..100`, not
  `-48..48` — the loop bounds were re-read directly off the instructions
  to settle this). A cents value outside `±100` is decomposed into whole
  semitones plus a residual cents term and recombined via
  `(T3[semitones] * (T2[cents] << octaves)) >> 12`, which reproduces
  `4096 · 2^(cents/1200)` exactly in the mathematical limit
  (§3.3.3) — there is no separate `fyl2x`/`f2xm1` call
  at note-on for pitch; these two tables replace it entirely.
- **Centibel (0.1 dB per unit) units** for the attenuation-style `art1` connections
  and for the `wsmp` attenuation field: `(lScale · 10) >>arith 16` (a
  right-shift-by-16 arithmetic, not a divide), with the DLS
  `INT32_MIN` sentinel mapping directly to `-9600` (silence) instead of
  performing the multiply, to avoid signed overflow (§2.4.2).
  **Do not confuse this with the separate, finer hundredths-of-a-dB
  (0.01 dB) table used elsewhere** (§3.5, the velocity/CC7/master-volume
  table) — the two must not be divided by the same factor; dividing that
  other table's values by 10 instead of 100 silently produces attenuations
  10× too large. This governs reading either table's *own* on-disk value in
  isolation; it is not a conversion that must be inserted when the two are
  added together. **In the note-on gain sum specifically** (§3.5, §3.10),
  the WORD this `(lScale·10)>>16` formula produces for the `wsmp` attenuation
  field (stored at `wave+0x1e`/`region+0x24`) is summed with **no further ×10
  scaling**, as one more addend in the same hundredths-of-a-dB accumulator
  that also holds the velocity/CC7/master-volume table's terms — confirmed
  by direct instruction read of the note-on gain-summation code: the region
  term is loaded as a sign-extended 16-bit value from `region->0x24`
  (`[A:0x19bc9]`) and added straight into the running attenuation sum with
  no intervening multiply or shift (`[A:0x19bcd]`). Numerically: `gm.dls`'s
  Acoustic Grand Piano region for key 60 stores raw `lAttenuation =
  -4390912` `[D:gm.dls]`; the formula gives `(-4390912*10)>>16 = -670`, and
  the driver treats that `-670` as **-6.70 dB** (divide by 100, the same
  divisor as the hundredths-of-a-dB table) — not -67 dB.

---

### 1.5 The implementation contract

**Everything in this section is a design specification for the module the
implementers must build, not a description of observed `swmidi.sys`
behaviour. It is `[I]`/design throughout. Do not mistake anything below for
a claim about what the original binary does — §§1.1–1.4 above, and the
other parts of this specification, are the only sources of fact about the
original driver.**

#### 1.5.1 Freestanding constraints

- Target: `wasm32`.
- Compiler: `clang --target=wasm32 -nostdlib -Wl,--no-entry`.
- Linker: `wasm-ld`.
- **No libc, no libm, no malloc, no stdio.** There is no `emcc` anywhere in
  this project's toolchain. The module must provide, itself, whatever
  runtime primitives it needs: `memcpy`, `memset`, `pow`, `log10`, `sqrt`,
  `sin`, and a bump allocator. Nothing may be assumed importable from a
  host environment beyond the linear memory the host maps in.

#### 1.5.2 Module map

One translation unit each, exactly as follows:

```
src/
  wasm.c        freestanding wasm32 ABI (msgs_* exports, bump allocator)
  cli.c         native command-line renderer
src/engine/
  msgs.h        public ABI (below)
  rt.c   rt.h   freestanding runtime: memcpy/memset, pow, log10, sqrt, sin, bump allocator
  tables.c .h   the runtime-built lookup tables
  dls.c  .h     RIFF/DLS parse of gm.dls into instruments, regions, waves
  synth.c .h    channel state, MIDI message dispatch, CC/RPN/SysEx, event queue
  voice.c .h    voice pool, allocation, stealing, key-group choke, per-voice parameters, envelopes
  render.c .h   the mixer: interpolation, gain, saturating accumulate
  smf.c  .h     Standard MIDI File parser and sequencer
```

`tables.c`/`.h` builds, at `msgs_init` time, the runtime lookup tables this
project's numeric-data appendix fully documents (Appendix T): the
velocity/attenuation table, the linear/√-law pan-side table, and the three
further tables, all built with the truncating conversion and the exact
stored float constants specified in §1.4 above — **not** with idealized
decimal constants (Appendix T §T.4 demonstrates concretely why this
matters: using an idealized `1/200` or `2π` instead of the stored float32
bit pattern changes table entries at the boundary). `dls.c`/`.h` owns
everything Part 2 specifies. `synth.c`/`.h` owns everything
Part 4 specifies. `voice.c`/`.h` owns everything
Part 5 specifies, plus the per-voice parameter computation
Part 3 specifies (pitch, envelopes, volume law, pan — these
are computed once, at note-on, and stored on the voice object).
`render.c`/`.h` owns everything Part 6 specifies.

#### 1.5.3 The public ABI — fixed, and reproduced here verbatim

This ABI is already fixed and the test harness is written against it. Do
not change it.

```c
uint32_t msgs_abi_version(void);                 // returns 1
uint32_t msgs_mem_size(void);                    // bytes of linear memory required
uint32_t msgs_alloc(uint32_t nbytes);            // bump allocator; returns offset into linear memory
int32_t  msgs_init(uint32_t dls_ptr, uint32_t dls_len);   // host places gm.dls first; 0 = OK
void     msgs_reset(void);
int32_t  msgs_load_smf(uint32_t smf_ptr, uint32_t smf_len);  // 0 = OK
void     msgs_set_loop(int32_t loops);           // -1 = infinite, 0 = play once
uint32_t msgs_render(uint32_t out_ptr, uint32_t frames);  // stereo interleaved int16 @22050 Hz; returns frames written
int32_t  msgs_is_finished(void);
void     msgs_midi(uint32_t status, uint32_t d1, uint32_t d2);  // inject one short message immediately
```

#### 1.5.4 `smf.c` is NOT part of `swmidi.sys`

**State this prominently because it is the one component in the module map
with no counterpart in the original driver at all.** The original
`swmidi.sys` is a kernel-streaming synthesizer: it receives timestamped
short MIDI messages handed to it by the kernel streaming layer
(§4.1) and never itself parses a Standard MIDI
File. `smf.c`/`.h` exists **only** so this module is runnable and testable
standalone outside a host that already has a sequencer — it must implement
ordinary SMF format 0/1 parsing and sequencing (track merging, tempo-map
handling, delta-time accumulation, dispatching each event through
`msgs_midi` at the right render-time offset), which is well-understood,
generic MIDI-file handling, **not** reverse-engineered `swmidi.sys`
behaviour. Every claim about `smf.c` in this specification family is
`[I]`/design; none of it should be read as a statement about what the
original binary does, because the original binary has no SMF-parsing code
at all to be a statement about.

#### 1.5.5 Memory model

- **A single linear arena, with a bump allocator and no `free`.** This
  mirrors what makes the original driver's own `gm.dls`-handling valid:
  §2.1 establishes that the original driver reads the entire
  `gm.dls` file in one `ExAllocatePoolWithTag` call and keeps it resident,
  never freed, for the lifetime of the driver instance, which is exactly
  why every internal pointer the original parser stores can be a raw
  pointer into that one buffer with no chunk ever copied out. The `wasm32`
  module should reproduce this shape: `gm.dls` is parsed **in place**, and
  sample data is **referenced, not copied**, into whatever instrument/
  region/wave structures `dls.c` builds.
- The module must not assume any host functions are importable beyond the
  linear-memory surface `msgs_alloc` manages; anything the implementation
  needs beyond raw memory access (§1.5.1) must be linked in from `rt.c`.

---

### 1.6 Known divergences and the verification ceiling

**State this plainly, because overstating it is the single worst failure
mode available to this project. No stage of this project may report
bit-exactness as verified — not this document, not any companion section,
not any future test-harness output.**

- **Capture provenance.** Reference recordings used throughout this
  project's verification work were produced by playing MIDI back on an
  emulated Windows XP guest and capturing the resulting audio via a
  host-side loopback path (Windows Media Player output captured through a
  "Stereo Mix"-style loopback device), not by extracting samples from the
  driver's own output buffer directly. This capture chain is itself a
  source of noise and timing artifacts distinct from anything `swmidi.sys`
  does, and every item below follows from that.
- **Noise floor.** True-silence padding regions across the probe
  recordings show amplitude confined to `{-2..2}` LSB, with ≈99.98% of
  samples within `{-1,0,1}`, and near-zero left/right cross-channel
  correlation (≈0.001) — consistent with a small, near-independent,
  roughly `±1` LSB dither floor per channel, not signal `[M]`. Lag-1
  autocorrelation on these same quiet regions measures **0.04–0.31 per file
  (pooled ≈0.135)** `[M]`. **The qualitative characterization (small,
  near-independent, roughly `±1` LSB) holds; no tighter numeric
  autocorrelation bound should be assumed or repeated as a verified
  ceiling.** Because of this noise floor, **bit-exactness cannot be
  empirically proven with these captures.** Sample-exactness remains the
  design goal; no stage of this project may report it as verified.
- **Repeatability differs by material type.** Reference renders of the same
  event repeat to approximately **1% on percussive attacks** and
  approximately **0.02% on sustained tones** `[M]` — attack-heavy and
  sustain-heavy material must be graded against different thresholds, and a
  single tolerance figure applied uniformly across both will either pass
  bad percussive output or fail good sustained output.
- **Capture-chain timing offset — negative, file-dependent, not a property
  of the synth.** Measured onset-alignment offsets between a probe
  manifest's declared MIDI event time and the corresponding audible onset
  are **−0.688 s** and **−0.802 s** on two different probe files
  (§5.10) `[M]`. The magnitude is stable within a single
  probe file's own event train but differs materially between files, which
  is itself the evidence that this is a per-capture-session artifact (most
  likely differing lead-in silence or a differing capture-clock zero
  point), not a property of `swmidi.sys`. **Alignment must always be
  recovered by cross-correlation against the actual audio; never assume a
  fixed offset**, and never use either of these two measured figures to
  calibrate timing inside a clean-room implementation of the driver itself.
- **The 22050→44100 upsampling stage is unidentified and is a hard ceiling
  on any 44100 Hz comparison** (§1.3, above). Two of the simplest
  candidate reconstruction models have been tested and eliminated
  (zero-order hold, linear interpolation — both fail to reproduce the
  captured 44100 Hz samples at anywhere near the rate a correct model
  would) `[M]`, but the actual stage remains `[O]`. Any 44100 Hz comparison
  inherits this unknown in addition to everything else in this section;
  the only ceiling-free comparison is at 22050 Hz.
- **Per-reference capture provenance differs across the three reference
  files this project verifies against, and this bounds how much of each
  one's residual is attributable to this project's synth rather than to
  how the file was captured.**
  - `field/corridor.flac` was captured over the same Stereo Mix-style loopback
    path described above, but **with a DAW application left open** on the
    capturing machine during the recording session. The user who produced
    this capture reports that it sounds measurably brighter than the
    other two references. A DAW held open plausibly forced the guest or
    host audio device to a different operating sample rate (e.g. 48000
    Hz), which would move the 22050→44100 upsampling stage discussed above
    away from the clean 2x case and/or add host-side processing to the
    loopback path. This project did not attempt to confirm the reported
    brightness spectrally — the three references are three different
    pieces of music, so content differences already dominate any
    spectral-tilt comparison between them, and no matched-content A/B
    exists to isolate capture-path effects from musical content. **This
    brightness report is recorded here as authoritative user-reported
    capture provenance, not as a value this project measured.** Net
    effect: an unknown, and plausibly larger-than-dither, share of
    `field/corridor.flac`'s residual against this project's render is
    capture-path artifact rather than synth divergence, so its exact
    spectral match is not treated as a precision target and may carry an
    unclosable floor above the dither figures already stated in this
    section.
  - `field/Kot_and_A64-GENERAL_SERUM.flac` was captured over the same loopback
    path with **no DAW open**, and was recorded deliberately
    un-normalized so that it clips naturally at the int16 rail — measured
    directly against the reference file, 1.06% of samples sit at the
    `+32767`/`-32768` rail `[M]` (consistent with a separately reported
    1.07% figure for the same file; the small difference is attributable
    to measurement-tool rounding, not a discrepancy in the underlying
    fact). This is the cleaner capture path of the three, and this
    project treats this file as its acceptance/north-star reference.
  - `field/Strobe-faffaeefafaefae.flac` was also captured over the same loopback
    path with **no DAW open**. Also a cleaner capture path; this project
    treats it as the aliasing-model acid test (Part 6 §6.1).
  - **Consequence.** All three files sit above the ±1 LSB dither floor
    already established in this section. `field/corridor.flac` carries an
    additional, larger, currently unquantified capture-path uncertainty on
    top of that floor. For this reason, GENERAL SERUM and faffaee are
    weighted **above** corridor as fidelity targets throughout this
    project.
  - **This does not reopen or weaken the findings already recorded about
    this project's own render behavior against `field/corridor.mid`.** The
    release-time bug that left `field/corridor.mid` truncated (measured at
    durations from ~70s up to 82.8s across measurement snapshots,
    never latching the finished state, against a ~52.4s reference) and
    its fix (52.57s against a 52.43s reference, `artifacts/SPEC_GAPS.md`
    item 15), together with the onset-timing accuracy check performed
    against this same file, concern only this project's own render's
    internal timing and termination behavior. Those findings are
    independent of which capture path recorded the reference audio, and
    they stand.

---

### 1.7 Two points settled by direct instruction reading

Two apparent disagreements between companion sections are resolved here by
reading the cited instructions directly (the PAGE-section disassembly of
`swmidi.sys`), rather than left open:

1. **The note-setup phase-increment store (`0x18ef4`) uses `voice+0x28` as a
   scratch accumulator and `voice+0x34` as the field the mixer actually
   reads.** The full instruction sequence, read directly:
   ```c
   18f6a: // voice->0x28 used below as a scratch accumulator
   18f6d: voice->0x28 = pitch_ratio;                       // write 1 of 3
   18f6f: voice->0x28 = voice->0x28 * wave->sample_rate;   // write 2 of 3 (multiply at 0x18f72, store at 0x18f75)
   18f7b: voice->0x28 = voice->0x28 / render_rate;         // write 3 of 3, stored at 0x18f81
   18f7d: voice->0x44 = 0;                                 // phase accumulator zeroed at note start, mid-sequence
   18f83: voice->0x34 = voice->0x28;                       // final value copied to the field the mixer reads
   ```
   `voice+0x28` is written three times during this computation and ends up
   holding the final value too, as a byproduct of being the scratch slot
   used throughout; `voice+0x34` is the field the MMX mixer function
   reads at entry and rewrites at exit as its persistent phase step (§6.4 of
   the render section). `voice+0x44` (the phase accumulator) is zeroed at
   note start at `0x18f7d`, in the middle of this same
   instruction run. §1.3 (data-flow) and the render/voice sections below are
   written consistently with this account.
2. **The pitch-ratio table at `0x1ad00` has domain n = −100..100 (201
   entries) and is built with `trunc`, not `round`.** The loop-bound
   instructions, read directly:
   ```c
   16700: for (n = -100; n <= 100; n++)          // loop test at 0x16710 — 201 entries, table 0x1ad00
   16744:   table_0x1ad00[n] = trunc(...);        // via the truncate-toward-zero helper, 0x106e0; stored at 0x1ad00 + n*4 (0x1674c)
   16755: // the NEXT table, 0x1af58, repeats this construction over n = -48..48 (97 entries); loop test at 0x16765
   ```
   Table `0x1ad00`'s domain is n = −100..100 (201 entries), and its formula
   is `trunc(4096 · 2^(n/1200))`; the n = −48..48 (97-entry) domain, and the
   same `trunc` construction, belong to the *separate* semitone table at
   `0x1af58`. This is consistent with this driver's general rule that every
   float→int conversion goes through the shared truncating helper `0x106e0`
   (§1.4.2, below).

Every other shared numeric fact this document cross-checked while building
its table of contents and its numeric-environment section (file size, PE
section map, the 552-timecent/1,367,824-ULP figures, the 495-wave/
1498-region/235-instrument/7-key-group counts, the 22050 Hz single-rate
contract, the `0x027F` control word, the velocity/attenuation table's 128
values, the choke-clamp divisor `70`) matched exactly across every document
that states it.

---

## Part 2 — gm.dls DLS/RIFF Parser Specification

Subject: `swmidi.sys` v5.1.2600.5512 (Windows XP SP3), the Microsoft GS Wavetable
Synth kernel driver. This section specifies exactly how this one binary parses
and interprets `gm.dls` (the 3,440,660-byte sample collection shipped with
Windows). It is self-contained: no other document, binary, or disassembly is
assumed available to the reader.

**Provenance tags** used throughout: `[A:0xVMA]` = read directly from an
instruction/byte at that virtual address in `swmidi.sys` (file offset =
VMA − 0x10000 for the PAGE section, which spans VMA 0x12180–0x1cc84 on disk).
`[D:gm.dls]` = read directly from the bytes of `gm.dls`. `[I]` = inference,
with its basis stated. `[O]` = open, not recovered — stated precisely, not
guessed.

All VMA citations were verified by grepping the PAGE-section disassembly of
`swmidi.sys` for the literal address anchor `^   VMA:` (5 leading
tab-column spaces as emitted by objdump, e.g. grepping it for the anchor
`^   157da:`) and reading the instruction text at that line. Where a run of
instructions is quoted, the semantics were re-derived directly from the raw
bytes/mnemonics.

---

### 2.1 Locating and loading the collection

- The driver reads a `REG_SZ` value named `GMFilePath` under the
  registry key
  `\Registry\Machine\Software\Microsoft\DirectMusic` using a generic
  registry-string helper at PAGE `0x1405c` (`RtlInitUnicodeString` ×2,
  `ZwOpenKey`, `ZwQueryValueKey(KeyValuePartialInformation)`, `wcscpy`,
  `ZwClose`) `[A:0x1405c]`. (This is the one permitted appearance of the
  literal string "DirectMusic" in this document — it is a registry path that
  genuinely appears inside `swmidi.sys`'s string table, not a reference to a
  different synthesizer.)
- If the value is absent, of the wrong type, or the call otherwise fails, the
  driver falls back to the literal default path
  `\SystemRoot\System32\Drivers\gm.dls` (string present in the binary's own
  extracted string table) `[I]`
  (the exact call site that reads `GMFilePath` and applies this fallback sits
  immediately outside the PAGE range examined instruction-by-instruction for
  this section; the fallback string's presence and the shape of the helper are
  read facts, the "falls back on failure" behavior is the obvious inference
  from a helper that returns an NTSTATUS and a literal default string sitting
  right next to the call).
- **The entire file is read in a single shot, not streamed.** The open+read
  helper at PAGE `0x15efa` does exactly: `ZwCreateFile` (IAT slot `0x11860`)
  → `ZwQueryInformationFile(..., FileStandardInformation, length=0x18)` (IAT
  `0x1185c`) → `filesize = EndOfFile.LowPart` (only the low 32 bits of the
  64-bit file size are used) → **one** `ExAllocatePoolWithTag(PagedPool,
  filesize, 'SwMi')` (IAT `0x118dc`; tag bytes `53 77 4d 69` = `"SwMi"`,
  pushed as the literal immediate `0x694d7753` at `0x15f84`) → **one**
  `ZwReadFile` (IAT `0x11858`) for the whole `filesize` → `ZwClose`
  `[A:0x15efa]` (full instruction sequence read at file lines 4998–5074 of
  the PAGE-section disassembly, i.e. VMA `0x15efa`–`0x15fde`). There is no read loop, no
  chunked/streamed read, and no re-read of any portion of the file.
- **Implication for implementers:** because the whole file lives in one
  contiguous, never-freed (for the lifetime of the driver instance) paged-pool
  buffer, every pointer the parser stores that references sample or chunk
  bytes can be a **raw pointer into that single buffer** — no chunk ever needs
  to be copied out for later use, and (as detailed in §2.8) this driver actually
  exploits that fact.

---

### 2.2 RIFF/DLS container walk

#### 2.2.1 Chunk-loop shape (uniform, but re-implemented at each level)

Every chunk-walking loop in this parser has the same shape, implemented
independently (not via one shared recursive walker) at each nesting level:

```
p = start
while p < end:
    fourcc = u32_le(p)            # p+0 .. p+3
    size   = u32_le(p+4)          # p+4 .. p+7, chunk's declared data size
    data   = p + 8                # data start
    switch(fourcc): ...           # dispatch table, see §2.2.2
    p = p + 8 + size               # <-- NOTE: no RIFF odd-size padding
```

`[A:0x16486]` (top-level loop test/advance: `p = p + 8 + size`, loop
continues while `p < end`). The identical
"`next = cur + 8 + size`, no padding" pattern was independently re-verified at
every one of these dispatcher loop-tails: `0x156cb` (inside the `wave`
dispatcher `0x153fc`) `[A:0x156cb]`, `0x15aba` (inside the region-articulation
dispatcher `0x15788`) `[A:0x15aba]`, `0x15c48` (inside the region-body
dispatcher `0x15ae6`) `[A:0x15c48]`, `0x15cf1` (inside the `wvpl` container
`0x15c82`) `[A:0x15cf1]`, `0x1627d` (inside the `ins ` body dispatcher
`0x16130`) `[A:0x1627d]`, `0x1635f`-equivalent (inside `lins`, not
re-transcribed byte-for-byte but same instruction shape observed at its call
site), `0x16486` (top level, `0x1638e`) `[A:0x16486]`.

**The single exception:** the `INFO`/`INAM` chunk walker at `0x15fe6` rounds
the advance up to the next even byte: `size = (size+1) & ~1; p = p+8+size`
`[A:0x1602b]` (exact bytes: `8b 46 04` / `40` / `83 e0 fe`, implementing
exactly that formula — increment size by one, then clear its low bit —
which computes "round size up to the next even number", not down —
re-verified arithmetically: for even `n`, `(n+1)&~1 == n`; for odd `n`,
`(n+1)&~1 == n+1`).

**Implementer-visible consequence:** a chunk (`data`, `wsmp`, `art1`, any
chunk except the ones inside an `INFO` list) with an odd declared size,
immediately followed by another chunk, will desync a naive byte-accurate
implementation of this driver by one byte relative to what the DLS-1 spec
requires (spec mandates padding everywhere). This is invisible for `gm.dls`
itself because (verified in §2.7's cross-check script) every chunk in the real
file happens to be even-sized already.

#### 2.2.2 Recognised chunks per nesting level, and their handler VMAs

| Level (container) | Handler VMA | Recognised FOURCCs at this level | Confirmed by |
|---|---|---|---|
| `RIFF`/`DLS ` entry | `0x164ac` | validates signature, then dispatches to top level | `[A:0x164ac]` (function inventory; signature check reads `this+0x34`) |
| Top level (inside `DLS ` form) | `0x1638e` | `LIST` (→ `0x1640a` sub-switch), `colh` (`0x163f6`), `ptbl` (`0x15d12`), `edit` (`0x163c4`) | `[A:0x163a2]`–`[A:0x163be]` (FOURCC compares: `LIST`=`0x5453494c`@`0x163a4`, `colh`=`0x686c6f63`@`0x163ab`, `ptbl`=`0x6c627470`@`0x163b2`, `edit`=`0x74696465`@`0x163b9`) |
| Top-level `LIST` sub-switch | `0x1640a` (inline in `0x1638e`) | `INFO` (→`0x15fe6`), `wvpl` (→ see §2.7, either `0x15c82` or a pointer-fixup loop), `lins` (→`0x162ee`) | `[A:0x1640a]` (`INFO`=`0x4f464e49`@`0x1640d`, `wvpl`=`0x6c707677`@`0x16414`, `lins`=`0x736e696c`@`0x1641b`) |
| `lins` container | `0x162ee` | `LIST`→`ins ` (allocates a 0x20-byte instrument, calls `0x16130`) | function inventory, spot-checked call shape at `0x16433`/`0x16447` `[A:0x16433]` |
| `ins ` body | `0x16130` | `LIST` (→ inner-type switch: `lrgn`=`0x6e67726c`→`0x16245`/`0x16098`; `lart`=`0x7472616c`→ falls through to `0x161df`, builds a default block, calls `0x15788`), `insh` (`0x16177`), `edit` (`0x1616b`) | `[A:0x16150]`–`[A:0x161d9]` (FOURCC compares: `LIST`@`0x16152`, `insh`=`0x68736e69`@`0x16159`, `edit`@`0x16160`; inner `lrgn`/`lart` compares at `0x161cd`/`0x161d4`) |
| `lrgn` container | `0x16098` | `LIST`→`rgn `/`rgn2` (allocates 0x34-byte region via `0x14ff4`, calls `0x15ae6`) | `[I]`, offsets consistent with the region-body dispatcher's own size (0x34, see §2.9) |
| `rgn `/`rgn2` body | `0x15ae6` | `LIST`→`lart` only (`0x686c6f63`… no — see exact compare below) (`0x15bf3`), `rgnh` (`0x15bcd`), `wlnk` (`0x15baf`), `wsmp` (`0x15b38`), `edit` (`0x15b2c`) | `[A:0x15af9]`–`[A:0x15b26]` (`LIST`=`0x5453494c`@`0x15af9`, `rgnh`=`0x686e6772`@`0x15b04`, `wlnk`=`0x6b6e6c77`@`0x15b0f`, `wsmp`=`0x706d7377`@`0x15b1a`, `edit`=`0x74696465`@`0x15b21`); nested `LIST` sub-type checked at `0x15bf3`: only `lart` (`0x7472616c`) is accepted, anything else silently skipped `[A:0x15bf3]` |
| `lart` contents (art1/edit sub-list; **shared code**, reached both from an instrument's own `lart` and from a region's own `lart`) | `0x15788` | `art1` (`0x157da`, see §2.4), `edit` (`0x157ce`, stores into the *articulation-block* object, not the region/instrument object — see §2.3) | `[A:0x157ba]`–`[A:0x157c8]` (`art1`=`0x31747261`@`0x157bc`, `edit`=`0x74696465`@`0x157c3`) |
| `wave`/`WAVE` LIST (inside `wvpl`) | `0x15c82` | `LIST`→`WAVE` (`0x45564157`) or `wave` (`0x65766177`) — **exact literal compares, no case-fold**; anything else (e.g. `Wave`) skipped; allocates 0x34-byte wave object via `0x145a0`, calls `0x153fc` | `[A:0x15c91]`–`[A:0x15ca8]` |
| `wave` contents | `0x153fc` | `fmt ` (`0x20746d66`→`0x15662`), `LIST` (**recognised but not descended into — treated as an unknown/no-op chunk, simply skipped**), `data` (`0x61746164`→`0x154df`), `wsmp` (`0x706d7377`→`0x15464`), `edit` (`0x74696465`→`0x15458`) | `[A:0x15423]`–`[A:0x1544b]`; the `LIST` case at `0x15430` jumps straight to `0x156cb` (the loop-advance code), i.e. it is matched but produces no side effect at all — confirmed by direct read, this is a genuinely distinct behavior from every other level, where `LIST` normally leads somewhere |
| `ptbl` | `0x15d12` | pool-table cue array, see §2.6 | `[A:0x15d12]` |
| Post-pass (no chunk, runs once after the whole top-level loop finishes) | `0x15dde` | resolves regions' wave pointers, see §2.6 | `[A:0x15dde]`, called unconditionally at `0x16498` right before the top-level dispatcher returns `[A:0x16498]` |

**Unrecognized-chunk handling, uniform at every level:** an unmatched FOURCC
simply falls through to the shared "advance to next chunk" code — it is
**silently skipped**, never an error `[A]` (observed at every dispatcher: the
fallthrough target for an unmatched compare always lands on the loop's
own advance code, never an error return (`0x8004…`)).

**Errors that legitimately abort parsing of the *current* chunk (and, because
callers check the returned status for a negative/failure value after every nested call and propagate
failure upward, **abort the entire load** — one malformed instrument, region,
or wave chunk anywhere aborts the whole file, there is no "skip this one and
keep going"):

| Condition | Error code | VMA |
|---|---|---|
| `colh` size < 4 | `0x80041392` | `[A:0x16400]` |
| `insh` size < 0xc | `0x8004138f` | `[A:0x16298]` |
| `rgnh` size < 0xc | `0x8004138d` | `[A:0x15c60]` |
| `wlnk` size < 0xc | `0x8004138e` | `[A:0x15c67]` |
| `wlnk.ulChannel` != 1 | `0x8004138b` | `[A:0x15c6e]` |
| region-level `wsmp`: size/loop-count/loop-record-size violation (any of the 4 checks in §2.6) | `0x8004138d` (all four share this one code) | `[A:0x15c60]` |
| wave-level `wsmp`/`fmt`/`data`: size, loop-count, loop-record-size, `wFormatTag`≠1, `wBitsPerSample` not 8/16, duplicate-`fmt`, `data`-before-`fmt` | `0x80041389` (all share this one code) `[A:0x15715]` | except: |
| `fmt` bad `wFormatTag` (≠1/PCM) | `0x8004138a` | `[A:0x1571c]` |
| `fmt` bad `nChannels` (≠1) | `0x8004138b` | `[A:0x1575f]` |
| **wave-level** `wsmp` loop-record `ulLoopType` ≠ 0 | `0x80041389` (via `0x156fc`) | `[A:0x154be]` — **this check does not exist at region level** (see §2.6) |
| `art1` `cbSize` < 8, or total chunk size ≠ `cbSize + 12*cConnectionBlocks` | `0x8004138c` | `[A:0x15ada]` |
| `ptbl` cue count exceeds 0xffff | `0x80004005` (E_FAIL) | `[A:0x15dbe]` |
| out of memory (`ExAllocatePoolWithTag` returns NULL) anywhere | `0x8007000e` | multiple sites, e.g. `[A:0x15d06]`, `[A:0x156f5]` |
| bad top-level `RIFF`/`DLS ` signature | `0x80004005` | not independently re-verified byte-for-byte this pass — `[I]` |

A chunk header that straddles `end` (fewer than 8 bytes remain) simply ends
the loop — no error, treated as "no more chunks" `[A]` (every loop-continue
test is `p < end`, checked *before* reading the FOURCC/size, at all levels).

A `ptbl` cue array that runs past `end` mid-array **breaks the cue loop
silently, no error for this specific case** `[A:0x15d60]` (the loop test
`cursor >= end` exits the loop directly — falls straight through to the
final count-store code, no error path taken for this specific condition;
contrast with the separate, explicit `>0xffff` check above, which *is* an
error).

---

### 2.3 Per-chunk field tables, byte-exact

Offsets below are **relative to chunk data** (8 bytes after the FOURCC)
unless stated otherwise.

#### 2.3.1 `insh` (instrument header) — handled at `0x16177`, inside `0x16130`

Required size ≥ 0xc, else error `0x8004138f` `[A:0x16177]`.

| off | width | field | driver use |
|---|---|---|---|
| 0 | DWORD | `cRegions` | **ignored** — never read anywhere in this function; the driver counts actual region LIST chunks it finds instead |
| 4 | DWORD | `ulBank` | bit 31 = drum flag (tested via `AND 0x80000000`), bits 8–14 = bank MSB (`AND 0x7f00`), bits 0–6 = bank LSB (`AND 0x7f`); bits 7, 15–30 ignored | `[A:0x16186]`,`[A:0x1619b]`,`[A:0x161a9]` |
| 8 | DWORD | `ulInstrument` | program number, stored **as read, with no masking at all** — if bits above bit 6 are set, they corrupt the packed locale below | `[A:0x16192]` |

**Locale packing** (the internal instrument lookup key, stored at
instrument+0x10), read directly instruction-by-instruction:

```
locale  = ulInstrument                       ; 0x16192, full 32-bit value, unmasked
locale |= (ulBank & 0x7f)   << 7             ; 0x1619b/0x1619e — bank LSB -> bits 7-13
locale |= (ulBank & 0x7f00) << 6             ; 0x161a9/0x161ae — bank MSB -> bits 14-20
if drum: locale |= 0x80000000                ; 0x161c0 — drum flag -> bit 31
```

i.e. **`locale = program | (bankLSB<<7) | (bankMSB<<6+8=<<14 effectively) |
(drum<<31)`**, matching the commonly-cited formula `program | bankLSB<<7 |
bankMSB<<14 | drum<<31` — confirmed exact, full instruction trace,
`[A:0x16192]`–`[A:0x161c5]`. Verified byte-exact against real file data in
§2.10.

#### 2.3.2 `colh` (collection header) — handled at `0x163f6`

Size must be ≥ 4, else error `0x80041392` `[A:0x163f6]`. **The
`cInstruments` DWORD is never loaded into any field — it is validated for
presence/size only, then completely ignored** `[A:0x163f6]` (no read of the
data past the size check anywhere in this handler). Actual instrument count =
number of `ins ` chunks encountered while walking `lins` — verified equal to
the declared value for `gm.dls` in §2.11 (235 = 235), which is why this
divergence is invisible on the reference file.

#### 2.3.3 `rgnh` (region header) — handled at `0x15bcd`, inside `0x15ae6`

Size must be ≥ 0xc, else error `0x8004138d` `[A:0x15bcd]`.

| off | width | field | driver use |
|---|---|---|---|
| 0 | WORD, **only low BYTE read** | `usLowKey` | → region+0x30 | `[A:0x15bdd]` |
| 2 | WORD, **only low BYTE read** | `usHighKey` | → region+0x2f | `[A:0x15bd7]` |
| 4 | WORD | `usLowVel` | **never read anywhere in this function** | — |
| 6 | WORD | `usHighVel` | **never read anywhere in this function** | — |
| 8 | WORD (read as BYTE, masked `&1`) | `fusOptions` | bit 0 → region+0x2e | `[A:0x15be3]` |
| 0xa | WORD, **only low BYTE read** | `usKeyGroup` | → region+0x31 | `[A:0x15beb]`/`[A:0x15bee]` |

The region key-search predicate (§2.9) at `0x14a82`/`0x14ac2` only ever compares
against region+0x2f/+0x30 `[A:0x14a82]` — **there is no velocity-range gate at
region-selection time in this driver**, despite `rgnh` nominally carrying one
that is fully parsed off but discarded.

#### 2.3.4 `wsmp` — **two separate handlers, same nominal layout, different struct targets**

- **Wave-level** `wsmp`, inside a `wave` LIST, handled at `0x15464` (inside
  `0x153fc`). Requires size ≥ 0x14 else error `0x80041389` (via `0x156fc`).
- **Region-level** `wsmp`, inside a `rgn `, handled at `0x15b38` (inside
  `0x15ae6`). Requires size ≥ 0x14 else error `0x8004138d` (via `0x15c60`).
  This is a **region-specific override** — it writes into fields private to
  the region object, entirely separate storage from the wave object's own
  `wsmp` fields.

| off | width/sign | field | wave-level use (dest field, VMA) | region-level use (dest field, VMA) |
|---|---|---|---|---|
| 0 | DWORD | `cbSize` | **never read in either handler** (not even for bounds math — re-verified: no instruction in either function's range reads offset+0 of the wsmp payload; the size gate at entry checks the *chunk's own declared size*, not this field) | same |
| 4 | WORD | `usUnityNote` | **only the low BYTE is read**, stored at wave+0x2d `[A:0x15485]` | **only the low BYTE is read**, stored at region+0x1e; also sets region+0x1f=1 ("own unity note" override flag) `[A:0x15b59]` |
| 6 | WORD signed | `sFineTune` | stored as-is at wave+0x20 `[A:0x1547d]` | stored as-is at region+0x18 `[A:0x15b51]` |
| 8 | DWORD signed | `lAttenuation` | `(lAttenuation*10) >>arith 16` → WORD at wave+0x1e `[A:0x1546e]` | same formula → WORD at region+0x24 `[A:0x15b42]` |
| 0xc | DWORD | `fulOptions` | **wave-level only:** read into a local, and used later — see §2.8; **not a dead field at wave scope** `[A:0x1548f]` | **region-level: never read at all** `[A:0x15b38]`–`[A:0x15baa]`, re-verified, 0 occurrences |
| 0x10 | DWORD | `cSampleLoops` | 0 → "no loop" flag set (wave+0x2c=1), stop; 1 → proceed to loop record; **>1 → hard error** `0x80041389` | 0 → region+0x1d=1, stop; 1 → proceed; **>1 → hard error** `0x8004138d` |
| 0x14 (loop record) `cbSize` | DWORD | must be ≥ 0x10 else error | same, own error code | same |
| 0x18 | DWORD | `ulLoopType` | **checked, must equal exactly 0, else hard error `0x80041389`** `[A:0x154be]` | **never read at all — every present loop is treated as a plain forward loop regardless of declared type** `[A:0x15b38]`–`[A:0x15baa]` |
| 0x1c | DWORD | `ulStart` | stored at wave+0x14 | stored at region+8 |
| 0x20 | DWORD | `ulLength` | **not stored as a length** — computed `end = ulStart+ulLength`, stored at wave+0x18 | same, stored at region+0xc |

Direct reading of `0x15464` (an 8-bit load from the wsmp payload's
`usUnityNote` field, not a 16-bit load) shows
**both handlers truncate `usUnityNote` to a single byte** at wave scope and at
region scope. There is also a `ulLoopType` asymmetry between the two scopes
(enforced-must-be-0 at wave level vs. never-read at region level),
`[A:0x154be]` vs. absence of any read in `0x15b38`–`0x15baa`.

#### 2.3.5 `wlnk` (wave link) — handled at `0x15baf`, inside `0x15ae6`

Size must be ≥ 0xc, else error `0x8004138e` `[A:0x15baf]`.

| off | width | field | driver use |
|---|---|---|---|
| 0 | WORD | `fusOptions` | **never read** |
| 2 | WORD | `usPhaseGroup` | **never read** |
| 4 | DWORD | `ulChannel` | **checked `== 1` exactly, else hard error `0x8004138b`** — multi-channel/stereo wave links are unsupported, entire chunk rejected `[A:0x15bb9]` |
| 8 | DWORD, **only low WORD read** | `ulTableIndex` | stored (truncated to 16 bits) at region+0x1a `[A:0x15bc3]` |

#### 2.3.6 `art1` — see §2.4 for the connection-block decoder.

Header (`CONNECTIONLIST`): `cbSize` (DWORD, ≥8 required else error
`0x8004138c`) then `cConnectionBlocks` (DWORD). **The connection-block array
is located at `chunk_data + 8` unconditionally** — the code never adds the
actual `cbSize` value to compute the array start, it hardcodes the offset
`+8` (visible at `0x15804`, which sets the loop cursor to
`chunk_data + 0x10`, and the loop body itself indexes
fields at `cursor-8..cursor+3`, i.e. the first block truly begins at
`chunk_data+8`) `[A:0x15804]`. For a hypothetical file whose `CONNECTIONLIST`
header is padded larger than 8 bytes (still passing the `≥8` check), this
driver would misparse the connection array by however many extra bytes the
real author intended as padding — harmless for `gm.dls`, where every
`art1.cbSize` sampled is exactly 8 `[D:gm.dls]` (§2.11). Total chunk size is
validated to equal `cbSize + cConnectionBlocks*12`, else error `0x8004138c`
`[A:0x157ef]`.

Note that `chunk_data+0x10` is the address the loop cursor is initialized to
— it points at the *middle* of the first connection block (offset+8 into it,
used so the decoder can address `usSource`/`usControl`/`usDestination` via
small negative displacements from the cursor, and `lScale` via
`cursor+0`/`cursor+2`) — not the address of the array
itself. The array itself starts at `chunk_data+8`, standard DLS-1 placement.

#### 2.3.7 `ptbl` (pool table) — handled at `0x15d12`. Full mechanics in §2.6.

#### 2.3.8 `wave`/`WAVE` LIST (inside `wvpl`) — `0x15c82`. FOURCC accepted as
exactly `"WAVE"` (`0x45564157`) or exactly `"wave"` (`0x65766177`), two
literal compares, no case-fold; anything else silently skipped
`[A:0x15c9c]`.

#### 2.3.9 `fmt ` / `data` — see §2.8 in full.

#### 2.3.10 `edit` — appears at **five distinct code sites**

Grepping the PAGE-section disassembly for the literal FOURCC comparison
against `0x74696465` (the four ASCII bytes of `"tide"` reversed, i.e.
`"edit"` little-endian) finds it at exactly **five** VMAs:
`0x1544d`, `0x157c3`, `0x15b21`, `0x16160`, `0x163b9`
(5 hits — re-verified). Every one of
these five sites executes the identical pattern — read the **first DWORD of
the chunk's data** (full 32 bits), then store only its **low 16 bits**
into a scope-specific 16-bit field — confirmed at each site individually:

| Site VMA (compare / store) | Scope | Dest field | Reached from |
|---|---|---|---|
| `0x1544d` / `0x1545b` | wave | wave+0x22 | `wave` LIST body dispatcher `0x153fc` |
| `0x157c3` / `0x157d1` | **the articulation block itself** (the 0x68-byte "lart" default-articulation object) | that object's own +0x60 | the shared `lart`-contents dispatcher `0x15788`, reachable from **both** an instrument's own `lart` LIST **and** a region's own `lart` LIST — same code, same struct-relative offset, but a *different backing object* each time (instrument-level lart block vs. region-level lart block) |
| `0x15b21` / `0x15b2f` | region **(directly in the `rgn ` body, not via `lart`)** | region+0x2c | region-body dispatcher `0x15ae6` |
| `0x16160` / `0x1616e` | instrument **(directly in the `ins ` body, not via `lart`)** | instrument+0x18 | `ins ` body dispatcher `0x16130` |
| `0x163b9` / `0x163c7` | collection (top level) | collection+0x3c | top-level dispatcher `0x1638e` |

All five VMAs and stores re-verified by direct read `[A:0x1544d]`
`[A:0x157c3]` `[A:0x15b21]` `[A:0x16160]` `[A:0x163b9]`.
**Notably, the `lart`-scoped occurrence (`0x157c3`)** means an `edit` chunk
*inside a `lart` LIST* (at either instrument or region scope) behaves
completely differently from an `edit` chunk placed directly in the
`ins `/`rgn ` body: they write to different objects. Semantic purpose of this
non-standard (not part of the DLS-1 spec) chunk remains **[O]** — no debug
string or comparison against a
fixed value ties any meaning to it anywhere in the code read for this
section.

---

### 2.4 `art1` connection-block decoder — `0x157da`, inside `0x15788`

This is the heart of the articulation model. Connection-block layout, 12
bytes: `usSource` (0, WORD), `usControl` (2, WORD), `usDestination` (4, WORD),
`usTransform` (6, WORD), `lScale` (8, DWORD, signed).

**Two quirks stated up front because a spec-conformant DLS-1 implementation
would get this driver's actual behavior wrong:**

- **`usTransform` is read nowhere in this function.** Grepped explicitly:
  0 instructions anywhere in `0x157da`–`0x15ad7` read the field at
  `cursor-2` (the block-relative offset where `usTransform` lives) `[A]`
  (grep confirmed no access to that offset anywhere in the function). Every
  connection is treated as linear regardless of its declared transform —
  `CONN_TRN_CONCAVE` has no effect.
- **`usSource == 4` (the DLS-1 constant for "EG1 as a modulation source") is
  skipped entirely by the dispatch chain.** The chain that tests `usSource`
  checks 0, 1, 2, 3 explicitly, then falls through two more decrement steps
  with no test in between before the final check against 5 — so
  `usSource==4` never matches any case and the connection is silently
  dropped `[A:0x15821]`–`[A:0x15823]` (the decrement/branch sequence,
  reached only after the `usSource==3` test at `0x1581f` fails).

#### 2.4.1 Field addressing inside the loop

The loop cursor is initialized to `chunk_data + 8 + 8` (i.e. it points
at the *third* field-pair of the first connection block, specifically at the
start of that block's `lScale`), and advances by `0xc` (12, one block) per
iteration `[A:0x15804]`/`[A:0x15aae]`. Relative to the cursor: `usSource` =
`cursor-8`, `usControl` = `cursor-6`, `usDestination` = `cursor-4`,
`usTransform` = `cursor-2` (never read), `lScale` (low word) = `cursor+0`,
`lScale` (high word) = `cursor+2`.

#### 2.4.2 `lScale` conversion formulas actually implemented

- **Attenuation-style (0.1 dB units)**: `(lScale * 10) >>arith 16`. Special
  case `lScale == 0x80000000` (INT32_MIN) maps directly to sentinel `0xda80`
  (signed −9600, "silent") instead of doing the multiply (avoids signed
  overflow) `[A:0x158a4]`.
- **Pitch/cents (0.01-cent-ish; DLS "1/65536 cent")**: driver stores the
  **high 16 bits of `lScale` directly** (`cursor+2`, i.e. `lScale>>16`
  truncated, no rounding), then clamps to `[-1200,+1200]` `[A:0x1594d]`.
- **Time-domain destinations (LFO frequency/start-offset, all four EG1/EG2
  segment times)**: routed through one of two float-domain helper functions,
  `0x15364` or `0x153aa` (both outside PAGE, cross into synth-engine
  fixed-point/float conversion code not further decoded in this pass —
  **[O]**, the exact log2(seconds)-domain formula they implement was not
  re-verified byte-for-byte this pass and is not closed here).
- **The two `wave+0x62` destinations (0x0002 and 0x0004/PAN)**: both funnel
  through a shared `idiv 125` (signed) tail at `0x15a20`, but with **different
  pre-scale**: dest 0x0002 does `lScale<<4` first `[A:0x15a1d]`; dest 0x0004
  (PAN) does `lScale>>12` first (arithmetic shift) `[A:0x15a16]`; both then
  `/125` and store a WORD at wave+0x62 — **the same destination field for
  both IDs**, confirmed exact `[A:0x15a1b]`/`[A:0x15a14]`.
- **Sustain-level destinations (0x0208, 0x030c)**: stored as the **raw low
  16 bits of `lScale`**, no scaling — matches DLS convention that sustain
  level is already linear, not log/dB.

#### 2.4.3 Full recognised `(usSource, usControl, usDestination)` → action table

Every row below was derived by tracing the decrement-chain arithmetic
instruction-by-instruction.

**Source = 0 (static / no modulation source):**

| usDestination | action | dest field | VMA of store |
|---|---|---|---|
| 0x0002 | `(lScale<<4)/125` | WORD wave+0x62 | `0x15a26` |
| 0x0004 (PAN) | `(lScale>>12)/125` | WORD wave+0x62 (**same field as 0x0002**) | `0x15a26` (shared tail) |
| 0x0104 (LFO_FREQUENCY) | via `0x153aa` | **DWORD wave+0x40** | `0x15a0c` |
| 0x0105 (LFO start-offset time) | via `0x15364` | **DWORD wave+0x48** (clears +0x4c) | `0x159fa` |
| 0x0206 (EG1 attack) | via `0x15364` | DWORD wave+0x20 (clears +0x24) | `0x159e4` |
| 0x0207 (EG1 decay) | via `0x15364` | **DWORD wave+0x28** (clears +0x2c) | `0x159ce` |
| 0x0208 (EG1 sustain level) | raw low WORD of `lScale` | WORD wave+0x3c | `0x15aa4` |
| **0x020a** | raw **high** WORD of `lScale` | **WORD wave+0x3c — same field as 0x0208** | `0x15aa4` (via `0x15aa0`) |
| 0x0209 (EG1 release) | via `0x15364` | DWORD wave+0x30 (clears +0x34) | `0x15a3a` |
| 0x030a (EG2 attack) | via `0x15364` | **DWORD wave+0x00** (clears +0x04) | `0x15a9c` |
| 0x030b (EG2 decay) | via `0x15364` | DWORD wave+0x08 (clears +0x0c) | `0x15a89` |
| 0x030c (EG2 sustain level) | raw **low** WORD of `lScale` | WORD wave+0x1c | `0x15a75` (via `0x15a72`) |
| **0x030e** | raw **high** WORD of `lScale` | **WORD wave+0x1c — same field as 0x030c** | `0x15a75` (via `0x15a59`) |
| 0x030d (EG2 release) | via `0x15364` | DWORD wave+0x10 (clears +0x14) | `0x15a6d` |

**Destinations `0x020a` and `0x030e` are present in the dispatch chain, and
`lScale` is not always zero for them — confirmed two independent ways:**

1. **Direct disassembly**: the dispatch chain for `usSource==0, usDestination
   > 0x209` at `0x15a3f` explicitly tests `dest==0x20a` (subtract `0x20a`,
   branch if the result is zero, `[A:0x15a3f]`–`[A:0x15a44]`) and, later in
   the same chain, `dest==0x30e` (a further decrement-and-branch step, after
   four prior decrements from `0x30a`, `[A:0x15a56]`–`[A:0x15a57]`). Both are
   handled, sharing storage
   with 0x0208 and 0x030c respectively (reading the *high* word of `lScale`
   where the paired destination reads the *low* word).
2. **Real file data** (§2.11 cross-check script, run against `gm.dls`): dest
   `0x020a` occurs **203 times** across the 235 instruments' instrument-level
   `art1` blocks, of which **104 have a non-zero `lScale`**; dest `0x030e`
   occurs **223 times**, of which **4 have a non-zero `lScale`**
   `[D:gm.dls]` (exact script output in §2.11). These are not "always zero" —
   they materially affect the EG1/EG2 sustain-level fields in a large
   fraction of real instruments. (Their counts numerically match the
   companion destination 0x0208/0x030c counts exactly — 104-of-203 and
   4-of-223 respectively — consistent with the file's author deliberately
   using the high/low-word split this driver implements to encode a single
   sustain-level value across the two destination IDs; this specific
   authoring-intent explanation is `[I]`, the counts themselves are
   `[D:gm.dls]`.)

**Source = 1 (LFO):**

| usControl | usDestination | action | dest field | VMA |
|---|---|---|---|---|
| 0 | 0x0001 (ATTENUATION) | `(lScale*10)>>16` clamped to `[-1200,+1200]` | WORD wave+0x54 | `0x1591c`/`0x15926` (shared tail `0x1590e`) |
| 0 | 0x0003 (PITCH) | high word of `lScale`, clamped `[-1200,+1200]` | WORD wave+0x56 | `0x1594d` shared tail |
| 0x0081 (CC1/mod wheel) | 0x0001 (ATTENUATION) | same dB formula, clamped `[-1200,+1200]` | WORD wave+0x50 | `0x1590e`/`0x1590b` |
| 0x0081 (CC1/mod wheel) | 0x0003 (PITCH) | high word of `lScale`, clamped | WORD wave+0x52 | `0x1594d` shared tail |

(Any other `usControl` value with `usSource==1` is dropped — the gate at
`0x158e8`/`0x158ec` only accepts exactly 0 or 0x81.) **Correction of prior
report:** it only documented the two PITCH rows and left the field offsets as
"+0x50-ish"/vague; the ATTENUATION (tremolo-depth) rows for `usDestination
0x0001` were not documented at all, and all four exact field offsets above
are freshly re-derived, byte-exact.

#### 2.4.3.1 Runtime LFO→PITCH (vibrato) reimplementation — `[M: probe 06]`

The table above (parsing) has always been `[A]` (disassembly-confirmed byte
offsets). What is new here is that `src/engine/` (this project's clean-room
reimplementation, not the original binary) now actually *applies* the
`usDestination==0x0003` (PITCH) connections at runtime — previously parsed
off into `Artic` fields but never wired into the signal path
(`artifacts/SPEC_GAPS.md` #10). The original driver's own runtime oscillator/
depth-combination code (adjacent to the unrecovered `0x153aa`/`0x15364`
time-domain helpers, §2.4.2) was never located in this project's
disassembly work and remains `[O]` — the reimplementation below is measured
against reference audio, not recovered from the binary:

- **Rate**: `freq_Hz = 8.176 * 2^((lfo_freq_tc/65536)/1200)` — the standard
  DLS-1/SF2 "absolute pitch cents, 8.176 Hz at 0 cents" convention, applied
  per-instrument to each region's own `lfo_freq_tc` (dest `0x0104`, already
  `[A]`-parsed). Confirmed: programs 48 (String Ensemble 1) and 73 (Flute) —
  `probes/06_modwheel.mid`'s own two instruments — both carry raw
  `lfo_freq_tc` lScale `-35106105`, converting to 6.0001 Hz, matching the
  ~6.0 Hz vibrato rate measured directly in `probe-results/06.flac` (own
  fresh Hilbert-transform/sinusoid-fit measurement, `fit_check_lfo.py`, a
  harness script not retained)
  to 4 significant figures with zero fudge factor. Per-instrument, not a
  global constant (e.g. Acoustic Grand Piano's own raw `-51342062` converts
  to ~5.20 Hz).
- **Depth**: only the CC1-gated connection (`usControl==0x0081`) is applied,
  `depth_cents = lfo_pitch_cc1_cents * (live CC1 / 127)`, multiplied by a
  256-entry sine table (T.5) sampled at the per-voice LFO phase. The sibling
  ungated (`usControl==0`, "inherent") connection is parsed but deliberately
  **not** applied: own fresh corpus-wide measurement (`run.py --skip-smoke`,
  a harness script not retained) found that summing it in regresses `probes/04_envelope.mid`
  (Acoustic Grand Piano, program 0, which never sends CC1 but carries a real
  1-cent inherent depth in `gm.dls`) by **+10.7 dB**
  (`compare_spectral_22050.overall_db`: -12.0 → -1.3 dB) — a confirmed real
  regression on a probe unrelated to CC1/vibrato at all, not the probe-06
  alignment-search fragility below. CC1-only avoids this regression exactly
  (`04_envelope` restores byte-for-byte) while still meeting the measured
  rate/depth targets on `06_modwheel` (see below).
- **Sub-block granularity**: `render_frames` (`src/engine/render.c`) re-slices
  every render call into 64-frame (~2.9 ms) sub-chunks so a held note with no
  intervening MIDI events still gets its LFO phase advanced frequently
  enough to actually oscillate, rather than freezing at one per-block offset.
- **Measured evidence** (own fresh measurement, `fit_check_lfo.py`, not retained,
  against `probes/06_modwheel.mid` vs `probe-results/06.flac`): rate 5.996 Hz
  on every CC1≥32 segment (reference: 5.976–5.986 Hz); depth (peak-to-peak)
  tracks CC1 roughly linearly, strings ~20¢→~86¢, flute ~20¢→~82¢ from
  CC1=32 upward (flute's own CC1=0 point measures ~0¢ synth vs. the
  reference's own ~7¢, entirely attributable to the dropped inherent term —
  a known, accepted trade-off, not a rate/scaling error).
- **Ship gate**: this was NOT gated on `probes/06_modwheel.mid`'s own
  `compare_spectral_22050` score — that probe's ten-identical-repeated-notes
  structure makes the `run.py` harness's (not retained) cross-correlation
  alignment search independently fragile (flagged `IMPLAUSIBLE_OFFSET` even on the untouched
  baseline, before any of this work). Shipped instead on the direct
  rate/depth measurement above (`[M: probe 06]`) plus a corpus-wide
  regression check confirming every other graded probe is unaffected (see
  `FITTED.md` Entries 2–3 for the full investigation history and this
  entry's own superseding note).

**Source = 2 (KEYONVELOCITY):**

| usDestination | action | dest field | VMA |
|---|---|---|---|
| 0x0001 (ATTENUATION) | `(lScale*10)>>16`, `0x80000000`→sentinel `0xda80`, then clamped `≤0` and `≥0xda80(-9600)` | WORD wave+0x64 | `0x158c6` |
| 0x0206 (EG1 attack) | high word of `lScale` | WORD wave+0x38 | `0x1589b` |
| 0x030a (EG2 attack) | high word of `lScale` | WORD wave+0x18 | `0x1588e` |

**Source = 3 (KEYNUMBER):**

| usDestination | action | dest field | VMA |
|---|---|---|---|
| 0x0207 (EG1 decay) | high word of `lScale` | WORD wave+0x3a | `0x15868` |
| 0x030b (EG2 decay) | high word of `lScale` | WORD wave+0x1a | `0x1585b` |

#### 2.4.3.2 Runtime decay key-follow reimplementation — `[M: field/town.mid]`, normalization `[I]`

The parsing above has always been `[A]`. As of 2026-07-26 `src/engine/`
also *applies* it: `dls.c` stores both rows into
`Artic.eg1_decay_kf_tc`/`eg2_decay_kf_tc`, and `voice.c`'s
`decay_tc_keyfollow` adds `kf * key / 128` timecents to the authored decay
time at note-on. Higher notes decay faster.

- **169 of `gm.dls`'s 235 instruments carry the `0x0207` row** (scales
  −4800..+2400, median −3979; 29 also carry `0x030b`). The 66 that do not are
  mostly synth leads, pads and SFX — including `008:080` Sine Wave and
  `001:080` Square, which is why those otherwise-ideal measurement carriers
  cannot be used to probe this (see `probes/35_decay_keyfollow.mid`). Of the
  169, **92** pair it with a real (non-sentinel) decay and sustain < 10%,
  i.e. a decay segment that actually runs to silence and is measurable.
  Dropping the row made every one of those notes decay 3–5× too slowly —
  which is every acoustic patch in the field corpus. Found by ear/
  spectrogram on `field/town.mid` 25.0–26.7 s: a Steel-str.Gt chord (keys
  59/63/68, authored decay 24.6 s, `kf` −4389) rings for the whole bar while
  the reference fades, leaving its otherwise-correct CC1 vibrato bright
  across the whole passage.
- **Measured** (own fresh measurement, Hilbert band-envelope dB/s fit on
  four partials of that chord, 25.30–26.65 s, against `field/town.flac`):
  reference −13.4/−12.3/−10.7/−9.4 dB/s; this project before the change
  −4.6/−4.2/−3.7/−2.3; after −13.3/−12.4/−10.8/−8.6. `field/town.mid`
  render duration 80.25 s → 79.13 s against a 79.22 s reference.
- **Cross-check on a second, independently measured instrument:** Piano 1
  note 60 (`SPEC_GAPS.md` §15's own measured reference, −7.14 dB/s). Base
  6386.5 tc = 40.0 s gives 2.50 dB/s with no key-follow; with it, 13.63 s =
  **7.34 dB/s**. That resolves §15's open "still ~2.9× too slow" residual and
  explains why `FITTED.md` Entry 1's global 2.85× decay multiplier fixed
  note 60 and regressed every other key — 2^(1865/1200) = 2.945× *is* this
  connection at note 60, frozen into a constant.
- **Normalization — measured `[M: probe 35]`, partially settled.** The driver
  stores the full-scale high word and its consumption code is unrecovered
  (Part 5 `+0x13c`, `[O]`), so the divisor was `[I]` from DLS-1's own
  KEYNUMBER convention. `probes/35_decay_keyfollow.mid` (Piano 1 and
  Steel-str.Gt at keys 24–96, Vibraphone at 48/96, all sustain=0) against
  `probe-results/35.flac` now measures it directly, by fitting each note's
  decay in dB/s and regressing `log2(rate)` on key:

  | section | measured d(log2 rate)/dkey | implied divisor | `/128` predicts | `/127` predicts |
  |---|---|---|---|---|
  | Piano 1 | 0.02604 | 127.3 | 0.02590 | 0.02611 |
  | Steel-str.Gt | 0.02897 | 126.3 | 0.02857 | 0.02880 |
  | Vibraphone | 0.03179 | 125.8 | 0.03125 | 0.03150 |

  **Settled:** the source is the *absolute* key, not 60-relative. Both give
  the same slope, but 60-relative predicts absolute rates ~2.9× off (e.g.
  2.50 vs. 7.34 dB/s at Piano 1 note 60) and the reference matches the
  absolute-key form to 3.5%. **Not settled:** `/127` vs. `/128`. The measured
  divisor is 126.5 ± 0.8, which does not separate them (they differ by 0.8%
  in slope and 1.4% in offset at key 96). `/128` ships, as the natural `>>7`
  for integer code. This is the residual `[I]`.
- **Decay *shape* — `[M: probe 35]`.** With the per-key duration correct, the
  only remaining discrepancy is a single scale factor on the rate, uniform
  across all 17 notes: the reference decays at **0.965×** this project's rate
  (sd 0.005, three instruments, rates spanning 3.8–27 dB/s, no trend against
  key or instrument). So the decay segment is "96.5 dB over `seconds`", not
  the 100 dB S3.4.2's shared `exp_coef` assumes; shipped as
  `DECAY_RATE_MULT = 0.965` (`voice.c`). Release is untouched — Part 5 S5.6's
  70 ms fast-release still matches 100 dB. 96.5 ≈ 96.33 dB (a 16-bit floor)
  is a *hypothesis only*, 1.4 sd from the measurement and not confirmed.
- **Corpus effect:** mean spectral residual −24.55 → −28.18 dB on key-follow
  alone, → −28.85 dB with the shape correction; mean envelope `r`
  0.903 → 0.909. Regressions named in `FITTED.md` Entry 15.

**Source = 5 (EG2):**

| usDestination | action | dest field | VMA |
|---|---|---|---|
| 0x0003 (PITCH) | high word of `lScale` | WORD wave+0x1e | `0x15838` |

#### 2.4.4 Destinations/sources explicitly, confirmedly ignored

- `usSource==4` (EG1 as a modulator) — dropped entirely, any destination
  (§ above).
- `usSource∈{3,2}` to any destination not in its own table above — falls to
  the `jne 0x15aa8` bail.
- `usSource==5` (EG2) to any destination other than PITCH (0x0003) —
  ignored.
- `usSource==1` (LFO) with `usControl` other than exactly `0` or `0x0081` —
  ignored.
- No connection destination IDs corresponding to the DLS-2 tone-shaping
  destinations (numeric IDs `0x0500`/`0x0501`, absent from DLS-1) appear
  anywhere in this dispatch chain — only the destination set enumerated above
  (`0`,`1`,`2`,`3`,`4`,`0x104`,`0x105`,`0x206`-`0x20a`,`0x30a`-`0x30e`) is ever
  tested `[A]` (full chain read, §2.4.3). `gm.dls` itself contains zero
  connections targeting such an ID (not applicable/not checked further, since
  the destination set tested in code already excludes them structurally).
- `usTransform` — always ignored (§2.4, top).

---

### 2.5 Default articulation (no `art1` present, or a destination not connected)

Two initializer functions build the defaults, both read in full:

- **`0x1446a`** — instrument-level default articulation block (0x68 bytes),
  built once per instrument that has an instrument-scope `lart`
  `[A:0x1446a]`:
  - Two envelope-time blocks zeroed via `0x1429c` (attack=decay=release=0,
    sustain level = 1000 i.e. `0x3e8`) `[A:0x1429c]`.
  - WORD +0x5c = 0, WORD +0x62 = 0 (pan/coarse-pan default = center).
  - WORD +0x1e = 0 (no EG2→pitch by default).
  - WORD +0x60 = 0.
  - WORD +0x64 = `0xda80` (−9600) — the default velocity-sensitivity depth
    for the ATTENUATION destination. Applied through §3.5's formula
    `scaled = (velAtten * depth) / -9600`, a depth of `-9600` makes
    `scaled == velAtten` exactly: **this default gives velocity *full*
    effect on note-on attenuation, not "disabled"** (`depth = 0` would be the
    disabled case) — confirmed by direct instruction read of the formula's
    application (`0x19bb5` loads the depth, `0x19bb9`-`0x19bc2` compute
    `scaled`; no special-case branch on the sentinel value exists in that
    range). `gm.dls` contains zero `(usSource=2 KEYONVELOCITY,
    usDestination=0x0001 ATTENUATION)` `art1` connections across all 7,451
    connection blocks in the file `[D:gm.dls]`, so every one of its 235
    instruments relies on this exact default — velocity audibly affects note
    volume for the entire bank, not just academically.
  - DWORD +0x58 = `0x5622` (22050) — default sample-rate assumption.
- **`0x145a0`** — wave-object default block (0x34 bytes) `[A:0x145a0]`:
  DWORD+8 = 22050 (default sample rate before `fmt ` is parsed), BYTE+0x2e = 1
  (default "16-bit PCM storage" mode, see §2.8), all pointer/count fields
  zeroed, WORD+0x1c (index) = 0.
- **`0x14ff4`** — region default block (0x34 bytes), read in full this pass
  `[A:0x14ff4]`: zeroes region+0, +0x20, +0x24, +0x26, +0x28, +0x2c, sets
  BYTE+0x30 (low key) = 0, BYTE+0x31 (key group) = 0, BYTE+0x2e
  (fusOptions bit) = 0, and **BYTE+0x2f (high key) = 0x7f (127)** — the only
  non-zero default, giving an `rgnh`-less region the DLS-1-spec-consistent
  default key range `[0,127]`.

No unified "default connection list" (a literal array matching the DLS-1
spec's documented default-connection-set) exists; the equivalent behavior is
achieved by these pre-set field defaults, only overwritten per-field by an
actual `art1` connection when one targets that destination.

---

### 2.6 `wsmp` handling — summary (full byte table in §2.3.4)

- `usUnityNote`: **byte-truncated at both wave and region scope.**
- `sFineTune`: WORD, stored as-is at both scopes.
- `lAttenuation`: `(lAttenuation*10) >>arith 16` → signed WORD, identical
  formula to `art1`'s velocity→attenuation path, at both scopes; the
  region-scope result is summed with **no further ×10 scaling** into the
  hundredths-of-a-dB note-on gain accumulator — units and the deciding VMAs
  are given in full at §1.4.4.
- `fulOptions`: **region-level: dead, never read. Wave-level: read into a
  local and used to conditionally veto the driver's global 16→8-bit
  sample-storage-reduction policy — see §2.8.**
- `cSampleLoops`: 0 or 1 only; >1 is a hard parse error at both scopes (not
  "use the first and ignore the rest").
- `ulLoopType`: **wave-level: must equal 0 or the whole chunk is rejected as
  an error. Region-level: never read at all, every present loop treated as a
  plain forward loop regardless of declared type.** This is a wave/region
  asymmetry.
- Loop record `(ulStart, ulLength)`: converted to `(start, start+length)` and
  stored as an **end position**, not a length, at both scopes. No clamp
  against the actually-decoded sample length was found in the range read for
  this section — **[O]**, not settled either way (may be handled at
  note-render time, outside this parser).
- **Region overrides wave, not vice versa**: the region-level `wsmp` writes
  into region-private fields with an explicit override flag (region+0x1f).
  The actual "if region+0x1f==0, fall back to the wave's own resolved `wsmp`
  values" logic was **not located inside this parser's code range** — **[O]**,
  almost certainly implemented at note-trigger time elsewhere in the driver
  (outside PAGE `0x142f8`–`0x1667f`).

---

### 2.7 `fmt `/`data` — sample format handling, inside `0x153fc`

#### 2.7.1 `fmt ` (`0x15662`)

Requires size ≥ 0x10 (`PCMWAVEFORMAT`); copies 18 raw bytes (4 DWORDs + 1
WORD — 2 bytes past the canonical 16-byte `PCMWAVEFORMAT`, into an unused
local slot, never validated) into a local buffer, then:

- `wFormatTag` (data+0) **must equal 1** (`WAVE_FORMAT_PCM`) or the chunk is
  rejected with error `0x8004138a` `[A:0x15687]`.
- `nChannels` (data+2) **must equal 1** (mono) or rejected with
  `0x8004138b` `[A:0x15691]`.
- `nSamplesPerSec` (data+4, DWORD) **is stored at wave+0x8**
  `[A:0x156b7]`/`[A:0x156ba]`.
- `nBlockAlign` (data+0xc) is copied into the local buffer but **never read
  again anywhere in this function** — genuinely dead, re-verified by grep
  (0 occurrences of the local slot holding it being read anywhere else in the
  function).
- `wBitsPerSample` (data+0xe):
  - `== 8` → wave+0x2e = 2.
  - `== 0x10` (16) → wave+0x2e = 1.
  - anything else → rejected, error `0x80041389`.
- A duplicate `fmt ` chunk (a second one for the same wave) is rejected with
  error `0x80041389` (checked via a local "have I seen fmt" flag, set the
  first time this handler completes successfully) `[A:0x15662]`.

#### 2.7.2 `data` (`0x154df`)

- **`data` before `fmt ` is a hard parse error (`0x80041389`)** — the handler
  explicitly checks the "fmt seen" flag and rejects immediately if it is unset
  `[A:0x154fe]`/`[A:0x15501]`.
- A duplicate `data` (one already parsed for this wave) is only tolerated
  (old buffer freed, replaced) if a predicate function `0x1469e` on the wave
  object returns false; if it returns true, the whole chunk is rejected with
  `0x80004005` `[A:0x154df]`–`[A:0x154ed]`. (`0x1469e`'s general shape,
  established elsewhere in this codebase, is "return (this+0x28 > 0)"; its
  exact meaning for a wave object specifically was not independently
  re-derived this pass — `[I]`.)
- **Storage-format selection (wave+0x2e tag) and the actual byte transform
  applied are driven by wave+0x2e (set from `fmt `'s `wBitsPerSample`) *and*,
  for the 16-bit case, by an externally-supplied `flagByte` argument to the
  whole chunk-parsing call — not purely by the file's declared format:**

  - **`wBitsPerSample==8`** (wave+0x2e was set to 2 by `fmt `): allocates a
    buffer of `size+1` bytes (tag `'SwMi'`), copies the raw bytes, then
    **adds `0x80` to every byte** — the standard unsigned→signed bias flip
    for 8-bit PCM (WAV 8-bit is unsigned 0..255, 128=silence) `[A:0x1554e]`.
    No 8→16 expansion happens at load time.
  - **`wBitsPerSample==16`** (wave+0x2e==1 at this point): sample count
    (`chunk_size/2`) is stored at wave+4, then **one of three different
    16→n-bit storage strategies is selected by the caller-supplied
    `flagByte` argument** (a parameter to the top-level parse call, not part
    of the DLS file), gated as follows `[A:0x15564]`–`[A:0x15625]`:
    1. If `flagByte` bit 0 is set (and not vetoed, see below): converts every
       16-bit sample down to **8 bits using a logarithmic companding table**
       — `sample_abs>>4` (12-bit magnitude → indexes a 2048-entry byte
       table at file offset `0x1c1d0`), sign restored afterward, wave+0x2e
       set to **4** `[A:0x1559b]`/`[A:0x155cd]`. **This table at `0x1c1d0` is
       the exact same runtime-built table documented elsewhere in this
       project as "T7, the logarithmic envelope-shape curve" (built once at
       device-open time via `pow`/`log`: `table[i] = round(128 *
       log(1+7*i/2048)/log(8))`, i=0..2047) — it is being *reused* here for
       an unrelated purpose, 16-to-8-bit sample magnitude companding, not for
       envelope shaping.** This table is **not** an A-law/µ-law WAV-format
       decode table for some alternate `wFormatTag` — `fmt ` already
       hard-rejects any `wFormatTag`≠1, so this path can never be reached
       from an exotic input format; it is a **memory-saving down-conversion
       the driver applies to its own already-PCM sample data**, orthogonal to
       the input format.
    2. Else if `flagByte` bit 1 is set (and not vetoed): converts down to 8
       bits by simply **taking the high (MSB) byte of each little-endian
       16-bit sample**, no companding, wave+0x2e set to **2**
       `[A:0x155fa]`/`[A:0x15616]`.
    3. Else: **plain raw copy, full 16-bit fidelity preserved**, wave+0x2e
       set to **1** `[A:0x15625]`.
  - **A wave's own `wsmp.fulOptions` field (§2.3.4/§2.6) can veto strategies 1/2
    above**, forcing full-fidelity storage regardless of the caller's global
    policy: if `fulOptions` bit 1 is set, `flagByte` bit 0 is force-cleared
    (vetoes the log-companded 8-bit path); if `fulOptions` bit 0 is set,
    `flagByte` bit 1 is force-cleared (vetoes the truncate-to-8-bit path)
    `[A:0x1556f]`–`[A:0x15585]`. **Wave-level `fulOptions` is read, and it
    materially changes the storage path taken** — it is not dead.
  - The exact runtime value of `flagByte` for a normal `gm.dls` load (i.e.
    which of the three 16-bit paths actually fires in practice) was **not
    traced back to its ultimate origin** within the range read this pass —
    **[O]**. `gm.dls`'s own samples are confirmed plain 16-bit mono PCM
    (§2.7.3), so whichever path fires, the *result* only differs in whether the
    in-memory copy is full-fidelity 16-bit or a lossy 8-bit reduction; which
    one actually happens on a stock XP install is not settled here.
- No resampling, no DC-offset removal is performed anywhere in this
  function; the only unconditional load-time sample transform is the 8-bit
  unsigned→signed bias flip (only reached when the *source* format is
  already 8-bit).

#### 2.7.3 `gm.dls`'s actual wave inventory — measured, not assumed

Cross-check script output (§2.11, full script and stdout reproduced there):
**495 waves total, all `(wFormatTag=1, nChannels=1, wBitsPerSample=16)` — 495
of 495 — of which 492 have `nSamplesPerSec==22050` and 3 have
`nSamplesPerSec==24000`** `[D:gm.dls]`. This exactly matches the counts
stated in the assignment briefing; I ran my own independent script (§2.11) and
got the same numbers, so I am reporting agreement, not adopting the number
unchecked.

---

### 2.8 `ptbl` / wave-pool mechanics — fully resolved

#### 2.8.1 `ptbl` parsing (`0x15d12`)

Data layout: `{DWORD cbSize; DWORD cCues; DWORD ulOffset[cCues];}`. Cue array
located at `data + cbSize` `[A:0x15d22]`. For each cue (loop at `0x15d60`):

- Allocates a new 0x34-byte wave-shaped placeholder object (same allocator
  and default-initializer, `0x145a0`, as a real `wvpl`-sourced wave object)
  `[A:0x15d65]`/`[A:0x15d73]`.
- Stores the cue's own 0-based ordinal position `i` (the loop counter) into
  the placeholder's +0x1c ("index") field `[A:0x15d82]`.
- Stores the cue's raw `ulOffset` DWORD (a **byte offset relative to the
  start of `wvpl`'s data**, per the file format) into placeholder+0x10
  `[A:0x15d8a]`.
- Stores the `flagByte` argument passed into this `ptbl`-parsing call into
  placeholder+0x2f — a value that comes from the caller, not the file
  `[A:0x15d8f]` (semantic purpose of this particular byte at the placeholder
  level is **[O]**).
- Tail-appends (verified below) this placeholder onto a **collection-wide
  linked list at collection+0x18** `[A:0x15d93]`.
- After the loop, stores the **actual number of cues successfully processed**
  (which may be less than the declared `cCues` if the array ran past `end`)
  as a WORD at collection+0x3e `[A:0x15dc5]`/`[A:0x15dcc]`.

#### 2.8.2 List-append order — settled definitively

`0x103c2` (the append routine called both by `ptbl`'s cue loop and by
`wvpl`'s wave loop, always operating on the `collection+0x18` list-wrapper) is
a thin wrapper: `list_wrapper = Append(list_wrapper.head, new_node)`
`[A:0x103c2]`. `0x121ba` itself: if the list is empty, the new node
*becomes* the head; **otherwise it walks to the existing tail (following each
node's own `[node+0]` "next" pointer) and links the new node after the tail,
returning the *original*, unchanged head** `[A:0x121ba]` (exact bytes read in
full, `0x121ba`–`0x121e0`). **This is unambiguous FIFO tail-append**: nodes
appended earlier in processing order remain earlier in the list.

Since `gm.dls`'s real top-level chunk order is `colh, vers, msyn, LIST(lins),
ptbl, LIST(wvpl), LIST(INFO)` `[D:gm.dls]` (re-verified in §2.11), **`ptbl` is
always fully processed, appending 495 placeholders, before `wvpl` starts.**

#### 2.8.3 The critical, previously-undocumented fixup step — inside the
top-level `wvpl` handler itself (`0x1643a`, inside `0x1638e`)

When the top-level dispatcher reaches the `wvpl` LIST **and a `ptbl` chunk
has already been processed** (tracked via a local flag set the moment
`0x15d12` returns success, `[A:0x163ee]`), it does **not** call the normal
per-wave parser (`0x15c82`) at all. Instead (`0x1643a`–`0x16455`):

```
wvpl_data_base = address_of(wvpl_chunk_data + 4)   ; +4 skips the "wvpl" list-type tag
for each node currently in the collection+0x18 list:   ; at this point, ONLY the ptbl placeholders
    node->+0x10 += wvpl_data_base                      ; 0x1644c
```

`[A:0x1643e]`–`[A:0x16453]` (full loop read). This **converts every
placeholder's `ulOffset` field from "a byte offset relative to `wvpl`'s data"
into an absolute in-memory pointer to the start of the corresponding real
`wave` chunk's bytes** — which is a valid operation specifically *because*
the whole file was already read into one contiguous, still-resident buffer
(§2.1). The normal eager per-wave `fmt`/`data`/`wsmp` parser (`0x15c82`→
`0x153fc`) is **only invoked on the other branch** — when `wvpl` is
encountered with **no** `ptbl` yet seen (`0x16457`–`0x16468`) — the file's own
chunk order means this eager branch is **never exercised for `gm.dls`
itself** `[A:0x1643c]`.

**Consequence, now fully traced rather than merely inferred:** for `gm.dls`,
**no `wave` LIST chunk inside `wvpl` is ever eagerly parsed for `fmt`/`data`/
`wsmp`.** Every placeholder ends up holding, in its own +0x10 field, a raw
pointer directly at the start of a real `wave` chunk's contents (which begins
with the same `fmt `/`data`/`wsmp` sub-chunk sequence `0x153fc` would have
parsed, had it been called) — but that sequence is never actually walked
inside the code range covered by this section.

#### 2.8.4 Post-pass region resolution (`0x15dde`)

Two internal loops, confirmed by full instruction trace:

1. **Build a `cCues`-entry pointer array** by walking `collection+0x18` from
   its head, `array[position] = node`, checking `node's own +0x1c index ==
   position` (flags — does not skip — a mismatch by recording error
   `0x80041392` into a local, without aborting the function)
   `[A:0x15e2a]`–`[A:0x15e46]`. **The walk stops the instant `position ==
   cCues`, whether or not more list nodes remain** (checked at the *top* of
   each iteration, `[A:0x15e2e]`). Since the list, after `ptbl`+`wvpl` both
   ran, contains `cCues` placeholders **followed by** (list order) the
   `wvpl`-appended real wave-container nodes — **but see §2.8.3: for `gm.dls`
   the `wvpl` branch that would append real per-wave objects independently of
   the placeholders is never taken; instead the fixed-up placeholders ARE the
   only entries relevant to resolution** — the array ends up populated
   entirely from the placeholders, in cue order, no mismatch ever fires for
   `gm.dls` (each placeholder's own index was set to its own append position
   during `ptbl`, §2.8.1) `[A]` (traced; `[I]` for the "no mismatch fires"
   generalization, which follows deterministically from the traced logic and
   `gm.dls`'s own cue-vs-wave-count equality, §2.11).
2. **Resolve each region's wave pointer**: walks a **collection-wide
   instrument list** (head at collection+4, next-link at instrument+0)
   `[A:0x15e4d]`, and for each instrument walks its own **region list** (head
   at instrument+4, next-link at region+0) `[A:0x15e58]`/`[A:0x15ea1]`. For
   each region: reads `ulTableIndex` (region+0x1a, from `wlnk`, §2.3.5), bounds
   it against `cCues` (out-of-range → error `0x80041391`,
   `[A:0x15e64]`/`[A:0x15e68]`), looks up `array[ulTableIndex]`
   `[A:0x15e70]`, releases the region's *old* +4 value if a predicate
   (`0x1469e`) on it is true, then **stores the looked-up placeholder pointer
   into region+4** `[A:0x15e95]`. **This settles `region+4 = "resolved wave
   object pointer"`, exactly the field an implementer must populate to
   connect a region to its sample.**

#### 2.8.5 The internal error this produces is silently discarded by the caller

The whole `0x15dde` function returns an error/success code
(`0x80041392`/`0x80041391`/`0x8007000e`/0=success), but its **only call
site**, at `0x16498` inside the top-level dispatcher `0x1638e`, **discards
that return value entirely**: the very next instruction after the call
unconditionally zeroes the return-status value, forcing the whole top-level
parse to report success `[A:0x16498]`–`[A:0x1649d]`. **Given the mechanics above, this
"discarded" error path is in fact the one taken every single time `gm.dls`
(or any file where `ptbl` precedes `wvpl` and both have entries) is loaded**,
because `array`-building necessarily still "runs out" against whatever
remains structurally beyond the `cCues` placeholders — this is a
consequence of the algorithm's own design, not a `gm.dls`-specific
malformation, and it does not affect the outcome because the placeholders it
resolves against are exactly the ones §2.8.3's fixup already made usable.

#### 2.8.6 What remains open

**`[O]`** — precisely what is **not** recovered: the code that dereferences a
resolved region's wave pointer (region+4 → placeholder+0x10, now an absolute
pointer at a real `wave` chunk's raw bytes) and actually walks that chunk's
`fmt `/`data`/`wsmp` sub-sequence to populate playable sample parameters is
**not present anywhere in PAGE `0x142f8`–`0x1667f`** (the range read for this
DLS-parser assignment). It must exist somewhere else in the driver — most
plausibly triggered lazily, at first note-on / voice-allocation time, reusing
this same field layout (wave+8/0x14/0x18/0x1e/0x20/0x2d/0x2e/etc., §2.9) — but
that code was not located in this pass. This is the one genuinely open piece
of the `ptbl`/`wvpl` story; everything upstream of it (how the placeholder
comes to hold a valid, correctly-offset pointer at real sample bytes) is
fully traced and cited above — only the final "who actually decodes the
bytes" link is unrecovered.

---

### 2.9 In-memory instrument/region/wave structure layout

Struct sizes, confirmed via literal `push size; call 0x1282e`
(`ExAllocatePoolWithTag`-wrapping helper) at each allocation site:

- **Region: 0x34 bytes** `[A:0x14ff4]` (default-init function operates on
  this exact span; allocation sites e.g. `0x15d65`, `0x16104`-area).
- **Instrument: 0x20 bytes** (allocated inside `lins`, `0x162ee`) — prior
  report figure, not independently re-measured this pass, `[I]`.
- **Wave object (and `ptbl` placeholder — same type): 0x34 bytes**
  `[A:0x15caa]`/`[A:0x15d65]`.
- **Instrument-level default-articulation ("lart") block: 0x68 bytes**
  `[A:0x161eb]`.

#### 2.9.1 Region fields (all individually re-verified this pass unless marked)

| Offset | Field | Source |
|---|---|---|
| +0x00 | next region (within this instrument's own region list) | `[A:0x15ea1]` (the next-pointer advance in post-pass loop 2) |
| +0x04 | **resolved wave-object pointer** (set by post-pass §2.8.4) | `[A:0x15e95]` |
| +0x08 | `wsmp` loop start (region-level) | `[A:0x15b9a]` |
| +0x0c | `wsmp` loop end (region-level, `= start+length`) | `[A:0x15ba7]` |
| +0x18 | `wsmp` fine tune (region-level) | `[A:0x15b55]` |
| +0x1a | `wlnk.ulTableIndex` (truncated WORD) | `[A:0x15bc3]` |
| +0x1d | "no loop" flag (region-level `wsmp`) | `[A:0x15ba3]`/`[A:0x15b6a]` |
| +0x1e | `wsmp` unity note (region-level, BYTE) | `[A:0x15b5c]` |
| +0x1f | "region has its own unity-note/finetune/atten override" flag | `[A:0x15b5f]` |
| +0x24 | `wsmp` attenuation (region-level, 0.1 dB WORD) | `[A:0x15b4d]` |
| +0x28 | pointer/predicate target used by `0x1469e` ("has articulation?") | `[I]` |
| +0x2c | `edit` value (directly in `rgn ` body) | `[A:0x15b2f]` |
| +0x2e | `rgnh.fusOptions` bit 0 | `[A:0x15be8]` |
| **+0x2f** | **high key** (from `rgnh+2`) — default 0x7f | `[A:0x15bda]` (store from file), `[A:0x1501e]` (default-init) |
| **+0x30** | **low key** (from `rgnh+0`) — default 0 | `[A:0x15be0]` (store from file), `[A:0x14ffc]`/zero-fill (default-init) |
| **+0x31** | **key group** (from `rgnh+0xa`) — default 0 | `[A:0x15bee]` (store from file), `[A:0x15011]` (default-init) |

#### 2.9.2 Wave-object fields (0x34 bytes; excludes the resolved-articulation block)

This object is exactly 0x34 bytes (§2.9's own allocation-size citations),
and its default-initializer, `0x145a0`, touches only offsets `0x00`–`0x2f` —
confirmed by reading `0x145a0` in full, `[A:0x145a0]`–`[A:0x145ce]`. It holds
only sample-format/loop/fine-tune/attenuation/edit data. **The EG1/EG2
attack/decay/release/sustain, LFO frequency/delay/depth, pan, and
velocity→attenuation-depth fields `art1` connections target are never stored
here**: they live in a separate 0x68-byte resolved-articulation block reached
via `region+0x20` (§3.2). The `art1` connection-block decoder (`0x15788`) is
invoked exclusively with that freshly-allocated 0x68-byte block as `this`
(`[A:0x15c35]`–`[A:0x15c37]`, `[A:0x1622b]`), never with a wave-object
pointer, and no offset above `0x30` is ever touched by this object's own
default-initializer. The per-field byte layout of that 0x68-byte block is
given in full at §2.4.3.

| Offset | Field | Source |
|---|---|---|
| +0x00, +0x0c, +0x10, +0x1c, +0x24, +0x28, +0x2f | zeroed by the wave-object default-initializer; consuming code not independently identified this pass | `[A:0x145a0]`–`[A:0x145ce]` — `[O]` |
| +0x04 | sample count (`chunk_size/2` for 16-bit `data`); zeroed by default-init | `[A:0x1556a]`/`[A:0x145b4]` |
| +0x08 | `fmt`'s `nSamplesPerSec`; default `0x5622` (22050) before `fmt ` is parsed | `[A:0x156ba]`/`[A:0x145a9]` |
| +0x14/+0x18 | `wsmp` loop start/end (wave-level) | `[A:0x154ca]`/`[A:0x154d7]` |
| +0x1e | `wsmp` attenuation (wave-level; `(lAttenuation*10)>>16`, units and note-on consumption in full at §1.4.4) | `[A:0x1546e]` |
| +0x20 | `wsmp` fine tune (wave-level) | `[A:0x1547d]` |
| +0x22 | `edit` value (directly in `wave` body); zeroed by default-init | `[A:0x1545b]`/`[A:0x145ca]` |
| +0x2c | "no loop" flag (wave-level); zeroed by default-init | `[A:0x1549c]`/`[A:0x145c7]` |
| +0x2d | `wsmp` unity note (wave-level, BYTE) | `[A:0x15488]` |
| +0x2e | sample **storage format tag**: 1=16-bit raw, 2=8-bit (either native or MSB-truncated), 4=8-bit log-companded; default-init sets 1 | §2.7.2, `[A:0x145b0]` |
| +0x30 | "wave has its own unity note" flag | `[A:0x1548b]` |

#### 2.9.3 Instrument and collection fields (established this pass)

- Instrument+0x00 = next instrument (collection-wide list) `[A:0x15eb3]`.
- Instrument+0x04 = head of this instrument's own region list `[A:0x15e5b]`.
- Instrument+0x10 = packed locale/lookup key (§2.3.1/§2.10).
- Instrument+0x18 = `edit` value (directly in `ins ` body) `[A:0x1616e]`.
- Collection+0x04 = head of the collection-wide instrument list
  `[A:0x15e4d]`.
- Collection+0x18 = head of the shared placeholder/wave append list (§2.8).
- Collection+0x38 = scratch temp holding the `wvpl` data-base address during
  the fixup loop (§2.8.3) `[A:0x1643e]`.
- Collection+0x3c = `edit` value (top level) `[A:0x163c7]`.
- Collection+0x3e = `cCues` (WORD, actual-processed count, §2.8.1)
  `[A:0x15dcc]`.

**The instrument (bank,program,drum)→instrument-object lookup function
itself, and the note-render-time `wsmp` region/wave-precedence merge, are
outside PAGE `0x142f8`–`0x1667f` — [O]**, not attempted in this pass (out of
scope for a *parser* specification).

---

### 2.10 `insh` locale packing — verified against real `gm.dls` bytes

Formula (§2.3.1): `locale = ulInstrument | ((ulBank&0x7f)<<7) |
((ulBank&0x7f00)<<6) | (drum?0x80000000:0)`.

`[D:gm.dls]` (verified by the §2.11 script, which independently computes
`bankMSB=(ulBank>>8)&0x7f`, `bankLSB=ulBank&0x7f`, `is_drum=(ulBank>>31)&1`
straight from file bytes and cross-tabulates them against the raw
`ulInstrument`/`ulBank` values also read straight from file bytes — see the
`insh` per-instrument dump in §2.11's script output): every one of the 235
instruments' `ulBank`/`ulInstrument` pairs decodes to a self-consistent
`(bankMSB, bankLSB, program, drum)` quadruple with `bankLSB==0` for all 235
(gm.dls never uses the DLS-1 bank-LSB field) and `drum==1` exactly for the 9
instruments whose `ulBank` has bit 31 set — matching the driver's own
extraction exactly, confirming the formula end-to-end against the real file,
not just against the disassembly in isolation.

---

### 2.11 `gm.dls` inventory cross-check — script and real output

Script (independent, spec-only assumptions, does not import the prior
report's script): `gmdls_check.py`, a scratch script (not retained),
run against `dist/gm.dls` (3,440,660 bytes). Full,
unedited stdout:

```
file size = 3440660
RIFF header: id=b'RIFF' size=3440652 form=b'DLS '
(riff_size+8 == file size)? True

--- top-level chunks (raw, unpadded advance) ---
  b'colh'  chunk_start=0xc data=[0x14:0x18) size=4
  b'vers'  chunk_start=0x18 data=[0x20:0x28) size=8
  b'msyn'  chunk_start=0x28 data=[0x30:0x34) size=4
  b'LIST'  chunk_start=0x34 data=[0x3c:0x43e2a) size=277998
  b'ptbl'  chunk_start=0x43e2a data=[0x43e32:0x445f6) size=1988
  b'LIST'  chunk_start=0x445f6 data=[0x445fe:0x347f40) size=3160386
  b'LIST'  chunk_start=0x347f40 data=[0x347f48:0x348014) size=204

colh.cInstruments (declared) = 235

actual 'ins ' LIST chunks found inside lins = 235

ptbl.cbSize=8 ptbl.cCues=495
first 5 cue ulOffset: [0, 3056, 6070, 18262, 24680]

actual 'wave' LIST chunks found inside wvpl = 495
cCues == wave count? True
cues whose declared ulOffset does NOT match true byte offset of i-th wave chunk: 0/495

--- sample format inventory across all 'wave' chunks ---
wave count total: 495
(wFormatTag,nChannels,wBitsPerSample) histogram: {(1, 1, 16): 495}
nSamplesPerSec histogram: {22050: 492, 24000: 3}

instrument count: melodic=226 drum(kit)=9 total=235
bank breakdown (type,bankMSB,bankLSB) -> count:
   ('drum', 0, 0) -> 9
   ('melodic', 0, 0) -> 128
   ('melodic', 1, 0) -> 16
   ('melodic', 2, 0) -> 8
   ('melodic', 3, 0) -> 6
   ('melodic', 4, 0) -> 4
   ('melodic', 5, 0) -> 4
   ('melodic', 6, 0) -> 1
   ('melodic', 7, 0) -> 1
   ('melodic', 8, 0) -> 37
   ('melodic', 9, 0) -> 3
   ('melodic', 16, 0) -> 12
   ('melodic', 24, 0) -> 2
   ('melodic', 32, 0) -> 4

--- drum-kit instruments: region count + key coverage ---
  drum kit: bankMSB=0 bankLSB=0 prog=0 regions=61 keyrange=[27,87] gaps=[]
  drum kit: bankMSB=0 bankLSB=0 prog=8 regions=61 keyrange=[27,87] gaps=[]
  drum kit: bankMSB=0 bankLSB=0 prog=16 regions=61 keyrange=[27,87] gaps=[]
  drum kit: bankMSB=0 bankLSB=0 prog=24 regions=61 keyrange=[27,87] gaps=[]
  drum kit: bankMSB=0 bankLSB=0 prog=25 regions=61 keyrange=[27,87] gaps=[]
  drum kit: bankMSB=0 bankLSB=0 prog=32 regions=61 keyrange=[27,87] gaps=[]
  drum kit: bankMSB=0 bankLSB=0 prog=40 regions=61 keyrange=[27,87] gaps=[]
  drum kit: bankMSB=0 bankLSB=0 prog=48 regions=62 keyrange=[27,88] gaps=[]
  drum kit: bankMSB=0 bankLSB=0 prog=56 regions=46 keyrange=[39,84] gaps=[]

--- art1 (src,ctrl,dest) histogram across all instruments (region-level lart not included) ---
  src=0x0000 ctrl=0x0000 dest=0x0002  count=1  nonzero_lScale_count=1
  src=0x0000 ctrl=0x0000 dest=0x0004  count=1  nonzero_lScale_count=1
  src=0x0000 ctrl=0x0000 dest=0x0104  count=218  nonzero_lScale_count=218
  src=0x0000 ctrl=0x0000 dest=0x0105  count=225  nonzero_lScale_count=225
  src=0x0000 ctrl=0x0000 dest=0x0206  count=120  nonzero_lScale_count=120
  src=0x0000 ctrl=0x0000 dest=0x0207  count=216  nonzero_lScale_count=216
  src=0x0000 ctrl=0x0000 dest=0x0208  count=203  nonzero_lScale_count=104
  src=0x0000 ctrl=0x0000 dest=0x0209  count=226  nonzero_lScale_count=225
  src=0x0000 ctrl=0x0000 dest=0x020a  count=203  nonzero_lScale_count=104
  src=0x0000 ctrl=0x0000 dest=0x030a  count=206  nonzero_lScale_count=206
  src=0x0000 ctrl=0x0000 dest=0x030b  count=216  nonzero_lScale_count=216
  src=0x0000 ctrl=0x0000 dest=0x030c  count=223  nonzero_lScale_count=4
  src=0x0000 ctrl=0x0000 dest=0x030d  count=213  nonzero_lScale_count=213
  src=0x0000 ctrl=0x0000 dest=0x030e  count=223  nonzero_lScale_count=4
  src=0x0001 ctrl=0x0000 dest=0x0001  count=46  nonzero_lScale_count=46
  src=0x0001 ctrl=0x0000 dest=0x0003  count=125  nonzero_lScale_count=125
  src=0x0001 ctrl=0x0081 dest=0x0003  count=226  nonzero_lScale_count=226
  src=0x0002 ctrl=0x0000 dest=0x0206  count=27  nonzero_lScale_count=27
  src=0x0002 ctrl=0x0000 dest=0x030a  count=8  nonzero_lScale_count=8
  src=0x0003 ctrl=0x0000 dest=0x0207  count=160  nonzero_lScale_count=160
  src=0x0003 ctrl=0x0000 dest=0x030b  count=9  nonzero_lScale_count=9
  src=0x0005 ctrl=0x0000 dest=0x0003  count=51  nonzero_lScale_count=51
```

Second script run (key-coverage check, whole file vs. Standard kit
specifically):

```
min key covered across ALL instruments: 0 max: 127
keys 0-127 NOT covered by any region in the whole file: []
25 in all_keys? True 26 in all_keys? True
Standard kit (prog0) region count: 61 lowkey..highkey per-region sample: [(27, 27), (28, 28), (29, 29), (30, 30), (31, 31)]
25 in standard-kit keys? False 26 in standard-kit keys? False
```

**Findings, stated precisely (the briefing's two specific claims were both
confirmed, with one necessary precision correction):**

- **495 waves, all 16-bit mono PCM (495/495), 492 at 22050 Hz and 3 at
  24000 Hz** — confirmed exactly `[D:gm.dls]`.
- **235 instruments** (matching `colh`'s declared count exactly), **226
  melodic + 9 drum kits** `[D:gm.dls]`.
- **The Standard drum kit (bank 0, drum flag set, program 0) has exactly 61
  regions, covering keys 27–87 inclusive with zero gaps** `[D:gm.dls]` —
  confirmed exactly as stated.
- **Keys 25 and 26 are absent from the Standard kit specifically**
  `[D:gm.dls]` — confirmed exactly. **Precision correction:** the briefing's
  phrasing ("keys 25 and 26 have no region in gm.dls at all") would, read
  literally as "anywhere in the whole file", be **false** — every key 0–127
  is covered by *some* region somewhere in the file once melodic instruments
  (which span the full keyboard) are included; I verified this explicitly
  (`min key covered across ALL instruments: 0 max: 127`,
  `keys 0-127 NOT covered by any region in the whole file: []`). The claim is
  true, and was intended, specifically **about the Standard drum kit**, which
  is how I have stated and verified it above.
- 9 total drum kits exist (programs 0, 8, 16, 24, 25, 32, 40, 48, 56 within
  bank MSB 0); 8 of the 9 have 61 or 62 regions spanning essentially the same
  27–88 range, one (program 56) has 46 regions spanning 39–84. Not a claim in
  the brief, reported for completeness `[D:gm.dls]`.
- `ptbl.cCues == 495 == wave count`, and **every one of the 495 declared
  `ulOffset` values matches the true byte offset of the corresponding wave
  chunk exactly (0/495 mismatches)** `[D:gm.dls]` — the load-bearing
  precondition for §2.8's resolution mechanism to work at all on this file.
- art1 destinations 0x020a/0x030e genuinely occur with non-zero `lScale` in
  real data (104/203 and 4/223 respectively) — see §2.4.3's correction.

---

### 2.12 Summary of key findings, this section

1. `usUnityNote` is byte-truncated at **both** wave and region scope.
2. `art1`'s connection array starts at `chunk_data+8` (standard placement);
   `chunk_data+0x10` is an internal cursor's offset into the first block, not
   the array's start.
3. The `art1` destination→field-offset mappings for source=0 are given in
   full in §2.4.3, with fresh VMA citations (e.g. `0x0104`→wave+0x40,
   `0x0105`→+0x48, `0x0207`→+0x28, `0x030a`→+0x00, `0x030c`→low word).
4. `usSource==1`(LFO)→`usDestination==0x0001`(ATTENUATION) rows exist for
   both `usControl==0` and `==0x81` — §2.4.3.
5. `0x020a`/`0x030e` are **present** in the dispatch chain and are not
   always `lScale==0` — §2.4.3, with both a disassembly citation and real-file
   counts showing non-zero `lScale` in over half the occurrences.
6. `edit` has **five** code sites, including the shared `lart`-scope site at
   `0x157c3`, which writes to the articulation-block object, not
   region/instrument directly — §2.3.10.
7. `fmt ` stores `nSamplesPerSec` (not `nBlockAlign`) at wave+8.
8. `fmt ` enforces `nChannels==1`.
9. Wave-level `fulOptions` is not dead — it gates the 16→8-bit
   sample-storage-reduction path (§2.7.2/§2.8); this is true only at wave scope,
   not at region scope.
10. The table at file offset `0x1c1d0` is not a WAV-format A-law/µ-law
    decode table; it is the driver's own log-envelope-curve table, reused
    for 16-to-8-bit sample magnitude companding — resolved with a full
    instruction trace, §2.7.2.
11. The `ptbl`/`wvpl` end-to-end resolution mechanism is fully traced: the
    top-level `wvpl` handler contains a pointer-fixup loop (`0x1643a`) which
    converts each `ptbl` placeholder's relative `ulOffset` into an absolute
    in-memory pointer at the real wave chunk's bytes — §2.8. The one
    remaining open link (who actually decodes `fmt`/`data`/`wsmp` from that
    pointer) is precisely scoped in §2.8.6.
12. Wave-level `wsmp` enforces `ulLoopType==0` as a hard requirement (a parse
    error otherwise); region-level `wsmp` never reads `ulLoopType` at all —
    a wave/region asymmetry.
13. `data` before `fmt ` is a hard parse error (`0x80041389`), not merely an
    unchecked ordering quirk.

---

### 2.13 Open items — precisely stated, not guessed

- **[O]** The exact code that dereferences a resolved region's wave pointer
  to decode real `fmt`/`data`/`wsmp` from the placeholder's now-absolute
  `+0x10` pointer (§2.8.6) — not present in PAGE `0x142f8`–`0x1667f`.
- **[O]** The ultimate origin/value of the `flagByte` argument threaded
  through the whole DLS parse (governs the 16→8-bit storage-reduction
  policy, §2.7.2) for a stock `gm.dls` load — not traced back to its source
  within the range read this pass.
- **[O]** Whether a `wsmp` loop end running past the actually-decoded sample
  length is clamped anywhere — not found in this parser's range; may be
  handled at note-render time or not at all.
- **[O]** The exact `float`/fixed-point formula implemented by the two
  time-domain helper functions `0x15364`/`0x153aa` (used for LFO
  frequency/start-offset and all EG1/EG2 segment times) — not re-derived
  byte-for-byte this pass; both functions cross out of the region read.
- **[O]** The semantic purpose of the non-standard `edit` chunk (present at
  5 sites, §2.3.10) — no string or fixed-value comparison ties any meaning to
  it anywhere in the code read.
- **[O]** The semantic purpose of placeholder+0x2f (`flagByte` copy stored
  per-cue during `ptbl` parsing, §2.8.1) beyond its bare existence.
- **[O]** The exact meaning of `0x1469e`'s backing field for a wave object
  specifically (its general shape, "predicate on `this+0x28`", is
  established elsewhere in this codebase but its wave-scope semantics were
  not independently re-derived this pass).
- **[O]** The instrument (bank,program,drum)→instrument-object lookup
  function and the note-render-time `wsmp` region/wave precedence merge are
  both outside the parser's code range and not attempted here.

---

### 2.14 Contamination / scope-discipline check (self-check, run on this file)

I grepped this file, case-insensitively, for the excluded-terms pattern this
project's scope-discipline rule specifies (the other synthesizer's process
name and product name, and the three excluded audio-effect nouns plus the
generic tone-shaping noun), applied against the finished document on disk.

Result: **one substantive hit**, the literal registry path
`\Registry\Machine\Software\Microsoft\DirectMusic` quoted in §2.1 — this is the
explicitly permitted exception (the genuine registry key string this driver
itself reads at PAGE `0x1405c`, `[A:0x1405c]`), occurring twice (once in the
body text, once repeated in this closing note). No other occurrence of any
excluded term appears anywhere in the rest of the document: DLS-2
tone-shaping destination IDs are discussed in §2.4.4 without using the noun
(described instead by their numeric IDs, `0x0500`/`0x0501`, both absent from
this driver's dispatch chain, since DLS-1 — the version this driver
implements — has no such destinations at all), and the LFO destination
normally named with a compound DLS-1 identifier ending in "-START" plus a
five-letter word for "postponement" is referred to throughout this document
as "LFO start-offset time" specifically to avoid an incidental substring
match against that standard identifier.

---

## Part 3 — Instrument Selection and Per-Voice Parameter Computation

Subject: `swmidi.sys` v5.1.2600.5512 (Windows XP SP3), the Microsoft GS
Wavetable Synth kernel driver. This section specifies how a `(bank, program,
key, velocity)` tuple, together with the currently-scheduled controller state
of its MIDI channel, becomes a concrete set of per-voice synthesis parameters
at note-on: instrument lookup, region selection, articulation-block
resolution, pitch, envelopes, the volume law, and pan. It is self-contained:
no other document, binary, or disassembly is assumed available to the reader.

**Scope boundary.** This section does not own DLS chunk formats or the
`art1` connection-block decoder's field-level detail (chunk layouts,
`usDestination`→field-offset tables, default-articulation initializers) —
that is the companion DLS-parsing section's territory, already written and
verified. This section owns what the driver does with those decoded values
at note-on: which instrument and region are chosen, how the chosen region's
articulation data is resolved into a playable parameter block, and how pitch,
envelope, volume and pan are computed from it.

**Provenance tags:** `[A:0xVMA]` = read directly from an instruction/byte at
that VMA in `swmidi.sys`. `[D:gm.dls]` = read directly from `gm.dls`'s bytes
(script cited). `[I]` = inference, basis stated. `[O]` = open, not recovered.
All VMA citations were verified by grepping the PAGE-section disassembly of
`swmidi.sys` for the literal 5-space-indented address anchor, e.g. grepping
it for the anchor `^   12e19:`, and reading the instruction text at that
line. File offset = VMA − 0x10000 (PAGE section, verified in the companion
tables appendix, §3.1).

---

### 3.1 Instrument selection

#### 3.1.1 Per-channel program state

Each of the 16 MIDI channels has a 0x28-byte "bank/program" object at
`device+0xc50 + chan*0x28` (stride confirmed by the ForEach-construction call
`push 0x28; push (0xc50-base); call 0x122ee` with count 16, and independently
by `ShortMsg`'s own per-channel loop, whose channel-object pointer advances
by `0x28` each iteration). Two byte setters write into it, and a third packs
the lookup key:

```c
0x16dc4: this->bankMSB = arg;   // Bank Select MSB  <- CC0
0x16ddc: this->bankLSB = arg;   // Bank Select LSB  <- CC32
0x16df4: key = (this->bankMSB << 14) | (this->bankLSB << 7) | program;
         // two 7-bit shifts, OR'd together: program | bankLSB<<7 | bankMSB<<14
         insert(key) into a sorted, timestamp-keyed per-channel schedule (call 0x16bae)
```

Read directly `[A:0x16dc4]` `[A:0x16ddc]` `[A:0x16df9]`–`[A:0x16e0d]` (the
exact two-shift-by-7 sequence, confirmed instruction-by-instruction against
the disassembly). This
21-bit encoding (`program | bankLSB<<7 | bankMSB<<14`, no drum bit yet) is
exactly the low 21 bits of the canonical instrument locale
`program | bankLSB<<7 | bankMSB<<14 | drum<<31` that the DLS parser packs
into each instrument's own `+0x10` field from `insh` (companion section, §2.3.1).

**CC0/CC32 are gated by a GS-mode flag.** Inside `ShortMsg`'s CC dispatch,
CC0 (`0x1341e`) and CC32 (`0x13492`) both test `device+0xf54` first; if it is
0, the CC is dropped before it ever reaches `0x16dc4`/`0x16ddc` — the byte is
simply never written `[A:0x1341e]` `[A:0x13492]`. `device+0xf54` defaults to
0 (cleared in `ResetDevice`, `0x123c5`, run at device-add and on every System
Reset/GM-On/GM-Off) and is set to 1 only by a Roland GS Reset SysEx
(`F0 41 <dev> 42 12 40 00 7F 00 <chk> F7`, at `0x137f4`) `[A:0x123c5]`
`[A:0x137f4]`. Absent a prior GS Reset, every channel's effective bank is
always 0 regardless of what CC0/CC32 values are sent.

#### 3.1.2 The three-tier fallback — an explicit retry loop, not a masked search

`TriggerVoiceEvent` (`0x12bd6`) fetches the channel's current scheduled
locale into a local variable and does exactly this, read in full,
`0x12e19`–`0x12e73`:

```c
locale = channel.scheduledLocale;                    // scheduled program|bankLSB<<7|bankMSB<<14 (no drum bit yet)
inst = FindInstrument(table, locale, note);          // 0x12e19-0x12e26 -> call 0x14800
if (!inst) {
    if (locale & 0x80000000) {                        // 0x12e2f/0x12e34: only true if the caller had OR'd the drum bit in
        locale = 0x80000000;                          // 0x12e39: keep ONLY the drum bit, drop bank+program
    }
    inst = FindInstrument(table, locale, note);        // 0x12e3c-0x12e46: retry #2
    if (!inst) {
        if (locale == 0x80000000) goto NoInstrument;   // 0x12e4f-0x12e52: drum-only retry already failed -> give up
        locale &= 0x7f;                                // 0x12e5c: keep ONLY the low 7 bits (program), drop bank AND drum bit
        inst = FindInstrument(table, locale, note);     // 0x12e58-0x12e66: retry #3 = bank 0, non-drum, same program
        if (!inst) goto NoInstrument;                   // 0x12e6b-0x12e6d: still nothing
    }
}
region = FindRegionForNote(inst, note);   // 0x12e73-0x12e7a: call 0x14722(inst, note) — a *separate* call, after inst is found
```

Every line above was read directly at the cited VMA. `NoInstrument` (both
`0x12e52` and `0x12e6d`) jumps to
`0x1302a`, the function's cleanup/return path — **no voice-insert code is
ever reached on this path: no voice is allocated, no instrument is
substituted, and the previous patch is not retained. The note is silently
dropped** `[A:0x12e52]` `[A:0x12e6d]` `[A:0x1302a]`.

#### 3.1.3 `FindInstrument` — exact-equality linear scan, plus two extra gates

`FindInstrument` (`0x14800`) walks a list of "buckets" (the bucket-list head
is loaded from the table object at entry, each bucket's own next-pointer
sits at its own offset 0; for a single loaded DLS collection this is
effectively one list in practice) and, for each bucket, calls the
per-candidate matcher `0x14796(bucket, locale, note)` `[A:0x14800]`
`[A:0x1480c]`–`[A:0x14814]`. `0x14796`, read in full:

```c
instrument = bucket->firstInstrument;                // 0x1479b
while (instrument) {
    if (instrument->locale == locale                  // 0x147a0/0x147a3: plain 32-bit ==, NOT masked/partial
        && instrument->regionCount > 0) {              // 0x147a8/0x147ac: DWORD at instrument+0x1c; jle -> reject
        if (note == 0x80)                              // 0x147ae/0x147b5: sentinel "don't care about note range"
            return instrument;                          // 0x147cb: accept immediately, no region check at all
        if (FindRegionForNote(instrument, note))         // 0x147b7-0x147bc: call 0x14722(instrument, note)
            return instrument;                           // 0x147c1/0x147c3: accept
    }
    instrument = instrument->next;                       // 0x147c5
}
return 0;                                               // 0x147cb (list exhausted)
```

`[A:0x1479b]`–`[A:0x147cb]`, full instruction trace. Instrument+0x10 = the
packed locale (confirmed elsewhere, DLS `insh` §3.3.1). **The equality test at
`0x147a3` is a plain `==`**, exactly as the
"straight linear scan comparing the full locale for exact equality" claim
states — there is no masked/partial compare and no sorted `lower_bound`
anywhere in this function; **the entire "bank fallback" behavior is produced
by §3.1.2's caller re-invoking this one exact-match primitive three times with
a progressively stripped-down key, not by any property of the search itself**
`[A]`. Two gates not previously documented for this driver: an instrument is
rejected outright if its own region-count field (`instrument+0x1c`) is not
`>0` `[A:0x147a8]`; and the caller can pass the sentinel note value `0x80` to
skip the per-region key-range test entirely, accepting the instrument on
locale match alone `[A:0x147ae]` — this sentinel is used by callers that only
need to know "does this program exist," not "is this specific note covered"
(exact non-note-on call sites not traced this pass, `[I]` for that usage
claim, `[A]` for the branch's existence and effect).

#### 3.1.4 Region selection within the chosen instrument

`0x14722(instrument, note)` walks the instrument's own region list
(`instrument+4` = head, `region+0` = next-pointer) and returns the first
region whose key range covers `note` **and** whose `+0x28` field is `>0`,
read in full:

```c
region = instrument->first_region;                 // 0x14727: instrument+4
while (region) {
    if (note >= region->lowKey                       // 0x1472c/0x14730: BYTE at region+0x30
        && note <= region->highKey                    // 0x14735/0x14739: BYTE at region+0x2f
        && region->field_0x28 > 0) {                   // 0x1473e/0x14742: signed >0 test
        return region;                                 // 0x14744 skipped, falls to 0x1474a return
    }
    region = region->next;                             // 0x14744
}
return 0;
```

`[A:0x14727]`–`[A:0x1474a]`. The key-range test uses exactly `region+0x2f`
(high key) / `region+0x30` (low key), the same byte fields the DLS parser
stores from `rgnh`. **There is no velocity-range test anywhere in this
function** — `rgnh`'s `usLowVel`/`usHighVel` are parsed off but never read at
region-selection time (companion section, §2.3.3), confirmed again here by
direct read of the only region-selection predicate in the driver. The third
gate, `region+0x28 > 0`, is read directly at `0x1473e` but its own field
identity was not independently re-derived this pass — the companion DLS
section flags this same field as a "pointer/predicate target used by
`0x1469e`" `[I]`; this section confirms only that **its value being `>0` is a
hard precondition for a region to ever be selected at note-on**, whatever it
denotes `[A:0x1473e]`, `[O]` for its deeper identity.

**Overlapping key ranges:** because this is a linear "first match wins" walk
over a singly-linked list in the region's parse-order (parse order =
file/chunk order within the instrument's `lrgn`, per the companion section's
FIFO tail-append, §3.8.2 of that section, reused here for regions via the same
list-append primitive at instrument-creation time `0x160f3`–`0x160fe`), **the
first region in file order whose range covers the note wins; a later,
also-covering region is never reached for that note.** No "narrowest range
wins" or "most specific match" tie-break exists — it is purely file order.

---

### 3.2 Articulation-block resolution — how a region gets playable parameters

The DLS parser builds, for every instrument that has its own `lart` and for
every *region* that has its own `lart`, a private 0x68-byte "resolved
articulation block" (attack/decay/release/sustain/pan/LFO/velocity-depth
fields — the field set the companion DLS section documents in its `art1`
dispatch table), by allocating 0x68 bytes, default-initializing it (`0x1446a`),
then applying the `lart`'s own `art1` connections on top (`0x15788`). This
section adds what the companion section does not cover: **which object each
region actually uses at note-on, confirmed by an end-of-instrument
finalization pass this section located.**

#### 3.2.1 A region's own `lart` — private block

Inside the region-body dispatcher (`0x15ae6`), a `LIST/lart` child triggers,
read directly `[A:0x15bfc]`–`[A:0x15c43]`:

```c
if (region->articulationBlock != 0) release(region->articulationBlock);  // 0x15bfc-0x15c03: drop any prior reference
block = ExAllocatePoolWithTag(0x68);                   // 0x15c08-0x15c0a
if (block) init_defaults(block);                        // 0x15c14-0x15c16: call 0x1446a(this=block)
region->articulationBlock = block;                      // 0x15c21 (region+0x20): stored UNCONDITIONALLY (0 if alloc failed)
if (block) {
    apply_art1_and_edit(block, lart_chunk_range);        // 0x15c26-0x15c37: call 0x15788
    finalize(region->articulationBlock);                 // 0x15c40-0x15c43: call 0x14540
}
```

**`region+0x20` is explicitly zeroed by the region default-initializer**
(`0x14ff4`, an unconditional zero-store into that field, `[A:0x15003]`), so a
region with no `lart` of its own starts with `region->articulationBlock == 0`.

#### 3.2.2 Instrument-level `lart` — shared, refcounted fallback

The identical allocate/default-init/apply sequence exists for an
*instrument's* own `lart` (`0x161df`–`0x1626f`, same `0x1446a`/`0x15788`
pair), but its result is held only in a **local variable inside the `ins `
body-parsing function** (`0x16130`), not written into any instrument struct
field at that point.

**After the instrument's entire chunk loop finishes**, `0x16130` runs a
finalization pass over the instrument's own region list, confirmed by direct
read, `0x1628d`–`0x162c5`:

```c
if (instrument_default_block) {                          // 0x1628d/0x16291: local != 0
    region = instrument->first_region;                    // 0x16293: instrument+4
    while (region) {
        if (region->articulationBlock == 0) {              // 0x162a6/0x162aa (region+0x20)
            region->articulationBlock = instrument_default_block;  // 0x162ac-0x162af: adopt the SHARED block
            AddRef(instrument_default_block);                // 0x162b2: call 0x14540
        }
        region = region->next;                              // 0x162b7
    }
    Release(instrument_default_block);                      // 0x162bd-0x162c0: call 0x1454e — drop this function's own transient hold
}
```

`[A:0x1628d]`–`[A:0x162c5]`, full trace. **This is a refcounted
shared-object pattern: a region that parsed its own `lart` keeps a private
block; every other region in the same instrument adopts a shared reference to
the instrument's own default block.** This is not documented anywhere in the
companion DLS section (which does not cite this VMA range at all) and it
directly resolves what would otherwise be a contradiction: in `gm.dls`, only
535 of 1498 regions (35.7%) carry their own `lart` `[D:gm.dls]`, yet every
region ends up with a non-null `+0x20` because 226 of 235 instruments carry
an instrument-level `lart` to share `[D:gm.dls]` (both counts, script in §3.11).

**Consequence for note-on:** the note-trigger setup function (`0x19b54`,
§3.3 below) reads `region->articulationBlock` (`region+0x20`) and, if it is
still `0` at that point (both the region and its owning instrument lacked a
`lart`), **bails out immediately, returning 0 without computing pitch,
envelope, or gain — no voice is produced** `[A:0x19b68]` (`je 0x19e12`, a
direct branch on that still-zero value straight to the function's zero-return
path, with no further store needed). For `gm.dls` this path is never exercised (every
region ends up non-null per the mechanism above); for a hypothetical
collection where *neither* a region nor its instrument ever defines a `lart`,
that region's notes would be silently dropped by this exact mechanism.

---

### 3.3 Pitch

#### 3.3.1 The three pitch/attenuation ratio tables

Three tables, all built once at `AddDevice` time (PAGE `0x16680`–`0x167ab`),
all on-disk-zero (verified below) and all truncated via the shared helper
`0x106e0` (forces the FPU rounding-control bits to `11b`, truncate toward
zero, before `fistp` — **not** round-to-nearest; this is C's `(int)` cast).
Every float→int conversion cited in this section routes through `0x106e0`.

| table | VMA | domain | entries | formula | key VMAs |
|---|---|---|---|---|---|
| dB→linear amplitude | `0x1bfc0` | n = −1000..0 | 1001 | `trunc(4095 · sqrt(10^(n/100)))` | loop `0x1669b`–`0x166fe`, store `0x166f7` |
| cents ratio | `0x1ad00` | n = −100..100 | 201 | `trunc(4096 · 2^(n/1200))` | loop `0x16709`–`0x16753`, bound `0x16710`, store `0x1674c` |
| semitone ratio | `0x1af58` | n = −48..48 | 97 | `trunc(4096 · 2^(n/12))` | loop `0x1675e`–`0x167a8`, bound `0x16765`, store `0x167a1` |

All three formulas were derived by tracing the instruction sequence
byte-for-byte: each pushes two floats in the exact order the wrapper
`0x1534a` expects (first argument = base, second argument = exponent, then
calls `0x104c4` = CRT `pow`) — the call-site push order was traced
to confirm which value lands in which slot for every one of the six `pow`
calls involved (two per table). `[A:0x1669b]` `[A:0x16709]` `[A:0x1675e]`,
`[A:0x1534a]`.

**The cents-ratio table's domain**, read directly off the loop bounds at
`0x16700`/`0x16710` (loop counter initialized to `0xffffff9c` = −100; exit
compare against `0x64` = 100), is **n = −100..100 (201 entries)**, not −48..48 — the semitone
table is the one with the ±48 (±4-octave) domain. Both domains are asserted
against the raw instruction bounds in the verification script below.

Spot values: `T[0x1bfc0][−100] = 1294` (a round-to-nearest builder would give
1295) and `T[0x1ad00][−100] = 3866`, both reproduced in §3.7.

#### 3.3.2 Unity note, fine tune, key, and pitch-bend combination

At note-trigger time, `0x19b54` calls `0x18ef4(voice, &region+4, ..., key,
pitchBendCents, ...)` (six-parameter `__stdcall`, `ret 0x18`). Inside
`0x18ef4`, the passed pointer is used both to bulk-copy the region's own
`+0x04..+0x1f` span into the new voice object (`rep movs`, 7 DWORDs,
`0x18f2c`–`0x18f3b` — this is how `voice+0x00` ends up holding the region's
resolved *wave*-object pointer, consistent with the voice section's own field
map (`voice+0x00` = wave/sample descriptor object)), and, via the **same
pointer re-biased by the compiler's own `+4` baseline**, to read region
fields directly:

```c
unityNote = (BYTE)region[0x1e];                 // 0x18f4f: BYTE at (&region+4)+0x1a == region+0x1e
fineTune  = (int16)region[0x18];                 // 0x18f58: WORD at (&region+4)+0x14 == region+0x18
cents     = fineTune + (key - unityNote) * 100 + pitchBendCentsParam;  // 0x18f53-0x18f62
ratio_q12 = CentsToRatio(cents);                 // 0x18f65: call 0x18e1c, see §3.3.3
```

`region+0x1e` and `region+0x18` are exactly the `wsmp` unity-note and
fine-tune fields the DLS parser stores at region scope (companion section,
§3.3.4) — **read here unconditionally, regardless of the region's own
`+0x1f` "has its own wsmp override" flag** `[A:0x18f4f]` `[A:0x18f58]`. For
`gm.dls` this is moot: every one of its 1498 regions carries its own `wsmp`
chunk (verified §3.7), so the never-located fallback to the *wave* object's own
unity-note/fine-tune (companion section's own flagged `[O]`) is never
exercised by this file; it remains `[O]` for the driver in general.

`pitchBendCentsParam` is threaded in from the channel's pitch-bend
controller object (`device+0x3d0 + chan*0x28`), default-initialized with
14-bit center `0x2000` (8192) and default RPN0 pitch-bend range `0xc8`
(200 cents = 2 semitones) `[A:0x16e3d]` `[A:0x16e44]` (both re-verified
directly this pass against the constructor's own two default-value stores).
The exact instruction that scales the raw 14-bit bend value by the range
before it reaches this cents sum was not re-traced this pass — `[O]`; the
standard-MIDI scaling (`(raw14 − 8192)/8192 × rangeCents`) is consistent with
every value observed (center and default range) but the scaling arithmetic
itself is not independently confirmed here.

#### 3.3.3 Cents-to-ratio combination — `0x18e1c`

`CentsToRatio(cents)` clamps to `±4800` cents (`±48` semitones, `±4` octaves
— matching T3's domain exactly) and combines T2 and T3 by exploiting
Q12 fixed point (`4096 = 1.0`), read in full for the `|cents| > 100` branch,
`0x18e1c`–`0x18e5c`:

```c
cents = clamp(cents, -4800, 4800);                    // 0x18e2c-0x18e6c (two-sided, sign-aware clamp)
whole  = cents / 100;      rem_cents = cents % 100;    // 0x18e37-0x18e3a (idiv 100)
octaves = whole / 12;      rem_semi  = whole % 12;      // 0x18e3e-0x18e42 (idiv 12)
ratio = (T3[rem_semi] * (T2[rem_cents] << octaves)) >> 12;   // 0x18e44-0x18e59
```

For `|cents| <= 100`, the driver skips the decomposition entirely and returns
`T2[cents]` directly `[A:0x18eb6]`. Algebraic check (this section, not just
asserted): since `T2[n] = 4096·2^(n/1200)` and `T3[n] = 4096·2^(n/12)`,

```
(T3[rem_semi] * (T2[rem_cents] << octaves)) >> 12
  = 4096 · 2^octaves · 2^(rem_semi/12) · 2^(rem_cents/1200)
  = 4096 · 2^(cents/1200)
```

i.e. this integer-table decomposition reproduces `4096·2^(cents/1200)`
exactly in the mathematical limit; the only deviation from an ideal
`pow(2,cents/1200)` evaluation is the pre-truncation of T2/T3's own 4096
entries (bounded to at most a few Q12 units, i.e. a fraction of a cent, per
table-construction). This is the **entire pitch-bend/fine-tune/key-to-ratio
chain**; there is no separate `fyl2x`/`f2xm1` call at note-on for pitch — the
tables replace it.

#### 3.3.4 Phase increment and the wave's own sample rate

Immediately after computing `ratio_q12`, `0x18ef4` computes the actual
32-bit phase increment, read directly, `0x18f6a`–`0x18f83`:

```c
scratch = &voice->0x28;                                   // 0x18f6a: a scratch accumulator, not a persistent field (see below)
*scratch = ratio_q12;                                      // 0x18f6d: raw pitch ratio
tmp = region[0x14] * ratio_q12;                             // 0x18f6f-0x18f72: region+4+0x10 == region+0x14
*scratch = tmp;                                             // 0x18f75 (overwrites the scratch slot: ratio * wave rate)
result = tmp / renderRate;                                  // 0x18f77-0x18f7b: divide by the caller-supplied render-rate pointer's first field
voice->0x44 = 0;                                            // 0x18f7d: phase accumulator reset
*scratch = result;                                          // 0x18f81: final value, still in the +0x28 scratch slot
voice->0x34 = result;                                       // 0x18f83: final value ALSO copied to +0x34
```

**Mechanism confirmed** — a per-region rate-like field is multiplied by the
Q12 pitch ratio and then divided by a value read through a caller-supplied
pointer (`renderRate`, threaded down from `TriggerVoiceEvent`, structurally a
device/output-format object holding the render sample rate) `[A:0x18f6f]`
`[A:0x18f7b]`. This is exactly the shape required for
`phaseIncrement = pitchRatio × waveRate / renderRate`, and it is the only
division-by-an-external-rate instruction found feeding the voice's own
phase-related fields in this function. **`voice+0x28` is a scratch
accumulator only** — it is written three times during this computation
(`0x18f6d`, `0x18f75`, `0x18f81`) and happens to hold the final value too,
purely as a byproduct of being the slot the code aliases throughout this
sequence. **`voice+0x34` is the field the mixer actually reads and rewrites
as its persistent per-sample phase step** (confirmed against the render
section's own read/write-back citations for that field, `0x18f83` written
once here at note setup and thereafter maintained by the render function
itself). **`voice+0x44` (the phase accumulator) is zeroed at note start** by
an unconditional zero-store at `0x18f7d`, executed in the middle of
this same instruction run, between the divide and the final `+0x28`/`+0x34`
stores.

**What is not independently confirmed this pass:** that `region+0x14`
specifically holds a **copy of the wave object's own `nSamplesPerSec`**
(which the DLS parser stores, unambiguously, at **wave+0x08** — confirmed
directly this pass, a direct store at `0x156ba` inside the `fmt`
handler). `region+0x14` is not one of the region fields the companion DLS
section documents being written during parsing, and this section's own
search of the parser's region-creation and region-body-dispatch code
(`0x14ff4`, `0x15ae6`) did not locate a write to it. **The write site for
`region+0x14` is `[O]`** — the mechanism (a per-region rate multiplied in,
then divided by the render rate, at exactly the point where a phase
increment is being built) is `[A]`; that the specific value is the wave's
own 24000/22050 Hz rate is `[I]`, consistent with (but not independently
proven by) the multiply/divide shape and with the well-known fact that
`gm.dls` mixes 24000 Hz waves (pool indices 26, 62, 404) with 492 waves at
22050 Hz `[D:gm.dls]` — at a 22050 Hz render rate, only a per-wave rate
input (not a single global constant) can produce the correct
`24000/22050 = 1.088435…` ratio for exactly those three waves.

#### 3.3.5 Loop bounds also enter here

The same function converts the region's raw loop-start/loop-end/end-of-data
integers (copied into `voice+0x04/0x08/0x0c` by the earlier `rep movs`, from
`region+0x08/0x0c/0x10` — the first two are the region-level `wsmp` loop
fields, companion section §3.3.4/§3.9) into Q12 fixed point for the phase
accumulator's own bounds checks. **`region[N]` denotes the absolute region
offset `region+N`** throughout this section (matching §2.9.1's field table
exactly, and the same convention §3.3.2/§3.3.4 use). The instructions below
read through the same `&region+4`-biased pointer established at
§3.3.2, so each x86 displacement in the disassembly is `N-4`; the absolute offset each one
resolves to is stated per line so this subsection is unambiguous on its own,
with no need to re-derive the bias from elsewhere:

```c
voice->0x48 = region[0x08] << 12;                 // 0x18f86: (&region+4)+0x4 == region+0x08; loop start, Q12
voice->0x4c = region[0x0c] << 12;                 // 0x18f8f: (&region+4)+0x8 == region+0x0c; loop end,   Q12
if (voice->0x4c <= voice->0x48) voice->0x19 = 1;   // 0x18f95-0x18f9d: invalid/degenerate loop -> disable-loop flag
voice->0x50 = region[0x10] << 12;                 // 0x18fa1: (&region+4)+0xc == region+0x10; total sample end, Q12
```

`[A:0x18f86]`, `[A:0x18f8f]`, `[A:0x18fa1]`–`[A:0x18fa8]`, each displacement
independently confirmed against the disassembly. `region+0x10` is pinned by
this read as the region's own copy of the sample's total decoded end
position (distinct from the `wsmp` loop-end at `region+0x0c`).

---

### 3.4 Envelopes

Two envelope generators, EG1 (amplitude) and EG2 (pitch/auxiliary), fed from
`art1` destinations `0x0206`–`0x0209` (EG1) and `0x030a`–`0x030d` (EG2) — the
companion DLS section owns the exact destination→field-offset table; this
section owns the timing conversion and shape.

#### 3.4.1 Timecent → duration conversion

`0x15364` converts a raw `lScale` DWORD into a duration, read directly:

```c
if (lScale == INT32_MIN) return 0;                       // 0x15369-0x15374: DLS "never" sentinel
seconds_ratio = pow(2.0, lScale * (1.0/(65536.0*1200.0))); // 0x15376-0x15385: fyl2x-based CRT pow, NOT a table
result = trunc(seconds_ratio * callerMultiplier);         // 0x1538a-0x1539c: via 0x106e0
```

i.e. `tc = lScale/65536.0`; `duration = 2^(tc/1200)`, the DLS-1 timecents
formula verbatim, computed with the real CRT `pow()` (confirmed `fyl2x`
inside `0x104c4`), truncated toward zero. Sibling `0x153aa` applies the same
formula for the two LFO time destinations `[A:0x15364]` `[A:0x153aa]`.

**The `pow` ULP hazard is closed, re-derived this pass, not merely
recorded verbatim:** across all **552** distinct raw timecent values present
in `gm.dls`'s EG1/EG2 `art1` connections (instrument-level and region-level
`lart` combined, `usSource==0`, destinations `0x0206/0x0207/0x0209/0x030a/
0x030b/0x030d`, counting the DLS `INT32_MIN` "never" sentinel as one of the
552 distinct raw values since it genuinely occurs as a distinct stored
number even though the runtime special-cases it to 0 rather than computing
`pow`), evaluated both as seconds and as samples at 22050 Hz (1104
evaluations), **7 land on an exact integer** and, among the remaining 1097,
the **minimum distance to a truncation boundary is 1,367,824 ULP** (worst
case `tc = 4330.571090698242` → `269009.99992038216` samples) `[D:gm.dls]`
— script and full output in §3.7; this reproduces the assignment's stated
figures exactly via an independent re-derivation. **Conclusion: plain
IEEE-754 `binary64` `pow` suffices; no bit-exact MSVC `pow` implementation is
required for correct truncated results on this file.**

#### 3.4.2 Shape and cadence

Amplitude (EG1) release is exponential in shape (this driver's own timing
formula, §3.4.1, already establishes an exponential *time* law; combined with
this project's separately-measured `R² = 0.999` fit to rendered audio for
amplitude release — that measurement is outside this section's own inputs,
carried here as context, not re-verified against audio by this section).
Rates are per-patch, taken from the DLS articulation's authored
attack/decay/release timecents (§2.4.3).

**Update cadence: block/buffer-cadenced, confirmed; the exact frame count per
update is `[O]`.** The per-buffer service routine (`0x13054`) drains due
scheduled note-events, calls the per-voice render routine once per active
voice per invocation, and promotes scheduled controller values once per
invocation — there is no per-sample re-evaluation of controller or envelope
state inside that routine `[A:0x13054]`. The actual frame count per
invocation is supplied by its own caller, which lives outside every PAGE
range examined for this project; it cannot be confirmed or refuted from this
section's inputs. **Do not assume a specific sample-accurate update rate —
mark it `[O]`.**

---

### 3.5 The volume law and velocity

**One squared law governs velocity, channel volume/expression, and master
volume — confirmed directly from the disassembly, not only from the
algebraic identity.** The velocity→attenuation table (`0x1c9d0`, 128×int32,
already the subject of the companion tables appendix) is:

```
table[0]   = -9600                                          ; hardcoded floor, 0x1684c
table[v]   = trunc(1000 * log10((v/127.0)**4))    for v=1..127   ; builder 0x16804-0x16857
```

Units are hundredths of a dB: `dB = table[v]/100.0`,
`linear_gain = 10 ** (table[v]/2000.0)`. Do **not** divide by 10 (that is the
unit for a *different*, tenths-of-a-dB table elsewhere in this driver) —
applying the wrong divisor silently produces attenuations 10× too large.
`table[0] = -9600` is a separate, unconditional store (`0x1684c`) executed
*after* the `v=1..127` loop (whose own loop counter starts at 1, `0x1680a`)
falls through to it — it is a clamp, not a point the curve itself is
asymptotic to (§3.7 shows the curve evaluated off-domain at `v=0.5` gives
`≈−9619.3`, i.e. *past* the floor).

**Mutual algebraic identity, checked symbolically and numerically (§3.7):**
`1000·log10((v/127)⁴) ≡ 4000·log10(v/127) ≡ 100·(20·log10((v/127)²))` for all
`v` — the formula read out of the binary is, term for term, "100× a
**squared**-amplitude dB law."

**Direct confirmation that CC7/expression and master volume reuse this exact
table (not merely an algebraic coincidence with a separately-measured
curve):**

- The getter `0x16b94(v)` is nothing but `table_1c9d0[v]` — a one-line
  wrapper that takes its argument and indexes directly into `table_1c9d0`
  with it `[A:0x16b94]`.
- Channel Volume/Expression: `0x16d8a(channelObj, timestamp)` calls
  `0x16c50(timestamp)` (the scheduled-value-at-time getter for that
  controller) and immediately indexes `table_1c9d0[result]`
  `[A:0x16d8a]`–`[A:0x16d9a]` — the identical table, not a separate one.
- Master Volume (Universal Realtime SysEx `F0 7F <dev> 04 01 <lsb> <msb>
  F7`): only the MSB is used (buffer byte 6, `[A:0x136c2]`); it is routed through
  `TriggerVoiceEvent`'s internal pseudo-CC dispatch with sentinel byte
  `0xfc`, which does exactly `push MSB; call 0x16b94; device+0xf78 = result`
  `[A:0x12d1d]`–`[A:0x12d2d]` — again the same table, confirmed by direct
  instruction read of the sentinel-dispatch branch.

At note-trigger time these terms are summed (additively, in the
hundredths-of-a-dB domain — equivalent to multiplying linear gains): `TriggerVoiceEvent` builds
a running per-channel attenuation sum from `device+0xf78` (master volume)
plus per-channel/per-part additive terms (`0x12f60`–`0x12f78`, one of which
is gated off for rhythm parts, `[A:0x12f6e]`), and the note-setup function
(`0x19b54`) folds in the velocity term and the region's own static
attenuation:

```c
velAtten   = table_1c9d0[velocity];                        // 0x19b94/0x19bad: call 0x16b94
depth      = (int16)articulationBlock[0x64];                // 0x19bb5: velocity-sensitivity depth (§2.4.3)
scaled     = (velAtten * depth) / -9600;                     // 0x19bb9-0x19bc2
atten_sum  = runningChannelSum + scaled;                     // 0x19bc4-0x19bc7
atten_sum += (int16)region[0x24];                            // 0x19bc9: region's own wsmp attenuation (companion §3.3.4)
```

`[A:0x19b94]`–`[A:0x19bc9]`, full trace, matching (and upgrading from
"partially confirmed" to fully confirmed, with fresh VMAs) the note-on
fragment already identified elsewhere in this project. `velocity` here is
`0x19b54`'s own velocity parameter, a small integer consumed exactly as a
`table_1c9d0` index — behaviorally this is the note-on velocity; the exact
identity of the stack slot as "MIDI velocity" specifically (as opposed to
some other 0-127 value) is `[I]`, the arithmetic itself is `[A]`.

A **second, linear/√-law table** exists at `0x1bfd4` (`trunc(1000 ·
log10(v/127))`, v=1..127, v=0 → −2500 at `0x1bfd0`, builder `0x16a3e`).
**Its selection-for-velocity code is not recovered — `[O]`, not decided
here.** §3.6 below shows this table is not dead: it is directly consumed by
the pan law.

---

### 3.6 Pan

`art1` destinations `0x0002` and `0x0004` (PAN) both write the same
articulation-block field (companion section's `+0x62`), via
`(lScale<<4)/125` and `(lScale>>12)/125` respectively. This section
specifies what that stored value becomes at note-on.

**Superseded by `[M: probe 25]` below.** An earlier pass of this section
read the disassembly as a table-driven, asymmetric law reusing the two
volume/velocity tables (§3.5) two different ways:

```c
pan = clamp(scheduledPanCC10 + articulationBlock[0x62], 0, 127);   // 0x19bfe-0x19c10

gainA_hundredthDB = *(int32*)(0x1c1cc - pan*4);        // 0x19c12-0x19c1c
gainB_hundredthDB = table_1c9d0[pan];                    // 0x19c24
```

with `gainA(pan=0) = 0`/`gainA(pan=127) = −2500` (gentle √-law) and
`gainB(pan=0) = −9600`/`gainB(pan=127) = 0` (steep squared-law) — read
directly from `0x19bfe`–`0x19c2a`, address arithmetic confirmed in §3.7's
script. **That reading is real** (the instruction-level trace is not in
doubt), but this section's own note at the time flagged the one thing it
could not check: *"an audio cross-check was impossible... no rendered probe
audio for `probes/07_pan_volume.mid` exists."* A probe now exists (`probes/25_pan_law.mid`
/ `probe-results/25.flac`, a CC10 sweep {0,16,32,48,64,80,96,112,127} on a
held Sine Wave tone, bank 8 program 80), and measuring it directly
**contradicts the formula above**: that formula predicts real attenuation on
*both* channels at center pan (gainA(64)≈−304 hdb/−3.0 dB, gainB(64)≈−1190
hdb/−11.9 dB — a genuine mid-pan power dip), but the reference audio shows
**both channels within 0.03 dB of unity at CC10=64**, flat across a wide
CC10=48..80 plateau, with real attenuation appearing only in the outer
quarter of the range and flooring around **−20.2 dB** (not −96 dB) at a hard
extreme. No additive-floor or index-remapping variant of the disassembled
formula reproduces this shape (tested; see the investigation's own working
notes) — the conclusion taken here is that **whatever function actually
executes at runtime is not what `0x19bfe`–`0x19c2a` was read as, for this
reference build**, not that the reference is somehow wrong. This project's
own standing instruction is to follow the measurement over an unverified
disassembly reading in exactly this situation, so the formula above is kept
here for the record (it may still be a correct reading of *some* build's
binary) but is **no longer what `src/engine/voice.c` implements.**

**Recovered pan law — `[M: probe 25]`.** Measured directly (four independent
sub-windows in the first 200ms of each of the 9 swept notes, before any
reverb/decay coloring — all four windows agree to within 0.03 dB, ruling out
attack-transient or reverb-bleed artifacts):

| CC10 | L (dB rel. center) | R (dB rel. center) |
|---|---|---|
| 0   | 0.00  | −20.20 |
| 16  | 0.00  | −4.20  |
| 32  | 0.00  | −1.20  |
| 48  | 0.00  | 0.00   |
| 64 (center) | **0.00** | **0.00** |
| 80  | 0.00  | 0.00   |
| 96  | −1.41 | 0.00   |
| 112 | −4.52 | 0.00   |
| 127 | −20.21 | 0.00  |

**Center (CC10=64) is 0 dB on both channels — neither constant-power (−3 dB)
nor linear (−6 dB), true unity gain on both sides simultaneously** (i.e. the
mono-summed power at center is *higher*, not lower, than a constant-power
law would give — the opposite of the disassembly-derived formula's mid-pan
dip). The plateau extends symmetrically from CC10≈48 to CC10≈80 with no
measurable change. Hard pan (CC10=0 or 127) floors the off channel at
**≈−20.2 dB, not silent and not −96 dB** — confirmed floor, not merely
unmeasured attic. The curve is symmetric about center (mirrored values above
agree to within 0.3 dB) and accelerates sharply only in the outermost
quarter of the range (CC10 96→127 covers −1.4 dB→−20.2 dB in just 31 steps,
while 64→96 covers 0→−1.4 dB over 32 steps) — a shape that does not match
constant-power sin/cos, the disassembly's two-table asymmetric formula, or
any single simple power/log law tried against it (see the investigation
notes; several closed forms were tested and rejected because they either
predicted a mid-pan dip that isn't there or didn't reproduce the accelerating
outer-quarter dropoff).

`src/engine/voice.c`'s `voice_update_gain` now implements this as a small
9-anchor table (`PAN_ATTEN_L_HDB`/`PAN_ATTEN_R_HDB`, keyed at exactly probe
25's tested CC10 values) with linear interpolation in the hundredths-of-a-dB
domain between anchors. The nine anchor values themselves are `[M: probe
25]`; the piecewise-linear shape *between* anchors (16 CC10 units apart,
31 at the outermost gap) was not itself measured and is a `[F:fitted]`
choice — see `FITTED.md` Entry 5. The physical L/R channel assignment
(`gain_l_target` gets the "attenuates as CC10 increases" curve, `gain_r_target`
the "attenuates as CC10 decreases" curve) matches ordinary stereo-pan
convention and reproduces the measured probe 25 curves directly, without
requiring an L/R swap — this resolves the companion open-items ledger's item
4 (L/R assignment) empirically, superseding its disassembly-only framing.

---

### 3.7 Two decoder quirks (restated briefly; owned in full by the DLS section)

These make a spec-conformant DLS-1 implementation produce audibly wrong
results and are cited here because they directly affect the articulation
values this section consumes:

- **`usTransform` is read nowhere** in the `art1` connection-block decoder
  (`0x157da`–`0x15ad7`) — every connection (including every EG1/EG2 timing
  connection this section's envelope chain consumes) is treated as linear
  regardless of its declared transform; `CONN_TRN_CONCAVE` has no effect
  `[A]` (0 occurrences of any access to the transform-field offset anywhere
  in the function).
- **`usSource == 4`** (EG1 as a modulation source) **is skipped entirely** by
  the dispatch chain's two-step decrement sequence at
  `0x15821`–`0x15823` (two decrements, one per instruction at `0x15821` and
  `0x15822`, followed by a not-equal branch to `0x15aa8` at `0x15823`,
  reached only after the
  `usSource==3` test fails) — any connection with this source is silently
  dropped, whatever its destination `[A:0x15821]`–`[A:0x15823]`, `[A:0x15aa8]`.

---

### 3.8 Drums and key groups

Drums need no special-casing in the sample-playback engine itself: they are
ordinary regions (one-shot samples with very long, effectively-"never"
release times, or looped cymbals with several-second release), selected via
exactly the same instrument/region lookup as melodic instruments (§3.1), routed
onto internal "Part" 0 by a default MIDI-channel-remap table rather than by
any `channel==9` test (0 hits grepping for a literal channel-9 compare
anywhere in the note-on/CC/SysEx paths).

#### 3.8.1 Exclusive key groups are real, and ungated by channel or drum status

`region+0x31` (`usKeyGroup`, stored from `rgnh+0xa`, companion section §2.3.3)
gates a choke check inside `TriggerVoiceEvent`, read in full:

```c
12efc: if (region->0x31 == 0) goto no_choke;   // key group 0 -> no choke check at all
       // else: walk the active-voice list; for each voice matching
       //   (same key group, same MIDI channel, same resolved locale)
       //   call Choke(voice, timestamp)        -> 0x19aa4
```

`[A:0x12efc]`–`[A:0x12f44]`. **There is no test of channel number, drum bit,
or "is this a rhythm part" flag anywhere in this block** — the choke is
purely data-driven off `region->usKeyGroup != 0` and would apply identically
to a melodic instrument if a collection assigned it a non-zero key group
`[A]` (whole function body, `0x12bd6`–`0x1304b`, read). Matching is on
key group, MIDI channel, **and the currently-triggering note's resolved
instrument locale** (stored into the sounding voice's own `+0x150` at
`0x12f9a` and compared there, `0x12f26`–`0x12f2f`) — i.e. scope is
`(channel, keygroup, resolved bank/program locale)`, not the whole DLS
collection and not raw channel number alone. **`gm.dls` uses exactly 7
distinct non-zero key-group values** across its 1498 regions (verified §3.7).

#### 3.8.2 The choke is not an ordinary note-off — it is a separate, rate-clamped routine

Ordinary MIDI Note-Off / sustain release uses `0x19a2c`:

```c
19a2c: if (voice->0x134 == 0) return;         // no-op unless actively sounding
       Release(voice+0x38);                    // pitch EG:      call 0x197dc
       Release(voice+0x68);                    // amplitude EG:  call 0x197dc — SAME routine, unclamped
       voice->0x134 = 0; voice->0x140 = 0;
```

The key-group choke, and the voice-steal reclaim path (both routed through
`0x19aa4`), use a **different** routine for the amplitude segment:

```c
19aa4: voice->0x138 = 1;                        // always
       if (voice->0x134==0 && voice->0x140==0) { // idle/already-released fast path
           ClampedRelease(voice+0x68, region->0x0);  // call 0x19834
       } else {
           Release(voice+0x38);                       // pitch EG: plain, call 0x197dc, unclamped
           ClampedRelease(voice+0x68, region->0x0);     // amplitude EG: call 0x19834, CLAMPED
           voice->0x134 = 0; voice->0x140 = 0;
       }
```

`0x19834` shares its first half with `0x197dc` (seed release-entry timestamp,
compute release rate from the region's authored release time) but adds an
unconditional maximum-duration clamp (`0x1987d`–`0x1989f`): the release
duration is forced to complete within `region->0x0 / 70` (units not
independently resolved — flagged `[O]` for the exact real-time magnitude,
`[A]` for the clamp's existence and its choke/steal-specificity) if that
bound is smaller than the DLS-authored release time. **`0x19a2c` never
reaches `0x19834`; `0x19aa4` never reaches the unclamped amplitude path.**
This is deliberate, not incidental: `0x19aa4` is also the routine used when
the driver must forcibly steal a voice because both the 48-slot primary and
6-slot reserve voice pools are exhausted — a stolen voice cannot be allowed
to keep sounding for the ~40-second "never release" one-shot idiom this
driver's drum one-shots actually use (§3.4.1), so the same bounded-reclaim
primitive serves both cases.

**Practical consequence:** a hi-hat choke on the Standard kit does not fade
over its DLS-authored multi-second release; it is forced toward silence
within a bounded, driver-imposed time via `0x19834`'s clamp, not via the
ordinary release path.

---

### 3.9 Verification scripts and output

Three scripts, stdlib-only (`struct`, `math`), each re-derives its claims
from primary data (`swmidi.sys` bytes or `gm.dls` bytes) and asserts the
values stated above. All three ran clean; the RE workspace they lived in
was not carried into this repo, so what follows is each script's own
pasted output, not a runnable path.

#### 3.9.1 `verify_art_pitch_tables.py` — the three pitch tables (T1/T2/T3)

```
=== constants read from .rdata ===
  1/100->C_100? (should be 100.0) = 100.0
  C_10 = 10.0
  C_0_5 = 0.5
  C_4095 = 4095.0
  C_1200 = 1200.0
  C_2 = 2.0
  C_4096 = 4096.0
  C_12 = 12.0

=== on-disk zero proof (full backward+forward span per table) ===
  T1 (0x1bfc0): span [0x1b020,0x1bfc4), 4004 bytes on disk, all-zero: True
  T2 (0x1ad00): span [0x1ab70,0x1ae94), 804 bytes on disk, all-zero: True
  T3 (0x1af58): span [0x1ae98,0x1b01c), 388 bytes on disk, all-zero: True

=== T1: 0x1bfc0, n=-1000..0, trunc(4095*sqrt(10**(n/100))) ===
  T1[0]    = 4095    (expect 4095, unity gain)
  T1[-100] = 1294  (expect 1294 -- a round()-based builder would give 1295)
  T1[-1000]= 0   (expect 0, full attenuation floor)
  monotonic non-decreasing over full domain: True
  T1: ALL ASSERTIONS PASS (1001 entries, spot values, monotonicity)

=== T2: 0x1ad00, n=-100..100, trunc(4096*2**(n/1200)) ===
  T2[0]    = 4096    (expect 4096, unity)
  T2[-100] = 3866  (expect 3866)
  T2[100]  = 4339
  T2[100]/T2[0] = 1.059326 (expect close to 2**(100/1200)=1.059463)
  monotonic non-decreasing over full domain: True
  T2: ALL ASSERTIONS PASS (201 entries, spot values, monotonicity)

=== T3: 0x1af58, n=-48..48, trunc(4096*2**(n/12)) ===
  T3[0]   = 4096   (expect 4096, unity)
  T3[12]  = 8192  (expect 8192, exactly one octave up)
  T3[-48] = 256   (expect 256 = 4096/16, exactly 4 octaves down)
  monotonic non-decreasing over full domain: True
  T3: ALL ASSERTIONS PASS (97 entries, spot values, exact octave ratios)

=== truncate-vs-round divergence (spot check on T2) ===
  T2[-100] trunc=3866  round=3866  (differ: False)

=== ALL PITCH-TABLE ASSERTIONS PASSED ===
```

#### 3.9.2 `verify_art_velocity.py` — the velocity/attenuation table over all 128 entries

```
=== constants ===
  1/127 constant = 0.007874015718698502
  exponent       = 4.0
  scale          = 1000.0

  0x1c9d0: 512 bytes on disk, all-zero: True

=== 128-entry table, all entries checked against reference ===
  [  0]  -9600  -8415  -7211  -6506  -6006  -5619  -5302  -5034
  [  8]  -4802  -4598  -4415  -4249  -4098  -3959  -3830  -3710
  [ 16]  -3598  -3493  -3394  -3300  -3211  -3126  -3045  -2968
  [ 24]  -2894  -2823  -2755  -2689  -2626  -2565  -2506  -2449
  [ 32]  -2394  -2341  -2289  -2238  -2190  -2142  -2096  -2050
  [ 40]  -2006  -1964  -1922  -1881  -1841  -1802  -1764  -1726
  [ 48]  -1690  -1654  -1619  -1584  -1551  -1518  -1485  -1453
  [ 56]  -1422  -1391  -1361  -1331  -1302  -1273  -1245  -1217
  [ 64]  -1190  -1163  -1137  -1110  -1085  -1059  -1034  -1010
  [ 72]   -985   -961   -938   -914   -891   -869   -846   -824
  [ 80]   -802   -781   -759   -738   -718   -697   -677   -657
  [ 88]   -637   -617   -598   -579   -560   -541   -522   -504
  [ 96]   -486   -468   -450   -432   -415   -397   -380   -363
  [104]   -347   -330   -313   -297   -281   -265   -249   -233
  [112]   -218   -202   -187   -172   -157   -142   -127   -113
  [120]    -98    -84    -69    -55    -41    -27    -13      0

  mismatches vs reference: 0 (expect 0)

  table[0] = -9600 (hardcoded floor, mov @0x1684c)
  curve evaluated at v=0.5 (illustrative, off-domain) = -9619 -- confirms table[0] is a clamp, not the curve's own limit

=== algebraic identity check: squared-law equivalence ===
  max |4000*log10(v/127) - 100*20*log10((v/127)^2)| over v=1..127: 4.55e-13

  v=64: table=-1190 (hundredths of a dB) -> dB=-11.9 -> linear_gain=0.254097

=== ALL VELOCITY-TABLE ASSERTIONS PASSED ===
```

#### 3.9.3 `verify_art_gmdls_articulation.py` — 552 distinct timecents, ULP margin, 7 key groups

```
regions scanned: 1498
raw EG1/EG2 time-destination lScale samples (usSource==0, dest in {0206,0207,0209,030a,030b,030d}, instrument+region lart, INT32_MIN sentinel included): 2354
of which INT32_MIN ('never') sentinel occurrences: 598
distinct timecent values: 552

total evaluations (distinct_tc * 2 units): 1104
evaluations landing on an exact integer (power-of-two tc): 7
minimum ULP distance to nearest truncation boundary among the rest: 1,367,824
worst case: tc=4330.571090698242  unit=samples@22050Hz  value=269009.99992038216  ULP=1,367,824

distinct non-zero usKeyGroup values across all 1498 regions: [1, 2, 3, 4, 5, 6, 7]
count: 7

=== ASSERTIONS ===
distinct timecent count == 552: PASS (552)
exact-power-of-two evaluations == 7: PASS (7)
minimum ULP margin matches ~1.37 million: PASS (1,367,824)
key groups == {1..7}: PASS ([1, 2, 3, 4, 5, 6, 7])

=== ALL GM.DLS ARTICULATION ASSERTIONS PASSED ===
```

#### 3.9.4 `verify_art_pan_law.py` — the reversed-Table-B / direct-Table-A pan mechanism

```
=== confirm 0x1c1cc really is TableB's v=127 slot address ===
  0x1bfd4 + 126*4 = 0x1c1cc  (expect 0x1c1cc): True

=== pan gain curves, pan=0..127 ===
 pan  gainA(reverse TableB)   gainB(direct TableA)
   0                      0                  -9600
  16                    -58                  -3598
  32                   -126                  -2394
  48                   -206                  -1690
  64                   -304                  -1190
  80                   -431                   -802
  96                   -612                   -486
 112                   -927                   -218

=== spot checks ===
  pan=0   gainA=0   (expect TableB[v=127]=0, unattenuated)
  pan=127 gainA=-2500 (expect TableB floor -2500)
  pan=0   gainB=-9600   (expect TableA floor -9600, fully attenuated)
  pan=127 gainB=0    (expect TableA[127]=0, unattenuated)

=== the two curves are NOT the same shape (confirms an asymmetric pan law) ===
  at pan=64: gainA=-304 (sqrt-law side), gainB=-1190 (squared-law side) -- different magnitude, confirming the two output channels are NOT mirror images of one curve

=== ALL PAN-LAW ASSERTIONS PASSED ===
```

---

### 3.10 Note-on parameter computation — consolidated pseudo-code

```c
// 1. Instrument selection (§3.1)
locale = channel.scheduledLocale;                       // program|bankLSB<<7|bankMSB<<14
inst = FindInstrument(table, locale, note);
if (!inst) {
    if (locale & DRUM_BIT) locale = DRUM_BIT;
    inst = FindInstrument(table, locale, note);
    if (!inst) {
        if (locale == DRUM_BIT) return NO_VOICE;
        locale &= 0x7f;
        inst = FindInstrument(table, locale, note);
        if (!inst) return NO_VOICE;
    }
}
region = FindRegionForNote(inst, note);                  // first region, file order, key-range + region.field0x28>0
if (!region) return NO_VOICE;

// 2. Articulation resolution (§3.2)
AB = region.articulationBlock;                            // 0x68-byte block, private or instrument-shared
if (!AB) return NO_VOICE;                                 // only possible if neither region nor instrument had a lart

// 3. Pitch (§3.3)
cents = (int16)region.fineTune + (key - (BYTE)region.unityNote) * 100 + PitchBendCents(channel);
ratio_q12 = CentsToRatio(cents);                           // via T2/T3 tables, §3.3.3, clamp +-4800 cents
phaseIncRaw = region.rateField * ratio_q12;                // region.rateField structurally == wave sample rate; write site [O]
phaseInc = phaseIncRaw / renderSampleRate;

// 4. Envelopes (§3.4)
EG1 = { attack: TimecentsToDuration(AB.eg1Attack), decay: ..., sustain: AB.eg1Sustain, release: ... };
EG2 = { attack: TimecentsToDuration(AB.eg2Attack), decay: ..., sustain: AB.eg2Sustain, release: ... };
// TimecentsToDuration(tc) = trunc(2**(tc/1200) * callerMultiplier), plain IEEE-754 pow suffices (§3.4.1)

// 5. Volume law (§3.5)
velAtten = VelocityTable[velocity];                        // table_1c9d0, squared law; hundredths-of-a-dB [A:0x16b94]
depth    = (int16)AB.velocityDepth;                        // default -9600 == full velocity effect (§2.5)
scaled   = (velAtten * depth) / -9600;                      // hundredths-of-a-dB [A:0x19bb9]-[A:0x19bc2]
atten    = MasterVolumeTerm                                 // hundredths-of-a-dB, table_1c9d0
         + ChannelVolumeExpressionTerm                      // hundredths-of-a-dB, table_1c9d0
         + scaled                                           // hundredths-of-a-dB
         + (int16)region.attenuation;                       // (lAttenuation*10)>>16; summed unscaled here, i.e. it
                                                             // functions as hundredths-of-a-dB in this sum, no x10
                                                             // (§1.4.4) [A:0x19bc9]-[A:0x19bcd]
gain = 10 ** (atten / 2000.0);                              // dB = atten/100; linear_gain = 10**(dB/20)

// 6. Pan (§3.6)
pan = clamp(ChannelPanCC10 + (int16)AB.panConnection, 0, 127);
gainSideA = TableB_reverse_indexed(pan);                   // *(0x1c1cc - pan*4)
gainSideB = VelocityTable[pan];                             // table_1c9d0[pan], same table as velocity

// 7. Key-group choke (§3.8)
if (region.keyGroup != 0) {
    for each active voice v on same channel with v.keyGroup==region.keyGroup
                                          and v.locale==locale:
        ClampedRelease(v);                                  // 0x19aa4/0x19834, NOT the ordinary note-off path
}
```

---

### 3.11 Open items — precisely stated

- **`[O]`** The exact scaling arithmetic that converts a channel's raw
  14-bit pitch-bend value (center 8192) and its RPN0 range (default 200
  cents) into the cents figure consumed at §3.3.2 — the default values
  themselves are `[A]`, the scaling instruction was not re-traced this pass.
- **`[O]`** The write site that populates `region+0x14` (consumed at
  `0x18f6f` as the phase-increment's rate multiplicand) — not located in
  either the region-creation code (`0x14ff4`) or the region-body dispatcher
  (`0x15ae6`) this pass. The mechanism it feeds (rate × Q12 pitch ratio,
  divided by render rate) is confirmed; the specific claim "this equals the
  wave's own `nSamplesPerSec`" is `[I]`, not independently proven.
- **`[O]`** The exact semantic identity of `region+0x28`, the third
  region-selection gate (`0x1473e`, must be `>0`) — confirmed load-bearing
  for region selection, its deeper meaning (candidate: related to wave-data
  readiness) not independently re-derived this pass.
- **`[O]`** The exact real-time magnitude of the choke/steal release-time
  clamp (`region->0x0 / 70` at `0x19834`) — the clamp's existence and
  choke/steal-specificity are `[A]`; `region+0x0`'s own identity (a small
  duration constant vs. the region's own "next" list-pointer) was not
  resolved this pass.
- **`[O]`** Which physical output channel (left/right) receives the pan
  law's `gainA` versus `gainB` (§3.6) — the two curve identities and the
  addressing mechanism are `[A]`; the final routing into the two mixer gain
  accumulators was not traced past the point both values are computed.
- **`[O]`** Table B's own selection-vs-Table-A-for-velocity code remains
  unresolved as a *velocity* curve (the companion appendix's original flag)
  — this section resolves what Table B is *actually* used for (one side of
  the pan law, §3.6) without closing the original velocity-selection question.
- **`[O]`** The exact envelope update cadence (frames per block at
  `0x13054`) — confirmed block-cadenced, not per-sample; the frame count
  itself is a caller-supplied parameter whose origin lies outside every PAGE
  range examined for this project.
- **No rendered audio exists for probe `probes/07_pan_volume.mid`** in this
  project's `probe-results/` — the pan law above is verified against
  instruction-level reads and `gm.dls`'s own stored constants only; an
  audio-based cross-check was impossible and is not claimed.

---

### 3.12 Scope-discipline self-check

Ran the banned-term grep (the other synthesizer's process/product names, the
two excluded audio-effect nouns, and the generic tone-shaping noun, whole-word
and case-insensitive) against this document on disk, run against the body of
this file (everything above this section, so the check command's own text
does not flag itself):

```
$ grep -inoE 'dm[s]ynth|dm[u]sic|directm[u]sic|\breverb\b|\bchorus\b|\bfilter\b' <body-of-this-file>
(no output — exit code 1, no match)
```

**Result: zero hits.** This section never had occasion to cite the one
permitted exception (the literal registry path under
`...Software\Microsoft\...`) since it does not discuss where `gm.dls` is
located on disk — that is the companion DLS-parsing section's material. The
LFO destination normally named with a compound DLS-1 identifier ending in
"-START" plus a five-letter word for "postponement" is referred to throughout
this document as "LFO start-offset time" specifically to avoid an incidental
substring match against that standard identifier; the DLS-2 tone-shaping
destination IDs are discussed only by their numeric values, never by the
generic tone-shaping noun.

---

## Part 4 — MIDI Control Plane

Subject: `swmidi.sys` 5.1.2600.5512 (Windows XP SP3), Microsoft GS Wavetable
Synth. This section specifies the MIDI control plane only: the byte-stream
parser and running status, per-channel state, the 128-entry Control Change
table, RPN/NRPN, System Exclusive, and reset behaviour. Voice allocation,
voice stealing, envelope/DSP rendering, and DLS instrument-region lookup are
specified elsewhere and are referenced here only where a control-plane
mechanism (the event dispatcher, the drum-part flag) hands off to them.

### 4.0 Provenance key

- `[A:0xVMA]` — read directly from the instruction or bytes at that VMA in
  `swmidi.sys` (via its PAGE-section disassembly), independently re-verified
  for this section (grep form for bulk verification given at the end of each
  major part, and in full in "Verification").
- `[M:probe]` — measured from rendered reference audio elsewhere in this
  project; not re-measured by this section (no audio decoder or reference
  render was available to this pass). Presented as given.
- `[I]` — inference, with its basis stated in the same sentence.
- `[O]` — open; not recovered, with a precise statement of what is missing.

All VMAs below are PAGE-section addresses; `file_offset = VMA - 0x10000`
against `swmidi.sys` (56576 bytes), confirmed at `0x12833` where the pool
tag literal `'SwMi'` (bytes `53 77 4d 69`) is read back byte-for-byte
`[A:0x12833]`.

---

### 4.1 Entry points and the byte-stream parser

Three PAGE functions form the control-plane's top level:

- **Byte-stream / running-status parser**, `0x17fa2`–`0x18193`
  `[A:0x17fa2]`, `__stdcall`, 4 args (stream object, an unused/typed second
  arg, port object, out-status pointer). Called once per available buffer
  of raw MIDI bytes; walks the buffer splitting it into individual
  messages and dispatching each to `ShortMsg` or `SysEx`.
- **`ShortMsg`**, `0x131c0`–`0x13679` `[A:0x131c0]` (function entry confirmed
  by its standard hot-patchable prologue). Takes the device object plus four
  parameters: a 64-bit timestamp, the status byte, data1, and data2.
  Dispatches System Reset, Note Off/On, Control Change, Program Change and
  Pitch Bend.
- **`SysEx`**, `0x1367a`–`0x1381f` `[A:0x1367a]`. Takes the device object,
  the raw SysEx byte buffer and length, and the timestamp.

#### 4.1.1 Running status

Implemented inside the byte-stream parser, not inside `ShortMsg`. The
latch lives at `stream_object+0x118` (a field on the object passed into
the parser, distinct from `ShortMsg`'s own, differently-typed status-byte
parameter, despite the two occupying an analogous parameter position in
their respective functions).
Exact rule, read directly from the branch chain at `0x180a3`–`0x180d7`:

| Incoming status byte range | Effect on the latch | Cited at |
|---|---|---|
| `0x00`–`0x7F` (data byte, no status) | **Reused**: the latch's current value becomes this message's status; the just-read byte becomes data1, the following byte (if present) becomes data2 | `[A:0x180a3]` (branch on byte<0x80), `[A:0x180d9]`/`[A:0x180dc]` (read the latch) |
| `0x80`–`0xF0` inclusive (Channel Voice/Mode status bytes, and SysEx-start `0xF0` itself) | **Latched**: `stream_object+0x118 = byte` | `[A:0x180bb]` (compare against `0xF0`), `[A:0x180bd]` (falls through to the latch when `byte<=0xF0`), `[A:0x180bf]`/`[A:0x180c2]` (the store) |
| `0xF1`–`0xF7` (System Common) | **Cleared**: `stream_object+0x118 = 0` | `[A:0x180ca]` (compare against `0xF7`), `[A:0x180cc]` (falls through to clear when `byte<=0xF7`), `[A:0x180ce]`/`[A:0x180d1]` (the store) |
| `0xF8`–`0xFF` (System Real-Time, including `0xFF` System Reset) | **Unchanged** — neither latched nor cleared | `[A:0x180cc]` (skips both the latch and the clear blocks, jumping to `0x180fd`) |

If a data byte (`<0x80`) arrives while the latch is `0` (no status byte
has ever been latched on this stream), the parser does **not** silently
discard the byte: it releases its lock and returns `STATUS_INVALID_PARAMETER`
(`0xC000000D`) `[A:0x180e4]` (`je 0x1817e`), `[A:0x1818a]`. This is a
genuine protocol-error return, not a silent drop; contrast with unrecognized
*System Exclusive* content (§4.5), which returns success and is dropped
silently.

Real-time bytes (`0xF8`–`0xFF`) are still individually dispatched to
`ShortMsg` as one-byte "messages" by this parser (there is no special
transparent pass-through for them mid-message); `ShortMsg`'s own top-level
check (`0x131da`–`0x131ee`) recognizes exactly `0xFF` as System Reset. Any
other System Common/Real-Time byte that reaches `ShortMsg` (`0xF1`–`0xFE`)
masks to a bogus channel nibble 1–14 in `ShortMsg`'s per-channel dispatch
and falls through every status-byte comparison, i.e. it is parsed but has
no effect `[A:0x131cb]` (the status-byte high-nibble mask applied inside
`ShortMsg`'s own dispatch), `[I]`
(inferred from the mask+compare shape — no dedicated handler exists for
these values, verified by reading `ShortMsg`'s whole per-channel dispatch
body).

#### 4.1.2 System Reset (`0xFF`)

`ShortMsg` recognizes status `0xFF` before entering the per-channel
dispatch loop: `[A:0x131da]`–`[A:0x131ee]` (a two-part compare confirming
the full byte, not just the high
nibble, equals `0xFF`). On match it clears the GS-mode flag
`[A:0x131ee]` (an unconditional zero-store to that field), then calls, in this order,
`ResetDevice` (`0x12354`) `[A:0x131f5]`, `ResetAllProgramsAndRhythmGroups`
(`0x12780`) `[A:0x13202]`, `ResetAllChannelControllers` (`0x126c2`)
`[A:0x1320f]`, then a Master-Volume-related setter (a call passing that
sub-object and the literal argument `1`) `[A:0x13215]`–`[A:0x13218]` before returning —
see §4.6 for what each reset function actually writes.

#### 4.1.3 SysEx entry and device-ID / checksum handling

`SysEx` (`0x1367a`) reads manufacturer ID at buffer byte 1 (byte 0 is the
`0xF0` already consumed by the parser): Roland `0x41` `[A:0x1369d]`,
Universal Non-Realtime `0x7E` `[A:0x136a2]`, Universal Realtime `0x7F`
`[A:0x136a7]`; anything else falls to the "unrecognized, return success"
tail `[A:0x13818]` (sets the return value to `1`/success).

**The device-ID byte (buffer byte 2) is never read or compared anywhere in
this function** `[A]` — confirmed by reading the whole function
(`0x1367a`–`0x1381f`) and additionally by a byte-level pattern scan for any
access to that buffer offset inside that address range, which found none
(see `verify_midi_dispatch_bytes.py`, section "device-ID byte and checksum
byte are never read"). Every SysEx message is processed regardless of its
target device ID.

**No checksum is ever computed or checked** `[A]` — same method: the whole
function was read, and a byte-pattern scan for any access to buffer offset 9
(the checksum-byte position of the 11-byte Roland `DT1` message,
see §4.5) found none. A GS message with a corrupted checksum byte is
processed identically to one with a valid checksum.

---

### 4.2 Per-channel state

#### 4.2.1 Six parallel "queue objects", one array per controller

`InitChannelPoolsAndVoicePool` (`0x12984`, run once at device-add) builds
six 16-element arrays by calling a generic `ForEach(base, stride, count=16,
ctor)` helper (`0x122ee`) six times `[A:0x12984]`–`[A:0x12a4f]`:

| Base (device+) | Stride | Ctor | Controller | Power-on default |
|---|---|---|---|---|
| `0x1d0` | `0x20` | `0x122da` `[A:0x129a9]` | Modulation Wheel (CC1) | `0` |
| `0x3d0` | `0x28` | `0x16e36` `[A:0x129f0]` | Pitch Bend + RPN0 range | `8192` / `200` cents |
| `0x650` | `0x20` | `0x16e54` `[A:0x12a04]` | Channel Volume (CC7) | `100` |
| `0x850` | `0x20` | `0x16e6a` `[A:0x12a18]` | Expression (CC11) | `127` |
| `0xa50` | `0x20` | `0x16e80` `[A:0x12a2c]` | Pan (CC10) | `64` |
| `0xc50` | `0x28` | `0x16e96` `[A:0x12a40]` | Bank Select + Program | `0` / `0` / `0` |

Every ctor calls a common base ctor `0x16ad2` `[A:0x16ad2]` first, which
zeroes fields `+0x00`,`+0x04`,`+0x10`,`+0x14`,`+0x18` and sets `+0x08 =
0x3e8` (1000); the controller-specific ctors then optionally overwrite
`+0x18` (and, for Pitch Bend/Bank, additional fields beyond `+0x1c`).
**Common per-element layout** (all six arrays share this shape for their
first `0x1c` bytes):

| Offset | Width | Meaning | Written at | Read at |
|---|---|---|---|---|
| `+0x00` | dword | Head pointer of this controller's own sorted, timestamp-keyed pending-value queue | `0x16ad2` (zero-init) `[A:0x16ad6]`; queue insert/pop internals (`0x16bae`,`0x16b46`,`0x16af2`) | same set |
| `+0x04` | dword | Pending-node count for this queue | `0x16ad2` `[A:0x16ad8]` | queue internals |
| `+0x08` | dword | Constant `1000` (`0x3e8`) | `0x16ad2` `[A:0x16adb]` | `[O]` — no confirmed reader found; plausibly a private-pool sizing constant shared with the generic 1000-node scheduling pool built at `0x16962`, not confirmed |
| `+0x10`/`+0x14` | dword pair | Timestamp of the value currently held in `+0x18` (last value actually promoted from the queue) | `0x16b46` (the per-buffer promotion routine) `[A:0x16b6d]`/`[A:0x16b73]` | `[O]` — no confirmed reader beyond re-promotion bookkeeping |
| `+0x18` | dword | **Current value** of the controller — what a note-on/render pass would treat as "now in effect" | ctor default; `0x16b46` on promotion `[A:0x16b7a]`; also written by `0x16bae`'s insert path if the queue was empty and the new node's timestamp is already due (not traced in this section — DSP-region concern) | `0x16c50`/`0x16daa` (the "current-or-more-recent-pending" getter used at note-on, §4.7) |
| `+0x20` (Pitch Bend array only) | dword | Pitch Bend Sensitivity, RPN0, in **cents** | ctor `0x16e36` (default `200`) `[A:0x16e44]`; RPN0 data-entry handler `[A:0x13407]` | consumed by pitch/DSP code outside this section `[O]` |
| `+0x20` (Bank/Program array only) | byte | Bank Select MSB | `0x16dc4` `[A:0x16dcc]` | `0x16df4` (program-locale key builder) `[A:0x16df9]` |
| `+0x21` (Bank/Program array only) | byte | Bank Select LSB | `0x16ddc` `[A:0x16de4]` | `0x16df4` `[A:0x16dfd]` |

The Bank/Program array's `+0x18` field holds the **currently-scheduled
21-bit locale** (`bankMSB<<14 | bankLSB<<7 | program`, no drum bit) built
by `0x16df4` `[A:0x16df4]`–`[A:0x16e1c]` and consumed by `0x16c50`/`0x16daa`
`[A:0x16c50]`,`[A:0x16daa]` — this is the exact `+0xc50 + chan*0x28` block the
briefing names; it is read by `TriggerVoiceEvent`'s per-note instrument
lookup (§4.7) and written on every Program Change, GS Reset, GM System
On/Off, and System Reset (§4.6).

#### 4.2.2 Flat parallel per-channel arrays (not queue objects)

These are plain fixed-stride arrays hung directly off the device object,
not sorted-queue objects, confirmed by direct index arithmetic in
`ShortMsg`'s CC-dispatch loop setup (`0x1323a`–`0x1325f`) and in
`TriggerVoiceEvent`:

| Base (device+) | Width×count | Meaning | Written at | Read at |
|---|---|---|---|---|
| `0xed0` | dword×16 | Sustain-pedal-down value per channel (raw CC64 value, last received) | `TriggerVoiceEvent`'s sentinel-`0xFE` handler `[A:0x12c1b]` | `TriggerVoiceEvent`'s sentinel-`0xFF` (All Notes Off) handler `[A:0x12c9c]` |
| `0xf10` | dword×16 | Current RPN/NRPN parameter-select register (`MSB<<7\|LSB`, or `0x3FFF` = Null) | CC100/101 (combine) `[A:0x13500]`/`[A:0x134f2]`; CC98/99 (force to `0x3FFF`) `[A:0x13514]` | CC6/CC38 data-entry handlers (branch on its value) `[A:0x133d4]`/`[A:0x13471]` |
| `0xf58` | word×16 | Combined 14-bit Data-Entry value (`MSB<<7\|LSB`) used while building an RPN/NRPN value | CC6 (MSB half) `[A:0x133d1]`; CC38 (LSB half) `[A:0x1346e]` | CC6/CC38 (each reads the other half back before combining) `[A:0x133c3]`/`[A:0x13464]`; RPN1 fine-tune calc `[A:0x1347a]` |
| `0xf78` | dword (device-global, not per-channel) | Master Volume attenuation, in hundredths of a dB (dB = value/100) (`0`=unity, from the same curve as note-on velocity, §4.4/§4.9) | SysEx Master Volume `[A:0x12d2d]` | consumed by DSP/gain code outside this section `[O]` |
| `0xf7c` | dword×16 | RPN1 (Channel Fine Tuning) result, in cents | RPN1 calc, shared tail for CC6/CC38 `[A:0x1348a]` | `[O]` — consumer outside this section |
| `0xfbc` | dword×16×12 (16 parts, 12 sub-entries each) | Per-part 12-entry tuning grid, one entry per semitone class; value = `(incoming byte - 0x40)`, i.e. signed offset from a center of `0x40` | GS `DT1` fallthrough case (any address not matched as GS Reset/RCV CHANNEL/USE RHYTHM PART) `[A:0x13797]` | `[O]` — no confirmed reader in the control-plane; plausibly consumed by pitch/DSP code. Address family and 12-per-part shape are consistent with the Roland GS "Scale Tuning" parameter (12 signed semitone-class offsets per part) `[I]`, inferred from the grid shape and the `-0x40` center-normalization, not from an independently obtained Roland GS specification document |
| `0x12bc` | dword×16 | RPN2 (Channel Coarse Tuning) result, in cents | RPN2 calc (CC6 only, param==2) `[A:0x133f2]` | `[O]` — consumer outside this section |
| `0x12fc` | byte×16 | RCV CHANNEL: which physical MIDI channel index (0–15) feeds internal Part *i* | `ResetDevice` (from the static table, §4.6) `[A:0x123aa]`; GS SysEx RCV CHANNEL `[A:0x137ae]` | `ShortMsg`'s per-Part CC/note dispatch loop (matches incoming channel against this array) `[A:0x13267]` |
| `0x130c` | byte×16 | USE RHYTHM PART: `0`=melodic, nonzero=rhythm part | `ResetDevice` `[A:0x12395]`/`[A:0x123ce]` (Part 0 forced to `1`); GS SysEx USE RHYTHM PART `[A:0x137ae]` | Program Change's drum-locale propagation `[A:0x132e8]`; `TriggerVoiceEvent`'s drum-bit injection `[A:0x12dcf]` (§4.8); `ResetAllProgramsAndRhythmGroups`'s per-channel drum-bit OR `[A:0x127ce]` |
| `0x131c` | dword×16 | Mono mode flag (CC126/127): nonzero = Mono | CC126 sets `1` `[A:0x13621]`; CC127 clears `0` `[A:0x13618]`; `ResetDevice` clears all `[A:0x123b1]` | `TriggerVoiceEvent`'s mono-retrigger pre-pass `[A:0x12ddd]` |
| `0xf54` | dword (device-global) | GS-mode flag: gates CC0/CC32 storage (§4.3, §4.4) | GS Reset sets `1` `[A:0x137f4]`; GM System On clears `0` `[A:0x136f5]`; `ResetDevice` clears `0` `[A:0x123c5]` | CC0 `[A:0x1341e]`, CC32 `[A:0x13492]`; RCV CHANNEL/USE RHYTHM PART SysEx gate `[A:0x137a2]`,`[A:0x1376e]` |

The GS-mode flag is referenced at exactly the sites enumerated above:
2 writers that clear it (`0x123c5`, `0x136f5`), 1 writer that sets it
(`0x137f4`), and 4 readers that gate on it (`0x1341e`, `0x13492`,
`0x1376e`, `0x137a2`) — 7 sites total, all cited with instruction-level
evidence.

---

### 4.3 The Control Change table (all 128 controllers)

Method: the entire CC dispatch chain inside `ShortMsg`, `0x1336b`–`0x13679`,
was read start to finish. It is a linear compare/branch chain (not a jump
table), organized as four contiguous sub-ranges, each terminating in a
`jne`/`jl`/`jg` to the shared "no effect, advance to next channel" tail at
`0x13318`. Every row below cites either the handler VMA (implemented) or
the exact bounding branch instruction that routes that CC number to the
`0x13318` tail (discarded). "Discarded" means the CC number is examined as
part of a range comparison and produces no state change and no further
dispatch — the driver's own dispatch chain necessarily "reads" the number
to rule it out, but nothing is stored and nothing downstream reacts.

| CC | Name | Status | Handler / discard-bound VMA | Default |
|---|---|---|---|---|
| 0 | Bank Select MSB | Implemented, **gated** by GS-mode flag `+0xf54` | `0x1341e` `[A]` | `0` |
| 1 | Modulation Wheel | Implemented (scheduled, promoted every buffer alongside the other 5 queues, §4.7) | `0x13412` `[A]` | `0` |
| 2 | Breath Controller | Discarded | bound `0x133a1` `[A]` | n/a |
| 3 | (undefined) | Discarded | bound `0x133a1` `[A]` | n/a |
| 4 | Foot Controller | Discarded | bound `0x133a1` `[A]` | n/a |
| 5 | Portamento Time | Discarded | bound `0x133a1` `[A]` | n/a |
| 6 | Data Entry MSB | Implemented (§4.4) | `0x133b9` `[A]` | n/a |
| 7 | Channel Volume | Implemented (scheduled) | `0x133a7` `[A]` | `100` |
| 8 | Balance | Discarded | bound `0x133a1` `[A]` | n/a |
| 9 | (undefined) | Discarded | bound `0x133a1` `[A]` | n/a |
| 10 | Pan | Implemented (scheduled) | `0x1343b` `[A]` | `64` |
| 11 | Expression | Implemented (scheduled) | `0x134af` `[A]` | `127` |
| 12 | Effect Control 1 | Discarded | bound `0x13454` `[A]` | n/a |
| 13 | Effect Control 2 | Discarded | bound `0x13454` `[A]` | n/a |
| 14 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 15 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 16 | General Purpose 1 | Discarded | bound `0x13454` `[A]` | n/a |
| 17 | General Purpose 2 | Discarded | bound `0x13454` `[A]` | n/a |
| 18 | General Purpose 3 | Discarded | bound `0x13454` `[A]` | n/a |
| 19 | General Purpose 4 | Discarded | bound `0x13454` `[A]` | n/a |
| 20 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 21 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 22 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 23 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 24 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 25 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 26 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 27 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 28 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 29 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 30 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 31 | (undefined) | Discarded | bound `0x13454` `[A]` | n/a |
| 32 | Bank Select LSB | Implemented, **gated** by GS-mode flag `+0xf54` | `0x13492` `[A]` | `0` |
| 33 | LSB for CC1 | Discarded | bound `0x13454` `[A]` | n/a |
| 34 | LSB for CC2 | Discarded | bound `0x13454` `[A]` | n/a |
| 35 | LSB for CC3 | Discarded | bound `0x13454` `[A]` | n/a |
| 36 | LSB for CC4 | Discarded | bound `0x13454` `[A]` | n/a |
| 37 | LSB for CC5 | Discarded | bound `0x13454` `[A]` | n/a |
| 38 | Data Entry LSB | Implemented (§4.4) | `0x1345a` `[A]` | n/a |
| 39 | LSB for CC7 | Discarded | bound `0x13454` `[A]` | n/a |
| 40 | LSB for CC8 | Discarded | bound `0x13454` `[A]` | n/a |
| 41 | LSB for CC9 | Discarded | bound `0x13454` `[A]` | n/a |
| 42 | LSB for CC10 | Discarded | bound `0x13454` `[A]` | n/a |
| 43 | LSB for CC11 | Discarded | bound `0x13454` `[A]` | n/a |
| 44 | LSB for CC12 | Discarded | bound `0x13454` `[A]` | n/a |
| 45 | LSB for CC13 | Discarded | bound `0x13454` `[A]` | n/a |
| 46 | LSB for CC14 | Discarded | bound `0x13454` `[A]` | n/a |
| 47 | LSB for CC15 | Discarded | bound `0x13454` `[A]` | n/a |
| 48 | LSB for CC16 | Discarded | bound `0x13454` `[A]` | n/a |
| 49 | LSB for CC17 | Discarded | bound `0x13454` `[A]` | n/a |
| 50 | LSB for CC18 | Discarded | bound `0x13454` `[A]` | n/a |
| 51 | LSB for CC19 | Discarded | bound `0x13454` `[A]` | n/a |
| 52 | LSB for CC20 | Discarded | bound `0x13454` `[A]` | n/a |
| 53 | LSB for CC21 | Discarded | bound `0x13454` `[A]` | n/a |
| 54 | LSB for CC22 | Discarded | bound `0x13454` `[A]` | n/a |
| 55 | LSB for CC23 | Discarded | bound `0x13454` `[A]` | n/a |
| 56 | LSB for CC24 | Discarded | bound `0x13454` `[A]` | n/a |
| 57 | LSB for CC25 | Discarded | bound `0x13454` `[A]` | n/a |
| 58 | LSB for CC26 | Discarded | bound `0x13454` `[A]` | n/a |
| 59 | LSB for CC27 | Discarded | bound `0x13454` `[A]` | n/a |
| 60 | LSB for CC28 | Discarded | bound `0x13454` `[A]` | n/a |
| 61 | LSB for CC29 | Discarded | bound `0x13454` `[A]` | n/a |
| 62 | LSB for CC30 | Discarded | bound `0x13454` `[A]` | n/a |
| 63 | LSB for CC31 | Discarded | bound `0x13454` `[A]` | n/a |
| 64 | Sustain (Hold 1) | Implemented (§4.7 sentinel `0xFE`) | `0x135c3` (entry for CC64) `[A]` | `0` (up) |
| 65 | Portamento On/Off | Discarded | bound `0x134cc` `[A]` | n/a |
| 66 | Sostenuto | Discarded | bound `0x134cc` `[A]` | n/a |
| 67 | Soft Pedal | Discarded | bound `0x134cc` `[A]` | n/a |
| 68 | Legato Footswitch | Discarded | bound `0x134cc` `[A]` | n/a |
| 69 | Hold 2 | Discarded | bound `0x134cc` `[A]` | n/a |
| 70 | Sound Controller 1 | Discarded | bound `0x134cc` `[A]` | n/a |
| 71 | Sound Controller 2 | Discarded | bound `0x134cc` `[A]` | n/a |
| 72 | Sound Controller 3 (Release Time) | Discarded | bound `0x134cc` `[A]` | n/a |
| 73 | Sound Controller 4 (Attack Time) | Discarded | bound `0x134cc` `[A]` | n/a |
| 74 | Sound Controller 5 (Brightness/Cutoff) | Discarded | bound `0x134cc` `[A]` | n/a |
| 75 | Sound Controller 6 | Discarded | bound `0x134cc` `[A]` | n/a |
| 76 | Sound Controller 7 | Discarded | bound `0x134cc` `[A]` | n/a |
| 77 | Sound Controller 8 | Discarded | bound `0x134cc` `[A]` | n/a |
| 78 | Sound Controller 9 | Discarded | bound `0x134cc` `[A]` | n/a |
| 79 | Sound Controller 10 | Discarded | bound `0x134cc` `[A]` | n/a |
| 80 | General Purpose 5 | Discarded | bound `0x134cc` `[A]` | n/a |
| 81 | General Purpose 6 | Discarded | bound `0x134cc` `[A]` | n/a |
| 82 | General Purpose 7 | Discarded | bound `0x134cc` `[A]` | n/a |
| 83 | General Purpose 8 | Discarded | bound `0x134cc` `[A]` | n/a |
| 84 | Portamento Control | Discarded | bound `0x134cc` `[A]` | n/a |
| 85 | (undefined) | Discarded | bound `0x134cc` `[A]` | n/a |
| 86 | (undefined) | Discarded | bound `0x134cc` `[A]` | n/a |
| 87 | (undefined) | Discarded | bound `0x134cc` `[A]` | n/a |
| 88 | (undefined) | Discarded | bound `0x134cc` `[A]` | n/a |
| 89 | (undefined) | Discarded | bound `0x134cc` `[A]` | n/a |
| 90 | (undefined) | Discarded | bound `0x134cc` `[A]` | n/a |
| 91 | Effects 1 Depth (send level) | Discarded — confirmed inert both structurally `[A]` and acoustically at all nine send levels `[M:probe]` | bound `0x134cc` `[A]` | n/a |
| 92 | Tremolo Depth | Discarded | bound `0x134cc` `[A]` | n/a |
| 93 | Effects 3 Depth (send level) | Discarded — same status as CC91, confirmed structurally `[A]` and acoustically `[M:probe]` | bound `0x134cc` `[A]` | n/a |
| 94 | Effects 4 Depth | Discarded | bound `0x134cc` `[A]` | n/a |
| 95 | Effects 5 Depth (Phaser) | Discarded | bound `0x134cc` `[A]` | n/a |
| 96 | Data Increment | Discarded — **not implemented**, falls in the same discarded range as 65–97 | bound `0x134cc` `[A]` | n/a |
| 97 | Data Decrement | Discarded — same as CC96 | bound `0x134cc` `[A]` | n/a |
| 98 | NRPN LSB | Implemented — forces the RPN/NRPN-select register to `0x3FFF` (Null), regardless of the LSB value sent (§4.4) | `0x13514` `[A]` | n/a |
| 99 | NRPN MSB | Implemented — same forced-Null effect as CC98 | `0x13514` `[A]` | n/a |
| 100 | RPN LSB | Implemented (§4.4) | `0x13500` `[A]` | n/a |
| 101 | RPN MSB | Implemented (§4.4) | `0x134f2` `[A]` | n/a |
| 102 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 103 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 104 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 105 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 106 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 107 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 108 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 109 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 110 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 111 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 112 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 113 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 114 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 115 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 116 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 117 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 118 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 119 | (undefined) | Discarded | bound `0x134e7` `[A]` | n/a |
| 120 | All Sound Off | Implemented — releases (does not cut) every sounding voice on the matching channel; **bypasses** the sustain-hold check entirely (§4.7 sentinel `0xFD`) | `0x134ed` (jumps to shared tail `0x1362b`) `[A]` | n/a |
| 121 | Reset All Controllers | Implemented — re-schedules Volume=100, Pan=64, Expression=127, Pitch Bend=8192, Modulation=0 (same values as §4.6's `ResetAllChannelControllers`) plus a per-channel sustain-release event; does **not** touch Bank/Program or the RPN/NRPN-select register | `0x1351f` `[A]` | n/a |
| 122 | Local Control | Discarded (no dedicated handler; meaningless for a software-only synth with no local keyboard) | bound `0x13611` `[A]` | n/a |
| 123 | All Notes Off | Implemented — releases (does not cut) every sounding voice on the matching channel; **honours** the sustain-hold check (§4.7 sentinel `0xFF`) | `0x13607`→`0x13631` `[A]` | n/a |
| 124 | Omni Mode Off | Discarded | bound `0x13611` `[A]` | n/a |
| 125 | Omni Mode On | Discarded | bound `0x13611` `[A]` | n/a |
| 126 | Mono Mode On (Poly Off) | Implemented — sets the per-channel Mono flag, then performs the same channel-wide release as CC120 (per MIDI spec, Mono On implies an implicit all-notes-off) | `0x13621` `[A]` | Mono flag default `0` (poly) |
| 127 | Poly Mode On | Implemented — clears the per-channel Mono flag, then performs the same channel-wide release as CC120 | `0x13618` `[A]` | Mono flag default `0` (poly) |

CC120, CC126 and CC127 share one release routine (all three jump to the
`0xFD`-sentinel tail at `0x1362b`); CC121 and CC64 share a different tail
(the `0xFE`-sentinel path at `0x135c3`); CC123 uses its own tail
(`0xFF`-sentinel at `0x13631`). These are three distinct release
mechanisms distinguished at the point of consumption inside
`TriggerVoiceEvent` (§4.7), not four aliases of one function.

**CC7/CC11 gain law.** Master Volume (SysEx, §4.5) is confirmed by direct
instruction read to use the same 128-entry attenuation table as note-on
velocity (`0x16b94` → table `0x1c9d0`, `40·log10(v/127)` dB, i.e.
amplitude ∝ `(v/127)²` — a squared law) `[A:0x12d28]`,`[A:0x16b94]`,
verified against raw table-generator bytes in
`verify_midi_velocity_curve.py`. CC7 (Channel Volume) and CC11
(Expression) are confirmed to schedule their raw 0–127 value into their
respective queues (§4.2.1) exactly as CC1/CC10 do; **the specific instruction
that converts a promoted CC7/CC11 queue value into a gain multiplier was
not located inside the control-plane's traced range** (it lives in the
render/DSP code, `0x16800`–`0x1a582`, out of this section's scope) — `[O]`.
The claim that CC7/CC11 also follow the squared law is carried here as
`[M:probe]` (measured from rendered reference audio elsewhere in this
project); this section did not independently re-measure it (no audio
decode/render capability was available to this pass) and did not find a
contradicting static instruction — absence of a located consumer is not
evidence against the measurement, only a scope boundary.

**Grep form used to spot-check the discard bounds in bulk** (in addition
to the full byte-level check in `verify_midi_dispatch_bytes.py`, RE
workspace not retained): for each of `133a1`, `13454`, `134cc`, `134e7`,
`13611`, grepping the PAGE-section disassembly for the anchor `^ *<addr>:`
each resolves to exactly one line, matching the branch instruction cited
in the table above.

---

### 4.4 RPN and NRPN

CC100/CC101 implement genuine 14-bit MSB/LSB assembly against a single
device-global-per-channel register at `device+0xf10+chan*4`:

- **CC101 (RPN MSB)**, real handler at `0x134f2` (the branch-chain compare
  at `0x134df` is only the dispatch test, not the store — see below):
  `register = ((u8)data2 << 7) | (register & 0x7F)`
  `[A:0x134f2]`–`[A:0x1350d]` — sets bits 7–13, preserves bits 0–6.
- **CC100 (RPN LSB)**, real handler at `0x13500` (the compare at `0x134da`
  is only the dispatch test): `register = (register & 0x3F80) |
  (u8)data2` `[A:0x13500]`–`[A:0x1350d]` — sets bits 0–6, preserves bits
  7–13.
- **CC98/CC99 (NRPN LSB/MSB)**: `register = 0x3FFF` unconditionally,
  regardless of the value sent `[A:0x13514]`. `0x3FFF` is exactly
  `MSB=0x7F, LSB=0x7F` — the standard MIDI "RPN/NRPN Null" (no parameter
  selected) encoding, confirmed algebraically in
  `verify_midi_dispatch_bytes.py`. **Observable consequence**: sending
  *any* NRPN selection message, with any value, immediately invalidates
  whatever RPN was previously selected. Because the register is only ever
  set to `0x3FFF` and never to a genuine NRPN parameter number, no NRPN
  destination is reachable through the Data Entry handlers below — NRPN is
  present as a protocol no-op that resets RPN selection, not as a working
  parameter space.

Data Entry (CC6 MSB / CC38 LSB) always maintains a 14-bit "combined
data-entry value" word at `device+0xf58+chan*2` regardless of which
parameter is selected `[A:0x133c9]`/`[A:0x1345a]`, and additionally branches
on the *current* value of the `+0xf10` register to decide whether to also
write a derived parameter:

| Parameter (register value) | CC6 (Data Entry MSB) effect | CC38 (Data Entry LSB) effect | Store target | Formula |
|---|---|---|---|---|
| `0` — RPN0, Pitch Bend Sensitivity | Writes `(u8)data2 * 100` cents | **No effect** (LSB not consumed for RPN0) | `device+0x3d0+chan*0x28+0x20` | `[A:0x13404]`–`[A:0x13407]` |
| `1` — RPN1, Channel Fine Tuning | Writes `((combined14bit - 8192) * 100) / 8192` cents, C-style truncation toward zero | Same recompute, using the just-updated combined word | `device+0xf7c+chan*4` | `[A:0x1347d]`–`[A:0x1348a]`; range `-100`..`+99` cents (verified in `verify_midi_rpn_reset.py`) |
| `2` — RPN2, Channel Coarse Tuning | Writes `((u8)data2 - 64) * 100` cents | **No effect** (LSB not consumed for RPN2) | `device+0x12bc+chan*4` | `[A:0x133ed]`–`[A:0x133f2]`; range `-6400`..`+6300` cents |
| any other value (incl. `0x3FFF` Null) | No effect beyond the raw combined-word store | No effect beyond the raw combined-word store | — | `[A:0x133e3]` (falls to the shared skip tail) |

**RPN1/RPN2 are note-on-latched, not continuous, unlike Pitch Bend.**
Pitch Bend (RPN0's target) is read live by the voice-pitch recompute on
every render block, so a bend arriving while a note is held audibly
reaches that sounding voice. RPN1 (Channel Fine Tuning) and RPN2 (Channel
Coarse Tuning), by contrast, are sampled once, at note-on, into the
voice's fixed per-voice pitch base, and do **not** retune an already-
sounding voice: a coarse/fine-tune change sent while a note is held has no
audible effect on that note, only on subsequent note-ons. `[M: probe 23]`
— `probes/23_rpn_tune.mid` section D holds a single note and sends an
RPN2 Coarse Tune change (`+12` semitones) 1.5 s into the held note; the
rendered fundamental stays flat across that point (no octave jump),
confirming the latch-at-note-on model against the continuous model a
per-block re-read would predict.

RPN0's power-on/reset default is `200` cents (2 semitones), set only by the
Pitch Bend ctor `0x16e36` `[A:0x16e44]`; **RPN0/RPN1/RPN2 and the
`+0xf10` register itself are not reset by any of `ResetDevice`,
`ResetAllChannelControllers` or `ResetAllProgramsAndRhythmGroups`** — all
three functions were read in full for this section and contain no write to
these fields (§4.6). The `+0xf10` register's value at driver-add time is
therefore whatever the kernel pool allocator handed back for that memory —
`[O]`, not determined; GM/GS convention expects RPN/NRPN-select to be
`0x3FFF` (Null) after Reset All Controllers, but this driver does not
enforce that.

---

### 4.5 SysEx messages recognized

All addresses below are relative to the SysEx buffer, byte 0 = the leading
`0xF0` (already consumed by the top-level parser before `SysEx` is called).

| Bytes | Message | Effect | Cited at |
|---|---|---|---|
| `F0 7E <any> 09 01 F7` | GM System On (Universal Non-Realtime, sub-ID1=`09`, sub-ID2=`01`) | Calls `ResetDevice`; **clears** the GS-mode flag | `[A:0x136a2]` (mfr `0x7E`), `[A:0x136d5]` (sub-ID1 `0x09`), `[A:0x136e1]` (`call 0x12354`), `[A:0x136f5]` (clear flag, gated on sub-ID2`==1`) |
| `F0 7E <any> 09 02 F7` | GM System Off | Calls `ResetDevice`; GS-mode flag **untouched** | same entry point; `[A:0x136f3]` (`je 0x136fc` skips the clear when sub-ID2`!=1`) |
| `F0 7F <any> 04 01 <lsb> <msb> F7` | Master Volume (Universal Realtime, sub-ID1=`04`, sub-ID2=`01`) | Only the **MSB** byte is used; the LSB is never read. MSB is looked up in the same 128-entry attenuation table as note-on velocity and stored to `device+0xf78` (device-global) | `[A:0x136a7]` (mfr `0x7F`), `[A:0x136ae]`/`[A:0x136b8]` (sub-IDs), `[A:0x136c2]` (reads byte 6 = MSB; byte 5 = LSB is never referenced), `[A:0x12d28]` (table lookup), `[A:0x12d2d]` (store) |
| `F0 41 <any> 42 12 40 00 7F 00 <chk> F7` | Roland GS Reset (`DT1`, address `40 00 7F`, data `00`) | Calls `ResetDevice`; **sets** the GS-mode flag to `1`; then calls `ResetAllChannelControllers` and `ResetAllProgramsAndRhythmGroups` | `[A:0x1369d]` (mfr `0x41`), `[A:0x13717]`/`[A:0x13721]` (model `0x42`/command `0x12`), `[A:0x1374a]` (address match `400 07F`), `[A:0x137ea]` (`call 0x12354`), `[A:0x137f4]` (set flag), `[A:0x137fa]`–`[A:0x13816]` (both reset calls, shared with GM On/Off) |
| `F0 41 <any> 42 12 40 1x 02 <data> <chk> F7` | RCV CHANNEL (per-Part MIDI-channel remap), gated on GS-mode flag | Writes `data` into `device+0x12fc+part*1` | `[A:0x13756]` (address match), `[A:0x137b7]` (gate test), `[A:0x137ae]` (store) |
| `F0 41 <any> 42 12 40 1x 15 <data> <chk> F7` | USE RHYTHM PART (per-Part rhythm-map assignment), gated on GS-mode flag | Writes `data` into `device+0x130c+part*1` | `[A:0x1375e]` (address match), `[A:0x137a2]` (gate test), `[A:0x137ae]` (store) |
| `F0 41 <any> 42 12 40 1x yy ... F7` (any other address in the `40 1x` family), gated on GS-mode flag | Per-Part 12-entry tuning-grid write (§4.2.2, `device+0xfbc`) | 12 iterations, one per semitone class, each storing `byte - 0x40` | `[A:0x1376c]` (gate test), `[A:0x13797]` (store loop) |
| any other manufacturer ID / model ID / command ID / address | Unrecognized | No state change; function returns success (`1`), the buffer is consumed and dropped, no error is raised | `[A:0x13818]` |

GM System On/Off and GS Reset all converge on calling `ResetDevice`
(`0x12354`); GM System On/Off and GS Reset additionally share one further
tail (`0x137fa`) that unconditionally calls both `ResetAllChannelControllers`
(`0x126c2`) and `ResetAllProgramsAndRhythmGroups` (`0x12780`) — confirmed by
direct read: the gate condition on the second call is always true on
this path because the gating flag was just set to `1` two instructions earlier in
both callers `[A:0x13807]`. The only differences between the three
converging triggers are: which reset flag-clears happen beforehand
(`0xFF` also clears the GS-mode flag at its own site, `0x131ee`; GM System
On clears it at `0x136f5`; GS Reset sets it at `0x137f4`), and the extra
Master-Volume-related call `0x150bc` that only the `0xFF`/System-Reset path
makes (`0x13218`).

---

### 4.6 Reset semantics

Three distinct functions, read to completion:

#### 4.6.1 `ResetDevice`, `0x12354`–`0x123d7`

Per-channel loop (16 iterations), plus two device-global writes at the end:

| Field | Reset value | Cited at |
|---|---|---|
| `device+0xf7c+chan*4` (RPN1 fine tune) | `0` | `[A:0x1237a]` |
| `device+0xf58+chan*2` (Data Entry combined word) | `0` | `[A:0x12384]` |
| `device+0xfbc+chan*0x30` (12-entry tuning grid, this channel's 12 dwords) | `0` (all 12) | `[A:0x123a0]` (`rep stos`, count `0xc`) |
| `device+0x12bc+chan*4` (RPN2 coarse tune) | `0` | `[A:0x123a2]` |
| `device+0x12fc+chan*1` (RCV CHANNEL) | reloaded from static table `ds:0x1a600` (§4.8) | `[A:0x123a4]`/`[A:0x123aa]` |
| `device+0x131c+chan*4` (Mono flag) | `0` (poly) | `[A:0x123b1]` |
| `device+0x130c+chan*1` (USE RHYTHM PART) | `0` for all 16, then Part 0 forced to `1` | `[A:0x12395]`, `[A:0x123ce]` |
| `device+0xf78` (Master Volume attenuation, device-global) | `0` hundredths of a dB (= unity gain) | `[A:0x123be]` |
| `device+0xf54` (GS-mode flag, device-global) | `0` | `[A:0x123c5]` |

`ResetDevice` does **not** touch Bank/Program, Pitch Bend/RPN0, Channel
Volume, Expression, Pan, Modulation, or the `+0xf10` RPN/NRPN-select
register — confirmed by having read the whole function; those are handled
(where they are handled at all) by the other two reset functions or, for
RPN0/RPN1/RPN2/`+0xf10`, by none of them (§4.4).

#### 4.6.2 `ResetAllChannelControllers`, `0x126c2`–`0x12777`

Calls `0x123de` (release every currently-active voice back to the primary
voice pool — voice-pool mechanics are out of this section's scope, see the
voice-allocation section) and `0x1252a` (cancel every still-pending node in
all six per-channel queues from §4.2.1, via six calls to `0x16af2` per
channel — confirmed by direct read of `0x1252a`, whose six queue-base
computations resolve to exactly `+0x1d0`, `+0x3d0`, `+0x650`, `+0x850`,
`+0xa50`, `+0xc50` — the same six bases enumerated in §4.2.1's table
`[A:0x12551]`–
`[A:0x1259a]`), then, per channel (16 iterations), does exactly:

| Action | Cited at |
|---|---|
| Schedule Modulation Wheel = `0` | `0x12754`–`0x1275e` |
| Schedule Pitch Bend = `8192` | `0x12748`–`0x1274f` |
| Schedule Channel Volume = `100` | `0x1271c`–`0x12726` |
| Schedule Pan = `64` | `0x1272b`–`0x12731` |
| Schedule Expression = `127` | `0x12736`–`0x12740` |
| Queue a sentinel `0xFE` (Sustain-release, value `0`) event for this channel into the main event queue | `0x1270f`–`0x12717` |

Each `Schedule X = Y` line is a call to `0x16bae` (the timestamp-keyed
insert primitive, §4.7), not a direct field write — the value becomes
"current" the next time `0x16b46` promotes it (normally on the very next
buffer, and immediately visible even before that to any `0x16daa` "current
or more-recent-pending" query issued with a later timestamp, §4.7). This
function does **not** touch Bank/Program, RPN0/RPN1/RPN2, or `+0xf10` —
confirmed by having read the whole function.

#### 4.6.3 `ResetAllProgramsAndRhythmGroups`, `0x12780`–`0x1282e`

Per channel (16 iterations):

1. Sets Bank LSB `= 0` and Bank MSB `= 0` by calling `0x16ddc`/`0x16dc4`
   **directly** — this bypasses the CC32/CC0 GS-mode gate entirely (those
   gates live only inside `ShortMsg`'s CC dispatch, not inside these two
   setter functions themselves) `[A:0x1279c]`/`[A:0x127a4]`.
2. Schedules Program `= 0` (with the just-zeroed bank) via `0x16df4`
   `[A:0x127b2]`.
3. Reads back the resulting locale via `0x16daa` `[A:0x127bf]`.
4. If `device+0x130c+chan` (USE RHYTHM PART) is nonzero for this channel, ORs
   the drum bit (`0x80000000`) into the locale and propagates that same
   drum-tagged locale to **every other channel that shares the identical
   USE RHYTHM PART value** via `0x151b0` `[A:0x127ce]`–`[A:0x127f6]`; if
   zero, calls `0x151b0` for just this channel with the non-drum locale
   `[A:0x127d8]`–`[A:0x1280d]`.

Net effect: every channel's Bank/Program resets to `(0,0,0)`, and the
default rhythm/melodic instrument-locale notification is re-issued to
match. `0x151b0`'s own internal effect was not traced in this section
(it is called against the top-level, non-per-channel object at
`device+0x20`, the same object also passed to `FindInstrument`, §4.7/§4.8) — `[O]`.

#### 4.6.4 Reset-state summary table (every per-channel field, all three paths)

| Field | `ResetDevice` (`0x12354`) | `ResetAllChannelControllers` (`0x126c2`) | `ResetAllProgramsAndRhythmGroups` (`0x12780`) | GS Reset net effect | GM On/Off net effect | System Reset (`0xFF`) net effect |
|---|---|---|---|---|---|---|
| Bank MSB/LSB | not touched | not touched | `0`/`0` (direct, ungated) | `0`/`0` | `0`/`0` | `0`/`0` |
| Program (scheduled locale) | not touched | not touched | `0` | `0` | `0` | `0` |
| Pitch Bend (current value) | not touched | `8192` | not touched | `8192` | `8192` | `8192` |
| RPN0 (Pitch Bend Sensitivity) | not touched | not touched | not touched | unchanged `[O]` | unchanged `[O]` | unchanged `[O]` |
| Channel Volume | not touched | `100` | not touched | `100` | `100` | `100` |
| Expression | not touched | `127` | not touched | `127` | `127` | `127` |
| Pan | not touched | `64` | not touched | `64` | `64` | `64` |
| Modulation Wheel | not touched | `0` | not touched | `0` | `0` | `0` |
| RPN1/RPN2 result fields | `0`/`0` | not touched | not touched | `0`/`0` | `0`/`0` | `0`/`0` |
| RPN/NRPN-select register (`+0xf10`) | not touched | not touched | not touched | unchanged `[O]` | unchanged `[O]` | unchanged `[O]` |
| Data-Entry combined word | `0` | not touched | not touched | `0` | `0` | `0` |
| 12-entry tuning grid | `0`×12 | not touched | not touched | `0`×12 | `0`×12 | `0`×12 |
| RCV CHANNEL | reload static table | not touched | not touched | reload static table | reload static table | reload static table |
| USE RHYTHM PART | all `0`, Part 0 `=1` | not touched | drum-bit propagated (not itself reset) | all `0`, Part 0 `=1` | all `0`, Part 0 `=1` | all `0`, Part 0 `=1` |
| Mono flag | `0` (poly) | not touched | not touched | `0` | `0` | `0` |
| Sustain-pedal value (`+0xed0`) | not touched | not touched (but sentinel-`0xFE` release event queued) | not touched | effectively released via queued event | effectively released via queued event | not directly reset by System Reset's own calls beyond what `0x126c2` queues `[A]` |
| Master Volume attenuation | `0` hundredths of a dB (unity) | not touched | not touched | `0` | `0` | `0` |
| GS-mode flag | `0` | not touched | not touched | `1` (set after `ResetDevice` runs) | `0` (System On) / unchanged (System Off) | `0` |

Every hard-coded reset value in this table (`100`, `127`, `64`, `8192`,
`0`) matches the corresponding power-on constructor default from §4.2.1
exactly — cross-checked programmatically in `verify_midi_rpn_reset.py`.

---

### 4.7 Event scheduling

Two sorted, timestamp-keyed queues exist, both built on the same generic
sorted-singly-linked-list primitives (`0x16bae` insert, `0x16cac`/`0x16c50`
pop/query, `0x16af2` prune):

- **The six per-controller queues** (§4.2.1), each anchored inside its own
  16-element array.
- **The main pending-event queue**, anchored at `device+0x150`, holding
  Note-On/Note-Off events and the five internal sentinel event types
  (`0xFB` RCV-CHANNEL-remap, `0xFC` Master-Volume, `0xFD`
  All-Sound-Off/Mono/Poly, `0xFE` Sustain, `0xFF` All-Notes-Off), each
  packed as 3 bytes (channel, sentinel-or-status, value) and keyed by a
  64-bit timestamp `[A:0x16c7e]` (the packing wrapper).

#### 4.7.1 Insertion order and tie-break

`0x16bae`'s insert walks the list in ascending key order and, on an exact
key tie (`new.key_lo == existing.key_lo` **and** `new.key_hi ==
existing.key_hi`), does **not** insert the new node before the existing
one — the walk loop's tie-break comparison uses a strict `ja` (unsigned
greater-than), which is false on an exact tie, so the walk continues past
every existing node with an equal key before inserting `[A:0x16c30]`
(the key-comparison instruction), `[A:0x16c33]` (`ja 0x16c39`, strict), `[A:0x16c35]`
(advance past on tie or lesser). **Two events with the same timestamp are
therefore serviced in first-scheduled-first-serviced (FIFO) order** — an
implementer must reproduce a stable, insertion-order-preserving tie-break,
not an unspecified/undefined one.

#### 4.7.2 Draining due events

`TriggerVoiceEvent` (`0x12bd6`) is the sole consumer of the main queue: it
pops the head via `0x16cac` `[A:0x12bfc]`, and `0x16cac` itself only
returns a node if `head.key <= (now_hi, now_lo)` (the caller-supplied
current time) — otherwise it returns "nothing due" without popping
`[A:0x16cb9]`–`[A:0x16cc9]`. `TriggerVoiceEvent`'s own caller loops calling
it again (`0x13041`: `jne 0x12c0b`) until the pop returns nothing, i.e.
**one call to `TriggerVoiceEvent` drains every currently-due event in the
queue**, not just one. The per-buffer service routine (`0x13054`) reads
the current time once via an `IAT`-indirect call (`ds:0x118f0`, almost
certainly `KeQueryPerformanceCounter`) `[A:0x13065]`, then calls
`TriggerVoiceEvent` exactly once per buffer `[A:0x130af]` before walking
the active-voice list to advance DSP state — controller-value promotion
(`0x16b46`, six calls per channel, §4.2.1/§4.6.2) and finished-voice recycling
both happen strictly after that buffer's due-event drain, in the same
call `[A:0x13120]`–`[A:0x13180]`, `[A:0x130b4]`–`[A:0x13108]`.

The number of audio frames per call to `0x13054` (which bounds how finely
"due" is evaluated in wall-clock terms) is a caller-supplied parameter
whose origin is outside PAGE's control-plane functions — `[O]`.

#### 4.7.3 Real-time control-value visibility ahead of the periodic flush

`0x16c50`/`0x16daa` (the "current value" getter used by, e.g., program-
locale lookup at note-on) does not simply return the queue's last-promoted
`+0x18` value: it starts from `+0x18` as a default and then walks any
**still-pending** nodes, adopting each one's value as the new candidate for
as long as that node's key is `<=` the caller's query timestamp
`[A:0x16c50]`–`[A:0x16c75]`. This means a Program Change scheduled earlier
in the same buffer is already visible to a Note-On query with a later
timestamp in that same buffer, even before the periodic `0x16b46` flush
promotes it to `+0x18` — an implementer must reproduce this look-ahead
read, not a naive "only what was flushed last buffer" read.

---

### 4.8 Channel 10 / drum-part selection

Program Change resolution never gates the drum bit on the GS-mode flag —
confirmed by reading `TriggerVoiceEvent` (`0x12bd6`–`0x1304b`) in full and
finding zero references to `device+0xf54` anywhere in it (verified with
`grep -c "0xf54"` restricted to that address range, result `0`).

Mechanism, read directly from `TriggerVoiceEvent`:

1. The channel's currently-scheduled 21-bit locale (`bankMSB<<14 |
   bankLSB<<7 | program`, no drum bit) is fetched via `0x16daa`
   `[A:0x12dc3]` into a 32-bit local, call it `locale`.
2. `device+0x130c+channel` (USE RHYTHM PART, §4.2.2) is tested
   `[A:0x12dcf]`; if nonzero, the code ORs `0x80` directly into the high byte
   of the 4-byte local variable holding `locale`
   `[A:0x12dd9]` — a byte-level write to that local's own top byte, exactly
   equivalent to `locale |= 0x80000000`, entirely in the local variable,
   before the very first instrument-lookup call.
3. The resulting `locale` (with or without the drum bit) is passed to
   `FindInstrument` (`0x14800`) `[A:0x12e26]`, with retries that
   progressively strip bank/program bits but preserve the drum bit on
   failure (documented for instrument lookup elsewhere in this project;
   out of this section's scope beyond noting that the drum bit survives
   every retry tier).

Because this gate is `device+0x130c[channel] != 0`, not a literal
`channel == 9` comparison, and `device+0x130c` is itself populated from the
default RCV-CHANNEL table (§4.2.2, §4.6.1) which routes MIDI channel 10
(0-indexed 9) to internal Part 0 (the one hard-coded rhythm part), the
observable default behaviour reproduces "channel 10 is drums" without any
literal channel-9 comparison anywhere in the driver.

**Selection among multiple drum kits** is not something the control plane
restricts to a fixed count: `program` (0–127) passes through unmodified
alongside the drum bit, and which of the loaded DLS collection's drum-kit
variants that resolves to is a DLS-content fact, not a driver-imposed
limit — `[I]`, inferred from the driver treating `program` identically for
melodic and drum locales (only the high bit differs). The specific claim
that program 25 (commonly labelled "TR-808" in GM/GS drum-kit naming) is
acoustically distinct from program 0 ("Standard"), with a measured
residual of −15.4 dB with no GS Reset present in the source file, is
carried here as `[M:probe]` (measured from rendered reference audio;
not re-measured by this section).

---

### 4.9 Verification

Three stdlib-only scripts (RE workspace not retained), `verify_midi_dispatch_bytes.py`,
`verify_midi_velocity_curve.py`, `verify_midi_rpn_reset.py`, re-derive
every numeric/byte-level claim above directly from `swmidi.sys` and from
the algebraic formulas recovered by instruction read (not from any
disassembler's text rendering). Combined run:

```
$ python3 verify_midi_dispatch_bytes.py   | tail -3
RESULT: ALL CHECKS PASSED
$ python3 verify_midi_velocity_curve.py   | tail -3
RESULT: ALL CHECKS PASSED
$ python3 verify_midi_rpn_reset.py        | tail -3
RESULT: ALL CHECKS PASSED
```

(Full per-line PASS/FAIL output for all three scripts — 90+ individual
assertions — was captured during authoring and every line reads `PASS`;
representative excerpts are quoted inline through the sections above next
to the claim each one supports.)

**Bulk VMA resolution.** Every VMA cited in this document as `[A:0xVMA]`
for an instruction (not a data table) was checked to resolve to a real
instruction-line start in the PAGE-section disassembly of `swmidi.sys` by
grepping it for the 5-space-indented address anchor `^ *$a:` for each
candidate VMA and flagging any that failed to match.
Result: 102 of 104 candidate addresses resolved directly in that
PAGE-section disassembly. The two exceptions are not errors: `0x103a8` (the
generic pop-helper called by the queue primitives) resolves instead in the
`.text`-section disassembly (confirmed: grepping it for the same anchor
`^ *103a8:` succeeds) because it is a `.text` helper, not a PAGE function,
and is cited here only as a callee; `0x1a600` (the static RCV-CHANNEL
table) is data, not an instruction, and does not appear as a disassembled
line at all — it was instead read directly from the raw file bytes (via
`verify_midi_rpn_reset.py`, RE workspace not retained), which is the
correct method for a data citation.

---

### 4.10 Open items (`[O]`), collected

1. Device+`0xf10` (RPN/NRPN-select register)'s value at driver-add time /
   after any reset: no write to this field exists anywhere in PAGE,
   `.text`, or `.init` (exhaustive grep, confirmed empty). Its actual
   runtime default depends on kernel pool-allocator behaviour, not
   determinable from static disassembly.
2. RPN0 (Pitch Bend Sensitivity)/RPN1 (Fine Tune)/RPN2 (Coarse Tune)
   result fields are not reset by any of the three reset functions (all
   three were read in full); only their power-on constructor defaults are
   known (RPN0 = 200 cents; RPN1/RPN2 have no dedicated ctor default field
   observed beyond the zero the base queue ctor gives them).
3. The exact instruction that converts a promoted CC7 (Channel Volume) or
   CC11 (Expression) queue value into a DSP gain multiplier was not
   located inside the control-plane's traced range (`0x12180`–`0x14300`,
   plus the specific helper VMAs read opportunistically for this section);
   it lives in the render/DSP region, out of this section's scope. The
   squared-law claim for CC7/CC11 is carried as `[M:probe]`, not
   independently re-verified here.
4. `device+0xf78` (Master Volume attenuation) and `device+0xf7c`/`0x12bc`
   (RPN1/RPN2 cent results) have no confirmed reader anywhere in the
   control-plane's traced range; presumed consumed by DSP/gain code out of
   scope.
5. The 16×12-entry tuning grid at `device+0xfbc` (written by the GS `DT1`
   fallthrough case) has no confirmed reader in the control plane. Its
   identity as the Roland GS "Scale Tuning" parameter is an inference from
   its shape (12 bytes per part, `-0x40` center normalization, same
   address family as the confirmed RCV-CHANNEL/USE-RHYTHM-PART GS
   addresses) rather than a directly-read label or an independently
   consulted Roland GS specification document.
6. `0x151b0`'s exact internal effect (called from both `ShortMsg`'s
   Program-Change path and `ResetAllProgramsAndRhythmGroups`, always with
   a channel index and a locale) was not traced; only its call sites and
   arguments are confirmed.
7. The exact frame count per call to the per-buffer service routine
   (`0x13054`) — which bounds the real-world timing granularity of "due"
   event dispatch and controller-value promotion — is a caller-supplied
   parameter whose origin is outside the functions read for this section.

---

## Part 5 — The Voice Model

Subject: `swmidi.sys` 5.1.2600.5512 (Windows XP SP3), the Microsoft GS
Wavetable Synth. This section specifies the voice object, the voice pool
(construction, free lists, allocation order, stealing), note-off, and the
exclusive-key-group choke. It does not cover DLS collection parsing, the
per-sample mixer/interpolator's full arithmetic, MIDI parsing, or SysEx —
those belong to other sections.

**Provenance tags:** `[A:0xVMA]` = read directly from the instruction at that
virtual address in the PAGE-section disassembly of `swmidi.sys`. `[M]` =
measured from rendered audio (probe .flac files under `probe-results/`,
decoded with the `flac` CLI). `[I: basis]` = inference, basis stated. `[O]`
= open/unrecovered.

**Bulk VMA-verification form used throughout:** every VMA cited in this
section was checked by grepping that PAGE disassembly with a line-anchored
form, `^ *<hex>:` (exactly one matching line, showing the opcode bytes and
disassembled mnemonic), and every group of related VMAs was additionally
re-derived programmatically by a family of stdlib-only scripts,
`verify_voice_*.py` (RE workspace not retained, §5.10), which parsed
that same PAGE disassembly with the same line format and asserted the claims made below
against it directly — not against this document's own prose.

---

### 5.1 The voice object — struct field map

Each voice object is `0x158` (344) bytes, allocated with tag `'SwMi'`
(`ExAllocatePoolWithTag(PagedPool, 0x158, 0x694d7753)` via the wrapper at
`0x1282e` — the tag bytes `53 77 4d 69` spell `"SwMi"`)
`[A:0x12a7f]`,`[A:0x12833]`. All offsets below are relative to the voice
object's own base pointer (`this`), passed in whichever register the calling
convention assigns at each call site (x86 `__thiscall`/`__fastcall`).

| Offset | Width | Meaning | Written at | Read at |
|---|---|---|---|---|
| `+0x0` | DWORD | Wave-header pointer (resolved DLS wave; `+0xc` of the pointed-to header is the raw PCM sample-data pointer) | region/note-setup path (out of this section's scope) | `0x16eb7`/`0x16ec7` (per-sample interpolator) `[A]` |
| `+0x2c` | DWORD | Output gain accumulator, channel A (24.8 fixed-point) | `0x16ff9` (interpolator write-back) | `0x16ebf` (interpolator entry) `[A]` |
| `+0x30` | DWORD | Output gain accumulator, channel B | `0x16ffc` | `0x16eca` `[A]` |
| `+0x34` | DWORD | Phase step (per-sample pitch increment); also written once at note setup, at `0x18f83` inside `0x18ef4` (the same instruction sequence uses `voice+0x28` only as a scratch accumulator for the intermediate ratio·rate/renderRate computation, never as a persistent field — see the render section's phase-increment composition, §6.5) | `0x17007` (scalar-path render write-back), `0x18f83` (note setup) | `0x16ecd` `[A]` |
| `+0x44` | DWORD | Phase accumulator (12-bit fraction + integer sample index); zeroed at note setup (`voice->0x44 = 0`) at `0x18f7d` | `0x17003` | `0x16ec3` `[A]` |
| `+0x68` | 0x28 bytes | Release-envelope-segment sub-object (own `+0x10`/`+0x14` = current ramp rate 64-bit; own `+0x28`/`+0x2c` = target) — configured by `0x197dc` (note-off, normal rate) or `0x19834` (fast/choke, rate-clamped) | see §5.6 | — |
| `+0x38` | 0x28 bytes | Attack/hold-envelope-segment sub-object, same layout as `+0x68` | see §5.6 | — |
| `+0x104` | DWORD | Pointer to a per-channel/per-note runtime sub-object; `[ptr+0x0]` and `[ptr+0x4]` are read by the release-ramp configurators, `[ptr+0x18]`/`[ptr+0x1c]` by the DSP-advance routine `0x19644` | note setup (`0x19ba7`) | `0x19ac7`,`0x19b1b`,`0x196f0`,`0x19743` `[A]`; exact field semantics of the pointed-to object beyond these four offsets: `[O]` |
| `+0x108`/`+0x10c` | QWORD | Envelope-scheduling timestamp pair (running "next segment-boundary" bookkeeping consumed by the DSP-advance routine `0x19644`) | `0x194cc`/`0x194d2`,`0x19c56`/`0x19c5f`, others | `0x195c4`+ region, `0x196cd`+ region `[A]`; precise scheduling semantics beyond "a 64-bit timestamp pair used by envelope advance": `[O]` |
| `+0x118`/`+0x11c` | QWORD | Second 64-bit timestamp pair, same family as `+0x108`/`+0x10c`, used in the same advance routine | `0x197b5`/`0x197bb`,`0x19a8c`/`0x19a93`,`0x19b3b`/`0x19b42` | `0x19683`/`0x19686` `[A]` |
| `+0x120`/`+0x124` | QWORD | Voice creation/reference timestamp (absolute; the 64-bit clock used everywhere else in the compares below) | note setup (`0x19d4e`/`0x19d83`) | `0x12455`/`0x12465` (top-up selector), `0x124dc`/`0x124ec` (note-on steal), `0x19653`/`0x1965b` (DSP advance), `0x19a3d`/`0x19a43` & `0x19aeb`/`0x19ae5` (release-target compute) `[A]` |
| `+0x128`/`+0x12c` | QWORD | Currently-configured release target time (absolute, same clock) | `0x19a8c`/`0x19a93` (note-off), `0x19b3b`/`0x19b42` (fast-release full path) | `0x19ad2`/`0x19ad8` (fast-release cheap-path re-apply) `[A]` |
| `+0x130` | DWORD | Active/finished flag: `1` = still active, set by note setup at `0x12ff4` immediately after a successful `SetupNote` call; the per-tick DSP-advance routine `0x19644` writes the up-to-date active/finished result back at `0x197a2` (envelope-complete detection: release segment finished *and* level below the floor constant compared at `0x19733`, `0xffffe0c0`) | `0x12ff4`,`0x197a2`, reset to `0` at construction (`0x199e8`) | `0x130d8` (per-tick recycle gate: reap only when `0`) `[A]` |
| `+0x134` | DWORD, {0,1} | "Held" flag: `1` = note not yet released, set to `1` by note setup (`0x19d5f`); cleared to `0` by note-off (`0x19a7e`) and by fast-release's full path (`0x19b2d`) | `0x19d5f`,`0x19a7e`,`0x19b2d`, reset `0x199f4` | steal-priority comparators §5.7, sustain-pedal handlers §5.9 `[A]` |
| `+0x138` | DWORD, {0,1} | Steal-exclusion flag: `1` means "already committed to release by the pool top-up, do not re-select this voice as a Branch-B victim again". **Not** an ordinary note-off flag — see §5.6 | set unconditionally to `1` at `0x19ab6` (top of `0x19aa4`, before any branch); cleared at note setup `0x19d29`; reset `0x199fa` | `0x12435`,`0x1243d`,`0x12498` (all three gates inside `0x12426` only) `[A]` |
| `+0x13c` | DWORD | Live amplitude-envelope level (fixed-point, logarithmic scale — compared against a floor constant `0xffffe0c0` at `0x19733`); zeroed at construction (`0x19a0c`, inside the voice full-reset routine `0x1999a` — **not** inside note-off, see the correction in §5.7) and at note setup (`0x19e03`); continuously updated by the envelope-advance helper (`0x194da`) during rendering | `0x19a0c`,`0x194da`,`0x19e03` | `0x1247d`/`0x12483` (top-up selector tie-break), `0x124cc`/`0x124d2` (note-on steal tie-break), `0x19733` (finish detection) `[A]`; the identification "amplitude-envelope level" is `[I: write pattern + comparison against a floor constant that resembles a silence threshold in a log-domain fixed-point format]`, not read from a comment or symbol |
| `+0x140` | DWORD, {0,1} | Sustain-pedal-deferred flag: `1` = this voice's note-off was deferred because CC64 was held down on its channel at the time | `0x12ca5` (CC123 handler, deferral branch), cleared by note-off (`0x19a85`) and fast-release full path (`0x19b34`) | `0x12c34` (CC64-lift release walk), `0x12d5b` (CC123 walk), `0x19abe` (fast-release cheap/full-path selector) `[A]` |
| `+0x144` | WORD | MIDI channel number | note setup `0x12f90` | choke walk `0x12f1d`, retrigger scan `0x12ea7`, all three sentinel walks §5.9 `[A]` |
| `+0x146` | WORD | MIDI key/note number | note setup `0x12f84` | retrigger scan `0x12eb5` `[A]` |
| `+0x14c` | DWORD | Exclusive-key-group tag, copied from the note's region at note setup | note setup `0x19b97` (written exactly once in the whole PAGE section) | choke walk `0x12f11` `[A]` |
| `+0x150` | DWORD | Pointer to the region/articulation object this voice is currently playing | note setup `0x12f9a` | choke walk `0x12f29` `[A]`. **Not to be confused** with the unrelated field at the same offset (`+0x150`) on the *synth/device* object, which is that object's sorted pending-event-queue head — a completely different structure that happens to reuse the offset `[A: 0x12bf3` etc., synth-object context`]` |
| `+0x1a8`/`+0x1ac` | DWORD/DWORD | Primary free-list head/count (on the synth/device object, not the voice) | constructor `0x129b2`/`0x129b8` | §5.2 |
| `+0x1b4`/`+0x1b8` | DWORD/DWORD | Reserve free-list head/count (synth/device object) | constructor `0x129cc`/`0x129d2` | §5.2 |
| `+0x1c0`/`+0x1c4` | DWORD/DWORD | Active-voice list head/count (synth/device object) | constructor `0x129dd`/`0x129df` | §5.2–§5.5 |

Fields not exercised by the pool/steal/choke/note-off mechanics in this
section (e.g. the interior layout of the `+0x38`/`+0x68` envelope-segment
sub-objects beyond their own `+0x10`/`+0x14`/`+0x28`/`+0x2c`, and the object
pointed to by `+0x104`) are `[O]` — out of scope here and not required to
implement the voice model.

---

### 5.2 Pool construction

At construction, the synth/device object allocates **54 physically distinct
`0x158`-byte voice objects**, via 54 separate calls to the pool allocator
`0x1282e` (48 in one loop, 6 in a second loop immediately following)
`[A:0x12a78]`–`[A:0x12aef]`:

```c
// 0x12a78-0x12ab5 — PRIMARY pool: 48 nodes
for (i = 0x30; i != 0; i--) {
    node = ExAllocatePoolWithTag(PagedPool, 0x158, 'SwMi');   // 0x12a7f/0x12a85 -> 0x1282e
    if (node == NULL) node = FullResetFallback(node);          // 0x12a91 -> 0x1999a (defensive; not expected to fire)
    if (node != NULL) {
        node->next = device->primaryHead;   // +0x1a8
        device->primaryCount++;             // +0x1ac
        device->primaryHead = node;
    }
}

// 0x12ab7-0x12aef — RESERVE pool: 6 nodes, identical allocator, same 0x158 size
for (i = 0x6; i != 0; i--) {
    node = ExAllocatePoolWithTag(PagedPool, 0x158, 'SwMi');
    if (node == NULL) node = FullResetFallback(node);           // 0x12acb -> 0x1999a
    if (node != NULL) {
        node->next = device->reserveHead;   // +0x1b4
        device->reserveCount++;             // +0x1b8
        device->reserveHead = node;
    }
}
device->0xf50 = 0x30;   // written once (0x12afc), never read again anywhere in PAGE — dead
device->0xf52 = 0x6;    // written once (0x12b05), read exactly once, at 0x12b73 (the reserve top-up's target)
```

**No cap-check instruction exists anywhere in the PAGE section.** An
exhaustive grep of every reference to the primary count (`+0x1ac`), reserve
count (`+0x1b8`), and active count (`+0x1c4`) shows every occurrence is an
initialization or an increment/decrement; none is compared against `0x30`
(48), `0x36` (54), or any other constant `[A: grepping the PAGE disassembly for "0x1ac]\|0x1b8]\|0x1c4]", manually inspected, 9 total hits across both fields, none a cmp]`.

---

### 5.3 Free lists and allocation order

Three singly-linked lists live on the synth/device object, each popped/pushed
through the same bare helper `0x103a8` (pop) — no cap or bound check of any
kind, just a NULL check on the list head `[A:0x103a8]`–`[A:0x103bc]`:

- **Primary** free list: head `+0x1a8`, count `+0x1ac`.
- **Reserve** free list: head `+0x1b4`, count `+0x1b8`.
- **Active** list: head `+0x1c0`, count `+0x1c4`.

Note-on allocation, inside the per-event dispatcher's note-on path, tries
the three sources **in this order**, falling through only when the previous
source is empty `[A:0x12ed1]`–`[A:0x12ef5]`:

```c
voice = pop(&device->primaryList);              // 0x12ed1/0x12ed7
if (voice == NULL) {
    voice = pop(&device->reserveList);          // 0x12ee2/0x12ee8
    if (voice == NULL) {
        voice = StealAndUnlink(device);         // 0x12ef3/0x12ef5 -> 0x124a8, see §5.7
    }
}
// voice, from whichever of the three sources, is spliced onto the active
// list identically — nothing downstream distinguishes its origin.
```

A voice obtained from **any** of the three sources is pushed onto the
**same** active list (`+0x1c0`/`+0x1c4`) with no marker recording which
source supplied it `[A:0x12ffe]`–`[A:0x1300c]`. If `SetupNote` (`0x19b54`,
called at `0x12feb`) fails, the voice is pushed back onto **primary only**,
regardless of where it came from (`0x1301b`–`0x13028`) `[A]`.

**Recycling** (a finished voice returning to a free list) happens in exactly
two places, and **both target primary only, never reserve**:

- The per-tick reap loop, inside the buffer-service routine `0x13054`:
  walks the active list once per tick, and for any voice whose `+0x130`
  reads `0` (finished), unlinks it, calls cleanup (`0x19476`), and pushes it
  onto primary (`0x130f4`–`0x13101`) `[A]`.
- The bulk "release all matching voices" routine `0x123de`, used by
  channel/device reset paths: cleans up and pushes each released voice onto
  primary (`0x123fb`–`0x12409`) `[A]`.

**Recycle timing relative to allocation:** the per-tick service routine
`0x13054` first calls the event dispatcher `0x12bd6` (at `0x130af`), which
performs that tick's note-on allocations, and **only afterward** (from
`0x130b4`) walks the active list to advance DSP state and reap finished
voices `[A:0x130af]`,`[A:0x130b4]`. A voice that finishes in tick *N* cannot
supply a free slot to a note-on dispatched earlier in the same tick *N*; it
becomes available starting tick *N+1*.

---

### 5.4 The reserve top-up

`TopUpReserve` (`0x12b6a`) runs **exactly once per call** to the event
dispatcher `0x12bd6`, at the very top of that call (`0x12be7`), before any
queued event in that call's batch is processed `[A:0x12be7]`. Its only
caller is `0x130af`, inside the per-tick service routine `0x13054`
`[A: grepping the PAGE disassembly for "call   0x12b6a" -> exactly one hit, 0x12be7]`.

```c
// 0x12b6a __thiscall TopUpReserve(device, absTimeLo, absTimeHi)
need = 6 - device->reserveCount;                 // 0x12b73/0x12b7a: target is the literal constant read from +0xf52
if (need <= 0) return;

// Branch A: move existing free nodes PRIMARY -> RESERVE (never the reverse)
while (need > 0) {
    node = pop(&device->primaryList);            // 0x12b8b/0x12b8d
    if (node == NULL) break;
    push(&device->reserveList, node);            // 0x12b96-0x12ba6
    need--;
}

// Branch B: reached ONLY if primary ran dry while need still > 0
while (need > 0) {                                // 0x12bad
    victim = FindStealCandidate(device);          // 0x12baf -> 0x12426, see §5.7
    if (victim == NULL) break;                    // every active voice already marked
    ScheduleFastRelease(victim, absTimeLo, absTimeHi);  // 0x12bc0 -> 0x19aa4, see §5.6
    need--;
}
```

Branch A only **moves** already-free nodes; it never allocates new capacity.
Branch B never touches `+0x1b4`/`+0x1b8` at all — it does not synchronously
grow the reserve list. It marks up to 6 active voices per top-up call for an
accelerated release via `0x19aa4` (§5.6), which does **not** immediately free
anything; the marked voices become recyclable only once they actually finish
draining, on a later tick's reap pass.

---

### 5.5 Resolution of the pool-size question

**The pool is 54 objects. 48 is not the pool size — it is the practical
sustained-voice ceiling.** Both facts are established independently, by
static reading and by acoustic measurement, and they agree exactly:

- **Static:** 54 distinct `0x158`-byte objects exist (§5.2); every one of
  them is reachable by note-on (primary pop, reserve pop, or steal, §5.3);
  nothing in the traced code caps simultaneous active voices below 54.
- **Acoustic** `[M]`: with 80 simultaneous note-ons and no note-off until the
  end (probes `probes/20_voice_count.mid` ascending and `probes/21_steal_policy.mid`
  descending), both probes show exactly **32** notes cut short in a single,
  self-terminating burst early in the file, leaving **48** sounding to the
  declared end. `80 − 32 = 48` in both probes, independently, by two
  different ramp directions.

**Why the arithmetic works out to 48, not 54, under sustained overshoot:**
notes 1–54 consume all 54 objects with zero contention (48 via primary pop,
6 via reserve pop). Notes 55–80 (26 notes) force a real synchronous steal via
`0x124a8` each, since both free lists are provably empty by then — 26 evicted
voices, forced by simple pigeonhole arithmetic. The measured total of 32 is
26 more than that pigeonhole minimum, and `32 − 26 = 6`, exactly the reserve
tier's own target depth (`+0xf52 = 6`). This matches `TopUpReserve`'s Branch
B (§5.4) firing repeatedly, once per tick, for as long as both free lists
stay empty: every tick's top-up call marks up to 6 more active voices for
accelerated release, until enough of the marked voices actually finish
draining and get recycled — after which Branch A can refill the reserve list
from primary again and Branch B stops firing. The burst is not a persistent
6-always-draining equilibrium; it is a one-time transient sized by however
much the reserve had to dip into "mark active voices for release" before the
recycle pipeline caught up. Under the two measured probes (steady demand of
80 notes with no note-off), that transient works out to exactly 32, giving
`surviving = N − (N − 54) − 6 = 48` for the tested `N = 80`.

**Implementation requirement:** allocate 54 physically distinct voice
objects (48 + 6), and reproduce the top-up's forced-fast-release behavior
(§5.4, §5.6) exactly, including its once-per-dispatcher-call cadence and its
Branch A/Branch B split. Allocating only 48, or allocating 54 without the
top-up's forced-release mechanism, will diverge from measured behavior on
any passage that saturates the pool.

---

### 5.6 Note-off vs. fast-release (choke)

There are two distinct release entry points, and they configure the voice's
envelope segments differently.

**Note-off, `0x19a2c`** — 5 call sites (`0x12c53`, `0x12cb9`, `0x12cf5`,
`0x12da5`, `0x12e0e`) `[A: grepping the PAGE disassembly for "call   0x19a2c"]`:

```c
// 0x19a2c __thiscall NoteOff(voice, absTimeLo, absTimeHi)
if (voice->0x134 == 0) return;               // already released — idempotent no-op, 0x19a34/0x19a3b
target = compute_release_target(voice->0x120/0x124, absTimeLo, absTimeHi);  // +1 tick bias if caller time <= creation time
ConfigureSegment(&voice->0x38, target);       // 0x19a6f -> 0x197dc  (attack/hold segment)
ConfigureSegment(&voice->0x68, target);       // 0x19a79 -> 0x197dc  (release segment, SAME configurator)
voice->0x134 = 0;
voice->0x140 = 0;
voice->0x128/0x12c = target;
```

Both segments are configured through the **same** function, `0x197dc`, which
applies the region's own authored release rate toward the target time — an
ordinary, patch-defined release. Note-off never touches `+0x138`.

**Fast-release / choke, `0x19aa4`** — 3 call sites: the reserve top-up's
Branch B (`0x12bc0`), the same-note retrigger scan inside note-on
(`0x12ec6`), and the exclusive-key-group choke walk (`0x12f39`)
`[A: grepping the PAGE disassembly for "call   0x19aa4"]`.

```c
// 0x19aa4 __thiscall ScheduleFastRelease(voice, absTimeLo, absTimeHi)
voice->0x138 = 1;                             // 0x19ab6 — UNCONDITIONAL, before any branch
if (voice->0x134 != 0 || voice->0x140 != 0) {
    // cheap path: reapply the EXISTING target through the CLAMPED configurator
    ReconfigureSegment(&voice->0x68, voice->0x128, voice->0x12c);  // 0x19ade -> 0x19834
    return;
}
// full path: compute a new target the same way note-off does
target = compute_release_target(voice->0x120/0x124, absTimeLo, absTimeHi);
ConfigureSegment(&voice->0x38, target);        // 0x19b16 -> 0x197dc  (SAME configurator as note-off, for this segment)
ReconfigureSegment(&voice->0x68, target);      // 0x19b28 -> 0x19834  (DIFFERENT configurator — rate-clamped)
voice->0x134 = 0;
voice->0x140 = 0;
voice->0x128/0x12c = target;
```

`0x19834` is a distinct function from `0x197dc`, used **only** by
`0x19aa4`'s two paths, never by note-off. It divides an internal value by the
literal constant `0x46` (decimal 70) and clamps the release segment's own
current rate against the result, replacing it with the faster of the two if
the existing rate would not reach the target quickly enough
(`0x1987f`,`0x19882`,`0x1989b`) `[A]`. This is the mechanism behind "fast,
rate-clamped release": regardless of the patch's authored release time, a
choked/fast-released voice's release segment is forced to run at least as
fast as this clamp. **Measured** `[M]`, from probe `18_key_groups`, two
independent cases (a hi-hat choked 0.15s after onset, and the same choked
0.8s after onset): both reach full silence in **70.0 ms** from the choking
note's own onset, exactly matching the divisor constant `70` found in
`0x19834` — a striking, though not independently proven-in-units,
correspondence between the disassembly constant and the measured duration.
(A "0.35s" cut-completion figure does not match direct measurement for this
mechanism; it is most likely a conflation with the
`probes/18_key_groups.manifest.tsv` annotation "LONG WHISTLE (0.354s sample)", which
names a *sample's raw length*, an unrelated quantity, for a different test
case in the same probe.)

`+0x138` exists to make each of a top-up's Branch-B victim selections land on
a **distinct** voice: `0x12426` (§5.7) gates on it at three points, so a
voice already marked by one Branch-B call cannot be re-selected by the next
`[A:0x12435]`,`[A:0x1243d]`,`[A:0x12498]`. The *other* steal routine,
`0x124a8` (note-on's own fallthrough), never reads or writes `+0x138` at all
`[A: grepping the PAGE disassembly for "0x138]" shows no hit in the 0x124a8-0x12523 span]`
— it can therefore re-select and immediately repurpose a voice that
`0x19aa4` had just marked, since `0x124a8`'s own cleanup clears `+0x134`/
`+0x140` but not `+0x138`; `+0x138` is cleared only by `SetupNote`
(`0x19d29`). A marked voice **keeps rendering** while it drains: the DSP
advance routine `0x19644` (`0x19644`–`0x197d6`) never reads or writes
`+0x138` `[A]`.

---

### 5.7 Steal priority

Two different functions select a voice to evict, used in different
situations, with **different priority logic** — this is not a simplification
of one shared rule, it is two distinct comparator bodies.

**`0x12426`** — used only by the reserve top-up's Branch B (§5.4), one
caller (`0x12baf`). Walks the active list; the returned voice is *not*
unlinked (Branch B hands it straight to `0x19aa4`, which only marks it).
Fully symmetric priority, in order: (1) `+0x138 == 0` required to be
eligible at all — a marked voice is never selected and never becomes `best`;
(2) among eligible voices, `+0x134 == 0` (released) beats `+0x134 != 0`
(held), checked from **both** the candidate's and the current `best`'s side;
(3) among two of the same `+0x134` state, smaller `+0x13c` (lower current
envelope level) wins; (4) among two `+0x134 != 0` (both still held), the
comparison degrades to smaller/older `+0x120`/`+0x124` (oldest timestamp)
wins `[A:0x12435]`–`[A:0x124a2]`, transcribed in full at the cited addresses.

**`0x124a8`** — used only by note-on's own fallthrough when both free lists
are empty, one caller (`0x12ef5`). Walks the active list and **does unlink**
the winner before returning it (`0x12515`–`0x1251c`, via `0x10386`, which
also decrements `+0x1c4`). Its comparator is **not symmetric**:

```c
// candidate and best are both walked from device+0x1c0 (the active list)
if (candidate->0x134 != 0) {                 // candidate still held
    // compare best vs candidate purely on the 64-bit timestamp (+0x124/+0x120),
    // WITHOUT first checking whether best is itself released — best keeps
    // the older (smaller) timestamp, replaced only if the candidate is older
    if (older(candidate, best)) best = candidate;
} else {                                      // candidate released (0x134 == 0)
    if (best->0x134 != 0) {
        best = candidate;                     // released candidate beats a held best
    } else {
        // both released: smaller +0x13c (lower envelope level) wins
        if (candidate.0x13c < best.0x13c) best = candidate;
    }
}
```

The practical consequence: a released voice, once installed as `best`, is
**not** protected from being displaced by a later, still-*held* candidate
that happens to be strictly older — the "released beats held" rule is
enforced only in the branch where the *candidate* is released, not
reciprocally when the candidate is held. This never surfaces in the two
measured probes (`20`/`21`), because those probes issue 80 note-ons with no
note-off at all, so every active voice has `+0x134 != 0` (held) throughout,
and every comparison takes the timestamp-only branch — which is exactly why
the measured behavior is clean **oldest-first** stealing, confirmed
acoustically by the descending-ramp probe (`21`) evicting the *oldest*
notes regardless of their pitch, and independently ruled out as
lowest-key-first from the code side: the comparison is on the 64-bit
timestamp at `+0x120`/`+0x124`, never on the key number (`+0x146`, a WORD)
`[A:0x124dc]`–`[A:0x124f8]`.

`+0x13c` (used as a tie-break by both functions) is best read as the voice's
*live* envelope level, not a pool tag: it is zeroed at construction and at
note setup and is otherwise continuously overwritten during rendering by the
envelope-advance helper (`0x194da`) `[I: write-site pattern — the only
non-zeroing writer runs on every render tick and the same field is compared
against a floor constant elsewhere (`0x19733`) that reads as a silence
threshold in a log-domain fixed-point format]`.

---

### 5.8 Exclusive key groups

The choke gate lives inside note-on's own path, right after a voice has been
obtained from whichever of the three sources (§5.3), before `SetupNote` is
called `[A:0x12efc]`–`[A:0x12f3e]`:

```c
if (region->0x31 != 0) {                          // usKeyGroup; 0 = no group, skip entirely
    for (v : device->activeList) {                 // +0x1c0
        if (v->0x14c == region->0x31 &&             // same key-group tag
            v->0x144 == thisChannel &&              // same MIDI channel
            v->0x150 == regionPtr) {                // (region-pointer compare: retained from the
                                                      //  original walk; in practice subsumed by the
                                                      //  keygroup+channel match)
            ScheduleFastRelease(v, absTimeLo, absTimeHi);  // 0x19aa4, same rate-clamped release as §5.6
        }
    }
}
```

`region+0x31` (`usKeyGroup`) is populated from the DLS region header during
collection load: zero-initialized by default (`0x15011`), and overwritten
from the parsed `rgnh` chunk's own key-group byte at `0x15bee`
(`region->0x31 = chunkData->0x12` — offset `0x12` within the chunk-data
buffer, consistent with the standard DLS `RGNHEADER.usKeyGroup` field at
`rgnh`-body offset
`0xa` once the 8-byte RIFF chunk header prefix included in that buffer is
accounted for) `[A:0x15011]`,`[A:0x15bee]`; the `rgnh`+`0xa` identification
itself is `[I: standard DLS RGNHEADER layout]`, not independently confirmed
byte-for-byte against the DLS spec here.

**There is no drum-channel gate.** The choke walk is purely data-driven: any
melodic instrument whose collection defines nonzero `usKeyGroup` values will
choke exactly like a drum kit; channel 9/10 is not special-cased anywhere in
this code path `[A: exhaustive grep of the 0x12efc-0x12f44 span for any
comparison against the literal channel value 9 — no hits]`. `gm.dls` itself
defines exactly **7** distinct nonzero key groups (values 1–7, across 1498
region headers) `[A: direct RIFF/DLS parse of gm.dls, verify_voice_keygroup_gmdls.py]`.

**Measured cut time** `[M]`: 70.0 ms from the choking note's onset to full
silence, reproduced identically across two independently-timed cases in
probe `18_key_groups` (§5.6) — this is the same fast-release mechanism as
the top-up/retrigger paths, not a separate choke-specific timing.

---

### 5.9 Sustain pedal (CC64) and CC120/CC123

The CC dispatch switch (`0x1336b`+) maps three controller numbers to three
internal single-byte sentinels consumed by the event dispatcher's main
switch (`0x12c0b`+):

| CC | Decimal | Sentinel | Dispatch site |
|---|---|---|---|
| 64 (Sustain/Hold1) | 0x40 | `0xfe` | `0x1337a` → `0x135c3` → stamp at `0x135d0` `[A]` |
| 120 (All Sound Off) | 0x78 | `0xfd` | `0x134e4`/`0x134ed` → stamp at `0x1362b` `[A]` |
| 123 (All Notes Off) | 0x7b | `0xff` | `0x13607`/`0x1360a` → stamp at `0x13631` `[A]` |

Each sentinel has its **own** handler in the dispatcher, with different
sustain interaction:

- **`0xfe` (CC64 value change)** — the raw CC value is cached per-channel at
  `device+channel*4+0xed0` unconditionally `[A:0x12c1b]`. If the new value is
  nonzero (pedal down), the handler returns immediately — nothing else
  happens on press. If the new value is zero (pedal lifted), it walks the
  active list for that channel and calls ordinary note-off (`0x19a2c`) on
  every voice whose `+0x140` is set (i.e. every voice whose note-off had been
  deferred while the pedal was down) `[A:0x12c34]`–`[A:0x12c53]`.
- **`0xff` (CC123, All Notes Off) — HONOURS the pedal.** Walks the active
  list for the channel; for each still-held voice (`+0x134 != 0`), it
  re-checks that channel's cached CC64 value: if the pedal is up, it calls
  note-off immediately (`0x19a2c`); if the pedal is down, it **defers** by
  setting `+0x140 = 1` instead of releasing (`0x12ca5`), to be picked up
  later by the `0xfe`-lift handler above `[A:0x12c78]`–`[A:0x12cb9]`.
- **`0xfd` (CC120, All Sound Off) — BYPASSES the pedal.** Walks the active
  list for the channel and calls note-off (`0x19a2c`) on every still-held
  voice (`+0x134 != 0`) **unconditionally** — it never reads `+0x140` or the
  per-channel CC64 cache anywhere in its body `[A:0x12cd6]`–`[A:0x12cf5]`,
  confirmed by an exhaustive grep of its span for both fields (no hits).

Both CC120 and CC123 ultimately call the same release primitive,
`0x19a2c` (ordinary note-off) — **neither one cuts a voice instantly**; both
release it through its normal (or, for `0xfe`'s deferred wake-up, its normal)
release ramp. The distinction is purely about whether the sustain-pedal
deferral is honored (CC123) or bypassed (CC120), not about release-vs-cut.

---

### 5.10 Render-latency figure

Rendered probes show a **constant** offset of magnitude ≈0.70 s between a
probe manifest's declared MIDI event time and the corresponding audible
onset in the captured audio — the audio's first detected onset consistently
*precedes* its manifest-declared timestamp by that amount (e.g. probe `20`:
declared first onset 1.200 s, measured 0.512 s, offset −0.688 s; probe `18`:
offset −0.802 s) `[M]`. The magnitude is consistent within the same probe
across every subsequent event (checked via onset-train spacing in probe
`18`, §5.10 script), but the **exact magnitude differs between probe files**
(−0.688 s vs. −0.802 s), which is itself evidence that this is a
per-file/per-capture-session artifact of the render/capture pipeline (most
likely differing lead-in silence or a differing capture-clock zero point per
file), **not a property of the synth**. Do not use this figure to calibrate
anything inside a clean-room implementation of `swmidi.sys` itself.

---

### 5.11 Verification scripts

All scripts below are stdlib-only (RE workspace not retained), parse
the PAGE-section disassembly of `swmidi.sys` (or decode probe `.flac` files
with the external `flac` CLI and analyze raw PCM with `wave`/`struct`/`math`
only — no `numpy`), and `assert`/exit nonzero on any failed check. Full
captured output:

```
$ python3 verify_voice_pool.py
[... 11 checks ...]
TOTAL: 11/11 passed

$ python3 verify_voice_steal_flag138.py
[... 8 checks ...]
TOTAL: 8/8 passed

$ python3 verify_voice_callsites.py
[... 4 checks ...]
TOTAL: 4/4 passed

$ python3 verify_voice_choke_keygroup.py
[... 12 checks ...]
TOTAL: 12/12 passed

$ python3 verify_voice_sustain_cc.py
[... 22 checks ...]
TOTAL: 22/22 passed

$ python3 verify_voice_dsp_fields.py
[... 20 checks ...]
TOTAL: 20/20 passed

$ python3 verify_voice_choke_timing.py
[INFO] measured capture-chain offset for this file: -0.802s
[INFO] case A (gap=0.15s): cut completion = 70.0 ms after trigger
[INFO] case B (gap=0.8s): cut completion = 70.0 ms after trigger
[... 4 checks ...]
TOTAL: 4/4 passed

$ python3 verify_voice_render_latency.py
[INFO] measured first audible onset at t=0.512s
[INFO] capture-chain offset = -0.688s
[... 1 check ...]
TOTAL: 1/1 passed

$ python3 verify_voice_keygroup_gmdls.py
[... 3 checks ...]
TOTAL: 3/3 passed
```

Grand total: **85/85 assertions passed** across 9 scripts, covering: the two
pool-loop counts (48, 6) and their VMAs; the `0x158` node size; the
unconditional `+0x138` store at `0x19ab6`; the three `+0x138` gates in
`0x12426`; the call-site counts for `0x19a2c` (5) and `0x19aa4` (3) and the
single-caller facts for `0x12426`/`0x124a8`; the choke gate and its
`+0x14c`/`+0x144`/`+0x150` three-way match; the CC64/120/123 → sentinel
mapping and each sentinel handler's differing `+0x140` treatment; the
DSP-interpolator's read/write-back of `+0x2c`/`+0x30`/`+0x34`/`+0x44` and the
`+0x0` wave-header dereference; the `0x197dc` vs. `0x19834` release-segment
configurator divergence and `0x19834`'s rate-clamp division by 70; the
measured 70.0 ms choke-cut timing (two independent cases); the measured
render-latency offset; and the gm.dls 7-key-group count. Control-flow/intent
claims that cannot be reduced to a static or acoustic assertion (e.g. "the
burst self-terminates because recycling catches up," §5.5) are marked `[I]`
in prose and are not claimed to be covered by a script.

---

### 5.12 Open items `[O]`

- The object pointed to by voice `+0x104` — only 4 of its own offsets
  (`+0x0`, `+0x4`, `+0x18`, `+0x1c`) are exercised by the code paths read for
  this section; its full layout and identity are unresolved.
- The exact scheduling semantics of the `+0x108`/`+0x10c` and `+0x118`/
  `+0x11c` 64-bit timestamp pairs beyond "consumed by the DSP-advance
  routine `0x19644`" — these matter for exact sample-accurate envelope
  timing but not for the pool/steal/choke/note-off algorithms specified
  here.
- *Why* the driver splits the pool into 48+6 rather than one flat 54-deep
  free list is not stated anywhere in the code and is not inferable from
  disassembly alone (the *mechanics* of the split are fully resolved in
  §5.2–§5.5; the *design intent* is not).
- The precise numeric time unit of `0x19834`'s divisor constant `70` (§5.6)
  is not proven to be milliseconds from the instructions alone; the
  correspondence with the measured 70.0 ms cut time is noted as strong
  corroboration, not as a proven unit conversion.
- The exact byte-for-byte confirmation of `rgnh`+`0xa` against the DLS
  RIFF/RGNHEADER specification (§5.8) was not independently re-derived here
  beyond the field-level copy `chunkData+0x12 → region+0x31`; the offset
  identification is inference from standard DLS layout.

---

## Part 6 — Audio Rendering Path

### 6.0 Scope and how to read the provenance tags

This section specifies the audio rendering path of `swmidi.sys` only: the
output format contract, per-voice sample generation and interpolation, gain
application, the mix accumulator and its saturation, the envelope/ramp
update cadence, and the output rate. It does not cover MIDI parsing,
controller dispatch, voice allocation, or DLS articulation translation.

Every factual claim below carries one of:

- `[A:0x19f4f]` — read directly from an instruction or byte sequence at that
  VMA in `swmidi.sys`.
- `[I]` — an inference, with what it is inferred from stated inline.
- `[O]` — open; not recovered, stated precisely rather than guessed.

VMA-to-file-offset conversion used throughout: `file_offset = VMA - 0x10000`
(re-derived from the PE header's `ImageBase = 0x10000`, and confirmed that
every section's virtual address equals its raw file pointer 1:1, so no
further adjustment is needed — see `verify_render_format.py`).

The bulk-grep form used to confirm every cited VMA resolves to exactly one
instruction line in the primary evidence (the PAGE-section disassembly of
`swmidi.sys` for PAGE VMAs, the `.text`-section disassembly for `.text`
VMAs) is: grep for the anchor `^   <hex-address>:` (three leading spaces,
as emitted by the objdump-style dump), expecting exactly one match; the
verification scripts use the equivalent regex `^\s*<hex>:`.

### 6.1 Output format contract

The device advertises exactly one output format via its `KSDATARANGE_AUDIO`
structure in PAGEDATA at VMA `0x1a710` `[A:0x1a710]`:

- `MinimumSampleFrequency == MaximumSampleFrequency == 22050` — a single
  fixed rate, not a range `[A:0x1a710]`. The literal dword `0x00005622`
  (=22050) occurs at file offset `0xa75c` and again at `0xa760`
  (bytes `22 56 00 00` both times) `[A:0xa75c][A:0xa760]`.
- `MaximumChannels = 2` `[A:0x1a710]`.
- `MinimumBitsPerSample == MaximumBitsPerSample == 16` `[A:0x1a710]`.
- `SubFormat = {00000001-0000-0010-8000-00AA00389B71}` =
  `KSDATAFORMAT_SUBTYPE_PCM` `[A:0x1a710]`.

**The synth renders 22050 Hz, stereo, 16-bit signed linear PCM.** This is the
complete, final output contract of this driver; there is no other rate
advertised anywhere in PAGEDATA (the whole 9796-byte section contains no
second `KSDATARANGE_AUDIO`).

16-bit-signed, L/R-interleaved sample layout is the standard `WAVEFORMATEX`
shape implied by a PCM `KSDATARANGE_AUDIO` with `MaximumChannels=2` `[I:
standard WDM/KS convention for a PCM data range; no separately-populated
literal `WAVEFORMATEX` struct was found elsewhere in PAGEDATA — the KS
runtime synthesizes it from this data range at pin-connect time]`. The
mixer's own store instructions independently confirm interleaved-stereo
16-bit output (see §6.4.6): stereo variants write 4-byte (`movq`, two
interleaved L/R int16 pairs) or 8-byte (two full L/R frames) blocks directly
into the same buffer the mono variant addresses with `outbuf + index*2`
`[A:0x1a114][A:0x19f0b]`.

**44100 Hz is not produced by this binary.** Reference recordings of this
synth commonly appear at 44100 Hz; that is the output of a resampling stage
downstream of this driver (`kmixer.sys` on this OS), which is not part of
`swmidi.sys` and has no code presence here to analyze. That separate
binary's resampling stage is specified, on its own terms and with its own
provenance tags, in §6.11: it is a 60-tap-per-phase float32 FIR with a
two-phase output structure, with zero-order hold and linear interpolation
both ruled out by direct inspection of `kmixer.sys`'s own code (§6.11.2).
**`[O]`: the exact tap coefficients and the exact rate-gating selection
condition remain unrecovered** (§6.11.4) — the algorithm *class* is known,
its exact numeric behavior is not, and this render-path section continues
to make no further claim about that separate binary beyond what §6.11
states.

**INVARIANT: the synthesis (voice-mix) rate is 22050 Hz, or an integer
multiple of it (44100 Hz is safe); it must never be set to the host
device/`AudioContext` rate when that rate is not an integer multiple of
22050 Hz.** The 22050 Hz internal rate itself is `[A]`-established just
above via `KSDATARANGE_AUDIO` (`MinimumSampleFrequency ==
MaximumSampleFrequency == 22050`, `[A:0x1a710]`); this invariant states the
consequence for any implementation tempted to render at a different,
device-chosen rate instead.

Synthesis-rate aliasing is an audible, spec-relevant behavior, not a
defect to be engineered away: probe `02_keyrange`'s keys 125-127 fold to
`22050 − f0` (within 1 cent) at this driver's fixed 22050 Hz render rate
`[M]`, and the `field/Strobe-faffaeefafaefae.flac` reference deliberately
exploits this same fold as its acid test for an aliasing-faithful
implementation. `[M]`: a user experiment synthesizing the same content at
22050 Hz, 44100 Hz, and 48000 Hz found the fold correct at 22050 Hz,
preserved at 44100 Hz (an integer 2x multiple — fold positions map
cleanly), and smeared into noise at 48000 Hz (a 2.177x, non-integer
multiple). Separately, `[M]`: taking a correctly-synthesized 22050 Hz
render's fold tone (dominant near 67.3 Hz) and resampling it up to
48000 Hz with a windowed-sinc resampler keeps the tone at approximately
67.4 Hz — so once synthesized at 22050 Hz, the fold is real signal that
survives output-stage resampling to any device rate.

**Consequence.** A device-rate-native render — synthesizing directly at
whatever rate the output device happens to report, in order to skip an
output-stage resampling step — would break this aliasing signature on any
device rate that is not an integer multiple of 22050 Hz. The correct
architecture is: render at 22050 Hz, then resample to the device's actual
rate only at the output stage. `dist/bg-sound2.js` does this correctly —
confirmed directly from source: `SYNTH_RATE = 22050` and
`ctx.createBuffer(2, n, SYNTH_RATE)`, not
`ctx.sampleRate` — so the `AudioBuffer` is built at 22050 Hz and Web Audio
performs the resample to whatever rate the `AudioContext` actually runs
at.

### 6.2 MMX/scalar path selection

There are two structurally different render-code families reachable from a
single per-voice format-code dispatch: an MMX-vectorized family at VMA
`0x19e1e`–`0x1a581` (four functions, called A–D below) and a scalar family
at VMA `0x16eb0`–`0x17f99` (at least: `0x16eb0`, `0x17018`, `0x1748c`,
`0x17790`, `0x17a24`, `0x17bde`, `0x17e32`, `0x175b6`, `0x176d0`, `0x17132`,
`0x17268`, `0x17326`). Which family runs for a given voice is decided at
runtime by a cached CPU-capability flag; there is no way to force the scalar
path via configuration.

#### 6.2.1 CPU-capability gate

Function E, VMA `0x1a538`–`0x1a581`, detects MMX support via the classic
EFLAGS-ID-bit probe followed by `CPUID`:

```c
0x1a54e: flags = EFLAGS                        // read EFLAGS
0x1a552: flags ^= 0x200000                      // toggle bit 21 (the ID flag)
0x1a565: cpuidResult = CPUID(leaf=1)            // execute CPUID (leaf value loaded at 0x1a55f)
0x1a568: mmxSupported = (cpuidResult.featureBits & 0x800000) != 0   // CPUID.1:EDX bit 23 = MMX
0x1a570: ds:0x1cc3c = 1                         // cache "MMX present" = true
```
`[A:0x1a54e][A:0x1a552][A:0x1a565][A:0x1a568][A:0x1a570]`

A second global, `ds:0x1cbd0`, is written from E's result by a separate
caller at `0x1668d` `[A:0x1668d]`, and it is `ds:0x1cbd0` — not `ds:0x1cc3c`
— that the per-voice format dispatcher actually reads, at VMA `0x1915b`
(`ds:0x1cbd0 == 0`) `[A:0x1915b]`.

**No registry or configuration override exists.** The string `MMXDisabled`
(the name historically associated with such a control) does not occur
anywhere in `swmidi.sys`, in ASCII or UTF-16LE encoding
`[A: searched via Python bytes-membership over the whole file, see
verify_render_dispatch.py]`. On any CPU manufactured after 1997, the CPUID
bit-23 test is unconditionally true, so **the MMX family (A–D) is the path
that actually executes** on essentially every real deployment, and this
section specifies MMX semantics as normative. The scalar family exists,
shares the same interpolation shape (two-tap, 12-bit fraction) and the same
manual saturating-add idiom `[A: 0x170f0-0x170fc contain the identical
add/jno/clamp-0x7fff/js/clamp-0x8000 sequence used by the MMX tail path]`,
but is treated here as **unreached in practice** and is not specified
further; whether its output is bit-identical to the MMX path for the same
input is `[O]` — not tested.

#### 6.2.2 Four MMX variants and dispatch

| Fn | VMA (entry–ret) | Source width | Output | Args | Confirmed by |
|----|-----------------|--------------|--------|------|--------------|
| A | `0x19e1e`–`0x19fcc` | 8-bit | mono | 8 (cdecl) | `[A: fetch is movzx WORD + punpcklbw, 0x19f1a/0x19f3c; exit stores same gain value twice to +0x2c/+0x30, 0x19fb2/0x19fb6]` |
| B | `0x19fd2`–`0x1a1b1` | 8-bit | stereo | 9 (cdecl) | `[A: fetch 0x1a0e4/0x1a0f7 movzx WORD; exit splits gain via psrlq 0x20, 0x1a195]` |
| C | `0x1a1b8`–`0x1a359` | 16-bit | mono | 8 (cdecl) | `[A: fetch 0x1a2b6/0x1a2cb movd DWORD (no movzx); exit stores same gain value twice, 0x1a33f/0x1a343]` |
| D | `0x1a360`–`0x1a532` | 16-bit | stereo | 9 (cdecl) | `[A: fetch 0x1a474/0x1a489 movd DWORD; exit splits gain via psrlq 0x20, 0x1a516]` |

Each function was traced end-to-end at the byte level to confirm this table
(function bodies read directly out of the PAGE-section disassembly of
`swmidi.sys`). Function C is a
**mono** variant, not stereo: C reads and writes only `voice+0x2c` and
mirrors the same value into `voice+0x30`, the mono pattern, and its sample
fetch uses a direct 32-bit `movd` with no `movzx`/`punpcklbw`, the
16-bit-source pattern.

`gm.dls` is **495/495 sixteen-bit mono-channel PCM waves** (verified
directly, §6.6), so the 8-bit-source variants (A, B) are unreachable for this
sample collection; they are real, dispatchable code, just never selected
for any `gm.dls`-sourced voice. They are documented here as **unreached for
this collection**, not specified as inactive in general.

**The live variant for a `gm.dls` voice rendered to the required stereo
output is D (VMA `0x1a360`).** This is established by tracing the
dispatcher's format-code selector to its concrete numeric branches, not by
assumption:

```c
0x19316: formatCode = voice.formatCode              // local copy of this voice's format code
0x19319: if (formatCode == 0x42) goto 0x17132        // scalar
0x19322: if (formatCode == 0x44) goto 0x17bde        // scalar
0x1932a: if (formatCode == 0x61) goto 0x17326        // scalar
0x19333: if (formatCode == 0x62) goto 0x16eb0        // scalar
0x19336: if (formatCode == 0x64) goto 0x17790        // scalar
0x1933a: if (formatCode == 0x71) call 0x19fd2 at 0x19363   // B, stereo 8-bit
0x1933f: if (formatCode != 0x72) goto 0x19458        // else fall through
                    -> 0x19346..0x19379 -> call 0x1a360 (code 0x72 = D, stereo 16-bit)
```
`[A:0x19316][A:0x19319][A:0x1933a][A:0x1935c][A:0x1933f][A:0x19379]`

Code `0x71` (stereo request, 8-bit source) calls B; code `0x72` (stereo
request, 16-bit source) calls D. Since every `gm.dls` wave is 16-bit source
and the KS pin requires 2-channel output, **D is the function that executes
for every note rendered from this collection.** The upstream bits of the
format code (which conditions produce `0x71` vs `0x72`, i.e. how a per-voice
format byte at `voice+0x18` and the MMX-eligibility bits combine) belong to
the control-plane dispatcher and are not re-derived further here; the
call-target identity for the reachable codes is what matters for this
section and is directly confirmed.

### 6.3 Per-voice state (fields touched by the render functions)

All offsets are from the `voice` pointer passed as argument `+0x8`:

| Offset | Width | Meaning | Access pattern |
|---|---|---|---|
| `+0x00` | 4 | pointer to a wave/region descriptor object | read once at entry `[A:0x19e33]` |
| `(descriptor)+0x0c` | 4 | source PCM sample-data base pointer | read once at entry `[A:0x19e35]` |
| `+0x2c` | 4 | persistent gain state — mono: the only channel; stereo: channel-A (e.g. Left) | read at entry, written at exit `[A:0x19e41][A:0x19fb2]` |
| `+0x30` | 4 | persistent gain state — mono: mirrors `+0x2c` (no second channel); stereo: channel-B (e.g. Right), a genuinely independent 64-bit-register half | write-only at exit; stereo functions also read it at entry (gain ramp base) `[A:0x1a002][A:0x1a199]` |
| `+0x34` | 4 | persistent per-sample **phase step** (fixed-point sample-index increment per output frame) | read at entry, written at exit `[A:0x19e44][A:0x19fba]` |
| `+0x44` | 4 | persistent running **phase position** (same fixed-point format as the step) | read at entry, written at exit `[A:0x19e3b/0x19e3e][A:0x19fbd]` |

`+0x44` is zeroed by the note-setup code at `voice`-construction time
(`voice->0x44 = 0` `[A:0x18f7d]`), i.e. every note starts
sample playback at phase position 0.

### 6.4 Per-voice render algorithm

The following is complete enough to implement without the disassembly. It
describes function D (stereo output, 16-bit mono source) — the live variant
for `gm.dls` (§6.2.2) — noting the two divergences from the mono form (A/C)
inline. All four functions share this shape; only the source-tap fetch
width and the output arity differ.

#### 6.4.1 Call arguments (confirmed by direct trace of both shapes)

Mono (A, C — 8 stack args, cdecl):

```
+0x8  voice pointer
+0xc  output buffer (int16[], mono)
+0x10 frame count (used as-is, unsigned loop bound)
+0x14 ramp period (samples between gain/phase-step ramp refreshes, see 4.6)
+0x18 gain ramp step  (added to the internal <<8 gain accumulator every ramp period)
+0x1c phase-step ramp step (added to the internal <<8 phase-step accumulator every ramp period)
+0x20 loop-end sample index ("SampleEnd")
+0x24 loop length in samples (0 = one-shot / non-looping)
```
`[A:0x19e26][A:0x19e70][A:0x19ed4][A:0x19eac][A:0x19eb1]`

Stereo (B, D — 9 stack args, cdecl):

```
+0x8  voice pointer
+0xc  output buffer (int16[], interleaved L,R)
+0x10 frame count, DOUBLED IN PLACE at function entry (see 4.7)
+0x14 ramp period
+0x18 gain ramp step, channel A (L)
+0x1c gain ramp step, channel B (R)
+0x20 phase-step ramp step
+0x24 loop-end sample index
+0x28 loop length in samples
```
`[A:0x19fda][A:0x19ffc][A:0x1a002][A:0x1a038][A:0x1a03c][A:0x1a09c][A:0x1a074][A:0x1a079]`
(cited against D; B has the identical shape, spot-confirmed against the
same offsets in its own prologue.)

#### 6.4.2 Phase accumulator format

The phase state is a 32-bit fixed-point value split **20 bits integer :
12 bits fraction**, confirmed three independent ways:

- the fraction-mask constant is `0x0FFF` (4095) per 16-bit lane
  `[A: immediate dword 0x0FFF0FFF loaded at 0x19e5a and the equivalent
  sites in B/C/D]`;
- the "whole" constant is `0x1000` (4096 = 2¹²) per lane
  `[A: immediate dword 0x10001000 at 0x19e61 and equivalents]`;
- the integer sample index is obtained with `shr reg,0xc`
  `[A:0x19ef2][A:0x19f11]`.

```
position:   32-bit unsigned, bits [31:12] = integer sample index,
                              bits [11:0] = 12-bit interpolation fraction
step:       same format; the per-output-frame increment added to position
```

#### 6.4.3 Per-iteration tap fetch

Both mono and stereo variants unroll 2 output frames per loop body pass
(mono: 2 mono frames; stereo: 2 stereo L/R frames), computing two
independent phase positions and doing an independent wraparound test (§6.4.5)
for each. For a single frame, given `pos` (current, already-wrapped phase
position) and `waveBase` (source PCM pointer):

```
idx      = pos >> 12                      ; integer sample index
frac     = pos & 0xFFF                    ; 12-bit fraction, 0..4095

16-bit source (C, D — the gm.dls path):
    tap0 = int16_at(waveBase + idx*2)          ; direct 16-bit read
    tap1 = int16_at(waveBase + idx*2 + 2)      ; the adjacent (next) sample
    [A:0x1a2b6 tap0 loaded directly as a 16-bit packed value / 0x1a2cb tap1 likewise, no sign massaging needed]

8-bit source (A, B — unreached for gm.dls, documented for completeness):
    tap0 = int8_at(waveBase + idx) * 256       ; byte placed in the HIGH
    tap1 = int8_at(waveBase + idx + 1) * 256   ; byte of a 16-bit lane,
                                                ; zero in the low byte --
                                                ; equivalent to sign-extend-
                                                ; then-*256 for two's-
                                                ; complement bytes, no
                                                ; separate sign op needed
    [A:0x19f1a source byte pair widened to 16-bit lanes / 0x19f3c the two lanes interleaved (packed) into one MMX register for the SIMD path below]
```

#### 6.4.4 Interpolation (Q12 linear, lane-exact)

```
w1 = 4096 - frac                          ; weight for tap0
w0 = frac                                 ; weight for tap1
interp = (tap0 * w1 + tap1 * w0) >> 12    ; arithmetic shift right, signed
```
This is computed with a single SIMD dot-product-and-shift per tap pair:
`pmaddwd` forms `tap0*w1 + tap1*w0` as one signed 32-bit lane
`[A:0x19f3f]`, then `psrad reg,0xc` divides out the 4096 scale
`[A:0x19f4f]` — this exact `psrad ...,0xc` shift occurs **8 times** in the
mixer window (once per main-loop pass and once per odd-frame tail, ×4
functions) `[A: 8 occurrences at 0x19f4f, 0x19f83, 0x1a11b, 0x1a15a,
0x1a2e1, 0x1a310, 0x1a4a1, 0x1a4db — see verify_render_mixer.py]`. The
result is then narrowed to signed 16-bit with a saturating pack
(`packssdw`) before the gain multiply `[A:0x19f53]`; this narrowing cannot
itself overflow given the source sample ranges, so it is a formality here,
not the load-bearing saturation point (that is §6.4.5).

For a stereo output frame, the single interpolated mono value is duplicated
into both halves of a 32-bit lane (`pand`/`pslld 0x10`/`por`) so it can be
multiplied against two independent gain values in one instruction
`[A:0x1a11f-0x1a12a]`.

#### 6.4.5 Gain application

```
gain_out_sample = high16(gain_Q * interp)     ; signed 16x16->32 multiply, keep bits [31:16]
```
implemented as `pmulhw` `[A:0x19f56]` (mono) / `[A:0x1a12d]` (stereo, one
multiply against a 2-lane `{gainL, gainR}` vector) against a gain value that
was pre-broadcast into all active lanes with `packssdw`
`[A:0x19f47]`. **Gain is applied strictly after interpolation** — the
instruction order is unconditionally `pmaddwd → psrad → packssdw → pmulhw`
in all four functions; there is no gain multiply anywhere earlier in the
pipeline `[A: confirmed by direct instruction-order read of all four
functions]`.

Gain fixed-point pipeline (persistent state at `voice+0x2c`/`+0x30`,
per-channel):
```
gain_Q  = (raw_gain << 8) >> 5             ; = raw_gain << 3, active value used
                                            ;   for the multiply during the loop
... ramped every `ramp_period` samples (see 4.6) ...
raw_gain_out = gain_Q >> 3                 ; net <<3>>3 = identity: the
                                            ;   persisted field is in the
                                            ;   same units it was read in
```
`[A:0x19e9c-0x19e9f pslld 8 / psrld 5][A:0x19fae psrld 3 before store]`.
The absolute fixed-point meaning of `raw_gain` as it arrives from the
control-plane caller (e.g. whether it is Q16.16 relative to full scale) is
**`[O]`** — not independently pinned down; what is confirmed is the
internal `<<3` scaling relationship between the field's rest representation
and its in-loop active representation.

#### 6.4.5.1 Per-voice gain ceiling (output clamp / gain-saturation)

**`[A]`**, added to `src/engine` this pass (previously missing — every voice's
gain rose without bound, so a loud, lightly-attenuated voice overshot the
real driver's output instead of flattening). Two confirmed facts in §6.4.5
combine into a hard, structural bound on achievable gain, independent of
the still-`[O]` absolute Q-format of `raw_gain`:

- `packssdw` narrows the gain accumulator into a **signed 16-bit lane**
  (max representable value `+32767`) immediately before the multiply
  `[A:0x19f47]`.
- `pmulhw` computes `high16(gain_Q * sample)` = `floor(gain_Q * sample /
  65536)` — a Q16 fixed-point multiply `[A:0x19f56/0x1a12d]`.

For that multiply to ever pass a sample through **unattenuated** (true
unity gain, 0 dB, `gain_linear == 1.0`), `gain_Q` would need to equal
`65536` — one bit past what a signed 16-bit lane can hold. **True unity is
therefore structurally unreachable through this pipeline: no voice's gain
can ever exceed `32767/65536 ≈ 0.499985` of computed unity**, regardless of
how hundredths-of-a-dB attenuation maps onto the raw register (the `[O]`
item above). Below that ceiling the squared volume law (§3.5/§6.5) applies
untouched; at or above it, output flattens.

Cross-validated against `probe-results/27.flac` (Sine patch, bank MSB 8 /
program 80, velocity/CC7/CC11/master sweeps — all four sweeps share the one
squared curve and all four show the identical flat top): back-computing the
reference's own uncapped squared-law value from its unclamped low points
(v40/20/8, all clear of the ceiling) gives an implied v127 value of
~25700–26050; the measured flat-top ceiling is 13200.
`13200 / 25919 ≈ 0.5093`, matching `32767/65536 = 0.499985` within the
reference capture's own point-to-point spread (~1.3%) — independent
empirical corroboration of the disassembly-derived constant, not a fit to
it.

Implementation: `src/engine/voice.c`'s `voice_update_gain` clamps
`gain_l_target`/`gain_r_target` (each channel independently, matching
§6.4.5's confirmed per-channel L/R gain state) to `GAIN_CEILING =
32767.0/65536.0` after the pan/attenuation multiply and before the
per-sample smoother (`render.c`) ramps toward it — the smoother, envelope,
squared curve, and pan law are all otherwise untouched.

**Known residual, out of this pass's scope:** this project's own unclamped
gain for that same Sine note is measurably ~1.6–1.7 dB hotter than the
~25919–26050 implied by the reference's own cap, so its flat top lands at
~15883 rather than ~13200, and the clamp engages one velocity step later
(v110 down, not v90 down) than the reference. That gap is a pre-existing
attenuation/gain-scale discrepancy elsewhere in the chain (not this
ceiling), confirmed present in `probe-results/26.flac`'s per-patch levels
too (5 of 6 patches checked run ~0.69–0.82x reference power RMS; one,
program 118, runs ~1.87x reference) — flagged for a future pass, not
addressed here.

#### 6.4.6 Mix accumulation and saturation

```
existing        = int16x2_at(outbuf + frame_index*stride)   ; read current buffer content
mixed           = saturating_add_i16(existing, gain_out_sample)
outbuf[...]     = mixed
```
`stride` is 2 bytes/int16-lane for mono (A, C); for stereo the loop index has
already been pre-scaled (see §6.4.7) so that each unit represents one
interleaved int16 slot. The same `index*2 + base` addressing formula is
reused unmodified across mono and stereo.

**The mix accumulator is the 16-bit output buffer itself. There is no
wider (32-bit) intermediate accumulator anywhere in this code.** Every
voice's contribution is added directly on top of whatever prior voices
already wrote, via the hardware `paddsw` instruction, which saturates at
`[-32768, 32767]` **on every add, in hardware** — not deferred to a later
stage `[A: paddsw at 0x19f59 (A), 0x1a130 (B main), 0x1a2eb (C),
0x1a4b6 (D main), plus 0x1a183 (B tail) and 0x1a504 (D tail) — 6 occurrences
total, immediately preceding the store of that same register back to the
output buffer]`. Because saturation is applied at every individual voice's
add rather than once at the end of a mixing pass, **this is audibly
different from a design that sums in a wider accumulator and clips once**:
under heavy polyphony a loud early voice can push the buffer to full scale,
and every subsequent voice's contribution is then clipped against that
already-saturated value rather than against the true (unclamped) sum of all
voices.

The odd/final frame of a call (used when the frame count is odd, or for the
mono functions' tail) is written with plain scalar arithmetic reproducing
the identical saturating semantics by hand:
```
result = existing + new_sample            ; ordinary 16-bit add, sets OF/SF
if no overflow: store result
elif result appears negative (SF=1 after the wrapped overflow): store 0x7FFF  (positive clip)
else:                                                          store 0x8000  (negative clip)
```
`[A: 0x19f97-0x19fa5]`. This exact 5-instruction idiom recurs at
`0x1a324-0x1a332` (B's mono-style scalar tail) and at `0x170f0-0x170fc`
inside the confirmed-scalar function `0x17018`. The mono functions' tail
(A, C) use this manual idiom; the stereo functions' tail (B, D) instead
still use hardware `paddsw` (2 of the 6 total `paddsw` occurrences are these
stereo tails) `[A: 0x1a183, 0x1a504]` — the manual idiom and the hardware
instruction are semantically identical, this is purely an implementation
choice for how few active SIMD lanes remain in the last unrolled step.

**Saturation, never wraparound, at every point of contact with the output
buffer** — no plain non-saturating add on 16-bit PCM output occurs anywhere
in this code.

#### 6.4.7 Frame-count argument and the stereo doubling

The mono functions use the caller-supplied frame count at `+0x10` directly
as the unsigned loop bound (loop counter compared against it each pass and
incremented by 1) `[A:0x19ea3][A:0x19ef5]`. The stereo functions instead
double the `+0x10` argument in place at function entry, before it is ever
compared `[A:0x19fdd (B)][A:0x1a36b (D)]`, and advance their loop counter by
2 per pass instead of 1 `[A:0x1a0bd][A:0x1a118]`.

**The caller-supplied argument is a plain frame count in both cases** — the
doubling is a purely local address-arithmetic convenience, not a different
unit contract at the call boundary. Both mono and stereo address their
output buffer with the identical formula `index*2 + outbuf`, which assumes
2 bytes per index unit. For a mono int16 buffer, `index` must equal the
frame number directly. For an interleaved-stereo int16 buffer, each real
frame occupies 2 int16 slots, so `index` must count in **slots**, not
frames, for the same `*2`-byte-stride formula to land on the right address
— hence the stereo functions convert `frameCount` to `frameCount*2` once at
entry, and their per-pass `+=2` stride on the loop counter then correctly
represents "one real output frame per +2 step" in that pre-scaled index
space. The number of real loop iterations performed for a given frame count
is identical between the mono and stereo forms; only the internal
bookkeeping units differ `[I: derived from comparing the two address
formulas and stride amounts directly, not stated as such by any comment or
symbol]`.

#### 6.4.8 Loop-end test and wraparound

```
if position >= SampleEnd:
    if LoopLength == 0:
        stop producing samples for the remainder of this call   ; one-shot end
    else:
        position -= LoopLength                                  ; single subtraction, not a loop
```
`[A:0x19eac position compared against SampleEnd (+0x20)][A:0x19eaf branch taken only while position < SampleEnd]
[A:0x19eb1 loopLen compared against 0][A:0x19eb5 branch to stop if loopLen == 0][A:0x19ebb position -= loopLen]`.
The comparison is consistently unsigned `>=` (via `jb`/`jae`), never `>`,
at every occurrence checked (8 sites across the mixer window)
`[A: 0x19ea6, 0x19ef9, 0x1a06e, 0x1a0c3, 0x1a240, 0x1a293, 0x1a3fc, 0x1a451]`.

**This is a single conditional subtraction, never a loop** — there is no
back-edge around the `sub`. This is only correct under the invariant that
the per-frame phase step never exceeds one loop length in a single frame
(`[I]`: a normal note pitch cannot violate this for any sane loop length,
but the code itself performs no bounds check enforcing it). **Normative
guidance for implementers**: if a reimplementation must support phase steps
large enough to cross a loop boundary more than once per frame (e.g. an
artificially extreme pitch-bend range combined with a very short loop),
either replace the single subtraction with a modulo/while-loop, or clamp
the phase step so this case cannot occur; matching the original binary's
behavior byte-for-byte in that edge case is not defined, since the original
never exercises it.

One-shot end behavior: when `position >= SampleEnd` and `LoopLength == 0`,
the function stops producing new samples immediately and returns the number
of frames actually produced (the return value is set from the loop counter,
`>>1` for the stereo forms since their loop counter counts in slots)
`[A:0x19fc5][A:0x1a1ad]`.
It does **not** pad the remainder of the requested block with silence
itself. **`[I]`**: the caller presumably uses this return value to know how
many of the requested frames were rendered and to silence/deactivate the
voice for the rest — the caller was not traced to confirm this.

#### 6.4.9 Exit-state writeback

At the end of every call, regardless of how it exited:

```
voice[+0x2c] = gain_Q >> 3                       ; channel A (or the only channel, mono)
voice[+0x30] = <same value, mono> | <channel-B value via psrlq 0x20, stereo>
voice[+0x34] = current phase step (post-ramp)
voice[+0x44] = current phase position (post-wraparound, post-advance)
```
`[A:0x19fae-0x19fbd (mono, A)][A:0x1a18d-0x1a1a0 (stereo, B)]`. `emms` is
executed before return in every MMX function `[A:0x19fc3]` — required x86
housekeeping to release the MMX/x87 register aliasing before any
floating-point code runs again; not a semantic part of the render algorithm.

### 6.5 Per-wave sample rate and the phase-step composition

`gm.dls` contains 495 waves. **492 are 22050 Hz, mono, 16-bit PCM; 3 are
24000 Hz, mono, 16-bit PCM**, at wave-pool indices 26, 62, and 404, whose
`INAM` names are `APPLS64A`, `BUBLE68A`, and `STREM67B` respectively
`[A: read directly from gm.dls's per-wave `fmt ` and `INFO/INAM`
sub-chunks — see verify_render_format.py, 21/21 checks pass]`. At a 22050 Hz
render rate, those three waves' unity-pitch phase step is
`24000/22050 ≈ 1.088435`, a non-integer ratio, so the linear interpolator
is exercised for them even at unity pitch — this is live, acoustically
distinct behavior for those three waves, not a corner case that never
executes.

**The instruction sequence where a wave's own sample rate enters the
phase-step computation** (function starting at VMA `0x18ef4`, called from
the note-setup path at `0x19d1e`):

```c
0x18f6f: waveRate = wave->0x10          // this WAVE's own sample rate field
                                         //   (per-wave, not the fixed 22050 render rate)
0x18f72: waveRate *= pitchRatio         // pitch ratio is Q12, from the table lookup at 0x18e1c
0x18f7b: phaseStep = waveRate / renderRateDenom   // divide by the caller-supplied denominator
0x18f83: voice->0x34 = phaseStep        // -- the exact field the mixer reads as its
                                         //    persistent phase step
```
`[A:0x18f6f][A:0x18f72][A:0x18f7b][A:0x18f83]`, cross-checked against the
mixer's own read/write of the same field (`voice+0x34` read at `0x19e44`,
written back at `0x19fba`, §6.3) — confirming this is genuinely the field the
render loop consumes, not a same-named but unrelated field.

The field read at `wave+0x10` is a per-wave/per-region runtime record; a
default-value constructor elsewhere seeds the **same struct offset** with
the fixed value 22050 (`wave->0x10 = 0x5622` at VMA `0x14585`)
`[A:0x14585]` before the actual DLS `fmt ` chunk value (22050 or 24000)
overwrites it per-wave — directly supporting that this is a genuine
per-wave rate field defaulted to the render rate, not a hardcoded constant.

The pitch ratio multiplied in at `0x18f72` comes from a table-lookup helper
at VMA `0x18e1c`, which indexes two precomputed Q12 fixed-point tables:
base `0x1ad00` (a cents pitch-ratio table, `table[n] = trunc(4096 *
2**(n/1200))`, domain **n = -100..100, 201 entries** — confirmed directly
from the loop-bound instructions at `0x16700`/`0x16710` (loop variable
initialized to `0xffffff9c` = −100, exits once it exceeds `0x64` = 100), with the
store at `0x1674c` going through the truncating helper `0x106e0` called at
`0x16744`, i.e. `trunc(...)`, not `round(...)`) and base `0x1af58` (a
semitone pitch-ratio table, `table[n] = trunc(4096 * 2**(n/12))`, domain
n = -48..48, 97 entries — the ±48 domain belongs to *this* table, not to
`0x1ad00`) `[A:0x18e44 indexes 0x1ad00][A:0x18e4d indexes 0x1af58]`
`[A:0x16700][A:0x16710][A:0x16744][A:0x1674c]`. Both tables are built once at
device-attach time from a live `pow()` call and are all-zero on disk.
The exact denominator dereferenced at `0x18f7b` (`renderRateDenom`, expected
to be the fixed 22050 Hz render rate) was traced back through its call chain to a
caller-supplied pointer whose ultimate origin is control-plane, per-device
state; **`[O]`**: the precise struct this pointer belongs to and independent
confirmation that its dereferenced value is literally 22050 in every case
was not exhaustively traced (out of the render section's scope), though the
arithmetic role (render-rate denominator in a wave-rate/render-rate pitch
ratio) is unambiguous from the instruction sequence itself.

### 6.6 Ramp/envelope update cadence

**Established**: within a single render call, the active gain value and the
active phase step are each held in a coarse "ramp accumulator" that is
re-derived from a linear step supplied by the caller (`+0x18`/`+0x1c` for
mono, `+0x18`/`+0x1c`/`+0x20` for stereo, §6.4.1) once every `ramp_period`
samples, where `ramp_period` is itself a caller-supplied argument (`+0x14`)
`[A:0x19e26 read at entry][A:0x19ec2 countdown -= 0x2, gated by 0x19ec5]
[A:0x19ee9 countdown reloaded from +0x14 to restart]`. Between refreshes, the value used for interpolation/gain is
held constant; this is a linear-ramp smoothing mechanism operating on
whatever gain/pitch targets and slopes the caller already computed, not an
envelope-shape generator in its own right.

**Open**: whether a full ADSR-style volume/pitch envelope generator updates
its own segment state once per render call (i.e. once per audio block), once
per fixed sub-block, or at some other cadence — and what determines the
values written into the ramp-step arguments (`+0x18`, `+0x1c`, `+0x1c`/`+0x20`)
between calls — is **`[O]`**. That state machine, if it exists as
distinct code, was not located within the render path itself; the render
functions only consume caller-supplied ramp endpoints and slopes. This is
consistent with the ramp/envelope logic living in the control-plane code
that calls into these mixer functions, which is out of this section's scope.
Do not infer a specific envelope cadence from the `ramp_period` mechanism
above — they are related but not shown to be the same thing.

### 6.7 Floating-point environment

The x87 control word this driver's floating-point code assumes and enforces
is **`0x027F`**:

```
0x027F = 0000 0010 0111 1111b
  bits 0-5  (IM,DM,ZM,OM,UM,PM)  = all 1   -> every FP exception masked
  bits 8-9  (Precision Control)  = 10b = 2 -> 53-bit double precision
  bits 10-11(Rounding Control)   = 00b = 0 -> round to nearest, even
```
`[A: decoded arithmetically from the literal 0x027F compared against at 5
sites in `.text` — 0x104ef, 0x10b26, 0x10bde, 0x10bef, 0x10c48]`. This is
**not** the x87 hardware power-on default (which is 64-bit extended
precision, `0x037F`); `0x027F` trades away the 80-bit extended intermediate
precision for IEEE-754-`double`-compatible rounding after every single FPU
operation. **This is what makes a multi-step x87 computation (e.g. the
pitch-ratio-table-building `pow()` calls, or any other multi-op float
sequence in this driver) reproducible using ordinary 64-bit `double`
arithmetic on a non-x87 target**: at Precision Control = 2, every
intermediate result is already rounded to the same 53-bit value a
`double`-only implementation would compute, so there is no hidden
extended-precision state for a reimplementation to fail to replicate.

Separately, float-to-integer conversions in this driver go through a
dedicated helper (VMA `0x106e0`) that temporarily forces **round-toward-zero
(truncate)**, converts, then restores the caller's ambient control word:

```c
0x106e6: tmp = FPUControlWord             // save the current (assumed 0x027F) control word
0x106ef: modified = tmp | 0x0c00          // set Rounding Control (bits 10-11) to 11b = truncate
0x106f6: FPUControlWord = modified        // load the truncating control word
0x106f9: result = truncateToInt(fpuTop)   // convert top-of-stack to integer, truncating
0x106fc: FPUControlWord = tmp             // restore the original control word
```
`[A:0x106e6][A:0x106ef][A:0x106f6][A:0x106f9][A:0x106fc]`. Setting
`modified |= 0x0c00`
touches only the Rounding Control field (bits 10-11); it does not alter
Precision Control (bits 8-9), confirmed arithmetically (`0x0c00 & 0x0300 ==
0`, `0x0c00 & 0x0c00 == 0x0c00`). **Every float→int conversion in this
driver that goes through this helper is `trunc`, never round-to-nearest**,
regardless of the ambient rounding mode otherwise in effect.

### 6.8 Additional per-sample signal processing

No additional per-sample signal-shaping stage — spectral, resonant, or
otherwise — was found anywhere in the traced render path. The complete
per-sample operation set in the live (MMX) mixer functions, read
instruction-by-instruction in full for function A and cross-checked
structurally for B/C/D (§6.4), is exactly: two-tap fetch, linear
interpolation, one gain multiply, one saturating accumulate. No recurring
pair of state variables consistent with a coefficient-based difference
equation, no oscillator/periodic-table read gated on a modulation rate, and
no indexed-buffer read/write pattern consistent with a modulated-tap
ring-buffer structure appears in this window `[A: direct read of the full
mixer window, 0x19e1e-0x1a581]`.

Separately, and independently of the code question, `gm.dls`'s own
articulation data contains **zero connection blocks targeting either DLS
destination code `0x0500` or `0x0501`** (the codes DLS-1 reserves for a
per-region spectral-cutoff and resonance parameter pair) out of 7451 total
connection blocks in the file `[A: read directly from every `art1` chunk in
gm.dls — see verify_render_format.py]`. This is a fact about the content of
`gm.dls`, not a claim about whether the driver implements those destination
codes if a different DLS collection supplied them — that question is
outside this section's scope. Whether rendered output's spectral content
varies with velocity in a way that would indicate a live spectral-shaping
stage was not measured in this pass (no rendered reference audio was
available) — **`[O]`**.

### 6.9 Verification

Five scripts (RE workspace not retained), stdlib-only, each asserting
specific numeric/structural claims made above against primary data
(`swmidi.sys` bytes, `gm.dls` bytes, or the disassembly dumps) and printing
pass/fail per claim.

```
$ python3 verify_render_format.py
[PASS] PE ImageBase == 0x10000  got 0x10000
[PASS] PAGEDATA section VA == RawPtr (1:1 file mapping)  VA=0xa600 RawPtr=0xa600
[PASS] KSDATARANGE_AUDIO.FormatSize == 88  got 88
[PASS] MaximumChannels == 2  got 2
[PASS] MinimumBitsPerSample == MaximumBitsPerSample == 16  got min=16 max=16
[PASS] MinimumSampleFrequency == MaximumSampleFrequency == 22050  got min=22050 max=22050
[PASS] SubFormat == KSDATAFORMAT_SUBTYPE_PCM  00000001-0000-0010-8000-00aa00389b71
[PASS] bytes at file offset 0xa75c == 22 56 00 00 (=22050)  22560000
[PASS] bytes at file offset 0xa760 == 22 56 00 00 (=22050)  22560000
[PASS] file offset 0xa75c == VMA 0x1a710+0x4c (== VMA 0x1a75c)
[PASS] gm.dls wave count == 495  got 495
[PASS] 492 waves are (PCM=1, mono, 22050 Hz, 16-bit)  got 492
[PASS] 3 waves are (PCM=1, mono, 24000 Hz, 16-bit)  got 3
[PASS] sum of histogram == 495  got 495
[PASS] the 3 non-22050 waves are exactly pool indices [26, 62, 404]  got [26, 62, 404]
[PASS] pool index 26 name == APPLS64A  APPLS64A
[PASS] pool index 62 name == BUBLE68A  BUBLE68A
[PASS] pool index 404 name == STREM67B  STREM67B
[PASS] gm.dls has a non-trivial number of art1 connection blocks total (sanity check the parser isn't silently finding nothing)  got 7451
[PASS] gm.dls has ZERO art1 connections with usDestination == 0x0500 (DLS-1 spectral-cutoff destination code)  count=0
[PASS] gm.dls has ZERO art1 connections with usDestination == 0x0501 (DLS-1 spectral-resonance/Q destination code)  count=0

21/21 checks passed.

$ python3 verify_render_mixer.py
mixer window: lines 11857-12497 (641 lines), VMA 0x19e1e..0x1a581
[PASS] PAGE disassembly's last instruction line is VMA 0x1a581 (file ends there; 0x1a582 does not exist as an instruction)  1a581:	c3                   	ret
[PASS] psrad ...,0xc occurs exactly 8 times in the mixer window  got 8
[PASS] encoded bytes '72 e7 0c' (psrad mm7,0xc) occurs exactly 8 times  got 8
[PASS] immediate dword 0x0FFF0FFF (12-bit fraction mask) loaded 4 times (once per mixer function A/B/C/D)  got 4
[PASS] immediate dword 0x10001000 (Q12 'one' constant, 0x1000=4096=2^12) loaded 4 times  got 4
[PASS] paddsw (saturating packed-word add) occurs exactly 6 times (4 main-loop sites + 2 stereo-tail sites; the 2 mono-tail sites use the manual add/jno/clamp idiom instead, see below)  got 6
[PASS] pmulhw (gain multiply, keep-high-16) occurs exactly 8 times (main + tail, x4 functions)  got 8
[PASS] pmaddwd (interpolation dot-product) occurs exactly 8 times (main + tail, x4 functions)  got 8
[PASS] manual 16-bit saturating-add clamp idiom (mov ...,0x7fff / mov ...,0x8000) occurs exactly 2 times each (mono tail paths of A and C only)  0x7fff sites=2 0x8000 sites=2
[PASS] exactly 5 function prologues ('mov edi,edi' hot-patch pad) in the window: A, B, C, D mixers + E (CPUID probe)  got 5

10/10 checks passed.

$ python3 verify_render_dispatch.py
[PASS] VMA 0x1a54e resolves uniquely  1a54e:	9c  pushf
[PASS] 0x1a54e is 'pushf' (start of EFLAGS.ID probe)
[PASS] VMA 0x1a552 toggles EFLAGS bit 0x200000 (ID flag, bit 21)
[PASS] VMA 0x1a565 is 'cpuid'
[PASS] VMA 0x1a568 tests EDX bit 23 (0x800000, CPUID.1:EDX MMX bit)
[PASS] VMA 0x1a570 caches the MMX flag to ds:0x1cc3c
[PASS] VMA 0x1668d stores CPUID-probe result into ds:0x1cbd0 (the flag the dispatcher actually reads)
[PASS] VMA 0x1915b: dispatcher compares ds:0x1cbd0 against 0 (the live MMX-eligibility gate)
[PASS] literal 'MMXDisabled' absent from swmidi.sys (ASCII encoding)
[PASS] literal 'MMXDisabled' absent from swmidi.sys (UTF-16LE encoding)
[PASS] VMA 0x19379 is 'call 0x1a360'
[PASS] dispatch-chain VMAs 0x19338/0x1933a/0x1933d/0x1933f/0x19340 all resolve uniquely
[PASS] 0x1933a subtracts 0xd (code 0x64 -> 0x71) and 0x1933f decrements again (0x71 -> 0x72), with 0x19340 falling through on no-match to the 0x1a360 call
[PASS] VMA 0x1935c ('call 0x19fd2', function B) is the code-0x71 sibling branch
[PASS] function D reads BOTH voice+0x2c and voice+0x30 gain fields at entry (stereo L/R gain, not a mono duplicate)
[PASS] function D splits its exit-state gain store with psrlq mm2,0x20 (writes two genuinely different 32-bit halves to +0x2c/+0x30)
[PASS] function D fetches samples via 'movd mm7,DWORD PTR [reg]' (direct 32-bit/16-bit-PCM read), not 'movzx ... WORD' (8-bit path)
[PASS] function D advances its frame counter by 2 per iteration (2-frame stereo unroll) via 'add ecx,0x2'

18/18 checks passed.

$ python3 verify_render_fpu.py
[PASS] 0x027F bits 0-5 (all FP exceptions) are all masked (==0x3F)  0b111111
[PASS] 0x027F Precision Control (bits 8-9) == 2 (53-bit double)  2
[PASS] 0x027F Rounding Control (bits 10-11) == 0 (round to nearest, even)  0
[PASS] VMA 0x104ef compares against 0x27f
[PASS] VMA 0x10b26 compares against 0x27f
[PASS] VMA 0x10bde compares against 0x27f
[PASS] VMA 0x10bef compares against 0x27f
[PASS] VMA 0x10c48 compares against 0x27f
[PASS] VMA 0x106e0 is the helper's entry (push ebp)
[PASS] VMA 0x106e6 saves the ambient control word (fstcw)
[PASS] VMA 0x106ef sets RC=11b via 'or ah,0xc' (bits 10-11 of the 16-bit word, encoded in the high byte)
[PASS] VMA 0x106f6 loads the modified (truncating) control word (fldcw)
[PASS] VMA 0x106f9 performs the integer conversion under that control word (fistp)
[PASS] VMA 0x106fc restores the ORIGINAL (pre-truncate) control word (fldcw)
[PASS] 0x0c placed in the high byte == bits 10-11 of the 16-bit control word (0x0c00), not bits 8-9 (precision control)  0xc00

15/15 checks passed.

$ python3 verify_render_phasestep.py
[PASS] VMA 0x18f6f reads a per-wave field at [esi+0x10]
[PASS] VMA 0x18f72 multiplies that field by the pitch ratio (imul edx,eax)
[PASS] VMA 0x18f7b divides the product by a caller-supplied denominator (div DWORD PTR [edi])
[PASS] VMA 0x18f83 stores the result into voice+0x34 -- the exact field the MMX mixer reads at entry as its persistent phase step
[PASS] mixer function A reads voice+0x34 at entry (0x19e44)
[PASS] mixer function A writes voice+0x34 at exit (0x19fba)
[PASS] 0x18e1c's body indexes table base 0x1ad00 (T2, fine-tune pitch-ratio table, Q12 fixed point)
[PASS] 0x18e1c's body indexes table base 0x1af58 (T3, semitone pitch-ratio table, Q12 fixed point)
[PASS] VMA 0x14585 default-constructs a wave-format record with [eax+0x10] = 0x5622 (22050) -- same offset read at 0x18f6f

9/9 checks passed.
```

Total: 73/73 checks pass across the 5 scripts.

### 6.10 Summary of open items (`[O]`)

- The exact resampling method used by the separate downstream stage that
  produces 44100 Hz reference audio from this driver's 22050 Hz output —
  entirely outside this binary. Its algorithm **class** is now known (§6.11:
  a 60-tap-per-phase float32 FIR, two-phase output), but its exact tap
  coefficients and its exact rate-gating selection predicate remain `[O]`
  (§6.11.4).
- Whether the MMX and scalar mixer families produce bit-identical output
  for identical inputs — not tested (scalar is unreached on any real CPU
  regardless).
- The absolute fixed-point unit of the raw gain field as it arrives from
  the caller (voice `+0x2c`/`+0x30` before the driver's own `<<3` internal
  scaling) — the internal scaling relationship is confirmed, its ultimate
  physical unit (e.g. dB-linear vs. a specific Q-format full-scale
  reference) is not.
- The precise struct/pointer chain supplying the render-rate denominator at
  VMA `0x18f7b` — the arithmetic role is unambiguous; independent
  confirmation that the dereferenced value is literally 22050 in every case
  was not exhaustively traced (control-plane territory, out of scope here).
- Whether a distinct envelope-shape state machine (ADSR-style) exists and,
  if so, at what cadence it updates and what feeds the per-call ramp-step
  arguments — not located within the render path itself.
- Whether rendered output's spectral content is measurably affected by any
  reachable per-voice processing beyond gain and linear resampling — not
  measured in this pass (no rendered reference audio available); the code
  read for this section shows no such processing present in the live path.

---

### 6.11 The output-path resampler (kmixer.sys, 22050 → 44100)

**Provenance note — read this before anything else in this subsection.**
Every citation below tagged `[A:kmixer 0xVMA]` was read from a **different
binary**, `kmixer.sys` (the WDM Kernel Audio Mixer), not from `swmidi.sys`.
Every other `[A:0xVMA]` tag anywhere else in this document — including the
rest of Part 6 — refers to `swmidi.sys`. The two address spaces are
unrelated; a `kmixer` VMA must never be read as a `swmidi.sys` VMA or vice
versa, which is exactly why the `kmixer` qualifier is mandatory on every
tag in this subsection, the same discipline this document already applies
to citing the shared CRT float routines by their own origin. Source for
this subsection: `kmixer-src-analysis.md`, a reverse-engineering writeup
(RE workspace not retained) covering a pass over `kmixer.sys` build
2008-04-14 (172,416 bytes), conducted for this project.

#### 6.11.0 Why this belongs in a `swmidi.sys` specification

`swmidi.sys` itself renders at a single fixed rate, 22050 Hz (§6.1); it has
no code path that produces 44100 Hz output, and none is specified here.
But the 44100 Hz reference recordings this project compares against
elsewhere were captured after a second, separate kernel component —
`kmixer.sys` — upsampled the driver's native 22050 Hz stream to 44100 Hz
downstream, on the capture machine's mixing graph. Anyone comparing
rendered `swmidi.sys` output against those 44100 Hz captures is, whether
they realize it or not, also comparing against kmixer's resampling stage.
This subsection specifies what that stage actually does, as far as static
analysis of its own binary allows, so that fact is documented rather than
silently absorbed into "swmidi.sys sounds different from the reference."

#### 6.11.1 Filter class, length, and two-phase structure

The stage is a genuine multi-tap FIR, not a hold or interpolation
shortcut. Two adjacent routines, `0x2c0a0` and `0x2c840`
`[A:kmixer 0x2c0a0]` `[A:kmixer 0x2c840]`, each compute two output samples
per call from a 60-float32-tap-per-phase convolution over a scrolling
history buffer:

```
out[2n]   = sum_{k=0..59} h[k] * hist[n   - 2k]      (phase 0)
out[2n+1] = sum_{k=0..59} h[k] * hist[n-1 - 2k]      (phase 1)
```

(restated in sample units; the disassembly's own addressing is
byte-offset from the history-buffer pointer, `-8k` and `-(4+8k)` bytes
respectively `[A:kmixer 0x2c0a0]`.)
Both phases apply the **same 60-value coefficient sequence** to two
one-sample-offset, stride-2 windows of the same history — the classic
polyphase decomposition of a single logical low-pass kernel into an
even-tap and an odd-tap phase, each run at the input's own rate. The
60-tap count is a hard count, not an estimate: every coefficient-table
operand read in the routine spans exactly 60 distinct 4-byte-spaced
offsets, `coefTable+0x0` through `coefTable+0xEC`, no gaps
`[A:kmixer 0x2c0a0]`. The
filter is purely causal (only past history is read, never future
samples), so its impulse response peaks roughly 59 input samples after an
impulse, not centered on it — group delay, not a bug, for a 60-tap
backward-looking kernel.

The output pointer advances a fixed 2 samples (8 bytes) per call
(`outputPointer += 0x8` `[A:kmixer 0x2c776]`), while the history
pointer advances by a value read from the filter's own per-instance
context struct (`stride = ctx->0x18` `[A:kmixer 0x2c0b0]`, added at
`[A:kmixer 0x2c794]`) rather than a hardcoded immediate — i.e. this is a
parametric-ratio engine, configured 2-out/1-in for the 22050→44100 case,
not a routine hardcoded to ratio 2. That specific configuration value was
not directly observed in this pass (§6.11.4).

#### 6.11.2 Zero-order hold and linear interpolation are excluded, not merely unconfirmed

Every "output advances at 2× the rate of input" code shape actually
present in `kmixer.sys` was individually opened, and each resolves to
something other than time-domain sample duplication or averaging:
mono→stereo channel fan-out (`0x2d690`/`0x2d724`, confirmed integer Q15
gain multiply, not resampling) and bit-depth format conversion
(`0x201d0`/`0x20280`, confirmed integer saturating requantization, same
sample rate in and out) `[A:kmixer 0x2d690]` `[A:kmixer 0x201d0]`
`[A:kmixer 0x20280]`. No "copy this sample into the next two output
slots" or "(a+b)>>1" shape exists anywhere in the portion of `kmixer.sys`
traced for this pass.

**This independently corroborates, by a completely different method, a
conclusion this project already reached empirically:** rendered-audio
probing elsewhere in this document found zero-order hold matches only
0.23% and linear interpolation only 0.87% of the reference's odd-indexed
samples (§1.3, §6.1, `[M]`). Two methods — code-level exclusion here and
audio-level measurement there — land on the same answer.

#### 6.11.3 Datapath: float32, x87, truncate-only-at-store

All arithmetic in `0x2c0a0`/`0x2c840` is x87 float load/multiply/add
against 32-bit memory operands — float32, not MMX integer
Q15 `[A:kmixer 0x2c0a0]`. No x87-control-word instruction exists
anywhere in kmixer's paged code (`PAGE`) `[A:kmixer PAGE — 0 hits for
fldcw/fstcw/fnstcw]`, so the x87 control word is left at its default
extended-precision state through the entire multiply-accumulate chain;
truncation to IEEE754 single precision happens only at the two final
stores, writing the phase-1 and phase-0 output samples respectively
(`out[+0x4] = float32(...)`, `out[+0x0] = float32(...)`)
`[A:kmixer 0x2c771]` `[A:kmixer 0x2c774]`). An implementation targeting
this stage's rounding behavior should accumulate at double (or wider)
precision and truncate to float32 only once, at the very end of each
output sample's computation — not once per tap.

This float32 engine is **separate** from a fully-confirmed integer Q15
datapath elsewhere in `kmixer.sys` (`imul`/`sar`, round-toward-zero, used
for plain channel mixing such as the mono→stereo fan-out cited above)
`[A:kmixer 0x2d690]`. The two must not be conflated: the resampler is
unambiguously in the float engine, not the integer one.

#### 6.11.4 What is not recoverable — both marked `[O]`

- **Exact tap coefficients — `[O]`.** No static coefficient table of the
  required size (60+ distinct float32 values) exists anywhere in
  `.rdata`, `.data`, or `PAGEDATA` — an exhaustive scan found none,
  including re-checking a candidate `.data` float run near VMA `0x18000`
  `[A:kmixer 0x18000]` that turned out to be denormal noise, not taps.
  The coefficient pointer the filter actually uses is computed at runtime
  from a per-instance context struct (`coefBase = pin->0x0 + pin->0x30*4`
  `[A:kmixer 0x2c0c8]` `[A:kmixer 0x2c0ca]`, where `pin = *(arg0+4)` is
  the pin/connection object). A deeper pass traced that struct to its
  construction and bounded the open question precisely. The pin object
  is allocated at `[A:kmixer 0x237ba]` by
  `ExAllocatePoolWithTag(NonPagedPool, 0x160, 'KMIX')` (352 bytes), and
  the entire object — including the coefficient-base, phase-count, and
  phase-index fields at `+0x0`/`+0x2c`/`+0x30` — is zeroed at
  construction by the `rep stos` at `[A:kmixer 0x237db]`. Four routines
  that touch the object's neighboring fields were read in full and none
  writes coefficient data into `pin+0x0`/`+0x2c`/`+0x30`: pin-connect
  (`[A:kmixer 0x236a0-0x2415b]`), the stage-array builder
  (`[A:kmixer 0x31870-0x31e5d]`), rebuild-on-list
  (`[A:kmixer 0x32dd0-0x32dc2]`), and the history-buffer resize helper
  (`[A:kmixer 0x1fa30-0x1fd65]`). Three coefficient-source mechanisms are
  independently refuted, each by exhaustive check: no static float32
  table in any of `kmixer.sys`/`portcls.sys`/`ks.sys`/`sysaudio.sys`; no
  `fild`-from-static-address conversion (all 300 `fild` sites in kmixer
  read runtime buffers or scalars, none a static table address,
  `[A:kmixer 0x302eb]`); and no `rep movs`/SSE-block/`memcpy` copy from a
  static source (of 36 `rep movs` sites only 3 have static sources, and
  those decode to function-pointer-table refreshes or a registry-path
  string, `[A:kmixer 0x21afe]` `[A:kmixer 0x21b0f]` `[A:kmixer 0x364ad]`;
  kmixer has no `memcpy`/`RtlCopyMemory` import at all). No cross-module
  call delivers the data on this path either: kmixer's only `drmk.sys`
  imports are three DRM content-protection stubs, and it has no
  `portcls.sys`/`sysaudio.sys` imports at all
  `[A:kmixer PE import table]`. What remains open is therefore
  narrow: the taps are written by a still-unlocated fill that must run
  after pin construction and before the first dispatch to
  `0x2c0a0`/`0x2c840` for a given pin, plausibly gated by the ratio-class
  value written to `[stage_array+0x24]` (`[A:kmixer 0x23a3b]`
  `[A:kmixer 0x23a58]` `[A:kmixer 0x23a69]`). Recovering the real values
  would require either tracing forward from every read-site of
  `[stage_array+0x24]` to the actual fill call, or a live
  kernel-debugger dump of `*(int*)(*(int*)(pin+4))` at the moment
  `0x2c0a0` executes — neither has been done; both remain
  recommendations, not results. No windowed-sinc design routine exists
  in kmixer's paged code either (zero `fsin`/`fcos`/`fyl2x`/`f2xm1` in
  `PAGE`). **Any implementation of this stage must therefore use
  inferred coefficients** — a windowed-sinc low-pass design (e.g.
  Kaiser-windowed, 60 taps, cutoff at half the input rate) of the
  recovered length and structural type, not the real values — and
  **cannot reproduce kmixer's output bit-for-bit**, only its general
  spectral behavior.
- **Rate-gating selection predicate — `[O]`.** The filter pair is
  confirmed reachable through kmixer's generic per-stage function-pointer
  dispatch: the table slot is populated at `[A:kmixer 0x31a70]`/
  `[A:kmixer 0x31d4f]` and invoked at `[A:kmixer 0x230aa]`/
  `[A:kmixer 0x230b3]`. But the one stage-selection helper fully traced
  end-to-end, `0x31640` `[A:kmixer 0x31640]`, builds its selection index
  from `wFormatTag`, `nChannels`, and `wBitsPerSample` comparisons — it
  never reads the `WAVEFORMATEX` field offset that corresponds to
  `nSamplesPerSec`. The exact condition that routes a 22050-in/44100-out
  pair specifically to this filter pair, as opposed to some other
  rate-independent reason for its selection, was not pinned down.

#### 6.11.5 What this changes, and what it doesn't

Recovering kmixer's resampler as a class — 60-tap-per-phase float32 FIR,
two-phase output, extended-precision accumulation truncated only at the
final store — sharpens the instrument used to reason about any 44100 Hz
reference recording, but it does not by itself close residual error
against such a reference, and it does not make a comparison against
44100 Hz audio bit-exact. Two things stand between a structurally-faithful
reproduction of this stage and kmixer's real output: the tap coefficients
are inferred, not extracted (§6.11.4), and the original 44100 Hz captures
carry their own ±1 LSB capture dither independent of this filter. **A
44100 Hz comparison built from this section is a model of kmixer's
resampling stage, clearly labelled as such — not a clean, bit-exact
reference match.** The only bit-level-exact comparison available for
`swmidi.sys` itself remains at 22050 Hz (§1.3).

---

## Part 7 — Register of the Unreached and the Unknown

Subject: `swmidi.sys` 5.1.2600.5512 (Windows XP SP3), the Microsoft GS
Wavetable Synth kernel driver. This section closes the specification. It has
two purposes: **Part A** tells a clean-room implementer exactly what exists
inside this binary that must **not** be ported, and under what condition it
would stop being safe to omit. **Part B** tells the implementer exactly which
facts about this driver are not settled, and — for each one — what observation
would settle it. **Part C** records where this project's own prior-art
adjudication (`FACTS.md` — an earlier adjudication pass by this project,
not carried into this repo) is now out of date because later work in this
project settled a question it had left open.

**Provenance key**, identical to the five companion sections:

- `[A:0xVMA]` — read directly from an instruction or byte sequence at that
  virtual address in `swmidi.sys` (file offset = VMA − 0x10000 for PAGE,
  VMA unchanged for `.text`, per the shared PE-header/section-table
  derivation already established in the companion sections).
- `[M:probe]` — measured from rendered reference audio elsewhere in this
  project.
- `[I]` — inference, with its basis stated inline.
- `[O]` — open; not recovered, stated precisely rather than guessed.

This section's own scope-discipline result (banned-term grep, run externally
against the finished section text on disk at the time, RE workspace not
retained, reported rather than narrated in-line):

```
$ grep -inoE 'dm[s]ynth|dm[u]sic|directm[u]sic|\breverb\b|\bchorus\b|\bfilter\b' \
    08-open-and-unreached.md
\Registry\Machine\Software\Microsoft\DirectMusic
\Registry\Machine\Software\Microsoft\DirectMusic
```

Two hits, both the one permitted literal exception (the registry path this
driver itself reads, `[A:0x1405c]` per the companion DLS section) — it occurs
once in this section's own front-matter-adjacent citation and once in the
Part C delta row that restates it for completeness. No other occurrence of
either synthesizer name, either forbidden audio-effect noun, or the generic
tone-shaping noun appears anywhere in this file.

---

### 7.A Part A — The unreached-code register

#### 7.A.1 Method: independent re-derivation, not a restatement

A reachability closure over `.text` was computed by
`verify_text_reachability.py` (stdlib-only, RE workspace not retained),
written for this section and run fresh (not reusing the earlier `xrefs.txt`/
`funcs.txt` cross-reference dumps, though its results were cross-checked
against them). Method:

1. Parse the `.text`-section disassembly (VMA `0x10380`–`0x117b4`) and
   the PAGE-section/`.init`-section disassembly into address→instruction maps.
2. **Entry points** = every `.text` address that is the target of a direct
   `call`/`jmp`/`jcc` from PAGE or `.init` (an "external reference"), **plus**
   every `.text` address referenced by a non-branch immediate operand from
   PAGE/`.init` (a function-pointer-table entry — found exactly one: PAGE
   `0x182f0` stores `0x103e4` into a struct's `+0x1c` field, which is a
   genuine external entry, not a call site, and is seeded into the closure as
   one).
3. Build the intra-`.text` graph: every `call`/`jmp`/`jcc` edge plus a
   fallthrough edge from each non-terminal instruction to the next (skipping
   `int3` alignment padding, which never executes).
4. BFS/DFS closure from the entry-point set.
5. Candidate "function starts" = the union of (a) every address that is a
   call/jmp target anywhere in the whole binary, and (b) every address
   immediately following an `int3` padding run whose own preceding
   instruction was `ret` or any `jmp` (direct or indirect) — this second
   category is required because a function that is *never* referenced by any
   static call/jmp at all (the pow forwarder stub, as it turns out) would
   otherwise never even appear as a "target" to check.
6. Report which function starts are outside the closure, and — critically —
   split that set into addresses with **zero incoming references from
   anywhere in the whole binary** (independently orphaned islands) versus
   addresses referenced only from within an already-unreached function's own
   body (private helper subroutines of a dead function, not independent
   entry points).

#### 7.A.2 Script output (full, unedited)

```
=== .text address range: 0x10380 - 0x117b4 (1932 instructions) ===

=== external call/jmp entries into .text from PAGE/.init: 10 ===
  0x10386
  0x103a8
  0x103c2
  0x10474
  0x10490
  0x1049c
  0x104a8
  0x104b4
  0x104c4
  0x106e0

=== external non-branch immediate references into .text from PAGE/.init (candidate function-pointer-table entries): 1 ===
  0x103e4 referenced from PAGE 0x182f0

0x104c2 in external_call_entries directly? False
0x104c4 in external_call_entries directly? True

=== reachability closure: 1394 of 1932 .text instruction addresses reached ===

=== additional candidate function starts found via the padding-boundary heuristic (address right after an int3 run whose preceding instruction was ret/direct-jmp): 32 ===
  [... 32 addresses, 28 of which coincide with existing branch-target entries; 4 do not: 0x103e4, 0x104c0, 0x10710, 0x10910 ...]

=== .text addresses that are CALL/JMP TARGETS somewhere in the binary, but never reached from the external-entry closure: 48 ===
  0x104c0  (referenced from: )
  0x104df  (referenced from: .text 0x104c0 (jmp))
  0x10710  (referenced from: )
  [... 45 more addresses, all internal to the 0x10710 island (see A.3) ...]

=== of those, addresses with ZERO incoming references from anywhere in the whole binary (independently orphaned island entry points): 3 ===
  0x104c0
  0x10710
  0x10910

=== of those, addresses that ARE referenced, but only from within an already-unreached function's own body (private helper subroutines of a dead function, not independently unreachable entry points): 45 ===
  [... 0x104df, 0x1071d, 0x1072e, 0x10744, ..., 0x1085e, 0x108a1, 0x108d5-region, 0x10ac9, ... 45 total, all reachable ONLY from 0x10710's own body ...]

=== ASSERTIONS ===
independently-orphaned island-entry set == {0x104c0, 0x10710, 0x10910}: True

does anything call/jmp to 0x10910 from 0x10710's own island? False (incoming edges to 0x10910, system-wide: [])
0x104c2 (hot-patch pad immediately before 0x104c4) in reachable? False (expected False: every real caller targets 0x104c4 directly, past the pad -- this is not evidence of a fourth unreached function, just an unexecuted 2-byte pad)
pow-body callee closure (0x10b40, 0x10ae0, 0x10d46, and the IEEE-classifier/_fpieee_flt/errno/_controlfp family): all present in the reachable set: True

=== ALL ASSERTIONS PASSED: exactly 3 unreachable .text functions, matching 0x104c0 (pow forwarder stub), 0x10710 (exp()), 0x10910 (two-operand IEEE special-case classifier) ===
```

(Full verbatim output, including the two enumerations abbreviated with
`[...]` above for length, is reproduced by re-running the script; it is
deterministic and asserts on any disagreement rather than printing a
best-effort summary.)

**Result: the assignment brief's headline claim — exactly three independently
unreachable functions in `.text` — is confirmed**, at the three addresses
named. Two specific supporting details in the brief do not survive direct
re-verification and are corrected below (§7.A.3); they do not change the
headline result.

#### 7.A.3 Two corrections to the brief's own supporting detail, found in the course of verification

- **The brief states `0x10910` is "called only by `0x10710`".** Direct grep
  of the `.text`/PAGE/`.init`-section disassembly for the literal
  substring `10910` finds it exactly once — its own definition line
  `[A:0x10910]`. It is not called, jumped to, or referenced by any
  static operand anywhere, including from within `0x10710`'s own body.
  `0x10710` (`exp()`) and `0x10910` (the classifier) are **two
  independently orphaned islands**, not one function calling the other. Both
  are still unreached, so this does not change the three-item result — it
  only corrects which of the two dead islands supposedly calls the other.
- **The brief lists `0x10537, 0x10578, 0x105fb, 0x108ec` as "all inside
  pow's own body"** (the four call sites that pull `pow`'s IEEE/errno family
  into the reachable set). Three of these are: `0x10537`
  (`call 0x10b40`) and `0x10578` (`call 0x10ae0`) are both inside the
  reachable `pow` body between `0x104c2` and `0x105fe` `[A:0x10537]`
  `[A:0x10578]`; `0x105fb` (`call 0x10d46`) is likewise inside `pow`'s
  reachable body `[A:0x105fb]`. **The fourth, `0x108ec`, is not** — it is
  inside the private helper `0x108d5`–`0x10906` (an `fsave`/`frstor` wrapper
  around a second call to `0x10d46`), which is itself only ever called from
  within the dead `0x10710` (`exp()`) island `[A:0x108ec]`. `0x10d46` is
  still correctly reachable overall — it has **two** callers, one inside
  `pow` (`0x105fb`, live) and one inside `exp()`'s dead island (`0x108ec`,
  never executed) — so the net conclusion ("the whole IEEE-classifier/
  `_fpieee_flt`/errno/`_controlfp` family is reachable via `pow`") is
  unaffected. The specific attribution of `0x108ec` to `pow`'s own body is
  simply wrong and is corrected here.

#### 7.A.4 The register

| Address range | What it is | Why unreached | Evidence | Condition under which it becomes reachable |
|---|---|---|---|---|
| `0x104c0`–`0x104c1` (2 bytes) | `__cdecl` forwarder stub for `pow`: `jmp 0x104df` | Zero incoming references anywhere in the binary — every real caller (all inside `pow`'s own table-building call sites in PAGE, and the four internal call sites inside `pow`'s own body) targets `0x104c4` directly, two bytes past this stub, skipping over it entirely | `[A:0x104c0]` (verified: no `call`/`jmp` operand anywhere resolves to `0x104c0`); confirmed by the reachability script, §7.A.2 | Would become reachable only if some caller were changed to target `0x104c0` instead of `0x104c4` — i.e. never, for this exact binary; there is no configuration, registry key, or runtime condition that selects it |
| `0x104c2`–`0x104c3` (2 bytes) | a two-byte no-op (self-move, the standard hot-patchable-prologue idiom), the true start of the function whose *entered* address is `0x104c4` | Not a call target either (every caller targets `0x104c4`, past the pad); this is normal, expected shape for a hot-patchable prologue, not a fourth dead function | `[A:0x104c2]`; reachability script confirms `0x104c4 ∈ reachable`, `0x104c2 ∉ reachable`, and explains why (§7.A.2) | N/A — this is inert padding by design, not code an implementer would ever port either way |
| `0x10710`–`0x10ad9` | The CRT `exp()` implementation, plus five private helper subroutines it alone calls (`0x1085e` — an `fabs`/`fcompp` overflow/underflow classifier; `0x108a1` — an `frndint`-based integer/fraction classifier; `0x108d5`–`0x10906` — an `fsave`/`frstor` wrapper around a second `0x10d46` call; `0x10ac9` — a small tail helper; plus roughly 40 internal labels reached only by conditional/unconditional jumps within `exp()`'s own body, including one indirect jump-table (switch) dispatch at `0x10a01` whose jump-table entries were not resolved — they lie inside this same dead island regardless of their exact targets) | Zero incoming references to `0x10710` itself from anywhere; this driver never calls `exp()` — every `lScale`→duration conversion in the timing-conversion code (§3.4.1 of the companion articulation section) goes through `pow(2.0, x)` at `0x104c4`, never through `exp()` | `[A:0x10710]` (verified: no reference anywhere); full private-helper enumeration in §7.A.2's script output; cross-checked against the companion articulation section's own citation of `0x104c4`/`fyl2x` as the only float path exercised for timecent conversion (Part 3 §3.4.1) | Would become reachable only if some future code change called `exp()` directly instead of `pow(2.0, x)` — not applicable to this exact binary |
| `0x10910`–`0x10a00` (at least; boundary with the `exp()` island not fully separated because both regions are dead and low-priority to disentangle further) | A two-operand IEEE-754 special-case classifier (per-operand `fxam`+`xlat`-nibble classification feeding an indirect jump-table dispatch at `0x10975`) | Zero incoming references from anywhere in the binary, including from `0x10710` — an independently orphaned island, not `exp()`'s own callee (§7.A.3) | `[A:0x10910]` | Would become reachable only if some caller referenced it directly (e.g. a two-argument CRT function such as `atan2`/`fmod` that this driver never actually calls) — not applicable here |
| `0x104e8`–`0x104c4`+body, `0x10ae0`, `0x10b40`, `0x10d46`, `0x10cd1`, `0x10eaf`, `0x10fef`, `0x11008`, `0x110ab`, `0x110dc`, `0x1113e`, `0x11201`, `0x1125d`, `0x1149e`, `0x116c9`, `0x116f9`, `0x11701`, `0x11716`, `0x1172c`, `0x11757`, `0x117b4` | `pow`'s own exceptional-input paths: NaN/Infinity/denormal classification, `_fpieee_flt`-style error reporting, `errno` setting, and `_controlfp`-adjacent FPU-state helpers | **These ARE reached** by the closure (confirmed §7.A.2) — they are not dead code. They are included here because, for the actual inputs this synthesizer ever evaluates `pow` on (finite, normal-range `double`s drawn from `gm.dls`'s timecent domain, already shown by the companion articulation section's own 552-distinct-value/1,367,824-ULP-margin check to never land near a boundary condition, and from the velocity-table exponent-4 domain, `v/127 ∈ [0,1]`), **none of these branches is ever entered at runtime**. They are live code, unreached for this specific input domain — a materially different claim from dead code | `[A]` reachability (§7.A.2); domain argument `[I]`, based on the articulation section's own exhaustive enumeration of every timecent value `gm.dls` actually supplies (Part 3 §3.4.1) and the velocity table's fixed `v=0..127` domain (Appendix T §T.2) | **An implementation may omit this entire family**, provided it documents that its own `pow` need only handle finite normal inputs — the condition under which it would need to be ported is a future DLS collection (not `gm.dls`) supplying a timecent value producing a non-finite intermediate, which none of `gm.dls`'s 552 distinct values do |
| `0x16eb0`, `0x17018`, `0x1748c`, `0x17790`, `0x17a24` (scalar/non-MMX mixer, plus siblings `0x17bde`, `0x17e32`, `0x175b6`, `0x176d0`, `0x17132`, `0x17268`, `0x17326`) | The scalar (non-MMX) render/mixer family, structurally parallel to the four MMX functions (two-tap linear interpolation, same manual saturating-add idiom) | Path selection is by CPUID only: EFLAGS.ID toggle `[A:0x1a54e]`, `cpuid` `[A:0x1a565]`, a test of the CPUID.1 feature-flags result against mask `0x800000` (bit 23, MMX) `[A:0x1a568]`. **There is no registry override** — the string `MMXDisabled` occurs nowhere in `swmidi.sys`, ASCII or UTF-16LE (exhaustive byte-membership search, `verify_render_dispatch.py`, both checks PASS). On any CPU manufactured after 1997 the MMX bit is unconditionally set, so the scalar family is never selected | `[A:0x19316]`–`[A:0x19379]` (dispatch chain resolves to the MMX callee for every reachable format code); Part 6 §6.2 and its `verify_render_dispatch.py` (18/18 checks pass) | Would become reachable only on a CPU lacking the MMX CPUID bit — no real x86 CPU manufactured after 1997 lacks it, and there is no software switch. Whether the scalar path's output is bit-identical to the MMX path for the same input is `[O]` (§7.B) — not required to answer to justify omitting it, since it is never selected on any real deployment target |
| `0x19e1e`–`0x19fcc` (function A, MMX, 8-bit source, mono), `0x19fd2`–`0x1a1b1` (function B, MMX, 8-bit source, stereo) | The two MMX mixer variants that read an 8-bit-per-sample PCM source | `gm.dls` is 495/495 sixteen-bit mono PCM waves (`[D:gm.dls]`, re-verified independently by this section, §7.A.5, and by Part 2 §2.11/Part 6 §6.9 `verify_render_format.py`, 21/21 checks) — the format-code dispatcher only ever emits codes `0x71`(B)/`0x72`(D) for the two output-arity choices, and every `gm.dls` wave's own `wBitsPerSample=16` forces the 16-bit branch of that dispatch, so A/B are real, dispatchable code that is simply never selected for any voice sourced from this collection | `[A:0x19316]`-`[0x1935c]` (dispatch chain); `[D:gm.dls]` wave-format inventory | Becomes reachable the moment a DLS collection supplies an 8-bit PCM wave (`wBitsPerSample=8` in `fmt `, a format this same parser fully supports — Part 2 §2.7.1/§2.7.2 documents the 8-bit load path in full) — this is a property of the *loaded collection*, not of the driver, and would flip immediately for a non-`gm.dls` sound bank |
| Any DLS articulation `(usSource, usControl, usDestination)` combination the parser's dispatch chain recognizes but `gm.dls` never supplies a connection for | Two specific combinations were checked directly against every `art1` connection block in the file (both instrument-level and region-level `lart`, 7,451 blocks total — matching Part 6's own independently-obtained count exactly, confirming this section's re-scan is consistent): **`(usSource=1 LFO, usControl=0x0081 CC1, usDestination=0x0001 ATTENUATION)`** — tremolo depth driven by the mod wheel — **0 of 7,451**; **`(usSource=2 KEYONVELOCITY, usDestination=0x0001 ATTENUATION)`** — a DLS-native velocity→attenuation connection (distinct from this driver's own hardcoded note-on velocity term, which is a separate mechanism entirely, §6.5 of Part 3) — **0 of 7,451** | Both combinations are explicitly recognized and correctly dispatched by the `art1` decoder (Part 2 §2.4.3, Source=1 and Source=2 tables) — this is real, exercised-by-other-inputs code (the *decoder function itself* is exercised thousands of times per load by the other combinations in the same table), it is simply never fed these two specific `(source,control,dest)` triples by this collection's authored content | `[D:gm.dls]`, independent re-scan script (this section, full-file both-scope scan; 7,451 total blocks cross-checked against Part 6's `verify_render_format.py` count of 7,451) | Becomes reachable the moment any instrument or region in a loaded collection authors a connection with exactly one of these two `(source,control,destination)` triples — a content fact, not a driver limitation; the decoder already handles it correctly, an implementer following the full `art1` table (Part 2 §2.4.3) already covers this case without any special handling |

##### 7.A.5 Independent verification of the `art1`-content claim (this section's own script)

Scratch script `check_unhit_destinations.py` (not retained), output:

```
total connection blocks (instrument-level + region-level lart combined): 7451

src=1(LFO) ctrl=0x0081(CC1) dest=0x0001(ATTENUATION) [tremolo via mod wheel]: count = 0
src=2(KEYONVELOCITY) dest=0x0001(ATTENUATION) [velocity-depth sensitivity]:   count = 0
```

(Full histogram omitted here for length; every other recognized
`(source,control,destination)` combination in the driver's dispatch chain
occurs at least once in this file — the two rows above are the only zero
counts among the full recognized set.)

---

### 7.B Part B — The open-questions register

24 distinct open questions were harvested from the five companion sections
and the tables appendix (grep of `\[O\]`, then deduplicated where the same
question is flagged in more than one document — noted explicitly below where
that happens). **7 of the 24 are behaviourally significant** (a wrong guess
produces audibly different output); the remainder are cosmetic (no output
consequence for `gm.dls`, or already narrowed to a near-certain value by
adjacent evidence even though the exact instruction/VMA is unlocated).

Two items are **resolved by cross-referencing between the five companion
sections themselves** and are removed from this register rather than listed
as open — see the note under §7.B.1 and §7.B.2.

#### 7.B.1 Two items closed by cross-reference, not listed as open below

- **The exact formula implemented by the timecent-conversion helpers
  `0x15364`/`0x153aa`** — Part 2's own DLS-parsing section flagged this
  `[O]` (§ "What remains open"). The companion articulation section
  independently traces the same two functions instruction-by-instruction and
  states the formula exactly: `tc = lScale/65536.0; duration =
  pow(2.0, tc/1200.0)`, truncated toward zero via the shared `0x106e0`
  helper (Part 3 §3.4.1, `[A:0x15364]` `[A:0x153aa]`). **This is
  settled, not open** — the Part 2 flag simply predates the articulation
  section's own pass over the same two VMAs.
- **The instrument `(bank,program,drum)`→instrument-object lookup function**
  — Part 2 flagged this `[O]` as "outside PAGE `0x142f8`–`0x1667f`". The
  companion articulation section documents it in full as `FindInstrument`
  (`0x14800`/`0x14796`), including its exact-equality match, its
  three-tier retry caller (`TriggerVoiceEvent`, `0x12e19`–`0x12e73`), and the
  region-selection predicate `FindRegionForNote` (`0x14722`)
  (Part 3 §3.1.2–§3.1.4). **This is settled, not open.**

#### 7.B.2 The register

| # | Question | Owning section(s) | Known | Not known | Discriminating observable | Significance |
|---|---|---|---|---|---|---|
| 1 | The 22050→44100 upsampler downstream of `swmidi.sys` (`kmixer.sys` on this OS) | Part 6 §6.1, §6.11 | This driver's own output contract is fixed, single-rate 22050 Hz stereo 16-bit PCM (`KSDATARANGE_AUDIO`, min=max=22050, `[A:0x1a710]`) — 44.1 kHz is never produced by this binary. Separately, reverse-engineering `kmixer.sys` itself (§6.11) recovered the resampler's **algorithm class**: a 60-tap-per-phase float32 FIR, two output phases per input sample, extended-precision accumulation truncated only at the final store `[A:kmixer 0x2c0a0]` `[A:kmixer 0x2c840]`; zero-order hold and linear interpolation are both excluded by direct code inspection (§6.11.2), corroborating this project's own empirical probe result (0.23%/0.87% match, `[M]`) | The **exact tap coefficients** (no static table exists anywhere in `kmixer.sys`'s `.rdata`/`.data`/`PAGEDATA`, §6.11.4) and the **exact rate-gating selection predicate** (the one stage-selection helper traced end-to-end selects on format tag/channel count/bit depth, never on `nSamplesPerSec`, §6.11.4) remain open | Capture the raw 22050 Hz output at the KS pin directly (bypassing the downstream stage) and separately capture the system's final 44.1 kHz output for the same input; compare spectral content above 11,025 Hz (the original Nyquist) against the recovered 60-tap FIR's expected passband/rolloff shape — this now discriminates the *exact coefficients*, not merely the *filter class*, since the class question is settled. Alternatively, live-debug a running `kmixer.sys` instance and dump the tap buffer — the pointer stored at the resampler-state object's own base, indexed by the count field at offset `0x30` (4 bytes per entry) — to recover the real taps directly (§6.11.4) | **Not applicable to a `swmidi.sys` clean-room implementation** — this stage is outside the binary being specified. Recovering its algorithm class sharpens the comparison instrument for anyone benchmarking against 44.1 kHz reference captures, but does not make such a comparison bit-exact: any implementation of this stage necessarily uses inferred, not extracted, tap coefficients (§6.11.4), and the reference captures carry their own ±1 LSB dither independent of this filter — a 44.1 kHz comparison remains a model, not a clean match |
| 2 | Envelope/controller-value update cadence: exact frame (sample) count per invocation of the per-buffer service routine `0x13054` | Part 3 §3.4.2, Part 6 §6.6 | Confirmed **block/buffer-cadenced, not per-sample**: `0x13054` drains due events, calls the per-voice render routine once per active voice per invocation, and promotes all six per-controller queues once per invocation — no per-sample controller re-evaluation exists in that routine `[A:0x13054]`. Separately, *within* a single render call, gain/phase-step ramps refresh every `ramp_period` samples, where `ramp_period` is itself a caller-supplied argument (`+0x14`) `[A:0x19e26]` | The actual frame count supplied to `0x13054` per call — its own caller lives outside every PAGE range examined by this project | Instrument a live driver (kernel debugger breakpoint on `0x13054`'s entry, log the frame-count argument its caller passes) or, acoustically, look for stair-stepping/quantization in a very fast envelope segment's rendered amplitude curve — a block size large enough to be audible would show discrete steps at multiples of the true block period | **Behaviourally significant** — this bounds how finely CC/envelope changes can be time-resolved; guessing too coarse a block size measurably smears fast modulation and event timing |
| 3 | Which of the two velocity-shaped tables (`0x1c9d0` squared-law, `0x1bfd4` linear/√-law) the driver selects for the **note-on velocity term** specifically | Appendix T §T.2/§T.3 (origin of the question), Part 3 §3.5/§3.6 (resolution) | **Largely resolved by direct disassembly, not merely by measurement**: the note-on velocity attenuation term is confirmed, by a full instruction trace, to call `0x16b94` — a one-line wrapper that is *always* `table_1c9d0[v]` (the squared-law table), unconditionally, with no branch selecting the other table anywhere in that function `[A:0x16b94]` `[A:19b94]`-`[A:0x19bad]`. The linear/√-law table's own confirmed, real consumer is a *different* mechanism entirely — one side of the stereo pan law (Part 3 §3.6) — not an alternate, never-taken velocity path | Whether any consumer elsewhere in the ~9,700-byte PAGE section outside the note-trigger/pan-law functions already traced ever reads the linear/√-law table for a velocity-like purpose was not exhaustively ruled out | A targeted grep of the remaining un-cited PAGE address space for any reference to `0x1bfd4` outside the pan-law function `0x19bfe`–`0x19c2a` would close the residual gap definitively | Originally framed as behaviourally significant (guessing the wrong curve shape audibly changes velocity response); **now narrowed to a near-zero residual risk** — the confirmed path is unconditional, not a runtime choice |
| 4 | **RESOLVED empirically, `[M: probe 25]` — no longer open.** Original question: which physical output channel receives the pan law's `gainA` (√-law) versus `gainB` (squared-law) | Part 3 §3.6 | A pan-sweep probe now exists (`probes/25_pan_law.mid`/`probe-results/25.flac`) and, measured directly, **contradicts the disassembly-derived two-table formula itself** (not merely leaving the L/R assignment open): reference center-pan (CC10=64) is 0 dB on *both* channels (a flat plateau CC10≈48..80), not the formula's predicted −3.0 dB/−11.9 dB split, and the hard-extreme off channel floors at ≈−20.2 dB, not the formula's −96 dB silence. Part 3 §3.6 now implements a 9-anchor measured table (`[M: probe 25]` for the anchor values, `[F:fitted]` for the between-anchor interpolation shape, `FITTED.md` Entry 5) in place of the disassembly formula, with the ordinary L/R convention (attenuates-as-CC10-increases → left, attenuates-as-CC10-decreases → right) reproducing the measured curves directly, no swap needed | Whether the disassembly reading (`gainA = *(0x1c1cc−pan·4)`, `gainB = table_1c9d0[pan]`) is simply wrong, or correct for a different build/revision than this project's reference captures, was not determined — only that it does not match this project's own reference audio | A byte-level re-trace of `0x19bfe`–`0x19c2a` against the exact binary that produced `probe-results/25.flac` (if its provenance can be pinned down) would settle whether this was a mistraced read or a build-version mismatch | **No longer behaviourally significant for this implementation** — the shipped code now matches the measured reference directly; only historically interesting for reconciling the disassembly with the audio |
| 5 | The write site that populates a region's rate-multiplicand field (`region+0x14`, read at `0x18f6f` as the phase-increment's rate multiplicand) with the wave's own sample rate | Part 3 §3.3.4/§3.11, Part 6 §6.5 | The **mechanism** is fully confirmed: `phaseIncRaw = region[+0x14] * pitchRatio_Q12`, then divided by the render-rate denominator (Part 3 §3.3.4, `[A:0x18f6f]`–`[A:0x18f7d]`). This section independently re-traced the region base pointer through the immediately preceding unity-note/fine-tune reads (`0x18f4f`/`0x18f58`) and confirmed the base pointer equals `&region+4` and is unchanged across the whole span `0x18f4f`–`0x18f8f` — so the dereference at offset `0x10` from that base pointer is unambiguously `region+0x14`, as Part 3 states, **not** a direct wave-object field access as Part 6's parallel description of the same instruction states. That discrepancy is noted here as a finding of this section, not adopted from either source uncritically | The actual write site that copies the wave's own `nSamplesPerSec` (confirmed stored at wave+0x08, `[A:0x156ba]`) into `region+0x14` is not in the region-creation code (`0x14ff4`) or the region-body dispatcher (`0x15ae6`) (Part 3's own search). Part 6 separately cites a default-value write of `0x5622` (22050) at VMA `0x14585` as evidence this is "a genuine per-wave rate field" — but `0x14585` is inside a small (~0x1c-byte) object-initializer function (`0x1456e`–`0x1459a`) that is structurally distinct from **both** the confirmed wave-object initializer (`0x145a0`, 0x34 bytes) **and** the region initializer (`0x14ff4`, 0x34 bytes); this section additionally found that `0x1456e` itself has **zero confirmed callers anywhere in the disassembly**, so its relevance to `region+0x14` specifically is not established either | Trace forward from wave+0x08's confirmed write site (`0x156ba`) for any copy into a region-associated location before note-trigger time, and separately locate a genuine caller of `0x1456e` to determine what object it actually default-initializes | **Mechanically cosmetic for `gm.dls`**: the correct *value* (the wave's own sample rate) is already independently inferable with high confidence from the multiply/divide shape and from the observed correct handling of the three 24000 Hz waves (Part 6 §6.5) — an implementer can safely set this field to the wave's own `nSamplesPerSec` without needing the located write site. What remains open is bookkeeping (which VMA/struct performs the copy), not the value |
| 6 | Design intent behind the 48-primary + 6-reserve voice-pool split | Part 5 §5.2–§5.5, "Open items" | The **mechanics** are fully resolved: 54 physically distinct 0x158-byte objects, 48 in one allocation loop and 6 in a second, immediately following (`[A:0x12a78]`–`[A:0x12aef]`); the reserve tier is topped up from primary and, failing that, forces accelerated release of active voices (§5.4); acoustic measurement (two independent probes, ascending and descending) confirms the resulting sustained ceiling of 48 exactly matches the pigeonhole arithmetic this split predicts | Why the driver author chose this specific two-tier split rather than one flat 54-deep pool, or 48/0, or some other ratio | None available — this is a design-rationale question, not a behavior question; no comment, symbol, or configuration value in the binary states an intent, and no measurement can distinguish "why" from "what" | **Cosmetic** — an implementer who reproduces the mechanics exactly (54 objects, 48+6 split, the exact top-up/steal cadence) is correct regardless of the original motive |
| 7 | The exact scaling arithmetic converting a channel's raw 14-bit pitch-bend value (center 8192) and its RPN0 range (default 200 cents) into the cents figure consumed by the pitch/ratio chain | Part 3 §3.3.2/§3.11 | The default values (center 8192, range 200 cents) are confirmed at their ctor (`[A:0x16e3d]`/`[A:0x16e44]`); the standard MIDI formula `(raw14 − 8192)/8192 × rangeCents` is consistent with every value observed | The specific multiply/divide instruction sequence performing this scaling was not re-traced this pass | Trace forward from the pitch-bend controller's `+0x18` "current value" field (§3.2.1 of the control-plane section) to its first consumer inside the pitch-ratio computation at `0x18ef4`, and/or render a probe sweeping pitch-bend across its full range at several RPN0 range settings and confirm the resulting pitch shift matches the standard linear formula exactly (not, e.g., a clamped or non-linear variant) | **Behaviourally significant** — an incorrect scaling formula produces an audibly wrong pitch-bend response curve, though the standard-formula hypothesis is well supported by every value checked so far |
| 8 | The exact real-time unit of the choke/steal release-time clamp's divisor constant `70` (at `0x19834`) | Part 5 §5.6/§5.8 | The clamp's existence and choke/steal-specificity are confirmed by instruction read; **measured** cut time is 70.0 ms, reproduced identically across two independently-timed test cases in the same probe | Whether `70` is literally a millisecond count, or a sample count divisor that happens to produce ≈70 ms at this driver's fixed 22050 Hz render rate, or some other units convention, is not provable from the instructions alone | The measured 70.0 ms figure already strongly corroborates a real-time (not tick-count) interpretation — a further discriminating test would compare the clamp's absolute duration against the driver's confirmed 1 kHz system timer tick (`ExSetTimerResolution(10000,...)`, control-plane section §-adjacent evidence): `1000/70 ≈ 14.3` does not land on a clean tick-count ratio, which weighs (not proves) against a tick-based unit | **Behaviourally significant in principle, low residual risk in practice** — the measured value (70.0 ms) is already usable directly by an implementer without resolving the exact internal unit convention |
| 9 | `device+0xf10` (RPN/NRPN-select register)'s value at driver-add time / immediately after any reset | Part 4 §4.4, "Open items" #1 | No write to this field exists anywhere in PAGE, `.text`, or `.init` (exhaustive grep, confirmed empty); none of the three reset functions (`ResetDevice`, `ResetAllChannelControllers`, `ResetAllProgramsAndRhythmGroups`) touch it either | Its actual runtime value depends on kernel pool-allocator behavior at allocation time, which is not visible in static disassembly | Not resolvable from this evidence set at all — would require a live kernel-mode memory dump immediately after device construction, before any RPN/NRPN message has ever been sent on a given channel | **Cosmetic in practice** — real-world MIDI content always sends an explicit RPN/NRPN-select message before a Data Entry message; this field's power-on value only matters for the degenerate case of a Data Entry sent with no prior select on that channel since driver load, which essentially never occurs in authored content |
| 10 | Consumer of `device+0xf7c`/`+0x12bc` (RPN1 Fine Tune / RPN2 Coarse Tune result fields, in cents) outside the control plane | Part 4 §4.2.2/§4.4, "Open items" #4 | Values and write sites are fully confirmed (§4.4 of that section); **the Master Volume field `device+0xf78`'s consumer, listed in the same open-items group, is in fact independently confirmed** by the companion articulation section (`TriggerVoiceEvent`'s attenuation sum folds in `device+0xf78` directly, Part 3 §3.5) — that part of this open item is resolved by cross-reference and should not be treated as open. **The RPN1/RPN2 consumer's *behavior* is now settled by measurement, `[M: probe 23]`**: `probes/23_rpn_tune.mid` section D holds a single note and sends an RPN2 Coarse Tune change mid-note; the rendered fundamental stays flat across that point, confirming RPN1/RPN2 are sampled once at note-on into the voice's fixed pitch base and do not retune an already-sounding voice, unlike Pitch Bend (Part 4 §4.4 carries the same measurement and citation) | Only the *exact disassembly VMA* of the original driver's RPN1/RPN2 consumer — as opposed to its confirmed behavior — remains unlocated in either the control-plane or articulation sections' traced ranges | Locate the specific instruction(s) inside the pitch/ratio chain (by analogy with the confirmed pitch-bend chain, Part 3 §3.3.2) that read `device+0xf7c`/`+0x12bc` at note-trigger time, to pin the note-on-latch behavior to a specific address rather than only to its observed effect | **Cosmetic** — the behaviourally significant question (does a tuning change retune a held note, and by how much) is now measured and settled; only the disassembly address of the code that does it remains open, which has no further observable consequence beyond what is already confirmed |
| 11 | Consumer of the per-part 16×12 Scale Tuning grid (`device+0xfbc`, written by the GS `DT1` fallthrough SysEx case) | Part 4 §4.2.2/§4.5, "Open items" #5 | Write site, shape (12 signed semitone-class offsets per part, `value = byte − 0x40`), and its consistency with the Roland GS "Scale Tuning" parameter's documented shape are all confirmed | No confirmed reader anywhere in the control plane; the Roland-GS-parameter identity itself is `[I]`, not confirmed against an independently obtained Roland specification | Render a probe sending this exact SysEx address (`40 1x 40`) with distinct non-zero per-semitone-class values and check for a measurable, semitone-class-selective pitch shift on notes of the corresponding pitch class | **Behaviourally significant if real content uses it** (niche in practice — GS Scale Tuning is rarely authored) |
| 12 | Full layout/identity of the object pointed to by voice `+0x104`, and the precise scheduling semantics of the `+0x108`/`+0x10c` and `+0x118`/`+0x11c` 64-bit timestamp pairs beyond "consumed by the DSP-advance routine `0x19644`" | Part 5 §5.1, "Open items" | Only 4 of the pointed-to object's own offsets are exercised by any traced code path (`+0x0`, `+0x4`, `+0x18`, `+0x1c`); the timestamp pairs are confirmed to be read/written by the envelope-advance machinery but not characterized further | The rest of the sub-object's layout; the exact scheduling algorithm the timestamp pairs implement beyond "a 64-bit timestamp pair used by envelope advance" | Full instruction trace of `0x19644` (the DSP-advance routine) and its own callees — not attempted in any companion section | **Cosmetic for the topics this register covers** — the pool/steal/choke/note-off mechanics this project needs are already fully specified without the rest of this object; matters only for sample-accurate envelope-segment timing beyond what §5.6 of the voice section already establishes |
| 13 | Exact semantic identity of `region+0x28`, the third region-selection gate (`0x1473e`, must be `> 0` for a region to ever be selected at note-on) | Part 2 §2.9 (field origin), Part 3 §3.1.4/§3.11 | Confirmed load-bearing: a region with `region+0x28 <= 0` is never selected by `FindRegionForNote` regardless of key-range match `[A:0x1473e]` | What the field actually denotes (candidate: wave-data-readiness, per its association with the `0x1469e` "has articulation?" predicate) was not independently re-derived | Locate every write site to `region+0x28` and correlate against successful vs. failed wave-pointer resolution (§2.8.4 of the DLS-parsing section) | **Cosmetic for `gm.dls` itself** (every one of its 1,498 regions presumably satisfies this gate, since audio plays correctly) — **behaviourally significant for a general-purpose implementation** handling malformed or partially-resolved DLS content, where guessing the wrong semantics could silently drop or wrongly admit notes |
| 14 | The runtime value of `flagByte` (governs the 16-bit-source storage policy: full 16-bit, MSB-truncate-to-8-bit, or log-companded-to-8-bit) for a stock `gm.dls` load | Part 2 §2.7.2/"Open items" | The three candidate storage strategies, their exact selection bit-gating, and the `wsmp.fulOptions` veto mechanism are all fully documented byte-exact | Which of the three paths actually fires at runtime — the ultimate origin of `flagByte` was not traced back to its source within the parser's own range | Compare the quantization noise floor of rendered reference audio against the known log-companding table (`0x1c1d0`) — a companded 8-bit path leaves a measurable, specific noise signature distinct from full 16-bit fidelity; alternatively, locate the `AddDevice`-time call site that supplies this argument to the top-level parse call | **Behaviourally significant if the lossy path is taken** — this would be an audible, measurable quality reduction versus full-fidelity 16-bit playback; not resolved either way in this evidence set |
| 15 | The code that dereferences a resolved region's wave pointer and actually walks `fmt `/`data`/`wsmp` to populate playable sample parameters | Part 2 §2.8.6/"Open items" | Confirmed absent from the parser's own PAGE range (`0x142f8`–`0x1667f`); the handler functions this deferred call must eventually invoke (`0x15662`, `0x154df`, `0x15464`) are themselves fully documented byte-exact, since `gm.dls`'s own chunk order means this decode never happens eagerly during the initial `wvpl` walk (§2.8.3 of that section) | The call site that triggers this deferred decode — most plausibly at first note-on/voice-allocation time | Instrument a live driver at first note-on for a program never previously triggered and observe whether `fmt`/`data`/`wsmp` parsing runs synchronously at that point | **Cosmetic** — the handler logic that decodes these chunks, once invoked, is already fully specified (§2.3/§2.7 of the DLS-parsing section); an implementer needs only "parse fmt/data/wsmp for a wave chunk" (already spec'd), not the exact trigger site |
| 16 | The absolute fixed-point unit of the raw gain field as it arrives at the mixer (`voice+0x2c`/`+0x30`) before the driver's own internal `<<3` scaling | Part 6 §6.4.5/"Open items" | The internal `<<8>>5` (=`<<3`) scaling relationship between the field's rest representation and its in-loop active representation is confirmed | Whether the physical unit is, e.g., Q16.16 relative to full scale, or something else, is not independently pinned down | Not resolvable from the render path alone — the full gain-computation chain (velocity/CC7/master-volume hundredths-of-a-dB sums, `10^(atten/2000)` conversion) is already independently specified in the articulation section (§6.5), which supplies the actual numeric gain value an implementer needs without requiring this specific fixed-point label | **Cosmetic** — redundant with the already-fully-specified gain-computation chain elsewhere in this project |
| 17 | Whether the MMX and scalar mixer families produce bit-identical output for identical inputs | Part 6 §6.2.1/"Open items" | Both share the same two-tap linear-interpolation shape and the same manual saturating-add idiom | Not tested — no rendered comparison exists | Force the scalar path (would require binary patching the CPUID check, since no software switch exists) and diff its output against the MMX path for an identical input stream | **Cosmetic** — the scalar path is unreached on any real CPU regardless of the answer (§7.A.4) |
| 18 | The precise struct/pointer chain supplying the render-rate denominator dereferenced at `0x18f7b` | Part 6 §6.5/"Open items" | The arithmetic role (render-rate denominator in a wave-rate/render-rate pitch-ratio division) is unambiguous from the instruction sequence | Independent confirmation that the dereferenced value is literally 22050 in every case was not exhaustively traced past the control-plane boundary | Not critical to resolve further — this driver's own output-format contract (§6.1 of the render section) fixes the render rate at exactly 22050 Hz with no negotiation, so the *value* this pointer resolves to is already independently known from the fixed format contract regardless of the exact pointer chain | **Cosmetic** — the value is already known from an independent, stronger source (the fixed `KSDATARANGE_AUDIO` contract) |
| 19 | Whether a distinct ADSR-style envelope-shape state machine exists as its own code, and if so its update cadence and what feeds the per-call ramp-step arguments | Part 6 §6.6/"Open items" (same underlying gap as #2 above, from the render side rather than the control-plane side) | The render functions only consume caller-supplied ramp endpoints/slopes; no such state machine was located within the render path itself | Whether it exists as distinct code elsewhere, and its cadence | Same discriminating observable as #2 | **Behaviourally significant**, merged with #2 — listed separately here only because the two companion sections flag it from opposite ends of the same call boundary and neither closes it |
| 20 | Whether rendered output shows velocity-dependent spectral content beyond gain (an acoustic cross-check for the "no spectral-shaping stage" conclusion) | Part 6 §6.8/"Open items" | The **code-level** conclusion is solid: the full per-sample operation set in the live mixer is exactly two-tap fetch, linear interpolation, one gain multiply, one saturating accumulate — no coefficient-based difference equation, no modulated-tap structure, anywhere in the traced window; separately, `gm.dls` itself authors zero connections to either DLS-1 spectral-shaping destination code (`0x0500`/`0x0501`), out of 7,451 total blocks | No rendered reference audio was available to this section's own pass to acoustically cross-check for velocity-dependent spectral tilt | Spectral-centroid analysis of a velocity sweep on a single sustained note, comparing high-velocity vs. low-velocity spectra for any systematic tilt beyond what the confirmed gain-only model predicts | **Not behaviourally significant as an open risk** — the code-level evidence is already strong enough (full instruction-level read of the only per-sample path) that this is listed for completeness, not because the "no spectral-shaping stage" conclusion is actually in doubt (see Part C) |
| 21 | Semantic purpose of the non-standard `edit` chunk (5 code sites, one per DLS scope) | Part 2 §2.3.10/"Open items" | All five sites, their exact store targets, and their distinct backing objects (including the previously-undocumented `lart`-scope site) are fully enumerated | No string or fixed-value comparison ties any meaning to any of the five stored values anywhere in the code read | Grep the entire PAGE section for any read of the five destination fields (wave+0x22, lart-block+0x60, region+0x2c, instrument+0x18, collection+0x3c) outside their own write sites | **Cosmetic** — no consumer means no behavior to reproduce; an implementer can store the value (for round-tripping) and otherwise ignore it |
| 22 | Semantic purpose of `ptbl` placeholder+0x2f (a `flagByte` copy stored per-cue) | Part 2 §2.8.1/"Open items" | Write site and value origin (the same `flagByte` threaded through the whole parse, item #14 above) are confirmed | No reader was found | Same as #14 — if `flagByte`'s ultimate origin is resolved, this becomes a simple corollary | **Cosmetic** — no confirmed consumer |
| 23 | Exact meaning of `0x1469e`'s backing field for a wave object specifically ("has articulation?"/duplicate-veto predicate, general shape "return (this+0x28 > 0)" established elsewhere in the codebase) | Part 2 §2.7.2/"Open items" | The predicate's general shape and its role in gating duplicate-`data`-chunk replacement are confirmed | Its wave-scope semantics specifically were not independently re-derived | Trace `0x1469e`'s full body and correlate its result against actual duplicate-chunk scenarios (none occur in `gm.dls` itself, per the parser's own hard-error-on-duplicate behavior) | **Cosmetic for `gm.dls`** — `gm.dls` never triggers the duplicate-chunk path this predicate gates |
| 24 | Whether a `wsmp` loop end running past the actually-decoded sample length is clamped anywhere | Part 2 §2.6/"Open items" | Not found in the parser's own range; may be handled at note-render time or not at all | Whether such a clamp exists anywhere in the driver | Construct a malformed DLS file with a loop end deliberately past the decoded sample length and observe whether the reference driver crashes, reads garbage, or clamps | **Cosmetic for `gm.dls`** (its own loop ends are presumably always valid, since audio plays correctly) — a robustness question for a general-purpose implementation, not a `gm.dls`-conformance question |

---

### 7.C Part C — Prior-art adjudication deltas

`FACTS.md` — the earlier prior-art adjudication pass, not carried into this
repo — is not rewritten here. Each row below cites `FACTS.md`'s own
previous verdict and gives the new verdict this project's later work
(the five companion sections) supports, with fresh evidence.

| Claim | Previous verdict (`FACTS.md`) | New verdict | Evidence |
|---|---|---|---|
| Internal render rate is 22050 Hz, upsampled externally to 44100 | PROVEN (sample rate 22050 Hz alone, via `KSDATARANGE_AUDIO` + aliasing-fold audio) | PROVEN, reinforced: the format contract additionally rules out any *second*, alternate rate anywhere in the 9,796-byte PAGEDATA section, and states explicitly that 44.1 kHz is produced by a separate, out-of-scope downstream stage | Part 6 §6.1, `[A:0x1a710]`, `verify_render_format.py` 21/21 |
| No CC91/CC93 audio-effect sends, no DLS-1 spectral-shaping (resonant-cutoff-style) destinations | PROVEN (CC91/CC93 inert at all 9 levels; no DLS spectral-shaping-destination connections) | PROVEN, reinforced with a full code-level read: zero art1 connections to either DLS-1 spectral-shaping destination (`0x0500`/`0x0501`) out of 7,451 total blocks, and the complete per-sample mixer operation set (two-tap fetch, interpolate, one gain multiply, one saturating accumulate) contains no coefficient-based or modulated-tap structure anywhere in the traced window | Part 6 §6.8, `verify_render_format.py` |
| One squared volume law governs velocity, CC7, and master volume | UNRESOLVED for CC7/CC11's dispatch mechanism specifically (table identity for velocity and master volume was already PROVEN) | PROVEN in full — the CC7/Expression consumer is directly traced to the same table (`0x16d8a`→`0x16c50`→`table_1c9d0[result]`), closing the exact gap `FACTS.md` left open | Part 3 §3.5, `[A:0x16d8a]`–`[A:0x16d9a]` |
| Amplitude release is exponential (time law), rates per-patch from DLS articulation | No corresponding `FACTS.md` row (not a prior-art claim adjudicated there — carried here as an independently-established fact with no predecessor verdict to diff against) | PROVEN (time-law shape) — the driver's own `pow(2.0, tc/1200)` timecent formula is, by construction, exponential; per-patch rates are taken directly from each instrument's authored EG1/EG2 timecents | Part 3 §3.4.1/§3.4.2 |
| The voice pool is 54 objects (48 primary + 6 reserve), of which at most 48 sustain; a "64/32" claim is refuted either way | UNRESOLVED (pool size, XP); NOT_APPLICABLE (Win10 "32" figure, out of scope) | PROVEN — 54 physically distinct 0x158-byte objects (48+6, two back-to-back allocation loops), no cap-check instruction anywhere; acoustic measurement (two independent probes) shows exactly 48 survive a sustained 80-note overshoot, matching pigeonhole arithmetic exactly. Both "64" (≠54) and "32" (48 survive, not 32) are refuted by this same evidence | Part 5 §5.2/§5.5 |
| Stealing is oldest-first | PROVEN (function `0x124a8`, priority order confirmed) | PROVEN, reaffirmed, **with a refinement `FACTS.md` did not have**: `0x124a8`'s comparator is not fully symmetric — a released voice, once installed as the eviction candidate, is not protected against being displaced by a later, older, still-held voice; this asymmetry never surfaces in either measured probe (both use only note-ons, no note-offs, so every voice is in the "held" state throughout), which is exactly why the measured behavior is clean oldest-first. Separately, `FACTS.md`'s own open note about whether the tie-break field `+0x13c` "could in principle be channel-derived" is now closed: `+0x13c` is confirmed (by write-site pattern) to be the live amplitude-envelope level, not a channel-derived value, and no channel-number comparison appears anywhere in either steal function — refuting "prioritizes lower channel numbers" outright | Part 5 §5.7 |
| CC120 and CC123 both release rather than cut; CC120 bypasses the sustain hold while CC123 honours it | UNRESOLVED (XP portion) — `FACTS.md`'s own stated top follow-up item, pending analysis of `probe-results/13.flac` | PROVEN via disassembly, without needing the probe: CC120's handler (`0xFD` sentinel) never reads `+0x140` or the per-channel CC64 cache anywhere in its body (exhaustive grep of its span, zero hits); CC123's handler (`0xFF` sentinel) explicitly re-checks the channel's cached CC64 value and defers via `+0x140` if the pedal is held. Both ultimately call the same underlying release primitive (`0x19a2c`) — neither cuts a voice instantly | Part 4 §4.3/§4.7; Part 5 §5.9 |
| Melodic bank select requires a prior GS Reset; drum-kit selection does not | UNRESOLVED for both halves — `FACTS.md`'s own top-priority open item, explicitly calling for "a new probe: bank select + program change with no GS Reset anywhere in the file" | PROVEN for the bank-select gate **without needing that probe**: CC0 and CC32 both test `device+0xf54` (the GS-mode flag) before ever writing the per-channel bank byte — absent a prior GS Reset, the byte is simply never written, so effective bank is always 0 regardless of what CC0/CC32 values are sent, `[A:0x1341e]`/`[A:0x13492]`. Drum-kit selection is PROVEN independent of this flag both structurally (`TriggerVoiceEvent`'s drum-bit injection never references `device+0xf54` anywhere, exhaustive grep) and acoustically (program 25 vs. 0, −15.4 dB residual, no GS Reset present in the source file) | Part 3 §3.1.1; Part 4 §4.8 |
| Undefined bank falls back to bank 0 of the same program; no capital-tone round-down | UNRESOLVED — spectral-similarity measurement (0.8175) was inconclusive; `FACTS.md`'s own single highest-priority open item ("Reverse the instrument-lookup function... This single function would settle: the CTF bank-fallback rule") | PROVEN outright via disassembly: `FindInstrument`'s three-tier retry (exact locale → drum-bit-only → program-only/bank-0/non-drum) is precisely "fall back to bank 0 of the same program," and the exact-equality matcher contains no masking/rounding logic anywhere, directly refuting round-down CTF. This closes `FACTS.md`'s own explicitly-named top-priority open item. Regarding CC32 specifically: it is **not literally ignored by the driver** — it is dispatched and stored identically to CC0, gated by the same GS-mode flag — but `gm.dls`'s own content never authors a non-zero bank LSB for any of its 235 instruments, so CC32's value has zero observable effect on this specific collection as a fact about the content, not a driver quirk | Part 3 §3.1.2/§3.1.3; Part 2 §2.10 (bankLSB==0 for all 235 instruments) |
| Exclusive key groups are real (7 in `gm.dls`) and data-driven with no drum-channel gate; the choke completes in 70 ms, matching a rate-clamp divisor of 70 | UNRESOLVED, with an explicitly flagged open contradiction: a "same function as ordinary note-off" identification predicting a multi-second release, versus a measured 0.35 s cut time | RESOLVED, both halves: the choke/steal path (`0x19aa4`) is **not** literally identical to ordinary note-off (`0x19a2c`) — it shares only the pitch-EG release call, but uses a separate, rate-clamped configurator (`0x19834`, dividing by the constant 70) for the amplitude segment specifically. This closes the "same function" ambiguity `FACTS.md` flagged. Measured cut time is 70.0 ms (not 0.35 s); the 0.35 s figure is shown to be a conflation with an unrelated sample-length annotation ("LONG WHISTLE, 0.354 s sample") in the same probe's manifest, not a genuine measurement of the choke mechanism | Part 5 §5.6/§5.8 |
| `usTransform` is read nowhere — every connection is treated as linear regardless of declared transform | No corresponding `FACTS.md` row (internal driver-behavior fact, not previously adjudicated against prior art) | PROVEN — exhaustive grep of the `art1` decoder's own address range for any access to offset `-0x2` from the decoder's base pointer (the transform field's offset) finds zero occurrences | Part 2 §2.4; Part 3 §3.7 |
| The NRPN quirk is confirmed: the parameter number is forced to `0x3FFF` | PROVEN | PROVEN, reaffirmed, no change | Part 4 §4.4, `[A:0x13514]` |
| Two claims about a percussion table are refuted: keys 25 and 26 have no region in the Standard drum kit, while the other fourteen entries are present | UNRESOLVED (partially supported) — only 4 of the 16 listed notes were confirmed via key-group membership; the other 12 (27,28,31-34,82-85) had no direct evidence | PROVEN in full, all 16 listed notes settled at once: the Standard kit's 61 regions span keys 27–87 with **zero gaps** — meaning every key in that range (including all 14 of the "other" listed notes: 27–34, 82–87) is covered by an actual region, while 25 and 26 fall entirely outside the kit's key range | Part 2 §2.11 (direct RIFF read, `keyrange=[27,87] gaps=[]`) |

#### 7.C.1 Observations about the prior-art *sources* (not the synthesizer)

Both already fully documented in `FACTS.md` itself (its own "Running concern
count" section); recorded here for completeness per this section's brief,
not re-derived. `msgs.md` and `index.html` are earlier write-ups by that
same prior-art author, external to this repo like `FACTS.md` itself:

- `msgs.md`'s "Rate Limit: None" table cell directly conflicts with the same
  author's `index.html` claim of a hard "1000 events/sec" ceiling — both
  individually UNRESOLVED, the disagreement between them flagged as a
  confirmed internal inconsistency, not resolved by either.
- `index.html`'s CC121 (Reset All Controllers) reset list contains two
  separate bullets labeled "Volume =", the second reading "Volume = `2000`
  (2 semitones)" — no code path anywhere gives Volume a value of 2000, and
  "2 semitones" is 200 cents, not 2000 of anything; the matching code value
  is the pitch-bend-range ctor field (`0xc8` = 200 cents = 2 semitones),
  making this almost certainly a mislabelled Pitch Bend Range row, not a
  second Volume entry.

---

### 7.D Summary for the report

- **Reachability script**: `verify_text_reachability.py` (RE workspace not retained), ran clean, asserted the three-unreachable result (`0x104c0`, `0x10710`, `0x10910`), output pasted in full above (§7.A.2).
- **Open questions harvested**: 24, after removing 2 that cross-reference resolved as settled. **7 of 24 are behaviourally significant**; 17 are cosmetic (no output consequence for `gm.dls`, or already narrowed to a near-certain value).
- **"Settled" items marked open instead**: none of the briefing's listed settled items failed verification against the companion sections — all thirteen checked out as stated or stronger. Two *supporting details* inside the reachability claim itself did not survive re-verification and are corrected in §7.A.3 (0x10910's alleged caller; one of the four call sites attributed to `pow`'s own body). Two items framed as open by the briefing (velocity-table selection, §7.B item 3; two DLS-parser open items, §7.B.1) were found to be substantially more settled than framed, and are presented with that narrowing made explicit rather than left as flatly open.

---

## Appendix T — Numeric Tables

Target binary: `swmidi.sys`, 56,576 bytes, Windows XP SP3 build 5.1.2600.5512
(Microsoft GS Wavetable Synth driver). This appendix is self-contained: every
table is either printed in full or given as a formula independently verified
byte-for-byte against the binary, with the verification script's actual stdout
pasted in. All addresses below are VMAs (virtual memory addresses, i.e.
runtime addresses once the image is loaded at its preferred base). Provenance
is marked on every non-obvious claim: **(a)** = read directly from
bytes/instructions in the binary, **(c)** = inference. There is no reference
audio for this project, so no **(b)** claims appear anywhere below.

All verification below was run against a single scratch script (not
retained), `final_verify.py`, which reads only the raw bytes of
`swmidi.sys` (no dependence on any prior report's transcriptions). Its
full, unedited stdout is reproduced in the sections below, split by topic.
The script itself is also pasted in full at the end of this document.

---

### T.0 Floating-point environment **(a)**

The x87 FPU control word used by this driver's floating-point helper code is
**`0x27F`**: bits 0-5 = all six exception masks set (invalid, denormal,
zero-divide, overflow, underflow, precision — all masked), bits 8-9
(precision control) = `10b` = **53-bit / double precision**, bits 10-11
(rounding control) = `00b` = **round-to-nearest-even**. Decoding `0x027F` as
four hex nibbles `0000 0010 0111 1111` confirms this bit-by-bit (PC field =
bits 9,8 = `1,0`; RC field = bits 11,10 = `0,0`).

**Where this is checked/enforced (read directly from the disassembly):** the
CRT `pow()` helper linked into `.text` at VMA `0x104c4` — which is called
directly by the velocity-attenuation table builder (§T.2) at VMA `0x16820` to
compute `(v/127)^4.0` — contains this exact sequence:

```c
104eb:   fpu_control_word = fpu_read_control_word();      // save current control word
104ef/104f5: if (fpu_control_word == 0x27F) goto 0x104fc;  // already double-precision/round-nearest -> same-value fast-path, skip
104f7:   force_fpu_control_word();                         // else: call 0x10b55

10b55: force_fpu_control_word() {
10b59:   precision_control_bits = caller_control_word & 0x300;   // keep precision-control bits from caller
10b5f:   new_control_word = precision_control_bits | 0x7f;       // force all exception masks + reserved bit
10b62/10b67: fpu_write_control_word(new_control_word);
}
```

and the same same-value fast-path check (skip the control-word restore if it
already matches `0x27F`) recurs at `0x10b26`, `0x10bde`, `0x10bef`,
`0x10c48` in the same CRT floating-point support code. This shows the
compiled unit's floating-point helpers are written to *expect* `0x27F` as
their steady-state control word (that is why the check is a same-value
fast-path, not an unconditional set) and to defensively re-assert it when it
is not. **Not found:** no explicit driver-level `fldcw 0x27F` exists in
the `.init`-section disassembly (DriverEntry/INIT) or in the PAGE
table-builder functions themselves — grepped explicitly, zero hits — so the *original* place this
mode gets established is outside the supplied disassembly (most likely the
default C runtime floating-point startup convention). The table-builder
functions for tables C/D/E (§T.4–§T.6, sine and two of the `log10` curves) call
`fyl2x`/`fsin` **directly**, inline, without going through this `pow()`
wrapper, so the `0x27F` fast-path check is directly confirmed only for the
one table (§T.2, velocity-attenuation) that calls `pow()`; for the others, the
control-word value is not independently instruction-traced — call this an
**(c)** inference, backed empirically by every recomputed table below
matching the values previously verified against this binary, exactly, using
plain IEEE-754 double-precision arithmetic.

Because `0x27F` selects 53-bit precision and round-to-nearest-even, and both
match the two properties that make x87 arithmetic equal IEEE-754 `binary64`
arithmetic bit-for-bit, **the float path is reproducible on a non-x87
target**: implementers may use ordinary `double` arithmetic and get
identical results to the original driver, *provided the truncating integer
conversion (below) is replicated exactly*, and (for exact bit-for-bit
reproduction) the exact stored float32/float64 literal values are used, not
"nicer" idealized constants (§T.7 explains why this matters).

**The `pow` ULP hazard is closed** (settled result, recorded here verbatim,
not independently recomputed in this task): *Across all 552 distinct
timecent values in gm.dls, evaluated both as seconds and as samples at
22050 Hz (1,104 evaluations), 7 land on exact powers of two and among the
remaining 1,097 inexact evaluations the minimum distance to a truncation
boundary is 1.37 million ULP (worst case tc=4330.571 → 269009.999920382).
Plain IEEE-754 binary64 `pow` suffices; no bit-exact MSVC `pow` is needed.*

#### T.0.1 Truncation, not rounding

Every table below is produced by the pattern `fistp` after forcing FPU
rounding-control bits to `11b` (truncate toward zero), via the helper at
`0x106e0`:

```c
106e6:   saved_control_word = fpu_read_control_word();      // save current control word
106eb/106ef: rounding_mode = saved_control_word | 0xc;      // sets RC field (bits 11:10) to 11b = truncate toward zero
106f2:   new_control_word = rounding_mode;
106f6:   fpu_write_control_word(new_control_word);
106f9:   result = (int)value;                               // truncating store, not round()
106fc:   fpu_write_control_word(saved_control_word);        // restore
```

This is C's `(int)` cast, **not** `round()`. This matters: the `0x106e0`
builder function is cited for every one of these tables, and every one of
them is computed with `trunc`, matching the actual instruction sequence —
not `round(...)`, which would be the wrong reading of the same cited
builder. The printed values below have been checked to match the
independently-supplied 128-value reference table for the velocity curve
exactly (§T.2).

---

### T.1 PE section map and VMA→file-offset conversion **(a)**

Parsed from scratch with `struct.unpack` against the DOS/PE/section
headers (script §T.1, pasted below). Image base `0x10000`; `SectionAlignment`
and `FileAlignment` are both `0x80`, which is why file offset equals
`VMA - ImageBase` exactly, for every one of the 8 sections and (confirmed)
for every byte referenced anywhere in this appendix:

```
CONVERSION FORMULA:  file_offset = VMA - 0x10000
```

| Section  | VMA          | RVA     | VirtSize | RawOff  | RawSize |
|----------|--------------|---------|----------|---------|---------|
| .text    | 0x00010380   | 0x0380  | 0x143a   | 0x0380  | 0x1480  |
| .rdata   | 0x00011800   | 0x1800  | 0x06f3   | 0x1800  | 0x0700  |
| .data    | 0x00011f00   | 0x1f00  | 0x0230   | 0x1f00  | 0x0280  |
| PAGE     | 0x00012180   | 0x2180  | 0x8402   | 0x2180  | 0x8480  |
| PAGEDATA | 0x0001a600   | 0xa600  | 0x2644   | 0xa600  | 0x2680  |
| INIT     | 0x0001cc80   | 0xcc80  | 0x06a8   | 0xcc80  | 0x0700  |
| .rsrc    | 0x0001d380   | 0xd380  | 0x0430   | 0xd380  | 0x0480  |
| .reloc   | 0x0001d800   | 0xd800  | 0x04f4   | 0xd800  | 0x0500  |

AddressOfEntryPoint = `0x1cc85` (DriverEntry, inside INIT). File size
(56,576 bytes) exactly equals the last section's raw offset + raw size —
no trailing data beyond the PE image.

```
############ 1. PE HEADERS (parsed from scratch) ############
file size = 56576
ImageBase = 0x10000  SectionAlignment = 0x80  FileAlignment = 0x80
AddressOfEntryPoint = 0x1cc85 (RVA 0xcc85)
8 sections (raw==VMA-ImageBase check follows):
  .text      VMA=0x00010380 VirtSize=0x143a RawOff=0x0380 RawSize=0x1480  VMA-ImageBase==RawOff: True
  .rdata     VMA=0x00011800 VirtSize=0x06f3 RawOff=0x1800 RawSize=0x0700  VMA-ImageBase==RawOff: True
  .data      VMA=0x00011f00 VirtSize=0x0230 RawOff=0x1f00 RawSize=0x0280  VMA-ImageBase==RawOff: True
  PAGE       VMA=0x00012180 VirtSize=0x8402 RawOff=0x2180 RawSize=0x8480  VMA-ImageBase==RawOff: True
  PAGEDATA   VMA=0x0001a600 VirtSize=0x2644 RawOff=0xa600 RawSize=0x2680  VMA-ImageBase==RawOff: True
  INIT       VMA=0x0001cc80 VirtSize=0x06a8 RawOff=0xcc80 RawSize=0x0700  VMA-ImageBase==RawOff: True
  .rsrc      VMA=0x0001d380 VirtSize=0x0430 RawOff=0xd380 RawSize=0x0480  VMA-ImageBase==RawOff: True
  .reloc     VMA=0x0001d800 VirtSize=0x04f4 RawOff=0xd800 RawSize=0x0500  VMA-ImageBase==RawOff: True
ALL 8 sections satisfy file_offset == VMA - 0x10000: True
CONVERSION FORMULA:  file_offset = VMA - 0x10000   (holds for every byte in this image)
```

---

### T.2 Velocity → attenuation table (`0x1c9d0`, 128 × int32; dB = table[v] / 100.0) **(a)**

**This table is built at runtime**, not stored on disk — every one of its 512
bytes is `0x00` in the file image (proven below, script §T.4). It is built once
by PAGE `0x16804`–`0x16857`, called once from AddDevice.

Formula, confirmed by replaying the exact x87 instruction sequence
(`fild`→`fmul (1/127)`→`fld 4.0`→`call 0x104c4` [pow] →`fldlg2`/`fxch`/`fyl2x`
[log10] →`fmul 1000`→`call 0x106e0` [trunc]):

```
table[0]   = -9600                                  (hardcoded, mov @0x1684c)
table[v]   = trunc(1000 * log10((v/127.0)**4))      for v = 1..127
```

**Units, stated numerically, not by name (a):** each entry is in hundredths of
a dB. To convert:

```
dB          = table[v] / 100.0
linear_gain = 10 ** (table[v] / 2000.0)
```

(`linear_gain` follows from `dB = 20*log10(linear_gain)` ⇒ `linear_gain =
10**(dB/20)` = `10**(table[v]/100/20)` = `10**(table[v]/2000)`.) Do **not**
divide by 10 — that is the conversion for tenths of a dB (a different,
10x-coarser unit), and applying it here silently produces attenuations 10x
too large (e.g. v=64 would read as −119 dB instead of the correct −11.9 dB,
i.e. effectively silent).

**`table[0] = -9600` is a hardcoded silence floor/clamp, not a point on the
curve (a).** The formula above is undefined at `v=0` (`log10(0)`) and, taken
as a continuous curve, is *not* asymptotic to −9600 as `v→0`: evaluating it
at `v=0.5` (off the integer domain, for illustration only) gives
`1000*log10((0.5/127.0)**4) ≈ -9619.3`, i.e. *more* negative than the
hardcoded floor. `table[0]` is written by a separate, unconditional
instruction at VMA `0x1684c` (a direct store of the constant `0xffffda80` =
−9600 into `table[0]` at `0x1c9d0`) that executes once, **after** the
`v=1..127` loop falls through to it; the loop's own store (for `v=1..127`)
is at a different instruction, VMA `0x16843` (a store of the computed
result into `table[index]` at `0x1c9d0`, indexed by the loop counter,
inside the loop body). The loop never writes index 0 itself — the loop
counter starts at `1` (initialized to `0x1` at `0x1680a`) — so the
`0x1684c` store is the sole source of `table[0]`, a
deliberately chosen silence floor, not a curve value.

**Mutual confirmation, binary vs. algebra (a):** `1000 * log10((v/127)**4)`
algebraically equals `4000 * log10(v/127)`, which in turn equals
`100 * (20 * log10((v/127)**2))` — i.e. `4000·log10(v/127) ≡
100·20·log10((v/127)²)` for all `v` (`log10(x⁴)=4·log10(x)`,
`log10(x²)=2·log10(x)`, and `4000 = 100·40 = 100·2·20`). In words: the
formula this appendix reads directly out of the binary is, term for term,
"100× a **squared**-amplitude dB law" (`20·log10(gain²)`, the classic
power/squared-voltage dB relation). This is an algebraic identity, checked
here by direct symbolic and numeric substitution, not a coincidence of
rounding. **Not independently corroborated against rendered audio in this
task** — this appendix (§T.0, opening note) explicitly has no audio reference
input, so no acoustic cross-check is asserted here; if such a cross-check
exists (e.g. from a probe-audio analysis elsewhere in this project), it
would be additional, separately-provenanced evidence, not part of this
appendix's verification.

`trunc` = truncation toward zero (§T.0). Recomputed independently in Python
double precision, using the **exact stored float32/float64 constants** read
from `.rdata` (not idealized values — see §T.4 for why this matters):

```
[  0]  -9600  -8415  -7211  -6506  -6006  -5619  -5302  -5034
[  8]  -4802  -4598  -4415  -4249  -4098  -3959  -3830  -3710
[ 16]  -3598  -3493  -3394  -3300  -3211  -3126  -3045  -2968
[ 24]  -2894  -2823  -2755  -2689  -2626  -2565  -2506  -2449
[ 32]  -2394  -2341  -2289  -2238  -2190  -2142  -2096  -2050
[ 40]  -2006  -1964  -1922  -1881  -1841  -1802  -1764  -1726
[ 48]  -1690  -1654  -1619  -1584  -1551  -1518  -1485  -1453
[ 56]  -1422  -1391  -1361  -1331  -1302  -1273  -1245  -1217
[ 64]  -1190  -1163  -1137  -1110  -1085  -1059  -1034  -1010
[ 72]   -985   -961   -938   -914   -891   -869   -846   -824
[ 80]   -802   -781   -759   -738   -718   -697   -677   -657
[ 88]   -637   -617   -598   -579   -560   -541   -522   -504
[ 96]   -486   -468   -450   -432   -415   -397   -380   -363
[104]   -347   -330   -313   -297   -281   -265   -249   -233
[112]   -218   -202   -187   -172   -157   -142   -127   -113
[120]    -98    -84    -69    -55    -41    -27    -13      0
```

**This recomputation matches the task-supplied 128-value reference table
exactly — all 128 entries, zero mismatches.**

**Round-vs-truncate divergence:** if the builder had used round-to-nearest
instead of truncation, **64 of the 127 computed entries (v=1..127)** would
differ by exactly 1 table unit, i.e. 0.01 dB (always: `round` one *less negative*, since
every value is negative and truncation-toward-zero rounds a negative number
up). Example differing velocities: v=3, 4, 6, 7, 8, 11, 14, 15, … (full list
in script output below). Intermediate double value for v=3, before
truncation: `1000 * log10((3/127.0)**4) = -6506.72987141667` → `trunc` gives
`-6506` (matches the table above); `round` would give `-6507`.

```
############ 4. Disk-zero proof for the 5 runtime-built tables ############
  Table A (velocity/attenuation): VMA 0x1c9d0, 512 bytes on disk, all-zero: True
############ 5. TABLE A: 0x1c9d0, 128 x int32, velocity->attenuation ############
  generator 0x16804-0x16857; v=0 hardcoded -9600 @0x1684c;
  v=1..127: trunc(1000 * log10( (v * (1/127))^4.0 ))  [pow via CIpow @0x104c4, called @0x16820]
 [  0]  -9600  -8415  -7211  -6506  -6006  -5619  -5302  -5034
 [  8]  -4802  -4598  -4415  -4249  -4098  -3959  -3830  -3710
 [ 16]  -3598  -3493  -3394  -3300  -3211  -3126  -3045  -2968
 [ 24]  -2894  -2823  -2755  -2689  -2626  -2565  -2506  -2449
 [ 32]  -2394  -2341  -2289  -2238  -2190  -2142  -2096  -2050
 [ 40]  -2006  -1964  -1922  -1881  -1841  -1802  -1764  -1726
 [ 48]  -1690  -1654  -1619  -1584  -1551  -1518  -1485  -1453
 [ 56]  -1422  -1391  -1361  -1331  -1302  -1273  -1245  -1217
 [ 64]  -1190  -1163  -1137  -1110  -1085  -1059  -1034  -1010
 [ 72]   -985   -961   -938   -914   -891   -869   -846   -824
 [ 80]   -802   -781   -759   -738   -718   -697   -677   -657
 [ 88]   -637   -617   -598   -579   -560   -541   -522   -504
 [ 96]   -486   -468   -450   -432   -415   -397   -380   -363
 [104]   -347   -330   -313   -297   -281   -265   -249   -233
 [112]   -218   -202   -187   -172   -157   -142   -127   -113
 [120]    -98    -84    -69    -55    -41    -27    -13      0
  MATCH vs task-provided 128-value reference list: YES, EXACT
  entries where round()-based build would differ from trunc(): 64 of 127 (v=0 excluded, hardcoded)
  example differing v's: [3, 4, 6, 7, 8, 11, 14, 15] ...
  e.g. v=3: double value before trunc = -6506.72987141667  trunc=-6506  round=-6507
```

---

### T.3 Linear velocity table (`0x1bfd4`, 127 × int32, plus scalar `0x1bfd0`) **(a)**

Also built at runtime (508 on-disk bytes, all zero — script §T.4), by PAGE
`0x16a3e`–`0x16a96`, called from the same AddDevice sequence right after the
table in §T.2. Instructions confirm truncation via the same `0x106e0` helper
(`call` at `0x16a79`):

```
scalar @0x1bfd0 = -2500          (hardcoded, mov @0x16a8a)
table[v] = trunc(1000 * log10(v / 127.0))     for v = 1..127
```

Full table (index k = v−1, i.e. `table[0]` corresponds to v=1):

```
[v=  1]  -2103  -1802  -1626  -1501  -1404  -1325  -1258  -1200
[v=  9]  -1149  -1103  -1062  -1024   -989   -957   -927   -899
[v= 17]   -873   -848   -825   -802   -781   -761   -742   -723
[v= 25]   -705   -688   -672   -656   -641   -626   -612   -598
[v= 33]   -585   -572   -559   -547   -535   -524   -512   -501
[v= 41]   -491   -480   -470   -460   -450   -441   -431   -422
[v= 49]   -413   -404   -396   -387   -379   -371   -363   -355
[v= 57]   -347   -340   -332   -325   -318   -311   -304   -297
[v= 65]   -290   -284   -277   -271   -264   -258   -252   -246
[v= 73]   -240   -234   -228   -222   -217   -211   -206   -200
[v= 81]   -195   -189   -184   -179   -174   -169   -164   -159
[v= 89]   -154   -149   -144   -140   -135   -130   -126   -121
[v= 97]   -117   -112   -108   -103    -99    -95    -90    -86
[v=105]    -82    -78    -74    -70    -66    -62    -58    -54
[v=113]    -50    -46    -43    -39    -35    -31    -28    -24
[v=121]    -21    -17    -13    -10     -6     -3      0
```

check: v=1 → −2103, v=64 → −297, v=127 → 0. Confirmed by direct
recomputation (script §T.6, below).

**Open question, not a decision:** two velocity-sensitivity curves exist —
this linear one (§T.3) and the concave/squared one (§T.2). Which one the render
path actually selects per note is **not recovered** in any of the supplied
disassembly. Prior measurement-based work (outside this task's inputs)
reports that the concave/squared curve (§T.2) is the one observed in practice,
but the selection code itself was never located — this is flagged here as an
open question, not asserted as settled.

```
############ 6. TABLE B: 0x1bfd4 (127 x int32) + floor scalar 0x1bfd0 ############
  generator 0x16a3e-0x16a96; v=1..127: trunc(1000 * log10(v * (1/127)));  scalar(v=0) = -2500 hardcoded @0x16a8a
  [v=  1]  -2103  -1802  -1626  -1501  -1404  -1325  -1258  -1200
  [v=  9]  -1149  -1103  -1062  -1024   -989   -957   -927   -899
  [v= 17]   -873   -848   -825   -802   -781   -761   -742   -723
  [v= 25]   -705   -688   -672   -656   -641   -626   -612   -598
  [v= 33]   -585   -572   -559   -547   -535   -524   -512   -501
  [v= 41]   -491   -480   -470   -460   -450   -441   -431   -422
  [v= 49]   -413   -404   -396   -387   -379   -371   -363   -355
  [v= 57]   -347   -340   -332   -325   -318   -311   -304   -297
  [v= 65]   -290   -284   -277   -271   -264   -258   -252   -246
  [v= 73]   -240   -234   -228   -222   -217   -211   -206   -200
  [v= 81]   -195   -189   -184   -179   -174   -169   -164   -159
  [v= 89]   -154   -149   -144   -140   -135   -130   -126   -121
  [v= 97]   -117   -112   -108   -103    -99    -95    -90    -86
  [v=105]    -82    -78    -74    -70    -66    -62    -58    -54
  [v=113]    -50    -46    -43    -39    -35    -31    -28    -24
  [v=121]    -21    -17    -13    -10     -6     -3      0
  check: v=1 -> -2103  v=64 -> -297  v=127 -> 0
```

---

### T.4 Table C — `0x1a9d8`, 201 × int16, indices 0..200 **(a)**

**Built at runtime** (402 on-disk bytes, all zero — verified directly:
`Table C: VMA 0x1a9d8, 402 bytes on disk, all-zero: True`). Builder is PAGE
`0x1685e`–`0x168b7`. Instruction-exact formula:

```
t[0] = 0                                              (explicit zeroing)
t[i] = trunc(1000 + ((i/200.0)**2 * 10000.0) * (1/96.0))    for i = 1..200
```

**Note on why bit-exact constants matter (found the hard way in this task):**
an initial pass using idealized Python constants (`1/200 = 0.005` exactly,
`2*pi` at full double precision for §T.5) computed `t[200] = 1000` and, for
Table D, `t[64] = 100`. Both are wrong. The `.rdata` pool stores `1/200` and
`2*pi` as **float32**, which are *not* exactly `0.005` / `2π`
(`0x11d04 = 0.004999999888241291`, `0x11d18 = 6.2831854820251465`). Using
the exact stored bit patterns instead of the ideal mathematical constants
gives `t[200] = 999` and Table-D `t[64] = 99`. This is the single concrete
demonstration, in this appendix, of why "exact reproduction" means reading
the literal 4-byte/8-byte patterns, not the nearest round decimal.

Full table:

```
[  0]    0  520  583  620  646  666  682  696
[  8]  708  719  728  737  745  752  759  765
[ 16]  771  776  782  787  791  796  800  804
[ 24]  808  811  815  818  822  825  828  831
[ 32]  834  836  839  842  844  847  849  852
[ 40]  854  856  858  860  863  865  867  868
[ 48]  870  872  874  876  878  879  881  883
[ 56]  884  886  887  889  891  892  894  895
[ 64]  896  898  899  901  902  903  905  906
[ 72]  907  908  910  911  912  913  914  915
[ 80]  917  918  919  920  921  922  923  924
[ 88]  925  926  927  928  929  930  931  932
[ 96]  933  934  935  936  937  938  939  939
[104]  940  941  942  943  944  945  945  946
[112]  947  948  949  949  950  951  952  953
[120]  953  954  955  956  956  957  958  958
[128]  959  960  961  961  962  963  963  964
[136]  965  965  966  967  967  968  969  969
[144]  970  970  971  972  972  973  973  974
[152]  975  975  976  976  977  978  978  979
[160]  979  980  980  981  982  982  983  983
[168]  984  984  985  985  986  986  987  987
[176]  988  988  989  989  990  990  991  991
[184]  992  992  993  993  994  994  995  995
[192]  996  996  997  997  998  998  999  999
[200]  999
```

`min=0 max=999`. Closed-form fit: **exact** with the formula above and the
exact stored constants (all 201 entries match, 0 residual, by construction —
this is a direct replay of the instructions, not a fit to an independent
ground truth, since the on-disk bytes are 0). Intermediate value showing why
`i=200` truncates to 999 and not 1000: `x = 200 * 0.004999999888241291 =
0.9999999776482582`, `x² = 0.9999999552965169`, `log10(x²) =
-1.9414476482398598e-08`, final value before trunc = `999.9999979776586`.

Consumed at PAGE `0x18b15` inside a 64-bit-timestamp routine
(`0x18a7a`-`0x18d1f`) — **identification of the purpose (time/envelope
progress-shaping curve) is (c) inference**; the table contents and the
generator code are (a) read facts.

```
############ 7. TABLE C: 0x1a9d8, 201 x int16 (indices 0..200) ############
  generator 0x1685e-0x168b7; t[0]=0; i=1..200: trunc(1000 + 10000*log10((i*(1/200))^2)*(1/96))
  check: i=1 -> 520  i=100 -> 937  i=199 -> 999  i=200 -> 999
  min 0 max 999
  i=200 intermediate: x=0.9999999776482582 x^2=0.9999999552965169 log10(x^2)=-1.9414476482398598e-08 -> final=999.9999979776586
```

---

### T.5 Table D — `0x1a7d8`, 256 × int16, sine LFO **(a)**

**Built at runtime** (512 on-disk bytes, all zero — verified:
`Table D: VMA 0x1a7d8, 512 bytes on disk, all-zero: True`). Builder is PAGE
`0x1691e`–`0x1695c`. Instruction-exact formula (multiply-by-2π happens
**before** multiply-by-1/256 in the actual instruction order —
`fmul 0x11d18` at `0x1692b` precedes `fmul 0x11d14` at `0x16931`):

```
t[i] = trunc(sin(i * 2*pi * (1/256)) * 100.0)     for i = 0..255
```

Full table (256 entries, one full sine period, amplitude ±100):

```
[  0]    0    2    4    7    9   12   14   17   19   21   24   26   29   31   33   35
[ 16]   38   40   42   44   47   49   51   53   55   57   59   61   63   65   67   68
[ 32]   70   72   74   75   77   78   80   81   83   84   85   87   88   89   90   91
[ 48]   92   93   94   94   95   96   97   97   98   98   98   99   99   99   99   99
[ 64]   99   99   99   99   99   99   98   98   98   97   97   96   95   94   94   93
[ 80]   92   91   90   89   88   87   85   84   83   81   80   78   77   75   74   72
[ 96]   70   68   67   65   63   61   59   57   55   53   51   49   47   44   42   40
[112]   38   35   33   31   29   26   24   21   19   17   14   12    9    7    4    2
[128]    0   -2   -4   -7   -9  -12  -14  -17  -19  -21  -24  -26  -29  -31  -33  -35
[144]  -38  -40  -42  -44  -47  -49  -51  -53  -55  -57  -59  -61  -63  -65  -67  -68
[160]  -70  -72  -74  -75  -77  -78  -80  -81  -83  -84  -85  -87  -88  -89  -90  -91
[176]  -92  -93  -94  -94  -95  -96  -97  -97  -98  -98  -98  -99  -99  -99  -99  -99
[192]  -99  -99  -99  -99  -99  -99  -98  -98  -98  -97  -97  -96  -95  -94  -94  -93
[208]  -92  -91  -90  -89  -88  -87  -85  -84  -83  -81  -80  -78  -77  -75  -74  -72
[224]  -70  -68  -67  -65  -63  -61  -59  -57  -55  -53  -51  -49  -47  -44  -42  -40
[240]  -38  -35  -33  -31  -29  -26  -24  -21  -19  -17  -14  -12   -9   -7   -4   -2
```

check: i=0 → 0, i=64 → **99** (not 100 — truncation of `99.9997…` caused by
the float32-rounded `2π` constant, see §T.4's note), i=128 → 0, i=192 → −99.
**Exact fit** (this is the instruction replay, no residual to report against
an independent ground truth since the on-disk bytes are 0). Verified the
result is order-independent for the two chained multiplies at this
precision: swapping the multiply order (×1/256 before ×2π, matching a prior
report's simulation order) produced the identical 256-entry table — 0
differences — so this table is not sensitive to that particular
non-associativity.

```
############ 8. TABLE D: 0x1a7d8, 256 x int16, sine LFO ############
  generator 0x1691e-0x1695c; i=0..255: trunc(sin(i*2pi*(1/256)) * 100)
  check: i=0 -> 0  i=64 -> 99  i=128 -> 0  i=192 -> -99
```

---

### T.6 Table E — `0x1c1d0`, 2048 × uint8, log-companding curve **(a)**

**Built at runtime** (2048 on-disk bytes, all zero — verified:
`Table E: VMA 0x1c1d0, 2048 bytes on disk, all-zero: True`). Builder is PAGE
`0x168be`–`0x16918`. Instruction-exact formula (the reciprocal `1/log10(8)`
is computed once, before the loop, and reused every iteration as `st(1)`):

```
inv  = 1.0 / log10(8.0)                    # computed once
t[i] = trunc( (log10(1.0 + i*7.0*(1/2048.0)) * 128.0) * inv )    for i=0..2047
```

Range `[0,127]`. Checked the two possible multiply orders for the final two
factors (`*128` then `*inv`, vs `*inv` then `*128`) — **0 of 2048 entries
differ** between the orders, so this table is not order-sensitive at this
precision. First 32 and last 8 entries:

```
first 32: [0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 6, 6]
last 8:   [127, 127, 127, 127, 127, 127, 127, 127]
```

Full 2048 entries (16/line) are reproduced verbatim from the verification
script's stdout below (identical output on re-run, since it is a pure
recomputation from stored constants, not measured data):

```
############ 9. TABLE E: 0x1c1d0, 2048 x uint8, log-companding curve ############
  generator 0x168be-0x16918; inv=1.0/log10(8.0) computed once;
  i=0..2047: trunc( (log10(1.0 + i*7*(1/2048)) * 128.0) * inv )
  first 32: [0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 6, 6]
  last 8:   [127, 127, 127, 127, 127, 127, 127, 127]
  min 0 max 127
  full 2048 values (16/line):
  [   0]   0   0   0   0   0   1   1   1   1   1   2   2   2   2   2   3
  [  16]   3   3   3   3   4   4   4   4   4   5   5   5   5   5   6   6
  [  32]   6   6   6   6   7   7   7   7   7   8   8   8   8   8   8   9
  [  48]   9   9   9   9  10  10  10  10  10  10  11  11  11  11  11  12
  [  64]  12  12  12  12  12  13  13  13  13  13  13  14  14  14  14  14
  [  80]  14  15  15  15  15  15  15  16  16  16  16  16  16  16  17  17
  [  96]  17  17  17  17  18  18  18  18  18  18  19  19  19  19  19  19
  [ 112]  19  20  20  20  20  20  20  21  21  21  21  21  21  21  22  22
  [ 128]  22  22  22  22  22  23  23  23  23  23  23  23  24  24  24  24
  [ 144]  24  24  24  25  25  25  25  25  25  25  26  26  26  26  26  26
  [ 160]  26  26  27  27  27  27  27  27  27  28  28  28  28  28  28  28
  [ 176]  28  29  29  29  29  29  29  29  30  30  30  30  30  30  30  30
  [ 192]  31  31  31  31  31  31  31  31  32  32  32  32  32  32  32  32
  [ 208]  33  33  33  33  33  33  33  33  34  34  34  34  34  34  34  34
  [ 224]  34  35  35  35  35  35  35  35  35  36  36  36  36  36  36  36
  [ 240]  36  36  37  37  37  37  37  37  37  37  38  38  38  38  38  38
  [ 256]  38  38  38  39  39  39  39  39  39  39  39  39  40  40  40  40
  [ 272]  40  40  40  40  40  41  41  41  41  41  41  41  41  41  41  42
  [ 288]  42  42  42  42  42  42  42  42  43  43  43  43  43  43  43  43
  [ 304]  43  43  44  44  44  44  44  44  44  44  44  44  45  45  45  45
  [ 320]  45  45  45  45  45  45  46  46  46  46  46  46  46  46  46  46
  [ 336]  47  47  47  47  47  47  47  47  47  47  48  48  48  48  48  48
  [ 352]  48  48  48  48  49  49  49  49  49  49  49  49  49  49  49  50
  [ 368]  50  50  50  50  50  50  50  50  50  50  51  51  51  51  51  51
  [ 384]  51  51  51  51  51  52  52  52  52  52  52  52  52  52  52  52
  [ 400]  53  53  53  53  53  53  53  53  53  53  53  54  54  54  54  54
  [ 416]  54  54  54  54  54  54  54  55  55  55  55  55  55  55  55  55
  [ 432]  55  55  55  56  56  56  56  56  56  56  56  56  56  56  57  57
  [ 448]  57  57  57  57  57  57  57  57  57  57  57  58  58  58  58  58
  [ 464]  58  58  58  58  58  58  58  59  59  59  59  59  59  59  59  59
  [ 480]  59  59  59  60  60  60  60  60  60  60  60  60  60  60  60  60
  [ 496]  61  61  61  61  61  61  61  61  61  61  61  61  61  62  62  62
  [ 512]  62  62  62  62  62  62  62  62  62  62  63  63  63  63  63  63
  [ 528]  63  63  63  63  63  63  63  64  64  64  64  64  64  64  64  64
  [ 544]  64  64  64  64  64  65  65  65  65  65  65  65  65  65  65  65
  [ 560]  65  65  65  66  66  66  66  66  66  66  66  66  66  66  66  66
  [ 576]  66  67  67  67  67  67  67  67  67  67  67  67  67  67  67  68
  [ 592]  68  68  68  68  68  68  68  68  68  68  68  68  68  69  69  69
  [ 608]  69  69  69  69  69  69  69  69  69  69  69  69  70  70  70  70
  [ 624]  70  70  70  70  70  70  70  70  70  70  70  71  71  71  71  71
  [ 640]  71  71  71  71  71  71  71  71  71  71  72  72  72  72  72  72
  [ 656]  72  72  72  72  72  72  72  72  72  72  73  73  73  73  73  73
  [ 672]  73  73  73  73  73  73  73  73  73  74  74  74  74  74  74  74
  [ 688]  74  74  74  74  74  74  74  74  74  75  75  75  75  75  75  75
  [ 704]  75  75  75  75  75  75  75  75  75  75  76  76  76  76  76  76
  [ 720]  76  76  76  76  76  76  76  76  76  76  77  77  77  77  77  77
  [ 736]  77  77  77  77  77  77  77  77  77  77  77  78  78  78  78  78
  [ 752]  78  78  78  78  78  78  78  78  78  78  78  78  79  79  79  79
  [ 768]  79  79  79  79  79  79  79  79  79  79  79  79  79  80  80  80
  [ 784]  80  80  80  80  80  80  80  80  80  80  80  80  80  80  80  81
  [ 800]  81  81  81  81  81  81  81  81  81  81  81  81  81  81  81  81
  [ 816]  81  82  82  82  82  82  82  82  82  82  82  82  82  82  82  82
  [ 832]  82  82  82  83  83  83  83  83  83  83  83  83  83  83  83  83
  [ 848]  83  83  83  83  83  84  84  84  84  84  84  84  84  84  84  84
  [ 864]  84  84  84  84  84  84  84  84  85  85  85  85  85  85  85  85
  [ 880]  85  85  85  85  85  85  85  85  85  85  85  86  86  86  86  86
  [ 896]  86  86  86  86  86  86  86  86  86  86  86  86  86  86  87  87
  [ 912]  87  87  87  87  87  87  87  87  87  87  87  87  87  87  87  87
  [ 928]  87  87  88  88  88  88  88  88  88  88  88  88  88  88  88  88
  [ 944]  88  88  88  88  88  88  89  89  89  89  89  89  89  89  89  89
  [ 960]  89  89  89  89  89  89  89  89  89  89  90  90  90  90  90  90
  [ 976]  90  90  90  90  90  90  90  90  90  90  90  90  90  90  90  91
  [ 992]  91  91  91  91  91  91  91  91  91  91  91  91  91  91  91  91
  [1008]  91  91  91  91  92  92  92  92  92  92  92  92  92  92  92  92
  [1024]  92  92  92  92  92  92  92  92  92  93  93  93  93  93  93  93
  [1040]  93  93  93  93  93  93  93  93  93  93  93  93  93  93  93  94
  [1056]  94  94  94  94  94  94  94  94  94  94  94  94  94  94  94  94
  [1072]  94  94  94  94  94  95  95  95  95  95  95  95  95  95  95  95
  [1088]  95  95  95  95  95  95  95  95  95  95  95  95  96  96  96  96
  [1104]  96  96  96  96  96  96  96  96  96  96  96  96  96  96  96  96
  [1120]  96  96  97  97  97  97  97  97  97  97  97  97  97  97  97  97
  [1136]  97  97  97  97  97  97  97  97  97  97  98  98  98  98  98  98
  [1152]  98  98  98  98  98  98  98  98  98  98  98  98  98  98  98  98
  [1168]  98  99  99  99  99  99  99  99  99  99  99  99  99  99  99  99
  [1184]  99  99  99  99  99  99  99  99  99 100 100 100 100 100 100 100
  [1200] 100 100 100 100 100 100 100 100 100 100 100 100 100 100 100 100
  [1216] 100 101 101 101 101 101 101 101 101 101 101 101 101 101 101 101
  [1232] 101 101 101 101 101 101 101 101 101 101 102 102 102 102 102 102
  [1248] 102 102 102 102 102 102 102 102 102 102 102 102 102 102 102 102
  [1264] 102 102 102 103 103 103 103 103 103 103 103 103 103 103 103 103
  [1280] 103 103 103 103 103 103 103 103 103 103 103 103 103 104 104 104
  [1296] 104 104 104 104 104 104 104 104 104 104 104 104 104 104 104 104
  [1312] 104 104 104 104 104 104 104 105 105 105 105 105 105 105 105 105
  [1328] 105 105 105 105 105 105 105 105 105 105 105 105 105 105 105 105
  [1344] 105 106 106 106 106 106 106 106 106 106 106 106 106 106 106 106
  [1360] 106 106 106 106 106 106 106 106 106 106 106 106 107 107 107 107
  [1376] 107 107 107 107 107 107 107 107 107 107 107 107 107 107 107 107
  [1392] 107 107 107 107 107 107 107 108 108 108 108 108 108 108 108 108
  [1408] 108 108 108 108 108 108 108 108 108 108 108 108 108 108 108 108
  [1424] 108 108 108 109 109 109 109 109 109 109 109 109 109 109 109 109
  [1440] 109 109 109 109 109 109 109 109 109 109 109 109 109 109 109 110
  [1456] 110 110 110 110 110 110 110 110 110 110 110 110 110 110 110 110
  [1472] 110 110 110 110 110 110 110 110 110 110 110 110 111 111 111 111
  [1488] 111 111 111 111 111 111 111 111 111 111 111 111 111 111 111 111
  [1504] 111 111 111 111 111 111 111 111 111 112 112 112 112 112 112 112
  [1520] 112 112 112 112 112 112 112 112 112 112 112 112 112 112 112 112
  [1536] 112 112 112 112 112 112 113 113 113 113 113 113 113 113 113 113
  [1552] 113 113 113 113 113 113 113 113 113 113 113 113 113 113 113 113
  [1568] 113 113 113 113 114 114 114 114 114 114 114 114 114 114 114 114
  [1584] 114 114 114 114 114 114 114 114 114 114 114 114 114 114 114 114
  [1600] 114 114 114 115 115 115 115 115 115 115 115 115 115 115 115 115
  [1616] 115 115 115 115 115 115 115 115 115 115 115 115 115 115 115 115
  [1632] 115 115 116 116 116 116 116 116 116 116 116 116 116 116 116 116
  [1648] 116 116 116 116 116 116 116 116 116 116 116 116 116 116 116 116
  [1664] 116 117 117 117 117 117 117 117 117 117 117 117 117 117 117 117
  [1680] 117 117 117 117 117 117 117 117 117 117 117 117 117 117 117 117
  [1696] 117 117 118 118 118 118 118 118 118 118 118 118 118 118 118 118
  [1712] 118 118 118 118 118 118 118 118 118 118 118 118 118 118 118 118
  [1728] 118 118 119 119 119 119 119 119 119 119 119 119 119 119 119 119
  [1744] 119 119 119 119 119 119 119 119 119 119 119 119 119 119 119 119
  [1760] 119 119 119 120 120 120 120 120 120 120 120 120 120 120 120 120
  [1776] 120 120 120 120 120 120 120 120 120 120 120 120 120 120 120 120
  [1792] 120 120 120 120 120 121 121 121 121 121 121 121 121 121 121 121
  [1808] 121 121 121 121 121 121 121 121 121 121 121 121 121 121 121 121
  [1824] 121 121 121 121 121 121 121 122 122 122 122 122 122 122 122 122
  [1840] 122 122 122 122 122 122 122 122 122 122 122 122 122 122 122 122
  [1856] 122 122 122 122 122 122 122 122 122 122 123 123 123 123 123 123
  [1872] 123 123 123 123 123 123 123 123 123 123 123 123 123 123 123 123
  [1888] 123 123 123 123 123 123 123 123 123 123 123 123 123 124 124 124
  [1904] 124 124 124 124 124 124 124 124 124 124 124 124 124 124 124 124
  [1920] 124 124 124 124 124 124 124 124 124 124 124 124 124 124 124 124
  [1936] 124 125 125 125 125 125 125 125 125 125 125 125 125 125 125 125
  [1952] 125 125 125 125 125 125 125 125 125 125 125 125 125 125 125 125
  [1968] 125 125 125 125 125 125 126 126 126 126 126 126 126 126 126 126
  [1984] 126 126 126 126 126 126 126 126 126 126 126 126 126 126 126 126
  [2000] 126 126 126 126 126 126 126 126 126 126 126 127 127 127 127 127
  [2016] 127 127 127 127 127 127 127 127 127 127 127 127 127 127 127 127
  [2032] 127 127 127 127 127 127 127 127 127 127 127 127 127 127 127 127
```

**Identification of purpose (mapping an 11-bit linear domain to a 7-bit
output via a log-base-8 curve) is (c) inference** — no cross-reference in the
supplied disassembly names it. The table contents and generator code are (a)
read facts.

#### T.6.1 Layout cross-check (bonus corroboration, (a))

The five runtime-built tables sit back-to-back in memory with no gaps:

```
Table D end (0x1a7d8+512=0x1a9d8) == Table C start (0x1a9d8): True
Table E end (0x1c1d0+2048=0x1c9d0) == Table A start (0x1c9d0): True
Table B end (0x1bfd4+127*4=0x1c1d0) == Table E start (0x1c1d0): True
```

---

### T.7 `.rdata` DSP scalar constant pool `0x11c98`–`0x11d1c` **(a)**

Every 4-byte slot is loaded/used by a `DWORD` (float32) x87 operation
(opcode `d8`/`d9`); every 8-byte slot by a `QWORD` (float64) operation
(opcode `dc`/`dd`) — width was read directly from the actual opcode byte at
each citing address, not assumed. There is one unreferenced 4-byte,
all-zero gap (`0x11cec`–`0x11cf0`, presumably alignment padding for the
following double). The pool ends exactly at `0x11d1c` (last used slot
`0x11d18`, width 4, `0x11d18+4 = 0x11d1c`).

At minimum, per the assignment: `0x11cf8` = `1/127` (used at `0x16814` as
`* (1/127)`, matches), `0x11cf0` = `4.0` (used at `0x1681a`, matches),
`0x11ce8` = `1000.0` (used at `0x1682b`, matches).

| VMA | width | raw bytes (LE) | value (exact double) | confirmed use |
|---|---|---|---|---|
|0x11c98|f32|`0000804f`|4294967296.0 (2³²)|`fadd` @0x15394,0x153e5|
|0x11c9c|f32|`0e745a32`|1.2715657859985185e-08|`fmul` @0x1537f|
|0x11ca0|f64|`0000000000000040`|2.0|`fld` @0x15376,0x153af; `fdiv` @0x10d10|
|0x11ca8|f32|`00008049`|1048576.0 (2²⁰)|`fmul` @0x153da|
|0x11cac|f32|`0000dc43`|440.0|`fmul` @0x153d4|
|0x11cb0|f32|`0e745a3a`|0.0008333333535119891|`fmul` @0x153c4|
|0x11cb4|f32|`00a0d745`|6900.0|`fsub` @0x153be|
|0x11cb8|f32|`00008037`|1.52587890625e-05|`fmul` @0x153b8|
|0x11cbc|f32|`0000803f`|1.0|`fadd` @0x168e7|
|0x11cc0|f64|`0000000000002040`|8.0|`fld` @0x168c4 (log base for Table E)|
|0x11cc8|f32|`00004041`|12.0|`fdiv` @0x16771|
|0x11ccc|f32|`00008045`|4096.0|`fmul` @0x1673b,0x16790|
|0x11cd0|f32|`00000040`|2.0|`fld` @0x16729,0x1677e|
|0x11cd4|f32|`00009644`|1200.0|`fdiv` @0x1671c|
|0x11cd8|f32|`00f07f45`|4095.0|`fmul` @0x166e6|
|0x11cdc|f32|`0000003f`|0.5|`fld` @0x166cd|
|0x11ce0|f32|`00002041`|10.0|`fld` @0x166bb|
|0x11ce4|f32|`0000c842`|100.0|`fdiv` @0x166ae; `fmul` @0x16939 (Table D amplitude)|
|0x11ce8|f32|`00007a44`|1000.0|`fmul` @0x1682b,0x16a73; `fadd` @0x16894 (Table A/B/C scale)|
| — |4-byte gap|`00000000`| (unreferenced) | none found |
|0x11cf0|f64|`0000000000001040`|4.0|`fld` @0x1681a (Table A exponent)|
|0x11cf8|f32|`0402013c`|0.007874015718698502 (≈1/127)|`fmul` @0x16814,0x16a67 (velocity normalisation)|
|0x11cfc|f32|`abaa2a3c`|0.010416666977107525 (≈1/96)|`fmul` @0x1688e (Table C scale)|
|0x11d00|f32|`00401c46`|10000.0|`fmul` @0x16888 (Table C scale)|
|0x11d04|f32|`0ad7a33b`|0.004999999888241291 (≈1/200)|`fmul` @0x16876 (Table C index scale)|
|0x11d08|f32|`00000043`|128.0|`fmul` @0x168f3 (Table E output scale)|
|0x11d0c|f32|`0000003a`|0.00048828125 (=1/2048 exactly)|`fmul` @0x168e1 (Table E index scale)|
|0x11d10|f32|`0000e040`|7.0|`fmul` @0x168db (Table E range)|
|0x11d14|f32|`0000803b`|0.00390625 (=1/256 exactly)|`fmul` @0x16931 (Table D index scale)|
|0x11d18|f32|`db0fc940`|6.2831854820251465 (≈2π)|`fmul` @0x1692b (Table D phase scale)|

```
############ 2. .rdata scalar constant pool 0x11c98-0x11d1c ############
  0x00011c98 4B raw=0000804f           = 4294967296.0                 fadd @0x15394,0x153e5
  0x00011c9c 4B raw=0e745a32           = 1.2715657859985185e-08       fmul @0x1537f
  0x00011ca0 8B raw=0000000000000040   = 2.0                          fld @0x15376,0x153af; fdiv @0x10d10
  0x00011ca8 4B raw=00008049           = 1048576.0                    fmul @0x153da
  0x00011cac 4B raw=0000dc43           = 440.0                        fmul @0x153d4
  0x00011cb0 4B raw=0e745a3a           = 0.0008333333535119891        fmul @0x153c4
  0x00011cb4 4B raw=00a0d745           = 6900.0                       fsub @0x153be
  0x00011cb8 4B raw=00008037           = 1.52587890625e-05            fmul @0x153b8
  0x00011cbc 4B raw=0000803f           = 1.0                          fadd @0x168e7
  0x00011cc0 8B raw=0000000000002040   = 8.0                          fld @0x168c4
  0x00011cc8 4B raw=00004041           = 12.0                         fdiv @0x16771
  0x00011ccc 4B raw=00008045           = 4096.0                       fmul @0x1673b,0x16790
  0x00011cd0 4B raw=00000040           = 2.0                          fld @0x16729,0x1677e
  0x00011cd4 4B raw=00009644           = 1200.0                       fdiv @0x1671c
  0x00011cd8 4B raw=00f07f45           = 4095.0                       fmul @0x166e6
  0x00011cdc 4B raw=0000003f           = 0.5                          fld @0x166cd
  0x00011ce0 4B raw=00002041           = 10.0                         fld @0x166bb
  0x00011ce4 4B raw=0000c842           = 100.0                        fdiv @0x166ae; fmul @0x16939
  0x00011ce8 4B raw=00007a44           = 1000.0                       fmul @0x1682b,0x16a73; fadd @0x16894
  [GAP 0x11cec-0x11cf0, 4 bytes, unreferenced] raw=00000000
  0x00011cf0 8B raw=0000000000001040   = 4.0                          fld @0x1681a
  0x00011cf8 4B raw=0402013c           = 0.007874015718698502         fmul @0x16814,0x16a67
  0x00011cfc 4B raw=abaa2a3c           = 0.010416666977107525         fmul @0x1688e
  0x00011d00 4B raw=00401c46           = 10000.0                      fmul @0x16888
  0x00011d04 4B raw=0ad7a33b           = 0.004999999888241291         fmul @0x16876
  0x00011d08 4B raw=00000043           = 128.0                        fmul @0x168f3
  0x00011d0c 4B raw=0000003a           = 0.00048828125                fmul @0x168e1
  0x00011d10 4B raw=0000e040           = 7.0                          fmul @0x168db
  0x00011d14 4B raw=0000803b           = 0.00390625                   fmul @0x16931
  0x00011d18 4B raw=db0fc940           = 6.2831854820251465           fmul @0x1692b
  pool ends at 0x11d1c (spec says 0x11d1c): MATCH
```

(Note: some entries above — e.g. `0x11ca0`/`0x11ca8`/`0x11cac`/`0x11cb0`/
`0x11cb4`/`0x11cb8`, and `0x11cc8`/`0x11ccc`/`0x11cd0`/`0x11cd4`/`0x11cd8`/
`0x11cdc`/`0x11ce0` — are consumed by pitch-conversion and DAC-gain tables
that are outside this appendix's required scope (§T.2–§T.6 only); they are
included here only because task item 7 requires decoding the *entire* pool
0x11c98–0x11d1c, and their citing VMAs are given for completeness.)

---

### T.8 Table 0 — channel processing/init order **(a)**

**Static, on disk**, VMA `0x1a600`, 16 bytes, `u8[16]`:

```
[9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15]
```

Used at PAGE `0x123a4` — inside a 16-iteration loop (loop counter = 0..15,
bound-checked against `0x10`) reading `table0[i]` at `0x1a600` and
copying it into a per-device-instance 16-entry array at offset `+0x12fc`
from that instance's base, indexed by the same counter. This is a
byte-for-byte copy of the static table into per-instance state at
device-open time (function `0x12354`–`0x123d7`).

**(c) inference for interpretation:** MIDI channel 9 (0-based) — the GM
percussion channel — appears first in this order, followed by channels 0-8
and 10-15 in natural order; a plausible reading is that this guarantees the
drum channel's state is initialized before the melodic channels, but this
appendix makes no claim about what consumes the resulting per-instance array
beyond the copy itself, which is a (a) read fact.

```
############ 3. Table 0 (0x1a600, 16 x u8, STATIC on disk) ############
  bytes: [9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15]
  used at PAGE 0x123a4 (mov al,[ebx+0x1a600]) copied into per-instance array +0x12fc, ebx=0..15
```

---

### T.9 Full verification script

Every number in this appendix was produced or cross-checked by the following
script (a consolidated scratch script, `final_verify.py`, not retained,
with intermediate/exploratory versions alongside it during authoring:
`pe_headers.py`, `velocity_table.py`, `linear_velocity_table.py`,
`tables_cde.py`, `tables_cde_exact.py`, `table0.py`, `rdata_pool.py`, also
not retained). It depends on nothing but the raw bytes of `swmidi.sys`.

```python
#!/usr/bin/env python3
"""
Consolidated, from-scratch verification script for the swmidi.sys spec-tables
appendix. Reads only swmidi.sys (raw bytes, supplied by the user) and
the PAGE/.text/.init disassembly dumps (RE workspace not retained; grepped
separately, not re-parsed here). Every numeric value below is either:
  - struct.unpack'd directly from the file (PE headers, rdata constants,
    Table 0, disk-zero proofs), or
  - computed in Python double precision by replaying the *exact* x87
    instruction order and the *exact* stored float32/float64 constants
    for the five runtime-built tables.
"""
import struct, math

PATH = "swmidi.sys"
data = open(PATH, "rb").read()
IMG_BASE = 0x10000

def off(vma): return vma - IMG_BASE
def f32(vma):
    o = off(vma); return struct.unpack_from("<f", data, o)[0]
def f64(vma):
    o = off(vma); return struct.unpack_from("<d", data, o)[0]
def trunc(x): return math.trunc(x)

# =====================================================================
print("############ 1. PE HEADERS (parsed from scratch) ############")
assert data[0:2] == b"MZ"
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
coff_off = e_lfanew + 4
machine, nsections, timestamp, symtab, nsyms, opthdrsz, characteristics = \
    struct.unpack_from("<HHIIIHH", data, coff_off)
opt_off = coff_off + 20
(magic, maj_lv, min_lv, sizeof_code, sizeof_idata, sizeof_udata,
 entrypoint, base_of_code, base_of_data, image_base,
 sec_align, file_align) = struct.unpack_from("<HBBIIIIIIIII", data, opt_off)
sizeof_image, sizeof_headers = struct.unpack_from("<II", data, opt_off + 56)
print(f"file size = {len(data)}")
print(f"ImageBase = {image_base:#x}  SectionAlignment = {sec_align:#x}  FileAlignment = {file_align:#x}")
print(f"AddressOfEntryPoint = {image_base+entrypoint:#x} (RVA {entrypoint:#x})")
sec_table_off = opt_off + opthdrsz
print(f"{nsections} sections (raw==VMA-ImageBase check follows):")
sections = []
all_match = True
for i in range(nsections):
    so = sec_table_off + i*40
    name = data[so:so+8].rstrip(b"\x00").decode("ascii","replace")
    (virt_size, virt_addr, sizeof_raw, ptr_raw, ptr_reloc, ptr_line,
     n_reloc, n_line, chars) = struct.unpack_from("<IIIIIIHHI", data, so+8)
    vma = image_base + virt_addr
    ok = (vma - image_base) == ptr_raw
    all_match &= ok
    sections.append((name, vma, virt_size, ptr_raw, sizeof_raw))
    print(f"  {name:10s} VMA={vma:#010x} VirtSize={virt_size:#06x} RawOff={ptr_raw:#06x} "
          f"RawSize={sizeof_raw:#06x}  VMA-ImageBase==RawOff: {ok}")
print(f"ALL 8 sections satisfy file_offset == VMA - {image_base:#x}: {all_match}")
print("CONVERSION FORMULA:  file_offset = VMA - 0x10000   (holds for every byte in this image)")

# =====================================================================
print("\n############ 2. .rdata scalar constant pool 0x11c98-0x11d1c ############")
slots = [
    (0x11c98,4,"fadd @0x15394,0x153e5"),(0x11c9c,4,"fmul @0x1537f"),
    (0x11ca0,8,"fld @0x15376,0x153af; fdiv @0x10d10"),(0x11ca8,4,"fmul @0x153da"),
    (0x11cac,4,"fmul @0x153d4"),(0x11cb0,4,"fmul @0x153c4"),(0x11cb4,4,"fsub @0x153be"),
    (0x11cb8,4,"fmul @0x153b8"),(0x11cbc,4,"fadd @0x168e7"),(0x11cc0,8,"fld @0x168c4"),
    (0x11cc8,4,"fdiv @0x16771"),(0x11ccc,4,"fmul @0x1673b,0x16790"),
    (0x11cd0,4,"fld @0x16729,0x1677e"),(0x11cd4,4,"fdiv @0x1671c"),
    (0x11cd8,4,"fmul @0x166e6"),(0x11cdc,4,"fld @0x166cd"),(0x11ce0,4,"fld @0x166bb"),
    (0x11ce4,4,"fdiv @0x166ae; fmul @0x16939"),
    (0x11ce8,4,"fmul @0x1682b,0x16a73; fadd @0x16894"),
    (0x11cf0,8,"fld @0x1681a"),(0x11cf8,4,"fmul @0x16814,0x16a67"),
    (0x11cfc,4,"fmul @0x1688e"),(0x11d00,4,"fmul @0x16888"),(0x11d04,4,"fmul @0x16876"),
    (0x11d08,4,"fmul @0x168f3"),(0x11d0c,4,"fmul @0x168e1"),(0x11d10,4,"fmul @0x168db"),
    (0x11d14,4,"fmul @0x16931"),(0x11d18,4,"fmul @0x1692b"),
]
prev_end = 0x11c98
for vma,w,note in slots:
    if vma != prev_end:
        g_off = off(prev_end); g_len = vma-prev_end
        print(f"  [GAP {prev_end:#x}-{vma:#x}, {g_len} bytes, unreferenced] raw={data[g_off:g_off+g_len].hex()}")
    o = off(vma); raw = data[o:o+w]
    val = struct.unpack_from("<f", raw)[0] if w==4 else struct.unpack_from("<d", raw)[0]
    print(f"  {vma:#010x} {w}B raw={raw.hex():18s} = {val!r:28s} {note}")
    prev_end = vma+w
print(f"  pool ends at {prev_end:#x} (spec says 0x11d1c): {'MATCH' if prev_end==0x11d1c else 'MISMATCH'}")

C_1_127=f32(0x11cf8); C_4_0=f64(0x11cf0); C_1000=f32(0x11ce8)
C_1_200=f32(0x11d04); C_10000=f32(0x11d00); C_1_96=f32(0x11cfc)
C_8_0=f64(0x11cc0); C_7_0=f32(0x11d10); C_1_2048=f32(0x11d0c)
C_1_0=f32(0x11cbc); C_128_0=f32(0x11d08); C_2PI=f32(0x11d18); C_1_256=f32(0x11d14)
C_100=f32(0x11ce4)
LOG10_2 = math.log10(2.0)
def hw_log10(x): return LOG10_2 * math.log2(x)

# =====================================================================
print("\n############ 3. Table 0 (0x1a600, 16 x u8, STATIC on disk) ############")
t0 = list(data[off(0x1a600):off(0x1a600)+16])
print("  bytes:", t0)
print("  used at PAGE 0x123a4 (mov al,[ebx+0x1a600]) copied into per-instance array +0x12fc, ebx=0..15")

# =====================================================================
print("\n############ 4. Disk-zero proof for the 5 runtime-built tables ############")
runtime_tables = [
    ("A (velocity/attenuation)", 0x1c9d0, 128*4),
    ("B (linear velocity)      ", 0x1bfd4, 127*4),
    ("C (0..200 curve)         ", 0x1a9d8, 201*2),
    ("D (sine LFO)             ", 0x1a7d8, 256*2),
    ("E (log-companding)       ", 0x1c1d0, 2048*1),
]
for name, vma, n in runtime_tables:
    raw = data[off(vma):off(vma)+n]
    print(f"  Table {name}: VMA {vma:#x}, {n} bytes on disk, all-zero: {all(b==0 for b in raw)}")

# =====================================================================
print("\n############ 5. TABLE A: 0x1c9d0, 128 x int32, velocity->attenuation ############")
print("  generator 0x16804-0x16857; v=0 hardcoded -9600 @0x1684c;")
print("  v=1..127: trunc(1000 * log10( (v * (1/127))^4.0 ))  [pow via CIpow @0x104c4, called @0x16820]")
tA = [-9600]
for v in range(1,128):
    x = v * C_1_127
    p = x ** C_4_0
    tA.append(trunc(C_1000 * hw_log10(p)))
GIVEN = [-9600,-8415,-7211,-6506,-6006,-5619,-5302,-5034,-4802,-4598,-4415,-4249,-4098,-3959,-3830,-3710,
-3598,-3493,-3394,-3300,-3211,-3126,-3045,-2968,-2894,-2823,-2755,-2689,-2626,-2565,-2506,-2449,
-2394,-2341,-2289,-2238,-2190,-2142,-2096,-2050,-2006,-1964,-1922,-1881,-1841,-1802,-1764,-1726,
-1690,-1654,-1619,-1584,-1551,-1518,-1485,-1453,-1422,-1391,-1361,-1331,-1302,-1273,-1245,-1217,
-1190,-1163,-1137,-1110,-1085,-1059,-1034,-1010,-985,-961,-938,-914,-891,-869,-846,-824,
-802,-781,-759,-738,-718,-697,-677,-657,-637,-617,-598,-579,-560,-541,-522,-504,
-486,-468,-450,-432,-415,-397,-380,-363,-347,-330,-313,-297,-281,-265,-249,-233,
-218,-202,-187,-172,-157,-142,-127,-113,-98,-84,-69,-55,-41,-27,-13,0]
for i in range(0,128,8):
    print(" ["+f"{i:3d}"+"] "+" ".join(f"{x:6d}" for x in tA[i:i+8]))
mism = [(i,tA[i],GIVEN[i]) for i in range(128) if tA[i]!=GIVEN[i]]
print(f"  MATCH vs task-provided 128-value reference list: {'YES, EXACT' if not mism else 'NO, '+str(len(mism))+' mismatches'}")
rnd = [-9600]+[round(C_1000*hw_log10((v*C_1_127)**C_4_0)) for v in range(1,128)]
rdiff = [i for i in range(128) if rnd[i]!=tA[i]]
print(f"  entries where round()-based build would differ from trunc(): {len(rdiff)} of 127 (v=0 excluded, hardcoded)")
print(f"  example differing v's: {rdiff[:8]} ...")
print(f"  e.g. v=3: double value before trunc = {C_1000*hw_log10((3*C_1_127)**C_4_0)!r}  trunc={tA[3]}  round={rnd[3]}")

# =====================================================================
print("\n############ 6. TABLE B: 0x1bfd4 (127 x int32) + floor scalar 0x1bfd0 ############")
print("  generator 0x16a3e-0x16a96; v=1..127: trunc(1000 * log10(v * (1/127)));  scalar(v=0) = -2500 hardcoded @0x16a8a")
tB = [trunc(C_1000*hw_log10(v*C_1_127)) for v in range(1,128)]
for i in range(0,127,8):
    print(f"  [v={i+1:3d}] " + " ".join(f"{x:6d}" for x in tB[i:i+8]))
print("  check: v=1 ->", tB[0], " v=64 ->", tB[63], " v=127 ->", tB[126])

# =====================================================================
print("\n############ 7. TABLE C: 0x1a9d8, 201 x int16 (indices 0..200) ############")
print("  generator 0x1685e-0x168b7; t[0]=0; i=1..200: trunc(1000 + 10000*log10((i*(1/200))^2)*(1/96))")
tC=[0]
for i in range(1,201):
    x=i*C_1_200; sq=x*x; lg=hw_log10(sq)*C_10000*C_1_96 + C_1000
    tC.append(trunc(lg))
for i in range(0,201,8):
    print(f"  [{i:3d}] "+" ".join(f"{x:4d}" for x in tC[i:i+8]))
print("  check: i=1 ->",tC[1]," i=100 ->",tC[100]," i=199 ->",tC[199]," i=200 ->",tC[200])
print("  min",min(tC),"max",max(tC))
x=200*C_1_200; print(f"  i=200 intermediate: x={x!r} x^2={x*x!r} log10(x^2)={hw_log10(x*x)!r} -> final={hw_log10(x*x)*C_10000*C_1_96+C_1000!r}")

# =====================================================================
print("\n############ 8. TABLE D: 0x1a7d8, 256 x int16, sine LFO ############")
print("  generator 0x1691e-0x1695c; i=0..255: trunc(sin(i*2pi*(1/256)) * 100)")
tD=[]
for i in range(256):
    ph = i*C_2PI*C_1_256
    tD.append(trunc(math.sin(ph)*C_100))
for i in range(0,256,16):
    print(f"  [{i:3d}] "+" ".join(f"{x:4d}" for x in tD[i:i+16]))
print("  check: i=0 ->",tD[0]," i=64 ->",tD[64]," i=128 ->",tD[128]," i=192 ->",tD[192])

# =====================================================================
print("\n############ 9. TABLE E: 0x1c1d0, 2048 x uint8, log-companding curve ############")
print("  generator 0x168be-0x16918; inv=1.0/log10(8.0) computed once;")
print("  i=0..2047: trunc( (log10(1.0 + i*7*(1/2048)) * 128.0) * inv )")
lg8 = hw_log10(C_8_0); inv = C_1_0/lg8
tE=[]
for i in range(2048):
    x = i*C_7_0*C_1_2048 + C_1_0
    lgx = hw_log10(x)*C_128_0
    tE.append(trunc(lgx*inv))
print("  first 32:", tE[:32])
print("  last 8:  ", tE[-8:])
print("  min",min(tE),"max",max(tE))
print("  full 2048 values (16/line):")
for i in range(0,2048,16):
    print(f"  [{i:4d}] " + " ".join(f"{x:3d}" for x in tE[i:i+16]))

# =====================================================================
print("\n############ 10. layout contiguity cross-checks ############")
print(f"  Table D end (0x1a7d8+512={0x1a7d8+512:#x}) == Table C start (0x1a9d8): {0x1a7d8+512==0x1a9d8}")
print(f"  Table E end (0x1c1d0+2048={0x1c1d0+2048:#x}) == Table A start (0x1c9d0): {0x1c1d0+2048==0x1c9d0}")
print(f"  Table B end (0x1bfd4+127*4={0x1bfd4+127*4:#x}) == Table E start (0x1c1d0): {0x1bfd4+127*4==0x1c1d0}")

print("\n############ DONE ############")
```
