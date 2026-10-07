/**
 * [학습 안내] 송출과 추론의 동기/비동기 비교 시험
 * 합성 실시간 영상과 실제 x264/OpenCV 모델을 사용해 off/async/sync와 queue 정책을 비교한다.
 * 공유 경로 추론과 별도 appsink 작업 스레드를 구분한다. 같은 detector를 여러 스레드가 동시에 호출하지 않는다.
 * 실제 카메라/네트워크/서보는 사용하지 않는다. 해당 결과를 Qt 표시 지연으로 해석하지 않는다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "performance/gstreamer_metrics.h"
#include "performance/performance_log.h"
#include "person_detector.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <atomic>
#include <iostream>
#include <thread>
#include <memory>
#include <stdexcept>

namespace {
struct Trial {
    std::string mode = "async", policy = "latest";
    int threads = 2, seconds = 30;
    GstElement *pipeline = nullptr;
    std::unique_ptr<PersonDetector> detector;
    std::atomic<bool> stop{false};
    std::atomic<bool> inferenceFailed{false};
    std::atomic<unsigned long long> input{0}, output{0}, inferred{0};
};
/**
 * pipeline running time과 PTS 차이로 프레임 나이를 구한다. 유효하지 않은 PTS는 -1로 구별한다.
 */
long long frameAgeMs(Trial &trial, GstBuffer *buffer) {
    const auto pts = GST_BUFFER_PTS(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(pts))
        return -1;
    auto *clock = gst_element_get_clock(trial.pipeline);
    if (!clock)
        return -1;
    const auto now = gst_clock_get_time(clock);
    const auto base = gst_element_get_base_time(trial.pipeline);
    gst_object_unref(clock);
    if (now < base || now - base < pts)
        return -1;
    return (now - base - pts) / GST_MSECOND;
}
/**
 * BGR 샘플 mapping을 해제할 수 있게 예외 경로를 포함해 추론과 나이 측정을 수행한다.
 */
void infer(Trial &trial, GstBuffer *buffer, GstCaps *caps) {
    GstVideoInfo info;
    GstVideoFrame frame;
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps) ||
        !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ))
        throw std::runtime_error("Cannot map BGR benchmark frame");
    cv::Mat image(GST_VIDEO_INFO_HEIGHT(&info), GST_VIDEO_INFO_WIDTH(&info), CV_8UC3,
                  GST_VIDEO_FRAME_PLANE_DATA(&frame, 0), GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0));
    const auto inputAge = frameAgeMs(trial, buffer);
    cv::Mat resized;
    if (image.cols != 640 || image.rows != 360) {
        cv::resize(image, resized, cv::Size(640, 360));
        image = resized;
    }
    const auto begin = ptz::perf::nowNs();
    try {
        const auto people = trial.detector->detect(image);
        const auto end = ptz::perf::nowNs();
        const auto age = frameAgeMs(trial, buffer);
        ptz::perf::record("inference", begin, end, inputAge, age, trial.mode);
        ptz::perf::instant("detection_result", people.size(), age);
        if (age > 600)
            ptz::perf::instant("stale_result", age);
        ++trial.inferred;
    } catch (...) {
        gst_video_frame_unmap(&frame);
        throw;
    }
    gst_video_frame_unmap(&frame);
}
/**
 * sync 비교 조건에서는 공유 경로의 매 세 번째 프레임에서 추론을 완료한 뒤 전달한다. 실제 backpressure 효과를 만들기 위한 지점이다.
 */
GstPadProbeReturn captureProbe(GstPad *pad, GstPadProbeInfo *info, gpointer user) {
    auto &trial = *static_cast<Trial *>(user);
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer)
        return GST_PAD_PROBE_OK;
    const auto number = ++trial.input;
    ptz::perf::instant("stream_input", number);
    // 공유 경로에서 매 세 번째 프레임 추론을 완료할 때까지 기다리는 비교 조건.
    if (trial.mode == "sync" && number % 3 == 0 && !trial.stop) {
        GstCaps *caps = gst_pad_get_current_caps(pad);
        try {
            infer(trial, buffer, caps);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            trial.inferenceFailed = true;
            trial.stop = true;
        }
        if (caps)
            gst_caps_unref(caps);
    }
    return GST_PAD_PROBE_OK;
}
/**
 * parser 이후 출력 버퍼 수를 기록한다. Qt 렌더링 FPS/실제 네트워크 성공률은 아니다.
 */
GstPadProbeReturn outputProbe(GstPad *, GstPadProbeInfo *info, gpointer user) {
    auto &trial = *static_cast<Trial *>(user);
    if (GST_PAD_PROBE_INFO_BUFFER(info))
        ptz::perf::instant("stream_encoded", ++trial.output);
    return GST_PAD_PROBE_OK;
}
/**
 * 시험 pipeline의 지정 pad에 관측 콜백을 연결하고 임시 객체 참조를 해제한다.
 */
void addProbe(GstElement *pipeline, const char *element, const char *padName,
              GstPadProbeCallback callback, Trial &trial) {
    auto *node = gst_bin_get_by_name(GST_BIN(pipeline), element);
    if (!node)
        throw std::runtime_error("Missing probe element");
    auto *pad = gst_element_get_static_pad(node, padName);
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, callback, &trial, nullptr);
    gst_object_unref(pad);
    gst_object_unref(node);
}
} // namespace
/**
 * 실행 인자와 자원을 준비한 뒤 위 함수들을 호출한다. 정상/오류 종료를 반환값으로 구분하고 보유 자원을 종료 경로에서 정리한다.
 */
