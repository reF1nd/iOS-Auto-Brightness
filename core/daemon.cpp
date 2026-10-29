// SPDX-License-Identifier: GPL-3.0-only
// Root daemon: Android access for the reference control loop in engine.cpp.
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "config.hpp"
#include "engine.hpp"
#include "settings_reader.hpp"

#ifndef IAB_VERSION
#define IAB_VERSION "dev"
#endif
// glibc and bionic accept "e" (O_CLOEXEC); keep host builds on macOS working.
#ifdef __APPLE__
#define IAB_CLOEXEC ""
#else
#define IAB_CLOEXEC "e"
#endif

namespace {

using iab::Config;
using iab::Engine;
using iab::SensorEvent;

volatile sig_atomic_t g_run = 1;
volatile sig_atomic_t g_reload = 0;
volatile sig_atomic_t g_reset_learning = 0;
volatile sig_atomic_t g_dump_status = 0;

void on_signal(int signal) {
  switch (signal) {
    case SIGHUP: g_reload = 1; break;
    case SIGUSR1: g_reset_learning = 1; break;
    case SIGUSR2: g_dump_status = 1; break;
    default: g_run = 0;
  }
}

int64_t clock_ms(clockid_t clock) {
  timespec now{};
  clock_gettime(clock, &now);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
int64_t monotonic_ms() { return clock_ms(CLOCK_MONOTONIC); }
#ifdef CLOCK_BOOTTIME
int64_t boottime_ms() { return clock_ms(CLOCK_BOOTTIME); }
#else
int64_t boottime_ms() { return monotonic_ms(); }
#endif

int local_hour_now() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  return local.tm_hour;
}

std::string sysroot() {
  const char* root = std::getenv("IAB_SYSROOT");
  return root ? root : "";
}
std::string device_path(const std::string& path) { return sysroot() + path; }

bool read_text(const std::string& path, std::string& out, size_t limit = 64) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  std::vector<char> buffer(limit);
  const ssize_t count = read(fd, buffer.data(), limit - 1);
  close(fd);
  if (count < 1) return false;
  out.assign(buffer.data(), static_cast<size_t>(count));
  return true;
}

int read_int(const std::string& path, int fallback) {
  std::string text;
  if (!read_text(path, text)) return fallback;
  return std::atoi(text.c_str());
}

std::string run_command(const std::string& command, size_t limit = 1 << 20) {
  std::string out;
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) return out;
  char buffer[4096];
  size_t count;
  while ((count = std::fread(buffer, 1, sizeof buffer, pipe)) > 0 && out.size() < limit) {
    out.append(buffer, count);
  }
  pclose(pipe);
  return out;
}

std::string json_escape(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
    else out += c;
  }
  return out;
}

float sanitized_offset(float offset) {
  return std::isfinite(offset) && std::fabs(offset) <= 0.3f ? offset : 0.0f;
}

// ASensorEvent layout of <android/sensor.h>.
struct NativeSensorEvent {
  int32_t version, sensor, type, reserved0;
  int64_t timestamp;
  float data[16];
  uint32_t flags;
  int32_t reserved1[3];
};
static_assert(sizeof(NativeSensorEvent) == 104, "ASensorEvent layout");

constexpr int kTypeLight = 5;
constexpr int kTypeProximity = 8;
constexpr int kTypeBackLight = 33171055;  // xiaomi.sensor.back_lux
constexpr int kSensorRateUs = 800000;

