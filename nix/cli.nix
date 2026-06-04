{ lib
, stdenv
, fcitx5-skk-mozc
, protobuf
, makeWrapper
}:

# Small CLI that exercises the IPC client standalone. Useful as a sanity check
# while developing: run `mozc-client-cli あさひしんぶん` and inspect candidates.
# Built from the same tree as fcitx5-skk-mozc, just with a different entry point.

stdenv.mkDerivation {
  pname = "mozc-client-cli";
  inherit (fcitx5-skk-mozc) version src patches postPatch;

  nativeBuildInputs = fcitx5-skk-mozc.nativeBuildInputs ++ [ makeWrapper ];
  buildInputs = fcitx5-skk-mozc.buildInputs;

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DSKK_MOZC_BUILD_CLI=ON"
    "-DSKK_MOZC_ENABLE=OFF"   # don't build the fcitx5 addon for this output
  ];

  meta = with lib; {
    description = "CLI tool to query Mozc for candidates (testing aid for fcitx5-skk-mozc)";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
  };
}
