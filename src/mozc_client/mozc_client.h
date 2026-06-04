/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Thin client over mozc_server's UNIX socket IPC.
 *
 * Design constraints (decided in design interview):
 * - We never SUBMIT to mozc; all learning lives in SKK's personal dict.
 *   Each query creates a session, asks for conversion, extracts candidates,
 *   sends RESET_CONTEXT, and deletes the session.
 * - 50 ms sync timeout for the whole roundtrip. On timeout/error the caller
 *   falls back to pure SKK candidates.
 * - Lazy-start mozc_server: if the socket is not connectable, fork the server
 *   binary once and retry briefly.
 */

#ifndef FCITX5_SKK_MOZC_CLIENT_H_
#define FCITX5_SKK_MOZC_CLIENT_H_

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skk_mozc {

struct MozcCandidate {
    // Surface form (e.g. "朝日新聞"). UTF-8.
    std::string value;
    // Reading the candidate was generated from (e.g. "あさひしんぶん").
    // For multi-bunsetsu results, this is the full reading.
    std::string reading;
    // Mozc internal cost. Lower = more likely. We expose it so the merger can
    // sort across sources without doing a second query.
    int32_t cost = 0;
    // Description / annotation (e.g. "[名詞]"). Empty if unavailable.
    std::string description;
};

struct MozcSegment {
    // Reading of this bunsetsu (e.g. "あさひ").
    std::string reading;
    // All candidates for this segment, ordered by mozc preference.
    std::vector<MozcCandidate> candidates;
    // Index of the currently focused candidate within `candidates`.
    int32_t focused_index = 0;
};

struct MozcConversionResult {
    // Top-level merged candidate list (concatenation of focused segment values).
    // This is what we surface in the "first SPC" SKK candidate list.
    std::vector<MozcCandidate> top_candidates;
    // The bunsetsu breakdown. Used by the refinement sub-mode.
    std::vector<MozcSegment> segments;
};

struct MozcClientOptions {
    // 50 ms by default (set in the interview). Whole-request budget.
    std::chrono::milliseconds timeout = std::chrono::milliseconds(50);
    // Cap candidates we accept per query; mozc may return many more.
    int max_candidates = 20;
    // Path to mozc_server binary. Empty = look up in PATH.
    std::string mozc_server_path;
    // Optional override for the IPC socket path. Empty = default.
    std::string socket_path_override;
    bool debug = false;
};

class MozcClient {
public:
    explicit MozcClient(MozcClientOptions options = {});
    ~MozcClient();

    MozcClient(const MozcClient &) = delete;
    MozcClient &operator=(const MozcClient &) = delete;

    // Convert a yomi (hiragana) to a structured candidate list.
    // Returns std::nullopt on timeout / IPC failure (caller should fall back).
    std::optional<MozcConversionResult> convert(const std::string &yomi);

    // Apply a structural edit to the previous conversion result and return
    // a fresh structured result. These power the refinement sub-mode keys.
    //
    // segment_index is the focused bunsetsu (0-based).
    // For resize: delta < 0 shrinks, delta > 0 grows (in characters).
    std::optional<MozcConversionResult>
    resizeSegment(int segment_index, int delta);

    // Move the focused candidate within a segment.
    std::optional<MozcConversionResult>
    moveSegmentCandidate(int segment_index, int delta);

    // Discard server-side state (per-query convention). Safe to call at any
    // time; failures here are not surfaced.
    void resetContext();

    // True if the last attempt got past the connect step. Used by the
    // integration layer to log "mozc unavailable" once per session.
    bool reachable() const { return reachable_; }

    // Public so that helper functions in the .cpp anonymous namespace can
    // touch it. Treat it as private; never reference it from outside the
    // .cpp file.
    struct Impl;

private:
    Impl *impl_;
    MozcClientOptions options_;
    bool reachable_ = true;
};

} // namespace skk_mozc

#endif // FCITX5_SKK_MOZC_CLIENT_H_
