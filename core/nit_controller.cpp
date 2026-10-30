// SPDX-License-Identifier: GPL-3.0-only
#include "nit_controller.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace iab {

namespace {

constexpr float kSame = 0.001f;

float clamp_range(float value, float low, float high) { return std::max(low, std::min(high, value)); }

}  // namespace

NitController::NitController(const RefactorConfig& config) : config_(config.valid() ? config : RefactorConfig()) {
  reset();
}

void NitController::reset() {
  const std::vector<float>& curve = config_.default_curve;
  anchors_ = {{0.0f, curve[1], 0, false},
              {config_.seg0(), curve[3], 0, false},
              {config_.seg1(), curve[6], 0, false},
              {config_.lux_max(), config_.max_nit, 0, false}};
  good_ = anchors_;
  order_ = 0;
  has_good_ = false;
  user_lux_ = user_logical_ = -1.0f;
}

float NitController::default_at(float lux) const {
  const std::vector<float>& curve = config_.default_curve;
  const float seg0 = config_.seg0(), seg1 = config_.seg1(), lux_max = config_.lux_max();
  if (!(lux > 0.0f)) return curve[1];
  if (lux <= seg0) return curve[1] + (curve[3] - curve[1]) * lux / seg0;
  if (lux <= seg1) return curve[3] + (curve[6] - curve[3]) * (lux - seg0) / (seg1 - seg0);
  if (lux <= lux_max) return curve[6] + (config_.max_nit - curve[6]) * (lux - seg1) / (lux_max - seg1);
  return config_.max_nit;
}

int NitController::segment(float lux) const {
  if (lux < config_.seg0()) return 0;
  return lux < config_.seg1() ? 1 : 2;
}

bool NitController::endpoint(float lux) const {
  for (const float at : {0.0f, config_.seg0(), config_.seg1(), config_.lux_max()}) {
    if (std::fabs(lux - at) < kSame) return true;
  }
  return false;
}

// blendedNitAtLux: a drag pulls the two ends of its segment by its distance
// from the default curve, weighted by how close each end is, never past the
// dragged value.
float NitController::blended(float anchor_lux, float base, float user_lux, float user_nit) const {
  float from, to;
  if (user_lux < config_.seg0()) from = 0.0f, to = config_.seg0();
  else if (user_lux < config_.seg1()) from = config_.seg0(), to = config_.seg1();
  else from = config_.seg1(), to = config_.lux_max();
  if (std::fabs(user_lux - from) < kSame || std::fabs(user_lux - to) < kSame) return base;
  if (!(std::fabs(anchor_lux - from) < kSame || std::fabs(anchor_lux - to) < kSame)) return base;
  const float delta = user_nit - default_at(user_lux), width = to - from;
  if (!(width > 0.0f)) return base;
  const float weight = (width - std::fabs(anchor_lux - user_lux)) / width;
  return clamp_range(delta * weight + base, std::min(base, user_nit), std::max(base, user_nit));
}

// addDragOnArrays
void NitController::add_drag(std::vector<Anchor>& anchors, float lux, float nit, bool main) {
  const float at = clamp_range(lux, 0.0f, config_.lux_max());
  const float value = clamp_range(nit, config_.min_nit, config_.max_nit);
  if (main) ++order_;
  const int order = order_;
  const int seg = segment(at);
  // Earlier drags in the same segment, and those the new one contradicts.
  for (size_t i = anchors.size(); i-- > 0;) {
    const Anchor& old = anchors[i];
    if (!old.user || endpoint(old.lux)) continue;
    const bool conflict = (old.lux < at && old.nit > value) || (old.lux > at && old.nit < value);
    if (segment(old.lux) == seg || conflict) anchors.erase(anchors.begin() + static_cast<long>(i));
  }
  size_t nearest = 0;
  float distance = std::fabs(anchors[0].lux - at);
  for (size_t i = 1; i < anchors.size(); ++i) {
    const float d = std::fabs(anchors[i].lux - at);
    if (d < distance) distance = d, nearest = i;
  }
  if (distance < kSame) {
    anchors[nearest].nit = value;
    anchors[nearest].order = order;
    anchors[nearest].user = true;
  } else if (anchors.size() < kMaxAnchors) {
    size_t index = anchors.size();
    for (size_t i = 0; i < anchors.size(); ++i) {
      if (anchors[i].lux > at) {
        index = i;
        break;
      }
    }
    anchors.insert(anchors.begin() + static_cast<long>(index), Anchor{at, value, order, true});
  } else {
    // The oldest inner anchor makes room (never reached with three segments).
    int oldest = -1, lowest = INT_MAX;
    for (size_t i = 1; i + 1 < anchors.size(); ++i) {
      if (anchors[i].order < lowest) lowest = anchors[i].order, oldest = static_cast<int>(i);
    }
    if (oldest >= 0) {
      anchors[static_cast<size_t>(oldest)] = {at, value, order, true};
      std::stable_sort(anchors.begin(), anchors.end(), [](const Anchor& a, const Anchor& b) { return a.lux < b.lux; });
    }
  }
  enforce(anchors, at, value);
}

