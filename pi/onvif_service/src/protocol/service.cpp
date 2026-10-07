/**
 * [학습 안내] SOAP Envelope와 Device/Media/PTZ/Events 서비스 분배
 * URL path와 namespace URI를 함께 검사해 정확한 서비스로 보내고 응답 Envelope를 만든다.
 * Device/Media 대부분은 정적 설정 조회다. PTZ는 내부 서보 IPC, Events는 같은 루프의 broker로 연결한다.
 * 잘못된 입력은 Sender Fault, 서보 통신 실패는 Receiver Fault다. API 일부 구현이며 전체 적합성 인증은 아니다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "protocol/service.h"
#include "events/event_service.h"
#include "events/event_broker.h"
#include "ptz/ptz_service.h"
#include "ptz/servo_bridge.h"
#include "xml/xml.h"
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <stdexcept>
namespace onvif {
/**
 * 현재 광고 IP/HTTP 포트의 공통 base URL을 만든다. 실제 요청 시 자동 IP 모드면 인터페이스 주소를 다시 읽는다.
 */
std::string Config::baseUrl() const {
    return "http://" + ip + ":" + std::to_string(port);
}
/**
 * Events 서비스의 XAddr를 만든다. 생성된 PullPoint는 이 HTTP 서버의 별도 구독 path를 사용한다.
 */
std::string Config::eventsUrl() const {
    return baseUrl() + "/onvif/events_service";
}
/**
 * 장치 관리 bootstrap XAddr를 만든다. 클라이언트는 여기서 서비스 목록을 조회한다.
 */
std::string Config::deviceUrl() const {
    return "http://" + ip + ":" + std::to_string(port) + "/onvif/device_service";
}
/**
 * Media 프로파일/스트림 URI 조회 endpoint를 만든다.
 */
std::string Config::mediaUrl() const {
    return "http://" + ip + ":" + std::to_string(port) + "/onvif/media_service";
}
/**
 * 표준 이동/정지/추적 요청 endpoint를 만든다.
 */
std::string Config::ptzUrl() const {
    return "http://" + ip + ":" + std::to_string(port) + "/onvif/ptz_service";
}
/**
 * 기존 MediaMTX RTSP URI를 반환한다. 비밀번호를 URI에 넣거나 조회만으로 카메라를 열지 않는다.
 */
