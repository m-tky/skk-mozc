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
#include "protocol/commands.pb.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace skk_mozc {

namespace mc = mozc::commands;

struct MozcClient::Impl {
    std::string socket_path;
    uint64_t session_id = 0;
    bool warned_unavailable = false;

    std::optional<std::vector<uint8_t>>
    roundtrip(const mc::Input &input, std::chrono::milliseconds timeout) {
        std::string serialized;
        if (!input.SerializeToString(&serialized)) {
            return std::nullopt;
        }
        std::vector<uint8_t> payload(serialized.begin(), serialized.end());
        return ipc::sendRequest(socket_path, payload, timeout);
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

void extractTopCandidates(const mc::Output &out,
                          int max_candidates,
                          const std::string &yomi,
                          std::vector<MozcCandidate> &dst) {
    if (!out.has_all_candidate_words()) {
        return;
    }
    const auto &all = out.all_candidate_words();
    dst.reserve(static_cast<size_t>(all.candidates_size()));
    for (int i = 0; i < all.candidates_size() &&
                    static_cast<int>(dst.size()) < max_candidates;
         ++i) {
        dst.push_back(fromCandidateWord(all.candidates(i), yomi));
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

bool ensureServerReachable(MozcClient::Impl &impl,
                           const MozcClientOptions &opts) {
    // Cheap probe: NO_OPERATION. If unreachable, try to lazy-start once.
    mc::Input ping;
    ping.set_type(mc::Input::NO_OPERATION);
    if (impl.call(ping, opts.timeout)) {
        return true;
    }
    if (ipc::spawnServer(opts.mozc_server_path)) {
        for (int i = 0; i < 6; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (impl.call(ping, opts.timeout)) return true;
        }
    }
    if (!impl.warned_unavailable && opts.debug) {
        std::fprintf(stderr,
                     "[skk-mozc] mozc_server unreachable; SKK only.\n");
        impl.warned_unavailable = true;
    }
    return false;
}

} // namespace

MozcClient::MozcClient(MozcClientOptions options)
    : impl_(new Impl()), options_(std::move(options)) {
    impl_->socket_path = ipc::resolveSocketPath(options_.socket_path_override);
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

    MozcConversionResult result;
    extractTopCandidates(*convert_out, options_.max_candidates, yomi,
                         result.top_candidates);
    extractSegments(*convert_out, result.segments);

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

std::optional<MozcConversionResult>
MozcClient::resizeSegment(int segment_index, int delta) {
    // Refinement mode needs its own live session; v0 stub.
    (void)segment_index; (void)delta;
    return std::nullopt;
}

std::optional<MozcConversionResult>
MozcClient::moveSegmentCandidate(int segment_index, int delta) {
    (void)segment_index; (void)delta;
    return std::nullopt;
}

} // namespace skk_mozc
