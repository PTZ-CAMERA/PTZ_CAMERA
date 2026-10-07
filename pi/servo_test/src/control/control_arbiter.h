#pragma once
#include "control/ptz_controller.h"
#include <cstdint>
namespace ptz {
enum class PtzMode { Auto, Manual, LocalCenter };
enum class TrackingGate { Disabled, Active, AfterTarget, ManualOverride };
// 요청(사용자 ON/OFF)과 현재 자동 출력 허용을 분리한다. 호출자는 동일 mutex를 소유한다.
class ControlArbiter {
public:
    explicit ControlArbiter(PtzController& controller) : controller_(controller) {}
    void manual(MotionClock::time_point now=MotionClock::now());
    void localCenter();
    void enable(bool enabled);
    bool startTracking(bool moveFirst,double pan,double tilt);
    bool automatic(int panDelta,int tiltDelta,std::uint64_t generation);
    bool stopAutomatic(std::uint64_t generation);
    void update(MotionClock::time_point now=MotionClock::now());
    bool tracking() const { return requested_ && gate_==TrackingGate::Active; }
    bool requested() const { return requested_; }
    std::uint64_t generation() const { return generation_; }
    PtzMode mode() const { return mode_; }
private:
    PtzController& controller_;
    bool requested_=false;
    TrackingGate gate_=TrackingGate::Disabled;
    PtzMode mode_=PtzMode::Manual;
    std::uint64_t generation_=0;
    MotionClock::time_point resumeAt_{};
};
const char* ptzModeName(PtzMode mode);
} // namespace ptz
