// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

namespace iab {

// Defaults and keys follow ios_brightness.conf of the v6.8 reference.
struct Config {
  float alpha_fast = 0.45f;
  float alpha_mid = 0.20f;
  float alpha_slow = 0.06f;
  float fast_threshold = 1.0f;
  float slow_threshold = 0.05f;
  float spike_up = 15.0f;
  float spike_down = 0.05f;
  float hyst_low = 0.025f;
  float hyst_high = 0.03f;
  float manual_gamma = 2.2f;
  float high_lux_threshold = 15000.0f;
  float high_lux_boost_max = 0.35f;
  float high_lux_min_pct = 0.90f;
  int high_lux_duration_ms = 8000;
  // Not in the reference: learning from slider drags, and the system dual
  // light sensor policy (0 restores the reference rear-sensor fallback only).
  bool learning_enabled = true;
  bool dual_sensor = true;
};

// Like the reference, only keys present in the file are overwritten.
bool load_config(const std::string& path, Config& config);

}  // namespace iab
