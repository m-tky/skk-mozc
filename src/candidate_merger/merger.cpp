/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "merger.h"

#include <algorithm>
#include <unordered_set>

namespace skk_mozc {

namespace {

void pushUnique(std::vector<MergedCandidate> &dst,
                std::unordered_set<std::string> &seen,
                MergedCandidate item) {
    if (seen.insert(item.value).second) {
        dst.push_back(std::move(item));
    }
}

} // namespace

std::vector<MergedCandidate> mergeCandidates(const MergeInputs &inputs) {
    std::vector<MergedCandidate> out;
    std::unordered_set<std::string> seen;
    out.reserve(inputs.skk_candidates.size() + inputs.mozc_candidates.size());

    // Slot 1: personal-dict hits, in their given order.
    int rank = 0;
    for (const auto &c : inputs.skk_candidates) {
        if (!c.from_personal_dict) continue;
        MergedCandidate m;
        m.value = c.value;
        m.annotation = c.annotation;
        m.rank_hint = rank++;
        pushUnique(out, seen, std::move(m));
    }

    // Slot 2: top-N mozc candidates, in mozc cost order.
    int mozc_taken = 0;
    for (const auto &mc : inputs.mozc_candidates) {
        if (mozc_taken >= inputs.mozc_protect_top) break;
        MergedCandidate m;
        m.value = mc.value;
        m.annotation = mc.description;
        m.rank_hint = rank++;
        if (seen.count(m.value)) {
            ++mozc_taken;
            continue;
        }
        out.push_back(std::move(m));
        seen.insert(out.back().value);
        ++mozc_taken;
    }

    // Slot 3: remaining mozc + SKK-system entries, interleaved.
    //   - Iterate mozc in cost order and skk-system in libskk order.
    //   - Emit them round-robin so neither completely buries the other.
    //   This is a deliberately simple rule; the personal-dict learning loop
    //   makes the long tail self-correct.
    std::vector<MozcCandidate> mozc_rest;
    if (static_cast<int>(inputs.mozc_candidates.size()) > inputs.mozc_protect_top) {
        mozc_rest.assign(
            inputs.mozc_candidates.begin() + inputs.mozc_protect_top,
            inputs.mozc_candidates.end());
    }
    std::vector<const SkkSideCandidate *> skk_system;
    for (const auto &c : inputs.skk_candidates) {
        if (!c.from_personal_dict) skk_system.push_back(&c);
    }

    size_t i = 0, j = 0;
    while (i < mozc_rest.size() || j < skk_system.size()) {
        if (i < mozc_rest.size()) {
            const auto &mc = mozc_rest[i++];
            if (!seen.count(mc.value)) {
                MergedCandidate m;
                m.value = mc.value;
                m.annotation = mc.description;
                m.rank_hint = rank++;
                out.push_back(std::move(m));
                seen.insert(out.back().value);
            }
        }
        if (j < skk_system.size()) {
            const auto *c = skk_system[j++];
            if (!seen.count(c->value)) {
                MergedCandidate m;
                m.value = c->value;
                m.annotation = c->annotation;
                m.rank_hint = rank++;
                out.push_back(std::move(m));
                seen.insert(out.back().value);
            }
        }
    }

    return out;
}

} // namespace skk_mozc
