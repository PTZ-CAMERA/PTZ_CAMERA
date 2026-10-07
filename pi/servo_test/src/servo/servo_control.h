#pragma once

#include <utility>
#include <cstdint>
#include "control/ptz_controller.h"

namespace ptz {
// 서보 출력과 공유 상태를 이 모듈에서 보호한다.
int angleToPulseWidth(int angle);
void setPanAngle(int angle);
void setTiltAngle(int angle);
void centerServos();
int currentPanAngle();
int currentTiltAngle();
std::pair<int, int> currentAngles();
void moveServosAutomatically(int panDelta, int tiltDelta);
bool moveServosAutomatically(int panDelta, int tiltDelta, std::uint64_t generation);
std::uint64_t trackingGeneration();
std::pair<bool, std::uint64_t> trackingState();
bool isTrackingRequested();
bool startTargetMove(double pan, double tilt, bool relative);
bool startVelocityMove(double panVelocity, double tiltVelocity, int timeoutMs);
void stopServoMotion();
bool startAutoTracking(bool moveFirst, double pan = 90, double tilt = 90);
void updateServoMotion();
ServoMotionStatus servoMotionStatus();
bool isTrackingEnabled();
void setTrackingEnabled(bool enabled);
bool isShuttingDown();
bool initializeGpio();
void cleanupGpio();
} // namespace ptz
