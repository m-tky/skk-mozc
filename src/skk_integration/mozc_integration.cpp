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
    MozcIntegration::DictAccessor dict_accessor;
    IntegrationOptions opts;
    std::shared_ptr<MozcClient> client;
    std::unique_ptr<Refiner> refiner;

    // Mozc-driven candidate panel state. While `panel_active` is true the
    // input panel's candidate list is owned by this integration (not libskk),
    // and every navigation / selection / cancel key is routed through
    // handlePanelKey. Cleared on commit, ESC, or any non-handled key.
    bool panel_active = false;
    std::string panel_yomi;
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

void MozcIntegration::setDictAccessor(DictAccessor accessor) {
    impl_->dict_accessor = std::move(accessor);
}

namespace {

// Forward decls (definitions follow).

// Install a CommonCandidateList from a merged candidate list and mark the
// integration as panel-owning.
void installMergedPanel(MozcIntegration::Impl *impl,
                        fcitx::InputContext *ic,
                        std::string yomi,
                        std::vector<MergedCandidate> merged);

// Tear down the mozc panel, optionally also clearing libskk's preedit (used
// when we commit so libskk doesn't keep showing ▽yomi after we wrote text).
void clearMozcPanel(MozcIntegration::Impl *impl, fcitx::InputContext *ic,
                    bool reset_libskk);

// ---- Implementations ----

void installMergedPanel(MozcIntegration::Impl *impl,
                        fcitx::InputContext *ic,
                        std::string yomi,
                        std::vector<MergedCandidate> merged) {
    auto fcitx_list = std::make_unique<fcitx::CommonCandidateList>();
    fcitx_list->setPageSize(9); // matches the digit-selection key mapping
    fcitx_list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
    fcitx_list->setSelectionKey(fcitx::KeyList{
        fcitx::Key(FcitxKey_1), fcitx::Key(FcitxKey_2),
        fcitx::Key(FcitxKey_3), fcitx::Key(FcitxKey_4),
        fcitx::Key(FcitxKey_5), fcitx::Key(FcitxKey_6),
        fcitx::Key(FcitxKey_7), fcitx::Key(FcitxKey_8),
        fcitx::Key(FcitxKey_9),
    });

    for (const auto &c : merged) {
        std::string text = c.value;
        std::string desc = c.annotation;
        auto *self = impl;
        auto cb = [self, text, yomi](fcitx::InputContext *ictx) {
            SKK_MOZC_LOG("panel: commit \"%s\" for yomi=\"%s\"",
                         text.c_str(), yomi.c_str());
            ictx->commitString(text);
            if (self->user_dict) {
                ::SkkCandidate *cand = skk_candidate_new(
                    yomi.c_str(), static_cast<gboolean>(0),
                    text.c_str(), nullptr, text.c_str());
                if (cand) {
                    if (skk_dict_select_candidate(self->user_dict, cand)) {
                        skk_dict_save(self->user_dict, nullptr);
                    }
                    g_object_unref(cand);
                }
            }
            clearMozcPanel(self, ictx, /*reset_libskk=*/true);
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
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void clearMozcPanel(MozcIntegration::Impl *impl, fcitx::InputContext *ic,
                    bool reset_libskk) {
    impl->panel_active = false;
    impl->panel_yomi.clear();
    impl->refiner.reset();
    ic->inputPanel().reset();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    if (reset_libskk && impl->libskk_ctx) {
        // Drop libskk's ▽ preedit so the application doesn't see a phantom
        // yomi after our commit.
        skk_context_reset(impl->libskk_ctx);
        ic->updatePreedit();
    }
}

} // namespace

bool MozcIntegration::handleRefinerKey_(fcitx::KeyEvent &keyEvent,
                                        fcitx::InputContext *ic) {
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

bool MozcIntegration::handleKey(fcitx::KeyEvent &keyEvent,
                                fcitx::InputContext *ic) {
    // 1. Bunsetsu refinement sub-mode wins everything.
    if (impl_->refiner && !impl_->refiner->done()) {
        return handleRefinerKey_(keyEvent, ic);
    }
    // 2. If we already own the panel, route the key through panel control.
    if (impl_->panel_active) {
        return handlePanelKey_(keyEvent, ic);
    }
    // 3. Otherwise watch for SPC in ▽ mode to potentially open the panel.
    return maybeOpenMozcPanel_(keyEvent, ic);
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
    installMergedPanel(impl_.get(), ic, yomi, std::move(merged));

    if (mozc_out && mozc_out->segments.size() >= 2) {
        if (auto session = impl_->client->beginRefinement(yomi)) {
            impl_->refiner =
                std::make_unique<Refiner>(std::move(session), yomi);
            SKK_MOZC_LOG("SPC: refinement sub-mode armed");
        }
    }
    keyEvent.filterAndAccept();
    return true;
}

bool MozcIntegration::handlePanelKey_(fcitx::KeyEvent &keyEvent,
                                      fcitx::InputContext *ic) {
    if (keyEvent.isRelease()) return false;
    auto *raw = ic->inputPanel().candidateList().get();
    auto *list = dynamic_cast<fcitx::CommonCandidateList *>(raw);
    if (!list) {
        impl_->panel_active = false;
        return false;
    }
    const auto &key = keyEvent.key();

    // ESC / Ctrl+G: cancel mozc augmentation, leave libskk in its ▽ state.
    if (key.check(FcitxKey_Escape) ||
        key.check(FcitxKey_g, fcitx::KeyState::Ctrl)) {
        SKK_MOZC_LOG("panel: ESC — cancelling, restoring SKK ▽");
        clearMozcPanel(impl_.get(), ic, /*reset_libskk=*/false);
        keyEvent.filterAndAccept();
        return true;
    }

    // Enter: commit the currently focused candidate.
    if (key.check(FcitxKey_Return)) {
        int idx = list->globalCursorIndex();
        if (idx < 0) idx = 0;
        if (idx < list->totalSize()) {
            list->candidate(idx).select(ic);
        }
        keyEvent.filterAndAccept();
        return true;
    }

    // SPC / Down: next candidate. Up: previous.
    if (key.check(FcitxKey_space) || key.check(FcitxKey_Down)) {
        list->nextCandidate();
        ic->updateUserInterface(
            fcitx::UserInterfaceComponent::InputPanel);
        keyEvent.filterAndAccept();
        return true;
    }
    if (key.check(FcitxKey_Up)) {
        list->prevCandidate();
        ic->updateUserInterface(
            fcitx::UserInterfaceComponent::InputPanel);
        keyEvent.filterAndAccept();
        return true;
    }

    // PageUp / PageDown.
    if (key.check(FcitxKey_Page_Up) || key.check(FcitxKey_Prior)) {
        if (list->hasPrev()) {
            list->prev();
            ic->updateUserInterface(
                fcitx::UserInterfaceComponent::InputPanel);
        }
        keyEvent.filterAndAccept();
        return true;
    }
    if (key.check(FcitxKey_Page_Down) || key.check(FcitxKey_Next)) {
        if (list->hasNext()) {
            list->next();
            ic->updateUserInterface(
                fcitx::UserInterfaceComponent::InputPanel);
        }
        keyEvent.filterAndAccept();
        return true;
    }

    // Digit 1-9, 0: direct selection on the current page.
    static const std::array<fcitx::KeySym, 10> kDigits = {
        FcitxKey_1, FcitxKey_2, FcitxKey_3, FcitxKey_4, FcitxKey_5,
        FcitxKey_6, FcitxKey_7, FcitxKey_8, FcitxKey_9, FcitxKey_0,
    };
    for (size_t i = 0; i < kDigits.size(); ++i) {
        if (key.check(kDigits[i])) {
            int page_start = list->currentPage() * list->pageSize();
            int idx = page_start + static_cast<int>(i);
            if (idx < list->totalSize()) {
                list->candidate(idx).select(ic);
            }
            keyEvent.filterAndAccept();
            return true;
        }
    }

    // Any other key: cancel the panel and let libskk see the key. This
    // matches Mozc's UX where typing a non-navigation key abandons the
    // current conversion.
    SKK_MOZC_LOG("panel: unhandled key — cancelling, deferring to libskk");
    clearMozcPanel(impl_.get(), ic, /*reset_libskk=*/false);
    return false;
}

void MozcIntegration::augmentCandidates(fcitx::InputContext *ic,
                                        SkkCandidateList *libskk_candidates) {
    // No-op in the new design. Mozc augmentation is now driven entirely from
    // the SPC-intercept path in maybeOpenMozcPanel_, before libskk has a
    // chance to react. Leaving this stub keeps the patched skk.cpp call site
    // unchanged so a future bump can reuse it.
    (void)ic;
    (void)libskk_candidates;
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
