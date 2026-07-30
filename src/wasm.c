/* wasm.c -- the freestanding wasm32 interface: the exported ABI (SPEC.adoc
 * Part 1 S1.5.3), the bump allocator over this module's own linear memory,
 * and the libc-named mem* symbols the freestanding target has no libc for.
 *
 * Everything platform-specific to wasm lives here; rt.c is portable math and
 * the rest of src/engine/ is the platform-agnostic synth. cli.c is the other
 * interface (native renderer) and supplies its own allocator instead.
 */
#include "engine/msgs.h"
#include "engine/rt.h"
#include "engine/tables.h"
#include "engine/dls.h"
#include "engine/synth.h"
#include "engine/voice.h"
#include "engine/render.h"
#include "engine/smf.h"

#define WASM_EXPORT __attribute__((visibility("default")))

/* ---------------------------------------------------------------------- */
/* memcpy / memset / memmove -- real libc-named symbols so that any
 * compiler-synthesized call (struct copies, zero-init, etc.) resolves. */

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

/* ---------------------------------------------------------------------- */
/* bump allocator, growing this module's own linear memory via memory.grow */

extern unsigned char __heap_base;

#define WASM_PAGE_SIZE 65536u

static uint32_t g_arena_next = 0;
static int g_arena_ready = 0;

static void arena_init(void) {
    if (g_arena_ready) return;
    g_arena_next = ((uint32_t)(uintptr_t)&__heap_base + 7u) & ~7u;
    g_arena_ready = 1;
}

/* ponytail: bump-only, never frees -- each msgs_load_smf leaks its event array
 * and sort scratch (~a few hundred KB per song). Fine for a page that switches
 * songs by hand; add a free list or a per-song arena mark/release if something
 * ever loads songs in a loop. */
void *rt_alloc(uint32_t nbytes) {
    arena_init();
    nbytes = (nbytes + 7u) & ~7u;
    uint32_t start = g_arena_next;
    uint32_t end = start + nbytes;
    uint32_t cur_bytes = (uint32_t)__builtin_wasm_memory_size(0) * WASM_PAGE_SIZE;
    if (end > cur_bytes) {
        uint32_t need = end - cur_bytes;
        uint32_t pages = (need + WASM_PAGE_SIZE - 1) / WASM_PAGE_SIZE;
        __builtin_wasm_memory_grow(0, pages);
    }
    g_arena_next = end;
    return (void *)(uintptr_t)start;
}

uint32_t rt_mem_size(void) {
    arena_init();
    return g_arena_next;
}

/* ---------------------------------------------------------------------- */
/* the ABI */

static int g_tables_built = 0;
static int g_dls_ok = 0;

WASM_EXPORT
uint32_t msgs_abi_version(void) {
    return 1;
}

/* The rate this build renders at: BASE_RATE * RESAMPLE_FACTOR (voice.h).
 * Additive to the required ABI, like msgs_debug_active_count below. Exported
 * so a host never has to keep its own copy of a build-time constant -- when
 * dist/compare.js did, a stale copy played every file at the wrong speed and
 * nothing anywhere could notice. */
WASM_EXPORT
uint32_t msgs_sample_rate(void) {
    return RENDER_RATE;
}

WASM_EXPORT
uint32_t msgs_mem_size(void) {
    return rt_mem_size();
}

WASM_EXPORT
uint32_t msgs_alloc(uint32_t nbytes) {
    return (uint32_t)(uintptr_t)rt_alloc(nbytes);
}

WASM_EXPORT
int32_t msgs_init(uint32_t dls_ptr, uint32_t dls_len) {
    if (!g_tables_built) {
        tables_build();
        g_tables_built = 1;
    }
    synth_reset();
    smf_set_loop(0);

    const uint8_t *dls_data = (const uint8_t *)(uintptr_t)dls_ptr;
    int rc = dls_load(dls_data, dls_len);
    g_dls_ok = (rc == 0);
    return rc == 0 ? 0 : rc;
}

/* Back to the state right after msgs_load_smf: channels at their defaults,
 * every voice silenced, the loaded song rewound to tick 0 and no longer
 * reporting finished. The sequencer rewind is what lets a host replay or
 * loop a song without reloading it -- msgs_is_finished() is a latch, so
 * resetting only the control plane would leave it stuck on. */
WASM_EXPORT
void msgs_reset(void) {
    synth_reset();
    smf_rewind();
}

WASM_EXPORT
int32_t msgs_load_smf(uint32_t smf_ptr, uint32_t smf_len) {
    const uint8_t *data = (const uint8_t *)(uintptr_t)smf_ptr;
    synth_reset();
    return smf_load(data, smf_len);
}

WASM_EXPORT
void msgs_set_loop(int32_t loops) {
    smf_set_loop(loops);
}

WASM_EXPORT
uint32_t msgs_render(uint32_t out_ptr, uint32_t frames) {
    int16_t *out = (int16_t *)(uintptr_t)out_ptr;
    return smf_render(out, frames);
}

WASM_EXPORT
int32_t msgs_is_finished(void) {
    return smf_is_finished();
}

WASM_EXPORT
void msgs_midi(uint32_t status, uint32_t d1, uint32_t d2) {
    synth_midi(status, d1, d2);
}

/* Temporary debug export for verifying the same-note retrigger fast-release
 * (SPEC.adoc S5.6/0x12ec6). Not part of the required 10-function ABI; harmless
 * to leave exported since it's additive and does not change any required
 * export's signature. Remove if a stricter export surface is desired. */
WASM_EXPORT
uint32_t msgs_debug_active_count(void) {
    extern Voice g_voices[NUM_VOICES];
    uint32_t n = 0;
    for (int i = 0; i < NUM_VOICES; i++) if (g_voices[i].active) n++;
    return n;
}
