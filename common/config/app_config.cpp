/**
 * [학습 안내] 모든 Pi 프로세스가 공유하는 시작 설정
 * JSON을 임시 객체에 읽고 전체 검증에 성공했을 때만 반영한다.
 * worker가 시작된 뒤에는 수정하지 않는다. 누락/오타/자료형/NaN/범위 오류를 시작 시 발견한다.
 */
#include "app_config.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
namespace edge {
AppConfig& appConfig() { static AppConfig config; return config; }
namespace {
using Json=nlohmann::json;
void keys(const Json& object, std::initializer_list<const char*> allowed) {
    if (!object.is_object()) throw std::invalid_argument("Configuration section must be object");
    std::set<std::string> names;
    for (auto name:allowed) names.insert(name);
    for (auto it=object.begin();it!=object.end();++it)
        if (!names.count(it.key())) throw std::invalid_argument("Unknown config key: "+it.key());
}
void integer(const Json& j,const char* key,int& value) {
    if (!j.contains(key)) return;
    if (!j[key].is_number_integer()) throw std::invalid_argument(std::string(key)+" must be integer");
    const auto n=j[key].get<long long>();
    if (n < -1000000 || n > 1000000) throw std::invalid_argument(std::string(key)+" overflow/range");
    value=static_cast<int>(n);
}
void boolean(const Json& j,const char* key,bool& value) {
    if (!j.contains(key)) return;
    if (!j[key].is_boolean()) throw std::invalid_argument(std::string(key)+" must be boolean");
    value=j[key].get<bool>();
}
void real(const Json& j,const char* key,double& value) {
    if (!j.contains(key)) return;
    if (!j[key].is_number()) throw std::invalid_argument(std::string(key)+" must be number");
    value=j[key].get<double>();
}
void string(const Json& j,const char* key,std::string& value) {
    if (!j.contains(key)) return;
    if (!j[key].is_string()) throw std::invalid_argument(std::string(key)+" must be string");
    value=j[key].get<std::string>();
}
bool identifier(const std::string& value) {
    if (value.empty() || value.size()>64) return false;
    for (unsigned char c:value)
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_')) return false;
    return true;
}
}
void validateConfig(const AppConfig& c) {
    const auto& p=c.ptz; const auto& t=c.tracking;
    if (!identifier(c.cameraId)||!identifier(c.rtspPath)||c.rtspPort<1||c.rtspPort>65535||
        c.controlPort<1||c.controlPort>65535||c.controlPort==c.rtspPort)
        throw std::invalid_argument("Invalid camera/network configuration");
    // 사용 하드웨어 핀은 고정한다. 잘못된 파일로 전원/다른 회로 핀을 열지 않는다.
    if (p.panPin!=18||p.tiltPin!=19||p.centerButtonPin!=17)
        throw std::invalid_argument("This board requires GPIO18/19/17");
    if (p.panMin<0||p.panMax>180||p.panMin>=p.panMax||p.panCenter<p.panMin||p.panCenter>p.panMax||
        p.tiltMin<0||p.tiltMax>180||p.tiltMin>=p.tiltMax||p.tiltCenter<p.tiltMin||p.tiltCenter>p.tiltMax)
        throw std::invalid_argument("Invalid servo range/center");
    if (!std::isfinite(p.targetSpeed)||!std::isfinite(p.maxVelocity)||p.targetSpeed<=0||
        p.targetSpeed>30||p.maxVelocity<=0||p.maxVelocity>30||p.buttonDebounceMs<30||
        p.buttonDebounceMs>100||p.buttonGlitchMs<1||p.buttonGlitchMs>p.buttonDebounceMs)
        throw std::invalid_argument("Invalid servo speed/debounce");
    if (t.deadZoneX<0||t.deadZoneX>=640||t.deadZoneY<0||t.deadZoneY>=360||t.lostTimeoutMs<100||
        t.lostTimeoutMs>10000||t.maxFrameAgeMs<50||t.maxFrameAgeMs>2000||t.manualResumeMs<100||
        t.manualResumeMs>5000||t.metadataHz<1||t.metadataHz>10)
        throw std::invalid_argument("Invalid tracking configuration");
}
void loadAppConfig(const std::string& path) {
    std::ifstream file(path,std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open camera config: "+path);
    std::string input;
    char bytes[4096];
    while (file.read(bytes,sizeof(bytes))||file.gcount()) {
        input.append(bytes,static_cast<std::size_t>(file.gcount()));
        if (input.size()>65536) throw std::invalid_argument("Config exceeds 64 KiB");
    }
    if (file.bad()) throw std::runtime_error("Config read failed");
    const auto j=Json::parse(input);
    keys(j,{"cameraId","rtsp","network","ptz","tracking"});
    AppConfig c;
    string(j,"cameraId",c.cameraId);
    if (j.contains("rtsp")) { const auto& r=j["rtsp"]; keys(r,{"port","path"}); integer(r,"port",c.rtspPort); string(r,"path",c.rtspPath); }
    if (j.contains("network")) { const auto& n=j["network"]; keys(n,{"controlPort"}); integer(n,"controlPort",c.controlPort); }
    if (j.contains("ptz")) {
        const auto& p=j["ptz"]; keys(p,{"panPin","tiltPin","centerButtonPin","panCenter","tiltCenter","panMin","panMax","tiltMin","tiltMax","invertPan","invertTilt","targetSpeed","maxVelocity","buttonDebounceMs","buttonGlitchMs"});
        integer(p,"panPin",c.ptz.panPin); integer(p,"tiltPin",c.ptz.tiltPin); integer(p,"centerButtonPin",c.ptz.centerButtonPin);
        integer(p,"panCenter",c.ptz.panCenter); integer(p,"tiltCenter",c.ptz.tiltCenter);
        integer(p,"panMin",c.ptz.panMin); integer(p,"panMax",c.ptz.panMax); integer(p,"tiltMin",c.ptz.tiltMin); integer(p,"tiltMax",c.ptz.tiltMax);
        integer(p,"buttonDebounceMs",c.ptz.buttonDebounceMs); integer(p,"buttonGlitchMs",c.ptz.buttonGlitchMs);
        boolean(p,"invertPan",c.ptz.invertPan); boolean(p,"invertTilt",c.ptz.invertTilt);
        real(p,"targetSpeed",c.ptz.targetSpeed); real(p,"maxVelocity",c.ptz.maxVelocity);
    }
    if (j.contains("tracking")) {
        const auto& t=j["tracking"]; keys(t,{"enabled","detectionWhenDisabled","deadZoneX","deadZoneY","lostTimeoutMs","maxFrameAgeMs","manualResumeMs","metadataHz"});
        boolean(t,"enabled",c.tracking.enabled); boolean(t,"detectionWhenDisabled",c.tracking.detectionWhenDisabled);
        integer(t,"deadZoneX",c.tracking.deadZoneX); integer(t,"deadZoneY",c.tracking.deadZoneY);
        integer(t,"lostTimeoutMs",c.tracking.lostTimeoutMs); integer(t,"maxFrameAgeMs",c.tracking.maxFrameAgeMs);
        integer(t,"manualResumeMs",c.tracking.manualResumeMs); integer(t,"metadataHz",c.tracking.metadataHz);
    }
    validateConfig(c); appConfig()=std::move(c);
}
void loadDefaultConfig() {
    if (const char* path=std::getenv("PTZ_CONFIG_FILE")) { loadAppConfig(path); return; }
    struct stat info{};
    if (stat("/etc/ptz-camera/camera.json",&info)==0) loadAppConfig("/etc/ptz-camera/camera.json");
    else if (errno==ENOENT) validateConfig(appConfig());
    else throw std::runtime_error("Cannot access camera configuration");
}
} // namespace edge