class Sensors {
 public:
  bool open(void (*log)(const char*)) {
    library_ = dlopen("libandroid.so", RTLD_NOW);
    if (!library_) {
      log("dlopen libandroid.so failed; using fallback light source");
      return false;
    }
    resolve(get_instance_, "ASensorManager_getInstance");
    resolve(get_instance_for_package_, "ASensorManager_getInstanceForPackage");
    resolve(get_default_, "ASensorManager_getDefaultSensor");
    resolve(get_list_, "ASensorManager_getSensorList");
    resolve(get_type_, "ASensor_getType");
    resolve(create_queue_, "ASensorManager_createEventQueue");
    resolve(destroy_queue_, "ASensorManager_destroyEventQueue");
    resolve(enable_, "ASensorEventQueue_enableSensor");
    resolve(disable_, "ASensorEventQueue_disableSensor");
    resolve(set_rate_, "ASensorEventQueue_setEventRate");
    resolve(get_events_, "ASensorEventQueue_getEvents");
    resolve(prepare_, "ALooper_prepare");
    resolve(poll_once_, "ALooper_pollOnce");
    if (!get_default_ || !create_queue_ || !enable_ || !get_events_ || !prepare_) {
      log("Missing NDK symbols; using fallback light source");
      return false;
    }
    if (get_instance_) manager_ = get_instance_();
    if (!manager_ && get_instance_for_package_) manager_ = get_instance_for_package_("com.android.system");
    void* looper = manager_ ? prepare_(1) : nullptr;
    queue_ = looper ? create_queue_(manager_, looper, 0, nullptr, nullptr) : nullptr;
    if (!queue_) {
      log("No sensor event queue; using fallback light source");
      return false;
    }
    front_ = get_default_(manager_, kTypeLight);
    if (front_ && enable_(queue_, front_) >= 0) {
      if (set_rate_) set_rate_(queue_, front_, kSensorRateUs);
      ndk_ = true;
      log("Front ALS ready (800000us)");
    }
    back_ = find(kTypeBackLight);
    if (back_ && enable_(queue_, back_) >= 0) {
      if (set_rate_) set_rate_(queue_, back_, kSensorRateUs);
      back_ok_ = true;
      log("Back ALS ready");
    }
    proximity_ = find(kTypeProximity);
    if (proximity_ && enable_(queue_, proximity_) >= 0) log("Proximity ready");
    return ndk_;
  }

  void set_light_enabled(bool enabled) {
    if (!queue_ || !disable_) return;
    if (enabled) {
      if (front_ && enable_(queue_, front_) >= 0 && set_rate_) set_rate_(queue_, front_, kSensorRateUs);
      if (back_ok_ && enable_(queue_, back_) >= 0 && set_rate_) set_rate_(queue_, back_, kSensorRateUs);
    } else {
      if (front_) disable_(queue_, front_);
      if (back_ok_) disable_(queue_, back_);
    }
  }

  void poll(std::vector<SensorEvent>& events) {
    if (!queue_) return;
    if (poll_once_) poll_once_(0, nullptr, nullptr, nullptr);
    const int64_t mono = monotonic_ms(), boot = boottime_ms();
    NativeSensorEvent event;
    while (get_events_(queue_, &event, 1) > 0) {
      SensorEvent out;
      if (event.type == kTypeLight) out.kind = SensorEvent::Kind::Front;
      else if (event.type == kTypeBackLight) out.kind = SensorEvent::Kind::Back;
      else if (event.type == kTypeProximity) out.kind = SensorEvent::Kind::Proximity;
      else continue;
      // Event timestamps use CLOCK_BOOTTIME; the loop runs on CLOCK_MONOTONIC.
      const int64_t age = boot - event.timestamp / 1000000;
      out.time = mono - std::clamp<int64_t>(age, 0, 60000);
      out.value = event.data[0];
      events.push_back(out);
    }
  }

  void close() {
    if (queue_ && disable_) {
      if (front_) disable_(queue_, front_);
      if (back_) disable_(queue_, back_);
      if (proximity_) disable_(queue_, proximity_);
    }
    if (queue_ && manager_ && destroy_queue_) destroy_queue_(manager_, queue_);
    queue_ = nullptr;
    if (library_) dlclose(library_);
    library_ = nullptr;
  }

  bool ndk() const { return ndk_; }
  bool back() const { return back_ok_; }

 private:
  template <typename F>
  void resolve(F& function, const char* name) {
    function = reinterpret_cast<F>(dlsym(library_, name));
  }
  const void* find(int type) {
    if (!get_list_ || !get_type_) return nullptr;
    const void* const* list = nullptr;
    const int count = get_list_(manager_, &list);
    for (int i = 0; i < count && list; ++i) {
      if (get_type_(list[i]) == type) return list[i];
    }
    return nullptr;
  }

