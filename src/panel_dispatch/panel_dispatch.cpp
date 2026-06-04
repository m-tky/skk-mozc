/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "panel_dispatch.h"

namespace skk_mozc::dispatch {

namespace {

bool isDigit(PanelKey k) {
    return k >= PanelKey::Digit1 && k <= PanelKey::Digit9;
}

int digitIndex(PanelKey k) {
    return static_cast<int>(k) - static_cast<int>(PanelKey::Digit1);
}

} // namespace

PanelDecision decidePanelAction(PanelKey k,
                                int cursor_global,
                                int total_size,
                                bool has_prev_page,
                                bool has_next_page) {
    PanelDecision out;
    switch (k) {
    case PanelKey::Escape:
    case PanelKey::CtrlG:
        // Hard cancel — matches SKK's "C-g aborts everything" convention so
        // the user's NEXT conversion starts from a clean state, not a yomi
        // concatenated with the abandoned one.
        out.action = PanelAction::HardCancel;
        return out;

    case PanelKey::Enter:
        if (cursor_global >= 0 && cursor_global < total_size) {
            out.action = PanelAction::Commit;
        } else {
            out.action = PanelAction::HardCancel;
        }
        return out;

    case PanelKey::Space:
    case PanelKey::Down:
        out.action = PanelAction::NextCandidate;
        return out;

    case PanelKey::Up:
        out.action = PanelAction::PrevCandidate;
        return out;

    case PanelKey::PageUp:
        out.action = has_prev_page ? PanelAction::PrevPage
                                   : PanelAction::Ignore;
        return out;
    case PanelKey::PageDown:
        out.action = has_next_page ? PanelAction::NextPage
                                   : PanelAction::Ignore;
        return out;

    case PanelKey::Backspace:
        // Re-edit the yomi: close the panel, libskk's ▽ stays intact, the
        // Backspace itself goes to libskk so the next press shortens the
        // yomi by one kana. Matches the standard SKK ▼ → Backspace → ▽
        // transition.
        out.action = PanelAction::SoftAbort;
        return out;

    case PanelKey::TextInput:
        // Standard SKK semantics: typing a printable key in conversion
        // mode commits the focused candidate and starts a new conversion
        // with the typed character. Without this users had to press Enter
        // first, which felt inconsistent.
        if (cursor_global >= 0 && cursor_global < total_size) {
            out.action = PanelAction::CommitAndForward;
        } else {
            // No focused candidate to commit; fall back to SoftAbort so
            // the typed key still reaches libskk.
            out.action = PanelAction::SoftAbort;
        }
        return out;

    default:
        // (fall through to the Other branch below)
        if (isDigit(k)) {
            out.action = PanelAction::CommitAtPage;
            out.page_index = digitIndex(k);
            return out;
        }
        // Function keys, modifier-only events, etc. Close the panel
        // quietly and let libskk see the key.
        out.action = PanelAction::SoftAbort;
        return out;
    }
}

bool isRefinementKey(PanelKey key) {
    // The PanelKey enum currently has no explicit slot for refinement-only
    // keys (Shift+Arrow / Tab). When the refinement sub-mode is re-added
    // we'll add e.g. RefineFocusNext / RefineShrink members here. For now
    // nothing qualifies, so the panel always wins.
    (void)key;
    return false;
}

RouteTarget decideRoute(const RouteState &state, PanelKey key) {
    if (!state.panel_active) {
        // No panel up. SPACE in ▽ mode opens one; everything else falls
        // through to libskk (which keeps editing the yomi, applies romaji,
        // etc.).
        if (key == PanelKey::Space) return RouteTarget::OpenPanel;
        return RouteTarget::Passthrough;
    }
    // Panel is up. The invariant the user-reported "ヤキニク... + 焼肉定食..."
    // double-commit bug taught us: ENTER and SPACE must NEVER route to the
    // refiner, because the refiner's commit() concatenates segment defaults
    // while the panel's Commit fires the focused (full-sentence) candidate.
    // Allowing both meant <enter><enter> committed twice with different
    // strings. Only true refinement keys (boundary edits, segment focus)
    // can ever escape to the refiner.
    if (state.refiner_armed && isRefinementKey(key)) {
        return RouteTarget::RefinerDispatch;
    }
    return RouteTarget::PanelDispatch;
}

} // namespace skk_mozc::dispatch
