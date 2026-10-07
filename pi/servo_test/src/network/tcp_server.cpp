/**
 * [학습 안내] Pi 내부 제어/관측 TCP 연결의 관리
 * loopback listener와 연결별 개행 수신 버퍼를 관리하고 protocol 처리기로 전달한다.
 * 여러 연결을 select 한 루프에서 다룬다. 명령별 새 스레드를 만들지 않는다.
 * 외부 Qt는 이 포트에 접속하지 않는다. 관측자로 등록한 ONVIF 서비스에만 내부 알림을 전달한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "network/tcp_server.h"
#include "protocol/command_protocol.h"
#include "config.h"

#include <algorithm>
#include <cerrno>
#include <iostream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ptz {
/**
 * Pi 내부 loopback에 TCP listener를 만든다. bind/listen 실패 때 소켓을 닫고 원인 errno를 보존한다.
 */
int openControlServer() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    const int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(CONTROL_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
        listen(fd, static_cast<int>(MAX_CLIENTS)) < 0) {
        const int savedError = errno;
        close(fd);
        errno = savedError;
        return -1;
    }
    return fd;
}

/**
 * 로컬 키보드 입력이 이미 실행 중인 서비스에 붙도록 연결한다. 실패하면 직접 모드 선택 여부를 상위에서 결정한다.
 */
int connectLocalServer() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(CONTROL_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/**
 * 짧은 응답을 nonblocking 최선 노력으로 전송한다. 연결이 끊겨도 SIGPIPE로 프로세스를 종료하지 않도록 한다.
 */
void reply(int fd, const std::string &message) {
    // 부분 송신을 처리하되 읽지 않는 관측자 때문에 GPIO loop를 기다리게 하지 않는다.
    // 큐가 꽉 차면 연결을 종료한다. 재접속 WATCH의 snapshot으로 상태를 복구한다.
    std::size_t sent = 0;
    while (sent < message.size()) {
        const auto n = send(fd,message.data()+sent,message.size()-sent,MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) sent += static_cast<std::size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else { shutdown(fd,SHUT_RDWR); return; }
    }
}

/**
 * 이전 이름을 유지한 내부 알림 함수다. 현재는 관측자로 등록한 ONVIF feed에만 전송하고 외부 Qt에 직접 방송하지 않는다.
 */
void broadcastQt(const std::vector<TcpClient> &clients, const std::string &line) {
    for (const auto &client : clients) {
        if (client.observer)
            reply(client.fd, line);
    }
}

namespace {
/**
 * 부분 수신을 연결 버퍼에 누적해 개행별로 처리한다. 길이 초과/연결 종료는 false로 알려 해당 연결을 닫게 한다.
 */
bool receiveClient(TcpClient &client, std::vector<TcpClient> &clients) {
    char input[128];
    const ssize_t count = recv(client.fd, input, sizeof(input), 0);
    if (count <= 0) {
        return false;
    }
    client.buffer.append(input, static_cast<std::size_t>(count));

    std::size_t end;
    while ((end = client.buffer.find('\n')) != std::string::npos) {
        const std::string command = client.buffer.substr(0, end);
        client.buffer.erase(0, end + 1);
        if (command.size() > MAX_COMMAND_LENGTH) {
            reply(client.fd, "ERR command too long\n");
            return false;
        }
        handleTcpCommand(client, clients, command);
    }

    if (client.buffer.size() > MAX_COMMAND_LENGTH) {
        reply(client.fd, "ERR command too long\n");
        return false;
    }
    return true;
}

} // namespace

/**
 * listener와 기존 소켓을 select 집합에 추가하고 최대 FD를 갱신한다.
 */
void addServerReadFds(const TcpServer &server, fd_set &readFds, int &maxFd) {
    if (server.fd >= 0) {
        FD_SET(server.fd, &readFds);
        maxFd = std::max(maxFd, server.fd);
    }
    for (const auto &client : server.clients) {
        FD_SET(client.fd, &readFds);
        maxFd = std::max(maxFd, client.fd);
    }
}

/**
 * 새 연결과 준비된 기존 입력을 처리한다. 삭제 중 iterator를 갱신하고 publisher 종료는 상태 알림으로 연결한다.
 */
void processServerReadFds(TcpServer &server, const fd_set &readFds) {
    if (server.fd >= 0 && FD_ISSET(server.fd, &readFds)) {
        sockaddr_in peerAddress{};
        socklen_t peerLength = sizeof(peerAddress);
        const int fd = accept(server.fd, reinterpret_cast<sockaddr *>(&peerAddress), &peerLength);
        if (fd >= FD_SETSIZE || server.clients.size() >= MAX_CLIENTS) {
            if (fd >= 0)
                close(fd);
        } else if (fd >= 0) {
            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &peerAddress.sin_addr, ip, sizeof(ip));
            const std::string peer =
                std::string(ip) + ":" + std::to_string(ntohs(peerAddress.sin_port));
            server.clients.push_back({fd, {}, peer});
            std::cout << "TCP client connected: " << peer << std::endl;
            if (hasCameraPublisher() && peer.rfind("127.0.0.1:", 0) != 0)
                reply(fd, "CAMERA:CONNECTED\n");
        }
    }
    for (auto it = server.clients.begin(); it != server.clients.end();) {
        if (FD_ISSET(it->fd, &readFds) && !receiveClient(*it, server.clients)) {
            std::cout << "TCP client disconnected: " << it->peer << std::endl;
            cameraPublisherDisconnected(*it, server.clients);
            close(it->fd);
            it = server.clients.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * 모든 연결/listener를 닫고 컨테이너를 비운다. GPIO 정리는 이 함수 밖의 생명주기 단계다.
 */
void closeControlServer(TcpServer &server) {
    for (const auto &client : server.clients)
        close(client.fd);
    server.clients.clear();
    if (server.fd >= 0)
        close(server.fd);
    server.fd = -1;
}
} // namespace ptz
