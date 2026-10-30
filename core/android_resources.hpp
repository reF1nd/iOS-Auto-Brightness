// SPDX-License-Identifier: GPL-3.0-only
// Reads typed values from the resources.arsc of an APK, so the daemon follows
// the framework-ext-res values (and their overlays) of the ROM it runs on.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace iab {

// "type/name" -> values: floats and integers as they are, booleans as 0 or 1,
// arrays item by item. Only the default configuration is read.
using ResourceValues = std::map<std::string, std::vector<float>>;

// The table of an APK whose resources.arsc is stored uncompressed, as Android
// requires for framework and overlay packages. Wanted names found in the APK
// replace earlier values in `out`, so overlays can be applied in order.
bool read_apk_resources(const std::string& apk, const std::vector<std::string>& wanted, ResourceValues& out,
                        std::string& error);
// The same for a resources.arsc already in memory.
bool read_table_resources(const std::string& table, const std::vector<std::string>& wanted, ResourceValues& out,
                          std::string& error);

}  // namespace iab
