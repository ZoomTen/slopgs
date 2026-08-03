/* synth.c -- channel state, MIDI dispatch, CC/RPN/SysEx. SPEC.adoc Part 4. */
#include "synth.h"
#include "voice.h"
#include "tables.h"
#include "dls.h"

Channel g_channels[16];
uint8_t g_gs_mode;
int32_t g_master_vol_hdb;

/* SPEC.adoc T.8 static table0, VMA 0x1a600: Part i's default physical channel; reloaded on every MIDI-level reset (S4.6.4). */
static const uint8_t kPartChannelDefault[16] = {
    9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15
};
/* SPEC.adoc S4.2.2 device+0x12fc: RCV CHANNEL map; only USE RHYTHM PART reads it (SPEC_LOG.adoc item44). */
static uint8_t part_to_channel[16];

/* SPEC.adoc S4.9.1 0x150bc acquire half; guarded so a re-acquire can't resurrect an invalid load (SPEC_LOG.adoc item25-followup-gm-off-measured). */
static void acquire_collection(void) {
    if (g_dls.first_instrument) g_dls.valid = 1;
}

/* SPEC.adoc S4.2.1 0x16df4: recomputes the scheduled locale from current bank+program (drum bit OR'd in live at note-on, S4.8). Called only from Program Change and the reset paths; CC0/CC32 deliberately do not (see synth.h). */
static void relatch_locale(Channel *c) {
    c->scheduled_locale = (uint32_t)c->program
        | (((uint32_t)c->bank_lsb & 0x7f) << 7)
        | (((uint32_t)c->bank_msb & 0x7f) << 14);
}

/* SPEC.adoc S4.6.4 reset-state table: fields the reset paths touch; RPN0/RPN-select/sustain excluded (SPEC_LOG.adoc item41). */
static void channel_defaults(Channel *c, int idx) {
    c->bank_msb = 0;
    c->bank_lsb = 0;
    c->program = 0;
    relatch_locale(c); /* -> 0, matching SPEC.adoc S4.6.4's reset table */
    c->volume = 100;
    c->expression = 127;
    c->pan = 64;
    c->modulation = 0;
    c->pitch_bend = 8192;
    c->rpn1_fine_cents = 0;
    c->rpn2_coarse_cents = 0;
    c->data_entry_combined = 0;
    c->is_rhythm = (idx == 9) ? 1 : 0; /* SPEC.adoc S4.8: channel 10 default rhythm part */
    c->mono_mode = 0;
}

static void channel_construct_only(Channel *c) {
    c->pb_range_cents = 200;
    c->rpn_select = 0x3FFF;
    c->sustain = 0;
}

void synth_reset(void) {
    g_gs_mode = 0;
    g_master_vol_hdb = 0;
    for (int i = 0; i < 16; i++) {
        channel_defaults(&g_channels[i], i);
        part_to_channel[i] = kPartChannelDefault[i]; /* SPEC.adoc S4.6.4: RCV CHANNEL reload, item44 */
    }
    voice_pool_reset();
    /* synth_reset() is reached by System Reset, GS Reset and GM System On/Off; System Reset and GM System On both acquire the collection (SPEC.adoc S4.9.1, SPEC_LOG.adoc item25-followup-gm-off-measured). */
    acquire_collection();
}

/* Device construction/open: SPEC.adoc S4.2.1 power-on defaults + synth_reset() body; resets must not touch RPN0/RPN-select/sustain (SPEC_LOG.adoc item41). */
void synth_construct(void) {
    for (int i = 0; i < 16; i++) channel_construct_only(&g_channels[i]);
    synth_reset();
}

/* A/B guard for measurement only (SYNTH_SCHEDULE_LOCALE); concurrent-change confound -- SPEC_LOG item55 */
#ifndef SYNTH_SCHEDULE_LOCALE
#define SYNTH_SCHEDULE_LOCALE 1
#endif

uint32_t synth_channel_locale(int ch) {
    /* SPEC.adoc S4.2.1/S4.8: bank/program half is the queued, latched value (relatch_locale); only the drum bit is read live at query time (SPEC_LOG.adoc item14). */
    Channel *c = &g_channels[ch];
#if SYNTH_SCHEDULE_LOCALE
    uint32_t locale = c->scheduled_locale;
#else
    uint32_t locale = (uint32_t)c->program
        | (((uint32_t)c->bank_lsb & 0x7f) << 7)
        | (((uint32_t)c->bank_msb & 0x7f) << 14);
#endif
    if (c->is_rhythm) locale |= 0x80000000u;
    return locale;
}

