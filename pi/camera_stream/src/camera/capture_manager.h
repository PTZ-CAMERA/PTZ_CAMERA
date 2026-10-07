#pragma once
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <opencv2/core.hpp>
#include <atomic>
#include "runtime/state.h"
// 기존 GStreamer pipeline의 소유자. CSI source는 이 객체가 한 번만 만든다.
class CaptureManager {
public:
    enum class Source { Camera, Synthetic };
    explicit CaptureManager(Source source=Source::Camera);
    ~CaptureManager();
    CaptureManager(const CaptureManager&)=delete;
    CaptureManager& operator=(const CaptureManager&)=delete;
    GstElement* pipeline() const { return pipeline_; }
    GstAppSink* analysisSink() const { return analysisSink_; }
    bool start();
    void stop();
    bool streamReady() const { return streamState_.load()==edge::StreamState::Streaming; }
    edge::CameraState cameraState() const { return cameraState_.load(); }
    edge::StreamState streamState() const { return streamState_.load(); }
    void markError() { cameraState_=edge::CameraState::Error; streamState_=edge::StreamState::Error; }
private:
    GstElement* pipeline_=nullptr;
    GstAppSink* analysisSink_=nullptr;
    Source source_;
    std::atomic<edge::CameraState> cameraState_{edge::CameraState::Stopped};
    std::atomic<edge::StreamState> streamState_{edge::StreamState::Stopped};
    void build();
};
// map된 GstSample의 수명 안에서만 cv::Mat view가 유효하다. 이미지 clone은 만들지 않는다.
class AnalysisFrame {
public:
    explicit AnalysisFrame(GstSample* sample);
    ~AnalysisFrame();
    AnalysisFrame(const AnalysisFrame&)=delete;
    AnalysisFrame& operator=(const AnalysisFrame&)=delete;
    const cv::Mat& image() const { return image_; }
    GstClockTime timestamp() const { return timestamp_; }
private:
    GstSample* sample_;
    GstVideoFrame video_{};
    bool mapped_=false;
    cv::Mat image_;
    GstClockTime timestamp_=GST_CLOCK_TIME_NONE;
};
int frameAgeMs(GstElement* pipeline,GstClockTime pts);
