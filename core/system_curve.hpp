// SPDX-License-Identifier: GPL-3.0-only
// The system's own auto-brightness curve, lux -> nits -> panel level.
//
// With config_refactor_brightness_policy (HyperOS on altdb) the framework
// computes getRefactorBrightness: RefactorNitController anchors give the
// slider's logical nits for the ambient lux, getCurrentNit turns them into
// actual nits and the display config into brightness. Without it the AOSP
// mapper interpolates the BrightnessConfiguration points with a monotone
// cubic spline; that curve is followed but not learned.
#pragma once

#include <vector>

#include "brightness_map.hpp"
#include "nit_controller.hpp"

namespace iab {

class SystemCurve {
 public:
  SystemCurve(const RefactorConfig& config = RefactorConfig(), const PanelMap& panel = PanelMap());
  SystemCurve(const std::vector<float>& lux, const std::vector<float>& nits, const PanelMap& panel);

  // False for the AOSP table: drags are not learned there.
  bool learnable() const { return !table_.valid(); }
  float nits(float lux) const;
  float default_nits(float lux) const;
  // Panel level (1..max_level) showing these nits.
  int level(float nits, int max_level) const;

  NitController& controller() { return controller_; }
  const NitController& controller() const { return controller_; }
  const PanelMap& panel() const { return panel_; }

 private:
  NitController controller_;
  MonotoneSpline table_;
  PanelMap panel_;
};

}  // namespace iab
