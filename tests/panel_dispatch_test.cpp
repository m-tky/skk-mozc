/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit tests for the panel decision state machine.
 *
 * These tests exercise the same dispatch logic that mozc_integration.cpp
 * runs in production, so the scenarios users hit in fcitx5 — "press SPC,
 * see candidates, press 2, commit", "press SPC then ESC then SPC again
 * with no stale yomi", etc. — are reproducible deterministically here.
 *
 * Build target: tests/panel_dispatch_test (no fcitx5 / libskk deps).
 * Invocation: returns 0 on pass, nonzero on fail; prints '[PASS] ...' /
 * '[FAIL] ...' to stdout.
 */

#include "panel_dispatch/panel_dispatch.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace dp = skk_mozc::dispatch;

namespace {

struct TestCase {
    const char *name;
    dp::PanelKey key;
    int cursor;
    int total;
    bool has_prev;
    bool has_next;
    dp::PanelAction expected_action;
    int expected_page_index;
};

int g_pass = 0;
int g_fail = 0;

const char *actionName(dp::PanelAction a) {
    switch (a) {
    case dp::PanelAction::Ignore:         return "Ignore";
    case dp::PanelAction::Commit:         return "Commit";
    case dp::PanelAction::CommitAtPage:   return "CommitAtPage";
    case dp::PanelAction::NextCandidate:  return "NextCandidate";
    case dp::PanelAction::PrevCandidate:  return "PrevCandidate";
    case dp::PanelAction::NextPage:       return "NextPage";
    case dp::PanelAction::PrevPage:       return "PrevPage";
    case dp::PanelAction::HardCancel:     return "HardCancel";
    case dp::PanelAction::SoftAbort:      return "SoftAbort";
    }
    return "?";
}

void run(const TestCase &t) {
    auto got = dp::decidePanelAction(t.key, t.cursor, t.total, t.has_prev,
                                     t.has_next);
    bool ok = got.action == t.expected_action &&
              (t.expected_page_index < 0 ||
               got.page_index == t.expected_page_index);
    if (ok) {
        ++g_pass;
        std::printf("[PASS] %s\n", t.name);
    } else {
        ++g_fail;
        std::printf("[FAIL] %s: got action=%s page=%d, expected action=%s "
                    "page=%d\n",
                    t.name, actionName(got.action), got.page_index,
                    actionName(t.expected_action), t.expected_page_index);
    }
}

} // namespace

