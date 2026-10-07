/**
 * [학습 안내] 실제 CaptureManager 코드를 합성 source로 시험한다.
 * CSI/GPIO를 열지 않는다. 독립 MediaMTX가 받는 H.264 분기와 bounded BGR 분기를 함께 검사한다.
 */
#include "camera/capture_manager.h"
#include "config/app_config.h"
#include <cassert>
#include <chrono>
#include <iostream>
int main(int argc,char** argv) {
    assert(argc==2); gst_init(&argc,&argv);
    edge::appConfig().rtspPort=18555;
    CaptureManager capture(CaptureManager::Source::Synthetic);
    assert(capture.cameraState()==edge::CameraState::Stopped);
    assert(capture.start());
    int samples=0;
    const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(12);
    while (samples<15 && std::chrono::steady_clock::now()<limit) {
        auto* sample=gst_app_sink_try_pull_sample(capture.analysisSink(),200*GST_MSECOND);
        if (!sample) continue;
        AnalysisFrame frame(sample);
        assert(frame.image().cols==640 && frame.image().rows==360 && frame.image().type()==CV_8UC3);
        assert(GST_CLOCK_TIME_IS_VALID(frame.timestamp()));
        ++samples;
    }
    assert(samples==15 && capture.streamReady() && capture.cameraState()==edge::CameraState::Running);
    capture.stop();
    assert(capture.cameraState()==edge::CameraState::Stopped && !capture.streamReady());
    std::cout<<"PASS single synthetic source / H264 publishing / BGR latest frames / cleanup\n";
}
