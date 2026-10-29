// SPDX-License-Identifier: GPL-3.0-only
#include "check.hpp"
#include "dual_sensor.hpp"

using namespace iab;

namespace {

void ambient_lux_weights_recent_samples() {
  LightRingBuffer buffer;
  buffer.push(0, 10.0f);
  buffer.push(2500, 100.0f);
  const float fast = buffer.ambient_lux(3000, 1000);
  const float slow = buffer.ambient_lux(3000, 3000);
  // The older sample still covers the first half of the short horizon.
  CHECK_NEAR(fast, 63.9037, 1e-3);
  CHECK(slow > 10.0f && slow < fast);
  CHECK(buffer.ambient_lux(3000, 3000) < buffer.ambient_lux(3400, 3000));
  LightRingBuffer empty;
  CHECK_NEAR(empty.ambient_lux(0, 1000), -1.0, 1e-9);
}

void prune_keeps_value_at_horizon() {
  LightRingBuffer buffer;
  buffer.push(0, 5.0f);
  buffer.push(100, 7.0f);
  buffer.prune(5000);
  CHECK_NEAR(buffer.ambient_lux(6000, 1000), 7.0, 1e-6);
}

void debounce_requires_sustained_change() {
  LightRingBuffer buffer;
  buffer.push(0, 1.0f);
  buffer.push(1000, 50.0f);
  CHECK_EQ(buffer.next_brightening(1200, 50.0f, 6.0f, 6.0f), 2000);
  CHECK_EQ(buffer.next_brightening(1200, 5.0f, 6.0f, 4.0f), 6000);  // small threshold, 5 s
  CHECK_EQ(buffer.next_darkening(1200, 0.5f), 2200);
}

void thresholds_follow_device_splines() {
  const Thresholds t = thresholds_for(5.0f);
  CHECK_NEAR(t.brightening, 8.51 + (13.66 - 8.51) / 2, 1e-3);
  CHECK_NEAR(t.darkening, 0.32 + (3.53 - 0.32) / 6 * 4, 1e-3);
  CHECK_NEAR(thresholds_for(6000.0f).brightening, 6001.0, 1e-3);
}

void assist_raises_and_hands_back() {
  DualSensorPolicy policy;
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
  DualSensorPolicy policy;
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
  DualSensorPolicy policy;
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
  DualSensorPolicy policy;
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

}  // namespace

int main() {
  ambient_lux_weights_recent_samples();
  prune_keeps_value_at_horizon();
  debounce_requires_sustained_change();
  thresholds_follow_device_splines();
  assist_raises_and_hands_back();
  short_rear_flash_is_ignored();
  rear_screen_light_is_dropped();
  covered_front_uses_brighter_rear();
  return finish("dual_sensor_tests");
}
