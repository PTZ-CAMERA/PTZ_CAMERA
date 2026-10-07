# Qt PTZ 제어를 ONVIF로 전환

## 현재 Pi 계약

- Device: `http://<PI_IP>:8080/onvif/device_service`
- PTZ: `http://<PI_IP>:8080/onvif/ptz_service`
- PTZ namespace: `http://www.onvif.org/ver20/ptz/wsdl`
- 공통 타입 namespace: `http://www.onvif.org/ver10/schema`
- SOAP 1.2 / HTTP Digest. 기존 사용자 입력 계정을 사용한다. 비밀번호를 소스에 넣지 않는다.
- 프로파일 `main`, PTZ configuration `ptz-config`, node `ptz-node`.
  실제 Qt 코드는 응답에서 토큰과 XAddr를 읽는다.

외부 Qt의 `PTZ:LEFT/RIGHT/UP/DOWN/CENTER` TCP 명령은 이제 거부한다.
Pi 내부 게이트웨이·키보드·카메라 연결은 기존 서보 서비스가 담당한다.
Qt의 사람 추적 `TRACK:ON/OFF`와 `DETECT`/`CAMERA`/`NO_DETECTION` 수신은 TCP 5000을
계속 사용한다. 사람 추적을 임의의 표준 ONVIF PTZ 명령으로 대체하지 않는다.

## Qt 개발 쪽에 전달할 요청서

```text
기존 Qt 프로젝트의 PTZ 이동 전송을 TCP 문자열 명령에서 ONVIF SOAP로 바꾼다.
Pi의 PTZ endpoint는 http://<PI_IP>:8080/onvif/ptz_service 이다.
기존 사용자명/비밀번호 UI를 HTTP Digest 인증 콜백에 연결한다.

1. GetServices 응답에서 namespace=http://www.onvif.org/ver20/ptz/wsdl인
   Service의 XAddr를 선택한다.
2. GetProfiles에서 선택 프로파일의 token과 PTZConfiguration/token, NodeToken을 읽는다.
3. GetNode와 GetConfigurationOptions로 지원 공간/범위/Timeout을 확인한다.
4. 버튼과 a/d/w/s 키의 누름/해제를 공통 PTZ 입력 상태로 관리한다.
   - 권장 초기 속도: x/y 각각 ±0.4.
   - a/왼쪽: x=-0.4, d/오른쪽: x=+0.4.
   - w/위: y=+0.4, s/아래: y=-0.4.
   - 여러 키가 눌리면 결합한 PanTilt 벡터를 보내 대각선 제어한다.
   - 키 자동 반복 이벤트 대신 QTimer로 명령을 갱신한다.
5. 누르고 있는 동안 ContinuousMove(Velocity/PanTilt, Timeout=PT0.6S)를 전송한다.
   - 250ms마다 현재 벡터로 갱신한다.
   - 모든 방향이 해제되면 Stop(PanTilt=true, Zoom=false)를 보낸다.
   - 창 비활성화, 입력 취소, 연결 종료 때도 Stop을 시도하고 키 상태를 비운다.
   - Stop 도착이 실패해도 Pi의 마지막 명령 Timeout으로 목표 갱신이 끝난다.
6. 중앙 버튼과 c 키는 AbsoluteMove(Position/PanTilt x=0,y=0)를 사용한다.
7. 5도 단발 이동이 필요하면 별도 동작으로 RelativeMove를 사용한다.
   - PAN ±5도: x=±5/130, y=0.
   - TILT ±5도: x=0, y=±5/100.
   - RelativeMove 뒤 즉시 Stop을 보내면 이동이 완료되기 전에 취소될 수 있다.
   - 같은 버튼의 clicked에 단발 이동과 pressed/released 연속 이동을 중복 연결하지 않는다.
8. GetStatus는 약 500ms 간격으로 조회해 Position과 MoveStatus를 표시한다.
   - HTTP 200 이동 응답은 명령 수락이다. 실제 이동 완료 표시로 사용하지 않는다.
   - Position은 서보에 출력한 명령 위치이고 엔코더 실측값은 아니다.
   - MOVING/IDLE은 소프트웨어 궤적 상태, UNKNOWN/Error는 GPIO 출력 오류 표시다.
9. 이동 요청은 한 번에 하나씩 보내고 대기 중 벡터는 최신 값으로 합친다.
   - 해제 시 대기 중 Move를 폐기한다. 진행 중 요청 완료 뒤 Stop을 우선 전송한다.
   - 늦은 Move가 Stop 이후 다시 시작하지 않도록 요청 순서를 관리한다.
   - 재연결 때 이전 키 누름/이동 요청을 자동 재생하지 않는다.
10. QNetworkAccessManager의 비동기 finished/errorOccurred, timeout을 사용한다.
    authenticationRequired에서 QAuthenticator에 UI 계정을 제공한다.
    URI/local-name 기반 XML 파싱, SOAP Fault와 HTTP 오류를 구분한다.
11. 이동 요청이 수락되면 자동 추적 UI를 OFF로 반영한다. Pi가 추적을 해제한다.
    TRACK 제어와 탐지 알림용 NetworkClient/TCP 5000 연결은 유지한다.
12. Zoom/Home/Preset/Speed 설정은 현재 지원 범위에 맞춰 비활성화한다.
    이동 요청에서 optional Speed 요소는 보내지 않는다.
```

