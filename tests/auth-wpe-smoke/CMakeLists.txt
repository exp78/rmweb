cmake_minimum_required(VERSION 3.16)
project(auth_wpe_smoke LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WPE REQUIRED IMPORTED_TARGET wpe-webkit-2.0 wpe-platform-2.0 glib-2.0 gobject-2.0)
add_executable(auth-wpe-smoke ../auth_wpe_smoke.cpp ../../engine/wpeqt/auth-policy.cpp)
target_link_libraries(auth-wpe-smoke PRIVATE Qt6::Core PkgConfig::WPE)
target_compile_definitions(auth-wpe-smoke PRIVATE QT_NO_KEYWORDS)
target_compile_options(auth-wpe-smoke PRIVATE -Wall -Wextra -Werror)

# Includes the real driver callbacks through a friend test-access seam.
add_executable(auth-navigation-smoke ../auth_navigation_smoke.cpp
    ../../engine/wpeqt/auth-policy.cpp ../../engine/wpeqt/auth-surface.cpp
    ../../engine/wpeqt/auth-passkey.cpp ../../engine/wpeqt/qtfbclient.cpp)
target_link_libraries(auth-navigation-smoke PRIVATE Qt6::Core Qt6::Gui PkgConfig::WPE)
target_compile_definitions(auth-navigation-smoke PRIVATE QT_NO_KEYWORDS)
target_compile_options(auth-navigation-smoke PRIVATE -Wall -Wextra -Werror)

add_executable(auth-frame-pacing-smoke ../auth_frame_pacing_smoke.cpp
    ../../engine/wpeqt/auth-policy.cpp ../../engine/wpeqt/auth-surface.cpp
    ../../engine/wpeqt/auth-passkey.cpp ../../engine/wpeqt/qtfbclient.cpp)
target_link_libraries(auth-frame-pacing-smoke PRIVATE Qt6::Core Qt6::Gui PkgConfig::WPE)
target_compile_definitions(auth-frame-pacing-smoke PRIVATE QT_NO_KEYWORDS)
target_compile_options(auth-frame-pacing-smoke PRIVATE -Wall -Wextra -Werror)
