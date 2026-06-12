/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ipc_socket.h"
#include "../util/xdg.h"
#include "ipc/ipc.pb.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace skk_mozc::ipc {

namespace {

namespace fs = std::filesystem;
using clock_t = std::chrono::steady_clock;

bool fileExists(const std::string &path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

// Construct the abstract UNIX socket address mozc_server listens on, given
// the key string from IPCPathInfo. Format: "\0tmp/.mozc.<key>.session".
std::vector<uint8_t> buildAbstractAddress(const std::string &key) {
    static const char kPrefix[] = "/tmp/.mozc.";
    static const char kSuffix[] = ".session";
    std::vector<uint8_t> out;
    out.reserve(sizeof(kPrefix) - 1 + key.size() + sizeof(kSuffix) - 1);
    out.insert(out.end(), kPrefix, kPrefix + sizeof(kPrefix) - 1);
    out.insert(out.end(), key.begin(), key.end());
    out.insert(out.end(), kSuffix, kSuffix + sizeof(kSuffix) - 1);
    out[0] = 0; // turn into abstract socket
    return out;
}

int connectWithTimeout(const std::vector<uint8_t> &addr,
                       std::chrono::milliseconds budget) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;

    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (addr.size() > sizeof(sa.sun_path)) {
        ::close(fd);
        return -1;
    }
    std::memcpy(sa.sun_path, addr.data(), addr.size());
    // For abstract sockets we MUST use exact byte-length (don't strlen, since
    // sun_path[0] == 0).
    socklen_t sun_len = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + addr.size());

    if (::connect(fd, reinterpret_cast<sockaddr *>(&sa), sun_len) == 0) {
        return fd;
    }
    if (errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    pollfd pfd{fd, POLLOUT, 0};
    if (::poll(&pfd, 1, static_cast<int>(budget.count())) <= 0) {
        ::close(fd);
        return -1;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool writeAll(int fd, const uint8_t *data, size_t len,
              const clock_t::time_point &deadline) {
    while (len > 0) {
        auto now = clock_t::now();
        if (now >= deadline) return false;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        pollfd pfd{fd, POLLOUT, 0};
        if (::poll(&pfd, 1, static_cast<int>(remaining.count())) <= 0) {
            return false;
        }
        ssize_t w = ::send(fd, data, len, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return false;
        }
        data += w;
        len -= static_cast<size_t>(w);
    }
    return true;
}

// Read until EOF (peer closes write half). Bounded by `deadline` and a
// generous max-size guard.
std::optional<std::vector<uint8_t>>
readUntilEof(int fd, const clock_t::time_point &deadline) {
    constexpr size_t kMaxResponse = 1u << 24; // 16 MiB
    std::vector<uint8_t> out;
    std::array<uint8_t, 4096> buf{};
    for (;;) {
        auto now = clock_t::now();
        if (now >= deadline) return std::nullopt;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        pollfd pfd{fd, POLLIN, 0};
        int n = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (n == 0) return std::nullopt; // timeout
        ssize_t r = ::recv(fd, buf.data(), buf.size(), 0);
        if (r == 0) return out; // EOF
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return std::nullopt;
        }
        if (out.size() + r > kMaxResponse) return std::nullopt;
        out.insert(out.end(), buf.data(), buf.data() + r);
    }
}

} // namespace

std::vector<uint8_t> resolveSocketAddress(const std::string &override_path) {
    std::string path = override_path.empty()
                           ? util::xdgDir("XDG_CONFIG_HOME", "/.config") +
                                 "/mozc/.session.ipc"
                           : override_path;
    if (!fileExists(path)) {
        return {};
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    std::string body = ss.str();

    mozc::ipc::IPCPathInfo info;
    if (!info.ParseFromString(body)) {
        return {};
    }
    if (info.key().empty()) {
        return {};
    }
    return buildAbstractAddress(info.key());
}

std::optional<std::vector<uint8_t>>
sendRequest(const std::vector<uint8_t> &socket_address,
            const std::vector<uint8_t> &payload,
            std::chrono::milliseconds timeout) {
    if (socket_address.empty()) return std::nullopt;
    auto deadline = clock_t::now() + timeout;

    int fd = connectWithTimeout(socket_address, timeout);
    if (fd < 0) return std::nullopt;
    struct CloseOnExit {
        int fd;
        ~CloseOnExit() { if (fd >= 0) ::close(fd); }
    } guard{fd};

    if (!writeAll(fd, payload.data(), payload.size(), deadline)) {
        return std::nullopt;
    }
    // Half-close so the server stops reading.
    ::shutdown(fd, SHUT_WR);
    return readUntilEof(fd, deadline);
}

bool spawnServer(const std::string &mozc_server_path) {
    std::string program =
        mozc_server_path.empty() ? std::string("mozc_server")
                                 : mozc_server_path;
    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::setsid();
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, 0);
            ::dup2(devnull, 1);
            ::dup2(devnull, 2);
            if (devnull > 2) ::close(devnull);
        }
        const char *argv[] = {
            program.c_str(),
            "--mode=server",
            nullptr,
        };
        ::execvp(program.c_str(), const_cast<char *const *>(argv));
        ::_exit(127);
    }
    return true;
}

} // namespace skk_mozc::ipc
