/**
 * [학습 안내] 카메라 송출과 사람 자동 추적의 실행 진입점
 * 카메라를 한 번 열어 tee에서 송출과 압축 전 분석 영상으로 나눈다.
 * GLib 메인 루프는 오류/종료를 감시하고 별도 std::thread가 OpenCV 추론을 수행한다.
 * 분석은 최신 대기 프레임을 선택하고 600ms 초과 결과로 이동하지 않는다. Qt 재생 버퍼는 추론 입력에 들어가지 않는다.
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
constexpr const char *kRtspUrl = "rtsp://127.0.0.1:8554/cam";
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

// 플러그인이 빠져 있으면 실행 초기에 원인을 바로 표시한다.
/**
 * GStreamer 요소를 생성하고 플러그인 누락을 예외로 알린다. 반환 객체는 이후 pipeline에 추가해 소유권을 넘긴다.
 */
GstElement *makeElement(const char *factory, const char *name) {
    GstElement *element = gst_element_factory_make(factory, name);
    if (!element) {
        throw std::runtime_error(std::string("GStreamer element unavailable: ") + factory);
    }
    return element;
}

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
gboolean handleBusMessage(GstBus *, GstMessage *message, gpointer) {
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
        stream_failed = true;
        g_main_loop_quit(main_loop);
        return G_SOURCE_REMOVE;
    }
    case GST_MESSAGE_EOS:
        std::cerr << "Camera stream ended\n";
        stream_failed = true;
        g_main_loop_quit(main_loop);
        return G_SOURCE_REMOVE;
    default:
        return G_SOURCE_CONTINUE;
    }
}

/**
 * 추적 상태 확인 → 최신 BGR 샘플 취득 → 추론 → 나이 검증 → 대상/이동량 계산 순서다. 종료 atomic을 확인하고 샘플/프레임 mapping을 해제한다.
 */
