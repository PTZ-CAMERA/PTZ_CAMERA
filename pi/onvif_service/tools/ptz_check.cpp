/**
 * [학습 안내] ONVIF PTZ 상태와 무변위 제어 경로 확인
 * 기본은 설정/상태 조회만 한다. --zero-test는 현재 위치/0변위/0속도/Stop을 요청한다.
 * 실제 서비스에 명령을 보내는 진단 도구이며 수동 명령은 추적 모드를 해제할 수 있다.
 * 비영 이동의 실물 방향이나 모터 정확도까지 검증하는 도구는 아니다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
// 기본은 상태 조회만 한다. --zero-test는 0변위/0속도/Stop으로 제어 경로를 확인한다.
#include "soap_client.h"
#include "protocol/service.h"
#include <iostream>
#include <thread>
#include <chrono>
namespace {
/**
 * SOAP Body에서 기대하는 operation response를 찾는다. HTTP 성공만으로 응답 의미를 성공 판정하지 않는다.
 */
const onvif::Node &payload(const onvif::Node &response, const std::string &ns,
                           const std::string &operation) {
    return onvif::client::required(onvif::client::required(response, onvif::SOAP, "Body"), ns,
                                   operation + "Response");
}
} // namespace
/**
 * 실행 인자와 자원을 준비한 뒤 위 함수들을 호출한다. 정상/오류 종료를 반환값으로 구분하고 보유 자원을 종료 경로에서 정리한다.
 */
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && std::string(argv[2]) != "--zero-test")) {
        std::cerr << "Usage: onvif_ptz_check DEVICE_URL [--zero-test]\n";
        return 1;
    }
    try {
        using onvif::client::call;
        using onvif::client::required;
        auto services =
            call(argv[1],
                 "<d:GetServices><d:IncludeCapability>false</d:IncludeCapability></d:GetServices>");
        std::string ptzUrl, mediaUrl;
        for (const auto &service : payload(*services, onvif::DEVICE, "GetServices").children) {
            const auto ns = onvif::trim(required(*service, onvif::DEVICE, "Namespace").text);
            if (ns == onvif::PTZ)
                ptzUrl = onvif::trim(required(*service, onvif::DEVICE, "XAddr").text);
            if (ns == onvif::MEDIA)
                mediaUrl = onvif::trim(required(*service, onvif::DEVICE, "XAddr").text);
        }
        if (ptzUrl.empty() || mediaUrl.empty())
            throw std::runtime_error("Media/PTZ service missing");
        auto profiles = call(mediaUrl, "<m:GetProfiles/>");
        const auto &profile =
            required(payload(*profiles, onvif::MEDIA, "GetProfiles"), onvif::MEDIA, "Profiles");
        const auto token = profile.attributes.at("token");
        const auto &configuration = required(profile, onvif::TYPES, "PTZConfiguration");
        const auto configToken = configuration.attributes.at("token");
        const auto query = "<p:GetStatus><p:ProfileToken>" + onvif::escape(token) +
                           "</p:ProfileToken></p:GetStatus>";
        auto state = call(ptzUrl, query);
        const auto &status =
            required(payload(*state, onvif::PTZ, "GetStatus"), onvif::PTZ, "PTZStatus");
        const auto &xy =
            required(required(status, onvif::TYPES, "Position"), onvif::TYPES, "PanTilt");
        std::cout << "PTZ XAddr: " << ptzUrl << " profile=" << token
                  << " commanded x=" << xy.attributes.at("x") << " y=" << xy.attributes.at("y")
                  << '\n';
        if (argc == 3) {
            const auto profileElement =
                "<p:ProfileToken>" + onvif::escape(token) + "</p:ProfileToken>";
            call(ptzUrl, "<p:GetNodes/>");
            call(ptzUrl, "<p:GetConfigurationOptions><p:ConfigurationToken>" +
                             onvif::escape(configToken) +
                             "</p:ConfigurationToken></p:GetConfigurationOptions>");
            call(ptzUrl, "<p:AbsoluteMove>" + profileElement + "<p:Position><t:PanTilt x=\"" +
                             onvif::escape(xy.attributes.at("x")) + "\" y=\"" +
                             onvif::escape(xy.attributes.at("y")) +
                             "\"/></p:Position></p:AbsoluteMove>");
            call(
                ptzUrl,
                "<p:RelativeMove>" + profileElement +
                    "<p:Translation><t:PanTilt x=\"0\" y=\"0\"/></p:Translation></p:RelativeMove>");
            call(ptzUrl,
                 "<p:ContinuousMove>" + profileElement +
                     "<p:Velocity><t:PanTilt x=\"0\" "
                     "y=\"0\"/></p:Velocity><p:Timeout>PT0.1S</p:Timeout></p:ContinuousMove>");
            call(ptzUrl, "<p:Stop>" + profileElement +
                             "<p:PanTilt>true</p:PanTilt><p:Zoom>false</p:Zoom></p:Stop>");
            auto finalState = call(ptzUrl, query);
            const auto &finalStatus =
                required(payload(*finalState, onvif::PTZ, "GetStatus"), onvif::PTZ, "PTZStatus");
            if (required(required(finalStatus, onvif::TYPES, "MoveStatus"), onvif::TYPES, "PanTilt")
                    .text != "IDLE")
                throw std::runtime_error("Motion not idle after Stop");
            std::cout
                << "PASS authenticated PTZ metadata/status and zero-displacement control path\n";
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
