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

/* [F:fitted] -- Per-sample one-pole gain smoothing, SPEC.md S6.6: Part 6's own reverse
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
 * re-fit against a reference. */
#ifndef GAIN_SMOOTH_ALPHA
#define GAIN_SMOOTH_ALPHA (0.003780968318281238 / RESAMPLE_FACTOR)
#endif

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

static void render_voice(Voice *v, int16_t *out, uint32_t frames) {
    uint32_t sample_end_q12 = (uint32_t)v->sample_end_s << 12;
    uint32_t loop_len_q12 = (uint32_t)v->loop_len_s << 12;
    int32_t sample_count = v->wave->sample_count;
    const int16_t *samples = v->wave->samples;

    for (uint32_t i = 0; i < frames && v->active; i++) {
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

        double env = voice_step_envelope(v);
        double sample = (double)interp * env;

        v->gain_l += (v->gain_l_target - v->gain_l) * GAIN_SMOOTH_ALPHA;
        v->gain_r += (v->gain_r_target - v->gain_r) * GAIN_SMOOTH_ALPHA;
        int32_t out_l = (int32_t)(sample * v->gain_l);
        int32_t out_r = (int32_t)(sample * v->gain_r);

        out[i * 2 + 0] = sat_add_i16(out[i * 2 + 0], out_l);
        out[i * 2 + 1] = sat_add_i16(out[i * 2 + 1], out_r);

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
                render_voice(v, out + (uint32_t)done * 2, chunk);
            }
        }
        voice_ramp_tick(chunk); /* retire pitch ramps whose horizon elapsed
            THIS sub-chunk -- after render_voice, so every frame the ramp
            owned was actually rendered with it before it's cleared */
        voices_advance_lfo(chunk);
        done += chunk;
    }
}
