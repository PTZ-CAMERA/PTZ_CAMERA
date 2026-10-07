/**
 * [학습 안내] ONVIF 요청에서 GPIO 서비스로 보내는 bounded RPC
 * loopback으로 접속해 명령 한 줄을 보내고 응답 한 줄을 받는다. 전체 기한은 300ms다.
 * GPIO를 직접 초기화하지 않고 RAII 소켓으로 통신 자원을 정리한다.
 * 실패/시간 초과는 BackendError로 올려 SOAP Receiver Fault로 변환한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "ptz/servo_bridge.h"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
namespace onvif {
namespace {
struct Socket {
    int fd = -1;
    ~Socket() {
        if (fd >= 0)
            close(fd);
    }
};
} // namespace
/**
 * NONBLOCK 소켓에 전체 기한을 적용해 연결/부분 송신/개행 수신을 처리한다. 모든 종료 경로에서 소켓을 해제한다.
 */
std::string servoRpc(unsigned port, const std::string &command) {
    Socket connection;
    connection.fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (connection.fd < 0)
        throw BackendError("Servo connection unavailable");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    auto wait = [&](short events) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0)
            throw BackendError("Servo request timed out");
        pollfd ready{connection.fd, events, 0};
        if (poll(&ready, 1, remaining) <= 0 || (ready.revents & (POLLERR | POLLNVAL)))
            throw BackendError("Servo request unavailable");
    };
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(connection.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS)
            throw BackendError("Servo service unavailable");
        wait(POLLOUT);
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(connection.fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error)
            throw BackendError("Servo service unavailable");
    }
    const auto line = command + '\n';
    std::size_t sent = 0;
    while (sent < line.size()) {
        wait(POLLOUT);
        const auto count =
            send(connection.fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
        if (count > 0)
            sent += count;
        else if (errno != EAGAIN && errno != EINTR)
            throw BackendError("Servo request failed");
    }
    std::string input;
    while (input.size() <= 128) {
        wait(POLLIN);
        char bytes[128];
        const auto count = recv(connection.fd, bytes, sizeof(bytes), 0);
        if (count > 0) {
            input.append(bytes, count);
            const auto end = input.find('\n');
            if (end != std::string::npos)
                return input.substr(0, end);
        } else if (count == 0 || (errno != EAGAIN && errno != EINTR))
            throw BackendError("Servo response failed");
    }
    throw BackendError("Invalid servo response length");
}
} // namespace onvif
