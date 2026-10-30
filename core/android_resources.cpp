// SPDX-License-Identifier: GPL-3.0-only
#include "android_resources.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace iab {

namespace {

// frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h
constexpr uint16_t kStringPool = 0x0001, kTable = 0x0002, kPackage = 0x0200, kType = 0x0201;
constexpr uint8_t kTypeSparse = 0x01, kTypeOffset16 = 0x02;
constexpr uint16_t kEntryComplex = 0x0001, kEntryCompact = 0x0008;
constexpr uint8_t kFloat = 0x04, kIntDec = 0x10, kIntHex = 0x11, kBoolean = 0x12;

class Reader {
 public:
  explicit Reader(const std::string& data) : data_(data) {}
  bool has(size_t at, size_t count) const { return at <= data_.size() && count <= data_.size() - at; }
  uint8_t u8(size_t at) const { return has(at, 1) ? static_cast<uint8_t>(data_[at]) : 0; }
  uint16_t u16(size_t at) const { return static_cast<uint16_t>(u8(at) | u8(at + 1) << 8); }
  uint32_t u32(size_t at) const { return static_cast<uint32_t>(u16(at)) | static_cast<uint32_t>(u16(at + 2)) << 16; }

 private:
  const std::string& data_;
};

// A string pool's entries as plain bytes (names are ASCII).
bool read_pool(const Reader& in, size_t at, std::vector<std::string>& out) {
  if (in.u16(at) != kStringPool || !in.has(at, in.u32(at + 4))) return false;
  const size_t end = at + in.u32(at + 4);
  const uint32_t count = in.u32(at + 8), flags = in.u32(at + 16), strings = in.u32(at + 20);
  const size_t offsets = at + in.u16(at + 2);
  if (!in.has(offsets, static_cast<size_t>(count) * 4)) return false;
  const bool utf8 = (flags & 0x100) != 0;
  out.clear();
  for (uint32_t i = 0; i < count; ++i) {
    size_t p = at + strings + in.u32(offsets + i * 4);
    std::string text;
    if (utf8) {
      p += (in.u8(p) & 0x80) ? 2 : 1;  // UTF-16 length
      size_t length = in.u8(p);
      if (length & 0x80) length = (length & 0x7f) << 8 | in.u8(p + 1), ++p;
      ++p;
      if (!in.has(p, length) || p + length > end) return false;
      for (size_t j = 0; j < length; ++j) text += static_cast<char>(in.u8(p + j));
    } else {
      size_t length = in.u16(p);
      if (length & 0x8000) length = (length & 0x7fff) << 16 | in.u16(p + 2), p += 2;
      p += 2;
      if (!in.has(p, length * 2) || p + length * 2 > end) return false;
      for (size_t j = 0; j < length; ++j) text += static_cast<char>(in.u16(p + j * 2) & 0xff);
    }
    out.push_back(text);
  }
  return true;
}

bool value_of(uint8_t type, uint32_t data, float& out) {
  switch (type) {
    case kFloat: std::memcpy(&out, &data, sizeof out); return true;
    case kIntDec:
    case kIntHex: out = static_cast<float>(static_cast<int32_t>(data)); return true;
    case kBoolean: out = data != 0 ? 1.0f : 0.0f; return true;
    default: return false;
  }
}

// One configuration of one type: the wanted entries it holds.
void read_type(const Reader& in, size_t at, const std::string& type, const std::vector<std::string>& keys,
               const std::vector<std::string>& wanted, ResourceValues& found, std::vector<std::string>& defaults) {
  const size_t header = in.u16(at + 2), size = in.u32(at + 4);
  const uint8_t flags = in.u8(at + 9);
  const uint32_t count = in.u32(at + 12), entries = in.u32(at + 16);
  // ResTable_config at +20 begins with its size; the default one is all zero.
  const size_t config_size = in.u32(at + 20);
  bool fallback = false;
  for (size_t i = 4; i < config_size && 20 + i < header; ++i) fallback = fallback || in.u8(at + 20 + i) != 0;
  const auto entry_at = [&](uint32_t i, uint32_t& index) -> int64_t {
    if (flags & kTypeSparse) {
      index = in.u16(at + header + i * 4);
      return static_cast<int64_t>(in.u16(at + header + i * 4 + 2)) * 4;
    }
    index = i;
    if (flags & kTypeOffset16) {
      const uint16_t offset = in.u16(at + header + i * 2);
      return offset == 0xffff ? -1 : static_cast<int64_t>(offset) * 4;
    }
    const uint32_t offset = in.u32(at + header + i * 4);
    return offset == 0xffffffffu ? -1 : static_cast<int64_t>(offset);
  };
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t index = 0;
    const int64_t offset = entry_at(i, index);
    if (offset < 0) continue;
    const size_t entry = at + entries + static_cast<size_t>(offset);
    if (entry + 8 > at + size || !in.has(entry, 8)) continue;
    const uint16_t entry_flags = in.u16(entry + 2);
    const uint32_t key = entry_flags & kEntryCompact ? in.u16(entry) : in.u32(entry + 4);
    if (key >= keys.size()) continue;
    const std::string name = type + "/" + keys[key];
    if (std::find(wanted.begin(), wanted.end(), name) == wanted.end()) continue;
    // A qualified configuration only counts when no default one exists.
    const bool is_default = std::find(defaults.begin(), defaults.end(), name) != defaults.end();
    if (is_default || (fallback && found.count(name))) continue;
    std::vector<float> values;
    bool ok = true;
    if (entry_flags & kEntryCompact) {
      float value = 0;
      ok = value_of(static_cast<uint8_t>(entry_flags >> 8), in.u32(entry + 4), value);
      values.push_back(value);
    } else if (entry_flags & kEntryComplex) {
      const size_t entry_size = in.u16(entry);
      const uint32_t items = in.u32(entry + 12);
      std::vector<std::pair<uint32_t, float>> ordered;
      for (uint32_t j = 0; ok && j < items; ++j) {
        const size_t map = entry + entry_size + static_cast<size_t>(j) * 12;
        if (map + 12 > at + size || !in.has(map, 12)) {
          ok = false;
          break;
        }
        float value = 0;
        ok = value_of(in.u8(map + 7), in.u32(map + 8), value);
        ordered.emplace_back(in.u32(map), value);
      }
      std::stable_sort(ordered.begin(), ordered.end());
      for (const auto& item : ordered) values.push_back(item.second);
    } else {
      const size_t value = entry + in.u16(entry);
      float number = 0;
      ok = in.has(value, 8) && value_of(in.u8(value + 3), in.u32(value + 4), number);
      values.push_back(number);
    }
    if (!ok) continue;
    found[name] = values;
    if (!fallback) defaults.push_back(name);
  }
}

