// SPDX-License-Identifier: GPL-3.0-only
// Port of HyperOS RefactorNitController (miui-services.jar): the anchors that
// map ambient lux to the slider's logical nits and how a slider drag moves
// them. The lux range has three segments (0..30, 30..600, 600.. lux by
// default); each keeps at most one dragged anchor next to the four default
// ones, so at most three drags are remembered.
#pragma once

#include <string>
#include <vector>

#include "brightness_map.hpp"

namespace iab {

struct Anchor {
  float lux = 0;
  float nit = 0;  // logical nits
  int order = 0;  // drag counter, 0 for default anchors
  bool user = false;
};

class NitController {
 public:
  static constexpr size_t kMaxAnchors = 7;

  explicit NitController(const RefactorConfig& config = RefactorConfig());

  // resetDefaultSpline
  void reset();
  // updateLogicalCurve: a drag to `logical` nits at `lux` (the framework's
  // ambient lux, clamped at the first high-lux point as the system does).
  void drag(float lux, float logical);
  // shouldUseGoodCurve: replaces a drag above the good curve with the good
  // curve's value. Returns true when it changed the anchors.
  bool use_good_curve();
  // getLogicalBrt with the rounding of getCurrentNit, for lux up to the first
  // high-lux point.
  float logical(float lux) const;
  float default_logical(float lux) const;

  const RefactorConfig& config() const { return config_; }
  const std::vector<Anchor>& anchors() const { return anchors_; }
  const std::vector<Anchor>& good_anchors() const { return good_; }
  bool has_good_curve() const { return has_good_; }
  int user_anchors() const;
  float user_lux() const { return user_lux_; }
  float user_logical() const { return user_logical_; }

  // learn.conf. parse() rejects files written for other default anchors.
  std::string serialize() const;
  bool parse(const std::string& text);
  // Takes over the framework's current state from its dumpsys block
  // ("anchors=[(lux,nit,user|def), ...]", goodAnchors, mIsHaveGoodCurve,
  // mUserLux, mUserLogicalNit) when it holds drags. Drag order is not dumped;
  // the dragged anchors are numbered from low to high lux.
  bool adopt(const std::string& dump);

 private:
  float default_at(float lux) const;
  int segment(float lux) const;
  bool endpoint(float lux) const;
  float blended(float anchor_lux, float base, float user_lux, float user_nit) const;
  void add_drag(std::vector<Anchor>& anchors, float lux, float nit, bool main);
  void enforce(std::vector<Anchor>& anchors, float lux, float nit) const;
  float evaluate(float lux, const std::vector<Anchor>& anchors) const;
  bool valid(const std::vector<Anchor>& anchors) const;

  RefactorConfig config_;
  std::vector<Anchor> anchors_, good_;
  int order_ = 0;
  bool has_good_ = false;
  float user_lux_ = -1.0f, user_logical_ = -1.0f;
};

}  // namespace iab
