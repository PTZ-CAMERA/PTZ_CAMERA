#pragma once
#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>
namespace onvif {
struct Event {std::string topic, utc, operation, data;};
struct PullResult {bool pending=false;std::vector<Event> messages;std::string termination;};
// ONVIF HTTP 이벤트 루프에서만 접근한다. 최신 property를 보관하고 구독별로 전달한다.
class EventBroker {
public:
    std::string create(int leaseSeconds,const std::vector<std::string>& filter);
    PullResult pull(const std::string& token,unsigned owner,int timeoutMs,int limit);
    std::string renew(const std::string& token,int seconds);
    void remove(const std::string& token);
    void synchronize(const std::string& token);
    void cancelOwner(unsigned owner);
    void ingest(const std::string& line);
    void setAvailable(bool available);
    std::string expiration(const std::string& token);
private:
    using Clock=std::chrono::steady_clock;
    struct Subscription {
        Clock::time_point expires,waitUntil{};
        std::vector<std::string> filter;
        std::deque<Event> queue;
        unsigned owner=0;
    };
    std::map<std::string,Subscription> subscriptions_;
    bool available_=false,camera_=false,tracking_=false;
    std::string detection_="<tt:SimpleItem Name=\"Detected\" Value=\"false\"/>";
    void cleanup();
    Subscription& subscription(const std::string& token);
    void publish(const std::string& topic,const std::string& data);
    void snapshot(Subscription& sub);
};
std::string eventUtc(int offsetSeconds=0);
std::string eventNotification(const Event& event);
std::string eventCapabilities();
std::string topicNamespaceXml();
} // namespace onvif
