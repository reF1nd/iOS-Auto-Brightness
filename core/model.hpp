// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace brightness {
struct Config {
    double alpha_fast=.45, alpha_mid=.20, alpha_slow=.06;
    double fast_threshold=1, slow_threshold=.05, spike_up=15, spike_down=.05;
    double hyst_low=.025, hyst_high=.03;
    double high_lux_threshold=15000, high_lux_boost_max=.35, high_lux_min_pct=.90;
    int high_lux_duration_ms=8000;
    bool learning_enabled=true, enabled=true;
    int user_hold_ms=5000, log_max_bytes=1048576, log_max_backups=2;
    static Config parse(const std::string& text);
    std::string serialize() const;
};
struct Point { double lux, value; int64_t time; };
struct Profile {
    std::string id;
    std::vector<double> nits, backlight;
    static Profile parse(const std::string& text);
    double toBrightness(double value) const;
    double toNits(double value) const;
};
double baseNits(double lux);
struct Learning {
    std::vector<Point> points;
    double legacy_offset=0;
    void add(double lux, double value, int64_t time);
    double target(double lux, const Profile& profile) const;
};
struct Frame {
    int64_t now=0;
    bool automatic=false, screen=false, blocked=false, near=false;
    double lux=-1, back=-1, temp=-1, current=0, minimum=0, maximum=1;
    int battery=-1;
    bool charging=false;
};
struct Output { bool apply=false; double value=0; std::string reason="waiting"; };
class Controller {
public:
    Config config;
    Profile profile;
    Learning learning;
    Output step(const Frame& frame);
    void hold(int64_t now);
    bool learn(double lux, double value, int64_t timestamp);
    void reset();
    double smoothLux() const { return smooth_; }
    const Frame& frame() const { return frame_; }
private:
    Frame frame_;
    double smooth_=-1, output_=-1, candidate_=-1;
    int64_t last_=0, hold_until_=0, high_until_=0, candidate_since_=0;
};
std::string jsonString(const std::string& s);
}
