/**
 * [학습 안내] 카메라 송출과 사람 자동 추적의 실행 진입점
 * 카메라를 한 번 열어 tee에서 송출과 압축 전 분석 영상으로 나눈다.
 * GLib 메인 루프는 오류/종료를 감시하고 별도 std::thread가 OpenCV 추론을 수행한다.
 * 분석은 최신 대기 프레임을 선택하고 설정 maxFrameAgeMs를 초과한 결과로 이동하지 않는다.
 * 추론 worker가 카메라 UI/녹화/검색을 담당하지 않으며 이동은 ServoClient의 IPC로 전달한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "performance/gstreamer_metrics.h"
#include "performance/performance_log.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <glib-unix.h>
#include <opencv2/imgcodecs.hpp>

#include "person_detector.h"
#include "camera/capture_manager.h"
#include "config/app_config.h"
#include "runtime/log.h"
#include "runtime/rate_limiter.h"
#include <memory>
#include "servo_client.h"
#include "tracking/target_selector.h"
#include "tracking/tracking_controller.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

// 이 주소로 발행한 영상을 MediaMTX가 RTSP·WebRTC·HLS 시청자에게 전달한다.
std::string rtspUrl() { return "rtsp://127.0.0.1:"+std::to_string(edge::appConfig().rtspPort)+"/"+edge::appConfig().rtspPath; }
constexpr const char *kModelPrototxt = "/usr/local/share/ptz-camera/deploy.prototxt";
constexpr const char *kModelWeights = "/usr/local/share/ptz-camera/mobilenet_iter_73000.caffemodel";
constexpr int kAnalysisWidth = 640;
constexpr int kAnalysisHeight = 360;
constexpr int kAnalysisInputFps = 10;
constexpr int kDnnThreads = 2; // 송출과 동시 실행에서 3스레드보다 안정적인 지연을 보였다.
constexpr int kStreamWidth = 1280;
constexpr int kStreamHeight = 720;
GMainLoop *main_loop = nullptr;
bool stream_failed = false;

/**
 * 종료 신호를 GLib/main 루프 종료로 연결한다. GPIO 정리나 복잡한 I/O는 신호 핸들러 안에서 하지 않는다.
 */
gboolean handleSignal(gpointer) {
    // Ctrl+C와 systemd의 종료 신호를 같은 경로로 처리한다.
    g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

/**
 * GStreamer ERROR/EOS를 받아 오류 표시 후 메인 루프를 종료한다. 정상 종료와 송출 실패 결과를 구분한다.
 */
gboolean handleBusMessage(GstBus *, GstMessage *message, gpointer owner) {
    // 카메라·인코더·RTSP 발행 중 어느 단계에서든 오류가 나면 종료한다.
    // 시청자가 남아 있으면 MediaMTX의 runOnDemandRestart가 송출기를 다시 실행한다.
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        std::cerr << "GStreamer error from " << GST_OBJECT_NAME(message->src) << ": "
                  << (error ? error->message : "unknown") << '\n';
        if (debug)
            std::cerr << debug << '\n';
        g_clear_error(&error);
        g_free(debug);
        static_cast<CaptureManager*>(owner)->markError();
        stream_failed = true;
        g_main_loop_quit(main_loop);
        return G_SOURCE_CONTINUE;
    }
    case GST_MESSAGE_EOS:
        std::cerr << "Camera stream ended\n";
        static_cast<CaptureManager*>(owner)->markError();
        stream_failed = true;
        g_main_loop_quit(main_loop);
        return G_SOURCE_CONTINUE;
    default:
        return G_SOURCE_CONTINUE;
    }
}

/**
 * 최신 샘플을 빌려 추론한다. 영상 worker와 독립이고 metadata만 rate limit한다.
 * 수동 중에는 출력 gate가 닫힌다. 설정에 따라 탐지만 유지할 수 있다.
 * 모델 실패는 상태/주기 로그로 알리고 재시도한다. RTSP pipeline은 계속 실행한다.
 */
