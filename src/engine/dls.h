/* dls.h -- RIFF/DLS parse of gm.dls into instruments, regions, waves.
 * SPEC.md Part 2. Structure field names are chosen for clarity in this
 * clean-room reimplementation; they are NOT a claim of matching the
 * original driver's in-memory byte offsets (which SPEC.md Part 2 S2.9
 * documents for the original binary only). */
#ifndef DLS_H
#define DLS_H

#include <stdint.h>

typedef struct Wave {
    uint32_t sample_rate;      /* fmt.nSamplesPerSec, wave scope */
    int16_t *samples;          /* referenced in place into the gm.dls buffer, NOT copied */
    int32_t sample_count;      /* data chunk size/2 for 16-bit PCM */
    int32_t loop_start;        /* wsmp loop start, wave scope, samples */
    int32_t loop_end;          /* wsmp loop start+length, wave scope, samples */
    int16_t fine_tune;         /* wsmp sFineTune, wave scope */
    int16_t attenuation_tenth_db; /* wsmp lAttenuation, (lScale*10)>>16, wave scope */
    uint8_t unity_note;        /* wsmp usUnityNote, byte-truncated, wave scope */
    uint8_t no_loop;           /* wsmp cSampleLoops==0, wave scope */
} Wave;

/* The 0x68-byte "resolved articulation block" of SPEC.md S3.2 -- distinct
 * from the Wave object (see SPEC_GAPS.md for the S2.9.2-vs-S3.2 field-
 * ownership contradiction this resolves). Private per-region, or a single
 * shared instrument-level default adopted (refcount-free here; we just
 * point multiple regions at the same Artic, since this arena never frees). */
typedef struct Artic {
    int32_t eg1_attack_tc, eg1_decay_tc, eg1_release_tc;  /* raw DLS timecents */
    int16_t eg1_sustain_permille;                          /* 0..1000 */
    int32_t eg2_attack_tc, eg2_decay_tc, eg2_release_tc;
    int16_t eg2_sustain_permille;
    int16_t eg2_to_pitch_cents;      /* src=5 -> dest 0x0003 */
    int16_t vel_to_atten_depth;      /* src=2 -> dest 0x0001, default -9600 */
    int16_t pan_cb;                  /* dest 0x0002/0x0004, small correction to CC10 pan */
    int32_t lfo_freq_tc, lfo_delay_tc; /* src=0 -> dest 0x0104/0x0105, timecents */
    int16_t lfo_pitch_inherent_cents; /* src=1(LFO), ctrl=0, dest=0x0003(PITCH):
                                          ungated ("always on") pitch-LFO depth,
                                          high word of lScale, clamped +-1200 */
    int16_t lfo_pitch_cc1_cents;      /* src=1(LFO), ctrl=0x0081(CC1/modwheel),
                                          dest=0x0003(PITCH): CC1-scaled pitch-LFO
                                          depth, same formula/clamp */
} Artic;

typedef struct Region {
    struct Region *next;
    Wave *wave;
    Artic *artic;
    int32_t loop_start, loop_end; /* region-scope wsmp override, samples */
    int16_t fine_tune;
    int16_t attenuation_tenth_db;
    uint8_t unity_note;
    uint8_t no_loop;
    uint8_t low_key, high_key;
    uint8_t key_group;
    uint16_t wave_pool_index;
    uint8_t has_own_wsmp; /* region carried its own wsmp chunk (S2.3.4) */
} Region;

typedef struct Instrument {
    struct Instrument *next;
    Region *first_region;
    uint32_t locale;   /* program | bankLSB<<7 | bankMSB<<14 | drum<<31 */
    int region_count;
} Instrument;

typedef struct DlsCollection {
    Instrument *first_instrument;
    Wave **wave_array;
    uint32_t wave_count;
    int valid;
} DlsCollection;

extern DlsCollection g_dls;

/* Parses `data`[0..len) as a DLS-1 RIFF collection in place. Returns 0 on
 * success, negative on a fatal parse error. Sample data is referenced
 * directly into `data`, never copied (SPEC.md S1.5.5). */
int dls_load(const uint8_t *data, uint32_t len);

/* Three-tier bank/program/drum fallback + region lookup, SPEC.md S3.1. */
Region *dls_find_region(uint32_t locale, uint8_t note);

#endif /* DLS_H */
