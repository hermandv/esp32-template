/* Minimal test helpers for host unit tests (no dependencies). */
#pragma once

#include <stdio.h>
#include <stdlib.h>

extern int ut_failures;
extern int ut_checks;
extern const char *ut_current;

#define UT_CHECK(cond)                                                                       \
    do {                                                                                     \
        ut_checks++;                                                                         \
        if (!(cond)) {                                                                       \
            ut_failures++;                                                                   \
            printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, ut_current, #cond);         \
        }                                                                                    \
    } while (0)

#define UT_EQ(a, b)                                                                          \
    do {                                                                                     \
        long long _a = (long long)(a), _b = (long long)(b);                                  \
        ut_checks++;                                                                         \
        if (_a != _b) {                                                                      \
            ut_failures++;                                                                   \
            printf("  FAIL %s:%d [%s] %s == %s (%lld != %lld)\n", __FILE__, __LINE__,        \
                   ut_current, #a, #b, _a, _b);                                              \
        }                                                                                    \
    } while (0)

#define UT_RUN(fn)                                                                           \
    do {                                                                                     \
        ut_current = #fn;                                                                    \
        int _before = ut_failures;                                                           \
        fn();                                                                                \
        printf("%s %s\n", (ut_failures == _before) ? "ok  " : "FAIL", #fn);                 \
    } while (0)

#define UT_MAIN_DEFS                                                                         \
    int ut_failures = 0;                                                                     \
    int ut_checks = 0;                                                                       \
    const char *ut_current = "";

#define UT_SUMMARY()                                                                         \
    (printf("\n%d checks, %d failures\n", ut_checks, ut_failures), (ut_failures == 0) ? 0 : 1)
