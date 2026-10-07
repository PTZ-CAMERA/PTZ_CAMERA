# RTSP 영상 전송: TCP와 UDP

## 먼저 구분할 것

RTSP는 영상 재생을 요청하고 세션을 관리하는 프로토콜입니다. 실제 H.264 영상은
보통 RTP 패킷에 담기며, 이 RTP 패킷을 **TCP 또는 UDP로 운반**합니다.
MediaMTX에서는 RTSP 연결 설정은 TCP로 하고, 영상 전송 방식은 클라이언트가
세션 설정 시 선택합니다. UDP 멀티캐스트도 있지만 여기서는 PC 한 대가 Pi에서
영상을 받는 유니캐스트를 비교합니다.

```text
Pi 카메라 → H.264 인코딩 → RTP → MediaMTX → RTP/TCP 또는 RTP/UDP → Qt
                               RTSP가 재생 세션을 설정·관리
```

## 차이

| 항목 | RTSP over TCP | RTSP over UDP |
| --- | --- | --- |
| 영상 패킷 | RTP를 RTSP TCP 연결 안에 실어 보냄 | RTP를 별도 UDP 패킷으로 보냄 |
| 패킷 손실 | TCP가 재전송·순서 복구 | 전송 계층에서 재전송을 보장하지 않음 |
| 지연 양상 | 손실이 있으면 뒤 패킷도 기다려 지연이 누적될 수 있음 | 늦거나 사라진 패킷을 기다리지 않아 실시간성에 유리할 수 있음 |
| 화면 품질 | 손실 구간의 데이터를 복구하는 대신 화면이 늦어질 수 있음 | 손실이 생기면 화면 깨짐·프레임 누락이 나타날 수 있음 |
| 네트워크 연결 | 보통 방화벽·NAT 환경에서 다루기 쉬움 | 영상용 UDP 포트도 통과해야 함 |
| 적합한 경우 | 끊김·손실을 줄이는 쪽이 우선일 때 | 같은 LAN에서 낮은 지연이 우선일 때 |

**UDP가 항상 더 빠른 것은 아닙니다.** 실제 화면 지연에는 Qt 재생기의 수신 버퍼,
지터 버퍼, 디코딩과 화면 표시, 카메라 인코딩 시간도 포함됩니다. 버퍼 설정이
같지 않으면 TCP/UDP 차이만 비교했다고 볼 수 없습니다. TCP라도 버퍼를 줄이면
지연이 개선될 수 있고, UDP라도 큰 버퍼를 쓰면 화면이 늦게 나옵니다.

## 이 프로젝트의 현재 상태

- `pi/camera_stream/mediamtx.yml`은 `rtspTransports: [udp, tcp]`입니다.
  Qt의 RTSP 시청 연결은 TCP 또는 UDP를 선택할 수 있습니다.
- Pi의 C++ 송출기도 같은 Pi 안의 MediaMTX에 RTSP/TCP로 발행합니다. Qt 쪽에
  UDP 시청을 추가하더라도 이 **내부 발행 연결은 TCP로 유지**할 수 있습니다.
- WebRTC는 Qt의 별도 영상 선택지입니다. 아래 TCP/UDP 비교는 **RTSP 시청
  연결**에 대한 것이며 WebRTC 전체를 TCP/UDP 하나로 구분하는 설명은 아닙니다.

## UDP를 비교 시험할 때

1. Pi에서는 TCP 8554와 UDP 8000(RTP)·8001(RTCP)이 열려 있는지 확인합니다.
2. Qt의 RTSP 재생기에서 UDP 전송을 선택합니다. 서버만 변경해도 클라이언트가
   TCP를 요청하면 계속 TCP로 재생됩니다.
3. 같은 해상도·비트레이트·플레이어 버퍼 조건에서 TCP와 UDP의 **재생 중 지연**,
   화면 깨짐, 끊김을 비교합니다. 첫 접속 시간은 카메라의 `runOnDemand` 시작
   시간이 포함되므로 따로 봅니다.
4. UDP가 PC에서 연결되지 않으면 UDP 8000·8001과 방화벽 설정을 확인합니다.

Pi는 두 전송 방식을 허용하지만 Qt 재생기의 선택 UI는 별도로 구현해야 합니다.
Qt가 사용하는 RTSP 재생 라이브러리와 버퍼 설정을 확인한 뒤 시험 조건을
맞추는 것이 좋습니다.

Pi에서는 FFmpeg로 TCP와 UDP 각각 한 프레임의 디코딩을 확인했습니다. Qt UI에서
UDP로 실제 재생되는지는 아직 확인하지 않았습니다.

## 참고 자료

- [MediaMTX RTSP 전송 방식](https://mediamtx.org/docs/features/rtsp-specific-features)
- [MediaMTX 패킷 손실과 지연](https://mediamtx.org/docs/features/decrease-packet-loss)
- [RTSP 표준 RFC 2326](https://www.rfc-editor.org/info/rfc2326/)
