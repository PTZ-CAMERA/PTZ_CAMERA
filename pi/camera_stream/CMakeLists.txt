cmake_minimum_required(VERSION 3.16)
project(ptz_camera_publisher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
pkg_check_modules(OPENCV REQUIRED IMPORTED_TARGET opencv4)

add_executable(ptz_camera_publisher src/main.cpp src/person_detector.cpp src/servo_client.cpp src/tracking/target_selector.cpp src/tracking/tracking_controller.cpp)
target_link_libraries(ptz_camera_publisher PRIVATE PkgConfig::GSTREAMER PkgConfig::OPENCV Threads::Threads)
target_compile_options(ptz_camera_publisher PRIVATE -Wall -Wextra -Wpedantic)

# 공통 성능 로그 헤더. PTZ_PERF_LOG가 설정된 실행에서만 기록한다.
target_include_directories(ptz_camera_publisher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../common)

target_include_directories(ptz_camera_publisher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# GPIO나 모델을 열지 않고 자동 추적용 RTSP reader만 유지한다.
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
add_executable(ptz_capture_lease src/tracking/capture_lease.cpp)
target_link_libraries(ptz_capture_lease PRIVATE PkgConfig::AVFORMAT Threads::Threads)
target_compile_options(ptz_capture_lease PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(tracking_test tests/tracking_test.cpp
    src/tracking/target_selector.cpp src/tracking/tracking_controller.cpp)
target_include_directories(tracking_test PRIVATE src)
target_link_libraries(tracking_test PRIVATE PkgConfig::OPENCV)
target_compile_options(tracking_test PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
add_test(NAME tracking COMMAND tracking_test)

# 설치된 서비스와 다른 포트/합성 영상으로 캡처 lease를 검증한다.
find_program(MEDIAMTX_EXECUTABLE mediamtx PATHS /usr/local/bin)
find_program(FFMPEG_EXECUTABLE ffmpeg)
find_program(CURL_EXECUTABLE curl)
if(MEDIAMTX_EXECUTABLE AND FFMPEG_EXECUTABLE AND CURL_EXECUTABLE)
    add_executable(capture_lease_fixture tests/capture_lease_fixture.cpp)
    add_test(NAME capture_lease COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/capture_lease_test.sh
        $<TARGET_FILE:ptz_capture_lease> $<TARGET_FILE:capture_lease_fixture>
        ${MEDIAMTX_EXECUTABLE} ${FFMPEG_EXECUTABLE} ${CURL_EXECUTABLE})
    set_tests_properties(capture_lease PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
endif()
