# Pi Edge Camera 구현 기록

작성일: 2026-10-07. 목표 구조는 [PROJECT_ARCHITECTURE](PROJECT_ARCHITECTURE.md)를 참고한다.
이번 범위는 Pi 기능 분리·metadata·캡처 생명주기·운영 적용이다. PC VMS/UI는 후속 작업이다.

## 1. 구현된 모듈과 읽는 순서

```text
PersonDetector → TargetSelector → TrackingController → ServoClient(내부 IPC)
                                                   ↓
ONVIF Adapter / Local Debug → servo_control adapter → ControlArbiter
                                                   → PtzController → ServoDriver
```

| 실제 파일 | 담당 역할 |
| --- | --- |
| `pi/camera_stream/src/person_detector.*` | 기존 OpenCV DNN 탐지 유지 |
| `pi/camera_stream/src/tracking/target_selector.*` | 최근 중심 거리로 대상 선택, 소실 시 초기화 |
| `pi/camera_stream/src/tracking/tracking_controller.*` | 좌표 변환, 8% deadband, 최대 3° 보정, 600ms 결과 제한 |
| `pi/servo_test/src/control/control_arbiter.*` | 수동/자동 우선순위, 추적 예약, 세대 번호 |
| `pi/servo_test/src/control/ptz_controller.*` | 출처와 독립적인 범위/목표/속도/Stop/출력 상태 |
| `pi/servo_test/src/hardware/servo_driver.*` | pigpio 초기화, 각도→pulse, 실제 출력과 cleanup |
| `pi/servo_test/src/servo/servo_control.*` | 기존 함수 이름 보존, mutex, 객체 연결, 종료 플래그 |
| `common/metadata/detection_metadata.h` | v2 IPC 자료형, 직렬화, 수치/필드/bbox 검증 |
| `pi/onvif_service/src/events/` | 내부 관측을 ONVIF PullPoint 이벤트로 변환 |
| `pi/camera_stream/src/tracking/capture_lease.cpp` | 추적 필요 시 내부 RTSP reader 유지 |
| `edge-camera/CMakeLists.txt` | 기존 Pi 프로젝트 세 개의 통합 빌드 진입점 |

기존 `pi/` 빌드·설치·실행 파일 경로를 유지한다. 같은 소스 복사본을 만들지 않는다.
물리적 디렉토리 전체 이전은 후속 단계이며 현재 디렉토리는 기능별로 분리했다.
추적 알고리즘/모델/영상 인코더와 PAN 방향 부호는 유지했다.

## 2. 제어 불변 조건

- GPIO 소유자는 `ptz-servo.service` 하나다. AI/ONVIF/lease는 GPIO를 초기화하지 않는다.
- adapter mutex 안에서 Arbiter/Controller/Driver 상태와 출력을 보호한다.
- 수동 키·중앙 버튼·수동 ONVIF 이동·Stop은 자동 추적/예약을 취소한다.
- 모드 전환 때 세대를 증가시킨다. AI 결과의 세대가 다르면 출력 전에 폐기한다.
- 현재 위치와 목표가 같으면 바로 추적, 이동이 필요하면 목표 이동 완료 후 추적한다.
- 이동/출력 실패 시 예약을 해제한다. 실패한 GPIO 쓰기는 성공한 각도로 보고하지 않는다.
- PAN 25~155°, TILT 40~140°, 중앙 90°/90°, 50Hz, 1000~2000us 유지.
- ContinuousMove timeout 0.1~5초와 기존 20ms 궤적 갱신을 유지한다.
- Stop은 이동 계획 취소이며 마지막 pulse는 유지한다. 실제 위치 센서/비상 정지 기능은 없다.
- 종료 때 신규 입력을 막고 버튼 등록을 제거한 뒤 PWM/핀/pigpio를 정리한다.

### 내부 IPC와 외부 인터페이스

TCP 5000은 `127.0.0.1`에만 bind한다. 외부 추적/이벤트는 ONVIF를 사용한다.
기존 내부 명령은 호환성을 위해 남겨두되 새 publisher는 세대 검증 경로를 사용한다.

- `TRACK:SNAPSHOT` → `TRACK:ON:<generation>` 또는 `TRACK:OFF:<generation>`.
- `TRACK:LEASE` → 추적 활성 또는 이동 후 추적 예약 시 ON. publisher 등록과 독립.
- `AUTO:MOVE2:<panDelta>,<tiltDelta>,<generation>` → 일치하는 활성 세대만 허용.
- `AUTO:DETECT2:<payload>` → 세대와 metadata 검증 후 `DETECT2:<payload>`로 관측 전달.
- 자동 이동은 ±3°, 개행 기준, 최대 명령 1024 bytes. metadata payload는 최대 768 bytes.
- 느린 관측자에게 부분 줄을 남기지 않는다. 송신할 수 없으면 연결을 종료하고
  재접속 WATCH snapshot으로 최신 상태를 복구한다. GPIO loop는 송신 대기를 하지 않는다.

