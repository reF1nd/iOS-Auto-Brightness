// SPDX-License-Identifier: GPL-3.0-only
#include "dual_sensor.hpp"

#include <cstddef>

namespace iab {

namespace {

// Framework configuration read from dumpsys display on Xiaomi 17 Pro Max.
constexpr int64_t kHorizonShort = 1000;
constexpr int64_t kHorizonLong = 3000;
// Both buffers keep the 5 s the small-threshold debounce needs.
constexpr int64_t kPruneHorizon = 5000;
constexpr int64_t kBrighteningDebounce = 1000;
constexpr int64_t kSmallBrighteningDebounce = 5000;
constexpr int64_t kDarkeningDebounce = 1000;
constexpr int64_t kPredictionTime = 100;
constexpr float kHbmMinimumLux = 4475.0f;
constexpr float kOtherDisplayMainLux = 10.0f;

struct Point {
  float x, y;
};

constexpr Point kBright[] = {
    {0, 2},       {3, 6},          {4, 8.51f},       {6, 13.66f},     {7, 14.84f},
    {8, 15.46f},  {9, 17.22f},     {15, 28.44f},     {30, 160},       {600, 992.58f},
    {1677.4f, 3000}, {2000, 4038.77f}, {2500, 5738.77f}, {3000, 7538.77f}, {8500, 13038.77f},
};
constexpr Point kSmallBright[] = {
    {0, 2},          {3, 6},            {4, 8.51f},       {6, 13.66f},     {7, 14.84f},
    {8, 15.46f},     {9, 17.22f},       {15, 23.064001f}, {30, 108},       {600, 835.54803f},
    {1677.4f, 2470.96f}, {2000, 3223.2622f}, {2500, 4443.262f}, {3000, 5738.77f}, {8500, 11223.262f},
};
constexpr Point kDark[] = {
    {0, 0},        {1, 0.32f},     {7, 3.53f},      {8, 3.8f},        {9, 4.1f},
    {10, 4.45f},   {11, 4.82f},    {12, 5.2f},      {13, 5.58f},      {14, 6.05f},
    {15, 6.9f},    {30, 15.83f},   {199, 34.31f},   {200, 41.39f},    {360, 45.34f},
    {600, 216.99f}, {1010.47f, 600}, {3000, 2052.56f}, {20000, 10011.96f},
};

template <size_t N>
float interpolate(const Point (&spline)[N], float x) {
  if (x <= spline[0].x) return spline[0].y;
  if (x >= spline[N - 1].x) return spline[N - 1].y;
  size_t i = 0;
  while (x >= spline[i + 1].x) ++i;
  const float slope = (spline[i + 1].y - spline[i].y) / (spline[i + 1].x - spline[i].x);
  return spline[i].y + slope * (x - spline[i].x);
}

float weight_integral(int64_t x) {
  const float value = static_cast<float>(x);
  return value * (value * 0.5f + static_cast<float>(kHorizonLong));
}

}  // namespace

Thresholds thresholds_for(float lux) {
  Thresholds result;
  result.brightening = lux > kHbmMinimumLux ? lux + 1.0f : interpolate(kBright, lux);
  result.small_brightening = lux > kHbmMinimumLux ? lux + 1.0f : interpolate(kSmallBright, lux);
  result.darkening = interpolate(kDark, lux);
  return result;
}

void LightRingBuffer::push(int64_t time, float lux) {
  if (!samples_.empty() && time < samples_.back().time) time = samples_.back().time;
  samples_.push_back({time, lux});
}

void LightRingBuffer::prune(int64_t horizon) {
  while (samples_.size() > 1 && samples_[1].time <= horizon) samples_.pop_front();
  if (!samples_.empty() && samples_.front().time < horizon) samples_.front().time = horizon;
}

float LightRingBuffer::ambient_lux(int64_t now, int64_t horizon) const {
  const size_t count = samples_.size();
  if (count == 0) return -1.0f;
  const int64_t start = now - horizon;
  size_t end_index = 0;
  while (end_index + 1 < count && samples_[end_index + 1].time <= start) ++end_index;
  float sum = 0.0f, total = 0.0f;
  int64_t end_time = kPredictionTime;
  for (size_t i = count; i-- > end_index;) {
    int64_t event_time = samples_[i].time;
    if (i == end_index && event_time < start) event_time = start;
    const int64_t start_time = event_time - now;
    const float weight = weight_integral(end_time) - weight_integral(start_time);
    total += weight;
    sum += samples_[i].lux * weight;
    end_time = start_time;
  }
  return total > 0.0f ? sum / total : samples_.back().lux;
}

int64_t LightRingBuffer::next_brightening(int64_t now, float lux, float threshold,
                                          float small_threshold) const {
  const bool small = lux < threshold;
  const float limit = small ? small_threshold : threshold;
  int64_t earliest = now;
  for (size_t i = samples_.size(); i-- > 0;) {
    if (samples_[i].lux <= limit) break;
    earliest = samples_[i].time;
  }
  return earliest + (small ? kSmallBrighteningDebounce : kBrighteningDebounce);
}

int64_t LightRingBuffer::next_darkening(int64_t now, float threshold) const {
  int64_t earliest = now;
  for (size_t i = samples_.size(); i-- > 0;) {
    if (samples_[i].lux >= threshold) break;
    earliest = samples_[i].time;
  }
  return earliest + kDarkeningDebounce;
}

void DualSensorPolicy::reset() {
  main_.clear();
  assist_.clear();
  use_assist_ = main_valid_ = assist_valid_ = false;
  applied_ = main_fast_ = main_slow_ = assist_fast_ = assist_slow_ = -1.0f;
  main_thresholds_ = assist_thresholds_ = Thresholds{};
}

void DualSensorPolicy::push_main(int64_t time, float lux) {
  if (lux >= 0.0f) main_.push(time, lux);
}

void DualSensorPolicy::push_assist(int64_t time, float lux, bool other_display_on) {
  if (!(lux >= 0.0f)) return;
  // HyperOS drops rear readings lit by the rear screen while the front is dim.
  if (other_display_on && main_valid_ && main_fast_ <= kOtherDisplayMainLux) {
    assist_.clear();
    lux = 0.0f;
  }
  assist_.push(time, lux);
}

void DualSensorPolicy::apply(float lux, bool assist) {
  applied_ = lux;
  use_assist_ = assist;
  main_thresholds_ = thresholds_for(lux);
}

void DualSensorPolicy::update(int64_t now) {
  main_.prune(now - kPruneHorizon);
  assist_.prune(now - kPruneHorizon);
  if (!main_.empty()) {
    main_fast_ = main_.ambient_lux(now, kHorizonShort);
    main_slow_ = main_.ambient_lux(now, kHorizonLong);
  }
  if (!main_valid_) {
    if (main_.empty()) return;
    main_valid_ = true;
    apply(main_fast_, false);
  } else {
    const Thresholds& t = main_thresholds_;
    const float threshold = main_fast_ < t.brightening ? t.small_brightening : t.brightening;
    const bool brighten = main_slow_ >= threshold && main_fast_ >= threshold &&
                          main_.next_brightening(now, main_fast_, t.brightening,
                                                 t.small_brightening) <= now;
    const bool darken =
        main_fast_ <= t.darkening && main_.next_darkening(now, t.darkening) <= now;
    if (brighten) {
      if (!use_assist_ || main_fast_ >= assist_fast_) apply(main_fast_, false);
    } else if (darken) {
      if (main_fast_ >= assist_fast_) {
        apply(main_fast_, false);
      } else if (!use_assist_) {
        apply(assist_fast_, true);
        assist_thresholds_ = thresholds_for(assist_fast_);
      }
    }
  }
  if (assist_.empty()) return;
  assist_slow_ = assist_.ambient_lux(now, kHorizonLong);
  assist_fast_ = assist_.ambient_lux(now, kHorizonShort);
  if (!assist_valid_) {
    assist_valid_ = true;
    assist_slow_ = assist_fast_;
    if (assist_fast_ > main_thresholds_.brightening) apply(assist_fast_, true);
    assist_thresholds_ = thresholds_for(assist_fast_);
    return;
  }
  const Thresholds& a = assist_thresholds_;
  const float threshold = assist_fast_ < a.brightening ? a.small_brightening : a.brightening;
  const bool brighten = assist_slow_ >= threshold && assist_fast_ >= threshold &&
                        assist_.next_brightening(now, assist_fast_, a.brightening,
                                                 a.small_brightening) <= now;
  const bool darken =
      assist_fast_ <= a.darkening && assist_.next_darkening(now, a.darkening) <= now;
  if (!brighten && !darken) return;
  const float main_brightening = main_thresholds_.brightening;
  if ((!use_assist_ && assist_slow_ > main_brightening && assist_fast_ > main_brightening) ||
      (use_assist_ && assist_fast_ > main_fast_)) {
    apply(assist_fast_, true);
  } else if (use_assist_) {
    apply(main_fast_, false);
  }
  assist_thresholds_ = thresholds_for(assist_fast_);
}

}  // namespace iab
