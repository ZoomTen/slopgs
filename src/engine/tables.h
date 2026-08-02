/* tables.h -- runtime-built lookup tables, SPEC.adoc Appendix T. */
#ifndef TABLES_H
#define TABLES_H

#include <stdint.h>

/* T2: cents pitch-ratio table, domain n=-100..100 (201 entries), Q12.
 * table_cents[n+100] = trunc(4096 * 2^(n/1200)) */
extern int32_t g_table_cents[201];

/* T3: semitone pitch-ratio table, domain n=-48..48 (97 entries), Q12.
 * table_semi[n+48] = trunc(4096 * 2^(n/12)) */
extern int32_t g_table_semi[97];

/* Velocity/attenuation table (0x1c9d0 in SPEC.adoc), 128 entries, squared law,
 * units: hundredths of a dB. table_vel[0] = -9600 (hardcoded floor);
 * table_vel[v] = trunc(1000*log10((v/127)^4)) for v=1..127.
 * Reused for CC7 (Channel Volume), CC11 (Expression, per SPEC.adoc 3.5's
 * [M:probe]-carried claim), and Master Volume -- same table, per SPEC.adoc. */
extern int32_t g_table_vel[128];

/* Linear/sqrt-law table (0x1bfd4+0x1bfd0), 128 entries (index 0 = the v=0
 * floor scalar, indices 1..127 = the trunc(1000*log10(v/127)) curve).
 * Consumed by the pan law (SPEC.adoc 3.6), reverse-indexed. */
extern int32_t g_table_lin[128];

/* T1 (dB->linear amplitude), domain n=-1000..0 (1001 entries), Q12.
 * table_dbamp[n+1000] = trunc(4095*sqrt(10^(n/100))). Built for completeness
 * per SPEC.adoc's table-appendix requirement; not wired into the gain path
 * (gain is computed directly via rt_pow, see SPEC_LOG.adoc). */
extern int32_t g_table_dbamp[1001];

/* Table C (0x1a9d8), 201 entries, envelope/time-progress shaping curve.
 * SPEC.adoc S3.4.2/T.4: linear-amplitude ratio -> position on the envelope's
 * normalized 96 dB scale, read only by the attack segment of the original's
 * envelope evaluator (`0x18b15`, SPEC_LOG.adoc item46). Not a shape gap: the
 * port's attack ramps linearly in amplitude, algebraically the same curve
 * this table's map inverts to (SPEC_LOG.adoc item46). Unused by default;
 * see voice.c's ENV_ATTACK_TABLE_C for the guarded, measured-not-shipped
 * quantized-attack variant that does read it. */
extern int16_t g_table_envshape[201];

/* Table D (0x1a7d8), 256 entries, sine LFO, amplitude +-100. Built for
 * completeness; real-time LFO modulation is not implemented (SPEC_LOG.adoc). */
extern int16_t g_table_sine[256];

/* Table E (0x1c1d0), 2048 entries, log-companding curve for the 16->8 bit
 * sample-storage reduction path. Built for completeness; not needed because
 * this implementation always keeps samples at full 16-bit fidelity,
 * referenced in place (SPEC_LOG.adoc). */
extern uint8_t g_table_companding[2048];

void tables_build(void);

#endif /* TABLES_H */
