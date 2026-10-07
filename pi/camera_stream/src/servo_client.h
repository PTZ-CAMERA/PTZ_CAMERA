#pragma once

#include <string>
#include "metadata/detection_metadata.h"

// 카메라 송출기는 GPIO를 직접 건드리지 않는다. 이 연결을 통해 서보 서비스에
// 추적 상태를 묻고, 최신 탐지 결과에 대응하는 이동 명령만 보낸다.
class ServoClient {
public:
    ~ServoClient();
    struct TrackingState { bool enabled = false; std::uint64_t generation = 0; };
    TrackingState trackingState();
    bool trackingEnabled();
    void move(int panDelta, int tiltDelta, std::uint64_t generation);
    void detection(const ptz::DetectionMetadata& metadata);
    void move(int panDelta, int tiltDelta);
    void detection(float confidence, int x, int y, int errorX, int errorY);
    void noDetection();

private:
    bool connectIfNeeded();
    bool sendLine(const std::string& line);
    std::string readLine();
    void disconnect();
    int fd_ = -1;
    std::string incoming_;
};
