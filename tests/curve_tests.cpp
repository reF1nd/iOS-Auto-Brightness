// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "check.hpp"
#include "curve.hpp"

using namespace iab;

namespace {

// Double-precision curve of the v6.8 WebUI (app.js luxToNits/nitsToBl), an
// independent implementation by the reference author.
double reference_nits(double lux) {
  if (lux <= 0) return 0.222;
  if (lux <= 0.5) return 0.222 + lux * 1.556;
  if (lux <= 2) return 1 + std::log(lux) / std::log(2.0) * 2;
  if (lux <= 10) return 3 + std::log(lux / 2) / std::log(5.0) * 12;
  if (lux <= 100) return 15 + std::log(lux / 10) / std::log(10.0) * 85;
  if (lux <= 1000) return 100 + std::log(lux / 100) / std::log(10.0) * 200;
  if (lux <= 10000) return 300 + std::log(lux / 1000) / std::log(10.0) * 200;
  return 500 + std::log(std::fmin(lux, 100000) / 10000) / std::log(10.0) * 38.5;
}

int reference_level(double lux, int max) {
  const double nits = reference_nits(lux);
  if (nits <= kNitsTable[0]) return 1;
  if (nits >= kNitsTable[51]) return max;
  for (int i = 0; i < 51; ++i) {
    if (nits >= kNitsTable[i] && nits < kNitsTable[i + 1]) {
      const double t = (nits - kNitsTable[i]) / (kNitsTable[i + 1] - kNitsTable[i]);
      const double level = std::round((i + t) * (8191.0 / 51) * max / 8191);
      return static_cast<int>(std::fmax(1, std::fmin(max, level)));
    }
  }
  return max;
}

}  // namespace

int main() {
  constexpr int kMax = 16383;
  CHECK_EQ(curve_level(0.0f, kMax), 1);
  CHECK_EQ(curve_level(-3.0f, kMax), 1);
  CHECK_EQ(curve_level(1e7f, kMax), level_for_nits(538.5f, kMax));
  CHECK_EQ(level_for_nits(0.1f, kMax), 1);
  CHECK_EQ(level_for_nits(600.0f, kMax), kMax);
  for (int i = 0; i < 51; ++i) {
    CHECK_EQ(level_for_nits(kNitsTable[i], kMax),
             std::max(1, static_cast<int>(static_cast<float>(i) * 160.60785f * kMax / 8191.0f + 0.5f)));
  }
  // Reference quirk: 0.5..0.707 lux evaluates below the table and maps to level 1.
  CHECK_EQ(curve_level(0.5f, kMax), level_for_nits(1.0f, kMax));
  CHECK_EQ(curve_level(0.6f, kMax), 1);
  CHECK(curve_level(0.4f, kMax) > curve_level(0.6f, kMax));
  int previous = 0;
  for (double lux = 1.0; lux <= 100000.0; lux *= 1.05) {
    const int level = curve_level(static_cast<float>(lux), kMax);
    CHECK(level >= previous);
    previous = level;
    const int expected = reference_level(lux, kMax);
    if (std::abs(level - expected) > 1) {
      std::fprintf(stderr, "lux %.3f level %d reference %d\n", lux, level, expected);
      ++g_failures;
    }
    CHECK(std::abs(curve_level(static_cast<float>(lux), 2047) - reference_level(lux, 2047)) <= 1);
  }
  CHECK_EQ(slider_level(255, 2.2f, kMax), kMax);
  CHECK_EQ(slider_level(0, 2.2f, kMax), 1);
  CHECK(slider_level(128, 2.2f, kMax) > slider_level(64, 2.2f, kMax));
  CHECK_NEAR(nits_for_lux(10.0f), 15.0, 1e-4);
  CHECK_NEAR(nits_for_lux(1000.0f), 300.0, 1e-3);
  return finish("curve_tests");
}
