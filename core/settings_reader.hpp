// SPDX-License-Identifier: GPL-3.0-only
// Reads Settings.System values from settings_system.xml, either Android binary
// XML (ABX, the default since Android 12) or the text XML the reference parsed.
#pragma once

#include <string>
#include <vector>

namespace iab {

struct SystemSettings {
  int brightness_mode = -1;  // screen_brightness_mode, -1 if unavailable
  int brightness = -1;       // screen_brightness 0..255, -1 if unavailable
};

bool parse_system_settings(const std::vector<unsigned char>& data, SystemSettings& out);
bool read_system_settings(const std::string& path, SystemSettings& out);

}  // namespace iab
