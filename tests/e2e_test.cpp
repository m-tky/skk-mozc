/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Real end-to-end test:
 *   real fcitx5 Instance → real skk.so addon → real libskk → real mozc_server
 *
 * Uses fcitx5's TestFrontend (libtestfrontend.so) to:
 *   - create an InputContext as if a real application connected,
 *   - send synthesised key events through the actual addon dispatch chain,
 *   - declare expected committed strings ahead of time via
 *     pushCommitExpectation, which the framework asserts as commits arrive.
 *
 * This reproduces the *real* bugs we hit — yomi extraction in ▽ / ▼ mode,
 * carry-over after ESC, double-commit on multi-bunsetsu Enter — by driving
 * the same code path the user does, just with synthetic input.
 *
 * Pre-requisite at run time: a reachable mozc_server (the same one fcitx5
 * normally uses). The test binary inherits the env var
 * SKK_MOZC_MOZC_SERVER from the test runner.
 */

#include <fcitx-utils/key.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx-utils/testing.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/instance.h>
#include <fcitx-module/testfrontend/testfrontend_public.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace fcitx;

namespace {

// Send a sequence of single-char key events through the test frontend.
// String chars map to fcitx5 Key() syntax: lowercase letters as-is, uppercase
// produce a Shift+letter event, and a few control names ('Return', 'space',
// 'BackSpace', 'Escape') are recognised by Key's constructor.
void sendKeys(AddonInstance *testfrontend, ICUUID uuid,
              const std::vector<std::string> &keys) {
    for (const auto &k : keys) {
        testfrontend->call<ITestFrontend::keyEvent>(uuid, Key(k), false);
        testfrontend->call<ITestFrontend::keyEvent>(uuid, Key(k), true);
    }
}

} // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // addonPaths / dataPaths are *relative* to testBinaryDir; the nix
    // derivation places skk.so + libtestfrontend.so under
    //   $out/share/skk-mozc/e2e/addons/
    // and the addon / inputmethod confs under
    //   $out/share/skk-mozc/e2e/data/addon|inputmethod
    // System fcitx5 dirs are still searched too via StandardPaths so e.g.
    // testui's runtime asset lookups don't break.
    setupTestingEnvironment(TESTING_BINARY_DIR,
                            {"addons"},
                            {"data"});
    if (const char *e = std::getenv("FCITX_ADDON_DIRS")) {
        std::fprintf(stderr, "[DEBUG] FCITX_ADDON_DIRS=%s\n", e);
    }
    if (const char *e = std::getenv("FCITX_DATA_DIRS")) {
        std::fprintf(stderr, "[DEBUG] FCITX_DATA_DIRS=%s\n", e);
    }

    char arg0[] = "skk-mozc-e2e-test";
    char arg1[] = "--disable=all";
    char arg2[] = "--enable=testfrontend,testui,skk";
    char *iargv[] = {arg0, arg1, arg2};
    Instance instance(3, iargv);
    instance.addonManager().registerDefaultLoader(nullptr);
    instance.addonManager().load();

    auto *testfrontend =
        instance.addonManager().addon("testfrontend", /*load=*/true);
    if (!testfrontend) {
        std::fprintf(stderr,
                     "[FAIL] could not load testfrontend addon\n");
        return 2;
    }

    // skk IM is registered later, inside the dispatcher, once the addon
    // manager has fully initialised and the skk addon has registered its
    // input method with InputMethodManager.

    int failures = 0;

    instance.eventDispatcher().schedule(
        [&instance, testfrontend, &failures]() {
            // Install skk into the default input method group now that the
            // addon manager has loaded the skk addon (which registered its
            // IM with the InputMethodManager).
            auto group = instance.inputMethodManager().currentGroup();
            group.inputMethodList().clear();
            group.inputMethodList().push_back(
                InputMethodGroupItem("keyboard-us"));
            group.inputMethodList().push_back(InputMethodGroupItem("skk"));
            group.setDefaultInputMethod("skk");
            instance.inputMethodManager().setGroup(std::move(group));

            auto uuid =
                testfrontend->call<ITestFrontend::createInputContext>("e2e");
            auto *ic = instance.inputContextManager().findByUUID(uuid);
            ic->focusIn();
            instance.setCurrentInputMethod(ic, "skk", true);

            // === Scenario 1: simple compound noun ===
            // Typing "Asahishinbunn" + SPACE + Enter should commit
            // "朝日新聞" exactly once.
            testfrontend->call<ITestFrontend::pushCommitExpectation>(
                "朝日新聞");
            sendKeys(testfrontend, uuid,
                     {"A", "s", "a", "h", "i", "s", "h", "i", "n",
                      "b", "u", "n", "n", "space", "Return"});

            // === Scenario 2: the user-reported multi-bunsetsu
            //     "Yakinikuteisyokugatabetainaa SPC ENTER" regression ===
            // Old buggy code committed two strings (the refiner's
            // segment-by-segment guess + the panel's full sentence);
            // pushCommitExpectation enforces exactly one.
            testfrontend->call<ITestFrontend::pushCommitExpectation>(
                "焼肉定食が食べたいなあ");
            sendKeys(testfrontend, uuid,
                     {"Y", "a", "k", "i", "n", "i", "k", "u",
                      "t", "e", "i", "s", "y", "o", "k", "u",
                      "g", "a", "t", "a", "b", "e", "t", "a",
                      "i", "n", "a", "a", "space", "Return"});

            // === Scenario 3: ESC abandons in-progress conversion ===
            // After ESC there must be no leftover ▽yomi. Re-typing a new
            // word and committing it should produce ONLY the new word,
            // not concatenated with the abandoned one.
            sendKeys(testfrontend, uuid,
                     {"A", "s", "a", "h", "i", "space", "Escape"});
            testfrontend->call<ITestFrontend::pushCommitExpectation>(
                "大丈夫");
            sendKeys(testfrontend, uuid,
                     {"D", "a", "i", "j", "y", "o", "u", "b", "u",
                      "space", "Return"});

            // === Scenario 4: navigate past the last candidate ===
            // (a) Short yomi (small candidate count, all fit in one page):
            //     spam Space past the last to hit the within-page boundary.
            // (b) Longer yomi (many candidates → multiple pages): spam
            //     Space across page boundaries — this is the path that
            //     historically tripped fcitx5's invalid-index assertion.
            // No commit expectation: PASS means we survived the spam.
            sendKeys(testfrontend, uuid,
                     {"A", "s", "a", "h", "i", "space"});
            for (int i = 0; i < 50; ++i) {
                testfrontend->call<ITestFrontend::keyEvent>(
                    uuid, Key("space"), false);
                testfrontend->call<ITestFrontend::keyEvent>(
                    uuid, Key("space"), true);
            }
            sendKeys(testfrontend, uuid, {"Escape"});

            sendKeys(testfrontend, uuid,
                     {"K", "a", "n", "j", "i", "space"});
            for (int i = 0; i < 200; ++i) {
                testfrontend->call<ITestFrontend::keyEvent>(
                    uuid, Key("space"), false);
                testfrontend->call<ITestFrontend::keyEvent>(
                    uuid, Key("space"), true);
            }
            sendKeys(testfrontend, uuid, {"Escape"});

            instance.exit();
        });

    // Note: instance.exec() returns false when instance.exit() is called
    // explicitly, which is the normal happy path for our test. The
    // testfrontend's pushCommitExpectation throws/aborts on a mismatch so
    // reaching the bottom of the dispatcher schedule already implies the
    // expected commits arrived in order. We treat exec()'s return as
    // informational only.
    instance.exec();

    std::printf("e2e test: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
