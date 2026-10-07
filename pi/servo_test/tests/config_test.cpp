/** [학습 안내] 잘못된 설정을 적용하지 않고 기존 값을 유지하는지 확인한다. GPIO는 열지 않는다. */
#include "config/app_config.h"
#include "runtime/rate_limiter.h"
#include <cassert>
#include <fstream>
#include <stdexcept>
#include <unistd.h>
int main() {
    char path[]="/tmp/ptz-config-test-XXXXXX";
    const int fd=mkstemp(path); assert(fd>=0); close(fd);
    auto load=[&](const char* content) { std::ofstream file(path); file<<content; file.close(); edge::loadAppConfig(path); };
    load(R"({"cameraId":"CAM01","ptz":{"panMin":30,"panMax":150,"invertPan":true},"tracking":{"metadataHz":10}})");
    assert(edge::appConfig().ptz.panMin==30&&edge::appConfig().ptz.invertPan);
    for (auto bad:{R"({"ptz":{"panMin":160,"panMax":150}})",R"({"ptz":{"panPin":5}})",
        R"({"tracking":{"metadataHz":0}})",R"({"tracking":{"enabled":1}})",R"({"ptz":{"panMin":30.5}})",
        R"({"rtsp":{"path":"../secret"}})",R"({"typo":1})",R"({"ptz":{"panCenter":200}})"}) {
        bool failed=false; try { load(bad); } catch(const std::exception&) { failed=true; }
        assert(failed && edge::appConfig().ptz.panMin==30);
    }
    unlink(path);
    edge::RateLimiter limiter(5); const auto now=edge::RateLimiter::Clock::now();
    assert(limiter.ready(now)); assert(!limiter.ready(now+std::chrono::milliseconds(199)));
    assert(limiter.ready(now+std::chrono::milliseconds(200))); limiter.reset(); assert(limiter.ready(now));
}