void trackingLoop(CaptureManager& capture,std::atomic<bool>& stopping) {
    const auto& config=edge::appConfig();
    auto* pipeline=capture.pipeline();
    auto* sink=capture.analysisSink();
    ServoClient servo;
    TrackingController tracking;
    edge::RateLimiter metadataRate(config.tracking.metadataHz);
    std::unique_ptr<PersonDetector> detector;
    auto modelRetry=std::chrono::steady_clock::time_point{};
    auto lastReport=std::chrono::steady_clock::now();
    std::string lastCameraState,lastStreamState,lastTargetState;
    std::uint64_t generation=0,frameId=0;
    const auto epoch=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    bool noDetectionSent=false;
    try {
        int dnnThreads=kDnnThreads;
        if (const char* value=std::getenv("PTZ_DNN_THREADS")) {
            const std::string option(value);
            if (option!="1"&&option!="2"&&option!="3") throw std::runtime_error("PTZ_DNN_THREADS must be 1/2/3");
            dnnThreads=std::stoi(option);
        }
        cv::setNumThreads(dnnThreads);
        ptz::perf::instant("tracking_worker_start",dnnThreads,kAnalysisInputFps);
        while (!stopping.load()) {
            const auto state=servo.trackingState();
            const std::string cameraState=edge::cameraStateName(capture.cameraState());
            const std::string streamState=edge::streamStateName(capture.streamState());
            if (streamState!=lastStreamState) { servo.streamState(streamState); lastStreamState=streamState; }
            // 주기적 상태도 보내 재접속한 feed가 최신 캡처 상태를 복원하게 한다.
            if (cameraState!=lastCameraState) { servo.cameraState(cameraState); lastCameraState=cameraState; }
            if (state.generation!=generation) {
                generation=state.generation; tracking.reset(); metadataRate.reset();
                noDetectionSent=false; lastTargetState.clear();
                servo.cameraState(cameraState); servo.streamState(streamState);
            }
            if (!state.enabled&&!config.tracking.detectionWhenDisabled) {
                tracking.reset();
                if (!noDetectionSent) { servo.noDetection(); noDetectionSent=true; }
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                continue;
            }
            const auto sendTarget=[&] {
                const std::string name=targetStateName(tracking.state());
                if (name!=lastTargetState) {
                    servo.targetState(name,generation); lastTargetState=name;
                    if (tracking.state()==TargetState::Acquired || tracking.state()==TargetState::Lost) metadataRate.reset();
                }
            };
            tracking.tick(); sendTarget();
            if (!detector) {
                if (std::chrono::steady_clock::now()<modelRetry) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(150)); continue;
                }
                try {
                    detector=std::make_unique<PersonDetector>(kModelPrototxt,kModelWeights);
                    edge::log("DNN","MobileNet-SSD ready; threads="+std::to_string(dnnThreads));
                } catch (const std::exception&) {
                    edge::log("DNN","Model unavailable; retry in 5 seconds");
                    servo.targetState("ERROR",generation);
                    modelRetry=std::chrono::steady_clock::now()+std::chrono::seconds(5); continue;
                }
            }
            auto* sample=gst_app_sink_try_pull_sample(sink,200*GST_MSECOND);
            if (!sample) { tracking.tick(); sendTarget(); continue; }
            GstClockTime pts=GST_CLOCK_TIME_NONE;
            int inputAge=-1;
            long long captureUnixMs=0;
            std::vector<PersonDetection> people;
            auto begin=std::chrono::steady_clock::now(),end=begin;
            try {
                // borrowed Mat는 이 블록 안에서만 사용한다. 통신 전 mapping을 해제한다.
                AnalysisFrame frame(sample);
                pts=frame.timestamp(); inputAge=frameAgeMs(pipeline,pts); ++frameId;
                const auto received=std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                captureUnixMs=inputAge>=0 ? received-inputAge : 0;
                begin=std::chrono::steady_clock::now();
                people=detector->detect(frame.image());
                end=std::chrono::steady_clock::now();
            } catch (const std::exception&) {
                edge::log("DNN","Analysis failed; retry model in 5 seconds");
                servo.targetState("ERROR",generation); detector.reset();
                modelRetry=std::chrono::steady_clock::now()+std::chrono::seconds(5); continue;
            }
            const int age=frameAgeMs(pipeline,pts);
            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
            if (ptz::perf::enabled()) {
                const auto b=std::chrono::duration_cast<std::chrono::nanoseconds>(begin.time_since_epoch()).count();
                const auto e=std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count();
                ptz::perf::record("inference",b,e,inputAge,age,"pts="+std::to_string(pts));
                ptz::perf::instant("detection_result",people.size(),age);
            }
            if (end-lastReport>=std::chrono::seconds(5)) {
                edge::log("DNN","inference="+std::to_string(elapsed)+"ms frame_age="+std::to_string(age)+
                    "ms people="+std::to_string(people.size()));
                servo.cameraState(cameraState); lastReport=end;
            }
            if (age>config.tracking.maxFrameAgeMs) {
                ptz::perf::instant("stale_result",age); tracking.tick(); sendTarget(); continue;
            }
            const auto current=servo.trackingState();
            if (current.generation!=generation || (!current.enabled&&!config.tracking.detectionWhenDisabled)) continue;
            const auto target=tracking.observe(people,kAnalysisWidth,kAnalysisHeight);
            sendTarget();
            if (!target) {
                if (!noDetectionSent) { servo.noDetection(); noDetectionSent=true; }
                continue;
            }
            noDetectionSent=false;
            const auto correction=tracking.update(*target,kAnalysisWidth,kAnalysisHeight,kStreamWidth,kStreamHeight,age);
            if (!correction) continue;
            const auto& r=*correction;
            if (current.enabled&&(r.panDelta||r.tiltDelta)) servo.move(r.panDelta,r.tiltDelta,generation);
            if (!metadataRate.ready()) continue;
            ptz::DetectionMetadata metadata;
            metadata.confidence=target->confidence; metadata.x=r.x; metadata.y=r.y;
            metadata.errorX=r.errorX; metadata.errorY=r.errorY;
            metadata.boxX=r.box.x; metadata.boxY=r.box.y; metadata.boxWidth=r.box.width; metadata.boxHeight=r.box.height;
            metadata.imageWidth=kStreamWidth; metadata.imageHeight=kStreamHeight;
            metadata.frameId=frameId; metadata.framePtsNs=GST_CLOCK_TIME_IS_VALID(pts)?static_cast<std::int64_t>(pts):-1;
            metadata.captureUnixMs=captureUnixMs;
            metadata.analysisUnixMs=std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            metadata.streamEpoch=epoch; metadata.generation=generation;
            servo.detection(metadata);
        }
    } catch (const std::exception& error) {
        edge::log("TRACK",std::string("Worker error: ")+error.what());
        servo.targetState("ERROR",generation);
    }
    servo.streamState(capture.streamState()==edge::StreamState::Error ? "ERROR" : "STOPPED");
    servo.cameraState(capture.cameraState()==edge::CameraState::Error ? "ERROR" : "STOPPED");
}

