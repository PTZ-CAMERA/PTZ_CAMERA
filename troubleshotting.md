# Troubleshooting log

Record each new problem investigated in this project here: symptom, cause,
checks, change, and verification. Add a new dated entry for each incident.

## 2026-10-03 — 사람 탐지의 입력 지연과 스레드 경쟁

- **증상:** 5fps 입력·DNN 2스레드에서 추론 뒤 프레임 나이가 약 529~674ms였고, 600ms 제한으로 버려지는 결과가 있었다.
- **측정:** 입력 시점의 프레임 나이는 약 210~378ms, 추론은 약 284~319ms였다. 입력을 10fps로 높이고 2스레드를 유지한 짧은 시험에서는 입력 나이 약 151~253ms, 추론 약 289~311ms, 결과 나이 약 440~564ms가 기록됐다. 10fps·3스레드는 추론 약 292~368ms, 결과 나이 약 541~607ms로 개선되지 않았다.
- **원인 판단:** 소프트웨어 H.264 송출과 DNN이 CPU를 공유한다. 모델 단독 시험에서 스레드 3개가 빨랐던 결과를 송출 중 전체 지연에 그대로 적용할 수 없었다. 입력 대기와 추론이 모두 결과 나이에 기여한다.
- **조치:** 탐지 입력을 최대 10fps로 늘리고 DNN은 2스레드로 유지했다. 버려지는 결과가 있는 상황에서도 오래된 프레임을 모두 처리하지 않도록 1개 대기 버퍼를 유지한다.
- **검증 한계:** 짧은 시험이며 실제 사람이 움직이는 장면의 서보 반응·장시간 열 상태는 아직 확인하지 않았다. 3스레드 시험 중 송출기 CPU는 약 250%, 온도는 70.1°C였고 `get_throttled=0x0`이었다.

## 2026-10-03 — 상태 조회가 잘못된 카메라 연결 알림을 보냄

- **증상:** Pi 내부에서 `TRACK:STATUS`만 조회해도 Qt에 `CAMERA:CONNECTED`가 전송됐고 조회 연결 종료 시 `CAMERA:DISCONNECTED`가 전송됐다. 실제 송출기 연결과 무관한 알림이었다.
- **원인:** 서보 서버가 모든 로컬 상태 조회 연결을 카메라 송출기로 간주했다.
- **조치:** 송출기가 별도 `CAMERA:HELLO`로 등록하도록 바꿨다. `TRACK:STATUS`는 상태 응답만 보낸다.
- **검증:** 단순 상태 조회에서 Qt 시험 클라이언트는 카메라 알림을 받지 않았다. 실제 송출기 연결·해제에는 `CAMERA:CONNECTED/DISCONNECTED`가 전달됐고 내부 시험의 8개 필드 `DETECT`도 전달됐다.

## 2026-10-02 — 사람 자동 추적에서 탐지 결과가 늦게 도착함

- **증상:** 실제 RTSP 송출과 OpenCV MobileNet-SSD 탐지를 함께 켰을 때 로그의 추론 시간이 약 300~320ms/프레임, 추론 뒤 프레임 나이가 약 600~700ms였다. 현재 제어 코드는 600ms가 지난 결과를 버리므로 자동 추적 반응이 부족할 수 있다.
- **확인:** `ptz_camera_publisher --benchmark-model`을 송출 없이 실행하면 1스레드 약 412ms, 2스레드 약 229ms, 3스레드 약 192ms/프레임이었다. 실시간 시험에서 사용한 설정은 2스레드다. `vcgencmd get_throttled`는 `0x0`이었고 시험 뒤 온도는 58.9°C였다.
- **원인 상태:** 추론 자체의 비용은 확인됐다. 카메라 입력, GStreamer 큐, 소프트웨어 H.264 인코딩이 프레임 나이에 각각 얼마나 기여하는지는 아직 분리 측정하지 않았다. 따라서 병목을 하나로 단정할 수 없다.
- **현재 조치:** 입력 시점의 프레임 나이와 추론 뒤 나이를 함께 기록하는 로그를 로컬 코드에 추가하고 빌드했다. 사용자의 작업 중지 요청에 따라 시스템 설치본에는 이 마지막 변경을 반영하지 않았다. 자동 추적은 `TRACK:OFF`로 두었다.
- **다음 검증:** 다음 세션에 설치본에 로그를 반영해 입력 나이를 측정한다. 송출과 동시에 2개/3개 스레드 설정을 비교한 뒤 큐와 인코더 설정을 조정한다. 실제 사람이 있는 장면에서 서보 반응과 탐지 품질도 확인한다.

