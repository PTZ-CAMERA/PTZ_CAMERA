/**
 * [학습 안내] Events broker와 SOAP 구독 동작 단위 검증
 * 초기 상태, deferred 응답, 토픽 선택, 최신 좌표 합치기, 동기화, 갱신/취소/만료를 확인한다.
 * broker에 가짜 상태를 직접 주입한다. 실제 센서·카메라·GPIO를 사용하지 않는다.
 * 대기 요청을 반환하는 데 실제 Timeout만큼 블로킹하지 않는지도 확인한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "protocol/service.h"
#include "events/event_broker.h"
#include "xml/xml.h"
#include <cassert>
#include "metadata/detection_metadata.h"
#include <chrono>
#include <iostream>
namespace {
/**
 * 테스트에서 입력 namespace가 명확한 Envelope를 만든다. 접두어 이름이 바뀌어도 처리 가능한지 검증하는 자료다.
 */
std::string soap(const std::string &body) {
    return "<s:Envelope xmlns:s=\"" + std::string(onvif::SOAP) + "\" xmlns:e=\"" + onvif::EVENTS +
           "\" xmlns:n=\"" + onvif::WSNT + "\" xmlns:pc=\"" + onvif::TOPICS + "\"><s:Body>" + body +
           "</s:Body></s:Envelope>";
}
/**
 * 구독 생성 응답의 SubscriptionReference/Address를 읽고 path의 마지막 token을 반환한다. 이 값으로 테스트 PullPoint endpoint를 만든다.
 */
std::string token(const onvif::Response &response) {
    auto doc = onvif::parseXml(response.xml);
    const auto *ref = doc->child(onvif::SOAP, "Body")
                          ->child(onvif::EVENTS, "CreatePullPointSubscriptionResponse")
                          ->child(onvif::EVENTS, "SubscriptionReference")
                          ->child("http://www.w3.org/2005/08/addressing", "Address");
    assert(ref);
    return ref->text.substr(ref->text.rfind('/') + 1);
}
} // namespace
/**
 * 테스트의 입력/실행/검증/정리를 순서대로 수행한다. assert 실패는 해당 계약이 깨졌다는 뜻이며 실물 성능 인증이 아니다.
 */
int main() {
    onvif::Config config;
    config.ip = "127.0.0.1";
    config.events = std::make_shared<onvif::EventBroker>();
    config.requestId = 1;
    const auto call = [&](const std::string &path, const std::string &body) {
        return onvif::soapResponse(config, path, soap(body));
    };
    assert(call("/onvif/events_service", "<e:GetEventProperties/>").status == 200);
    auto created = call("/onvif/events_service",
                        "<e:CreatePullPointSubscription><e:InitialTerminationTime>PT60S</"
                        "e:InitialTerminationTime></e:CreatePullPointSubscription>");
    assert(created.status == 200);
    const auto id = token(created), path = "/onvif/pullpoint/" + id;
    const std::string pull = "<e:PullMessages><e:Timeout>PT1S</e:Timeout><e:MessageLimit>16</"
                             "e:MessageLimit></e:PullMessages>";
    auto initial = call(path, pull);
    assert(initial.status == 200 && !initial.deferred &&
           initial.xml.find("Initialized") != std::string::npos);
    const auto begin = std::chrono::steady_clock::now();
    auto pending = call(path, pull);
    assert(pending.deferred &&
           std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(100));
    config.requestId = 2;
    assert(call(path, pull).status == 500);
    config.requestId = 1;
    config.events->setAvailable(true);
    config.events->ingest("CAMERA:CONNECTED");
    config.events->ingest("TRACK:ON");
    for (int x = 100; x < 200; ++x)
        config.events->ingest("DETECT:person,0.9," + std::to_string(x) + ",360,0,0,90,90");
    auto delivery = call(path, pull);
    assert(!delivery.deferred && delivery.xml.find("Value=\"199\"") != std::string::npos);
    assert(delivery.xml.find("Value=\"100\"") ==
           std::string::npos); // 최신 property만 남겼는지 확인.
    // v2 계약의 실제 broker XML과 비정상 입력 폐기를 확인한다.
    ptz::DetectionMetadata d;
    d.confidence=.9; d.x=640; d.y=360;
    d.boxX=500; d.boxY=200; d.boxWidth=280; d.boxHeight=320;
    d.frameId=7; d.framePtsNs=123456789; d.captureUnixMs=1000;
    d.analysisUnixMs=1300; d.streamEpoch=999; d.generation=2;
    const auto encoded=ptz::encodeDetection(d);
    assert(ptz::decodeDetection(encoded));
    assert(!ptz::decodeDetection(encoded+",extra"));
    auto invalid=d; invalid.boxWidth=9999;
    assert(!ptz::decodeDetection(ptz::encodeDetection(invalid)));
    invalid=d; invalid.errorX=INT64_MIN;
    assert(!ptz::decodeDetection(ptz::encodeDetection(invalid)));
    config.events->ingest("DETECT2:"+encoded);
    auto enriched=call(path,pull);
    assert(enriched.xml.find("Name=\"BoxWidth\" Value=\"280\"")!=std::string::npos);
    assert(enriched.xml.find("Name=\"FramePtsNs\" Value=\"123456789\"")!=std::string::npos);
    assert(enriched.xml.find("pts-estimated")!=std::string::npos);
    config.events->ingest("DETECT2:"+ptz::encodeDetection(invalid));
    assert(call(path,pull).deferred); // 잘못된 입력이 새로운 통지로 바뀌면 안 된다.
    config.events->cancelOwner(config.requestId);
    assert(call(path, "<e:SetSynchronizationPoint/>").status == 200);
    assert(call(path, pull).xml.find("Initialized") != std::string::npos);
    assert(call(path, "<n:Renew><n:TerminationTime>PT120S</n:TerminationTime></n:Renew>").status ==
           200);
    assert(call(path, "<n:Unsubscribe/>").status == 200);
    assert(call(path, pull).status == 500);
    auto filtered =
        call("/onvif/events_service",
             "<e:CreatePullPointSubscription><e:Filter><n:TopicExpression "
             "Dialect=\"http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet\">pc:Analytics/"
             "PersonDetection</n:TopicExpression></e:Filter></e:CreatePullPointSubscription>");
    assert(filtered.status == 200);
    auto specific = call("/onvif/pullpoint/" + token(filtered), pull);
    assert(specific.xml.find("Camera/Publishing") == std::string::npos &&
           specific.xml.find("Analytics/PersonDetection") != std::string::npos);
    const auto expired = config.events->create(0, {});
    assert(call("/onvif/pullpoint/" + expired, pull).status == 500);
    assert(call("/onvif/events_service",
                "<e:CreatePullPointSubscription><e:InitialTerminationTime>PT0S</"
                "e:InitialTerminationTime></e:CreatePullPointSubscription>")
               .status == 500);
    std::cout << "PASS initialization, asynchronous pull, topics, latest state, synchronization, "
                 "renew/unsubscribe/expiry\n";
}
