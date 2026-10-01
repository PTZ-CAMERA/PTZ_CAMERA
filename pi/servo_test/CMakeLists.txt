cmake_minimum_required(VERSION 3.16)
project(servo_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(PIGPIO_INCLUDE_DIR pigpio.h REQUIRED)
find_library(PIGPIO_LIBRARY NAMES pigpio REQUIRED)

add_executable(servo_test src/main.cpp)
target_include_directories(servo_test PRIVATE ${PIGPIO_INCLUDE_DIR})
target_link_libraries(servo_test PRIVATE ${PIGPIO_LIBRARY} pthread rt)