  void* library_ = nullptr;
  void* (*get_instance_)() = nullptr;
  void* (*get_instance_for_package_)(const char*) = nullptr;
  const void* (*get_default_)(void*, int) = nullptr;
  int (*get_list_)(void*, const void* const**) = nullptr;
  int (*get_type_)(const void*) = nullptr;
  void* (*create_queue_)(void*, void*, int, void*, void*) = nullptr;
  int (*destroy_queue_)(void*, void*) = nullptr;
  int (*enable_)(void*, const void*) = nullptr;
  int (*disable_)(void*, const void*) = nullptr;
  int (*set_rate_)(void*, const void*, int32_t) = nullptr;
  ssize_t (*get_events_)(void*, NativeSensorEvent*, size_t) = nullptr;
  void* (*prepare_)(int) = nullptr;
  int (*poll_once_)(int, int*, int*, void**) = nullptr;
  void* manager_ = nullptr;
  void* queue_ = nullptr;
  const void *front_ = nullptr, *back_ = nullptr, *proximity_ = nullptr;
  bool ndk_ = false, back_ok_ = false;
};

std::string g_state_dir;

void log_line(const char* text) {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  std::printf("%02d-%02d %02d:%02d:%02d [iOS_Bright] %s\n", local.tm_mon + 1, local.tm_mday,
              local.tm_hour, local.tm_min, local.tm_sec, text);
  std::fflush(stdout);
}

class AndroidPlatform final : public iab::Platform {
 public:
  AndroidPlatform(const std::string& backlight, Sensors& sensors)
      : backlight_(backlight), sensors_(sensors) {}

