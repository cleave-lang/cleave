/* Minimal hand-rolled test framework for the Cleave compiler.
 *
 * Each test binary is a standalone executable: include this header,
 * write TEST(name) functions, run them from main() via RUN(name),
 * end main() with REPORT() to print summary + return non-zero on
 * any failure.
 *
 * No external dependencies. The framework is the whole file. */

#ifndef CLEAVE_TEST_H
#define CLEAVE_TEST_H

#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;
static const char *current_test = "?";

#define TEST(name) static void name(void)

#define RUN(name)                                                              \
    do {                                                                       \
        current_test = #name;                                                  \
        name();                                                                \
    } while (0)

/* FAIL_AT requires at least one variadic argument (intentional: pedantic-C11
 * disallows empty __VA_ARGS__). Callers should always pass the fmt args. */
#define FAIL_AT(fmt, ...)                                                      \
    do {                                                                       \
        tests_failed++;                                                        \
        fprintf(stderr, "  FAIL %s:%d in %s: " fmt "\n", __FILE__, __LINE__,   \
                current_test, __VA_ARGS__);                                    \
    } while (0)

#define ASSERT(cond)                                                           \
    do {                                                                       \
        tests_run++;                                                           \
        if (!(cond)) FAIL_AT("assertion '%s' failed", #cond);                  \
    } while (0)

#define ASSERT_EQ_INT(actual, expected)                                        \
    do {                                                                       \
        tests_run++;                                                           \
        long _a = (long)(actual), _e = (long)(expected);                       \
        if (_a != _e) FAIL_AT("expected %ld, got %ld", _e, _a);                \
    } while (0)

#define ASSERT_EQ_STR_LEN(actual, len, expected)                               \
    do {                                                                       \
        tests_run++;                                                           \
        size_t _el = strlen(expected);                                         \
        if ((size_t)(len) != _el ||                                            \
            memcmp((actual), (expected), _el) != 0) {                          \
            FAIL_AT("expected '%s', got '%.*s'", (expected), (int)(len),       \
                    (actual));                                                 \
        }                                                                      \
    } while (0)

#define REPORT()                                                               \
    do {                                                                       \
        int _pass = tests_run - tests_failed;                                  \
        printf("%s: %d assertions, %d passed, %d failed\n", __FILE__,          \
               tests_run, _pass, tests_failed);                                \
        return tests_failed == 0 ? 0 : 1;                                      \
    } while (0)

#endif /* CLEAVE_TEST_H */
