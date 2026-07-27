/* render.c -- the mixer: interpolation, gain, saturating accumulate.
 * SPEC.md Part 6.
 *
 *  - Q12 phase accumulator: bits[31:12] integer sample index, bits[11:0]
 *    12-bit interpolation fraction (S6.4.2).
 *  - Two-tap linear interpolation, gain applied strictly after interpolation
 *    (S6.4.4/S6.4.5).
 *  - Saturating accumulate directly into the 16-bit output buffer, no wider
 *    intermediate accumulator (S6.4.6).
 *  - Per-wave sample rate is honoured in the phase-step composition (S6.5):
 *    the phase step was computed at note-on from each wave's own
 *    `sample_rate` field (voice.c), not a fixed constant, so gm.dls's three
 *    24000 Hz waves get a correctly non-unity phase step at this 22050 Hz
 *    render rate.
 *  - Loop-end handling is a single conditional subtraction, never a
 *    modulo/while loop (S6.4.8), with the same caveat SPEC.md itself states:
 *    this is only exact if the phase step never exceeds one loop length in
 *    a single output sample (true for any sane pitch/loop combination).
 */
#include "render.h"
#include "voice.h"

/* Amplitude segment length, SPEC.adoc S6.6 / `[A:0x18fba]`. The driver renders
 * each voice in segments of `min(next envelope change, device->+0x18, buffer
 * end)`, samples envelope x gain at the segment's END (`[A:0x19720]`), and
 * ramps the applied amplitude linearly to it across the segment
 * (`[A:0x190c8]`). Its own cap is `(rate + 19) / 20` = 1103 frames = 50.0 ms at
 * 22050 `[A:0x128f9]`, but the cap is never what binds: the driver's buffers
 * are 0x1000 bytes = 1024 frames `[A:0x184c0]` and the per-call frame count
 * comes from the KS descriptor above it (`[A:0x181fa]`, SPEC.adoc open question
 * #2), so in practice the buffer ends the segment first.
 *
 * The LENGTH is therefore `[F:fitted]`, not recovered -- it cannot be, because
 * it was set by the capture machine's audio buffering rather than by anything
 * in the binary. Two sources disagree about it and both are reported here
 * rather than one being quietly dropped:
 *
 * DIRECT MEASUREMENT says ~512 frames (23.2 ms). Probe 45's ten sub-segment
 * releases give 23.6/23.6/23.7/23.5/23.4 and 23.5/23.5/23.6/23.5/23.7 ms, sd
 * 0.1 over a 52x level range; probe 44's four short-release patches imply
 * 24-28 ms; probe 46's note-off sweep spreads its early-release start over
 * 27.8 ms with a 12.5 ms mean, half a segment, which is where a uniformly
 * placed note-off inside one lands.
 *
 * THE CORPUS SWEEP says 128 (5.8 ms). 256 ships, splitting them. 63 items,
 * mean spectral residual / mean envelope r / field-only dead, against
 * -31.1483 / 0.9238 / 2260 ms for the one-pole this replaces, all measured
 * AFTER the reap fix in voice_step_envelope (before it, a CC11 gate could
 * delete live voices and every number here was contaminated):
 *
 *      128 fr   -31.1778 / 0.9251 / 2595 ms      the corpus optimum
 *      256 fr   -31.1440 / 0.9252 / 2545 ms   <- ships
 *      384 fr   -31.0694 / 0.9243 / 2525 ms
 *      512 fr   -30.9013 / 0.9229 / 2460 ms      the directly-measured value
 *
 * 256 costs 0.004 dB of corpus mean -- flat -- and has the best envelope r of
 * anything tested including the one-pole. It is also the best of any build on
 * the gating tests this work started from: sine-gate -25.00 -> -27.61 (128
 * gives -26.20, 512 gives -28.20), sine2 -25.50 -> -26.86.
 *
 * The two sources disagree at all because we cannot match the reference's grid
 * PHASE -- that is a property of the machine that recorded it. A note-off is
 * misplaced by up to one segment in EITHER direction, so a short segment scores
 * better partly by under-applying the mechanism rather than by being right.
 *
 * WHAT IS STILL WRONG AT 512, and unexplained: 08_reverb +9.45 dB and
 * 03_velocity +7.10 against the one-pole, with nothing else above +3.04. The
 * reap fix changed both by exactly zero, so it is not that. The items that
 * regress with segment length are decay-dominated -- those two plus
 * 04_envelope +1.55 and 41_sustain_decay_curve +1.81 -- which points at the
 * chord: we render a decay segment as a straight line between its endpoints,
 * and a chord across 23 ms of a fast geometric decay sits measurably above the
 * curve. If that is the whole story then the reference's effective segment is
 * finer than probe 45's 23.5 ms and one of the two readings is wrong. Not
 * settled. Chase it before moving this constant again -- and do not re-fit it
 * against the corpus and call the result a measurement.
 *
 * Defaults to SERVICE_BLOCK_FRAMES (voice.h) rather than carrying its own
 * number: it is the same buffer, and the sweep moved them together.
 *
 * ponytail: no separate knob for the driver's own 50 ms cap -- at every length
 * in that sweep the block ends the segment first, so the cap would be dead
 * code. Add it if the block ever grows past 1103 frames. */