  void discover_fallback() {
    const std::string als = trimmed(run_command("getprop persist.sys.display.als_name 2>/dev/null"));
    for (const auto& [directory, file] :
         {std::make_pair("/sys/bus/iio/devices", "in_illuminance_input"),
          std::make_pair("/sys/class/sensors", "lux")}) {
      DIR* dir = opendir(device_path(directory).c_str());
      if (!dir) continue;
      while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const std::string candidate = device_path(std::string(directory) + "/" + entry->d_name + "/" + file);
        if (access(candidate.c_str(), R_OK) == 0) {
          fallback_path_ = candidate;
          break;
        }
      }
      closedir(dir);
      if (!fallback_path_.empty()) {
        log_line(("Fallback(sysfs): " + fallback_path_).c_str());
        return;
      }
    }
    std::string name = als;
    if (name.size() < 4) {
      name = trimmed(run_command(
          "dumpsys sensorservice 2>/dev/null|grep -oE '[a-zA-Z0-9_]+ Ambient Light Sensor Non-wakeup'|head -1"));
    }
    if (name.size() >= 4) {
      fallback_command_ = "dumpsys sensorservice 2>/dev/null|grep -A10 '" + name +
                          ": last'|grep -oE '[0-9]+\\.[0-9]+'|tail -1";
      log_line(("Fallback(dumpsys): " + name).c_str());
    } else {
      log_line("No sensor found (fallback)");
    }
  }

  bool read_settings(iab::SystemSettings& out) override {
    const bool ok = iab::read_system_settings(device_path("/data/system/users/0/settings_system.xml"), out);
    settings_status_ = ok ? "ok" : "unreadable";
    return ok;
  }
  void set_system_writable(bool writable) override {
    chmod(backlight_file("brightness").c_str(), writable ? 0644 : 0444);
  }
  void set_light_sensors_enabled(bool enabled) override { sensors_.set_light_enabled(enabled); }
  void poll_sensors(std::vector<SensorEvent>& events) override { sensors_.poll(events); }
  bool ndk_sensors() override { return sensors_.ndk(); }
  bool back_sensor() override { return sensors_.back(); }
  int power_state() override {
    const int fd = open(backlight_file("bl_power").c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 1;
    char buffer[8] = {};
    const ssize_t count = read(fd, buffer, 7);
    close(fd);
    return count > 0 && buffer[0] == '0' ? 1 : 0;
  }
  int read_level() override { return read_int(backlight_file("brightness"), -1); }
  void write_level(int level) override {
    const std::string path = backlight_file("brightness");
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    // Root writes 0444 nodes on Android; simulated trees run unprivileged.
    if (fd < 0 && errno == EACCES && !sysroot().empty()) {
      struct stat info{};
      if (stat(path.c_str(), &info) == 0 && chmod(path.c_str(), info.st_mode | S_IWUSR) == 0) {
        fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
        chmod(path.c_str(), info.st_mode & 07777);
      }
    }
    if (fd < 0) return;
    char buffer[16];
    const int length = std::snprintf(buffer, sizeof buffer, "%d", level);
    if (write(fd, buffer, static_cast<size_t>(length)) < 0) {}
    close(fd);
  }
  float fallback_lux() override {
    if (!fallback_path_.empty()) {
      std::string text;
      return read_text(fallback_path_, text, 32) ? std::strtof(text.c_str(), nullptr) : -1.0f;
    }
    if (fallback_command_.empty()) return -1.0f;
    const int64_t now = monotonic_ms();
    if (now - dumpsys_checked_ < 15000) return dumpsys_lux_;
    dumpsys_checked_ = now;
    const std::string text = run_command(fallback_command_, 128);
    if (!text.empty()) dumpsys_lux_ = std::strtof(text.c_str(), nullptr);
    return dumpsys_lux_;
  }
  int local_hour() override { return local_hour_now(); }
  bool read_battery(int& capacity, bool& charging) override {
    capacity = read_int(device_path("/sys/class/power_supply/battery/capacity"), -1);
    std::string status;
    const bool status_ok = read_text(device_path("/sys/class/power_supply/battery/status"), status, 32);
    charging = status_ok && status.find("Charging") != std::string::npos;
    return capacity >= 0 || status_ok;
  }
  int boost_temperature() override {
    if (!zones_scanned_) {
      zones_scanned_ = true;
      for (int zone = 0; zone <= 19 && zones_.size() < 8; ++zone) {
        std::string type;
        if (!read_text(device_path("/sys/class/thermal/thermal_zone" + std::to_string(zone) + "/type"), type, 32)) {
          continue;
        }
        for (const char* word : {"battery", "skin", "shell", "quiet", "back", "soc"}) {
          if (type.find(word) != std::string::npos) {
            zones_.push_back(zone);
            break;
          }
        }
      }
    }
    int highest = 0;
    for (const int zone : zones_) {
      int value = read_int(device_path("/sys/class/thermal/thermal_zone" + std::to_string(zone) + "/temp"), -1);
      if (value > 1000) value /= 1000;
      if (value >= 16 && value <= 79) highest = std::max(highest, value);
    }
    return highest;
  }
  bool other_display_on() override {
    DIR* dir = opendir(device_path("/sys/class/backlight").c_str());
    if (!dir) return false;
    bool on = false;
    while (dirent* entry = readdir(dir)) {
      if (entry->d_name[0] == '.') continue;
      const std::string base = device_path(std::string("/sys/class/backlight/") + entry->d_name);
      if (base == backlight_) continue;
      std::string power;
      if (read_text(base + "/bl_power", power, 8) && power[0] == '0' && read_int(base + "/brightness", 0) > 0) {
        on = true;
      }
    }
    closedir(dir);
    return on;
  }
  bool user_adjusted() override {
    const std::string point = user_point();
    if (point.empty()) return false;
    const bool changed = point != last_user_point_ && point.find("NaN") == std::string::npos;
    last_user_point_ = point;
    return changed;
  }
  void save_learning(float offset) override {
    const std::string path = g_state_dir + "/learn.conf", temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "w" IAB_CLOEXEC);
    if (!file) return;
    std::fprintf(file, "user_offset=%f\n", offset);
    const bool ok = std::fflush(file) == 0 && fsync(fileno(file)) == 0;
    std::fclose(file);
    if (ok) rename(temporary.c_str(), path.c_str());
  }
  void log(const std::string& line) override { log_line(line.c_str()); }

  void prime_user_point() { last_user_point_ = user_point(); }
  const std::string& settings_status() const { return settings_status_; }
  std::string backlight_file(const char* name) const { return backlight_ + "/" + name; }

 private:
  static std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
  }
  // mUserLux/mUserBrightness of the framework mapping strategy change only on
  // user slider input, never on automatic rewrites of screen_brightness.
  std::string user_point() {
    const std::string dump = run_command("dumpsys display 2>/dev/null");
    std::string point;
    for (const char* key : {"mUserLux=", "mUserBrightness="}) {
      const size_t at = dump.find(key);
      if (at == std::string::npos) return "";
      point += dump.substr(at, dump.find('\n', at) - at) + ";";
    }
    return point;
  }

  std::string backlight_;
  Sensors& sensors_;
  std::string fallback_path_, fallback_command_;
  int64_t dumpsys_checked_ = -15000;
  float dumpsys_lux_ = -1.0f;
  bool zones_scanned_ = false;
  std::vector<int> zones_;
  std::string last_user_point_;
  std::string settings_status_ = "unread";
};

