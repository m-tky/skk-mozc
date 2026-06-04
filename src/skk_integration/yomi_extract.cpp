/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "yomi_extract.h"

#include <array>
#include <cstring>
#include <libskk/libskk.h>

namespace skk_mozc {

std::string libskkCurrentYomi(SkkContext *ctx) {
    if (!ctx) return {};
    const gchar *preedit = skk_context_get_preedit(ctx);
    if (!preedit) return {};
    static const std::string kTriangleDown = "\xe2\x96\xbd"; // ▽ U+25BD
    std::string s = preedit;
    if (s.rfind(kTriangleDown, 0) != 0) return {};
    s.erase(0, kTriangleDown.size());
    // Defensive: strip any stray SKK markers that occasionally leak through
    // when libskk's preedit assembly is mid-transition.
    static const std::array<std::string, 3> kStrayMarkers = {
        "\xe2\x96\xbc", // ▼
        "\xe2\x96\xb3", // △
        "\xe2\x96\xb2", // ▲
    };
    for (const auto &m : kStrayMarkers) {
        size_t pos;
        while ((pos = s.find(m)) != std::string::npos) {
            s.erase(pos, m.size());
        }
    }
    return s;
}

} // namespace skk_mozc
