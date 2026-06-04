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

// libskkCurrentYomi has moved to ../skk_integration/yomi_extract.cpp so the
// libskk-talking part can be unit-tested independently.
using ::skk_mozc::libskkCurrentYomi;

} // namespace

struct MozcIntegration::Impl {
    SkkContext *libskk_ctx = nullptr;
    SkkDict *user_dict = nullptr;
    MozcIntegration::DictAccessor dict_accessor;
    MozcIntegration::FullReset full_reset;
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

bool MozcIntegration::ownsCandidatePanel() const {
    return impl_->panel_active;
}

void MozcIntegration::setDictAccessor(DictAccessor accessor) {
    impl_->dict_accessor = std::move(accessor);
}

void MozcIntegration::setFullReset(FullReset cb) {
    impl_->full_reset = std::move(cb);
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

// Mirror the focused candidate of the active panel into the inline preedit
// so the application shows a live preview of what would commit if the user
// hit Enter. Standard SKK ▼ / mozc behaviour.
void mirrorFocusedCandidateToPreedit(fcitx::InputContext *ic) {
    auto *raw = ic->inputPanel().candidateList().get();
    auto *list = dynamic_cast<fcitx::CommonCandidateList *>(raw);
    if (!list) return;
    int idx = list->globalCursorIndex();
    if (idx < 0 || idx >= list->totalSize()) return;
    auto display = list->candidate(idx).text().toString();
    fcitx::Text pre;
    pre.append(display, {fcitx::TextFormatFlag::Underline,
                          fcitx::TextFormatFlag::HighLight});
    ic->inputPanel().setClientPreedit(pre);
    ic->inputPanel().setPreedit(pre);
    ic->updatePreedit();
}

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

namespace {
// Forward decl; the definition lives further down alongside the panel-key
// translation helpers.
skk_mozc::dispatch::PanelKey classifyPanelKey(const fcitx::Key &k);
} // namespace

bool MozcIntegration::handleKey(fcitx::KeyEvent &keyEvent,
                                fcitx::InputContext *ic) {
    if (keyEvent.isRelease()) {
        // Release events do not drive state; bail before classification so
        // we don't burn cycles on every key release.
        return false;
    }
    namespace dp = skk_mozc::dispatch;
    dp::RouteState st{
        /*panel_active=*/impl_->panel_active,
        /*refiner_armed=*/impl_->refiner && !impl_->refiner->done(),
    };
    auto target = dp::decideRoute(st, classifyPanelKey(keyEvent.key()));
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
    installMergedPanel(impl_.get(), ic, yomi, std::move(merged));

    // Refinement sub-mode (Shift+Arrow / Tab to tweak bunsetsu boundaries)
    // is disabled in v1: it shared the ENTER/SPACE keys with the panel and
    // produced a second, competing commit (Refiner's segment-by-segment
    // concatenation vs. the panel's full-sentence top candidate). That
    // looked to the user as two pasted strings, one of which was wrong.
    // We will re-introduce it once we have a clean key-routing split so
    // the two UIs don't both claim Enter.
    (void)mozc_out;
    keyEvent.filterAndAccept();
    return true;
}

namespace {

// Map a fcitx5 KeyEvent to the dispatcher's neutral PanelKey enum so the
// decision tree can be unit-tested without fcitx5 in the build.
skk_mozc::dispatch::PanelKey classifyPanelKey(const fcitx::Key &k) {
    using PK = skk_mozc::dispatch::PanelKey;
    if (k.check(FcitxKey_Escape))                      return PK::Escape;
    if (k.check(FcitxKey_g, fcitx::KeyState::Ctrl))    return PK::CtrlG;
    if (k.check(FcitxKey_Return))                      return PK::Enter;
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
    // Any other printable ASCII (letters, punctuation) is treated as text
    // input. We exclude modifier-only events so e.g. Shift alone doesn't
    // accidentally commit. SKK feeds these to libskk for romaji→kana
    // conversion or henkan-start triggers.
    auto sym = static_cast<uint32_t>(k.sym());
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
    auto pk = classifyPanelKey(key);
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
    auto refresh_ui = [&]() {
        // Update the inline preedit before painting so the focused
        // candidate is visible at the cursor position too.
        mirrorFocusedCandidateToPreedit(ic);
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    };

    switch (decision.action) {
    case A::Ignore:
        keyEvent.filterAndAccept();
        return true;
    case A::Commit: {
        int idx = list->globalCursorIndex();
        if (idx >= 0 && idx < list->totalSize()) {
            list->candidate(idx).select(ic);
        }
        keyEvent.filterAndAccept();
        return true;
    }
    case A::CommitAtPage: {
        int page_start = list->currentPage() * list->pageSize();
        int idx = page_start + decision.page_index;
        if (idx >= 0 && idx < list->totalSize()) {
            list->candidate(idx).select(ic);
        }
        keyEvent.filterAndAccept();
        return true;
    }
    case A::CommitAndForward: {
        // Commit the focused candidate (its select() callback runs
        // full_reset, leaving libskk in a fresh ▽-less state), then return
        // false WITHOUT filterAndAccept'ing so the key continues through
        // SkkState::keyEvent to libskk — which then begins a new SKK input
        // with the typed character.
        int idx = list->globalCursorIndex();
        if (idx >= 0 && idx < list->totalSize()) {
            list->candidate(idx).select(ic);
        } else {
            clearMozcPanel(impl_.get(), ic, /*reset_libskk=*/false);
        }
        return false;
    }
    case A::NextCandidate:
        list->nextCandidate();
        refresh_ui();
        keyEvent.filterAndAccept();
        return true;
    case A::PrevCandidate:
        list->prevCandidate();
        refresh_ui();
        keyEvent.filterAndAccept();
        return true;
    case A::NextPage:
        list->next();
        refresh_ui();
        keyEvent.filterAndAccept();
        return true;
    case A::PrevPage:
        list->prev();
        refresh_ui();
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
