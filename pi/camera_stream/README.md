# Raspberry Pi 카메라 송출

Pi의 C++17 송출기(`src/main.cpp`)가 GStreamer 라이브러리로 IMX219 영상을
캡처하고 H.264로 인코딩하여 MediaMTX에 RTSP로 발행합니다. MediaMTX가
같은 영상을 RTSP, WebRTC, HLS로 제공합니다. Raspberry Pi 4 / Raspberry Pi OS
Bullseye에서 확인한 구성입니다.

송출 경로: `libcamerasrc → videoconvert → x264enc → h264parse → rtspclientsink → MediaMTX`

MediaMTX는 카메라 드라이버나 인코더가 아니라 영상 서버입니다. C++ 송출기가
발행한 한 스트림을 여러 시청자에게 전달하고, WebRTC/HLS 접속도 제공합니다.

## 설치

1. [MediaMTX 공식 릴리스](https://github.com/bluenviron/mediamtx/releases)에서
   Linux arm64 바이너리를 내려받아 `/usr/local/bin/mediamtx`에 설치합니다.
   이 Pi에는 v1.21.1이 이미 설치되어 있습니다.
2. C++ 빌드 및 GStreamer 실행 패키지를 설치합니다.

   ```sh
   sudo apt update
   sudo apt install cmake g++ pkg-config libgstreamer1.0-dev \
     libcamera0 gstreamer1.0-plugins-base \
     gstreamer1.0-plugins-ugly gstreamer1.0-rtsp
   ```

3. 프로젝트 루트에서 `sudo ./pi/camera_stream/install.sh`를 실행합니다.
   이 스크립트는 C++ 송출기를 빌드하고 설정·바이너리·systemd 유닛을 시스템 경로에 복사하며
   서비스를 부팅 시 시작하도록 등록합니다. 현재 스트림이 중지된 상태라면
   설치 중에도 그 상태를 유지합니다.
4. `sudo systemctl start ptz-camera.service`로 영상을 시작합니다.

프로젝트 파일을 수정한 뒤에는 설치 스크립트를 다시 실행해야 시스템 복사본에
변경이 반영됩니다.

## 접속

Pi의 LAN 주소를 `hostname -I`로 확인한 뒤 `<PI_IP>`를 바꿔 넣습니다.

| 용도 | 주소 |
| --- | --- |
| 브라우저 / Qt WebEngine (WebRTC) | `http://<PI_IP>:8889/cam/` |
| VLC 등 RTSP 클라이언트 | `rtsp://<PI_IP>:8554/cam` |
| 브라우저 대체 경로 (HLS) | `http://<PI_IP>:8891/cam/` |

WebRTC는 TCP 8889(연결 설정)와 UDP 8189(영상), RTSP는 TCP 8554를 사용합니다.
HLS는 pigpio의 기본 포트 8888과 충돌하지 않도록 8891을 사용합니다.

## 운영

```sh
sudo systemctl start ptz-camera.service
sudo systemctl stop ptz-camera.service
sudo systemctl restart ptz-camera.service
systemctl status ptz-camera.service
journalctl -u ptz-camera.service -f
journalctl -u ptz-camera-watchdog.service -n 20
```

`ptz-camera-watchdog.timer`는 30초마다 실제 프레임을 검사합니다. 연속으로
프레임을 받지 못하면 서비스를 재시작하지만, 사용자가 서비스를 중지한 상태는
그대로 둡니다. 카메라 송출과 서보 제어는 독립적으로 동작합니다.

현재 송출기는 1280×720, 30fps, H.264 Constrained Baseline을 사용합니다.
이 Pi에서 GStreamer의 `v4l2h264enc`가 프레임 처리 오류를 내어 `x264enc`
소프트웨어 인코딩으로 구성했습니다. CPU 사용량은 송출기 기준 약 1코어 이상이며,
고해상도/고프레임 설정 시 여유를 확인해야 합니다.
