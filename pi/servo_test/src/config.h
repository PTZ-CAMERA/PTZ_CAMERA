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
inline const int& PAN_MIN_ANGLE = geometry::panMin;
inline const int& PAN_MAX_ANGLE = geometry::panMax;
inline const int& TILT_MIN_ANGLE = geometry::tiltMin;
inline const int& TILT_MAX_ANGLE = geometry::tiltMax;
constexpr int STEP_DEGREES = 5;
inline const double& TARGET_SPEED_DEG_PER_SEC = geometry::targetSpeed; // 절대/상대 이동의 고정 목표 갱신 속도.
inline const double& MAX_VELOCITY_DEG_PER_SEC = geometry::maxVelocity; // 정규화 연속 속도 1의 목표 갱신 속도.
inline const int& CONTROL_PORT = edge::appConfig().controlPort;
constexpr std::size_t MAX_CLIENTS = 8;
constexpr std::size_t MAX_COMMAND_LENGTH = 1024;
inline unsigned buttonGlitchUs() { return edge::appConfig().ptz.buttonGlitchMs*1000; }   // 설정된 시간 동안 안정적인 입력만 전달.
inline unsigned buttonDebounceUs() { return edge::appConfig().ptz.buttonDebounceMs*1000; } // 30~100ms로 검증된 중복 눌림 제한.

} // namespace ptz
