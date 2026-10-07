# Edge Camera

Pi 구성 요소의 통합 CMake 진입점이다. 기존 서비스와 설치 경로를 유지하기 위해 실제
소스는 현재 `pi/`에 둔다. 같은 코드를 두 곳에 복사하지 않는다.

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
