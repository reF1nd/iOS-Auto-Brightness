// SPDX-License-Identifier: GPL-3.0-only
// Scenario tests for the reference control loop with a simulated phone.
#include <cmath>
#include <string>
#include <vector>

#include "check.hpp"
#include "engine.hpp"

using namespace iab;

namespace {

constexpr int kMax = 16383;

// The default system curve (altdb framework-ext-res) at this lux.
int curve_level(float lux, int max_level) {
  static const SystemCurve curve;
  return curve.level(curve.nits(lux), max_level);
}

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
  bool drag = false;
  float drag_lux = 0, drag_logical = 0;
  int64_t elapsed = 1000000;
  std::string saved;
  int saves = 0;
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
  bool user_drag(float& lux, float& logical) override {
    if (!drag) return false;
    drag = false;
    lux = drag_lux;
    logical = drag_logical;
    return true;
  }
  int64_t elapsed_ms() override { return elapsed; }
  void save_learning(const std::string& text) override {
    saved = text;
    ++saves;
  }
  void log(const std::string& line) override { lines.push_back(line); }
};

struct Rig {
  FakePhone phone;
  Config config;
  Engine engine;
  int64_t now = 0;

  explicit Rig(float initial_lux, int current = 1000, Config c = Config{}, const SystemCurve& curve = SystemCurve())
      : config(c), engine(phone, c, kMax, curve) {
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
    phone.elapsed += engine.poll_ms();
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

// A drag the framework learned: it reports the lux and slider logical nits
// and shows the chosen level on the panel.
void drag(Rig& rig, float lux, float logical, int level) {
  rig.phone.level = level;
  rig.phone.drag = true;
  rig.phone.drag_lux = lux;
  rig.phone.drag_logical = logical;
  rig.engine.check_learning(rig.now);
}

void settle(Rig& rig, int checks) {
  for (int i = 0; i < checks; ++i) {
    rig.now += 1000;
    rig.phone.elapsed += 1000;
    rig.engine.check_learning(rig.now);
  }
}

int user_anchors(const Engine& engine) { return engine.curve().controller().user_anchors(); }

void dark_drag_is_learned_and_kept() {
  Rig rig(0.0f, 1);
  rig.ticks(30);
  CHECK_EQ(rig.engine.status().target, curve_level(0.0f, kMax));
  drag(rig, 0.0f, 7.9595423f, 154);
  CHECK(rig.engine.status().last_learning == "learned");
  CHECK_EQ(user_anchors(rig.engine), 1);
  CHECK(rig.phone.saved.find("anchor=0 7.959542") != std::string::npos);
  // While the panel settles at the dragged level nothing is written.
  rig.phone.writes.clear();
  rig.ticks(2);
  CHECK(rig.phone.writes.empty());
  settle(rig, 2);
  CHECK_EQ(rig.engine.status().current, 154);
  // Then the curve through the new anchor takes over: 7.96 nits at 0 lux.
  rig.ticks(30);
  const SystemCurve& curve = rig.engine.curve();
  CHECK_EQ(rig.engine.status().target, curve.level(7.9595423f, kMax));
  CHECK_EQ(rig.phone.level, rig.engine.status().target);

  // After a restart the saved anchors drive the dark target by themselves.
  SystemCurve restored;
  CHECK(restored.controller().parse(rig.phone.saved));
  Rig restarted(0.0f, 1, Config{}, restored);
  restarted.ticks(30);
  CHECK_EQ(restarted.engine.status().target, curve.level(7.9595423f, kMax));
}

void learning_follows_the_system_segments() {
  Rig rig(60.0f, curve_level(60.0f, kMax));
  rig.ticks(30);
  drag(rig, 60.0f, 300.0f, 3000);
  settle(rig, 2);
  drag(rig, 200.0f, 250.0f, 2500);  // same segment, contradicts the first
  settle(rig, 2);
  CHECK_EQ(user_anchors(rig.engine), 1);
  drag(rig, 5.0f, 40.0f, 500);
  settle(rig, 2);
  drag(rig, 3000.0f, 700.0f, 9000);
  settle(rig, 2);
  CHECK_EQ(user_anchors(rig.engine), 3);
  CHECK_EQ(rig.phone.saves, 4);
}

void night_factor_applies_on_top() {
  Rig rig(0.0f, 1);
  rig.phone.hour = 23;
  rig.ticks(30);
  drag(rig, 0.0f, 50.0f, 900);
  settle(rig, 2);
  rig.ticks(30);
  const SystemCurve& curve = rig.engine.curve();
  // 50 logical nits are about 34 actual nits in the dark.
  CHECK_EQ(rig.engine.status().target, static_cast<int>(0.95f * static_cast<float>(curve.level(curve.nits(0.0f), kMax))));
  CHECK_NEAR(curve.nits(0.0f), 34.357, 1e-3);
}

void learning_waits_for_the_ramp() {
  Rig rig(60.0f, curve_level(60.0f, kMax));
  rig.ticks(30);
  drag(rig, 60.0f, 400.0f, 2500);
  rig.phone.level = 2800;
  settle(rig, 1);
  rig.phone.level = 3000;
  settle(rig, 1);
  rig.phone.writes.clear();
  rig.tick();
  CHECK(rig.phone.writes.empty());
  settle(rig, 2);
  CHECK_EQ(rig.engine.status().current, 3000);
}

void pending_drag_is_not_overwritten() {
  Rig rig(60.0f, curve_level(60.0f, kMax));
  rig.ticks(30);
  rig.phone.writes.clear();
  drag(rig, 60.0f, 400.0f, 2500);
  rig.front(5.0f);
  rig.ticks(3);
  CHECK(rig.phone.writes.empty());
}

void system_rewrites_are_not_learned() {
  Rig rig(60.0f, curve_level(60.0f, kMax));
  rig.ticks(30);
  rig.phone.level = 4000;
  rig.phone.settings.brightness = 200;
  settle(rig, 12);
  CHECK(rig.engine.status().last_learning == "none");
  CHECK_EQ(rig.phone.saves, 0);
}

void learning_can_be_disabled() {
  Config config;
  config.learning_enabled = false;
  Rig rig(60.0f, curve_level(60.0f, kMax), config);
  rig.ticks(30);
  drag(rig, 60.0f, 400.0f, 4000);
  settle(rig, 3);
  CHECK(rig.engine.status().last_learning == "disabled");
  CHECK_EQ(user_anchors(rig.engine), 0);
}

void table_curve_is_followed_not_learned() {
  const SystemCurve table({0, 30, 600, 5000}, {1, 83.13f, 126.14f, 678.4f}, PanelMap());
  Rig rig(300.0f, 100, Config{}, table);
  rig.ticks(30);
  CHECK_EQ(rig.engine.status().target, table.level(table.nits(300.0f), kMax));
  drag(rig, 300.0f, 400.0f, 4000);
  CHECK(rig.engine.status().last_learning == "unsupported");
  CHECK_EQ(rig.phone.saves, 0);
}

// A drag above the good curve (900 logical nits at 500 lux, good 421).
Rig bright_drag() {
  Rig rig(500.0f, curve_level(500.0f, kMax));
  rig.ticks(30);
  drag(rig, 500.0f, 900.0f, 8000);
  settle(rig, 2);
  CHECK(rig.engine.curve().controller().has_good_curve());
  return rig;
}

void screen_off(Rig& rig, int64_t milliseconds, float wake_lux) {
  rig.phone.power = 0;
  rig.ticks(2);
  rig.phone.elapsed += milliseconds;
  rig.phone.power = 1;
  rig.front(wake_lux);
  rig.ticks(3);
}

void good_curve_after_long_screen_off() {
  Rig rig = bright_drag();
  screen_off(rig, 31 * 60 * 1000, 500.0f);
  CHECK(!rig.engine.curve().controller().has_good_curve());
  CHECK(rig.engine.status().last_learning == "good_curve");
  CHECK_NEAR(rig.engine.curve().controller().logical(500.0f), 421.134, 1e-3);
  CHECK(rig.phone.saved.find("good_curve=0") != std::string::npos);
}

void good_curve_needs_time_and_light_change() {
  Rig short_off = bright_drag();
  screen_off(short_off, 5 * 60 * 1000, 100.0f);
  CHECK(short_off.engine.curve().controller().has_good_curve());
  Rig same_light = bright_drag();
  screen_off(same_light, 10 * 60 * 1000, 505.0f);
  CHECK(same_light.engine.curve().controller().has_good_curve());
  Rig changed = bright_drag();
  screen_off(changed, 7 * 60 * 1000, 100.0f);
  CHECK(!changed.engine.curve().controller().has_good_curve());

  RefactorConfig config;
  config.reset_policy = false;
  Rig disabled(500.0f, curve_level(500.0f, kMax), Config{}, SystemCurve(config, PanelMap()));
  disabled.ticks(30);
  drag(disabled, 500.0f, 900.0f, 8000);
  settle(disabled, 2);
  screen_off(disabled, 60 * 60 * 1000, 500.0f);
  CHECK(disabled.engine.curve().controller().has_good_curve());
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
  Rig rig(0.0f, 1);
  rig.ticks(30);
  drag(rig, 0.0f, 7.9595423f, 154);
  settle(rig, 2);
  rig.engine.reset_learning();
  CHECK_EQ(user_anchors(rig.engine), 0);
  CHECK(rig.phone.saved.find("order=0\n") != std::string::npos);
  CHECK_EQ(rig.engine.curve_level(0.0f), curve_level(0.0f, kMax));
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
  dark_drag_is_learned_and_kept();
  learning_follows_the_system_segments();
  night_factor_applies_on_top();
  learning_waits_for_the_ramp();
  pending_drag_is_not_overwritten();
  system_rewrites_are_not_learned();
  learning_can_be_disabled();
  table_curve_is_followed_not_learned();
  good_curve_after_long_screen_off();
  good_curve_needs_time_and_light_change();
  rear_sensor_takes_over_when_brighter();
  rear_sensor_never_lowers_lux();
  reference_rear_fallback_without_dual_policy();
  learning_reset();
  return finish("engine_tests");
}
