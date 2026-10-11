> 과거 설계/구현 기록입니다. 현재 작업 트리와 배포 차이는 [현재 구조](EDGE_CAMERA_ARCHITECTURE.md), [전체 시스템](SYSTEM_ARCHITECTURE.md)을 우선 확인하세요.

# Edge Camera 소프트웨어 구현 보고서

작성일: 2026-10-07. 사용자 지시에 따라 실물 시험·5분/30분 반복 시험·운영 서비스 재시작은
수행하지 않는다. 현재 설치 서비스는 이전 커밋 a649d49 버전이고 아래 내용은 새 소스다.

## 구현 범위

| Milestone | 수정/추가 파일 | 클래스 역할과 변경 | 검증 |
| --- | --- | --- | --- |
| 1 PTZ | common/config, ptz_controller, servo_driver, button, config_test | JSON 범위·중앙·반전·80ms debounce, 최종 clamp, 정수 overflow/NaN/출력 오류 | 설정/가짜 Driver/가짜 GPIO |
| 2 Capture/Detection | camera/capture_manager.*, main, person_detector | 기존 tee pipeline RAII, 단일 CSI source, 빌린 BGR frame, 분석1개/송출2개 큐, 비정상 모델 수치 거절 | 합성 source로 실제 pipeline 검사 |
| 3 Tracking | tracking_controller.*, target_selector.*, worker | 선택 책임 통합, NO_TARGET/ACQUIRED/TRACKING/LOST, 소실1초, dead zone, 오래된 결과 폐기 | 가상 시각으로 소실/재획득/보정 검사 |
| 4 RTSP | CaptureManager, mediamtx.yml, configure_access, capture_lease | 기존 x264/RTSP 유지, 설정 path/port 연동, PC 역할인 WebRTC/HLS 기본 OFF | 격리 MediaMTX·합성 H.264 |
| 5 Metadata/Control | control/camera_control.*, event_broker/service, command_protocol, rate_limiter | Digest JSON tracking_set/get·metadata_get, 상태 이벤트/5Hz 실시간 값 분리 | JSON/인증/이벤트 모의 시험 |
| 6 ONVIF Device/Media | protocol/service, main | 기존 조회 유지, Device capability/category와 확장 주소 광고, 설정 RTSP URI | 기존 SOAP/HTTP/인증 시험 |
| 7 ONVIF PTZ | ptz_service, controller/driver | 기존 이동/Stop/GetStatus, GotoHomePosition 중앙 목표, GPIO 단일 소유 | 좌표/RPC/가짜 출력 시험 |
| 8 Arbitration | control_arbiter.*, servo_control, command_protocol | 사용자 추적 요청 유지, 수동 일시 중지, Stop/timeout 후 유예·새 세대로 AUTO 복귀 | 가상 시각·세대·취소·fault 검사 |

소프트웨어 시험 통과는 실제 방향·기구 충돌·추종·VMS 수신의 최종 통과를 의미하지 않는다.
추가된 .cpp는 한글 설명을 포함한다. 기존 프로그램·설치 경로와 PersonDetector 모델을 재사용한다.

## Data Flow

```text
CaptureManager: libcamerasrc 1개 → tee
  ├─ bounded raw queue → x264 → h264parse → RTSP publish → MediaMTX → VMS
  └─ bounded BGR appsink → AnalysisFrame(view) → PersonDetector
       → TrackingController(기존 TargetSelector 정책) → 세대/나이 확인
       ├─ 자동 출력 요청 → 내부 IPC → ControlArbiter → PtzController → ServoDriver
       └─ 상태/실시간 metadata → 내부 관측 → ONVIF Events → VMS

VMS custom JSON → 기존 인증 HTTP 서버 → loopback RPC → 추적 요청 설정
VMS ONVIF PTZ → 같은 Controller → 수동 우선 정책
```

## Thread Flow

- GStreamer 내부 worker가 캡처/인코딩/분기를 처리한다. AI는 별도 기존 std::thread다.
- AnalysisFrame은 sample/map 수명을 소유한다. 통신 전에 mapping을 해제한다.
- 모델 실패는 5초 후 재시도하고 영상 pipeline과 분리한다.
- 서보 main의 기존 20ms loop를 유지한다. callback/network/AI는 공통 adapter mutex를 사용한다.
- ONVIF와 custom JSON은 기존 HTTP poll loop와 Digest를 공유한다. 새 TCP 서버/thread 없음.
- JSON은 시작 설정과 제어 요청에만 사용한다. 큰 모델/frame에 JSON을 적용하지 않는다.

## Configuration

기본 예제: `edge-camera/config/camera.json`.
기본 운영 파일: `/etc/ptz-camera/camera.json`, 별도 파일: `PTZ_CONFIG_FILE=/path/file.json`.
파일이 없으면 기존 기본값을 사용한다. 명시한 파일 오류, 잘못된 자료형/범위/키는 시작을 거절한다.
설정은 worker 시작 전에 읽으며 실행 중 reload는 지원하지 않는다.

- 실제 사용 핀은 GPIO18/19/17로 검증한다.
- 현재 범위25~155/40~140, 중앙90/90을 보존했다.
- invertPan/invertTilt는 min+max-angle로 물리 출력만 반전한다. 논리 각도/ONVIF 좌표는 동일하다.
- 반전 후에도 물리 각도는 같은 범위 안이다. center 값은 논리 각도 기준이다.
- dead zone은 송출1280×720의 pixel 단위다. 예제51/29는 기존 약8%를 유지한다.
- metadataHz1~10, 기본5. lostTimeoutMs1000, manualResumeMs1000, frame age 제한600ms.
- RTSP path/port 변경 후에는 MediaMTX private 설정도 재생성해야 한다.
- GPIO·서보·ONVIF·lease 프로세스는 같은 파일을 읽어야 한다.
- 설치 스크립트는 기존 사용자 JSON을 덮어쓰지 않는다.

