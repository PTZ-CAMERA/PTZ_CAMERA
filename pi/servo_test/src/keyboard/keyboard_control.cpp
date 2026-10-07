/**
 * [학습 안내] 터미널 키 입력과 로컬 제어 클라이언트
 * termios로 Enter 없는 입력을 만들고 a/d/w/s/c/q를 제어 동작에 매핑한다.
 * 서비스가 켜져 있으면 로컬 TCP로 전송하고 직접 모드는 servo_control을 호출한다.
 * 원래 터미널 설정과 소켓을 종료 경로에서 복원한다. 클라이언트 종료가 전체 GPIO 서비스 종료를 의미하지 않는다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "performance/performance_log.h"
#include "keyboard/keyboard_control.h"
#include "servo/servo_control.h"
#include "config.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ptz {
/**
 * 원래 termios를 저장하고 canonical/echo를 꺼 단일 키 입력을 만든다. 저장값은 종료 때 반드시 복원한다.
 */
bool enableKeyboard(termios &savedTerminal) {
    if (tcgetattr(STDIN_FILENO, &savedTerminal) != 0) {
        std::cerr << "Cannot read terminal settings.\n";
        return false;
    }
    // Enter 없이 한 글자씩 읽고 종료 시 저장한 설정을 복원한다.
    termios keyTerminal = savedTerminal;
    keyTerminal.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    keyTerminal.c_cc[VMIN] = 0;
    keyTerminal.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &keyTerminal) != 0) {
        std::cerr << "Could not enable single-key input.\n";
        return false;
    }
    return true;
}

/**
 * 시작 전에 저장한 termios로 복원한다. 정상 종료/오류/신호 경로에서 호출한다.
 */
void restoreKeyboard(const termios &savedTerminal) {
    tcsetattr(STDIN_FILENO, TCSANOW, &savedTerminal);
}

/**
 * 방향/중앙 키를 로컬 명령 문자열로 매핑한다. 알 수 없는 키는 nullptr로 무시한다.
 */
const char *commandForKey(char key) {
    switch (key) {
    case 'a':
        return "PTZ:LEFT\n";
    case 'd':
        return "PTZ:RIGHT\n";
    case 'w':
        return "PTZ:UP\n";
    case 's':
        return "PTZ:DOWN\n";
    case 'c':
        return "PTZ:CENTER\n";
    default:
        return nullptr;
    }
}

/**
 * 직접 GPIO 모드의 키를 처리한다. 수동 제어는 추적을 해제하고 q는 메인 루프 종료 플래그만 바꾼다.
 */
void handleLocalKey(char key, volatile std::sig_atomic_t &stopRequested) {
    perf::Scope keyTiming("keyboard_command", std::string(1, key));
    if (commandForKey(key))
        setTrackingEnabled(false);
    switch (key) {
    case 'a':
        setPanAngle(currentPanAngle() - STEP_DEGREES);
        break;
    case 'd':
        setPanAngle(currentPanAngle() + STEP_DEGREES);
        break;
    case 'w':
        setTiltAngle(currentTiltAngle() + STEP_DEGREES);
        break;
    case 's':
        setTiltAngle(currentTiltAngle() - STEP_DEGREES);
        break;
    case 'c':
        centerServos();
        break;
    case 'q':
        stopRequested = 1;
        break;
    default:
        break;
    }
}

/**
 * 서비스 소켓과 stdin을 select로 함께 감시한다. 입력 모드 복원과 소켓 close를 공통 종료 경로에서 수행한다.
 */
int runKeyboardClient(int fd, volatile std::sig_atomic_t &stopRequested) {
    if (!isatty(STDIN_FILENO)) {
        std::cerr << "A terminal is required for keyboard control.\n";
        close(fd);
        return 1;
    }
    termios savedTerminal{};
    if (!enableKeyboard(savedTerminal)) {
        close(fd);
        return 1;
    }

    std::cout << "Connected to local PTZ service. a/d: PAN  w/s: TILT  c: center  q: quit\n"
              << std::flush;
    int result = 0;
    while (!stopRequested) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(STDIN_FILENO, &readFds);
        FD_SET(fd, &readFds);
        timeval timeout{0, 100000};
        const int ready = select(fd + 1, &readFds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            result = 1;
            break;
        }
        if (ready == 0)
            continue;
        if (FD_ISSET(fd, &readFds)) {
            char response[128];
            const ssize_t count = recv(fd, response, sizeof(response), 0);
            if (count <= 0) {
                std::cerr << "PTZ service disconnected.\n";
                result = 1;
                break;
            }
            std::cerr.write(response, count);
        }
        if (FD_ISSET(STDIN_FILENO, &readFds)) {
            char key = 0;
            if (read(STDIN_FILENO, &key, 1) != 1)
                continue;
            const char *command = commandForKey(key);
            if (key == 'q')
                stopRequested = 1;
            if (command && send(fd, command, std::strlen(command), MSG_NOSIGNAL) < 0) {
                std::cerr << "Could not send PTZ command.\n";
                result = 1;
                break;
            }
        }
    }
    restoreKeyboard(savedTerminal);
    close(fd);
    return result;
}

} // namespace ptz
