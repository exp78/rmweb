cmake_minimum_required(VERSION 3.21)
project(rmweb_display_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Gui Qml Quick)
qt_standard_project_setup()

add_executable(display-spike main.cpp)
target_link_libraries(display-spike PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick)
