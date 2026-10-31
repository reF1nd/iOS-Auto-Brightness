// SPDX-License-Identifier: GPL-3.0-only
#include <cmath>
#include <string>

#include "altdb_display.hpp"
#include "check.hpp"
#include "display_dump.hpp"

using namespace iab;
using iab::test::kAltdbDisplayDump;

namespace {

std::string replaced(std::string text, const std::string& from, const std::string& to) {
  const size_t at = text.find(from);
  if (at != std::string::npos) text.replace(at, from.size(), to);
  return text;
}

void section_is_display_zero() {
  const std::string section = main_display_section(kAltdbDisplayDump);
  CHECK(section.rfind("Display Power Controller:\n", 0) == 0);
  CHECK(section.find("mDisplayId=0") != std::string::npos);
  CHECK(section.find("mDisplayId=1") == std::string::npos);
  CHECK(main_display_section("Display Power Controller:\n  mDisplayId=1\n").empty());
  CHECK(main_display_section("").empty());
}

void light_config_of_the_running_system() {
  DualSensorConfig config;
  CHECK(read_dual_sensor_config(main_display_section(kAltdbDisplayDump), config));
  CHECK_EQ(config.brightening.size(), 15u);
  CHECK_EQ(config.small_brightening.size(), 15u);
  CHECK_EQ(config.darkening.size(), 19u);
  CHECK_NEAR(config.brightening[1].x, 3.0, 1e-6);
  CHECK_NEAR(config.brightening[1].y, 6.0, 1e-6);
  CHECK_NEAR(config.darkening.back().y, 10011.96, 1e-2);
  // Display 0's HighBrightnessModeController, not DisplayDeviceConfig above it.
  CHECK_NEAR(config.hbm_minimum_lux, 4475.0, 1e-3);
  CHECK_EQ(config.horizon_short, 1000);
  CHECK_EQ(config.horizon_long, 3000);
  CHECK_EQ(config.weighting_intercept, 3000);
  CHECK_EQ(config.brightening_debounce, 1000);
  CHECK_EQ(config.small_brightening_debounce, 5000);
  CHECK_EQ(config.darkening_debounce, 1000);
  CHECK_EQ(config.assist_brightening_debounce, 1000);
  CHECK_EQ(config.assist_small_brightening_debounce, 5000);
  CHECK_EQ(config.assist_darkening_debounce, 1000);
  CHECK(config.valid());

  const Thresholds t = thresholds_for(config, 5.0f);
  CHECK_NEAR(t.brightening, 8.51 + (13.66 - 8.51) / 2, 1e-3);
  CHECK_NEAR(t.darkening, 0.32 + (3.53 - 0.32) / 6 * 4, 1e-3);
  CHECK_NEAR(thresholds_for(config, 5000.0f).brightening, 5001.0, 1e-3);
}

void no_policy_no_config() {
  DualSensorConfig config;
  const std::string dump = kAltdbDisplayDump;
  CHECK(!read_dual_sensor_config(main_display_section(replaced(dump, "Dual Sensor Policy Configuration:", "")),
                                 config));
  CHECK(!read_dual_sensor_config(main_display_section(replaced(dump, "mHysteresisDarkSpline=", "x=")), config));
  CHECK(!read_dual_sensor_config(main_display_section(replaced(dump, "  mWeightingIntercept=3000\n", "")), config));
  CHECK(!config.valid());
  // Without HBM data the framework compares with 6000 lux.
  CHECK(read_dual_sensor_config(main_display_section(replaced(dump, "mHbmData=HBM{minLux: 4475.0", "mHbmData=null")),
                                config));
  CHECK_NEAR(config.hbm_minimum_lux, 6000.0, 1e-3);
}

void thermal_limit_of_display_zero() {
  const std::string dump = kAltdbDisplayDump;
  // Not mCurrentThermalMaxBrightness (nits), nor display 1's.
  CHECK_NEAR(read_thermal_limit(main_display_section(dump)), 0.37485194, 1e-6);
  CHECK_NEAR(read_thermal_limit(main_display_section(replaced(dump, "mBatteryThermalLimitBrightness=1.0",
                                                              "mBatteryThermalLimitBrightness=0.25"))),
             0.25, 1e-6);
  CHECK_NEAR(read_thermal_limit(main_display_section(replaced(dump, "mThermalMaxBrightness=0.37485194",
                                                              "mThermalMaxBrightness=NaN"))),
             1.0, 1e-6);
  CHECK_NEAR(read_thermal_limit(main_display_section(replaced(dump, "mThermalMaxBrightness=0.37485194",
                                                              "mThermalMaxBrightness=-1.0"))),
             1.0, 1e-6);
  CHECK(std::isnan(read_thermal_limit("Display Power Controller:\n  mDisplayId=0\n")));
}

}  // namespace

int main() {
  section_is_display_zero();
  light_config_of_the_running_system();
  no_policy_no_config();
  thermal_limit_of_display_zero();
  return finish("display_dump_tests");
}
