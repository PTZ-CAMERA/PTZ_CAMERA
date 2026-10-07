#pragma once
#include <string>
namespace edge {
struct PtzConfig {
    int panPin=18, tiltPin=19, centerButtonPin=17;
    int panCenter=90, tiltCenter=90, panMin=25, panMax=155, tiltMin=40, tiltMax=140;
    bool invertPan=false, invertTilt=false;
    double targetSpeed=15, maxVelocity=30;
    int buttonDebounceMs=80, buttonGlitchMs=30;
};
struct TrackingConfig {
    bool enabled=false, detectionWhenDisabled=false;
    int deadZoneX=51, deadZoneY=29, lostTimeoutMs=1000, maxFrameAgeMs=600;
    int manualResumeMs=1000, metadataHz=5;
};
struct AppConfig {
    std::string cameraId="CAM01", rtspPath="cam";
    int rtspPort=8554, controlPort=5000;
    PtzConfig ptz;
    TrackingConfig tracking;
};
// main에서 한 번 로딩한 뒤 worker를 시작한다. 실행 중 설정 reload는 제공하지 않는다.
AppConfig& appConfig();
void loadAppConfig(const std::string& path);
void loadDefaultConfig();
void validateConfig(const AppConfig& config);
} // namespace edge
