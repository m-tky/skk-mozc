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

enable_testing()
add_test(NAME panel-dispatch COMMAND panel-dispatch-test)

include(GNUInstallDirs)
install(TARGETS panel-dispatch-test
        DESTINATION ${CMAKE_INSTALL_BINDIR})
