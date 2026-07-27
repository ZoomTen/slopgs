/* voice.h -- voice pool, allocation, stealing, key-group choke, per-voice
 * parameters, envelopes. SPEC.md Part 5 (pool/steal/choke) + Part 3
 * (per-voice parameter computation: pitch, envelopes, volume law, pan). */
#ifndef VOICE_H
#define VOICE_H

#include <stdint.h>
#include "dls.h"

/* BASE_RATE is the driver's own rate (SPEC.md S1.3/S6.1: single fixed rate,
 * not a range), and is the rate every fitted constant in this engine was
 * measured against. RESAMPLE_FACTOR is how many times finer we render than
 * that; every rate-derived constant scales by it, so changing the engine's
 * rate is this one define and nothing else. Overridable so a rate A/B is one
 * -D rather than an edit. Ships at 1: oversampling is a deliberate deviation
 * from the driver (see the invariant below), not a free quality win. */
#define BASE_RATE 22050
#ifndef RESAMPLE_FACTOR
#define RESAMPLE_FACTOR 1
#endif
#define RENDER_RATE (BASE_RATE * RESAMPLE_FACTOR)

/* Sub-block modulation granularity shared by render.c's LFO stepping and
 * voice.c's EG2 stepping -- both must advance modulation at the same
 * cadence, so this is the one definition both include instead of two
 * separately-maintained copies. See render.c's render_frames for why this
 * cadence exists at all. */
#define LFO_UPDATE_FRAMES (64 * RESAMPLE_FACTOR)

/* RESAMPLE_FACTOR is an INTEGER multiplier, never a rate ratio. Writing
 * ((double)48000/(double)BASE_RATE) here does not give you a 48kHz engine, it
 * gives you a silently detuned one: RENDER_RATE becomes a double, every
 * *_FRAMES macro truncates on assignment, and voice_update_pitch's phase-step
 * division stops being integer division (abandoning the floor semantics
 * c27d12e pinned down). -Wall -Wextra warns on none of it. The `%` below is
 * the guard -- a double operand is a hard compile error, not a warning.
 *
 * For a 48kHz *device*, resample at the output stage; nothing in here moves.
 * 48000 is not a multiple of BASE_RATE at any factor (gcd 150, so the ratio
 * is 320/147 from 22050 and still x/147 from any multiple of it), so there is
 * no factor that turns the output resample into a clean one. */
_Static_assert(RENDER_RATE % BASE_RATE == 0 && RENDER_RATE >= BASE_RATE,
    "RESAMPLE_FACTOR must be a positive integer (internal-rate invariant)");

/* INVARIANT: an integer multiple of BASE_RATE. NEVER set to the
 * device/AudioContext rate when that is not a BASE_RATE multiple -- synthesis-
 * rate aliasing (probe 02 keys 125-127, the faffaee reference) is real,
 * audible, spec-relevant output; a non-multiple render rate smears the
 * fold into noise. Render at BASE_RATE and resample at the output stage. See
 * SPEC.md verification-ceiling / internal-rate invariant.
 *
 * Raising RESAMPLE_FACTOR is itself a fidelity LOSS, not a gain: at factor 2
 * the Nyquist moves to 22050 and the 11025Hz fold probe 02 exists to capture
 * stops happening at all. The references have it; an oversampled render will
 * not. */

