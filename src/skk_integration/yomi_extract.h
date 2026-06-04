/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pulled out of mozc_integration.cpp so it can be exercised against a real
 * libskk SkkContext from tests/libskk_integration_test.cpp without dragging
 * fcitx5 into that test target.
 */

#ifndef FCITX5_SKK_MOZC_YOMI_EXTRACT_H_
#define FCITX5_SKK_MOZC_YOMI_EXTRACT_H_

#include <string>

extern "C" {
typedef struct _SkkContext SkkContext;
}

namespace skk_mozc {

// Returns the live yomi the user is composing in libskk's ▽ mode, with the
// ▽ marker stripped. Returns an empty string in every other state — ▼
// (candidate showing), register mode, direct-input mode — because those
// states do not surface a usable mozc query string.
std::string libskkCurrentYomi(SkkContext *ctx);

} // namespace skk_mozc

#endif // FCITX5_SKK_MOZC_YOMI_EXTRACT_H_
