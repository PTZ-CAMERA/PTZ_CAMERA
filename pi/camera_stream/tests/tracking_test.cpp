/** [학습 안내] 대상 선택과 좌표/보정 계산을 이미지·GPIO 없이 검증한다. */
#include "tracking/target_selector.h"
#include "tracking/tracking_controller.h"
#include <cassert>
int main() {
    TargetSelector selector;
    TrackingController tracking;
    std::vector<PersonDetection> people{{cv::Rect(20,100,40,100),.9f},
                                        {cv::Rect(280,130,80,100),.8f}};
    auto chosen=selector.select(people,640,360);
    assert(chosen && chosen->box.x==280);
    auto centered=tracking.update(*chosen,640,360,1280,720,300);
    assert(centered && centered->panDelta==0 && centered->tiltDelta==0);
    assert(centered->box==cv::Rect(560,260,160,200));
    assert(!tracking.update(*chosen,640,360,1280,720,601));
    auto left=tracking.update(people.front(),640,360,1280,720,300);
    assert(left && left->panDelta==-3 && left->tiltDelta>0);
    assert(!selector.select({},640,360));
    assert(!tracking.update(*chosen,0,360,1280,720,300));
    const auto now=TrackingController::Clock::now();
    assert(tracking.state()==TargetState::NoTarget);
    assert(tracking.observe(people,640,360,now)); assert(tracking.state()==TargetState::Acquired);
    assert(tracking.observe(people,640,360,now+std::chrono::milliseconds(100)));
    assert(tracking.state()==TargetState::Tracking);
    assert(!tracking.observe({},640,360,now+std::chrono::milliseconds(500)));
    assert(tracking.state()==TargetState::Tracking);
    tracking.tick(now+std::chrono::milliseconds(1101)); assert(tracking.state()==TargetState::Lost);
    assert(tracking.observe(people,640,360,now+std::chrono::milliseconds(1200)));
    assert(tracking.state()==TargetState::Acquired);
}
