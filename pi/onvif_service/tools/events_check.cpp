/**
 * [학습 안내] 실제 Events 서비스의 구독 수명주기 진단
 * GetServices → GetEventProperties → CreatePullPointSubscription → PullMessages → Renew → Unsubscribe 순서다.
 * 초기 property 알림을 출력해 Qt가 해석할 필드 구조를 확인한다. 이동/추적 시작 명령을 보내지 않는다.
 * 주어진 계정 파일을 공통 SOAP 클라이언트가 사용한다. 구독 주소는 서버 응답에서 읽는다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "soap_client.h"
#include "protocol/service.h"
#include <iostream>
namespace {
/**
 * SOAP Body에서 기대하는 operation response를 찾는다. HTTP 성공만으로 응답 의미를 성공 판정하지 않는다.
 */
const onvif::Node &payload(const onvif::Node &response, const std::string &operation) {
    return onvif::client::required(onvif::client::required(response, onvif::SOAP, "Body"),
                                   onvif::EVENTS, operation + "Response");
}
} // namespace
/**
 * 실행 인자와 자원을 준비한 뒤 위 함수들을 호출한다. 정상/오류 종료를 반환값으로 구분하고 보유 자원을 종료 경로에서 정리한다.
 */
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: onvif_events_check DEVICE_URL\n";
        return 1;
    }
    try {
        using onvif::client::call;
        using onvif::client::required;
        auto services =
            call(argv[1],
                 "<d:GetServices><d:IncludeCapability>false</d:IncludeCapability></d:GetServices>");
        const auto &list = required(required(*services, onvif::SOAP, "Body"), onvif::DEVICE,
                                    "GetServicesResponse");
        std::string address;
        for (const auto &service : list.children)
            if (required(*service, onvif::DEVICE, "Namespace").text == onvif::EVENTS)
                address = required(*service, onvif::DEVICE, "XAddr").text;
        if (address.empty())
            throw std::runtime_error("Events service not found");
        call(address, "<e:GetEventProperties/>");
        auto created =
            call(address, "<e:CreatePullPointSubscription><e:InitialTerminationTime>PT60S</"
                          "e:InitialTerminationTime></e:CreatePullPointSubscription>");
        const auto &reference = required(payload(*created, "CreatePullPointSubscription"),
                                         onvif::EVENTS, "SubscriptionReference");
        const auto subscription =
            required(reference, "http://www.w3.org/2005/08/addressing", "Address").text;
        auto messages =
            call(subscription, "<e:PullMessages><e:Timeout>PT0.2S</e:Timeout><e:MessageLimit>16</"
                               "e:MessageLimit></e:PullMessages>");
        std::cout << "Events XAddr: " << address << '\n';
        for (const auto &message : payload(*messages, "PullMessages").children) {
            if (message->ns != onvif::WSNT || message->name != "NotificationMessage")
                continue;
            const auto &topic = required(*message, onvif::WSNT, "Topic");
            const auto &data = required(
                required(required(*message, onvif::WSNT, "Message"), onvif::TYPES, "Message"),
                onvif::TYPES, "Data");
            std::cout << topic.text;
            for (const auto &item : data.children)
                std::cout << ' ' << item->attributes.at("Name") << '='
                          << item->attributes.at("Value");
            std::cout << '\n';
        }
        call(subscription, "<n:Renew><n:TerminationTime>PT60S</n:TerminationTime></n:Renew>");
        call(subscription, "<n:Unsubscribe/>");
        std::cout << "PASS subscription, initialization, renewal, unsubscribe\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
