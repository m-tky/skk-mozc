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
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSKK REQUIRED IMPORTED_TARGET libskk)

add_executable(libskk-integration-test
    libskk_integration_test.cpp
    skk_integration/yomi_extract.cpp
)
target_include_directories(libskk-integration-test PRIVATE .)
target_link_libraries(libskk-integration-test PRIVATE PkgConfig::LIBSKK)

enable_testing()
add_test(NAME panel-dispatch COMMAND panel-dispatch-test)
add_test(NAME libskk-integration COMMAND libskk-integration-test)

include(GNUInstallDirs)
install(TARGETS panel-dispatch-test libskk-integration-test
        DESTINATION ${CMAKE_INSTALL_BINDIR})