// enforceMonotonicityAndRestore: default anchors go back to the default
// curve, the dragged segment's ends are blended, and everything left of the
// drag is capped by it and everything right of it floored by it.
void NitController::enforce(std::vector<Anchor>& anchors, float lux, float nit) const {
  size_t k = anchors.size();
  for (size_t i = 0; i < anchors.size(); ++i) {
    if (std::fabs(anchors[i].lux - lux) < kSame) {
      k = i;
      break;
    }
  }
  if (k == anchors.size()) return;
  const auto base = [&](const Anchor& anchor) { return anchor.user ? anchor.nit : default_at(anchor.lux); };
  float previous = anchors[k].nit;
  for (size_t i = k; i-- > 0;) {
    anchors[i].nit = std::min(blended(anchors[i].lux, base(anchors[i]), lux, nit), previous);
    previous = anchors[i].nit;
  }
  previous = anchors[k].nit;
  for (size_t i = k + 1; i < anchors.size(); ++i) {
    anchors[i].nit = std::max(blended(anchors[i].lux, base(anchors[i]), lux, nit), previous);
    previous = anchors[i].nit;
  }
}

// updateLogicalCurve
void NitController::drag(float lux, float logical) {
  const float at = lux >= config_.high_lux[0] ? config_.high_lux[0] : lux;
  user_lux_ = at;
  user_logical_ = logical;
  add_drag(anchors_, at, logical, true);
  const float good = linear_spline(config_.good_lux, config_.good_nit, at);
  has_good_ = false;
  if (good < logical) {
    good_ = anchors_;
    add_drag(good_, at, good, false);
    has_good_ = true;
  }
}

bool NitController::use_good_curve() {
  if (!has_good_) return false;
  anchors_ = good_;
  has_good_ = false;
  return true;
}

// evaluateOnArray: linear in lux between anchors, flat outside them.
float NitController::evaluate(float lux, const std::vector<Anchor>& anchors) const {
  if (anchors.empty()) return config_.min_nit;
  if (lux <= anchors.front().lux) return anchors.front().nit;
  if (lux >= anchors.back().lux) return anchors.back().nit;
  for (size_t i = 0; i + 1 < anchors.size(); ++i) {
    if (lux <= anchors[i + 1].lux) {
      const float t = (lux - anchors[i].lux) / (anchors[i + 1].lux - anchors[i].lux);
      return clamp_range(anchors[i].nit + (anchors[i + 1].nit - anchors[i].nit) * t, config_.min_nit, config_.max_nit);
    }
  }
  return anchors.back().nit;
}

float NitController::logical(float lux) const {
  const float at = lux > config_.high_lux[0] ? config_.high_lux[0] : lux;
  return static_cast<float>(std::floor(static_cast<double>(evaluate(at, anchors_)) * 1000.0 + 0.5)) / 1000.0f;
}

float NitController::default_logical(float lux) const {
  const float at = lux > config_.high_lux[0] ? config_.high_lux[0] : lux;
  return static_cast<float>(std::floor(static_cast<double>(default_at(at)) * 1000.0 + 0.5)) / 1000.0f;
}

int NitController::user_anchors() const {
  return static_cast<int>(std::count_if(anchors_.begin(), anchors_.end(), [](const Anchor& a) { return a.user; }));
}

std::string NitController::serialize() const {
  std::string text = "version=2\ndefaults=";
  char item[96];
  for (size_t i = 0; i < config_.default_curve.size(); ++i) {
    std::snprintf(item, sizeof item, "%s%.9g", i ? " " : "", config_.default_curve[i]);
    text += item;
  }
  std::snprintf(item, sizeof item, "\norder=%d\nuser=%.9g %.9g\ngood_curve=%d\n", order_, user_lux_, user_logical_,
                has_good_ ? 1 : 0);
  text += item;
  const auto lines = [&](const char* key, const std::vector<Anchor>& anchors) {
    for (const Anchor& anchor : anchors) {
      std::snprintf(item, sizeof item, "%s=%.9g %.9g %d %d\n", key, anchor.lux, anchor.nit, anchor.order,
                    anchor.user ? 1 : 0);
      text += item;
    }
  };
  lines("anchor", anchors_);
  if (has_good_) lines("good", good_);
  return text;
}

