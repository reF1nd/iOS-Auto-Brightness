// SPDX-License-Identifier: GPL-3.0-only
#include "curve.hpp"

#include <cmath>

namespace iab {

const float kNitsTable[52] = {
    0.222323999f, 4.44442701f, 6.89745378f, 11.746254f, 18.4992638f, 27.1982155f,
    37.0761871f, 47.7880821f, 60.8551521f, 75.8850784f, 91.5810089f, 106.566017f,
    118.267197f, 135.453842f, 153.958282f, 173.797348f, 194.969818f, 218.686035f,
    243.58606f, 270.287659f, 296.118011f, 324.675568f, 353.026825f, 383.425079f,
    415.1698f, 446.740448f, 465.361542f, 468.123108f, 471.112091f, 473.581757f,
    476.674683f, 479.022186f, 482.374146f, 485.223328f, 487.664429f, 490.82724f,
    492.951416f, 496.036407f, 499.129517f, 502.195435f, 505.346191f, 507.992096f,
    511.172577f, 514.392029f, 516.955383f, 520.755188f, 523.192139f, 526.367737f,
    529.734192f, 532.000549f, 535.321716f, 538.537659f,
};

namespace {
constexpr float kLn2 = 0.693147182f;
constexpr float kLn5 = 1.60943794f;
constexpr float kLn10 = 2.30258512f;
}  // namespace

float nits_for_lux(float lux) {
  // The reference evaluates the 0.5..2 lux segment as 2*log2(lux)+1, which is
  // discontinuous at 0.5 lux (1 nit -> -1 nit). It is kept for parity.
  if (lux <= 0.5f) return std::fmaf(lux, 1.556f, 0.222f);
  if (lux <= 2.0f) return std::fmaf(std::log(lux) / kLn2, 2.0f, 1.0f);
  if (lux <= 10.0f) return std::fmaf(std::log(lux * 0.5f) / kLn5, 12.0f, 3.0f);
  if (lux <= 100.0f) return std::fmaf(std::log(lux / 10.0f) / kLn10, 85.0f, 15.0f);
  if (lux <= 1000.0f) return std::fmaf(std::log(lux / 100.0f) / kLn10, 200.0f, 100.0f);
  if (lux <= 10000.0f) return std::fmaf(std::log(lux / 1000.0f) / kLn10, 200.0f, 300.0f);
  return std::fmaf(std::log(std::fmin(lux, 100000.0f) / 10000.0f) / kLn10, 38.5f, 500.0f);
}

int level_for_nits(float nits, int max_level) {
  if (!(nits > kNitsTable[0])) return 1;
  if (nits >= kNitsTable[51]) return max_level;
  for (int i = 0; i < 51; ++i) {
    const float low = kNitsTable[i], high = kNitsTable[i + 1];
    if (low > nits || high <= nits) continue;
    float position = (nits - low) / (high - low) + static_cast<float>(i);
    position = position * 160.60785f * static_cast<float>(max_level) / 8191.0f + 0.5f;
    const int level = static_cast<int>(position);
    if (level < 1) return 1;
    return level < max_level ? level : max_level;
  }
  return max_level;
}

int curve_level(float lux, int max_level) {
  if (!(lux > 0.0f)) return 1;
  return level_for_nits(nits_for_lux(lux), max_level);
}

int slider_level(int slider, float gamma, int max_level) {
  const float position = std::pow(static_cast<float>(slider) / 255.0f, gamma);
  return level_for_nits(std::fmaf(position, 538.316f, 0.222f), max_level);
}

}  // namespace iab
