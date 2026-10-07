/**
 * [학습 안내] ONVIF WS-Discovery multicast 검색 클라이언트
 * 선택한 로컬 IPv4에서 Probe를 보내고 요청 MessageID와 대응하는 ProbeMatches만 받는다.
 * 장치 XAddrs를 출력하며 인증/영상/PTZ 제어는 하지 않는다.
 * UDP 손실이나 LAN multicast 차단 때문에 응답이 없을 수 있다. 일정 시간 후 종료한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "protocol/service.h"
#include "xml/xml.h"
#include <iostream>
#include <fstream>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
/**
 * 실행 인자와 자원을 준비한 뒤 위 함수들을 호출한다. 정상/오류 종료를 반환값으로 구분하고 보유 자원을 종료 경로에서 정리한다.
 */
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: onvif_probe LOCAL_IPV4 [DEST_IPV4=239.255.255.250]\n";
        return 1;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 1;
    in_addr local{};
    if (inet_pton(AF_INET, argv[1], &local) != 1) {
        close(fd);
        return 1;
    }
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &local, sizeof(local)) < 0) {
        close(fd);
        return 1;
    }
    const unsigned char ttl = 2;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(3702);
    if (inet_pton(AF_INET, argc == 3 ? argv[2] : "239.255.255.250", &address.sin_addr) != 1) {
        close(fd);
        return 1;
    }
    std::ifstream file("/proc/sys/kernel/random/uuid");
    std::string uuid;
    file >> uuid;
    const std::string id = "urn:uuid:" + uuid;
    const std::string xml =
        "<s:Envelope xmlns:s=\"" + std::string(onvif::SOAP) +
        "\" xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" xmlns:d=\"" +
        onvif::DISCOVERY +
        "\" xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"><s:Header><a:MessageID>" + id +
        "</a:MessageID><a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To><a:Action>" +
        onvif::DISCOVERY +
        "/Probe</a:Action></s:Header><s:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types>"
        "</d:Probe></s:Body></s:Envelope>";
    timeval timeout{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (sendto(fd, xml.data(), xml.size(), 0, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) < 0) {
        close(fd);
        return 1;
    }
    int result = 1;
    for (int i = 0; i < 16; ++i) {
        char buffer[8192];
        const auto count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0)
            break;
        try {
            auto doc = onvif::parseXml(std::string(buffer, count));
            const auto *h = doc->child(onvif::SOAP, "Header");
            const auto *relates =
                h ? h->child("http://schemas.xmlsoap.org/ws/2004/08/addressing", "RelatesTo")
                  : nullptr;
            if (!relates || onvif::trim(relates->text) != id)
                continue;
            const auto *body = doc->child(onvif::SOAP, "Body");
            const auto *matches = body ? body->child(onvif::DISCOVERY, "ProbeMatches") : nullptr;
            if (!matches)
                continue;
            for (const auto &match : matches->children)
                if (const auto *xaddr = match->child(onvif::DISCOVERY, "XAddrs")) {
                    std::cout << onvif::trim(xaddr->text) << '\n';
                    result = 0;
                }
            if (result == 0)
                break;
        } catch (...) {
            continue;
        }
    }
    close(fd);
    if (result)
        std::cerr << "No matching ProbeMatches within timeout\n";
    return result;
}