#define NUM_VOICES 54 /* 48 primary + 6 reserve, SPEC.md S5.2 -- implemented
                         here as one flat pool; see SPEC_GAPS.md for the
                         48/6-split reserve-topup mechanic this simplifies
                         away. */

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
    uint32_t phase_step;  /* Q12 per-output-sample increment -- the CURRENTLY-
                             APPLIED value, ramped towards phase_step_target
                             one sample at a time by render.c's render_voice
                             (see PITCH_RAMP_MS in voice.c);
                             never written directly outside voice_note_on's
                             initial snap and that ramp step, SPEC.md S6.6 */
    int32_t  bend_cents_applied; /* bend the in-flight ramp is aimed at; LFO and
                             EG2 changes are carried through instantly instead */
    uint32_t phase_step_target;  /* live target voice_update_pitch recomputes
                             every block (bend/LFO/EG2); phase_step glides
                             toward this instead of jumping, mirroring
                             voice.h's gain_l/gain_l_target split */
    int32_t  phase_step_ramp_step; /* HELD per-sample slope towards
                             phase_step_target, in 1/256 phase_step LSBs (whole
                             LSBs are too coarse below ~a semitone). Sized once
                             by ramp_reaim when a bend moves the target, over a
                             FIXED horizon private to this voice -- not a
                             shared clock's period -- and held constant until
                             ramp_left (below) expires, SPEC.md S6.6 */
    int32_t  phase_step_ramp_acc;  /* leftover 1/256ths from the slope above,
                             carried sample to sample by render_voice */
    uint32_t ramp_left;    /* frames left on the current ramp; 0 = settled.
                             voice_ramp_tick counts this down and snaps
                             phase_step to its target on expiry -- render.c's
                             per-sample accumulator has no arrival test */
    uint32_t base_ratio_q12; /* note-on base pitch ratio, clamped once at note-on
                               (incl. latched RPN1/RPN2); live bend/LFO multiply
                               this OUTSIDE the clamp -- `[M: probe 30]` */
    int32_t base_cents;   /* fine_tune + (note-unity_note)*100: fixed for the
                              voice's life; live modulation (bend/LFO/EG2) is
                              added on top each block by voice_update_pitch */
    int32_t loop_start_s, loop_len_s; /* samples; loop_len_s==0 => one-shot */
    int32_t sample_end_s;             /* one-shot end / loop end, samples */

    /* Frames this voice must wait before it produces anything, counted from the
     * start of the render call it was created in. The driver dispatches a whole
     * buffer's events before rendering a sample of it, but each voice carries
     * its own start timestamp and the renderer clips it into the buffer
     * (`[A:0x19653]`-`[A:0x1967d]`) -- so a note-on lands on its exact sample
     * even though its EVENT was handled at the buffer top. This is that clip.
     * Without it note-ons come out as early as note-offs, which the reference
     * plainly does not do. */
    uint32_t start_delay;

    double gain_l, gain_r; /* gain in force for the CURRENT amplitude segment:
                               gain_*_target sampled at the segment boundary and
                               held for its duration, since the driver's mixer
                               only re-reads the summed attenuation when it
                               re-aims a ramp `[A:0x19490]`. Read by
                               voice_step_envelope's audible-level finish test */

    /* Amplitude ramp segment, SPEC.adoc S6.6 / `[A:0x18fba]`. The driver samples
     * envelope x gain ONCE, at the END of each segment, and the mixer ramps the
     * applied amplitude linearly (in the amplitude domain, `[A:0x190dc]`) from
     * the previous segment's value to it. Everything the release-shape work
     * chased falls out of that: a release shorter than one segment renders as a
     * single straight line to zero, a longer one as a chain of linear chords of
     * its geometric curve, and a note-off landing mid-segment is not heard until
     * the segment START -- early, which is what probe 46 measures. See
     * SPEC_GAPS.adoc item 21's resolution. */
    uint32_t amp_left;            /* frames left in this segment; 0 = re-aim */
    double amp_l, amp_r;          /* applied amplitude, envelope x gain */
    double amp_step_l, amp_step_r;/* per-frame increment towards the target */
    int amp_retire;               /* envelope finished inside this segment;
                                     the voice dies at the segment's end, not
                                     the instant the envelope did, so the ramp
                                     it is already committed to still lands */
    double gain_l_target, gain_r_target; /* live target gain, recomputed
                               every block by voice_update_gain from
                               atten_const_hdb plus current channel vol/
                               expr/pan/master vol */
    int32_t atten_const_hdb; /* velocity attenuation + region/wsmp attenuation:
                                 fixed for the voice's life (SPEC.md S3.5/
                                 S3.10); everything CC-driven is NOT baked in
                                 here, see voice_update_gain */

    EnvStage env_stage;
    double env_level;         /* 0..1 amplitude envelope multiplier */
    double env_attack_step;   /* linear increment per sample during attack */
    double env_decay_coef;    /* SPEC.md S5.1.2.1: geometric per-sample ratio
                                  toward env_sustain_level (env_level *=
                                  env_decay_coef), same mechanism as
                                  env_release_coef below, NOT an approach-to-
                                  target coefficient -- the ramp is terminated
                                  exactly by env_decay_samples_left, not by
                                  this coefficient asymptoting. */
    double env_release_coef;  /* multiplicative approach-to-zero coefficient */
    double env_sustain_level; /* 0..1 */
    int32_t env_decay_samples_left; /* SPEC.md S5.1.2.1: exact countdown so the
                                  decay ramp snaps to env_sustain_level AT the
                                  rescaled decay-to-sustain duration rather
                                  than relying on a threshold test (a nonzero-
                                  target geometric ramp never crosses a
                                  threshold the way an approach-to-zero one
                                  does). 0 while not in ENV_DECAY. */

    /* EG2, the PITCH envelope. SPEC.md S2.4.3 documents `(usSource=5 EG2,
     * usDestination=0x0003 PITCH)` as a real, dispatched connection
     * `[A:0x15838]`, and S2.4.4 confirms EG2 to any OTHER destination is
     * ignored -- so pitch is the one EG2 modulation this driver implements.
     * Same four-segment shape as EG1; stepped at the modulation sub-chunk
     * cadence rather than per sample (see voice_step_eg2). */
    EnvStage eg2_stage;
    double eg2_level;          /* 0..1 */
    double eg2_attack_step;    /* linear increment per modulation block */
    double eg2_decay_coef;     /* multiplicative approach-to-sustain, per block */
    double eg2_release_coef;   /* multiplicative approach-to-zero, per block */
    double eg2_sustain_level;  /* 0..1 */
    double eg2_depth_cents;    /* connection scale; 0 = no EG2->pitch (the default) */

    int held;             /* note not yet released (CC64/CC120/CC123 semantics) */
    int sustain_deferred;  /* CC64 held down at note-off time */
    uint32_t age;          /* allocation sequence number, for oldest-first steal */

    int in_reserve;             /* SPEC.md S5.2/S5.3/S5.4 [A]: 1 while this
                                    voice is a FREE (inactive) node currently
                                    tagged as belonging to the reserve tier;
                                    meaningless while active==1. Recycling
                                    always clears it back to 0 (primary) --
                                    S5.3 "both target primary only, never
                                    reserve". See voice_topup_reserve (voice.c). */
    int fast_release_committed; /* SPEC.md S5.6 +0x138 [A]: 1 once this voice
                                    has been committed to a fast release by
                                    ANY of the three fast-release callers
                                    (key-group choke, same-note retrigger, or
                                    the reserve top-up's Branch B). Sole
                                    purpose: gate eligibility in
                                    find_steal_candidate_symmetric so the same
                                    voice isn't re-marked by a later top-up
                                    call while still draining. Cleared at note
                                    setup (voice_note_on). */

    /* Pitch LFO (vibrato), SPEC.md LFO section `[M: probe 06]`. lfo_freq_hz/
     * lfo_delay_s are cached once at note-on from this voice's own artic
     * (region-specific, so different instruments vibrato at different
     * rates -- a per-instrument derivation, not a global constant).
     * lfo_phase is 0..1 cycles into g_table_sine, held fixed within one
     * render.c sub-chunk and advanced between sub-chunks by
     * voices_advance_lfo() so a held note actually oscillates instead of
     * freezing at one offset for an entire long chunk. */
    double lfo_phase;
    double lfo_freq_hz;
    double lfo_delay_s;
    double lfo_elapsed_s; /* seconds since note-on; phase stays 0 until this exceeds lfo_delay_s */
} Voice;

