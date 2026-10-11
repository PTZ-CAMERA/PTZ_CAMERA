# 처리 구조와 자원 사용량 측정

[문서 안내](../../docs/README.md) · [전체 구조](../../docs/SYSTEM_ARCHITECTURE.md) · [Pi 현재 구조](../../docs/EDGE_CAMERA_ARCHITECTURE.md)

향후 작업: [CPU 코어 배치별 실제 스트리밍 비교 계획](cpu_affinity_plan.md).

목적은 Raspberry Pi 4의 **영상 송출·탐지 분리, 순차 추론, 이벤트 루프, 추론 스레드 수**를
동일 조건의 측정값으로 설명하는 것이다. 단순히 스레드를 많이 쓰는 것을 성능 향상으로
간주하지 않는다. 도구는 C++17과 Bash로 작성했다.

## 파일과 역할

| 위치 | 역할 |
| --- | --- |
| `common/performance/performance_log.h` | 선택적으로 활성화되는 CSV 이벤트 기록, monotonic 시간, PID/TID, 부모 이벤트 ID |
| `tools/performance/pipeline_benchmark.cpp` | 합성 영상·실제 MobileNet-SSD·실제 x264 인코더로 동기/비동기 경로 비교 |
| `tools/performance/resource_sampler.cpp` | `/proc` 기반 프로세스/스레드 CPU, RSS, 스레드 수, 문맥 전환과 Pi 온도·클럭 수집 |
| `tools/performance/summarize.cpp` | 준비 구간 제외, 평균·p95·최대, 이벤트율, 스레드별 CPU를 Markdown으로 집계 |
| `tools/performance/run_suite.sh` | 조건별 실행, 원본 CSV·환경·모델 해시·결과 저장 |

## 1. 재현 가능한 소프트웨어 비교 시험

```sh
sudo apt install cmake g++ pkg-config libopencv-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly

# 모델은 기존 카메라 설치 위치를 사용한다.
# 없으면 카메라 README의 fetch_model.sh/설치 절차로 먼저 준비한다.
./tools/performance/run_suite.sh 180 3 5
```

인자는 **조건별 실행 시간(초), 반복 횟수, 준비 구간(초)** 순서다. 위 설정은
6조건 × 180초 × 3회로 약 54분과 빌드·초기화 시간이 필요하다.
빠른 정상 동작 확인은 `./tools/performance/run_suite.sh 20 1 5`로 실행한다.

원본은 `tools/performance/results/<실행시각>-<PID>/`에 저장하고 Git에서 제외한다.
각 조건의 `summary.md`는 준비 구간을 제외한 결과다. 채택할 측정값과 조건만
`docs/performance/`에 남긴다. 출력 디렉토리는 실행 전에 생성해야 한다.

### 공통 조건

- Raspberry Pi 4, 같은 OS·GStreamer·OpenCV·모델 파일 사용.
- 입력: `videotestsrc pattern=ball`, BGR 1280×720, 실시간 30fps.
- 추론 입력: BGR 640×360에서 모델의 300×300 전처리를 수행한다.
- 인코더: x264 ultrafast/zerolatency, 2500kbps, B 프레임 0, key interval 30.
- 출력: H.264 parser 이후 `fakesink`. 네트워크 수신자와 Qt를 포함하지 않는다.
- 같은 합성 패턴을 쓰므로 객체 탐지 정확도를 평가하지 않는다.
- 실제 카메라와 GPIO를 열지 않는다. 서보 이동 명령을 보내지 않는다.
- 불필요한 빌드·다른 부하 작업을 병행하지 않는다. 실행 중인 다른 서비스 상태도 기록한다.
- 기록 활성화 상태로 모든 조건을 비교한다. 로그 비용은 측정값에 포함된다.
- 반복별 순서를 반대로 진행해 온도/순서 편향을 줄이지만 완전히 제거하지는 않는다.
- 장시간 비교에는 냉각 방식, 주변 온도, 전원, Qt 시청 여부를 추가 기록한다.

### 비교 조건

| 이름 | 처리 경로 | 추론 스레드 설정 | 대기 정책 | 확인할 질문 |
| --- | --- | ---: | --- | --- |
| off | 영상 송출만 | 해당 없음 | 해당 없음 | 탐지를 추가하기 전 비용은? |
| async1 | 독립 탐지 스레드 | 1 | 최신 프레임 | 병렬 연산이 없을 때 비용은? |
| async2 | 독립 탐지 스레드 | 2 | 최신 프레임 | 현재 운영 선택의 전체 비용은? |
| async3 | 독립 탐지 스레드 | 3 | 최신 프레임 | 스레드 증가가 송출과 결과 나이를 개선하는가? |
| sync2 | 공유 영상 경로에서 순차 추론 | 2 | 매 세 번째 입력 프레임에서 기다림 | 느린 연산이 공유 경로를 막으면 어떻게 되는가? |
| fifo2 | 독립 탐지 스레드 | 2 | queue/appsink 각각 최대 10개, 오래된 프레임부터 처리 | 유한 버퍼라도 결과가 오래될 수 있는가? |

