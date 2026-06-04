/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * UNIX socket framing for talking to mozc_server.
 *
 * Mozc's IPC framing (see mozc/src/ipc/unix_ipc.cc):
 *   uint32_t  payload_size  (little-endian)
 *   uint8_t[] payload        (protobuf-serialized commands.Input/Output)
 *
 * mozc_server creates its socket at
 *   $XDG_RUNTIME_DIR/.mozc.<uid>.session   (Linux, modern)
 * with a fallback to /tmp/.<uid>.mozc/.mozc.<uid>.session on older mozc.
 * The first reachable path wins.
 */

#ifndef FCITX5_SKK_MOZC_IPC_SOCKET_H_
#define FCITX5_SKK_MOZC_IPC_SOCKET_H_

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skk_mozc::ipc {

// Resolve the UNIX socket path mozc_server is expected to listen on.
// Returns the first existing candidate path, or the preferred default if none
// exists yet (so the caller can try to connect and trigger lazy start).
std::string resolveSocketPath(const std::string &override_path = "");

// One-shot blocking request. Connects, sends `payload`, reads a response,
// closes. Returns the response bytes on success, std::nullopt on
// timeout/error. The total timeout covers connect + send + recv.
std::optional<std::vector<uint8_t>>
sendRequest(const std::string &socket_path,
            const std::vector<uint8_t> &payload,
            std::chrono::milliseconds timeout);

// Attempt to spawn mozc_server in the background. Returns true if the spawn
// syscall succeeded (does NOT wait for the server to be ready). Idempotent:
// safe to call when a server is already running.
bool spawnServer(const std::string &mozc_server_path);

} // namespace skk_mozc::ipc

#endif // FCITX5_SKK_MOZC_IPC_SOCKET_H_
