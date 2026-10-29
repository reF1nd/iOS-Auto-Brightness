// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>

inline int g_failures = 0;

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                                         \
    }                                                                       \
  } while (0)

#define CHECK_EQ(actual, expected)                                          \
  do {                                                                      \
    const auto a_ = (actual);                                               \
    const auto e_ = (expected);                                             \
    if (!(a_ == e_)) {                                                      \
      std::fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #actual, \
                   static_cast<long long>(a_), static_cast<long long>(e_)); \
      ++g_failures;                                                         \
    }                                                                       \
  } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                             \
  do {                                                                      \
    const double a_ = (actual);                                             \
    const double e_ = (expected);                                           \
    if (!(std::fabs(a_ - e_) <= (tolerance))) {                             \
      std::fprintf(stderr, "%s:%d: %s == %.6f, expected %.6f\n", __FILE__, __LINE__, #actual, a_, e_); \
      ++g_failures;                                                         \
    }                                                                       \
  } while (0)

inline int finish(const char* name) {
  if (g_failures) {
    std::fprintf(stderr, "%s: %d failure(s)\n", name, g_failures);
    return 1;
  }
  std::printf("%s: ok\n", name);
  return 0;
}
