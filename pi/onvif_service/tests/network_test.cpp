/**
 * [학습 안내] HTTP 서버의 요청 경계/오류 처리 통합 검증
 * 별도 loopback 포트의 자식 서버에 분할 POST를 보내 주소/프로파일/URI를 확인한다.
 * 알 수 없는 경로, 큰 본문, 중복 길이, chunked 요청 거부도 확인한다.
 * 실물 모터 이동 명령을 보내지 않는다. 종료 시 자식 서버를 정리한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "protocol/service.h"
#include "xml/xml.h"
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
namespace {
struct Child {
    pid_t pid = -1;
    ~Child() {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
    }
};
/**
 * 테스트 loopback 서버에 연결하고 수신 기한을 설정한다. 실패한 FD는 닫는다.
 */
int connect(unsigned port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    timeval timeout{4, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return fd;
}
/**
 * 테스트 HTTP 문자열을 보내 연결 종료까지 받는다. peer의 소켓 조각과 완성된 메시지를 혼동하지 않는다.
 */
std::string exchange(unsigned port, const std::string &request, bool split = false) {
    int fd = connect(port);
    assert(fd >= 0);
    const auto sendAll = [&](const std::string &data) {
        std::size_t at = 0;
        while (at < data.size()) {
            const auto n = send(fd, data.data() + at, data.size() - at, MSG_NOSIGNAL);
            assert(n > 0);
            at += n;
        }
    };
    if (split) {
        sendAll(request.substr(0, request.size() / 2));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        sendAll(request.substr(request.size() / 2));
    } else
        sendAll(request);
    std::string result;
    char bytes[4096];
    ssize_t count;
    while ((count = recv(fd, bytes, sizeof(bytes), 0)) > 0)
        result.append(bytes, count);
    close(fd);
    return result;
}
/**
 * 테스트 SOAP POST를 실제 소켓으로 보내고 HTTP 200 본문을 읽는다. 일부 요청은 의도적으로 나눠 전송한다.
 */
std::string post(unsigned port, const std::string &path, const std::string &operation) {
    const auto xml = "<e:Envelope xmlns:e=\"" + std::string(onvif::SOAP) + "\" xmlns:d=\"" +
                     onvif::DEVICE + "\" xmlns:m=\"" + onvif::MEDIA + "\" xmlns:t=\"" +
                     onvif::TYPES + "\"><e:Body>" + operation + "</e:Body></e:Envelope>";
    const auto result = exchange(port,
                                 "POST " + path +
                                     " HTTP/1.1\r\nHost: localhost\r\nContent-Type: "
                                     "application/soap+xml\r\nContent-Length: " +
                                     std::to_string(xml.size()) + "\r\n\r\n" + xml,
                                 true);
    assert(result.rfind("HTTP/1.1 200", 0) == 0);
    return result.substr(result.find("\r\n\r\n") + 4);
}
} // namespace
/**
 * 테스트의 입력/실행/검증/정리를 순서대로 수행한다. assert 실패는 해당 계약이 깨졌다는 뜻이며 실물 성능 인증이 아니다.
 */
int main(int argc, char **argv) {
    assert(argc == 2);
    int reservation = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(reservation, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    getsockname(reservation, reinterpret_cast<sockaddr *>(&address), &length);
    const unsigned port = ntohs(address.sin_port);
    close(reservation);
    const auto value = std::to_string(port);
    Child child;
    child.pid = fork();
    assert(child.pid >= 0);
    if (child.pid == 0) {
        execl(argv[1], argv[1], "--bind", "127.0.0.1", "--advertise-ip", "127.0.0.1", "--port",
              value.c_str(), "--no-discovery", "--allow-unauthenticated", nullptr);
        _exit(127);
    }
    bool ready = false;
    for (int i = 0; i < 100; ++i) {
        int fd = connect(port);
        if (fd >= 0) {
            close(fd);
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(ready);
    auto services = onvif::parseXml(
        post(port, "/onvif/device_service",
             "<d:GetServices><d:IncludeCapability>false</d:IncludeCapability></d:GetServices>"));
    const auto *reply =
        services->child(onvif::SOAP, "Body")->child(onvif::DEVICE, "GetServicesResponse");
    assert(reply && reply->children.size() == 4);
    auto profiles = onvif::parseXml(post(port, "/onvif/media_service", "<m:GetProfiles/>"));
    assert(profiles->child(onvif::SOAP, "Body")
               ->child(onvif::MEDIA, "GetProfilesResponse")
               ->child(onvif::MEDIA, "Profiles")
               ->attributes.at("token") == "main");
    auto stream = onvif::parseXml(post(
        port, "/onvif/media_service",
        "<m:GetStreamUri><m:StreamSetup><t:Stream>RTP_unicast</"
        "t:Stream><t:Transport><t:Protocol>RTSP</t:Protocol>"
        "</t:Transport></m:StreamSetup><m:ProfileToken>main</m:ProfileToken></m:GetStreamUri>"));
    assert(stream->child(onvif::SOAP, "Body")
               ->child(onvif::MEDIA, "GetStreamUriResponse")
               ->child(onvif::MEDIA, "MediaUri")
               ->child(onvif::TYPES, "Uri")
               ->text == "rtsp://127.0.0.1:8554/cam");
    assert(exchange(port, "POST /bad HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n")
               .rfind("HTTP/1.1 404", 0) == 0);
    assert(exchange(port, "POST /onvif/media_service HTTP/1.1\r\nContent-Length: 70000\r\n\r\n")
               .rfind("HTTP/1.1 413", 0) == 0);
    assert(
        exchange(
            port,
            "POST /onvif/media_service HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\n")
            .rfind("HTTP/1.1 400", 0) == 0);
    assert(
        exchange(port, "POST /onvif/media_service HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n")
            .rfind("HTTP/1.1 400", 0) == 0);
    std::cout << "PASS HTTP SOAP flow, partial receive, unknown endpoint, limits and headers\n";
}
