#!/usr/bin/env bash
#
# Scenario-level tests that invoke a real mozc_server through the standalone
# mozc-client-cli. These complement the pure unit tests in
# panel_dispatch_test.cpp by checking the data side of the integration:
# does mozc actually return reasonable candidates for the kinds of inputs
# our users care about?
#
# Usage:
#   nix build .#mozc-client-cli
#   ./tests/scenarios.sh ./result/bin/mozc-client-cli
#
# Pre-requisites: mozc_server reachable (e.g. via SKK_MOZC_MOZC_SERVER env
# or one already running via fcitx5-daemon).

set -uo pipefail

CLI="${1:-./result/bin/mozc-client-cli}"
[[ -x "$CLI" ]] || { echo "missing CLI binary: $CLI" >&2; exit 2; }

pass=0
fail=0

# expect_contains <name> <yomi> <expected-substring-in-first-output>
expect_contains() {
    local name="$1"
    local yomi="$2"
    local expected="$3"
    local got
    got=$("$CLI" "$yomi" 2>/dev/null | head -1 | cut -f1)
    if [[ "$got" == *"$expected"* ]]; then
        printf '[PASS] %-40s yomi=%q -> %q\n' "$name" "$yomi" "$got"
        pass=$((pass + 1))
    else
        printf '[FAIL] %-40s yomi=%q -> %q (expected substring %q)\n' \
            "$name" "$yomi" "$got" "$expected"
        fail=$((fail + 1))
    fi
}

# expect_any <name> <yomi>
# Passes if mozc returned at least one candidate.
expect_any() {
    local name="$1"
    local yomi="$2"
    local count
    count=$("$CLI" "$yomi" 2>/dev/null | wc -l)
    if (( count > 0 )); then
        printf '[PASS] %-40s yomi=%q -> %d candidates\n' "$name" "$yomi" "$count"
        pass=$((pass + 1))
    else
        printf '[FAIL] %-40s yomi=%q -> no candidates\n' "$name" "$yomi"
        fail=$((fail + 1))
    fi
}

echo "== Single-bunsetsu compound nouns =="
expect_contains "Asahi Shimbun"          "あさひしんぶん"          "朝日新聞"
expect_contains "Ministry of Economy"    "けいざいさんぎょうしょう" "経済産業省"
expect_contains "Sashimi knife"          "さしみほうちょう"        "刺"
expect_contains "Tomorrow's weather"     "あすのてんき"           "明日"

echo ""
echo "== Multi-bunsetsu sentences =="
expect_contains "I am a student"         "わたしはがくせいです"    "私"
expect_contains "Today's weather"        "きょうのてんき"         "今日"

echo ""
echo "== Common single nouns (every IME should hit) =="
expect_any "ありがとう"  "ありがとう"
expect_any "じかん"     "じかん"
expect_any "しごと"     "しごと"

echo ""
echo "$pass passed, $fail failed"
exit $(( fail > 0 ? 1 : 0 ))
