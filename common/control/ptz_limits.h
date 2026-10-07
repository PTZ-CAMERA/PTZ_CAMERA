#pragma once
#include "config/app_config.h"
namespace ptz::geometry {
// 각 프로세스가 동일한 시작 설정을 읽는다. worker 실행 중 reload는 하지 않는다.
inline const int& panMin=edge::appConfig().ptz.panMin;
inline const int& panMax=edge::appConfig().ptz.panMax;
inline const int& tiltMin=edge::appConfig().ptz.tiltMin;
inline const int& tiltMax=edge::appConfig().ptz.tiltMax;
inline const double& targetSpeed=edge::appConfig().ptz.targetSpeed;
inline const double& maxVelocity=edge::appConfig().ptz.maxVelocity;
inline double panRange() { return panMax-panMin; }
inline double tiltRange() { return tiltMax-tiltMin; }
inline double panMid() { return (panMin+panMax)/2.0; }
inline double tiltMid() { return (tiltMin+tiltMax)/2.0; }
} // namespace ptz::geometry
