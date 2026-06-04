# fcitx5-skk-mozc

fcitx5-skk に Mozc 辞書由来の連文節変換候補を統合する fcitx5 IME。
SKK の入力哲学はそのまま、辞書にない複合語 (例:「あさひしんぶん→朝日新聞」) を mozc の連文節変換でカバーする。

## 設計サマリ

### 全体方針

| 項目 | 決定 |
|------|------|
| 形態 | fcitx5 用 IME (デスクトップ全域) |
| ベース | fcitx5-skk を patch で拡張、libskk は upstream のまま |
| Mozc 連携 | `mozc_server` と UNIX socket + protobuf IPC |
| Mozc コミット | しない (学習は SKK 個人辞書のみ) |
| ライセンス | GPLv3+ (fcitx5-skk 由来、`SPDX-License-Identifier: GPL-3.0-or-later`) |

### 候補生成とランキング

▽ モードで SPC を押したとき:

1. libskk が SKK 辞書から候補を生成。
2. 並列に Mozc IPC で `SessionCommand::CONVERT` を投げ、候補を取得 (50ms 同期タイムアウト)。
3. マージ:
   - SKK 個人辞書 (~/.skk-jisyo) ヒット → 最先頭。
   - 残りは mozc コストで統合、同じ表記は重複排除。
   - SKK システム辞書専用エントリ (mozc に無い表記) は mozc 上位より後ろに配置。
4. 候補ウィンドウは出所マークなしで一覧表示。

Mozc IPC が間に合わない / 失敗した場合は SKK のみで graceful fallback。

### 文節境界調整サブモード

Mozc 候補が候補ウィンドウに入った時点で「refinement モード」に突入する。
これは fcitx5-skk の通常入力フローを一時的に乗っ取るサブステートで、以下のキーを受け取る:

| キー | 動作 |
|------|------|
| `Shift+←/→` | 先頭文節 (注目文節) の境界を縮める/伸ばす |
| `Tab / Shift+Tab` | 注目文節を一つ右/左に移動 |
| `Space` | 注目文節の次候補へ |
| `Enter` | 全文確定 → SKK 個人辞書に各文節を登録 |
| `ESC / C-g` | SKK ▽ モードに戻る (refinement 中断) |

mozc とは状態を共有しないので、`SessionCommand::EXPAND_SUGGESTION` 等の引数も自前で持つ。Mozc 側に学習を任せないため `RESET_CONTEXT` を毎回送って状態をクリアする。

### 送り仮名

SKK の送り仮名セマンティクス (▽お*Ru) を維持。Mozc には完成形 yomi (「おくる」) を投げて、追加候補源として並走させる。複合語の主な悩みは送り仮名のない名詞で起きるため、これで実用上カバー可能。

### flake.nix

#### inputs

- `nixpkgs` (NixOS unstable 想定)
- `fcitx5-skk-src` — `github:fcitx/fcitx5-skk`、rev で直 pin
- `mozc-src` — `github:google/mozc`、rev で直 pin。`src/protocol/commands.proto` の参照元
- `home-manager` (HM モジュール用)
- `flake-utils`

#### outputs

| output | 内容 |
|--------|------|
| `packages.<system>.fcitx5-skk-mozc` | ビルド済み fcitx5 アドオン (`.so` + `addon/skk-mozc.conf` + dictionary metadata) |
| `packages.<system>.default` | 上に同じ |
| `devShells.<system>.default` | C++ 開発用 (`fcitx5`, `libskk`, `protobuf`, `cmake`, `mozc-src` を参照) |
| `homeManagerModules.default` | HM 用モジュール |

#### Home Manager オプション

すべて `programs.fcitx5-skk-mozc.*` 配下:

| オプション | 型 | デフォルト | 説明 |
|------------|------|----------|------|
| `enable` | bool | `false` | アドオンを有効化 |
| `mozc.enable` | bool | `true` | Mozc 候補マージを有効化 (false なら純粋 SKK) |
| `mozc.ipcTimeoutMs` | int | `50` | mozc_server 応答待ち上限 (ms) |
| `mozc.maxCandidates` | int | `20` | 1 クエリで mozc から取り込む候補数上限 |
| `extraSkkDictionaries` | listOf path | `[]` | システム辞書に追加する SKK 辞書 |
| `debug` | bool | `false` | `~/.cache/skk-mozc/log` に詳細ログ出力 |

`enable = true` で:
- アドオン package が `i18n.inputMethod.fcitx5.addons` に追加される
- `nixpkgs.mozc` も自動で addons に追加される (mozc_server の binary を提供するため)
- `nixpkgs.skkDictionaries.l` がデフォルトで dictionary_list に入る
- `~/.skk-jisyo` が空ファイルとして `home.activation` で初期化される

### リポジトリ構造

