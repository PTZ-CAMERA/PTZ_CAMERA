/**
 * [학습 안내] 실제 GPIO 출력 계층
 * pigpio 초기화/출력 핀 설정/서보 pulse/종료만 담당한다.
 * 버튼은 별도 입력 모듈이며, 그 콜백을 제거한 다음 이 driver를 정리한다.
 * pulse=0은 출력 중지다. 1000~2000us의 일반 출력은 약 50Hz로 반복된다.
 */
#include "hardware/servo_driver.h"
#include "config.h"
#include "performance/performance_log.h"
#include <pigpio.h>
#include <algorithm>
#include <iostream>
namespace ptz {
// pigpio를 이 프로세스에서 한 번 열고 PAN/TILT 출력 핀을 구성한다.
bool PigpioServoDriver::initialize() {
    if (initialized_) return true;
    if (gpioInitialise() < 0) {
        std::cerr << "pigpio initialization failed; check permissions/other GPIO owner.\n";
        return false;
    }
    initialized_ = true;
    if (gpioSetMode(PAN_GPIO, PI_OUTPUT) < 0 || gpioSetMode(TILT_GPIO, PI_OUTPUT) < 0) {
        cleanup();
        return false;
    }
    return true;
}
// µs pulse를 GPIO에 쓰고 반환값을 확인한다. 호출부의 성능 이벤트도 연결한다.
bool PigpioServoDriver::write(unsigned gpio, int pulseUs) {
    if (!initialized_) return false;
    const auto begin = perf::enabled() ? perf::nowNs() : 0;
    const int result = gpioServo(gpio, pulseUs);
    if (perf::enabled())
        perf::record("gpio_write", begin, perf::nowNs(), gpio, pulseUs,
                     "result=" + std::to_string(result));
    if (result < 0)
        std::cerr << "gpioServo(GPIO" << gpio << ") failed: " << result << '\n';
    return result >= 0;
}
// pulse=0, 입력 모드 복원, pigpio 종료 순서로 하드웨어를 정리한다.
void PigpioServoDriver::cleanup() {
    if (!initialized_) return;
    gpioServo(PAN_GPIO, 0);
    gpioServo(TILT_GPIO, 0);
    gpioSetMode(PAN_GPIO, PI_INPUT);
    gpioSetMode(TILT_GPIO, PI_INPUT);
    gpioTerminate();
    initialized_ = false;
}
} // namespace ptz
