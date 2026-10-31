// SPDX-License-Identifier: GPL-3.0-only
// The daemon's end of the Zygisk reporter, fed through socketpairs the way
// zygisk/java/iab/Reporter.java writes.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

#include "check.hpp"
#include "system_link.hpp"

using namespace iab;

namespace {

std::string message(const std::string& type, const std::string& payload) {
  return type + " " + std::to_string(payload.size()) + "\n" + payload;
}

void put(int fd, const std::string& data) {
  CHECK_EQ(write(fd, data.data(), data.size()), static_cast<ssize_t>(data.size()));
}

struct Pair {
  SystemLink link{[](int) { return true; }};
  int peer = -1;
  Pair() {
    int fds[2];
    CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    link.attach(fds[0]);
    peer = fds[1];
  }
  ~Pair() {
    if (peer >= 0) close(peer);
  }
};

const std::string kAnchors =
    "version=2\ndefaults=0.0 2.0 30.0 135.0 0.0 600.0 220.0 5000.0 1060.0\norder=2\nuser=0.0 8.783608\n"
    "good_curve=0\nanchor=0.0 8.783608 2 1\nanchor=30.0 135.0 0 0\nanchor=600.0 220.0 0 0\nanchor=5000.0 1060.0 0 0\n";

void reports_state_and_anchors() {
  Pair pair;
  CHECK(!pair.link.connected());
  put(pair.peer, message("hello", "1") + message("state", "4") + message("anchors", kAnchors));
  const unsigned events = pair.link.service();
  CHECK(events & SystemLink::kConnected);
  CHECK(events & SystemLink::kState);
  CHECK(events & SystemLink::kAnchors);
  CHECK(pair.link.connected());
  CHECK_EQ(pair.link.display_state(), 4);
  CHECK(pair.link.anchors() == kAnchors);
  // Nothing new, nothing reported.
  CHECK_EQ(pair.link.service(), 0u);
  put(pair.peer, message("state", "4") + message("anchors", kAnchors));
  CHECK_EQ(pair.link.service(), 0u);
  put(pair.peer, message("state", "2"));
  CHECK_EQ(pair.link.service(), static_cast<unsigned>(SystemLink::kState));
  CHECK_EQ(pair.link.display_state(), 2);
  // Unknown types are skipped for later protocol additions.
  put(pair.peer, message("lux", "12.5") + message("state", "1"));
  CHECK_EQ(pair.link.service(), static_cast<unsigned>(SystemLink::kState));
  CHECK_EQ(pair.link.display_state(), 1);
}

void reports_thermal_limit_and_scene() {
  Pair pair;
  put(pair.peer, message("hello", "1"));
  pair.link.service();
  CHECK(std::isnan(pair.link.limit()));
  CHECK_EQ(pair.link.scene(), -1);
  put(pair.peer, message("limit", "0.37485194") + message("scene", "3"));
  const unsigned events = pair.link.service();
  CHECK(events & SystemLink::kLimit);
  CHECK(events & SystemLink::kScene);
  CHECK_NEAR(pair.link.limit(), 0.37485194, 1e-7);
  CHECK_EQ(pair.link.scene(), 3);
  put(pair.peer, message("limit", "0.37485194") + message("scene", "3"));
  CHECK_EQ(pair.link.service(), 0u);
  put(pair.peer, message("limit", "1.0") + message("scene", "0"));
  CHECK_EQ(pair.link.service(), static_cast<unsigned>(SystemLink::kLimit | SystemLink::kScene));
  CHECK_NEAR(pair.link.limit(), 1.0, 1e-9);
  CHECK_EQ(pair.link.scene(), 0);
  close(pair.peer);
  pair.peer = -1;
  pair.link.service();
  CHECK(std::isnan(pair.link.limit()));
  CHECK_EQ(pair.link.scene(), -1);
  for (const std::string& bad : {message("limit", "1.5"), message("limit", "x"), message("scene", "-2")}) {
    Pair other;
    put(other.peer, message("hello", "1") + bad);
    CHECK(other.link.service() & SystemLink::kRejected);
  }
}

void reassembles_split_messages() {
  Pair pair;
  const std::string data = message("hello", "1") + message("anchors", kAnchors);
  for (size_t at = 0; at < data.size(); at += 7) {
    put(pair.peer, data.substr(at, 7));
    pair.link.service();
  }
  CHECK(pair.link.connected());
  CHECK(pair.link.anchors() == kAnchors);
}

void rejects_bad_peers() {
  for (const std::string& data :
       {message("hello", "2"), message("state", "2"), std::string("hello x\n1"), std::string(100, 'x'),
        message("hello", "1") + "anchors " + std::to_string(SystemLink::kMaxPayload + 1) + "\n"}) {
    Pair pair;
    put(pair.peer, data);
    CHECK(pair.link.service() & SystemLink::kRejected);
    CHECK(!pair.link.connected());
    CHECK(pair.link.fds().empty());
  }
  // A header still on its way is no error.
  Pair partial;
  put(partial.peer, "hello 1");
  CHECK_EQ(partial.link.service(), 0u);
  CHECK_EQ(partial.link.fds().size(), 1u);
}

void loses_and_resets() {
  Pair pair;
  put(pair.peer, message("hello", "1") + message("state", "2") + message("anchors", kAnchors));
  pair.link.service();
  CHECK(pair.link.send_reset());
  char buffer[16] = {};
  CHECK_EQ(read(pair.peer, buffer, sizeof buffer), 6);
  CHECK(std::strcmp(buffer, "reset\n") == 0);
  close(pair.peer);
  pair.peer = -1;
  CHECK(pair.link.service() & SystemLink::kLost);
  CHECK(!pair.link.connected());
  CHECK_EQ(pair.link.display_state(), SystemLink::kUnknown);
  CHECK(pair.link.anchors().empty());
  CHECK(!pair.link.send_reset());
}

#ifdef __linux__
int connect_to(const std::string& name) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path + 1, name.data(), name.size());
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
  CHECK_EQ(connect(fd, reinterpret_cast<const sockaddr*>(&address), length), 0);
  return fd;
}

void listens_on_an_abstract_socket() {
  const std::string name = "iab_brightness_test_" + std::to_string(getpid());
  bool allow = false;
  SystemLink link([&](int) { return allow; });
  CHECK(link.listen(name));
  CHECK(!SystemLink([](int) { return true; }).listen(name));  // taken
  const int refused = connect_to(name);
  CHECK(link.service() & SystemLink::kRejected);
  close(refused);
  allow = true;
  const int first = connect_to(name);
  put(first, message("hello", "1"));
  CHECK(link.service() & SystemLink::kConnected);
  // A restarted system_server replaces the old connection.
  const int second = connect_to(name);
  put(second, message("hello", "1") + message("state", "3"));
  const unsigned events = link.service();
  CHECK(events & SystemLink::kLost);
  CHECK(events & SystemLink::kConnected);
  CHECK_EQ(link.display_state(), 3);
  CHECK_EQ(link.fds().size(), 2u);
  close(first);
  close(second);
}
#endif

}  // namespace

int main() {
  reports_state_and_anchors();
  reports_thermal_limit_and_scene();
  reassembles_split_messages();
  rejects_bad_peers();
  loses_and_resets();
#ifdef __linux__
  listens_on_an_abstract_socket();
#endif
  return finish("system_link_tests");
}
