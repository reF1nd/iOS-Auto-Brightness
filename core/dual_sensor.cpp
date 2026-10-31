// SPDX-License-Identifier: GPL-3.0-only
#include "dual_sensor.hpp"

#include <cstddef>

namespace iab {

namespace {

// Framework constants: AmbientLightRingBuffer prediction time, the 5 s both
// buffers keep (the small-threshold debounce), and the front lux below which
// the rear screen's own light on the rear sensor is dropped.
constexpr int64_t kPredictionTime = 100;
constexpr int64_t kPruneHorizon = 5000;
constexpr float kOtherDisplayMainLux = 10.0f;
// The reporter checks the scene every 500 ms; rear readings from just before
// a torch report were already lit by it.
constexpr int64_t kSceneLatency = 1000;

float interpolate(const std::vector<SplinePoint>& spline, float x) {
  if (x <= spline.front().x) return spline.front().y;
  if (x >= spline.back().x) return spline.back().y;
  size_t i = 0;
  while (x >= spline[i + 1].x) ++i;
  const float slope = (spline[i + 1].y - spline[i].y) / (spline[i + 1].x - spline[i].x);
  return spline[i].y + slope * (x - spline[i].x);
}

bool increasing(const std::vector<SplinePoint>& spline) {
  if (spline.size() < 2) return false;
  for (size_t i = 1; i < spline.size(); ++i) {
    if (!(spline[i].x > spline[i - 1].x)) return false;
  }
  return true;
}

}  // namespace

bool DualSensorConfig::valid() const {
  return increasing(brightening) && increasing(small_brightening) && increasing(darkening) && hbm_minimum_lux > 0.0f &&
         horizon_short > 0 && horizon_long >= horizon_short && weighting_intercept > 0 && brightening_debounce >= 0 &&
         small_brightening_debounce >= 0 && darkening_debounce >= 0 && assist_brightening_debounce >= 0 &&
         assist_small_brightening_debounce >= 0 && assist_darkening_debounce >= 0 && assist_factor > 0.0f;
}

Thresholds thresholds_for(const DualSensorConfig& config, float lux) {
  Thresholds result;
  const bool hbm = lux > config.hbm_minimum_lux;
  result.brightening = hbm ? lux + 1.0f : interpolate(config.brightening, lux);
  result.small_brightening = hbm ? lux + 1.0f : interpolate(config.small_brightening, lux);
  result.darkening = interpolate(config.darkening, lux);
  return result;
}

void LightRingBuffer::configure(int64_t intercept, int64_t brightening, int64_t small_brightening,
                                int64_t darkening) {
  intercept_ = intercept;
  brightening_ = brightening;
  small_brightening_ = small_brightening;
  darkening_ = darkening;
}

float LightRingBuffer::weight_integral(int64_t x) const {
  const float value = static_cast<float>(x);
  return value * (value * 0.5f + static_cast<float>(intercept_));
}

void LightRingBuffer::push(int64_t time, float lux) {
  if (!samples_.empty() && time < samples_.back().time) time = samples_.back().time;
  samples_.push_back({time, lux});
}

void LightRingBuffer::prune(int64_t horizon) {
  while (samples_.size() > 1 && samples_[1].time <= horizon) samples_.pop_front();
  if (!samples_.empty() && samples_.front().time < horizon) samples_.front().time = horizon;
}

void LightRingBuffer::drop_after(int64_t time) {
  while (!samples_.empty() && samples_.back().time > time) samples_.pop_back();
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
  return earliest + (small ? small_brightening_ : brightening_);
}

int64_t LightRingBuffer::next_darkening(int64_t now, float threshold) const {
  int64_t earliest = now;
  for (size_t i = samples_.size(); i-- > 0;) {
    if (samples_[i].lux >= threshold) break;
    earliest = samples_[i].time;
  }
  return earliest + darkening_;
}

void DualSensorPolicy::configure(const DualSensorConfig& config) {
  config_ = config;
  main_.configure(config.weighting_intercept, config.brightening_debounce, config.small_brightening_debounce,
                  config.darkening_debounce);
  // DualSensorPolicy builds its AmbientLightRingBuffer with the long horizon as intercept.
  assist_.configure(config.horizon_long, config.assist_brightening_debounce,
                    config.assist_small_brightening_debounce, config.assist_darkening_debounce);
  reset();
}

void DualSensorPolicy::reset() {
  main_.clear();
  assist_.clear();
  pending_reset_ = false;
  use_assist_ = main_valid_ = assist_valid_ = false;
  applied_ = main_fast_ = main_slow_ = assist_fast_ = assist_slow_ = -1.0f;
  main_thresholds_ = assist_thresholds_ = Thresholds{};
}

void DualSensorPolicy::reset_assist() {
  assist_.clear();
  assist_fast_ = assist_slow_ = -1.0f;
}

void DualSensorPolicy::set_scene(unsigned scene, int64_t now) {
  const unsigned rising = scene & ~scene_;
  scene_ = scene;
  if (rising & kAssistBlocked) assist_.drop_after(now - kSceneLatency);
  // SceneDetector.applyNightDrivingBrightness -> shouldResetAssistSensorValue:
  // clear the rear values now on the front sensor, else at its next reading.
  if (rising & kNightDriving) {
    if (main_valid_ && !use_assist_) {
      pending_reset_ = false;
      reset_assist();
    } else {
      pending_reset_ = true;
    }
  }
}

void DualSensorPolicy::push_main(int64_t time, float lux) {
  if (lux >= 0.0f) main_.push(time, lux);
}

void DualSensorPolicy::push_assist(int64_t time, float lux, bool other_display_on) {
  if (!(lux >= 0.0f)) return;
  if (config_.assist_factor != 1.0f) lux *= config_.assist_factor;
  if ((scene_ & kNightDriving) && main_valid_ && !use_assist_) {
    if (pending_reset_) {
      pending_reset_ = false;
      reset_assist();
    }
    return;
  }
  if (scene_ & kAssistBlocked) return;
  // HyperOS drops rear readings lit by the rear screen while the front is dim.
  if (other_display_on && main_valid_ && main_fast_ <= kOtherDisplayMainLux) {
    assist_.clear();
    lux = 0.0f;
  } else {
    assist_.prune(time - kPruneHorizon);
  }
  assist_.push(time, lux);
}

void DualSensorPolicy::apply(float lux, bool assist) {
  applied_ = lux;
  use_assist_ = assist;
  main_thresholds_ = thresholds_for(config_, lux);
}

void DualSensorPolicy::update(int64_t now) {
  if (!config_.valid()) return;
  main_.prune(now - kPruneHorizon);
  assist_.prune(now - kPruneHorizon);
  if (!main_.empty()) {
    main_fast_ = main_.ambient_lux(now, config_.horizon_short);
    main_slow_ = main_.ambient_lux(now, config_.horizon_long);
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
        assist_thresholds_ = thresholds_for(config_, assist_fast_);
      }
    }
  }
  if (assist_.empty()) return;
  assist_slow_ = assist_.ambient_lux(now, config_.horizon_long);
  assist_fast_ = assist_.ambient_lux(now, config_.horizon_short);
  if (!assist_valid_) {
    assist_valid_ = true;
    assist_slow_ = assist_fast_;
    if (assist_fast_ > main_thresholds_.brightening) apply(assist_fast_, true);
    assist_thresholds_ = thresholds_for(config_, assist_fast_);
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
    // The framework keeps its lux in a reflective scene.
    if (!(scene_ & kReflective)) apply(assist_fast_, true);
  } else if (use_assist_) {
    apply(main_fast_, false);
  }
  assist_thresholds_ = thresholds_for(config_, assist_fast_);
}

}  // namespace iab
