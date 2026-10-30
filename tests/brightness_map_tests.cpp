// SPDX-License-Identifier: GPL-3.0-only
#include <initializer_list>

#include "brightness_map.hpp"
#include "check.hpp"
#include "system_curve.hpp"

using namespace iab;

namespace {

void panel_map_follows_display_config() {
  const PanelMap panel;
  CHECK(panel.valid());
  // The framework reported 7.96 nits for brightness 0.008645099.
  CHECK_NEAR(panel.nits(0.008645099f), 7.96, 0.01);
  CHECK_NEAR(panel.brightness(7.96f), 0.008645, 1e-5);
  CHECK_NEAR(panel.nits(0.0f), 0.0, 1e-9);
  CHECK_NEAR(panel.brightness(3500.0f), 1.0, 1e-6);
  for (const float nits : {0.5f, 1.0f, 30.0f, 600.0f, 2500.0f}) {
    CHECK_NEAR(panel.nits(panel.brightness(nits)), nits, nits * 1e-4);
  }
  CHECK(!PanelMap({0.1f, 0.05f}, {1.0f, 2.0f}).valid());
  CHECK(!PanelMap({0.1f}, {1.0f}).valid());
}

void refactor_nits_matches_framework() {
  const RefactorConfig config;
  CHECK(config.valid());
  // Default anchors (lux, logical nits) against the framework's
  // BrightnessConfiguration; at 30 lux getCurrentNit also lifts the low end
  // (factor 2), 0.17 nits above the table.
  CHECK_NEAR(refactor_nits(config, 135.0f, 30.0f), 83.30, 0.01);
  CHECK_NEAR(refactor_nits(config, 220.0f, 600.0f), 126.14, 0.01);
  CHECK_NEAR(refactor_nits(config, 1060.0f, 5000.0f), 678.4, 0.01);
  CHECK_NEAR(refactor_nits(config, 1060.0f, 17000.0f), 2175.0, 0.05);
  CHECK_NEAR(refactor_nits(config, 1060.0f, 72000.0f), 3500.0, 0.05);
  // The dark drag the device logged: 7.9595423 logical nits at 0 lux became
  // brightness 0.008644696.
  CHECK_NEAR(PanelMap().brightness(refactor_nits(config, 7.9595423f, 0.0f)), 0.008644696, 2e-7);
  // Slider ends: about 1..254 nits in the dark, 6..551 nits at 300 lux.
  CHECK_NEAR(refactor_nits(config, 1.0f, 0.0f), 1.0, 1e-4);
  CHECK_NEAR(refactor_nits(config, 1060.0f, 0.0f), 254.4, 0.01);
  CHECK_NEAR(refactor_nits(config, 1.0f, 300.0f), 6.0, 1e-4);
  CHECK_NEAR(refactor_nits(config, 1060.0f, 300.0f), 551.2, 0.01);
}

void config_validation() {
  RefactorConfig config;
  config.first_factor.pop_back();
  CHECK(!config.valid());
  config = RefactorConfig();
  config.default_curve = {0, 2, 30, 135, 0, 20, 220, 5000};
  CHECK(!config.valid());
  config = RefactorConfig();
  config.good_nit.pop_back();
  CHECK(!config.valid());
  config = RefactorConfig();
  config.max_nit = 0.5f;
  CHECK(!config.valid());
}

void splines() {
  const std::vector<float> x{0, 10, 20}, y{1, 2, 4};
  CHECK_NEAR(linear_spline(x, y, -5.0f), 1.0, 1e-9);
  CHECK_NEAR(linear_spline(x, y, 15.0f), 3.0, 1e-6);
  CHECK_NEAR(linear_spline(x, y, 50.0f), 4.0, 1e-9);
  // Tangents as in the dumped MonotoneCubicSpline: 5.43 at 0 lux, 4.935 at 1.
  const MonotoneSpline spline({0, 1, 2, 3}, {1, 6.43f, 10.87f, 15.3f});
  CHECK(spline.valid());
  CHECK_NEAR(spline.interpolate(0.5f), 3.776875, 1e-4);
  CHECK_NEAR(spline.interpolate(2.0f), 10.87, 1e-5);
  CHECK_NEAR(spline.interpolate(-1.0f), 1.0, 1e-9);
  CHECK_NEAR(spline.interpolate(9.0f), 15.3, 1e-5);
  float previous = 0.0f;
  for (int i = 0; i <= 300; ++i) {
    const float value = spline.interpolate(static_cast<float>(i) / 100.0f);
    CHECK(value >= previous);
    previous = value;
  }
  CHECK(!MonotoneSpline({0, 1}, {2, 1}).valid());
  CHECK(!MonotoneSpline({0, 0}, {1, 2}).valid());
}

void system_curves() {
  const SystemCurve refactor;
  CHECK(refactor.learnable());
  CHECK_NEAR(refactor.nits(30.0f), 83.30, 0.01);
  CHECK_NEAR(refactor.default_nits(30.0f), 83.30, 0.01);
  CHECK_EQ(refactor.level(3500.0f, 16383), 16383);
  CHECK_EQ(refactor.level(0.0f, 16383), 1);
  int previous = 0;
  for (int i = 0; i <= 60; ++i) {
    const int level = refactor.level(refactor.nits(std::pow(10.0f, static_cast<float>(i) / 10.0f - 1.0f)), 16383);
    CHECK(level >= previous);
    previous = level;
  }
  const SystemCurve table({0, 30, 600}, {1, 83.13f, 126.14f}, PanelMap());
  CHECK(!table.learnable());
  CHECK_NEAR(table.nits(30.0f), 83.13, 1e-4);
  CHECK_NEAR(table.nits(-3.0f), 1.0, 1e-6);
  CHECK_NEAR(table.default_nits(600.0f), 126.14, 1e-4);
}

}  // namespace

int main() {
  panel_map_follows_display_config();
  refactor_nits_matches_framework();
  config_validation();
  splines();
  system_curves();
  return finish("brightness_map_tests");
}