## ContinuousMove 예시

```xml
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
            xmlns:p="http://www.onvif.org/ver20/ptz/wsdl"
            xmlns:t="http://www.onvif.org/ver10/schema">
  <s:Body>
    <p:ContinuousMove>
      <p:ProfileToken>main</p:ProfileToken>
      <p:Velocity><t:PanTilt x="0.4" y="0"/></p:Velocity>
      <p:Timeout>PT0.6S</p:Timeout>
    </p:ContinuousMove>
  </s:Body>
</s:Envelope>
```

## Stop·중앙 복귀 예시(Body 안의 연산)

```xml
<p:Stop>
  <p:ProfileToken>main</p:ProfileToken>
  <p:PanTilt>true</p:PanTilt>
  <p:Zoom>false</p:Zoom>
</p:Stop>
```

```xml
<p:AbsoluteMove>
  <p:ProfileToken>main</p:ProfileToken>
  <p:Position><t:PanTilt x="0" y="0"/></p:Position>
</p:AbsoluteMove>
```

## 범위와 동작

| 요소 | 의미 |
| --- | --- |
| absolute x | -1..1 → PAN 25..155°, 0=90° |
| absolute y | -1..1 → TILT 40..140°, 0=90° |
| relative x/y | -1..1 → 각각 ±130°/±100°의 변위. 도달 범위에서 clamp |
| velocity x/y | -1..1 → 각각 최대 ±30°/s의 목표각도 갱신 |
| Absolute/Relative 속도 | 고정 15°/s, optional Speed 미지원 |
| Continuous Timeout | 0.1..5초, 생략 시 1초 |

Absolute/Relative은 서보 서비스의 약 20ms 루프가 목표를 조금씩 갱신한다.
연속 이동은 Timeout, Stop, 각도 경계, 중앙 버튼, 로컬 키, 자동 추적 ON 요청으로 중단될 수 있다.
새 수동 명령은 자동 추적을 OFF로 바꾸고 기존 수동 궤적을 덮어쓴다.
Stop은 목표각도 갱신을 멈추고 마지막 펄스를 유지한다. 실측 각도 센서가 없으므로
즉각적인 기계적 정지나 실제 도착 위치를 보장하는 비상정지 기능은 아니다.

현재 PAN 장착 방향의 좌우 반전 보정은 사용자 요청대로 별도 작업으로 남긴다.
이번 변환은 기존 각도 증가/감소 기준을 유지하므로 실물 방향을 검증해야 한다.

## 확인과 한계

Pi 내부 RPC는 loopback에서만 받으며 300ms 전체 기한을 둔다. RPC는 ONVIF 이벤트
루프 안에서 동기적으로 응답을 기다리는 구간이다. 서보 궤적과 영상은 별도 프로세스에서
계속 실행한다. 백엔드 장애는 SOAP Receiver Fault로 반환한다.

조회·좌표 변환·잘못된 입력은 ONVIF CTest로, 궤적·경계·타임아웃·정지는 순수 계산 및
가짜 GPIO CTest로 확인한다. 실제 서비스에서는 0변위/0속도/Stop 연결 경로를 확인한다.
실제 PC Qt, 비영 이동의 실물 방향/속도, 다른 ONVIF 클라이언트 호환성은 별도 검증한다.
ONVIF 전체 Profile S/T 적합성 인증을 의미하지 않는다.

공식 참고: [PTZ WSDL](https://www.onvif.org/ver20/ptz/wsdl/ptz.wsdl),
[PTZ 26.06 규격](https://www.onvif.org/specs/2606/ONVIF-PTZ-Service-Spec-v2606.pdf).

최신 코드와 서버 응답을 확인한 [Pi PTZ 계약·실제 응답](onvif/qt_ptz_contract.md)도 함께 전달한다.
