{
  description = "fcitx5-skk + Mozc dictionary integration";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";

    # Upstream fcitx5-skk and mozc are pinned directly.
    # We apply our patches over these sources at build time, so that rebasing
    # against newer upstream commits is a `nix flake update` + patch refresh.
    fcitx5-skk-src = {
      url = "github:fcitx/fcitx5-skk";
      flake = false;
    };
    mozc-src = {
      url = "github:google/mozc";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, fcitx5-skk-src, mozc-src }:
    let
      # System-independent: the HM module.
      homeManagerModules.default = import ./modules/home-manager.nix {
        inherit self;
      };
    in
    flake-utils.lib.eachDefaultSystem
      (system:
        let
          pkgs = import nixpkgs { inherit system; };
          fcitx5-skk-mozc = pkgs.callPackage ./nix/package.nix {
            inherit fcitx5-skk-src mozc-src;
            skkMozcSources = ./src;
            skkMozcPatches = ./patches/fcitx5-skk;
          };
          mozc-client-cli = pkgs.callPackage ./nix/cli.nix {
            inherit mozc-src;
            skkMozcSources = ./src;
          };
          skk-mozc-tests = pkgs.callPackage ./nix/tests.nix {
            skkMozcSources = ./src;
            skkMozcTests = ./tests;
          };
          skk-mozc-e2e-test = pkgs.callPackage ./nix/e2e.nix {
            skkMozcSources = ./src;
            skkMozcTests = ./tests;
            inherit fcitx5-skk-mozc;
          };
        in
        {
          packages = {
            inherit fcitx5-skk-mozc mozc-client-cli skk-mozc-tests
                    skk-mozc-e2e-test;
            default = fcitx5-skk-mozc;
          };

          # `nix flake check` runs the test binary too.
          checks = {
            inherit skk-mozc-tests;
            # Guard against scripts/versions.env drifting from flake.lock.
            # The non-Nix build path (scripts/build.sh) reads versions.env,
            # so this is the only thing that keeps those two truth sources
            # honest. If the check fails, run scripts/sync-versions.sh.
            versions-env-in-sync = pkgs.runCommand "versions-env-in-sync"
              { nativeBuildInputs = [ pkgs.bash pkgs.jq pkgs.diffutils ]; }
              ''
                cp -r ${./.} src
                chmod -R u+w src
                cd src
                ./scripts/sync-versions.sh --check
                touch $out
              '';
          };

          devShells.default = pkgs.mkShell {
            inputsFrom = [ fcitx5-skk-mozc ];
            packages = with pkgs; [
              cmake
              ninja
              pkg-config
              kdePackages.extra-cmake-modules
              gettext
              protobuf
              fcitx5
              libskk
              gobject-introspection
              glib
              # For reading commands.proto:
              mozc
              # Convenience:
              clang-tools
              gdb
            ];

            shellHook = ''
              echo "fcitx5-skk-mozc dev shell"
              echo "  fcitx5-skk source: ${fcitx5-skk-src}"
              echo "  mozc source     : ${mozc-src}"
              echo "  proto file      : ${mozc-src}/src/protocol/commands.proto"
              export MOZC_PROTO_DIR=${mozc-src}/src/protocol
              export FCITX5_SKK_SRC=${fcitx5-skk-src}
            '';
          };

          formatter = pkgs.nixpkgs-fmt;
        }
      ) // {
        inherit homeManagerModules;
      };
}