## 2026-10-02 — RTSP client disconnected while enabling UDP

- **Symptom:** After changing `rtspTransports` from `[tcp]` to `[udp, tcp]`, the active PC RTSP session and the local C++ publisher were disconnected.
- **Cause:** MediaMTX hot-reloaded the file but had to close and recreate its RTSP listener to enable UDP ports. The publisher saw an RTSP end-of-file error and exited. The already connected Qt RTSP player must reconnect.
- **Change:** Kept the C++ publisher configured for RTSP/TCP and enabled RTSP/UDP alongside TCP in MediaMTX. No separate video encode path was added.
- **Verification:** MediaMTX reopened TCP 8554 and UDP 8000 (RTP) / 8001 (RTCP). FFmpeg decoded a frame through RTSP/UDP and then through RTSP/TCP; MediaMTX logged `with UDP` and `with TCP` respectively. Remote Qt UDP playback is not yet verified.

## 2026-10-02 — RTSP playback latency in the Qt UI

- **Symptom:** The RTSP view in Qt feels delayed compared with the desired PTZ response.
- **Current path:** The Pi publishes 1280×720/30fps H.264 to MediaMTX. The encoder uses zero-latency tuning, no B frames, and a 30-frame key interval. At the initial inspection, MediaMTX permitted only RTSP/TCP and the PC had an established TCP session; UDP was enabled later.
- **Checks:** The Pi reported `throttled=0x0` and 43.3°C, so the snapshot showed no thermal or undervoltage throttling. End-to-end camera-to-Qt latency and Qt player buffering have not yet been measured.
- **Buffer audit:** Pi code does not set a playback/jitter buffer. MediaMTX's unspecified `writeQueueSize` therefore uses its v1.21.1 default of 512 outgoing packets; `udpReadBufferSize: 0` uses the OS default socket receive buffer (212,992 bytes on this Pi). Neither value specifies a fixed video delay. Qt's player buffer is outside this repository and remains unknown.
- **Next check:** Identify the Qt RTSP playback backend and lower its receive/jitter buffer while dropping late frames; compare steady-state delay with first-frame startup delay. If LAN conditions allow, test UDP RTSP separately and compare loss/stability before changing the production transport.

## 2026-10-02 — Manual servo test could not lock pigpio

- **Symptom:** Running `./build/servo_test` printed `Can't lock /var/run/pigpio.pid` and an unhelpful `try sudo` message.
- **Cause:** `ptz-servo.service` was already running and owned pigpio. The failure was caused by a second GPIO process, not missing root permission.
- **Change:** The default terminal mode now connects as a TCP client to the running local servo service; `--client` selects that mode explicitly and `--direct` selects direct GPIO control after stopping the service. The pigpio error message now mentions an existing process. Added TCP connection and command logs to diagnose Qt input.
- **Verification:** Rebuilt and reinstalled. Running `./build/servo_test` while the service stayed active connected to localhost and sent LEFT, RIGHT, CENTER; the service logged each command and angle change. No second pigpio lock attempt occurred.

## 2026-10-02 — Qt PTZ buttons appear to send no command

- **Symptom:** The Qt UI showed button activity but the Pi servos did not visibly respond.
- **Evidence:** `ptz-servo.service` was active and listening on `0.0.0.0:5000`. At inspection, there was no established TCP 5000 connection. The recent service journal contained no command from the PC; it showed only the local terminal client test from `127.0.0.1`. The provided Qt `NetworkClient::sendLine()` writes only when `isConnected()` is true.
- **Change:** Added Pi journal entries for TCP connect/disconnect and each received command with peer address. The default terminal mode now connects to the running service, avoiding the pigpio lock conflict.
- **Verification:** Local TCP keyboard commands reached the service and changed the logged PAN angle. A connection and command from the PC have not yet been observed; check the Qt `connected`/`connectionError` signals and ensure `connectToServer("192.168.0.92", 5000)` is called before sending commands.

## 2026-10-02 — Servo service exited abnormally on systemd stop

