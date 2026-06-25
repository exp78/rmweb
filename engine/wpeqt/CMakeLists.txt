cmake_minimum_required(VERSION 3.16)
project(rmweb_wpeqt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Qt6 from the device sysroot; WPE WebKit + GLib from our staged prefix (build/stage), resolved
# via pkg-config (the in-container build seeds the sysroot + pins PKG_CONFIG_SYSROOT_DIR).
find_package(Qt6 REQUIRED COMPONENTS Gui Qml Quick)
qt_standard_project_setup()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WPE REQUIRED IMPORTED_TARGET
    wpe-webkit-2.0 wpe-platform-2.0 glib-2.0 gobject-2.0)

qt_add_executable(rmweb-wpeqt main.cpp)
target_link_libraries(rmweb-wpeqt PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick PkgConfig::WPE)
# WPE/WebKit pull in GLib's gio, whose GDBus structs have members named `signals`/`slots`.
# Disable Qt's `signals`/`slots`/`emit` keyword macros (we use Q_SIGNALS/Q_SLOTS/Q_EMIT).
target_compile_definitions(rmweb-wpeqt PRIVATE QT_NO_KEYWORDS)