#ifndef GAIN_SEGMENT_FRAMES
#define GAIN_SEGMENT_FRAMES SERVICE_BLOCK_FRAMES
#endif

/* SUPERSEDED 2026-07-28, kept only as the record of what this replaced.
 * [F:fitted] -- Per-sample one-pole gain smoothing, SPEC.adoc S6.6: Part 6's own reverse
 * engineering confirms gain is NOT applied as an instant per-block jump --
 * "active phase step are each held in a coarse ramp accumulator ... a
 * linear-ramp smoothing mechanism operating on [gain]" (S6.6). The exact
 * ramp_period is marked [O] (its caller lives outside every examined PAGE
 * range), so this is a from-scratch one-pole low-pass standing in for that
 * confirmed-to-exist, unrecovered ramp, tuned against real reference audio
 * rather than derived from a recovered constant. See FITTED.md Entry 4.
 *
 * Without this, voice_update_gain's live per-block CC7/CC11 read (S3.5/
 * S3.10) applies a controller value the instant its MIDI message is
 * dispatched. GENERAL_SERUM channels 1-2's own Expression (CC11) automation
 * contains genuine literal-0 messages lasting only ~7-11ms (a fast
 * tremolo/portamento curve authored as discrete CC steps) before the next
 * value arrives; g_table_vel[0] is -96dB, so an instant jump to that value
 * truncates every output sample in that window to exact 0 (measured:
 * 162/244-sample all-zero runs, chopping an otherwise-correct, smoothly
 * decaying sustained chord into fragments -- reference audio (not retained) has
 * none). GAIN_SMOOTH_ALPHA = 1 - exp(-1/(0.012 * BASE_RATE)): a 12ms
 * one-pole time constant, long enough to absorb any single ~7-11ms blip
 * without ever reaching the far end of it, short enough not to measurably
 * soften real note-level dynamics (attack/decay/release already have their
 * own, separately-modeled multi-ms-to-multi-second time constants).
 * Verified: eliminates the 162/244-sample zero runs in the GENERAL_SERUM
 * channels 1-2 excerpt without moving the 18-probe corpus mean by any
 * measurable amount (see report). See FITTED.md Entry 4.
 *
 * ponytail: rate-scaled by division, not recomputed -- alpha ~= 1/(tau*R) for
 * small alpha, so this is exact to 0.14% at factor 2 (tau 11.972 -> 11.983ms),
 * inside the 0.23% the literal already sits off its own stated formula (exact
 * at BASE_RATE would be 0.003772156967). Dividing also keeps factor 1
 * bit-identical to the build this constant was verified in, which recomputing
 * would not. Recompute as 1 - exp(-1/(0.012 * RENDER_RATE)) if tau is ever
 * re-fit against a reference.
 *
 * The one-pole stood in for a ramp SPEC.adoc said existed but whose mechanism was
 * unrecovered. The mechanism is now recovered (GAIN_SEGMENT_FRAMES above), so
 * the stand-in is gone. Its stated job -- keeping GENERAL_SERUM's ~7-11 ms
 * literal-0 CC11 blips from truncating the output to exact zero -- the real
 * thing does better: a controller value is not read at all until the segment
 * boundary, so a blip shorter than one segment cannot reach the output even in
 * principle. */

