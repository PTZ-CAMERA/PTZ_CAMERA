#pragma once
#include <algorithm>
namespace ptz {
// 하드웨어 경계. 제어 입력의 출처와 ONVIF를 알지 못한다.
class ServoDriver {
public:
    virtual ~ServoDriver() = default;
    virtual bool initialize() = 0;
    virtual bool write(unsigned gpio, int pulseUs) = 0;
    virtual void cleanup() = 0;
};
class PigpioServoDriver final : public ServoDriver {
public:
    bool initialize() override;
    bool write(unsigned gpio, int pulseUs) override;
    void cleanup() override;
private:
    bool initialized_ = false;
};
inline int angleToPulseWidth(int angle) {
    return 1000 + std::clamp(angle, 0, 180) * 1000 / 180;
}
} // namespace ptz
