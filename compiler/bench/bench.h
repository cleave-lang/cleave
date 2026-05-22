/* Minimal microbenchmark harness for the Cleave compiler.
 *
 * Each bench binary is standalone: include this header, call BENCH(name, code)
 * macros from main(), exit 0.
 *
 * Output format (one line per benchmark, machine-parseable):
 *   BENCH name=<name> iters=<N> ops_per_sec=<N>
 *
 * Per-bench protocol:
 *   - 100 warmup iterations (not measured)
 *   - Time iterations in batches of 100 until at least 0.5s of wall time
 *     has accumulated.
 *   - Report iters/seconds as ops_per_sec.
 *
 * No external dependencies. Uses CLOCK_MONOTONIC (POSIX). */

#ifndef CLEAVE_BENCH_H
#define CLEAVE_BENCH_H

/* clock_gettime requires POSIX.1-2008 visibility. _POSIX_C_SOURCE is defined
 * in the Makefile (-D flag) so include order in callers doesn't matter. */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double bench_now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#define BENCH_WARMUP    100
#define BENCH_BATCH     100
#define BENCH_TARGET_S  0.5

/* BENCH(name, block):
 *   name  - string literal (no spaces)
 *   block - any statement(s) doing the work to measure. Should be
 *           self-contained (re-create state each iteration).
 */
#define BENCH(name, block)                                                     \
    do {                                                                       \
        for (long _w = 0; _w < BENCH_WARMUP; ++_w) {                           \
            block;                                                             \
        }                                                                      \
        long _iters = 0;                                                       \
        double _start = bench_now_seconds();                                   \
        double _elapsed = 0.0;                                                 \
        while (_elapsed < BENCH_TARGET_S) {                                    \
            for (long _b = 0; _b < BENCH_BATCH; ++_b) {                        \
                block;                                                         \
                _iters++;                                                      \
            }                                                                  \
            _elapsed = bench_now_seconds() - _start;                           \
        }                                                                      \
        double _ops = (double)_iters / _elapsed;                               \
        printf("BENCH name=%s iters=%ld ops_per_sec=%.0f\n",                   \
               name, _iters, _ops);                                            \
    } while (0)

#endif /* CLEAVE_BENCH_H */
