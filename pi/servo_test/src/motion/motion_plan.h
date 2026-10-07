#pragma once
#include <chrono>
namespace ptz {
using MotionClock = std::chrono::steady_clock;
// 순수 목표각도 계산 상태. GPIO 쓰기와 mutex는 servo_control 모듈이 담당한다.
struct MotionPlan {
    enum class Mode { Idle, Target, Velocity } mode = Mode::Idle;
    double pan = 90, tilt = 90, targetPan = 90, targetTilt = 90;
    double panVelocity = 0, tiltVelocity = 0;
    MotionClock::time_point last{}, deadline{};
};
void cancelPlan(MotionPlan& plan);
void targetPlan(MotionPlan& plan, double pan, double tilt, MotionClock::time_point now);
void velocityPlan(MotionPlan& plan, double pan, double tilt, int timeoutMs, MotionClock::time_point now);
bool advancePlan(MotionPlan& plan, MotionClock::time_point now);
} // namespace ptz
