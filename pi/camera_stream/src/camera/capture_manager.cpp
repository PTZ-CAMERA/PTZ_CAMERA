/**
 * [학습 안내] 기존 단일 캡처/인코딩/분석 pipeline을 재사용하는 RAII 소유자
 * 요소는 생성 즉시 bin에 넣고 caps/sample/mapping을 모든 종료 경로에서 해제한다.
 * 영상 queue는 raw 2개, 분석 queue/appsink는 각 1개이며 오래된 프레임을 버린다.
 */
#include "camera/capture_manager.h"
#include "config/app_config.h"
#include <stdexcept>
#include <string>
CaptureManager::CaptureManager(Source source):source_(source) {
    pipeline_=gst_pipeline_new("ptz-camera-publisher");
    if (!pipeline_) throw std::runtime_error("Cannot create camera pipeline");
    try { build(); } catch (...) { gst_object_unref(pipeline_); pipeline_=nullptr; throw; }
}
CaptureManager::~CaptureManager() { stop(); if (pipeline_) gst_object_unref(pipeline_); }
bool CaptureManager::start() {
    cameraState_=edge::CameraState::Starting;
    if (gst_element_set_state(pipeline_,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE) { markError(); return false; }
    return true;
}
void CaptureManager::stop() {
    if (pipeline_) gst_element_set_state(pipeline_,GST_STATE_NULL);
    cameraState_=edge::CameraState::Stopped; streamState_=edge::StreamState::Stopped;
}
void CaptureManager::build() {
    auto make=[&](const char* factory,const char* name) {
        auto* e=gst_element_factory_make(factory,name);
        if (!e) throw std::runtime_error(std::string("Missing GStreamer element: ")+factory);
        if (!gst_bin_add(GST_BIN(pipeline_),e)) { gst_object_unref(e); throw std::runtime_error("Cannot add pipeline element"); }
        return e;
    };
    // caps는 g_object_set이 참조를 획득하므로 지역 소유권을 RAII로 정리한다.
    struct Caps { GstCaps* value=nullptr; ~Caps(){ if(value) gst_caps_unref(value); } } c,e,a;
    GstCaps*& camera_caps=c.value; GstCaps*& encoder_caps=e.value; GstCaps*& analysis_caps=a.value;
    const auto& config=edge::appConfig();
    const std::string rtspUrl="rtsp://127.0.0.1:"+std::to_string(config.rtspPort)+"/"+config.rtspPath;
        // 카메라를 한 번만 열고, 압축 전 영상을 송출과 사람 탐지로 나눈다.
        auto *camera = make(source_==Source::Camera ? "libcamerasrc" : "videotestsrc", "camera");
        if (source_==Source::Synthetic) g_object_set(camera,"is-live",TRUE,nullptr);
        auto *camera_filter = make("capsfilter", "camera-format");
        auto *split = make("tee", "video-split");
        auto *stream_queue = make("queue", "stream-queue");
        auto *convert = make("videoconvert", "convert");
        auto *encoder_filter = make("capsfilter", "encoder-format");
        auto *encoder = make("x264enc", "h264-encoder");
        auto *parser = make("h264parse", "h264-parser");
        auto *sink = make("rtspclientsink", "rtsp-publisher");
        auto *analysis_queue = make("queue", "analysis-queue");
        auto *rate = make("videorate", "analysis-rate");
        auto *scale = make("videoscale", "analysis-scale");
        auto *analysis_convert = make("videoconvert", "analysis-convert");
        auto *analysis_filter = make("capsfilter", "analysis-format");
        auto *analysis_sink = make("appsink", "person-frames");

        // 캡처 크기와 속도를 고정하고, 인코더 입력을 I420 색상 형식으로 맞춘다.
        camera_caps = gst_caps_from_string("video/x-raw,width=1280,height=720,framerate=30/1");
        encoder_caps = gst_caps_from_string("video/x-raw,format=I420");
        const std::string analysisCaps = "video/x-raw,format=BGR,width=640,height=360,framerate=" +
                                         std::to_string(10) + "/1";
        analysis_caps = gst_caps_from_string(analysisCaps.c_str());
        g_object_set(stream_queue,"max-size-buffers",2,"max-size-bytes",0,"max-size-time",gint64(0),"leaky",2,nullptr);
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
        g_object_set(sink, "location", rtspUrl.c_str(), "protocols", 4, nullptr);

        // 요소를 파이프라인에 넣고 앞 단계의 출력과 다음 단계의 입력을 연결한다.

        if (!gst_element_link_many(camera, camera_filter, split, nullptr) ||
            !gst_element_link_many(split, stream_queue, convert, encoder_filter, encoder, parser,
                                   sink, nullptr) ||
            !gst_element_link_many(split, analysis_queue, rate, scale, analysis_convert,
                                   analysis_filter, analysis_sink, nullptr)) {
            throw std::runtime_error("Cannot link streaming and person analysis branches");
        }


    analysisSink_=GST_APP_SINK(analysis_sink);
    auto* pad=gst_element_get_static_pad(parser,"src");
    gst_pad_add_probe(pad,GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad*,GstPadProbeInfo*,gpointer value) {
            static_cast<CaptureManager*>(value)->streamState_=edge::StreamState::Streaming;
            return GST_PAD_PROBE_OK;
        },this,nullptr);
    gst_object_unref(pad);
    pad=gst_element_get_static_pad(camera_filter,"src");
    gst_pad_add_probe(pad,GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad*,GstPadProbeInfo*,gpointer value) {
            auto* owner=static_cast<CaptureManager*>(value);
            if (owner->cameraState_.load()!=edge::CameraState::Error) owner->cameraState_=edge::CameraState::Running;
            return GST_PAD_PROBE_OK;
        },this,nullptr);
    gst_object_unref(pad);
}
AnalysisFrame::AnalysisFrame(GstSample* sample):sample_(sample) {
    if (!sample_) throw std::runtime_error("Missing analysis sample");
    GstVideoInfo info;
    auto* buffer=gst_sample_get_buffer(sample_);
    if (!buffer||!gst_video_info_from_caps(&info,gst_sample_get_caps(sample_))||
        GST_VIDEO_INFO_FORMAT(&info)!=GST_VIDEO_FORMAT_BGR||
        !gst_video_frame_map(&video_,&info,buffer,GST_MAP_READ)) {
        gst_sample_unref(sample_); sample_=nullptr; throw std::runtime_error("Invalid BGR analysis frame");
    }
    mapped_=true; timestamp_=GST_BUFFER_PTS(buffer);
    try {
        image_=cv::Mat(GST_VIDEO_INFO_HEIGHT(&info),GST_VIDEO_INFO_WIDTH(&info),CV_8UC3,
            GST_VIDEO_FRAME_PLANE_DATA(&video_,0),GST_VIDEO_FRAME_PLANE_STRIDE(&video_,0));
    } catch (...) {
        gst_video_frame_unmap(&video_); mapped_=false;
        gst_sample_unref(sample_); sample_=nullptr; throw;
    }
}
AnalysisFrame::~AnalysisFrame() {
    if (mapped_) gst_video_frame_unmap(&video_);
    if (sample_) gst_sample_unref(sample_);
}
int frameAgeMs(GstElement* pipeline,GstClockTime pts) {
    if (!GST_CLOCK_TIME_IS_VALID(pts)) return -1;
    auto* clock=gst_element_get_clock(pipeline);
    if (!clock) return -1;
    const auto now=gst_clock_get_time(clock),base=gst_element_get_base_time(pipeline);
    gst_object_unref(clock);
    if (now<base||now-base<pts) return -1;
    const auto age=(now-base-pts)/GST_MSECOND;
    return age>2147483647 ? 2147483647 : static_cast<int>(age);
}
