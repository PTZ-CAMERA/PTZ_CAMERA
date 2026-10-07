/**
 * [학습 안내] 표준 PTZ 요청을 서보 제어 명령으로 변환
 * 프로파일/노드/설정 토큰과 좌표 공간/범위/Timeout을 검증한 뒤 내부 RPC를 만든다.
 * 공유 geometry 제한값을 사용해 ONVIF 정규화 좌표와 GPIO 각도 제한이 달라지지 않게 한다.
 * MoveAndStartTracking은 현재 위치 또는 PTZVector 위치 이후 AI에 제어를 위임한다. 객체 ID/Geo/프리셋은 미지원이다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "ptz/ptz_service.h"
#include "control/ptz_limits.h"
#include "ptz/servo_bridge.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <regex>
#include <sstream>
namespace onvif {
namespace g = ptz::geometry;
namespace {
/**
 * 선택 Node의 공백 정리된 text를 읽는다. Node가 없는 경우의 의미는 operation별로 판단한다.
 */
std::string text(const Node *node) {
    return node ? trim(node->text) : std::string{};
}
/**
 * 요청이 현재 프로파일/설정/노드를 가리키는지 검사한다. 다른 토큰으로 이동을 실행하지 않는다.
 */
void token(const Node &op, const char *field, const char *expected) {
    if (text(op.child(PTZ, field)) != expected)
        throw std::invalid_argument(std::string("Unknown ") + field);
}
/**
 * 지원하는 absolute/relative/velocity 공간과 성분 범위를 XML로 만든다. Zoom/Speed 공간은 추가하지 않는다.
 */
std::string spaces() {
    std::string result;
    for (const auto &[tag, uri] :
         {std::pair<std::string, std::string>{"AbsolutePanTiltPositionSpace", POSITION_SPACE},
          {"RelativePanTiltTranslationSpace", TRANSLATION_SPACE},
          {"ContinuousPanTiltVelocitySpace", VELOCITY_SPACE}})
        result += "<tt:" + tag + "><tt:URI>" + uri +
                  "</tt:URI><tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange>"
                  "<tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange></tt:" +
                  tag + ">";
    return result;
}
/**
 * 2축 PTZ node와 지원 공간/프리셋·Home 범위를 설명한다. 실물 피드백이나 전체 적합성 인증을 표시하지 않는다.
 */
std::string node() {
    return "<tptz:PTZNode token=\"ptz-node\"><tt:Name>MG90S Pan Tilt</tt:Name>"
           "<tt:SupportedPTZSpaces>" +
           spaces() +
           "</tt:SupportedPTZSpaces><tt:MaximumNumberOfPresets>0</tt:MaximumNumberOfPresets>"
           "<tt:HomeSupported>false</tt:HomeSupported></tptz:PTZNode>";
}
/**
 * 좌표 문자열 전체를 실수로 파싱하고 finite/-1..1을 확인한다. NaN/Infinity/뒤의 쓰레기 문자를 허용하지 않는다.
 */
double number(const std::string &input) {
    std::size_t count = 0;
    double value;
    try {
        value = std::stod(input, &count);
    } catch (...) {
        throw std::invalid_argument("Invalid vector number");
    }
    if (count != input.size() || !std::isfinite(value) || std::abs(value) > 1)
        throw std::invalid_argument("Vector must be finite within -1..1");
    return value;
}
/**
 * PanTilt x/y와 coordinate space를 검증한다. 지원하지 않는 Zoom이나 공간은 서보 RPC 전에 거부한다.
 */
std::pair<double, double> vector(const Node &parent, const char *uri) {
    if (parent.child(TYPES, "Zoom"))
        throw std::invalid_argument("Zoom is not supported");
    const auto *xy = parent.child(TYPES, "PanTilt");
    if (!xy)
        throw std::invalid_argument("PanTilt vector required");
    const auto space = xy->attributes.find("space");
    if (space != xy->attributes.end() && space->second != uri)
        throw std::invalid_argument("Unsupported PTZ coordinate space");
    const auto x = xy->attributes.find("x"), y = xy->attributes.find("y");
    if (x == xy->attributes.end() || y == xy->attributes.end())
        throw std::invalid_argument("Both x and y required");
    return {number(x->second), number(y->second)};
}
/** Timeout을 xs:duration으로 읽고 연속 이동의 0.1~5초 범위를 확인해 ms로 바꾼다. */
int duration(const Node *timeout) {
    if (!timeout)
        return 1000;
    // xs:duration의 날짜/시간 표기 중 0.1~5초 범위를 수용한다. 월/년은 0만 허용.
    static const std::regex pattern(
        R"(^P(?:([0-9]+)Y)?(?:([0-9]+)M)?(?:([0-9]+)D)?(?:T(?:([0-9]+)H)?(?:([0-9]+)M)?(?:([0-9]+(?:\.[0-9]+)?)S)?)?$)");
    const auto value = text(timeout);
    std::smatch match;
    if (value.size() > 64 || !std::regex_match(value, match, pattern))
        throw std::invalid_argument("Invalid Timeout");
    double values[6]{};
    bool any = false;
    for (int i = 1; i <= 6; ++i)
        if (match[i].matched) {
            values[i - 1] = std::stod(match[i].str());
            any = true;
        }
    const auto seconds = values[2] * 86400 + values[3] * 3600 + values[4] * 60 + values[5];
    if (!any || values[0] != 0 || values[1] != 0 || !std::isfinite(seconds) || seconds < 0.1 ||
        seconds > 5)
        throw std::invalid_argument("Timeout must be 0.1..5 seconds");
    return static_cast<int>(std::lround(seconds * 1000));
}
/**
 * 테스트 콜백 또는 실제 loopback bridge를 사용해 짧은 요청을 보낸다. backend ERR은 SOAP Receiver 계층으로 올린다.
 */
