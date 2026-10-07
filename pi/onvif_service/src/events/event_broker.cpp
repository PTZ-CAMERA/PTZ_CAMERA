/**
 * [학습 안내] 구독별 ONVIF property 알림 저장소
 * 카메라 송출/추적/탐지의 최신 상태와 구독의 lease, 대기 요청 소유자, 전달 큐를 관리한다.
 * 같은 ONVIF poll 루프에서만 접근하므로 별도 mutex나 broker 작업 스레드를 만들지 않는다.
 * Changed는 토픽별 최신 값으로 합친다. 영상 좌표의 오래된 backlog를 재생하지 않는다. 이벤트 시간은 센서 노출 시각이 아니다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "events/event_broker.h"
#include "xml/xml.h"
#include "metadata/detection_metadata.h"
#include <openssl/rand.h>
#include <nlohmann/json.hpp>
#include "config/app_config.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
namespace onvif {
namespace {
/**
 * Data/Source에 넣는 SimpleItem의 이름/값을 XML escape한다.
 */
std::string item(const std::string &name, const std::string &value) {
    return "<tt:SimpleItem Name=\"" + escape(name) + "\" Value=\"" + escape(value) + "\"/>";
}
/**
 * boolean 상태를 XML의 true/false 텍스트로 표현한다.
 */
std::string bit(bool value) {
    return value ? "true" : "false";
}
/**
 * 구독이 토픽을 선택했는지 검사한다. 비어 있는 필터는 현재 지원 토픽 전체다.
 */
bool selected(const std::vector<std::string> &filter, const std::string &topic) {
    return filter.empty() || std::find(filter.begin(), filter.end(), topic) != filter.end();
}
/**
 * Initialized snapshot을 유지하면서 같은 토픽의 Changed를 최신 값으로 합친다. 큐 상한도 적용한다.
 */
void enqueue(std::deque<Event> &queue, const Event &event) {
    // property 최신 값만 남겨 느린 UI가 오래된 좌표를 뒤늦게 재생하지 않게 한다.
    if (event.operation == "Changed") {
        for (auto it = queue.begin(); it != queue.end();) {
            if (it->topic == event.topic && it->operation == "Changed")
                it = queue.erase(it);
            else
                ++it;
        }
    }
    if (queue.size() >= 64)
        queue.pop_front();
    queue.push_back(event);
}
} // namespace
/**
 * 현재 이벤트 생성 시각을 UTC 문자열로 만든다. 센서 노출/추론 시작 시각과 동일하다고 해석하지 않는다.
 */
std::string eventUtc(int offsetSeconds) {
    const auto now = std::chrono::system_clock::now() + std::chrono::seconds(offsetSeconds);
    const auto stamp = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;
    std::tm utc{};
    gmtime_r(&stamp, &utc);
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &utc);
    std::ostringstream result;
    result << text << '.' << std::setfill('0') << std::setw(3) << milliseconds << 'Z';
    return result.str();
}

/**
 * lease가 지난 구독을 제거한다. 무한 구독/큐 누적으로 메모리를 차지하지 않게 한다.
 */
void EventBroker::cleanup() {
    const auto now = Clock::now();
    for (auto it = subscriptions_.begin(); it != subscriptions_.end();)
        if (it->second.expires <= now)
            it = subscriptions_.erase(it);
        else
            ++it;
}
/**
 * 만료 정리 후 token을 찾는다. 존재하지 않거나 만료된 구독은 명확히 실패한다.
 */
EventBroker::Subscription &EventBroker::subscription(const std::string &token) {
    cleanup();
    const auto it = subscriptions_.find(token);
    if (it == subscriptions_.end())
        throw std::invalid_argument("Subscription not found or expired");
    return it->second;
}
/**
 * 현재 세 property를 Initialized로 구독 큐에 넣는다. Topic 필터에 맞는 것만 전달한다.
 */