void trackingLoop(GstElement *pipeline, GstAppSink *sink, std::atomic<bool> &stopping) {
    try {
        int dnnThreads = kDnnThreads;
        if (const char *value = std::getenv("PTZ_DNN_THREADS")) {
            const std::string option(value);
            if (option != "1" && option != "2" && option != "3")
                throw std::runtime_error("PTZ_DNN_THREADS must be 1, 2 or 3");
            dnnThreads = std::stoi(option);
        }
        cv::setNumThreads(dnnThreads);
        ptz::perf::instant("tracking_worker_start", dnnThreads, kAnalysisInputFps);
        PersonDetector detector(kModelPrototxt, kModelWeights);
        ServoClient servo;
        TargetSelector selector;
        TrackingController tracking;
        std::uint64_t frameId = 0;
        const auto epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        bool noDetectionSent = false;
        bool wasTracking = false;
        std::uint64_t previousGeneration = 0;
        auto lastReport = std::chrono::steady_clock::now();
        int staleFrames = 0;
        std::cout << "Person detector ready (MobileNet-SSD, " << kAnalysisInputFps << " fps input, "
                  << dnnThreads << " DNN threads, latest frame only)\n";
        while (!stopping.load()) {
            const auto trackingState = servo.trackingState();
            if (!trackingState.enabled) {
                if (wasTracking)
                    std::cout << "Person tracking stopped\n";
                wasTracking = false;
                selector.reset();
                noDetectionSent = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                continue;
            }
            if (trackingState.generation != previousGeneration) {
                selector.reset(); noDetectionSent = false;
                previousGeneration = trackingState.generation;
            }
            if (!wasTracking)
                std::cout << "Person tracking started\n";
            wasTracking = true;

            GstSample *sample = gst_app_sink_try_pull_sample(sink, 200 * GST_MSECOND);
            if (!sample)
                continue;
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            const GstClockTime timestamp = GST_BUFFER_PTS(buffer);
            GstVideoInfo info;
            GstVideoFrame video;
            if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
                !gst_video_frame_map(&video, &info, buffer, GST_MAP_READ)) {
                gst_sample_unref(sample);
                continue;
            }
            cv::Mat bgr(GST_VIDEO_INFO_HEIGHT(&info), GST_VIDEO_INFO_WIDTH(&info), CV_8UC3,
                        GST_VIDEO_FRAME_PLANE_DATA(&video, 0),
                        GST_VIDEO_FRAME_PLANE_STRIDE(&video, 0));
            int inputAgeMs = -1;
            if (GST_CLOCK_TIME_IS_VALID(timestamp)) {
                GstClock *clock = gst_element_get_clock(pipeline);
                if (clock) {
                    const GstClockTime clockNow = gst_clock_get_time(clock);
                    const GstClockTime baseTime = gst_element_get_base_time(pipeline);
                    gst_object_unref(clock);
                    if (clockNow > baseTime && clockNow - baseTime > timestamp)
                        inputAgeMs =
                            static_cast<int>((clockNow - baseTime - timestamp) / GST_MSECOND);
                }
            }
            const auto receivedUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto captureUnixMs = inputAgeMs >= 0 ? receivedUnixMs - inputAgeMs : 0;
            ++frameId;
            const auto inferenceStart = std::chrono::steady_clock::now();
            const auto people = detector.detect(bgr);
            const auto inferenceEnd = std::chrono::steady_clock::now();
            gst_video_frame_unmap(&video);
            gst_sample_unref(sample);

            // PTS는 파이프라인 running time이다. 오래된 추론 결과로 서보를 움직이지 않는다.
            int frameAgeMs = -1;
            if (GST_CLOCK_TIME_IS_VALID(timestamp)) {
                GstClock *clock = gst_element_get_clock(pipeline);
                if (clock) {
                    const GstClockTime clockNow = gst_clock_get_time(clock);
                    const GstClockTime baseTime = gst_element_get_base_time(pipeline);
                    gst_object_unref(clock);
                    if (clockNow > baseTime && clockNow - baseTime > timestamp)
                        frameAgeMs =
                            static_cast<int>((clockNow - baseTime - timestamp) / GST_MSECOND);
                }
            }
            if (ptz::perf::enabled()) {
                const auto beginNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         inferenceStart.time_since_epoch())
                                         .count();
                const auto endNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       inferenceEnd.time_since_epoch())
                                       .count();
                ptz::perf::record("inference", beginNs, endNs, inputAgeMs, frameAgeMs,
                                  "pts=" + std::to_string(timestamp));
                ptz::perf::instant("detection_result", people.size(), frameAgeMs);
            }
            if (frameAgeMs > 600) {
                ++staleFrames;
                ptz::perf::instant("stale_result", frameAgeMs);
            }
            if (inferenceEnd - lastReport >= std::chrono::seconds(5)) {
                const auto inferMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         inferenceEnd - inferenceStart)
                                         .count();
                std::cout << "Person analysis: inference=" << inferMs
                          << "ms, frame_age=" << frameAgeMs << "ms, input_age=" << inputAgeMs
                          << "ms, people=" << people.size() << ", stale=" << staleFrames
                          << std::endl;
                lastReport = inferenceEnd;
                staleFrames = 0;
            }
            if (frameAgeMs > 600)
                continue;
            const auto currentState = servo.trackingState();
            if (!currentState.enabled || currentState.generation != trackingState.generation)
                continue;

            const auto target = selector.select(people, kAnalysisWidth, kAnalysisHeight);
            if (!target) {
                selector.reset();
                if (!noDetectionSent) {
                    servo.noDetection();
                    noDetectionSent = true;
                }
                continue;
            }
            noDetectionSent = false;
            const auto correction = tracking.update(*target, kAnalysisWidth, kAnalysisHeight,
                                                    kStreamWidth, kStreamHeight, frameAgeMs);
            if (!correction) continue;
            const auto& r = *correction;
            if (r.panDelta || r.tiltDelta)
                servo.move(r.panDelta, r.tiltDelta, trackingState.generation);
            ptz::DetectionMetadata metadata;
            metadata.confidence = target->confidence;
            metadata.x = r.x; metadata.y = r.y;
            metadata.errorX = r.errorX; metadata.errorY = r.errorY;
            metadata.boxX = r.box.x; metadata.boxY = r.box.y;
            metadata.boxWidth = r.box.width; metadata.boxHeight = r.box.height;
            metadata.imageWidth = kStreamWidth; metadata.imageHeight = kStreamHeight;
            metadata.frameId = frameId;
            metadata.framePtsNs = GST_CLOCK_TIME_IS_VALID(timestamp) ? static_cast<std::int64_t>(timestamp) : -1;
            // CaptureUnixMs는 PTS 나이로 추정한 시각이다. 센서 노출 시각을 직접 읽은 값이 아니다.
            metadata.captureUnixMs = captureUnixMs;
            metadata.analysisUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            metadata.streamEpoch = epoch;
            metadata.generation = trackingState.generation;
            servo.detection(metadata);
        }
    } catch (const std::exception &error) {
        std::cerr << "Person tracking unavailable: " << error.what() << '\n';
    }
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
    GstElement *pipeline = gst_pipeline_new("ptz-camera-publisher");
    if (!pipeline) {
        std::cerr << "Cannot create GStreamer pipeline\n";
        return 1;
    }

    GstCaps *camera_caps = nullptr;
    GstCaps *encoder_caps = nullptr;
    GstCaps *analysis_caps = nullptr;
    int result = 1;
    guint performanceTimer = 0;
    try {
        // 카메라를 한 번만 열고, 압축 전 영상을 송출과 사람 탐지로 나눈다.
        auto *camera = makeElement("libcamerasrc", "camera");
        auto *camera_filter = makeElement("capsfilter", "camera-format");
        auto *split = makeElement("tee", "video-split");
        auto *stream_queue = makeElement("queue", "stream-queue");
        auto *convert = makeElement("videoconvert", "convert");
        auto *encoder_filter = makeElement("capsfilter", "encoder-format");
        auto *encoder = makeElement("x264enc", "h264-encoder");
        auto *parser = makeElement("h264parse", "h264-parser");
        auto *sink = makeElement("rtspclientsink", "rtsp-publisher");
        auto *analysis_queue = makeElement("queue", "analysis-queue");
        auto *rate = makeElement("videorate", "analysis-rate");
        auto *scale = makeElement("videoscale", "analysis-scale");
        auto *analysis_convert = makeElement("videoconvert", "analysis-convert");
        auto *analysis_filter = makeElement("capsfilter", "analysis-format");
        auto *analysis_sink = makeElement("appsink", "person-frames");

        // 캡처 크기와 속도를 고정하고, 인코더 입력을 I420 색상 형식으로 맞춘다.
        camera_caps = gst_caps_from_string("video/x-raw,width=1280,height=720,framerate=30/1");
        encoder_caps = gst_caps_from_string("video/x-raw,format=I420");
        const std::string analysisCaps = "video/x-raw,format=BGR,width=640,height=360,framerate=" +
                                         std::to_string(kAnalysisInputFps) + "/1";
        analysis_caps = gst_caps_from_string(analysisCaps.c_str());
        g_object_set(camera_filter, "caps", camera_caps, nullptr);
        g_object_set(encoder_filter, "caps", encoder_caps, nullptr);
        g_object_set(analysis_filter, "caps", analysis_caps, nullptr);
        // 대기 중인 낡은 프레임을 버린다. 분석 지연이 송출 경로까지 전파되지 않는다.
        g_object_set(analysis_queue, "max-size-buffers", 1, "max-size-bytes", 0, "max-size-time",
                     gint64(0), "leaky", 2, nullptr);
        g_object_set(rate, "drop-only", TRUE, nullptr);
        g_object_set(analysis_sink, "max-buffers", 1, "drop", TRUE, "sync", FALSE,
                     "enable-last-sample", FALSE, nullptr);
        // 지연을 줄이고 B 프레임을 끈다. B 프레임이 있으면 브라우저 WebRTC 재생에 문제가 생긴다.
        // bitrate 단위는 kbit/s, key-int-max=30은 30fps 기준 약 1초마다 키 프레임을 뜻한다.
        g_object_set(encoder, "tune", 0x00000004, "speed-preset", 1, "bitrate", 2500, "key-int-max",
                     30, "bframes", 0, nullptr);
        // 재접속한 시청자도 H.264 설정 정보를 받을 수 있도록 주기적으로 삽입한다.
        g_object_set(parser, "config-interval", 1, nullptr);
        // 로컬 발행은 TCP, 외부 시청자는 MediaMTX에서 RTSP TCP/UDP를 선택한다.
        g_object_set(sink, "location", kRtspUrl, "protocols", 4, nullptr);

        // 요소를 파이프라인에 넣고 앞 단계의 출력과 다음 단계의 입력을 연결한다.
        gst_bin_add_many(GST_BIN(pipeline), camera, camera_filter, split, stream_queue, convert,
                         encoder_filter, encoder, parser, sink, analysis_queue, rate, scale,
                         analysis_convert, analysis_filter, analysis_sink, nullptr);
        if (!gst_element_link_many(camera, camera_filter, split, nullptr) ||
            !gst_element_link_many(split, stream_queue, convert, encoder_filter, encoder, parser,
                                   sink, nullptr) ||
            !gst_element_link_many(split, analysis_queue, rate, scale, analysis_convert,
                                   analysis_filter, analysis_sink, nullptr)) {
            throw std::runtime_error("Cannot link streaming and person analysis branches");
        }

        addPerformanceProbe(camera_filter, "src", "stream_input");
        addPerformanceProbe(encoder_filter, "src", "encoder_input");
        addPerformanceProbe(parser, "src", "stream_encoded");
        ptz::perf::instant("publisher_start", kStreamWidth, kStreamHeight);

        // 버스는 실행 중 오류·영상 종료를 전달한다. GLib 루프가 이를 계속 감시한다.
        main_loop = g_main_loop_new(nullptr, FALSE);
        GstBus *bus = gst_element_get_bus(pipeline);
        gst_bus_add_watch(bus, handleBusMessage, nullptr);
        gst_object_unref(bus);
        if (ptz::perf::enabled())
            performanceTimer = g_timeout_add_seconds(1, performanceQueueTimer, pipeline);
        g_unix_signal_add(SIGINT, handleSignal, nullptr);
        g_unix_signal_add(SIGTERM, handleSignal, nullptr);

        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            throw std::runtime_error("Cannot start camera pipeline");
        }
        std::atomic<bool> stopping{false};
        std::thread tracking(trackingLoop, pipeline, GST_APP_SINK(analysis_sink),
                             std::ref(stopping));
        std::cout << "Publishing 1280x720 H.264 to " << kRtspUrl << std::endl;
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
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    if (camera_caps)
        gst_caps_unref(camera_caps);
    if (encoder_caps)
        gst_caps_unref(encoder_caps);
    if (analysis_caps)
        gst_caps_unref(analysis_caps);
    if (main_loop)
        g_main_loop_unref(main_loop);
    return result;
}
