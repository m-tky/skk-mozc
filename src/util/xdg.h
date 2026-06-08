/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Header-only XDG base-directory resolution. log.cpp (cache) and
 * ipc_socket.cpp (config) each used to carry the same three-line
 * "env var → $HOME/<sub> → /tmp" fallback; this folds them into one helper.
 */

#ifndef FCITX5_SKK_MOZC_UTIL_XDG_H_
#define FCITX5_SKK_MOZC_UTIL_XDG_H_

#include <cstdlib>
#include <string>

namespace skk_mozc::util {

// Resolve an XDG base directory. Returns $`envVar` if set and non-empty,
// otherwise $HOME + `homeSubdir` (e.g. "/.cache"), otherwise "/tmp".
inline std::string xdgDir(const char *envVar, const char *homeSubdir) {
    if (const char *r = std::getenv(envVar); r && *r) return r;
    if (const char *h = std::getenv("HOME"); h && *h) {
        return std::string(h) + homeSubdir;
    }
    return "/tmp";
}

} // namespace skk_mozc::util

#endif // FCITX5_SKK_MOZC_UTIL_XDG_H_
