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
 * persisted by SKK to ~/.skk-jisyo separately.
 *
 * Note: mozc's CandidateWord doesn't expose an internal cost field. The
 * `index` field gives us mozc's ranking order, which is what the merger needs.
 */

#include "mozc_client.h"
#include "ipc_socket.h"
#include "../log/log.h"
#include "protocol/commands.pb.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace skk_mozc {

namespace mc = mozc::commands;

struct MozcClient::Impl {
    std::vector<uint8_t> socket_address;
    uint64_t session_id = 0;
    bool warned_unavailable = false;

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
};

namespace {

// Iterate UTF-8 characters of a string, invoking `f(char_view)` for each.
template <typename F>
void forEachUtf8Char(const std::string &s, F &&f) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t step = 1;
        if ((c & 0x80) == 0)        step = 1;
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        if (i + step > s.size()) break;
        f(std::string_view(s).substr(i, step));
        i += step;
    }
}

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

} // namespace

MozcClient::MozcClient(MozcClientOptions options)
    : impl_(new Impl()), options_(std::move(options)) {
    impl_->socket_address =
        ipc::resolveSocketAddress(options_.socket_path_override);
}

MozcClient::~MozcClient() {
    if (impl_) {
        if (impl_->session_id != 0) {
            mc::Input in;
            in.set_type(mc::Input::DELETE_SESSION);
            in.set_id(impl_->session_id);
            (void)impl_->call(in, options_.timeout);
        }
        delete impl_;
    }
}

void MozcClient::resetContext() {
    if (impl_->session_id == 0) return;
    mc::Input in;
    in.set_type(mc::Input::SEND_COMMAND);
    in.set_id(impl_->session_id);
    in.mutable_command()->set_type(mc::SessionCommand::RESET_CONTEXT);
    (void)impl_->call(in, options_.timeout);
}

std::optional<MozcConversionResult>
MozcClient::convert(const std::string &yomi) {
    if (yomi.empty() || !reachable_) {
        return std::nullopt;
    }
    if (!ensureServerReachable(*impl_, options_)) {
        reachable_ = false;
        return std::nullopt;
    }

    mc::Input create;
    create.set_type(mc::Input::CREATE_SESSION);
    auto create_out = impl_->call(create, options_.timeout);
    if (!create_out || !create_out->has_id()) {
        return std::nullopt;
    }
    impl_->session_id = create_out->id();

    // Feed each kana character via SEND_KEY {key_string: ...}. Mozc treats
    // key_string as direct character insertion into the composition buffer.
    bool feed_ok = true;
    forEachUtf8Char(yomi, [&](std::string_view ch) {
        if (!feed_ok) return;
        mc::Input k;
        k.set_type(mc::Input::SEND_KEY);
        k.set_id(impl_->session_id);
        k.mutable_key()->set_key_string(std::string(ch));
        if (!impl_->call(k, options_.timeout)) feed_ok = false;
    });
    if (!feed_ok) {
        impl_->session_id = 0;
        return std::nullopt;
    }

    // SPACE triggers conversion in mozc's session state machine.
    mc::Input spc;
    spc.set_type(mc::Input::SEND_KEY);
    spc.set_id(impl_->session_id);
    spc.mutable_key()->set_special_key(mc::KeyEvent::SPACE);
    auto convert_out = impl_->call(spc, options_.timeout);
    if (!convert_out) {
        impl_->session_id = 0;
        return std::nullopt;
    }

    MozcConversionResult result = outputToResult(
        *convert_out, yomi, options_.max_candidates);

    resetContext();
    {
        mc::Input del;
        del.set_type(mc::Input::DELETE_SESSION);
        del.set_id(impl_->session_id);
        (void)impl_->call(del, options_.timeout);
    }
    impl_->session_id = 0;

    if (result.top_candidates.empty() && result.segments.empty()) {
        return std::nullopt;
    }
    return result;
}

namespace {

// Feed yomi into a live session and trigger conversion. Returns the
// post-conversion Output, or std::nullopt on IPC failure. The session is
// LEFT OPEN — caller owns it.
std::optional<mc::Output>
feedYomiAndConvert(MozcClient::Impl &impl,
                   uint64_t session_id,
                   const std::string &yomi,
                   std::chrono::milliseconds timeout) {
    bool ok = true;
    forEachUtf8Char(yomi, [&](std::string_view ch) {
        if (!ok) return;
        mc::Input k;
        k.set_type(mc::Input::SEND_KEY);
        k.set_id(session_id);
        k.mutable_key()->set_key_string(std::string(ch));
        if (!impl.call(k, timeout)) ok = false;
    });
    if (!ok) return std::nullopt;
    mc::Input spc;
    spc.set_type(mc::Input::SEND_KEY);
    spc.set_id(session_id);
    spc.mutable_key()->set_special_key(mc::KeyEvent::SPACE);
    return impl.call(spc, timeout);
}

} // namespace

std::unique_ptr<RefinementSession>
MozcClient::beginRefinement(const std::string &yomi) {
    if (yomi.empty() || !reachable_) return nullptr;
    if (!ensureServerReachable(*impl_, options_)) {
        reachable_ = false;
        return nullptr;
    }
    mc::Input create;
    create.set_type(mc::Input::CREATE_SESSION);
    auto create_out = impl_->call(create, options_.timeout);
    if (!create_out || !create_out->has_id()) return nullptr;
    uint64_t sid = create_out->id();
    auto first = feedYomiAndConvert(*impl_, sid, yomi, options_.timeout);
    if (!first) {
        mc::Input del;
        del.set_type(mc::Input::DELETE_SESSION);
        del.set_id(sid);
        (void)impl_->call(del, options_.timeout);
        return nullptr;
    }
    auto state = outputToResult(*first, yomi, options_.max_candidates);
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
    {
        mc::Input rc;
        rc.set_type(mc::Input::SEND_COMMAND);
        rc.set_id(session_id_);
        rc.mutable_command()->set_type(mc::SessionCommand::RESET_CONTEXT);
        (void)impl_->call(rc, options_.timeout);
    }
    {
        mc::Input del;
        del.set_type(mc::Input::DELETE_SESSION);
        del.set_id(session_id_);
        (void)impl_->call(del, options_.timeout);
    }
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