## 3. ONVIF와 metadata 계약

### 운영 서비스

- Device: `http://192.168.0.92:8080/onvif/device_service`
- Media: `http://192.168.0.92:8080/onvif/media_service`
- PTZ: `http://192.168.0.92:8080/onvif/ptz_service`
- Events: `http://192.168.0.92:8080/onvif/events_service`
- ProfileToken: `main`. 주소는 환경에 따라 달라지므로 VMS는 조회 결과를 사용한다.
- 인증: 기존 HTTP Digest와 저장소 밖 계정 파일 유지. 실제 비밀번호는 기록하지 않는다.

추적 시작은 `MoveAndStartTracking`, 종료는 `Stop`이다. 지원은 현재 위치/TargetPosition의
PTZVector 방식이다. ObjectID/Geo/Preset/Speed 지정은 미지원이다. 조회/이동의 기존
AbsoluteMove/RelativeMove/ContinuousMove/GetStatus 계약도 유지한다.

Events는 GetEventProperties, CreatePullPointSubscription, PullMessages,
SetSynchronizationPoint, Renew, Unsubscribe를 제공한다.
Topic namespace는 `urn:ptz-camera:events`이며 다음 제조사 확장 토픽을 공개한다.

- Camera/Publishing: Connected, Available.
- Tracking/State: Enabled, Available.
- Analytics/PersonDetection: 아래 탐지 필드와 Available.

| 필드 | 의미/단위 |
| --- | --- |
| Detected / Class / Confidence | 유무, person, 0~1 |
| SchemaVersion / SelectedTarget | 새 탐지 메시지는 2, 현재 선택 대상 여부 |
| X / Y / ErrorX / ErrorY | 송출 이미지 pixel 중심/화면 중심 오차 |
| BoxX / BoxY / BoxWidth / BoxHeight | 송출 이미지 pixel bbox, 오른쪽/아래 끝 제외 |
| ImageWidth / ImageHeight | 현재 1280×720 |
| FrameId | 해당 publisher에서 분석한 샘플 순번. 센서 전체 프레임 번호가 아님 |
| StreamEpoch | publisher 시작 식별자. 재시작 후 FrameId 중복 구별 |
| FramePtsNs | 해당 파이프라인 running time의 ns, -1은 알 수 없음 |
| CaptureUnixMs | PTS 나이로 추정한 UTC ms, 0은 알 수 없음 |
| CaptureTimeSource | pts-estimated 또는 unknown. 센서 노출 시각 직접 측정이 아님 |
| AnalysisUnixMs | 추론 결과 생성 UTC ms |
| Generation | 서보 추적 모드 전환 번호 |
| PanAngle / TiltAngle | 서보 서비스의 명령 각도. 실측 엔코더 값 아님 |
| tt:Message UtcTime | ONVIF broker 이벤트 생성 시각. CaptureUnixMs와 구분 |

레거시 탐지/Detected=false는 일부 필드가 없을 수 있다. VMS는 존재 여부를 확인해야 한다.
FramePtsNs와 영상 timestamp의 대응은 별도 검증이 필요하다. FrameId만으로 영상 동기화를
보장하지 않는다. 큰 64bit 정수는 JavaScript 정밀도 손실 없이 문자열/BigInt로 처리한다.
현재 객체 선택은 안정적인 ObjectID/Re-ID를 제공하지 않는다.
이것은 ONVIF Events 확장이며 Profile M RTP metadata/전체 적합성 인증 구현은 아니다.

## 4. 추적 중 캡처 유지

기존 MediaMTX runOnDemand를 재사용하고 runOnInit으로 C++ `ptz_capture_lease`를 실행한다.

```text
시청자 없음 + 추적 OFF → reader 없음 → publisher 종료/대기
시청자 있음           → 일반 reader → publisher 유지
시청자 없음 + 추적 ON → 내부 reader → publisher와 AI 유지
추적 OFF/제어 단절     → 내부 reader 해제 → 일반 시청자가 없으면 종료 유예 후 정지
```

