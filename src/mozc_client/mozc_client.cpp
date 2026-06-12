/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mozc IPC client. Talks to a running mozc_server via the vendored
 * commands.proto.
 *
 * Conversion flow per query (no learning):
 *   CREATE_SESSION
 *   SEND_KEY {key_string: <each utf8 char of yomi>}   x N
 *   SEND_KEY {special_key: SPACE}                      -> triggers conversion
 *   -> read Output.all_candidate_words.candidates
 *   SEND_COMMAND {type: RESET_CONTEXT}
 *   DELETE_SESSION
 *
 * We never SUBMIT (which would touch mozc's user_history). All learning is
 * persisted by SKK to its own user dictionary separately.
 *
 * Note: mozc's CandidateWord doesn't expose an internal cost field. The
 * `index` field gives us mozc's ranking order, which is what the merger needs.
 */

#include "mozc_client.h"
#include "ipc_socket.h"
#include "../log/log.h"
#include "../util/utf8.h"
#include "protocol/commands.pb.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <list>
#include <optional>
#include <thread>
#include <unordered_map>

namespace skk_mozc {

namespace mc = mozc::commands;

struct MozcClient::Impl {
    std::vector<uint8_t> socket_address;
    uint64_t session_id = 0;
    bool warned_unavailable = false;

    // LRU cache: list keeps insertion order (front = most recent), map gives
    // O(1) lookup to the list node.
    struct CacheEntry {
        std::string yomi;
        MozcConversionResult result;
        std::chrono::steady_clock::time_point inserted_at;
    };
    std::list<CacheEntry> cache_lru;
    std::unordered_map<std::string,
                       std::list<CacheEntry>::iterator> cache_index;

    void cachePut(const std::string &yomi,
                  const MozcConversionResult &result,
                  size_t capacity) {
        if (capacity == 0) return;
        if (auto it = cache_index.find(yomi); it != cache_index.end()) {
            cache_lru.erase(it->second);
            cache_index.erase(it);
        }
        cache_lru.push_front(
            {yomi, result, std::chrono::steady_clock::now()});
        cache_index[yomi] = cache_lru.begin();
        while (cache_lru.size() > capacity) {
            cache_index.erase(cache_lru.back().yomi);
            cache_lru.pop_back();
        }
    }

    std::optional<MozcConversionResult>
    cacheGet(const std::string &yomi,
             std::chrono::milliseconds ttl) {
        auto it = cache_index.find(yomi);
        if (it == cache_index.end()) return std::nullopt;
        auto age = std::chrono::steady_clock::now() - it->second->inserted_at;
        if (age > ttl) {
            cache_lru.erase(it->second);
            cache_index.erase(it);
            return std::nullopt;
        }
        // Promote to front (LRU touch).
        cache_lru.splice(cache_lru.begin(), cache_lru, it->second);
        it->second = cache_lru.begin();
        return it->second->result;
    }

    std::optional<std::vector<uint8_t>>
    roundtrip(const mc::Input &input, std::chrono::milliseconds timeout) {
        std::string serialized;
        if (!input.SerializeToString(&serialized)) {
            return std::nullopt;
        }
        std::vector<uint8_t> payload(serialized.begin(), serialized.end());
        return ipc::sendRequest(socket_address, payload, timeout);
    }

    std::optional<mc::Output>
    call(const mc::Input &input, std::chrono::milliseconds timeout) {
        auto raw = roundtrip(input, timeout);
        if (!raw) return std::nullopt;
        mc::Output out;
        if (!out.ParseFromArray(raw->data(),
                                static_cast<int>(raw->size()))) {
            return std::nullopt;
        }
        return out;
    }

    // Tear down session `id` (no-op if 0). DELETE_SESSION is the only call
    // that frees mozc-side session state; every teardown path funnels here.
    void deleteSession(uint64_t id, std::chrono::milliseconds timeout) {
        if (id == 0) return;
        mc::Input in;
        in.set_type(mc::Input::DELETE_SESSION);
        in.set_id(id);
        (void)call(in, timeout);
    }

    // Wipe session `id`'s conversion context (no-op if 0). Issued before every
    // teardown so mozc never snapshots an abandoned conversion into its
    // user_history — the project's "no mozc learning" invariant.
    void resetContext(uint64_t id, std::chrono::milliseconds timeout) {
        if (id == 0) return;
        mc::Input in;
        in.set_type(mc::Input::SEND_COMMAND);
        in.set_id(id);
        in.mutable_command()->set_type(mc::SessionCommand::RESET_CONTEXT);
        (void)call(in, timeout);
    }
};

namespace {

using Deadline = std::chrono::steady_clock::time_point;

std::optional<std::chrono::milliseconds>
remainingBudget(Deadline deadline) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return std::nullopt;
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
    if (left.count() <= 0) {
        return std::chrono::milliseconds(1);
    }
    return left;
}

struct SessionTeardown {
    MozcClient::Impl &impl;
    uint64_t id = 0;
    std::chrono::milliseconds timeout;
    bool active = true;

    ~SessionTeardown() {
        if (!active || id == 0) return;
        impl.resetContext(id, timeout);
        impl.deleteSession(id, timeout);
        if (impl.session_id == id) {
            impl.session_id = 0;
        }
    }

    void dismiss() { active = false; }
};

MozcCandidate fromCandidateWord(const mc::CandidateWord &cw,
                                const std::string &yomi) {
    MozcCandidate out;
    out.value = cw.value();
    out.reading = cw.has_key() ? cw.key() : yomi;
    // Use mozc-provided rank as a cost proxy (lower = more likely). The merger
    // only needs ordering, not absolute magnitude.
    out.cost = cw.has_index() ? static_cast<int32_t>(cw.index()) : 0;
    if (cw.has_annotation() && cw.annotation().has_description()) {
        out.description = cw.annotation().description();
    }
    return out;
}

// Mozc's `all_candidate_words` is per-segment (the currently focused one),
// so for multi-bunsetsu input it only shows alternatives for the first
// bunsetsu. The full-sentence conversion lives in `preedit.segment[].value`.
// We prepend the concatenated form so SKK users see "私は学生です" instead
// of just "私は" when they hit SPC on "わたしはがくせいです".
std::string concatenatePreedit(const mc::Output &out) {
    if (!out.has_preedit()) return {};
    std::string s;
    for (int i = 0; i < out.preedit().segment_size(); ++i) {
        s += out.preedit().segment(i).value();
    }
    return s;
}

void extractTopCandidates(const mc::Output &out,
                          int max_candidates,
                          const std::string &yomi,
                          std::vector<MozcCandidate> &dst) {
    std::string full = concatenatePreedit(out);
    if (!full.empty()) {
        MozcCandidate c;
        c.value = std::move(full);
        c.reading = yomi;
        c.cost = -1; // pin to the top: lower index than any per-segment cand
        dst.push_back(std::move(c));
    }
    if (out.has_all_candidate_words()) {
        const auto &all = out.all_candidate_words();
        dst.reserve(dst.size() + static_cast<size_t>(all.candidates_size()));
        for (int i = 0; i < all.candidates_size() &&
                        static_cast<int>(dst.size()) < max_candidates;
             ++i) {
            dst.push_back(fromCandidateWord(all.candidates(i), yomi));
        }
    }
}

void extractSegments(const mc::Output &out,
                     std::vector<MozcSegment> &dst) {
    if (!out.has_preedit()) return;
    const auto &pre = out.preedit();
    for (int i = 0; i < pre.segment_size(); ++i) {
        const auto &seg = pre.segment(i);
        MozcSegment s;
        s.reading = seg.key();
        MozcCandidate c;
        c.value = seg.value();
        c.reading = seg.key();
        s.candidates.push_back(std::move(c));
        s.focused_index = 0;
        dst.push_back(std::move(s));
    }
}

int extractFocusedSegment(const mc::Output &out) {
    if (!out.has_preedit() || !out.preedit().has_highlighted_position()) {
        return -1;
    }
    // mozc's highlighted_position is the segment index of the HIGHLIGHT
    // annotation, which matches what the user sees as the focused bunsetsu.
    int idx = static_cast<int>(out.preedit().highlighted_position());
    if (idx < 0 || idx >= out.preedit().segment_size()) return -1;
    return idx;
}

MozcConversionResult outputToResult(const mc::Output &out,
                                    const std::string &yomi,
                                    int max_candidates) {
    MozcConversionResult r;
    extractTopCandidates(out, max_candidates, yomi, r.top_candidates);
    extractSegments(out, r.segments);
    r.focused_segment = extractFocusedSegment(out);
    return r;
}

bool ensureServerReachable(MozcClient::Impl &impl,
                           const MozcClientOptions &opts) {
    // Re-resolve the socket address each attempt; the rendezvous file changes
    // when mozc_server restarts.
    impl.socket_address = ipc::resolveSocketAddress(opts.socket_path_override);

    // Cheap probe: NO_OPERATION. If unreachable, try to lazy-start once.
    mc::Input ping;
    ping.set_type(mc::Input::NO_OPERATION);
    if (!impl.socket_address.empty() && impl.call(ping, opts.timeout)) {
        return true;
    }
    SKK_MOZC_LOG("mozc_server probe failed (socket %s); lazy-start \"%s\"",
                 impl.socket_address.empty() ? "missing" : "present",
                 opts.mozc_server_path.c_str());
    if (ipc::spawnServer(opts.mozc_server_path)) {
        for (int i = 0; i < 6; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            impl.socket_address =
                ipc::resolveSocketAddress(opts.socket_path_override);
            if (!impl.socket_address.empty() && impl.call(ping, opts.timeout)) {
                SKK_MOZC_LOG("mozc_server reachable after lazy-start");
                return true;
            }
        }
    }
    if (!impl.warned_unavailable) {
        SKK_MOZC_LOG("mozc_server unreachable; falling back to SKK only");
        if (opts.debug) {
            std::fprintf(stderr,
                         "[skk-mozc] mozc_server unreachable; SKK only.\n");
        }
        impl.warned_unavailable = true;
    }
    return false;
}

// Feed yomi into an open session and trigger conversion (SPACE). Returns the
// post-conversion Output, or std::nullopt on IPC failure. The session is LEFT
// OPEN — the caller owns its teardown. Shared by the one-shot convert() and
// the refinement-session bootstrap.
std::optional<mc::Output>
feedYomiAndConvert(MozcClient::Impl &impl,
                   uint64_t session_id,
                   const std::string &yomi,
                   Deadline deadline) {
    bool ok = true;
    skk_mozc::utf8::forEachChar(yomi, [&](std::string_view ch) {
        if (!ok) return;
        auto timeout = remainingBudget(deadline);
        if (!timeout) {
            ok = false;
            return;
        }
        mc::Input k;
        k.set_type(mc::Input::SEND_KEY);
        k.set_id(session_id);
        k.mutable_key()->set_key_string(std::string(ch));
        if (!impl.call(k, *timeout)) ok = false;
    });
    if (!ok) return std::nullopt;
    auto timeout = remainingBudget(deadline);
    if (!timeout) return std::nullopt;
    mc::Input spc;
    spc.set_type(mc::Input::SEND_KEY);
    spc.set_id(session_id);
    spc.mutable_key()->set_special_key(mc::KeyEvent::SPACE);
    return impl.call(spc, *timeout);
}

} // namespace

MozcClient::MozcClient(MozcClientOptions options)
    : impl_(new Impl()), options_(std::move(options)) {
    impl_->socket_address =
        ipc::resolveSocketAddress(options_.socket_path_override);
}

MozcClient::~MozcClient() {
    if (impl_) {
        impl_->resetContext(impl_->session_id, options_.timeout);
        impl_->deleteSession(impl_->session_id, options_.timeout);
        delete impl_;
    }
}

void MozcClient::resetContext() {
    impl_->resetContext(impl_->session_id, options_.timeout);
}

bool MozcClient::reachabilityCoolingDown() {
    if (reachable_) return false;
    auto since = std::chrono::steady_clock::now() - last_unreachable_at_;
    if (since < unreachable_cooldown_) {
        // Still cooling down — short-circuit so we don't pay the probe cost on
        // every keystroke while mozc_server is down.
        return true;
    }
    // Cooldown elapsed: optimistically clear the flag so the caller re-probes.
    // If the probe fails again the cooldown restarts.
    SKK_MOZC_LOG("cooldown elapsed, re-probing mozc_server");
    reachable_ = true;
    return false;
}

bool MozcClient::probeServerReachable() {
    if (ensureServerReachable(*impl_, options_)) return true;
    reachable_ = false;
    last_unreachable_at_ = std::chrono::steady_clock::now();
    return false;
}

std::optional<MozcConversionResult>
MozcClient::convert(const std::string &yomi) {
    if (yomi.empty()) {
        return std::nullopt;
    }
    if (reachabilityCoolingDown()) return std::nullopt;
    SKK_MOZC_LOG("convert: yomi=\"%s\"", yomi.c_str());
    // Cache lookup is intentionally BEFORE the probe: a fresh cached answer
    // can be served even when mozc_server is currently unreachable, so a
    // transient server restart doesn't make recently-typed yomi disappear.
    if (auto cached = impl_->cacheGet(yomi, options_.cache_ttl)) {
        SKK_MOZC_LOG("convert: cache HIT for yomi=\"%s\"", yomi.c_str());
        return cached;
    }
    if (!probeServerReachable()) return std::nullopt;

    auto deadline = std::chrono::steady_clock::now() + options_.timeout;
    auto timeout = remainingBudget(deadline);
    if (!timeout) return std::nullopt;
    mc::Input create;
    create.set_type(mc::Input::CREATE_SESSION);
    auto create_out = impl_->call(create, *timeout);
    if (!create_out || !create_out->has_id()) {
        return std::nullopt;
    }
    impl_->session_id = create_out->id();
    SessionTeardown teardown{*impl_, impl_->session_id, options_.timeout};

    auto convert_out =
        feedYomiAndConvert(*impl_, impl_->session_id, yomi, deadline);
    if (!convert_out) {
        return std::nullopt;
    }

    MozcConversionResult result = outputToResult(
        *convert_out, yomi, options_.max_candidates);

    resetContext();
    impl_->deleteSession(impl_->session_id, options_.timeout);
    teardown.dismiss();
    impl_->session_id = 0;

    if (result.top_candidates.empty() && result.segments.empty()) {
        return std::nullopt;
    }
    impl_->cachePut(yomi, result, options_.cache_capacity);
    return result;
}

std::unique_ptr<RefinementSession>
MozcClient::beginRefinement(const std::string &yomi) {
    if (yomi.empty()) return nullptr;
    if (reachabilityCoolingDown()) return nullptr;
    if (!probeServerReachable()) return nullptr;
    auto deadline = std::chrono::steady_clock::now() + options_.timeout;
    auto timeout = remainingBudget(deadline);
    if (!timeout) return nullptr;
    mc::Input create;
    create.set_type(mc::Input::CREATE_SESSION);
    auto create_out = impl_->call(create, *timeout);
    if (!create_out || !create_out->has_id()) return nullptr;
    uint64_t sid = create_out->id();
    SessionTeardown teardown{*impl_, sid, options_.timeout};
    auto first = feedYomiAndConvert(*impl_, sid, yomi, deadline);
    if (!first) {
        return nullptr;
    }
    auto state = outputToResult(*first, yomi, options_.max_candidates);
    teardown.dismiss();
    return std::unique_ptr<RefinementSession>(
        new RefinementSession(impl_, sid, options_, std::move(state)));
}

RefinementSession::RefinementSession(MozcClient::Impl *impl,
                                     uint64_t session_id,
                                     MozcClientOptions options,
                                     MozcConversionResult initial)
    : impl_(impl), session_id_(session_id), options_(std::move(options)),
      current_(std::move(initial)) {}

RefinementSession::~RefinementSession() {
    if (dead_ || !impl_) return;
    // RESET_CONTEXT first so mozc doesn't snapshot the abandoned conversion
    // into its user_history, then DELETE_SESSION.
    impl_->resetContext(session_id_, options_.timeout);
    impl_->deleteSession(session_id_, options_.timeout);
}

namespace {

std::optional<MozcConversionResult>
sendSpecial(MozcClient::Impl &impl, uint64_t sid,
            const MozcClientOptions &opts,
            mc::KeyEvent::SpecialKey key,
            std::initializer_list<mc::KeyEvent::ModifierKey> mods,
            MozcConversionResult &cache) {
    mc::Input in;
    in.set_type(mc::Input::SEND_KEY);
    in.set_id(sid);
    auto *k = in.mutable_key();
    k->set_special_key(key);
    for (auto m : mods) k->add_modifier_keys(m);
    auto out = impl.call(in, opts.timeout);
    if (!out) return std::nullopt;
    std::string yomi;
    if (!cache.top_candidates.empty()) {
        yomi = cache.top_candidates.front().reading;
    }
    cache = outputToResult(*out, yomi, opts.max_candidates);
    return cache;
}

} // namespace

std::optional<MozcConversionResult>
RefinementSession::shrinkFocusedSegment() {
    if (dead_) return std::nullopt;
    return sendSpecial(*impl_, session_id_, options_,
                       mc::KeyEvent::LEFT, {mc::KeyEvent::SHIFT}, current_);
}

std::optional<MozcConversionResult>
RefinementSession::growFocusedSegment() {
    if (dead_) return std::nullopt;
    return sendSpecial(*impl_, session_id_, options_,
                       mc::KeyEvent::RIGHT, {mc::KeyEvent::SHIFT}, current_);
}

std::optional<MozcConversionResult>
RefinementSession::focusNextSegment() {
    if (dead_) return std::nullopt;
    return sendSpecial(*impl_, session_id_, options_,
                       mc::KeyEvent::RIGHT, {}, current_);
}

std::optional<MozcConversionResult>
RefinementSession::focusPrevSegment() {
    if (dead_) return std::nullopt;
    return sendSpecial(*impl_, session_id_, options_,
                       mc::KeyEvent::LEFT, {}, current_);
}

std::optional<MozcConversionResult>
RefinementSession::nextCandidate() {
    if (dead_) return std::nullopt;
    return sendSpecial(*impl_, session_id_, options_,
                       mc::KeyEvent::SPACE, {}, current_);
}

std::optional<MozcConversionResult>
RefinementSession::prevCandidate() {
    if (dead_) return std::nullopt;
    return sendSpecial(*impl_, session_id_, options_,
                       mc::KeyEvent::UP, {}, current_);
}

} // namespace skk_mozc
