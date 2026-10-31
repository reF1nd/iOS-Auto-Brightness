// SPDX-License-Identifier: GPL-3.0-only
// Root daemon: Android access for the reference control loop in engine.cpp.
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "android_resources.hpp"
#include "config.hpp"
#include "display_dump.hpp"
#include "engine.hpp"
#include "settings_reader.hpp"
#include "system_curve.hpp"
#include "system_link.hpp"

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

std::string trimmed(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
  return text;
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

constexpr char kRefactorSection[] = "Refactor Nit Controller Configuration:";

// The RefactorNitController block of the main display in dumpsys display.
std::string refactor_section(const std::string& dump) {
  for (size_t at = dump.find(kRefactorSection); at != std::string::npos; at = dump.find(kRefactorSection, at + 1)) {
    const size_t end = dump.find("\n\n", at);
    const std::string block = dump.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (block.find("mDisplayId=0\n") != std::string::npos) return block;
  }
  return "";
}

float dump_value(const std::string& block, const char* key) {
  const size_t at = block.find(key);
  if (at == std::string::npos) return NAN;
  return std::strtof(block.c_str() + at + std::strlen(key), nullptr);
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
// "Optimization light fov Sensor": AutomaticBrightnessControllerImpl.switchLightSensor
// reads it for display 0 when ro.miui.support.light.fov is 1.
constexpr int kTypeLightFov = 33172111;
constexpr int kSensorRateUs = 800000;

class Sensors {
 public:
  // main_type: the light sensor the framework reads for the main display.
  bool open(void (*log)(const char*), int main_type) {
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
    front_ = main_type == kTypeLight ? nullptr : find(main_type);
    main_type_ = front_ ? main_type : kTypeLight;
    if (!front_) front_ = get_default_(manager_, kTypeLight);
    if (front_ && enable_(queue_, front_) >= 0) {
      if (set_rate_) set_rate_(queue_, front_, kSensorRateUs);
      ndk_ = true;
      log(main_type_ == kTypeLight ? "Front ALS ready (800000us)" : "Front ALS ready (800000us, fov sensor as the system)");
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

  // driving_factor > 0: night driving; AutomaticBrightnessController then
  // takes values[2] times the fov factor when values[1] is 1.
  void poll(std::vector<SensorEvent>& events, float driving_factor) {
    if (!queue_) return;
    if (poll_once_) poll_once_(0, nullptr, nullptr, nullptr);
    const int64_t mono = monotonic_ms(), boot = boottime_ms();
    NativeSensorEvent event;
    while (get_events_(queue_, &event, 1) > 0) {
      SensorEvent out;
      if (event.type == main_type_) out.kind = SensorEvent::Kind::Front;
      else if (event.type == kTypeBackLight) out.kind = SensorEvent::Kind::Back;
      else if (event.type == kTypeProximity) out.kind = SensorEvent::Kind::Proximity;
      else continue;
      // Event timestamps use CLOCK_BOOTTIME; the loop runs on CLOCK_MONOTONIC.
      const int64_t age = boot - event.timestamp / 1000000;
      out.time = mono - std::clamp<int64_t>(age, 0, 60000);
      out.value = event.data[0];
      if (out.kind == SensorEvent::Kind::Front && driving_factor > 0.0f && event.data[1] == 1.0f) {
        out.value = event.data[2] * driving_factor;
      }
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
  int main_type_ = kTypeLight;
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
  AndroidPlatform(const std::string& backlight, Sensors& sensors, iab::SystemLink& link, float main_factor)
      : backlight_(backlight), sensors_(sensors), link_(link), main_factor_(main_factor) {}

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
    const bool ok = iab::read_system_settings(settings_path(), out);
    settings_status_ = ok ? "ok" : "unreadable";
    return ok;
  }
  void set_system_writable(bool writable) override {
    chmod(backlight_file("brightness").c_str(), writable ? 0644 : 0444);
  }
  void set_light_sensors_enabled(bool enabled) override { sensors_.set_light_enabled(enabled); }
  void poll_sensors(std::vector<SensorEvent>& events) override {
    sensors_.poll(events, (scene() & iab::DualSensorPolicy::kNightDriving) ? main_factor_ : 0.0f);
  }
  bool ndk_sensors() override { return sensors_.ndk(); }
  bool back_sensor() override { return sensors_.back(); }
  int power_state() override {
    // bl_power reads '0' in the AOD too; the reporter knows DOZE apart from ON.
    if (link_.connected() && link_.display_state() != iab::SystemLink::kUnknown) {
      const int state = link_.display_state();
      return state == kDisplayOn || state == kDisplayVr || state == kDisplayOnSuspend ? 1 : 0;
    }
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
  // Reported live by the Zygisk part; otherwise read from dumpsys display.
  float thermal_limit() override {
    if (link_.connected() && std::isfinite(link_.limit())) return link_.limit();
    if (monotonic_ms() - dumped_ >= 8000) display_dump();
    return std::isfinite(dumped_limit_) ? dumped_limit_ : 1.0f;
  }
  // Only the Zygisk part can see the torch and SceneDetector.
  unsigned scene() override {
    return link_.connected() && link_.scene() >= 0 ? static_cast<unsigned>(link_.scene()) : 0u;
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
  bool user_drag(float& lux, float& logical) override {
    // The reporter sends mUserLux/mUserLogicalNit with every anchor change.
    if (link_.connected()) {
      if (link_.anchors().empty()) return false;
      float user_lux = 0.0f, user_logical = 0.0f;
      const std::string point = link_point(link_.anchors(), user_lux, user_logical);
      return changed(point, user_lux, user_logical, lux, logical);
    }
    // dumpsys display is large: query it when the settings file changed (a
    // drag persists the slider) and otherwise every 8 s.
    const int64_t now = monotonic_ms();
    struct stat info{};
    long long stamp = 0;
    if (stat(settings_path().c_str(), &info) == 0) {
#ifdef __APPLE__
      const long nanoseconds = info.st_mtimespec.tv_nsec;
#else
      const long nanoseconds = info.st_mtim.tv_nsec;
#endif
      stamp = (static_cast<long long>(info.st_mtime) * 1000000000LL + nanoseconds) ^
              static_cast<long long>(info.st_size);
    }
    if (stamp == settings_stamp_ && now - user_checked_ < 8000) return false;
    settings_stamp_ = stamp;
    user_checked_ = now;
    float user_lux = 0.0f, user_logical = 0.0f;
    const std::string point = user_point(display_dump(), user_lux, user_logical);
    return changed(point, user_lux, user_logical, lux, logical);
  }
  bool follows_system() override { return link_.connected() && !link_.anchors().empty(); }
  bool system_anchors(std::string& text) override {
    if (!follows_system() || link_.anchors() == seen_anchors_) return false;
    text = seen_anchors_ = link_.anchors();
    return true;
  }
  void reset_system_anchors() override { link_.send_reset(); }
  int64_t elapsed_ms() override { return boottime_ms(); }
  void save_learning(const std::string& text) override {
    const std::string path = g_state_dir + "/learn.conf", temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "w" IAB_CLOEXEC);
    if (!file) return;
    std::fwrite(text.data(), 1, text.size(), file);
    const bool ok = std::fflush(file) == 0 && fsync(fileno(file)) == 0;
    std::fclose(file);
    if (ok) rename(temporary.c_str(), path.c_str());
  }
  void log(const std::string& line) override { log_line(line.c_str()); }

  // dumpsys display, also noting the thermal limit it shows.
  std::string display_dump() {
    std::string dump = run_command("dumpsys display 2>/dev/null");
    dumped_ = monotonic_ms();
    const float limit = iab::read_thermal_limit(iab::main_display_section(dump));
    if (std::isfinite(limit)) dumped_limit_ = limit;
    return dump;
  }
  void prime_user_point(const std::string& dump) {
    float lux = 0.0f, logical = 0.0f;
    last_user_point_ = user_point(dump, lux, logical);
    user_checked_ = monotonic_ms();
  }
  const std::string& settings_status() const { return settings_status_; }
  const char* link_status() const { return link_.connected() ? "zygisk" : "dumpsys"; }
  std::string backlight_file(const char* name) const { return backlight_ + "/" + name; }

 private:
  // Display.STATE_* values the reporter sends.
  static constexpr int kDisplayOn = 2, kDisplayVr = 5, kDisplayOnSuspend = 6;

  // mUserLux/mUserLogicalNit of RefactorNitController change only when a
  // drag reaches updateLogicalCurve, never on automatic slider rewrites.
  static std::string user_point(const std::string& dump, float& lux, float& logical);
  static std::string link_point(const std::string& anchors, float& lux, float& logical);
  static std::string point_text(float lux, float logical) {
    if (!std::isfinite(lux) || !std::isfinite(logical)) return "";
    char point[64];
    std::snprintf(point, sizeof point, "%.7g %.7g", lux, logical);
    return point;
  }
  // Both sources format the point alike, so switching between them is no drag.
  bool changed(const std::string& point, float user_lux, float user_logical, float& lux, float& logical) {
    if (point.empty()) return false;
    const bool moved = point != last_user_point_ && user_lux >= 0.0f && user_logical >= 0.0f;
    last_user_point_ = point;
    if (moved) lux = user_lux, logical = user_logical;
    return moved;
  }

  std::string backlight_;
  Sensors& sensors_;
  iab::SystemLink& link_;
  std::string seen_anchors_;
  std::string fallback_path_, fallback_command_;
  int64_t dumpsys_checked_ = -15000;
  float dumpsys_lux_ = -1.0f;
  float main_factor_;
  int64_t dumped_ = INT64_MIN / 2;
  float dumped_limit_ = NAN;
  std::string settings_path() const { return device_path("/data/system/users/0/settings_system.xml"); }

  std::string last_user_point_;
  long long settings_stamp_ = 0;
  int64_t user_checked_ = 0;
  std::string settings_status_ = "unread";
};

std::string AndroidPlatform::user_point(const std::string& dump, float& lux, float& logical) {
  const std::string block = refactor_section(dump);
  if (block.empty()) return "";
  lux = dump_value(block, "mUserLux=");
  logical = dump_value(block, "mUserLogicalNit=");
  return point_text(lux, logical);
}

std::string AndroidPlatform::link_point(const std::string& anchors, float& lux, float& logical) {
  const size_t at = anchors.find("\nuser=");
  if (at == std::string::npos || std::sscanf(anchors.c_str() + at + 6, "%f %f", &lux, &logical) != 2) return "";
  return point_text(lux, logical);
}

// Only system_server may report: uid 1000 and its process name.
bool system_server_peer(int fd) {
#ifdef SO_PEERCRED
  ucred peer{};
  socklen_t size = sizeof peer;
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != 1000) return false;
  std::string name;
  if (!read_text("/proc/" + std::to_string(peer.pid) + "/cmdline", name, 64)) return false;
  return std::strcmp(name.c_str(), "system_server") == 0;
#else
  (void)fd;
  return false;
#endif
}


std::string property(const char* name) {
  return trimmed(run_command(std::string("getprop ") + name + " 2>/dev/null", 256));
}

// Float.valueOf of the property; 1 when unset, like loadSensorConfig.
float factor_property(const char* name) {
  const std::string text = property(name);
  char* end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  return end != text.c_str() && std::isfinite(value) && value > 0.0f ? value : 1.0f;
}

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

std::vector<float> float_list(const std::string& text, size_t at, size_t end) {
  std::vector<float> out;
  std::string list = text.substr(at, end - at);
  char* cursor = &list[0];
  while (*cursor) {
    char* next = nullptr;
    const float value = std::strtof(cursor, &next);
    if (next == cursor) break;
    out.push_back(value);
    cursor = next;
    while (*cursor == ',' || *cursor == ' ') ++cursor;
  }
  return out;
}

// screenBrightnessMap of the main display, from the first DisplayDeviceConfig
// in the dump ("mNits=[...] ... mBrightness=[...]" on one line).
iab::PanelMap read_panel_map(const std::string& dump) {
  const size_t nits = dump.find("mNits=[");
  if (nits == std::string::npos) return iab::PanelMap({}, {});
  const size_t brightness = dump.find("mBrightness=[", nits);
  if (brightness == std::string::npos || dump.find('\n', nits) < brightness) return iab::PanelMap({}, {});
  return iab::PanelMap(float_list(dump, brightness + 13, dump.find(']', brightness)),
                       float_list(dump, nits + 7, dump.find(']', nits)));
}

// The (lux, nits) points of the first BrightnessConfiguration: the default
// curve of the AOSP mapper.
bool read_brightness_configuration(const std::string& dump, std::vector<float>& lux, std::vector<float>& nits) {
  static const char kKey[] = "mConfig=BrightnessConfiguration{[";
  const size_t at = dump.find(kKey);
  if (at == std::string::npos) return false;
  const size_t end = dump.find(']', at);
  for (size_t p = dump.find('(', at); p < end; p = dump.find('(', p + 1)) {
    const std::vector<float> pair = float_list(dump, p + 1, dump.find(')', p));
    if (pair.size() == 2) lux.push_back(pair[0]), nits.push_back(pair[1]);
  }
  return lux.size() >= 2;
}

bool safe_package(const std::string& name) {
  return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_';
  });
}

// framework-ext-res (com.miui.rom) values of RefactorBrightnessUtil and
// RefactorNitController, with the enabled overlays applied in priority order.
iab::RefactorConfig read_refactor_config() {
  static const std::vector<std::string> kWanted = {
      "dimen/config_minLogicalNit",        "dimen/config_maxLogicalNit",
      "dimen/config_firstNitNode",         "dimen/config_secondNitNode",
      "array/config_firstFactorConfig",    "array/config_secondFactorConfig",
      "array/config_defaultLogicalCurve",  "array/config_highLuxPoints",
      "array/config_highLuxScalingFactorPoints", "array/config_goodCurveLuxPoints",
      "array/config_goodCurveNitPoints",   "bool/config_brightness_reset_policy"};
  iab::RefactorConfig config;
  std::string base = trimmed(run_command("pm path com.miui.rom 2>/dev/null"));
  if (base.rfind("package:", 0) == 0) base = base.substr(8);
  if (base.empty()) base = "/system_ext/framework/framework-ext-res/framework-ext-res.apk";
  iab::ResourceValues values;
  std::string error;
  if (!iab::read_apk_resources(device_path(base), kWanted, values, error)) {
    log_line(("Curve: " + base + ": " + error + "; using the altdb values").c_str());
    return config;
  }
  int overlays = 0;
  const std::string list = run_command("cmd overlay list com.miui.rom 2>/dev/null");
  for (size_t at = list.find("[x] "); at != std::string::npos; at = list.find("[x] ", at + 1)) {
    const std::string name = trimmed(list.substr(at + 4, list.find('\n', at) - at - 4));
    if (!safe_package(name)) continue;
    const std::string dump = run_command("cmd overlay dump " + name + " 2>/dev/null");
    const size_t key = dump.find("mBaseCodePath");
    const size_t colon = key == std::string::npos ? key : dump.find(": ", key);
    if (colon == std::string::npos) continue;
    const std::string path = trimmed(dump.substr(colon + 2, dump.find('\n', colon) - colon - 2));
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".apk") == 0 &&
        iab::read_apk_resources(device_path(path), kWanted, values, error)) {
      ++overlays;
    }
  }
  iab::RefactorConfig read = config;
  const auto scalar = [&](const char* name, float& field) {
    const auto found = values.find(name);
    if (found != values.end() && found->second.size() == 1) field = found->second[0];
  };
  const auto array = [&](const char* name, std::vector<float>& field) {
    const auto found = values.find(name);
    if (found != values.end()) field = found->second;
  };
  scalar("dimen/config_minLogicalNit", read.min_nit);
  scalar("dimen/config_maxLogicalNit", read.max_nit);
  scalar("dimen/config_firstNitNode", read.first_node);
  scalar("dimen/config_secondNitNode", read.second_node);
  array("array/config_firstFactorConfig", read.first_factor);
  array("array/config_secondFactorConfig", read.second_factor);
  array("array/config_defaultLogicalCurve", read.default_curve);
  array("array/config_highLuxPoints", read.high_lux);
  array("array/config_highLuxScalingFactorPoints", read.high_scale);
  array("array/config_goodCurveLuxPoints", read.good_lux);
  array("array/config_goodCurveNitPoints", read.good_nit);
  float reset = 0.0f;
  scalar("bool/config_brightness_reset_policy", reset);
  read.reset_policy = reset != 0.0f;
  if (!read.valid()) {
    log_line(("Curve: unusable framework-ext-res values in " + base + "; using the altdb values").c_str());
    return config;
  }
  char text[256];
  std::snprintf(text, sizeof text,
                "Curve: framework-ext-res + %d overlays, anchors (0,%g) (%g,%g) (%g,%g) (%g,%g), reset policy %s",
                overlays, read.default_curve[1], read.seg0(), read.default_curve[3], read.seg1(), read.default_curve[6],
                read.lux_max(), read.max_nit, read.reset_policy ? "on" : "off");
  log_line(text);
  return read;
}

// The system's auto-brightness curve for this ROM (see system_curve.hpp).
iab::SystemCurve load_system_curve(const std::string& dump, const iab::PanelMap& panel) {
  const std::string policy = trimmed(run_command(
      "cmd overlay lookup android android:bool/config_refactor_brightness_policy 2>/dev/null"));
  const bool section = !refactor_section(dump).empty();
  const bool refactor = policy == "true" || (policy != "false" && section);
  std::vector<float> lux, nits;
  const bool table = read_brightness_configuration(dump, lux, nits);
  char text[256];
  if (!refactor && table) {
    std::snprintf(text, sizeof text, "Curve: BrightnessConfiguration (%zu points); refactor policy %s, drags are not learned",
                  lux.size(), policy.empty() ? "unknown" : policy.c_str());
    log_line(text);
    return iab::SystemCurve(lux, nits, panel);
  }
  iab::RefactorConfig config = read_refactor_config();
  if (dump.find("mResetShortTermModelPolicyDisable=true") != std::string::npos) config.reset_policy = false;
  iab::SystemCurve curve(config, panel);
  if (table) {
    // On altdb the two agree within 1.5% from 1 lux up (the table starts at
    // 1 nit, the anchors at 2); far apart means unexpected values were read.
    float worst = 0.0f, worst_lux = 0.0f;
    for (size_t i = 0; i < lux.size(); ++i) {
      if (lux[i] < 1.0f || !(nits[i] > 0.0f)) continue;
      const float deviation = std::fabs(curve.default_nits(lux[i]) - nits[i]) / nits[i];
      if (deviation > worst) worst = deviation, worst_lux = lux[i];
    }
    std::snprintf(text, sizeof text, "Curve check: defaults vs BrightnessConfiguration max %.1f%% at %.0f lux%s",
                  worst * 100.0f, worst_lux, worst > 0.05f ? " (unexpected, check the ROM values)" : "");
    log_line(text);
  }
  if (!refactor) log_line("Curve: no refactor policy or BrightnessConfiguration found; HyperOS curve without learning input");
  return curve;
}

time_t file_mtime(const std::string& path) {
  struct stat info{};
  return stat(path.c_str(), &info) == 0 ? info.st_mtime : 0;
}

void write_status(const Engine& engine, const AndroidPlatform& platform, const Config& config,
                  const std::string& backlight, const std::string& curve_source, int64_t started, int64_t now) {
  const iab::EngineStatus s = engine.status();
  const iab::SystemCurve& curve = engine.curve();
  const iab::NitController& controller = curve.controller();
  std::string anchors;
  // [lux, logical nits, dragged, panel level there before the night factor]
  for (const iab::Anchor& anchor : curve.learnable() ? controller.anchors() : std::vector<iab::Anchor>{}) {
    char item[96];
    std::snprintf(item, sizeof item, "%s[%.2f,%.2f,%d,%d]", anchors.empty() ? "" : ",", anchor.lux, anchor.nit,
                  anchor.user ? 1 : 0,
                  curve.level(iab::refactor_nits(controller.config(), anchor.nit, anchor.lux), s.max_level));
    anchors += item;
  }
  // The current and the default curve for the WebUI chart, 0.1..100000 lux,
  // before the night and battery limits.
  std::string current, defaults;
  for (int i = 0; i <= 30; ++i) {
    const float lux = std::pow(10.0f, static_cast<float>(i) / 5.0f - 1.0f);
    char item[48];
    std::snprintf(item, sizeof item, "%s[%.4g,%d]", current.empty() ? "" : ",", lux, engine.curve_level(lux));
    current += item;
    std::snprintf(item, sizeof item, "%s[%.4g,%d]", defaults.empty() ? "" : ",", lux,
                  curve.level(curve.default_nits(lux), s.max_level));
    defaults += item;
  }
  char buffer[8192];
  const int length = std::snprintf(
      buffer, sizeof buffer,
      "{\"ok\":true,\"version\":\"%s\",\"pid\":%d,\"heartbeat_ms\":%lld,\"uptime_ms\":%lld,"
      "\"mode\":\"%s\",\"mode_setting\":%d,\"screen_on\":%s,\"proximity_near\":%s,"
      "\"front_lux\":%.2f,\"back_lux\":%.2f,\"input_lux\":%.2f,\"lux_source\":\"%s\","
      "\"filtered_lux\":%.2f,\"smoothed_lux\":%.2f,\"alpha\":%.3f,\"nits\":%.2f,"
      "\"target\":%d,\"current\":%d,\"max\":%d,\"poll_ms\":%d,\"slider\":%d,"
      "\"curve_source\":\"%s\",\"learnable\":%s,\"learning_enabled\":%s,\"last_learning\":\"%s\","
      "\"anchors\":[%s],\"good_curve\":%s,\"reset_policy\":%s,\"user_lux\":%.2f,\"user_logical\":%.2f,"
      "\"curve\":[%s],\"default_curve\":[%s],\"dual_sensor\":%s,\"dual_policy\":%s,"
      "\"thermal_limit\":%.4f,\"scene\":%u,"
      "\"sunlight\":%s,\"fast_dark\":%s,\"stable_count\":%d,\"bright_count\":%d,\"dark_count\":%d,"
      "\"sunlight_count\":%d,\"battery\":%d,\"charging\":%s,\"settings\":\"%s\",\"backlight\":\"%s\","
      "\"system_link\":\"%s\",\"local_hour\":%d}\n",
      IAB_VERSION, static_cast<int>(getpid()), static_cast<long long>(now),
      static_cast<long long>(now - started), s.automatic ? "auto" : "manual", s.mode_setting,
      s.screen_on ? "true" : "false", s.proximity_near ? "true" : "false", s.front_lux, s.back_lux,
      s.input_lux, s.lux_source.c_str(), s.filtered_lux, s.smoothed_lux, s.alpha, s.nits, s.target,
      s.current, s.max_level, s.poll_ms, s.slider, json_escape(curve_source).c_str(),
      curve.learnable() ? "true" : "false", config.learning_enabled ? "true" : "false", s.last_learning.c_str(),
      anchors.c_str(), controller.has_good_curve() ? "true" : "false",
      controller.config().reset_policy ? "true" : "false", controller.user_lux(), controller.user_logical(),
      current.c_str(), defaults.c_str(), config.dual_sensor ? "true" : "false", s.dual_policy ? "true" : "false",
      s.thermal_limit, s.scene, s.sunlight ? "true" : "false",
      s.fast_dark ? "true" : "false", s.stable_count, s.bright_count, s.dark_count, s.sunlight_count,
      s.battery, s.charging ? "true" : "false", platform.settings_status().c_str(),
      json_escape(backlight).c_str(), platform.link_status(), local_hour_now());
  if (length <= 0 || static_cast<size_t>(length) >= sizeof buffer) return;
  const std::string path = g_state_dir + "/status.json", temporary = path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "w" IAB_CLOEXEC);
  if (!file) return;
  std::fwrite(buffer, 1, static_cast<size_t>(length), file);
  std::fclose(file);
  rename(temporary.c_str(), path.c_str());
}

