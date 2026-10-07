> 최신 상태: 초기 분석 기준 문서다. 후속 소프트웨어 변경/제어 계약은 [구현 보고서](EDGE_CAMERA_SOFTWARE_REPORT.md)를 우선한다.

# Edge AI PTZ Network Camera — Raspberry Pi 4

작성일: 2026-10-07. 기준 커밋: a649d49 및 현재 Pi 설치 서비스.
목표는 기능 추가보다 영상·탐지·추적·실물 PTZ·ONVIF·metadata/control의 연결 검증이다.

## 1. System Role

Pi는 Edge Camera다. 캡처·사람 탐지·로컬 추적·서보·RTSP·외부 장치 인터페이스를 맡는다.
Recording/SQLite/Event Search/Playback/WebRTC Gateway/Qt API/REST/WebSocket/UI는 PC VMS 책임이다.
현재 Pi MediaMTX의 WebRTC/HLS 설정은 이전 직접 연결 구성이다. 새로운 PC 기능을 Pi에
추가하지 않으며 기존 시청 경로 전환은 VMS 연결 확인 후 별도로 진행한다.

### 현재 소스 구조

```text
edge-camera/                 # 기존 Pi 프로젝트의 통합 CMake
pi/camera_stream/src/
  main.cpp                   # 캡처/tee/인코딩/worker 실행
  person_detector.*          # 기존 MobileNet-SSD
  servo_client.*             # 내부 IPC
  tracking/
    target_selector.*        # 최근 중심에 가까운 사람
    tracking_controller.*    # 오차/dead zone/step 계산
    capture_lease.cpp        # 추적용 내부 RTSP reader
pi/servo_test/src/
  main.cpp                   # 서비스/키 입력/20ms loop/신호 종료
  control/                   # Arbiter, PtzController
  hardware/                  # PigpioServoDriver
  motion/                    # 순수 궤적 계산
  servo/                     # 기존 함수 adapter와 mutex
  button/, keyboard/         # 로컬 입력
  protocol/, network/        # 내부 개행 TCP
pi/onvif_service/src/
  protocol/, xml/, auth/     # SOAP/Expat/Digest
  ptz/, events/, network/    # PTZ RPC, broker/PullPoint, HTTP/Discovery
common/
  control/, metadata/, performance/
tools/performance/           # GPIO mock와 CPU/RSS/CSV 도구
```

## 2. 현재 구현 분석과 재사용

| 분석 항목 | 현재 구현 | 재사용/남은 작업 |
| --- | --- | --- |
| Capture | 하나의 libcamerasrc를 tee로 분기 | pipeline 유지, 필요한 수명 관리만 추출 |
| RTSP | x264 H.264, 1280×720/30fps, TCP 로컬 발행 | 기존 `/cam` 유지. `/cam01`은 예시이며 강제 변경하지 않음 |
| MediaMTX | RTSP 서버, runOnDemand publisher, runOnInit lease | 인코더와 서버 분리 유지 |
| PersonDetector | CPU OpenCV DNN Caffe MobileNet-SSD | class15/score0.55/NMS0.45 유지 |
| Tracking | 최근 중심 선택, 8% dead zone, 최대3°, 600ms 결과 제한 | 기존 정책 유지, target 상태/소실 시간 추가 필요 |
| Servo/PWM | Controller/Driver/pigpio, 20ms 궤적, 범위/timeout | GPIO 소유자 유지, 실물 시험 미완료 |
| TCP/Network | POSIX socket, select/poll, 개행 IPC, libcurl 진단 | 내부 프로토콜 재사용. JSON 라이브러리는 없음 |
| ONVIF | Device/Media/PTZ/Events, Digest, WS-Discovery | 기존 SOAP/XML/인증/RPC 재사용 |
| Metadata | 별도 ONVIF PullPoint, bbox/오차/각도/시각/세대 | 영상 burn-in 없음. 상태 이벤트/rate limit 보강 필요 |
| Capture 생명주기 | 시청자 또는 추적용 reader가 캡처 유지 | 검증된 demand 방식 유지 |
| 현재 문제 | PAN 방향 미확인, 실물 추종/5분/30분 시험 미완료 | 결과를 소프트웨어 시험과 구분 |
| 목표와 차이 | 수동/Stop이 추적 OFF, 별도 custom switch 없음 | 수동 일시 중지/자동 복귀와 명시적 추적 OFF 구분 필요 |

현재 범위는 PAN25~155°, TILT40~140°다. 예시의 30~150°로 임의 변경하지 않는다.
기구 안전 범위는 실물 확인으로 확정하며 현재 숫자만으로 충돌 안전성을 증명하지 않는다.

