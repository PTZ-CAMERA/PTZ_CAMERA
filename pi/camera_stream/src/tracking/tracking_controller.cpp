/**
 * [학습 안내] 추적 계산 계층
 * 600ms 초과 결과를 폐기하고 기존 8% deadband/최대 3도 보정을 유지한다.
 * 음의 frameAge는 시각을 알 수 없는 경우다. 실제 GPIO/수동 우선 정책은 하위 제어 서비스에 있다.
 */
#include "tracking/tracking_controller.h"
#include <algorithm>
#include "config/app_config.h"
#include "runtime/log.h"
#include <cmath>
namespace {
int step(double error) {
    if (error==0) return 0;

    const int degrees = std::clamp(int(std::lround(std::abs(error) * 4)), 1, 3);
    return error > 0 ? degrees : -degrees;
}
}
std::optional<TrackingCorrection> TrackingController::update(const PersonDetection& target,
    int iw, int ih, int ow, int oh, int age) const {
    if (!std::isfinite(target.confidence) || age > edge::appConfig().tracking.maxFrameAgeMs || iw <= 0 || ih <= 0 || ow <= 1 || oh <= 1) return std::nullopt;
    const auto clipped = target.box & cv::Rect(0, 0, iw, ih);
    if (clipped.empty()) return std::nullopt;
    TrackingCorrection r{};
    const int cx = clipped.x + clipped.width / 2, cy = clipped.y + clipped.height / 2;
    r.x = cx * ow / iw; r.y = cy * oh / ih;
    r.errorX = (cx - iw / 2) * ow / iw; r.errorY = (cy - ih / 2) * oh / ih;
    r.panDelta = std::abs(r.errorX)<edge::appConfig().tracking.deadZoneX ? 0 : step(double(r.errorX) / (ow / 2));
    r.tiltDelta = std::abs(r.errorY)<edge::appConfig().tracking.deadZoneY ? 0 : step(-double(r.errorY) / (oh / 2));
    // 장착 방향은 기존 부호를 유지한다. 방향 보정은 실물 검증과 별도로 진행한다.
    const int right = (clipped.x + clipped.width) * ow / iw;
    const int bottom = (clipped.y + clipped.height) * oh / ih;
    r.box = cv::Rect(clipped.x * ow / iw, clipped.y * oh / ih,
                    right - clipped.x * ow / iw, bottom - clipped.y * oh / ih);
    return r;
}

const char* targetStateName(TargetState state) {
    switch (state) {
    case TargetState::NoTarget: return "NO_TARGET";
    case TargetState::Acquired: return "ACQUIRED";
    case TargetState::Tracking: return "TRACKING";
    case TargetState::Lost: return "LOST";
    }
    return "NO_TARGET";
}
// 최근 중심 정책을 유지한다. 짧은 탐지 누락에는 기준 중심을 보존하되 이동 요청은 만들지 않는다.
std::optional<PersonDetection> TrackingController::observe(const std::vector<PersonDetection>& people,
    int width,int height,Clock::time_point now) {
    tick(now);
    const auto target=selector_.select(people,width,height);
    if (!target) return std::nullopt;
    const auto next=(state_==TargetState::NoTarget||state_==TargetState::Lost) ? TargetState::Acquired : TargetState::Tracking;
    if (next!=state_) edge::log("TRACK",targetStateName(next));
    state_=next; lastSeen_=now;
    return target;
}
// 입력이 오지 않거나 전부 오래된 결과여도 일정 시간 후 LOST가 된다.
void TrackingController::tick(Clock::time_point now) {
    if (lastSeen_ && state_!=TargetState::Lost &&
        now-*lastSeen_ >= std::chrono::milliseconds(edge::appConfig().tracking.lostTimeoutMs)) {
        state_=TargetState::Lost; selector_.reset(); edge::log("TRACK","LOST");
    }
}
void TrackingController::reset() {
    state_=TargetState::NoTarget; lastSeen_.reset(); selector_.reset();
}