int32_t synth_pitch_bend_cents(int ch) {
    Channel *c = &g_channels[ch];
    int32_t raw = (int32_t)c->pitch_bend - 8192;
    /* (raw*rangeCents)>>13 floor matches SPEC S3.3.2(c); sar-vs-/ and int64 bound reasoning -- SPEC_LOG item55 */
    return (int32_t)(((int64_t)raw * c->pb_range_cents) >> 13);
}

static void recompute_rpn1(Channel *c) {
    int32_t combined = c->data_entry_combined;
    c->rpn1_fine_cents = (int16_t)(((combined - 8192) * 100) / 8192);
}

static void cc_data_entry_msb(Channel *c, uint32_t d2) {
    c->data_entry_combined = (uint16_t)((c->data_entry_combined & 0x7F) | ((d2 & 0x7F) << 7));
    if (c->rpn_select == 0) {
        c->pb_range_cents = (uint16_t)(d2 * 100);
    } else if (c->rpn_select == 1) {
        recompute_rpn1(c);
    } else if (c->rpn_select == 2) {
        c->rpn2_coarse_cents = (int16_t)(((int32_t)d2 - 64) * 100);
    }
}

static void cc_data_entry_lsb(Channel *c, uint32_t d2) {
    c->data_entry_combined = (uint16_t)((c->data_entry_combined & (0x7F << 7)) | (d2 & 0x7F));
    if (c->rpn_select == 1) {
        recompute_rpn1(c);
    }
    /* RPN0/RPN2 do not consume the LSB half (SPEC.adoc S4.4). */
}

/* SPEC.adoc S4.3 CC121 row [A:0x1351f]: Volume/Pan re-schedule only when the value byte is non-zero; Expression/Bend/Modulation are unconditional (SPEC_LOG.adoc item42; Pan's own value byte is item42's open sub-question). */
static void reset_all_channel_controllers(Channel *c, uint32_t val) {
    c->modulation = 0;
    c->pitch_bend = 8192;
    if (val != 0) c->volume = 100;
    c->pan = 64;
    c->expression = 127;
}

/* SPEC.adoc S4.6.2 -- ResetAllChannelControllers, reached by the reset paths and USE RHYTHM PART. Unlike CC121 above, this always resets Channel Volume, across every channel. */
static void reset_all_channel_controllers_device(void) {
    /* ponytail: voice_pool_reset() also zeroes the reserve top-up accumulator and voice age counter, which 0x123de does not -- harmless since every voice is already inactive here. */
    voice_pool_reset();
    for (int i = 0; i < 16; i++) {
        Channel *c = &g_channels[i];
        c->modulation = 0;
        c->pitch_bend = 8192;
        c->volume = 100;
        c->pan = 64;
        c->expression = 127;
        /* SPEC.adoc S4.6.4: sustain byte not touched here (queues sentinel-0xFE only); no voice_sustain_lift() needed since voice_pool_reset() above already cut every voice (SPEC_LOG.adoc item41). */
    }
}

static void dispatch_cc(int ch, uint32_t cc, uint32_t val) {
    Channel *c = &g_channels[ch];
    switch (cc) {
        /* CC0/CC32 write bank bytes directly (SPEC.adoc S4.2.1, 0x16dc4/0x16ddc) without re-latching the scheduled locale; only Program Change/reset does that. */
        case 0: if (g_gs_mode) c->bank_msb = (uint8_t)val; break;
        case 1: c->modulation = (uint8_t)val; break;
        case 6: cc_data_entry_msb(c, val); break;
        case 7: c->volume = (uint8_t)val; break;
        case 10: c->pan = (uint8_t)val; break;
        case 11: c->expression = (uint8_t)val; break;
        case 32: if (g_gs_mode) c->bank_lsb = (uint8_t)val; break;
        case 38: cc_data_entry_lsb(c, val); break;
        case 64:
            c->sustain = (uint8_t)val;
            if (val == 0) voice_sustain_lift(ch);
            break;
        case 98: case 99: c->rpn_select = 0x3FFF; break;
        case 100: c->rpn_select = (uint16_t)((c->rpn_select & (0x7F << 7)) | (val & 0x7F)); break;
        case 101: c->rpn_select = (uint16_t)((c->rpn_select & 0x7F) | ((val & 0x7F) << 7)); break;
        case 120: voice_all_sound_off(ch); break;
        case 121:
            reset_all_channel_controllers(c, val);
            voice_sustain_lift(ch);
            break;
        case 123: voice_all_notes_off(ch); break;
        case 126: c->mono_mode = 1; voice_all_sound_off(ch); break;
        case 127: c->mono_mode = 0; voice_all_sound_off(ch); break;
        default: break; /* every other CC: parsed off, no effect (SPEC.adoc S4.3) */
    }
}