/**
 * GLib 주기 콜백으로 queue 현재 깊이를 기록한다. 주기 관측이므로 관측 사이의 최대값을 보장하지 않는다.
 */
gboolean performanceQueueTimer(gpointer pipeline) {
    ptz::perf::recordQueueLevels(static_cast<GstElement *>(pipeline));
    return G_SOURCE_CONTINUE;
}

// 버퍼 이벤트 시점은 Pi 파이프라인 내부 기준이다. Qt 도착 시간이 아니다.
/**
 * 해당 pad를 지나는 버퍼의 시간/PTS를 기록한다. 버퍼를 수정하거나 Qt 도착 시간을 측정하지 않는다.
 */
GstPadProbeReturn performanceProbe(GstPad *, GstPadProbeInfo *info, gpointer eventName) {
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer)
        ptz::perf::instant(static_cast<const char *>(eventName), GST_BUFFER_PTS(buffer));
    return GST_PAD_PROBE_OK;
}
/**
 * 계측이 켜진 실행에만 pad probe를 붙인다. 정상 동작에서 추가 CSV 비용을 피하도록 조건을 둔다.
 */
void addPerformanceProbe(GstElement *element, const char *padName, const char *eventName) {
    if (!ptz::perf::enabled())
        return;
    auto *pad = gst_element_get_static_pad(element, padName);
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, performanceProbe,
                      const_cast<char *>(eventName), nullptr);
    gst_object_unref(pad);
}

} // namespace

/**
 * 실행 인자와 자원을 준비한 뒤 위 함수들을 호출한다. 정상/오류 종료를 반환값으로 구분하고 보유 자원을 종료 경로에서 정리한다.
 */
