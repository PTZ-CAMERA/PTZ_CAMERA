#pragma once
#include "person_detector.h"
#include <optional>
struct TrackingCorrection {
    int x, y, errorX, errorY, panDelta, tiltDelta;
    cv::Rect box;
};
// 이미지 좌표를 송출 좌표로 변환하고 작은 이동량만 계산한다.
class TrackingController {
public:
    std::optional<TrackingCorrection> update(const PersonDetection& target,
        int inputWidth, int inputHeight, int outputWidth, int outputHeight, int frameAgeMs) const;
};