std::string rpc(const Config &config, const std::string &command) {
    const auto result =
        config.servoRequest ? config.servoRequest(command) : servoRpc(config.servoPort, command);
    if (result.rfind("ERR", 0) == 0)
        throw BackendError("Servo rejected request");
    return result;
}
/**
 * 서보의 OK 수락을 확인한다. 실제 축이 목표에 도착했다는 확인은 아니다.
 */
void accepted(const Config &config, const std::string &command) {
    if (rpc(config, command) != "OK")
        throw BackendError("Invalid servo acknowledgement");
}
/**
 * XML boolean의 true/false/1/0을 파싱하고 생략 시 명세 기본값을 사용한다.
 */
bool boolean(const Node *value, bool fallback) {
    if (!value)
        return fallback;
    const auto input = text(value);
    if (input == "true" || input == "1")
        return true;
    if (input == "false" || input == "0")
        return false;
    throw std::invalid_argument("Invalid Stop boolean");
}
} // namespace
/**
 * 이 서버의 위치/상태/추적 시작 지원을 광고한다. MoveAndTrack는 지원한 PTZVector 방식만 표시한다.
 */
std::string ptzCapabilities() {
    return "<tptz:Capabilities EFlip=\"false\" Reverse=\"false\" "
           "GetCompatibleConfigurations=\"true\" MoveStatus=\"true\" StatusPosition=\"true\" "
           "MoveAndTrack=\"PTZVector\"/>";
}
/**
 * 프로파일에서 참조하는 node/공간/기본 Timeout/위치 제한을 만든다. 공유 geometry와 일관된 normalized 범위를 제공한다.
 */
std::string ptzConfiguration(const std::string &tag) {
    return "<" + tag +
           " token=\"ptz-config\"><tt:Name>MG90S Pan Tilt</tt:Name><tt:UseCount>1</tt:UseCount>"
           "<tt:NodeToken>ptz-node</tt:NodeToken><tt:DefaultAbsolutePantTiltPositionSpace>" +
           POSITION_SPACE +
           "</tt:DefaultAbsolutePantTiltPositionSpace><tt:DefaultRelativePanTiltTranslationSpace>" +
           TRANSLATION_SPACE +
           "</"
           "tt:DefaultRelativePanTiltTranslationSpace><tt:DefaultContinuousPanTiltVelocitySpace>" +
           VELOCITY_SPACE +
           "</tt:DefaultContinuousPanTiltVelocitySpace><tt:DefaultPTZTimeout>PT1S</"
           "tt:DefaultPTZTimeout>"
           "<tt:PanTiltLimits><tt:Range><tt:URI>" +
           POSITION_SPACE +
           "</tt:URI><tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange>"
           "<tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange></tt:Range></"
           "tt:PanTiltLimits></" +
           tag + ">";
}
/**
 * 조회는 XML, 이동/추적/Stop은 검증된 내부 RPC로 연결한다. Absolute와 Relative의 서로 다른 정규화 의미를 유지한다.
 */
