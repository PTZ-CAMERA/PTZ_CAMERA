#pragma once
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace ptz {
// IPC v2 자료형. bbox는 송출 이미지의 pixel 좌표이며 오른쪽/아래쪽 끝은 제외한다.
// framePtsNs는 해당 publisher의 running time, streamEpoch는 재시작 식별자다.
// captureUnixMs는 PTS로 추정한 UTC이며 0은 알 수 없음. object ID는 발급하지 않는다.
// streamEpoch와 frameId를 함께 사용해야 publisher 재시작 전후의 프레임을 구분할 수 있다.
// generation은 수동 개입 뒤 오래된 추론 결과를 거르는 제어 세대이며 프레임 시각이 아니다.
struct DetectionMetadata {
    double confidence = 0;
    std::int64_t x=0, y=0, errorX=0, errorY=0;
    std::int64_t boxX=0, boxY=0, boxWidth=0, boxHeight=0;
    std::int64_t imageWidth=1280, imageHeight=720;
    std::int64_t frameId=0, framePtsNs=-1, captureUnixMs=0, analysisUnixMs=0, streamEpoch=0;
    std::int64_t generation=0, panAngle=90, tiltAngle=90;
};
inline bool validDetection(const DetectionMetadata& d) {
    return std::isfinite(d.confidence) && d.confidence >= 0 && d.confidence <= 1 &&
        d.imageWidth > 0 && d.imageWidth <= 8192 && d.imageHeight > 0 && d.imageHeight <= 8192 &&
        d.x >= 0 && d.x < d.imageWidth && d.y >= 0 && d.y < d.imageHeight &&
        d.errorX >= -d.imageWidth && d.errorX <= d.imageWidth &&
        d.errorY >= -d.imageHeight && d.errorY <= d.imageHeight &&
        d.boxX >= 0 && d.boxY >= 0 && d.boxWidth > 0 && d.boxHeight > 0 &&
        d.boxX <= d.imageWidth && d.boxWidth <= d.imageWidth - d.boxX &&
        d.boxY <= d.imageHeight && d.boxHeight <= d.imageHeight - d.boxY &&
        d.frameId > 0 && d.framePtsNs >= -1 && d.captureUnixMs >= 0 &&
        d.analysisUnixMs > 0 && (d.captureUnixMs == 0 || d.analysisUnixMs >= d.captureUnixMs) &&
        d.streamEpoch > 0 && d.generation >= 0 &&
        d.panAngle >= 0 && d.panAngle <= 180 && d.tiltAngle >= 0 && d.tiltAngle <= 180;
}
inline std::string encodeDetection(const DetectionMetadata& d) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::setprecision(8) << d.confidence;
    for (auto v : {d.x,d.y,d.errorX,d.errorY,d.boxX,d.boxY,d.boxWidth,d.boxHeight,
                  d.imageWidth,d.imageHeight,d.frameId,d.framePtsNs,d.captureUnixMs,
                  d.analysisUnixMs,d.streamEpoch,d.generation,d.panAngle,d.tiltAngle}) s << ',' << v;
    return s.str();
}
inline std::optional<DetectionMetadata> decodeDetection(std::string_view input) {
    // 필드 수/전체 소비/정수 overflow를 검사한다. 무한대/NaN과 범위를 벗어난 bbox도 거절한다.
    if (input.size() > 768) return std::nullopt;
    std::array<std::string_view,19> fields{};
    for (std::size_t i=0; i<fields.size(); ++i) {
        const auto end = input.find(',');
        if ((i+1<fields.size()) != (end != std::string_view::npos)) return std::nullopt;
        fields[i] = input.substr(0,end);
        if (fields[i].empty()) return std::nullopt;
        if (end != std::string_view::npos) input.remove_prefix(end+1);
    }
    DetectionMetadata d;
    std::istringstream number{std::string(fields[0])};
    number.imbue(std::locale::classic());
    number >> std::noskipws >> d.confidence;
    if (!number || !number.eof()) return std::nullopt;
    std::array<std::int64_t*,18> values{&d.x,&d.y,&d.errorX,&d.errorY,&d.boxX,&d.boxY,
        &d.boxWidth,&d.boxHeight,&d.imageWidth,&d.imageHeight,&d.frameId,&d.framePtsNs,
        &d.captureUnixMs,&d.analysisUnixMs,&d.streamEpoch,&d.generation,&d.panAngle,&d.tiltAngle};
    for (std::size_t i=0; i<values.size(); ++i) {
        auto field=fields[i+1];
        const auto r=std::from_chars(field.data(),field.data()+field.size(),*values[i]);
        if (r.ec!=std::errc{} || r.ptr!=field.data()+field.size()) return std::nullopt;
    }
    return validDetection(d) ? std::optional<DetectionMetadata>(d) : std::nullopt;
}
} // namespace ptz