extern Voice g_voices[NUM_VOICES];
extern uint32_t g_voice_age_counter;

void voice_pool_reset(void);
void voice_note_on(int channel, int note, int velocity);

/* SPEC.md S5.4 [A] mechanism / [O] cadence: advances the top-up's own tick
 * clock by `frames` rendered samples and runs one top-up every
 * TOPUP_INTERVAL_FRAMES, standing in for the real driver's per-service-tick
 * (0x13054 -> 0x12bd6 -> 0x12b6a) cadence. The top-up tops the reserve tier
 * back up to TOPUP_RESERVE_COUNT free voices, first by retagging
 * already-free primary nodes (Branch A), then -- only if primary is ALSO
 * empty -- by proactively fast-releasing (never synchronously freeing) up to
 * that many active voices (Branch B). See voice.c's own comment above the
 * definition for why the cadence must be a wall-clock period and not
 * "once per render_frames() call". */
void voice_topup_tick(uint32_t frames);
void voice_note_off(int channel, int note);
void voice_all_sound_off(int channel);   /* CC120: bypasses sustain hold */
void voice_all_notes_off(int channel);   /* CC123: honours sustain hold */
void voice_sustain_lift(int channel);    /* CC64 released: release deferred voices */

/* Advances voice v's amplitude envelope by exactly one output sample
 * (1/22050 s) and returns the resulting 0..1 level. Deactivates the voice
 * (v->active = 0) once a release segment has fully decayed. Called from
 * render.c's per-sample mixer loop -- render.c owns sample fetch,
 * interpolation, and saturating accumulation; voice.c owns the envelope
 * state machine itself. */
