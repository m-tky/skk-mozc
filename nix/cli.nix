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

  # src/ unpacks as a "src" sourceRoot; we don't need to rename it. The
  # postPatch step puts the CLI-specific CMakeLists.txt at the root and stages
  # the vendored proto files alongside.
  postPatch = ''
    cp mozc_client/CMakeLists-cli.txt CMakeLists.txt
    mkdir -p proto/protocol
    cp ${mozc-src}/src/protocol/*.proto proto/protocol/
  '';

  meta = with lib; {
    description = "CLI tool to query Mozc for candidates (testing aid for fcitx5-skk-mozc)";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
    mainProgram = "mozc-client-cli";
  };
}
