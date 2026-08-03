/* dls.h -- RIFF/DLS parse of gm.dls into instruments, regions, waves. SPEC.adoc Part 2. */
/* Field names are chosen for clarity in this clean-room reimplementation, not a claim of matching the driver's in-memory offsets (SPEC.adoc Part 2 S2.9 documents those for the original binary only). */
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
    int16_t attenuation_hdb;   /* wsmp lAttenuation, (lScale*10)>>16, hundredths of a dB, wave scope */
    uint8_t unity_note;        /* wsmp usUnityNote, byte-truncated, wave scope */
    uint8_t no_loop;           /* wsmp cSampleLoops==0, wave scope */
} Wave;

/* art1 usSource values gm.dls/the driver actually use, SPEC.adoc S2.4.3 (not the full DLS-1 enumeration -- just the rows apply_art1() switches on). */
enum ArtSource {
    ART_SRC_NONE           = 0x0000, /* unconditional (RPN-less) block */
    ART_SRC_LFO            = 0x0001,
    ART_SRC_KEYONVELOCITY  = 0x0002,
    ART_SRC_KEYNUMBER      = 0x0003,
    ART_SRC_EG2            = 0x0005,
};

/* art1 usDestination values, SPEC.adoc S2.4.3; ART_DST_PAN_COARSE reuses the spec's reserved 0x0002 slot for the same pan_cb field (see apply_art1). */
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

/* art1 usControl, src=ART_SRC_LFO -> dest=ART_DST_PITCH only, SPEC.adoc S2.4.4. */
enum ArtLfoControl {
    ART_CTRL_NONE = 0x0000,
    ART_CTRL_CC1  = 0x0081, /* modwheel */
};

/* The 0x68-byte resolved articulation block, distinct from Wave (SPEC.adoc S3.2, SPEC_LOG.adoc item2's S2.9.2-vs-S3.2 field-ownership fix). Shared per-instrument default or private per-region. */
typedef struct Artic {
    int32_t eg1_attack_tc, eg1_decay_tc, eg1_release_tc;  /* raw DLS timecents */
    int16_t eg1_sustain_permille;                          /* 0..1000 */
    int32_t eg2_attack_tc, eg2_decay_tc, eg2_release_tc;
    int16_t eg2_sustain_permille;
    int16_t eg2_to_pitch_cents;      /* src=ART_SRC_EG2 -> dest=ART_DST_PITCH */
    int16_t vel_to_atten_depth;      /* src=ART_SRC_KEYONVELOCITY -> dest=ART_DST_ATTENUATION, default -9600 */
    int16_t pan_cb;                  /* dest=ART_DST_PAN_COARSE/ART_DST_PAN, small correction to CC10 pan */
    int32_t lfo_freq_tc, lfo_delay_tc; /* src=ART_SRC_NONE -> dest=ART_DST_LFO_FREQUENCY/ART_DST_LFO_DELAY, timecents */
    int16_t lfo_pitch_inherent_cents; /* src=LFO,ctrl=NONE -> PITCH: ungated pitch-LFO depth, hi word of lScale, clamped +-1200 */
    int16_t lfo_pitch_cc1_cents;      /* src=LFO,ctrl=CC1 -> PITCH: CC1-scaled pitch-LFO depth, same formula/clamp */
    int16_t eg1_attack_vel_tc;      /* src=KEYONVELOCITY -> EG1_ATTACKTIME: velocity attack scaling, hi word of lScale, full-scale (vel 127) cent offset */
    int16_t eg1_decay_kf_tc, eg2_decay_kf_tc; /* src=KEYNUMBER -> EG1/EG2_DECAYTIME: decay key-follow, hi word of lScale, full-scale (key 127) cent offset */
} Artic;

typedef struct Region {
    struct Region *next;
    Wave *wave;
    Artic *artic;
    int32_t loop_start, loop_end; /* region-scope wsmp override, samples */
    int16_t fine_tune;
    int16_t attenuation_hdb; /* art1 lScale, (lScale*10)>>16, hundredths of a dB, region scope */
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

/* Parses a DLS-1 RIFF collection in place; sample data is referenced into `data`, never copied (SPEC.adoc S1.5.5). */
int dls_load(const uint8_t *data, uint32_t len);

/* Three-tier bank/program/drum fallback + region lookup, SPEC.adoc S3.1. */
Region *dls_find_region(uint32_t locale, uint8_t note);

#endif /* DLS_H */
