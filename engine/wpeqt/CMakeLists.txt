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
# Export symbols to the dynamic table so backtrace_symbols_fd() can name our frames in the crash handler.
target_link_options(rmweb-wpeqt PRIVATE -rdynamic)

# The bundle ships our libs in lib/ next to bin/: bake a $ORIGIN-relative RUNPATH into the
# binary so bin/rmweb-wpeqt also starts WITHOUT the LD_LIBRARY_PATH wrapper (rmweb-env.sh).
# Use INSTALL_RPATH (not a raw -Wl,-rpath flag) so CMake keeps `$ORIGIN` literal; we never
# run `cmake --install` for this target, so force the install rpath into the build tree too.
set_target_properties(rmweb-wpeqt PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib"
    BUILD_WITH_INSTALL_RPATH TRUE)

# Hardening: the browser runs as root on the device, and a segfault there costs a reboot —
# surface problems at compile time and make the binary itself harder to exploit.
# _FORTIFY_SOURCE=2 is a no-op without optimization, so define it only for optimized configs
# (the release build uses -DCMAKE_BUILD_TYPE=Release -> -O2; see scripts/build-wpeqt.sh).
target_compile_options(rmweb-wpeqt PRIVATE
    -Wall -Wextra -fstack-protector-strong
    $<$<CONFIG:Release>:-D_FORTIFY_SOURCE=2>
    $<$<CONFIG:RelWithDebInfo>:-D_FORTIFY_SOURCE=2>
    $<$<CONFIG:MinSizeRel>:-D_FORTIFY_SOURCE=2>)
# Full RELRO + bind-now: GOT is read-only after relocation (root process, no lazy-binding window).
target_link_options(rmweb-wpeqt PRIVATE -Wl,-z,relro,-z,now)
