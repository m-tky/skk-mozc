/*
 * SPDX-FileCopyrightText: 2026 fcitx5-skk-mozc contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * UNIX socket transport for talking to mozc_server.
 *
 * The mozc protocol is more peculiar than a typical RPC:
 *   1. On Linux the server binds an *abstract* UNIX socket whose name is
 *      derived from a key the server publishes in a rendezvous file
 *      ($XDG_CONFIG_HOME/mozc/.session.ipc), serialized as a mozc.ipc.IPCPathInfo
 *      protobuf message.
 *   2. Once connected, the client sends its serialized request, then
 *      shutdown(SHUT_WR) to mark end-of-message. The server reads until EOF,
 *      processes, and writes back, then closes its write side.
 *   3. There is no length prefix on the wire.
 *
 * See mozc/src/ipc/{ipc_path_manager,unix_ipc}.{h,cc} for the canonical
 * implementation.
 */

#ifndef FCITX5_SKK_MOZC_IPC_SOCKET_H_
#define FCITX5_SKK_MOZC_IPC_SOCKET_H_

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skk_mozc::ipc {

// Resolve mozc_server's session socket. The returned bytes are the contents
// of sun_path; they may legitimately start with a NUL byte (Linux abstract
// socket) and are not null-terminated.
//
// On error returns an empty vector (caller treats this as "mozc unreachable").
std::vector<uint8_t> resolveSocketAddress(const std::string &override_path = "");

// One-shot blocking request: connect, send `payload`, shutdown(SHUT_WR),
// recv until EOF, close. `socket_address` comes from resolveSocketAddress().
std::optional<std::vector<uint8_t>>
sendRequest(const std::vector<uint8_t> &socket_address,
            const std::vector<uint8_t> &payload,
            std::chrono::milliseconds timeout);

// Attempt to spawn mozc_server in the background. Returns true if the spawn
// syscall succeeded (does NOT wait for the server to be ready).
bool spawnServer(const std::string &mozc_server_path);

} // namespace skk_mozc::ipc

#endif // FCITX5_SKK_MOZC_IPC_SOCKET_H_
