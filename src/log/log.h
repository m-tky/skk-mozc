/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tiny logger used when SKK_MOZC_DEBUG=1.
 *
 * Output:
 *   $XDG_CACHE_HOME/skk-mozc/log  (default: ~/.cache/skk-mozc/log)
 *
 * Each line is prefixed with an ISO timestamp + pid. The log file is opened
 * lazily on first call, kept open for the process lifetime, and uses
 * line-buffered writes so a `tail -f` on the file reflects activity
 * immediately.
 */

#ifndef FCITX5_SKK_MOZC_LOG_H_
#define FCITX5_SKK_MOZC_LOG_H_

#include <string>

namespace skk_mozc::log {

// Turn debug logging on/off. Idempotent. Until enabled, all `info()` calls
// are no-ops.
void setEnabled(bool on);
bool enabled();

// Append a line. Newline is added automatically.
void info(const std::string &msg);

// printf-style convenience.
void infof(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace skk_mozc::log

// SKK_MOZC_LOG("...") expands to a no-op when logging is disabled, so the
// formatting cost is paid only when SKK_MOZC_DEBUG=1.
#define SKK_MOZC_LOG(...)                                                       \
    do {                                                                        \
        if (::skk_mozc::log::enabled()) {                                       \
            ::skk_mozc::log::infof(__VA_ARGS__);                                \
        }                                                                       \
    } while (0)

#endif // FCITX5_SKK_MOZC_LOG_H_
