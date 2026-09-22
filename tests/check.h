// Minimal assertion helpers shared by the unit tests (no external deps).
#pragma once

#include <cstdio>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                             \
        }                                                                                                              \
    } while (0)

#define TEST_SUMMARY()                                                                                                 \
    do {                                                                                                               \
        std::printf("  %d checks, %d failures\n", g_checks, g_failures);                                              \
        return g_failures == 0 ? 0 : 1;                                                                                \
    } while (0)
