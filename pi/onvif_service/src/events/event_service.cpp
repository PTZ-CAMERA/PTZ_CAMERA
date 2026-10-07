/**
 * [학습 안내] ONVIF Events/PullPoint SOAP API의 의미 처리
 * TopicSet/구독 생성/알림 가져오기/동기화/갱신/취소를 broker 동작으로 연결한다.
 * PullMessages에 데이터가 없으면 deferred를 반환한다. 실제 소켓 대기는 network/server가 담당한다.
 * 토픽과 데이터 필드는 프로젝트 확장이며 GetEventProperties로 공개한다. Profile M RTP 메타데이터 스트림을 구현했다는 뜻은 아니다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "events/event_service.h"
#include "events/event_broker.h"
#include <algorithm>
#include <cmath>
#include <regex>
#include <sstream>
#include <stdexcept>
namespace onvif {
namespace {
/**
 * 선택 Node의 공백 정리된 text를 읽는다. Node가 없는 경우의 의미는 operation별로 판단한다.
 */
std::string text(const Node *node) {
    return node ? trim(node->text) : std::string{};
}
/** 구독 lease와 Pull Timeout의 서로 다른 상한을 매개변수로 검사한다. 허용한 상대 PT 형식을 ms로 반환한다. */
int duration(const Node *node, int fallback, int minMs, int maxMs) {
    if (!node)
        return fallback;
    const auto value = text(node);
    std::smatch match;
    static const std::regex pattern(
        R"(^PT(?:([0-9]+)H)?(?:([0-9]+)M)?(?:([0-9]+(?:\.[0-9]+)?)S)?$)");
    if (value.size() > 64 || !std::regex_match(value, match, pattern))
        throw std::invalid_argument("Use relative PT duration");
    double seconds = 0;
    bool any = false;
    for (int i = 1; i <= 3; ++i)
        if (match[i].matched) {
            seconds += std::stod(match[i].str()) * (i == 1 ? 3600 : i == 2 ? 60 : 1);
            any = true;
        }
    if (!any || !std::isfinite(seconds) || seconds * 1000 < minMs || seconds * 1000 > maxMs)
        throw std::invalid_argument("Duration outside supported range");
    return std::lround(seconds * 1000);
}
/**
 * GetEventProperties의 MessageDescription을 만든다. SimpleItem 이름/타입을 클라이언트에 공개한다.
 */
std::string desc(const std::vector<std::pair<std::string, std::string>> &items) {
    std::string result =
        "<tt:MessageDescription IsProperty=\"true\"><tt:Source><tt:SimpleItemDescription "
        "Name=\"ProfileToken\" Type=\"tt:ReferenceToken\"/></tt:Source><tt:Data>";
    for (const auto &[name, type] : items)
        result += "<tt:SimpleItemDescription Name=\"" + name + "\" Type=\"xs:" + type + "\"/>";
    return result + "<tt:SimpleItemDescription Name=\"Available\" "
                    "Type=\"xs:boolean\"/></tt:Data></tt:MessageDescription>";
}
/**
 * Concrete/ConcreteSet의 공개 QName 토픽 선택을 검증한다. prefix alias는 실제 URI로 해석하며 미지원 ContentFilter는 거부한다.
 */
std::vector<std::string> filters(const Node *filter) {
    if (!filter || filter->children.empty())
        return {};
    const auto *expression = filter->child(WSNT, "TopicExpression");
    if (!expression || filter->children.size() != 1)
        throw std::invalid_argument("Only TopicExpression filter supported");
    const auto dialect = expression->attributes.find("Dialect");
    if (dialect == expression->attributes.end() ||
        (dialect->second != "http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet" &&
         dialect->second != "http://docs.oasis-open.org/wsn/t-1/TopicExpression/Concrete"))
        throw std::invalid_argument("Unsupported topic dialect");
    std::vector<std::string> result;
    std::istringstream terms(text(expression));
    std::string term;
    while (std::getline(terms, term, '|')) {
        term = trim(term);
        const auto colon = term.find(':');
        if (colon == std::string::npos)
            throw std::invalid_argument("Qualified topic required");
        const auto ns = expression->prefixes.find(term.substr(0, colon));
        const auto topic = term.substr(colon + 1);
        if (ns == expression->prefixes.end() || ns->second != TOPICS ||
            (topic != "Camera/Publishing" && topic != "Tracking/State" &&
             topic != "Analytics/PersonDetection"))
            throw std::invalid_argument("Unknown topic");
        result.push_back(topic);
    }
    if (result.empty())
        throw std::invalid_argument("Empty topic filter");
    return result;
}
} // namespace
/**
 * Events와 SubscriptionManager 요청을 구독/알림 상태로 연결한다. endpoint 종류와 operation namespace를 함께 확인한다.
 */