/* Sub-block modulation granularity, SPEC.md LFO section `[M: probe 06]`:
 * render_frames is otherwise called once per MIDI-event-free chunk (smf.c
 * only splits chunks at dispatched events), so a held note with no events
 * for several seconds would get exactly one voices_update_modulation() call
 * for that whole span -- fine for pitch bend/CC7/CC11 (they don't move
 * without an event either), but fatal for a continuously-oscillating pitch
 * LFO: the vibrato would be a single frozen offset per chunk, not an
 * oscillation. LFO_UPDATE_FRAMES re-slices every render_frames call into
 * ~2.9ms sub-chunks (64 frames @ 22050Hz, comfortably above a 6Hz LFO's own
 * Nyquist-ish update-rate need) so voices_update_modulation() -- and the LFO
 * phase advance -- run at that cadence regardless of how long the
 * surrounding MIDI-event-free span is. render_voice's state (phase_pos) is
 * unaffected by the slicing: N one-shot calls of length k are equivalent to
 * one call of length N*k except that modulation now refreshes between
 * them, so gain smoothing and the GAIN_CEILING clamp keep working exactly
 * as before, just with finer-grained inputs. Defined in voice.h since
 * voice.c's EG2 stepping shares this exact cadence. */

static int16_t sat_add_i16(int32_t a, int32_t b) {
    int32_t s = a + b;
    if (s > 32767) return 32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}

static void render_voice(Voice *v, int16_t *out, uint32_t frames, uint32_t block_left) {
    /* Clip the voice into this call the way the driver clips it into the
     * buffer: a note-on dispatched at the block top still starts on its own
     * sample. See Voice::start_delay. */
    if (v->start_delay) {
        if (v->start_delay >= frames) { v->start_delay -= frames; return; }
        out += (uint32_t)v->start_delay * 2u;
        frames -= v->start_delay;
        block_left -= v->start_delay;
        v->start_delay = 0;
    }
    uint32_t sample_end_q12 = (uint32_t)v->sample_end_s << 12;
    uint32_t loop_len_q12 = (uint32_t)v->loop_len_s << 12;
    int32_t sample_count = v->wave->sample_count;
    const int16_t *samples = v->wave->samples;

    for (uint32_t i = 0; i < frames && v->active; i++) {
        if (v->amp_left == 0) {
            /* Re-aim: start a new amplitude segment. SPEC.adoc S6.6 /
             * `[A:0x18fba]`. The envelope is advanced across the WHOLE segment
             * first and its value at the far end is the ramp target -- the
             * driver evaluates it as a closed-form function of the segment's
             * end time (`[A:0x19720]`), and ours is incremental, so stepping it
             * ahead is the same thing. That look-ahead is the mechanism, not a
             * shortcut: it is why a note-off dispatched into an already-running
             * segment is not heard until the segment's start. */
            uint32_t seg = GAIN_SEGMENT_FRAMES;
            uint32_t next = voice_env_frames_to_change(v);
            if (next < seg) seg = next;
            /* ...and never past the end of this service block, the way the
             * driver's segment is clamped to the buffer end `[A:0x196de]`.
             * Without this a segment shortened by an attack boundary knocks
             * every later one out of block alignment, and the envelope
             * look-ahead then runs past events that have not been dispatched
             * yet. */
            if (block_left < seg) seg = block_left;
            if (seg == 0) seg = 1;

            double env_end = v->env_level;
            for (uint32_t k = 0; k < seg; k++) env_end = voice_step_envelope(v);
            /* voice_step_envelope clears `active` the moment the envelope hits
             * its finish floor. Hold the voice open for the rest of the segment
             * so the ramp it is already committed to actually lands on zero --
             * that landing IS the reference's straight-line release. */
            v->amp_retire = !v->active;
            v->active = 1;

            v->gain_l = v->gain_l_target;
            v->gain_r = v->gain_r_target;
            v->amp_step_l = (env_end * v->gain_l - v->amp_l) / (double)seg;
            v->amp_step_r = (env_end * v->gain_r - v->amp_r) / (double)seg;
            v->amp_left = seg;
        }

        /* SPEC.md S6.6/S6.4.1: phase step is a ramp accumulator, not a
         * direct write (see voice_update_pitch / RAMP_HORIZON_FRAMES in
         * voice.c for the measurement behind the fixed-duration linear slew
         * this applies one sample at a time). Advance it before use so
         * the very first sample after a target change already reflects one
         * step, matching the gain smoother's ordering just below. */
        if (v->phase_step_ramp_step) {
            /* Apply the held slope. It is in 1/256 phase_step LSBs: a whole-LSB
             * slope cannot express this ramp at all below about a semitone (a
             * +-2 semitone bend at key 72 moves ~420 Q12 LSBs, under 1 LSB per
             * sample), and truncating it to 1 silently turns the ramp into a
             * rate limit -- measured, that made the period inert from 10ms to
             * 40ms on probe 33's +-2 conditions. Accumulate and spend whole
             * LSBs, keep the remainder.
             *
             * No arrival test here: voice_ramp_tick owns that, via a per-voice
             * frame countdown rather than by watching phase_step approach its
             * target -- see voice.c. Nothing here clamps to the target either;
             * overshoot before the countdown fires is bounded by one rounding
             * step, since the slope was sized to land exactly on arrival. */
            v->phase_step_ramp_acc += v->phase_step_ramp_step;
            int32_t step = v->phase_step_ramp_acc >> 8; /* arithmetic, signed */
            v->phase_step_ramp_acc -= step << 8;
            int64_t next = (int64_t)v->phase_step + step;
            if (next < 0) next = 0; /* phase_step is unsigned; a big downward
                slope must not wrap it to ~4 billion */
            v->phase_step = (uint32_t)next;
        }

        uint32_t pos = v->phase_pos;
        int32_t idx = (int32_t)(pos >> 12);
        int32_t frac = (int32_t)(pos & 0xFFFu);

        if (idx >= sample_count) idx = sample_count > 0 ? sample_count - 1 : 0;
        if (idx < 0) idx = 0;
        int32_t idx1 = (idx + 1 < sample_count) ? idx + 1 : idx;

        int32_t tap0 = samples ? samples[idx] : 0;
        int32_t tap1 = samples ? samples[idx1] : 0;
        int32_t interp = (tap0 * (4096 - frac) + tap1 * frac) >> 12;

        /* One linear step of the segment's amplitude ramp. Envelope and gain
         * are NOT applied separately here: the driver sums them into a single
         * attenuation, converts that to linear once (`[A:0x190dc]`), and ramps
         * the product -- which is why its short releases are straight lines in
         * amplitude rather than the envelope's own curve. */
        v->amp_l += v->amp_step_l;
        v->amp_r += v->amp_step_r;
        int32_t out_l = (int32_t)((double)interp * v->amp_l);
        int32_t out_r = (int32_t)((double)interp * v->amp_r);

        out[i * 2 + 0] = sat_add_i16(out[i * 2 + 0], out_l);
        out[i * 2 + 1] = sat_add_i16(out[i * 2 + 1], out_r);

        if (--v->amp_left == 0 && v->amp_retire) {
            v->active = 0;
            v->amp_retire = 0;
        }

        pos += v->phase_step;
        if (pos >= sample_end_q12) {
            if (loop_len_q12 == 0) {
                v->active = 0; /* one-shot end: stop producing (S6.4.8) */
            } else {
                pos -= loop_len_q12;
            }
        }
        v->phase_pos = pos;
    }
}