int main() {
    // Scenarios written in the spirit of "what the user hits while actually
    // typing", not exhaustive permutations.
    std::vector<TestCase> cases = {
        // --- ESC / Ctrl+G must always be a HARD cancel so the next SPC
        //     starts with a fresh yomi. This was the user-reported bug:
        //     "前の記録を引き継いでしまう" (previous record carries over).
        {"ESC mid-selection → HardCancel",
         dp::PanelKey::Escape, /*cur*/3, /*total*/9, false, false,
         dp::PanelAction::HardCancel, -1},
        {"Ctrl+G mid-selection → HardCancel",
         dp::PanelKey::CtrlG, /*cur*/0, /*total*/5, false, false,
         dp::PanelAction::HardCancel, -1},
        {"ESC with empty list → HardCancel",
         dp::PanelKey::Escape, -1, 0, false, false,
         dp::PanelAction::HardCancel, -1},

        // --- Enter on the focused candidate.
        {"Enter at cursor 0 → Commit",
         dp::PanelKey::Enter, 0, 5, false, false,
         dp::PanelAction::Commit, -1},
        {"Enter at cursor 4 (last) → Commit",
         dp::PanelKey::Enter, 4, 5, false, false,
         dp::PanelAction::Commit, -1},
        {"Enter with no candidates → HardCancel",
         dp::PanelKey::Enter, -1, 0, false, false,
         dp::PanelAction::HardCancel, -1},

        // --- Navigation: Space / Down advance, Up retreats.
        {"Space → NextCandidate", dp::PanelKey::Space, 0, 9, false, false,
         dp::PanelAction::NextCandidate, -1},
        {"Down → NextCandidate", dp::PanelKey::Down, 0, 9, false, false,
         dp::PanelAction::NextCandidate, -1},
        {"Up → PrevCandidate", dp::PanelKey::Up, 4, 9, false, false,
         dp::PanelAction::PrevCandidate, -1},

        // --- Paging.
        {"PageDown with hasNext=true → NextPage",
         dp::PanelKey::PageDown, 0, 20, false, true,
         dp::PanelAction::NextPage, -1},
        {"PageDown with hasNext=false → Ignore",
         dp::PanelKey::PageDown, 0, 9, false, false,
         dp::PanelAction::Ignore, -1},
        {"PageUp with hasPrev=true → PrevPage",
         dp::PanelKey::PageUp, 0, 20, true, true,
         dp::PanelAction::PrevPage, -1},
        {"PageUp with hasPrev=false → Ignore",
         dp::PanelKey::PageUp, 0, 9, false, false,
         dp::PanelAction::Ignore, -1},

        // --- Digit selection: 1-9 → CommitAtPage with page-relative index.
        //     The user explicitly wanted digit selection to work.
        {"Digit 1 → CommitAtPage idx=0",
         dp::PanelKey::Digit1, 0, 9, false, false,
         dp::PanelAction::CommitAtPage, 0},
        {"Digit 2 → CommitAtPage idx=1",
         dp::PanelKey::Digit2, 0, 9, false, false,
         dp::PanelAction::CommitAtPage, 1},
        {"Digit 5 → CommitAtPage idx=4",
         dp::PanelKey::Digit5, 0, 9, false, false,
         dp::PanelAction::CommitAtPage, 4},
        {"Digit 9 → CommitAtPage idx=8",
         dp::PanelKey::Digit9, 0, 9, false, false,
         dp::PanelAction::CommitAtPage, 8},

        // --- Anything else falls through (SoftAbort).
        {"Unknown key → SoftAbort",
         dp::PanelKey::Other, 0, 9, false, false,
         dp::PanelAction::SoftAbort, -1},

        // --- Bare modifier presses (Shift_L, Ctrl_L, etc.) must NOT
        //     close the panel. User-reported regression: holding Shift
        //     to type capital K after SPC → the Shift_L event closed
        //     the mozc panel via SoftAbort, then K reached libskk's
        //     ▽かね and triggered okurigana register mode "せぐか".
        {"ModifierOnly press with focused cand → Ignore",
         dp::PanelKey::ModifierOnly, 0, 9, false, false,
         dp::PanelAction::Ignore, -1},
        {"ModifierOnly press with no cursor → Ignore",
         dp::PanelKey::ModifierOnly, -1, 0, false, false,
         dp::PanelAction::Ignore, -1},

        // --- Backspace returns to ▽ for re-editing the yomi.
        {"Backspace → SoftAbort (re-edit yomi)",
         dp::PanelKey::Backspace, 0, 9, false, false,
         dp::PanelAction::SoftAbort, -1},

        // --- Text input commits the focused candidate and forwards the
        //     keystroke. This is the user-reported bug: pressing Space to
        //     navigate then typing the next char should commit the
        //     focused candidate, not melt it back to hiragana.
        {"TextInput with focused cand → CommitAndForward",
         dp::PanelKey::TextInput, 0, 9, false, false,
         dp::PanelAction::CommitAndForward, -1},
        {"TextInput with cursor 5 → CommitAndForward",
         dp::PanelKey::TextInput, 5, 9, false, false,
         dp::PanelAction::CommitAndForward, -1},
        {"TextInput with no cursor → SoftAbort",
         dp::PanelKey::TextInput, -1, 0, false, false,
         dp::PanelAction::SoftAbort, -1},
    };

    for (const auto &c : cases) run(c);

    // ----- decideRoute() tests -----
    // These lock in the fix for the "Yakinikuteisyokugatabetainaa<space>
    // <enter><enter> → ヤキニクテイショク... + 焼肉定食..." double-commit bug.
    //
    // The bug existed because the refiner and the panel were both armed at
    // the same time and both claimed ENTER: the refiner's commit (segment
    // concatenation) fired on the first Enter, the panel's commit (top
    // candidate) fired on the second. Each test below pins down a routing
    // decision that, if reverted, would let that regression back in.

    struct RouteCase {
        const char *name;
        dp::RouteState state;
        dp::PanelKey key;
        dp::RouteTarget expected;
    };
    std::vector<RouteCase> route_cases = {
        // No panel up: SPACE opens one, everything else passes through.
        {"no panel + SPC → OpenPanel",
         {/*panel*/false, /*refiner*/false},
         dp::PanelKey::Space, dp::RouteTarget::OpenPanel},
        {"no panel + ENTER → Passthrough",
         {/*panel*/false, /*refiner*/false},
         dp::PanelKey::Enter, dp::RouteTarget::Passthrough},
        {"no panel + letter → Passthrough",
         {/*panel*/false, /*refiner*/false},
         dp::PanelKey::TextInput, dp::RouteTarget::Passthrough},

        // Panel up, no refiner: panel handles all interaction.
        {"panel + ENTER → PanelDispatch",
         {/*panel*/true, /*refiner*/false},
         dp::PanelKey::Enter, dp::RouteTarget::PanelDispatch},
        {"panel + SPACE → PanelDispatch (next candidate)",
         {/*panel*/true, /*refiner*/false},
         dp::PanelKey::Space, dp::RouteTarget::PanelDispatch},
        {"panel + ESC → PanelDispatch (hard cancel)",
         {/*panel*/true, /*refiner*/false},
         dp::PanelKey::Escape, dp::RouteTarget::PanelDispatch},
        {"panel + digit → PanelDispatch",
         {/*panel*/true, /*refiner*/false},
         dp::PanelKey::Digit2, dp::RouteTarget::PanelDispatch},

        // *** The double-commit regression guards. ***
        // Even when a refiner is somehow armed alongside an active panel,
        // ENTER and the navigation keys MUST go to the panel — never to
        // the refiner — so we don't double-commit.
        {"REGRESSION: panel + refiner + ENTER → PanelDispatch (no refiner!)",
         {/*panel*/true, /*refiner*/true},
         dp::PanelKey::Enter, dp::RouteTarget::PanelDispatch},
        {"REGRESSION: panel + refiner + SPACE → PanelDispatch",
         {/*panel*/true, /*refiner*/true},
         dp::PanelKey::Space, dp::RouteTarget::PanelDispatch},
        {"REGRESSION: panel + refiner + digit → PanelDispatch",
         {/*panel*/true, /*refiner*/true},
         dp::PanelKey::Digit1, dp::RouteTarget::PanelDispatch},
        {"REGRESSION: panel + refiner + ESC → PanelDispatch",
         {/*panel*/true, /*refiner*/true},
         dp::PanelKey::Escape, dp::RouteTarget::PanelDispatch},
        {"REGRESSION: panel + refiner + Up/Down/PageUp/PageDown → PanelDispatch",
         {/*panel*/true, /*refiner*/true},
         dp::PanelKey::Up, dp::RouteTarget::PanelDispatch},

        // --- Refinement keys do reach RefinerDispatch when armed ---
        {"panel + refiner + Shift+← (RefineShrink) → RefinerDispatch",
         {/*panel*/true, /*refiner*/true},
         dp::PanelKey::RefineShrink, dp::RouteTarget::RefinerDispatch},
        {"panel + refiner + Shift+→ (RefineGrow) → RefinerDispatch",
         {/*panel*/true, /*refiner*/true},
         dp::PanelKey::RefineGrow, dp::RouteTarget::RefinerDispatch},
        {"panel + refiner + Tab (RefineFocusNext) → RefinerDispatch",
         {/*panel*/true, /*refiner*/true},
         dp::PanelKey::RefineFocusNext, dp::RouteTarget::RefinerDispatch},
        {"panel + refiner + Shift+Tab (RefineFocusPrev) → RefinerDispatch",
         {/*panel*/true, /*refiner*/true},
         dp::PanelKey::RefineFocusPrev, dp::RouteTarget::RefinerDispatch},

        // --- Without refiner, refinement keys are silently ignored ---
        {"panel without refiner + RefineShrink → PanelDispatch (Ignore)",
         {/*panel*/true, /*refiner*/false},
         dp::PanelKey::RefineShrink, dp::RouteTarget::PanelDispatch},
    };

    int route_pass = 0, route_fail = 0;
    auto routeTargetName = [](dp::RouteTarget t) -> const char * {
        switch (t) {
        case dp::RouteTarget::OpenPanel:       return "OpenPanel";
        case dp::RouteTarget::PanelDispatch:   return "PanelDispatch";
        case dp::RouteTarget::RefinerDispatch: return "RefinerDispatch";
        case dp::RouteTarget::Passthrough:     return "Passthrough";
        }
        return "?";
    };
    for (const auto &rc : route_cases) {
        auto got = dp::decideRoute(rc.state, rc.key);
        bool ok = got == rc.expected;
        if (ok) {
            ++route_pass;
            std::printf("[PASS] %s\n", rc.name);
        } else {
            ++route_fail;
            std::printf("[FAIL] %s: got %s, expected %s\n", rc.name,
                        routeTargetName(got), routeTargetName(rc.expected));
        }
    }

    std::printf("\n%d passed, %d failed (incl. %d route + %d action)\n",
                g_pass + route_pass, g_fail + route_fail,
                route_pass, g_pass);
    return (g_fail + route_fail) == 0 ? 0 : 1;
}