`sync2`는 tee 앞의 pad probe에서 추론을 완료한 다음 영상 전달을 계속한다.
메인 스레드에서 함수를 호출하는 것만으로 동기 비교 조건을 만들지는 않는다.
공유 경로의 지연 때문에 입력 자체가 느려지면 추론 빈도도 낮아질 수 있다.
따라서 sync/async의 연산 횟수가 동일하다고 해석하지 않는다.

`async`는 tee 뒤의 별도 GStreamer queue와 `std::thread`에서 appsink 프레임을
처리한다. 추론 한 번은 동기 함수이며, 같은 모델로 여러 프레임을 동시에 추론하지 않는다.
`latest`는 queue/appsink 각각 최대 1개를 대기시키고 오래된 대기 프레임을 버린다.
실행 중인 추론을 취소하지 않으며 전체 파이프라인의 프레임 수가 1개라는 뜻도 아니다.
`fifo`는 탐지가 따라잡지 못하면 대기 나이가 증가하고 버퍼가 차면 상류에 대기가 전파된다.
시험 도구는 600ms 초과 결과를 `stale_result`로 표시하며 이동 명령은 보내지 않는다.

OpenCV `setNumThreads(1/2/3)`는 연산 라이브러리에 요청하는 병렬화 설정이다.
프로세스 전체 스레드 수를 1/2/3으로 제한하는 설정이 아니다. GStreamer와 인코더의
내부 스레드가 별도로 존재하며 실제 스레드 수는 `/proc`에서 확인한다.

## 2. 운영 프로그램의 이벤트 로그

기본 실행은 성능 CSV를 쓰지 않는다. `PTZ_PERF_LOG`를 설정하면 활성화한다.
공통 헤더를 추가한 새 바이너리로 실행해야 한다. 기존 설치 바이너리는 빌드만으로
교체되지 않는다. 이 작업에서는 운영 서비스 설치·재시작을 수행하지 않았다.

```sh
cmake -S pi/camera_stream -B pi/camera_stream/build
cmake --build pi/camera_stream/build -j2
cmake -S pi/servo_test -B pi/servo_test/build
cmake --build pi/servo_test/build -j2
mkdir -p tools/performance/results/manual
```

실행할 프로세스에 다음 환경변수를 전달한다.

```text
PTZ_PERF_LOG=/절대경로/events-%p.csv
PTZ_DNN_THREADS=2
```

`%p`는 해당 PID로 치환한다. 프로세스마다 다른 파일을 사용해야 한다.
같은 파일을 두 프로세스가 열면 데이터가 덮어써진다. 파일은 실행 때 새로 작성한다.
`PTZ_DNN_THREADS`는 카메라 송출기에 적용하며 허용 값은 1, 2, 3이다. 기본은 2이다.

systemd에서는 해당 서비스의 drop-in에 `[Service]`와 `Environment=`를 설정한다.
카메라는 MediaMTX 서비스가 실행한 송출기가 환경을 상속한다. 서보 서비스에는
별도 환경을 설정한다. 설치와 재시작은 카메라 시청/모터 배선 상태를 확인한 뒤 수행한다.
서보 서비스 재시작은 시작 시 두 축에 중앙 복귀 명령을 출력한다.

설정 예시(파일 경로와 서비스명은 각각 맞춘다):

```ini
[Service]
Environment="PTZ_PERF_LOG=/var/log/ptz-performance/events-%p.csv"
Environment="PTZ_DNN_THREADS=2"
```

`sudo mkdir -p /var/log/ptz-performance`로 쓰기 가능한 디렉토리를 먼저 만들고,
`sudo systemctl edit ptz-camera.service` 또는 `ptz-servo.service`로 drop-in을 추가한다.
새 바이너리 설치 후 서비스 재시작 시 반영된다. 로그를 끄려면 추가한 Environment
줄만 삭제하고 재시작한다. 다른 설정까지 지우는 `systemctl revert`는 사용하지 않는다.

### 이벤트 해석

| 이벤트 | duration/value 의미 |
| --- | --- |
| inference | duration: 모델 전처리·추론·후처리 시간, value1: 입력 나이(ms), value2: 결과 나이(ms) |
| detection_result | value1: 남은 사람 박스 수, value2: 결과 나이(ms) |
| stale_result | value1: 600ms를 초과한 결과의 나이(ms) |
| stream_input / encoder_input / stream_encoded | 해당 지점을 통과한 버퍼 시점·수. 운영 로그 value1은 PTS(ns) |
| queue_state | value1: queue에 대기 중인 버퍼 수, value2: 대기 중 버퍼의 미디어 시간 합(ms), detail: 큐 이름 |
| tracking_status_request | 상태 요청·응답을 기다린 동기 호출 시간 |
| tcp_command | 개행 분리·CR 제거 이후 명령 처리 시작부터 완료까지 |
| gpio_write | gpioServo() 호출 시간, value1: BCM GPIO, value2: 펄스폭(us), detail: 반환 코드 |
| button_callback | active-low 콜백 처리 시간. 중복 눌림은 별도 이벤트로 표시 |
| button_debounce_ignored | debounce로 제외한 눌림 |
| keyboard_command | 직접 GPIO 제어 모드의 키 처리 시간 |

