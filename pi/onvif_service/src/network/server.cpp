/**
 * [학습 안내] ONVIF HTTP, WS-Discovery와 내부 이벤트 feed의 비동기 입력 루프
 * HTTP를 연결별로 누적하고 Digest 인증 뒤 SOAP 요청을 dispatch한다. UDP Probe에는 unicast로 응답한다.
 * PullMessages는 대기 상태를 저장하고 poll 루프로 돌아간다. 새 이벤트 또는 Timeout 때 HTTP 응답을 완성한다.
 * PTZ 내부 RPC에는 최대 300ms 동기 구간이 있다. 영상/AI와 서보 궤적은 별도 프로세스에서 계속 실행한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "network/server.h"
#include "events/event_broker.h"
#include "control/camera_control.h"
#include "events/servo_feed.h"
#include "auth/digest.h"
#include "xml/xml.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <cctype>
#include <stdexcept>
#include <vector>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
namespace onvif {
/**
 * 선택한 활성 인터페이스의 IPv4를 읽는다. 없으면 광고할 잘못된 URL을 만들지 않고 오류를 올린다.
 */
std::string interfaceAddress(const std::string &name) {
    ifaddrs *addresses = nullptr;
    if (getifaddrs(&addresses) != 0)
        throw std::runtime_error("Cannot read network interfaces");
    std::string ip;
    for (auto *address = addresses; address; address = address->ifa_next)
        if (address->ifa_addr && address->ifa_addr->sa_family == AF_INET &&
            name == address->ifa_name && (address->ifa_flags & IFF_UP)) {
            char value[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(address->ifa_addr)->sin_addr, value,
                      sizeof(value));
            ip = value;
            break;
        }
    freeifaddrs(addresses);
    if (ip.empty())
        throw std::runtime_error("No IPv4 address on interface " + name);
    return ip;
}
namespace {
constexpr std::size_t maxClients = 16, maxHeader = 16384, maxBody = 65536;
struct Client {
    int fd;
    std::string input, output;
    std::size_t sent = 0, headerEnd = 0, bodyLength = 0;
    std::string path;
    std::chrono::steady_clock::time_point deadline;
    unsigned id = 0; // FD 재사용과 구별되는 이 HTTP 요청의 소유자 ID.
    bool pending = false; // 이벤트 대기 중이며 이미 완료된 Body를 다시 수신하지 않는다.
};
/**
 * 상태/Content-Type/Content-Length/추가 헤더를 붙여 한 HTTP 응답을 구성한다. 본문 길이는 바이트 수 기준이다.
 */
std::string http(int status, const std::string &body, bool soap = false,
                 const std::string &extra = {}, const char *mime = nullptr) {
    const std::map<int, std::string> reasons{{200, "OK"},
                                             {401, "Unauthorized"},
                                             {400, "Bad Request"},
                                             {404, "Not Found"},
                                             {405, "Method Not Allowed"},
                                             {408, "Request Timeout"},
                                             {413, "Content Too Large"},
                                             {431, "Request Header Fields Too Large"},
                                             {500, "Internal Server Error"}};
    return "HTTP/1.1 " + std::to_string(status) + " " + reasons.at(status) + "\r\nContent-Type: " +
           (mime   ? mime
            : soap ? "application/soap+xml; charset=utf-8"
                   : "text/plain; charset=utf-8") +
           "\r\n" + extra + "Content-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}
/**
 * 헤더 이름을 소문자로 통일해 HTTP의 대소문자 무관 비교를 적용한다.
 */
std::string lower(std::string value) {
    for (char &c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
// TCP 수신 경계와 HTTP 본문 경계는 다르다. 누적 버퍼에서 완성된 요청만 처리한다.
/**
 * 헤더/길이/경로/Digest를 확인하고 완성된 본문만 SOAP에 전달한다. deferred Pull은 입력 버퍼와 request ID를 유지한 채 루프로 돌아간다.
 */
void readRequest(Client &client, const Config &config, DigestAuth *auth) {
    if (!client.headerEnd) {
        const auto separator = client.input.find("\r\n\r\n");
        if (separator == std::string::npos) {
            if (client.input.size() > maxHeader)
                client.output = http(431, "Header too large\n");
            return;
        }
        if (separator > maxHeader) {
            client.output = http(431, "Header too large\n");
            return;
        }
        const auto lineEnd = client.input.find("\r\n");
        std::istringstream first(client.input.substr(0, lineEnd));
        std::string method, version, extra;
        if (!(first >> method >> client.path >> version) || (first >> extra) ||
            (version != "HTTP/1.1" && version != "HTTP/1.0")) {
            client.output = http(400, "Invalid request line\n");
            return;
        }
        if (method == "GET" && client.path == "/onvif/topics.xml") {
            client.output = http(200, topicNamespaceXml(), false, {}, "application/xml");
            return;
        }
        if (method != "POST") {
            client.output = http(405, "Use SOAP POST\n");
            return;
        }
        if (client.path != "/onvif/device_service" && client.path != "/onvif/media_service" &&
            client.path != "/onvif/ptz_service" && client.path != "/onvif/events_service" && client.path != "/camera/control" &&
            client.path.rfind("/onvif/pullpoint/", 0) != 0) {
            client.output = http(404, "Unknown endpoint\n");
            return;
        }
        std::map<std::string, std::string> headers;
        std::size_t offset = lineEnd + 2;
        while (offset < separator) {
            const auto end = client.input.find("\r\n", offset);
            const auto colon = client.input.find(':', offset);
            if (colon == std::string::npos || colon >= end) {
                client.output = http(400, "Invalid header\n");
                return;
            }
            const auto key = lower(trim(client.input.substr(offset, colon - offset)));
            if (headers.count(key)) {
                client.output = http(400, "Duplicate header\n");
                return;
            }
            headers[key] = trim(client.input.substr(colon + 1, end - colon - 1));
            offset = end + 2;
        }
        if (client.path=="/camera/control" && lower(headers["content-type"]).rfind("application/json",0)!=0) {
            client.output=http(400,"JSON Content-Type required\n"); return;
        }
        if (headers.count("transfer-encoding") || !headers.count("content-length")) {
            client.output = http(400, "Content-Length required; chunked requests unsupported\n");
            return;
        }
        const auto &length = headers.at("content-length");
        const auto parsed =
            std::from_chars(length.data(), length.data() + length.size(), client.bodyLength);
        if (parsed.ec != std::errc{} || parsed.ptr != length.data() + length.size()) {
            client.output = http(400, "Invalid Content-Length\n");
            return;
        }
        if (client.bodyLength > maxBody) {
            client.output = http(413, "Body too large\n");
            return;
        }
        if (auth && !auth->verify(headers["authorization"], method, client.path)) {
            client.output = http(401, "Authentication required\n", false, auth->challenge());
            std::cout << "ONVIF authentication rejected" << std::endl;
            return;
        }
        client.headerEnd = separator + 4;
    }
    // 본문 일부만 도착했으면 다음 POLLIN까지 기다린다.
    if (client.input.size() < client.headerEnd + client.bodyLength)
        return;
    Config current = config;
    current.requestId = client.id;
    if (current.autoAddress)
        current.ip = interfaceAddress(current.interface);
    if (client.path=="/camera/control") {
        const auto result=cameraControl(current,client.input.substr(client.headerEnd,client.bodyLength));
        client.output=http(result.status,result.xml,false,{},"application/json");
        return;
    }
    const auto result = soapResponse(current, client.path,
                                     client.input.substr(client.headerEnd, client.bodyLength));
    if (result.deferred) {
        if (!client.pending)
            client.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        client.pending = true;
        return;
    }
    client.pending = false;
    client.output = http(result.status, result.xml, true);
    std::cout << "ONVIF POST " << client.path << " status=" << result.status
              << " bytes=" << client.bodyLength << std::endl;
}
/**
 * HTTP listener를 NONBLOCK/CLOEXEC으로 생성한다. 이후 읽기/쓰기 준비는 poll에서 기다린다.
 */
int openTcp(const Config &config, const std::string &bindAddress) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        throw std::runtime_error(std::strerror(errno));
    const int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
        listen(fd, maxClients) < 0) {
        const auto error = std::string(std::strerror(errno));
        close(fd);
        throw std::runtime_error("HTTP listener: " + error);
    }
    return fd;
}
/**
 * 선택 NIC에서 WS-Discovery multicast를 수신한다. 검색 응답의 목적지와 HTTP 주소 광고는 별도로 처리한다.
 */
int openDiscovery(const Config &config) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        throw std::runtime_error(std::strerror(errno));
    const int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(3702);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    ip_mreq membership{};
    inet_pton(AF_INET, "239.255.255.250", &membership.imr_multiaddr);
    const auto localIp = interfaceAddress(config.interface);
    inet_pton(AF_INET, localIp.c_str(), &membership.imr_interface);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) < 0) {
        const auto error = std::string(std::strerror(errno));
        close(fd);
        throw std::runtime_error("Discovery listener: " + error);
    }
    return fd;
}
} // namespace
/**
 * HTTP/UDP/backend feed를 한 poll 루프에서 처리한다. pending pull은 재확인하고 연결 종료 때 broker의 요청 소유권도 해제한다.
 */
