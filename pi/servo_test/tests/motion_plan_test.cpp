/**
 * [학습 안내] 순수 궤적 계산의 경계/시간 검증
 * 가상 steady_clock 시각을 직접 전달해 목표 도착, timeout, 축 경계, 취소를 확인한다.
 * GPIO 없이 결정적인 입력으로 실행하므로 모터에 전원이 없어도 검증할 수 있다.
 * 계산된 명령 각도가 맞는지 확인하며 물리 회전 시간을 검증하지 않는다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "motion/motion_plan.h"
#include <cassert>
#include <cmath>
#include <iostream>
/**
 * 테스트의 입력/실행/검증/정리를 순서대로 수행한다. assert 실패는 해당 계약이 깨졌다는 뜻이며 실물 성능 인증이 아니다.
 */
int main() {
    using namespace ptz;
    const MotionClock::time_point start{};
    MotionPlan plan;
    targetPlan(plan, 95, 85, start);
    for (int i = 1; i <= 50; ++i)
        advancePlan(plan, start + std::chrono::milliseconds(i * 20));
    assert(plan.mode == MotionPlan::Mode::Idle && plan.pan == 95 && plan.tilt == 85);
    velocityPlan(plan, 30, 0, 500, start);
    advancePlan(plan, start + std::chrono::milliseconds(100));
    assert(std::abs(plan.pan - 98) < 1e-8);
    const auto old = plan.pan;
    advancePlan(plan, start + std::chrono::seconds(1));
    assert(plan.pan == old && plan.mode == MotionPlan::Mode::Idle);
    plan.pan = 154;
    plan.tilt = 139;
    velocityPlan(plan, 30, 30, 1000, start);
    advancePlan(plan, start + std::chrono::milliseconds(100));
    assert(plan.pan == 155 && plan.tilt == 140 && plan.mode == MotionPlan::Mode::Idle);
    plan.pan = 90;
    plan.tilt = 90;
    targetPlan(plan, 999, -999, start);
    for (int i = 1; i <= 500; ++i)
        advancePlan(plan, start + std::chrono::milliseconds(i * 20));
    assert(plan.pan == 155 && plan.tilt == 40);
    velocityPlan(plan, -30, 30, 5000, start);
    cancelPlan(plan);
    assert(!advancePlan(plan, start + std::chrono::milliseconds(20)));
    std::cout << "PASS target ramp, limits, timeout, stop without GPIO\n";
}
