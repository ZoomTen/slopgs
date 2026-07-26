/* rt.h -- runtime primitives the core synth needs.
 * SPEC.md Part 1 S1.5.1: "the module must provide, itself, whatever runtime
 * primitives it needs: memcpy, memset, pow, log10, sqrt, sin, and a bump
 * allocator."
 *
 * rt.c implements the *portable* half (math only -- no libc, no libm). The
 * allocator, and for the freestanding target the libc-named memcpy/memset/
 * memmove symbols, are per-interface: wasm.c (bump allocator over linear
 * memory) or cli.c (malloc). */
#ifndef RT_H
#define RT_H

#include <stdint.h>
#include <stddef.h>

/* math primitives, plain IEEE-754 double, not bit-exact vs MSVC CRT
 * (SPEC.md S1.4.3: not required to be -- margin is >1.3M ULP on gm.dls). */
double rt_pow(double base, double exponent);
double rt_log(double x);
double rt_log10(double x);
double rt_sqrt(double x);
double rt_sin(double x);
double rt_exp(double x);

/* Allocator. Never freed and never rewound -- gm.dls and any loaded SMF must
 * survive msgs_reset(), which is a control-plane reset, not a memory one.
 * Implemented per-interface: wasm.c bumps over linear memory (grown via
 * memory.grow, a core wasm instruction needing no host import); cli.c mallocs.
 * Returns a real pointer so the same core code builds on wasm32 and on a
 * 64-bit host, where an allocation offset would not fit in a uint32. */
void    *rt_alloc(uint32_t nbytes);
uint32_t rt_mem_size(void); /* total bytes handed out so far */

#endif /* RT_H */