void EventBroker::snapshot(Subscription &sub) {
    const std::vector<Event> events{
        {"Camera/Publishing", eventUtc(), "Initialized",
         item("Connected", bit(camera_))+item("CameraState",cameraState_)+item("StreamState",streamState_)+item("Available", bit(available_))},
        {"Tracking/State", eventUtc(), "Initialized",
         item("Enabled", bit(tracking_))+item("AutoAllowed",bit(autoAllowed_))+item("TargetState",targetState_)+item("Available", bit(available_))},
        {"Analytics/PersonDetection", eventUtc(), "Initialized",
         detection_ + item("Available", bit(available_))},
        {"PTZ/State",eventUtc(),"Initialized",runtimeData_+item("Available",bit(available_))}};
    for (const auto &event : events)
        if (selected(sub.filter, event.topic))
            enqueue(sub.queue, event);
}
/**
 * 난수 구독 token, lease, 필터를 만들고 초기 상태를 넣는다. 최대 구독 수를 넘기면 생성하지 않는다.
 */
std::string EventBroker::create(int leaseSeconds, const std::vector<std::string> &filter) {
    cleanup();
    if (subscriptions_.size() >= 16)
        throw std::invalid_argument("PullPoint limit reached");
    unsigned char random[16];
    if (RAND_bytes(random, sizeof(random)) != 1)
        throw std::runtime_error("Cannot generate subscription token");
    std::ostringstream token;
    token << std::hex << std::setfill('0');
    for (auto byte : random)
        token << std::setw(2) << unsigned(byte);
    auto &sub = subscriptions_[token.str()];
    sub.expires = Clock::now() + std::chrono::seconds(leaseSeconds);
    sub.filter = filter;
    snapshot(sub);
    return token.str();
}
/**
 * monotonic lease 잔여 시간으로 응답에 필요한 UTC TerminationTime을 만든다.
 */
std::string EventBroker::expiration(const std::string &token) {
    const auto remain =
        std::chrono::duration_cast<std::chrono::seconds>(subscription(token).expires - Clock::now())
            .count();
    return eventUtc(static_cast<int>(std::max<long long>(0, remain)));
}
/**
 * 구독의 단일 대기 소유자와 deadline을 유지한다. 데이터/만료 준비 전에는 블로킹 없이 pending을 반환한다.
 */
PullResult EventBroker::pull(const std::string &token, unsigned owner, int timeoutMs, int limit) {
    auto &sub = subscription(token);
    if (sub.owner && sub.owner != owner)
        throw std::invalid_argument("Another PullMessages is pending for this subscription");
    if (!sub.owner) {
        sub.owner = owner;
        sub.waitUntil = Clock::now() + std::chrono::milliseconds(timeoutMs);
    }
    PullResult result;
    result.termination = expiration(token);
    if (sub.queue.empty() && Clock::now() < sub.waitUntil) {
        result.pending = true;
        return result;
    }
    while (!sub.queue.empty() && int(result.messages.size()) < limit) {
        result.messages.push_back(std::move(sub.queue.front()));
        sub.queue.pop_front();
    }
    sub.owner = 0;
    return result;
}
/**
 * 유효 구독의 lease를 갱신하고 새 TerminationTime을 반환한다.
 */
std::string EventBroker::renew(const std::string &token, int seconds) {
    subscription(token).expires = Clock::now() + std::chrono::seconds(seconds);
    return expiration(token);
}
/**
 * 유효 token의 구독을 삭제해 이후 Pull/Renew에서 다시 사용할 수 없게 한다.
 */
void EventBroker::remove(const std::string &token) {
    subscription(token);
    subscriptions_.erase(token);
}
/**
 * 기존 큐 대신 현재 Initialized snapshot을 넣어 클라이언트 상태를 다시 맞춘다.
 */
void EventBroker::synchronize(const std::string &token) {
    auto &sub = subscription(token);
    sub.queue.clear();
    snapshot(sub);
}
/**
 * HTTP 연결이 끊긴 pending 요청의 소유권을 해제한다. 구독 자체는 lease가 만료될 때까지 유지한다.
 */
void EventBroker::cancelOwner(unsigned owner) {
    for (auto &[token, sub] : subscriptions_)
        if (sub.owner == owner)
            sub.owner = 0;
}
/**
 * 새 Changed property를 선택한 각 구독에 fan-out한다. 큐 정책은 enqueue 한 곳에서 적용한다.
 */
