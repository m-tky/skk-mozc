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

    # Stage skk.so + its conf files into the runtime layout the e2e binary
    # expects (matched by SKK_MOZC_ADDON_DIR / SKK_MOZC_DATA_DIR).
    runtime=$PWD/e2e-runtime
    mkdir -p $runtime/addons \
             $runtime/data/addon \
             $runtime/data/inputmethod \
             $runtime/data/skk
    cp ${fcitx5-skk-mozc}/lib/fcitx5/skk.so $runtime/addons/
    cp ${fcitx5-skk-mozc}/share/fcitx5/addon/skk.conf $runtime/data/addon/
    cp ${fcitx5-skk-mozc}/share/fcitx5/inputmethod/skk.conf $runtime/data/inputmethod/
    cp ${fcitx5-skk-mozc}/share/fcitx5/skk/dictionary_list $runtime/data/skk/

    # Also include the system fcitx5 testfrontend + testui in the addon
    # search path so loading them succeeds inside the test.
    cp ${fcitx5}/lib/fcitx5/libtestfrontend.so $runtime/addons/ || true
    cp ${fcitx5}/lib/fcitx5/libtestui.so       $runtime/addons/ || true
    cp ${fcitx5}/share/fcitx5/testing/addon/testfrontend.conf \
       $runtime/data/addon/ 2>/dev/null || true
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
    # Copy the staged runtime tree into the output so the installed binary
    # can resolve TESTING_BINARY_DIR.
    mkdir -p $out/share/skk-mozc/e2e
    cp -r ./e2e-runtime/. $out/share/skk-mozc/e2e/
  '';

  meta = with lib; {
    description = "fcitx5-testfrontend-based end-to-end test for skk-mozc";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
    mainProgram = "skk-mozc-e2e-test";
  };
}
