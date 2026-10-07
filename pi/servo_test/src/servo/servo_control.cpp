/**
 * [학습 안내] 기존 호출부를 보존하는 adapter
 * 함수 이름/IPC는 유지하면서 정책(Arbiter), 이동(Controller), GPIO(Driver)를 연결한다.
 * main과 버튼 callback이 공유하는 mutex를 이 경계에서 잡는다.
 * Controller/Driver에는 ONVIF나 자동 추적 출처에 대한 의존성이 없다.
 */
#include "servo/servo_control.h"
#include "control/control_arbiter.h"
#include "button/center_button.h"
#include <atomic>
#include <mutex>
namespace ptz {
namespace {
std::mutex controlMutex;
PigpioServoDriver driver;
PtzController controller(driver);
ControlArbiter arbiter(controller);
std::atomic<bool> shuttingDown{false};
}
void setPanAngle(int angle) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (shuttingDown) return;
    arbiter.manual(); controller.setPan(angle);
}
void setTiltAngle(int angle) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (shuttingDown) return;
    arbiter.manual(); controller.setTilt(angle);
}
void centerServos() {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (shuttingDown) return;
    arbiter.manual(); controller.center();
}
std::pair<int,int> currentAngles() {
    std::lock_guard<std::mutex> lock(controlMutex); return controller.angles();
}
int currentPanAngle() { return currentAngles().first; }
int currentTiltAngle() { return currentAngles().second; }
bool isTrackingEnabled() {
    std::lock_guard<std::mutex> lock(controlMutex); return arbiter.tracking();
}
bool isTrackingRequested() {
    std::lock_guard<std::mutex> lock(controlMutex); return arbiter.requested();
}
std::uint64_t trackingGeneration() {
    std::lock_guard<std::mutex> lock(controlMutex); return arbiter.generation();
}
std::pair<bool, std::uint64_t> trackingState() {
    std::lock_guard<std::mutex> lock(controlMutex);
    return {arbiter.tracking(), arbiter.generation()};
}
void setTrackingEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (!shuttingDown) arbiter.enable(enabled);
}
bool isShuttingDown() { return shuttingDown.load(); }
void moveServosAutomatically(int dx, int dy) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (!shuttingDown) arbiter.automatic(dx, dy, arbiter.generation());
}
bool moveServosAutomatically(int dx, int dy, std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(controlMutex);
    return !shuttingDown && arbiter.automatic(dx, dy, generation);
}
bool startTargetMove(double pan, double tilt, bool relative) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (shuttingDown || !controller.target(pan, tilt, relative)) return false;
    arbiter.manual(); return true;
}
bool startVelocityMove(double pan, double tilt, int timeoutMs) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (shuttingDown || !controller.velocity(pan, tilt, timeoutMs)) return false;
    arbiter.manual(); return true;
}
bool startAutoTracking(bool moveFirst, double pan, double tilt) {
    std::lock_guard<std::mutex> lock(controlMutex);
    return !shuttingDown && arbiter.startTracking(moveFirst, pan, tilt);
}
void stopServoMotion() {
    std::lock_guard<std::mutex> lock(controlMutex);
    arbiter.manual(); controller.stop();
}
void updateServoMotion() {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (!shuttingDown) arbiter.update();
}
ServoMotionStatus servoMotionStatus() {
    std::lock_guard<std::mutex> lock(controlMutex); return controller.status();
}
bool initializeGpio() {
    shuttingDown = false;
    if (!driver.initialize()) return false;
    if (!configureCenterButton()) { cleanupGpio(); return false; }
    return true;
}
void cleanupGpio() {
    // callback을 먼저 막고 등록 해제한다. mutex를 잡은 상태에서 콜백 종료를 기다리지 않는다.
    shuttingDown = true;
    cleanupCenterButton();
    std::lock_guard<std::mutex> lock(controlMutex);
    arbiter.manual(); controller.stop(); driver.cleanup();
}
} // namespace ptz
