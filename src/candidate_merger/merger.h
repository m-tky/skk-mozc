/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ranking decisions (locked in the design interview):
 *   1. Entries that hit the SKK *personal* dict come first, in SKK order.
 *      Rationale: SKK already moves recently-used items to the head of the
 *      personal dict, so this slot reflects the user's own preference.
 *   2. The rest of the SKK system-dict entries + mozc candidates are merged
 *      by mozc cost, deduplicated by surface form. SKK system entries with
 *      no mozc counterpart are placed *after* the top N mozc entries so that
 *      mozc's likely candidates are not buried by SKK's rare-character
 *      entries.
 *   3. No visual marker for source: once SKK personal dict learns a former
 *      mozc candidate, the user's experience should be continuous.
 */

#ifndef FCITX5_SKK_MOZC_MERGER_H_
#define FCITX5_SKK_MOZC_MERGER_H_

#include "../mozc_client/mozc_client.h"

#include <string>
#include <vector>

namespace skk_mozc {

// One entry from the SKK side, classified by source.
struct SkkSideCandidate {
    std::string value;        // surface form, UTF-8
    std::string annotation;   // optional ;-prefixed annotation, no leading ';'
    bool from_personal_dict;  // true => slot 1 priority
};

struct MergedCandidate {
    std::string value;
    std::string annotation;
    // Combined ranking signal for diagnostic logging only; the merger emits
    // candidates already in display order.
    int32_t rank_hint = 0;
};

struct MergeInputs {
    // SKK candidates, in libskk-provided order. The merger trusts that ones
    // marked `from_personal_dict` are already in user-preference order.
    std::vector<SkkSideCandidate> skk_candidates;
    // Mozc candidates, in mozc-cost order (lowest cost first).
    std::vector<MozcCandidate> mozc_candidates;
    // How many top mozc candidates are protected from being overtaken by
    // SKK-only system-dict entries.
    int mozc_protect_top = 5;
};

std::vector<MergedCandidate> mergeCandidates(const MergeInputs &inputs);

} // namespace skk_mozc

#endif // FCITX5_SKK_MOZC_MERGER_H_
