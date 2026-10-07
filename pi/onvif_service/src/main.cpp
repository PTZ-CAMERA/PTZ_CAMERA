/**
 * [학습 안내] ONVIF 서비스의 설정/인증/실행 진입점
 * IPv4/포트/UUID/계정 파일을 검증하고 단일 HTTP/UDP 이벤트 루프를 실행한다.
 * 장치 UUID는 machine-id와 프로젝트 namespace로 생성해 재시작 후 유지한다.
 * 운영 실행은 계정 파일 없이 시작하지 않는다. 비밀번호는 stdout이나 argv에 넣지 않는다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "network/server.h"
#include "auth/digest.h"
#include "config/app_config.h"
#include <cstdlib>
#include <algorithm>
#include <charconv>
#include <csignal>
#include <fstream>
#include <cctype>
#include <uuid/uuid.h>
#include <iostream>
#include <stdexcept>
#include <arpa/inet.h>
namespace {
volatile std::sig_atomic_t stop = 0;
/**
 * main의 종료 플래그를 설정해 소켓/구독 정리는 일반 실행 흐름에서 수행하게 한다.
 */
void signalHandler(int) {
    stop = 1;
}
/**
 * 문자열 전체가 1~65535 정수인지 검사한다. 잘못된 설정으로 임의 포트에 바인딩하지 않게 한다.
 */
unsigned port(const std::string &value) {
    unsigned number = 0;
    const auto p = std::from_chars(value.data(), value.data() + value.size(), number);
    if (p.ec != std::errc{} || p.ptr != value.data() + value.size() || number < 1 || number > 65535)
        throw std::runtime_error("Port must be 1..65535");
    return number;
}
/**
 * UUID의 길이/하이픈 위치/16진수 문자를 검증한다. endpoint 식별 문자열 형식을 보장한다.
 */
bool validUuid(const std::string &id) {
    if (id.size() != 36)
        return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-')
                return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(id[i])))
            return false;
    }
    return true;
}
/**
 * 원본 machine-id를 공개하지 않고 프로젝트 namespace UUID로 변환한다. 같은 Pi에서는 서비스 재시작 후 같은 식별자를 얻는다.
 */
std::string machineUuid() {
    std::ifstream file("/etc/machine-id");
    std::string id;
    file >> id;
    if (id.size() != 32 ||
        !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c); }))
        throw std::runtime_error("Cannot read machine-id; supply --uuid UUID");
    // 원본 machine-id 대신 프로젝트 전용 namespace의 안정적인 UUID를 사용한다.
    uuid_t space, result;
    uuid_parse("1e09f8ec-7b3b-46d9-97f2-8b37999342fd", space);
    uuid_generate_sha1(result, space, id.data(), id.size());
    char value[37]{};
    uuid_unparse_lower(result, value);
    return value;
}
} // namespace
/**
 * 실행 인자와 자원을 준비한 뒤 위 함수들을 호출한다. 정상/오류 종료를 반환값으로 구분하고 보유 자원을 종료 경로에서 정리한다.
 */
int main(int argc, char **argv) {
    try {
        edge::loadDefaultConfig();
        onvif::Config config;
        config.rtspPort=edge::appConfig().rtspPort;
        config.servoPort=edge::appConfig().controlPort;
        std::string bind = "0.0.0.0";
        bool allowUnauthenticated = false;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--allow-unauthenticated") {
                allowUnauthenticated = true;
                continue;
            }
            if (key == "--no-discovery") {
                config.discovery = false;
                continue;
            }
            if (key == "--help") {
                std::cout
                    << "ptz_onvif [--interface eth0] [--advertise-ip IPv4] [--bind IPv4] "
                       "[--port 8080] [--rtsp-port 8554] [--servo-port 5000] [--uuid UUID] "
                       "[--no-discovery] [--credentials-file FILE] [--allow-unauthenticated]\n";
                return 0;
            }
            if (++i == argc)
                throw std::runtime_error("Option needs value: " + key);
            const std::string value = argv[i];
            if (key == "--interface")
                config.interface = value;
            else if (key == "--advertise-ip")
                config.ip = value;
            else if (key == "--bind")
                bind = value;
            else if (key == "--port")
                config.port = port(value);
            else if (key == "--servo-port")
                config.servoPort = port(value);
            else if (key == "--rtsp-port")
                config.rtspPort = port(value);
            else if (key == "--credentials-file")
                config.credentialsFile = value;
            else if (key == "--uuid")
                config.uuid = value;
            else
                throw std::runtime_error("Unknown option: " + key);
        }
        if (config.credentialsFile.empty()) {
            if (const char *directory = std::getenv("CREDENTIALS_DIRECTORY"))
                config.credentialsFile = std::string(directory) + "/onvif.auth";
        }
        if (config.credentialsFile.empty() && !allowUnauthenticated)
            throw std::runtime_error("Credentials required; use --credentials-file FILE");
        if (config.credentialsFile.empty() && allowUnauthenticated && bind != "127.0.0.1")
            throw std::runtime_error(
                "Unauthenticated testing is allowed only with --bind 127.0.0.1");
        if (!config.credentialsFile.empty()) {
            onvif::loadCredentials(config.credentialsFile);
            config.authRequired = true;
        }
        if (config.ip.empty()) {
            config.autoAddress = true;
            config.ip = onvif::interfaceAddress(config.interface);
        }
        in_addr address{};
        if (inet_pton(AF_INET, config.ip.c_str(), &address) != 1 ||
            inet_pton(AF_INET, bind.c_str(), &address) != 1)
            throw std::runtime_error("IPv4 address required");
        if (config.uuid.empty())
            config.uuid = machineUuid();
        if (!validUuid(config.uuid))
            throw std::runtime_error("Invalid UUID");
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);
        return onvif::runServer(config, bind, stop);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