- **Symptom:** Stopping `ptz-servo.service` logged pigpio `sigHandler: Unhandled signal 18, terminating`, and systemd reported exit status 255.
- **Cause:** systemd sends SIGCONT during service shutdown; pigpio's installed signal handler treated it as an unexpected signal before normal GPIO cleanup finished. The install script also started and immediately restarted the service.
- **Change:** The C++ process now ignores SIGCONT after pigpio initialization, while handling SIGTERM in its normal shutdown loop. The installer starts or restarts the service once.
- **Verification:** Rebuilt and reinstalled the C++ program. `systemctl stop ptz-servo.service` then logged `Servo pulses stopped.` and systemd reported `Succeeded`; the service started again normally.

## 2026-10-02 — `jq` package download returned 404 during on-demand setup

- **Symptom:** `apt-get install jq` failed because the Bullseye security repository returned 404 for its indexed `jq` and `libjq1` package versions. `apt-get update` did not resolve it.
- **Change:** Avoided the extra package. The on-demand watchdog reads MediaMTX's localhost-only Prometheus metrics with the already installed `curl` and `awk` tools.
- **Verification:** MediaMTX exposed `paths_readers{name="cam",readerType="",state="notReady"} 0` while idle and a reader count of 1 during an RTSP or WebRTC viewing session.

## 2026-10-02 — GStreamer hardware H.264 encoder failed during C++ migration

- **Symptom:** `libcamerasrc → videoconvert → v4l2h264enc` started but then reported `Failed to process frame` from `GstV4l2VideoEnc` on this Pi 4. A direct NV12 path failed format negotiation.
- **Check:** The same 1280×720, 30fps camera stream worked with `videoconvert → x264enc`; the `rtspclientsink` package was missing and was installed as `gstreamer1.0-rtsp`.
- **Cause:** The V4L2 encoder path could not process these GStreamer camera buffers in the tested configuration. The exact driver/buffer incompatibility is not yet established.
- **Change:** Added a C++17 GStreamer publisher using `libcamerasrc`, `videoconvert`, `x264enc`, `h264parse`, and `rtspclientsink`; MediaMTX now starts this executable. The old `libcamera-vid | ffmpeg` publishing script was removed.
- **Verification:** CMake build passed. MediaMTX accepted the local RTSP publisher; FFmpeg decoded 151 frames in a 5-second stream sample. RTSP reported H.264 Constrained Baseline, 1280×720, 30fps, and the WebRTC page returned HTTP 200. The publisher used approximately 115% CPU during this check; remote browser playback remains to be checked.

## 2026-10-01 — Intermittent magenta/pink video frames

- **Symptom:** The PC stream occasionally shows a pink image with horizontal corruption.
- **Isolation:** Stopped `ptz-camera.service` and recorded 20 seconds directly from `libcamera-vid` to `/tmp/ptz_camera_20s.h264` (593 frames). Frames 295 (9.84 s) and 530 (17.67 s) visibly contain magenta corruption. This occurred before MediaMTX, WebRTC, the LAN, or Qt could affect the image.
- **Other checks:** A separate 12-second MJPEG capture (353 frames) showed no obvious color outliers, but this short sample cannot rule out an intermittent camera-side issue. `vcgencmd get_throttled` was `0x0`, the Pi Ethernet interface showed no RX/TX errors, and no servo process was running during inspection.
- **Conclusion:** The fault is on the Pi capture/encode side, not solely in network delivery or PC playback. The exact point (CSI cable/connector, camera, ISP, or H.264 encoding) is not yet established.
- **Change/verification:** No capture settings changed. The camera service was restarted after the direct tests and RTSP again reported H.264 1280×720. Inspect the ribbon/connector with power removed, then repeat a direct capture after reseating; if the artifact remains, compare longer MJPEG and H.264 captures to separate camera input from encoder behavior.

## 2026-10-01 — Camera page loads but live video stops

