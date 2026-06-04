{ lib
, stdenv
, cmake
, ninja
, pkg-config
, kdePackages
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
    kdePackages.extra-cmake-modules
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
  # protos so the patched CMakeLists.txt can find them. Mirrors the layout the
  # standalone CLI uses (proto/protocol/*.proto and proto/ipc/*.proto), so
  # protoc invocations are identical.
  postPatch = ''
    cp -r ${skkMozcSources}/mozc_client      src/mozc_client
    cp -r ${skkMozcSources}/candidate_merger src/candidate_merger
    cp -r ${skkMozcSources}/bunsetsu         src/bunsetsu
    cp -r ${skkMozcSources}/log              src/log
    cp -r ${skkMozcSources}/panel_dispatch   src/panel_dispatch
    cp -r ${skkMozcSources}/skk_integration  src/skk_integration

    mkdir -p src/proto/protocol src/proto/ipc
    cp ${mozc-src}/src/protocol/*.proto src/proto/protocol/
    cp ${mozc-src}/src/ipc/ipc.proto    src/proto/ipc/ipc.proto
  '';

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DENABLE_QT=Off"  # Configure UI is unused on this HM-driven build.
  ];

  meta = with lib; {
    description = "fcitx5 SKK input method enhanced with Mozc dictionary lookup";
    homepage = "https://github.com/fcitx/fcitx5-skk";
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
    maintainers = [ ];
  };
})