## 3. Capture Flow

```text
CSI → libcamerasrc → 1280×720/30fps → tee
                                   ├─ streaming queue → 인코딩
                                   └─ analysis queue → 분석 샘플
```

하나의 publisher만 카메라를 연다. lease/ONVIF/서보는 카메라를 열지 않는다.
GStreamer 샘플을 map한 메모리를 cv::Mat view로 읽고 추론 완료 뒤 unmap한다.
분기 자체에서 전체 이미지 복사본을 별도 큐에 쌓지 않는다. 색상 변환/resize/model blob
생성에 필요한 메모리 작업은 존재한다.
AI queue와 appsink는 각각 대기 1개, 오래된 프레임 폐기다. 송출 queue는 현재 GStreamer
기본 한도를 사용하므로 추후 송출 지연 시험에서 명시적 상한을 검토한다.

### 생명주기 선택

현재 demand 방식을 유지한다. 시청 또는 추적이 필요하면 reader가 publisher를 유지한다.
최근 합성 영상 시험에서 idle/추적 ON/OFF/제어 단절을 검증했고 실제 RTSP 수신 종료 후
reader 0/대기 상태도 확인했다. 항상 실행으로 바꾸면 idle 전력/CPU가 늘고 기존 운영
동작이 달라지므로 우선 현재 방식의 반복 시작·종료 안정성을 측정한다.

## 4. Streaming Flow

```text
raw image → videoconvert/I420 → x264enc → h264parse
          → rtspclientsink → MediaMTX → RTSP → VMS
```

x264enc가 인코더다. MediaMTX는 이미 압축된 영상을 제공하는 서버다.
현 설정: ultrafast/zerolatency, 2500kbit/s, B-frame0, key interval30.
VMS에는 ONVIF GetStreamUri로 확인한 `rtsp://<PI_IP>:8554/cam`을 전달한다.
PC ffprobe/ffplay/libavformat 수신 검증은 PC 결과와 구분한다.

## 5. Detection Flow

분석 분기는 640×360 BGR, 공급 상한10fps다. 추론은 별도 worker에서 실행한다.
300×300 모델 전처리 → forward → person/score 필터 → NMS → PersonDetection 벡터.
10fps 공급 상한은 실제 추론10fps를 보장하지 않는다. 과거 실제 장면의 추론289~311ms는
약3fps이며 새 구조/부하에서 다시 측정해야 한다.
현재 추적 OFF에서는 추론도 쉬고, 필요하면 후속 설정으로 탐지 유지와 자동 출력만
분리한다. 이번 단계에서 동작을 동시에 바꾸지 않는다.

## 6. Tracking Flow

```text
PersonDetection[] → TargetSelector → TrackingController
 → 보정/metadata → 내부 IPC → ControlArbiter → PtzController
```

현재 선택은 이전 대상 중심에 가까운 사람, 최초에는 화면 중심에 가까운 사람이다.
TrackingController가 선택기를 소유하거나 호출하는 형태로 책임을 모을 수 있다.
현재 즉시 대상 없음 처리는 존재하지만 NO_TARGET/ACQUIRED/TRACKING/LOST와
설정된 소실 시간은 없다. Milestone3에서 기존 선택 정책을 유지하며 상태를 추가한다.
표적 소실 때 보정 출력을 중지한다. 기존 자동 제어는 속도 유지가 아닌 작은 step이므로
마지막 방향으로 무한 이동하는 계획은 생성하지 않는다. 자동 중앙 복귀 기본 OFF.

## 7. PTZ Flow

```text
AI / ONVIF / Local Center
 → adapter의 동일 mutex → ControlArbiter → PtzController
 → ServoDriver → pigpio → GPIO18(PAN)/GPIO19(TILT)
```

버튼/네트워크/AI는 직접 gpioServo를 호출하지 않는다. PWM 생성은 하위 Driver가 맡는다.
Controller는 목표·상대·속도·Stop·범위·오류를 관리하며 출처를 알지 못한다.
출력 위치와 GetStatus는 명령 값이고 엔코더 측정값이 아니다.
중앙 버튼 GPIO17은 내부 pull-up, active-low이며 출력 코드와 별도 모듈이다.
외부 4.8~6V 서보 전원과 Pi GND를 공통 연결한다.

## 8. ONVIF Flow

VMS(ONVIF Client) → Pi(Device) → 좌표/인증 검증 → 내부 RPC → Controller.