std::string find_backlight() {
  const std::string preferred = device_path("/sys/class/backlight/panel0-backlight");
  if (access((preferred + "/brightness").c_str(), F_OK) == 0) return preferred;
  std::vector<std::string> names;
  if (DIR* dir = opendir(device_path("/sys/class/backlight").c_str())) {
    while (dirent* entry = readdir(dir)) {
      if (entry->d_name[0] != '.') names.emplace_back(entry->d_name);
    }
    closedir(dir);
  }
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    const std::string base = device_path("/sys/class/backlight/" + name);
    if (read_int(base + "/max_brightness", 0) > 0) return base;
  }
  return preferred;
}

float load_offset(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "r" IAB_CLOEXEC);
  if (!file) return 0.0f;
  float offset = 0.0f;
  if (std::fscanf(file, "user_offset=%f", &offset) != 1) offset = 0.0f;
  std::fclose(file);
  return sanitized_offset(offset);
}

time_t file_mtime(const std::string& path) {
  struct stat info{};
  return stat(path.c_str(), &info) == 0 ? info.st_mtime : 0;
}

void write_status(const Engine& engine, const AndroidPlatform& platform, const Config& config,
                  const std::string& backlight, int64_t started, int64_t now) {
  const iab::EngineStatus s = engine.status();
  char buffer[4096];
  const int length = std::snprintf(
      buffer, sizeof buffer,
      "{\"ok\":true,\"version\":\"%s\",\"pid\":%d,\"heartbeat_ms\":%lld,\"uptime_ms\":%lld,"
      "\"mode\":\"%s\",\"mode_setting\":%d,\"screen_on\":%s,\"proximity_near\":%s,"
      "\"front_lux\":%.2f,\"back_lux\":%.2f,\"input_lux\":%.2f,\"lux_source\":\"%s\","
      "\"filtered_lux\":%.2f,\"smoothed_lux\":%.2f,\"alpha\":%.3f,\"nits\":%.2f,"
      "\"target\":%d,\"current\":%d,\"max\":%d,\"poll_ms\":%d,\"slider\":%d,"
      "\"user_offset\":%.4f,\"learning_enabled\":%s,\"last_learning\":\"%s\",\"dual_sensor\":%s,"
      "\"sunlight\":%s,\"fast_dark\":%s,\"stable_count\":%d,\"bright_count\":%d,\"dark_count\":%d,"
      "\"sunlight_count\":%d,\"battery\":%d,\"charging\":%s,\"settings\":\"%s\",\"backlight\":\"%s\","
      "\"local_hour\":%d}\n",
      IAB_VERSION, static_cast<int>(getpid()), static_cast<long long>(now),
      static_cast<long long>(now - started), s.automatic ? "auto" : "manual", s.mode_setting,
      s.screen_on ? "true" : "false", s.proximity_near ? "true" : "false", s.front_lux, s.back_lux,
      s.input_lux, s.lux_source.c_str(), s.filtered_lux, s.smoothed_lux, s.alpha, s.nits, s.target,
      s.current, s.max_level, s.poll_ms, s.slider, s.user_offset, config.learning_enabled ? "true" : "false",
      s.last_learning.c_str(), config.dual_sensor ? "true" : "false", s.sunlight ? "true" : "false",
      s.fast_dark ? "true" : "false", s.stable_count, s.bright_count, s.dark_count, s.sunlight_count,
      s.battery, s.charging ? "true" : "false", platform.settings_status().c_str(),
      json_escape(backlight).c_str(), local_hour_now());
  if (length <= 0 || static_cast<size_t>(length) >= sizeof buffer) return;
  const std::string path = g_state_dir + "/status.json", temporary = path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "w" IAB_CLOEXEC);
  if (!file) return;
  std::fwrite(buffer, 1, static_cast<size_t>(length), file);
  std::fclose(file);
  rename(temporary.c_str(), path.c_str());
}