std::string Config::streamUrl() const {
    return "rtsp://" + ip + ":" + std::to_string(rtspPort) + "/cam";
}
namespace {
constexpr const char *ADDRESSING = "http://www.w3.org/2005/08/addressing";
constexpr const char *OLD_ADDRESSING = "http://schemas.xmlsoap.org/ws/2004/08/addressing";
const std::string scopes =
    "onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/name/PiPTZ";
std::atomic<unsigned> serial{1};
const auto instance = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
/**
 * 있으면 Node text를 trim하고 없으면 빈 문자열을 반환한다. 상위 검증이 필수/선택 필드를 구별한다.
 */
std::string content(const Node *node) {
    return node ? trim(node->text) : std::string{};
}
/**
 * 서비스 응답 본문에 SOAP/타입 namespace를 붙인다. 요청 MessageID가 있으면 RelatesTo/Action도 연결한다.
 */
std::string envelope(const std::string &body, const Node *request = nullptr,
                     const std::string &action = {}) {
    std::string header;
    if (request)
        if (auto *h = request->child(SOAP, "Header")) {
            const Node *id = h->child(ADDRESSING, "MessageID");
            std::string ns = ADDRESSING;
            if (!id) {
                id = h->child(OLD_ADDRESSING, "MessageID");
                ns = OLD_ADDRESSING;
            }
            if (id)
                header = "<a:Action s:mustUnderstand=\"true\" xmlns:a=\"" + ns + "\">" +
                         escape(action) +
                         "</a:Action>"
                         "<a:RelatesTo xmlns:a=\"" +
                         ns + "\">" + escape(content(id)) + "</a:RelatesTo>";
        }
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><s:Envelope xmlns:s=\"" + std::string(SOAP) +
           "\" xmlns:tds=\"" + DEVICE + "\" xmlns:trt=\"" + MEDIA + "\" xmlns:tt=\"" + TYPES +
           "\" xmlns:tptz=\"" + std::string(PTZ) + "\" xmlns:tev=\"" + std::string(EVENTS) +
           "\" xmlns:wsnt=\"" + WSNT +
           "\" xmlns:wsa=\"http://www.w3.org/2005/08/addressing\" "
           "xmlns:wstop=\"http://docs.oasis-open.org/wsn/t-1\" "
           "xmlns:pc=\"" +
           TOPICS +
           "\" xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
           "xmlns:ter=\"http://www.onvif.org/ver10/error\"><s:Header>" +
           header + "</s:Header><s:Body>" + body + "</s:Body></s:Envelope>";
}
/**
 * 입력 문제/서버 문제에 맞는 SOAP Fault를 구성한다. 이유 문자열은 XML escape해 반환한다.
 */
Response fault(const std::string &reason, const std::string &code = "ter:InvalidArgVal",
               bool receiver = false) {
    return {500, envelope("<s:Fault><s:Code><s:Value>" +
                          std::string(receiver ? "s:Receiver" : "s:Sender") +
                          "</s:Value><s:Subcode><s:Value>" + code +
                          "</s:Value></s:Subcode></s:Code><s:Reason><s:Text xml:lang=\"en\">" +
                          escape(reason) + "</s:Text></s:Reason></s:Fault>")};
}
/**
 * 현재 제공하는 Media 기능만 광고한다. 없는 Snapshot/OSD 등을 true로 표시하지 않는다.
 */
std::string mediaCapabilities() {
    return "<trt:Capabilities SnapshotUri=\"false\" Rotation=\"false\" VideoSourceMode=\"false\" "
           "OSD=\"false\">"
           "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"1\"/>"
           "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"false\" "
           "RTP_RTSP_TCP=\"true\"/>"
           "</trt:Capabilities>";
}
/**
 * 장치 보안/시스템 지원 범위를 광고한다. 실제 HTTP Digest 설정과 UsernameToken 미지원 상태를 구분한다.
 */
std::string deviceCapabilities(bool authRequired) {
    return "<tds:Capabilities><tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" "
           "IPVersion6=\"false\" DynDNS=\"false\"/>"
           "<tds:Security TLS1.0=\"false\" TLS1.1=\"false\" TLS1.2=\"false\" "
           "UsernameToken=\"false\" HttpDigest=\"" +
           std::string(authRequired ? "true" : "false") +
           "\"/>"
           "<tds:System DiscoveryResolve=\"false\" DiscoveryBye=\"false\" "
           "RemoteDiscovery=\"false\" SystemBackup=\"false\" "
           "SystemLogging=\"false\" FirmwareUpgrade=\"false\"/></tds:Capabilities>";
}
/**
 * 영상 설정과 PTZConfiguration이 연결된 고정 프로파일 XML을 만든다. 동적 카메라 건강 검사의 결과는 아니다.
 */
std::string profile(const char *name) {
    // 인코더 설정은 기존 C++ 송출기의 1280x720/30fps H.264 설정을 설명한다.
    const std::string tag = std::string("trt:") + name;
    return "<" + tag +
           " token=\"main\" fixed=\"true\"><tt:Name>Pi PTZ 720p</tt:Name>"
           "<tt:VideoSourceConfiguration token=\"source-config\"><tt:Name>IMX219</tt:Name>"
           "<tt:UseCount>1</tt:UseCount><tt:SourceToken>source0</tt:SourceToken>"
           "<tt:Bounds x=\"0\" y=\"0\" width=\"1280\" "
           "height=\"720\"/></tt:VideoSourceConfiguration>"
           "<tt:VideoEncoderConfiguration token=\"encoder-config\"><tt:Name>H264 720p</tt:Name>"
           "<tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>"
           "<tt:Resolution><tt:Width>1280</tt:Width><tt:Height>720</tt:Height></tt:Resolution>"
           "<tt:Quality>5</tt:Quality><tt:RateControl><tt:FrameRateLimit>30</tt:FrameRateLimit>"
           "<tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>2500</tt:BitrateLimit></"
           "tt:RateControl>"
           "<tt:H264><tt:GovLength>30</tt:GovLength><tt:H264Profile>Baseline</tt:H264Profile></"
           "tt:H264>"
           "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</"
           "tt:IPv4Address>"
           "</tt:Address><tt:Port>0</tt:Port><tt:TTL>1</tt:TTL><tt:AutoStart>false</tt:AutoStart></"
           "tt:Multicast>"
           "<tt:SessionTimeout>PT60S</tt:SessionTimeout></tt:VideoEncoderConfiguration>" +
           ptzConfiguration("tt:PTZConfiguration") + "</" + tag + ">";
}
} // namespace
/**
 * Envelope/Body/namespace/path를 검사해 서비스를 분배한다. deferred Events는 즉시 빈 대기 결과로 올리고 완성된 응답만 감싼다.
 */
