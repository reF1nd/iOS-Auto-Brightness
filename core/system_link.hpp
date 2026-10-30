// SPDX-License-Identifier: GPL-3.0-only
// Receiving end of zygisk/java/iab/Reporter.java. system_server connects to
// the abstract socket "iab_brightness" and sends "<type> <length>\n<payload>"
// messages: hello (protocol version), state (Display.STATE_* of display 0)
// and anchors (RefactorNitController in learn.conf form). The daemon only
// sends "reset\n". Without a peer the daemon falls back to dumpsys and
// bl_power.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace iab {

class SystemLink {
 public:
  static constexpr int kVersion = 1;
  static constexpr size_t kMaxPayload = 8192;
  // Display.STATE_UNKNOWN; also used before the first report.
  static constexpr int kUnknown = 0;

  enum Event : unsigned {
    kConnected = 1u << 0,
    kLost = 1u << 1,
    kState = 1u << 2,
    kAnchors = 1u << 3,
    kRejected = 1u << 4,  // wrong peer, protocol version or framing
  };

  // Decides whether the process behind an accepted socket may report.
  using PeerCheck = std::function<bool(int fd)>;
  explicit SystemLink(PeerCheck check);
  ~SystemLink();
  SystemLink(const SystemLink&) = delete;
  SystemLink& operator=(const SystemLink&) = delete;

  bool listen(const std::string& name);
  // Takes a socket that is already connected (tests use a socketpair).
  void attach(int fd);
  // Accepts and reads whatever is pending without blocking; returns Events.
  unsigned service();
  // Descriptors to wait on for readability.
  std::vector<int> fds() const;

  // A peer said hello with kVersion.
  bool connected() const { return hello_; }
  int display_state() const { return state_; }
  const std::string& anchors() const { return anchors_; }
  bool send_reset();

 private:
  unsigned drop(unsigned events);
  unsigned read_peer();
  unsigned parse();

  PeerCheck check_;
  int listen_fd_ = -1, peer_fd_ = -1;
  bool hello_ = false;
  int state_ = kUnknown;
  std::string buffer_, anchors_;
};

}  // namespace iab
