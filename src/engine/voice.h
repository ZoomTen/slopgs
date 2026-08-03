/* voice.h -- voice pool, allocation, stealing, key-group choke, per-voice parameters, envelopes. SPEC.adoc Part 5 (pool/steal/choke) + Part 3. */
#ifndef VOICE_H
#define VOICE_H

#include <stdint.h>
#include "dls.h"

#define BASE_RATE 22050 /* every fitted constant here is measured against it; RESAMPLE_FACTOR ships at 1, see invariant below */
#ifndef RESAMPLE_FACTOR
#define RESAMPLE_FACTOR 1
#endif
#define RENDER_RATE (BASE_RATE * RESAMPLE_FACTOR)

#define LFO_UPDATE_FRAMES (64 * RESAMPLE_FACTOR) /* shared modulation cadence for render.c's LFO and voice.c's EG2 stepping */

#ifndef SERVICE_BLOCK_FRAMES
#define SERVICE_BLOCK_FRAMES (256u * RESAMPLE_FACTOR) /* audio service block: one clock for re-aim/gain ramp/reserve top-up, see render.c's GAIN_SEGMENT_FRAMES */
#endif

/* RESAMPLE_FACTOR must stay an INTEGER multiple of BASE_RATE: a non-integer ratio silently detunes RENDER_RATE, and a non-multiple render rate loses probe 02's real, audible synthesis-rate aliasing (keys 125-127); the _Static_assert below guards it. */
_Static_assert(RENDER_RATE % BASE_RATE == 0 && RENDER_RATE >= BASE_RATE,
    "RESAMPLE_FACTOR must be a positive integer (internal-rate invariant)");

#define NUM_VOICES 54 /* 48 primary + 6 reserve, SPEC.adoc S5.2, one flat tagged pool -- SPEC_LOG item 7 */

typedef enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_IDLE } EnvStage;

