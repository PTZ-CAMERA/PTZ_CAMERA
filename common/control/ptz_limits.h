#pragma once

namespace ptz::geometry {
// ONVIF 좌표 변환과 실제 서보 제한이 같은 값을 사용하도록 한 곳에 둔다.
inline constexpr int panMin = 25, panMax = 155;
inline constexpr int tiltMin = 40, tiltMax = 140;
inline constexpr double panRange = panMax - panMin, tiltRange = tiltMax - tiltMin;
inline constexpr double panMid = (panMin + panMax) / 2.0, tiltMid = (tiltMin + tiltMax) / 2.0;
inline constexpr double targetSpeed = 15.0, maxVelocity = 30.0;
} // namespace ptz::geometry
