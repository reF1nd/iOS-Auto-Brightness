// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "model.hpp"
namespace brightness {
std::string readFile(const std::string& path, size_t limit=65536);
void atomicWrite(const std::string& path,const std::string& data);
struct Store {
    std::string directory, profile;
    int64_t user=0;
    std::string path() const;
    std::string encode(const Learning& data) const;
    Learning decode(const std::string& data) const;
    Learning load(std::string& status) const;
    void save(const Learning& data, bool clearBackup=false) const;
};
}
