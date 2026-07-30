/* synth.c -- channel state, MIDI dispatch, CC/RPN/SysEx. SPEC.adoc Part 4.
 *
 * Simplifications relative to SPEC.adoc (all recorded in SPEC_LOG.adoc):
 *  - Five of the six S4.2.1 controller queues (Modulation, Pitch Bend,
 *    Channel Volume, Expression, Pan) are still written directly to a flat
 *    "current" field on receipt rather than through a real pending-node
 *    queue. SPEC_LOG.adoc #14 (resolved entry) argues this is a *proven*
 *    behavioral no-op in this engine specifically -- not a shortcut taken
 *    on faith -- because smf.c's event loop dispatches every event
 *    synchronously in nondecreasing sample-time order with a stable FIFO
 *    tie-break (matching SPEC.adoc S4.7.1 exactly), so "write directly the
 *    instant the event is due" and "insert into a queue, then promote, with
 *    a look-ahead read for anything queried in between" always compute the
 *    identical value at every read site this codebase has. See SPEC_LOG.adoc
 *    #14 for the full argument.
 *  - The SIXTH queue (Bank Select + Program) is different in kind, not just
 *    in cadence: the value it schedules (the 21-bit locale) is *derived*
 *    from other live state (the bank bytes) at a distinct trigger event
 *    (Program Change / reset), not the raw incoming byte itself. That *is*
 *    implemented as real scheduling below (`scheduled_locale`, latched only
 *    on Program Change/reset, SPEC.adoc S4.2.1/S4.6) -- SPEC_LOG.adoc #14.
 *  - No RCV CHANNEL "Part" indirection layer: channel index doubles as
 *    "Part" index directly (channel 9, 0-based, is rhythm by default,
 *    matching the *observable* default in SPEC.adoc S4.8 without the
 *    indirection table).
 *  - No 12-entry-per-part GS Scale Tuning grid.
 */
#include "synth.h"
#include "voice.h"
#include "tables.h"
#include "dls.h"

Channel g_channels[16];
uint8_t g_gs_mode;
int32_t g_master_vol_hdb;

/* SPEC.adoc S4.9.1's `0x150bc(1)` acquire half, restricted to this engine's
 * one-shot gm.dls load: only set the loaded flag if a collection actually
 * parsed, so a re-acquire after a failed load can't resurrect a g_dls that
 * was never valid. */
static void acquire_collection(void) {
    if (g_dls.first_instrument) g_dls.valid = 1;
}

/* SPEC.adoc S4.2.1 `0x16df4`: (re)compute the scheduled 21-bit locale from the
 * channel's CURRENT bank bytes + program, with no drum bit (that is OR'd in
 * live at note-on time instead, §4.8). Called only from the sites SPEC.adoc
 * cites as writers of the Bank/Program queue's "+0x18" field: Program
 * Change, and the three reset paths (via channel_defaults). Bank Select
 * (CC0/CC32) deliberately does NOT call this -- see synth.h. */
static void relatch_locale(Channel *c) {
    c->scheduled_locale = (uint32_t)c->program
        | (((uint32_t)c->bank_lsb & 0x7f) << 7)
        | (((uint32_t)c->bank_msb & 0x7f) << 14);
}

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
    c->pb_range_cents = 200;
    c->rpn1_fine_cents = 0;
    c->rpn2_coarse_cents = 0;
    c->rpn_select = 0x3FFF;
    c->data_entry_combined = 0;
    c->sustain = 0;
    c->is_rhythm = (idx == 9) ? 1 : 0; /* SPEC.adoc S4.8: channel 10 default rhythm part */
    c->mono_mode = 0;
}

void synth_reset(void) {
    g_gs_mode = 0;
    g_master_vol_hdb = 0;
    for (int i = 0; i < 16; i++) channel_defaults(&g_channels[i], i);
    voice_pool_reset();
    /* synth_reset() stands in for this engine's device construction/open
     * (called by src/cli.c and src/wasm.c to initialise) as well as the MIDI
     * System Reset (0xFF) and GM System On/Off handlers below -- and per
     * SPEC.adoc S4.9.1's call-site table, device construction, System Reset
     * and GM System On all acquire the instrument collection. */
    acquire_collection();
}

