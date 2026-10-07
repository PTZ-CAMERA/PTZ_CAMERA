# Qt: Pi를 ONVIF로 검색하고 RTSP URL 받기

Qt 구현은 PC의 별도 프로젝트에서 진행한다. Pi 저장소에는 Qt 설치/프로젝트를 추가하지 않는다.
Pi의 [ONVIF 서비스](../pi/onvif_service/README.md)가 요청에 응답한다.

## Qt 쪽 구현 요청서

```text
기존 Qt 프로젝트에 ONVIF 영상 주소 조회 기능을 추가한다.

QtNetwork의 QUdpSocket, QNetworkAccessManager와 QXmlStreamReader를 사용한다.
UI 스레드를 동기 waitForReadyRead()/반복 대기로 막지 않는다.

1. 장치 검색:
   - 사용자가 선택한 LAN 인터페이스에서 239.255.255.250:3702로 Probe 전송.
   - SOAP 1.2 / WS-Discovery 2005/04 / WS-Addressing 2004/08 사용.
   - MessageID는 QUuid로 매 검색마다 새로 생성.
   - Types는 dn:NetworkVideoTransmitter.
   - dn namespace = http://www.onvif.org/ver10/network/wsdl.
   - 첫 버전에서는 Scopes 필터를 생략.
   - readyRead에서 ProbeMatches를 파싱하고 RelatesTo가 요청 MessageID인지 확인.
   - EndpointReference/Address를 장치 키로 사용해 중복 응답 제거.
   - XAddrs에서 HTTP Device 서비스 주소를 얻고 장치 목록에 표시.
   - 검색 시간은 예를 들어 3초, 1초 간격 최대 3회 재전송하며 UI 취소 지원.

2. 미디어 주소:
   - Device XAddr에 GetServices(IncludeCapability=false) SOAP POST.
   - Namespace=http://www.onvif.org/ver10/media/wsdl인 Service의 XAddr 선택.
   - GetCapabilities(Category=Media)는 필요할 때의 대체 조회 경로.

3. 프로파일:
   - Media XAddr에 GetProfiles POST.
   - Profiles의 token을 읽고 선택할 프로파일을 저장한다.
   - Pi는 현재 token=main 하나를 제공하지만 UI에서 문자열을 하드코딩하지 않는다.

4. 영상 URL:
   - 같은 Media XAddr에 GetStreamUri POST.
   - ProfileToken에 선택된 token 사용.
   - StreamSetup/tt:Stream=RTP_unicast.
   - RTSP TCP를 선택했다면 Transport/tt:Protocol=RTSP.
   - RTSP UDP를 선택했다면 Transport/tt:Protocol=UDP.
   - 응답 MediaUri/tt:Uri를 현재 영상 플레이어에 전달한다.
   - 재생기의 RTSP transport 설정도 UI 선택에 맞춰 적용한다.

5. 로그인:
   - UI 사용자명/비밀번호를 ONVIF HTTP 요청에도 연결한다.
   - QNetworkAccessManager::authenticationRequired에 연결해 QAuthenticator의
     setUser()/setPassword()로 UI 값을 제공한다. Qt가 HTTP Digest를 처리하게 한다.
   - 이 신호는 QueuedConnection으로 연결하지 않는다.
   - 선택한 Pi의 host/port/realm인 경우에만 계정을 제공한다.
   - 재요청에도 인증이 거부되면 중단하고 로그인 실패를 표시한다. 무한 재시도하지 않는다.
   - 사용자명/비밀번호 변경 시 이전 인증 캐시를 clearAccessCache()로 비운다.
   - WS-Security UsernameToken은 현재 Pi에서 지원하지 않는다. HTTP Digest를 사용한다.
   - 반환된 RTSP URL에 비밀번호가 포함돼 있다고 가정하지 않는다.
   - RTSP 플레이어에도 같은 UI 계정을 별도로 전달한다. 재생기에서 URL userinfo를
     요구한다면 QUrl로 인코딩해 구성하고 해당 URL을 로그에 출력하지 않는다.
   - WebRTC/HLS를 사용할 때는 MediaMTX HTTP 인증에도 같은 계정을 적용한다.

6. 요청/파싱:
   - 모든 POST는 application/soap+xml; charset=utf-8, SOAP 1.2 Envelope 사용.
   - QByteArray 본문을 post()에 전달해 Content-Length가 있는 요청 생성.
   - finished/errorOccurred 신호와 QTimer 또는 지원 버전의 transfer timeout 사용.
   - HTTP 오류와 SOAP Fault, 빈 URL, 누락된 token을 구분해 표시.
   - XML prefix 이름(s/soap/tt 등)에 의존하지 않고 namespaceUri + local name으로 파싱.
   - 선택 장치 변경/취소 시 이전 요청 취소. 늦게 도착한 이전 응답을 새 장치에 적용하지 않는다.
   - 검증한 http/https 서비스 주소와 rtsp 영상 주소만 해당 클라이언트에 전달한다.

7. 제어 연결:
   - PTZ 이동은 ONVIF PTZ 서비스로 전환한다. 상세 요청서는 onvif_ptz_qt_migration.md를 따른다.
   - TRACK ON/OFF와 탐지/카메라 알림용 TCP 5000 연결은 유지한다.
   - 외부 PTZ 문자열 명령은 Pi에서 거부한다.
   - 장치 검색이 막힌 환경에서는 Device 서비스 URL 수동 입력도 제공한다.
```

