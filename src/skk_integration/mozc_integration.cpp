/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mozc_integration.h"

#include "../bunsetsu/refiner.h"
#include "../candidate_merger/merger.h"
#include "../mozc_client/mozc_client.h"

#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <libskk/libskk.h>

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
    o.mozc_enabled = readBool("SKK_MOZC_ENABLE", o.mozc_enabled);
    o.ipc_timeout_ms = readInt("SKK_MOZC_IPC_TIMEOUT_MS", o.ipc_timeout_ms);
    o.max_mozc_candidates = readInt("SKK_MOZC_MAX_CANDIDATES",
                                    o.max_mozc_candidates);
    o.debug = readBool("SKK_MOZC_DEBUG", o.debug);
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
    // libskk exposes the in-progress preedit; the portion under the
    // conversion ("▽" marker) is what we want as the yomi.
    const gchar *preedit = skk_context_get_preedit(ctx);
    if (!preedit) return {};
    std::string s = preedit;
    // Strip the leading ▽ marker (U+25BD, UTF-8 e2 96 bd) if present.
    static const std::string kHeisei = "\xe2\x96\xbd";
    if (s.rfind(kHeisei, 0) == 0) {
        s.erase(0, kHeisei.size());
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
    if (options.mozc_enabled) {
        MozcClientOptions co;
        co.timeout = std::chrono::milliseconds(options.ipc_timeout_ms);
        co.max_candidates = options.max_mozc_candidates;
        co.debug = options.debug;
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

    // Repaint preedit using the refiner's view.
    auto v = impl_->refiner->view();
    fcitx::Text pre;
    pre.append(v.preedit_text, fcitx::TextFormatFlag::Underline);
    // We can't easily byte-range a highlight without splitting; the visual
    // distinction of focused bunsetsu is best handled by future enhancement.
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
        return;
    }

    auto mozc_out = impl_->client->convert(yomi);

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

    // Rebuild fcitx5 candidate list with the merged contents.
    auto fcitx_list = std::make_unique<fcitx::CommonCandidateList>();
    for (const auto &c : merged) {
        fcitx::Text display(c.value);
        std::function<void(fcitx::InputContext *)> cb =
            [text = c.value, yomi, this](fcitx::InputContext *ictx) {
                ictx->commitString(text);
                this->recordCommit(yomi, text);
            };
        fcitx_list->append<CallbackCandidateWord>(std::move(display),
                                                  std::move(cb));
        if (!c.annotation.empty()) {
            // Annotation goes onto the just-appended entry's comment slot via
            // the public ModifiableCandidateList API.
            // (Skipped in v0; see CLAUDE.md "オープン論点".)
        }
    }
    ic->inputPanel().setCandidateList(std::move(fcitx_list));
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);

    // Hand off to refinement mode if mozc returned a usable bunsetsu split.
    // beginRefinement() spends an extra mozc roundtrip; we deliberately accept
    // that cost only when there's >= 2 segments, since single-bunsetsu
    // conversions don't benefit from boundary editing.
    if (mozc_out && mozc_out->segments.size() >= 2) {
        if (auto session = impl_->client->beginRefinement(yomi)) {
            impl_->refiner =
                std::make_unique<Refiner>(std::move(session), yomi);
        }
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
