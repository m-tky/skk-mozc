/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mozc_integration.h"

#include "../bunsetsu/refiner.h"
#include "../candidate_merger/merger.h"
#include "../log/log.h"
#include "../mozc_client/mozc_client.h"
#include "../panel_dispatch/panel_dispatch.h"
#include "yomi_extract.h"

#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <libskk/libskk.h>

#include <array>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <typeinfo>

namespace skk_mozc {

namespace {

// Local CandidateWord that fires a std::function on select(). Wrapped here
// because CandidateWord::select is pure-virtual and we need to plumb the
// commit + learning side-effect through it.
class CallbackCandidateWord : public fcitx::CandidateWord {
public:
    CallbackCandidateWord(fcitx::Text text,
                          std::function<void(fcitx::InputContext *)> cb)
        : fcitx::CandidateWord(std::move(text)), cb_(std::move(cb)) {}
    CallbackCandidateWord(fcitx::Text text, fcitx::Text comment,
                          std::function<void(fcitx::InputContext *)> cb)
        : fcitx::CandidateWord(std::move(text)), cb_(std::move(cb)) {
        setComment(std::move(comment));
    }
    void select(fcitx::InputContext *ic) const override { cb_(ic); }

private:
    std::function<void(fcitx::InputContext *)> cb_;
};

} // namespace

IntegrationOptions IntegrationOptions::fromEnv() {
    IntegrationOptions o;
    auto readInt = [](const char *name, int fallback) {
        if (const char *v = std::getenv(name); v && *v) {
            try { return std::stoi(v); } catch (...) {}
        }
        return fallback;
    };
    auto readBool = [](const char *name, bool fallback) {
        if (const char *v = std::getenv(name); v && *v) {
            return std::string(v) != "0";
        }
        return fallback;
    };
    auto readStr = [](const char *name, std::string fallback) {
        if (const char *v = std::getenv(name); v && *v) return std::string(v);
        return fallback;
    };
    o.mozc_enabled = readBool("SKK_MOZC_ENABLE", o.mozc_enabled);
    o.ipc_timeout_ms = readInt("SKK_MOZC_IPC_TIMEOUT_MS", o.ipc_timeout_ms);
    o.max_mozc_candidates = readInt("SKK_MOZC_MAX_CANDIDATES",
                                    o.max_mozc_candidates);
    o.debug = readBool("SKK_MOZC_DEBUG", o.debug);
    o.mozc_server_path = readStr("SKK_MOZC_MOZC_SERVER", "");
    return o;
}

namespace {

// Minimum yomi length (in UTF-8 chars) to bother asking mozc. For single-kana
// queries SKK's own lookup is fine, and mozc would mostly return junk.
constexpr int kMinYomiCharsForMozc = 2;

int utf8Chars(const std::string &s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t step = 1;
        if ((c & 0x80) == 0)        step = 1;
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        i += step;
        ++n;
    }
    return n;
}

// libskkCurrentYomi has moved to ../skk_integration/yomi_extract.cpp so the
// libskk-talking part can be unit-tested independently.
using ::skk_mozc::libskkCurrentYomi;

// Convert a hiragana string to the romaji sequence that libskk's default
// rom-kana rule will fold BACK into that hiragana when typed in hiragana mode.
// Used to re-inject the remainder yomi after a partial Mozc commit:
//   user picks 「右」 out of ▽みぎはし's mozc panel → remainder is 「はし」 →
//   we feed "Hashi" so libskk lands in ▽はし mode and the user can SPC again.
//
// `start_henkan` capitalises the first romaji letter so SKK enters ▽ mode on
// the first kana (uppercase letter == start-preedit + that kana in default
// keymap). When false we just emit lowercase romaji.
//
// Combined kana (きゃ etc.) are matched as a 2-codepoint window first; the
// fallback for an isolated small kana is the x-prefix form ("xa" / "xya"
// /etc.) which the default table also recognises.
std::string kanaToRomajiForLibskk(const std::string &kana, bool start_henkan) {
    struct Pair { const char *kana; const char *romaji; };
    // Two-codepoint combos first. Listed in the same order as libskk's
    // default rom-kana table so the produced romaji is exactly what that
    // table maps back. Only combos that the table accepts as a single token
    // are listed; anything missing falls back to per-codepoint encoding.
    static const Pair kCombos[] = {
        {"きゃ","kya"}, {"きゅ","kyu"}, {"きょ","kyo"},
        {"ぎゃ","gya"}, {"ぎゅ","gyu"}, {"ぎょ","gyo"},
        {"しゃ","sha"}, {"しゅ","shu"}, {"しょ","sho"},
        {"じゃ","ja"},  {"じゅ","ju"},  {"じょ","jo"},
        {"ちゃ","cha"}, {"ちゅ","chu"}, {"ちょ","cho"},
        {"にゃ","nya"}, {"にゅ","nyu"}, {"にょ","nyo"},
        {"ひゃ","hya"}, {"ひゅ","hyu"}, {"ひょ","hyo"},
        {"びゃ","bya"}, {"びゅ","byu"}, {"びょ","byo"},
        {"ぴゃ","pya"}, {"ぴゅ","pyu"}, {"ぴょ","pyo"},
        {"みゃ","mya"}, {"みゅ","myu"}, {"みょ","myo"},
        {"りゃ","rya"}, {"りゅ","ryu"}, {"りょ","ryo"},
        {"ふぁ","fa"},  {"ふぃ","fi"},  {"ふぇ","fe"},  {"ふぉ","fo"},
    };
    static const Pair kSingles[] = {
        {"あ","a"},  {"い","i"},  {"う","u"},  {"え","e"},  {"お","o"},
        {"か","ka"}, {"き","ki"}, {"く","ku"}, {"け","ke"}, {"こ","ko"},
        {"が","ga"}, {"ぎ","gi"}, {"ぐ","gu"}, {"げ","ge"}, {"ご","go"},
        {"さ","sa"}, {"し","shi"},{"す","su"}, {"せ","se"}, {"そ","so"},
        {"ざ","za"}, {"じ","ji"}, {"ず","zu"}, {"ぜ","ze"}, {"ぞ","zo"},
        {"た","ta"}, {"ち","chi"},{"つ","tsu"},{"て","te"}, {"と","to"},
        {"だ","da"}, {"ぢ","di"}, {"づ","du"}, {"で","de"}, {"ど","do"},
        {"な","na"}, {"に","ni"}, {"ぬ","nu"}, {"ね","ne"}, {"の","no"},
        {"は","ha"}, {"ひ","hi"}, {"ふ","fu"}, {"へ","he"}, {"ほ","ho"},
        {"ば","ba"}, {"び","bi"}, {"ぶ","bu"}, {"べ","be"}, {"ぼ","bo"},
        {"ぱ","pa"}, {"ぴ","pi"}, {"ぷ","pu"}, {"ぺ","pe"}, {"ぽ","po"},
        {"ま","ma"}, {"み","mi"}, {"む","mu"}, {"め","me"}, {"も","mo"},
        {"や","ya"}, {"ゆ","yu"}, {"よ","yo"},
        {"ら","ra"}, {"り","ri"}, {"る","ru"}, {"れ","re"}, {"ろ","ro"},
        {"わ","wa"}, {"を","wo"}, {"ん","nn"},
        {"ぁ","xa"}, {"ぃ","xi"}, {"ぅ","xu"}, {"ぇ","xe"}, {"ぉ","xo"},
        {"ゃ","xya"},{"ゅ","xyu"},{"ょ","xyo"},
        {"っ","xtu"},{"ー","-"},
    };

    auto utf8CharLen = [](unsigned char c) -> size_t {
        if ((c & 0x80) == 0)        return 1;
        if ((c & 0xE0) == 0xC0)     return 2;
        if ((c & 0xF0) == 0xE0)     return 3;
        if ((c & 0xF8) == 0xF0)     return 4;
        return 1;
    };

    std::string out;
    out.reserve(kana.size() * 2);
    bool first = true;
    size_t i = 0;
    while (i < kana.size()) {
        size_t len1 = utf8CharLen(static_cast<unsigned char>(kana[i]));
        if (i + len1 > kana.size()) break;
        std::string ch1 = kana.substr(i, len1);

        const char *romaji = nullptr;
        size_t step = 0;
        if (i + len1 < kana.size()) {
            size_t len2 = utf8CharLen(
                static_cast<unsigned char>(kana[i + len1]));
            if (i + len1 + len2 <= kana.size()) {
                std::string pair = ch1 + kana.substr(i + len1, len2);
                for (const auto &p : kCombos) {
                    if (pair == p.kana) {
                        romaji = p.romaji;
                        step = len1 + len2;
                        break;
                    }
                }
            }
        }
        if (!romaji) {
            for (const auto &p : kSingles) {
                if (ch1 == p.kana) {
                    romaji = p.romaji;
                    step = len1;
                    break;
                }
            }
        }
        if (!romaji) {
            // Unknown character (e.g. mid-romaji garbage, latin, punct). Pass
            // it through verbatim and hope libskk does something reasonable.
            out += ch1;
            i += len1;
            first = false;
            continue;
        }
        if (first && start_henkan) {
            // Uppercase first letter == start-preedit trigger in libskk's
            // default hiragana keymap.
            out += static_cast<char>(std::toupper(
                static_cast<unsigned char>(romaji[0])));
            out += romaji + 1;
        } else {
            out += romaji;
        }
        first = false;
        i += step;
    }
    return out;
}

} // namespace