struct Range {
  const char* key;
  float minimum, maximum;
};
constexpr Range kRanges[] = {
    {"alpha_fast", 0.01f, 0.95f},        {"alpha_mid", 0.01f, 0.95f},
    {"alpha_slow", 0.001f, 0.95f},       {"fast_threshold", 0.05f, 10.0f},
    {"slow_threshold", 0.001f, 1.0f},    {"spike_up", 1.0f, 100.0f},
    {"spike_down", 0.001f, 1.0f},        {"hyst_low", 0.0f, 0.5f},
    {"hyst_high", 0.0f, 0.5f},           {"manual_gamma", 0.5f, 5.0f},
    {"high_lux_threshold", 0.0f, 200000.0f}, {"high_lux_boost_max", 0.0f, 1.0f},
    {"high_lux_min_pct", 0.0f, 1.0f},    {"high_lux_duration_ms", 0.0f, 120000.0f},
    {"learning_enabled", 0.0f, 1.0f},    {"dual_sensor", 0.0f, 1.0f},
    {"log_max_bytes", 65536.0f, 8388608.0f}, {"log_max_backups", 1.0f, 8.0f},
};

// Validates WebUI input before it replaces config.conf.
int validate_config() {
  char line[512];
  float fast = 1.0f, slow = 0.05f;
  int number = 0;
  while (std::fgets(line, sizeof line, stdin)) {
    ++number;
    std::string text(line);
    text.erase(std::find(text.begin(), text.end(), '#'), text.end());
    text.erase(0, text.find_first_not_of(" \t\r\n"));
    text.erase(text.find_last_not_of(" \t\r\n") + 1);
    if (text.empty()) continue;
    const size_t equals = text.find('=');
    std::string key = text.substr(0, equals == std::string::npos ? 0 : equals);
    key.erase(key.find_last_not_of(" \t") + 1);
    const std::string value = equals == std::string::npos ? "" : text.substr(equals + 1);
    char* end = nullptr;
    const float parsed = std::strtof(value.c_str(), &end);
    const Range* range = nullptr;
    for (const Range& candidate : kRanges) {
      if (key == candidate.key) range = &candidate;
    }
    if (!range || value.empty() || *end != '\0' || !std::isfinite(parsed) || parsed < range->minimum ||
        parsed > range->maximum) {
      std::printf("{\"ok\":false,\"error\":\"第 %d 行无效：%s\"}\n", number, json_escape(text).c_str());
      return 1;
    }
    if (key == "fast_threshold") fast = parsed;
    if (key == "slow_threshold") slow = parsed;
  }
  if (!(fast > slow)) {
    std::printf("{\"ok\":false,\"error\":\"fast_threshold 必须大于 slow_threshold\"}\n");
    return 1;
  }
  std::printf("{\"ok\":true}\n");
  return 0;
}

void sleep_ms(int64_t milliseconds) {
  timespec duration{static_cast<time_t>(milliseconds / 1000), static_cast<long>(milliseconds % 1000) * 1000000};
  nanosleep(&duration, nullptr);
}

