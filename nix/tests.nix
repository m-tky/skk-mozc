{ lib
, stdenv
, cmake
, ninja

, skkMozcSources
, skkMozcTests
}:

# Pure unit tests with no fcitx5 / libskk / protobuf dependency. Runs as part
# of `nix flake check` via flakeOutputs.checks.
stdenv.mkDerivation {
  pname = "skk-mozc-tests";
  version = "0.1.0-unstable";

  src = skkMozcTests;

  nativeBuildInputs = [ cmake ninja ];

  postUnpack = ''
    sourceRoot=tests
  '';

  postPatch = ''
    cp -r ${skkMozcSources}/panel_dispatch panel_dispatch
  '';

  cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    ./panel-dispatch-test
    runHook postCheck
  '';

  meta = with lib; {
    description = "Unit tests for fcitx5-skk-mozc panel state machine";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
  };
}
