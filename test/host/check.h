#pragma once

/*
 * Minimal check harness shared by the host tests (one test binary per
 * pure module, each a single translation unit).
 *
 *   SCENARIO("...");          prints a scenario header
 *   CHECK(cond);              boolean check
 *   CHECK_EQ(a, b);           integer equality, prints both values
 *   CHECK_STR(a, b);          C-string equality (NULL-safe)
 *   return CHECK_SUMMARY();   prints "<n> checks, <m> failures", exit code
 */

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do {                                              \
    g_checks++;                                                       \
    if (!(cond)) {                                                    \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        g_failures++;                                                 \
    }                                                                 \
} while (0)

#define CHECK_EQ(a, b) do {                                           \
    g_checks++;                                                       \
    long long _a = (long long)(a), _b = (long long)(b);               \
    if (_a != _b) {                                                   \
        printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n",             \
               __FILE__, __LINE__, #a, #b, _a, _b);                   \
        g_failures++;                                                 \
    }                                                                 \
} while (0)

#define CHECK_STR(a, b) do {                                          \
    g_checks++;                                                       \
    const char *_a = (a), *_b = (b);                                  \
    if (!(_a == _b || (_a && _b && strcmp(_a, _b) == 0))) {           \
        printf("  FAIL %s:%d: %s == %s (\"%s\" != \"%s\")\n",         \
               __FILE__, __LINE__, #a, #b,                            \
               _a ? _a : "(null)", _b ? _b : "(null)");               \
        g_failures++;                                                 \
    }                                                                 \
} while (0)

#define SCENARIO(name) printf("--- %s\n", name)

#define CHECK_SUMMARY()                                               \
    (printf("\n%d checks, %d failures\n", g_checks, g_failures),      \
     g_failures ? 1 : 0)