void render_frames(int16_t *out, uint32_t frames) {
    /* Per-block modulation recompute: pitch (bend, SPEC.md S4.4; LFO,
     * SPEC.md LFO section `[M: probe 06]`) and gain (CC7 volume/CC11
     * expression/CC10 pan/master volume, SPEC.md S3.5/S3.6/S3.10) both
     * live-read every sub-chunk so a controller change reaches notes
     * already sounding, not just new note-ons. render_frames is the single
     * call site both smf_render's per-chunk playback and its
     * no-SMF-loaded/direct-msgs_midi path route through; smf.c splits
     * chunks at every dispatched event (0xE0 pitch bend, 0xB0 CC1/7/10/11),
     * and LFO_UPDATE_FRAMES (above) additionally re-slices each of those
     * chunks so a long, event-free held note still gets frequent-enough
     * modulation updates for its LFO to actually oscillate. */
    for (uint32_t i = 0; i < frames * 2; i++) out[i] = 0;
    uint32_t done = 0;
    while (done < frames) {
        uint32_t chunk = frames - done;
        if (chunk > LFO_UPDATE_FRAMES) chunk = LFO_UPDATE_FRAMES;
        /* SPEC.md S5.4: the reserve top-up runs once per audio service tick
           -- a wall-clock period, NOT once per call into this function.
           smf.c splits render_frames() at every dispatched MIDI event, so a
           per-call cadence runs at MIDI event density (measured: ~800/s on a
           dense field MIDI) and cascades Branch B into total silence. The
           tick clock lives in voice.c; feed it elapsed frames. */
        voice_topup_tick(chunk);
        voices_update_modulation();
        for (int vi = 0; vi < NUM_VOICES; vi++) {
            Voice *v = &g_voices[vi];
            if (v->active && v->wave) {
                render_voice(v, out + (uint32_t)done * 2, chunk, frames - done);
            }
        }
        voice_ramp_tick(chunk); /* retire pitch ramps whose horizon elapsed
            THIS sub-chunk -- after render_voice, so every frame the ramp
            owned was actually rendered with it before it's cleared */
        voices_advance_lfo(chunk);
        done += chunk;
    }
}
