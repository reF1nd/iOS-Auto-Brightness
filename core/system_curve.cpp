// SPDX-License-Identifier: GPL-3.0-only
#include "system_curve.hpp"

namespace iab {

SystemCurve::SystemCurve(const RefactorConfig& config, const PanelMap& panel)
    : controller_(config), panel_(panel.valid() ? panel : PanelMap()) {}

SystemCurve::SystemCurve(const std::vector<float>& lux, const std::vector<float>& nits, const PanelMap& panel)
    : table_(lux, nits), panel_(panel.valid() ? panel : PanelMap()) {}

float SystemCurve::nits(float lux) const {
  if (table_.valid()) return table_.interpolate(lux > 0.0f ? lux : 0.0f);
  return refactor_nits(controller_.config(), controller_.logical(lux), lux);
}

float SystemCurve::default_nits(float lux) const {
  if (table_.valid()) return nits(lux);
  return refactor_nits(controller_.config(), controller_.default_logical(lux), lux);
}

int SystemCurve::level(float nits, int max_level) const {
  const int level = static_cast<int>(panel_.brightness(nits) * static_cast<float>(max_level) + 0.5f);
  if (level < 1) return 1;
  return level < max_level ? level : max_level;
}

}  // namespace iab
