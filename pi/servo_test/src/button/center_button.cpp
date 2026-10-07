/**
 * [학습 안내] GPIO17 중앙 복귀 버튼 입력
 * 내부 pull-up의 active-low 신호에 설정된 glitch filter와 debounce 중복 눌림 방지를 적용한다.
 * pigpio alert 콜백 스레드에서 실행되므로 debounce 상태와 서보 상태의 잠금 책임을 분리한다.
 * 유효 눌림은 자동 추적/기존 궤적을 취소하고 두 축에 90도 명령을 출력한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "performance/performance_log.h"
#include "button/center_button.h"
#include "servo/servo_control.h"
#include "config.h"

#include <pigpio.h>
#include <mutex>

namespace ptz {
namespace {
std::mutex buttonMutex;
bool buttonSeen = false;
uint32_t lastButtonTick = 0;
} // namespace

/**
 * 유효 active-low 입력만 debounce 검사 뒤 수동 중앙 복귀로 처리한다. 종료 중 호출은 무시한다.
 */
void handleCenterButton(int gpio, int level, uint32_t tick, void *) {
    // 풀업 입력: 평소 HIGH, GPIO17과 GND가 연결되면 LOW(눌림).
    if (gpio != static_cast<int>(CENTER_BUTTON_GPIO) || level != PI_LOW || isShuttingDown()) {
        return;
    }
    perf::Scope buttonTiming("button_callback");
    {
        std::lock_guard<std::mutex> lock(buttonMutex);
        // 부호 없는 뺄셈은 pigpio의 32비트 tick이 순환해도 간격을 계산한다.
        if (buttonSeen && uint32_t(tick - lastButtonTick) < buttonDebounceUs()) {
            perf::instant("button_debounce_ignored");
            return;
        }
        buttonSeen = true;
        lastButtonTick = tick;
    }
    setTrackingEnabled(false);
    centerServos();
}

/**
 * 입력/pull-up/glitch filter/alert callback을 등록하고 API 반환값으로 성공 여부를 보고한다.
 */
bool configureCenterButton() {
    // 설정된 글리치 필터 뒤 콜백에서 debounce 중복 눌림을 걸러낸다.
    return gpioSetMode(CENTER_BUTTON_GPIO, PI_INPUT) >= 0 &&
           gpioSetPullUpDown(CENTER_BUTTON_GPIO, PI_PUD_UP) >= 0 &&
           gpioGlitchFilter(CENTER_BUTTON_GPIO, buttonGlitchUs()) >= 0 &&
           gpioSetAlertFuncEx(CENTER_BUTTON_GPIO, handleCenterButton, nullptr) >= 0;
}

/**
 * 새 콜백을 제거하고 필터/pull-up을 해제한다. 모터 출력 정리는 별도 GPIO cleanup이 담당한다.
 */
void cleanupCenterButton() {
    gpioSetAlertFuncEx(CENTER_BUTTON_GPIO, nullptr, nullptr);
    gpioGlitchFilter(CENTER_BUTTON_GPIO, 0);
    gpioSetPullUpDown(CENTER_BUTTON_GPIO, PI_PUD_OFF);
}
} // namespace ptz
