/* synth.h -- channel state, MIDI dispatch, CC/RPN/SysEx. SPEC.adoc Part 4. */
#ifndef SYNTH_H
#define SYNTH_H

#include <stdint.h>

typedef struct Channel {
    uint8_t bank_msb, bank_lsb, program;
    /* SPEC.adoc S4.2.1: the Bank/Program queue's "+0x18 current value" is the
     * SCHEDULED 21-bit locale (programme|bankLSB<<7|bankMSB<<14, no drum
     * bit), latched by 0x16df4 only on Program Change / GS Reset / GM
     * System On-Off / System Reset -- NOT recomputed live from bank_msb/
     * bank_lsb on every read. A Bank Select alone does not move it; only a
     * following Program Change (or a reset) does (SPEC_LOG.adoc #14). */
    uint32_t scheduled_locale;
    uint8_t volume;       /* CC7, default 100 */
    uint8_t expression;   /* CC11, default 127 */
    uint8_t pan;          /* CC10, default 64 */
    uint8_t modulation;   /* CC1, default 0 (unused: no LFO, SPEC_LOG.adoc) */
    uint16_t pitch_bend;  /* raw 14-bit, center 8192 */
    uint16_t pb_range_cents; /* RPN0, default 200 */
    int16_t rpn1_fine_cents;
    int16_t rpn2_coarse_cents;
    uint16_t rpn_select;  /* MSB<<7|LSB, 0x3FFF = Null */
    uint16_t data_entry_combined;
    uint8_t sustain;      /* CC64 raw value */
    uint8_t is_rhythm;    /* SPEC.adoc S4.8: USE RHYTHM PART gate, simplified per-channel */
    uint8_t mono_mode;
} Channel;

extern Channel g_channels[16];
extern uint8_t g_gs_mode;          /* gates CC0/CC32 storage, SPEC.adoc S3.1.1/S4.2.2 */
extern int32_t g_master_vol_hdb;   /* master volume attenuation, hundredths of a dB */

void synth_reset(void);
uint32_t synth_channel_locale(int ch); /* program|bankLSB<<7|bankMSB<<14|drum<<31 */
int32_t synth_pitch_bend_cents(int ch);

/* Dispatch one short MIDI message (explicit status byte; running-status
 * expansion, if any, is the caller's job -- SPEC_LOG.adoc). status==0xFF is
 * treated as System Reset. */
void synth_midi(uint32_t status, uint32_t d1, uint32_t d2);

/* Internal-only SysEx handler (NOT part of the public ABI -- see
 * SPEC_LOG.adoc: msgs_midi's 3-byte signature cannot carry a SysEx message,
 * so this is invoked only from smf.c for SysEx events embedded in a loaded
 * Standard MIDI File). buf points at the byte right after the leading 0xF0. */
void synth_sysex(const uint8_t *buf, uint32_t len);

#endif /* SYNTH_H */