- Device: GetDeviceInformation/GetCapabilities/GetServices, Discovery.
- Media: GetProfiles/GetStreamUri. ProfileToken=`main`.
- PTZ: ContinuousMove/Stop/GetStatus/AbsoluteMove/RelativeMove.
- Events: GetEventProperties/PullPoint/PullMessages/Renew/Unsubscribe 등.
- GotoHomePosition/Preset/Zoom은 현재 미지원이다.
- HTTP Digest 사용. 외부 PTZ는 loopback TCP5000을 직접 호출하지 않는다.

현재 MoveAndStartTracking/Stop에 연결한 추적 전환은 기존 계약이다. 새 목표에서는
custom Tracking ON/OFF를 표준 이동/정지와 분리해야 한다. 같은 이름으로 의미를 조용히
바꾸지 않고 VMS 계약을 갱신한 뒤 적용한다.

## 9. Metadata Flow / Custom Control 제안

현재: publisher → 내부 TCP → 서보 관측 → ONVIF broker → VMS PullPoint.
영상에 bbox를 그리지 않는다. bbox/점수/오차/명령 각도/프레임·시각을 별도로 전달한다.

현재 저장소에는 JSON 라이브러리가 없다. 새 공개 TCP/JSON 서버를 바로 추가하지 않는다.
Milestone5에서 우선 다음 두 방안을 비교하고 VMS 계약을 확정한다.

1. 기존 인증 HTTP/SOAP에 **제조사 확장 Tracking Set/Get**을 추가하고 metadata는 기존 Events 사용.
2. VMS가 JSON 채널을 요구하면 내부 TCP framing을 재사용해 별도 인증된 양방향 채널 구성.
   JSON parser 선택/포트/인증/길이 제한/부분 송신/재접속을 먼저 정의한다.

첫 방안은 기존 인증/네트워크/Expat를 재사용하며 추가 대형 의존성이 없다.
확장 명령을 표준 ONVIF PTZ로 광고하지 않는다. VMS의 JSON 요청 DTO와 Pi wire 형식은
달라도 되지만 VMS에서 변환 규칙을 명시해야 한다. 두 방안 모두 현재는 제안이다.

State event: PERSON_DETECTED/PERSON_LOST/TRACKING_STARTED/TRACKING_STOPPED는 전환 시 전달.
Realtime bbox/오차/각도는 설정 상한5~10Hz와 최신 값 우선 정책을 사용한다.
현재 broker는 같은 property의 Changed를 합치지만 전환 이벤트와 명시적 rate limit은
구분돼 있지 않다. 이 작업도 Milestone5다.

## 10. Manual/Auto Arbitration

### 현재 구현

수동 명령/중앙/Stop → 추적 OFF와 예약 취소. 다시 켜야 자동 추적한다.
세대가 다른 AI 결과는 최종 제어 경계에서 버린다.
연속 속도는 요청 timeout0.1~5초(기본1초) 후 계획을 중단한다.

### 새 목표 — Milestone8에서 변경

```text
trackingRequested=true
 → AUTO
 → 수동 이동: MANUAL, 자동 출력 일시 차단, 새 세대
 → Stop/속도 lease 만료: 계획 중단, 수동 유예
 → 유예 종료: 요청이 여전히 ON이고 정상 상태이면 AUTO
tracking_set(false): 요청 취소, 자동 복귀 없음
```

추적 요청과 현재 출력 허용을 구분한다. 수동 동안에도 추적 요청에 따른 캡처 유지가
필요하다. LOCAL_CENTER는 중앙 우선이며 복귀 정책을 VMS/사용자 계약에 명시한다.
출력 fault/종료에서는 자동 복귀하지 않는다. 이전 AI 결과는 복귀 후에도 재사용하지 않는다.

## 11. Thread Model

| 실행 주체 | 책임 |
| --- | --- |
| Publisher GLib main | bus 오류/종료, pipeline 수명 |
| GStreamer 내부 worker | 캡처/인코딩/queue 분기 |
| Publisher std::thread | 최신 샘플·DNN·추적 계산·IPC |
| OpenCV 내부 연산 | 설정된 CPU thread 수, 현재 기본2 |
| Servo main select loop | 키/TCP/20ms 계획 갱신 |
| pigpio alert callback | GPIO17 debounce/중앙 입력, 동일 제어 mutex |
| pigpio 내부 | PWM/timing 관리 |
| ONVIF poll loop | HTTP/Discovery/feed/비동기 PullPoint |
| capture lease 프로세스 | 추적 수요 조회/압축 RTSP packet 소비 |

새 기능마다 thread를 추가하지 않는다. 상태의 실행 소유자와 callback 수명을 문서화한다.
Pi 내부 IPC는 유지하며 자기 자신에게 ONVIF 자동 추적 명령을 보내지 않는다.

## 12. Runtime State

