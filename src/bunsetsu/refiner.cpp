/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "refiner.h"

#include "../util/utf8.h"

#include <algorithm>

namespace skk_mozc {

namespace {

int focusedSegmentIndex(const MozcConversionResult &state) {
    // mozc reports the highlighted bunsetsu directly; fall back to 0 if not
    // available (e.g., when the conversion hasn't selected one yet).
    if (state.focused_segment >= 0) return state.focused_segment;
    return 0;
}

} // namespace

Refiner::Refiner(std::unique_ptr<RefinementSession> session,
                 std::string original_yomi)
    : session_(std::move(session)),
      original_yomi_(std::move(original_yomi)) {}

Refiner::~Refiner() = default;

bool Refiner::dispatch(RefinerAction action) {
    if (done_ || !session_) return false;
    std::optional<MozcConversionResult> next;
    switch (action) {
    case RefinerAction::ShrinkSegment:
        next = session_->shrinkFocusedSegment();
        break;
    case RefinerAction::GrowSegment:
        next = session_->growFocusedSegment();
        break;
    case RefinerAction::FocusNextSegment:
        next = session_->focusNextSegment();
        break;
    case RefinerAction::FocusPrevSegment:
        next = session_->focusPrevSegment();
        break;
    case RefinerAction::NextCandidate:
        next = session_->nextCandidate();
        break;
    case RefinerAction::PrevCandidate:
        next = session_->prevCandidate();
        break;
    case RefinerAction::Commit:
        done_ = true;
        return true;
    case RefinerAction::Abort:
        done_ = true;
        aborted_ = true;
        return true;
    }
    if (!next) {
        // IPC failure mid-refinement: treat as abort so the caller falls back
        // to the SKK ▽ state.
        done_ = true;
        aborted_ = true;
    }
    return true;
}

RefinerView Refiner::view() const {
    RefinerView v;
    if (!session_) return v;
    const auto &state = session_->current();
    int cursor = 0;
    int focused = focusedSegmentIndex(state);
    for (int i = 0; i < static_cast<int>(state.segments.size()); ++i) {
        const auto &seg = state.segments[i];
        const std::string &value =
            seg.candidates.empty()
                ? std::string()
                : seg.candidates[std::clamp(
                      seg.focused_index, 0,
                      static_cast<int>(seg.candidates.size()) - 1)]
                      .value;
        int len_chars = utf8::countChars(value);
        if (i == focused) {
            v.focused_begin_chars = cursor;
            v.focused_end_chars = cursor + len_chars;
        }
        v.preedit_text += value;
        cursor += len_chars;
        v.segment_boundaries.push_back(cursor);
    }
    return v;
}

const MozcConversionResult &Refiner::currentResult() const {
    return session_->current();
}

std::optional<RefinerCommit> Refiner::commit() {
    if (!done_ || aborted_ || !session_) return std::nullopt;
    RefinerCommit c;
    const auto &state = session_->current();
    for (const auto &seg : state.segments) {
        if (seg.candidates.empty()) continue;
        const auto &winner = seg.candidates[
            std::clamp(seg.focused_index, 0,
                       static_cast<int>(seg.candidates.size()) - 1)];
        c.text += winner.value;
        c.learn_entries.emplace_back(seg.reading, winner.value);
    }
    return c;
}

} // namespace skk_mozc
