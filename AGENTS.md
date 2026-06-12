# fcitx5-skk-mozc

An fcitx5 IME that augments fcitx5-skk with multi-bunsetsu conversion
candidates sourced from the Mozc dictionary. SKK's input philosophy is
preserved as-is; compound words missing from the SKK dictionary (e.g.
「あさひしんぶん→朝日新聞」) get covered by Mozc's multi-bunsetsu
conversion.

## Design summary

### Overall direction

| Item | Decision |
|------|------|
| Form factor | fcitx5 IME (system-wide on the desktop) |
| Base | Extend fcitx5-skk via patch; libskk stays upstream |
| Mozc bridge | UNIX socket + protobuf IPC to `mozc_server` |
| Mozc commit | None — all learning lives in the SKK personal dict |
| License | GPLv3+ (inherited from fcitx5-skk, `SPDX-License-Identifier: GPL-3.0-or-later`) |

### Candidate generation and ranking

When SPC is pressed in ▽ mode:

1. libskk produces candidates from the SKK dictionaries.
2. In parallel, we issue `SessionCommand::CONVERT` over Mozc IPC and
   collect its candidates (50 ms synchronous timeout).
3. Merge:
   - SKK personal dict hits (libskk's writable user-dict slot, which by
     default is `~/.local/share/fcitx5/skk/user.dict` because fcitx5-skk
     auto-attaches it) → pinned to the top.
   - The rest are integrated by Mozc rank, with duplicate surfaces
     deduplicated.
   - SKK system-dict-only entries (surfaces Mozc didn't produce) sit
     below the top Mozc results.
4. The candidate window displays everything in one list, with no
   source marker.

If Mozc IPC times out or fails, we gracefully fall back to SKK-only
candidates.

### Bunsetsu boundary refinement sub-mode

The moment a Mozc-sourced candidate lands in the candidate window we
enter "refinement mode". This is a temporary sub-state that takes over
fcitx5-skk's normal input flow and consumes the following keys:

| Key | Action |
|------|------|
| `Shift+←/→` | Shrink / grow the leading (focused) bunsetsu boundary |
| `Tab / Shift+Tab` | Move focus to the next / previous bunsetsu |
| `Space` | Cycle the focused bunsetsu's candidates |
| `Enter` | Commit the full text → register each segment to the SKK personal dict |
| `ESC / C-g` | Drop refinement, return to SKK ▽ mode |

We don't share state with Mozc, so arguments to commands like
`SessionCommand::EXPAND_SUGGESTION` are tracked locally. Since we
deliberately keep learning out of Mozc, `RESET_CONTEXT` is sent after
every query to wipe its state.

### Okurigana

SKK's okurigana semantics (▽お*Ru) are preserved verbatim. Mozc is
queried in parallel with the *completed* yomi (e.g. "おくる"), acting
as an extra candidate source. The compound-word pain point is mainly
okurigana-less nouns, so this is enough in practice.

### flake.nix

#### inputs

- `nixpkgs` (assumes NixOS unstable)
- `fcitx5-skk-src` — `github:fcitx/fcitx5-skk`, pinned by rev
- `mozc-src` — `github:google/mozc`, pinned by rev. Source of
  `src/protocol/commands.proto`
- `home-manager` (for the HM module)
- `flake-utils`

#### outputs

| output | content |
|--------|------|
| `packages.<system>.fcitx5-skk-mozc` | Built fcitx5 addon (`.so` + `addon/skk-mozc.conf` + dictionary metadata) |
| `packages.<system>.default` | Same as above |
| `devShells.<system>.default` | C++ dev shell (`fcitx5`, `libskk`, `protobuf`, `cmake`, `mozc-src` references) |
| `homeManagerModules.default` | Home Manager module |

#### Home Manager options

All under `programs.fcitx5-skk-mozc.*`:

| Option | Type | Default | Description |
|------------|------|----------|------|
| `enable` | bool | `false` | Enable the addon |
| `mozc.enable` | bool | `true` | Enable Mozc candidate merge (`false` = plain SKK) |
| `mozc.ipcTimeoutMs` | int | `50` | Maximum mozc_server response wait (ms) |
| `mozc.maxCandidates` | int | `20` | Cap on Mozc candidates accepted per query |
| `extraSkkDictionaries` | listOf path | `[]` | Extra SKK dictionaries appended to the system list |
| `debug` | bool | `false` | Emit verbose logs to `~/.cache/skk-mozc/log` |

When `enable = true`:
- The addon package is added to `i18n.inputMethod.fcitx5.addons`
- `nixpkgs.mozc` is auto-added too (it supplies the `mozc_server` binary)
- `nixpkgs.skkDictionaries.l` lands in the default dictionary_list
- The module does *not* create a personal dictionary file. fcitx5-skk's
  default `~/.local/share/fcitx5/skk/user.dict` is used as the write
  target as-is (only `~/.cache/skk-mozc` is pre-created via
  `home.activation`).

### Repository layout

```
.
├── AGENTS.md                          Design + implementation notes (this file)
├── README.md                          User-facing
├── LICENSE                            GPLv3+
├── flake.nix
├── flake.lock                         Auto-generated
├── nix/
│   └── package.nix                    fcitx5-skk-mozc derivation
├── patches/
│   └── fcitx5-skk/
│       └── 0001-add-mozc-integration-hooks.patch
├── src/                               Sources added by this repo
│   ├── CMakeLists.txt
│   ├── mozc_client/                   Mozc IPC client
│   │   ├── mozc_client.h
│   │   ├── mozc_client.cpp
│   │   └── ipc_socket.cpp
│   ├── candidate_merger/              SKK + Mozc merge
│   │   ├── merger.h
│   │   └── merger.cpp
│   ├── bunsetsu/                      Bunsetsu refinement sub-mode
│   │   ├── refiner.h
│   │   └── refiner.cpp
│   ├── skk_integration/               Hooks into fcitx5-skk
│   │   ├── mozc_integration.h
│   │   └── mozc_integration.cpp
│   └── proto/                         (protoc-generated at build time)
│       └── .gitkeep
└── modules/
    └── home-manager.nix
```

### Build integration

`patches/fcitx5-skk/0001-add-mozc-integration-hooks.patch` changes the
following in fcitx5-skk:

1. `src/CMakeLists.txt` — adds `src/skk_integration/` (and friends) to
   `target_sources`; adds protobuf and the generated proto to
   `target_link_libraries`/`target_include_directories`.
2. `src/skk.h` — adds `std::unique_ptr<MozcIntegration> mozc_;` to
   `SkkState` (forward-declared).
3. `src/skk.cpp`:
   - At the top of `SkkState::keyEvent`, insert
     `if (mozc_ && mozc_->handleKey(keyEvent)) return;`.
   - In `SkkState::updateUI`, right after libskk's candidates are
     fetched, insert
     `if (mozc_) mozc_->augmentCandidates(candidateList.get());`.
   - In the `SkkEngine` constructor, build
     `mozc_ = std::make_unique<MozcIntegration>(...)`.

The patch is intentionally minimal-line, written by reading the real
source. New files are NOT shipped in the patch — they live under `src/`
in this repo and are copied into the fcitx5-skk tree via `postUnpack`
before the build.

### Mozc IPC protocol

Pull Mozc's `commands.proto` from `nixpkgs.mozc.src` at
`src/protocol/commands.proto` and generate `commands.pb.{h,cc}` with
`protoc`.

- UNIX socket: `$XDG_RUNTIME_DIR/.mozc.<uid>.session`, matching the
  convention in `mozc/src/ipc/unix_ipc.cc`.
- Framing: 4-byte little-endian message length + protobuf payload (to
  match Mozc's `IPCClient`).
- Main commands: `Input{type=SEND_KEY/SEND_COMMAND, ...}` —
  - `SEND_KEY` (throw the yomi) → `Output{preedit, candidates}`
  - `SEND_COMMAND{type=CONVERT/CONVERT_NEXT/...}`
  - `SEND_COMMAND{type=RESET_CONTEXT}` sent after every query so no
    learning is triggered.
- If `mozc_server` isn't running, fork `mozc_server --mode=server &`
  and short-retry the connection.

### How we keep Mozc from learning

A typical Mozc session looks like:
```
CREATE_SESSION → SEND_KEY (yomi) → SEND_COMMAND{CONVERT} → SEND_COMMAND{SUBMIT} → DELETE_SESSION
```
`SUBMIT` is what writes to `user_history`, so we avoid it. Instead:
```
CREATE_SESSION → SEND_KEY → SEND_COMMAND{CONVERT} → [extract candidates] → SEND_COMMAND{RESET_CONTEXT} → DELETE_SESSION
```
The session is discarded. Learning lives entirely in libskk's writable
user dictionary slot (by default `~/.local/share/fcitx5/skk/user.dict`).

## Coexistence with fcitx5-mozc

If a user keeps the fcitx5-mozc addon enabled (the `mozc` IM = pure
Mozc input) while using skk-mozc, the two **share the same
`mozc_server` instance**. This is intentional by design (the HM module
even pulls in `pkgs.mozc` precisely to provide the server binary), but
there are behavioural consequences:

- skk-mozc operates on a **pure lookup** assumption — it never tells
  Mozc about commits (it does NOT send `SUBMIT`, and tears the session
  down with `RESET_CONTEXT` + `DELETE_SESSION`).
- Anything typed through fcitx5-mozc lands in Mozc's `user_history`,
  and that learning is reflected back in skk-mozc's queries
  (one-directional cross-pollination).
- This is convenient most of the time (words you type via fcitx5-mozc
  surface in skk-mozc too), but it loosely conflicts with the design
  goal of "all learning concentrated in the SKK personal dict only".
- Users who want a clean split should remove fcitx5-mozc from `addons`
  or operate it under a separate user account.

## Milestones

- **M0** ✅: scaffolding (flake.nix, AGENTS.md, directory layout,
  source skeleton)
- **M1** ✅: Mozc IPC client wired up and verified standalone
  (`mozc-client-cli`)
- **M2** ✅ (build only): fcitx5-skk patch + merger integration with
  the resulting `skk.so` compiling cleanly. Real-machine load
  verification is deferred to M5.
- **M3** ✅ (impl + build): Full multi-bunsetsu conversion +
  RefinementSession driving boundary tweaks / focus moves / candidate
  cycling via live Mozc IPC.
- **M4** ✅: `nix flake check` passes for all outputs.
- **M5** ✅ (code complete): `recordCommit` wired to libskk user dict
  (`skk_dict_select_candidate` + `skk_dict_save`); `focused_segment`
  extracted from `Preedit.highlighted_position`; refinement-mode
  preedit rendered as a 3-slice underline + HighLight; candidate
  annotation routed to the comment slot. Real-machine load happens
  separately on a NixOS host via HM.

**Current state: M0–M5 fully implemented / built at the code level.
Real-machine verification (M5 tail) is operational testing on a NixOS
machine.**

### Mozc IPC details discovered during M1 (deltas from the original notes)

While prototyping with the CLI, four points turned out to differ from
the initial guess in this file. The code already matches the real
behaviour.

| Item | Original guess (wrong) | Real behaviour (right) |
|------|----------------|-------------|
| Socket path | `$XDG_RUNTIME_DIR/.mozc.<uid>.session` (file UNIX socket) | Read `~/.config/mozc/.session.ipc` as a rendezvous file, pull the `key` field out of the `mozc.ipc.IPCPathInfo` proto inside it, then connect to the abstract socket `\0tmp/.mozc.<key>.session` |
| Conversion trigger | Send `SessionCommand::CONVERT` | `SessionCommand::CONVERT` doesn't exist. Sending SPACE (special_key=SPACE) via SEND_KEY transitions the Mozc session into conversion internally |
| Candidate ranking | `CandidateWord.cost()` | `CandidateWord` has no cost field. `CandidateWord.index()` (Mozc's internal rank) is available and we pass it to the merger as a cost stand-in |
| Message framing | 4-byte little-endian length prefix + protobuf payload | No frame length. One message = `send(payload) → shutdown(SHUT_WR) → recv until EOF → close`. The server speaks the same protocol |

Secondary findings:
- `commands.proto` imports `protocol/candidate_window.proto`,
  `protocol/config.proto`, `protocol/engine_builder.proto`, and
  `protocol/user_dictionary_storage.proto`. Pass `-I proto` to
  `protoc` and stage all of them under `proto/protocol/*.proto`.
- `IPCPathInfo` lives in `src/ipc/ipc.proto` (a different proto
  package from `commands.proto`); pull it into the build separately.
- For a long sentence like "わたしはがくせいです" sent with a single
  SPACE, Mozc returns the confirmed form for the **first bunsetsu
  only**. Getting the full multi-bunsetsu result requires additional
  SPACE / CONVERT_NEXT_PAGE etc. across segments — handled in M2/M3.
- For "さしみほうちょう" Mozc also splits into "刺し身" + "包丁", so
  `top_candidates` doesn't carry "刺身包丁" as a single entry. The
  Refiner has to synthesize a candidate from the joined segments.

### M3 structure

- **Full multi-bunsetsu conversion**: concatenate
  `Output.preedit.segment[].value` and prepend the resulting string to
  `top_candidates` at the head (cost = -1). SKK ▽ + SPC on
  「わたしはがくせいです」 then instantly returns 「私は学生です」.
- **RefinementSession**: `MozcClient::beginRefinement(yomi)` returns a
  `RefinementSession` that holds a live Mozc session; the Refiner
  drives it via `shrink/grow/focusNext/focusPrev/next/prev`.
  Internally it actually emits SEND_KEY + SpecialKey(LEFT/RIGHT/UP/SPACE) +
  ModifierKey(SHIFT).
- **No-learn enforcement**: `RefinementSession`'s destructor issues
  `RESET_CONTEXT` then `DELETE_SESSION`, in that order, so nothing
  lingers in Mozc's `user_history`.
- **Extra-SPACE loop**: Refiner.dispatch(NextCandidate) → SPACE sent →
  Mozc reflects the "next candidate" in `Output.preedit`, letting us
  cycle without leaving the sub-mode.

Note: tracking the "focused segment" is something Mozc surfaces via
`Preedit.highlighted_position`, but the current extractor doesn't pick
that up yet (v0 always draws focus on segment 0 in the UI). Handled
during real-machine verification in M5.

### M2 findings (already addressed)

- fcitx5-skk upstream ships a Qt6 configuration GUI (`gui/`). Since
  we run it declaratively via HM, build with `-DENABLE_QT=Off` and
  skip it.
- `mozc_integration.cpp::recordCommit` was originally a 3-argument
  call that didn't match `skk_candidate_new(midasi, gboolean okuri,
  text, annotation, output)`'s 5-argument signature, so it failed.
  libskk's official user-dict write API doesn't persist just by
  constructing a `SkkCandidate`; you have to append it to a `SkkUserDict`
  handle. The function was stubbed out until M3 (see "Open issues").
- The patch applies cleanly across all 3 files (`skk.h`, `skk.cpp`,
  `src/CMakeLists.txt`); `skk.so` builds at 777 KB and links against
  fcitx5/libskk/libprotobuf.

### M1 sanity-check candidates

| Input (yomi) | Top Mozc candidate | Notes |
|-------------|---------------|---------|
| あさひしんぶん | 朝日新聞 | Single bunsetsu, behaves correctly |
| けいざいさんぎょうしょう | 経済産業省 | 4-kanji compound returned in one shot |
| きょうのてんき | 今日の天気 | Multi-bunsetsu collapsed (Mozc-side single candidate) |
| さしみほうちょう | 刺し身 | "包丁" dropped — to be recovered by M3 Refiner composition |
| わたしはがくせいです | 私は | First bunsetsu only — multi-SPACE handled in M2 |

## Development

```bash
# Enter the dev shell
nix develop

# Build with fcitx5-skk patched from patches/
nix build .#fcitx5-skk-mozc

# Standalone IPC sanity check (requires mozc_server up)
nix run .#mozc-client-cli -- "さしみほうちょう"
```

## Known open questions

- `mozc_server` socket-path detection depends on the upstream logic in
  `mozc/src/base/system_util.cc`. On Linux,
  `$XDG_RUNTIME_DIR/.mozc.<uid>.session` is standard, but the fallback
  for non-Wayland sessions still needs verification.
- The bunsetsu refinement UI (which bunsetsu is "focused") is rendered
  via fcitx5's `Text` `HighLight` flag, but whether this collides with
  SKK's own preedit rendering needs real-machine verification.
- Mozc-server version skew (hitting an old `mozc_server` with a new
  proto) is out of scope for v1 (we assume proto/server stay in sync
  via flake.lock).
