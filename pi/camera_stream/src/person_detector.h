#pragma once

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

#include <string>
#include <vector>

// MobileNet-SSD가 반환하는 사람의 위치와 신뢰도. 좌표는 입력 영상 기준이다.
struct PersonDetection {
    cv::Rect box;
    float confidence;
};

class PersonDetector {
public:
    PersonDetector(const std::string& prototxt, const std::string& weights);
    std::vector<PersonDetection> detect(const cv::Mat& bgr);

private:
    cv::dnn::Net network_;
};
