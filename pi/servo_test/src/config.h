#pragma once

#include "control/ptz_limits.h"
#include <cstddef>
#include <cstdint>

namespace ptz {
// BCM 번호. 서보 전원은 외부 공급하고 Pi와 GND를 공통 연결한다.
constexpr unsigned PAN_GPIO = 18;
constexpr unsigned TILT_GPIO = 19;
constexpr unsigned CENTER_BUTTON_GPIO = 17;
// 기구가 끝에 부딪히지 않도록 두 축의 이동 범위를 따로 제한한다.
constexpr int PAN_MIN_ANGLE = geometry::panMin;
constexpr int PAN_MAX_ANGLE = geometry::panMax;
constexpr int TILT_MIN_ANGLE = geometry::tiltMin;
constexpr int TILT_MAX_ANGLE = geometry::tiltMax;
constexpr int STEP_DEGREES = 5;
constexpr double TARGET_SPEED_DEG_PER_SEC = geometry::targetSpeed; // 절대/상대 이동의 고정 목표 갱신 속도.
constexpr double MAX_VELOCITY_DEG_PER_SEC = geometry::maxVelocity; // 정규화 연속 속도 1의 목표 갱신 속도.
constexpr uint16_t CONTROL_PORT = 5000;
constexpr std::size_t MAX_CLIENTS = 8;
constexpr std::size_t MAX_COMMAND_LENGTH = 1024;
constexpr unsigned BUTTON_GLITCH_US = 20000;   // 입력이 20ms 동안 안정적일 때만 전달.
constexpr uint32_t BUTTON_DEBOUNCE_US = 200000; // 버튼을 다시 누르기까지 200ms 무시.

} // namespace ptz
