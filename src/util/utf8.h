/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Header-only UTF-8 primitives shared across the codebase. Before this header
 * existed, four separate translation units (mozc_client, mozc_integration,
 * refiner, yomi_extract) each carried their own near-identical copy of the
 * lead-byte length / codepoint iteration logic. Consolidating them here keeps
 * the byte-fiddling in one tested place.
 *
 * All functions are inline + header-only so no new .cpp has to be threaded
 * into the various CMake targets (skk.so, the CLI, the test binaries) — every
 * consumer just includes this with a relative path.
 */

#ifndef FCITX5_SKK_MOZC_UTIL_UTF8_H_
#define FCITX5_SKK_MOZC_UTIL_UTF8_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace skk_mozc::utf8 {

// Number of bytes the UTF-8 sequence starting with lead byte `c` occupies
// (1..4). A malformed / continuation lead byte is reported as 1 so callers
// always make forward progress.
inline size_t leadByteLen(unsigned char c) {
    if ((c & 0x80) == 0)         return 1;
    if ((c & 0xE0) == 0xC0)      return 2;
    if ((c & 0xF0) == 0xE0)      return 3;
    if ((c & 0xF8) == 0xF0)      return 4;
    return 1;
}

// Count the number of UTF-8 characters (codepoints) in `s`. A truncated
// trailing sequence still counts as one character — matching the historical
// behaviour of the callers this replaced.
inline int countChars(std::string_view s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) {
        i += leadByteLen(static_cast<unsigned char>(s[i]));
        ++n;
    }
    return n;
}

// Invoke `f(std::string_view)` for each whole UTF-8 character in `s`. A
// truncated trailing sequence (lead byte promises more bytes than remain) is
// dropped rather than passed through.
template <typename F>
inline void forEachChar(std::string_view s, F &&f) {
    for (size_t i = 0; i < s.size();) {
        size_t step = leadByteLen(static_cast<unsigned char>(s[i]));
        if (i + step > s.size()) break;
        f(s.substr(i, step));
        i += step;
    }
}

// Decode the codepoint starting at s[i], advancing `i` past it. Returns 0 and
// advances by a single byte on a malformed lead byte; returns 0 and jumps `i`
// to the end on a truncated multibyte sequence. This lets a caller pass the
// offending byte through verbatim without looping forever.
inline uint32_t decode(std::string_view s, size_t &i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp;
    size_t len;
    if ((c & 0x80) == 0)         { cp = c;        len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { i += 1; return 0; }
    if (i + len > s.size()) { i = s.size(); return 0; }
    for (size_t k = 1; k < len; ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    i += len;
    return cp;
}

// Append the UTF-8 encoding of codepoint `cp` to `out`.
inline void encode(uint32_t cp, std::string &out) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

} // namespace skk_mozc::utf8

#endif // FCITX5_SKK_MOZC_UTIL_UTF8_H_