struct MozcIntegration::Impl {
    SkkContext *libskk_ctx = nullptr;
    SkkDict *user_dict = nullptr;
    MozcIntegration::DictAccessor dict_accessor;
    MozcIntegration::FullReset full_reset;
    MozcIntegration::ConfigAccessor config_accessor;
    IntegrationOptions opts;
    std::shared_ptr<MozcClient> client;
    std::unique_ptr<Refiner> refiner;

    // Mozc-driven candidate panel state. While `panel_active` is true the
    // input panel's candidate list is owned by this integration (not libskk),
    // and every navigation / selection / cancel key is routed through
    // handlePanelKey. Cleared on commit, ESC, or any non-handled key.
    bool panel_active = false;
    std::string panel_yomi;

    // Set true by a candidate's select() callback when it has already taken
    // care of libskk's preedit state — specifically, when a partial Mozc
    // candidate (one whose reading is a prefix of panel_yomi) commits its
    // surface AND re-feeds the remainder yomi back into libskk as ▽<rest>.
    // handlePanelKey_ checks this AFTER select() returns and passes
    // reset_libskk=false to clearMozcPanel, so the ▽<rest> we just typed
    // back in survives.
    bool skip_libskk_reset_on_panel_clear = false;
};

MozcIntegration::MozcIntegration(SkkContext *libskk_context,
                                 IntegrationOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->libskk_ctx = libskk_context;
    impl_->opts = options;
    log::setEnabled(options.debug);
    SKK_MOZC_LOG(
        "MozcIntegration init: mozc_enabled=%d timeout=%dms maxCand=%d "
        "server=%s",
        options.mozc_enabled, options.ipc_timeout_ms,
        options.max_mozc_candidates,
        options.mozc_server_path.empty() ? "(default)"
                                         : options.mozc_server_path.c_str());
    // (Egg-like-newline is forced to true in the patched SkkState::applyConfig
    // — see patches/fcitx5-skk/0001-add-mozc-integration-hooks.patch. Doing
    // it from this constructor would be overridden, because applyConfig is
    // invoked from SkkEngine::factory AFTER our SkkState ctor returns.)
    if (options.mozc_enabled) {
        MozcClientOptions co;
        co.timeout = std::chrono::milliseconds(options.ipc_timeout_ms);
        co.max_candidates = options.max_mozc_candidates;
        co.debug = options.debug;
        co.mozc_server_path = options.mozc_server_path;
        impl_->client = std::make_shared<MozcClient>(co);
    }
}

MozcIntegration::~MozcIntegration() = default;

void MozcIntegration::setUserDict(SkkDict *user_dict) {
    impl_->user_dict = user_dict;
}