`start_ns/end_ns`는 steady_clock 기반 monotonic 시간이고 `duration_us`는 마이크로초다.
PID/TID로 실행 주체를 구분한다. `event_id/parent_id`로 명령/버튼 콜백과 GPIO 호출을
연결한다. 현재 명령 ID는 Pi 내부 로그용으로 Qt 프로토콜을 변경하지 않는다.

**command start → GPIO return**은 명령 해석 시작 이후의 시간이다. 네트워크 송신,
수신 대기, 완성되지 않은 줄의 대기, 실제 PWM 첫 출력, 실제 모터 도착 시간은 포함하지 않는다.
버튼 측정도 콜백 진입 이후이며 물리 접점 발생 시점과 pigpio의 필터 지연은 포함하지 않는다.
프레임 나이는 GStreamer PTS/running time 기준이며 센서 노출 시각을 별도 검증한 값은 아니다.
queue_state는 약 1초 간격의 관측이며 appsink 내부 대기량과 실행 중인 추론 프레임은 포함하지 않는다.
stream_encoded는 parser 출력 버퍼율이며 Qt FPS나 실제 네트워크 전달 성공률이 아니다.
인코더 입력/출력 이벤트가 있으나 인코딩 한 프레임의 지연을 자동 매칭해서 계산하지는 않는다.

### 운영 프로세스 자원과 요약

```sh
# PID는 실제 카메라 송출기 또는 서보 서비스 PID로 바꾼다.
./tools/performance/build/resource_sampler --pid 12345 --seconds 180 \
  --output tools/performance/results/manual/resources.csv

./tools/performance/build/summarize /절대경로/events-12345.csv \
  tools/performance/results/manual/resources.csv 5 \
  > tools/performance/results/manual/summary.md
```

같은 PID의 이벤트/자원 CSV를 짝지어 사용한다. 프로세스 전체 CPU는 코어 하나를
100%로 표현하므로 Pi 4에서 100% 초과가 정상이다. `tid=0`은 프로세스 집계 행이다.
첫 CPU 샘플과 읽을 수 없는 수치는 -1이다. RSS는 프로세스에만 기록하며 스레드별로
합산하지 않는다. 문맥 전환은 스레드별 관측 구간의 증가량이며 전체 실행 누적값이 아니다.
자원 수집 간격은 약 1초로, 구간 내 짧은 피크를 정확히 포착하지 못할 수 있다.
온도는 thermal_zone0, 클럭은 cpu0 기준이다. 다른 보드에서 파일이 없으면 -1이다.
`vcgencmd get_throttled`의 시험 전·후 값도 별도 저장한다. 현재 상태 비트와 부팅 이후
과거 발생 비트를 구별해서 해석한다.

## 3. 판정 방법과 추가 현장 시험

- 송출 버퍼율 유지와 탐지 결과 나이의 p95를 함께 비교한다.
- 탐지율만 높은 조건보다, 송출·명령 응답을 유지하면서 결과가 신선한 조건을 선택한다.
- 평균·p95·최대와 샘플 수를 모두 표시한다. 짧은 1회 시험은 성능 결론이 아니라 예비 측정이다.
- 고정 FPS 공급을 넘는 이벤트율이 나오면 준비/종료 구간과 버퍼 단위를 먼저 확인한다.
- 로그 없는 동일 조건도 별도로 실행해 기록 비용을 평가한다. 현재 자동 시험은 로그 ON이다.
- 실제 카메라에서 TRACK OFF/ON을 비교하고, 운영 코드의 상태 조회 지연을 확인한다.
- 실제 서보 명령 시험은 목표 각도·반복 횟수를 미리 정하고 측정한다. 제어 응답 로그만으로
  기계적 회전 정확도나 안전 범위를 입증하지 않는다.
- Qt 표시 지연은 별도 영상 시간 표시/고속 촬영 등으로 측정한다. Pi 추론 시간과 더하거나
  같은 값으로 간주하지 않는다. TCP 전체 왕복 측정도 현재 정상 PTZ 무응답 프로토콜과 구별한다.

결과 문서에는 모델·소스 버전, 실행 명령, 환경, 반복 횟수, 준비 구간, 다른 서비스,
온도/스로틀링, 원본 위치, 미검증 범위를 남긴다. 값이 기대와 다르면 버퍼·CPU·시각 기준을
조사한 뒤 `troubleshotting.md`에도 기록한다.

## 4. GPIO를 쓰지 않는 회귀 검증

```sh
cmake -S tools/performance -B tools/performance/build -DPTZ_BUILD_GPIO_MOCK_TEST=ON
cmake --build tools/performance/build -j2
ctest --test-dir tools/performance/build --output-on-failure
```

`servo_logging_test`는 가짜 pigpio 함수로 각도 제한, 자동/수동 제어, 버튼 debounce,
키 입력, 종료 시 펄스 중지와 로그의 부모 이벤트 연결을 검증한다. 실물 PWM 파형이나
기계적 회전의 정확성을 검증하는 시험은 아니다.
