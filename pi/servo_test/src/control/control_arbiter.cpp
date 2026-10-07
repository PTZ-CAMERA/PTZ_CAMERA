/**
 * [학습 안내] 수동·자동 입력의 우선순위와 세대
 * 추론은 수백 ms 걸리므로 ON→OFF→ON 사이에 끝난 옛 결과가 새 추적을 움직일 수 있다.
 * 모드 전환마다 generation을 올리고, AI가 시작 시 읽은 세대와 일치할 때만 보정을 허용한다.
 * 목표 이동 후 추적 예약은 Controller가 도착/오류 상태를 알리면 여기서 전환한다.
 */
#include "control/control_arbiter.h"
#include "config.h"
#include <cmath>
namespace ptz {
// 수동 입력이 새 자동 보정과 이동 후 추적 예약을 무효화한다.
void ControlArbiter::manual() {
    tracking_ = afterTarget_ = false;
    ++generation_;
}
// 활성화는 이전 수동 계획을 중단한다. 전환마다 AI 결과 세대를 바꾼다.
void ControlArbiter::enable(bool enabled) {
    ++generation_;
    afterTarget_ = false;
    tracking_ = enabled;
    if (enabled) controller_.stop();
}
// 유효 목표로 먼저 이동할 경우 도착할 때까지 추적을 예약한다.
bool ControlArbiter::startTracking(bool moveFirst, double pan, double tilt) {
    if (!std::isfinite(pan) || !std::isfinite(tilt) || pan < PAN_MIN_ANGLE ||
        pan > PAN_MAX_ANGLE || tilt < TILT_MIN_ANGLE || tilt > TILT_MAX_ANGLE) return false;
    controller_.stop();
    if (moveFirst && !controller_.target(pan, tilt, false)) return false;
    ++generation_;
    afterTarget_ = moveFirst && controller_.status().moving;
    tracking_ = !afterTarget_;
    return true;
}
// bool뿐 아니라 추론 시작 세대까지 일치할 때만 Controller를 호출한다.
bool ControlArbiter::automatic(int dx, int dy, std::uint64_t generation) {
    if (!tracking_ || generation != generation_) return false;
    controller_.adjust(dx, dy);
    if (controller_.status().fault) manual();
    return true;
}
// 계획 완료 시 추적으로 전환하고 출력 오류 시 예약/추적을 해제한다.
void ControlArbiter::update() {
    controller_.update();
    const auto state = controller_.status();
    if (state.fault) { if (requested()) manual(); return; }
    if (afterTarget_ && !state.moving) {
        afterTarget_ = false; tracking_ = true;
    }
}
} // namespace ptz
