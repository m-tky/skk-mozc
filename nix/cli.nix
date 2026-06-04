{ lib
, stdenv
, cmake
, ninja
, pkg-config
, protobuf

, mozc-src
, skkMozcSources
}:

# Builds just the standalone IPC sanity-check CLI. Does NOT depend on
# fcitx5-skk or the upstream patches, so it's the fastest path to verify the
# Mozc IPC client compiles and talks to a running mozc_server.

stdenv.mkDerivation {
  pname = "mozc-client-cli";
  version = "0.1.0-unstable";

  src = skkMozcSources;

  nativeBuildInputs = [ cmake ninja pkg-config protobuf ];
  buildInputs = [ protobuf ];

  # The source tree has its CMakeLists.txt named "CMakeLists-cli.txt" so it
  # doesn't shadow anything in src/. Move it into place and add the protos.
  postUnpack = ''
    sourceRoot=source
  '';

  postPatch = ''
    cp mozc_client/CMakeLists-cli.txt CMakeLists.txt
    mkdir -p proto
    cp ${mozc-src}/src/protocol/commands.proto proto/commands.proto
    cp ${mozc-src}/src/protocol/config.proto   proto/config.proto
  '';

  meta = with lib; {
    description = "CLI tool to query Mozc for candidates (testing aid for fcitx5-skk-mozc)";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
    mainProgram = "mozc-client-cli";
  };
}