int run(const std::string& state) {
  g_state_dir = state;
  const std::string config_path = state + "/config.conf";
  const std::string lock_path = state + "/core.lock";
  const int lock = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
    log_line("Another core instance holds core.lock; exiting");
    return 1;
  }
  struct sigaction action{};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  for (const int signal : {SIGTERM, SIGINT, SIGHUP, SIGUSR1, SIGUSR2}) sigaction(signal, &action, nullptr);
  ::signal(SIGPIPE, SIG_IGN);

  Config config;
  iab::load_config(config_path, config);
  time_t config_mtime = file_mtime(config_path);
  const float offset = load_offset(state + "/learn.conf");
  if (std::fabs(offset) > 0.001f) {
    char text[64];
    std::snprintf(text, sizeof text, "Learn: offset=%.3f", offset);
    log_line(text);
  }
  const std::string backlight = find_backlight();
  int max_level = read_int(backlight + "/max_brightness", 0);
  if (max_level <= 0) max_level = 16383;
  {
    FILE* file = std::fopen((state + "/backlight").c_str(), "w" IAB_CLOEXEC);
    if (file) std::fprintf(file, "%s\n", backlight.c_str()), std::fclose(file);
  }
  char text[256];
  std::snprintf(text, sizeof text, "iOS Brightness %s (v6.8 reference engine) | %s | Max: %d", IAB_VERSION,
                backlight.c_str(), max_level);
  log_line(text);
  std::snprintf(text, sizeof text, "阳光模式: threshold=%.0f boost_max=%.0f%% min=%.0f%% duration=%dms",
                config.high_lux_threshold, config.high_lux_boost_max * 100.0f, config.high_lux_min_pct * 100.0f,
                config.high_lux_duration_ms);
  log_line(text);

  Sensors sensors;
  sensors.open(log_line);
  AndroidPlatform platform(backlight, sensors);
  if (!sensors.ndk()) platform.discover_fallback();
  platform.prime_user_point();
  Engine engine(platform, config, max_level, offset);
  engine.begin(monotonic_ms());

  log_line("Waiting for sensor...");
  float lux = -1.0f;
  for (int attempt = 0; attempt < 40 && g_run; ++attempt) {
    if (sensors.ndk()) {
      std::vector<SensorEvent> events;
      sensors.poll(events);
      engine.apply_events(events);
      lux = engine.front_lux();
    } else {
      lux = platform.fallback_lux();
    }
    if (lux >= 0.0f) break;
    sleep_ms(250);
  }
  if (lux < 0.0f) {
    lux = 100.0f;
    log_line("⚠️ Sensor timeout: 100.0 lux");
  } else {
    std::snprintf(text, sizeof text, "✅ Sensor ready: %.1f lux%s", lux, sensors.back() ? " +back" : "");
    log_line(text);
  }
  const int64_t started = monotonic_ms();
  engine.start(started, lux);
  {
    FILE* file = std::fopen((state + "/daemon.pid").c_str(), "w" IAB_CLOEXEC);
    if (file) std::fprintf(file, "%d\n", static_cast<int>(getpid())), std::fclose(file);
  }
  write_status(engine, platform, config, backlight, started, started);

  int64_t last_tick = started, config_checked = started;
  while (g_run) {
    const int64_t now = monotonic_ms();
    if (g_reload || now - config_checked >= 5000) {
      config_checked = now;
      const time_t mtime = file_mtime(config_path);
      if (g_reload || mtime > config_mtime) {
        g_reload = 0;
        config_mtime = mtime;
        iab::load_config(config_path, config);
        engine.set_config(config);
        log_line("Config reloaded");
      }
    }
    if (g_reset_learning) {
      g_reset_learning = 0;
      engine.reset_learning();
    }
    if (now - last_tick >= engine.poll_ms()) {
      engine.tick(now);
      last_tick = now;
    }
    if (g_dump_status) {
      g_dump_status = 0;
      write_status(engine, platform, config, backlight, started, monotonic_ms());
    }
    const int64_t remaining = engine.poll_ms() - (monotonic_ms() - last_tick);
    if (remaining > 0 && g_run && !g_dump_status) sleep_ms(std::min<int64_t>(remaining, 1000));
  }
  engine.shutdown();
  sensors.close();
  unlink((state + "/daemon.pid").c_str());
  unlink((state + "/status.json").c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--validate-config") == 0) return validate_config();
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("%s\n", IAB_VERSION);
    return 0;
  }
  if (argc == 3 && std::strcmp(argv[1], "--state") == 0) return run(argv[2]);
  std::fprintf(stderr, "usage: %s --state DIR | --validate-config | --version\n", argv[0]);
  return 2;
}