void synth_midi(uint32_t status, uint32_t d1, uint32_t d2) {
    if (status == 0xFF) {
        synth_reset();
        return;
    }
    uint32_t kind = status & 0xF0;
    int ch = (int)(status & 0x0F);

    switch (kind) {
        case 0x80:
            voice_note_off(ch, (int)d1);
            break;
        case 0x90:
            if (d2 == 0) voice_note_off(ch, (int)d1);
            else voice_note_on(ch, (int)d1, (int)d2);
            break;
        case 0xB0:
            dispatch_cc(ch, d1, d2);
            break;
        case 0xC0:
            g_channels[ch].program = (uint8_t)d1;
            relatch_locale(&g_channels[ch]); /* SPEC.adoc S4.2.1: schedules the new locale from the already-selected bank */
            break;
        case 0xE0:
            g_channels[ch].pitch_bend = (uint16_t)(((d2 & 0x7F) << 7) | (d1 & 0x7F));
            break;
        default:
            break; /* poly pressure, channel pressure: not modeled */
    }
}

void synth_sysex(const uint8_t *buf, uint32_t len) {
    if (len < 1) return;
    uint8_t mfr = buf[0];

    if (mfr == 0x7E && len >= 4) { /* GM System On/Off */
        /* SPEC.adoc S4.5: reset runs before sub-ID2 is examined, so System Off clears GS mode exactly as System On does; the driver's own sub-ID2-gated clear is dead code (SPEC_LOG.adoc item25-resolved). */
        if (buf[2] == 0x09) {
            synth_reset();
            /* SPEC.adoc S4.9.1: System Off (sub-ID2 0x02) releases the collection after synth_reset()'s acquire; System On needs nothing more (SPEC_LOG.adoc item25-followup-gm-off-measured). */
            if (buf[3] == 0x02) g_dls.valid = 0;
        }
        return;
    }
    if (mfr == 0x7F && len >= 6) { /* Master Volume, Universal Realtime */
        if (buf[2] == 0x04 && buf[3] == 0x01) {
            uint8_t msb = buf[5];
            if (msb > 127) msb = 127;
            g_master_vol_hdb = g_table_vel[msb];
        }
        return;
    }
    if (mfr == 0x41 && len >= 8) { /* Roland GS family, DT1 */
        if (buf[2] == 0x42 && buf[3] == 0x12) {
            uint8_t a0 = buf[4], a1 = buf[5], a2 = buf[6];
            if (a0 == 0x40 && a1 == 0x00 && a2 == 0x7F) { /* GS Reset */
                /* SPEC.adoc S4.5: GS Reset does not call the acquire/release at all; save/restore g_dls.valid around synth_reset() (SPEC_LOG.adoc item25-followup-gm-off-measured). */
                int had_collection = g_dls.valid;
                synth_reset();
                g_dls.valid = had_collection;
                g_gs_mode = 1;
                return;
            }
            /* SPEC.adoc S4.5/S4.2.2: RCV CHANNEL/USE RHYTHM PART/Scale Tuning are each gated individually in the driver; one guard ahead of this whole block is behaviorally identical here (SPEC_LOG.adoc item37). */
            if (!g_gs_mode) return;
            /* GS part-parameter address: a0=0x40 fixed, a1=0x1<block> (0x10-0x1F), a2=<param> (SPEC.adoc S4.2.2). Guarding on a1's range (not a0's low nibble) keeps this from misfiring on GS Reset (a1=0x00), handled above. */
            if (a0 == 0x40 && a1 >= 0x10 && a1 <= 0x1F && len >= 9) {
                uint8_t block = a1 & 0x0F; /* == Part index, SPEC.adoc S4.2.2 */
                if (a2 == 0x02) { /* RCV CHANNEL: writes the per-Part channel map, no shared reset tail (SPEC.adoc S4.5/T.8, SPEC_LOG.adoc item44) */
                    part_to_channel[block] = buf[7];
                } else if (a2 == 0x15) { /* USE RHYTHM PART */
                    /* Resolves through the RCV CHANNEL map, honoring an earlier remap (SPEC.adoc S4.5/T.8, SPEC_LOG.adoc item44, closes item11). Masked to 0-15 as a defensive array bound, not a SPEC reading (item44). */
                    uint8_t ch = part_to_channel[block] & 0x0F;
                    g_channels[ch].is_rhythm = buf[7] ? 1 : 0;
                    /* SPEC.adoc S4.5: fires the shared reset tail; Bank/Program survive since the tail's second call is gated off on this path (SPEC_LOG.adoc item25-resolved). */
                    reset_all_channel_controllers_device();
                }
                /* per-part tuning grid: not modeled (SPEC_LOG.adoc item36) */
            }
        }
        return;
    }
    /* any other manufacturer/model/command: unrecognized, dropped silently. */
}
