/**
 * [학습 안내] 명령 각도와 궤적의 공통 제어 계층
 * AI/ONVIF/키보드에 대한 조건문이 없다. 외부에서 허용된 이동만 전달받는다.
 * 기존 MotionPlan과 속도/범위/타임아웃을 재사용한다. 상태는 센서 측정값이 아닌 명령 값이다.
 * 이 객체에 대한 접근은 servo_control adapter의 단일 mutex 아래 이루어진다.
 */
#include "control/ptz_controller.h"
#include "config.h"
#include "runtime/log.h"
#include "performance/performance_log.h"
#include <algorithm>
#include <cmath>
#include <iostream>
namespace ptz {
// 출력 성공 때만 명령 각도를 저장하고 실패를 fault로 남긴다.
bool PtzController::write(unsigned gpio, int angle, int& current) {
    // 마지막 출력 경계에서도 clamp한다. 반전은 같은 기구 범위의 양 끝을 교환한다.
    const bool pan=gpio==PAN_GPIO;
    if (!pan && gpio!=TILT_GPIO) { fault_=true; return false; }
    const int minimum=pan ? PAN_MIN_ANGLE : TILT_MIN_ANGLE;
    const int maximum=pan ? PAN_MAX_ANGLE : TILT_MAX_ANGLE;
    angle=std::clamp(angle,minimum,maximum);
    const bool invert=pan ? edge::appConfig().ptz.invertPan : edge::appConfig().ptz.invertTilt;
    const int output=invert ? minimum+maximum-angle : angle;
    if (!driver_.write(gpio, angleToPulseWidth(output))) {
        fault_ = true;
        return false;
    }
    current = angle; // 실패한 출력을 성공한 위치처럼 보고하지 않는다.
    return true;
}
// 현재 출력 명령 각도를 표시한다. 물리 위치 측정값은 아니다.
void PtzController::print() const {
    edge::log("PTZ","PAN: "+std::to_string(pan_)+" deg TILT: "+std::to_string(tilt_)+" deg");
}
// 직접 PAN 입력은 이전 궤적을 취소하고 기구 범위를 제한한다.
void PtzController::setPan(int angle) {
    stop(); fault_ = false;
    write(PAN_GPIO, std::clamp(angle, PAN_MIN_ANGLE, PAN_MAX_ANGLE), pan_);
    print();
}
// TILT만 갱신하지만 공유 계획은 취소해 이전 이동이 덮어쓰지 않게 한다.
void PtzController::setTilt(int angle) {
    stop(); fault_ = false;
    write(TILT_GPIO, std::clamp(angle, TILT_MIN_ANGLE, TILT_MAX_ANGLE), tilt_);
    print();
}
// 두 축을 중앙으로 출력한다. 호출자는 동일 mutex를 유지한다.
void PtzController::center() {
    stop(); fault_ = false;
    write(PAN_GPIO, edge::appConfig().ptz.panCenter, pan_); write(TILT_GPIO, edge::appConfig().ptz.tiltCenter, tilt_); print();
}
// 이미 승인된 상대 보정을 축별 범위 안에서 출력한다.
void PtzController::adjust(int dx, int dy) {
    const int pan = int(std::clamp(static_cast<long long>(pan_) + dx, static_cast<long long>(PAN_MIN_ANGLE), static_cast<long long>(PAN_MAX_ANGLE)));
    const int tilt = int(std::clamp(static_cast<long long>(tilt_) + dy, static_cast<long long>(TILT_MIN_ANGLE), static_cast<long long>(TILT_MAX_ANGLE)));
    const bool changed = pan != pan_ || tilt != tilt_;
    if (pan != pan_) write(PAN_GPIO, pan, pan_);
    if (tilt != tilt_) write(TILT_GPIO, tilt, tilt_);
    if (changed) print();
}
// 즉시 도착이 아닌 목표 계획을 수락한다. 상대 이동은 현재 명령 위치 기준이다.
bool PtzController::target(double pan, double tilt, bool relative) {
    if (!std::isfinite(pan) || !std::isfinite(tilt)) return false;
    motion_.pan = pan_; motion_.tilt = tilt_; fault_ = false;
    targetPlan(motion_, relative ? pan_ + pan : pan, relative ? tilt_ + tilt : tilt,
               MotionClock::now());
    perf::instant("servo_target_accepted", std::lround(motion_.targetPan), std::lround(motion_.targetTilt));
    return true;
}
// 속도와 lease 범위를 검사하고 기존 소수 각도를 보존한다.
bool PtzController::velocity(double pan, double tilt, int timeoutMs) {
    if (!std::isfinite(pan) || !std::isfinite(tilt) ||
        std::abs(pan) > MAX_VELOCITY_DEG_PER_SEC || std::abs(tilt) > MAX_VELOCITY_DEG_PER_SEC ||
        timeoutMs < 100 || timeoutMs > 5000) return false;
    // 연속 명령 갱신은 소수 각도를 보존해 작은 속도가 반올림으로 사라지지 않게 한다.
    if (motion_.mode != MotionPlan::Mode::Velocity || fault_) {
        motion_.pan = pan_; motion_.tilt = tilt_;
    }
    fault_ = false;
    velocityPlan(motion_, pan, tilt, timeoutMs, MotionClock::now());
    return true;
}
// 마지막 pulse는 유지하고 이후 목표 갱신만 중단한다.
void PtzController::stop() {
    cancelPlan(motion_); // pulse는 유지한다. 물리적 비상 정지와 다르다.
}
// 약 20ms 제어 주기에서 계획을 전진시키고 변한 축만 쓴다.
void PtzController::update(MotionClock::time_point now) {
    if (motion_.mode == MotionPlan::Mode::Idle) return;
    perf::Scope timing("servo_motion_tick");
    advancePlan(motion_, now);
    const int pan = std::clamp(int(std::lround(motion_.pan)), PAN_MIN_ANGLE, PAN_MAX_ANGLE);
    const int tilt = std::clamp(int(std::lround(motion_.tilt)), TILT_MIN_ANGLE, TILT_MAX_ANGLE);
    const bool changed = pan != pan_ || tilt != tilt_;
    bool ok = true;
    if (pan != pan_) ok = write(PAN_GPIO, pan, pan_);
    if (tilt != tilt_) ok = write(TILT_GPIO, tilt, tilt_) && ok;
    if (!ok) stop();
    if (changed) print();
}
// 상태 조회는 명령 각도·계획 진행·출력 오류의 snapshot이다.
ServoMotionStatus PtzController::status() const {
    return {pan_, tilt_, motion_.mode != MotionPlan::Mode::Idle, fault_};
}
} // namespace ptz
