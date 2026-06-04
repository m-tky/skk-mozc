{ lib
, stdenv
, cmake
, ninja
, pkg-config
, libskk
, glib

, skkMozcSources
, skkMozcTests
}:

# Two test binaries:
#   - panel-dispatch-test:   pure unit (no fcitx5/libskk/protobuf)
#   - libskk-integration-test: exercises libskkCurrentYomi against a real
#                              libskk SkkContext for end-to-end yomi state
#                              transitions (carry-over, ▼ mode, reset).
stdenv.mkDerivation {
  pname = "skk-mozc-tests";
  version = "0.1.0-unstable";

  src = skkMozcTests;

  nativeBuildInputs = [ cmake ninja pkg-config ];
  buildInputs = [ libskk glib ];

  postUnpack = ''
    sourceRoot=tests
  '';

  postPatch = ''
    cp -r ${skkMozcSources}/panel_dispatch  panel_dispatch
    cp -r ${skkMozcSources}/skk_integration skk_integration
  '';

  cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    ./panel-dispatch-test
    ./libskk-integration-test
    runHook postCheck
  '';

  meta = with lib; {
    description = "Tests for fcitx5-skk-mozc (panel state machine + libskk)";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
  };
}
