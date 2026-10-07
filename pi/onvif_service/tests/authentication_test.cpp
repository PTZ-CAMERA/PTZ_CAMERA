/**
 * [학습 안내] Digest 인증과 replay/URI 바인딩 검증
 * 알려진 해시값과 MD5/SHA-256 응답, 같은 nc 재사용, 순서가 바뀐 nc를 확인한다.
 * 가짜 계정의 임시 파일과 자식 HTTP 서버에서 libcurl 로그인 성공/실패를 검증한다.
 * 테스트 계정은 운영 계정과 다르다. 실제 비밀번호를 테스트에 하드코딩하지 않는다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "auth/digest.h"
#include "protocol/service.h"
#include <curl/curl.h>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
namespace {
struct Child {
    pid_t pid = -1;
    std::string path;
    ~Child() {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
        if (!path.empty())
            unlink(path.c_str());
    }
};
/** 응답 내용을 저장/출력하지 않고 수신 바이트만 승인한다. 인증 테스트에서는 상태 코드가 검증 대상이다. */
std::size_t discard(char *, std::size_t a, std::size_t b, void *) {
    return a * b;
}
/**
 * 테스트 계정의 누락/실패/성공 HTTP 상태를 얻는다. 공개 서버의 실제 계정으로 실행하지 않는다.
 */
long call(const std::string &url, const char *password = nullptr) {
    CURL *handle = curl_easy_init();
    assert(handle);
    const std::string xml = "<s:Envelope xmlns:s=\"" + std::string(onvif::SOAP) + "\" xmlns:m=\"" +
                            onvif::MEDIA + "\"><s:Body><m:GetProfiles/></s:Body></s:Envelope>";
    auto *headers = curl_slist_append(nullptr, "Content-Type: application/soap+xml");
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, xml.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discard);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 3000L);
    if (password) {
        curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
        curl_easy_setopt(handle, CURLOPT_USERNAME, "test-user");
        curl_easy_setopt(handle, CURLOPT_PASSWORD, password);
    }
    const auto code = curl_easy_perform(handle);
    assert(code == CURLE_OK);
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    return status;
}
/**
 * 테스트용 nonce와 요청 정보로 Digest authorization을 만든다. 운영 비밀번호를 사용하지 않는다.
 */
std::string header(onvif::DigestAuth &auth, const std::string &path, const std::string &algorithm,
                   unsigned count, const std::string &sharedNonce = {}) {
    const auto challenge = auth.challenge();
    const auto at = challenge.find("nonce=\"") + 7;
    const auto nonce =
        sharedNonce.empty() ? challenge.substr(at, challenge.find('"', at) - at) : sharedNonce;
    std::ostringstream nc;
    nc << std::hex;
    nc.width(8);
    nc.fill('0');
    nc << count;
    const auto ha1 = onvif::hashHex(algorithm, "test-user:PTZ-CAMERA:test-pass");
    const auto ha2 = onvif::hashHex(algorithm, "POST:" + path);
    const auto response =
        onvif::hashHex(algorithm, ha1 + ":" + nonce + ":" + nc.str() + ":testnonce:auth:" + ha2);
    return "Digest username=\"test-user\", realm=\"PTZ-CAMERA\", nonce=\"" + nonce + "\", uri=\"" +
           path + "\", algorithm=" + algorithm + ", qop=auth, nc=" + nc.str() +
           ", cnonce=\"testnonce\", response=\"" + response + "\"";
}
} // namespace
/**
 * 테스트의 입력/실행/검증/정리를 순서대로 수행한다. assert 실패는 해당 계약이 깨졌다는 뜻이며 실물 성능 인증이 아니다.
 */
int main(int argc, char **argv) {
    assert(argc == 2);
    // OpenSSL 결과를 알려진 MD5/SHA-256 값과 확인한다.
    assert(onvif::hashHex("MD5", "abc") == "900150983cd24fb0d6963f7d28e17f72");
    assert(onvif::hashHex("SHA-256", "abc") ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    onvif::DigestAuth auth({"test-user", "test-pass"});
    for (const auto *algorithm : {"MD5", "SHA-256"}) {
        auto value = header(auth, "/onvif/media_service", algorithm, 1);
        assert(auth.verify(value, "POST", "/onvif/media_service"));
        assert(!auth.verify(value, "POST", "/onvif/media_service")); // replay
        auto second = header(auth, "/onvif/media_service", algorithm, 2);
        const auto nonceStart = second.find("nonce=\"") + 7;
        const auto shared = second.substr(nonceStart, second.find('"', nonceStart) - nonceStart);
        auto first = header(auth, "/onvif/media_service", algorithm, 1, shared);
        assert(auth.verify(second, "POST", "/onvif/media_service"));
        assert(auth.verify(first, "POST", "/onvif/media_service"));
        assert(!auth.verify(first, "POST", "/onvif/media_service"));
        auto wrongUri = header(auth, "/onvif/device_service", algorithm, 1);
        assert(!auth.verify(wrongUri, "POST", "/onvif/media_service"));
        auto bad = header(auth, "/onvif/media_service", algorithm, 1);
        bad[bad.size() - 2] = bad[bad.size() - 2] == '0' ? '1' : '0';
        assert(!auth.verify(bad, "POST", "/onvif/media_service"));
    }
    int reservation = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(reservation, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    getsockname(reservation, reinterpret_cast<sockaddr *>(&address), &length);
    const auto port = ntohs(address.sin_port);
    close(reservation);
    char path[] = "/tmp/ptz-auth-test-XXXXXX";
    int file = mkstemp(path);
    assert(file >= 0);
    const char content[] = "username=test-user\npassword=test-pass\n";
    assert(write(file, content, sizeof(content) - 1) == sizeof(content) - 1);
    close(file);
    Child child;
    child.path = path;
    child.pid = fork();
    assert(child.pid >= 0);
    const auto number = std::to_string(port);
    if (child.pid == 0) {
        execl(argv[1], argv[1], "--port", number.c_str(), "--bind", "127.0.0.1", "--advertise-ip",
              "127.0.0.1", "--no-discovery", "--credentials-file", path, nullptr);
        _exit(127);
    }
    for (int i = 0; i < 100; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        const bool ready =
            connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
        close(fd);
        if (ready)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const std::string url = "http://127.0.0.1:" + number + "/onvif/media_service";
    assert(call(url) == 401);
    assert(call(url, "wrong-pass") == 401);
    assert(call(url, "test-pass") == 200);
    curl_global_cleanup();
    std::cout << "PASS missing/wrong/correct credentials, Digest interoperability, replay and URI "
                 "binding\n";
}
