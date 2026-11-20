// SPDX-License-Identifier: GPL-3.0-only
// Zygisk part of the module. HyperOS forgets its brightness anchors on reboot;
// this hands learn-touch.conf and anchors.dex to system_server, where
// iab.AnchorRestore writes the anchors back into RefactorNitController and
// then reports the display state and anchors to the daemon. SystemUI and Settings observe their brightness sliders;
// all processes unload the native library after specialization. The companion runs as root: it reads the files
// system_server may not, and stops handing them over when system_server keeps
// restarting, in case the restore is what crashes it.
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "zygisk.hpp"

namespace {

constexpr const char* kTag = "iOS_Bright";
constexpr const char* kState = "/data/adb/ios_auto_brightness";
constexpr const char* kDex = "/data/adb/modules/ios_auto_brightness/anchors.dex";
constexpr int kMaxQuickStarts = 2;
constexpr long long kQuickSeconds = 120;
constexpr size_t kMaxLearn = 16384;
constexpr size_t kMaxDex = 1 << 20;

bool read_file(const std::string& path, std::string& out, size_t limit) {
  out.clear();
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  char buffer[4096];
  ssize_t n;
  while ((n = read(fd, buffer, sizeof buffer)) > 0 && out.size() + static_cast<size_t>(n) <= limit) {
    out.append(buffer, static_cast<size_t>(n));
  }
  close(fd);
  return n == 0;
}

bool write_file(const std::string& path, const std::string& data) {
  const std::string temporary = path + ".tmp";
  const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  const bool ok = write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()) && fsync(fd) == 0;
  close(fd);
  return ok && rename(temporary.c_str(), path.c_str()) == 0;
}