bool MozcIntegration::ownsCandidatePanel() const {
    return impl_->panel_active;
}

void MozcIntegration::setDictAccessor(DictAccessor accessor) {
    impl_->dict_accessor = std::move(accessor);
}

void MozcIntegration::setFullReset(FullReset cb) {
    impl_->full_reset = std::move(cb);
}

void MozcIntegration::setConfigAccessor(ConfigAccessor accessor) {
    impl_->config_accessor = std::move(accessor);
}

namespace {

// Forward decls (definitions follow).

// Install a CommonCandidateList from a merged candidate list and mark the
// integration as panel-owning. `segments` powers per-bunsetsu learning on
// commit of the full-sentence top candidate.
void installMergedPanel(MozcIntegration::Impl *impl,
                        fcitx::InputContext *ic,
                        std::string yomi,
                        std::vector<MergedCandidate> merged,
                        std::vector<MozcSegment> segments);

// Tear down the mozc panel, optionally also clearing libskk's preedit (used
// when we commit so libskk doesn't keep showing ▽yomi after we wrote text).
void clearMozcPanel(MozcIntegration::Impl *impl, fcitx::InputContext *ic,
                    bool reset_libskk);

// Live SKK dict lookup against every dict the engine has. Used by both the
// initial SPC interception and the refinement-rebuild path.
std::vector<SkkSideCandidate>
lookupSkkCandidates(MozcIntegration::Impl *impl, const std::string &yomi);

// ---- Implementations ----

// Mirror the focused candidate of the active panel into the inline preedit
// so the application shows a live preview of what would commit if the user
// hit Enter. Standard SKK ▼ / mozc behaviour.
void mirrorFocusedCandidateToPreedit(fcitx::InputContext *ic) {
    auto *raw = ic->inputPanel().candidateList().get();
    auto *list = dynamic_cast<fcitx::CommonCandidateList *>(raw);
    if (!list) return;
    int idx = list->globalCursorIndex();
    if (idx < 0 || idx >= list->totalSize()) return;
    // candidate(int) is PAGE-RELATIVE; candidateFromAll(int) is GLOBAL.
    // globalCursorIndex() is global, so use the global accessor — using
    // candidate() crashed at cursor positions outside page 0 with
    // std::invalid_argument("CommonCandidateList: invalid index").
    auto display = list->candidateFromAll(idx).text().toString();
    fcitx::Text pre;
    pre.append(display, {fcitx::TextFormatFlag::Underline,
                          fcitx::TextFormatFlag::HighLight});
    // Inline preedit at the application's cursor position — yes.
    ic->inputPanel().setClientPreedit(pre);
    // NOT setPreedit(): that duplicates the focused candidate at the top
    // of fcitx5's own candidate panel window, which the user finds
    // distracting. The candidate list alone is enough there.
    ic->updatePreedit();
}

// Selection key layouts mirror fcitx5-skk's three modes so the user's
// CandidateChooseKey config from skk.conf transparently applies to our
// merged panel.
fcitx::KeyList selectionKeysFor(
    MozcIntegration::CandidateChooseKeyStyle style) {
    using S = MozcIntegration::CandidateChooseKeyStyle;
    switch (style) {
    case S::ABC:
        return {
            fcitx::Key(FcitxKey_a), fcitx::Key(FcitxKey_b),
            fcitx::Key(FcitxKey_c), fcitx::Key(FcitxKey_d),
            fcitx::Key(FcitxKey_e), fcitx::Key(FcitxKey_f),
            fcitx::Key(FcitxKey_g), fcitx::Key(FcitxKey_h),
            fcitx::Key(FcitxKey_i), fcitx::Key(FcitxKey_j),
        };
    case S::Qwerty:
        return {
            fcitx::Key(FcitxKey_a), fcitx::Key(FcitxKey_s),
            fcitx::Key(FcitxKey_d), fcitx::Key(FcitxKey_f),
            fcitx::Key(FcitxKey_g), fcitx::Key(FcitxKey_h),
            fcitx::Key(FcitxKey_j), fcitx::Key(FcitxKey_k),
            fcitx::Key(FcitxKey_l), fcitx::Key(FcitxKey_semicolon),
        };
    case S::Digit:
    default:
        return {
            fcitx::Key(FcitxKey_1), fcitx::Key(FcitxKey_2),
            fcitx::Key(FcitxKey_3), fcitx::Key(FcitxKey_4),
            fcitx::Key(FcitxKey_5), fcitx::Key(FcitxKey_6),
            fcitx::Key(FcitxKey_7), fcitx::Key(FcitxKey_8),
            fcitx::Key(FcitxKey_9),
        };
    }
}

// Push (yomi, surface) into the SKK user dict. Centralised here so all
// commit paths (panel, refiner future) share the same learn logic.
void learnIntoUserDict(MozcIntegration::Impl *impl,
                       const std::string &yomi,
                       const std::string &surface) {
    if (!impl->user_dict || yomi.empty() || surface.empty()) return;
    ::SkkCandidate *cand = skk_candidate_new(
        yomi.c_str(), static_cast<gboolean>(0),
        surface.c_str(), nullptr, surface.c_str());
    if (!cand) return;
    if (skk_dict_select_candidate(impl->user_dict, cand)) {
        skk_dict_save(impl->user_dict, nullptr);
    }
    g_object_unref(cand);
}

void installMergedPanel(MozcIntegration::Impl *impl,
                        fcitx::InputContext *ic,
                        std::string yomi,
                        std::vector<MergedCandidate> merged,
                        std::vector<MozcSegment> segments) {
    auto fcitx_list = std::make_unique<fcitx::CommonCandidateList>();
    MozcIntegration::SkkConfigSnapshot cfg;
    if (impl->config_accessor) cfg = impl->config_accessor();
    // Multi-page panel restored. The 'CommonCandidateList: invalid index'
    // crash root cause was a cursor/currentPage_ desync producing
    // cursorIndex() == -1 → label(-1). The CORRECT cure is:
    //
    //   (a) setCursorKeepInSamePage(false): fcitx5's moveCursor() then
    //       auto-runs setPage(cursor / pageSize) on every nextCandidate /
    //       prevCandidate, keeping cursor and currentPage_ in lockstep.
    //   (b) Whenever WE call list->next() / list->prev() (explicit page
    //       paging via PageUp/PageDown), we MUST also move the cursor onto
    //       the new page — fcitx5 doesn't do that for us, and skipping it
    //       is precisely how the desync used to creep in.
    //   (c) Label / selection-key list sized exactly to pageSize so
    //       label(idx) for idx in [size(), labels_.size()) can never
    //       throw 'invalid label idx'.
    int page_size = cfg.page_size > 0 ? cfg.page_size : 9;
    fcitx_list->setPageSize(page_size);
    fcitx_list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
    auto sel = selectionKeysFor(cfg.choose_key);
    sel.resize(page_size); // pad/trim with default Keys (empty labels)
    fcitx_list->setSelectionKey(sel);
    fcitx_list->setCursorIncludeUnselected(false);
    fcitx_list->setCursorKeepInSamePage(false); // (a) above

    // Pre-compute the concatenated segment surface so the callback can
    // decide whether to break the commit into per-segment learn pairs.
    std::string segments_concat;
    for (const auto &seg : segments) {
        if (!seg.candidates.empty()) {
            segments_concat += seg.candidates.front().value;
        }
    }
    for (const auto &c : merged) {
        std::string text = c.value;
        std::string desc = c.annotation;
        // Reading the candidate covers. For Mozc per-segment candidates this
        // is the SEGMENT's reading (e.g. 「みぎ」 out of panel yomi
        // 「みぎはし」), which is what triggers the partial-commit re-injection
        // below.
        std::string cand_reading =
            c.reading.empty() ? yomi : c.reading;
        auto *self = impl;
        auto cb = [self, text, cand_reading, yomi, segments, segments_concat]
                  (fcitx::InputContext *ictx) {
            SKK_MOZC_LOG(
                "panel: commit \"%s\" reading=\"%s\" panel_yomi=\"%s\"",
                text.c_str(), cand_reading.c_str(), yomi.c_str());
            ictx->commitString(text);
            // Learn (candidate's actual reading, text) — not (panel_yomi,
            // text). For a partial candidate like 「右」 picked out of
            // 「みぎはし」, the meaningful pair is (「みぎ」, 「右」). The old
            // code learned (「みぎはし」, 「右」) which is plainly wrong.
            learnIntoUserDict(self, cand_reading, text);
            // If the user picked the full-sentence candidate (whose value
            // matches the mozc preedit's segment concatenation), also learn
            // each bunsetsu independently. This way next time the user
            // converts a sub-phrase ("やきにくていしょく"), the SKK personal
            // dict has it as a direct hit and beats mozc to position #1.
            if (!segments_concat.empty() && text == segments_concat) {
                for (const auto &seg : segments) {
                    if (seg.candidates.empty()) continue;
                    learnIntoUserDict(self, seg.reading,
                                      seg.candidates.front().value);
                }
                SKK_MOZC_LOG("panel: also learned %zu segments",
                             segments.size());
            }

            // Partial-commit handling: when the chosen candidate's reading is
            // a strict prefix of the panel yomi (Mozc per-segment candidate
            // like 「右」 with reading「みぎ」 from panel yomi「みぎはし」), the
            // user expects the REMAINDER (「はし」) to stay live so they can
            // hit SPC again and pick e.g. 「端」.
            //
            // We translate the remainder back into romaji and feed it through
            // libskk so it lands in ▽<remainder> mode. clearMozcPanel is
            // about to be called by handlePanelKey_ — we set the skip flag
            // so that call does NOT wipe libskk's freshly-injected preedit.
            if (cand_reading.size() < yomi.size() &&
                yomi.compare(0, cand_reading.size(), cand_reading) == 0 &&
                self->libskk_ctx) {
                std::string remainder = yomi.substr(cand_reading.size());
                std::string romaji =
                    kanaToRomajiForLibskk(remainder, /*start_henkan=*/true);
                SKK_MOZC_LOG(
                    "panel: partial commit — remainder yomi=\"%s\" → "
                    "feeding \"%s\" back into libskk",
                    remainder.c_str(), romaji.c_str());
                // Wipe libskk's stale ▽<panel_yomi> preedit. We can't use
                // full_reset here because it clears OUR panel too; go
                // straight to skk_context_reset.
                skk_context_reset(self->libskk_ctx);
                if (!romaji.empty()) {
                    skk_context_process_key_events(
                        self->libskk_ctx, romaji.c_str());
                }
                self->skip_libskk_reset_on_panel_clear = true;
            }
            // NOTE: do NOT call clearMozcPanel(self, ictx, ...) from here.
            // We are executing inside CallbackCandidateWord::select, which
            // was reached via list->candidate(idx).select(ic). Calling
            // clearMozcPanel would invoke ic->inputPanel().reset(), which
            // destroys the very CandidateList that owns this lambda — i.e.
            // destroys *this CallbackCandidateWord* while its select() body
            // is still on the call stack. That is undefined behaviour and
            // matches the user-reported "candidates が一番下まで来てから
            // non-Space を打つと crash" symptom (fcitx5's IOEventCallback
            // catches the resulting exception → FCITX_FATAL → abort).
            // Panel teardown is now done by the caller (handlePanelKey_)
            // *after* select() returns, so the candidate list is destroyed
            // only when no one is iterating over it any more.
        };
        if (!desc.empty()) {
            fcitx_list->append<CallbackCandidateWord>(
                fcitx::Text(text), fcitx::Text(desc), std::move(cb));
        } else {
            fcitx_list->append<CallbackCandidateWord>(
                fcitx::Text(text), std::move(cb));
        }
    }
    fcitx_list->setGlobalCursorIndex(0);
    ic->inputPanel().setCandidateList(std::move(fcitx_list));
    impl->panel_active = true;
    impl->panel_yomi = std::move(yomi);
    // Show the first candidate as inline preedit immediately so the
    // application's cursor area reflects the conversion preview.
    mirrorFocusedCandidateToPreedit(ic);
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void clearMozcPanel(MozcIntegration::Impl *impl, fcitx::InputContext *ic,
                    bool reset_libskk) {
    impl->panel_active = false;
    impl->panel_yomi.clear();
    impl->refiner.reset();
    ic->inputPanel().reset();
    if (reset_libskk) {
        if (impl->libskk_ctx) {
            const gchar *pre_before = skk_context_get_preedit(impl->libskk_ctx);
            SKK_MOZC_LOG("clearMozcPanel(reset=true): libskk preedit before = \"%s\"",
                         pre_before ? pre_before : "(null)");
        }
        if (impl->full_reset) {
            SKK_MOZC_LOG("clearMozcPanel: calling SkkState::reset() via full_reset");
            impl->full_reset();
        } else if (impl->libskk_ctx) {
            SKK_MOZC_LOG("clearMozcPanel: no full_reset cb, calling skk_context_reset directly");
            skk_context_reset(impl->libskk_ctx);
            ic->updatePreedit();
        }
        if (impl->libskk_ctx) {
            const gchar *pre_after = skk_context_get_preedit(impl->libskk_ctx);
            SKK_MOZC_LOG("clearMozcPanel(reset=true): libskk preedit after  = \"%s\"",
                         pre_after ? pre_after : "(null)");
        }
    }
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

} // namespace

bool MozcIntegration::handleRefinerKey_(fcitx::KeyEvent &keyEvent,
                                        fcitx::InputContext *ic) {
    // Only refinement-only keys reach this function (decideRoute() gates):
    // Tab / Shift+Tab move bunsetsu focus, Shift+←/→ shrink/grow it. The
    // refiner does the IPC to mozc; on success we re-install the panel
    // from the new MozcConversionResult so the focused candidate updates.
    const auto &key = keyEvent.key();
    std::optional<RefinerAction> act;
    if (key.check(FcitxKey_Tab) &&
        !key.states().test(fcitx::KeyState::Shift)) {
        act = RefinerAction::FocusNextSegment;
    } else if (key.check(FcitxKey_Tab, fcitx::KeyState::Shift) ||
               key.check(FcitxKey_ISO_Left_Tab)) {
        act = RefinerAction::FocusPrevSegment;
    } else if (key.check(FcitxKey_Left, fcitx::KeyState::Shift)) {
        act = RefinerAction::ShrinkSegment;
    } else if (key.check(FcitxKey_Right, fcitx::KeyState::Shift)) {
        act = RefinerAction::GrowSegment;
    }
    if (!act) return false;

    impl_->refiner->dispatch(*act);
    if (impl_->refiner->done()) {
        // Refinement-only keys never trigger done()/aborted, but bail
        // defensively if something else has set those flags.
        impl_->refiner.reset();
        keyEvent.filterAndAccept();
        return true;
    }

    // Rebuild the panel from the refiner's updated mozc state. The new
    // segment concatenation becomes the focused (top) candidate; existing
    // SKK system-dict hits for the original yomi stay underneath them.
    const auto &refined = impl_->refiner->currentResult();
    std::string yomi = impl_->panel_yomi;
    std::vector<SkkSideCandidate> skk_cands =
        lookupSkkCandidates(impl_.get(), yomi);
    MergeInputs mi;
    mi.skk_candidates = std::move(skk_cands);
    mi.mozc_candidates = refined.top_candidates;
    auto merged = mergeCandidates(mi);
    if (!merged.empty()) {
        installMergedPanel(impl_.get(), ic, yomi, std::move(merged),
                           refined.segments);
    }
    keyEvent.filterAndAccept();
    return true;
}

namespace {
// Forward decls; definitions live further down.
skk_mozc::dispatch::PanelKey classifyPanelKey(
    const fcitx::Key &k,
    MozcIntegration::CandidateChooseKeyStyle choose_style);
std::vector<SkkSideCandidate>
lookupSkkCandidates(MozcIntegration::Impl *impl, const std::string &yomi);
} // namespace

bool MozcIntegration::handleKey(fcitx::KeyEvent &keyEvent,
                                fcitx::InputContext *ic) {
    // Boundary exception barrier. fcitx5's contract for addon callbacks
    // (and IO event sources in general) is "do not throw". If an exception
    // escapes here, fcitx5's IOEventCallback wrapper catches it via
    // FCITX_FATAL and abort()s the *entire* fcitx5 process — we have seen
    // that in the wild as a SIGABRT of the daemon. Catch everything,
    // record what we can, and degrade gracefully by declining the key
    // (letting libskk handle it normally).
    try {
        if (keyEvent.isRelease()) {
            // Release events do not drive state; bail before classification
            // so we don't burn cycles on every key release.
            return false;
        }
        namespace dp = skk_mozc::dispatch;
        dp::RouteState st{
            /*panel_active=*/impl_->panel_active,
            /*refiner_armed=*/impl_->refiner && !impl_->refiner->done(),
        };
        MozcIntegration::SkkConfigSnapshot cfg;
        if (impl_->config_accessor) cfg = impl_->config_accessor();
        auto target = dp::decideRoute(st,
            classifyPanelKey(keyEvent.key(), cfg.choose_key));
        switch (target) {
        case dp::RouteTarget::RefinerDispatch:
            return handleRefinerKey_(keyEvent, ic);
        case dp::RouteTarget::PanelDispatch:
            return handlePanelKey_(keyEvent, ic);
        case dp::RouteTarget::OpenPanel:
            return maybeOpenMozcPanel_(keyEvent, ic);
        case dp::RouteTarget::Passthrough:
            return false;
        }
        return false;
    } catch (const std::exception &e) {
        SKK_MOZC_LOG("handleKey: caught exception (type=%s): %s — "
                     "declining key so libskk owns it",
                     typeid(e).name(), e.what());
        return false;
    } catch (...) {
        SKK_MOZC_LOG("handleKey: caught non-std exception — "
                     "declining key so libskk owns it");
        return false;
    }
}

namespace {

// A yomi is "clean" only if it contains no ASCII letters. SKK's romaji-to-
// kana machine leaves trailing latin chars (e.g. "へんかn" while waiting for
// the next vowel) on the preedit; running mozc on those yields garbage.
bool yomiIsClean(const std::string &s) {
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return false;
    }
    return true;
}

// Pull libskk's lookup results for `yomi` from every dictionary the engine
// has. The first writable dict is treated as the personal dict so the merger
// can pin its hits to slot 1.
std::vector<SkkSideCandidate>
lookupSkkCandidates(MozcIntegration::Impl *impl, const std::string &yomi) {
    std::vector<SkkSideCandidate> out;
    if (!impl->dict_accessor) return out;
    auto dicts = impl->dict_accessor();
    for (::SkkDict *dict : dicts) {
        if (!dict) continue;
        gint n = 0;
        ::SkkCandidate **cands =
            skk_dict_lookup(dict, yomi.c_str(),
                            static_cast<gboolean>(0), &n);
        if (!cands) continue;
        for (gint i = 0; i < n; ++i) {
            ::SkkCandidate *c = cands[i];
            if (!c) continue;
            SkkSideCandidate sc;
            const gchar *text = skk_candidate_get_text(c);
            sc.value = text ? text : "";
            const gchar *ann = skk_candidate_get_annotation(c);
            sc.annotation = ann ? ann : "";
            sc.from_personal_dict =
                (impl->user_dict != nullptr && dict == impl->user_dict);
            sc.reading = yomi; // libskk hits always cover the full yomi
            if (!sc.value.empty()) {
                out.push_back(std::move(sc));
            }
            g_object_unref(c);
        }
        g_free(cands);
    }
    return out;
}

} // namespace

bool MozcIntegration::maybeOpenMozcPanel_(fcitx::KeyEvent &keyEvent,
                                          fcitx::InputContext *ic) {
    if (!impl_->client) return false;
    if (keyEvent.isRelease()) return false;
    if (!keyEvent.key().check(FcitxKey_space)) return false;

    const gchar *raw_preedit = skk_context_get_preedit(impl_->libskk_ctx);
    SKK_MOZC_LOG("SPC: raw libskk preedit=\"%s\"",
                 raw_preedit ? raw_preedit : "(null)");
    std::string yomi = libskkCurrentYomi(impl_->libskk_ctx);
    if (yomi.empty() || utf8Chars(yomi) < kMinYomiCharsForMozc) {
        SKK_MOZC_LOG("SPC: skip — not in ▽ mode or yomi too short");
        return false;
    }
    if (!yomiIsClean(yomi)) {
        // SKK is mid-romaji (e.g. trailing 'n' waiting for a vowel). Let
        // libskk handle SPC and finish kana conversion first.
        SKK_MOZC_LOG("SPC: skip — yomi has pending romaji (\"%s\")",
                     yomi.c_str());
        return false;
    }

    // Look up libskk's own candidates BEFORE consuming the key. We don't let
    // libskk process SPC because that would transition it into ▼ mode and
    // make state management nightmarish; instead we read the dicts directly.
    auto skk_cands = lookupSkkCandidates(impl_.get(), yomi);

    SKK_MOZC_LOG("SPC: querying mozc with yomi=\"%s\" (skk hits=%zu)",
                 yomi.c_str(), skk_cands.size());
    auto mozc_out = impl_->client->convert(yomi);
    SKK_MOZC_LOG("SPC: mozc=%s top=%zu segs=%zu",
                 mozc_out ? "ok" : "miss",
                 mozc_out ? mozc_out->top_candidates.size() : 0,
                 mozc_out ? mozc_out->segments.size() : 0);

    // If neither SKK nor mozc returned anything useful, let libskk own the
    // SPC (it'll likely enter register mode for the unknown yomi).
    if (skk_cands.empty() &&
        (!mozc_out || mozc_out->top_candidates.empty())) {
        SKK_MOZC_LOG("SPC: nothing to show — deferring to libskk");
        return false;
    }

    // Merge SKK + mozc candidates per the design ranking rules.
    MergeInputs mi;
    mi.skk_candidates = std::move(skk_cands);
    if (mozc_out) {
        mi.mozc_candidates = std::move(mozc_out->top_candidates);
    }
    auto merged = mergeCandidates(mi);
    if (merged.empty()) return false;

    SKK_MOZC_LOG("SPC: opening merged panel (%zu candidates)", merged.size());
    std::vector<MozcSegment> segs;
    if (mozc_out) segs = mozc_out->segments;
    installMergedPanel(impl_.get(), ic, yomi, std::move(merged), segs);

    // Refinement sub-mode: arm a live mozc session so Shift+←/→ + Tab can
    // re-segment the conversion. By design only those four keys reach the
    // refiner — Enter / Space / digits / Esc stay with the panel — so the
    // double-commit regression (see panel_dispatch_test REGRESSION cases)
    // cannot recur.
    if (mozc_out && segs.size() >= 2 && impl_->client) {
        if (auto session = impl_->client->beginRefinement(yomi)) {
            impl_->refiner =
                std::make_unique<Refiner>(std::move(session), yomi);
            SKK_MOZC_LOG("SPC: refinement sub-mode armed");
        }
    }
    keyEvent.filterAndAccept();
    return true;
}

namespace {

// Map a fcitx5 KeyEvent to the dispatcher's neutral PanelKey enum so the
// decision tree can be unit-tested without fcitx5 in the build. The
// choose_style argument decides which key set picks candidate 1-9 directly
// (Digit/ABC/Qwerty) so the SKK config from ~/.config/fcitx5/conf/skk.conf
// propagates here without our panel reverting to digits 1-9.
skk_mozc::dispatch::PanelKey classifyPanelKey(
    const fcitx::Key &k,
    MozcIntegration::CandidateChooseKeyStyle choose_style) {
    using PK = skk_mozc::dispatch::PanelKey;
    // Configured selection keys first — these "look like" letters but should
    // act as digit selectors when the user picked ABC or Qwerty mode.
    {
        auto sel = selectionKeysFor(choose_style);
        for (size_t i = 0; i < sel.size() && i < 9; ++i) {
            if (k.check(sel[i].sym())) {
                return static_cast<PK>(
                    static_cast<int>(PK::Digit1) + static_cast<int>(i));
            }
        }
    }
    if (k.check(FcitxKey_Escape))                      return PK::Escape;
    if (k.check(FcitxKey_g, fcitx::KeyState::Ctrl))    return PK::CtrlG;
    // Return AND KP_Enter — without KP_Enter, numpad-Enter fell through to
    // PK::Other → SoftAbort → libskk's `commit-unhandled` for "\n" → commit
    // + newline reached the application.
    if (k.check(FcitxKey_Return) || k.check(FcitxKey_KP_Enter))
        return PK::Enter;
    // Refinement-only keys must come BEFORE plain Space/Down/Up so the
    // modifier check has a chance to match.
    if (k.check(FcitxKey_Left, fcitx::KeyState::Shift))
        return PK::RefineShrink;
    if (k.check(FcitxKey_Right, fcitx::KeyState::Shift))
        return PK::RefineGrow;
    if (k.check(FcitxKey_Tab) &&
        !k.states().test(fcitx::KeyState::Shift))
        return PK::RefineFocusNext;
    if (k.check(FcitxKey_Tab, fcitx::KeyState::Shift) ||
        k.check(FcitxKey_ISO_Left_Tab))
        return PK::RefineFocusPrev;
    if (k.check(FcitxKey_space))                       return PK::Space;
    if (k.check(FcitxKey_Down))                        return PK::Down;
    if (k.check(FcitxKey_Up))                          return PK::Up;
    if (k.check(FcitxKey_Page_Up) || k.check(FcitxKey_Prior))
        return PK::PageUp;
    if (k.check(FcitxKey_Page_Down) || k.check(FcitxKey_Next))
        return PK::PageDown;
    if (k.check(FcitxKey_BackSpace))                   return PK::Backspace;
    if (k.check(FcitxKey_1)) return PK::Digit1;
    if (k.check(FcitxKey_2)) return PK::Digit2;
    if (k.check(FcitxKey_3)) return PK::Digit3;
    if (k.check(FcitxKey_4)) return PK::Digit4;
    if (k.check(FcitxKey_5)) return PK::Digit5;
    if (k.check(FcitxKey_6)) return PK::Digit6;
    if (k.check(FcitxKey_7)) return PK::Digit7;
    if (k.check(FcitxKey_8)) return PK::Digit8;
    if (k.check(FcitxKey_9)) return PK::Digit9;
    auto sym = static_cast<uint32_t>(k.sym());
    // Bare modifier press (Shift_L, Shift_R, Control_L/R, Caps_Lock,
    // Meta_L/R, Alt_L/R, Super_L/R, Hyper_L/R — keysym 0xffe1..0xffee).
    // These arrive a few ms before the actual chord key (Shift+K etc.)
    // and we MUST surface them as a distinct PanelKey so dispatch can
    // Ignore them rather than SoftAbort the panel. Treating Shift_L as
    // PK::Other made the user-reported register-mode regression
    // ("▽かね*か【せぐ】") possible.
    if (sym >= 0xffe1 && sym <= 0xffee) {
        return PK::ModifierOnly;
    }
    // Printable ASCII (letters, punctuation) is treated as text input.
    // We exclude modifier-state-bearing events so e.g. Ctrl+P doesn't
    // accidentally commit. SKK feeds these to libskk for romaji→kana
    // conversion or henkan-start triggers.
    bool modifier_only = k.states().test(fcitx::KeyState::Ctrl) ||
                         k.states().test(fcitx::KeyState::Alt) ||
                         k.states().test(fcitx::KeyState::Super);
    if (!modifier_only && sym >= 0x20 && sym <= 0x7e) {
        return PK::TextInput;
    }
    return PK::Other;
}

} // namespace

bool MozcIntegration::handlePanelKey_(fcitx::KeyEvent &keyEvent,
                                      fcitx::InputContext *ic) {
    if (keyEvent.isRelease()) return false;
    auto *raw = ic->inputPanel().candidateList().get();
    auto *list = dynamic_cast<fcitx::CommonCandidateList *>(raw);
    if (!list) {
        SKK_MOZC_LOG("panel: panel_active but no CommonCandidateList — "
                     "force-clearing");
        clearMozcPanel(impl_.get(), ic, /*reset_libskk=*/true);
        return false;
    }
    const auto &key = keyEvent.key();
    MozcIntegration::SkkConfigSnapshot pk_cfg;
    if (impl_->config_accessor) pk_cfg = impl_->config_accessor();
    auto pk = classifyPanelKey(key, pk_cfg.choose_key);
    auto decision = skk_mozc::dispatch::decidePanelAction(
        pk, list->globalCursorIndex(), list->totalSize(),
        list->hasPrev(), list->hasNext());

    SKK_MOZC_LOG("panel: key sym=0x%x states=0x%x cursor=%d total=%d → "
                 "action=%d page_idx=%d",
                 static_cast<unsigned>(key.sym()),
                 static_cast<unsigned>(key.states()),
                 list->globalCursorIndex(), list->totalSize(),
                 static_cast<int>(decision.action), decision.page_index);

    using A = skk_mozc::dispatch::PanelAction;
    // The candidate's select() callback may need to leave libskk in a
    // freshly-injected ▽<remainder> state (partial Mozc commit path). It
    // signals this via impl_->skip_libskk_reset_on_panel_clear. We snapshot
    // the flag AFTER select() returns and pass its negation to
    // clearMozcPanel — and we always clear the flag here so each press
    // starts clean.
    auto teardown_after_select = [&]() {
        bool reset = !impl_->skip_libskk_reset_on_panel_clear;
        impl_->skip_libskk_reset_on_panel_clear = false;
        clearMozcPanel(impl_.get(), ic, /*reset_libskk=*/reset);
    };
    auto refresh_ui = [&]() {
        // Updating the inline preedit on every navigation crashed fcitx5
        // with 'CommonCandidateList: invalid index' near page boundaries
        // (likely a fcitx5 5.1.19 bug — even bare setClientPreedit() races
        // with the candidate-panel renderer). Until that's understood we
        // keep the inline preedit at whatever was set on panel install and
        // rely on the candidate panel's own focus highlight to show the
        // user which row is active. The mirror DOES still happen on
        // commit (via the candidate's select callback) and on install
        // (so the first candidate is previewed before any nav).
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    };

    switch (decision.action) {
    case A::Ignore:
        keyEvent.filterAndAccept();
        return true;
    case A::Commit: {
        // IMPORTANT: tear the panel down AFTER select() returns, never from
        // inside the candidate's select callback — that would destroy this
        // CandidateWord while we're still executing its select() body. See
        // the long NOTE in installMergedPanel's cb_ for the symptom.
        //
        // ALSO IMPORTANT: candidateFromAll(int) takes a GLOBAL index;
        // candidate(int) is page-relative. globalCursorIndex() is global,
        // so the global accessor is the matching one. Passing the global
        // cursor to candidate() crashed at cursor positions outside page 0
        // with "invalid index" — the user-visible "bottom 候補で commit
        // が効かない" regression.
        int idx = list->globalCursorIndex();
        if (idx >= 0 && idx < list->totalSize()) {
            list->candidateFromAll(idx).select(ic);
        }
        teardown_after_select();
        keyEvent.filterAndAccept();
        return true;
    }
    case A::CommitAtPage: {
        int page_start = list->currentPage() * list->pageSize();
        int idx = page_start + decision.page_index;
        if (idx >= 0 && idx < list->totalSize()) {
            list->candidateFromAll(idx).select(ic);
        }
        teardown_after_select();
        keyEvent.filterAndAccept();
        return true;
    }
    case A::CommitAndForward: {
        // Commit the focused candidate, fully reset libskk (so it is in
        // direct mode), then return false WITHOUT filterAndAccept'ing so the
        // typed key continues through SkkState::keyEvent to libskk — which
        // then begins a new SKK input from the typed character.
        int idx = list->globalCursorIndex();
        if (idx >= 0 && idx < list->totalSize()) {
            list->candidateFromAll(idx).select(ic);
            teardown_after_select();
        } else {
            clearMozcPanel(impl_.get(), ic, /*reset_libskk=*/false);
        }
        return false;
    }
    case A::NextCandidate:
        // Don't advance past the very last candidate — Mozc users expect
        // Space to stop, not silently wrap. Page crossing within range is
        // handled automatically by fcitx5 because we set
        // setCursorKeepInSamePage(false) in installMergedPanel.
        if (list->totalSize() > 0 &&
            list->globalCursorIndex() + 1 < list->totalSize()) {
            list->nextCandidate();
            refresh_ui();
        }
        keyEvent.filterAndAccept();
        return true;
    case A::PrevCandidate:
        if (list->globalCursorIndex() > 0) {
            list->prevCandidate();
            refresh_ui();
        }
        keyEvent.filterAndAccept();
        return true;
    case A::NextPage:
        // CommonCandidateList::next() advances currentPage_ but does NOT
        // touch the global cursor — leaving cursor on the old page makes
        // cursorIndex() return -1, which the renderer then turns into
        // label(-1) → 'invalid label idx' → fcitx5 abort. Re-anchor the
        // cursor to the first slot of the new page so they stay synced.
        if (list->hasNext()) {
            list->next();
            list->setGlobalCursorIndex(list->currentPage() * list->pageSize());
            refresh_ui();
        }
        keyEvent.filterAndAccept();
        return true;
    case A::PrevPage:
        if (list->hasPrev()) {
            list->prev();
            list->setGlobalCursorIndex(list->currentPage() * list->pageSize());
            refresh_ui();
        }
        keyEvent.filterAndAccept();
        return true;
    case A::HardCancel:
        clearMozcPanel(impl_.get(), ic, /*reset_libskk=*/true);
        keyEvent.filterAndAccept();
        return true;
    case A::SoftAbort:
        clearMozcPanel(impl_.get(), ic, /*reset_libskk=*/false);
        return false; // forward key to libskk
    }
    return false;
}

void MozcIntegration::augmentCandidates(fcitx::InputContext *ic,
                                        SkkCandidateList *libskk_candidates) {
    // No-op in the new design. Mozc augmentation is now driven entirely from
    // the SPC-intercept path in maybeOpenMozcPanel_, before libskk has a
    // chance to react. Leaving this stub keeps the patched skk.cpp call site
    // unchanged so a future bump can reuse it.
    //
    // The try/catch is here for the same reason as in handleKey: if this
    // ever grows real logic again, a stray throw from this code path would
    // be FATAL'd by fcitx5's updateUI IO callback wrapper and kill the
    // daemon. Keep the boundary safe regardless of what the body does.
    try {
        (void)ic;
        (void)libskk_candidates;
    } catch (const std::exception &e) {
        SKK_MOZC_LOG("augmentCandidates: caught exception (type=%s): %s",
                     typeid(e).name(), e.what());
    } catch (...) {
        SKK_MOZC_LOG("augmentCandidates: caught non-std exception");
    }
}

void MozcIntegration::recordCommit(const std::string &yomi,
                                   const std::string &surface) {
    if (yomi.empty() || surface.empty()) return;
    if (!impl_->user_dict) {
        // No writable dict was provided; silently skip. The merged candidate
        // is still committed to the application, just not learned.
        return;
    }
    // Build a transient SkkCandidate (libskk owns the memory via GObject
    // refcount) and have the user dict adopt it. `okuri = FALSE` because the
    // mozc-side candidates we merge come from full-word conversion, not the
    // SKK okurigana sub-mode. `output` mirrors `text` since SKK's display
    // and committed string are identical for these entries.
    ::SkkCandidate *cand = skk_candidate_new(
        yomi.c_str(),
        static_cast<gboolean>(0),        // okuri
        surface.c_str(),                  // text
        nullptr,                          // annotation
        surface.c_str());                 // output
    if (!cand) return;
    if (skk_dict_select_candidate(impl_->user_dict, cand)) {
        skk_dict_save(impl_->user_dict, nullptr);
    }
    g_object_unref(cand);
}

} // namespace skk_mozc
