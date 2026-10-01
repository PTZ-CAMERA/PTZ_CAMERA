# Raspberry Pi 서보 제어

`servo_test`는 MG90S PAN/TILT 두 축과 중앙 복귀 버튼을 pigpio C API로 제어합니다.
GPIO는 BCM 번호 기준으로 PAN 18, TILT 19, 버튼 17입니다. 서보 전원은 외부
4.8~6V에서 공급하고 Pi GND와 외부 전원 GND를 공통 연결합니다.

## 설치 및 실행

```sh
sudo apt install cmake g++ libpigpio-dev
sudo ./pi/servo_test/install.sh
systemctl status ptz-servo.service
```

설치 스크립트는 C++ 프로그램을 빌드하고 `ptz-servo.service`를 시작합니다.
부팅 후에도 서비스가 자동 시작됩니다. 서버를 끄려면
`sudo systemctl stop ptz-servo.service`를 실행합니다.

터미널 수동 제어만 할 때는 서비스를 먼저 중지하고 다음처럼 실행합니다. pigpio를
사용하는 두 프로세스를 동시에 실행하지 마세요.

```sh
sudo systemctl stop ptz-servo.service
sudo ./pi/servo_test/build/servo_test
```

터미널에서 `--server`를 붙이면 TCP와 키보드 제어를 동시에 사용할 수 있습니다.
키는 `a`/`d`(PAN), `w`/`s`(TILT), `c`(중앙), `q`(종료)입니다.

## Qt와 TCP 명령

Qt `NetworkClient::sendLine()`과 같이 TCP `5000` 포트로 UTF-8 명령 한 줄을
보냅니다. 각 줄의 끝은 `\n` 또는 `\r\n`입니다. 한 연결에서 여러 줄을
연속으로 보낼 수 있습니다.

| 명령 | 동작 |
| --- | --- |
| `PTZ:LEFT` | PAN -5° |
| `PTZ:RIGHT` | PAN +5° |
| `PTZ:UP` | TILT +5° |
| `PTZ:DOWN` | TILT -5° |
| `PTZ:CENTER` | PAN/TILT 90° |

PAN은 25~155°, TILT는 40~140°로 제한합니다. 각 버튼 입력은 한 번에 5°만
움직이므로 누르고 있는 동안 계속 움직이려면 Qt가 명령을 반복 전송해야 합니다.
현재 Qt 코드의 `TRACK:ON/OFF`는 객체 추적 기능이 없으므로 서버가
`ERR tracking unavailable`을 반환합니다. 잘못된 명령도 `ERR`를 반환합니다.
정상 PTZ 명령에는 응답하지 않습니다. 현재 Qt 수신 코드는 PTZ 응답을 해석하지
않기 때문입니다. TCP 연결 성공과 실제 서보 이동은 별개이며, Pi 로그에서
각도 변경을 확인할 수 있습니다.

```sh
journalctl -u ptz-servo.service -f
```

이 서버는 인증 없이 LAN의 TCP 5000 포트에서 명령을 받으므로 신뢰할 수 있는
네트워크에서만 사용하세요.
