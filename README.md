# fcitx5-skk-mozc

> 日本語版 / Japanese: [README.ja.md](./README.ja.md)

A fcitx5 Japanese input method that augments **fcitx5-skk** with candidates from the **Mozc** dictionary and conversion engine.

SKK's input philosophy is preserved (explicit conversion boundaries, ▽ / ▼ markers, fast commit). On top of that, multi-kanji compound words and multi-bunsetsu phrases that aren't in the SKK dictionary — the things you'd normally have to enter one segment at a time — get filled in by Mozc.

## What you get

- When you press SPC in ▽ mode, the SKK dictionary lookup and a Mozc query run in parallel. The results are merged.
- **Compound words and full sentences that the SKK system dictionary doesn't carry**:
  - `さしみほうちょう` → `刺し身包丁` (kanji compound, not in SKK-JISYO.L)
  - `くらうどさーびす` → `クラウドサービス` (loanword compound)
  - `にゅーよーくしゅう` → `ニューヨーク州` (loanword + kanji suffix)
  - `わたしはがくせいです` → `私は学生です` (multi-bunsetsu)
- Your personal SKK dictionary always wins. Picking a Mozc-only candidate immediately learns it into the SKK user dictionary (libskk's writable dict slot, by default `~/.local/share/fcitx5/skk/user.dict`), so it surfaces at the top on the second use, behaving like any other SKK entry.
- Mozc-style bunsetsu refinement while a multi-segment candidate is showing:
  - `Shift+←/→` shrink / grow the focused bunsetsu
  - `Tab / Shift+Tab` move focus
  - `Space` cycle the focused bunsetsu's candidates
  - `Enter` commit (learns each segment to the SKK user dictionary)
  - `ESC` / `C-g` abort refinement and return to plain SKK ▽

We never call SUBMIT on the Mozc side, so Mozc's own `user_history` stays untouched. All learning lives in your SKK personal dictionary.

## Design

Full design notes (in Japanese) are in [`CLAUDE.md`](./CLAUDE.md). Highlights:

- fcitx5-skk is extended via a small patch; the bulk of the logic lives in this repo's `src/` so upstream rebases are mechanical.
- Communication with Mozc is over `mozc_server`'s UNIX socket. The socket address is read from `~/.config/mozc/.session.ipc` (an `IPCPathInfo` protobuf), then we connect to the Linux abstract socket `\0tmp/.mozc.<key>.session`.
- 50 ms synchronous timeout per query, with a graceful fallback to SKK-only candidates when Mozc isn't reachable.

## Installation

### Option A: Nix flake (recommended)

#### Home Manager

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    home-manager.url = "github:nix-community/home-manager";
    skk-mozc.url = "github:m-tky/skk-mozc";
  };

  outputs = { self, nixpkgs, home-manager, skk-mozc, ... }: {
    homeConfigurations.you = home-manager.lib.homeManagerConfiguration {
      pkgs = nixpkgs.legacyPackages.x86_64-linux;
      modules = [
        skk-mozc.homeManagerModules.default
        ({ pkgs, ... }: {
          i18n.inputMethod.enabled = "fcitx5";
          programs.fcitx5-skk-mozc = {
            enable = true;
            mozc.ipcTimeoutMs = 50;
            mozc.maxCandidates = 20;
          };
        })
      ];
    };
  };
}
```

#### One-off build from the command line

```bash
nix build github:m-tky/skk-mozc#fcitx5-skk-mozc
# Produces ./result/lib/fcitx5/skk.so
```

#### Sanity-checking the Mozc IPC

```bash
# Requires a running mozc_server.
nix run github:m-tky/skk-mozc#mozc-client-cli -- "さしみほうちょう"
# => 刺し身包丁 / 刺し身 / 刺身 / ...
```

### Option B: Without Nix

Install the dependencies from your distribution and use the bundled build script.

#### System packages

| Distro | Packages |
|--------|----------|
| Debian / Ubuntu | `git cmake ninja-build pkg-config gettext extra-cmake-modules libprotobuf-dev protobuf-compiler fcitx5 libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev libskk-dev libglib2.0-dev gobject-introspection libgirepository1.0-dev mozc-server` |
| Arch | `git cmake ninja pkgconf gettext extra-cmake-modules protobuf fcitx5 libskk glib2 gobject-introspection mozc` |
| Fedora | `git cmake ninja-build pkgconf gettext extra-cmake-modules protobuf-devel protobuf-compiler fcitx5-devel libskk-devel glib2-devel gobject-introspection-devel mozc` |

#### Build + install

```bash
git clone https://github.com/m-tky/skk-mozc.git
cd skk-mozc