// The root manager lists module.prop's description; lead it with "[state] ".
// service.sh writes the states without a core in the same form.
void show_state(const std::string& prop, const std::string& state) {
  static std::string shown;
  if (prop.empty() || state == shown) return;
  std::string text;
  // module.prop is a few hundred bytes; a full buffer means it would be cut.
  if (!read_text(prop, text, 4096) || text.size() >= 4095) return;
  const size_t at = text.compare(0, 12, "description=") == 0 ? 0 : text.find("\ndescription=");
  if (at == std::string::npos) return;
  const size_t value = text.find('=', at) + 1;
  const size_t end = std::min(text.find('\n', value), text.size());
  std::string base = text.substr(value, end - value);
  if (base.compare(0, 1, "[") == 0 && base.find("] ") != std::string::npos) base.erase(0, base.find("] ") + 2);
  text.replace(value, end - value, "[" + state + "] " + base);
  const std::string temporary = prop + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "w" IAB_CLOEXEC);
  if (!file) return;
  const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
  if (std::fclose(file) == 0 && ok && chmod(temporary.c_str(), 0644) == 0 && rename(temporary.c_str(), prop.c_str()) == 0) {
    shown = state;
  } else {
    unlink(temporary.c_str());
  }
}

std::string running_state(const iab::EngineStatus& s, const AndroidPlatform& platform, bool zygisk_off) {
  if (zygisk_off) return "⚠️ 运行中 · Zygisk 已停用，重装模块可恢复";
  if (!s.automatic) return "✅ 运行中 · 手动亮度，不接管";
  return std::string("✅ 运行中 · 自动亮度 · ") +
         (std::strcmp(platform.link_status(), "zygisk") == 0 ? "Zygisk 联动" : "dumpsys 轮询");
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

// Sleeps until the timeout or a report arrives.
void wait_for(const iab::SystemLink& link, int64_t ms) {
  std::vector<pollfd> fds;
  for (const int fd : link.fds()) fds.push_back({fd, POLLIN, 0});
  if (fds.empty()) {
    sleep_ms(ms);
    return;
  }
  poll(fds.data(), static_cast<nfds_t>(fds.size()), static_cast<int>(ms));
}

void log_link(unsigned events, const iab::SystemLink& link, int& rejected) {
  if (events & iab::SystemLink::kConnected) log_line("System link: Zygisk reporter connected");
  if (events & iab::SystemLink::kLost) log_line("System link: reporter gone; back to dumpsys and bl_power");
  if ((events & iab::SystemLink::kRejected) && rejected < 5) {
    ++rejected;
    log_line("System link: refused a peer that is not system_server or speaks another protocol");
  }
  if ((events & iab::SystemLink::kScene) && link.connected()) {
    const int scene = link.scene();
    char text[128];
    std::snprintf(text, sizeof text, "System link: scene%s%s%s%s", scene == 0 ? " clear" : "",
                  (scene & iab::DualSensorPolicy::kAssistBlocked) ? " rear-blocked(torch)" : "",
                  (scene & iab::DualSensorPolicy::kNightDriving) ? " night-driving" : "",
                  (scene & iab::DualSensorPolicy::kReflective) ? " reflective" : "");
    log_line(text);
  }
  if ((events & iab::SystemLink::kState) && link.connected()) {
    char text[64];
    std::snprintf(text, sizeof text, "System link: display state %d", link.display_state());
    log_line(text);
  }
}

int run(const std::string& state, const std::string& prop) {
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
  const std::string backlight = find_backlight();
  int max_level = read_int(backlight + "/max_brightness", 0);
  if (max_level <= 0) max_level = 16383;
  {
    FILE* file = std::fopen((state + "/backlight").c_str(), "w" IAB_CLOEXEC);
    if (file) std::fprintf(file, "%s\n", backlight.c_str()), std::fclose(file);
  }
  char text[256];
  std::snprintf(text, sizeof text, "iOS Brightness %s (system curve) | %s | Max: %d", IAB_VERSION,
                backlight.c_str(), max_level);
  log_line(text);
  std::snprintf(text, sizeof text, "阳光模式: threshold=%.0f boost_max=%.0f%% min=%.0f%% duration=%dms",
                config.high_lux_threshold, config.high_lux_boost_max * 100.0f, config.high_lux_min_pct * 100.0f,
                config.high_lux_duration_ms);
  log_line(text);

  // AutomaticBrightnessControllerImpl.loadSensorConfig.
  const bool fov_sensor = property("ro.miui.support.light.fov") == "1";
  const float main_factor = factor_property("ro.miui.sensor.fov.factor");
  Sensors sensors;
  sensors.open(log_line, fov_sensor ? kTypeLightFov : kTypeLight);
  iab::SystemLink link(system_server_peer);
  if (!link.listen("iab_brightness")) log_line("System link: cannot listen; using dumpsys and bl_power");
  AndroidPlatform platform(backlight, sensors, link, main_factor);
  if (!sensors.ndk()) platform.discover_fallback();
  const std::string dump = platform.display_dump();
  platform.prime_user_point(dump);
  iab::PanelMap panel = read_panel_map(dump);
  if (!panel.valid()) {
    log_line("Panel map: dumpsys unavailable, using the altdb display config");
    panel = iab::PanelMap();
  }
  iab::SystemCurve curve = load_system_curve(dump, panel);
  const bool zygisk_off = access((state + "/zygisk.disabled").c_str(), F_OK) == 0;
  const std::string curve_source = !curve.learnable() ? "BrightnessConfiguration" : "RefactorNitController";
  {
    std::string saved;
    bool restored = false;
    if (read_text(state + "/learn.conf", saved, 16384) && !saved.empty()) {
      iab::NitController& controller = curve.controller();
      restored = controller.parse(saved);
      if (restored) {
        std::snprintf(text, sizeof text, "Learn: %d dragged anchors restored%s", controller.user_anchors(),
                      controller.has_good_curve() ? " (good curve pending)" : "");
      } else if (saved.find("version=2") == std::string::npos) {
        std::snprintf(text, sizeof text, "Learn: learn.conf of an older format is not used");
      } else {
        std::snprintf(text, sizeof text, "Learn: learn.conf was made for other default anchors; starting over");
      }
      log_line(text);
    }
    // Nothing of ours yet: take over the drags the framework holds since boot.
    if (!restored && curve.learnable() && curve.controller().adopt(refactor_section(dump))) {
      std::snprintf(text, sizeof text, "Learn: took over %d dragged anchors from the system",
                    curve.controller().user_anchors());
      log_line(text);
      platform.save_learning(curve.controller().serialize());
    }
    // The framework forgets its anchors on reboot and then writes its default
    // curve in between our writes. The Zygisk library puts learn.conf back
    // into it at boot; report whether the two agree.
    if (curve.learnable() && curve.controller().user_anchors() > 0) {
      iab::NitController system(curve.controller().config());
      const bool same = system.adopt(refactor_section(dump)) && system.same(curve.controller());
      log_line(same ? "Learn: the system holds the same anchors"
                    : "Learn: the system holds other anchors and writes its own curve in between "
                      "(Zygisk restore not active)");
    }
    if (zygisk_off) {
      log_line("Zygisk: anchor restore stopped after system_server restarted repeatedly; install the module "
               "again to re-enable it");
    }
  }
  Engine engine(platform, config, max_level, curve);
  {
    iab::DualSensorConfig dual;
    if (iab::read_dual_sensor_config(iab::main_display_section(dump), dual)) {
      dual.assist_factor = factor_property("ro.miui.sensor.fov_back.factor");
      engine.set_dual_sensor(dual);
      std::snprintf(text, sizeof text,
                    "Dual sensor: system thresholds (%zu/%zu/%zu points), HBM %.0f lux, horizons %lld/%lld ms, "
                    "debounce %lld/%lld/%lld ms, rear %lld/%lld/%lld ms x%g",
                    dual.brightening.size(), dual.small_brightening.size(), dual.darkening.size(),
                    dual.hbm_minimum_lux, static_cast<long long>(dual.horizon_short),
                    static_cast<long long>(dual.horizon_long), static_cast<long long>(dual.brightening_debounce),
                    static_cast<long long>(dual.small_brightening_debounce),
                    static_cast<long long>(dual.darkening_debounce),
                    static_cast<long long>(dual.assist_brightening_debounce),
                    static_cast<long long>(dual.assist_small_brightening_debounce),
                    static_cast<long long>(dual.assist_darkening_debounce), dual.assist_factor);
      log_line(text);
    } else if (sensors.back()) {
      log_line("Dual sensor: no DualSensorPolicy for display 0 in dumpsys display; the rear sensor only stands in "
               "for a missing front reading");
    }
  }
  engine.begin(monotonic_ms());

  log_line("Waiting for sensor...");
  float lux = -1.0f;
  for (int attempt = 0; attempt < 40 && g_run; ++attempt) {
    if (sensors.ndk()) {
      std::vector<SensorEvent> events;
      platform.poll_sensors(events);
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
  write_status(engine, platform, config, backlight, curve_source, started, started);

  int64_t last_tick = started, config_checked = started;
  int rejected = 0;
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
    } else {
      engine.check_learning(now);
    }
    if (g_dump_status) {
      g_dump_status = 0;
      write_status(engine, platform, config, backlight, curve_source, started, monotonic_ms());
    }
    show_state(prop, running_state(engine.status(), platform, zygisk_off));
    const int64_t remaining = engine.poll_ms() - (monotonic_ms() - last_tick);
    if (remaining > 0 && g_run && !g_dump_status) wait_for(link, std::min<int64_t>(remaining, 1000));
    const unsigned events = link.service();
    log_link(events, link, rejected);
    if (events & (iab::SystemLink::kState | iab::SystemLink::kLimit | iab::SystemLink::kConnected |
                  iab::SystemLink::kLost)) {
      // Screen on, off or AOD, or a new thermal limit: act on it now rather than at the next poll.
      engine.recheck_power();
      last_tick = monotonic_ms() - engine.poll_ms();
    }
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
  if (argc == 3 && std::strcmp(argv[1], "--state") == 0) return run(argv[2], "");
  if (argc == 5 && std::strcmp(argv[1], "--state") == 0 && std::strcmp(argv[3], "--module-prop") == 0) {
    return run(argv[2], argv[4]);
  }
  std::fprintf(stderr, "usage: %s --state DIR [--module-prop FILE] | --validate-config | --version\n", argv[0]);
  return 2;
}
