/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Integration test that runs against a real libskk SkkContext. No fcitx5
 * dependency — the panel/UI side is covered separately by
 * panel_dispatch_test.cpp.
 *
 * Scenarios exercised here are the ones that broke historically:
 *
 *   1. After typing romaji that produces "▽あさひしんぶん", libskkCurrentYomi
 *      returns exactly "あさひしんぶん" (no marker leak).
 *
 *   2. After feeding SPC (which makes libskk transition to ▼ mode),
 *      libskkCurrentYomi returns "" so we don't send the marker char to
 *      mozc by mistake.
 *
 *   3. After skk_context_reset(ctx), libskkCurrentYomi returns "" — this is
 *      the path the new ESC handler hits to prevent "previous record carries
 *      over" between conversions.
 *
 *   4. Trailing romaji (e.g. "へんかn" while the kana converter waits for a
 *      vowel) makes libskkCurrentYomi return a string with a trailing latin
 *      char; the SPC interception in production uses yomiIsClean() to skip
 *      mozc in that case. This test confirms libskk really does leave a
 *      'n' in the preedit.
 *
 *   5. End-to-end carry-over check: type yomi A, reset, type yomi B, verify
 *      libskk's preedit is the fresh "▽B" (no concatenation with A).
 */

#include "skk_integration/yomi_extract.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <libskk/libskk.h>

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond, name) do {                                             \
    if (cond) { ++g_pass; std::printf("[PASS] %s\n", name); }              \
    else      { ++g_fail; std::printf("[FAIL] %s\n", name); }              \
} while (0)

#define CHECK_EQ(actual, expected, name) do {                              \
    std::string _a = (actual);                                             \
    std::string _e = (expected);                                           \
    if (_a == _e) { ++g_pass; std::printf("[PASS] %s\n", name); }          \
    else { ++g_fail;                                                       \
           std::printf("[FAIL] %s: got \"%s\" expected \"%s\"\n",          \
                       name, _a.c_str(), _e.c_str()); }                    \
} while (0)

// Feeds a sequence of key events into the context. Returns true if every
// event was accepted.
bool feed(SkkContext *ctx, const char *seq) {
    return skk_context_process_key_events(ctx, seq);
}

SkkContext *makeContext() {
    SkkRule *rule = skk_rule_new("default", nullptr);
    if (!rule) return nullptr;
    SkkContext *ctx = skk_context_new(nullptr, 0);
    skk_context_set_typing_rule(ctx, rule);
    skk_context_set_input_mode(ctx, SKK_INPUT_MODE_HIRAGANA);
    g_object_unref(rule);
    return ctx;
}

} // namespace

