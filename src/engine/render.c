/* render.c -- the mixer: interpolation, gain, saturating accumulate. SPEC.adoc Part 6 (S6.4.2 Q12 phase, S6.4.4/S6.4.5 interpolate-then-gain, S6.4.6 saturating accumulate, S6.4.8 loop-end subtraction). */
#include "render.h"
#include "voice.h"

#ifndef GAIN_SEGMENT_FRAMES
#define GAIN_SEGMENT_FRAMES SERVICE_BLOCK_FRAMES /* SPEC.adoc S6.6/[A:0x18fba]: min(next envelope change, device->+0x18, buffer end), gain sampled at the segment END -- corpus sweep table and mechanism at SPEC_LOG item 21 (item57 addendum). ponytail: no separate knob for the driver's 50ms cap -- dead at every swept length; add if the block ever exceeds 1103 frames. */
#endif

static int16_t sat_add_i16(int32_t a, int32_t b) {
    int32_t s = a + b;
    if (s > 32767) return 32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}

static void render_voice(Voice *v, int16_t *out, uint32_t frames, uint32_t block_left) {
    if (v->start_delay) { /* clip the voice into this call the way the driver clips it: a note-on dispatched at the block top still starts on its own sample */
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
            uint32_t seg = GAIN_SEGMENT_FRAMES;
            uint32_t next = voice_env_frames_to_change(v);
            if (next < seg) seg = next;
            if (block_left < seg) seg = block_left; /* ...and never past this service block's end [A:0x196de], DSP-advance region [A:0x196cd]-[A:0x196f0]: a segment that ran past it would outrun undispatched events. */
            if (seg == 0) seg = 1;

            double env_end = v->env_level;
            for (uint32_t k = 0; k < seg; k++) env_end = voice_step_envelope(v);
            v->amp_retire = !v->active;
            v->active = 1;

            v->gain_l = v->gain_l_target;
            v->gain_r = v->gain_r_target;
            v->amp_step_l = (env_end * v->gain_l - v->amp_l) / (double)seg;
            v->amp_step_r = (env_end * v->gain_r - v->amp_r) / (double)seg;
            v->amp_left = seg;
        }

        if (v->phase_step_ramp_step) { /* phase step is a ramp accumulator (SPEC.adoc S6.6/S6.4.1, SPEC_LOG entry 9), advanced before use so sample 0 already reflects one step */
            v->phase_step_ramp_acc += v->phase_step_ramp_step; /* 1/256 phase_step LSB precision required below ~a semitone; whole-LSB truncation measured to make the ramp inert -- SPEC_LOG item52 */
            int32_t step = v->phase_step_ramp_acc >> 8; /* arithmetic, signed */
            v->phase_step_ramp_acc -= step << 8;
            int64_t next = (int64_t)v->phase_step + step;
            if (next < 0) next = 0; /* phase_step is unsigned; a big downward slope must not wrap it to ~4 billion */
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

        v->amp_l += v->amp_step_l; /* one ramp step of the segment's amplitude; envelope x gain is summed to ONE attenuation and ramped as a single product [A:0x190dc], item 21 */
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
    for (uint32_t i = 0; i < frames * 2; i++) out[i] = 0;
    uint32_t done = 0;
    while (done < frames) {
        uint32_t chunk = frames - done;
        if (chunk > LFO_UPDATE_FRAMES) chunk = LFO_UPDATE_FRAMES; /* sub-block modulation granularity: re-slice into LFO_UPDATE_FRAMES sub-chunks so a long event-free held note's LFO still oscillates instead of freezing at one offset -- SPEC_LOG entry 2 */
        voice_topup_tick(chunk); /* SPEC.adoc S5.4: reserve top-up runs once per wall-clock service tick, NOT once per render_frames() call (smf.c splits at every event, ~800/s measured) -- SPEC_LOG entry 14 */
        voices_update_modulation(); /* per-block modulation recompute (pitch S4.4/LFO, gain S3.5/S3.6/S3.10): live-read every sub-chunk so a change reaches already-sounding notes */
        for (int vi = 0; vi < NUM_VOICES; vi++) {
            Voice *v = &g_voices[vi];
            if (v->active && v->wave) {
                render_voice(v, out + (uint32_t)done * 2, chunk, frames - done);
            }
        }
        voice_ramp_tick(chunk); /* retire pitch ramps whose horizon elapsed THIS sub-chunk -- after render_voice, so every frame the ramp owned was actually rendered with it before it's cleared */
        voices_advance_lfo(chunk);
        done += chunk;
    }
}
