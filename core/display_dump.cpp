// SPDX-License-Identifier: GPL-3.0-only
#include "display_dump.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace iab {

namespace {

constexpr char kController[] = "\nDisplay Power Controller:\n";

// The number after "<key>" at the start of a line, indented or not
// ("mCurrentThermalMaxBrightness=" is not "mThermalMaxBrightness=").
bool number(const std::string& section, const char* key, double& out) {
  const size_t length = std::strlen(key);
  for (size_t at = section.find(key); at != std::string::npos; at = section.find(key, at + 1)) {
    const size_t line = at == 0 ? std::string::npos : section.rfind('\n', at - 1);
    const size_t indent = line == std::string::npos ? 0 : line + 1;
    if (section.find_first_not_of(' ', indent) != at) continue;
    const char* begin = section.c_str() + at + length;
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin) return false;
    out = value;
    return true;
  }
  return false;
}

bool duration(const std::string& section, const char* key, int64_t& out) {
  double value = 0.0;
  if (!number(section, key, value) || !(value >= 0.0 && value < 3600000.0)) return false;
  out = static_cast<int64_t>(value);
  return true;
}

// "<key>LinearSpline{[(x, y: slope), ..., (x, y)]}".
bool spline(const std::string& section, const char* key, std::vector<SplinePoint>& out) {
  const std::string needle = std::string(key) + "LinearSpline{[";
  const size_t at = section.find(needle);
  if (at == std::string::npos) return false;
  const size_t end = section.find(']', at);
  if (end == std::string::npos) return false;
  out.clear();
  for (size_t open = section.find('(', at); open < end; open = section.find('(', open + 1)) {
    const char* cursor = section.c_str() + open + 1;
    char* next = nullptr;
    const float x = std::strtof(cursor, &next);
    if (next == cursor || *next != ',') return false;
    cursor = next + 1;
    const float y = std::strtof(cursor, &next);
    if (next == cursor || !std::isfinite(x) || !std::isfinite(y)) return false;
    out.push_back({x, y});
  }
  return !out.empty();
}

}  // namespace

std::string main_display_section(const std::string& dump) {
  for (size_t at = dump.find(kController); at != std::string::npos; at = dump.find(kController, at + 1)) {
    const size_t body = at + std::strlen(kController);
    // "-------------------------" and the id follow the heading.
    const size_t id = dump.find("\n  mDisplayId=", at);
    if (id == std::string::npos || id > body + 64) continue;
    if (std::atoi(dump.c_str() + id + 14) != 0 || dump[id + 14] != '0') continue;
    const size_t next = dump.find(kController, body);
    return dump.substr(at + 1, next == std::string::npos ? std::string::npos : next - at - 1);
  }
  return "";
}

bool read_dual_sensor_config(const std::string& section, DualSensorConfig& out) {
  if (section.find("Dual Sensor Policy Configuration:") == std::string::npos) return false;
  DualSensorConfig config;
  if (!spline(section, "mHysteresisBrightSpline=", config.brightening) ||
      !spline(section, "mHysteresisSmallBrightSpline=", config.small_brightening) ||
      !spline(section, "mHysteresisDarkSpline=", config.darkening)) {
    return false;
  }
  if (!duration(section, "mAmbientLightHorizonShort=", config.horizon_short) ||
      !duration(section, "mAmbientLightHorizonLong=", config.horizon_long) ||
      !duration(section, "mWeightingIntercept=", config.weighting_intercept) ||
      !duration(section, "mBrighteningLightDebounceConfig=", config.brightening_debounce) ||
      !duration(section, "mSmallBrighteningLightDebounceConfig=", config.small_brightening_debounce) ||
      !duration(section, "mDarkeningLightDebounceConfig=", config.darkening_debounce) ||
      !duration(section, "mAssistLightSensorBrighteningDebounce=", config.assist_brightening_debounce) ||
      !duration(section, "mAssistLightSensorSmallBrighteningDebounce=", config.assist_small_brightening_debounce) ||
      !duration(section, "mAssistLightSensorDarkeningDebounce=", config.assist_darkening_debounce)) {
    return false;
  }
  static const char kHbm[] = "mHbmData=HBM{minLux: ";
  const size_t hbm = section.find(kHbm);
  if (hbm != std::string::npos) {
    const float lux = std::strtof(section.c_str() + hbm + sizeof kHbm - 1, nullptr);
    if (lux > 0.0f && std::isfinite(lux)) config.hbm_minimum_lux = lux;
  }
  if (!config.valid()) return false;
  out = config;
  return true;
}

float read_thermal_limit(const std::string& section) {
  double thermal = NAN, battery = NAN;
  const bool has_thermal = number(section, "mThermalMaxBrightness=", thermal);
  const bool has_battery = number(section, "mBatteryThermalLimitBrightness=", battery);
  if (!has_thermal && !has_battery) return NAN;
  double limit = 1.0;
  if (std::isfinite(thermal) && thermal > 0.0) limit = std::fmin(limit, thermal);
  if (std::isfinite(battery) && battery > 0.0) limit = std::fmin(limit, battery);
  return static_cast<float>(limit);
}

}  // namespace iab
