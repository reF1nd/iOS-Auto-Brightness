// SPDX-License-Identifier: GPL-3.0-only
#include "brightness_map.hpp"

#include <cmath>

namespace iab {

namespace {

bool increasing(const std::vector<float>& values, size_t from, size_t to) {
  for (size_t i = from; i < to; ++i) {
    if (!std::isfinite(values[i]) || (i > from && !(values[i] > values[i - 1]))) return false;
  }
  return true;
}

bool finite(const std::vector<float>& values) {
  for (const float value : values) {
    if (!std::isfinite(value)) return false;
  }
  return true;
}

// RefactorBrightnessUtil.getFirstFactor / getSecondFactor.
float factor(const std::vector<float>& config, float lux) {
  int i = 0;
  while (i < 6 && lux > config[i]) ++i;
  if (i == 0) return config[6];
  if (i == 6) return config[11];
  return (lux - config[i - 1]) / (config[i] - config[i - 1]) * (config[6 + i] - config[5 + i]) + config[5 + i];
}

}  // namespace

PanelMap::PanelMap()
    : brightness_{0.000366256f, 0.002258576f, 0.006043218f, 0.010438286f, 0.017885484f,
                  0.03699182f, 0.49993896f, 0.8240142f, 1.0f},
      nits_{1.0f, 2.0f, 5.0f, 10.0f, 24.0f, 59.8f, 800.0f, 2000.0f, 3500.0f} {}

PanelMap::PanelMap(const std::vector<float>& brightness, const std::vector<float>& nits) {
  if (brightness.size() != nits.size() || brightness.size() < 2) return;
  for (size_t i = 0; i < brightness.size(); ++i) {
    if (!(brightness[i] >= 0.0f && brightness[i] <= 1.0f && nits[i] > 0.0f)) return;
    if (i > 0 && !(brightness[i] > brightness[i - 1] && nits[i] > nits[i - 1])) return;
  }
  brightness_ = brightness;
  nits_ = nits;
}

float PanelMap::nits(float brightness) const {
  // Below the first point the line runs to zero nits at zero brightness.
  if (brightness <= brightness_[0]) return brightness > 0.0f ? nits_[0] * brightness / brightness_[0] : 0.0f;
  size_t i = 1;
  while (i + 1 < brightness_.size() && brightness > brightness_[i]) ++i;
  return nits_[i - 1] + (brightness - brightness_[i - 1]) / (brightness_[i] - brightness_[i - 1]) * (nits_[i] - nits_[i - 1]);
}

float PanelMap::brightness(float nits) const {
  if (nits <= nits_[0]) return nits > 0.0f ? brightness_[0] * nits / nits_[0] : 0.0f;
  size_t i = 1;
  while (i + 1 < nits_.size() && nits > nits_[i]) ++i;
  const float value = brightness_[i - 1] + (nits - nits_[i - 1]) / (nits_[i] - nits_[i - 1]) * (brightness_[i] - brightness_[i - 1]);
  return value < 1.0f ? value : 1.0f;
}

bool RefactorConfig::valid() const {
  if (!(std::isfinite(min_nit) && std::isfinite(max_nit) && min_nit > 0.0f && max_nit > min_nit)) return false;
  if (!(first_node > min_nit && first_node < max_nit && second_node > min_nit && second_node < max_nit)) return false;
  for (const std::vector<float>* config : {&first_factor, &second_factor}) {
    if (config->size() != 12 || !increasing(*config, 0, 6) || !finite(*config)) return false;
  }
  if (default_curve.size() < 8 || !finite(default_curve)) return false;
  if (!(seg0() > 0.0f && seg1() > seg0() && lux_max() > seg1())) return false;
  if (high_lux.size() < 2 || high_lux.size() != high_scale.size() || !increasing(high_lux, 0, high_lux.size()) ||
      !finite(high_scale)) {
    return false;
  }
  return good_lux.size() >= 2 && good_lux.size() == good_nit.size() && increasing(good_lux, 0, good_lux.size()) &&
         finite(good_nit);
}

float linear_spline(const std::vector<float>& x, const std::vector<float>& y, float at) {
  if (at <= x.front()) return y.front();
  if (at >= x.back()) return y.back();
  size_t i = 1;
  while (at > x[i]) ++i;
  return y[i - 1] + (at - x[i - 1]) / (x[i] - x[i - 1]) * (y[i] - y[i - 1]);
}

float refactor_nits(const RefactorConfig& config, float logical, float lux) {
  const float capped = lux > config.high_lux[0] ? config.high_lux[0] : lux;
  const double first = factor(config.first_factor, capped), second = factor(config.second_factor, capped);
  double nits = logical;
  if (logical > config.first_node) {
    const double gain = (config.first_node - config.max_nit * second) / static_cast<double>(config.first_node - config.max_nit);
    nits = config.first_node - (config.first_node - logical) * gain;
  }
  if (nits <= config.second_node) {
    const double gain = (config.second_node - config.min_nit * first) / static_cast<double>(config.second_node - config.min_nit);
    nits = config.second_node - (config.second_node - nits) * gain;
  }
  float result = static_cast<float>(nits);
  if (lux > config.high_lux[0]) result *= linear_spline(config.high_lux, config.high_scale, lux);
  return result;
}

MonotoneSpline::MonotoneSpline(const std::vector<float>& x, const std::vector<float>& y) {
  const size_t n = x.size();
  if (n < 2 || y.size() != n || !increasing(x, 0, n) || !finite(y)) return;
  std::vector<float> d(n - 1), m(n);
  for (size_t i = 0; i + 1 < n; ++i) {
    d[i] = (y[i + 1] - y[i]) / (x[i + 1] - x[i]);
    if (d[i] < 0.0f) return;
  }
  m[0] = d[0];
  for (size_t i = 1; i + 1 < n; ++i) m[i] = (d[i - 1] + d[i]) * 0.5f;
  m[n - 1] = d[n - 2];
  for (size_t i = 0; i + 1 < n; ++i) {
    if (d[i] == 0.0f) {
      m[i] = m[i + 1] = 0.0f;
    } else {
      const float a = m[i] / d[i], b = m[i + 1] / d[i];
      const float h = static_cast<float>(std::hypot(a, b));
      if (h > 3.0f) {
        m[i] *= 3.0f / h;
        m[i + 1] *= 3.0f / h;
      }
    }
  }
  x_ = x;
  y_ = y;
  m_ = m;
}

float MonotoneSpline::interpolate(float at) const {
  const size_t n = x_.size();
  if (std::isnan(at)) return at;
  if (at <= x_[0]) return y_[0];
  if (at >= x_[n - 1]) return y_[n - 1];
  size_t i = 0;
  while (at >= x_[i + 1]) {
    ++i;
    if (at == x_[i]) return y_[i];
  }
  const float h = x_[i + 1] - x_[i], t = (at - x_[i]) / h;
  return (y_[i] * (1 + 2 * t) + h * m_[i] * t) * (1 - t) * (1 - t) + (y_[i + 1] * (3 - 2 * t) + h * m_[i + 1] * (t - 1)) * t * t;
}

}  // namespace iab
