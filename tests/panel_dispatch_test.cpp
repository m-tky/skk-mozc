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

#include "../src/panel_dispatch/panel_dispatch.h"

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

        // --- Anything else falls through (SoftAbort): typing a letter
        //     while the panel is open should close it and let libskk
        //     extend the ▽ yomi.
        {"Unknown key → SoftAbort",
         dp::PanelKey::Other, 0, 9, false, false,
         dp::PanelAction::SoftAbort, -1},
    };

    for (const auto &c : cases) run(c);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
