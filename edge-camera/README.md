# Edge Camera

Pi 구성 요소의 통합 CMake 진입점이다. 기존 서비스와 설치 경로를 유지하기 위해 실제
소스는 현재 `pi/`에 둔다. 같은 코드를 두 곳에 복사하지 않는다.

## 디렉토리와 빌드 구조

```text
edge-camera/
├── CMakeLists.txt          # 기존 세 프로젝트를 add_subdirectory로 연결
├── config/camera.json      # 하드웨어·영상·추적 설정
└── README.md

통합 CMake
├── pi/camera_stream → ptz_camera_publisher
├── pi/servo_test → servo_test
└── pi/onvif_service → ptz_onvif
    └── common/config → 공유 설정
```

통합 빌드는 실행 파일을 만들며 실제 서비스 설치는 각 `pi/*/install.sh`가 담당합니다.
설치 스크립트는 서비스를 변경할 수 있으므로 소스 빌드와 운영 적용을 구분하세요.

```sh
cmake -S edge-camera -B edge-camera/build
cmake --build edge-camera/build -j2
ctest --test-dir edge-camera/build --output-on-failure
```

## 코드 읽는 순서

1. `pi/camera_stream/src/person_detector.*`: 사람 탐지.
2. `pi/camera_stream/src/tracking/target_selector.*`: 대상 선택.
3. `pi/camera_stream/src/tracking/tracking_controller.*`: 좌표와 보정량 계산.
4. `pi/camera_stream/src/servo_client.*`: 내부 IPC.
5. `pi/servo_test/src/control/control_arbiter.*`: 수동/자동 우선순위.
6. `pi/servo_test/src/control/ptz_controller.*`: 출처와 독립적인 이동 계획.
7. `pi/servo_test/src/hardware/servo_driver.*`: 실제 PWM 출력.
8. `pi/onvif_service/src/events/`: 외부 ONVIF 이벤트.

`servo/servo_control.*`은 기존 호출부를 보존하고 공유 mutex를 소유하는 adapter다.
`common/metadata/detection_metadata.h`는 IPC 탐지 자료형과 검증 규칙이다.
`tracking/capture_lease.cpp`는 자동 추적 중에만 내부 RTSP reader를 유지한다.

세부 계약과 검증 조건은 [Pi 구현 기록](../docs/EDGE_CAMERA_IMPLEMENTATION.md)을 참고한다.

## 최신 구현

[Edge Camera 설계](../docs/EDGE_CAMERA_ARCHITECTURE.md)와
[소프트웨어 변경·VMS 계약·적용 방법](../docs/EDGE_CAMERA_SOFTWARE_REPORT.md)을 참고한다.
설정 예제는 `config/camera.json`이다. 실제 모터/장시간 검증은 사용자가 진행한다.

## 현재 시스템 연동 기준 (2026-10-11)

Qt 라이브는 Pi 직접 RTSP, Web 라이브는 PC VMS 내부 libdatachannel WebRTC입니다. PC MediaMTX 게이트웨이는 제거했고 Pi RTSP 서버는 유지합니다. 녹화·SQLite·자연어 검색은 VMS 책임입니다.

최신 Pi 소스의 독립 /camera/control 추적 설정과 VMS의 기존 ONVIF Tracking 계약은 아직 맞춰야 합니다. 소스 업데이트가 설치 서비스 적용을 의미하지 않습니다. 전체 역할·검증 범위는 [저장소 README](../README.md)를 기준으로 확인하세요.