추가 대형 dependency는 설치하지 않았다. 이 Pi에 있는 `nlohmann/json.hpp`를 사용한다.
다른 Pi에서는 `nlohmann-json3-dev` 개발 헤더가 필요하다.

## VMS 연결 계약

### 표준 PTZ

Device/Media/PTZ/Events 주소와 ProfileToken `main` 유지. RTSP 기본 `/cam` 유지.
ContinuousMove/AbsoluteMove/RelativeMove/Stop/GetStatus와 GotoHomePosition을 사용한다.
표준 좌표 -1~1의 공간을 조회한다. 중앙 위치는 설정값이며 encoder 실측은 없다.

**Stop의 새 동작:** 수동 계획을 중단하지만 사용자 추적 요청이 ON이면 유예 뒤 AUTO로 복귀한다.
추적을 완전히 끄려면 아래 custom tracking_set(false)를 사용한다.
기존 MoveAndStartTracking은 호환을 위해 남겨두지만 새 VMS의 추적 스위치는 custom channel을 사용한다.

### Custom camera control

```text
POST http://<PI_IP>:8080/camera/control
Content-Type: application/json
HTTP Digest: 기존 카메라 계정
```

```json
{"type":"tracking_set","cameraId":"CAM01","requestId":"r1","data":{"enabled":true}}
```

```json
{"type":"tracking_set","cameraId":"CAM01","data":{"enabled":false}}
```

```json
{"type":"tracking_get","cameraId":"CAM01"}
```

```json
{"type":"metadata_get","cameraId":"CAM01"}
```

성공 응답 type은 요청명 뒤에 `_result`다. tracking 응답에는 enabled(사용자 요청),
autoAllowed(현재 출력 허용), ptzMode, generation, moving/fault, pan/tilt를 제공한다.
metadata 응답에는 최신 cameraState/streamState/trackingState/targetState/PTZ/detection을 제공한다.
metadata_get은 상태 조회이고 이벤트 이력 저장 API는 아니다. 요청 빈도는 VMS에서5Hz 이하로 제한한다.
요청 상한4096 bytes, 잘못된 cameraId/자료형/필드는400, backend 장애500, 무인증401이다.
큰 FrameId/StreamEpoch/Generation은 JSON 문자열로 전달해 JavaScript 정밀도를 보존한다.

명령 시험 예시(실제 추적이 시작되므로 최종 실물 검증 시 사용):

```sh
curl --digest --user '<사용자명>' \
  -H 'Content-Type: application/json' \
  -d '{"type":"tracking_set","cameraId":"CAM01","data":{"enabled":true}}' \
  http://192.168.0.92:8080/camera/control
```

비밀번호는 curl 프롬프트에 입력한다. argv/저장소에 넣지 않는다.
이 endpoint는 카메라 custom feature이며 PC VMS REST API가 아니다.

### 이벤트

기존 ONVIF PullPoint를 사용한다. `urn:ptz-camera:events` 제조사 topic:

- Camera/Publishing: 카메라/송출 상태.
- Tracking/State: 추적 요청, 자동 출력 허용, 대상 상태.
- PTZ/State: 모드·이동·오류·명령 각도·세대.
- Analytics/PersonDetection: 기존 bbox/confidence/error/frame/time.
- Runtime/StateEvent: PERSON_DETECTED/PERSON_LOST/TRACKING_STARTED/TRACKING_STOPPED.

실시간 property Changed는 최신 값으로 합친다. 상태 전환은 비 property 이벤트로 분리해
같은 topic의 연속 전환을 합치지 않는다. 각 구독 큐64개 상한이며 영구 이벤트 저장은 VMS 책임이다.
대상 소실은 timeout 기준이고 짧은 탐지 누락은 bbox를 숨기되 대상 LOST 전환은 지연한다.
이는 Profile M RTP metadata 구현이나 ONVIF 전체 인증을 의미하지 않는다.

## Build / 실행 / 검증

```sh
cmake -S edge-camera -B edge-camera/build
cmake --build edge-camera/build -j2
ctest --test-dir edge-camera/build --output-on-failure
```

기존 독립 빌드도 유지한다.

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
```

GPIO mock는 실제 드라이버/키/버튼/내부 parser를 시험한다.

```sh
cmake -S tools/performance -B tools/performance/build -DPTZ_BUILD_GPIO_MOCK_TEST=ON
cmake --build tools/performance/build --target servo_logging_test -j2
ctest --test-dir tools/performance/build --output-on-failure
```

실행 방법은 기존 서비스/키 클라이언트를 유지한다. 새 binary를 설치하고 서비스를
재시작하면 시작 중앙 출력이 발생하므로, 이번 구현 작업에서는 실행하지 않는다.

```sh
# 최종 실물 연결 후 적용. ONVIF 도구를 먼저 갱신해 설정 renderer 버전을 맞춘다.
sudo ./pi/onvif_service/install.sh
sudo ./pi/servo_test/install.sh
sudo ./pi/camera_stream/install.sh
./pi/servo_test/build/servo_test --client
journalctl -u ptz-servo -u ptz-onvif -u ptz-camera -f
```

## 실제 소프트웨어 결과

시험 중단 지시 전에 완료된 빌드/모의 시험: 서보3, 카메라3, ONVIF7, GPIO mock1이 통과했다.
이는 당시 소스 기준이며 마지막 상태 전달 보강 이후 추가 시험은 사용자 지시로 수행하지 않았다.
실물/PTZ 방향/실제 카메라 사람 추종/PC VMS/30분 시험은 사용자 지시로 수행하지 않는다.
