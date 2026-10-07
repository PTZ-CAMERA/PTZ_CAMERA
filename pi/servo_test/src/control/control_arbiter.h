#pragma once
#include "control/ptz_controller.h"
#include <cstdint>
namespace ptz {
// source 정책만 관리한다. 오래된 추론 결과는 generation으로 구별한다.
class ControlArbiter {
public:
    explicit ControlArbiter(PtzController& controller) : controller_(controller) {}
    void manual();
    void enable(bool enabled);
    bool startTracking(bool moveFirst, double pan, double tilt);
    bool automatic(int panDelta, int tiltDelta, std::uint64_t generation);
    void update();
    bool tracking() const { return tracking_; }
    bool requested() const { return tracking_ || afterTarget_; }
    std::uint64_t generation() const { return generation_; }
private:
    PtzController& controller_;
    bool tracking_ = false, afterTarget_ = false;
    std::uint64_t generation_ = 0;
};
} // namespace ptz
