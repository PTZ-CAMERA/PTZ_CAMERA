/**
 * [학습 안내] 실제 ONVIF 서비스에서 RTSP URL 조회
 * GetServices → GetProfiles → GetStreamUri의 클라이언트 흐름을 확인한다.
 * 미디어 주소와 토큰은 응답에서 읽는다. 이 도구 자체가 RTSP PLAY를 수행하지 않는다.
 * 로그에는 중간 주소/토큰만, stdout에는 영상 URL만 출력한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
// Pi에서 Qt의 영상 주소 조회 흐름을 확인한다.
#include "soap_client.h"
#include "protocol/service.h"
#include <iostream>

/**
 * 실행 인자와 자원을 준비한 뒤 위 함수들을 호출한다. 정상/오류 종료를 반환값으로 구분하고 보유 자원을 종료 경로에서 정리한다.
 */
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: onvif_query http://PI_IP:8080/onvif/device_service\n";
        return 1;
    }
    using onvif::client::call;
    using onvif::client::required;
    int exit = 0;
    try {
        auto services =
            call(argv[1],
                 "<d:GetServices><d:IncludeCapability>false</d:IncludeCapability></d:GetServices>");
        const auto &entries = required(required(*services, onvif::SOAP, "Body"), onvif::DEVICE,
                                       "GetServicesResponse");
        std::string media;
        for (const auto &entry : entries.children)
            if (onvif::trim(required(*entry, onvif::DEVICE, "Namespace").text) == onvif::MEDIA)
                media = onvif::trim(required(*entry, onvif::DEVICE, "XAddr").text);
        if (media.empty())
            throw std::runtime_error("Media service not found");
        std::cerr << "Media XAddr: " << media << '\n';
        auto profiles = call(media, "<m:GetProfiles/>");
        const auto &list =
            required(required(*profiles, onvif::SOAP, "Body"), onvif::MEDIA, "GetProfilesResponse");
        const auto &profile = required(list, onvif::MEDIA, "Profiles");
        const auto token = profile.attributes.at("token");
        std::cerr << "Profile token: " << token << '\n';
        auto stream = call(
            media, "<m:GetStreamUri><m:StreamSetup><t:Stream>RTP_unicast</t:Stream><t:Transport>"
                   "<t:Protocol>RTSP</t:Protocol></t:Transport></m:StreamSetup><m:ProfileToken>" +
                       onvif::escape(token) + "</m:ProfileToken></m:GetStreamUri>");
        const auto &reply =
            required(required(*stream, onvif::SOAP, "Body"), onvif::MEDIA, "GetStreamUriResponse");
        const auto &uri = required(required(reply, onvif::MEDIA, "MediaUri"), onvif::TYPES, "Uri");
        std::cout << onvif::trim(uri.text) << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        exit = 1;
    }
    return exit;
}
