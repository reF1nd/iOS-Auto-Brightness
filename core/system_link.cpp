// SPDX-License-Identifier: GPL-3.0-only
#include "system_link.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <utility>

// macOS host builds have no MSG_NOSIGNAL; there SIGPIPE is not a concern for tests.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace iab {

namespace {

bool nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

}  // namespace

SystemLink::SystemLink(PeerCheck check) : check_(std::move(check)) {}

SystemLink::~SystemLink() {
  if (peer_fd_ >= 0) close(peer_fd_);
  if (listen_fd_ >= 0) close(listen_fd_);
}

bool SystemLink::listen(const std::string& name) {
  sockaddr_un address{};
  if (name.empty() || name.size() + 1 > sizeof address.sun_path) return false;
  address.sun_family = AF_UNIX;
  // Abstract namespace: a leading NUL, no file, gone with the process.
  std::memcpy(address.sun_path + 1, name.data(), name.size());
  const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  if (!nonblocking(fd) || bind(fd, reinterpret_cast<const sockaddr*>(&address), length) != 0 || ::listen(fd, 2) != 0) {
    close(fd);
    return false;
  }
  listen_fd_ = fd;
  return true;
}

void SystemLink::attach(int fd) {
  drop(0);
  if (nonblocking(fd)) {
    peer_fd_ = fd;
  } else {
    close(fd);
  }
}

std::vector<int> SystemLink::fds() const {
  std::vector<int> out;
  if (listen_fd_ >= 0) out.push_back(listen_fd_);
  if (peer_fd_ >= 0) out.push_back(peer_fd_);
  return out;
}

unsigned SystemLink::drop(unsigned events) {
  if (peer_fd_ >= 0) {
    close(peer_fd_);
    peer_fd_ = -1;
    if (hello_) events |= kLost;
  }
  hello_ = false;
  state_ = kUnknown;
  buffer_.clear();
  anchors_.clear();
  return events;
}

unsigned SystemLink::service() {
  unsigned events = 0;
  if (listen_fd_ >= 0) {
    // A restarted system_server connects again; the newest peer wins.
    for (int fd; (fd = accept(listen_fd_, nullptr, nullptr)) >= 0;) {
      if (!check_ || !check_(fd)) {
        close(fd);
        events |= kRejected;
        continue;
      }
      events = drop(events);
      attach(fd);
    }
  }
  if (peer_fd_ >= 0) events |= read_peer();
  return events;
}

unsigned SystemLink::read_peer() {
  char chunk[4096];
  while (true) {
    const ssize_t n = read(peer_fd_, chunk, sizeof chunk);
    if (n > 0) {
      buffer_.append(chunk, static_cast<size_t>(n));
      if (buffer_.size() > kMaxPayload * 4) return drop(kRejected);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return parse();
    // Closed by the peer (or broken): keep what was complete.
    const unsigned events = parse();
    return drop(events);
  }
}

unsigned SystemLink::parse() {
  unsigned events = 0;
  while (true) {
    const size_t end = buffer_.find('\n');
    if (end == std::string::npos) {
      return buffer_.size() > 64 ? drop(events | kRejected) : events;
    }
    const size_t space = buffer_.find(' ');
    if (space == std::string::npos || space > end || space == 0) return drop(events | kRejected);
    const std::string type = buffer_.substr(0, space);
    char* tail = nullptr;
    const unsigned long length = std::strtoul(buffer_.c_str() + space + 1, &tail, 10);
    if (tail != buffer_.c_str() + end || length > kMaxPayload) return drop(events | kRejected);
    if (buffer_.size() < end + 1 + length) return events;
    const std::string payload = buffer_.substr(end + 1, length);
    buffer_.erase(0, end + 1 + length);
    if (type == "hello") {
      if (std::atoi(payload.c_str()) != kVersion) return drop(events | kRejected);
      hello_ = true;
      events |= kConnected;
    } else if (!hello_) {
      return drop(events | kRejected);
    } else if (type == "state") {
      const int state = std::atoi(payload.c_str());
      if (state != state_) state_ = state, events |= kState;
    } else if (type == "anchors") {
      if (payload != anchors_) anchors_ = payload, events |= kAnchors;
    }
    // Other types are left for later protocol additions.
  }
}

bool SystemLink::send_reset() {
  if (!hello_) return false;
  static constexpr char kReset[] = "reset\n";
  return send(peer_fd_, kReset, sizeof kReset - 1, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof kReset - 1);
}

}  // namespace iab
