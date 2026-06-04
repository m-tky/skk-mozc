{ lib
, stdenv
, cmake
, ninja
, pkg-config
, fcitx5
, libskk
, libgee
, glib

, skkMozcSources
, skkMozcTests
, fcitx5-skk-mozc # the addon under test
}:

# Builds the fcitx5-testfrontend-based end-to-end test binary. NOT auto-run
# during `nix flake check` because it requires a reachable mozc_server.
#
# Run manually:
#   nix build .#skk-mozc-e2e-test
#   ./result/bin/skk-mozc-e2e-test
#
# (mozc_server must already be running, e.g. via the user's fcitx5 daemon.)
stdenv.mkDerivation {
  pname = "skk-mozc-e2e-test";
  version = "0.1.0-unstable";

  src = skkMozcTests;

  nativeBuildInputs = [ cmake ninja pkg-config ];
  buildInputs = [ fcitx5 libskk libgee glib ];

  postUnpack = ''
    sourceRoot=tests
  '';

  postPatch = ''
    cp -r ${skkMozcSources}/panel_dispatch  panel_dispatch
    cp -r ${skkMozcSources}/skk_integration skk_integration
  '';

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DSKK_MOZC_BUILD_E2E=ON"
    "-DSKK_MOZC_FCITX5_INCLUDE_DIR=${fcitx5}/include/Fcitx5/Module"
    "-DSKK_MOZC_E2E_RUNTIME_DIR=${placeholder "out"}/share/skk-mozc/e2e"
  ];

  # Skip the test during build — needs mozc_server.
  doCheck = false;

  postInstall = ''
    # Stage skk.so + fcitx5 conf files at the runtime layout the e2e binary
    # expects (matched at compile time by SKK_MOZC_ADDON_DIR / SKK_MOZC_DATA_DIR).
    runtime=$out/share/skk-mozc/e2e
    mkdir -p $runtime/addons \
             $runtime/data/addon \
             $runtime/data/inputmethod \
             $runtime/data/skk
    cp ${fcitx5-skk-mozc}/lib/fcitx5/skk.so $runtime/addons/
    cp ${fcitx5-skk-mozc}/share/fcitx5/addon/skk.conf $runtime/data/addon/
    cp ${fcitx5-skk-mozc}/share/fcitx5/inputmethod/skk.conf $runtime/data/inputmethod/
    cp ${fcitx5-skk-mozc}/share/fcitx5/skk/dictionary_list $runtime/data/skk/

    # Mirror testfrontend / testui addons into our search path so the
    # Instance can load them (they live in nixpkgs.fcitx5).
    cp ${fcitx5}/lib/fcitx5/libtestfrontend.so $runtime/addons/ || true
    cp ${fcitx5}/lib/fcitx5/libtestui.so       $runtime/addons/ || true
    if [ -d ${fcitx5}/share/fcitx5/testing/addon ]; then
      cp ${fcitx5}/share/fcitx5/testing/addon/*.conf \
         $runtime/data/addon/ 2>/dev/null || true
    fi
    # nixpkgs ships the testing addon .conf files with the @PROJECT_VERSION@
    # placeholder unresolved, which fcitx5's addon loader rejects. Substitute
    # the actual fcitx5 version so the dependency check passes.
    fcitx5_ver=${fcitx5.version}
    for f in $runtime/data/addon/test*.conf; do
      [ -f "$f" ] || continue
      sed -i "s/@PROJECT_VERSION@/$fcitx5_ver/g" "$f"
    done
  '';

  meta = with lib; {
    description = "fcitx5-testfrontend-based end-to-end test for skk-mozc";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
    mainProgram = "skk-mozc-e2e-test";
  };
}
