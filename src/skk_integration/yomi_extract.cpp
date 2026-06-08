/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "yomi_extract.h"

#include "../util/utf8.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <libskk/libskk.h>

namespace skk_mozc {

namespace {

// Normalise a katakana yomi to hiragana so mozc / the SKK dict (both
// hiragana-keyed) see the reading SKK itself would look up. SKK's katakana
// input mode (q toggle) shows the midashigo as full-width katakana, and the
// hankaku-katakana mode (C-q) shows half-width — but the underlying reading
// is hiragana. Without this, mozc treats e.g.「サシミ」as already-converted
// text and only offers full/half-width katakana display variants, and the SKK
// dict lookup misses entirely.
//
// Full-width katakana (U+30A1..U+30F6) maps to hiragana by subtracting 0x60.
// 「ー」(U+30FC) and 「・」(U+30FB) are left as-is (they read the same in
// hiragana context). ヷヸヹヺ (U+30F7..U+30FA) have no hiragana counterpart and
// fall outside the range, so they pass through. Half-width katakana
// (U+FF66..U+FF9F) is mapped via a table, folding a following dakuten /
// handakuten mark into the voiced / semi-voiced hiragana.
std::string katakanaToHiragana(const std::string &s) {
    // Index = codepoint - 0xFF66 (U+FF66 ヲ … U+FF9D ン). The two marks
    // U+FF9E (dakuten) and U+FF9F (handakuten) are handled separately below.
    static const uint32_t kHalfBase[] = {
        0x3092,                                 // ヲ を
        0x3041, 0x3043, 0x3045, 0x3047, 0x3049, // ァ ィ ゥ ェ ォ
        0x3083, 0x3085, 0x3087,                 // ャ ュ ョ
        0x3063,                                 // ッ
        0x30FC,                                 // ー (prolonged, kept)
        0x3042, 0x3044, 0x3046, 0x3048, 0x304A, // ア イ ウ エ オ
        0x304B, 0x304D, 0x304F, 0x3051, 0x3053, // カ キ ク ケ コ
        0x3055, 0x3057, 0x3059, 0x305B, 0x305D, // サ シ ス セ ソ
        0x305F, 0x3061, 0x3064, 0x3066, 0x3068, // タ チ ツ テ ト
        0x306A, 0x306B, 0x306C, 0x306D, 0x306E, // ナ ニ ヌ ネ ノ
        0x306F, 0x3072, 0x3075, 0x3078, 0x307B, // ハ ヒ フ ヘ ホ
        0x307E, 0x307F, 0x3080, 0x3081, 0x3082, // マ ミ ム メ モ
        0x3084, 0x3086, 0x3088,                 // ヤ ユ ヨ
        0x3089, 0x308A, 0x308B, 0x308C, 0x308D, // ラ リ ル レ ロ
        0x308F, 0x3093,                         // ワ ン
    };

    // Can a hiragana base take a dakuten by simple +1 (か行/さ行/た行/は行)?
    auto canVoicePlusOne = [](uint32_t h) {
        return (h >= 0x304B && h <= 0x3062) || // か..ち (odd = unvoiced)
               (h >= 0x3064 && h <= 0x3068) || // つ て と
               (h >= 0x306F && h <= 0x307B);   // は..ほ rows (handakuten too)
    };
    auto canHandakuten = [](uint32_t h) {
        return h >= 0x306F && h <= 0x307B;     // は ひ ふ へ ほ
    };

    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = utf8::decode(s, i);
        if (cp == 0) {
            // Malformed lead byte: copy the single byte through.
            out += s[start];
            continue;
        }
        // Full-width katakana → hiragana.
        if (cp >= 0x30A1 && cp <= 0x30F6) {
            utf8::encode(cp - 0x60, out);
            continue;
        }
        // Half-width katakana.
        if (cp >= 0xFF66 && cp <= 0xFF9D) {
            uint32_t h = kHalfBase[cp - 0xFF66];
            // Fold a trailing half-width dakuten / handakuten into the kana.
            size_t peek = i;
            uint32_t mark = peek < s.size() ? utf8::decode(s, peek) : 0;
            if (mark == 0xFF9E) { // ゙ dakuten
                if (h == 0x3046) { h = 0x3094; i = peek; } // ヴ → ゔ
                else if (canVoicePlusOne(h)) { h += 1; i = peek; }
            } else if (mark == 0xFF9F) { // ゚ handakuten
                if (canHandakuten(h)) { h += 2; i = peek; }
            }
            utf8::encode(h, out);
            continue;
        }
        // Everything else (already hiragana, ー, ・, punctuation) passes
        // through unchanged.
        utf8::encode(cp, out);
    }
    return out;
}

} // namespace

std::string libskkCurrentYomi(SkkContext *ctx) {
    if (!ctx) return {};
    const gchar *preedit = skk_context_get_preedit(ctx);
    if (!preedit) return {};
    static const std::string kTriangleDown = "\xe2\x96\xbd"; // ▽ U+25BD
    std::string s = preedit;
    if (s.rfind(kTriangleDown, 0) != 0) return {};
    s.erase(0, kTriangleDown.size());
    // Okurigana mode (▽X*Y, where X is the henkan stem and Y is the
    // okurigana suffix): drop the '*' separator and feed the concatenated
    // kana to mozc. Mozc handles verb / adjective conjugations natively, so
    // X+Y is a valid query — we just lose SKK's own per-stem 送りあり
    // matching, which is acceptable since the user's personal dict will
    // pick up commonly used pairs.
    size_t star = s.find('*');
    if (star != std::string::npos) {
        s.erase(star, 1);
    }
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
    // Katakana input modes (q / C-q) surface the midashigo as katakana, but
    // the reading mozc and the SKK dict expect is hiragana. Normalise so
    // conversion behaves the same as in hiragana mode.
    return katakanaToHiragana(s);
}

} // namespace skk_mozc
