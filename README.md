# fcitx5-skk-mozc

fcitx5-skk に Mozc の連文節変換候補を統合する fcitx5 IME。

SKK の入力哲学 (送り仮名の明示、文節境界はユーザが決める) を保ちつつ、SKK 辞書にない複合語 (例: 「あさひしんぶん」→「朝日新聞」) を Mozc の辞書 + 連文節変換で補う。

設計の意思決定の経緯は [`CLAUDE.md`](./CLAUDE.md) を参照。

## 使い方 (Home Manager)

```nix
{
  inputs.skk-mozc.url = "github:YOUR_USER/skk-mozc";

  outputs = { self, nixpkgs, home-manager, skk-mozc, ... }: {
    homeConfigurations.you = home-manager.lib.homeManagerConfiguration {
      # ...
      modules = [
        skk-mozc.homeManagerModules.default
        ({ ... }: {
          i18n.inputMethod = {
            enabled = "fcitx5";
            fcitx5.addons = [ ];  # the module appends its own
          };
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

## 挙動概要

- ▽ モードで SPC を押すと、SKK 辞書と並列に Mozc に問い合わせ、両者をマージした候補列が出る。
- 個人辞書 (~/.skk-jisyo) のヒットが最上位。
- 50 ms 以内に Mozc が答えなければ SKK のみで先に進む (graceful fallback)。
- Mozc が複数文節を提示した場合、「refinement モード」に入る:
  - `Shift+←/→` で先頭文節の境界を伸縮
  - `Tab/Shift+Tab` で注目文節を移動
  - `Space` で次候補
  - `Enter` で確定 → 各文節を SKK 個人辞書に学習
  - `ESC / C-g` で SKK ▽ に戻る (refinement 中断)
- Mozc 側には学習を残さない (`RESET_CONTEXT` + `DELETE_SESSION`)。すべての学習は SKK の ~/.skk-jisyo に集約。

## 開発

```bash
nix develop

# fcitx5 addon 本体をビルド
nix build .#fcitx5-skk-mozc

# IPC が動くか単体テスト (mozc_server が走っていること)
nix run .#mozc-client-cli -- "あさひしんぶん"
```

## ライセンス

GPL-3.0-or-later (fcitx5-skk 由来)。詳細は [LICENSE](./LICENSE)。
