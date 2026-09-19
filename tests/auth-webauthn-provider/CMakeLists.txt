cmake_minimum_required(VERSION 3.16)
project(native_webauthn_provider_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WPE REQUIRED IMPORTED_TARGET wpe-webkit-2.0 wpe-platform-2.0 gio-2.0)
add_executable(native-webauthn-provider-test provider-test.cpp)
target_link_libraries(native-webauthn-provider-test PRIVATE PkgConfig::WPE ${CMAKE_DL_LIBS})
target_compile_options(native-webauthn-provider-test PRIVATE -Wall -Wextra -Werror)

# Export the test-only syslog sink for real shared-engine diagnostic checks.
set_target_properties(native-webauthn-provider-test PROPERTIES ENABLE_EXPORTS TRUE)
