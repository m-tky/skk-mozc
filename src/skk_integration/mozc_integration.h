/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Façade that the patched skk.cpp talks to. Keeps the patch small: 3
 * call-sites referencing this single header.
 *
 * Lifetime: one instance per SkkState (i.e., per input context), owned via
 * std::unique_ptr from SkkState.
 */

#ifndef FCITX5_SKK_MOZC_INTEGRATION_H_
#define FCITX5_SKK_MOZC_INTEGRATION_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward-decl rather than #include fcitx5 headers so this file can be lifted
// into a CLI build (no fcitx5 link needed) for unit tests.
namespace fcitx {
class KeyEvent;
class InputContext;
} // namespace fcitx

// libskk forward decls. The integration calls into libskk to read the current
// yomi / candidates and to inject learning into the user dict.
extern "C" {
typedef struct _SkkContext SkkContext;
typedef struct _SkkCandidateList SkkCandidateList;
typedef struct _SkkDict SkkDict;
}

namespace skk_mozc {

class MozcClient;

struct IntegrationOptions {
    bool mozc_enabled = true;
    int ipc_timeout_ms = 50;
    int max_mozc_candidates = 20;
    bool debug = false;
    // Absolute path to mozc_server. On most distros it's in $PATH as
    // "mozc_server", but Nix-installed mozc places it at
    // <store>/lib/mozc/mozc_server (not on $PATH), so the HM module sets
    // this explicitly via SKK_MOZC_MOZC_SERVER.
    std::string mozc_server_path;

    // Reads SKK_MOZC_ENABLE / SKK_MOZC_IPC_TIMEOUT_MS /
    // SKK_MOZC_MAX_CANDIDATES / SKK_MOZC_DEBUG / SKK_MOZC_MOZC_SERVER from
    // the environment. Anything unset falls back to the defaults above.
    static IntegrationOptions fromEnv();
};

class MozcIntegration {
public:
    MozcIntegration(SkkContext *libskk_context,
                    IntegrationOptions options);
    ~MozcIntegration();

    // Tell the integration which libskk dictionary to write user learning
    // into. Pass the first writable SkkDict the engine has (typically
    // ~/.skk-jisyo). May be called at construction time or any time later;
    // a null pointer disables learning until set again.
    void setUserDict(SkkDict *user_dict);

    // Provide an accessor for the engine's full dictionary list. The
    // integration consults this in handleKey to merge libskk lookup results
    // with mozc candidates BEFORE libskk consumes the SPC. The accessor may
    // be called multiple times; return value should reflect the current
    // engine state.
    using DictAccessor = std::function<std::vector<SkkDict *>()>;
    void setDictAccessor(DictAccessor accessor);

    // Callback that fully resets SkkState. Called after we commit a
    // merged candidate so the cached SkkState::preedit_ is cleared along
    // with libskk's own state — otherwise the application keeps seeing
    // ▽yomi as preedit after our commit.
    using FullReset = std::function<void()>;
    void setFullReset(FullReset cb);

    // SKK configuration the integration should mirror onto its own panel
    // so that page size / selection-key behaviour stays consistent with
    // upstream fcitx5-skk's UI.
    enum class CandidateChooseKeyStyle { Digit, ABC, Qwerty };
    struct SkkConfigSnapshot {
        int page_size = 9;
        CandidateChooseKeyStyle choose_key = CandidateChooseKeyStyle::Digit;
    };
    using ConfigAccessor = std::function<SkkConfigSnapshot()>;
    void setConfigAccessor(ConfigAccessor accessor);

    // Called from the *top* of SkkState::keyEvent. Returns true if the
    // refinement sub-mode consumed the key (in which case the caller skips
    // libskk dispatch and the rest of its keyEvent).
    bool handleKey(fcitx::KeyEvent &keyEvent,
                   fcitx::InputContext *ic);

    // Called from SkkState::updateUI after libskk has produced its candidate
    // list. Inspects the libskk SkkCandidateList and, if we are in a state
    // where mozc augmentation makes sense (▽ mode with a multi-char yomi),
    // builds a merged candidate list and pushes it onto fcitx5's input panel.
    void augmentCandidates(fcitx::InputContext *ic,
                           SkkCandidateList *libskk_candidates);

    // Called when SKK commits text from a merged candidate; routes the
    // learning back into libskk's user dict.
    void recordCommit(const std::string &yomi,
                      const std::string &surface);

    // True while the mozc-driven panel owns the candidate-list slot of the
    // input panel. Patched skk.cpp's updateUI() checks this so it does not
    // reset() the panel out from under us when libskk emits a stray
    // notify::preedit between our setCandidateList() and the user's next key.
    bool ownsCandidatePanel() const;

public:
    // Exposed so the helpers in the .cpp can reach Impl. Treat as private
    // outside the .cpp.
    struct Impl;

private:
    // Sub-routers for handleKey(). Each returns true iff the event was
    // consumed.
    bool handleRefinerKey_(fcitx::KeyEvent &keyEvent,
                           fcitx::InputContext *ic);
    bool handlePanelKey_(fcitx::KeyEvent &keyEvent,
                         fcitx::InputContext *ic);
    // Pre-libskk SPC interception: opens a mozc-owned panel if the current
    // ▽ yomi is suitable for mozc augmentation. Returns true iff opened.
    bool maybeOpenMozcPanel_(fcitx::KeyEvent &keyEvent,
                             fcitx::InputContext *ic);

    std::unique_ptr<Impl> impl_;
};

} // namespace skk_mozc

#endif // FCITX5_SKK_MOZC_INTEGRATION_H_
