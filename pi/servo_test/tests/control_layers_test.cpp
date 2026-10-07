/**
 * [학습 안내] GPIO 없이 Controller/Arbiter 경계를 검증한다.
 * 모드 세대가 바뀌면 이전 AI 명령을 거부하고 수동 입력이 자동 입력을 취소하는지 확인한다.
 * 잘못된 명령, 출력 실패와 이동 후 추적 예약 취소를 검증한다. 모터를 실제 구동하지 않는다.
 */
#include "control/control_arbiter.h"
#include "config.h"
#include <array>
#include <cassert>
#include <limits>
namespace {
class FakeDriver final : public ptz::ServoDriver {
public:
    std::array<int,54> pulses{};
    bool fail=false;
    bool initialize() override { return true; }
    bool write(unsigned pin,int pulse) override {
        if (fail) return false;
        pulses.at(pin)=pulse; return true;
    }
    void cleanup() override {}
};
}
int main() {
    using namespace ptz;
    FakeDriver driver;
    PtzController controller(driver);
    ControlArbiter arbiter(controller);
    controller.center();
    assert(driver.pulses[18]==1500 && driver.pulses[19]==1500);
    controller.setPan(999); controller.setTilt(-999);
    assert(controller.status().pan==155 && controller.status().tilt==40);
    controller.center();
    arbiter.enable(true);
    const auto old=arbiter.generation();
    assert(arbiter.automatic(1,-1,old));
    assert(controller.status().pan==91 && controller.status().tilt==89);
    arbiter.manual(); controller.setPan(100);
    arbiter.enable(true);
    assert(!arbiter.automatic(3,3,old));
    assert(controller.status().pan==100);
    assert(arbiter.automatic(1,0,arbiter.generation()));
    assert(!controller.velocity(1,0,99));
    assert(!controller.target(std::numeric_limits<double>::quiet_NaN(),90,false));
    assert(!arbiter.startTracking(true,999,90));
    assert(arbiter.startTracking(true,120,100));
    assert(arbiter.requested() && !arbiter.tracking() && controller.status().moving);
    arbiter.manual(); controller.stop(); arbiter.update();
    assert(!arbiter.requested());
    assert(arbiter.startTracking(true,101,89));
    assert(arbiter.tracking()); // 현재 명령 위치와 동일한 목표는 즉시 추적 허용.
    driver.fail=true;
    arbiter.automatic(1,0,arbiter.generation());
    assert(controller.status().fault && controller.status().pan==101 && !arbiter.tracking());
}
