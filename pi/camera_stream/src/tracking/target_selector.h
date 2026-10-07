#pragma once
#include "person_detector.h"
#include <optional>
// 최근 중심 거리로 대상 선택. 신원 재식별/영구 object ID를 보장하지 않는다.
class TargetSelector {
public:
    std::optional<PersonDetection> select(const std::vector<PersonDetection>& people, int width, int height);
    void reset() { previous_.reset(); }
private:
    std::optional<cv::Point> previous_;
};
