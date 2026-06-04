# fcitx5-skk-mozc

> English version: [README.md](./README.md)

fcitx5-skk に **Mozc 辞書由来の候補** を統合する fcitx5 入力メソッド。
SKK の入力哲学はそのまま残しつつ、SKK 辞書にない複合語 (例: 「あさひしんぶん」→「朝日新聞」) を Mozc の連文節変換でカバーします。

## 何が出来るのか

- SKK の▽モードで SPC を押したとき、SKK 辞書と並列に Mozc 辞書を引いて候補をマージ。
- **複合語が一発で出る**:
  - 「あさひしんぶん」→「朝日新聞」
  - 「けいざいさんぎょうしょう」→「経済産業省」
  - 「わたしはがくせいです」→「私は学生です」 (連文節)
- 個人辞書 (`~/.skk-jisyo`) のヒットは常に最上位。Mozc 由来の候補も、選んだ瞬間に個人辞書に学習されるので、二度目以降は SKK のトップヒットとして即座に出ます。
- 文節境界の調整も可能 (Mozc 互換):
  - `Shift+←/→` で先頭文節を伸縮
  - `Tab` で注目文節を移動
  - `Space` で次候補
  - `Enter` で確定 (= SKK 個人辞書に追加)
  - `ESC/C-g` で SKK の▽モードに戻る

Mozc には学習を残しません。すべての学習は `~/.skk-jisyo` に集約されます。

## 設計

詳細は [`CLAUDE.md`](./CLAUDE.md) に記載しています。要点:

- fcitx5-skk 本体を patch で拡張 (新規ファイル中心、フックは数行)。
- Mozc とは `mozc_server` への UNIX socket + protobuf IPC で会話。
  ソケットアドレスは `~/.config/mozc/.session.ipc` の `IPCPathInfo` proto から取り出し、Linux abstract socket `\0tmp/.mozc.<key>.session` に接続。
- 50 ms 同期タイムアウト。失敗時は SKK のみで graceful fallback。
- mozc セッションは `RESET_CONTEXT` + `DELETE_SESSION` で終わるので mozc 側の `user_history` は発火しません。

## インストール

### 方法 A: Nix flake (推奨)

#### Home Manager

`flake.nix`:

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

#### コマンドラインから一発ビルド (試用)

```bash
nix build github:m-tky/skk-mozc#fcitx5-skk-mozc
# 結果: ./result/lib/fcitx5/skk.so
```

#### Mozc IPC の動作確認

```bash
# mozc_server が起動している状態で:
nix run github:m-tky/skk-mozc#mozc-client-cli -- "あさひしんぶん"
# => 朝日新聞 / 朝日新聞 / あさひしんぶん / ...
```

### 方法 B: Nix を使わない場合

依存パッケージをディストリビューションから入れた上で、付属のビルドスクリプトを使います。

#### 必要なパッケージ

| ディストロ | パッケージ |
|------------|-----------|
| Debian / Ubuntu | `git cmake ninja-build pkg-config gettext extra-cmake-modules libprotobuf-dev protobuf-compiler fcitx5 libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev libskk-dev libglib2.0-dev gobject-introspection libgirepository1.0-dev mozc-server` |
| Arch | `git cmake ninja pkgconf gettext extra-cmake-modules protobuf fcitx5 libskk glib2 gobject-introspection mozc` |
| Fedora | `git cmake ninja-build pkgconf gettext extra-cmake-modules protobuf-devel protobuf-compiler fcitx5-devel libskk-devel glib2-devel gobject-introspection-devel mozc` |

#### ビルド + インストール

```bash
git clone https://github.com/m-tky/skk-mozc.git
cd skk-mozc

# ビルド (./build/ 配下に skk.so が出来る)
./scripts/build.sh

# システム全体にインストール (/usr/local/lib/fcitx5/skk.so)
INSTALL=1 ./scripts/build.sh

# 自分のホーム以下にインストールしたい場合
INSTALL=1 PREFIX=$HOME/.local ./scripts/build.sh
```

スクリプトは:

1. 動作確認済みの fcitx5-skk と mozc を pin した rev で `./build/upstream/` に clone
2. このリポジトリの `src/` を fcitx5-skk のツリーに展開
3. mozc の `protocol/*.proto` と `ipc/ipc.proto` を staging
4. `patches/fcitx5-skk/*.patch` を適用
5. `cmake + ninja` でビルド
6. `INSTALL=1` なら `$PREFIX` (default `/usr/local`) にインストール

#### fcitx5 を再起動

```bash
fcitx5 -r
# または
pkill -SIGHUP fcitx5
```

#### 動作確認

fcitx5-configtool で SKK を有効化し、適当なテキストエディタで:

```
Q あさひしんぶん SPC
```

を打って「朝日新聞」が候補リストの上位に出れば成功です。

## 設定

実行時の挙動は環境変数で制御します。fcitx5 が起動するときの環境にセットしてください (例: `~/.profile`, `~/.config/fcitx5/profile`, あるいは home-manager 経由)。

| 環境変数 | デフォルト | 説明 |
|----------|----------|------|
| `SKK_MOZC_ENABLE` | `1` | `0` にすると Mozc を呼ばず純粋な fcitx5-skk として動く |
| `SKK_MOZC_IPC_TIMEOUT_MS` | `50` | mozc_server 応答待ち上限 (ミリ秒) |
| `SKK_MOZC_MAX_CANDIDATES` | `20` | 1 クエリで取り込む Mozc 候補の上限 |
| `SKK_MOZC_DEBUG` | `0` | `1` で `~/.cache/skk-mozc/log` に詳細ログを出す |

Home Manager から使う場合は `programs.fcitx5-skk-mozc.{mozc.enable, mozc.ipcTimeoutMs, mozc.maxCandidates, debug}` 経由で同じ設定にできます。

## SKK 辞書

- システム辞書 (例: SKK-JISYO.L) は fcitx5-skk の dictionary list でそのまま設定します (HM 経由のときは `nixpkgs.skkDictionaries.l` をデフォルトで投入)。
- ユーザ辞書は `~/.skk-jisyo` を自動使用。確定するたびにここに学習が積まれます。Mozc 由来の候補を選んだ場合も同じ場所に書き込まれるので、辞書の出所を意識する必要はありません。

## 開発

```bash
# Nix の dev shell
nix develop

# 単体テスト用 CLI (mozc_server に直接問い合わせる)
nix build .#mozc-client-cli
./result/bin/mozc-client-cli "あさひしんぶん"

# 本体
nix build .#fcitx5-skk-mozc

# 全 outputs を検証
nix flake check
```

upstream の追従:

- fcitx5-skk と mozc は `flake.nix` の inputs に rev で直接 pin。`nix flake update` で更新します。
- patch は `patches/fcitx5-skk/` 配下。upstream のリリースで構造が大きく変わったら、patch を rebase してコミットします。
- 非 Nix 用には `scripts/versions.env` に同じ rev が書いてあるので、flake.lock を更新したらこちらも合わせてください。

## ライセンス

GPL-3.0-or-later (fcitx5-skk の継承)。詳細は [`LICENSE`](./LICENSE)。
