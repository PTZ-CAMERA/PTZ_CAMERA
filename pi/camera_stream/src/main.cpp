#include <gst/gst.h>
#include <glib-unix.h>

#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

// 이 주소로 발행한 영상을 MediaMTX가 RTSP·WebRTC·HLS 시청자에게 전달한다.
constexpr const char *kRtspUrl = "rtsp://127.0.0.1:8554/cam";
GMainLoop *main_loop = nullptr;
bool stream_failed = false;

// 플러그인이 빠져 있으면 실행 초기에 원인을 바로 표시한다.
GstElement *makeElement(const char *factory, const char *name) {
    GstElement *element = gst_element_factory_make(factory, name);
    if (!element) {
        throw std::runtime_error(std::string("GStreamer element unavailable: ") + factory);
    }
    return element;
}

gboolean handleSignal(gpointer) {
    // Ctrl+C와 systemd의 종료 신호를 같은 경로로 처리한다.
    g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

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
        if (debug) std::cerr << debug << '\n';
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

} // namespace

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    GstElement *pipeline = gst_pipeline_new("ptz-camera-publisher");
    if (!pipeline) {
        std::cerr << "Cannot create GStreamer pipeline\n";
        return 1;
    }

    GstCaps *camera_caps = nullptr;
    GstCaps *encoder_caps = nullptr;
    int result = 1;
    try {
        // 영상 경로: Pi 카메라 → 색상 변환 → H.264 인코딩 → RTSP 발행.
        // libcamerasrc는 Pi 카메라를 읽고, MediaMTX는 RTSP 수신 서버 역할을 한다.
        auto *camera = makeElement("libcamerasrc", "camera");
        auto *camera_filter = makeElement("capsfilter", "camera-format");
        auto *convert = makeElement("videoconvert", "convert");
        auto *encoder_filter = makeElement("capsfilter", "encoder-format");
        auto *encoder = makeElement("x264enc", "h264-encoder");
        auto *parser = makeElement("h264parse", "h264-parser");
        auto *sink = makeElement("rtspclientsink", "rtsp-publisher");

        // 캡처 크기와 속도를 고정하고, 인코더 입력을 I420 색상 형식으로 맞춘다.
        camera_caps = gst_caps_from_string("video/x-raw,width=1280,height=720,framerate=30/1");
        encoder_caps = gst_caps_from_string("video/x-raw,format=I420");
        g_object_set(camera_filter, "caps", camera_caps, nullptr);
        g_object_set(encoder_filter, "caps", encoder_caps, nullptr);
        // 지연을 줄이고 B 프레임을 끈다. B 프레임이 있으면 브라우저 WebRTC 재생에 문제가 생긴다.
        // bitrate 단위는 kbit/s, key-int-max=30은 30fps 기준 약 1초마다 키 프레임을 뜻한다.
        g_object_set(encoder, "tune", 0x00000004, "speed-preset", 1,
                     "bitrate", 2500, "key-int-max", 30, "bframes", 0, nullptr);
        // 재접속한 시청자도 H.264 설정 정보를 받을 수 있도록 주기적으로 삽입한다.
        g_object_set(parser, "config-interval", 1, nullptr);
        // GST_RTSP_LOWER_TRANS_TCP = 1 << 2. MediaMTX 설정도 RTSP TCP만 허용한다.
        g_object_set(sink, "location", kRtspUrl, "protocols", 4, nullptr);

        // 요소를 파이프라인에 넣고 앞 단계의 출력과 다음 단계의 입력을 연결한다.
        gst_bin_add_many(GST_BIN(pipeline), camera, camera_filter, convert,
                         encoder_filter, encoder, parser, sink, nullptr);
        if (!gst_element_link_many(camera, camera_filter, convert, encoder_filter,
                                   encoder, parser, sink, nullptr)) {
            throw std::runtime_error("Cannot link camera, encoder and RTSP publisher");
        }

        // 버스는 실행 중 오류·영상 종료를 전달한다. GLib 루프가 이를 계속 감시한다.
        main_loop = g_main_loop_new(nullptr, FALSE);
        GstBus *bus = gst_element_get_bus(pipeline);
        gst_bus_add_watch(bus, handleBusMessage, nullptr);
        gst_object_unref(bus);
        g_unix_signal_add(SIGINT, handleSignal, nullptr);
        g_unix_signal_add(SIGTERM, handleSignal, nullptr);

        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            throw std::runtime_error("Cannot start camera pipeline");
        }
        std::cout << "Publishing 1280x720 H.264 to " << kRtspUrl << std::endl;
        g_main_loop_run(main_loop);
        result = stream_failed ? 1 : 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }

    // 카메라와 RTSP 연결을 닫고 GStreamer 자원을 해제한다.
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    if (camera_caps) gst_caps_unref(camera_caps);
    if (encoder_caps) gst_caps_unref(encoder_caps);
    if (main_loop) g_main_loop_unref(main_loop);
    return result;
}