void EventBroker::publish(const std::string &topic, const std::string &data) {
    cleanup();
    const Event event{topic, eventUtc(), "Changed", data + item("Available", bit(available_))};
    for (auto &[token, sub] : subscriptions_)
        if (selected(sub.filter, topic))
            enqueue(sub.queue, event);
}
/**
 * backend 관측 연결 가용성을 바꾸고 상태를 갱신한다. Available=false일 때 오래된 탐지 위치를 유지하지 않는다.
 */
void EventBroker::setAvailable(bool available) {
    if (available_ == available)
        return;
    available_ = available;
    if (!available) {
        camera_ = false; cameraState_="ERROR"; streamState_="ERROR";
        autoAllowed_=false; metadataJson_="{}";
        detection_ = item("Detected", "false");
    }
    publish("Camera/Publishing", item("Connected", bit(camera_)));
    publish("Tracking/State", item("Enabled", bit(tracking_)));
    publish("Analytics/PersonDetection", detection_);
}
/**
 * 신뢰한 내부 줄 알림을 읽어 camera/tracking/detection property로 변환한다. detector 수치도 다시 finite/범위 검사한다.
 */
void EventBroker::ingest(const std::string &line) {
    if (line.rfind("CAMERA:STATE:",0)==0) {
        const auto state=line.substr(13);
        if (state!="STARTING"&&state!="RUNNING"&&state!="ERROR"&&state!="STOPPED") return;
        cameraState_=state;
        if (state=="ERROR") streamState_="ERROR";
        publish("Camera/Publishing",item("Connected",bit(camera_))+item("CameraState",cameraState_)+item("StreamState",streamState_));
        return;
    }
    if (line.rfind("CAMERA:STREAM:",0)==0) {
        const auto state=line.substr(14);
        if (state!="STREAMING"&&state!="STOPPED"&&state!="ERROR") return;
        streamState_=state;
        publish("Camera/Publishing",item("Connected",bit(camera_))+item("CameraState",cameraState_)+item("StreamState",streamState_));
        return;
    }
    if (line.rfind("TARGET:",0)==0) {
        const auto state=line.substr(7);
        if (state!="NO_TARGET"&&state!="ACQUIRED"&&state!="TRACKING"&&state!="LOST"&&state!="ERROR") return;
        targetState_=state;
        if ((state=="LOST"||state=="NO_TARGET"||state=="ERROR") && personPresent_) {
            personPresent_=false; transition("PERSON_LOST");
        }
        publish("Tracking/State",item("Enabled",bit(tracking_))+item("AutoAllowed",bit(autoAllowed_))+item("TargetState",targetState_));
        return;
    }
    if (line.rfind("RUNTIME:",0)==0) {
        int requested=0,allowed=0,fault=0,moving=0,pan=0,tilt=0;
        unsigned long long generation=0;
        char mode[16]{},extra=0;
        if (std::sscanf(line.c_str(),"RUNTIME:%d,%d,%15[^,],%llu,%d,%d,%d,%d%c",
            &requested,&allowed,mode,&generation,&fault,&moving,&pan,&tilt,&extra)!=8 ||
            (std::string(mode)!="AUTO"&&std::string(mode)!="MANUAL"&&std::string(mode)!="LOCAL_CENTER")||
            (requested!=0&&requested!=1)||(allowed!=0&&allowed!=1)||(fault!=0&&fault!=1)||
            (moving!=0&&moving!=1)||pan<0||pan>180||tilt<0||tilt>180) return;
        autoAllowed_=allowed!=0; moving_=moving!=0; fault_=fault!=0; pan_=pan; tilt_=tilt; ptzMode_=mode;
        runtimeData_=item("Mode",ptzMode_)+item("Moving",bit(moving_))+item("Fault",bit(fault_))+
            item("PanAngle",std::to_string(pan_))+item("TiltAngle",std::to_string(tilt_))+
            item("Generation",std::to_string(generation));
        publish("PTZ/State",runtimeData_);
        publish("Tracking/State",item("Enabled",bit(tracking_))+item("AutoAllowed",bit(autoAllowed_))+item("TargetState",targetState_));
        return;
    }
    if (line == "CAMERA:CONNECTED" || line == "CAMERA:DISCONNECTED") {
        const bool camera = line == "CAMERA:CONNECTED";
        if (camera == camera_)
            return;
        camera_ = camera;
        if (!camera_) {
            if (cameraState_!="ERROR") { cameraState_="STOPPED"; streamState_="STOPPED"; }
            targetState_="NO_TARGET"; metadataJson_="{}";
        }
        publish("Camera/Publishing", item("Connected", bit(camera_))+item("CameraState",cameraState_)+item("StreamState",streamState_));
    } else if (line == "TRACK:ON" || line == "TRACK:OFF") {
        const bool enabled = line == "TRACK:ON";
        if (enabled == tracking_)
            return;
        tracking_ = enabled;
        transition(enabled ? "TRACKING_STARTED" : "TRACKING_STOPPED");
        if (!enabled) { targetState_="NO_TARGET"; personPresent_=false; metadataJson_="{}"; }
        publish("Tracking/State", item("Enabled", bit(tracking_))+item("AutoAllowed",bit(autoAllowed_))+item("TargetState",targetState_));
    } else if (line == "NO_DETECTION") {
        detection_ = item("Detected", "false"); metadataJson_="{}";
        publish("Analytics/PersonDetection", detection_);
    } else if (line.rfind("DETECT2:", 0) == 0) {
        const auto metadata = ptz::decodeDetection(std::string_view(line).substr(8));
        if (!metadata) return;
        const auto& d = *metadata;
        if (!personPresent_) { personPresent_=true; transition("PERSON_DETECTED"); }
        metadataJson_=nlohmann::json{{"confidence",d.confidence},{"bbox",{{"x",d.boxX},{"y",d.boxY},{"width",d.boxWidth},{"height",d.boxHeight}}},
            {"errorX",d.errorX},{"errorY",d.errorY},{"pan",d.panAngle},{"tilt",d.tiltAngle},
            {"frameId",std::to_string(d.frameId)},{"framePtsNs",std::to_string(d.framePtsNs)},
            {"captureUnixMs",d.captureUnixMs},{"analysisUnixMs",d.analysisUnixMs},
            {"streamEpoch",std::to_string(d.streamEpoch)}}.dump();
        // UtcTime는 broker 생성 시각이고 CaptureUnixMs는 PTS 기반 추정 UTC다.
        // Profile M의 RTP metadata나 안정적인 ObjectID를 광고하지 않는다.
        detection_ = item("Detected","true") + item("Class","person") +
                     item("Confidence",std::to_string(d.confidence)) +
                     item("SchemaVersion","2") + item("SelectedTarget","true") +
                     item("CaptureTimeSource", d.captureUnixMs ? "pts-estimated" : "unknown");
        for (const auto& field : std::vector<std::pair<std::string,std::int64_t>>{
                {"X",d.x},{"Y",d.y},{"ErrorX",d.errorX},{"ErrorY",d.errorY},
                {"BoxX",d.boxX},{"BoxY",d.boxY},{"BoxWidth",d.boxWidth},{"BoxHeight",d.boxHeight},
                {"ImageWidth",d.imageWidth},{"ImageHeight",d.imageHeight},{"FrameId",d.frameId},
                {"FramePtsNs",d.framePtsNs},{"CaptureUnixMs",d.captureUnixMs},
                {"AnalysisUnixMs",d.analysisUnixMs},{"StreamEpoch",d.streamEpoch},
                {"Generation",d.generation},{"PanAngle",d.panAngle},{"TiltAngle",d.tiltAngle}})
            detection_ += item(field.first,std::to_string(field.second));
        publish("Analytics/PersonDetection",detection_);
    } else if (line.rfind("DETECT:person,", 0) == 0) {
        float confidence;
        int x, y, ex, ey, pan, tilt;
        char extra;
        if (std::sscanf(line.c_str(), "DETECT:person,%f,%d,%d,%d,%d,%d,%d%c", &confidence, &x, &y,
                        &ex, &ey, &pan, &tilt, &extra) != 7 ||
            !std::isfinite(confidence) || confidence < 0 || confidence > 1 || x < 0 || x > 1280 ||
            y < 0 || y > 720)
            return;
        detection_ = item("Detected", "true") + item("Class", "person") +
                     item("Confidence", std::to_string(confidence)) + item("X", std::to_string(x)) +
                     item("Y", std::to_string(y)) + item("ErrorX", std::to_string(ex)) +
                     item("ErrorY", std::to_string(ey)) + item("PanAngle", std::to_string(pan)) +
                     item("TiltAngle", std::to_string(tilt)) + item("ImageWidth", "1280") +
                     item("ImageHeight", "720");
        publish("Analytics/PersonDetection", detection_);
    }
}
// 상태 전환은 일반 property coalescing에 합치지 않는다. 구독별 상한64는 유지한다.
void EventBroker::transition(const std::string& name) {
    const Event event{"Runtime/StateEvent",eventUtc(),"",item("Type",name)};
    cleanup();
    for (auto& [token,sub]:subscriptions_)
        if (selected(sub.filter,event.topic)) enqueue(sub.queue,event);
}
std::string EventBroker::snapshotJson() const {
    const auto trackingState=!tracking_ ? "DISABLED" : !autoAllowed_ ? "IDLE" : targetState_=="LOST" ? "LOST" :
        targetState_=="ACQUIRED"||targetState_=="TRACKING" ? "TRACKING" : "IDLE";
    return nlohmann::json{{"timestamp",eventUtc()},{"available",available_},
        {"cameraState",cameraState_},{"streamState",streamState_},{"trackingState",trackingState},
        {"targetState",targetState_},{"tracking",tracking_},{"autoAllowed",autoAllowed_},
        {"ptzMode",ptzMode_},{"pan",pan_},{"tilt",tilt_},{"moving",moving_},{"fault",fault_},
        {"detection",nlohmann::json::parse(metadataJson_)}}.dump();
}
/**
 * 표준 WS-Notification/tt:Message 외피와 공개된 vendor topic의 Source/Data를 구성한다.
 */
