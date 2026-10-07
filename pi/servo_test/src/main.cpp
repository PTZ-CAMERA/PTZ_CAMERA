/**
 * [학습 안내] GPIO 서비스와 로컬 키보드 모드의 진입점
 * 모드 선택 → GPIO 초기화 → 중앙 출력 → select 입력/20ms 궤적 갱신 → 자원 정리 순서다.
 * main 입력 루프와 pigpio 버튼 콜백은 서로 다른 실행 흐름이다. 각도와 GPIO 갱신은 servo_control의 mutex로 보호한다.
 * GPIO는 이 프로세스가 단독 소유한다. Pi 밖의 클라이언트는 ONVIF를 사용한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "config.h"
#include "keyboard/keyboard_control.h"
#include "servo/servo_control.h"
#include "network/tcp_server.h"
#include "protocol/command_protocol.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/select.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopRequested = 0;
/**
 * sig_atomic_t 플래그만 설정한다. 신호 안에서 mutex나 std::cout/GPIO cleanup을 실행하지 않는다.
 */
void onSignal(int) {
    // 시그널 안에서는 플래그만 변경하고 메인 루프에서 정리한다.
    stopRequested = 1;
}
} // namespace

/**
 * 실행 인자와 자원을 준비한 뒤 위 함수들을 호출한다. 정상/오류 종료를 반환값으로 구분하고 보유 자원을 종료 경로에서 정리한다.
 */
int main(int argc, char *argv[]) {
    using namespace ptz;
    const bool tcpMode = argc == 2 && std::string(argv[1]) == "--server";
    const bool clientMode = argc == 2 && std::string(argv[1]) == "--client";
    const bool directMode = argc == 2 && std::string(argv[1]) == "--direct";
    if (argc != 1 && !tcpMode && !clientMode && !directMode) {
        std::cerr << "Usage: " << argv[0] << " [--server|--client|--direct]\n";
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // 기본 실행은 이미 켜진 서비스에 붙는다. 서비스가 없을 때만 직접 GPIO를 연다.
    if (clientMode || (argc == 1 && isatty(STDIN_FILENO))) {
        const int clientFd = connectLocalServer();
        if (clientFd >= 0)
            return runKeyboardClient(clientFd, stopRequested);
        if (clientMode) {
            std::cerr << "PTZ service is not listening on 127.0.0.1:" << CONTROL_PORT << ".\n";
            return 1;
        }
    }

    // 서버 모드에서는 터미널 없이도 동작한다. 터미널이 있으면 키 제어도 함께 받는다.
    const bool keyboardEnabled = isatty(STDIN_FILENO);
    if (!tcpMode && !keyboardEnabled) {
        std::cerr << "A terminal is required for keyboard control.\n";
        return 1;
    }
    TcpServer server;
    server.fd = tcpMode ? openControlServer() : -1;
    if (tcpMode && server.fd < 0) {
        std::cerr << "Cannot listen on TCP port " << CONTROL_PORT << ": " << std::strerror(errno)
                  << '\n';
        return 1;
    }

    if (!initializeGpio()) {
        closeControlServer(server);
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    // systemd는 서비스 종료 시 SIGCONT도 보낸다. pigpio의 기본 핸들러가 이를
    // 오류로 종료시키지 않도록 무시하고 SIGTERM 경로에서 GPIO를 정리한다.
    std::signal(SIGCONT, SIG_IGN);
    centerServos();

    // 터미널의 줄 입력·에코를 끄면 Enter 없이 a/d/w/s/c/q를 받을 수 있다.
    termios savedTerminal{};
    if (keyboardEnabled && !enableKeyboard(savedTerminal)) {
        cleanupGpio();
        closeControlServer(server);
        return 1;
    }

    if (keyboardEnabled)
        std::cout << "a/d: PAN  w/s: TILT  c: center  q: quit\n" << std::flush;
    if (tcpMode)
        std::cout << "PTZ TCP server listening on port " << CONTROL_PORT << '\n' << std::flush;

    while (!stopRequested) {
        updateServoMotion();
        publishTrackingChanges(server.clients);
        // 키보드·새 TCP 연결·기존 클라이언트를 한 루프에서 처리한다.
        fd_set readFds;
        FD_ZERO(&readFds);
        int maxFd = -1;
        if (keyboardEnabled) {
            FD_SET(STDIN_FILENO, &readFds);
            maxFd = STDIN_FILENO;
        }
        addServerReadFds(server, readFds, maxFd);
        timeval timeout{0, 20000}; // 궤적 목표를 약 20ms마다 갱신한다.
        const int ready = select(maxFd + 1, &readFds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            std::cerr << "select failed: " << std::strerror(errno) << '\n';
            break;
        }
        if (ready == 0)
            continue;

        processServerReadFds(server, readFds);
        if (keyboardEnabled && FD_ISSET(STDIN_FILENO, &readFds)) {
            char key = 0;
            if (read(STDIN_FILENO, &key, 1) != 1)
                continue;
            handleLocalKey(key, stopRequested);
        }
    }

    closeControlServer(server);
    cleanupGpio();
    // 종료할 때 터미널 입력 설정을 원래대로 돌린다.
    if (keyboardEnabled)
        restoreKeyboard(savedTerminal);
    std::cout << "Servo pulses stopped.\n";
    return 0;
}
