cmake_minimum_required(VERSION 3.16)
project(ptz_qt_video_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS WebEngineWidgets)

add_executable(ptz_qt_video_test main.cpp)
target_link_libraries(ptz_qt_video_test PRIVATE Qt6::WebEngineWidgets)
