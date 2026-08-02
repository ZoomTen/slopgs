/* tables.c -- runtime-built lookup tables, SPEC.adoc Appendix T.
 *
 * Every table below is built with the *truncating* toward-zero conversion
 * (SPEC.adoc S1.4.2: "every float->int conversion truncates toward zero,
 * never rounds" -- the driver's own 0x106e0 helper). A plain C `(int32_t)`
 * cast on a double truncates toward zero per the C standard, which is
 * exactly this semantic -- no rounding intrinsic is used anywhere here.
 */
#include "tables.h"
#include "rt.h"

int32_t g_table_cents[201];
int32_t g_table_semi[97];
int32_t g_table_vel[128];
int32_t g_table_lin[128];
int32_t g_table_dbamp[1001];
int16_t g_table_envshape[201];
int16_t g_table_sine[256];
uint8_t g_table_companding[2048];

static int32_t trunc_i32(double x) {
    return (int32_t)x; /* C (int) cast: truncates toward zero */
}

void tables_build(void) {
    int i;

    /* T2: cents ratio table, n = -100..100 */
    for (i = -100; i <= 100; i++) {
        double v = 4096.0 * rt_pow(2.0, (double)i / 1200.0);
        g_table_cents[i + 100] = trunc_i32(v);
    }

    /* T3: semitone ratio table, n = -48..48 */
    for (i = -48; i <= 48; i++) {
        double v = 4096.0 * rt_pow(2.0, (double)i / 12.0);
        g_table_semi[i + 48] = trunc_i32(v);
    }

    /* Velocity/attenuation table (squared law), hundredths of a dB */
    g_table_vel[0] = -9600;
    for (i = 1; i <= 127; i++) {
        double ratio = (double)i / 127.0;
        double v = 1000.0 * rt_log10(rt_pow(ratio, 4.0));
        g_table_vel[i] = trunc_i32(v);
    }

    /* Linear/sqrt-law table: index 0 = v=0 floor, indices 1..127 = curve */
    g_table_lin[0] = -2500;
    for (i = 1; i <= 127; i++) {
        double ratio = (double)i / 127.0;
        double v = 1000.0 * rt_log10(ratio);
        g_table_lin[i] = trunc_i32(v);
    }

    /* T1: dB -> linear amplitude, n = -1000..0 */
    for (i = -1000; i <= 0; i++) {
        double v = 4095.0 * rt_sqrt(rt_pow(10.0, (double)i / 100.0));
        g_table_dbamp[i + 1000] = trunc_i32(v);
    }

    /* Table C: envelope/time-progress shaping curve, i = 0..200. SPEC.adoc
     * T.4: t[i] = trunc(1000 + log10((i/200)^2) * 10000 * (1/96)). The two
     * .rdata fraction constants driving this loop are float32 in the
     * original (0x11d04 = 1/200, 0x11cfc = 1/96); 1/200 in particular is
     * the float32 0.004999999888241291, not the exact decimal 0.005 --
     * using the exact decimal changes t[200] from SPEC's 999 to 1000, so
     * the rounding is load-bearing and is reproduced here as the exact
     * double promotion of that float32 bit pattern. */
    g_table_envshape[0] = 0;
    for (i = 1; i <= 200; i++) {
        double x = (double)i * 0.004999999888241291; /* 1/200 float32, 0x11d04 */
        double v = rt_log10(x * x) * 10000.0 * 0.010416666977107525 + 1000.0; /* 1/96 float32, 0x11cfc */
        g_table_envshape[i] = (int16_t)trunc_i32(v);
    }

    /* Table D: sine LFO, i = 0..255, amplitude +-100 */
    for (i = 0; i < 256; i++) {
        double phase = (double)i * 6.28318530717958647692 * (1.0 / 256.0);
        double v = rt_sin(phase) * 100.0;
        g_table_sine[i] = (int16_t)trunc_i32(v);
    }

    /* Table E: log-companding curve, i = 0..2047, range [0,127] */
    {
        double inv = 1.0 / rt_log10(8.0);
        for (i = 0; i < 2048; i++) {
            double x = 1.0 + (double)i * 7.0 * (1.0 / 2048.0);
            double v = (rt_log10(x) * 128.0) * inv;
            int32_t iv = trunc_i32(v);
            if (iv < 0) iv = 0;
            if (iv > 255) iv = 255;
            g_table_companding[i] = (uint8_t)iv;
        }
    }
}
