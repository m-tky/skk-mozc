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
    Other = 0,    // function keys, modifiers — close panel quietly
    Backspace,    // re-edit the yomi; libskk gets the keystroke
    TextInput,    // printable ASCII (a-z, A-Z, punctuation) — start a new
                  // SKK conversion, so commit the focused candidate first
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
    // Refinement-only keys. Routed to RefinerDispatch when refiner_armed.
    RefineShrink,    // Shift+← : shrink focused bunsetsu
    RefineGrow,      // Shift+→ : grow focused bunsetsu
    RefineFocusNext, // Tab     : move attention to next bunsetsu
    RefineFocusPrev, // Shift+Tab : previous bunsetsu
};

enum class PanelAction {
    Ignore,           // do nothing (e.g. key release we don't care about)
    Commit,           // commit candidate under the cursor + hard reset
    CommitAtPage,     // commit candidate at page-relative index (carries digit)
    CommitAndForward, // commit focused candidate + forward the key to libskk
                      // so the next SKK input starts from the typed key
    NextCandidate,    // cursor++
    PrevCandidate,    // cursor--
    NextPage,         // page++ (no-op if !hasNext)
    PrevPage,         // page-- (no-op if !hasPrev)
    HardCancel,       // close panel AND reset libskk's ▽
    SoftAbort,        // close panel; libskk ▽ kept; pass key through to libskk
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

// Higher-level routing decision: given the state of the integration
// (whether the mozc panel and/or refinement sub-mode are active), where
// should this key be dispatched?
//
// This is the seam where the "double commit" bug lived: when both panel
// and refiner claimed Enter, two competing commits fired (the panel's
// full-sentence top candidate vs. the refiner's segment-by-segment
// concatenation), pasting both into the application. `decideRoute()`
// codifies the invariant that ENTER (and the other panel-commit keys)
// ALWAYS route to the panel — never to the refiner — so the regression
// can be caught by unit tests without simulating real fcitx5 input.
enum class RouteTarget {
    OpenPanel,       // SPC interception while no panel is open
    PanelDispatch,   // hand off to decidePanelAction()
    RefinerDispatch, // refinement-specific keys (Shift+Arrow / Tab); only
                     // valid when refiner_armed is true
    Passthrough,     // let libskk see the key
};

struct RouteState {
    bool panel_active  = false;
    bool refiner_armed = false; // ignored entirely while the panel/refiner
                                 // share ENTER and SPACE — see decideRoute().
};

// Returns where the key should go. Panel always wins ENTER / SPACE /
// digits / arrows so the refiner cannot produce a competing commit.
RouteTarget decideRoute(const RouteState &state, PanelKey key);

// True iff `key` is one of the refinement-only keys the refiner would
// legitimately consume (boundary editing, segment focus). All commit /
// navigation keys are excluded so they stay with the panel.
bool isRefinementKey(PanelKey key);

} // namespace skk_mozc::dispatch

#endif // FCITX5_SKK_MOZC_PANEL_DISPATCH_H_
