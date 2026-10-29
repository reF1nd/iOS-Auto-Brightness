// SPDX-License-Identifier: GPL-3.0-only
#include "settings_reader.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace iab {

namespace {

constexpr size_t kMaxFileSize = 8u << 20;

int to_int(const std::string& text) {
  if (text.empty()) return -1;
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || value < 0 || value > 1000000) return -1;
  return static_cast<int>(value);
}

void assign(SystemSettings& out, const std::string& name, const std::string& value) {
  if (name == "screen_brightness_mode") out.brightness_mode = to_int(value);
  else if (name == "screen_brightness") out.brightness = to_int(value);
}

// Token layout of android.util.BinaryXmlSerializer.
class AbxReader {
 public:
  explicit AbxReader(const std::vector<unsigned char>& data) : data_(data) {}

  bool parse(SystemSettings& out) {
    if (data_.size() < 4 || std::memcmp(data_.data(), "ABX\0", 4) != 0) return false;
    pos_ = 4;
    bool in_setting = false;
    std::string name, value;
    while (pos_ < data_.size()) {
      const unsigned char token = data_[pos_++];
      const int command = token & 0x0f, type = token & 0xf0;
      std::string text;
      switch (command) {
        case 0:  // START_DOCUMENT
        case 1:  // END_DOCUMENT
          break;
        case 2:  // START_TAG
          if (!interned(text)) return false;
          in_setting = text == "setting";
          name.clear();
          value.clear();
          break;
        case 3:  // END_TAG
          if (!interned(text)) return false;
          if (in_setting && text == "setting") assign(out, name, value);
          in_setting = false;
          break;
        case 15: {  // ATTRIBUTE
          std::string attribute;
          if (!interned(attribute) || !typed(type, text)) return false;
          if (in_setting && attribute == "name") name = text;
          else if (in_setting && attribute == "value") value = text;
          break;
        }
        default:
          if (command > 10 || !typed(type, text)) return false;
      }
    }
    return true;
  }

 private:
  bool bytes(size_t count, const unsigned char** at) {
    if (data_.size() - pos_ < count) return false;
    *at = data_.data() + pos_;
    pos_ += count;
    return true;
  }
  bool u16(unsigned* value) {
    const unsigned char* at;
    if (!bytes(2, &at)) return false;
    *value = static_cast<unsigned>(at[0]) << 8 | at[1];
    return true;
  }
  bool utf(std::string& text) {
    unsigned length;
    const unsigned char* at;
    if (!u16(&length) || !bytes(length, &at)) return false;
    text.assign(reinterpret_cast<const char*>(at), length);
    return true;
  }
  bool interned(std::string& text) {
    unsigned index;
    if (!u16(&index)) return false;
    if (index == 0xffff) {
      if (!utf(text)) return false;
      pool_.push_back(text);
      return true;
    }
    if (index >= pool_.size()) return false;
    text = pool_[index];
    return true;
  }
  bool number(size_t width, std::string& text) {
    const unsigned char* at;
    if (!bytes(width, &at)) return false;
    unsigned long long raw = 0;
    for (size_t i = 0; i < width; ++i) raw = raw << 8 | at[i];
    const long long value = width == 4 ? static_cast<int>(static_cast<unsigned>(raw))
                                       : static_cast<long long>(raw);
    text = std::to_string(value);
    return true;
  }
  bool typed(int type, std::string& text) {
    const unsigned char* at;
    unsigned length;
    switch (type >> 4) {
      case 1: text.clear(); return true;          // NULL
      case 2: return utf(text);                    // STRING
      case 3: return interned(text);               // STRING_INTERNED
      case 4:                                      // BYTES_HEX
      case 5:                                      // BYTES_BASE64
        text.clear();
        return u16(&length) && bytes(length, &at);
      case 6: case 7: return number(4, text);     // INT, INT_HEX
      case 8: case 9: return number(8, text);     // LONG, LONG_HEX
      case 10: text.clear(); return bytes(4, &at);  // FLOAT
      case 11: text.clear(); return bytes(8, &at);  // DOUBLE
      case 12: text = "true"; return true;
      case 13: text = "false"; return true;
      default: return false;
    }
  }

  const std::vector<unsigned char>& data_;
  size_t pos_ = 0;
  std::vector<std::string> pool_;
};

// The reference looked up name="key" and then the next value="...".
int text_value(const std::string& xml, const char* key) {
  const size_t name = xml.find(std::string("name=\"") + key + "\"");
  if (name == std::string::npos) return -1;
  const size_t value = xml.find("value=\"", name);
  if (value == std::string::npos) return -1;
  return std::atoi(xml.c_str() + value + 7);
}

}  // namespace

bool parse_system_settings(const std::vector<unsigned char>& data, SystemSettings& out) {
  out = SystemSettings{};
  if (data.size() >= 4 && std::memcmp(data.data(), "ABX\0", 4) == 0) {
    SystemSettings parsed;
    if (!AbxReader(data).parse(parsed)) return false;
    out = parsed;
    return true;
  }
  const std::string xml(data.begin(), data.end());
  out.brightness_mode = text_value(xml, "screen_brightness_mode");
  out.brightness = text_value(xml, "screen_brightness");
  return true;
}

bool read_system_settings(const std::string& path, SystemSettings& out) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return false;
  std::vector<unsigned char> data;
  unsigned char chunk[16384];
  size_t count;
  while ((count = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
    data.insert(data.end(), chunk, chunk + count);
    if (data.size() > kMaxFileSize) break;
  }
  std::fclose(file);
  if (data.size() > kMaxFileSize) return false;
  return parse_system_settings(data, out);
}

}  // namespace iab
