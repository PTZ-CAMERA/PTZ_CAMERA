/**
 * [학습 안내] 표준 PTZ 요청의 좌표 변환과 검증 테스트
 * Config의 servoRequest 콜백을 가짜 backend로 바꿔 실제 GPIO 없이 명령 문자열을 확인한다.
 * 잘못된 수/토큰/공간/Zoom/Timeout과 backend 장애의 Fault를 검증한다.
 * 조회·변환의 통과가 실물 서보 방향 보정이나 ONVIF 전체 적합성을 뜻하지 않는다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "protocol/service.h"
#include "ptz/ptz_service.h"
#include "ptz/servo_bridge.h"
#include "xml/xml.h"
#include <cassert>
#include <iostream>
namespace {
/**
 * 테스트에서 입력 namespace가 명확한 Envelope를 만든다. 접두어 이름이 바뀌어도 처리 가능한지 검증하는 자료다.
 */
std::string soap(const std::string &body) {
    return "<s:Envelope xmlns:s=\"" + std::string(onvif::SOAP) + "\" xmlns:p=\"" + onvif::PTZ +
           "\" xmlns:t=\"" + onvif::TYPES + "\"><s:Body>" + body + "</s:Body></s:Envelope>";
}
} // namespace
/**
 * 테스트의 입력/실행/검증/정리를 순서대로 수행한다. assert 실패는 해당 계약이 깨졌다는 뜻이며 실물 성능 인증이 아니다.
 */
int main() {
    onvif::Config config;
    config.ip = "192.168.0.92";
    std::string received;
    config.servoRequest = [&](const std::string &command) {
        received = command;
        return command == "ONVIF:STATUS" ? "STATE:103,65,1,0" : "OK";
    };
    const auto call = [&](const std::string &operation) {
        return onvif::soapResponse(config, "/onvif/ptz_service", soap(operation));
    };
    auto nodes = call("<p:GetNodes/>");
    assert(nodes.status == 200);
    assert(nodes.xml.find("VelocityGenericSpace") != std::string::npos &&
           nodes.xml.find("ZoomPositionSpace") == std::string::npos);
    assert(call("<p:GetConfigurationOptions><p:ConfigurationToken>ptz-config</"
                "p:ConfigurationToken></p:GetConfigurationOptions>")
               .status == 200);
    assert(call("<p:GetConfigurationOptions><p:ConfigurationToken>bad</p:ConfigurationToken></"
                "p:GetConfigurationOptions>")
               .status == 500);
    assert(call("<p:AbsoluteMove><p:ProfileToken>main</p:ProfileToken><p:Position><t:PanTilt "
                "x=\"0.2\" y=\"-0.5\"/></p:Position></p:AbsoluteMove>")
               .status == 200);
    assert(received == "ONVIF:ABS:103.000000,65.000000");
    assert(call("<p:RelativeMove><p:ProfileToken>main</p:ProfileToken><p:Translation><t:PanTilt "
                "x=\"0.1\" y=\"-0.2\"/></p:Translation></p:RelativeMove>")
               .status == 200);
    assert(received == "ONVIF:REL:13.000000,-20.000000");
    assert(call("<p:ContinuousMove><p:ProfileToken>main</p:ProfileToken><p:Velocity><t:PanTilt "
                "x=\"-0.5\" y=\"1\"/></p:Velocity><p:Timeout>PT0.5S</p:Timeout></p:ContinuousMove>")
               .status == 200);
    assert(received == "ONVIF:VEL:-15.000000,30.000000,500");
    assert(call("<p:Stop><p:ProfileToken>main</p:ProfileToken></p:Stop>").status == 200 &&
           received == "ONVIF:STOP");
    received.clear();
    assert(
        call("<p:Stop><p:ProfileToken>main</p:ProfileToken><p:PanTilt>false</p:PanTilt></p:Stop>")
                .status == 200 &&
        received.empty());
    assert(call("<p:MoveAndStartTracking><p:ProfileToken>main</p:ProfileToken></"
                "p:MoveAndStartTracking>")
                   .status == 200 &&
           received == "ONVIF:TRACK");
    assert(call("<p:MoveAndStartTracking><p:ProfileToken>main</"
                "p:ProfileToken><p:TargetPosition><t:PanTilt x=\"0.2\" "
                "y=\"-0.5\"/></p:TargetPosition></p:MoveAndStartTracking>")
                   .status == 200 &&
           received == "ONVIF:MOVE_TRACK:103.000000,65.000000");
    assert(call("<p:MoveAndStartTracking><p:ProfileToken>main</p:ProfileToken><p:ObjectID>1</"
                "p:ObjectID></p:MoveAndStartTracking>")
               .status == 500);
    auto status = call("<p:GetStatus><p:ProfileToken>main</p:ProfileToken></p:GetStatus>");
    assert(status.status == 200);
    auto parsed = onvif::parseXml(status.xml);
    auto *state = parsed->child(onvif::SOAP, "Body")
                      ->child(onvif::PTZ, "GetStatusResponse")
                      ->child(onvif::PTZ, "PTZStatus");
    assert(state &&
           state->child(onvif::TYPES, "MoveStatus")->child(onvif::TYPES, "PanTilt")->text ==
               "MOVING");
    for (const auto *invalid : {"nan", "2", "1oops"}) {
        auto request =
            "<p:AbsoluteMove><p:ProfileToken>main</p:ProfileToken><p:Position><t:PanTilt x=\"" +
            std::string(invalid) + "\" y=\"0\"/></p:Position></p:AbsoluteMove>";
        assert(call(request).status == 500);
    }
    assert(call("<p:ContinuousMove><p:ProfileToken>main</p:ProfileToken><p:Velocity><t:PanTilt "
                "x=\"1\" y=\"0\"/></p:Velocity><p:Timeout>PT6S</p:Timeout></p:ContinuousMove>")
               .status == 500);
    assert(call("<p:AbsoluteMove><p:ProfileToken>main</p:ProfileToken><p:Position><t:Zoom "
                "x=\"1\"/></p:Position></p:AbsoluteMove>")
               .status == 500);
    assert(call("<p:AbsoluteMove><p:ProfileToken>main</p:ProfileToken><p:Position><t:PanTilt "
                "space=\"wrong\" x=\"0\" y=\"0\"/></p:Position></p:AbsoluteMove>")
               .status == 500);
    config.servoRequest = [](const std::string &) -> std::string {
        throw onvif::BackendError("Servo unavailable");
    };
    auto fault = call("<p:GetStatus><p:ProfileToken>main</p:ProfileToken></p:GetStatus>");
    assert(fault.status == 500 && fault.xml.find("s:Receiver") != std::string::npos);
    std::cout << "PASS PTZ metadata, coordinate mapping, moves/stop/status, validation and backend "
                 "faults\n";
}