/* A/B guard for measurement purposes only (SPEC_LOG.adoc #14): 1 (default,
 * shipped) = SPEC.adoc S4.2.1's scheduled locale, latched only at Program
 * Change/reset. 0 = the prior behavior this entry replaces (recompute live
 * from bank_msb/bank_lsb/program on every call). This exists so the two
 * behaviors can be A/B'd from the exact same tree state (see report: another
 * concurrent, unrelated change landed in voice.c/render.c during this work,
 * so this entry's effect cannot be cleanly isolated from that concurrent work
 * -- flipping this single macro and rebuilding twice in a row can be.
 * Not a permanent feature flag; not exposed by
 * the Makefile. */
#ifndef SYNTH_SCHEDULE_LOCALE
#define SYNTH_SCHEDULE_LOCALE 1
#endif

uint32_t synth_channel_locale(int ch) {
    /* SPEC.adoc S4.2.1/S4.8: the bank/program half is the QUEUED, previously-
     * latched value (relatch_locale, only re-run on Program Change/reset),
     * not recomputed live from bank_msb/bank_lsb here -- a Bank Select with
     * no following Program Change must NOT retarget a channel's instrument.
     * The drum bit is the one part of this that IS read live, at query
     * time, per §4.8. */
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
    /* (raw * rangeCents) >> 13, FLOOR -- not C truncation toward zero.
     * SPEC.adoc S4.4 says "C-style truncation toward zero"; the driver disagrees
     * `[A:0x16d60]`:
     *     16d73  sub  eax,0x2000              ; value - 8192
     *     16d78  imul eax,DWORD PTR [esi+0x20] ; * range in cents (default 0xc8)
     *     16d7c  sar  eax,0xd                 ; ARITHMETIC shift = floor
     * `sar` floors; `/` rounds toward zero. They differ by one cent on every
     * downward bend that is not an exact multiple of 8192, which is most of
     * them. 32-bit `imul` there vs. int64 here is not a difference: |raw| <=
     * 8192 and range <= 4800 peaks at 39.3M, well inside int32. */
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

/* SPEC.adoc S4.3's CC121 row says the handler "re-schedules Volume=100, Pan=64,
 * Expression=127, Pitch Bend=8192, Modulation=0" `[A:0x1351f]`. The Volume
 * half of that is wrong: `[M: probe 37]`, CC121 does not touch Channel Volume
 * at all. probes/37_rac_volume_order.mid plays one sine per case and the
 * reference reads (case RMS, dB, against its own CC7=40 and CC7=100 controls
 * at -29.29 and -13.39):
 *
 *   CC7=40 then CC121, same tick   -29.29   CC7=40, CC121 +50ms    -29.29
 *   CC121 then CC7=40, same tick   -29.29   CC7=40, CC121 +500ms   -29.29
 *
 * Exact to 0.00 dB on all four -- so this is not S4.2.1's queue-ordering
 * question (SPEC_LOG.adoc #14) at all: a CC121 half a second LATER leaves 40
 * standing, which no same-timestamp tie-break can produce. Expression is a
 * different story and does get reset (case G lands 18.30 dB above a surviving
 * CC11=40), so the exemption is Volume's alone.
 *
 * Audible on tests/warm-echo.mid, whose two tracks both send CC7 (76 on ch0,
 * 52 on ch1) and then CC121 at tick 0: resetting them to 100 rendered the
 * two-channel section +4.84 dB against the one-channel section where the
 * reference has -2.03 dB. r 0.833 -> 0.994, residual -25.25 -> -34.68 dB. */
#ifndef CC121_RESETS_VOLUME
#define CC121_RESETS_VOLUME 0
#endif

static void reset_all_channel_controllers(Channel *c) {
    c->modulation = 0;
    c->pitch_bend = 8192;
#if CC121_RESETS_VOLUME
    c->volume = 100;
#endif
    c->pan = 64;
    c->expression = 127;
}

/* SPEC.adoc S4.6.2 -- the driver's ResetAllChannelControllers, which the reset
 * paths and USE RHYTHM PART all route through. Distinct from CC121 above: this
 * one does reset Channel Volume, and it runs across every channel. */
static void reset_all_channel_controllers_device(void) {
    /* SPEC.adoc S4.6.2/S5.3: this tail's `0x123de` is the bulk immediate
     * reclaim to the free pool, not the authored envelope release that
     * voice_all_sound_off() (CC120/CC126/CC127, S4.3) performs -- so it must
     * cut, not release. voice_pool_reset() is that immediate cut.
     * ponytail: it also zeroes the reserve top-up accumulator and voice age
     * counter, which 0x123de does not -- harmless here since every voice has
     * just gone inactive, but not an exact mechanical match. */
    voice_pool_reset();
    for (int i = 0; i < 16; i++) {
        Channel *c = &g_channels[i];
        c->modulation = 0;
        c->pitch_bend = 8192;
        c->volume = 100;
        c->pan = 64;
        c->expression = 127;
        c->sustain = 0;
    }
}

static void dispatch_cc(int ch, uint32_t cc, uint32_t val) {
    Channel *c = &g_channels[ch];
    switch (cc) {
        /* CC0/CC32 deliberately do NOT call relatch_locale(): SPEC.adoc
         * S4.2.1 writes these bytes directly (`0x16dc4`/`0x16ddc`) and
         * leaves the SCHEDULED locale alone until the next Program Change
         * (or reset) re-derives it from whatever bank is current then. */
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
            reset_all_channel_controllers(c);
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
            relatch_locale(&g_channels[ch]); /* SPEC.adoc S4.2.1: schedules
                                                 the new locale now, from
                                                 whatever bank was already
                                                 selected */
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
        /* SPEC.adoc S4.5: the reset is reached before the sub-ID2 byte is
         * examined, so System Off clears GS mode exactly as System On does.
         * The driver's own sub-ID2-gated clear is dead code against the reset
         * that just ran; not reproduced. */
        if (buf[2] == 0x09) {
            synth_reset();
            /* SPEC.adoc S4.9.1: System Off (sub-ID2 0x02) releases the
             * collection after synth_reset()'s acquire; System On (0x01)
             * needs nothing more -- synth_reset() already re-acquired. */
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
                /* SPEC.adoc S4.5: "GS Reset does not call it at all" -- save
                 * and restore around synth_reset()'s new acquire so this path
                 * leaves the collection's loaded state unchanged. */
                int had_collection = g_dls.valid;
                synth_reset();
                g_dls.valid = had_collection;
                g_gs_mode = 1;
                return;
            }
            /* SPEC.adoc S4.5, S4.2.2: RCV CHANNEL (0x137b7), USE RHYTHM PART
             * (0x137a2) and GS Scale Tuning (0x1376e) are each individually
             * gated on GS mode in the driver, and every other address in this
             * family is already a no-op -- so one guard ahead of the whole
             * block is behaviorally identical. */
            if (!g_gs_mode) return;
            /* GS part-parameter address is 3 bytes: a0=0x40 (fixed),
             * a1=0x1<block> (0x10-0x1F), a2=<param>. Guarding on a1's range
             * (not a0's low nibble) also keeps this from misfiring on GS
             * Reset (a0=0x40,a1=0x00,a2=0x7F), handled above. SPEC.adoc S4.2.2. */
            if (a0 == 0x40 && a1 >= 0x10 && a1 <= 0x1F && len >= 9) {
                uint8_t block = a1 & 0x0F;
                /* Default RCV-CHANNEL table (SPEC.adoc T.8, static table0 at
                 * VMA 0x1a600: [9,0,1,2,...,8,10,11,...,15]) maps GS Part
                 * block -> MIDI channel index. RCV CHANNEL itself remains
                 * unmodeled (no Part indirection layer, SPEC_LOG.adoc #11),
                 * so this default mapping is what g_channels[] is keyed by. */
                static const uint8_t block_to_channel[16] = {
                    9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15
                };
                uint8_t ch = block_to_channel[block];
                if (a2 == 0x02) { /* RCV CHANNEL: not modeled (no Part
                                      indirection layer, SPEC_LOG.adoc) */
                } else if (a2 == 0x15) { /* USE RHYTHM PART */
                    g_channels[ch].is_rhythm = buf[7] ? 1 : 0;
                    /* SPEC.adoc S4.5: this message falls into the shared reset
                     * tail, so it fires a device-wide controller reset. Bank
                     * and Program survive -- the tail's second call is gated
                     * off on this path. */
                    reset_all_channel_controllers_device();
                }
                /* per-part tuning grid: not modeled */
            }
        }
        return;
    }
    /* any other manufacturer/model/command: unrecognized, dropped silently. */
}