- lease는 추적 필요 상태만 250ms 간격으로 조회한다. 디코딩/모델/GPIO를 사용하지 않는다.
- FFmpeg libavformat으로 압축 packet을 읽고 버린다. AI가 자기 자신에게 ONVIF를 보내지 않는다.
- reader가 추가되므로 내부 RTSP 전송 비용은 있지만 별도 인코딩·추론은 발생하지 않는다.
- 개방/읽기 중에도 종료/제어 단절을 검사한다. 소켓 조회 timeout은 200ms다.
- RTSP 개방 제한 16초, 읽기 제한 1초, 실패 후 재시도 1초. 종료 시간은 엄격한 실시간 보장이 아니다.
- 운영 마지막 reader 종료 유예는 기존 5초다.
- 카메라/서보 서비스 자체를 중지하면 무인 추적은 동작하지 않는다.

## 5. 빌드와 시험 조건

추가 개발 패키지:

```sh
sudo apt install libavformat-dev libavcodec-dev libavutil-dev
```

기존 프로젝트별 빌드도 유지한다.

```sh
cmake -S pi/servo_test -B pi/servo_test/build
cmake --build pi/servo_test/build -j2
ctest --test-dir pi/servo_test/build --output-on-failure

cmake -S pi/camera_stream -B pi/camera_stream/build
cmake --build pi/camera_stream/build -j2
ctest --test-dir pi/camera_stream/build --output-on-failure

cmake -S pi/onvif_service -B pi/onvif_service/build
cmake --build pi/onvif_service/build -j2
ctest --test-dir pi/onvif_service/build --output-on-failure

cmake -S tools/performance -B tools/performance/build -DPTZ_BUILD_GPIO_MOCK_TEST=ON
cmake --build tools/performance/build --target servo_logging_test -j2
ctest --test-dir tools/performance/build --output-on-failure
```

| 시험 | 결과 | 범위 |
| --- | --- | --- |
| Servo CTest | 2/2 통과 | 순수 궤적, FakeDriver 제어 정책/세대/오류 |
| Camera CTest | 2/2 통과 | 대상/좌표/결과 나이, 캡처 lease 생명주기 |
| ONVIF CTest | 6/6 통과 | XML/HTTP/인증/PTZ/이벤트/비동기 구독 |
| Performance GPIO mock | 1/1 통과 | 실제 키·버튼·IPC parser·v2 metadata와 가짜 PWM |
| 모델 점검 | 성공 | 기존 MobileNet-SSD 로딩/추론 |
| 설치된 ONVIF | 성공 | Events 조회/구독/초기 알림/Renew/Unsubscribe, PTZ 상태 조회 |
| 실제 카메라 RTSP | 성공 | 한 프레임 디코딩, 종료 후 reader 0/notReady |

캡처 lease 시험은 실제 카메라 대신 합성 H.264와 격리된 MediaMTX/가짜 제어 서버를 사용한다.
일반 시청자 없이 추적 ON으로 시작, OFF로 해제, 제어 서버 단절로 해제를 검증한다.
시험 포트는 loopback 18500/18554/19998이며 기존 설치 서비스에 접속하지 않는다.
MediaMTX/FFmpeg/curl이 없으면 해당 통합 시험은 등록되지 않으므로 시험 목록을 확인한다.

## 6. 운영 적용과 복구

세 서비스의 binary와 MediaMTX private 설정을 백업한 뒤 새 버전을 적용했다.
백업: `/var/backups/ptz-edge-20261007-163231` (root 전용).
계정은 유지했고 시작 동작으로 두 축은 90°를 출력했다. 현재 추적 OFF, 서비스 세 개 active다.
Events/추적 API의 이전 소스-운영 버전 차이를 해소했다.

재설치는 각 프로젝트 install.sh를 사용한다. 카메라 install.sh는 lease binary도 설치한다.
서보 재시작은 중앙 복귀를 수행한다. 계정 파일 내용과 private YAML은 Git에 넣지 않는다.

복구할 때 세 서비스를 중지하고 백업의 servo_test/ptz_onvif/ptz_camera_publisher와
mediamtx.yml을 원래 설치 위치로 복원한 뒤 시작한다. 옛 YAML에는 runOnInit이 없으므로
새 lease binary가 남아 있어도 실행되지 않는다.

## 7. 남은 실물/PC 작업

- 사람 이동 장면의 실제 추종·소실·재획득과 수동 개입 확인.
- PAN 반전 보정과 기구 범위/방향 확인. 이번에 방향을 변경하지 않았다.
- 간헐적 핑크 프레임 조사와 Qt/Web 실제 표시 지연 측정.
- 외부 VMS의 metadata 해석과 영상 오버레이 시각 정렬.
- Preset, Recording/Playback 등은 후속 기능이다.
- PC VMS 구현 후 일반 클라이언트 경로를 VMS API/영상 주소로 전환.

이번 시험은 실물 서보 추종 성능이나 영상과 bbox의 완전 동기화까지 증명하지 않는다.
