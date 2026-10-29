// SPDX-License-Identifier: GPL-3.0-only
// Front/rear light sensor selection modelled on HyperOS
// com.android.server.display.DualSensorPolicy: the rear (assist) sensor takes
// over only when it is brighter than the front sensor's brightening threshold,
// debounced like the framework's ambient lux, and hands back once the front
// sensor is at least as bright. It can therefore raise but never lower lux.
#pragma once

#include <cstdint>
#include <deque>

namespace iab {

class LightRingBuffer {
 public:
  void clear() { samples_.clear(); }
  bool empty() const { return samples_.empty(); }
  void push(int64_t time, float lux);
  void prune(int64_t horizon);
  // Framework calculateAmbientLux(): time-weighted, recent samples dominate.
  float ambient_lux(int64_t now, int64_t horizon) const;
  int64_t next_brightening(int64_t now, float lux, float threshold, float small_threshold) const;
  int64_t next_darkening(int64_t now, float threshold) const;

 private:
  struct Sample {
    int64_t time;
    float lux;
  };
  std::deque<Sample> samples_;
};

struct Thresholds {
  float brightening = -1.0f;
  float small_brightening = -1.0f;
  float darkening = -1.0f;
};
// MiuiHysteresisLevels of the target device (HBM minimum lux 4475).
Thresholds thresholds_for(float lux);

class DualSensorPolicy {
 public:
  void reset();
  void push_main(int64_t time, float lux);
  void push_assist(int64_t time, float lux, bool other_display_on);
  void update(int64_t now);

  bool using_assist() const { return use_assist_; }
  // Ambient lux applied from the assist sensor while it is in control.
  float assist_lux() const { return applied_; }
  float main_fast() const { return main_fast_; }
  float assist_fast() const { return assist_fast_; }
  float applied() const { return applied_; }

 private:
  void apply(float lux, bool assist);

  LightRingBuffer main_, assist_;
  bool use_assist_ = false;
  bool main_valid_ = false, assist_valid_ = false;
  float applied_ = -1.0f;
  float main_fast_ = -1.0f, main_slow_ = -1.0f;
  float assist_fast_ = -1.0f, assist_slow_ = -1.0f;
  Thresholds main_thresholds_, assist_thresholds_;
};

}  // namespace iab
