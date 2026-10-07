#pragma once
#include "events/event_broker.h"
#include <chrono>
#include <poll.h>
namespace onvif {
class ServoFeed {
public:
    ServoFeed(unsigned port,EventBroker& broker):port_(port),broker_(broker){}
    ~ServoFeed();
    void reconnectIfDue();
    int fd() const{return fd_;}
    short events() const;
    void process(short flags);
private:
    unsigned port_;EventBroker& broker_;int fd_=-1;bool connecting_=false;
    std::size_t sent_=0;std::string input_;
    std::chrono::steady_clock::time_point retry_{};
    void disconnect();
};
}
