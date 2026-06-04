{ self }:
{ config, lib, pkgs, ... }:

let
  cfg = config.programs.fcitx5-skk-mozc;
  pkg = self.packages.${pkgs.stdenv.hostPlatform.system}.fcitx5-skk-mozc;
in
{
  options.programs.fcitx5-skk-mozc = {
    enable = lib.mkEnableOption "fcitx5-skk-mozc input method";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkg;
      defaultText = lib.literalExpression
        "self.packages.\${system}.fcitx5-skk-mozc";
      description = "The fcitx5-skk-mozc package to install.";
    };

    mozc = {
      enable = lib.mkOption {
        type = lib.types.bool;
        default = true;
        description = ''
          Whether to merge Mozc dictionary candidates into the SKK candidate
          list. Set to false to behave as a pure fcitx5-skk.
        '';
      };

      ipcTimeoutMs = lib.mkOption {
        type = lib.types.ints.between 1 1000;
        default = 50;
        description = ''
          Maximum time to wait for mozc_server to respond before falling back
          to SKK-only candidates. 50 ms covers the warm-server case
          comfortably while keeping the SPC press feel snappy.
        '';
      };

      maxCandidates = lib.mkOption {
        type = lib.types.ints.between 1 100;
        default = 20;
        description = ''
          Upper bound on the number of Mozc candidates accepted per query.
        '';
      };
    };

    debug = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Enable verbose logging to ~/.cache/skk-mozc/log. Includes a one-line
        warning when mozc_server is unreachable.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = [ cfg.package pkgs.mozc ];

    # Wire the addon into fcitx5 if home-manager's i18n.inputMethod is being
    # used. Supports both the legacy `enabled = "fcitx5"` syntax and the
    # current `enable + type = "fcitx5"` form. We don't force it on so that
    # users can integrate at the NixOS system level too.
    i18n.inputMethod = lib.mkIf
      (
        ((config.i18n.inputMethod.enabled or null) == "fcitx5")
        || (((config.i18n.inputMethod.type or null) == "fcitx5")
            && (config.i18n.inputMethod.enable or false))
      ) {
        fcitx5.addons = [ cfg.package pkgs.mozc ];
      };

    # Runtime config. The addon reads these from the env on startup since
    # the legacy fcitx5-skk config schema doesn't know about mozc fields.
    home.sessionVariables = lib.mkIf cfg.enable {
      SKK_MOZC_ENABLE = if cfg.mozc.enable then "1" else "0";
      SKK_MOZC_IPC_TIMEOUT_MS = toString cfg.mozc.ipcTimeoutMs;
      SKK_MOZC_MAX_CANDIDATES = toString cfg.mozc.maxCandidates;
      SKK_MOZC_DEBUG = if cfg.debug then "1" else "0";
    };

    # SKK dictionaries themselves are not managed by this module; users
    # configure them via the standard fcitx5-skk location at
    # ~/.local/share/fcitx5/skk/dictionary_list. This avoids clobbering setups
    # that already manage ~/.config/fcitx5 declaratively.

    home.activation.skkMozcInitUserDict = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      run mkdir -p $HOME
      if [ ! -e $HOME/.skk-jisyo ]; then
        run touch $HOME/.skk-jisyo
      fi
      run mkdir -p $HOME/.cache/skk-mozc
    '';
  };
}