int main() {
    skk_init();

    // === Scenario 1: ▽ yomi extraction ===
    {
        SkkContext *ctx = makeContext();
        // "A s a h i s h i n b u n" → ▽あさひしんぶん
        // 'nn' at the end so libskk commits the pending 'n' to ん.
        feed(ctx, "A s a h i s h i n b u n n");
        std::string preedit =
            skk_context_get_preedit(ctx) ? skk_context_get_preedit(ctx) : "";
        std::printf("  scenario1 preedit=\"%s\"\n", preedit.c_str());
        CHECK_EQ(skk_mozc::libskkCurrentYomi(ctx), "あさひしんぶん",
                 "▽ yomi extracts cleanly");
        g_object_unref(ctx);
    }

    // === Scenario 2: ▼ mode (after SPC) gives empty yomi ===
    {
        SkkContext *ctx = makeContext();
        // 'nn' at the end so libskk commits the pending 'n' to ん.
        feed(ctx, "A s a h i s h i n b u n n");
        feed(ctx, "space"); // libskk attempts conversion
        std::string preedit =
            skk_context_get_preedit(ctx) ? skk_context_get_preedit(ctx) : "";
        std::printf("  scenario2 preedit=\"%s\"\n", preedit.c_str());
        // After SPC, libskk is no longer in pure ▽ mode (either ▼ if a
        // candidate exists or registration if not). Our extractor must
        // return empty in both cases so we don't poison mozc with markers.
        CHECK(skk_mozc::libskkCurrentYomi(ctx).empty(),
              "non-▽ mode returns empty yomi");
        g_object_unref(ctx);
    }

    // === Scenario 3: skk_context_reset clears everything ===
    {
        SkkContext *ctx = makeContext();
        // 'nn' at the end so libskk commits the pending 'n' to ん.
        feed(ctx, "A s a h i s h i n b u n n");
        skk_context_reset(ctx);
        std::string preedit_after_reset =
            skk_context_get_preedit(ctx) ? skk_context_get_preedit(ctx) : "";
        std::printf("  scenario3 preedit after reset=\"%s\"\n",
                    preedit_after_reset.c_str());
        CHECK(preedit_after_reset.empty(),
              "reset clears libskk preedit (no carry-over)");
        CHECK(skk_mozc::libskkCurrentYomi(ctx).empty(),
              "reset clears extractor too");
        g_object_unref(ctx);
    }

    // === Scenario 4: trailing romaji 'n' stays in preedit ===
    {
        SkkContext *ctx = makeContext();
        // "henka" — h-e-n-k-a produces "へんか", then a stray 'n' that
        // libskk's romaji converter holds while waiting for a vowel.
        feed(ctx, "H e n k a n");
        std::string yomi = skk_mozc::libskkCurrentYomi(ctx);
        std::printf("  scenario4 yomi=\"%s\"\n", yomi.c_str());
        // We assert there's a trailing 'n' (latin) — this is what trips
        // mozc and what yomiIsClean() in production guards against.
        bool has_latin = false;
        for (char c : yomi) if (c >= 'a' && c <= 'z') { has_latin = true; break; }
        CHECK(has_latin,
              "trailing 'n' is left in yomi for libskk to finish kana");
        g_object_unref(ctx);
    }

    // === Scenario 5.5: okurigana mode (▽X*Y) drops the '*' separator ===
    // Type "Okuru" → ▽お*る. We expect libskkCurrentYomi to return "おる"
    // — the * is stripped so mozc can convert it as a verb.
    {
        SkkContext *ctx = makeContext();
        feed(ctx, "O k u R u");
        const gchar *preedit = skk_context_get_preedit(ctx);
        std::string p = preedit ? preedit : "";
        std::printf("  scenario5.5 preedit=\"%s\"\n", p.c_str());
        auto y = skk_mozc::libskkCurrentYomi(ctx);
        std::printf("  scenario5.5 yomi=\"%s\"\n", y.c_str());
        // The yomi mozc gets is the concatenation of stem + okurigana
        // with no marker. libskk's exact preedit varies slightly with
        // its rule; what matters is that we extract clean kana for mozc.
        bool no_star = y.find('*') == std::string::npos;
        bool no_marker = y.find("\xe2\x96\xbd") == std::string::npos;
        CHECK(no_star && no_marker,
              "okurigana yomi has no * and no ▽ marker");
        g_object_unref(ctx);
    }

    // === Scenario 6: katakana mode yomi is normalised to hiragana ===
    // SKK's katakana mode (q toggle) shows the midashigo as full-width
    // katakana (▽サシミ), but mozc and the SKK dict are hiragana-keyed.
    // libskkCurrentYomi must hand back hiragana so conversion behaves the
    // same as in hiragana mode (the user-reported "katakana mode だと全角/
    // 半角カタカナ候補しか出ない" bug).
    {
        SkkContext *ctx = makeContext();
        skk_context_set_input_mode(ctx, SKK_INPUT_MODE_KATAKANA);
        // Capitalised first letter starts ▽ henkan mode; in katakana mode the
        // midashigo renders as katakana.
        feed(ctx, "S a s h i m i");
        const gchar *preedit = skk_context_get_preedit(ctx);
        std::string p = preedit ? preedit : "";
        std::printf("  scenario6 preedit=\"%s\"\n", p.c_str());
        CHECK_EQ(skk_mozc::libskkCurrentYomi(ctx), "さしみ",
                 "katakana-mode yomi normalised to hiragana");
        g_object_unref(ctx);
    }

    // === Scenario 5: end-to-end carry-over check ===
    // Mirrors the user-reported bug: convert A, abort, convert B → must NOT
    // see A's yomi concatenated.
    {
        SkkContext *ctx = makeContext();
        // First conversion: ▽あさひしんぶん
        // 'nn' at the end so libskk commits the pending 'n' to ん.
        feed(ctx, "A s a h i s h i n b u n n");
        CHECK_EQ(skk_mozc::libskkCurrentYomi(ctx), "あさひしんぶん",
                 "carry-over: first yomi loaded");
        // Simulate ESC handler: hard reset.
        skk_context_reset(ctx);
        CHECK(skk_mozc::libskkCurrentYomi(ctx).empty(),
              "carry-over: state is clean after reset");
        // Second conversion: ▽きょう
        feed(ctx, "K y o u");
        CHECK_EQ(skk_mozc::libskkCurrentYomi(ctx), "きょう",
                 "carry-over: second yomi is fresh, not concatenated");
        g_object_unref(ctx);
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
