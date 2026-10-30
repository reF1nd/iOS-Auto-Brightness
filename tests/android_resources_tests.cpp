// SPDX-License-Identifier: GPL-3.0-only
// resources.arsc reader against tables built here in the binary layout of
// frameworks/base/libs/androidfw (ResourceTypes.h) and zipped as APKs.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "android_resources.hpp"
#include "check.hpp"

using namespace iab;

namespace {

struct Bytes {
  std::string data;
  Bytes& u8(uint32_t v) { data += static_cast<char>(v & 0xff); return *this; }
  Bytes& u16(uint32_t v) { return u8(v).u8(v >> 8); }
  Bytes& u32(uint32_t v) { return u16(v & 0xffff).u16(v >> 16); }
  Bytes& raw(const std::string& s) { data += s; return *this; }
  void set32(size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) data[at + i] = static_cast<char>(v >> (8 * i) & 0xff);
  }
  void pad() { while (data.size() % 4) u8(0); }
};

uint32_t bits(float value) {
  uint32_t out;
  std::memcpy(&out, &value, sizeof out);
  return out;
}

std::string pool(const std::vector<std::string>& strings, bool utf8) {
  Bytes chunk;
  chunk.u16(0x0001).u16(28).u32(0).u32(static_cast<uint32_t>(strings.size())).u32(0).u32(utf8 ? 0x100 : 0);
  chunk.u32(static_cast<uint32_t>(28 + strings.size() * 4)).u32(0);
  Bytes text;
  for (const std::string& s : strings) {
    chunk.u32(static_cast<uint32_t>(text.data.size()));
    if (utf8) {
      text.u8(static_cast<uint32_t>(s.size())).u8(static_cast<uint32_t>(s.size())).raw(s).u8(0);
    } else {
      text.u16(static_cast<uint32_t>(s.size()));
      for (const char c : s) text.u16(static_cast<unsigned char>(c));
      text.u16(0);
    }
  }
  chunk.raw(text.data);
  chunk.pad();
  chunk.set32(4, static_cast<uint32_t>(chunk.data.size()));
  return chunk.data;
}

struct Entry {
  uint32_t key;
  enum { Simple, Complex, Compact, Missing } kind;
  uint8_t type;
  std::vector<uint32_t> values;
};

// One ResTable_type chunk. offsets: 0 plain uint32, 1 offset16, 2 sparse.
std::string type_chunk(uint8_t id, const std::vector<Entry>& entries, int offsets, bool qualified) {
  constexpr uint32_t kConfig = 64;
  const uint32_t header = 20 + kConfig;
  Bytes body;
  std::vector<int64_t> at;
  for (const Entry& e : entries) {
    if (e.kind == Entry::Missing) {
      at.push_back(-1);
      continue;
    }
    at.push_back(static_cast<int64_t>(body.data.size()));
    if (e.kind == Entry::Compact) {
      body.u16(e.key).u16(0x0008 | static_cast<uint32_t>(e.type) << 8).u32(e.values[0]);
    } else if (e.kind == Entry::Simple) {
      body.u16(8).u16(0).u32(e.key).u16(8).u8(0).u8(e.type).u32(e.values[0]);
    } else {
      body.u16(16).u16(0x0001).u32(e.key).u32(0).u32(static_cast<uint32_t>(e.values.size()));
      for (size_t i = 0; i < e.values.size(); ++i) {
        body.u32(0x02000000u + static_cast<uint32_t>(i)).u16(8).u8(0).u8(e.type).u32(e.values[i]);
      }
    }
  }
  Bytes table;
  uint32_t count = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (offsets == 2) {
      if (at[i] < 0) continue;
      table.u16(static_cast<uint32_t>(i)).u16(static_cast<uint32_t>(at[i] / 4));
      ++count;
    } else if (offsets == 1) {
      table.u16(at[i] < 0 ? 0xffff : static_cast<uint32_t>(at[i] / 4));
      ++count;
    } else {
      table.u32(at[i] < 0 ? 0xffffffffu : static_cast<uint32_t>(at[i]));
      ++count;
    }
  }
  table.pad();
  Bytes chunk;
  const uint8_t flags = offsets == 2 ? 0x01 : offsets == 1 ? 0x02 : 0;
  chunk.u16(0x0201).u16(header).u32(0).u8(id).u8(flags).u16(0).u32(count);
  chunk.u32(static_cast<uint32_t>(header + table.data.size()));
  Bytes config;
  config.u32(kConfig);
  while (config.data.size() < kConfig) config.u8(0);
  if (qualified) config.data[14] = 1;  // a density
  chunk.raw(config.data).raw(table.data).raw(body.data);
  chunk.set32(4, static_cast<uint32_t>(chunk.data.size()));
  return chunk.data;
}

std::string table(const std::vector<std::string>& chunks, bool utf8) {
  const std::string types = pool({"array", "bool", "dimen"}, utf8);
  const std::string keys = pool({"config_curve", "config_flag", "config_nit", "config_other"}, utf8);
  Bytes package;
  package.u16(0x0200).u16(288).u32(0).u32(0x7f);
  for (int i = 0; i < 128; ++i) package.u16(0);
  package.u32(288).u32(3).u32(static_cast<uint32_t>(288 + types.size())).u32(4).u32(0);
  package.raw(types).raw(keys);
  for (const std::string& chunk : chunks) package.raw(chunk);
  package.set32(4, static_cast<uint32_t>(package.data.size()));
  Bytes out;
  out.u16(0x0002).u16(12).u32(0).u32(1).raw(pool({}, true)).raw(package.data);
  out.set32(4, static_cast<uint32_t>(out.data.size()));
  return out.data;
}

