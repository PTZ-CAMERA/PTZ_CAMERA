#include <pigpio.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <mutex>

#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

int angleToPulseWidth(int angle);

namespace {

// BCM 번호 기준. 서보 전원은 외부 4.8~6V를 쓰고 Pi와 전원 GND를 공통 연결한다.
constexpr unsigned PAN_GPIO = 18;
constexpr unsigned TILT_GPIO = 19;
constexpr unsigned CENTER_BUTTON_GPIO = 17;
// 기구가 끝에 부딪히지 않도록 두 축의 이동 범위를 따로 제한한다.
constexpr int PAN_MIN_ANGLE = 25;
constexpr int PAN_MAX_ANGLE = 155;
constexpr int TILT_MIN_ANGLE = 40;
constexpr int TILT_MAX_ANGLE = 140;
constexpr int STEP_DEGREES = 5;
constexpr unsigned BUTTON_GLITCH_US = 20000;   // 입력이 20ms 동안 안정적일 때만 전달.
constexpr uint32_t BUTTON_DEBOUNCE_US = 200000; // 버튼을 다시 누르기까지 200ms 무시.

// 키 입력과 pigpio 버튼 콜백은 서로 다른 실행 흐름이므로 각도와 GPIO 쓰기를 보호한다.
std::mutex controlMutex;
int panAngle = 90;
int tiltAngle = 90;
bool buttonSeen = false;
uint32_t lastButtonTick = 0;
std::atomic<bool> shuttingDown{false};
volatile std::sig_atomic_t stopRequested = 0;

void onSignal(int) {
    // 시그널 핸들러에서는 안전한 플래그 변경만 하고 정리는 메인 루프가 수행한다.
    stopRequested = 1;
}

void printAnglesLocked() {
    std::cout << "PAN: " << panAngle << " deg\n"
              << "TILT: " << tiltAngle << " deg\n" << std::flush;
}

bool writeServoLocked(unsigned gpio, int angle, int& currentAngle) {
    // gpioServo()의 값은 주기가 약 20ms인 서보 펄스의 HIGH 폭(µs)이다.
    const int pulse = angleToPulseWidth(angle);
    const int result = gpioServo(gpio, pulse);
    if (result < 0) {
        std::cerr << "gpioServo(GPIO" << gpio << ") failed: " << result << '\n';
        return false;
    }
    currentAngle = angle;
    return true;
}

} // namespace

int angleToPulseWidth(int angle) {
    // 0°=1000µs, 90°=1500µs, 180°=2000µs로 선형 변환한다.
    angle = std::clamp(angle, 0, 180);
    return 1000 + angle * 1000 / 180;
}

void setPanAngle(int angle) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (shuttingDown.load()) return;
    // 펄스 변환보다 먼저 실제 장착 상태에 맞춘 PAN 제한을 적용한다.
    angle = std::clamp(angle, PAN_MIN_ANGLE, PAN_MAX_ANGLE);
    writeServoLocked(PAN_GPIO, angle, panAngle);
    printAnglesLocked();
}

void setTiltAngle(int angle) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (shuttingDown.load()) return;
    angle = std::clamp(angle, TILT_MIN_ANGLE, TILT_MAX_ANGLE);
    writeServoLocked(TILT_GPIO, angle, tiltAngle);
    printAnglesLocked();
}

void centerServos() {
    // 두 축을 같은 잠금 아래 갱신해 버튼·키 입력이 중간에 섞이지 않게 한다.
    std::lock_guard<std::mutex> lock(controlMutex);
    if (shuttingDown.load()) return;
    writeServoLocked(PAN_GPIO, 90, panAngle);
    writeServoLocked(TILT_GPIO, 90, tiltAngle);
    printAnglesLocked();
}

void handleCenterButton(int gpio, int level, uint32_t tick, void*) {
    // 풀업 입력: 평소 HIGH, GPIO17과 GND가 연결되면 LOW(눌림).
    if (gpio != static_cast<int>(CENTER_BUTTON_GPIO) || level != PI_LOW ||
        shuttingDown.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(controlMutex);
        // 부호 없는 뺄셈은 pigpio의 32비트 tick이 순환해도 간격을 계산한다.
        if (buttonSeen && uint32_t(tick - lastButtonTick) < BUTTON_DEBOUNCE_US) {
            return;
        }
        buttonSeen = true;
        lastButtonTick = tick;
    }
    centerServos();
}