## GetStreamUri 요청 예시

URL: `http://<PI_IP>:8080/onvif/media_service`

```xml
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"
            xmlns:trt="http://www.onvif.org/ver10/media/wsdl"
            xmlns:tt="http://www.onvif.org/ver10/schema">
  <s:Body>
    <trt:GetStreamUri>
      <trt:StreamSetup>
        <tt:Stream>RTP_unicast</tt:Stream>
        <tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>
      </trt:StreamSetup>
      <trt:ProfileToken>main</trt:ProfileToken>
    </trt:GetStreamUri>
  </s:Body>
</s:Envelope>
```

응답의 `tt:Uri`는 `rtsp://<PI_IP>:8554/cam`이다. URI를 받는 단계와 RTSP 영상
연결 단계는 별도이며, 영상 연결 실패 시 URL 조회 실패로 표시하지 않는다.

## 확인 범위

Pi에서 C++ HTTP 클라이언트로 조회 흐름, UDP multicast Probe 응답을 검증한다.
실제 PC의 multicast 도달 여부·Qt XML 파싱·영상 플레이어 연결은 PC에서 확인해야 한다.
ONVIF API 부분 구현이므로 일반 ONVIF 클라이언트의 모든 초기화 요청을 지원한다고
가정하지 않는다. 필요한 요청이 추가로 확인되면 Pi의 지원 범위를 함께 확장한다.

공식 메시지 형식: [Media WSDL](https://www.onvif.org/ver10/media/wsdl/media.wsdl),
[Device WSDL](https://www.onvif.org/ver10/device/wsdl/devicemgmt.wsdl).

## HTTP 인증 연결 핵심

```cpp
// username/password는 입력 UI에서 읽는다. 소스에 비밀번호를 하드코딩하지 않는다.
connect(manager, &QNetworkAccessManager::authenticationRequired,
        this, [this](QNetworkReply* reply, QAuthenticator* authenticator) {
    if (reply->url().host() != selectedDeviceUrl.host() ||
        reply->url().port(80) != selectedDeviceUrl.port(80) ||
        authenticator->realm() != QStringLiteral("PTZ-CAMERA"))
        return;
    if (reply->property("credentialsProvided").toBool())
        return;
    reply->setProperty("credentialsProvided", true);
    authenticator->setUser(username);
    authenticator->setPassword(password);
});
```

위 예시의 멤버명은 PC 프로젝트에 맞춰 연결한다. 요청 완료 시 인증 실패를 처리하고,
장치/계정 변경 시 인증 캐시와 이전 요청을 정리한다. 서비스 검색의 성공과 로그인·영상
접속의 성공을 UI에서 구분한다.

Qt 공식 문서: [authenticationRequired](https://doc.qt.io/qt-6/qnetworkaccessmanager.html#authenticationRequired).

PTZ 버튼·키보드 전환은 [ONVIF PTZ 변경 요청서](onvif_ptz_qt_migration.md)에 정리했다.
