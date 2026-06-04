/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "refiner.h"

#include <algorithm>

namespace skk_mozc {

namespace {

// UTF-8 character count for a byte string. Treats invalid bytes as 1 char.
int utf8CharLen(const std::string &s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t step = 1;
        if ((c & 0x80) == 0)        step = 1;
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        i += step;
        ++n;
    }
    return n;
}

} // namespace

Refiner::Refiner(std::shared_ptr<MozcClient> client,
                 MozcConversionResult initial,
                 std::string original_yomi)
    : client_(std::move(client)),
      original_yomi_(std::move(original_yomi)),
      state_(std::move(initial)) {}

Refiner::~Refiner() = default;

bool Refiner::dispatch(RefinerAction action) {
    if (done_) return false;
    switch (action) {
    case RefinerAction::ShrinkSegment:
        resize(-1);
        return true;
    case RefinerAction::GrowSegment:
        resize(+1);
        return true;
    case RefinerAction::FocusNextSegment:
        moveFocus(+1);
        return true;
    case RefinerAction::FocusPrevSegment:
        moveFocus(-1);
        return true;
    case RefinerAction::NextCandidate:
        cycleCandidate(focused_segment_, +1);
        return true;
    case RefinerAction::PrevCandidate:
        cycleCandidate(focused_segment_, -1);
        return true;
    case RefinerAction::Commit:
        done_ = true;
        return true;
    case RefinerAction::Abort:
        done_ = true;
        aborted_ = true;
        return true;
    }
    return false;
}

void Refiner::cycleCandidate(int segment_index, int delta) {
    if (segment_index < 0 ||
        segment_index >= static_cast<int>(state_.segments.size())) {
        return;
    }
    auto &seg = state_.segments[segment_index];
    if (seg.candidates.empty()) {
        // We didn't preload alternatives; lazy-fetch via the client.
        auto fresh = client_->moveSegmentCandidate(segment_index, delta);
        if (fresh) {
            state_ = std::move(*fresh);
        }
        return;
    }
    int n = static_cast<int>(seg.candidates.size());
    seg.focused_index = ((seg.focused_index + delta) % n + n) % n;
}

void Refiner::resize(int delta_chars) {
    auto fresh = client_->resizeSegment(focused_segment_, delta_chars);
    if (fresh) {
        state_ = std::move(*fresh);
        // Focus may have collapsed if we shrunk past the start; clamp.
        if (focused_segment_ >=
            static_cast<int>(state_.segments.size())) {
            focused_segment_ = std::max(
                0, static_cast<int>(state_.segments.size()) - 1);
        }
    }
}

void Refiner::moveFocus(int delta) {
    int n = static_cast<int>(state_.segments.size());
    if (n == 0) return;
    focused_segment_ = ((focused_segment_ + delta) % n + n) % n;
}

RefinerView Refiner::view() const {
    RefinerView v;
    int cursor = 0;
    for (int i = 0; i < static_cast<int>(state_.segments.size()); ++i) {
        const auto &seg = state_.segments[i];
        const std::string &value =
            seg.candidates.empty()
                ? std::string()
                : seg.candidates[std::clamp(
                      seg.focused_index, 0,
                      static_cast<int>(seg.candidates.size()) - 1)]
                      .value;
        int len_chars = utf8CharLen(value);
        if (i == focused_segment_) {
            v.focused_begin_chars = cursor;
            v.focused_end_chars = cursor + len_chars;
        }
        v.preedit_text += value;
        cursor += len_chars;
        v.segment_boundaries.push_back(cursor);
    }
    return v;
}

std::optional<RefinerCommit> Refiner::commit() {
    if (!done_ || aborted_) {
        return std::nullopt;
    }
    RefinerCommit c;
    for (const auto &seg : state_.segments) {
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