# Build (produces ./build/work/build/src/skk.so)
./scripts/build.sh

# Install system-wide (/usr/local/lib/fcitx5/skk.so)
INSTALL=1 ./scripts/build.sh

# Install under your home directory
INSTALL=1 PREFIX=$HOME/.local ./scripts/build.sh
```

The script:

1. Clones fcitx5-skk and mozc at the revisions known to apply cleanly (see `scripts/versions.env`).
2. Stages this repo's `src/` files into the fcitx5-skk tree.
3. Copies mozc's `protocol/*.proto` and `ipc/ipc.proto` into the build.
4. Applies `patches/fcitx5-skk/*.patch`.
5. Runs `cmake + ninja`.
6. Optionally installs into `$PREFIX` (default `/usr/local`).

#### Restart fcitx5

```bash
fcitx5 -r
# or
pkill -SIGHUP fcitx5
```

#### Verify

Enable SKK in `fcitx5-configtool`, open any text editor, then:

```
Q さしみほうちょう SPC
```

`刺し身包丁` should be near the top of the candidate list (it isn't in SKK-JISYO.L, so plain fcitx5-skk wouldn't find it).

## Configuration

Runtime behavior is controlled by environment variables read by the addon at startup. Set them in whatever shell starts fcitx5 (`~/.profile`, `~/.config/fcitx5/profile`, or via home-manager).

| Variable | Default | Description |
|----------|---------|-------------|
| `SKK_MOZC_ENABLE` | `1` | Set to `0` to disable Mozc lookup and behave as plain fcitx5-skk |
| `SKK_MOZC_IPC_TIMEOUT_MS` | `50` | Max time to wait for mozc_server (ms) |
| `SKK_MOZC_MAX_CANDIDATES` | `20` | Cap on Mozc candidates accepted per query |
| `SKK_MOZC_DEBUG` | `0` | Set to `1` for verbose logging to `~/.cache/skk-mozc/log` |

With Home Manager the equivalent options are `programs.fcitx5-skk-mozc.{mozc.enable, mozc.ipcTimeoutMs, mozc.maxCandidates, debug}`.

## SKK dictionaries

This addon does not try to manage SKK dictionaries on your behalf — the standard fcitx5-skk dictionary list at `~/.local/share/fcitx5/skk/dictionary_list` keeps working unchanged. Add system dicts there as usual, e.g. via Home Manager:

```nix
home.file.".local/share/fcitx5/skk/dictionary_list".text = ''
  file=${pkgs.skkDictionaries.l}/share/skk/SKK-JISYO.L,mode=readonly,type=file
  file=${pkgs.skkDictionaries.jinmei}/share/skk/SKK-JISYO.jinmei,mode=readonly,type=file
'';
```

All learning (SKK- or Mozc-sourced) goes into libskk's writable user dictionary slot. If you don't configure one explicitly, fcitx5-skk falls back to `~/.local/share/fcitx5/skk/user.dict`, which is where the bulk of your personal entries live. To pin a specific path, add `file=~/your-dict,mode=readwrite,type=file` to the dictionary list above.

## Development

```bash
# Nix dev shell
nix develop

# Standalone IPC sanity-check CLI
nix build .#mozc-client-cli
./result/bin/mozc-client-cli "さしみほうちょう"

# Full addon
nix build .#fcitx5-skk-mozc

# All flake outputs
nix flake check
```

Upstream tracking:

- fcitx5-skk and mozc are pinned directly as flake inputs by revision. Bump with `nix flake update`.
- Patches live under `patches/fcitx5-skk/`. If upstream's structure shifts significantly, rebase the patches and commit.
- For non-Nix builds, the same revisions are mirrored in `scripts/versions.env`; keep both files in sync when bumping.

## License

GPL-3.0-or-later (inherited from fcitx5-skk). See [`LICENSE`](./LICENSE).
