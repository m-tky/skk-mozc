cmake_minimum_required(VERSION 3.16)
project(skk-mozc-tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# panel_dispatch and the test driver share their source layout with the
# main repo; the Nix derivation copies them next to this CMakeLists.

add_executable(panel-dispatch-test
    panel_dispatch_test.cpp
    panel_dispatch/panel_dispatch.cpp
)
target_include_directories(panel-dispatch-test PRIVATE .)

# libskk integration test — links the real libskk so the libskkCurrentYomi
# path is exercised against actual SKK state transitions instead of mocks.
# libskk.pc in nixpkgs declares -I<prefix>/include/skk-1.0 which doesn't
# exist (the headers are at <prefix>/include/libskk/), so we resolve the
# library and prefix manually and link the gobject/glib deps via pkg-config.
find_package(PkgConfig REQUIRED)
find_library(LIBSKK_LIB skk REQUIRED)
get_filename_component(LIBSKK_LIBDIR "${LIBSKK_LIB}" DIRECTORY)
get_filename_component(LIBSKK_PREFIX "${LIBSKK_LIBDIR}" DIRECTORY)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gee-0.8)

add_executable(libskk-integration-test
    libskk_integration_test.cpp
    skk_integration/yomi_extract.cpp
)
target_include_directories(libskk-integration-test PRIVATE
    .
    "${LIBSKK_PREFIX}/include")
target_link_libraries(libskk-integration-test PRIVATE
    "${LIBSKK_LIB}"
    PkgConfig::GLIB)

enable_testing()
add_test(NAME panel-dispatch COMMAND panel-dispatch-test)
add_test(NAME libskk-integration COMMAND libskk-integration-test)

# --- e2e_test ---------------------------------------------------------------
# Real end-to-end test: links the actual fcitx5 instance + testfrontend +
# our installed skk.so. Requires a reachable mozc_server at runtime, so we
# don't run it inside the nix sandbox by default — the test runner sets
# SKK_MOZC_RUN_E2E=1 explicitly when invoking it.
option(SKK_MOZC_BUILD_E2E "Build the fcitx5 testfrontend-based e2e test" OFF)
if(SKK_MOZC_BUILD_E2E)
    pkg_check_modules(FCITX5_CORE REQUIRED IMPORTED_TARGET Fcitx5Core)
    pkg_check_modules(FCITX5_UTILS REQUIRED IMPORTED_TARGET Fcitx5Utils)

    add_executable(skk-mozc-e2e-test
        e2e_test.cpp)
    target_include_directories(skk-mozc-e2e-test PRIVATE
        "${SKK_MOZC_FCITX5_INCLUDE_DIR}")
    target_link_libraries(skk-mozc-e2e-test PRIVATE
        PkgConfig::FCITX5_CORE
        PkgConfig::FCITX5_UTILS)
    target_compile_definitions(skk-mozc-e2e-test PRIVATE
        TESTING_BINARY_DIR="${SKK_MOZC_E2E_RUNTIME_DIR}"
        SKK_MOZC_ADDON_DIR="${SKK_MOZC_E2E_RUNTIME_DIR}/addons"
        SKK_MOZC_DATA_DIR="${SKK_MOZC_E2E_RUNTIME_DIR}/data")
    install(TARGETS skk-mozc-e2e-test DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

include(GNUInstallDirs)
install(TARGETS panel-dispatch-test libskk-integration-test
        DESTINATION ${CMAKE_INSTALL_BINDIR})