Response soapResponse(const Config &config, const std::string &path, const std::string &xml) {
    // endpoint와 XML namespace를 함께 검사해 Device/Media 명령을 구분한다.
    try {
        auto document = parseXml(xml);
        if (document->ns != SOAP || document->name != "Envelope")
            return fault("SOAP 1.2 Envelope required");
        const auto *body = document->child(SOAP, "Body");
        if (!body || body->children.size() != 1)
            return fault("Exactly one SOAP operation required");
        const Node &op = *body->children.front();
        const bool device = path == "/onvif/device_service" && op.ns == DEVICE;
        const bool media = path == "/onvif/media_service" && op.ns == MEDIA;
        const bool ptz = path == "/onvif/ptz_service" && op.ns == PTZ;
        const bool events =
            (path == "/onvif/events_service" || path.rfind("/onvif/pullpoint/", 0) == 0) &&
            (op.ns == EVENTS || op.ns == WSNT);
        if (!device && !media && !ptz && !events)
            return fault("Unknown endpoint or namespace", "ter:ActionNotSupported");
        if (events) {
            auto response = eventOperation(config, path, op);
            if (response.deferred)
                return response;
            const std::string prefix = op.ns == WSNT ? "wsnt:" : "tev:";
            const auto tag = prefix + op.name + "Response";
            const auto action =
                op.ns == WSNT
                    ? std::string("http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/") +
                          op.name + "Response"
                    : std::string(EVENTS) +
                          (op.name == "PullMessages" || op.name == "SetSynchronizationPoint"
                               ? "/PullPointSubscription/"
                               : "/EventPortType/") +
                          op.name + "Response";
            response.xml =
                envelope("<" + tag + ">" + response.xml + "</" + tag + ">", document.get(), action);
            return response;
        }
        std::string result;
        const std::string prefix = device ? "tds:" : ptz ? "tptz:" : "trt:";
        if (ptz)
            result = ptzOperation(config, op);
        else if (device && op.name == "GetDeviceInformation")
            result = "<tds:Manufacturer>PTZ-CAMERA</tds:Manufacturer><tds:Model>Raspberry Pi "
                     "PTZ</tds:Model>"
                     "<tds:FirmwareVersion>0.2</tds:FirmwareVersion><tds:SerialNumber>" +
                     escape(config.uuid) +
                     "</tds:SerialNumber><tds:HardwareId>RaspberryPi4</tds:HardwareId>";
        else if (op.name == "GetServiceCapabilities")
            result = device ? deviceCapabilities(config.authRequired) : mediaCapabilities();
        else if (device && op.name == "GetServices") {
            const auto include = content(op.child(DEVICE, "IncludeCapability"));
            if (include != "false" && include != "0" && include != "true" && include != "1")
                return fault("IncludeCapability boolean required");
            for (const auto &entry :
                 {std::pair<std::string, std::string>{DEVICE, config.deviceUrl()},
                  {MEDIA, config.mediaUrl()},
                  {PTZ, config.ptzUrl()},
                  {EVENTS, config.eventsUrl()}})
                result +=
                    "<tds:Service><tds:Namespace>" + entry.first + "</tds:Namespace><tds:XAddr>" +
                    escape(entry.second) + "</tds:XAddr>" +
                    ((include == "true" || include == "1")
                         ? "<tds:Capabilities>" +
                               (entry.first == DEVICE   ? deviceCapabilities(config.authRequired)
                                : entry.first == PTZ    ? ptzCapabilities()
                                : entry.first == EVENTS ? eventCapabilities()
                                                        : mediaCapabilities()) +
                               "</tds:Capabilities>"
                         : std::string{}) +
                    "<tds:Version><tt:Major>2</tt:Major><tt:Minor>0</tt:Minor></tds:Version></"
                    "tds:Service>";
        } else if (device && op.name == "GetCapabilities") {
            const auto category = content(op.child(DEVICE, "Category"));
            if (!category.empty() && category != "All" && category != "Media" &&
                category != "PTZ" && category != "Events")
                return fault("Capability category unsupported", "ter:ActionNotSupported");
            result = "<tds:Capabilities>";
            if (category.empty() || category == "All" || category == "Media")
                result +=
                    "<tt:Media><tt:XAddr>" + escape(config.mediaUrl()) +
                    "</tt:XAddr><tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast>"
                    "<tt:RTP_TCP>false</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>"
                    "</tt:StreamingCapabilities></tt:Media>";
            if (category.empty() || category == "All" || category == "PTZ")
                result += "<tt:PTZ><tt:XAddr>" + escape(config.ptzUrl()) + "</tt:XAddr></tt:PTZ>";
            if (category.empty() || category == "All" || category == "Events")
                result += "<tt:Events><tt:XAddr>" + escape(config.eventsUrl()) +
                          "</tt:XAddr><tt:WSSubscriptionPolicySupport>false</"
                          "tt:WSSubscriptionPolicySupport>"
                          "<tt:WSPullPointSupport>true</"
                          "tt:WSPullPointSupport><tt:WSPausableSubscriptionManagerInterfaceSupport>"
                          "false</tt:WSPausableSubscriptionManagerInterfaceSupport></tt:Events>";
            result += "</tds:Capabilities>";
        } else if (device && op.name == "GetScopes") {
            std::istringstream in(scopes);
            std::string scope;
            while (in >> scope)
                result += "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>" + scope +
                          "</tt:ScopeItem></tds:Scopes>";
        } else if (device && op.name == "GetSystemDateAndTime") {
            const auto now = std::time(nullptr);
            std::tm utc{};
            gmtime_r(&now, &utc);
            result =
                "<tds:SystemDateAndTime><tt:DateTimeType>Manual</"
                "tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings>"
                "<tt:TimeZone><tt:TZ>UTC0</tt:TZ></tt:TimeZone><tt:UTCDateTime><tt:Time><tt:Hour>" +
                std::to_string(utc.tm_hour) + "</tt:Hour><tt:Minute>" + std::to_string(utc.tm_min) +
                "</tt:Minute><tt:Second>" + std::to_string(utc.tm_sec) +
                "</tt:Second></tt:Time><tt:Date><tt:Year>" + std::to_string(utc.tm_year + 1900) +
                "</tt:Year><tt:Month>" + std::to_string(utc.tm_mon + 1) + "</tt:Month><tt:Day>" +
                std::to_string(utc.tm_mday) +
                "</tt:Day></tt:Date></tt:UTCDateTime></tds:SystemDateAndTime>";
        } else if (media && op.name == "GetProfiles")
            result = profile("Profiles");
        else if (media && (op.name == "GetProfile" || op.name == "GetStreamUri")) {
            if (content(op.child(MEDIA, "ProfileToken")) != "main")
                return fault("Unknown profile token");
            if (op.name == "GetProfile")
                result = profile("Profile");
            else {
                const Node *setup = op.child(MEDIA, "StreamSetup");
                const Node *transport = setup ? setup->child(TYPES, "Transport") : nullptr;
                const auto protocol =
                    content(transport ? transport->child(TYPES, "Protocol") : nullptr);
                if (!setup || content(setup->child(TYPES, "Stream")) != "RTP_unicast" ||
                    (protocol != "RTSP" && protocol != "UDP") ||
                    (transport && transport->child(TYPES, "Tunnel")))
                    return fault("Only RTP_unicast over RTSP or UDP supported");
                // URL 조회는 카메라를 시작하지 않는다. 실제 연결은 MediaMTX가 받는다.
                result = "<trt:MediaUri><tt:Uri>" + escape(config.streamUrl()) +
                         "</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
                         "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT0S</"
                         "tt:Timeout></trt:MediaUri>";
            }
        } else
            return fault("Operation not implemented", "ter:ActionNotSupported");
        const std::string tag = prefix + op.name + "Response";
        return {200, envelope("<" + tag + ">" + result + "</" + tag + ">", document.get(),
                              op.ns + "/" + op.name + "Response")};
    } catch (const BackendError &e) {
        return fault(e.what(), "ter:Action", true);
    } catch (const std::exception &e) {
        return fault(e.what());
    }
}
/**
 * Probe의 QName/Scope/MessageID를 검증하고 요청과 연관된 ProbeMatch를 만든다. SOAP HTTP 제어와 별도의 UDP 검색 단계다.
 */
