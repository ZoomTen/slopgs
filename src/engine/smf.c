/* smf.c -- Standard MIDI File parser and sequencer. SPEC.md S1.5.4: not part
 * of the original driver, ordinary generic SMF handling. */
#include "smf.h"
#include "synth.h"
#include "voice.h"
#include "render.h"
#include "rt.h"

typedef struct Event {
    uint32_t abs_tick;
    uint32_t seq;
    uint8_t kind;      /* 0=midi short, 1=sysex, 2=tempo meta */
    uint8_t status, d1, d2;
    const uint8_t *data; /* sysex payload (kind==1) */
    uint32_t data_len;
    uint32_t tempo_usec; /* kind==2 */
    uint32_t sample_time; /* computed */
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
                if (emit) emit(abs_tick, seq_base + count, 0, 0, 0, 0, 0, usec, 2, ctx);
                count++;
            }
            /* other meta types (track name, end-of-track, etc.): ignored */
            p += mlen;
        } else if (b == 0xF0 || b == 0xF7) { /* sysex (and sysex continuation, treated the same) */
            p++;
            uint32_t slen = read_vlq(&p, end);
            if (p + slen > end) break;
            if (b == 0xF0) {
                if (emit) emit(abs_tick, seq_base + count, 0, 0, 0, p, slen, 0, 1, ctx);
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
            if (emit) emit(abs_tick, seq_base + count, running_status, d1, d2, 0, 0, 0, 0, ctx);
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
            if (emit) emit(abs_tick, seq_base + count, running_status, d1, d2, 0, 0, 0, 0, ctx);
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
    const uint8_t *track_ptrs[512];
    uint32_t track_lens[512];
    uint32_t nt = 0;
    while (p + 8 <= end && nt < 512 && nt < ntracks) {
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
    uint32_t track_seq_bases[512];
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
        double st = time_accum * 22050.0 + 0.5;
        e->sample_time = (st < 0.0) ? 0 : (uint32_t)st;
        if (e->kind == 2) cur_tempo = e->tempo_usec;
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
    if (e->kind == 0) {
        synth_midi(e->status, e->d1, e->d2);
    } else if (e->kind == 1) {
        synth_sysex(e->data, e->data_len);
    }
    /* kind==2 (tempo): already folded into the sample_time precompute, no
     * runtime action needed. */
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

        uint32_t next_event_sample = (g_event_index < g_event_count) ? g_events[g_event_index].sample_time : g_song_length_samples;
        uint32_t chunk = frames - produced;
        if (next_event_sample > g_sample_pos) {
            uint32_t until = next_event_sample - g_sample_pos;
            if (until < chunk) chunk = until;
        }
        if (chunk == 0) chunk = 1;
        render_frames(out + produced * 2, chunk);
        produced += chunk;
        g_sample_pos += chunk;
    }
    return produced;
}

int32_t smf_is_finished(void) {
    return g_finished;
}
