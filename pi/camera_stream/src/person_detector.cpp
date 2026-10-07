/**
 * [학습 안내] MobileNet-SSD 사람 탐지와 중복 박스 제거
 * 모델 로딩 → 300x300 입력 전처리 → forward → person/점수 필터 → NMS 순서다.
 * OpenCV CPU backend를 사용한다. 같은 network 객체는 탐지 작업 스레드 한 곳에서만 호출한다.
 * 신뢰도는 모델 점수다. 고정 임계값이 실제 정확도나 확률을 보장하는 것은 아니다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "person_detector.h"

#include <algorithm>
#include <stdexcept>

namespace {
constexpr int kPersonClassId = 15;            // 이 VOC 모델의 배경=0, person=15.
constexpr float kConfidenceThreshold = 0.55f; // 이 점수보다 낮은 탐지 후보는 버린다.
constexpr float kNmsThreshold = 0.45f; // 박스 IoU가 이 값보다 크면 낮은 점수 박스를 중복으로 본다.
} // namespace

/**
 * Caffe 모델을 로딩하고 CPU/OpenCV backend를 선택한다. 빈 network이면 초기화 실패를 올린다.
 */
PersonDetector::PersonDetector(const std::string &prototxt, const std::string &weights)
    : network_(cv::dnn::readNetFromCaffe(prototxt, weights)) {
    if (network_.empty())
        throw std::runtime_error("MobileNet-SSD model is empty");
    network_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    network_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
}

/**
 * BGR 입력을 검증한 뒤 모델 전처리/추론/필터/NMS 결과를 PersonDetection 벡터로 반환한다. confidence와 좌표를 묶어 후속 제어에 전달한다.
 */
std::vector<PersonDetection> PersonDetector::detect(const cv::Mat &bgr) {
    if (bgr.empty() || bgr.type() != CV_8UC3)
        throw std::runtime_error("Person detector needs an 8-bit BGR frame");

    // 학습 때 사용한 300x300 BGR 입력과 정규화를 그대로 적용한다.
    cv::Mat blob = cv::dnn::blobFromImage(bgr, 0.007843, cv::Size(300, 300),
                                          cv::Scalar(127.5, 127.5, 127.5), false, false);
    network_.setInput(blob);
    cv::Mat output = network_.forward(); // [1, 1, detection_count, 7]
    if (output.dims != 4 || output.size[3] != 7)
        throw std::runtime_error("Unexpected MobileNet-SSD output shape");

    std::vector<cv::Rect> boxes;
    std::vector<float> scores;
    const int count = output.size[2];
    const float *data = reinterpret_cast<const float *>(output.data);
    for (int i = 0; i < count; ++i) {
        // SSD 결과 한 줄: [이미지 번호, 클래스 번호, 신뢰도, x1, y1, x2, y2].
        // 좌표는 영상 크기 대비 0~1 비율이다.
        const float *row = data + i * 7;
        // 사람 클래스가 아니거나 신뢰도가 기준보다 낮으면 후보에서 제외한다.
        if (static_cast<int>(row[1]) != kPersonClassId || row[2] < kConfidenceThreshold)
            continue;
        // 비율 좌표를 입력 영상의 픽셀 좌표로 바꾸고 영상 경계 안으로 제한한다.
        const int x1 = std::clamp(static_cast<int>(row[3] * bgr.cols), 0, bgr.cols);
        const int y1 = std::clamp(static_cast<int>(row[4] * bgr.rows), 0, bgr.rows);
        const int x2 = std::clamp(static_cast<int>(row[5] * bgr.cols), 0, bgr.cols);
        const int y2 = std::clamp(static_cast<int>(row[6] * bgr.rows), 0, bgr.rows);
        if (x2 <= x1 || y2 <= y1)
            continue;
        boxes.emplace_back(x1, y1, x2 - x1, y2 - y1);
        scores.push_back(row[2]);
    }

    // NMS는 신뢰도가 높은 박스를 우선 남긴다. 두 박스의 IoU(겹친 면적 / 합친 면적)가
    // kNmsThreshold보다 크면 같은 사람의 중복 탐지로 보고 낮은 점수 박스를 제거한다.
    std::vector<int> keep;
    cv::dnn::NMSBoxes(boxes, scores, kConfidenceThreshold, kNmsThreshold, keep);
    std::vector<PersonDetection> people;
    // NMS가 남긴 인덱스로 박스와 신뢰도를 묶어 결과로 돌려준다.
    for (int index : keep)
        people.push_back({boxes[index], scores[index]});
    return people;
}