// A zip with a manifest before resources.arsc, like an APK.
std::string zip(const std::string& arsc, uint16_t method = 0) {
  Bytes out, directory;
  const std::vector<std::pair<std::string, std::string>> files = {{"AndroidManifest.xml", "manifest"},
                                                                  {"resources.arsc", arsc}};
  for (const auto& [name, data] : files) {
    const uint32_t offset = static_cast<uint32_t>(out.data.size());
    const uint16_t m = name == "resources.arsc" ? method : 0;
    out.u32(0x04034b50).u16(20).u16(0).u16(m).u16(0).u16(0).u32(0).u32(static_cast<uint32_t>(data.size()));
    out.u32(static_cast<uint32_t>(data.size())).u16(static_cast<uint32_t>(name.size())).u16(4).raw(name).u32(0);
    out.raw(data);
    directory.u32(0x02014b50).u16(20).u16(20).u16(0).u16(m).u16(0).u16(0).u32(0);
    directory.u32(static_cast<uint32_t>(data.size())).u32(static_cast<uint32_t>(data.size()));
    directory.u16(static_cast<uint32_t>(name.size())).u16(0).u16(0).u16(0).u16(0).u32(0).u32(offset).raw(name);
  }
  const uint32_t start = static_cast<uint32_t>(out.data.size());
  out.raw(directory.data);
  out.u32(0x06054b50).u16(0).u16(0).u16(2).u16(2).u32(static_cast<uint32_t>(directory.data.size())).u32(start).u16(0);
  return out.data;
}

std::string save(const std::string& data) {
  char path[] = "/tmp/iab_resources_XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) return "";
  if (write(fd, data.data(), data.size()) != static_cast<ssize_t>(data.size())) {}
  close(fd);
  return path;
}

const std::vector<std::string> kWanted = {"array/config_curve", "bool/config_flag", "dimen/config_nit"};

std::string base_table(bool utf8) {
  return table({type_chunk(3, {{2, Entry::Simple, 0x04, {bits(2.0f)}}}, 0, true),
                type_chunk(1, {{0, Entry::Complex, 0x04, {bits(0), bits(2), bits(30), bits(135.5f)}}}, 0, false),
                type_chunk(2, {{3, Entry::Missing, 0, {}}, {1, Entry::Simple, 0x12, {0xffffffffu}}}, 1, false),
                type_chunk(3, {{3, Entry::Simple, 0x10, {7}}, {2, Entry::Simple, 0x04, {bits(1060.0f)}}}, 2, false)},
               utf8);
}

void reads_values_of_the_default_configuration() {
  for (const bool utf8 : {true, false}) {
    ResourceValues values;
    std::string error;
    CHECK(read_table_resources(base_table(utf8), kWanted, values, error));
    CHECK(error.empty());
    CHECK_EQ(values.size(), 3u);
    CHECK_EQ(values["array/config_curve"].size(), 4u);
    CHECK_NEAR(values["array/config_curve"][3], 135.5, 1e-6);
    CHECK_NEAR(values["bool/config_flag"][0], 1.0, 1e-9);
    // The density-qualified 2.0 comes first but the default 1060 wins.
    CHECK_NEAR(values["dimen/config_nit"][0], 1060.0, 1e-6);
    CHECK(values.count("dimen/config_other") == 0);
  }
}

void overlays_replace_values_in_order() {
  const std::string base = save(zip(base_table(true)));
  const std::string overlay =
      save(zip(table({type_chunk(2, {{1, Entry::Compact, 0x12, {0}}}, 0, false),
                      type_chunk(3, {{2, Entry::Simple, 0x11, {500}}}, 0, false)},
                     true)));
  ResourceValues values;
  std::string error;
  CHECK(read_apk_resources(base, kWanted, values, error));
  CHECK(error.empty());
  CHECK(read_apk_resources(overlay, kWanted, values, error));
  CHECK_NEAR(values["bool/config_flag"][0], 0.0, 1e-9);
  CHECK_NEAR(values["dimen/config_nit"][0], 500.0, 1e-9);
  CHECK_EQ(values["array/config_curve"].size(), 4u);
  unlink(base.c_str());
  unlink(overlay.c_str());
}

void rejects_what_it_cannot_read() {
  ResourceValues values;
  std::string error;
  CHECK(!read_apk_resources("/nonexistent/framework-ext-res.apk", kWanted, values, error));
  const std::string compressed = save(zip(base_table(true), 8));
  CHECK(!read_apk_resources(compressed, kWanted, values, error));
  CHECK(error == "resources.arsc is compressed");
  unlink(compressed.c_str());
  const std::string text = save("not a zip at all");
  CHECK(!read_apk_resources(text, kWanted, values, error));
  unlink(text.c_str());
  CHECK(!read_table_resources("short", kWanted, values, error));
  // Truncated and corrupted tables fail or skip entries, never read past the data.
  const std::string full = base_table(true);
  for (size_t cut = 8; cut < full.size(); cut += 7) {
    std::string broken = full.substr(0, cut);
    for (int i = 0; i < 4; ++i) broken[4 + i] = static_cast<char>(broken.size() >> (8 * i) & 0xff);
    ResourceValues partial;
    read_table_resources(broken, kWanted, partial, error);
    std::string flipped = full;
    flipped[cut] = static_cast<char>(flipped[cut] ^ 0x5a);
    read_table_resources(flipped, kWanted, partial, error);
  }
  CHECK(values.empty());
}

}  // namespace

int main() {
  reads_values_of_the_default_configuration();
  overlays_replace_values_in_order();
  rejects_what_it_cannot_read();
  return finish("android_resources_tests");
}
