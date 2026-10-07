/**
 * [학습 안내] Device/Media와 검색 메시지 단위 검증
 * XML namespace, 프로파일, URI 요청 전송 방식과 Fault, DTD 거부, Probe 필터를 확인한다.
 * 실제 SOAP 처리 함수를 사용하며 HTTP 소켓/카메라/GPIO는 열지 않는다.
 * assert는 테스트 빌드에서 활성화한다. 통과는 해당 입력/출력 계약의 확인이다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "protocol/service.h"
#include "xml/xml.h"
#include <cassert>
#include <iostream>
namespace {
/**
 * 테스트에서 입력 namespace가 명확한 Envelope를 만든다. 접두어 이름이 바뀌어도 처리 가능한지 검증하는 자료다.
 */
std::string soap(const std::string &operation) {
    return "<s:Envelope xmlns:s=\"" + std::string(onvif::SOAP) + "\" xmlns:d=\"" + onvif::DEVICE +
           "\" xmlns:m=\"" + onvif::MEDIA + "\" xmlns:t=\"" + onvif::TYPES + "\"><s:Body>" +
           operation + "</s:Body></s:Envelope>";
}
} // namespace
/**
 * 테스트의 입력/실행/검증/정리를 순서대로 수행한다. assert 실패는 해당 계약이 깨졌다는 뜻이며 실물 성능 인증이 아니다.
 */
int main() {
    onvif::Config config;
    config.ip = "192.168.0.92";
    config.uuid = "00000000-0000-4000-8000-000000000001";
    auto profiles = onvif::soapResponse(config, "/onvif/media_service", soap("<m:GetProfiles/>"));
    assert(profiles.status == 200);
    auto doc = onvif::parseXml(profiles.xml);
    auto *profile = doc->child(onvif::SOAP, "Body")
                        ->child(onvif::MEDIA, "GetProfilesResponse")
                        ->child(onvif::MEDIA, "Profiles");
    assert(profile && profile->attributes.at("token") == "main");
    for (const auto &transport : {"UDP", "RTSP"}) {
        const auto response = onvif::soapResponse(
            config, "/onvif/media_service",
            soap("<m:GetStreamUri><m:StreamSetup><t:Stream>RTP_unicast</"
                 "t:Stream><t:Transport><t:Protocol>" +
                 std::string(transport) +
                 "</t:Protocol></t:Transport></m:StreamSetup><m:ProfileToken>main</"
                 "m:ProfileToken></m:GetStreamUri>"));
        assert(response.status == 200);
        auto xml = onvif::parseXml(response.xml);
        auto *uri = xml->child(onvif::SOAP, "Body")
                        ->child(onvif::MEDIA, "GetStreamUriResponse")
                        ->child(onvif::MEDIA, "MediaUri")
                        ->child(onvif::TYPES, "Uri");
        assert(uri && uri->text == "rtsp://192.168.0.92:8554/cam");
    }
    assert(onvif::soapResponse(
               config, "/onvif/media_service",
               soap("<m:GetStreamUri><m:ProfileToken>bad</m:ProfileToken></m:GetStreamUri>"))
               .status == 500);
    assert(onvif::soapResponse(config, "/onvif/media_service", soap("<d:GetProfiles/>")).status ==
           500);
    assert(
        onvif::soapResponse(
            config, "/onvif/device_service",
            soap("<d:GetServices><d:IncludeCapability>false</d:IncludeCapability></d:GetServices>"))
            .status == 200);
    assert(onvif::soapResponse(config, "/onvif/device_service",
                               "<!DOCTYPE x [<!ENTITY data 'x'>]><x>&data;</x>")
               .status == 500);
    const std::string probe =
        "<s:Envelope xmlns:s=\"" + std::string(onvif::SOAP) +
        "\" xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" xmlns:d=\"" +
        onvif::DISCOVERY +
        "\" "
        "xmlns:camera=\"http://www.onvif.org/ver10/network/"
        "wsdl\"><s:Header><a:MessageID>urn:test:1</a:MessageID></s:Header>"
        "<s:Body><d:Probe><d:Types>camera:NetworkVideoTransmitter</d:Types></d:Probe></s:Body></"
        "s:Envelope>";
    const auto match = onvif::probeResponse(config, probe);
    assert(!match.empty());
    assert(match.find(config.deviceUrl()) != std::string::npos);
    auto unsupported = probe;
    unsupported.replace(unsupported.find("NetworkVideoTransmitter"), 23, "SomethingElse");
    assert(onvif::probeResponse(config, unsupported).empty());
    assert(onvif::probeResponse(config, "<broken>").empty());
    assert(onvif::probeResponse(config, soap("<m:GetProfiles/>")).empty());
    std::cout << "PASS profiles, URI transports, faults, XML namespaces/DTD rejection, discovery\n";
}
