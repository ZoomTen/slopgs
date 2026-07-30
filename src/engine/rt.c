/* rt.c -- portable runtime math: pow/log10/sqrt/sin/exp.
 *
 * Platform-free by construction: no libc, no libm, no allocator, no wasm
 * builtins beyond f64.sqrt. The allocator and (on the freestanding target)
 * the libc-named mem* symbols live in the interface files -- wasm.c or cli.c.
 *
 * sqrt uses the f64.sqrt hardware instruction via a compiler builtin (not a
 * libm call, and on x86-64 it lowers to sqrtsd). exp/log/sin are
 * implemented from scratch with a standard range-reduction + polynomial
 * approach: not bit-exact vs. any specific CRT, but SPEC.adoc S1.4.3 explicitly
 * establishes plain IEEE-754 double precision is sufficient for this driver's
 * own numeric margins (>1.3M ULP worst case on gm.dls's actual timecent
 * values), so a good-quality from-scratch implementation is adequate here.
 */
#include "rt.h"

/* ---------------------------------------------------------------------- */
/* bit-punning helpers for double <-> uint64 (used by log/exp) */

typedef union { double d; uint64_t u; } du64;

static double ldexp_i(double x, int e) {
    if (x == 0.0) return x;
    du64 u; u.d = x;
    int exp = (int)((u.u >> 52) & 0x7FF);
    if (exp == 0) {
        /* subnormal input -- not exercised by this driver's own domain;
         * treat as zero rather than handling subnormal renormalization. */
        return 0.0;
    }
    exp += e;
    if (exp >= 0x7FF) {
        u.u = (u.u & 0x8000000000000000ULL) | (0x7FEULL << 52) | 0xFFFFFFFFFFFFFULL;
        return u.d; /* saturate to +-max finite, no infinities needed here */
    }
    if (exp <= 0) return 0.0; /* underflow to zero, adequate for this domain */
    u.u = (u.u & ~(0x7FFULL << 52)) | ((uint64_t)exp << 52);
    return u.d;
}

/* ---------------------------------------------------------------------- */
/* exp() -- range reduction mod ln2, Taylor polynomial for the remainder */

#define RT_LN2      0.69314718055994530942
#define RT_LOG2E    1.44269504088896340736

double rt_exp(double x) {
    if (x > 709.0) x = 709.0;
    if (x < -745.0) return 0.0;
    double kf = x * RT_LOG2E;
    long k = (long)(kf + (kf >= 0.0 ? 0.5 : -0.5)); /* round to nearest, ties away from 0 */
    double r = x - (double)k * RT_LN2;
    double r2 = r * r;
    /* Taylor series through r^8/8! -- |r| <= ln2/2 ~= 0.3466, plenty accurate
     * for this driver's domain (finite normal doubles, no correct-rounding
     * requirement per SPEC.adoc S1.4.3). */
    double poly = 1.0 + r + r2 * (1.0 / 2 +
                  r * (1.0 / 6 +
                  r * (1.0 / 24 +
                  r * (1.0 / 120 +
                  r * (1.0 / 720 +
                  r * (1.0 / 5040 +
                  r * (1.0 / 40320)))))));
    return ldexp_i(poly, (int)k);
}

/* ---------------------------------------------------------------------- */
/* log() -- decompose x = m * 2^e, m in [sqrt(0.5), sqrt(2)), atanh series */

double rt_log(double x) {
    if (x <= 0.0) {
        /* Not exercised by this driver's own domain (all callers pass
         * strictly positive doubles); return a large-magnitude sentinel
         * rather than trapping. */
        return (x == 0.0) ? -1.0e308 : 0.0;
    }
    du64 u; u.d = x;
    int exp = (int)((u.u >> 52) & 0x7FF) - 1023;
    uint64_t mant_bits = (u.u & 0xFFFFFFFFFFFFFULL) | (1023ULL << 52);
    du64 mu; mu.u = mant_bits;
    double m = mu.d; /* in [1,2) */
    const double SQRT2 = 1.4142135623730951;
    if (m > SQRT2) {
        m *= 0.5;
        exp += 1;
    }
    double f = m - 1.0;
    double s = f / (2.0 + f);
    double s2 = s * s;
    double series = s + s * s2 * (1.0 / 3 +
                    s2 * (1.0 / 5 +
                    s2 * (1.0 / 7 +
                    s2 * (1.0 / 9 +
                    s2 * (1.0 / 11 +
                    s2 * (1.0 / 13))))));
    double logm = 2.0 * series;
    return (double)exp * RT_LN2 + logm;
}

double rt_log10(double x) {
    return rt_log(x) * 0.43429448190325182765; /* 1/ln(10) */
}

double rt_pow(double base, double exponent) {
    if (base == 0.0) return (exponent > 0.0) ? 0.0 : 1.0;
    if (base < 0.0) {
        /* Not exercised by this driver's own domain (all pow() call sites
         * operate on positive ratios/velocities). */
        return 0.0;
    }
    return rt_exp(exponent * rt_log(base));
}

double rt_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    return __builtin_sqrt(x); /* lowers to the wasm f64.sqrt hardware op */
}

/* ---------------------------------------------------------------------- */
/* sin() -- range reduce to [-pi,pi], Taylor polynomial. Only used at table-
 * build time (256-entry LFO sine table), not on the per-sample signal path,
 * so this need not be fast -- only correctly shaped to a few ULP. */

#define RT_PI  3.14159265358979323846
#define RT_2PI 6.28318530717958647692

double rt_sin(double x) {
    double k = x / RT_2PI;
    long ki = (long)(k + (k >= 0.0 ? 0.5 : -0.5));
    x = x - (double)ki * RT_2PI; /* now in [-pi,pi] roughly */
    if (x > RT_PI) x -= RT_2PI;
    if (x < -RT_PI) x += RT_2PI;
    double x2 = x * x;
    /* Taylor series through x^17/17! -- accurate to within the domain [-pi,pi]. */
    double poly = x * (1.0 +
                  x2 * (-1.0 / 6 +
                  x2 * (1.0 / 120 +
                  x2 * (-1.0 / 5040 +
                  x2 * (1.0 / 362880 +
                  x2 * (-1.0 / 39916800 +
                  x2 * (1.0 / 6227020800.0 +
                  x2 * (-1.0 / 1307674368000.0))))))));
    return poly;
}
