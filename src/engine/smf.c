/* smf.c -- Standard MIDI File parser and sequencer. SPEC.md S1.5.4: not part
 * of the original driver, ordinary generic SMF handling. */
#include "smf.h"
#include "synth.h"
#include "voice.h"
#include "render.h"
#include "rt.h"

enum {
    EKIND_MIDI = 0,
    EKIND_SYSEX,
    EKIND_TEMPO,
};

#define MAX_TRACKS 512

typedef struct Event {
    uint32_t abs_tick;
    uint32_t seq;
    uint8_t kind;           // EKIND_*
    uint8_t status, d1, d2;
    const uint8_t *data;    // kind == EKIND_SYSEX
    uint32_t data_len;
    uint32_t tempo_usec;    // kind == EKIND_TEMPO
    uint32_t sample_time;   // computed
} Event;

static Event *g_events = 0;
static uint32_t g_event_count = 0;
static uint32_t g_song_length_samples = 0;

static uint32_t g_event_index = 0;
static uint32_t g_sample_pos = 0;
static int32_t g_loop_remaining = 0; /* -1 = infinite, 0 = no more loops */
static int g_finished = 0;
static int g_loaded = 0;

static uint32_t rd_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint16_t rd_u16be(const uint8_t *p) {
    return (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
}
static int fourcc_is(const uint8_t *p, char a, char b, char c, char d) {
    return p[0] == (uint8_t)a && p[1] == (uint8_t)b && p[2] == (uint8_t)c && p[3] == (uint8_t)d;
}

static uint32_t read_vlq(const uint8_t **pp, const uint8_t *end) {
    uint32_t v = 0;
    const uint8_t *p = *pp;
    while (p < end) {
        uint8_t b = *p++;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    *pp = p;
    return v;
}

/* Walks one MTrk's bytes, calling `emit` for each event with abs_tick, or
 * just counting if `emit` is 0. Returns event count. */
typedef void (*EmitFn)(uint32_t abs_tick, uint32_t seq, uint8_t status, uint8_t d1, uint8_t d2,
                        const uint8_t *sysex_data, uint32_t sysex_len, uint32_t tempo_usec, uint8_t kind, void *ctx);

static uint32_t walk_track(const uint8_t *data, uint32_t len, EmitFn emit, void *ctx, uint32_t seq_base) {
    const uint8_t *p = data;
    const uint8_t *end = data + len;
    uint32_t abs_tick = 0;
    uint8_t running_status = 0;
    uint32_t count = 0;

    while (p < end) {
        uint32_t delta = read_vlq(&p, end);
        abs_tick += delta;
        if (p >= end) break;
        uint8_t b = *p;

        if (b == 0xFF) { /* meta event */
            p++;
            if (p >= end) break;
            uint8_t type = *p++;
            uint32_t mlen = read_vlq(&p, end);
            if (p + mlen > end) break;
            if (type == 0x51 && mlen == 3) {
                uint32_t usec = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
                if (emit) emit(abs_tick, seq_base + count, 0, 0, 0, 0, 0, usec, EKIND_TEMPO, ctx);
                count++;
            }
            /* other meta types (track name, end-of-track, etc.): ignored */
            p += mlen;
        } else if (b == 0xF0 || b == 0xF7) { /* sysex (and sysex continuation, treated the same) */
            p++;
            uint32_t slen = read_vlq(&p, end);
            if (p + slen > end) break;
            if (b == 0xF0) {
                if (emit) emit(abs_tick, seq_base + count, 0, 0, 0, p, slen, 0, EKIND_SYSEX, ctx);
                count++;
            }
            p += slen;
        } else if (b & 0x80) { /* new status byte */
            running_status = b;
            p++;
            uint8_t kind = b & 0xF0;
            uint8_t d1 = (p < end) ? *p++ : 0;
            uint8_t d2 = 0;
            if (kind != 0xC0 && kind != 0xD0) {
                d2 = (p < end) ? *p++ : 0;
            }
            if (emit) emit(abs_tick, seq_base + count, running_status, d1, d2, 0, 0, 0, EKIND_MIDI, ctx);
            count++;
        } else { /* running status data byte */
            if (running_status == 0) { p++; continue; } /* malformed: skip byte defensively */
            uint8_t kind = running_status & 0xF0;
            uint8_t d1 = b;
            p++;
            uint8_t d2 = 0;
            if (kind != 0xC0 && kind != 0xD0) {
                d2 = (p < end) ? *p++ : 0;
            }
            if (emit) emit(abs_tick, seq_base + count, running_status, d1, d2, 0, 0, 0, EKIND_MIDI, ctx);
            count++;
        }
    }
    return count;
}

static void count_cb(uint32_t abs_tick, uint32_t seq, uint8_t status, uint8_t d1, uint8_t d2,
                      const uint8_t *sd, uint32_t sl, uint32_t tempo, uint8_t kind, void *ctx) {
    (void)abs_tick; (void)seq; (void)status; (void)d1; (void)d2; (void)sd; (void)sl; (void)tempo; (void)kind; (void)ctx;
}

typedef struct { Event *arr; uint32_t idx; } FillCtx;

static void fill_cb(uint32_t abs_tick, uint32_t seq, uint8_t status, uint8_t d1, uint8_t d2,
                     const uint8_t *sd, uint32_t sl, uint32_t tempo, uint8_t kind, void *ctxp) {
    FillCtx *ctx = (FillCtx *)ctxp;
    Event *e = &ctx->arr[ctx->idx++];
    e->abs_tick = abs_tick;
    e->seq = seq;
    e->kind = kind;
    e->status = status; e->d1 = d1; e->d2 = d2;
    e->data = sd; e->data_len = sl;
    e->tempo_usec = tempo;
    e->sample_time = 0;
}

/* SPEC.md S4.2.1/S4.7: the Bank/Program pair is the one scheduled-controller
 * queue whose value a note-on reads back, and the read is a LOOK-AHEAD -- at
 * one timestamp the note takes the last Program Change BEFORE THE NEXT NOTE
 * EVENT of that timestamp, not the one that preceded it in the byte stream.
 * The forward reach and where it stops were measured separately; the reach
 * first, `[M: probe 40]`:
 * 13 of 13 cases, every one within 1.9 dB of its control against 28-49 dB of
 * separation between the three controls (000:080 / 001:080 / 008:080, all
 * program 80 so only the bank moves). The two cases that separate this from
 * plain stream order are F (`b1 pc80, on60, b8 pc80` -> 008:080) and G
 * (`b8 pc80, on60, b1 pc80` -> 001:080); stream order scores 11/13 and an
 * order-independent "highest bank wins" reading 8/13. Probe 38 says the same
 * for the program half, 7 of its 8 cases, including its case G -- three
 * layered locales at one tick on three keys, serum_opening's own shape, which
 * collapses to the last group's patch on all three notes and sits 19 dB from
 * what stream order renders. (38's case F disagrees with 40's case G on
 * byte-identical input; 1 outlier in 21 observations across the two captures,
 * unexplained and still open -- not yet written up in SPEC_GAPS.md.)
 *
 * Probe 40's K settles that this is the PROGRAM CHANGE queue and not the bank
 * byte: two Bank Selects with ONE Program Change and no queue tie at all reads
 * the last bank, in stream order. L/M settle that a Bank Select with no
 * Program Change after it never reaches the note, so there is no live bank
 * read at voice-render time either.
 *
 * Where the reach STOPS is probe 42, which restages 40's cases across separate
 * tracks the way a sequencer lays out layered parts, and adds the two shapes
 * 40 structurally cannot have: more than one note in a tick. Its F/G still say
 * the read passes over a following group (3.67/3.29 dB against 28.65/30.69 for
 * plain stream order), so the scope is the merged tick and not the track. But
 * H (three groups, three keys) and J (serum_opening's Type B shape -- two
 * tracks playing both keys, a third playing only the upper one, the last group
 * the sine) both refute reaching all the way to the end of the tick: 3.15 and
 * 2.40 dB for stopping at the next note, against 12.05 and 23.31 for running
 * to the end. Probe 40 cannot see the difference, 13/13 either way, because it
 * never puts two notes in one tick. Probe means, ref-vs-build, for
 * stream / reach-to-end / stop-at-next-note: 42 is 8.98/5.68/2.70, 38 is
 * 10.20/9.01/8.89 (38's case G improves 9.46 -> 8.55).
 *
 * THIS IS AN EMPIRICAL FIT, NOT THE DRIVER'S MECHANISM. Read from
 * artifacts/swmidi.sys, the driver is unambiguously reach-to-end:
 *   - `[A:0x17fa2]` the KSMUSICFORMAT parser advances its 64-bit timestamp
 *     per BLOCK (`add [ebp-0x18],eax / adc [ebp-0x14],edx`, 0x18073), never
 *     between messages inside one block, so a tick's messages tie exactly.
 *   - `[A:0x13667]` note-on/off queue into device+0x150 and `[A:0x132d2]`
 *     Program Change schedules into device+0xc50+part*0x28, same timestamp.
 *   - `[A:0x16bae]` insert walks PAST every equal-timestamp node -- FIFO.
 *   - `[A:0x12bd6]` the drain, called once per audio block from the render
 *     callback `[A:0x130af]` with now+nframes, pops every due event in a loop
 *     (0x1302a -> 0x16cac -> jne 0x12c0b).
 *   - `[A:0x12dbc]` each note-on reads its locale at its OWN event timestamp
 *     via 0x16daa -> `[A:0x16c50]`, which walks the whole list and returns the
 *     LAST node with ts <= req (default: object+0x18).
 * Nothing there stops at a note. What the driver does NOT have is any
 * serialisation between MIDI parsing and rendering -- they are separate KS pin
 * entry points (dispatch table at 0x1cbd4) -- so a note-on can drain before
 * the rest of its tick has been submitted. That is the likeliest source of
 * what probes 38/42 measured, and the only explanation offered so far for
 * probe 38's case F answering differently to byte-identical input in two
 * captures. Treat what follows as fitted to the captures we have; if the
 * references are re-recorded and 42's H/J stop reproducing, delete it and go
 * back to reach-to-end.
 *
 * So the narrowest change that reproduces the captures: within one timestamp,
 * each NOTE
 * event slides right over the run of Bank Select / Program Change events
 * immediately following it, preserving their order among themselves so each
 * Program Change still latches the bank byte that was live when it ran.
 * Nothing else moves -- not SysEx, not tempo, not the other five S4.2.1
 * queues. Moving those too was measured and is wrong twice over: it puts
 * controllers ahead of a tick-0 GS Reset (wiping it) and probe 37 already
 * pinned same-tick CC7/CC121 behaviour a blanket hoist would disturb.
 *
 * On serum_opening this restores the Type B saw layer the reach-to-end reading
 * deleted: its tick is `[t4 b1 pc80][n3][n15][t5 b1 pc81][n3'][n15'][t6 b8
 * pc80][n15'']`, and stopping at the next note leaves n3' on 001:081, which
 * survives the same-key choke. Both gestures improve over reach-to-end (Type A
 * 10.8 -> 9.3, Type B 25.2 -> 16.7 dB rms band error) and the two field files
 * densest in same-tick groups stop regressing (GENERAL_SERUM -19.24 -> -19.54,
 * CrystalOscillator -24.66 -> -24.96). Type B is still 16.7 dB out, now
 * uniformly too LOUD rather than too quiet -- a level/patch question, not an
 * ordering one, and unresolved.
 *
 * A/B guard for measurement only, same role as synth.c's SYNTH_SCHEDULE_LOCALE:
 * 0 restores plain stream order. Not exposed by the Makefile. */
#ifndef SMF_BANKPROG_LOOKAHEAD
#define SMF_BANKPROG_LOOKAHEAD 1
#endif

/* Audio service block, SPEC.adoc S6.6 / `[A:0x13054]`. The driver renders one
 * audio buffer per call and drains EVERY event due anywhere inside that buffer
 * before rendering a single sample of it -- `0x12bd6` is called with
 * `pos + nframes` `[A:0x130a3]`-`[A:0x130af]`, not with `pos`. That one fact is
 * what makes a note-off take effect from the START of the block containing it,
 * which is the early release probe 46 measures and which nothing in the
 * envelope code could explain. Matches render.c's GAIN_SEGMENT_FRAMES
 * deliberately -- same buffer, two halves of one mechanism; see that macro for
 * the corpus sweep behind the length, and for why 512 ships even though the
 * corpus prefers 128. */
#ifndef SERVICE_BLOCK_FRAMES
#define SERVICE_BLOCK_FRAMES (512u * RESAMPLE_FACTOR)
#endif

static int is_note_event(const Event *e) {
    if (e->kind != EKIND_MIDI) return 0;
    if ((e->status & 0xF0) == 0x80 || (e->status & 0xF0) == 0x90) return 1;
    return 0;
}

static int is_bankprog_event(const Event *e) {
    if (e->kind != EKIND_MIDI) return 0;
    if ((e->status & 0xF0) == 0xC0) return 1;
    return (e->status & 0xF0) == 0xB0 && (e->d1 == 0 || e->d1 == 32);
}

/* One pass per equal-tick run: each note event bubbles right over the run of
 * Bank Select / Program Change events immediately following it, stopping at
 * another note or at anything else (SysEx, tempo, any other CC, pitch bend). */
static void slide_notes_past_bankprog(Event *arr, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint32_t end = i;
        while (end < n && arr[end].abs_tick == arr[i].abs_tick) end++;
        for (uint32_t k = i; k < end; k++) {
            if (!is_note_event(&arr[k])) continue;
            uint32_t j = k;
            while (j + 1 < end && is_bankprog_event(&arr[j + 1])) {
                Event tmp = arr[j]; arr[j] = arr[j + 1]; arr[j + 1] = tmp;
                j++;
            }
        }
        i = end;
    }
}

static int event_less(const Event *a, const Event *b) {
    if (a->abs_tick != b->abs_tick) return a->abs_tick < b->abs_tick;
    return a->seq < b->seq;
}

/* Stable bottom-up merge sort using one scratch buffer (bump-allocated once). */
static void merge_sort_events(Event *arr, uint32_t n) {
    if (n < 2) return;
    Event *tmp = (Event *)rt_alloc(n * sizeof(Event));
    for (uint32_t width = 1; width < n; width *= 2) {
        for (uint32_t i = 0; i < n; i += 2 * width) {
            uint32_t left = i, mid = i + width < n ? i + width : n, right = i + 2 * width < n ? i + 2 * width : n;
            uint32_t a = left, b = mid, k = left;
            while (a < mid && b < right) {
                if (event_less(&arr[a], &arr[b])) tmp[k++] = arr[a++];
                else tmp[k++] = arr[b++];
            }
            while (a < mid) tmp[k++] = arr[a++];
            while (b < right) tmp[k++] = arr[b++];
        }
        for (uint32_t i = 0; i < n; i++) arr[i] = tmp[i];
    }
}

int smf_load(const uint8_t *data, uint32_t len) {
    g_loaded = 0;
    g_events = 0;
    g_event_count = 0;
    g_song_length_samples = 0;
    smf_rewind(); /* a new song starts at its own tick 0, not wherever the previous one stopped */

    if (len < 14) return -1;
    if (!fourcc_is(data, 'M', 'T', 'h', 'd')) return -1;
    uint32_t hdr_len = rd_u32be(data + 4);
    if (hdr_len < 6 || 8 + hdr_len > len) return -1;
    uint16_t format = rd_u16be(data + 8);
    uint16_t ntracks = rd_u16be(data + 10);
    uint16_t division = rd_u16be(data + 12);
    (void)format;

    const uint8_t *p = data + 8 + hdr_len;
    const uint8_t *end = data + len;

    /* Pass 1: locate each track's byte range, and count events. */
    const uint8_t *track_ptrs[MAX_TRACKS];
    uint32_t track_lens[MAX_TRACKS];
    uint32_t nt = 0;
    while (p + 8 <= end && nt < MAX_TRACKS && nt < ntracks) {
        if (!fourcc_is(p, 'M', 'T', 'r', 'k')) break;
        uint32_t tlen = rd_u32be(p + 4);
        const uint8_t *tdata = p + 8;
        if (tdata + tlen > end) break;
        track_ptrs[nt] = tdata;
        track_lens[nt] = tlen;
        nt++;
        p = tdata + tlen;
    }

    uint32_t total = 0;
    uint32_t seq_base = 0;
    uint32_t track_seq_bases[MAX_TRACKS];
    for (uint32_t i = 0; i < nt; i++) {
        track_seq_bases[i] = seq_base;
        uint32_t c = walk_track(track_ptrs[i], track_lens[i], 0, 0, seq_base);
        total += c;
        seq_base += c + 1;
    }

    if (total == 0) {
        g_loaded = 1; /* an empty song is a valid (trivially silent) load */
        return 0;
    }

    Event *arr = (Event *)rt_alloc(total * sizeof(Event));
    FillCtx fc; fc.arr = arr; fc.idx = 0;
    for (uint32_t i = 0; i < nt; i++) {
        walk_track(track_ptrs[i], track_lens[i], fill_cb, &fc, track_seq_bases[i]);
    }

    merge_sort_events(arr, total);
#if SMF_BANKPROG_LOOKAHEAD
    slide_notes_past_bankprog(arr, total);
#endif

    /* Tempo map pass: convert abs_tick -> sample_time. */
    int smpte = (division & 0x8000) != 0;
    double ticks_per_second = 0.0;
    if (smpte) {
        int fps = -(int8_t)(division >> 8);
        int tpf = division & 0xFF;
        if (fps <= 0) fps = 30;
        ticks_per_second = (double)fps * (double)tpf;
    }
    uint32_t division_q = smpte ? 1 : (division & 0x7FFF);
    if (division_q == 0) division_q = 24;

    double time_accum = 0.0;
    uint32_t last_tick = 0;
    uint32_t cur_tempo = 500000;

    for (uint32_t i = 0; i < total; i++) {
        Event *e = &arr[i];
        uint32_t delta_ticks = e->abs_tick - last_tick;
        if (smpte) {
            time_accum = (double)e->abs_tick / ticks_per_second;
        } else {
            double seconds_per_tick = ((double)cur_tempo / 1000000.0) / (double)division_q;
            time_accum += (double)delta_ticks * seconds_per_tick;
        }
        last_tick = e->abs_tick;
        double st = time_accum * (double)RENDER_RATE + 0.5;
        e->sample_time = (st < 0.0) ? 0 : (uint32_t)st;
        if (e->kind == EKIND_TEMPO) cur_tempo = e->tempo_usec;
    }

    g_events = arr;
    g_event_count = total;
    g_song_length_samples = arr[total - 1].sample_time;
    g_loaded = 1;
    return 0;
}

void smf_set_loop(int32_t loops) {
    g_loop_remaining = loops;
}

static void dispatch_event(Event *e) {
    switch (e->kind) {
        case EKIND_MIDI:
            return synth_midi(e->status, e->d1, e->d2);
        case EKIND_SYSEX:
            return synth_sysex(e->data, e->data_len);
        case EKIND_TEMPO:
        /* no action, already folded into sample_time precompute */
            break;
    }
}

/* Rewinds the sequencer to the top of the loaded song. Clearing g_finished
 * here is what makes a *second* playback possible at all: it is a latch, set
 * once the last note of a song has rung out, and nothing else ever clears it
 * -- so without this a reloaded or replayed song reports "finished" on its
 * very first render call and the host stops immediately. */
void smf_rewind(void) {
    g_event_index = 0;
    g_sample_pos = 0;
    g_finished = 0;
}

uint32_t smf_render(int16_t *out, uint32_t frames) {
    if (!g_loaded) {
        /* No SMF loaded: msgs_render still renders whatever voices are
         * active from direct msgs_midi() injection (ABI requirement --
         * msgs_render must work standalone, not only when a song is
         * loaded). */
        render_frames(out, frames);
        return frames;
    }

    uint32_t produced = 0;
    while (produced < frames) {
        while (g_event_index < g_event_count && g_events[g_event_index].sample_time <= g_sample_pos) {
            dispatch_event(&g_events[g_event_index]);
            g_event_index++;
        }

        int song_ended = (g_event_index >= g_event_count) && (g_sample_pos >= g_song_length_samples);
        if (song_ended) {
            if (g_loop_remaining != 0) {
                if (g_loop_remaining > 0) g_loop_remaining--;
                smf_rewind();
                continue;
            } else {
                render_frames(out + produced * 2, frames - produced);
                produced = frames;
                if (!voice_any_active()) g_finished = 1;
                break;
            }
        }

        /* Blocks are anchored to the absolute sample position, not to wherever
         * this call happens to start, so the grid does not move with the host's
         * buffer size. */
        uint32_t chunk = SERVICE_BLOCK_FRAMES - (g_sample_pos % SERVICE_BLOCK_FRAMES);
        if (chunk > frames - produced) chunk = frames - produced;

        if (chunk == 0) chunk = 1;

        /* Drain-ahead: every event due inside this block acts NOW, before a
         * sample of it is rendered. Each carries the offset it really falls at,
         * which only a note-on uses -- that is the whole note-on/note-off
         * asymmetry the reference shows, in two lines. */
        while (g_event_index < g_event_count &&
               g_events[g_event_index].sample_time < g_sample_pos + chunk) {
            voice_set_event_offset(g_events[g_event_index].sample_time - g_sample_pos);
            dispatch_event(&g_events[g_event_index]);
            g_event_index++;
        }
        voice_set_event_offset(0);
        render_frames(out + produced * 2, chunk);
        produced += chunk;
        g_sample_pos += chunk;
    }
    return produced;
}

int32_t smf_is_finished(void) {
    return g_finished;
}
