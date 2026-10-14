#pragma once
// Minimal test helpers: CHECK records failures and keeps going; main returns test_result().
#include <cmath>
#include <cstdio>

inline int g_failures = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs(double(a) - double(b)) <= double(eps))
inline int test_result() { return g_failures ? 1 : 0; }