int runServer(const Config &config, const std::string &bindAddress,
              volatile std::sig_atomic_t &stop) {
    int listener = -1, discovery = -1;
    std::vector<Client> clients;
    try {
        std::unique_ptr<DigestAuth> auth;
        if (config.authRequired)
            auth = std::make_unique<DigestAuth>(loadCredentials(config.credentialsFile));
        Config runtime = config;
        if (!runtime.events)
            runtime.events = std::make_shared<EventBroker>();
        ServoFeed feed(runtime.servoPort, *runtime.events);
        unsigned nextClientId = 0;
        listener = openTcp(config, bindAddress);
        if (config.discovery)
            discovery = openDiscovery(config);
        std::cout << "ONVIF device=" << config.deviceUrl() << " media=" << config.mediaUrl()
                  << " stream=" << config.streamUrl() << " discovery=" << config.discovery
                  << std::endl;
        while (!stop) {
            // 연결별 읽기/쓰기 준비 상태만 기다린다. 느린 클라이언트 때문에 recv에서 막히지 않는다.
            feed.reconnectIfDue();
            std::vector<pollfd> fds{
                {listener, POLLIN, 0}, {discovery, POLLIN, 0}, {feed.fd(), feed.events(), 0}};
            for (const auto &client : clients)
                fds.push_back(
                    {client.fd, static_cast<short>(client.output.empty() ? POLLIN : POLLOUT), 0});
            const int ready = poll(fds.data(), fds.size(), 100);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error("poll failed");
            }
            feed.process(fds[2].revents);
            if (discovery >= 0 && (fds[1].revents & POLLIN)) {
                char data[8192];
                sockaddr_in peer{};
                socklen_t length = sizeof(peer);
                const auto count = recvfrom(discovery, data, sizeof(data), MSG_TRUNC,
                                            reinterpret_cast<sockaddr *>(&peer), &length);
                if (count > 0 && count <= static_cast<ssize_t>(sizeof(data))) {
                    Config current = config;
                    if (current.autoAddress)
                        current.ip = interfaceAddress(current.interface);
                    const auto response = probeResponse(current, std::string(data, count));
                    if (!response.empty()) {
                        // multicast 검색 요청에 대한 응답은 요청자의 주소/포트로 unicast 전송한다.
                        sendto(discovery, response.data(), response.size(), MSG_NOSIGNAL,
                               reinterpret_cast<sockaddr *>(&peer), length);
                        std::cout << "ONVIF ProbeMatch returned" << std::endl;
                    }
                }
            }
            // 기존 클라이언트부터 처리해 새 연결 추가 시 poll 인덱스가 변하지 않게 한다.
            for (std::size_t i = clients.size(); i > 0; --i) {
                auto &client = clients[i - 1];
                const auto flags = fds[i + 2].revents;
                bool closeClient = false;
                if (client.pending)
                    readRequest(client, runtime, auth.get());
                if (flags & POLLIN) {
                    char bytes[8192];
                    const auto count = recv(client.fd, bytes, sizeof(bytes), 0);
                    if (count > 0) {
                        if (client.pending)
                            closeClient = true;
                        else {
                            client.input.append(bytes, count);
                            readRequest(client, runtime, auth.get());
                        }
                    } else if (count == 0 ||
                               (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                        closeClient = true;
                }
                if (flags & POLLOUT) {
                    const auto count = send(client.fd, client.output.data() + client.sent,
                                            client.output.size() - client.sent, MSG_NOSIGNAL);
                    if (count > 0) {
                        client.sent += count;
                        if (client.sent == client.output.size())
                            closeClient = true;
                    } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                               errno != EINTR)
                        closeClient = true;
                }
                // 연결 시작 시 정한 기한을 넘기면 닫아 불완전한 요청이 슬롯을 계속 점유하지 않게 한다.
                if ((flags & (POLLERR | POLLNVAL)) ||
                    std::chrono::steady_clock::now() > client.deadline)
                    closeClient = true;
                if ((flags & POLLHUP) && client.output.empty())
                    closeClient = true;
                if (closeClient) {
                    runtime.events->cancelOwner(client.id);
                    close(client.fd);
                    clients.erase(clients.begin() + static_cast<long>(i - 1));
                }
            }
            if (fds[0].revents & POLLIN) {
                const int fd = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd >= 0) {
                    if (clients.size() >= maxClients)
                        close(fd);
                    else
                        clients.push_back(
                            {fd,
                             {},
                             {},
                             0,
                             0,
                             0,
                             {},
                             std::chrono::steady_clock::now() + std::chrono::seconds(3),
                             ++nextClientId,
                             false});
                }
            }
        }
        for (const auto &client : clients)
            close(client.fd);
        if (discovery >= 0)
            close(discovery);
        close(listener);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        for (const auto &client : clients)
            close(client.fd);
        if (discovery >= 0)
            close(discovery);
        if (listener >= 0)
            close(listener);
        return 1;
    }
}
} // namespace onvif
