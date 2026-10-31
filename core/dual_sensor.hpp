// SPDX-License-Identifier: GPL-3.0-only
// Front/rear light sensor selection modelled on HyperOS
// com.android.server.display.DualSensorPolicy: the rear (assist) sensor takes
// over only when it is brighter than the front sensor's brightening threshold,
// debounced like the framework's ambient lux, and hands back once the front
// sensor is at least as bright. It can therefore raise but never lower lux.
// Thresholds, debounces and horizons are the running system's, read from
// dumpsys display (display_dump.hpp); without them the policy stays off.
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace iab {

struct SplinePoint {
  float x, y;
};

// The main display's light sensor configuration as the framework uses it.
struct DualSensorConfig {
  // MiuiHysteresisLevels (HysteresisLevelsImpl) splines.
  std::vector<SplinePoint> brightening, small_brightening, darkening;
  // HighBrightnessModeData.minimumLux; HysteresisLevelsImpl uses 6000 without HBM.
  float hbm_minimum_lux = 6000.0f;
  // AutomaticBrightnessController: horizons, weighting and debounces of the front sensor.
  int64_t horizon_short = 0, horizon_long = 0, weighting_intercept = 0;
  int64_t brightening_debounce = 0, small_brightening_debounce = 0, darkening_debounce = 0;
  // DualSensorPolicy: debounces of the rear sensor (its weighting uses horizon_long).
  int64_t assist_brightening_debounce = 0, assist_small_brightening_debounce = 0, assist_darkening_debounce = 0;
  // ro.miui.sensor.fov_back.factor: rear readings are scaled by it.
  float assist_factor = 1.0f;

  bool valid() const;
};

class LightRingBuffer {
 public:
  void configure(int64_t intercept, int64_t brightening, int64_t small_brightening, int64_t darkening);
  void clear() { samples_.clear(); }
  bool empty() const { return samples_.empty(); }
  void push(int64_t time, float lux);
  void prune(int64_t horizon);
  // Forgets the samples taken after this time.
  void drop_after(int64_t time);
  // Framework calculateAmbientLux(): time-weighted, recent samples dominate.
  float ambient_lux(int64_t now, int64_t horizon) const;
  int64_t next_brightening(int64_t now, float lux, float threshold, float small_threshold) const;
  int64_t next_darkening(int64_t now, float threshold) const;

 private:
  struct Sample {
    int64_t time;
    float lux;
  };
  float weight_integral(int64_t x) const;

  std::deque<Sample> samples_;
  int64_t intercept_ = 0, brightening_ = 0, small_brightening_ = 0, darkening_ = 0;
};

struct Thresholds {
  float brightening = -1.0f;
  float small_brightening = -1.0f;
  float darkening = -1.0f;
};
// HysteresisLevelsImpl: above the HBM minimum lux both brightening thresholds are lux + 1.
Thresholds thresholds_for(const DualSensorConfig& config, float lux);

class DualSensorPolicy {
 public:
  // Scene flags the framework reports (zygisk/java/iab/Reporter.java).
  enum Scene : unsigned {
    // AutomaticBrightnessControllerImpl drops rear readings: the torch is on,
    // went off less than 1.8 s ago, or the ROM disables the assist sensor.
    kAssistBlocked = 1u << 0,
    // SceneDetector night driving: rear readings are dropped while the front
    // sensor is in use.
    kNightDriving = 1u << 1,
    // SceneDetector reflective scene on a rear-screen device: no switching to
    // the rear sensor.
    kReflective = 1u << 2,
  };

  void configure(const DualSensorConfig& config);
  bool configured() const { return config_.valid(); }
  const DualSensorConfig& config() const { return config_; }
  void reset();
  void set_scene(unsigned scene, int64_t now);
  unsigned scene() const { return scene_; }
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
  void reset_assist();

  DualSensorConfig config_;
  LightRingBuffer main_, assist_;
  unsigned scene_ = 0;
  bool pending_reset_ = false;
  bool use_assist_ = false;
  bool main_valid_ = false, assist_valid_ = false;
  float applied_ = -1.0f;
  float main_fast_ = -1.0f, main_slow_ = -1.0f;
  float assist_fast_ = -1.0f, assist_slow_ = -1.0f;
  Thresholds main_thresholds_, assist_thresholds_;
};

}  // namespace iab
