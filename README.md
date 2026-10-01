# PTZ_CAMERA

Raspberry Pi 4와 MG90S 서보 두 개로 만든 2축 PTZ 카메라 프로토타입입니다.
Pi가 서보를 제어하고 IMX219 카메라 영상을 송출합니다. PC의 Qt UI는 별도
프로젝트에서 개발합니다.

## 프로젝트 구성

```text
PTZ_CAMERA/
├── pi/
│   ├── servo_test/       # pigpio 기반 PAN/TILT 테스트 (C++17)
│   └── camera_stream/    # C++17 카메라 송출기 → MediaMTX 및 systemd 서비스
├── docs/
│   └── sessions/         # 작업 기록
└── troubleshotting.md    # 문제별 조사·조치 기록
```

현재는 Raspberry Pi만 사용합니다. ESP32 실험 코드와 PC Qt UI 코드는 이 저장소에
포함하지 않습니다.

## 서보 테스트

서보 신호는 BCM GPIO18(PAN), GPIO19(TILT), 중앙 복귀 버튼은 GPIO17과 GND에
연결합니다. 서보 두 개는 외부 4.8~6V 전원으로 구동하고 **Pi GND와 외부 전원
GND를 공통 연결**합니다. GPIO에는 서보 신호만 연결합니다.

```sh
cmake -S pi/servo_test -B pi/servo_test/build
cmake --build pi/servo_test/build -j4
sudo ./pi/servo_test/build/servo_test
```

키는 `a`/`d`(PAN), `w`/`s`(TILT), `c`(중앙), `q`(종료)입니다. 현재 범위는
PAN 25~155도, TILT 40~140도입니다. 자세한 내용은
[서보 코드](pi/servo_test/src/main.cpp)를 참고하세요.

## 카메라 송출

Pi에 MediaMTX와 서비스를 설치하는 방법은 [카메라 송출 안내](pi/camera_stream/README.md)에
있습니다. 설치된 Pi에서는 다음 명령으로 송출을 켜거나 끕니다.

```sh
sudo systemctl start ptz-camera.service
sudo systemctl stop ptz-camera.service
systemctl status ptz-camera.service
```

Pi의 현재 LAN 주소는 `hostname -I`로 확인합니다. 주소가 `192.168.0.92`라면
WebRTC는 `http://192.168.0.92:8889/cam/`, RTSP는
`rtsp://192.168.0.92:8554/cam`입니다. PC의 Qt UI에서는 이 주소를 사용합니다.

## 기록

- [작업 기록](docs/sessions/2026-10-01.md)
- [2026-10-02 정리 기록](docs/sessions/2026-10-02.md)
- [트러블슈팅](troubleshotting.md) — 새 문제를 조사할 때마다 여기에 추가합니다.

현재 알려진 문제: 카메라 원본 H.264에 간헐적으로 핑크색 프레임이 발생합니다.
자세한 조사 결과는 트러블슈팅 기록에 있습니다.
