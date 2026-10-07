/**
 * [학습 안내] 추적 계산 계층
 * 600ms 초과 결과를 폐기하고 기존 8% deadband/최대 3도 보정을 유지한다.
 * 음의 frameAge는 시각을 알 수 없는 경우다. 실제 GPIO/수동 우선 정책은 하위 제어 서비스에 있다.
 */
#include "tracking/tracking_controller.h"
#include <algorithm>
#include <cmath>
namespace {
int step(double error) {
    if (std::abs(error) < 0.08) return 0;
    const int degrees = std::clamp(int(std::lround(std::abs(error) * 4)), 1, 3);
    return error > 0 ? degrees : -degrees;
}
}
std::optional<TrackingCorrection> TrackingController::update(const PersonDetection& target,
    int iw, int ih, int ow, int oh, int age) const {
    if (age > 600 || iw <= 0 || ih <= 0 || ow <= 1 || oh <= 1) return std::nullopt;
    const auto clipped = target.box & cv::Rect(0, 0, iw, ih);
    if (clipped.empty()) return std::nullopt;
    TrackingCorrection r{};
    const int cx = clipped.x + clipped.width / 2, cy = clipped.y + clipped.height / 2;
    r.x = cx * ow / iw; r.y = cy * oh / ih;
    r.errorX = (cx - iw / 2) * ow / iw; r.errorY = (cy - ih / 2) * oh / ih;
    r.panDelta = step(double(r.errorX) / (ow / 2));
    r.tiltDelta = step(-double(r.errorY) / (oh / 2));
    // 장착 방향은 기존 부호를 유지한다. 방향 보정은 실물 검증과 별도로 진행한다.
    const int right = (clipped.x + clipped.width) * ow / iw;
    const int bottom = (clipped.y + clipped.height) * oh / ih;
    r.box = cv::Rect(clipped.x * ow / iw, clipped.y * oh / ih,
                    right - clipped.x * ow / iw, bottom - clipped.y * oh / ih);
    return r;
}
