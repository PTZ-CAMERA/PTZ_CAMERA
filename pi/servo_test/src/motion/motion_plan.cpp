/**
 * [학습 안내] GPIO와 분리한 목표각도 궤적 계산
 * MotionPlan은 Idle/Target/Velocity 중 한 상태를 가진다. 시간 차이로 다음 목표를 계산한다.
 * 이 모듈 자체는 GPIO나 스레드를 소유하지 않는다. 호출자는 같은 제어 mutex 아래 접근한다.
 * 늦게 깨어나면 한 번에 최대 100ms 분량만 계산하며 만료된 연속 명령은 추가 출력하지 않는다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "motion/motion_plan.h"
#include "config.h"
#include <algorithm>
#include <cmath>
namespace ptz {
/**
 * 계산 계획만 Idle로 바꾼다. GPIO 펄스 유지/정지는 상위 GPIO 모듈이 결정한다.
 */
void cancelPlan(MotionPlan &plan) {
    plan.mode = MotionPlan::Mode::Idle;
}
/**
 * 목표를 기구 범위에 clamp하고 시작 시각을 저장한다. 이미 현재 위치면 Idle로 처리한다.
 */
void targetPlan(MotionPlan &plan, double pan, double tilt, MotionClock::time_point now) {
    plan.targetPan = std::clamp(pan, double(PAN_MIN_ANGLE), double(PAN_MAX_ANGLE));
    plan.targetTilt = std::clamp(tilt, double(TILT_MIN_ANGLE), double(TILT_MAX_ANGLE));
    plan.last = now;
    plan.mode = (plan.targetPan == plan.pan && plan.targetTilt == plan.tilt)
                    ? MotionPlan::Mode::Idle
                    : MotionPlan::Mode::Target;
}
/**
 * 축별 signed 속도와 만료 시각을 저장한다. 두 속도 모두 0이면 즉시 Idle이다.
 */
void velocityPlan(MotionPlan &plan, double pan, double tilt, int timeoutMs,
                  MotionClock::time_point now) {
    plan.panVelocity = pan;
    plan.tiltVelocity = tilt;
    plan.deadline = now + std::chrono::milliseconds(timeoutMs);
    plan.last = now;
    plan.mode = (pan == 0 && tilt == 0) ? MotionPlan::Mode::Idle : MotionPlan::Mode::Velocity;
}
/**
 * elapsed 시간으로 고정 속도 목표 이동 또는 연속 이동을 계산한다. 만료·경계·도착에서 종료하고 목표가 바뀌었는지 반환한다.
 */
bool advancePlan(MotionPlan &plan, MotionClock::time_point now) {
    if (plan.mode == MotionPlan::Mode::Idle)
        return false;
    if (plan.mode == MotionPlan::Mode::Velocity && now >= plan.deadline) {
        cancelPlan(plan);
        return false; // 지연된 과거 이동량을 만료 후 출력하지 않는다.
    }
    const double dt = std::clamp(std::chrono::duration<double>(now - plan.last).count(), 0.0, 0.1);
    plan.last = now;
    const double oldPan = plan.pan, oldTilt = plan.tilt;
    if (plan.mode == MotionPlan::Mode::Target) {
        const auto approach = [dt](double current, double target) {
            const double step = TARGET_SPEED_DEG_PER_SEC * dt;
            return current + std::clamp(target - current, -step, step);
        };
        plan.pan = approach(plan.pan, plan.targetPan);
        plan.tilt = approach(plan.tilt, plan.targetTilt);
        if (std::abs(plan.pan - plan.targetPan) < 1e-8 &&
            std::abs(plan.tilt - plan.targetTilt) < 1e-8)
            cancelPlan(plan);
    } else {
        plan.pan = std::clamp(plan.pan + plan.panVelocity * dt, double(PAN_MIN_ANGLE),
                              double(PAN_MAX_ANGLE));
        plan.tilt = std::clamp(plan.tilt + plan.tiltVelocity * dt, double(TILT_MIN_ANGLE),
                               double(TILT_MAX_ANGLE));
        // 경계에 도달한 축은 멈추고 다른 축은 계속 진행한다.
        if ((plan.pan == PAN_MIN_ANGLE && plan.panVelocity < 0) ||
            (plan.pan == PAN_MAX_ANGLE && plan.panVelocity > 0))
            plan.panVelocity = 0;
        if ((plan.tilt == TILT_MIN_ANGLE && plan.tiltVelocity < 0) ||
            (plan.tilt == TILT_MAX_ANGLE && plan.tiltVelocity > 0))
            plan.tiltVelocity = 0;
        if (plan.panVelocity == 0 && plan.tiltVelocity == 0)
            cancelPlan(plan);
    }
    return plan.pan != oldPan || plan.tilt != oldTilt;
}
} // namespace ptz