- **Symptom:** The PC WebRTC player lost video. The `ptz-camera.service` process was still active, but RTSP `/cam` returned 404 because no stream was available.
- **Evidence:** The IMX219 remained visible in `libcamera-vid --list-cameras`. MediaMTX logged a local RTSP publisher `i/o timeout` at 02:47:04, followed by `no stream is available on path 'cam'` for PC WebRTC requests. Pi power reported `throttled=0x0`; no camera or voltage errors appeared in recent kernel logs. A standalone 5-second camera capture produced a 5.9 MB H.264 file.
- **Cause:** The camera-to-MediaMTX publishing pipeline stopped delivering video frames while its parent service stayed active. The underlying reason for the stall is not yet confirmed; the available checks do not establish a cable or power fault.
- **Change:** Restarted `ptz-camera.service`. Added `pi/camera_stream/watchdog.sh` and a systemd timer that checks for a decodable RTSP frame every 30 seconds, retries once, then restarts the camera service if both checks fail. The timer skips an intentionally stopped camera service.
- **Verification:** After restart, RTSP decoded a frame and reported H.264 1280×720; the WebRTC page returned HTTP 200. The timer ran successfully on a healthy stream. PC playback after the change still needs confirmation.

## 2026-10-01 — Servo movement stops before 0°/180°

- **Symptom:** The servos appear to stop around the middle portion of their travel.
- **Cause:** The current application deliberately clamps PAN to 30–150° and TILT to 40–140° in `pi/servo_test/src/main.cpp`. These values came from the original control requirements; they are software limits, not a measured limit of the installed servos.
- **Check:** `angleToPulseWidth()` maps 0–180° to 1000–2000 µs, but `setPanAngle()` and `setTiltAngle()` clamp their inputs before calling it.
- **Initial finding:** At the time of inspection, PAN was 30–150° and TILT was 40–140°.
- **Follow-up change:** At the user's request to extend PAN slightly, PAN was changed to 25–155°; TILT remains 40–140°.
- **Verification:** The CMake build passed. Physical clearance and maximum travel remain unverified. A `servo_test` process was already running, so that process continues using the previous limits until restarted.

## 2026-10-01 — `servo_test` failed to initialize pigpio

- **Symptom:** `sudo ./build/servo_test` printed `initInitialise: bind to port 8888 failed (Address already in use)` and `pigpio initialization failed`.
- **Cause:** The MediaMTX camera service's HLS listener and pigpio both used TCP port 8888. `ss -lntp` showed `mediamtx` listening on 8888.
- **Change:** Set `hlsAddress: :8891` in `pi/camera_stream/mediamtx.yml`, updated the HLS URL in `pi/camera_stream/README.md`, and restarted `ptz-camera.service`.
- **Verification:** Port 8888 was free; a small C program successfully called `gpioInitialise()` and `gpioTerminate()`. The camera RTSP stream still reported H.264 at 1280×720, and the WebRTC and HLS pages both returned HTTP 200. The user still needs to retry the full servo program.

## 2026-10-06 — 성능 시험 종료 단계의 CSV 실행 오류

- 증상: 6조건의 실행과 CSV 생성 후 마지막 요약 단계에서 `events.csv: Permission denied`가 발생했다.
- 확인: FIFO 조건의 runtime/events/resources 파일과 종료 전후 스로틀링 파일이 존재했고, 동일 CSV를 `summarize`에 직접 전달하면 정상 요약됐다. CSV 권한을 실행 가능하게 바꿀 문제는 아니다.
- 원인: 시험 셸 스크립트가 실행 중인 동안 같은 파일에 소스 해시 기록 줄을 추가했다. Bash가 남은 명령을 읽을 파일 위치가 변경되어 CSV 경로를 명령으로 해석했다.
- 조치: 실행이 종료된 뒤 저장된 6조건의 CSV를 다시 요약했다. 시험 중 실행 스크립트를 수정하지 않고, 수정된 스크립트 전체를 별도 짧은 회귀 시험으로 다시 실행한다.
- 범위: GStreamer/OpenCV 시험 프로세스의 실행 실패가 아니며 실제 카메라·모터에는 접근하지 않았다. 회귀 시험 결과는 아래 후속 기록에 남긴다.
- 후속 검증: 수정된 전체 스크립트를 `run_suite.sh 6 1 1`로 실행해 6조건 모두 실행·요약·정상 종료를 확인했다. 결과 위치는 `tools/performance/results/20261006-155545-175531/`이다.

## 2026-10-07 — ONVIF 서비스 계정 검증 경고

