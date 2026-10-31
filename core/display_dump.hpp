// SPDX-License-Identifier: GPL-3.0-only
// What the running framework reports about the main display in dumpsys
// display: its light sensor configuration and its thermal brightness limit.
#pragma once

#include <string>

#include "dual_sensor.hpp"

namespace iab {

// The "Display Power Controller:" block of display 0, up to the next one.
std::string main_display_section(const std::string& dump);

// MiuiHysteresisLevels, the HBM minimum lux, the AutomaticBrightnessController
// horizons and debounces and the DualSensorPolicy debounces of that block.
// False when the framework runs no dual sensor policy there.
bool read_dual_sensor_config(const std::string& section, DualSensorConfig& out);

// DisplayPowerControllerImpl.adjustBrightnessByThermal / adjustBrightnessByBattery:
// the lower of mThermalMaxBrightness (unset as -1 or NaN) and
// mBatteryThermalLimitBrightness, as a brightness in (0, 1]; 1 when neither
// limits, NaN when the block has neither field.
float read_thermal_limit(const std::string& section);

}  // namespace iab