// The other end may be gone; MSG_NOSIGNAL keeps SIGPIPE from killing the companion.
bool send_all(int fd, const void* data, size_t size) {
  const auto* bytes = static_cast<const char*>(data);
  while (size > 0) {
    const ssize_t n = send(fd, bytes, size, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    bytes += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

bool receive_all(int fd, void* data, size_t size) {
  auto* bytes = static_cast<char*>(data);
  while (size > 0) {
    const ssize_t n = read(fd, bytes, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    bytes += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

bool send_blob(int fd, const std::string& data) {
  const uint32_t size = static_cast<uint32_t>(data.size());
  return send_all(fd, &size, sizeof size) && send_all(fd, data.data(), size);
}

bool receive_blob(int fd, std::string& data, size_t limit) {
  uint32_t size = 0;
  if (!receive_all(fd, &size, sizeof size) || size > limit) return false;
  data.resize(size);
  return receive_all(fd, data.data(), size);
}

// zygisk.starts holds "<boot id> <seconds since boot> <count>" of the last
// system_server start; count is how many starts in a row came within
// kQuickSeconds of the one before. A third quick start, as in a crash loop,
// leaves zygisk.disabled behind; installing the module again clears both.
bool allowed_this_start() {
  const std::string state = kState, disabled = state + "/zygisk.disabled", starts = state + "/zygisk.starts";
  struct stat info {};
  if (lstat(disabled.c_str(), &info) == 0) return false;
  std::string boot, saved;
  if (!read_file("/proc/sys/kernel/random/boot_id", boot, 64) || boot.empty()) return false;
  while (!boot.empty() && boot.back() == '\n') boot.pop_back();
  timespec now {};
  clock_gettime(CLOCK_BOOTTIME, &now);
  long long last = -1;
  int count = 0;
  if (read_file(starts, saved, 160)) {
    char id[64] = {};
    if (std::sscanf(saved.c_str(), "%63s %lld %d", id, &last, &count) != 3 || boot != id) count = 0;
  }
  count = count > 0 && now.tv_sec >= last && now.tv_sec - last < kQuickSeconds ? count + 1 : 1;
  if (!write_file(starts, boot + " " + std::to_string(now.tv_sec) + " " + std::to_string(count) + "\n")) return false;
  if (count <= kMaxQuickStarts) return true;
  write_file(disabled, "system_server restarted " + std::to_string(count) + " times within " +
                           std::to_string(kQuickSeconds) + " s of each other\n");
  return false;
}

// Only system_server needs the learned curve and counts toward its boot guard.
// App processes request just the dex before specialization.
void companion(int fd) {
  char request;
  if (!receive_all(fd, &request, sizeof request)) return;
  std::string learn, dex;
  if (request == 'U') {
    if (access((std::string(kState) + "/zygisk.disabled").c_str(), F_OK) != 0 && !read_file(kDex, dex, kMaxDex)) {
      dex.clear();
    }
    send_blob(fd, dex);
    return;
  }
  if (request != 'S') return;
  if (!allowed_this_start() || !read_file(kDex, dex, kMaxDex)) dex.clear();
  if (dex.empty() || !read_file(std::string(kState) + "/learn-touch.conf", learn, kMaxLearn) ||
      learn.compare(0, 10, "version=2\n") != 0) {
    learn.clear();
  }
  if (send_blob(fd, learn)) send_blob(fd, dex);
}

// learn-touch.conf is passed as a Java string; modified UTF-8 is only safe for ASCII.
bool printable(const std::string& text) {
  for (const char c : text) {
    if (c != '\n' && (c < 0x20 || c > 0x7e)) return false;
  }
  return true;
}

bool failed(JNIEnv* env, const char* step) {
  if (!env->ExceptionCheck()) return false;
  env->ExceptionClear();
  __android_log_print(ANDROID_LOG_WARN, kTag, "System anchors: %s failed", step);
  return true;
}

void load(JNIEnv* env, const char* class_name, const std::string& argument, const std::string& dex) {
  if (env->PushLocalFrame(16) != 0) {
    env->ExceptionClear();
    return;
  }
  jbyteArray bytes = env->NewByteArray(static_cast<jsize>(dex.size()));
  if (!failed(env, "dex copy") && bytes) {
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(dex.size()), reinterpret_cast<const jbyte*>(dex.data()));
    jclass buffers = env->FindClass("java/nio/ByteBuffer");
    jclass loaders = env->FindClass("java/lang/ClassLoader");
    jclass memory = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (!failed(env, "class lookup") && buffers && loaders && memory) {
      jmethodID wrap = env->GetStaticMethodID(buffers, "wrap", "([B)Ljava/nio/ByteBuffer;");
      jmethodID system = env->GetStaticMethodID(loaders, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
      jmethodID create = env->GetMethodID(memory, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
      jmethodID find = env->GetMethodID(loaders, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
      if (!failed(env, "method lookup") && wrap && system && create && find) {
        jobject buffer = env->CallStaticObjectMethod(buffers, wrap, bytes);
        jobject parent = failed(env, "ByteBuffer.wrap") ? nullptr : env->CallStaticObjectMethod(loaders, system);
        jobject loader = failed(env, "system class loader") || !parent ? nullptr
                                                                        : env->NewObject(memory, create, buffer, parent);
        jobject found = failed(env, "dex load") || !loader
                            ? nullptr
                            : env->CallObjectMethod(loader, find, env->NewStringUTF(class_name));
        auto* restore = static_cast<jclass>(found);
        jmethodID start = failed(env, "class load") || !restore
                              ? nullptr
                              : env->GetStaticMethodID(restore, "start", "(Ljava/lang/String;)V");
        if (!failed(env, "start lookup") && start) {
          env->CallStaticVoidMethod(restore, start, env->NewStringUTF(argument.c_str()));
          failed(env, "start");
        }
      }
    }
  }
  env->PopLocalFrame(nullptr);
}

class Module : public zygisk::ModuleBase {
 public:
  void onLoad(zygisk::Api* api, JNIEnv* env) override {
    api_ = api;
    env_ = env;
  }

  void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
    api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    if (!args->nice_name || args->uid >= 100000) return;  // Only the primary Android user.
    const char* name = env_->GetStringUTFChars(args->nice_name, nullptr);
    if (!name) { env_->ExceptionClear(); return; }
    process_ = name;
    env_->ReleaseStringUTFChars(args->nice_name, name);
    if (process_ != "com.android.systemui" && process_ != "com.android.settings") return;
    const int fd = api_->connectCompanion();
    if (fd < 0) return;
    if (!send_all(fd, "U", 1) || !receive_blob(fd, dex_, kMaxDex)) dex_.clear();
    close(fd);
  }

  void postAppSpecialize(const zygisk::AppSpecializeArgs*) override {
    if (!dex_.empty()) load(env_, "iab.SliderObserver", process_, dex_);
    std::string().swap(dex_);
  }

  void preServerSpecialize(zygisk::ServerSpecializeArgs*) override {
    // Only the dex runs after specialization; the library can go.
    api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    const int fd = api_->connectCompanion();
    if (fd < 0) return;
    if (!send_all(fd, "S", 1) || !receive_blob(fd, learn_, kMaxLearn) || !receive_blob(fd, dex_, kMaxDex) || !printable(learn_)) {
      learn_.clear(), dex_.clear();
    }
    close(fd);
  }

  void postServerSpecialize(const zygisk::ServerSpecializeArgs*) override {
    if (!dex_.empty()) load(env_, "iab.AnchorRestore", learn_, dex_);
    std::string().swap(learn_);
    std::string().swap(dex_);
  }

 private:
  zygisk::Api* api_ = nullptr;
  JNIEnv* env_ = nullptr;
  std::string learn_, dex_, process_;
};

}  // namespace

REGISTER_ZYGISK_MODULE(Module)
REGISTER_ZYGISK_COMPANION(companion)