int main(int argc, char **argv) {
    Trial trial;
    std::string prototxt = "/usr/local/share/ptz-camera/deploy.prototxt";
    std::string weights = "/usr/local/share/ptz-camera/mobilenet_iter_73000.caffemodel";
    try {
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 == argc)
                throw std::runtime_error("Every option needs a value");
            const std::string key = argv[i], value = argv[i + 1];
            if (key == "--mode")
                trial.mode = value;
            else if (key == "--policy")
                trial.policy = value;
            else if (key == "--threads")
                trial.threads = std::stoi(value);
            else if (key == "--seconds")
                trial.seconds = std::stoi(value);
            else if (key == "--prototxt")
                prototxt = value;
            else if (key == "--weights")
                weights = value;
            else
                throw std::runtime_error("Unknown option: " + key);
        }
        if ((trial.mode != "off" && trial.mode != "async" && trial.mode != "sync") ||
            (trial.policy != "latest" && trial.policy != "fifo") || trial.threads < 1 ||
            trial.threads > 3 || trial.seconds < 1)
            throw std::runtime_error("Invalid trial options");
        gst_init(nullptr, nullptr);
        cv::setNumThreads(trial.threads);
        if (trial.mode != "off")
            trial.detector = std::make_unique<PersonDetector>(prototxt, weights);
        std::string pipeline =
            "videotestsrc is-live=true pattern=ball ! "
            "video/x-raw,format=BGR,width=1280,height=720,framerate=30/1 "
            "! identity name=capture ! tee name=split "
            "split. ! queue name=stream-queue ! videoconvert ! video/x-raw,format=I420 "
            "! x264enc tune=zerolatency speed-preset=ultrafast bitrate=2500 key-int-max=30 "
            "bframes=0 "
            "! h264parse name=encoded ! fakesink sync=false ";
        if (trial.mode == "async") {
            const bool latest = trial.policy == "latest";
            pipeline +=
                "split. ! queue name=analysis-queue max-size-buffers=" +
                std::string(latest ? "1" : "10") +
                " max-size-bytes=0 max-size-time=0 leaky=" + (latest ? "downstream" : "no") +
                " ! videorate drop-only=true ! videoscale ! "
                "video/x-raw,format=BGR,width=640,height=360,framerate=10/1 "
                "! appsink name=analysis sync=false max-buffers=" +
                (latest ? "1" : "10") + " drop=" + (latest ? "true" : "false");
        }
        GError *error = nullptr;
        trial.pipeline = gst_parse_launch(pipeline.c_str(), &error);
        if (error) {
            const std::string message = error->message;
            g_error_free(error);
            if (trial.pipeline)
                gst_object_unref(trial.pipeline);
            throw std::runtime_error(message);
        }
        addProbe(trial.pipeline, "capture", "src", captureProbe, trial);
        addProbe(trial.pipeline, "encoded", "src", outputProbe, trial);
        GstElement *sink = trial.mode == "async"
                               ? gst_bin_get_by_name(GST_BIN(trial.pipeline), "analysis")
                               : nullptr;
        std::thread worker;
        if (gst_element_set_state(trial.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("Cannot start benchmark pipeline");
        const auto start = ptz::perf::nowNs();
        ptz::perf::instant("trial_start", trial.threads, trial.seconds,
                           trial.mode + ":" + trial.policy);
        if (sink)
            worker = std::thread([&] {
                try {
                    while (!trial.stop) {
                        auto *sample =
                            gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
                        if (!sample)
                            continue;
                        try {
                            infer(trial, gst_sample_get_buffer(sample),
                                  gst_sample_get_caps(sample));
                        } catch (...) {
                            gst_sample_unref(sample);
                            throw;
                        }
                        gst_sample_unref(sample);
                    }
                } catch (const std::exception &e) {
                    std::cerr << e.what() << '\n';
                    trial.inferenceFailed = true;
                    trial.stop = true;
                }
            });
        auto *bus = gst_element_get_bus(trial.pipeline);
        bool failed = false;
        auto lastQueueReport = start;
        while (!trial.stop && ptz::perf::nowNs() - start < trial.seconds * 1000000000LL) {
            if (ptz::perf::nowNs() - lastQueueReport >= 1000000000LL) {
                ptz::perf::recordQueueLevels(trial.pipeline);
                lastQueueReport = ptz::perf::nowNs();
            }
            auto *message = gst_bus_timed_pop_filtered(
                bus, 100 * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
            if (message) {
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                    GError *err = nullptr;
                    gchar *debug = nullptr;
                    gst_message_parse_error(message, &err, &debug);
                    std::cerr << (err ? err->message : "pipeline error") << '\n';
                    g_clear_error(&err);
                    g_free(debug);
                    failed = true;
                }
                gst_message_unref(message);
                break;
            }
        }
        trial.stop = true;
        if (worker.joinable())
            worker.join();
        gst_element_set_state(trial.pipeline, GST_STATE_NULL);
        ptz::perf::instant("trial_end", trial.input, trial.output, trial.mode);
        if (sink)
            gst_object_unref(sink);
        gst_object_unref(bus);
        gst_object_unref(trial.pipeline);
        std::cout << "mode=" << trial.mode << " threads=" << trial.threads
                  << " policy=" << trial.policy << " input=" << trial.input
                  << " encoded=" << trial.output << " inferred=" << trial.inferred << '\n';
        return failed || trial.inferenceFailed ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
