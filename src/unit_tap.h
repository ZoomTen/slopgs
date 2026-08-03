/* unit_tap.h -- minimal TAP version 14 emitter. */
#ifndef UNIT_TAP_H
#define UNIT_TAP_H

void tap_begin(void); /* prints "TAP version 14" */
int tap_ok_at(int cond, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
int tap_is_int_at(long long got, long long want, const char *file, int line,
                  const char *fmt, ...) __attribute__((format(printf, 5, 6)));
int tap_is_near_at(double got, double want, double tol, const char *file,
                   int line, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
void tap_skip(const char *reason, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void tap_todo_at(int cond, const char *reason, const char *file, int line,
                 const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void tap_diag(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void tap_bail(const char *fmt, ...) /* "Bail out! ..." then exit(1) */
    __attribute__((format(printf, 1, 2)));
/* prints the trailing "1..N" plan; returns exit status */
int tap_done(void);

#define ok(c, ...) tap_ok_at((c), __FILE__, __LINE__, __VA_ARGS__)
#define is_int(g, w, ...)                                                      \
    tap_is_int_at((long long) (g), (long long) (w), __FILE__, __LINE__,        \
                  __VA_ARGS__)
#define is_near(g, w, t, ...)                                                  \
    tap_is_near_at((double) (g), (double) (w), (double) (t), __FILE__,         \
                   __LINE__, __VA_ARGS__)
#define todo_ok(c, r, ...)                                                     \
    tap_todo_at((c), (r), __FILE__, __LINE__, __VA_ARGS__)
#endif
