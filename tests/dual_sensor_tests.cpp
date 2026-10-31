// SPDX-License-Identifier: GPL-3.0-only
#include "altdb_display.hpp"
#include "check.hpp"
#include "dual_sensor.hpp"

using namespace iab;

namespace {

// AutomaticBrightnessController weighting and debounces on altdb.
LightRingBuffer ring() {
  LightRingBuffer buffer;
  buffer.configure(3000, 1000, 5000, 1000);
  return buffer;
}

DualSensorPolicy policy_for_test() {
  DualSensorPolicy policy;
  policy.configure(test::altdb_dual_sensor());
  return policy;
}

// Both sensors dim for 3 s: the front sensor is in control.
int64_t settle(DualSensorPolicy& policy, float main, float assist) {
  int64_t t = 0;
  for (; t <= 3000; t += 250) {
    policy.push_main(t, main);
    policy.push_assist(t, assist, false);
    policy.update(t);
  }
  return t;
}

void ambient_lux_weights_recent_samples() {
  LightRingBuffer buffer = ring();
  buffer.push(0, 10.0f);
  buffer.push(2500, 100.0f);
  const float fast = buffer.ambient_lux(3000, 1000);
  const float slow = buffer.ambient_lux(3000, 3000);
  // The older sample still covers the first half of the short horizon.
  CHECK_NEAR(fast, 63.9037, 1e-3);
  CHECK(slow > 10.0f && slow < fast);
  CHECK(buffer.ambient_lux(3000, 3000) < buffer.ambient_lux(3400, 3000));
  LightRingBuffer empty = ring();
  CHECK_NEAR(empty.ambient_lux(0, 1000), -1.0, 1e-9);
}

void prune_keeps_value_at_horizon() {
  LightRingBuffer buffer = ring();
  buffer.push(0, 5.0f);
  buffer.push(100, 7.0f);
  buffer.prune(5000);
  CHECK_NEAR(buffer.ambient_lux(6000, 1000), 7.0, 1e-6);
}

void debounce_requires_sustained_change() {
  LightRingBuffer buffer = ring();
  buffer.push(0, 1.0f);
  buffer.push(1000, 50.0f);
  CHECK_EQ(buffer.next_brightening(1200, 50.0f, 6.0f, 6.0f), 2000);
  CHECK_EQ(buffer.next_brightening(1200, 5.0f, 6.0f, 4.0f), 6000);  // small threshold, 5 s
  CHECK_EQ(buffer.next_darkening(1200, 0.5f), 2200);
}

void assist_raises_and_hands_back() {
  DualSensorPolicy policy = policy_for_test();
  int64_t t = 0;
  for (; t <= 3000; t += 250) {
    policy.push_main(t, 5.0f);
    policy.push_assist(t, 0.0f, false);
    policy.update(t);
  }
  CHECK(!policy.using_assist());
  for (; t <= 7000; t += 250) {
    policy.push_main(t, 5.0f);
    policy.push_assist(t, 200.0f, false);
    policy.update(t);
  }
  CHECK(policy.using_assist());
  CHECK(policy.assist_lux() > 100.0f);
  // 400 lux is below the big brightening threshold around 200 lux (408), so
  // the main sensor needs the 5 s small-threshold debounce to take back over.
  for (; t <= 11000; t += 250) {
    policy.push_main(t, 400.0f);
    policy.push_assist(t, 200.0f, false);
    policy.update(t);
  }
  CHECK(policy.using_assist());
  for (; t <= 14000; t += 250) {
    policy.push_main(t, 400.0f);
    policy.push_assist(t, 200.0f, false);
    policy.update(t);
  }
  CHECK(!policy.using_assist());
}

void short_rear_flash_is_ignored() {
  DualSensorPolicy policy = policy_for_test();
  int64_t t = 0;
  for (; t <= 3000; t += 250) {
    policy.push_main(t, 5.0f);
    policy.push_assist(t, 0.0f, false);
    policy.update(t);
  }
  policy.push_assist(t, 500.0f, false);
  policy.update(t);
  t += 250;
  policy.push_assist(t, 0.0f, false);
  policy.update(t);
  for (t += 250; t <= 6000; t += 250) policy.update(t);
  CHECK(!policy.using_assist());
}

void rear_screen_light_is_dropped() {
  DualSensorPolicy policy = policy_for_test();
  int64_t t = 0;
  for (; t <= 3000; t += 250) {
    policy.push_main(t, 2.0f);
    policy.update(t);
  }
  for (; t <= 8000; t += 250) {
    policy.push_main(t, 2.0f);
    policy.push_assist(t, 300.0f, true);
    policy.update(t);
  }
  CHECK(!policy.using_assist());
}

void covered_front_uses_brighter_rear() {
  DualSensorPolicy policy = policy_for_test();
  int64_t t = 0;
  for (; t <= 4000; t += 250) {
    policy.push_main(t, 80.0f);
    policy.push_assist(t, 40.0f, false);
    policy.update(t);
  }
  CHECK(!policy.using_assist());
  for (; t <= 7000; t += 250) {
    policy.push_main(t, 0.0f);
    policy.push_assist(t, 40.0f, false);
    policy.update(t);
  }
  CHECK(policy.using_assist());
  CHECK_NEAR(policy.assist_lux(), 40.0, 1.0);
}

void drop_after_forgets_recent_samples() {
  LightRingBuffer buffer = ring();
  buffer.push(0, 1.0f);
  buffer.push(500, 900.0f);
  buffer.push(800, 900.0f);
  buffer.drop_after(400);
  CHECK_NEAR(buffer.ambient_lux(1000, 1000), 1.0, 1e-6);
}

void unconfigured_policy_stays_on_front() {
  DualSensorPolicy policy;
  CHECK(!policy.configured());
  for (int64_t t = 0; t <= 8000; t += 250) {
    policy.push_main(t, 2.0f);
    policy.push_assist(t, 500.0f, false);
    policy.update(t);
  }
  CHECK(!policy.using_assist());
}

void torch_readings_are_dropped() {
  DualSensorPolicy policy = policy_for_test();
  int64_t t = settle(policy, 5.0f, 0.0f);
  // The torch lights the rear sensor; the report arrives half a second later.
  policy.push_assist(t, 800.0f, false);
  policy.update(t);
  t += 250;
  policy.push_assist(t, 800.0f, false);
  policy.update(t);
  policy.set_scene(DualSensorPolicy::kAssistBlocked, t + 250);
  for (t += 250; t <= 9000; t += 250) {
    policy.push_main(t, 5.0f);
    policy.push_assist(t, 800.0f, false);
    policy.update(t);
  }
  CHECK(!policy.using_assist());
  // Torch off: the rear sensor counts again.
  policy.set_scene(0, t);
  for (; t <= 14000; t += 250) {
    policy.push_main(t, 5.0f);
    policy.push_assist(t, 800.0f, false);
    policy.update(t);
  }
  CHECK(policy.using_assist());
}

void night_driving_keeps_front() {
  DualSensorPolicy policy = policy_for_test();
  int64_t t = settle(policy, 5.0f, 0.0f);
  policy.set_scene(DualSensorPolicy::kNightDriving, t);
  for (; t <= 9000; t += 250) {
    policy.push_main(t, 5.0f);
    policy.push_assist(t, 300.0f, false);
    policy.update(t);
  }
  CHECK(!policy.using_assist());
  CHECK_NEAR(policy.assist_fast(), -1.0, 1e-6);
}

void reflective_scene_blocks_switching() {
  DualSensorPolicy policy = policy_for_test();
  int64_t t = settle(policy, 5.0f, 0.0f);
  policy.set_scene(DualSensorPolicy::kReflective, t);
  for (; t <= 9000; t += 250) {
    policy.push_main(t, 5.0f);
    policy.push_assist(t, 300.0f, false);
    policy.update(t);
  }
  CHECK(!policy.using_assist());
  // The rear thresholds kept following meanwhile: the next rise switches.
  policy.set_scene(0, t);
  for (; t <= 12000; t += 250) {
    policy.push_main(t, 5.0f);
    policy.push_assist(t, 300.0f, false);
    policy.update(t);
  }
  CHECK(!policy.using_assist());
  for (; t <= 16000; t += 250) {
    policy.push_main(t, 5.0f);
    policy.push_assist(t, 1500.0f, false);
    policy.update(t);
  }
  CHECK(policy.using_assist());
}

void rear_factor_scales_readings() {
  DualSensorConfig config = test::altdb_dual_sensor();
  config.assist_factor = 2.0f;
  DualSensorPolicy policy;
  policy.configure(config);
  int64_t t = settle(policy, 5.0f, 0.0f);
  for (; t <= 9000; t += 250) {
    policy.push_main(t, 5.0f);
    policy.push_assist(t, 100.0f, false);
    policy.update(t);
  }
  CHECK(policy.using_assist());
  CHECK_NEAR(policy.assist_lux(), 200.0, 1.0);
}

}  // namespace

int main() {
  ambient_lux_weights_recent_samples();
  prune_keeps_value_at_horizon();
  debounce_requires_sustained_change();
  assist_raises_and_hands_back();
  short_rear_flash_is_ignored();
  rear_screen_light_is_dropped();
  covered_front_uses_brighter_rear();
  drop_after_forgets_recent_samples();
  unconfigured_policy_stays_on_front();
  torch_readings_are_dropped();
  night_driving_keeps_front();
  reflective_scene_blocks_switching();
  rear_factor_scales_readings();
  return finish("dual_sensor_tests");
}