typedef struct Voice {
    int active;         /* producing sound */
    int channel;
    int note;
    uint32_t locale;    /* resolved instrument locale, for key-group scope match */
    uint8_t key_group;
    Wave *wave;
    Artic *artic;

    uint32_t phase_pos;   /* Q12: bits31:12 index, bits11:0 fraction */
    uint32_t phase_step;  /* Q12 per-output-sample increment; ramped towards phase_step_target one sample at a time by render.c's render_voice, never written directly outside voice_note_on's snap and that ramp step, SPEC.adoc S6.6 */
    int32_t  bend_cents_applied; /* bend the in-flight ramp is aimed at; LFO/EG2 changes are carried through instantly instead */
    uint32_t phase_step_target;  /* live target voice_update_pitch recomputes every block (bend/LFO/EG2); phase_step glides toward this, mirroring gain_l/gain_l_target */
    int32_t  phase_step_ramp_step; /* HELD per-sample slope towards phase_step_target, in 1/256 phase_step LSBs (whole LSBs too coarse below ~a semitone); sized by ramp_reaim over a fixed horizon, held until ramp_left expires, SPEC.adoc S6.6 */
    int32_t  phase_step_ramp_acc;  /* leftover 1/256ths from the slope above, carried sample to sample by render_voice */
    uint32_t ramp_left;    /* frames left on the current ramp; 0 = settled. voice_ramp_tick snaps phase_step to target on expiry */
    uint32_t base_ratio_q12; /* note-on base pitch ratio, clamped once at note-on (incl. latched RPN1/RPN2); live bend/LFO multiply this OUTSIDE the clamp -- `[M: probe 30]` */
    int32_t base_cents;   /* fine_tune + (note-unity_note)*100: fixed for the voice's life; live modulation added on top each block by voice_update_pitch */
    int32_t loop_start_s, loop_len_s; /* samples; loop_len_s==0 => one-shot */
    int32_t sample_end_s;             /* one-shot end / loop end, samples */

    uint32_t start_delay; /* frames before this voice produces anything, so a note-on lands on its exact sample despite being dispatched at the buffer top */

    double gain_l, gain_r; /* gain in force for the CURRENT amplitude segment: gain_*_target sampled at the segment boundary and held for its duration `[A:0x19490]` */

    uint32_t amp_left;            /* frames left in this segment; 0 = re-aim. Amplitude ramp segment: envelope x gain sampled once per segment END, ramped linearly, SPEC.adoc S6.6 / `[A:0x18fba]` / SPEC_LOG item 21 */
    double amp_l, amp_r;          /* applied amplitude, envelope x gain */
    double amp_step_l, amp_step_r;/* per-frame increment towards the target */
    int amp_retire;               /* envelope finished inside this segment; voice dies at the segment's end, not the envelope's instant, so the committed ramp lands */
    double gain_l_target, gain_r_target; /* live target gain, recomputed every block by voice_update_gain from atten_const_hdb plus current channel vol/expr/pan/master vol */
    int32_t atten_const_hdb; /* velocity + region/wsmp attenuation: fixed for the voice's life (SPEC.adoc S3.5/S3.10); CC-driven terms NOT baked in here, see voice_update_gain */

    EnvStage env_stage;
    double env_level;         /* 0..1 amplitude envelope multiplier */
    double env_attack_step;   /* linear increment per sample during attack */
    int32_t env_attack_elapsed; /* ENV_ATTACK_TABLE_C only: samples advanced so far into the attack segment */
    int32_t env_attack_samples; /* ENV_ATTACK_TABLE_C only: attack segment length in samples (>=1 whenever env_stage==ENV_ATTACK) */
    double env_decay_coef;    /* SPEC.adoc S5.1.2.1: geometric per-sample ratio toward env_sustain_level, terminated exactly by env_decay_samples_left, not by this coefficient asymptoting */
    double env_release_coef;  /* multiplicative approach-to-zero coefficient */
    double env_sustain_level; /* 0..1 */
    int32_t env_decay_samples_left; /* SPEC.adoc S5.1.2.1: exact countdown so the decay ramp snaps to env_sustain_level at the rescaled duration rather than a threshold test. 0 while not in ENV_DECAY. */

    EnvStage eg2_stage; /* EG2, the PITCH envelope. SPEC.adoc S2.4.3/S2.4.4 [A:0x15838]: EG2 to any destination other than PITCH is ignored */
    double eg2_level;          /* 0..1 */
    double eg2_attack_step;    /* linear increment per modulation block */
    double eg2_decay_coef;     /* multiplicative approach-to-sustain, per block */
    double eg2_release_coef;   /* multiplicative approach-to-zero, per block */
    double eg2_sustain_level;  /* 0..1 */
    double eg2_depth_cents;    /* connection scale; 0 = no EG2->pitch (the default) */

    int held;             /* note not yet released (CC64/CC120/CC123 semantics) */
    int sustain_deferred;  /* CC64 held down at note-off time */
    uint32_t age;          /* allocation sequence number, for oldest-first steal */

    int in_reserve;             /* SPEC.adoc S5.2/S5.3/S5.4 [A]: 1 while this voice is a FREE node tagged reserve-tier; meaningless while active==1. Recycling always clears it to 0 (primary). See voice_topup_reserve. */
    int fast_release_committed; /* SPEC.adoc S5.6 +0x138 [A]: 1 once committed to a fast release by choke, retrigger, or top-up Branch B; gates find_steal_candidate_symmetric so the same voice isn't re-marked mid-drain. Cleared at voice_note_on. */

    double lfo_phase; /* Pitch LFO (vibrato), SPEC.adoc LFO section `[M: probe 06]`; rate/delay cached at note-on from this voice's own artic, SPEC_LOG entry 6 */
    double lfo_freq_hz;
    double lfo_delay_s;
    double lfo_elapsed_s; /* seconds since note-on; phase stays 0 until this exceeds lfo_delay_s */
} Voice;

extern Voice g_voices[NUM_VOICES];
extern uint32_t g_voice_age_counter;

void voice_pool_reset(void);
void voice_note_on(int channel, int note, int velocity);

void voice_topup_tick(uint32_t frames); /* SPEC.adoc S5.4: tops the reserve via Branch A (retag) then Branch B (fast-release), on a wall-clock tick -- SPEC_LOG item 7 / entry 14 */
void voice_note_off(int channel, int note);
void voice_all_sound_off(int channel);   /* CC120: bypasses sustain hold */
void voice_all_notes_off(int channel);   /* CC123: honours sustain hold */
void voice_sustain_lift(int channel);    /* CC64 released: release deferred voices */

double voice_step_envelope(Voice *v);

void voice_set_event_offset(uint32_t frames);

#define ENV_NO_CHANGE 0xFFFFFFFFu /* "no scheduled boundary" sentinel for voice_env_frames_to_change, below */
uint32_t voice_env_frames_to_change(const Voice *v);

int voice_any_active(void);

void voice_update_pitch(Voice *v); /* recomputes v->phase_step from v->base_cents plus every live pitch source (bend, LFO, EG2) -- the ONLY place phase_step is computed */

void voices_update_modulation(void);

void voice_ramp_tick(uint32_t frames); /* retire pitch ramps whose horizon has elapsed; call once per rendered sub-chunk, AFTER render_voice has actually rendered those frames */
void voices_advance_lfo(uint32_t frames);

void voice_update_gain(Voice *v); /* recomputes gain_l/gain_r from atten_const_hdb plus live master/CC7/CC11/CC10 (SPEC.adoc S3.5/S3.6/S3.10) -- the ONLY place gain is computed */

#endif /* VOICE_H */
