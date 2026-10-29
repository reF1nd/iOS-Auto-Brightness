// SPDX-License-Identifier: GPL-3.0-only
// Scenario tests for the reference control loop with a simulated phone.
#include <cmath>
#include <string>
#include <vector>

#include "check.hpp"
#include "curve.hpp"
#include "engine.hpp"

using namespace iab;

namespace {

constexpr int kMax = 16383;

struct FakePhone : Platform {
  SystemSettings settings{1, 100};
  bool writable = true;
  bool light_enabled = true;
  std::vector<SensorEvent> pending;
  bool back = false;
  int power = 1;
  int level = 1000;
  std::vector<int> writes;
  int hour = 12;
  int capacity = 80;
  bool charging = false;
  int temperature = 30;
  bool other_display = false;
  bool user_drag = false;
  int user_checks = 0;
  float saved_offset = 99.0f;
  std::vector<std::string> lines;

  bool read_settings(SystemSettings& out) override {
    out = settings;
    return true;
  }
  void set_system_writable(bool value) override { writable = value; }
  void set_light_sensors_enabled(bool enabled) override { light_enabled = enabled; }
  void poll_sensors(std::vector<SensorEvent>& events) override {
    events.insert(events.end(), pending.begin(), pending.end());
    pending.clear();
  }
  bool ndk_sensors() override { return true; }
  bool back_sensor() override { return back; }
  int power_state() override { return power; }
  int read_level() override { return level; }
  void write_level(int value) override {
    level = value;
    writes.push_back(value);
  }
  float fallback_lux() override { return -1.0f; }
  int local_hour() override { return hour; }
  bool read_battery(int& c, bool& ch) override {
    c = capacity;
    ch = charging;
    return true;
  }
  int boost_temperature() override { return temperature; }
  bool other_display_on() override { return other_display; }
  bool user_adjusted() override {
    ++user_checks;
    return user_drag;
  }
  void save_learning(float offset) override { saved_offset = offset; }
  void log(const std::string& line) override { lines.push_back(line); }
};

struct Rig {
  FakePhone phone;
  Config config;
  Engine engine;
  int64_t now = 0;