```
.
├── CLAUDE.md                          設計+実装メモ (このファイル)
├── README.md                          ユーザ向け
├── LICENSE                            GPLv3+
├── flake.nix
├── flake.lock                         自動生成
├── nix/
│   └── package.nix                    fcitx5-skk-mozc derivation 本体
├── patches/
│   └── fcitx5-skk/
│       └── 0001-add-mozc-integration-hooks.patch
├── src/                               このリポジトリで新規追加するソース
│   ├── CMakeLists.txt
│   ├── mozc_client/                   Mozc IPC クライアント
│   │   ├── mozc_client.h
│   │   ├── mozc_client.cpp
│   │   └── ipc_socket.cpp
│   ├── candidate_merger/              SKK + Mozc マージ
│   │   ├── merger.h
│   │   └── merger.cpp
│   ├── bunsetsu/                      文節調整サブモード
│   │   ├── refiner.h
│   │   └── refiner.cpp
│   ├── skk_integration/               fcitx5-skk へのフック実装
│   │   ├── mozc_integration.h
│   │   └── mozc_integration.cpp
│   └── proto/                         (build 時に protoc 生成)
│       └── .gitkeep
└── modules/
    └── home-manager.nix
```

### ビルド統合

`patches/fcitx5-skk/0001-add-mozc-integration-hooks.patch` は fcitx5-skk の以下を変更する:

1. `src/CMakeLists.txt` — `src/skk_integration/` 配下を `target_sources` に追加、protobuf と generated proto を `target_link_libraries`/`target_include_directories` に追加。
2. `src/skk.h` — `SkkState` に `std::unique_ptr<MozcIntegration> mozc_;` を追加 (forward declaration)。
3. `src/skk.cpp`:
   - `SkkState::keyEvent` 先頭で `if (mozc_ && mozc_->handleKey(keyEvent)) return;` を挿入。
   - `SkkState::updateUI` で libskk 候補を取得した直後に `if (mozc_) mozc_ ->augmentCandidates(candidateList.get());` を挿入。
   - `SkkEngine` コンストラクタで `mozc_ = std::make_unique<MozcIntegration>(...)` 相当を構築。

patch は実 source を見ながら最小行数で作る。新規ファイルは patch ではなく `src/` 配下にそのまま置き、`postUnpack` で fcitx5-skk のツリーにコピーしてからビルドする。

### Mozc IPC プロトコル

Mozc の `commands.proto` を `nixpkgs.mozc.src` の `src/protocol/commands.proto` から取り込み、protoc で `commands.pb.{h,cc}` を生成。

- UNIX socket は `$XDG_RUNTIME_DIR/.mozc.<uid>.session` (`mozc/src/ipc/unix_ipc.cc` の慣習に合わせる)
- フレーミング: 4-byte little-endian message length + protobuf payload (mozc の `IPCClient` 実装に合わせる)
- 主に使うコマンド: `Input{type=SEND_KEY/SEND_COMMAND, ...}` で:
  - `SEND_KEY` (yomi を投げる) → `Output{preedit, candidates}`
  - `SEND_COMMAND{type=CONVERT/CONVERT_NEXT/...}`
  - `SEND_COMMAND{type=RESET_CONTEXT}` 毎クエリ後に送信、学習を発生させない
- mozc_server が起動していない場合: `mozc_server --mode=server &` を fork + 短時間 retry で再接続

### Mozc の学習を発生させない方法

通常の mozc セッションは:
```
CREATE_SESSION → SEND_KEY (yomi) → SEND_COMMAND{CONVERT} → SEND_COMMAND{SUBMIT} → DELETE_SESSION
```
SUBMIT が user_history に学習を残すので、これを避ける。代わりに:
```
CREATE_SESSION → SEND_KEY → SEND_COMMAND{CONVERT} → [候補抽出] → SEND_COMMAND{RESET_CONTEXT} → DELETE_SESSION
```
としてセッションを破棄。学習はすべて SKK 個人辞書 (~/.skk-jisyo) に任せる。

## マイルストーン

- **M0**: scaffolding (flake.nix, CLAUDE.md, ディレクトリ構成、空のソース)
- **M1**: Mozc IPC クライアントを単体テストで動かす (`mozc-client-cli` 補助バイナリ: yomi を引数に取って候補を出力)
- **M2**: fcitx5-skk patch + マージャ統合、▽ SPC で SKK + Mozc 候補がマージされて出る
- **M3**: 文節境界調整サブモードを実装
- **M4**: HM モジュール統合、`nix flake check` でビルド完走
- **M5**: 実機 (NixOS + HM) で日常使用可能なレベルに調整

現状: **M0 進行中**。

## 開発

```bash
# devShell に入る
nix develop

# fcitx5-skk のソースを patches/ で patch して試しにビルド
nix build .#fcitx5-skk-mozc

# IPC クライアント単体テスト (要 mozc_server 起動中)
nix run .#mozc-client-cli -- "あさひしんぶん"
```

## 既知のオープン論点

- mozc_server のソケットパス検出は upstream の `mozc/src/base/system_util.cc` のロジック依存。Linux では `$XDG_RUNTIME_DIR/.mozc.<uid>.session` が標準だが、Wayland セッションでないケースの fallback を要確認。
- 文節境界調整 UI の表示 (どの文節が「注目」されているか) は fcitx5 の `Text` の `HighLight` flag で表現するが、SKK の preedit と競合しないかは実機検証が必要。
- mozc-server の version skew (古い mozc に新しい proto で叩く) は v1 では考慮しない (同じ flake.lock 内で proto/server を同期させる前提)。