std::string ptzOperation(const Config &config, const Node &op) {
    if (op.name == "GetServiceCapabilities")
        return ptzCapabilities();
    if (op.name == "GetNodes")
        return node();
    if (op.name == "GetNode") {
        token(op, "NodeToken", "ptz-node");
        return node();
    }
    if (op.name == "GetConfigurations")
        return ptzConfiguration("tptz:PTZConfiguration");
    if (op.name == "GetConfiguration") {
        token(op, "PTZConfigurationToken", "ptz-config");
        return ptzConfiguration("tptz:PTZConfiguration");
    }
    if (op.name == "GetConfigurationOptions") {
        token(op, "ConfigurationToken", "ptz-config");
        return "<tptz:PTZConfigurationOptions><tt:Spaces>" + spaces() +
               "</tt:Spaces>"
               "<tt:PTZTimeout><tt:Min>PT0.1S</tt:Min><tt:Max>PT5S</tt:Max></tt:PTZTimeout></"
               "tptz:PTZConfigurationOptions>";
    }
    token(op, "ProfileToken", "main");
    if (op.name == "GetCompatibleConfigurations")
        return ptzConfiguration("tptz:PTZConfiguration");
    if (op.name == "GetPresets")
        return {}; // 프리셋 0개. Home은 광고하지 않는다.
    if (op.name == "MoveAndStartTracking") {
        for (const auto *unsupported : {"PresetToken", "GeoLocation", "ObjectID", "Speed"})
            if (op.child(PTZ, unsupported))
                throw std::invalid_argument("Tracking supports only current position or PTZVector");
        if (const auto *target = op.child(PTZ, "TargetPosition")) {
            const auto [x, y] = vector(*target, POSITION_SPACE);
            accepted(config, "ONVIF:MOVE_TRACK:" + std::to_string(g::panMid + x * g::panRange / 2) +
                                 "," + std::to_string(g::tiltMid + y * g::tiltRange / 2));
        } else
            accepted(config, "ONVIF:TRACK");
        return {};
    }
    if (op.name == "GetStatus") {
        const auto result = rpc(config, "ONVIF:STATUS");
        int pan = 0, tilt = 0, moving = 0, fault = 0;
        char extra = 0;
        if (std::sscanf(result.c_str(), "STATE:%d,%d,%d,%d%c", &pan, &tilt, &moving, &fault,
                        &extra) != 4 ||
            pan < g::panMin || pan > g::panMax || tilt < g::tiltMin || tilt > g::tiltMax ||
            (moving != 0 && moving != 1) || (fault != 0 && fault != 1))
            throw BackendError("Invalid servo status");
        const auto now = std::time(nullptr);
        std::tm utc{};
        gmtime_r(&now, &utc);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return "<tptz:PTZStatus><tt:Position><tt:PanTilt x=\"" +
               std::to_string((pan - g::panMid) / (g::panRange / 2)) + "\" y=\"" +
               std::to_string((tilt - g::tiltMid) / (g::tiltRange / 2)) + "\" space=\"" +
               POSITION_SPACE + "\"/></tt:Position><tt:MoveStatus><tt:PanTilt>" +
               (fault    ? "UNKNOWN"
                : moving ? "MOVING"
                         : "IDLE") +
               "</tt:PanTilt></tt:MoveStatus>" +
               (fault ? "<tt:Error>GPIO output failed</tt:Error>" : "") + "<tt:UtcTime>" + stamp +
               "</tt:UtcTime></tptz:PTZStatus>";
    }
    if (op.name == "Stop") {
        const bool panTilt = boolean(op.child(PTZ, "PanTilt"), true);
        boolean(op.child(PTZ, "Zoom"), true);
        if (panTilt)
            accepted(config, "ONVIF:STOP");
        return {};
    }
    if (op.name == "AbsoluteMove" || op.name == "RelativeMove" || op.name == "ContinuousMove") {
        if (op.child(PTZ, "Speed"))
            throw std::invalid_argument(
                "Optional Speed is not supported; use default target speed");
        const char *field = op.name == "AbsoluteMove"   ? "Position"
                            : op.name == "RelativeMove" ? "Translation"
                                                        : "Velocity";
        const auto *parent = op.child(PTZ, field);
        if (!parent)
            throw std::invalid_argument(std::string(field) + " required");
        if (parent->child(TYPES, "Zoom"))
            throw std::invalid_argument("Zoom is not supported");
        if (!parent->child(TYPES, "PanTilt"))
            return {}; // 생략된 축의 기존 이동은 변경하지 않는다.
        const auto [x, y] = vector(*parent, op.name == "AbsoluteMove"   ? POSITION_SPACE
                                            : op.name == "RelativeMove" ? TRANSLATION_SPACE
                                                                        : VELOCITY_SPACE);
        std::string command;
        if (op.name == "AbsoluteMove")
            command = "ONVIF:ABS:" + std::to_string(g::panMid + x * g::panRange / 2) + "," +
                      std::to_string(g::tiltMid + y * g::tiltRange / 2);
        else if (op.name == "RelativeMove")
            command = "ONVIF:REL:" + std::to_string(x * g::panRange) + "," +
                      std::to_string(y * g::tiltRange);
        else
            command = "ONVIF:VEL:" + std::to_string(x * g::maxVelocity) + "," +
                      std::to_string(y * g::maxVelocity) + "," +
                      std::to_string(duration(op.child(PTZ, "Timeout")));
        accepted(config, command);
        return {};
    }
    throw std::invalid_argument("PTZ operation not implemented");
}
} // namespace onvif
