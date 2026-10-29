// SPDX-License-Identifier: GPL-3.0-only
#include "engine.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "curve.hpp"

namespace iab {

namespace {

constexpr float kTrendSlopeFast = 50.0f;

bool night_hour(int hour) { return hour >= 18 || hour < 6; }

float clamp_unit(float value) {
  if (value < 0.0f) value = 0.0f;
  return value > 1.0f ? 1.0f : value;
}

int clamp_level(int level, int max_level) {
  if (level < 1) return 1;
  return level < max_level ? level : max_level;
}

}  // namespace

Engine::Engine(Platform& platform, const Config& config, int max_level, float user_offset)
    : platform_(platform), config_(config), max_(max_level), user_offset_(user_offset) {}

void Engine::log(const char* format, ...) {
  char line[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof line, format, args);
  va_end(args);
  platform_.log(line);
}

void Engine::refresh_settings(int64_t now) {
  if (cached_auto_ >= 0 && now - last_settings_ < 8000) return;
  last_settings_ = now;
  SystemSettings settings;
  if (!platform_.read_settings(settings)) return;
  if (settings.brightness_mode >= 0) cached_auto_ = settings.brightness_mode;
  if (settings.brightness >= 0) cached_slider_ = settings.brightness < 255 ? settings.brightness : 255;
}

void Engine::refresh_battery(int64_t now) {
  if (battery_checked_ != 0 && now - battery_checked_ < 30000) return;
  battery_checked_ = now;
  int capacity = -1;
  bool charging = false;
  if (!platform_.read_battery(capacity, charging)) return;
  if (capacity >= 0 && capacity <= 100) battery_ = capacity;
  charging_ = charging;
}

bool Engine::can_boost(int64_t now) {
  refresh_battery(now);
  if (battery_ <= 29 && !charging_) return false;
  if (thermal_checked_ == 0 || now - thermal_checked_ >= 60000) {
    thermal_checked_ = now;
    thermal_ = platform_.boost_temperature();
  }
  return thermal_ * 1000 < 45001;
}

void Engine::enable_sensors() {
  if (!platform_.ndk_sensors() || sensors_enabled_) return;
  platform_.set_light_sensors_enabled(true);
  sensors_enabled_ = true;
}

void Engine::disable_sensors() {
  if (!platform_.ndk_sensors() || !sensors_enabled_) return;
  platform_.set_light_sensors_enabled(false);
  sensors_enabled_ = false;
}

void Engine::apply_events(const std::vector<SensorEvent>& events) {
  bool other_display = false, checked = false;
  for (const SensorEvent& event : events) {
    switch (event.kind) {
      case SensorEvent::Kind::Front:
        front_lux_ = event.value;
        dual_.push_main(event.time, event.value);
        break;
      case SensorEvent::Kind::Back:
        back_lux_ = event.value;
        if (!checked) other_display = platform_.other_display_on(), checked = true;
        dual_.push_assist(event.time, event.value, other_display);
        break;
      case SensorEvent::Kind::Proximity:
        proximity_near_ = event.value == 0.0f;
        break;
    }
  }
}

void Engine::begin(int64_t now) {
  refresh_settings(now);
  platform_.set_system_writable(cached_auto_ == 0);
  current_ = platform_.read_level();
  if (current_ >= 1) log("Warm start: %d", current_);
}

void Engine::start(int64_t now, float initial_lux) {
  for (float& value : trend_) value = initial_lux;
  last_stable_lux_ = initial_lux;
  smoothed_ = initial_lux;
  screen_on_ = platform_.power_state() == 1;
  log("Screen: %s", screen_on_ ? "ON" : "OFF");
  last_power_check_ = now;
  last_screen_ = true;
  first_ = true;
  mode_ = -1;
  lux_source_ = platform_.ndk_sensors() ? "front" : "fallback";
}

float Engine::select_lux(int64_t now) {
  if (!platform_.ndk_sensors()) {
    lux_source_ = "fallback";
    return platform_.fallback_lux();
  }
  if (front_lux_ < 0.0f && platform_.back_sensor() && back_lux_ >= 0.0f) {
    lux_source_ = "back";
    return back_lux_;
  }
  lux_source_ = "front";
  if (!config_.dual_sensor || !platform_.back_sensor() || front_lux_ < 0.0f) return front_lux_;
  dual_.update(now);
  if (!dual_.using_assist()) return front_lux_;
  lux_source_ = "rear";
  return dual_.assist_lux();
}

int Engine::apply_offset_and_limits(int level, int64_t now) {
  if (std::fabs(user_offset_) >= 0.001f) {
    const int sum = level + static_cast<int>(user_offset_ * static_cast<float>(max_));
    level = sum < 1 ? 1 : (sum < max_ ? sum : max_);
  }
  if (night_hour(platform_.local_hour())) level = static_cast<int>(0.95f * static_cast<float>(level));
  refresh_battery(now);
  if (battery_ <= 14 && !charging_) {
    const int cap = static_cast<int>(static_cast<float>(max_) * 0.7f);
    if (cap < level) level = cap;
  }
  return level;
}

void Engine::wake(int64_t now) {
  // The reference reapplies the curve at once from the last front reading.
  dual_.reset();
  float lux = platform_.ndk_sensors() ? front_lux_ : platform_.fallback_lux();
  if (lux >= 0.0f) {
    for (float& value : trend_) value = lux;
    lux_prev1_ = lux_prev2_ = lux;
    smoothed_ = lux;
  } else {
    lux = smoothed_;
  }
  const int level = apply_offset_and_limits(curve_level(lux, max_), now);
  platform_.write_level(clamp_level(level, max_));
  transition_.duration = 0;
  transition_.to = level;
  last_power_check_ = now;
  last_screen_ = screen_on_ = true;
  current_ = target_ = level;
}

void Engine::learn() {
  const int slider = cached_slider_;
  if (prev_slider_ >= 0 && std::abs(slider - prev_slider_) >= 10) {
    // The framework rewrites screen_brightness while auto brightness runs;
    // only a genuine drag also moves its short-term model point.
    if (!config_.learning_enabled) {
      last_learning_ = "disabled";
    } else if (!platform_.user_adjusted()) {
      last_learning_ = "ignored_system_change";
    } else {
      const int reference = slider_level(slider, config_.manual_gamma, max_);
      float offset = user_offset_ * 0.9f +
                     static_cast<float>(reference - target_) / static_cast<float>(max_) * 0.1f;
      if (offset > 0.3f) offset = 0.3f;
      if (offset < -0.3f) offset = -0.3f;
      user_offset_ = offset;
      platform_.save_learning(user_offset_);
      log("Learn: offset=%.3f (slider %d→%d)", user_offset_, prev_slider_, slider);
      last_learning_ = "learned";
    }
  }
  prev_slider_ = slider;
}

void Engine::tick(int64_t now) {
  refresh_settings(now);
  const bool automatic = cached_auto_ != 0;
  if (static_cast<int>(automatic) != mode_) {
    log("Mode: %s", automatic ? "AUTO" : "MANUAL");
    prev_slider_ = -1;
    if (automatic) {
      platform_.set_system_writable(false);
      enable_sensors();
      poll_ms_ = 800;
    } else {
      platform_.set_system_writable(true);
      disable_sensors();
      poll_ms_ = 5000;
      transition_.duration = 0;
    }
    if (sunlight_) log("☀️ 阳光模式 重置 (mode change)");
    mode_ = automatic;
    sunlight_ = sunlight_exiting_ = false;
    sunlight_confirm_ = 0;
    first_ = true;
    sunlight_level_ = 0;
  }
  if (automatic) {
    enable_sensors();
    std::vector<SensorEvent> events;
    platform_.poll_sensors(events);
    apply_events(events);
  }
  if (now - last_power_check_ >= 2000) {
    const bool on = platform_.power_state() == 1;
    last_power_check_ = now;
    if (on != last_screen_) {
      log("Screen: %s", on ? "ON" : "OFF");
      last_screen_ = screen_on_ = on;
      if (automatic && on) {
        wake(now);
        return;
      }
    }
  }
  if (!screen_on_) {
    disable_sensors();
    if (poll_ms_ <= 14999) poll_ms_ = 15000;
    return;
  }
  if (automatic && proximity_near_) {
    if (poll_ms_ <= 7999) poll_ms_ = 8000;
    return;
  }
  if (!automatic) {
    const int level = platform_.read_level();
    if (level > 0) current_ = level;
    if (poll_ms_ <= 4999) poll_ms_ = 5000;
    if (now - last_log_ >= 8000) {
      log("MANUAL passthrough slider=%d cur=%d poll=%dms", cached_slider_, current_, poll_ms_);
      last_log_ = now;
    }
    return;
  }

  const float lux = select_lux(now);
  input_lux_ = lux;
  if (lux >= 0.0f) {
    fast_dark_ = false;
    const float p1 = lux_prev1_, p2 = lux_prev2_;
    if (lux > 0.0f && p2 > 0.0f && p1 > 0.0f && p1 < p2 && lux < p1 &&
        (p2 - lux) / (p2 + 0.01f) > 0.35f) {
      fast_dark_ = true;
    }
    lux_prev2_ = p1;
    lux_prev1_ = lux;
    if (smoothed_ < 1.0f) {
      filtered_ = lux;
    } else {
      float upper = smoothed_ * config_.spike_up;
      if (upper < 5.0f) upper = 5.0f;
      float lower = smoothed_ * config_.spike_down;
      if (!(lower > lux)) lower = lux;
      filtered_ = upper < lux ? upper : lower;
    }
    trend_[trend_index_] = filtered_;
    trend_index_ = (trend_index_ + 1) % 5;
    float slope = 0.0f;
    const bool full = trend_filled_ >= 4;
    if (trend_filled_ <= 4) ++trend_filled_;
    if (full) {
      float sum = 0.0f, weighted = 0.0f;
      for (int i = 0; i < 5; ++i) {
        const float value = trend_[(trend_index_ + i) % 5];
        sum += value;
        weighted = std::fmaf(value, static_cast<float>(i), weighted);
      }
      slope = std::fmaf(weighted, 5.0f, sum * -10.0f) / 50.0f;
    }
    const float relative = std::fabs(filtered_ - smoothed_) / std::fmax(smoothed_, 0.1f);
    const float trend_floor = std::fabs(slope) > kTrendSlopeFast ? 0.3f : 0.0f;
    float alpha;
    if (relative >= config_.fast_threshold) {
      alpha = config_.alpha_fast;
    } else if (relative <= config_.slow_threshold) {
      alpha = config_.alpha_slow;
    } else {
      alpha = std::fmaf((relative - config_.slow_threshold) /
                            (config_.fast_threshold - config_.slow_threshold),
                        config_.alpha_mid - config_.alpha_slow, config_.alpha_slow);
    }
    if (filtered_ < smoothed_ && relative > config_.slow_threshold) {
      const float darkening = std::fmaf(0.7f - config_.alpha_slow,
                                        std::fmin(relative / config_.fast_threshold, 1.0f),
                                        config_.alpha_slow);
      if (darkening > alpha) alpha = darkening;
    }
    alpha_ = std::fmax(alpha, trend_floor);
    smoothed_ = std::fmaf(alpha_, filtered_, smoothed_ * (1.0f - alpha_));
    if (smoothed_ < 0.0f) smoothed_ = 0.0f;
  }
  int target = curve_level(smoothed_, max_);
  nits_ = smoothed_ > 0.0f ? nits_for_lux(smoothed_) : 0.222f;
  target = apply_offset_and_limits(target, now);

  const float threshold = config_.high_lux_threshold;
  if (threshold > 0.0f) {
    auto boosted = [&](int level) {
      float boost = 0.0f;
      if (!(threshold > filtered_)) {
        float limit = config_.high_lux_boost_max < 0.0f ? 0.0f : config_.high_lux_boost_max;
        if (limit > 1.0f) limit = 1.0f;
        boost = (filtered_ - threshold) / threshold * limit;
        if (boost > limit) boost = limit;
      }
      const int scaled = static_cast<int>((boost + 1.0f) * static_cast<float>(level));
      const int floor_level =
          static_cast<int>(static_cast<float>(max_) * clamp_unit(config_.high_lux_min_pct) + 0.5f);
      int result = floor_level > scaled ? floor_level : scaled;
      if (result > max_) result = max_;
      return std::make_pair(result > 1 ? result : 1, boost);
    };
    if (sunlight_) {
      if (!can_boost(now)) {
        if (!sunlight_exiting_) {
          sunlight_exiting_ = true;
          sunlight_exit_start_ = now;
          log("☀️ 阳光模式 温保 exit");
        }
      } else if (!(threshold > filtered_)) {
        sunlight_confirm_ = 2;
        sunlight_exiting_ = false;
        target = sunlight_level_ = boosted(target).first;
      } else if (!(threshold * 0.7f > filtered_)) {
        if (sunlight_level_ > 0) target = sunlight_level_;
      } else {
        if (!sunlight_exiting_) {
          sunlight_exiting_ = true;
          sunlight_exit_start_ = now;
          log("☀️ 阳光模式 退出中 (lux=%.0f < %.0f)", filtered_, threshold * 0.7f);
        }
        if (now - sunlight_exit_start_ >= config_.high_lux_duration_ms) {
          sunlight_exiting_ = sunlight_ = false;
          sunlight_confirm_ = 0;
          sunlight_level_ = 0;
          log("☀️ 阳光模式 OFF");
        } else {
          target = sunlight_level_;
        }
      }
    } else if (threshold > filtered_) {
      sunlight_confirm_ = 0;
    } else if (sunlight_confirm_++ >= 1 && can_boost(now)) {
      sunlight_exiting_ = false;
      sunlight_ = true;
      ++sunlight_count_;
      const auto result = boosted(target);
      log("☀️ 阳光模式 ON boost=%.0f%% br=%d→%d", result.second * 100.0f, target, result.first);
      target = sunlight_level_ = result.first;
    }
  }

  // Hysteresis against the previous target. The reference compares with the
  // current level when no target exists yet but then substitutes that empty
  // (zero) target, driving the panel to its minimum at startup; keep the
  // current level instead.
  const int reference = transition_.to > 0 ? transition_.to : current_;
  if (!fast_dark_ && reference >= 1) {
    const float ratio = std::fabs(static_cast<float>(target - reference)) / static_cast<float>(reference);
    const float band = std::fmaf(static_cast<float>(reference) / static_cast<float>(max_),
                                 config_.hyst_high - config_.hyst_low, config_.hyst_low);
    if (!(ratio > band)) target = reference;
  }
  int previous_to = transition_.to;
  if (first_) {
    const int level = platform_.read_level();
    if (level > 0) current_ = level;
    transition_.duration = 0;
    transition_.to = previous_to = target;
  }
  int from = current_;
  if (sunlight_ && target != current_) {
    transition_ = {current_, target, now, 1000};
  } else if (fast_dark_ && target < current_) {
    transition_ = {current_, target, now, 200};
  } else if (previous_to == target && transition_.duration >= 1) {
    from = transition_.from;
  } else {
    const float relative = static_cast<float>(std::abs(target - current_)) / static_cast<float>(max_);
    int duration;
    if (relative < 0.02f) duration = 0;
    else if (relative < 0.05f) duration = 80;
    else if (relative < 0.15f) duration = 180;
    else if (relative < 0.3f) duration = 300;
    else duration = 400;
    if (target > current_) duration = static_cast<int>(static_cast<unsigned>(duration) * 45878u >> 16);
    transition_ = {current_, target, now, duration};
  }
  int level = target;
  const int delta = target - from;
  if (transition_.duration != 0 && delta != 0) {
    const int64_t elapsed = now - transition_.start;
    if (transition_.duration <= elapsed) {
      transition_.duration = 0;
    } else {
      const float t = static_cast<float>(elapsed) / static_cast<float>(transition_.duration);
      float eased;
      if (t < 0.5f) {
        eased = t * (t * (t * 4.0f));
      } else {
        eased = std::pow(std::fmaf(t, -2.0f, 2.0f), 3.0f) * -0.5f;
        eased = eased + 1.0f;
      }
      const int step = from + static_cast<int>(std::fmaf(static_cast<float>(delta), eased, 0.5f));
      level = target > from ? (step < target ? step : target) : (step > target ? step : target);
      level = clamp_level(level, max_);
    }
  }
  if (level != current_) {
    if (level > current_) ++bright_count_;
    else ++dark_count_;
    platform_.write_level(clamp_level(level, max_));
    current_ = level;
  }
  target_ = target;

  learn();
  if (std::fabs(filtered_ - last_stable_lux_) < 0.5f) {
    ++stable_count_;
  } else {
    stable_count_ = 0;
    last_stable_lux_ = filtered_;
  }
  if (transition_.duration > 0) {
    poll_ms_ = 800;
  } else {
    refresh_battery(now);
    const int base = battery_ <= 14 && !charging_ ? 2000 : 800;
    if (stable_count_ > 20) poll_ms_ = 8000;
    else if (stable_count_ > 10) poll_ms_ = base > 1600 ? base : 1600;
    else poll_ms_ = base;
  }
  if (now - last_log_ >= 8000) {
    log("AUTO lux=%.1f sm=%.1f a=%.2f nits=%.1f tgt=%d cur=%d poll=%dms%s%s%s%s", filtered_, smoothed_,
        alpha_, nits_, target, current_, poll_ms_, sunlight_ ? " [阳光]" : "", fast_dark_ ? " [FAST]" : "",
        lux_source_ == "rear" ? " [REAR]" : (platform_.back_sensor() ? " +back" : ""),
        proximity_near_ ? " [PROX]" : "");
    last_log_ = now;
  }
  first_ = false;
}

void Engine::reset_learning() {
  user_offset_ = 0.0f;
  platform_.save_learning(user_offset_);
  last_learning_ = "reset";
  log("Learn: offset reset");
}

void Engine::shutdown() {
  platform_.set_system_writable(true);
  platform_.save_learning(user_offset_);
  log("Stopped (bright:%d dark:%d 阳光:%d)", bright_count_, dark_count_, sunlight_count_);
}

EngineStatus Engine::status() const {
  EngineStatus s;
  s.automatic = mode_ == 1 || (mode_ < 0 && cached_auto_ != 0);
  s.screen_on = screen_on_;
  s.proximity_near = proximity_near_;
  s.sunlight = sunlight_;
  s.fast_dark = fast_dark_;
  s.using_rear = lux_source_ == "rear";
  s.front_lux = front_lux_;
  s.back_lux = back_lux_;
  s.input_lux = input_lux_;
  s.filtered_lux = filtered_;
  s.smoothed_lux = smoothed_;
  s.alpha = alpha_;
  s.nits = nits_;
  s.user_offset = user_offset_;
  s.target = target_;
  s.current = current_;
  s.poll_ms = poll_ms_;
  s.slider = cached_slider_;
  s.mode_setting = cached_auto_;
  s.max_level = max_;
  s.stable_count = stable_count_;
  s.bright_count = bright_count_;
  s.dark_count = dark_count_;
  s.sunlight_count = sunlight_count_;
  s.battery = battery_;
  s.charging = charging_;
  s.lux_source = lux_source_;
  s.last_learning = last_learning_;
  return s;
}

}  // namespace iab