double voice_step_envelope(Voice *v);

/* Set by smf.c while draining a service block: where inside the current
 * render call the event being dispatched falls. Only note-on consumes it. */
void voice_set_event_offset(uint32_t frames);

/* "No scheduled boundary" sentinel for voice_env_frames_to_change, below. */
#define ENV_NO_CHANGE 0xFFFFFFFFu
uint32_t voice_env_frames_to_change(const Voice *v);

/* 1 if any voice is currently producing sound, else 0 (used by smf.c to
 * decide when playback has fully finished, including release tails). */
int voice_any_active(void);

/* Recomputes v->phase_step from v->base_cents plus every live per-block
 * pitch modulation source (currently: pitch bend, SPEC.md S4.4; LFO and EG2
 * are wired in as zero-contributing hooks for the next steps). This is the
 * ONLY place phase_step is computed -- voice_note_on calls it too, so
 * note-on and per-block recompute can never drift apart. */
void voice_update_pitch(Voice *v);

/* Calls voice_update_pitch for every active voice. Called once per render
 * block (render_frames) so pitch modulation that changes between blocks --
 * e.g. a pitch-bend message dispatched by the sequencer between chunks --
 * reaches notes that are already sounding, not just newly-triggered ones. */
void voices_update_modulation(void);

/* Advances every active voice's pitch-LFO phase by `freq_hz * frames/RENDER_RATE`
 * cycles (wrapped to [0,1)), gated on each voice's own lfo_delay_s so the
 * oscillator's phase=0 start lines up with the end of the start-delay, not
 * note-on. Called by render.c once per sub-chunk (SPEC.md LFO section
 * `[M: probe 06]`) so a held note's vibrato actually oscillates instead of
 * freezing at one per-block value for a long event-free chunk. */
void voice_ramp_tick(uint32_t frames); /* retire pitch ramps whose horizon has
    elapsed; call once per rendered sub-chunk, AFTER render_voice has actually
    rendered those frames */
void voices_advance_lfo(uint32_t frames);

/* Recomputes v->gain_l/v->gain_r from v->atten_const_hdb (the fixed velocity+
 * region attenuation baked in at note-on) plus every live gain source: g_
 * master_vol_hdb and the owning channel's current volume(CC7)/expression
 * (CC11)/pan(CC10), SPEC.md S3.5 (squared volume law via g_table_vel), S3.10
 * (attenuation sum), S3.6 (pan law). This is the ONLY place gain_l/gain_r are
 * computed -- voice_note_on calls it too, mirroring voice_update_pitch. */
void voice_update_gain(Voice *v);

#endif /* VOICE_H */
