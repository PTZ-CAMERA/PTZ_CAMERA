/**
 * [학습 안내] Digest+HTTP long-poll+내부 feed의 통합 검증
 * 별도 가짜 backend와 자식 ONVIF 서버를 띄워 synthetic 알림을 실제 소켓으로 전달한다.
 * PT2S 대기 요청이 새 데이터 도착 직후 응답하는지 측정한다.
 * 출력 지연은 로컬 모의 publish→응답 시간이며 AI 추론/Qt 영상/실물 반응 시간이 아니다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "soap_client.h"
#include "protocol/service.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
namespace {
/**
 * 테스트용 임시 loopback 포트를 확보한다. 실제 서비스 포트를 재사용해 운영 프로세스를 종료하지 않는다.
 */
int listener(unsigned &port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size);
    port = ntohs(address.sin_port);
    assert(listen(fd, 8) == 0);
    return fd;
}
/**
 * 테스트 backend의 짧은 알림도 부분 송신을 고려해 끝까지 보낸다.
 */
void sendAll(int fd, const std::string &text) {
    std::size_t at = 0;
    while (at < text.size()) {
        const auto count = send(fd, text.data() + at, text.size() - at, MSG_NOSIGNAL);
        assert(count > 0);
        at += count;
    }
}
/**
 * 통합 테스트의 SOAP Body에서 기대하는 Events response를 꺼낸다.
 */
const onvif::Node &response(const onvif::Node &root, const std::string &name) {
    return onvif::client::required(onvif::client::required(root, onvif::SOAP, "Body"),
                                   onvif::EVENTS, name + "Response");
}
} // namespace
/**
 * 테스트의 입력/실행/검증/정리를 순서대로 수행한다. assert 실패는 해당 계약이 깨졌다는 뜻이며 실물 성능 인증이 아니다.
 */
int main(int argc, char **argv) {
    assert(argc == 2);
    unsigned fakePort, httpPort;
    int fakeListener = listener(fakePort), reservation = listener(httpPort);
    close(reservation);
    char credentialFile[] = "/tmp/ptz-events-test-XXXXXX";
    int file = mkstemp(credentialFile);
    assert(file >= 0);
    const char credential[] = "username=test-user\npassword=test-pass\n";
    assert(write(file, credential, sizeof(credential) - 1) > 0);
    close(file);
    const auto http = std::to_string(httpPort), backend = std::to_string(fakePort);
    const auto pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(fakeListener);
        execl(argv[1], argv[1], "--bind", "127.0.0.1", "--advertise-ip", "127.0.0.1", "--port",
              http.c_str(), "--servo-port", backend.c_str(), "--no-discovery", "--credentials-file",
              credentialFile, nullptr);
        _exit(127);
    }
    const int feed = accept(fakeListener, nullptr, nullptr);
    assert(feed >= 0);
    close(fakeListener);
    char watch[128];
    assert(recv(feed, watch, sizeof(watch), 0) > 0);
    sendAll(feed, "CAMERA:DISCONNECTED\nTRACK:OFF\nNO_DETECTION\n");
    setenv("PTZ_CREDENTIALS_FILE", credentialFile, 1);
    const auto address = "http://127.0.0.1:" + http + "/onvif/events_service";
    using onvif::client::call;
    using onvif::client::required;
    call(address, "<e:GetEventProperties/>");
    auto created = call(address, "<e:CreatePullPointSubscription><e:InitialTerminationTime>PT60S</"
                                 "e:InitialTerminationTime></e:CreatePullPointSubscription>");
    const auto &reference = required(response(*created, "CreatePullPointSubscription"),
                                     onvif::EVENTS, "SubscriptionReference");
    const auto subscription =
        required(reference, "http://www.w3.org/2005/08/addressing", "Address").text;
    const std::string pull = "<e:PullMessages><e:Timeout>PT2S</e:Timeout><e:MessageLimit>16</"
                             "e:MessageLimit></e:PullMessages>";
    call(subscription, pull); // 초기 상태를 먼저 소비한다.
    std::atomic<long long> sentNs{0};
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        sentNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();
        sendAll(feed, "CAMERA:CONNECTED\nTRACK:ON\nDETECT:person,0.9,640,360,0,0,90,90\n");
    });
    const auto start = std::chrono::steady_clock::now();
    auto delivery = call(subscription, pull);
    const auto done = std::chrono::steady_clock::now();
    producer.join();
    const auto &payload = response(*delivery, "PullMessages");
    bool detection = false;
    for (const auto &message : payload.children)
        if (message->ns == onvif::WSNT && message->name == "NotificationMessage") {
            if (required(*message, onvif::WSNT, "Topic").text.find("PersonDetection") !=
                std::string::npos)
                detection = true;
        }
    assert(detection &&
           done - start < std::chrono::seconds(1)); // PT2S 전체를 기다리지 않았는지 확인.
    const auto doneNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(done.time_since_epoch()).count();
    std::cout << "PASS authenticated asynchronous event feed; synthetic publish-to-response="
              << (doneNs - sentNs.load()) / 1e6 << "ms\n";
    call(subscription, "<n:Unsubscribe/>");
    close(feed);
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    unlink(credentialFile);
}
