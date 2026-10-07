/** [학습 안내] 대상 선택만 담당하며 GPIO/네트워크 의존성이 없다. */
#include "tracking/target_selector.h"
#include <algorithm>
std::optional<PersonDetection> TargetSelector::select(const std::vector<PersonDetection>& people,
                                                     int width, int height) {
    if (people.empty()) return std::nullopt;
    const auto reference = previous_.value_or(cv::Point(width / 2, height / 2));
    const auto center = [](const PersonDetection& p) {
        return cv::Point(p.box.x + p.box.width / 2, p.box.y + p.box.height / 2);
    };
    const auto closest = std::min_element(people.begin(), people.end(), [&](const auto& a, const auto& b) {
        return cv::norm(center(a) - reference) < cv::norm(center(b) - reference);
    });
    previous_ = center(*closest);
    return *closest;
}
