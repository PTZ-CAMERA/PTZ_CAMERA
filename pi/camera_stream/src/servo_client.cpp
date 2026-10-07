/**
 * [학습 안내] Pi 내부 카메라 송출기와 GPIO 서비스의 통신
 * 127.0.0.1:5000에서 CAMERA:HELLO로 등록하고 추적 상태/탐지 결과/작은 이동량을 전달한다.
 * 이 TCP는 내부 IPC다. 외부 Qt의 추적/알림 인터페이스는 ONVIF로 전환했다.
 * send/recv의 경계는 명령 줄 경계와 다르므로 부분 송신과 개행 누적 수신을 처리한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "performance/performance_log.h"
#include "servo_client.h"
#include "config/app_config.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iomanip>
#include <charconv>
#include <sstream>

namespace {
unsigned servoPort() { return static_cast<unsigned>(edge::appConfig().controlPort); }
}

/**
 * 연결을 닫아 카메라 publisher 등록이 서버에서 해제되게 한다. GPIO 종료는 서보 서비스가 담당한다.
 */
ServoClient::~ServoClient() {
    disconnect();
}

/**
 * 소켓과 미완성 응답 버퍼를 함께 초기화한다. 다음 호출은 새 연결과 등록을 다시 시도한다.
 */
void ServoClient::disconnect() {
    if (fd_ >= 0)
        close(fd_);
    fd_ = -1;
    incoming_.clear();
}

/**
 * 연결이 없으면 loopback으로 접속해 CAMERA:HELLO/READY를 교환한다. 단순 상태 조회 연결과 실제 publisher를 구별한다.
 */
bool ServoClient::connectIfNeeded() {
    if (fd_ >= 0)
        return true;
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0)
        return false;
    timeval timeout{0, 200000};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(servoPort());
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0) {
        // 단순한 상태 조회 연결과 실제 카메라 송출기를 구별한다.
        constexpr char hello[] = "CAMERA:HELLO\n";
        std::size_t sent = 0;
        while (sent < sizeof(hello) - 1) {
            const ssize_t n = send(fd_, hello + sent, sizeof(hello) - 1 - sent, MSG_NOSIGNAL);
            if (n <= 0) {
                disconnect();
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        if (readLine() == "CAMERA:READY")
            return true;
    }
    disconnect();
    return false;
}

/**
 * 개행을 붙여 부분 송신이 끝날 때까지 보낸다. 실패하면 연결을 폐기해 다음 호출에서 재연결하게 한다.
 */
bool ServoClient::sendLine(const std::string &line) {
    if (!connectIfNeeded())
        return false;
    const std::string message = line + '\n';
    std::size_t sent = 0;
    while (sent < message.size()) {
        const ssize_t n = send(fd_, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            disconnect();
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

/**
 * recv 조각을 incoming 버퍼에 누적하고 개행 한 줄만 꺼낸다. 다음 줄은 남겨두며 실패/긴 입력은 연결을 정리한다.
 */
std::string ServoClient::readLine() {
    while (incoming_.size() <= 128) {
        const auto end = incoming_.find('\n');
        if (end != std::string::npos) {
            std::string result = incoming_.substr(0, end);
            incoming_.erase(0, end + 1);
            return result;
        }
        char bytes[128];
        const ssize_t n = recv(fd_, bytes, sizeof(bytes), 0);
        if (n <= 0)
            break;
        incoming_.append(bytes, static_cast<std::size_t>(n));
    }
    disconnect();
    return {};
}

/**
 * 서보의 실제 추적 허용 상태를 동기 요청/응답으로 조회한다. 추론/송출 전체를 이 호출과 동일한 처리 방식으로 보지는 않는다.
 */
bool ServoClient::trackingEnabled() {
    ptz::perf::Scope timing("tracking_status_request");
    if (!sendLine("TRACK:STATUS"))
        return false;
    return readLine() == "TRACK:ON";
}

/**
 * 이미 계산된 작은 두 축 이동량을 내부 서비스에 전달한다. 서보 범위 제한과 최종 GPIO 쓰기는 수신 측이 수행한다.
 */
void ServoClient::move(int panDelta, int tiltDelta) {
    sendLine("AUTO:MOVE:" + std::to_string(panDelta) + "," + std::to_string(tiltDelta));
}

/**
 * 선택된 사람의 중심/오차/점수를 내부 관측 알림으로 전달한다. Qt 외부 전달은 ONVIF broker가 담당한다.
 */
void ServoClient::detection(float confidence, int x, int y, int errorX, int errorY) {
    std::ostringstream line;
    line << std::fixed << std::setprecision(2) << "AUTO:DETECT:" << confidence << ',' << x << ','
         << y << ',' << errorX << ',' << errorY;
    sendLine(line.str());
}

/**
 * 사람이 없는 상태를 전달해 이전 좌표가 UI에 남지 않게 한다.
 */
void ServoClient::noDetection() {
    sendLine("AUTO:NO_DETECTION");
}

// 한 번의 snapshot에서 모드와 세대를 함께 읽어 모드 전환 중 불일치를 피한다.
ServoClient::TrackingState ServoClient::trackingState() {
    if (!sendLine("TRACK:SNAPSHOT")) return {};
    const auto line = readLine();
    const auto pos = line.rfind(':');
    if (pos == std::string::npos ||
        (line.substr(0,pos) != "TRACK:ON" && line.substr(0,pos) != "TRACK:OFF")) {
        disconnect(); return {};
    }
    std::uint64_t generation = 0;
    const auto r = std::from_chars(line.data()+pos+1, line.data()+line.size(), generation);
    if (r.ec != std::errc{} || r.ptr != line.data()+line.size()) { disconnect(); return {}; }
    return {line.substr(0,pos) == "TRACK:ON", generation};
}
// AI가 시작할 때 받은 세대를 포함해 이전 모드의 결과를 최종 GPIO 경계에서 폐기한다.
void ServoClient::move(int pan, int tilt, std::uint64_t generation) {
    sendLine("AUTO:MOVE2:" + std::to_string(pan) + "," + std::to_string(tilt) + "," + std::to_string(generation));
}
// bbox/시각은 압축 영상과 별도로 전송한다. SOAP 변환은 ONVIF 서비스가 담당한다.
void ServoClient::detection(const ptz::DetectionMetadata& metadata) {
    sendLine("AUTO:DETECT2:" + ptz::encodeDetection(metadata));
}

// 상태는 변할 때만 worker가 호출한다. 이동보다 별도로 처리되는 메타데이터다.
void ServoClient::targetState(const std::string& state,std::uint64_t generation) {
    sendLine("AUTO:TARGET:"+state+","+std::to_string(generation));
}
void ServoClient::cameraState(const std::string& state) {
    sendLine("CAMERA:STATE:"+state);
}

void ServoClient::streamState(const std::string& state) {
    sendLine("CAMERA:STREAM:"+state);
}