int main(int argc, char *argv[]) {
    try { edge::loadDefaultConfig(); } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
    if ((argc == 2 && std::string(argv[1]) == "--check-model") ||
        (argc == 3 && std::string(argv[1]) == "--detect-image") ||
        (argc == 2 && std::string(argv[1]) == "--benchmark-model")) {
        try {
            PersonDetector detector(kModelPrototxt, kModelWeights);
            if (argc == 2 && std::string(argv[1]) == "--check-model") {
                const cv::Mat blank(kAnalysisHeight, kAnalysisWidth, CV_8UC3, cv::Scalar::all(0));
                detector.detect(blank);
                std::cout << "MobileNet-SSD loaded and inferred successfully\n";
            } else if (argc == 2) {
                const cv::Mat blank(kAnalysisHeight, kAnalysisWidth, CV_8UC3, cv::Scalar::all(0));
                for (int threads = 1; threads <= 3; ++threads) {
                    cv::setNumThreads(threads);
                    detector.detect(blank); // 첫 실행의 모델 초기화 비용은 제외한다.
                    long totalMs = 0;
                    for (int i = 0; i < 4; ++i) {
                        const auto start = std::chrono::steady_clock::now();
                        detector.detect(blank);
                        totalMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count();
                    }
                    std::cout << threads << " thread(s): " << totalMs / 4
                              << " ms/frame without video encoding\n";
                }
            } else {
                const cv::Mat image = cv::imread(argv[2]);
                if (image.empty())
                    throw std::runtime_error("Cannot read test image");
                const auto people = detector.detect(image);
                std::cout << "People detected: " << people.size() << '\n';
                for (const auto &person : people)
                    std::cout << "confidence=" << person.confidence << " box=" << person.box.x
                              << ',' << person.box.y << ',' << person.box.width << ','
                              << person.box.height << '\n';
            }
            return 0;
        } catch (const std::exception &error) {
            std::cerr << "MobileNet-SSD check failed: " << error.what() << '\n';
            return 1;
        }
    }
    gst_init(&argc, &argv);
    std::unique_ptr<CaptureManager> capture;
    GstElement* pipeline=nullptr;
    int result = 1;
    guint performanceTimer = 0,busWatch=0;
    try {
        capture=std::make_unique<CaptureManager>();
        pipeline=capture->pipeline();
        auto* camera_filter=gst_bin_get_by_name(GST_BIN(pipeline),"camera-format");
        auto* encoder_filter=gst_bin_get_by_name(GST_BIN(pipeline),"encoder-format");
        auto* parser=gst_bin_get_by_name(GST_BIN(pipeline),"h264-parser");
        addPerformanceProbe(camera_filter, "src", "stream_input");
        addPerformanceProbe(encoder_filter, "src", "encoder_input");
        addPerformanceProbe(parser, "src", "stream_encoded");
        gst_object_unref(camera_filter); gst_object_unref(encoder_filter); gst_object_unref(parser);
        ptz::perf::instant("publisher_start", kStreamWidth, kStreamHeight);

        // 버스는 실행 중 오류·영상 종료를 전달한다. GLib 루프가 이를 계속 감시한다.
        main_loop = g_main_loop_new(nullptr, FALSE);
        GstBus *bus = gst_element_get_bus(pipeline);
        busWatch=gst_bus_add_watch(bus, handleBusMessage, capture.get());
        gst_object_unref(bus);
        if (ptz::perf::enabled())
            performanceTimer = g_timeout_add_seconds(1, performanceQueueTimer, pipeline);
        g_unix_signal_add(SIGINT, handleSignal, nullptr);
        g_unix_signal_add(SIGTERM, handleSignal, nullptr);

        if (!capture->start()) {
            throw std::runtime_error("Cannot start camera pipeline");
        }
        std::atomic<bool> stopping{false};
        std::thread tracking(trackingLoop,std::ref(*capture),std::ref(stopping));
        std::cout << "Publishing 1280x720 H.264 to " << rtspUrl() << std::endl;
        g_main_loop_run(main_loop);
        stopping.store(true);
        tracking.join();
        ptz::perf::instant("publisher_end");
        result = stream_failed ? 1 : 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }

    if (performanceTimer)
        g_source_remove(performanceTimer);
    // 카메라와 RTSP 연결을 닫고 GStreamer 자원을 해제한다.
    if (busWatch) g_source_remove(busWatch);
    capture.reset();
    if (main_loop)
        g_main_loop_unref(main_loop);
    return result;
}
