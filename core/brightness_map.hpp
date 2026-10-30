// SPDX-License-Identifier: GPL-3.0-only
// The system auto-brightness curve pieces: the panel's nits <-> brightness
// map, the HyperOS logical-nit slider (RefactorBrightnessUtil and
// RefactorNitController.getCurrentNit) and Android's monotone cubic spline.
#pragma once

#include <vector>

namespace iab {

// screenBrightnessMap of the display config: brightness 0..1 <-> nits, linear
// between points. A panel level is brightness * max level.
class PanelMap {
 public:
  PanelMap();  // Xiaomi 17 Pro Max (altdb) main display
  PanelMap(const std::vector<float>& brightness, const std::vector<float>& nits);

  bool valid() const { return brightness_.size() >= 2; }
  float nits(float brightness) const;
  float brightness(float nits) const;

 private:
  std::vector<float> brightness_, nits_;
};

// framework-ext-res values read by RefactorBrightnessUtil and
// RefactorNitController. The defaults are those of the altdb ROM.
struct RefactorConfig {
  float min_nit = 1.0f;       // dimen/config_minLogicalNit
  float max_nit = 1060.0f;    // dimen/config_maxLogicalNit
  float first_node = 30.0f;   // dimen/config_firstNitNode
  float second_node = 100.0f; // dimen/config_secondNitNode
  // array/config_firstFactorConfig and config_secondFactorConfig: six lux
  // breakpoints, then the factor at each of them.
  std::vector<float> first_factor{5, 30, 100, 500, 1000, 66520, 1, 2, 6, 6, 12, 80};
  std::vector<float> second_factor{1, 30, 1000, 3000, 5000, 8000, 0.24f, 0.52f, 0.52f, 0.58f, 0.64f, 1};
  // array/config_defaultLogicalCurve: [2] and [5] split the lux range into
  // three segments, [7] ends it; [1], [3] and [6] are the default anchors.
  std::vector<float> default_curve{0, 2, 30, 135, 0, 600, 220, 5000, 1060};
  std::vector<float> high_lux{8000, 17000, 37200, 72000};          // array/config_highLuxPoints
  std::vector<float> high_scale{1, 2.05189f, 2.05189f, 3.30189f};  // config_highLuxScalingFactorPoints
  std::vector<float> good_lux{0, 30, 1000, 5000};                  // array/config_goodCurveLuxPoints
  std::vector<float> good_nit{50, 300, 550, 1060};                 // array/config_goodCurveNitPoints
  // bool/config_brightness_reset_policy (an overlay sets it on altdb) and
  // not disabled by the cloud config.
  bool reset_policy = true;

  bool valid() const;
  float seg0() const { return default_curve[2]; }
  float seg1() const { return default_curve[5]; }
  float lux_max() const { return default_curve[7]; }
};

// android.util.Spline.LinearSpline: clamps outside the points.
float linear_spline(const std::vector<float>& x, const std::vector<float>& y, float at);

// getCurrentNit with auto brightness on: actual nits of a slider position
// (logical nits) at the ambient lux.
float refactor_nits(const RefactorConfig& config, float logical, float lux);

// android.util.Spline.MonotoneCubicSpline, used for the AOSP lux -> nits
// curve (BrightnessConfiguration) when the refactor policy is off.
class MonotoneSpline {
 public:
  MonotoneSpline() = default;
  MonotoneSpline(const std::vector<float>& x, const std::vector<float>& y);

  bool valid() const { return x_.size() >= 2; }
  float interpolate(float at) const;

 private:
  std::vector<float> x_, y_, m_;
};

}  // namespace iab
