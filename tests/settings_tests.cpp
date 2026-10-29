// SPDX-License-Identifier: GPL-3.0-only
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "check.hpp"
#include "config.hpp"
#include "settings_reader.hpp"

using namespace iab;

namespace {

// Minimal android.util.BinaryXmlSerializer encoder for fixtures.
class Abx {
 public:
  Abx() { bytes_ = {'A', 'B', 'X', 0, 0x10}; }
  Abx& start(const std::string& tag) {
    bytes_.push_back(0x32);
    interned(tag);
    return *this;
  }
  Abx& end(const std::string& tag) {
    bytes_.push_back(0x33);
    interned(tag);
    return *this;
  }
  Abx& text(const std::string& name, const std::string& value) {
    bytes_.push_back(0x2f);
    interned(name);
    utf(value);
    return *this;
  }
  Abx& pooled(const std::string& name, const std::string& value) {
    bytes_.push_back(0x3f);
    interned(name);
    interned(value);
    return *this;
  }
  Abx& integer(const std::string& name, int32_t value) {
    bytes_.push_back(0x6f);
    interned(name);
    for (int shift = 24; shift >= 0; shift -= 8) bytes_.push_back(static_cast<unsigned char>(value >> shift));
    return *this;
  }
  Abx& flag(const std::string& name) {
    bytes_.push_back(0xcf);
    interned(name);
    return *this;
  }
  Abx& null(const std::string& name) {
    bytes_.push_back(0x1f);
    interned(name);
    return *this;
  }
  std::vector<unsigned char> finish() {
    bytes_.push_back(0x11);
    return bytes_;
  }

 private:
  void u16(unsigned value) {
    bytes_.push_back(static_cast<unsigned char>(value >> 8));
    bytes_.push_back(static_cast<unsigned char>(value));
  }
  void utf(const std::string& value) {
    u16(static_cast<unsigned>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }
  void interned(const std::string& value) {
    const auto found = pool_.find(value);
    if (found != pool_.end()) return u16(found->second);
    u16(0xffff);
    utf(value);
    const unsigned index = static_cast<unsigned>(pool_.size());
    pool_[value] = index;
  }
  std::vector<unsigned char> bytes_;
  std::map<std::string, unsigned> pool_;
};

std::vector<unsigned char> setting(Abx& abx, int id, const std::string& name, const std::string& value) {
  abx.start("setting").integer("id", id).text("name", name).text("value", value).pooled("package", "android");
  abx.text("defaultValue", value).flag("defaultSysSet").end("setting");
  return {};
}

void binary_settings() {
  Abx abx;
  abx.start("settings").integer("version", 236);
  setting(abx, 1, "wifi_assistant", "1");
  setting(abx, 2, "sub_display_screen_brightness", "11");
  setting(abx, 3, "screen_brightness_mode", "1");
  abx.start("setting").integer("id", 4).text("name", "font_scale").null("value").end("setting");
  setting(abx, 5, "screen_brightness", "35");
  setting(abx, 6, "screen_brightness_mode_backup", "0");
  abx.end("settings");
  SystemSettings out;
  CHECK(parse_system_settings(abx.finish(), out));
  CHECK_EQ(out.brightness_mode, 1);
  CHECK_EQ(out.brightness, 35);
}

void truncated_binary_is_rejected() {
  Abx abx;
  abx.start("settings");
  setting(abx, 1, "screen_brightness", "35");
  std::vector<unsigned char> data = abx.finish();
  data.resize(data.size() - 9);
  SystemSettings out;
  CHECK(!parse_system_settings(data, out));
  CHECK_EQ(out.brightness, -1);
}

void text_settings() {
  const std::string xml =
      "<?xml version='1.0'?>\n<settings version=\"236\">\n"
      "<setting id=\"7\" name=\"screen_brightness\" value=\"140\" package=\"android\" />\n"
      "<setting id=\"8\" name=\"screen_brightness_mode\" value=\"0\" package=\"android\" />\n</settings>\n";
  SystemSettings out;
  CHECK(parse_system_settings(std::vector<unsigned char>(xml.begin(), xml.end()), out));
  CHECK_EQ(out.brightness, 140);
  CHECK_EQ(out.brightness_mode, 0);
}

void config_overrides_only_present_keys() {
  const char* path = "settings_tests.conf";
  FILE* file = std::fopen(path, "w");
  std::fputs("# comment\nalpha_fast=0.5\n hyst_low = 0.01\nunknown=3\nhigh_lux_duration_ms=4000.7\n"
             "dual_sensor=0\nlearning_enabled=0\nspike_up=nan\n",
             file);
  std::fclose(file);
  Config config;
  config.alpha_slow = 0.1f;
  CHECK(load_config(path, config));
  CHECK_NEAR(config.alpha_fast, 0.5, 1e-6);
  CHECK_NEAR(config.hyst_low, 0.01, 1e-6);
  CHECK_NEAR(config.alpha_slow, 0.1, 1e-6);
  CHECK_NEAR(config.spike_up, 15.0, 1e-6);
  CHECK_EQ(config.high_lux_duration_ms, 4000);
  CHECK(!config.dual_sensor);
  CHECK(!config.learning_enabled);
  std::remove(path);
  CHECK(!load_config("missing.conf", config));
}

}  // namespace

int main() {
  binary_settings();
  truncated_binary_is_rejected();
  text_settings();
  config_overrides_only_present_keys();
  return finish("settings_tests");
}
