#pragma once
#include "hardware/servo_driver.h"
#include "motion/motion_plan.h"
#include <utility>
namespace ptz {
struct ServoMotionStatus { int pan, tilt; bool moving, fault; };
// 입력 출처와 무관한 이동 계획/출력 상태. 호출자는 한 mutex로 접근을 직렬화한다.
class PtzController {
public:
    explicit PtzController(ServoDriver& driver) : driver_(driver) {}
    void setPan(int angle);
    void setTilt(int angle);
    void center();
    void adjust(int panDelta, int tiltDelta);
    bool target(double pan, double tilt, bool relative);
    bool velocity(double panVelocity, double tiltVelocity, int timeoutMs);
    void stop();
    void update(MotionClock::time_point now=MotionClock::now());
    ServoMotionStatus status() const;
    std::pair<int,int> angles() const { return {pan_, tilt_}; }
private:
    ServoDriver& driver_;
    int pan_ = 90, tilt_ = 90;
    MotionPlan motion_;
    bool fault_ = false;
    bool write(unsigned gpio, int angle, int& current);
    void print() const;
};
} // namespace ptz
