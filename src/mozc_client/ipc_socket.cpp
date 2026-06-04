/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ipc_socket.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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

std::string xdgRuntimeDir() {
    if (const char *r = std::getenv("XDG_RUNTIME_DIR"); r && *r) {
        return r;
    }
    return "/run/user/" + std::to_string(::getuid());
}

std::string xdgConfigHome() {
    if (const char *r = std::getenv("XDG_CONFIG_HOME"); r && *r) {
        return r;
    }
    if (const char *h = std::getenv("HOME"); h && *h) {
        return std::string(h) + "/.config";
    }
    return "/tmp";
}

int connectWithTimeout(const std::string &path,
                       std::chrono::milliseconds budget) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return -1;
    }
    std::memcpy(addr.sun_path, path.data(), path.size());

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
        return fd;
    }
    if (errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    pollfd pfd{fd, POLLOUT, 0};
    int n = ::poll(&pfd, 1, static_cast<int>(budget.count()));
    if (n <= 0) {
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
              std::chrono::milliseconds budget,
              const clock_t::time_point &deadline) {
    while (len > 0) {
        auto now = clock_t::now();
        if (now >= deadline) {
            return false;
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        pollfd pfd{fd, POLLOUT, 0};
        int n = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (n <= 0) {
            return false;
        }
        ssize_t w = ::send(fd, data, len, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return false;
        }
        data += w;
        len -= static_cast<size_t>(w);
    }
    (void)budget;
    return true;
}

bool readAll(int fd, uint8_t *data, size_t len,
             const clock_t::time_point &deadline) {
    while (len > 0) {
        auto now = clock_t::now();
        if (now >= deadline) {
            return false;
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        pollfd pfd{fd, POLLIN, 0};
        int n = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (n <= 0) {
            return false;
        }
        ssize_t r = ::recv(fd, data, len, 0);
        if (r == 0) {
            return false; // EOF before complete
        }
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return false;
        }
        data += r;
        len -= static_cast<size_t>(r);
    }
    return true;
}

} // namespace

std::string resolveSocketPath(const std::string &override_path) {
    if (!override_path.empty()) {
        return override_path;
    }
    uid_t uid = ::getuid();
    // Order matters: the first path that exists wins, falling back to the
    // canonical one if nothing's there yet (so a lazy server start can create
    // it). The XDG_CONFIG_HOME variant is what current mozc actually uses; the
    // /tmp + XDG_RUNTIME_DIR forms are kept for older mozc builds.
    std::array<std::string, 3> candidates = {
        xdgConfigHome() + "/mozc/.session.ipc",
        xdgRuntimeDir() + "/.mozc." + std::to_string(uid) + ".session",
        "/tmp/.mozc." + std::to_string(uid) + ".session",
    };
    for (const auto &c : candidates) {
        if (fileExists(c)) {
            return c;
        }
    }
    return candidates.front();
}

std::optional<std::vector<uint8_t>>
sendRequest(const std::string &socket_path,
            const std::vector<uint8_t> &payload,
            std::chrono::milliseconds timeout) {
    auto deadline = clock_t::now() + timeout;

    int fd = connectWithTimeout(socket_path, timeout);
    if (fd < 0) {
        return std::nullopt;
    }

    struct CloseOnExit {
        int fd;
        ~CloseOnExit() { if (fd >= 0) ::close(fd); }
    } guard{fd};

    // Frame: 4-byte LE length + payload.
    uint32_t size = static_cast<uint32_t>(payload.size());
    std::array<uint8_t, 4> header = {
        static_cast<uint8_t>(size & 0xff),
        static_cast<uint8_t>((size >> 8) & 0xff),
        static_cast<uint8_t>((size >> 16) & 0xff),
        static_cast<uint8_t>((size >> 24) & 0xff),
    };
    if (!writeAll(fd, header.data(), header.size(), timeout, deadline)) {
        return std::nullopt;
    }
    if (!writeAll(fd, payload.data(), payload.size(), timeout, deadline)) {
        return std::nullopt;
    }

    std::array<uint8_t, 4> rsp_header{};
    if (!readAll(fd, rsp_header.data(), rsp_header.size(), deadline)) {
        return std::nullopt;
    }
    uint32_t rsp_len = static_cast<uint32_t>(rsp_header[0]) |
                       (static_cast<uint32_t>(rsp_header[1]) << 8) |
                       (static_cast<uint32_t>(rsp_header[2]) << 16) |
                       (static_cast<uint32_t>(rsp_header[3]) << 24);
    // Sanity: refuse absurd allocations.
    if (rsp_len > (16u * 1024u * 1024u)) {
        return std::nullopt;
    }
    std::vector<uint8_t> body(rsp_len);
    if (rsp_len > 0 && !readAll(fd, body.data(), body.size(), deadline)) {
        return std::nullopt;
    }
    return body;
}

bool spawnServer(const std::string &mozc_server_path) {
    if (mozc_server_path.empty()) {
        return false;
    }
    pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        // Child: detach from parent, exec server.
        ::setsid();
        // Close stdio; mozc_server will reopen its own logging.
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, 0);
            ::dup2(devnull, 1);
            ::dup2(devnull, 2);
            if (devnull > 2) ::close(devnull);
        }
        const char *argv[] = {
            mozc_server_path.c_str(),
            "--mode=server",
            nullptr,
        };
        ::execv(mozc_server_path.c_str(), const_cast<char *const *>(argv));
        ::_exit(127);
    }
    // Parent: let it run detached. We don't wait; reap via SIGCHLD/SA_NOCLDWAIT
    // by relying on the child setsid'ing into its own process group.
    return true;
}

} // namespace skk_mozc::ipc
