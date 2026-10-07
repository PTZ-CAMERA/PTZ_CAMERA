/**
 * [학습 안내] 서보 내부 알림을 ONVIF broker로 가져오는 비동기 어댑터
 * loopback 관측 연결을 유지해 CAMERA/TRACK/DETECT 상태를 broker에 반영한다.
 * poll의 읽기/쓰기 준비 신호만 처리하며 연결 실패 때 1초 후 재접속한다.
 * AI 경로에 SOAP 직렬화를 넣지 않는다. 단순 상태 통신이므로 영상/추론의 처리 경로를 유지한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "events/servo_feed.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
namespace onvif {
namespace {
const std::string watch = "ONVIF:WATCH\n";
}
/**
 * 내부 관측 소켓을 닫는다. GPIO/자동 추적 설정을 이 소멸자에서 임의로 변경하지 않는다.
 */
ServoFeed::~ServoFeed() {
    if (fd_ >= 0)
        close(fd_);
}
/**
 * 소켓/부분 줄 상태를 정리하고 broker 가용성을 false로 알린다. 다음 접속은 backoff 이후다.
 */
void ServoFeed::disconnect() {
    if (fd_ >= 0)
        close(fd_);
    fd_ = -1;
    connecting_ = false;
    sent_ = 0;
    input_.clear();
    retry_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    broker_.setAvailable(false);
}
/**
 * 기한이 되었을 때만 비동기 접속을 시도한다. 실패 중 매 poll마다 connect를 반복하지 않는다.
 */
void ServoFeed::reconnectIfDue() {
    if (fd_ >= 0 || std::chrono::steady_clock::now() < retry_)
        return;
    fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        disconnect();
        return;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
        connecting_ = false;
    else if (errno == EINPROGRESS)
        connecting_ = true;
    else
        disconnect();
}
/**
 * 현재 연결/관측 등록/수신 단계에 필요한 poll 이벤트를 선택한다.
 */
short ServoFeed::events() const {
    return connecting_ || sent_ < watch.size() ? POLLOUT : POLLIN;
}
/**
 * 접속 완료/부분 WATCH 송신/줄 수신을 처리한다. 새 알림이 오면 같은 루프의 pending Pull 응답을 깨울 자료를 갱신한다.
 */
void ServoFeed::process(short flags) {
    if (fd_ < 0)
        return;
    if (flags & (POLLERR | POLLHUP | POLLNVAL)) {
        disconnect();
        return;
    }
    if (flags & POLLOUT) {
        if (connecting_) {
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error) {
                disconnect();
                return;
            }
            connecting_ = false;
        }
        const auto count = send(fd_, watch.data() + sent_, watch.size() - sent_, MSG_NOSIGNAL);
        if (count > 0)
            sent_ += count;
        else if (count < 0 && errno != EAGAIN && errno != EINTR) {
            disconnect();
            return;
        }
    }
    if (flags & POLLIN) {
        char bytes[1024];
        const auto count = recv(fd_, bytes, sizeof(bytes), 0);
        if (count <= 0) {
            if (count == 0 || (errno != EAGAIN && errno != EINTR))
                disconnect();
            return;
        }
        input_.append(bytes, count);
        if (input_.size() > 8192) {
            disconnect();
            return;
        }
        std::size_t end;
        while ((end = input_.find('\n')) != std::string::npos) {
            const auto line = input_.substr(0, end);
            if (line.rfind("ERR", 0) == 0) {
                disconnect();
                return;
            }
            broker_.setAvailable(true);
            broker_.ingest(line);
            input_.erase(0, end + 1);
        }
    }
}
} // namespace onvif
