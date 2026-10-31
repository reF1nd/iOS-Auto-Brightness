// SPDX-License-Identifier: GPL-3.0-only
#include "engine.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace iab {

namespace {

constexpr float kTrendSlopeFast = 50.0f;
// AutomaticBrightnessControllerImpl.isNeedResetShortTermModel.
constexpr int64_t kResetAfterMs = 1800000, kResetLightChangeAfterMs = 360000;
constexpr float kResetLuxChange = 20.0f;

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

Engine::Engine(Platform& platform, const Config& config, int max_level, const SystemCurve& curve)
    : platform_(platform), config_(config), max_(max_level), curve_(curve) {}

void Engine::set_config(const Config& config) { config_ = config; }

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

// The reference also refused above 45 C; temperature is left to the
// framework's own limit (apply_thermal_limit).
bool Engine::can_boost(int64_t now) {
  refresh_battery(now);
  return battery_ > 29 || charging_;
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
        fresh_front_ = true;
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
  if (!config_.dual_sensor || !dual_.configured() || !platform_.back_sensor() || front_lux_ < 0.0f) {
    return front_lux_;
  }
  dual_.update(now);
  if (!dual_.using_assist()) return front_lux_;
  lux_source_ = "rear";
  return dual_.assist_lux();
}

int Engine::apply_limits(int level, int64_t now) {
  if (night_hour(platform_.local_hour())) level = static_cast<int>(0.95f * static_cast<float>(level));
  refresh_battery(now);
  if (battery_ <= 14 && !charging_) {
    const int cap = static_cast<int>(static_cast<float>(max_) * 0.7f);
    if (cap < level) level = cap;
  }
  return level;
}

// DisplayPowerControllerImpl caps every brightness, the sunlight boost included.
int Engine::apply_thermal_limit(int level) {
  float limit = platform_.thermal_limit();
  if (!(limit > 0.0f && limit < 1.0f)) limit = 1.0f;
  if (limit != thermal_limit_) {
    thermal_limit_ = limit;
    if (limit < 1.0f) {
      log("Thermal: the system limits brightness to %.4f (level %d)", limit,
          static_cast<int>(limit * static_cast<float>(max_) + 0.5f));
    } else {
      log("Thermal: no system limit");
    }
  }
  if (limit >= 1.0f) return level;
  const int cap = static_cast<int>(limit * static_cast<float>(max_) + 0.5f);
  return level < cap ? level : (cap > 1 ? cap : 1);
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
  const int level = apply_thermal_limit(apply_limits(curve_level(lux), now));
  platform_.write_level(clamp_level(level, max_));
  transition_.duration = 0;
  transition_.to = level;
  last_power_check_ = now;
  last_screen_ = screen_on_ = true;
  current_ = target_ = level;
  reset_check_ = closed_at_ >= 0;
  woke_at_ = now;
  fresh_front_ = false;
}

// needResetShortTermModelPolicy on the first light reading after the screen
// comes back: after 30 minutes off, or 6 minutes off with the light changed by
// more than 20 lux, a drag above the good curve falls back to it.
void Engine::check_good_curve(float lux) {
  reset_check_ = false;
  // The framework applies it itself, and the reporter brings the result.
  if (platform_.follows_system()) return;
  const int64_t off = platform_.elapsed_ms() - closed_at_;
  if (!curve_.learnable() || !curve_.controller().config().reset_policy) return;
  if (!(off >= kResetAfterMs || (std::fabs(lux - closed_lux_) > kResetLuxChange && off >= kResetLightChangeAfterMs))) {
    return;
  }
  if (!curve_.controller().use_good_curve()) return;
  platform_.save_learning(curve_.controller().serialize());
  last_learning_ = "good_curve";
  log("Learn: good curve after %llds off (%.1f -> %.1f lux)", static_cast<long long>(off / 1000), closed_lux_, lux);
}

void Engine::check_learning(int64_t now) {
  if (mode_ != 1 || !screen_on_) {
    learn_pending_ = false;
    return;
  }
  // Only a slider drag in auto mode reaches updateLogicalCurve; the framework
  // rewrites screen_brightness on its own all the time.
  float lux = 0.0f, logical = 0.0f;
  if (platform_.user_drag(lux, logical)) {
    if (!config_.learning_enabled) {
      last_learning_ = "disabled";
      return;
    }
    if (!curve_.learnable()) {
      last_learning_ = "unsupported";
      return;
    }
    NitController& controller = curve_.controller();
    controller.drag(lux, logical);
    platform_.save_learning(controller.serialize());
    std::string anchors;
    for (const Anchor& anchor : controller.anchors()) {
      char item[48];
      std::snprintf(item, sizeof item, "%s(%.1f,%.1f%s)", anchors.empty() ? "" : " ", anchor.lux, anchor.nit,
                    anchor.user ? ",user" : "");
      anchors += item;
    }
    log("Learn: %.1f lux -> %.2f logical nits%s anchors=%s", controller.user_lux(), logical,
        controller.has_good_curve() ? " (above good curve)" : "", anchors.c_str());
    last_learning_ = "learned";
    // The system shows the dragged level; leave it until the panel settles.
    learn_pending_ = true;
    learn_since_ = now;
    learn_level_ = platform_.read_level();
    return;
  }
  follow_system();
  if (!learn_pending_) return;
  const int level = platform_.read_level();
  if (level != learn_level_ && now - learn_since_ < 10000) {
    learn_level_ = level;
    return;
  }
  if (now - learn_since_ < 1000) return;
  learn_pending_ = false;
  // Continue from what the panel shows instead of easing from our last write.
  if (level >= 1) {
    current_ = level;
    transition_ = {level, level, now, 0};
  }
}

// The framework's anchors as reported: drags come through user_drag above;
// this catches what changes them otherwise (its good curve, a reset), so the
// system and the daemon keep writing the same curve.
void Engine::follow_system() {
  std::string text;
  if (!platform_.system_anchors(text) || !config_.learning_enabled || !curve_.learnable()) return;
  NitController& ours = curve_.controller();
  NitController system(ours.config());
  if (!system.parse(text) || system.same(ours)) return;
  const bool reset = system.user_anchors() == 0;
  const bool good = !reset && ours.has_good_curve() && !system.has_good_curve();
  ours = system;
  platform_.save_learning(ours.serialize());
  last_learning_ = reset ? "reset" : good ? "good_curve" : "learned";
  log("Learn: following the system's anchors (%s, %d dragged)", reset ? "reset" : good ? "good curve" : "changed",
      ours.user_anchors());
}

void Engine::tick(int64_t now) {
  refresh_settings(now);
  const bool automatic = cached_auto_ != 0;
  if (static_cast<int>(automatic) != mode_) {
    log("Mode: %s", automatic ? "AUTO" : "MANUAL");
    learn_pending_ = false;
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
    dual_.set_scene(platform_.scene(), now);
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
      if (!on) {
        closed_at_ = platform_.elapsed_ms();
        closed_lux_ = smoothed_;
      }
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
  if (reset_check_ && ((fresh_front_ && lux >= 0.0f) || now - woke_at_ >= 5000)) {
    check_good_curve(lux >= 0.0f ? lux : smoothed_);
  }
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
  nits_ = curve_.nits(smoothed_);
  int target = apply_limits(curve_.level(nits_, max_), now);

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
          log("☀️ 阳光模式 电量 exit");
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

  target = apply_thermal_limit(target);

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
  // The reference restarts the sunlight and fast-darkening ramps every round
  // and samples them at their start, so a steady sunlight target never
  // reaches the panel; a ramp to the same target runs on instead.
  if (previous_to == target && transition_.duration >= 1) {
    from = transition_.from;
  } else if (sunlight_ && target != current_) {
    transition_ = {current_, target, now, 1000};
  } else if (fast_dark_ && target < current_) {
    transition_ = {current_, target, now, 200};
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
  // While a drag settles the panel shows the user's choice; do not overwrite it.
  if (level != current_ && !learn_pending_) {
    if (level > current_) ++bright_count_;
    else ++dark_count_;
    platform_.write_level(clamp_level(level, max_));
    current_ = level;
  }
  target_ = target;

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
  platform_.reset_system_anchors();
  curve_.controller().reset();
  learn_pending_ = false;
  platform_.save_learning(curve_.controller().serialize());
  last_learning_ = "reset";
  log("Learn: reset");
}

void Engine::shutdown() {
  platform_.set_system_writable(true);
  platform_.save_learning(curve_.controller().serialize());
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
  s.thermal_limit = thermal_limit_;
  s.scene = dual_.scene();
  s.dual_policy = dual_.configured();
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
