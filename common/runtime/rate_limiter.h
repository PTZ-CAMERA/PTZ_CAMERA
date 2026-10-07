#pragma once
#include <chrono>
#include <stdexcept>
namespace edge {
// 실시간 값은 상한 주기로 보내고 상태 전환은 별도 경로로 즉시 보낸다.
class RateLimiter {
public:
    using Clock=std::chrono::steady_clock;
    explicit RateLimiter(int hz) {
        if (hz<1||hz>10) throw std::invalid_argument("Metadata rate must be 1..10Hz");
        period_=std::chrono::nanoseconds(1000000000LL/hz);
    }
    bool ready(Clock::time_point now=Clock::now()) {
        if (now<next_) return false;
        next_=now+period_; return true;
    }
    void reset() { next_={}; }
private:
    Clock::duration period_;
    Clock::time_point next_{};
};
}