namespace {

int currentPanAngle() {
    std::lock_guard<std::mutex> lock(controlMutex);
    return panAngle;
}

int currentTiltAngle() {
    std::lock_guard<std::mutex> lock(controlMutex);
    return tiltAngle;
}

void cleanupGpio() {
    // 먼저 콜백의 새 동작을 막고 서보 펄스를 끈 뒤 GPIO 설정을 해제한다.
    shuttingDown.store(true);
    {
        std::lock_guard<std::mutex> lock(controlMutex);
        gpioServo(PAN_GPIO, 0);  // 0은 서보 펄스 출력 중지.
        gpioServo(TILT_GPIO, 0);
    }
    gpioSetAlertFuncEx(CENTER_BUTTON_GPIO, nullptr, nullptr);
    gpioGlitchFilter(CENTER_BUTTON_GPIO, 0);
    gpioSetPullUpDown(CENTER_BUTTON_GPIO, PI_PUD_OFF);
    gpioSetMode(PAN_GPIO, PI_INPUT);
    gpioSetMode(TILT_GPIO, PI_INPUT);
    gpioTerminate();
}

bool configureGpio() {
    // 버튼에는 내부 풀업과 pigpio 글리치 필터를 적용하고 콜백을 등록한다.
    return gpioSetMode(PAN_GPIO, PI_OUTPUT) >= 0 &&
           gpioSetMode(TILT_GPIO, PI_OUTPUT) >= 0 &&
           gpioSetMode(CENTER_BUTTON_GPIO, PI_INPUT) >= 0 &&
           gpioSetPullUpDown(CENTER_BUTTON_GPIO, PI_PUD_UP) >= 0 &&
           gpioGlitchFilter(CENTER_BUTTON_GPIO, BUTTON_GLITCH_US) >= 0 &&
           gpioSetAlertFuncEx(CENTER_BUTTON_GPIO, handleCenterButton, nullptr) >= 0;
}

} // namespace

int main() {
    if (gpioInitialise() < 0) {
        std::cerr << "pigpio initialization failed (try sudo).\n";
        return 1;
    }
    if (!configureGpio()) {
        std::cerr << "GPIO setup failed.\n";
        cleanupGpio();
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    centerServos();

    // 터미널의 줄 입력·에코를 끄면 Enter 없이 a/d/w/s/c/q를 받을 수 있다.
    termios savedTerminal{};
    if (tcgetattr(STDIN_FILENO, &savedTerminal) != 0) {
        std::cerr << "A terminal is required for keyboard control.\n";
        cleanupGpio();
        return 1;
    }
    termios keyTerminal = savedTerminal;
    keyTerminal.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    keyTerminal.c_cc[VMIN] = 0;
    keyTerminal.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &keyTerminal) != 0) {
        std::cerr << "Could not enable single-key input.\n";
        cleanupGpio();
        return 1;
    }

    std::cout << "a/d: PAN  w/s: TILT  c: center  q: quit\n" << std::flush;
    while (!stopRequested) {
        // 짧은 대기마다 SIGINT 플래그를 확인해 Ctrl+C에도 종료 처리를 수행한다.
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(STDIN_FILENO, &readFds);
        timeval timeout{0, 100000}; // 최대 100ms 대기.
        const int ready = select(STDIN_FILENO + 1, &readFds, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        char key = 0;
        if (read(STDIN_FILENO, &key, 1) != 1) continue;
        switch (key) {
            case 'a': setPanAngle(currentPanAngle() - STEP_DEGREES); break;
            case 'd': setPanAngle(currentPanAngle() + STEP_DEGREES); break;
            case 'w': setTiltAngle(currentTiltAngle() + STEP_DEGREES); break;
            case 's': setTiltAngle(currentTiltAngle() - STEP_DEGREES); break;
            case 'c': centerServos(); break;
            case 'q': stopRequested = 1; break;
            default: break;
        }
    }

    cleanupGpio();
    // 종료할 때 터미널 입력 설정을 원래대로 돌린다.
    tcsetattr(STDIN_FILENO, TCSANOW, &savedTerminal);
    std::cout << "Servo pulses stopped.\n";
    return 0;
}
