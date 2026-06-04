/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mozc_integration.h"

#include "../bunsetsu/refiner.h"
#include "../candidate_merger/merger.h"
#include "../log/log.h"
#include "../mozc_client/mozc_client.h"

#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <libskk/libskk.h>

#include <array>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>

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

std::string libskkCurrentYomi(SkkContext *ctx) {
    // We only care about ▽ mode (yomi input). In ▼ mode the preedit contains
    // an already-converted candidate (e.g. "▼朝日"), which is not a usable
    // mozc query. Returning empty makes the caller skip mozc work entirely
    // for non-▽ states, which is what we want until the SPC-intercept
    // redesign lands.
    const gchar *preedit = skk_context_get_preedit(ctx);
    if (!preedit) return {};
    static const std::string kTriangleDown = "\xe2\x96\xbd"; // ▽ U+25BD
    std::string s = preedit;
    if (s.rfind(kTriangleDown, 0) != 0) return {};
    s.erase(0, kTriangleDown.size());
    // Defensive: strip any other stray SKK markers if present.
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

} // namespace

struct MozcIntegration::Impl {
    SkkContext *libskk_ctx = nullptr;
    SkkDict *user_dict = nullptr;
    IntegrationOptions opts;
    std::shared_ptr<MozcClient> client;
    std::unique_ptr<Refiner> refiner;
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

bool MozcIntegration::handleKey(fcitx::KeyEvent &keyEvent,
                                fcitx::InputContext *ic) {
    if (!impl_->refiner || impl_->refiner->done()) {
        return false;
    }
    const auto &key = keyEvent.key();
    std::optional<RefinerAction> act;
    if (key.check(FcitxKey_Escape) ||
        (key.check(FcitxKey_g, fcitx::KeyState::Ctrl))) {
        act = RefinerAction::Abort;
    } else if (key.check(FcitxKey_Return)) {
        act = RefinerAction::Commit;
    } else if (key.check(FcitxKey_Tab) &&
               !key.states().test(fcitx::KeyState::Shift)) {
        act = RefinerAction::FocusNextSegment;
    } else if (key.check(FcitxKey_Tab, fcitx::KeyState::Shift) ||
               key.check(FcitxKey_ISO_Left_Tab)) {
        act = RefinerAction::FocusPrevSegment;
    } else if (key.check(FcitxKey_Left, fcitx::KeyState::Shift)) {
        act = RefinerAction::ShrinkSegment;
    } else if (key.check(FcitxKey_Right, fcitx::KeyState::Shift)) {
        act = RefinerAction::GrowSegment;
    } else if (key.check(FcitxKey_space)) {
        act = RefinerAction::NextCandidate;
    } else if (key.check(FcitxKey_space, fcitx::KeyState::Shift)) {
        act = RefinerAction::PrevCandidate;
    }
    if (!act) {
        return false;
    }

    impl_->refiner->dispatch(*act);
    if (impl_->refiner->done()) {
        if (impl_->refiner->aborted()) {
            // Drop the refiner, let SKK ▽ resume from its own state.
            impl_->refiner.reset();
            keyEvent.filterAndAccept();
            return true;
        }
        if (auto c = impl_->refiner->commit()) {
            ic->commitString(c->text);
            for (const auto &[yomi, surface] : c->learn_entries) {
                recordCommit(yomi, surface);
            }
        }
        impl_->refiner.reset();
        keyEvent.filterAndAccept();
        return true;
    }

    // Repaint preedit using the refiner's view. Split into three runs so the
    // focused bunsetsu is rendered with HighLight on top of Underline.
    auto v = impl_->refiner->view();
    fcitx::Text pre;
    auto sliceByChars = [&](int begin, int end) {
        // Convert UTF-8 char range to a substring of v.preedit_text.
        int idx = 0;
        size_t byte_begin = 0, byte_end = v.preedit_text.size();
        for (size_t i = 0; i < v.preedit_text.size();) {
            if (idx == begin) byte_begin = i;
            if (idx == end) { byte_end = i; break; }
            unsigned char c =
                static_cast<unsigned char>(v.preedit_text[i]);
            size_t step = 1;
            if ((c & 0x80) == 0)        step = 1;
            else if ((c & 0xE0) == 0xC0) step = 2;
            else if ((c & 0xF0) == 0xE0) step = 3;
            else if ((c & 0xF8) == 0xF0) step = 4;
            i += step;
            ++idx;
        }
        return v.preedit_text.substr(byte_begin, byte_end - byte_begin);
    };
    if (v.focused_end_chars > v.focused_begin_chars) {
        auto head = sliceByChars(0, v.focused_begin_chars);
        auto mid  = sliceByChars(v.focused_begin_chars, v.focused_end_chars);
        auto tail = sliceByChars(v.focused_end_chars, INT32_MAX);
        if (!head.empty())
            pre.append(head, fcitx::TextFormatFlag::Underline);
        pre.append(mid, {fcitx::TextFormatFlag::Underline,
                         fcitx::TextFormatFlag::HighLight});
        if (!tail.empty())
            pre.append(tail, fcitx::TextFormatFlag::Underline);
    } else {
        pre.append(v.preedit_text, fcitx::TextFormatFlag::Underline);
    }
    ic->inputPanel().setClientPreedit(pre);
    ic->updatePreedit();
    keyEvent.filterAndAccept();
    return true;
}

void MozcIntegration::augmentCandidates(fcitx::InputContext *ic,
                                        SkkCandidateList *libskk_candidates) {
    if (!impl_->client || !libskk_candidates) {
        return;
    }
    if (!skk_candidate_list_get_page_visible(libskk_candidates)) {
        return;
    }
    std::string yomi = libskkCurrentYomi(impl_->libskk_ctx);
    if (utf8Chars(yomi) < kMinYomiCharsForMozc) {
        SKK_MOZC_LOG("augment: skip — yomi too short (%zu bytes)", yomi.size());
        return;
    }

    SKK_MOZC_LOG("augment: yomi=\"%s\"", yomi.c_str());
    auto mozc_out = impl_->client->convert(yomi);
    SKK_MOZC_LOG("augment: mozc=%s top=%zu segs=%zu",
                 mozc_out ? "ok" : "miss",
                 mozc_out ? mozc_out->top_candidates.size() : 0,
                 mozc_out ? mozc_out->segments.size() : 0);

    // Build SKK candidate snapshot. libskk doesn't tag entries by source dict,
    // so we approximate "from personal dict" via index < N where N is the
    // number of personal-dict matches; libskk presents personal-dict hits
    // first by convention.
    std::vector<SkkSideCandidate> skk_side;
    gint size = skk_candidate_list_get_size(libskk_candidates);
    for (gint i = 0; i < size; ++i) {
        SkkSideCandidate sc;
        ::SkkCandidate *raw = skk_candidate_list_get(libskk_candidates, i);
        if (!raw) continue;
        const gchar *text = skk_candidate_get_text(raw);
        sc.value = text ? text : "";
        const gchar *ann = skk_candidate_get_annotation(raw);
        sc.annotation = ann ? ann : "";
        // libskk does not surface a "from personal dict" flag in its public
        // API; conservatively mark the first entry as personal-dict-priority.
        sc.from_personal_dict = (i == 0);
        skk_side.push_back(std::move(sc));
    }

    MergeInputs mi;
    mi.skk_candidates = std::move(skk_side);
    if (mozc_out) {
        mi.mozc_candidates = std::move(mozc_out->top_candidates);
    }
    auto merged = mergeCandidates(mi);

    // v0 architectural-fix safety: do NOT replace libskk's candidate panel.
    // Replacing it caused a split where libskk-driven selection (handleCandidate
    // in skk.cpp) picked from libskk's internal list while users saw OUR list,
    // producing commits that didn't match the display.
    //
    // Until the SPC-intercept rewrite (next phase) properly takes over both
    // candidate display AND selection, we only LOG what mozc would have
    // contributed. SKK's native UI is left untouched.
    (void)merged;
    (void)ic;
    SKK_MOZC_LOG("augment: log-only mode — %zu merged candidates would be shown",
                 merged.size());

    // Refinement sub-mode is also gated off in this safety mode; it depends
    // on us owning the panel.
    (void)mozc_out;
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
