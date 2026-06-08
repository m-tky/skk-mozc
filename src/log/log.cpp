/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "log.h"

#include "../util/xdg.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <unistd.h>

namespace skk_mozc::log {

namespace {

namespace fs = std::filesystem;

std::atomic<bool> g_enabled{false};
std::mutex g_mu;
FILE *g_fp = nullptr;

FILE *ensureOpen() {
    if (g_fp) return g_fp;
    std::string dir = util::xdgDir("XDG_CACHE_HOME", "/.cache") + "/skk-mozc";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string path = dir + "/log";
    g_fp = std::fopen(path.c_str(), "ae"); // append + close-on-exec
    if (g_fp) {
        std::setvbuf(g_fp, nullptr, _IOLBF, 0); // line-buffered
    } else {
        // Last-ditch fallback so we don't lose visibility entirely.
        g_fp = stderr;
    }
    return g_fp;
}

std::string timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    char buf[32];
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[40];
    std::snprintf(out, sizeof(out), "%s.%03lld", buf, static_cast<long long>(ms));
    return out;
}

} // namespace

void setEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void info(const std::string &msg) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(g_mu);
    FILE *fp = ensureOpen();
    if (!fp) return;
    std::fprintf(fp, "%s [%d] %s\n", timestamp().c_str(), ::getpid(),
                 msg.c_str());
}

void infof(const char *fmt, ...) {
    if (!enabled()) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    info(buf);
}

} // namespace skk_mozc::log