현재 MotionPlan은 Idle/Target/Velocity enum이고 그 외 상태에는 bool이 남아 있다.
아래는 목표 상태이며 구현 완료 상태로 간주하지 않는다.

| 상태 | 값/의미 |
| --- | --- |
| CameraState | STOPPED/STARTING/RUNNING/ERROR: 실제 캡처 수명 |
| StreamState | STOPPED/STREAMING/ERROR: 발행 상태 |
| TrackingState | DISABLED/IDLE/TRACKING/LOST: 추적/대상 상태 |
| TargetState | NO_TARGET/ACQUIRED/TRACKING/LOST: 선택 대상 상태 |
| PtzMode | AUTO/MANUAL, 필요하면 LOCAL_CENTER를 입력 출처로 구분 |

Publisher 연결은 물리 CSI 연결 판정이 아니다. 장치·영상·추적·출력 오류를 각각 표현한다.
FrameId는 분석 샘플 순번, PTS는 pipeline running time, 캡처 UTC는 PTS 나이 기반 추정이다.
metadata를 실제 영상과 맞추는 시계 대응은 별도 검증한다.

## 13. Error Handling

- 출력 범위 제한, NaN/무한대 거절, 입력 크기/필드/정수 overflow 검사.
- 연속 이동 timeout과 명시적 Stop. 종료에서는 추적/계획 취소와 PWM/GPIO 정리.
- GStreamer ERROR/EOS 종료와 on-demand 재시작. AI 예외는 로그에 남으며 현재 재시도 정책 보강 필요.
- 느린 관측자는 GPIO loop를 막지 않게 연결을 닫고 최신 snapshot으로 복구.
- 인증 파일/비밀번호는 Git 및 로그에 넣지 않는다.
- 프레임 단위 무한 로그를 피하고 상태 전환/오류/주기 요약을 기록한다.
- 새 조사/해결은 root troubleshotting.md에 남긴다.

## 14. Test Plan / Milestone 판정

소스 구현, 모의 시험, Pi 실제 시험, PC VMS 시험을 따로 기록한다.

| 단계 | 현재 증거 | 완료에 필요한 증거 |
| --- | --- | --- |
| 1 PTZ Hardware | 키/중앙/clamp/timeout/GPIO mock | **5분 실물 반복, 버튼, Ctrl+C, 충돌 없음·방향 관찰** |
| 2 Camera+Detection | 모델 점검/예비 실제 탐지 | 현재 카메라 사람 bbox/confidence, 단일 캡처 확인 |
| 3 Auto Tracking | 계산/오래된 결과/세대 모의 시험 | 상태/LOST, 실제 좌우·상하·dead zone |
| 4 RTSP | Pi 로컬 실제 한 프레임 디코딩 | PC ffprobe/ffplay/libavformat, 안정성/지연 |
| 5 Metadata/Control | ONVIF Events 구독/metadata v2 | 별도 추적 스위치, 상태 이벤트/rate limit, VMS 왕복 |
| 6 ONVIF Device/Media | 구현/인증 조회 | 실제 VMS 검색/URI 획득 |
| 7 ONVIF PTZ | SOAP/RPC/상태/무변위 시험 | VMS 명령→실물 방향/Stop |
| 8 Arbitration | 수동 우선·옛 세대 폐기 | 수동/Stop/timeout 후 AUTO 복귀 계약·실물 확인 |

초기 계획의 가장 앞선 미완료 단계는 Milestone1의 실물 시험이었다. 이후 사용자 지시로
모든 실물/장시간 검증을 최종 사용자 작업으로 미루고 Milestone1~8의 소프트웨어를 구현했다.
마지막 지시로 추가 모의 시험도 중단했다. 실제 기구 충돌/방향을 PASS로 쓰지 않는다.
30분 통합 시험은 각 흐름의 실물 연결 뒤 수행한다. CPU/RSS/온도/frame gap/metadata
갱신/명령/오류를 함께 남기며 RSS의 순간 변화만으로 memory leak을 단정하지 않는다.

### 각 단계 보고 항목

수정/추가 파일, 클래스 역할, data/thread flow, 빌드/실행/검증 명령, 실제 결과와 한계.
현재 단계 상세 기록은 [EDGE_CAMERA_SOFTWARE_REPORT](EDGE_CAMERA_SOFTWARE_REPORT.md)에 작성한다.
후속 기능은 필요성을 먼저 설명하고 한 단계씩 추가한다. 이전 목표는
[PROJECT_ARCHITECTURE](PROJECT_ARCHITECTURE.md), 이전 구현 증거는
[EDGE_CAMERA_IMPLEMENTATION](EDGE_CAMERA_IMPLEMENTATION.md)를 참고한다.
