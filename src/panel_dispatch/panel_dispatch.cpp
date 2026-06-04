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

    default:
        if (isDigit(k)) {
            out.action = PanelAction::CommitAtPage;
            out.page_index = digitIndex(k);
            return out;
        }
        // Any other key → close panel softly and let libskk see the key.
        // This is how typing more letters or Backspace gets back to ▽ mode.
        out.action = PanelAction::SoftAbort;
        return out;
    }
}

} // namespace skk_mozc::dispatch
