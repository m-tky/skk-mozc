/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Refinement sub-mode (entered when SKK ▽ + SPC yields a mozc conversion).
 *
 * Keys handled here (decided in the interview):
 *   Shift+←  shrink current bunsetsu by 1 char
 *   Shift+→  grow current bunsetsu by 1 char
 *   Tab      move focus to next bunsetsu
 *   Shift+Tab move focus to prev bunsetsu
 *   Space    cycle current bunsetsu to next candidate
 *   Enter    commit full text; tell caller to learn into ~/.skk-jisyo
 *   ESC/C-g  abort, return to plain SKK ▽
 *
 * This module is intentionally fcitx5-agnostic. Integration layer translates
 * fcitx5 KeyEvent into Refiner::Action and renders Refiner::view() back to
 * fcitx5's preedit + candidate panel.
 */

#ifndef FCITX5_SKK_MOZC_REFINER_H_
#define FCITX5_SKK_MOZC_REFINER_H_

#include "../mozc_client/mozc_client.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skk_mozc {

enum class RefinerAction {
    ShrinkSegment,
    GrowSegment,
    FocusNextSegment,
    FocusPrevSegment,
    NextCandidate,
    PrevCandidate,
    Commit,
    Abort,
};

struct RefinerView {
    // Composite preedit (focused candidate of each segment, concatenated).
    std::string preedit_text;
    // UTF-8 character range [begin, end) in preedit_text that is currently
    // focused (used for highlight rendering).
    int focused_begin_chars = 0;
    int focused_end_chars = 0;
    // Bunsetsu boundaries in characters (cumulative). Useful for drawing
    // separators in the preedit if desired.
    std::vector<int> segment_boundaries;
};

struct RefinerCommit {
    // The accepted full text, ready to be sent to the application.
    std::string text;
    // (yomi, surface) pairs to be appended to ~/.skk-jisyo. The integration
    // layer routes these into libskk's user dict update path.
    std::vector<std::pair<std::string, std::string>> learn_entries;
};

class Refiner {
public:
    // Takes ownership of `session`, which is a live mozc session already
    // primed with the initial conversion.
    Refiner(std::unique_ptr<RefinementSession> session,
            std::string original_yomi);
    ~Refiner();

    // Apply a keypress-derived action. Returns true if the refiner consumed
    // it. On Commit/Abort the refiner becomes "done" (see done() / aborted()).
    bool dispatch(RefinerAction action);

    bool done() const { return done_; }
    bool aborted() const { return aborted_; }

    // Snapshot for the UI layer.
    RefinerView view() const;

    // Available only after dispatch(Commit). Pulls together text + entries
    // to register into the SKK personal dict.
    std::optional<RefinerCommit> commit();

private:
    std::unique_ptr<RefinementSession> session_;
    std::string original_yomi_;
    bool done_ = false;
    bool aborted_ = false;
};

} // namespace skk_mozc

#endif // FCITX5_SKK_MOZC_REFINER_H_
