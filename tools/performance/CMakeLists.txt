cmake_minimum_required(VERSION 3.16)
project(ptz_performance LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../common/config/edge_config.cmake)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
pkg_check_modules(OPENCV REQUIRED IMPORTED_TARGET opencv4)
add_executable(pipeline_benchmark pipeline_benchmark.cpp ../../pi/camera_stream/src/person_detector.cpp)
target_include_directories(pipeline_benchmark PRIVATE ../../common ../../pi/camera_stream/src)
target_link_libraries(pipeline_benchmark PRIVATE PkgConfig::GSTREAMER PkgConfig::OPENCV Threads::Threads)
add_executable(resource_sampler resource_sampler.cpp)
add_executable(summarize summarize.cpp)
foreach(program pipeline_benchmark resource_sampler summarize)
    target_compile_options(${program} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

# 실물 GPIO 없이 기존 제어 동작과 이벤트 연결을 검증하는 선택적 테스트.
option(PTZ_BUILD_GPIO_MOCK_TEST "Build servo logging test with fake pigpio" OFF)
if(PTZ_BUILD_GPIO_MOCK_TEST)
    find_path(PIGPIO_INCLUDE_DIR pigpio.h REQUIRED)
    add_executable(servo_logging_test
        servo_logging_test.cpp
        ../../pi/servo_test/src/servo/servo_control.cpp
        ../../pi/servo_test/src/control/ptz_controller.cpp
        ../../pi/servo_test/src/control/control_arbiter.cpp
        ../../pi/servo_test/src/hardware/servo_driver.cpp
        ../../pi/servo_test/src/motion/motion_plan.cpp
        ../../pi/servo_test/src/button/center_button.cpp
        ../../pi/servo_test/src/keyboard/keyboard_control.cpp
        ../../pi/servo_test/src/network/tcp_server.cpp
        ../../pi/servo_test/src/protocol/command_protocol.cpp
    )
    target_include_directories(servo_logging_test PRIVATE
        ../../common ../../pi/servo_test/src ${PIGPIO_INCLUDE_DIR})
    target_link_libraries(servo_logging_test PRIVATE edge_config Threads::Threads)
    # Release 빌드에서도 검증식이 제거되지 않도록 한다.
    target_compile_options(servo_logging_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
    enable_testing()
    add_test(NAME servo_logging
        COMMAND servo_logging_test ${CMAKE_CURRENT_BINARY_DIR}/servo-test-events.csv)
endif()
