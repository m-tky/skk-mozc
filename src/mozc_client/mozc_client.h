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

class RefinementSession;

class MozcClient {
public:
    explicit MozcClient(MozcClientOptions options = {});
    ~MozcClient();

    MozcClient(const MozcClient &) = delete;
    MozcClient &operator=(const MozcClient &) = delete;

    // One-shot conversion. Creates a fresh session, sends the yomi + SPACE,
    // extracts candidates, tears down. Use this for the initial ▽ SPC.
    std::optional<MozcConversionResult> convert(const std::string &yomi);

    // Open a session that stays alive while the user refines the conversion.
    // The session holds mozc-side state (current segment focus, candidate
    // cursors) across multiple operations. Closing it issues RESET_CONTEXT so
    // no learning is recorded.
    //
    // Returns nullptr on failure (caller falls back to the one-shot result).
    std::unique_ptr<RefinementSession>
    beginRefinement(const std::string &yomi);

    // Discard server-side state. Safe to call at any time.
    void resetContext();

    bool reachable() const { return reachable_; }

    // Implementation handle. Public so the .cpp anonymous-namespace helpers
    // can touch it; do not reference from outside the .cpp file.
    struct Impl;

    const MozcClientOptions &options() const { return options_; }

private:
    Impl *impl_;
    MozcClientOptions options_;
    bool reachable_ = true;
};

// Live, stateful conversation with mozc_server during the refinement sub-mode.
// All methods return the post-action conversion result; std::nullopt means
// the IPC failed and the caller should abort refinement.
class RefinementSession {
public:
    ~RefinementSession();

    RefinementSession(const RefinementSession &) = delete;
    RefinementSession &operator=(const RefinementSession &) = delete;

    // Refinement actions. Internally these send SEND_KEY with the appropriate
    // (special_key, modifier_keys) tuple that mozc treats as the same action
    // a user would trigger from the keyboard.
    std::optional<MozcConversionResult> shrinkFocusedSegment();
    std::optional<MozcConversionResult> growFocusedSegment();
    std::optional<MozcConversionResult> focusNextSegment();
    std::optional<MozcConversionResult> focusPrevSegment();
    std::optional<MozcConversionResult> nextCandidate();
    std::optional<MozcConversionResult> prevCandidate();

    // Most recent conversion result (post last action).
    const MozcConversionResult &current() const { return current_; }

private:
    friend class MozcClient;
    RefinementSession(MozcClient::Impl *impl, uint64_t session_id,
                      MozcClientOptions options,
                      MozcConversionResult initial);

    MozcClient::Impl *impl_;
    uint64_t session_id_;
    MozcClientOptions options_;
    MozcConversionResult current_;
    bool dead_ = false;
};

} // namespace skk_mozc

#endif // FCITX5_SKK_MOZC_CLIENT_H_
