# Qt에 전달할 Pi ONVIF PTZ 계약

2026-10-07 현재 코드와 실행 중인 서버의 인증된 조회 응답을 확인했다.
조회만 수행했으며 이번 확인에서 이동 명령은 보내지 않았다. 비밀번호는 기록하지 않는다.

## 주소와 토큰

| 항목 | 값 |
| --- | --- |
| Device XAddr | http://192.168.0.92:8080/onvif/device_service |
| Media XAddr | http://192.168.0.92:8080/onvif/media_service |
| PTZ XAddr | http://192.168.0.92:8080/onvif/ptz_service |
| Media ProfileToken | main |
| PTZConfiguration token | ptz-config |
| NodeToken | ptz-node |
| PTZ namespace | http://www.onvif.org/ver20/ptz/wsdl |
| 공통 타입 namespace | http://www.onvif.org/ver10/schema |

실제 Qt 코드는 GetServices/GetProfiles에서 XAddr와 토큰을 읽는다.

## 지원 API

- 이동: AbsoluteMove, RelativeMove, ContinuousMove.
- 정지·상태: Stop, GetStatus.
- 조회: GetNodes, GetNode, GetConfigurations, GetConfiguration,
  GetConfigurationOptions, GetCompatibleConfigurations, GetServiceCapabilities.
- Media: GetProfiles, GetProfile, GetStreamUri.
- GetPresets는 빈 목록을 반환한다.
- **미구현:** GotoHomePosition, SetHomePosition, 프리셋 관리, Zoom,
  optional Speed, PTZ 설정 변경. HomeSupported=false, MaximumNumberOfPresets=0.

중앙 복귀는 GotoHomePosition 대신 **AbsoluteMove(PanTilt x=0,y=0)**를 사용한다.
현재 제한이 대칭이므로 PAN=90°, TILT=90°에 해당한다.

## 좌표·각도 규칙

x는 PAN, y는 TILT이고 각 좌표/변위/속도 성분의 범위는 -1..1이다.

| 요청 | 서보 명령 변환 |
| --- | --- |
| AbsoluteMove | PAN=90+65x, TILT=90+50y |
| RelativeMove | PAN 변화량=130x°, TILT 변화량=100y°, 결과를 각도 범위에 clamp |
| ContinuousMove | PAN/TILT 목표각도 갱신 속도=각 성분 ×30°/s |
| 각도 제한 | PAN 25~155°, TILT 40~140° |
| Absolute/Relative 속도 | 고정 15°/s, Speed 요소를 보내면 오류 |
| Continuous Timeout | 0.1~5초, 생략 시 1초 |

단발 5도 이동은 PAN x=±5/130,y=0 / TILT x=0,y=±5/100이다.
권장 연속 조작은 ±0.4 속도, PT0.6S Timeout, 250ms 주기 갱신이다.
해제/창 비활성화 때 Stop(PanTilt=true,Zoom=false)을 보내고 대기 중 이동을 폐기한다.

코드상 양수는 각도 증가, 음수는 각도 감소다. 기존 방향 정의는 +x=오른쪽,
+y=위쪽이지만 **PAN 실제 좌우가 반대라는 사용자 보고가 있고 보정은 아직 미반영**이다.
실제 방향이 맞는다고 가정하지 않는다. TILT 실물 방향도 장착 상태에서 확인한다.
방향 보정은 Pi의 별도 작업으로 남겨두며 이번 계약 조회에서 변경하지 않았다.

GetStatus의 Position은 출력 명령 각도이며 엔코더 실측값이 아니다. MoveStatus는
소프트웨어 궤적의 MOVING/IDLE, GPIO 출력 오류 때 UNKNOWN/Error다.
Stop은 목표 갱신을 중단하고 마지막 펄스를 유지한다.

## 좌표 공간

- Absolute: `http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace`
- Relative: `http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationGenericSpace`
- Continuous: `http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace`

space 속성 생략 시 해당 기본 공간을 사용한다. 명시하면 위 URI와 일치해야 한다.

## 실제 조회 응답

서버의 HTTP 200 응답 본문을 읽기 좋게 정렬해 저장했다. 인증 헤더/비밀번호는 포함하지 않는다.

- [GetProfiles 전체 응답](responses/GetProfiles.xml)
- [GetConfigurationOptions 전체 응답](responses/GetConfigurationOptions.xml)

GetProfiles의 주요 값은 Profiles/token=main, PTZConfiguration/token=ptz-config,
NodeToken=ptz-node, DefaultPTZTimeout=PT1S다.
GetConfigurationOptions는 위 세 공간 각각의 XRange/YRange=-1..1과
PTZTimeout Min=PT0.1S, Max=PT5S를 반환한다. Zoom/Speed 공간은 없다.

## 인증과 기존 TCP 연결

- HTTP SOAP 조회/제어에는 HTTP Digest 인증이 필요하다. SHA-256/MD5를 지원한다.
- 사용자명/비밀번호는 Qt UI에서 읽어 QAuthenticator에 전달한다. 코드나 로그에 넣지 않는다.
- WS-Security UsernameToken은 미지원이다. UDP 검색은 로그인 없이 가능하다.
- 자동 추적 ON은 ONVIF MoveAndStartTracking, OFF는 Stop을 사용한다. TCP 5000은 내부 IPC 전용이다.
- 탐지/카메라/추적 상태는 ONVIF Events/PullPoint의 제조사 확장 topic에서 받는다.
  기존 DETECT/NO_DETECTION/CAMERA 문자열은 내부 IPC 용도이며 외부 SOAP 필드와 구분한다.
- 외부 PTZ:LEFT/RIGHT/UP/DOWN/CENTER는 거부한다. 이동 버튼/키는 ONVIF로 전환한다.
- 수동 ONVIF 이동/Stop은 추적을 해제하므로 Qt 추적 UI도 OFF로 반영한다.
- 외부 제어/이벤트는 기존 HTTP Digest를 사용한다. metadata v2와 현재 계약은 [Pi 구현 기록](../EDGE_CAMERA_IMPLEMENTATION.md)을 참고한다.

상세 Qt 입력/요청 순서와 XML 요청 예시는 [전환 요청서](../onvif_ptz_qt_migration.md)에 있다.
서버 PTZ 코드는 `pi/onvif_service/src/ptz/ptz_service.cpp`, 공유 제한값은
`common/control/ptz_limits.h`다.