std::string eventNotification(const Event &event) {
    return "<wsnt:NotificationMessage><wsnt:Topic "
           "Dialect=\"http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet\">pc:" +
           event.topic + "</wsnt:Topic><wsnt:Message><tt:Message UtcTime=\"" + event.utc +
           "\"" + (event.operation.empty() ? std::string{} : " PropertyOperation=\""+event.operation+"\"") + "><tt:Source>" +
           item("ProfileToken", "main") + "</tt:Source><tt:Data>" + event.data +
           "</tt:Data></tt:Message></wsnt:Message></wsnt:NotificationMessage>";
}
/**
 * 현재 PullPoint 상한과 미지원 policy/persistence를 정확히 광고한다.
 */
std::string eventCapabilities() {
    return "<tev:Capabilities WSSubscriptionPolicySupport=\"false\" "
           "WSPausableSubscriptionManagerInterfaceSupport=\"false\" MaxPullPoints=\"16\" "
           "PersistentNotificationStorage=\"false\"/>";
}
/**
 * 클라이언트가 TopicNamespaceLocation에서 가져갈 정적 토픽 계층 XML을 만든다.
 */
std::string topicNamespaceXml() {
    return "<?xml version=\"1.0\"?><wstop:TopicNamespace "
           "xmlns:wstop=\"http://docs.oasis-open.org/wsn/t-1\" "
           "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
           "xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
           "targetNamespace=\"urn:ptz-camera:events\" name=\"PiPTZ\">"
           "<wstop:Topic name=\"Camera\"><wstop:Topic name=\"Publishing\"/></wstop:Topic>"
           "<wstop:Topic name=\"Tracking\"><wstop:Topic name=\"State\"/></wstop:Topic>"
           "<wstop:Topic name=\"Analytics\"><wstop:Topic "
           "name=\"PersonDetection\"/></wstop:Topic>"
           "<wstop:Topic name=\"PTZ\"><wstop:Topic name=\"State\"/></wstop:Topic>"
           "<wstop:Topic name=\"Runtime\"><wstop:Topic name=\"StateEvent\"/></wstop:Topic></wstop:TopicNamespace>";
}
} // namespace onvif