  explicit Rig(float initial_lux, int current = 1000, Config c = Config{})
      : config(c), engine(phone, c, kMax, 0.0f) {
    phone.level = current;
    engine.begin(now);
    phone.pending.push_back({SensorEvent::Kind::Front, now, initial_lux});
    std::vector<SensorEvent> events;
    phone.poll_sensors(events);
    engine.apply_events(events);
    engine.start(now, initial_lux);
  }
  void front(float lux) { phone.pending.push_back({SensorEvent::Kind::Front, now, lux}); }
  void rear(float lux) { phone.pending.push_back({SensorEvent::Kind::Back, now, lux}); }
  void tick() {
    now += engine.poll_ms();
    engine.tick(now);
  }
  void ticks(int count) {
    for (int i = 0; i < count; ++i) tick();
  }
};

void startup_does_not_flash_minimum() {
  // Target within the hysteresis band of the current level: the reference
  // would substitute its empty previous target (0) and write level 1.
  const int target = curve_level(10.0f, kMax);
  Rig rig(10.0f, target + 10);
  rig.ticks(3);
  CHECK(rig.phone.writes.empty());
  CHECK(!rig.phone.writable);
  CHECK_EQ(rig.engine.status().current, target + 10);
}

void large_steps_wait_for_a_settled_target() {
  Rig rig(10.0f, curve_level(10.0f, kMax));
  rig.ticks(3);
  CHECK(rig.phone.writes.empty());
  rig.front(300.0f);
  rig.tick();
  // A new transition starts at t=0, which reproduces the current level, and
  // the reference restarts it whenever the target moves outside hysteresis.
  CHECK(rig.phone.writes.empty());
  rig.ticks(12);
  CHECK(!rig.phone.writes.empty());
  const EngineStatus s = rig.engine.status();
  CHECK_EQ(rig.phone.level, s.target);
  CHECK(s.target > curve_level(10.0f, kMax));
  for (const int level : rig.phone.writes) CHECK(level > curve_level(10.0f, kMax));
}

void small_steps_are_written_at_once() {
  Rig rig(3.0f, curve_level(3.0f, kMax));
  rig.ticks(3);
  rig.front(6.0f);
  rig.tick();
  // Less than 2% of the panel range: no transition, written in the same poll.
  CHECK_EQ(rig.phone.writes.size(), 1u);
  CHECK_EQ(rig.phone.level, rig.engine.status().target);
}

void small_changes_stay_inside_hysteresis() {
  Rig rig(200.0f, curve_level(200.0f, kMax));
  rig.ticks(2);
  rig.front(205.0f);
  rig.ticks(6);
  CHECK(rig.phone.writes.empty());
}

void smoothing_matches_reference_formula() {
  Rig rig(100.0f, curve_level(100.0f, kMax));
  rig.ticks(1);
  rig.front(150.0f);
  rig.tick();
  // relative 0.5 between slow (0.05) and fast (1.0): interpolated alpha.
  const float alpha = std::fmaf((0.5f - 0.05f) / (1.0f - 0.05f), 0.20f - 0.06f, 0.06f);
  CHECK_NEAR(rig.engine.status().alpha, alpha, 1e-6);
  CHECK_NEAR(rig.engine.status().smoothed_lux, std::fmaf(alpha, 150.0f, 100.0f * (1.0f - alpha)), 1e-3);
}

void spikes_are_clamped() {
  Rig rig(10.0f, curve_level(10.0f, kMax));
  rig.ticks(1);
  rig.front(5000.0f);
  rig.tick();
  CHECK_NEAR(rig.engine.status().filtered_lux, 150.0, 1e-3);  // 10 lux * spike_up
}

void fast_darkening_uses_short_transition() {
  Rig rig(400.0f, curve_level(400.0f, kMax));
  rig.ticks(2);
  rig.front(300.0f);
  rig.tick();
  rig.front(200.0f);
  rig.tick();
  CHECK(rig.engine.status().fast_dark);
  rig.ticks(8);
  CHECK(!rig.phone.writes.empty());
  CHECK(rig.phone.level < curve_level(400.0f, kMax));
}

void sunlight_boost_after_two_samples() {
  Rig rig(20000.0f, curve_level(20000.0f, kMax));
  rig.ticks(1);
  CHECK(!rig.engine.status().sunlight);
  rig.ticks(3);
  const EngineStatus s = rig.engine.status();
  CHECK(s.sunlight);
  CHECK(s.target >= static_cast<int>(kMax * 0.9f));
  CHECK_EQ(s.sunlight_count, 1);
}

void hot_phone_blocks_sunlight() {
  Rig rig(20000.0f, curve_level(20000.0f, kMax));
  rig.phone.temperature = 46;
  rig.ticks(4);
  CHECK(!rig.engine.status().sunlight);
}

void manual_mode_releases_node() {
  Rig rig(50.0f, 777);
  rig.phone.settings.brightness_mode = 0;
  rig.ticks(11);  // settings are re-read every 8 s
  rig.phone.writes.clear();
  rig.front(5000.0f);
  rig.ticks(3);
  CHECK(rig.phone.writable);
  CHECK(rig.phone.writes.empty());
  CHECK_EQ(rig.engine.poll_ms(), 5000);
  CHECK(!rig.engine.status().automatic);
}

void screen_off_then_wake_writes_curve_at_once() {
  Rig rig(30.0f, curve_level(30.0f, kMax));
  rig.ticks(3);
  rig.phone.power = 0;
  rig.ticks(3);
  CHECK_EQ(rig.engine.poll_ms(), 15000);
  CHECK(!rig.phone.light_enabled);
  rig.phone.power = 1;
  rig.front(500.0f);
  rig.tick();
  CHECK_EQ(rig.phone.writes.size(), 1u);
  CHECK_EQ(rig.phone.level, curve_level(500.0f, kMax));
  // The reference keeps the screen-off cadence for one more poll after waking.
  CHECK_EQ(rig.engine.poll_ms(), 15000);
}

void proximity_holds_brightness() {
  Rig rig(30.0f, curve_level(30.0f, kMax));
  rig.ticks(2);
  rig.phone.pending.push_back({SensorEvent::Kind::Proximity, rig.now, 0.0f});
  rig.front(3000.0f);
  rig.ticks(3);
  CHECK(rig.phone.writes.empty());
  CHECK_EQ(rig.engine.poll_ms(), 8000);
}

void night_and_low_battery_limits() {
  Rig night(800.0f, 100);
  night.phone.hour = 21;
  night.ticks(3);
  CHECK_EQ(night.engine.status().target, static_cast<int>(0.95f * static_cast<float>(curve_level(800.0f, kMax))));

  Rig battery(50000.0f, 100);
  battery.phone.capacity = 10;
  battery.config.high_lux_threshold = 0.0f;
  battery.engine.set_config(battery.config);
  battery.ticks(3);
  CHECK(battery.engine.status().target <= static_cast<int>(kMax * 0.7f));
  CHECK_EQ(battery.engine.poll_ms(), 2000);
}

void stable_light_slows_polling() {
  Rig rig(80.0f, curve_level(80.0f, kMax));
  rig.ticks(12);
  CHECK_EQ(rig.engine.poll_ms(), 1600);
  rig.ticks(10);
  CHECK_EQ(rig.engine.poll_ms(), 8000);
}

void learning_follows_reference_formula_only_for_drags() {
  Rig rig(60.0f, curve_level(60.0f, kMax));
  rig.ticks(11);  // one settings refresh after the first tick
  rig.phone.settings.brightness = 200;
  rig.phone.user_drag = false;
  rig.ticks(11);
  CHECK_EQ(rig.phone.user_checks, 1);
  CHECK_NEAR(rig.engine.user_offset(), 0.0, 1e-9);
  CHECK(rig.engine.status().last_learning == "ignored_system_change");

  rig.phone.settings.brightness = 120;
  rig.phone.user_drag = true;
  rig.ticks(11);
  const int target = rig.engine.status().target;
  const float expected =
      static_cast<float>(slider_level(120, 2.2f, kMax) - target) / static_cast<float>(kMax) * 0.1f;
  CHECK_EQ(rig.phone.user_checks, 2);
  CHECK(rig.engine.status().last_learning == "learned");
  CHECK(std::fabs(rig.engine.user_offset()) > 0.0f);
  CHECK_NEAR(rig.phone.saved_offset, rig.engine.user_offset(), 1e-9);
  CHECK(std::fabs(rig.engine.user_offset()) <= 0.3f);
  CHECK((expected > 0) == (rig.engine.user_offset() > 0));
}

void learning_can_be_disabled() {
  Config config;
  config.learning_enabled = false;
  Rig rig(60.0f, curve_level(60.0f, kMax), config);
  rig.ticks(11);
  rig.phone.settings.brightness = 220;
  rig.phone.user_drag = true;
  rig.ticks(11);
  CHECK_EQ(rig.phone.user_checks, 0);
  CHECK_NEAR(rig.engine.user_offset(), 0.0, 1e-9);
}

void rear_sensor_takes_over_when_brighter() {
  Rig rig(5.0f, curve_level(5.0f, kMax));
  rig.phone.back = true;
  rig.rear(0.0f);
  rig.ticks(3);
  CHECK(rig.engine.status().lux_source == "front");
  for (int i = 0; i < 6; ++i) {
    rig.rear(300.0f);
    rig.tick();
  }
  CHECK(rig.engine.status().lux_source == "rear");
  CHECK(rig.engine.status().smoothed_lux > 5.0f);
  rig.rear(1.0f);
  rig.ticks(6);
  CHECK(rig.engine.status().lux_source == "front");
}

void rear_sensor_never_lowers_lux() {
  Rig rig(200.0f, curve_level(200.0f, kMax));
  rig.phone.back = true;
  for (int i = 0; i < 8; ++i) {
    rig.rear(0.0f);
    rig.tick();
  }
  CHECK(rig.engine.status().lux_source == "front");
}

void reference_rear_fallback_without_dual_policy() {
  Config config;
  config.dual_sensor = false;
  Rig rig(5.0f, curve_level(5.0f, kMax), config);
  rig.phone.back = true;
  for (int i = 0; i < 6; ++i) {
    rig.rear(300.0f);
    rig.tick();
  }
  CHECK(rig.engine.status().lux_source == "front");
}

void learning_reset() {
  Rig rig(60.0f);
  rig.engine.reset_learning();
  CHECK_NEAR(rig.phone.saved_offset, 0.0, 1e-9);
}

}  // namespace

int main() {
  startup_does_not_flash_minimum();
  large_steps_wait_for_a_settled_target();
  small_steps_are_written_at_once();
  small_changes_stay_inside_hysteresis();
  smoothing_matches_reference_formula();
  spikes_are_clamped();
  fast_darkening_uses_short_transition();
  sunlight_boost_after_two_samples();
  hot_phone_blocks_sunlight();
  manual_mode_releases_node();
  screen_off_then_wake_writes_curve_at_once();
  proximity_holds_brightness();
  night_and_low_battery_limits();
  stable_light_slows_polling();
  learning_follows_reference_formula_only_for_drags();
  learning_can_be_disabled();
  rear_sensor_takes_over_when_brighter();
  rear_sensor_never_lowers_lux();
  reference_rear_fallback_without_dual_policy();
  learning_reset();
  return finish("engine_tests");
}
