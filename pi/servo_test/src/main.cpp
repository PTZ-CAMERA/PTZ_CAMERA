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

constexpr unsigned PAN_GPIO = 18;
constexpr unsigned TILT_GPIO = 19;
constexpr unsigned CENTER_BUTTON_GPIO = 17;
constexpr int PAN_MIN_ANGLE = 25;
constexpr int PAN_MAX_ANGLE = 155;
constexpr int TILT_MIN_ANGLE = 40;
constexpr int TILT_MAX_ANGLE = 140;
constexpr int STEP_DEGREES = 5;
constexpr unsigned BUTTON_GLITCH_US = 20000;   // Require 20 ms of stable input.
constexpr uint32_t BUTTON_DEBOUNCE_US = 200000; // Ignore repeated presses for 200 ms.

std::mutex controlMutex;
int panAngle = 90;
int tiltAngle = 90;
bool buttonSeen = false;
uint32_t lastButtonTick = 0;
std::atomic<bool> shuttingDown{false};
volatile std::sig_atomic_t stopRequested = 0;

void onSignal(int) {
    stopRequested = 1;
}

void printAnglesLocked() {
    std::cout << "PAN: " << panAngle << " deg\n"
              << "TILT: " << tiltAngle << " deg\n" << std::flush;
}

bool writeServoLocked(unsigned gpio, int angle, int& currentAngle) {
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
    angle = std::clamp(angle, 0, 180);
    return 1000 + angle * 1000 / 180;
}

void setPanAngle(int angle) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (shuttingDown.load()) return;
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
    std::lock_guard<std::mutex> lock(controlMutex);
    if (shuttingDown.load()) return;
    writeServoLocked(PAN_GPIO, 90, panAngle);
    writeServoLocked(TILT_GPIO, 90, tiltAngle);
    printAnglesLocked();
}

void handleCenterButton(int gpio, int level, uint32_t tick, void*) {
    if (gpio != static_cast<int>(CENTER_BUTTON_GPIO) || level != PI_LOW ||
        shuttingDown.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(controlMutex);
        // Unsigned subtraction remains correct when pigpio's 32-bit tick wraps.
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
    shuttingDown.store(true);
    {
        std::lock_guard<std::mutex> lock(controlMutex);
        gpioServo(PAN_GPIO, 0);  // 0 disables servo pulses.
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
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(STDIN_FILENO, &readFds);
        timeval timeout{0, 100000}; // Wake regularly to process SIGINT.
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
    tcsetattr(STDIN_FILENO, TCSANOW, &savedTerminal);
    std::cout << "Servo pulses stopped.\n";
    return 0;
}