bool NitController::valid(const std::vector<Anchor>& anchors) const {
  if (anchors.size() < 4 || anchors.size() > kMaxAnchors) return false;
  for (size_t i = 0; i < anchors.size(); ++i) {
    const Anchor& anchor = anchors[i];
    if (!(std::isfinite(anchor.lux) && anchor.lux >= 0.0f && anchor.lux <= config_.lux_max())) return false;
    if (!(std::isfinite(anchor.nit) && anchor.nit >= config_.min_nit && anchor.nit <= config_.max_nit)) return false;
    if (anchor.order < 0 || (i > 0 && !(anchor.lux > anchors[i - 1].lux))) return false;
  }
  for (const float at : {0.0f, config_.seg0(), config_.seg1(), config_.lux_max()}) {
    if (std::none_of(anchors.begin(), anchors.end(), [&](const Anchor& a) { return std::fabs(a.lux - at) < kSame; })) {
      return false;
    }
  }
  return true;
}

bool NitController::parse(const std::string& text) {
  reset();
  std::istringstream input(text);
  std::string line;
  bool version = false, defaults = false, good_curve = false;
  int order = 0;
  float user_lux = -1.0f, user_logical = -1.0f;
  std::vector<Anchor> anchors, good;
  while (std::getline(input, line)) {
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = line.substr(0, equals);
    const char* value = line.c_str() + equals + 1;
    if (key == "version") {
      version = std::atoi(value) == 2;
    } else if (key == "defaults") {
      std::istringstream numbers(value);
      std::vector<float> curve;
      float number;
      while (numbers >> number) curve.push_back(number);
      defaults = curve.size() == config_.default_curve.size();
      for (size_t i = 0; defaults && i < curve.size(); ++i) {
        defaults = std::fabs(curve[i] - config_.default_curve[i]) < kSame;
      }
    } else if (key == "order") {
      order = std::atoi(value);
    } else if (key == "user") {
      if (std::sscanf(value, "%f %f", &user_lux, &user_logical) != 2) user_lux = user_logical = -1.0f;
    } else if (key == "good_curve") {
      good_curve = std::atoi(value) == 1;
    } else if (key == "anchor" || key == "good") {
      Anchor anchor;
      int user = 0;
      if (std::sscanf(value, "%f %f %d %d", &anchor.lux, &anchor.nit, &anchor.order, &user) != 4) return false;
      anchor.user = user == 1;
      (key == "anchor" ? anchors : good).push_back(anchor);
    }
  }
  if (!version || !defaults || order < 0 || !valid(anchors) || (good_curve && !valid(good))) return false;
  anchors_ = anchors;
  if (good_curve) good_ = good;
  has_good_ = good_curve;
  order_ = order;
  user_lux_ = user_lux;
  user_logical_ = user_logical;
  return true;
}

bool NitController::adopt(const std::string& dump) {
  const auto list = [&](const char* key, std::vector<Anchor>& out) {
    const size_t at = dump.find(key);
    if (at == std::string::npos) return false;
    const size_t end = dump.find(']', at);
    for (size_t p = dump.find('(', at); p < end; p = dump.find('(', p + 1)) {
      Anchor anchor;
      char kind[8] = {};
      if (std::sscanf(dump.c_str() + p, "(%f,%f,%7[a-z])", &anchor.lux, &anchor.nit, kind) != 3) return false;
      anchor.user = std::strcmp(kind, "user") == 0;
      out.push_back(anchor);
    }
    return true;
  };
  const auto value = [&](const char* key) {
    const size_t at = dump.find(key);
    return at == std::string::npos ? -1.0f : std::strtof(dump.c_str() + at + std::strlen(key), nullptr);
  };
  std::vector<Anchor> anchors, good;
  if (!list("anchors=[", anchors) || !valid(anchors)) return false;
  const bool has_good = dump.find("mIsHaveGoodCurve=true") != std::string::npos;
  if (has_good && (!list("goodAnchors=[", good) || !valid(good))) return false;
  int order = 0;
  for (Anchor& anchor : anchors) {
    if (anchor.user) anchor.order = ++order;
  }
  if (order == 0) return false;
  int good_order = 0;
  for (Anchor& anchor : good) {
    if (anchor.user) anchor.order = ++good_order;
  }
  anchors_ = anchors;
  if (has_good) good_ = good;
  has_good_ = has_good;
  order_ = order;
  user_lux_ = value("mUserLux=");
  user_logical_ = value("mUserLogicalNit=");
  return true;
}

}  // namespace iab