std::string probeResponse(const Config &config, const std::string &xml) {
    try {
        auto doc = parseXml(xml);
        const auto *body = doc->child(SOAP, "Body");
        const auto *header = doc->child(SOAP, "Header");
        if (doc->ns != SOAP || doc->name != "Envelope" || !body || !header)
            return {};
        const auto *probe = body->child(DISCOVERY, "Probe");
        if (!probe)
            return {};
        const auto *id = header->child(OLD_ADDRESSING, "MessageID");
        std::string addressNs = OLD_ADDRESSING;
        if (!id) {
            id = header->child(ADDRESSING, "MessageID");
            addressNs = ADDRESSING;
        }
        if (!id || content(id).empty())
            return {};
        // QName의 접두어 이름 대신 접두어가 가리키는 namespace로 종류를 판별한다.
        if (const auto *types = probe->child(DISCOVERY, "Types")) {
            std::istringstream input(content(types));
            std::string type;
            while (input >> type) {
                const auto colon = type.find(':');
                const auto prefix = colon == std::string::npos ? "" : type.substr(0, colon);
                const auto local = colon == std::string::npos ? type : type.substr(colon + 1);
                const auto ns = types->prefixes.find(prefix);
                if (ns == types->prefixes.end() ||
                    !((ns->second == "http://www.onvif.org/ver10/network/wsdl" &&
                       local == "NetworkVideoTransmitter") ||
                      (ns->second == DEVICE && local == "Device")))
                    return {};
            }
        }
        if (const auto *requested = probe->child(DISCOVERY, "Scopes")) {
            const auto rule = requested->attributes.find("MatchBy");
            if (rule != requested->attributes.end() &&
                rule->second != "http://schemas.xmlsoap.org/ws/2005/04/discovery/rfc3986")
                return {};
            std::istringstream input(content(requested));
            std::string scope;
            while (input >> scope)
                if (scope != "onvif://www.onvif.org/type/video_encoder" &&
                    scope != "onvif://www.onvif.org/name/PiPTZ")
                    return {};
        }
        // 요청 MessageID는 RelatesTo로 돌려주고 응답에는 새 MessageID를 부여한다.
        const auto number = serial++;
        std::ifstream uuidFile("/proc/sys/kernel/random/uuid");
        std::string messageUuid;
        uuidFile >> messageUuid;
        if (messageUuid.empty())
            return {};
        return "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"" + std::string(SOAP) +
               "\" xmlns:a=\"" + addressNs + "\" xmlns:d=\"" + DISCOVERY +
               "\" xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\" xmlns:tds=\"" +
               std::string(DEVICE) +
               "\">"
               "<s:Header><a:Action>" +
               DISCOVERY + "/ProbeMatches</a:Action><a:MessageID>urn:uuid:" + messageUuid +
               "</a:MessageID><a:RelatesTo>" + escape(content(id)) +
               "</a:RelatesTo>"
               "<a:To>" +
               (addressNs == OLD_ADDRESSING ? std::string(OLD_ADDRESSING) + "/role/anonymous"
                                            : std::string(ADDRESSING) + "/anonymous") +
               "</a:To><d:AppSequence InstanceId=\"" + std::to_string(instance) +
               "\" MessageNumber=\"" + std::to_string(number) +
               "\"/></"
               "s:Header><s:Body><d:ProbeMatches><d:ProbeMatch><a:EndpointReference><a:Address>urn:"
               "uuid:" +
               escape(config.uuid) +
               "</a:Address></a:EndpointReference><d:Types>tds:Device "
               "dn:NetworkVideoTransmitter</d:Types><d:Scopes>" +
               scopes + "</d:Scopes><d:XAddrs>" + escape(config.deviceUrl()) +
               "</d:XAddrs><d:MetadataVersion>1</d:MetadataVersion></d:ProbeMatch></"
               "d:ProbeMatches></s:Body></s:Envelope>";
    } catch (...) {
        return {};
    }
}
} // namespace onvif
