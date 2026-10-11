/**
 * [학습 안내] 수동 우선, 요청 유지, 자동 복귀를 담당하는 정책 계층
 * trackingRequested가 ON이어도 수동 조작 동안 자동 출력은 허용하지 않는다.
 * Stop/timeout 이후 유예가 끝나면 새 세대에서 자동 출력을 다시 허용한다.
 * 명시적 추적 OFF/중앙 버튼/출력 오류는 요청을 취소하므로 자동 복귀하지 않는다.
 */
#include "control/control_arbiter.h"
#include "config.h"
#include "runtime/log.h"
#include <cmath>
namespace ptz {
const char* ptzModeName(PtzMode mode) {
    switch (mode) {
    case PtzMode::Auto: return "AUTO";
    case PtzMode::Manual: return "MANUAL";
    case PtzMode::LocalCenter: return "LOCAL_CENTER";
    }
    return "MANUAL";
}
void ControlArbiter::manual(MotionClock::time_point now) {
    // 사용자의 추적 요청(requested_)과 지금 자동 출력을 허용하는 상태(gate_)를 분리한다.
    // 정책은 여기서 결정하고 GPIO 출력은 Controller/Driver 계층에서 수행한다.
    if (mode_!=PtzMode::Manual) edge::log("PTZ",std::string("Mode ")+ptzModeName(mode_)+" -> MANUAL");
    mode_=PtzMode::Manual;
    gate_=requested_ ? TrackingGate::ManualOverride : TrackingGate::Disabled;
    resumeAt_=now+std::chrono::milliseconds(edge::appConfig().tracking.manualResumeMs);
    ++generation_; // 수동 명령 직전에 계산된 AI 결과도 여기서 무효화한다.
}
void ControlArbiter::localCenter() {
    requested_=false; gate_=TrackingGate::Disabled; mode_=PtzMode::LocalCenter;
    ++generation_;
    edge::log("PTZ","LOCAL_CENTER; tracking request cancelled");
}
void ControlArbiter::enable(bool enabled) {
    if (enabled && controller_.status().fault) enabled=false;
    if (!enabled && mode_==PtzMode::Auto) controller_.stop();
    ++generation_;
    requested_=enabled;
    gate_=enabled ? TrackingGate::Active : TrackingGate::Disabled;
    if (enabled) { controller_.stop(); mode_=PtzMode::Auto; }
    edge::log("TRACK",enabled ? "Tracking requested ON" : "Tracking requested OFF");
}
bool ControlArbiter::startTracking(bool moveFirst,double pan,double tilt) {
    if (moveFirst && (!std::isfinite(pan)||!std::isfinite(tilt)||pan<PAN_MIN_ANGLE||pan>PAN_MAX_ANGLE||
        tilt<TILT_MIN_ANGLE||tilt>TILT_MAX_ANGLE)) return false;
    if (controller_.status().fault) return false;
    controller_.stop();
    if (moveFirst&&!controller_.target(pan,tilt,false)) return false;
    ++generation_; requested_=true; mode_=PtzMode::Auto;
    gate_=moveFirst&&controller_.status().moving ? TrackingGate::AfterTarget : TrackingGate::Active;
    return true;
}
bool ControlArbiter::automatic(int dx,int dy,std::uint64_t generation) {
    if (!tracking()||generation!=generation_) return false;
    controller_.adjust(dx,dy);
    if (controller_.status().fault) { enable(false); controller_.stop(); }
    return true;
}
bool ControlArbiter::stopAutomatic(std::uint64_t generation) {
    if (!tracking()||generation!=generation_) return false;
    controller_.stop(); // LOST는 사용자 추적 요청을 취소하지 않는다. 재획득을 허용한다.
    return true;
}
void ControlArbiter::update(MotionClock::time_point now) {
    controller_.update(now);
    const auto state=controller_.status();
    if (state.fault) { if (requested_) enable(false); controller_.stop(); return; }
    if (!requested_) return;
    if (gate_==TrackingGate::AfterTarget&&!state.moving) gate_=TrackingGate::Active;
    if (gate_!=TrackingGate::ManualOverride) return;
    if (state.moving) {
        resumeAt_=now+std::chrono::milliseconds(edge::appConfig().tracking.manualResumeMs);
        return;
    }
    if (now>=resumeAt_) {
        gate_=TrackingGate::Active; mode_=PtzMode::Auto; ++generation_;
        edge::log("PTZ","Mode MANUAL -> AUTO");
    }
}
} // namespace ptz
