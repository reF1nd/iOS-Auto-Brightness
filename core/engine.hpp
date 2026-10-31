// SPDX-License-Identifier: GPL-3.0-only
// Port of the v6.8 reference daemon's control loop (main() of
// ios_brightness_daemon) driving the system's own brightness curve instead of
// the reference one. Every tick mirrors one pass of its polling loop;
// hardware and Android access go through Platform so the logic is testable.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"
#include "dual_sensor.hpp"
#include "settings_reader.hpp"
#include "system_curve.hpp"

namespace iab {

struct SensorEvent {
  enum class Kind { Front, Back, Proximity };
  Kind kind;
  int64_t time;  // monotonic milliseconds
  float value;
};

class Platform {
 public:
  virtual ~Platform() = default;
  virtual bool read_settings(SystemSettings& out) = 0;
  // 0644 lets the system write the panel node again, 0444 is the reference lock.
  virtual void set_system_writable(bool writable) = 0;
  virtual void set_light_sensors_enabled(bool enabled) = 0;
  virtual void poll_sensors(std::vector<SensorEvent>& events) = 0;
  virtual bool ndk_sensors() = 0;
  virtual bool back_sensor() = 0;
  // 1: the main display is lit (the Zygisk reporter's display state when
  // connected, which tells the AOD apart; otherwise bl_power reads '0' or
  // cannot be opened); 0 otherwise.
  virtual int power_state() = 0;
  virtual int read_level() = 0;
  virtual void write_level(int level) = 0;
  virtual float fallback_lux() = 0;
  virtual int local_hour() = 0;
  virtual bool read_battery(int& capacity, bool& charging) = 0;
  // The framework's thermal brightness limit of the main display as a
  // brightness in (0, 1]; 1 when it does not limit.
  virtual float thermal_limit() { return 1.0f; }
  // DualSensorPolicy::Scene flags of the framework.
  virtual unsigned scene() { return 0; }
  virtual bool other_display_on() = 0;
  // True once per slider drag the framework learned in auto mode, with the
  // lux and logical nits RefactorNitController.updateLogicalCurve received.
  virtual bool user_drag(float& lux, float& logical) = 0;
  // CLOCK_BOOTTIME milliseconds (SystemClock.elapsedRealtime).
  virtual int64_t elapsed_ms() = 0;
  virtual void save_learning(const std::string& text) = 0;
  virtual void log(const std::string& line) = 0;
  // While the Zygisk reporter is connected, the framework's anchors arrive
  // live: once per change, in learn.conf form.
  virtual bool follows_system() { return false; }
  virtual bool system_anchors(std::string& text) {
    (void)text;
    return false;
  }
  // Asks the framework to drop its anchors too (resetDefaultSpline).
  virtual void reset_system_anchors() {}
};

struct EngineStatus {
  bool automatic = false, screen_on = false, proximity_near = false;
  bool sunlight = false, fast_dark = false, using_rear = false, dual_policy = false;
  float front_lux = -1, back_lux = -1, input_lux = -1, filtered_lux = 0, smoothed_lux = 0;
  float alpha = 0, nits = 0, thermal_limit = 1;
  unsigned scene = 0;
  int target = 0, current = 0, poll_ms = 0, slider = -1, mode_setting = -1, max_level = 0;
  int stable_count = 0, bright_count = 0, dark_count = 0, sunlight_count = 0;
  int battery = 100;
  bool charging = false;
  std::string lux_source = "front";
  std::string last_learning = "none";
};

class Engine {
 public:
  Engine(Platform& platform, const Config& config, int max_level, const SystemCurve& curve = SystemCurve());

  void set_config(const Config& config);
  // The framework's light sensor configuration; without it the rear sensor
  // only stands in for a missing front reading.
  void set_dual_sensor(const DualSensorConfig& config) { dual_.configure(config); }
  // Reference startup before the sensor wait: settings, node lock, warm start.
  void begin(int64_t now);
  void apply_events(const std::vector<SensorEvent>& events);
  float front_lux() const { return front_lux_; }
  void start(int64_t now, float initial_lux);
  void tick(int64_t now);
  // Called about once a second between ticks to catch slider drags promptly.
  void check_learning(int64_t now);
  // Panel level of the curve at this lux, before the night and battery limits.
  int curve_level(float lux) const { return curve_.level(curve_.nits(lux), max_); }
  void reset_learning();
  // The reported display state changed: check it on the next tick.
  void recheck_power() { last_power_check_ = INT64_MIN / 2; }
  void shutdown();

  int poll_ms() const { return poll_ms_; }
  const SystemCurve& curve() const { return curve_; }
  EngineStatus status() const;

 private:
  struct Transition {
    int from = 0, to = 0;
    int64_t start = 0;
    int duration = 0;
  };

  void refresh_settings(int64_t now);
  void refresh_battery(int64_t now);
  bool can_boost(int64_t now);
  void enable_sensors();
  void disable_sensors();
  float select_lux(int64_t now);
  int apply_limits(int level, int64_t now);
  int apply_thermal_limit(int level);
  void wake(int64_t now);
  void check_good_curve(float lux);
  void follow_system();
  void log(const char* format, ...) __attribute__((format(printf, 2, 3)));

  Platform& platform_;
  Config config_;
  const int max_;
  SystemCurve curve_;
  DualSensorPolicy dual_;
  bool learn_pending_ = false;
  int64_t learn_since_ = 0;
  int learn_level_ = -1;
  // Screen-off bookkeeping of AutomaticBrightnessControllerImpl for the good
  // curve: when and at what lux the screen went off, and the check pending
  // until the first light reading after waking.
  int64_t closed_at_ = -1, woke_at_ = 0;
  float closed_lux_ = 0;
  bool reset_check_ = false, fresh_front_ = false;

  // Reference globals.
  int cached_auto_ = -1, cached_slider_ = 128;
  int64_t last_settings_ = 0;
  float front_lux_ = -1, back_lux_ = -1;
  bool proximity_near_ = false, sensors_enabled_ = true;
  float lux_prev1_ = -1, lux_prev2_ = -1, last_stable_lux_ = -1;
  float trend_[5] = {};
  int trend_index_ = 0, trend_filled_ = 0, stable_count_ = 0;
  Transition transition_;
  bool sunlight_ = false, sunlight_exiting_ = false, fast_dark_ = false;
  int sunlight_confirm_ = 0, sunlight_level_ = 0;
  int64_t sunlight_exit_start_ = 0;
  int bright_count_ = 0, dark_count_ = 0, sunlight_count_ = 0;
  int poll_ms_ = 800;
  int battery_ = 100;
  bool charging_ = false;
  int64_t battery_checked_ = 0;
  float thermal_limit_ = 1.0f;

  // Reference main() locals.
  int mode_ = -1;
  bool first_ = true, screen_on_ = true, last_screen_ = true;
  int64_t last_power_check_ = 0, last_log_ = 0;
  int current_ = -1;
  float smoothed_ = 0, filtered_ = 0, alpha_ = 0.06f, nits_ = 0, input_lux_ = -1;
  int target_ = 0;
  std::string lux_source_ = "front", last_learning_ = "none";
};

}  // namespace iab
