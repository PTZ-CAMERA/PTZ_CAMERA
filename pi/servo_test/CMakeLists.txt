cmake_minimum_required(VERSION 3.16)
project(servo_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../common/config/edge_config.cmake)

find_path(PIGPIO_INCLUDE_DIR pigpio.h REQUIRED)
find_library(PIGPIO_LIBRARY NAMES pigpio REQUIRED)

add_executable(servo_test
    src/main.cpp
    src/servo/servo_control.cpp
    src/control/ptz_controller.cpp
    src/control/control_arbiter.cpp
    src/hardware/servo_driver.cpp
    src/motion/motion_plan.cpp
    src/button/center_button.cpp
    src/keyboard/keyboard_control.cpp
    src/network/tcp_server.cpp
    src/protocol/command_protocol.cpp
)
target_include_directories(servo_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PIGPIO_INCLUDE_DIR}
)
target_link_libraries(servo_test PRIVATE edge_config ${PIGPIO_LIBRARY} pthread rt)

# 공통 성능 로그 헤더. PTZ_PERF_LOG가 설정된 실행에서만 기록한다.
target_include_directories(servo_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../common)
enable_testing()
add_executable(motion_plan_test tests/motion_plan_test.cpp src/motion/motion_plan.cpp)
target_include_directories(motion_plan_test PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../../common)
target_compile_options(motion_plan_test PRIVATE -UNDEBUG)
target_link_libraries(motion_plan_test PRIVATE edge_config)
add_test(NAME motion_plan COMMAND motion_plan_test)

add_executable(control_layers_test tests/control_layers_test.cpp
    src/control/ptz_controller.cpp src/control/control_arbiter.cpp src/motion/motion_plan.cpp)
target_include_directories(control_layers_test PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../../common)
target_compile_options(control_layers_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
target_link_libraries(control_layers_test PRIVATE edge_config)
add_test(NAME control_layers COMMAND control_layers_test)

add_executable(config_test tests/config_test.cpp)
target_link_libraries(config_test PRIVATE edge_config)
target_compile_options(config_test PRIVATE -UNDEBUG)
add_test(NAME config COMMAND config_test)
