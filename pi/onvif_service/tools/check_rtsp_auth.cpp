/**
 * [학습 안내] 실제 MediaMTX RTSP Digest 접근 확인
 * 같은 TCP 연결에서 DESCRIBE → challenge → Digest 재요청을 수행한다.
 * 비밀번호는 파일에서 읽고 해시 응답만 전송한다. IPv4 URI/포트/응답 크기를 검증한다.
 * 200/SDP는 DESCRIBE 성공이다. 전체 영상 PLAY/렌더링 지연을 측정한 결과는 아니다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
// MediaMTX의 RTSP Digest challenge에 같은 TCP 연결에서 응답한다.
// 비밀번호는 명령행/URL/로그에 넣지 않는다. DESCRIBE까지만 검증하며 PLAY는 하지 않는다.
#include "auth/digest.h"
#include "xml/xml.h"
#include <openssl/rand.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
namespace {
struct Socket {
    int fd = -1;
    ~Socket() {
        if (fd >= 0)
            close(fd);
    }
};
struct Reply {
    int status;
    std::map<std::string, std::string> headers;
    std::string body;
};
/**
 * 헤더 이름을 소문자로 통일해 HTTP의 대소문자 무관 비교를 적용한다.
 */
std::string lower(std::string text) {
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
/**
 * 같은 TCP 소켓에 RTSP DESCRIBE를 보내고 header/body 길이를 기준으로 응답을 읽는다. 전송 경계와 메시지 경계를 구별한다.
 */
Reply request(int fd, const std::string &uri, unsigned sequence,
              const std::string &authorization = {}) {
    const auto text = "DESCRIBE " + uri + " RTSP/1.0\r\nCSeq: " + std::to_string(sequence) +
                      "\r\nAccept: application/sdp\r\n" +
                      (authorization.empty() ? "" : "Authorization: " + authorization + "\r\n") +
                      "\r\n";
    std::size_t sent = 0;
    while (sent < text.size()) {
        const auto count = send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
        if (count <= 0)
            throw std::runtime_error("Cannot send RTSP request");
        sent += count;
    }
    std::string input;
    std::size_t separator;
    auto readMore = [&] {
        char bytes[4096];
        const auto count = recv(fd, bytes, sizeof(bytes), 0);
        if (count <= 0)
            throw std::runtime_error("RTSP receive timeout/closed connection");
        input.append(bytes, count);
        if (input.size() > 65536)
            throw std::runtime_error("RTSP response too large");
    };
    while ((separator = input.find("\r\n\r\n")) == std::string::npos)
        readMore();
    std::istringstream stream(input.substr(0, separator));
    std::string version, line;
    Reply reply{};
    stream >> version >> reply.status;
    std::getline(stream, line);
    while (std::getline(stream, line)) {
        const auto colon = line.find(':');
        if (colon != std::string::npos)
            reply.headers[lower(onvif::trim(line.substr(0, colon)))] =
                onvif::trim(line.substr(colon + 1));
    }
    const auto content = reply.headers.find("content-length");
    unsigned length = 0;
    if (content != reply.headers.end()) {
        const auto &value = content->second;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || length > 65536)
            throw std::runtime_error("Invalid RTSP content length");
    }
    while (input.size() < separator + 4 + length)
        readMore();
    reply.body = input.substr(separator + 4, length);
    return reply;
}
/**
 * RTSP Digest challenge에서 필요한 named 값을 추출한다. 검사 대상은 알려진 MediaMTX challenge다.
 */
std::string field(const std::string &header, const std::string &name) {
    const auto marker = name + "=";
    const auto start = header.find(marker);
    if (start == std::string::npos)
        return {};
    const auto at = start + marker.size();
    if (header[at] == '"') {
        const auto end = header.find('"', at + 1);
        return end == std::string::npos ? "" : header.substr(at + 1, end - at - 1);
    }
    const auto end = header.find(',', at);
    return onvif::trim(header.substr(at, end == std::string::npos ? end : end - at));
}
} // namespace
/**
 * 실행 인자와 자원을 준비한 뒤 위 함수들을 호출한다. 정상/오류 종료를 반환값으로 구분하고 보유 자원을 종료 경로에서 정리한다.
 */
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: check_rtsp_auth rtsp://PI_IPV4:8554/cam\n";
        return 1;
    }
    try {
        const std::string uri = argv[1];
        if (uri.rfind("rtsp://", 0) != 0 || uri.find_first_of("@\r\n ") != std::string::npos)
            throw std::runtime_error("Use IPv4 RTSP URL without embedded credentials");
        const auto slash = uri.find('/', 7);
        const auto colon = uri.find(':', 7);
        if (slash == std::string::npos || colon == std::string::npos || colon > slash)
            throw std::runtime_error("Explicit port and stream path required");
        const auto host = uri.substr(7, colon - 7), port = uri.substr(colon + 1, slash - colon - 1);
        unsigned number = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || !number ||
            number > 65535 || inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
            throw std::runtime_error("Invalid IPv4 endpoint");
        address.sin_port = htons(number);
        Socket connection;
        connection.fd = socket(AF_INET, SOCK_STREAM, 0);
        timeval timeout{25, 0};
        setsockopt(connection.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(connection.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(connection.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
            throw std::runtime_error("Cannot connect RTSP endpoint");
        auto reply = request(connection.fd, uri, 1);
        const char *path = std::getenv("PTZ_CREDENTIALS_FILE");
        if (reply.status == 401 && path) {
            const auto credentials = onvif::loadCredentials(path);
            const auto header = reply.headers.at("www-authenticate");
            if (header.rfind("Digest ", 0) != 0)
                throw std::runtime_error("Digest challenge required");
            const auto realm = field(header, "realm"), nonce = field(header, "nonce");
            auto algorithm = field(header, "algorithm");
            if (algorithm.empty())
                algorithm = "MD5";
            const auto qop = field(header, "qop");
            if (realm.empty() || nonce.empty() || (qop != "" && qop != "auth"))
                throw std::runtime_error("Unsupported Digest challenge");
            const auto ha1 = onvif::hashHex(algorithm, credentials.username + ":" + realm + ":" +
                                                           credentials.password);
            const auto ha2 = onvif::hashHex(algorithm, "DESCRIBE:" + uri);
            std::string parameters, suffix;
            if (qop == "auth") {
                unsigned char random[16];
                if (RAND_bytes(random, sizeof(random)) != 1)
                    throw std::runtime_error("Random generation failed");
                const auto cnonce = onvif::hashHex(
                    "MD5", std::string(reinterpret_cast<char *>(random), sizeof(random)));
                suffix = ":00000001:" + cnonce + ":auth:";
                parameters = ", qop=auth, nc=00000001, cnonce=\"" + cnonce + "\"";
            } else
                suffix = ":";
            const auto digest = onvif::hashHex(algorithm, ha1 + ":" + nonce + suffix + ha2);
            reply = request(connection.fd, uri, 2,
                            "Digest username=\"" + credentials.username + "\", realm=\"" + realm +
                                "\", nonce=\"" + nonce + "\", uri=\"" + uri + "\", response=\"" +
                                digest + "\", algorithm=" + algorithm + parameters);
        }
        const bool sdp = reply.body.find("a=rtpmap:") != std::string::npos;
        std::cout << "RTSP DESCRIBE status=" << reply.status << " SDP=" << sdp << '\n';
        return reply.status == 200 && sdp ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
