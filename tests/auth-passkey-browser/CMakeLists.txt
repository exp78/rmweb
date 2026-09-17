cmake_minimum_required(VERSION 3.16)
project(auth_passkey_browser_smoke LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WPE REQUIRED IMPORTED_TARGET wpe-webkit-2.0 wpe-platform-2.0 glib-2.0 gobject-2.0)
add_executable(auth-passkey-browser-smoke browser-test.cpp
    ../../engine/wpeqt/auth-policy.cpp ../../engine/wpeqt/auth-surface.cpp
    ../../engine/wpeqt/auth-passkey.cpp ../../engine/wpeqt/qtfbclient.cpp)
target_link_libraries(auth-passkey-browser-smoke PRIVATE Qt6::Core Qt6::Gui PkgConfig::WPE)
target_compile_definitions(auth-passkey-browser-smoke PRIVATE QT_NO_KEYWORDS)
target_compile_options(auth-passkey-browser-smoke PRIVATE -Wall -Wextra -Werror)
