#pragma once
#include "person_detector.h"
#include "tracking/target_selector.h"
#include <chrono>
#include <optional>
enum class TargetState { NoTarget, Acquired, Tracking, Lost };
const char* targetStateName(TargetState state);
struct TrackingCorrection {
    int x, y, errorX, errorY, panDelta, tiltDelta;
    cv::Rect box;
};
// 대상 선택/소실 상태를 소유한다. 출력은 순수 보정 요청이며 GPIO/ONVIF는 접근하지 않는다.
class TrackingController {
public:
    using Clock=std::chrono::steady_clock;
    std::optional<PersonDetection> observe(const std::vector<PersonDetection>& people,
        int width,int height,Clock::time_point now=Clock::now());
    void tick(Clock::time_point now=Clock::now());
    void reset();
    TargetState state() const { return state_; }
    std::optional<TrackingCorrection> update(const PersonDetection& target,
        int inputWidth,int inputHeight,int outputWidth,int outputHeight,int frameAgeMs) const;
private:
    TargetSelector selector_;
    TargetState state_=TargetState::NoTarget;
    std::optional<Clock::time_point> lastSeen_;
};
