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

/* art1 connection-block usSource values that gm.dls/the driver actually use,
 * SPEC.md S2.4.3. Not the full DLS-1 source enumeration -- just the rows
 * apply_art1() (dls.c) switches on. */
enum ArtSource {
    ART_SRC_NONE           = 0x0000, /* unconditional (RPN-less) block */
    ART_SRC_LFO            = 0x0001,
    ART_SRC_KEYONVELOCITY  = 0x0002,
    ART_SRC_KEYNUMBER      = 0x0003,
    ART_SRC_EG2            = 0x0005,
};

/* art1 connection-block usDestination values, SPEC.md S2.4.3. ART_DST_PAN is
 * the DLS-1-spec pan slot; ART_DST_PAN_COARSE is the driver's own reuse of
 * the spec's reserved 0x0002 slot for the same pan_cb field (see apply_art1). */
enum ArtDest {
    ART_DST_ATTENUATION         = 0x0001,
    ART_DST_PAN_COARSE          = 0x0002,
    ART_DST_PITCH               = 0x0003,
    ART_DST_PAN                 = 0x0004,
    ART_DST_LFO_FREQUENCY       = 0x0104,
    ART_DST_LFO_DELAY           = 0x0105,
    ART_DST_EG1_ATTACKTIME      = 0x0206,
    ART_DST_EG1_DECAYTIME       = 0x0207,
    ART_DST_EG1_SUSTAINLEVEL_LO = 0x0208,
    ART_DST_EG1_RELEASETIME     = 0x0209,
    ART_DST_EG1_SUSTAINLEVEL_HI = 0x020a,
    ART_DST_EG2_ATTACKTIME      = 0x030a,
    ART_DST_EG2_DECAYTIME       = 0x030b,
    ART_DST_EG2_SUSTAINLEVEL_LO = 0x030c,
    ART_DST_EG2_RELEASETIME     = 0x030d,
    ART_DST_EG2_SUSTAINLEVEL_HI = 0x030e,
};

/* art1 usControl, src=ART_SRC_LFO -> dest=ART_DST_PITCH only, SPEC.md S2.4.4. */
enum ArtLfoControl {
    ART_CTRL_NONE = 0x0000,
    ART_CTRL_CC1  = 0x0081, /* modwheel */
};

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
    int16_t eg2_to_pitch_cents;      /* src=ART_SRC_EG2 -> dest=ART_DST_PITCH */
    int16_t vel_to_atten_depth;      /* src=ART_SRC_KEYONVELOCITY -> dest=ART_DST_ATTENUATION, default -9600 */
    int16_t pan_cb;                  /* dest=ART_DST_PAN_COARSE/ART_DST_PAN, small correction to CC10 pan */
    int32_t lfo_freq_tc, lfo_delay_tc; /* src=ART_SRC_NONE -> dest=ART_DST_LFO_FREQUENCY/ART_DST_LFO_DELAY, timecents */
    int16_t lfo_pitch_inherent_cents; /* src=ART_SRC_LFO, ctrl=ART_CTRL_NONE,
                                          dest=ART_DST_PITCH: ungated ("always
                                          on") pitch-LFO depth, high word of
                                          lScale, clamped +-1200 */
    int16_t lfo_pitch_cc1_cents;      /* src=ART_SRC_LFO, ctrl=ART_CTRL_CC1,
                                          dest=ART_DST_PITCH: CC1-scaled
                                          pitch-LFO depth, same formula/clamp */
    int16_t eg1_attack_vel_tc;      /* src=ART_SRC_KEYONVELOCITY -> dest=ART_DST_EG1_ATTACKTIME:
                                          attack-time velocity scaling, high
                                          word of lScale, full-scale
                                          (velocity 127) cent offset */
    int16_t eg1_decay_kf_tc, eg2_decay_kf_tc; /* src=ART_SRC_KEYNUMBER -> dest
                                          ART_DST_EG1_DECAYTIME/ART_DST_EG2_DECAYTIME:
                                          decay-time key-follow, high word of
                                          lScale, full-scale (key 127) cent
                                          offset */
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

enum {
    DLS_DEFAULT_UNITY_NOTE = 60, /* middle C, wave/region wsmp-absent default */
    WLNK_CHANNEL_LEFT = 1,       /* wlnk ulChannel: only mono/left link is used */
};

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