- 확인: `systemd-analyze verify`에서 새 유닛의 `User=nobody`가 특수 공용 계정이라고 경고했다. ONVIF 응답 실패는 아니었다.
- 조치: `User=ptz-onvif`, `DynamicUser=true`로 바꿔 서비스 전용 임시 계정을 사용했다. systemd 247과 machine-id 읽기 권한을 확인했다.
- 검증: 유닛 반영·ONVIF 서비스 재시작 후 active 상태, eth0 ProbeMatches, GetServices/GetProfiles/GetStreamUri 조회 성공. 기존 영상/서보 서비스는 재시작하지 않았다.
- 범위: 유닛 검사에서 함께 출력된 기존 OS의 plymouth/myservice 경고는 이번 프로젝트 변경 대상이 아니므로 설정을 수정하지 않았다.

## 2026-10-07 — PAN 좌우 방향 반전 여부 확인

- 사용자 보고: 장착된 카메라의 좌우 이동 방향이 명령과 반대였다.
- 코드 확인: 수동 LEFT/a는 PAN 각도를 5도 감소시키고 RIGHT/d는 5도 증가시킨다. 자동 추적은 화면 오른쪽 오차에 양의 PAN 보정량을 사용한다.
- 현재 상태: 소스에는 PAN 방향 반전 설정이나 출력 반전 로직이 없다. 좌우 반전 수정은 아직 반영되지 않았다.
- 이번 확인: GPIO 이동 명령, 코드 변경, 서비스 재시작은 수행하지 않았다. 실행 중인 설치 바이너리의 물리적 움직임을 새로 검증한 것은 아니다.
- 후속 수정: 수동/자동 제어 모두에 일관되게 적용되는 PAN 방향 설정을 추가하고 중앙 위치와 각도 제한을 유지한 뒤 확인한다.

## 2026-10-07 — RTSP 인증 확인 도구가 정상 계정에도 401 반환

- 증상: MediaMTX에 인증을 적용한 뒤 libcurl 7.74 기반 DESCRIBE 확인 도구가 정상 계정에도 401을 반환했다. ONVIF HTTP Digest 조회는 같은 계정으로 성공했다.
- 조사: MediaMTX의 실제 사용자/IP/권한 설정과 challenge 종류를 확인했다. 서버는 MD5 Digest, qop 없는 RTSP challenge를 보냈다. 구형 libcurl의 기본 재시도 프로토콜 목록에는 RTSP가 없었으며 허용 목록 변경 후에도 해당 확인 도구는 인증을 완료하지 못했다. 라이브러리 내부의 추가 원인을 확정하지는 않았다.
- 조치: RTSP 확인 도구를 같은 TCP 연결에서 DESCRIBE → 401 challenge → nonce/realm/URI로 Digest 계산 → 인증 DESCRIBE 재요청하는 방식으로 수정했다. 비밀번호를 URL·argv·출력에 넣지 않는다.
- 검증: 실제 LAN 주소에서 무인증/잘못된 계정은 401, 정상 계정은 200과 SDP 반환. 서버 설정 변경 없이 새 확인 도구로 성공했으므로 초기 401 결과를 서버 비밀번호 설정 실패로 판단하지 않았다. loopback FFprobe에서도 H.264 1280×720을 확인했다.

## 2026-10-07 — Events 소스와 설치된 ONVIF 서비스 버전 차이

- 증상: 새 소스의 이벤트 시험은 통과했으나 실행 중 서버 조회는 Events service not found였다.
- 원인: 이전 중단 작업의 소스/build와 /usr/local 설치 binary가 일치하지 않았다.
- 조치: 기존 binary/MediaMTX private 설정을 root 전용 디렉토리에 백업하고,
  Pi 기능 분리·metadata v2와 함께 최신 서보/ONVIF/publisher/lease를 적용했다.
  기존 인증 파일은 유지했다. 서보 재시작은 시작 동작대로 중앙 90°를 출력했다.
- 검증: 세 서비스 active. 실제 GetServices에서 Events 주소 발견, 구독/초기 알림/
  Renew/Unsubscribe 성공. backend Available=true, 추적 Enabled=false 확인.
  실제 RTSP 한 프레임 디코딩 성공 후 reader 0/notReady로 복귀했다.
- 후속: 실제 사람 추적·방향·Qt/VMS overlay는 이번 소프트웨어 시험과 구분한다.
