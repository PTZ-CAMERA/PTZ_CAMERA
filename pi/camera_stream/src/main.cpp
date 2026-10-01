#include <gst/gst.h>
#include <glib-unix.h>

#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr const char *kRtspUrl = "rtsp://127.0.0.1:8554/cam";
GMainLoop *main_loop = nullptr;
bool stream_failed = false;

GstElement *makeElement(const char *factory, const char *name) {
    GstElement *element = gst_element_factory_make(factory, name);
    if (!element) {
        throw std::runtime_error(std::string("GStreamer element unavailable: ") + factory);
    }
    return element;
}

gboolean handleSignal(gpointer) {
    g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

gboolean handleBusMessage(GstBus *, GstMessage *message, gpointer) {
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
        auto *camera = makeElement("libcamerasrc", "camera");
        auto *camera_filter = makeElement("capsfilter", "camera-format");
        auto *convert = makeElement("videoconvert", "convert");
        auto *encoder_filter = makeElement("capsfilter", "encoder-format");
        auto *encoder = makeElement("x264enc", "h264-encoder");
        auto *parser = makeElement("h264parse", "h264-parser");
        auto *sink = makeElement("rtspclientsink", "rtsp-publisher");

        camera_caps = gst_caps_from_string("video/x-raw,width=1280,height=720,framerate=30/1");
        encoder_caps = gst_caps_from_string("video/x-raw,format=I420");
        g_object_set(camera_filter, "caps", camera_caps, nullptr);
        g_object_set(encoder_filter, "caps", encoder_caps, nullptr);
        // No B frames: browsers can receive MediaMTX's WebRTC H.264 output.
        g_object_set(encoder, "tune", 0x00000004, "speed-preset", 1,
                     "bitrate", 2500, "key-int-max", 30, "bframes", 0, nullptr);
        g_object_set(parser, "config-interval", 1, nullptr);
        // GST_RTSP_LOWER_TRANS_TCP = 1 << 2. MediaMTX accepts TCP only.
        g_object_set(sink, "location", kRtspUrl, "protocols", 4, nullptr);

        gst_bin_add_many(GST_BIN(pipeline), camera, camera_filter, convert,
                         encoder_filter, encoder, parser, sink, nullptr);
        if (!gst_element_link_many(camera, camera_filter, convert, encoder_filter,
                                   encoder, parser, sink, nullptr)) {
            throw std::runtime_error("Cannot link camera, encoder and RTSP publisher");
        }

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

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    if (camera_caps) gst_caps_unref(camera_caps);
    if (encoder_caps) gst_caps_unref(encoder_caps);
    if (main_loop) g_main_loop_unref(main_loop);
    return result;
}
