/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pure state-machine logic for the mozc panel mode. Extracted from
 * mozc_integration.cpp so it can be exercised in tests without dragging
 * fcitx5 + libskk into the build.
 *
 * mozc_integration.cpp is responsible for:
 *   1. Translating fcitx5 KeyEvent → PanelKey via classifyKey()
 *   2. Calling decidePanelAction() with the current panel state
 *   3. Executing the returned PanelAction against the real input panel /
 *      libskk context
 *
 * The translation + execution is glue; the *decision* is what we test here.
 */

#ifndef FCITX5_SKK_MOZC_PANEL_DISPATCH_H_
#define FCITX5_SKK_MOZC_PANEL_DISPATCH_H_

namespace skk_mozc::dispatch {

enum class PanelKey {
    Other = 0,    // any key we don't specifically recognise
    Space,
    Escape,
    CtrlG,
    Enter,
    Up,
    Down,
    PageUp,
    PageDown,
    Digit1, Digit2, Digit3, Digit4, Digit5,
    Digit6, Digit7, Digit8, Digit9,
};

enum class PanelAction {
    Ignore,         // do nothing (e.g. key release we don't care about)
    Commit,         // commit candidate under the cursor + hard reset
    CommitAtPage,   // commit candidate at page-relative index (carries digit)
    NextCandidate,  // cursor++
    PrevCandidate,  // cursor--
    NextPage,       // page++ (no-op if !hasNext)
    PrevPage,       // page-- (no-op if !hasPrev)
    HardCancel,     // close panel AND reset libskk's ▽
    SoftAbort,      // close panel; libskk ▽ kept; pass key through to libskk
};

struct PanelDecision {
    PanelAction action = PanelAction::Ignore;
    // For CommitAtPage: 0-8 selecting the digit pressed.
    int page_index = -1;
};

// Decide what to do when a key arrives in panel-active mode.
//
//   cursor_global: list->globalCursorIndex() (may be -1 if nothing focused)
//   total_size   : list->totalSize()
//   has_prev_page: list->hasPrev()
//   has_next_page: list->hasNext()
PanelDecision decidePanelAction(PanelKey k,
                                int cursor_global,
                                int total_size,
                                bool has_prev_page,
                                bool has_next_page);

} // namespace skk_mozc::dispatch

#endif // FCITX5_SKK_MOZC_PANEL_DISPATCH_H_