Response eventOperation(const Config &config, const std::string &path, const Node &op) {
    if (!config.events)
        throw std::runtime_error("Event service not initialized");
    const auto base = "/onvif/pullpoint/";
    const bool pullpoint = path.rfind(base, 0) == 0;
    const auto token = pullpoint ? path.substr(std::string(base).size()) : std::string{};
    auto &broker = *config.events;
    std::string result;
    if (!pullpoint && op.ns == EVENTS && op.name == "GetServiceCapabilities")
        return {200, eventCapabilities()};
    if (!pullpoint && op.ns == EVENTS && op.name == "GetEventProperties") {
        result = "<tev:TopicNamespaceLocation>" + escape(config.baseUrl() + "/onvif/topics.xml") +
                 "</tev:TopicNamespaceLocation>"
                 "<wsnt:FixedTopicSet>true</wsnt:FixedTopicSet><wstop:TopicSet>"
                 "<pc:Camera><Publishing wstop:topic=\"true\">" +
                 desc({{"Connected", "boolean"}}) +
                 "</Publishing></pc:Camera>"
                 "<pc:Tracking><State wstop:topic=\"true\">" +
                 desc({{"Enabled", "boolean"}}) +
                 "</State></pc:Tracking>"
                 "<pc:Analytics><PersonDetection wstop:topic=\"true\">" +
                 desc({{"Detected", "boolean"},
                       {"Class", "string"},
                       {"Confidence", "float"},
                       {"X", "int"},
                       {"Y", "int"},
                       {"ErrorX", "int"},
                       {"ErrorY", "int"},
                       {"PanAngle", "int"},
                       {"TiltAngle", "int"},
                       {"ImageWidth", "int"},
                       {"ImageHeight", "int"},
                       {"SchemaVersion", "int"}, {"SelectedTarget", "boolean"},
                       {"BoxX", "int"}, {"BoxY", "int"}, {"BoxWidth", "int"}, {"BoxHeight", "int"},
                       {"FrameId", "long"}, {"FramePtsNs", "long"}, {"CaptureUnixMs", "long"},
                       {"AnalysisUnixMs", "long"}, {"StreamEpoch", "long"}, {"Generation", "long"},
                       {"CaptureTimeSource", "string"}}) +
                 "</PersonDetection></pc:Analytics></wstop:TopicSet>"
                 "<wsnt:TopicExpressionDialect>http://docs.oasis-open.org/wsn/t-1/TopicExpression/"
                 "Concrete</wsnt:TopicExpressionDialect>"
                 "<wsnt:TopicExpressionDialect>http://www.onvif.org/ver10/tev/topicExpression/"
                 "ConcreteSet</wsnt:TopicExpressionDialect>"
                 "<tev:MessageContentFilterDialect/><tev:MessageContentSchemaLocation>http://"
                 "www.onvif.org/ver10/schema/onvif.xsd</tev:MessageContentSchemaLocation>";
    } else if (!pullpoint && op.ns == EVENTS && op.name == "CreatePullPointSubscription") {
        if (op.child(EVENTS, "SubscriptionPolicy"))
            throw std::invalid_argument("SubscriptionPolicy unsupported");
        const auto lifetime =
            duration(op.child(EVENTS, "InitialTerminationTime"), 60000, 1000, 3600000) / 1000;
        const auto id = broker.create(lifetime, filters(op.child(EVENTS, "Filter")));
        result = "<tev:SubscriptionReference><wsa:Address>" + escape(config.baseUrl() + base + id) +
                 "</wsa:Address></tev:SubscriptionReference>"
                 "<wsnt:CurrentTime>" +
                 eventUtc() + "</wsnt:CurrentTime><wsnt:TerminationTime>" + broker.expiration(id) +
                 "</wsnt:TerminationTime>";
    } else if (pullpoint && op.ns == EVENTS && op.name == "PullMessages") {
        if (!op.child(EVENTS, "Timeout") || !op.child(EVENTS, "MessageLimit"))
            throw std::invalid_argument("Timeout and MessageLimit required");
        const auto timeout = duration(op.child(EVENTS, "Timeout"), 1000, 0, 10000);
        const auto value = text(op.child(EVENTS, "MessageLimit"));
        std::size_t at = 0;
        const int limit = std::stoi(value, &at);
        if (at != value.size() || limit < 1 || limit > 64)
            throw std::invalid_argument("MessageLimit must be 1..64");
        const auto response =
            broker.pull(token, config.requestId ? config.requestId : 1, timeout, limit);
        if (response.pending)
            return {200, {}, true};
        result = "<tev:CurrentTime>" + eventUtc() + "</tev:CurrentTime><tev:TerminationTime>" +
                 response.termination + "</tev:TerminationTime>";
        for (const auto &message : response.messages)
            result += eventNotification(message);
    } else if (pullpoint && op.ns == EVENTS && op.name == "SetSynchronizationPoint") {
        broker.synchronize(token);
    } else if (pullpoint && op.ns == WSNT && op.name == "Renew") {
        const auto lifetime =
            duration(op.child(WSNT, "TerminationTime"), 60000, 1000, 3600000) / 1000;
        result = "<wsnt:TerminationTime>" + broker.renew(token, lifetime) +
                 "</wsnt:TerminationTime><wsnt:CurrentTime>" + eventUtc() + "</wsnt:CurrentTime>";
    } else if (pullpoint && op.ns == WSNT && op.name == "Unsubscribe") {
        broker.remove(token);
    } else
        throw std::invalid_argument("Event operation/endpoint unsupported");
    return {200, result};
}
} // namespace onvif
