cmake_minimum_required(VERSION 3.16)
project(ptz_camera_publisher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)

add_executable(ptz_camera_publisher src/main.cpp)
target_link_libraries(ptz_camera_publisher PRIVATE PkgConfig::GSTREAMER)
target_compile_options(ptz_camera_publisher PRIVATE -Wall -Wextra -Wpedantic)
