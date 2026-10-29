// SPDX-License-Identifier: GPL-3.0-only
#include "config.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace iab {

namespace {

struct FloatKey {
  const char* name;
  float Config::*field;
};

constexpr FloatKey kFloatKeys[] = {
    {"alpha_fast", &Config::alpha_fast},
    {"alpha_mid", &Config::alpha_mid},
    {"alpha_slow", &Config::alpha_slow},
    {"fast_threshold", &Config::fast_threshold},
    {"slow_threshold", &Config::slow_threshold},
    {"spike_up", &Config::spike_up},
    {"spike_down", &Config::spike_down},
    {"hyst_low", &Config::hyst_low},
    {"hyst_high", &Config::hyst_high},
    {"manual_gamma", &Config::manual_gamma},
    {"high_lux_threshold", &Config::high_lux_threshold},
    {"high_lux_boost_max", &Config::high_lux_boost_max},
    {"high_lux_min_pct", &Config::high_lux_min_pct},
};

void trim(char* text) {
  char* end = text + std::strlen(text);
  while (end > text && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
  char* start = text;
  while (*start == ' ' || *start == '\t') ++start;
  if (start != text) std::memmove(text, start, std::strlen(start) + 1);
}

}  // namespace

bool load_config(const std::string& path, Config& config) {
  FILE* file = std::fopen(path.c_str(), "r");
  if (!file) return false;
  char line[256];
  while (std::fgets(line, sizeof line, file)) {
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
    char key[64];
    float value;
    if (std::sscanf(line, "%63[^=]=%f", key, &value) != 2 || !std::isfinite(value)) continue;
    trim(key);
    bool matched = false;
    for (const FloatKey& entry : kFloatKeys) {
      if (std::strcmp(key, entry.name) == 0) {
        config.*entry.field = value;
        matched = true;
      }
    }
    if (matched) continue;
    if (std::strcmp(key, "high_lux_duration_ms") == 0) {
      config.high_lux_duration_ms = static_cast<int>(value);
    } else if (std::strcmp(key, "learning_enabled") == 0) {
      config.learning_enabled = value != 0.0f;
    } else if (std::strcmp(key, "dual_sensor") == 0) {
      config.dual_sensor = value != 0.0f;
    }
  }
  std::fclose(file);
  return true;
}

}  // namespace iab