bool read_package(const Reader& in, size_t at, const std::vector<std::string>& wanted, ResourceValues& found,
                  std::string& error) {
  const size_t header = in.u16(at + 2), end = at + in.u32(at + 4);
  std::vector<std::string> types, keys, defaults;
  if (!read_pool(in, at + in.u32(at + 268), types) || !read_pool(in, at + in.u32(at + 276), keys)) {
    error = "bad package string pool";
    return false;
  }
  for (size_t chunk = at + header; chunk + 8 <= end;) {
    const size_t size = in.u32(chunk + 4);
    if (size < 8 || !in.has(chunk, size) || chunk + size > end) {
      error = "bad package chunk";
      return false;
    }
    const uint8_t id = in.u8(chunk + 8);
    if (in.u16(chunk) == kType && id >= 1 && id <= types.size()) {
      read_type(in, chunk, types[id - 1], keys, wanted, found, defaults);
    }
    chunk += size;
  }
  return true;
}

}  // namespace

bool read_table_resources(const std::string& table, const std::vector<std::string>& wanted, ResourceValues& out,
                          std::string& error) {
  const Reader in(table);
  if (in.u16(0) != kTable || in.u32(4) != table.size()) {
    error = "not a resource table";
    return false;
  }
  ResourceValues found;
  for (size_t chunk = in.u16(2); chunk + 8 <= table.size();) {
    const size_t size = in.u32(chunk + 4);
    if (size < 8 || !in.has(chunk, size)) {
      error = "bad table chunk";
      return false;
    }
    if (in.u16(chunk) == kPackage && !read_package(in, chunk, wanted, found, error)) return false;
    chunk += size;
  }
  for (const auto& item : found) out[item.first] = item.second;
  return true;
}

bool read_apk_resources(const std::string& apk, const std::vector<std::string>& wanted, ResourceValues& out,
                        std::string& error) {
  const int fd = open(apk.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    error = "cannot open";
    return false;
  }
  const auto read_at = [&](off_t at, size_t count, std::string& into) {
    into.assign(count, '\0');
    size_t done = 0;
    while (done < count) {
      const ssize_t n = pread(fd, &into[done], count - done, at + static_cast<off_t>(done));
      if (n <= 0) return false;
      done += static_cast<size_t>(n);
    }
    return true;
  };
  bool ok = false;
  std::string tail, directory, local, table;
  const off_t length = lseek(fd, 0, SEEK_END);
  const size_t span = static_cast<size_t>(std::min<off_t>(length, 65557));
  if (length >= 22 && read_at(length - static_cast<off_t>(span), span, tail)) {
    const Reader end(tail);
    size_t record = std::string::npos;
    for (size_t i = span - 22 + 1; i-- > 0;) {
      if (end.u32(i) == 0x06054b50) {
        record = i;
        break;
      }
    }
    if (record == std::string::npos) {
      error = "no zip directory";
    } else if (read_at(end.u32(record + 16), end.u32(record + 12), directory)) {
      const Reader dir(directory);
      const uint16_t entries = end.u16(record + 10);
      error = "no resources.arsc";
      for (size_t p = 0, i = 0; i < entries && dir.u32(p) == 0x02014b50; ++i) {
        const uint16_t method = dir.u16(p + 10), name = dir.u16(p + 28), extra = dir.u16(p + 30), note = dir.u16(p + 32);
        const uint32_t stored = dir.u32(p + 20), header = dir.u32(p + 42);
        if (dir.has(p + 46, name) && directory.compare(p + 46, name, "resources.arsc") == 0) {
          if (method != 0) {
            error = "resources.arsc is compressed";
          } else if (read_at(header, 30, local) && Reader(local).u32(0) == 0x04034b50 &&
                     read_at(static_cast<off_t>(header) + 30 + Reader(local).u16(26) + Reader(local).u16(28), stored,
                             table)) {
            ok = read_table_resources(table, wanted, out, error);
            if (ok) error.clear();
          } else {
            error = "cannot read resources.arsc";
          }
          break;
        }
        p += 46 + name + extra + note;
      }
    } else {
      error = "cannot read zip directory";
    }
  }
  close(fd);
  return ok;
}

}  // namespace iab
