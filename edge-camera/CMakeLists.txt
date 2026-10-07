cmake_minimum_required(VERSION 3.16)
project(edge_camera LANGUAGES CXX)
# 기존 pi 프로젝트를 그대로 사용하는 통합 진입점. 소스/설치 경로를 복제하지 않는다.
enable_testing()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../pi/servo_test servo)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../pi/camera_stream camera)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../pi/onvif_service onvif)
