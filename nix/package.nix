{ lib
, stdenv
, cmake
, ninja
, pkg-config
, extra-cmake-modules
, gettext
, protobuf
, fcitx5
, libskk
, glib
, gobject-introspection

, fcitx5-skk-src
, mozc-src
, skkMozcSources    # path: this repo's ./src
, skkMozcPatches    # path: this repo's ./patches/fcitx5-skk
}:

let
  # The proto file we vendor at build time, taken from the pinned mozc source.
  # If mozc updates commands.proto incompatibly, our build will catch it.
  mozcCommandsProto = "${mozc-src}/src/protocol/commands.proto";
  mozcConfigProto   = "${mozc-src}/src/protocol/config.proto";
in

stdenv.mkDerivation (finalAttrs: {
  pname = "fcitx5-skk-mozc";
  version = "0.1.0-unstable";

  # Start from fcitx5-skk's tree, then overlay our sources and apply patches.
  src = fcitx5-skk-src;

  patches = [
    # The hook patch is intentionally minimal: it only injects forward
    # declarations and 3 call-sites into fcitx5-skk's skk.{h,cpp}. The bulk of
    # the integration lives in src/skk_integration/ which gets copied in below.
    "${skkMozcPatches}/0001-add-mozc-integration-hooks.patch"
  ];

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    extra-cmake-modules
    gettext
    protobuf
    gobject-introspection
  ];

  buildInputs = [
    fcitx5
    libskk
    glib
    protobuf
  ];

  # Drop our additional sources into the upstream tree and stage the mozc
  # protos so the patched CMakeLists.txt can find them.
  postPatch = ''
    cp -r ${skkMozcSources}/mozc_client      src/mozc_client
    cp -r ${skkMozcSources}/candidate_merger src/candidate_merger
    cp -r ${skkMozcSources}/bunsetsu         src/bunsetsu
    cp -r ${skkMozcSources}/skk_integration  src/skk_integration

    mkdir -p src/proto
    cp ${mozcCommandsProto} src/proto/commands.proto
    cp ${mozcConfigProto}   src/proto/config.proto

    # Mark the included tree as our customized variant.
    if [ -f config.h.in ]; then
      echo '#define FCITX5_SKK_MOZC_VARIANT 1' >> config.h.in
    fi
  '';

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DBUILD_SHARED_LIBS=ON"
    "-DSKK_MOZC_ENABLE=ON"
  ];

  meta = with lib; {
    description = "fcitx5 SKK input method enhanced with Mozc dictionary lookup";
    homepage = "https://github.com/fcitx/fcitx5-skk";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
    maintainers = [ ];
  };
})
