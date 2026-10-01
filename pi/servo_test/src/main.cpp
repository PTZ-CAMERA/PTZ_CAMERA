#include <pigpio.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

int angleToPulseWidth(int angle);

namespace {

// BCM 번호 기준. 서보 전원은 외부 4.8~6V를 쓰고 Pi와 전원 GND를 공통 연결한다.
constexpr unsigned PAN_GPIO = 18;
constexpr unsigned TILT_GPIO = 19;
constexpr unsigned CENTER_BUTTON_GPIO = 17;
// 기구가 끝에 부딪히지 않도록 두 축의 이동 범위를 따로 제한한다.
constexpr int PAN_MIN_ANGLE = 25;
constexpr int PAN_MAX_ANGLE = 155;
constexpr int TILT_MIN_ANGLE = 40;
constexpr int TILT_MAX_ANGLE = 140;
constexpr int STEP_DEGREES = 5;
constexpr uint16_t CONTROL_PORT = 5000;
constexpr std::size_t MAX_CLIENTS = 8;
constexpr std::size_t MAX_COMMAND_LENGTH = 64;
constexpr unsigned BUTTON_GLITCH_US = 20000;   // 입력이 20ms 동안 안정적일 때만 전달.
constexpr uint32_t BUTTON_DEBOUNCE_US = 200000; // 버튼을 다시 누르기까지 200ms 무시.

// 키 입력과 pigpio 버튼 콜백은 서로 다른 실행 흐름이므로 각도와 GPIO 쓰기를 보호한다.
std::mutex controlMutex;
int panAngle = 90;
int tiltAngle = 90;
bool buttonSeen = false;
uint32_t lastButtonTick = 0;
std::atomic<bool> shuttingDown{false};
volatile std::sig_atomic_t stopRequested = 0;

void onSignal(int) {
    // 시그널 핸들러에서는 안전한 플래그 변경만 하고 정리는 메인 루프가 수행한다.
    stopRequested = 1;
}

struct TcpClient {
    int fd;
    std::string buffer;
};

int openControlServer() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    const int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(CONTROL_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(fd, static_cast<int>(MAX_CLIENTS)) < 0) {
        const int savedError = errno;
        close(fd);
        errno = savedError;
        return -1;
    }
    return fd;
}

void reply(int fd, const std::string& message) {
    // Qt가 응답을 읽지 않아도 제어 루프가 막히지 않도록 송신은 최선 노력으로 처리한다.
    send(fd, message.data(), message.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
}

void printAnglesLocked() {
    std::cout << "PAN: " << panAngle << " deg\n"
              << "TILT: " << tiltAngle << " deg\n" << std::flush;
}

bool writeServoLocked(unsigned gpio, int angle, int& currentAngle) {
    // gpioServo()의 값은 주기가 약 20ms인 서보 펄스의 HIGH 폭(µs)이다.
    const int pulse = angleToPulseWidth(angle);
    const int result = gpioServo(gpio, pulse);
    if (result < 0) {
        std::cerr << "gpioServo(GPIO" << gpio << ") failed: " << result << '\n';
        return false;
    }
    currentAngle = angle;
    return true;
}

} // namespace

int angleToPulseWidth(int angle) {
    // 0°=1000µs, 90°=1500µs, 180°=2000µs로 선형 변환한다.
    angle = std::clamp(angle, 0, 180);
    return 1000 + angle * 1000 / 180;
}

void setPanAngle(int angle) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (shuttingDown.load()) return;
    // 펄스 변환보다 먼저 실제 장착 상태에 맞춘 PAN 제한을 적용한다.
    angle = std::clamp(angle, PAN_MIN_ANGLE, PAN_MAX_ANGLE);
    writeServoLocked(PAN_GPIO, angle, panAngle);
    printAnglesLocked();
}

void setTiltAngle(int angle) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (shuttingDown.load()) return;
    angle = std::clamp(angle, TILT_MIN_ANGLE, TILT_MAX_ANGLE);
    writeServoLocked(TILT_GPIO, angle, tiltAngle);
    printAnglesLocked();
}

void centerServos() {
    // 두 축을 같은 잠금 아래 갱신해 버튼·키 입력이 중간에 섞이지 않게 한다.
    std::lock_guard<std::mutex> lock(controlMutex);
    if (shuttingDown.load()) return;
    writeServoLocked(PAN_GPIO, 90, panAngle);
    writeServoLocked(TILT_GPIO, 90, tiltAngle);
    printAnglesLocked();
}

void handleCenterButton(int gpio, int level, uint32_t tick, void*) {
    // 풀업 입력: 평소 HIGH, GPIO17과 GND가 연결되면 LOW(눌림).
    if (gpio != static_cast<int>(CENTER_BUTTON_GPIO) || level != PI_LOW ||
        shuttingDown.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(controlMutex);
        // 부호 없는 뺄셈은 pigpio의 32비트 tick이 순환해도 간격을 계산한다.
        if (buttonSeen && uint32_t(tick - lastButtonTick) < BUTTON_DEBOUNCE_US) {
            return;
        }
        buttonSeen = true;
        lastButtonTick = tick;
    }
    centerServos();
}

namespace {

int currentPanAngle() {
    std::lock_guard<std::mutex> lock(controlMutex);
    return panAngle;
}

int currentTiltAngle() {
    std::lock_guard<std::mutex> lock(controlMutex);
    return tiltAngle;
}

void handleTcpCommand(int fd, std::string command) {
    // Qt NetworkClient::sendLine()은 명령 뒤에 '\n'을 붙인다.
    if (!command.empty() && command.back() == '\r') command.pop_back();
    if (command.empty()) return;

    if (command == "PTZ:LEFT") setPanAngle(currentPanAngle() - STEP_DEGREES);
    else if (command == "PTZ:RIGHT") setPanAngle(currentPanAngle() + STEP_DEGREES);
    else if (command == "PTZ:UP") setTiltAngle(currentTiltAngle() + STEP_DEGREES);
    else if (command == "PTZ:DOWN") setTiltAngle(currentTiltAngle() - STEP_DEGREES);
    else if (command == "PTZ:CENTER") centerServos();
    else if (command == "TRACK:ON" || command == "TRACK:OFF") {
        std::cerr << "Tracking is not implemented: " << command << '\n';
        reply(fd, "ERR tracking unavailable\n");
        return;
    } else {
        std::cerr << "Invalid TCP command: " << command << '\n';
        reply(fd, "ERR unknown command\n");
        return;
    }
    // 현재 Qt 파서는 PTZ 응답을 지원하지 않으므로 정상 명령에는 응답하지 않는다.
    // 처리 결과는 Pi의 각도 로그에서 확인할 수 있다.
}

bool receiveClient(TcpClient& client) {
    char input[128];
    const ssize_t count = recv(client.fd, input, sizeof(input), 0);
    if (count <= 0) {
        if (count == 0 && !client.buffer.empty())
            handleTcpCommand(client.fd, client.buffer);
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
        handleTcpCommand(client.fd, command);
    }

    if (client.buffer.size() > MAX_COMMAND_LENGTH) {
        reply(client.fd, "ERR command too long\n");
        return false;
    }
    return true;
}

void cleanupGpio() {
    // 먼저 콜백의 새 동작을 막고 서보 펄스를 끈 뒤 GPIO 설정을 해제한다.
    shuttingDown.store(true);
    {
        std::lock_guard<std::mutex> lock(controlMutex);
        gpioServo(PAN_GPIO, 0);  // 0은 서보 펄스 출력 중지.
        gpioServo(TILT_GPIO, 0);
    }
    gpioSetAlertFuncEx(CENTER_BUTTON_GPIO, nullptr, nullptr);
    gpioGlitchFilter(CENTER_BUTTON_GPIO, 0);
    gpioSetPullUpDown(CENTER_BUTTON_GPIO, PI_PUD_OFF);
    gpioSetMode(PAN_GPIO, PI_INPUT);
    gpioSetMode(TILT_GPIO, PI_INPUT);
    gpioTerminate();
}

bool configureGpio() {
    // 버튼에는 내부 풀업과 pigpio 글리치 필터를 적용하고 콜백을 등록한다.
    return gpioSetMode(PAN_GPIO, PI_OUTPUT) >= 0 &&
           gpioSetMode(TILT_GPIO, PI_OUTPUT) >= 0 &&
           gpioSetMode(CENTER_BUTTON_GPIO, PI_INPUT) >= 0 &&
           gpioSetPullUpDown(CENTER_BUTTON_GPIO, PI_PUD_UP) >= 0 &&
           gpioGlitchFilter(CENTER_BUTTON_GPIO, BUTTON_GLITCH_US) >= 0 &&
           gpioSetAlertFuncEx(CENTER_BUTTON_GPIO, handleCenterButton, nullptr) >= 0;
}

} // namespace

int main(int argc, char* argv[]) {
    const bool tcpMode = argc == 2 && std::string(argv[1]) == "--server";
    if (argc != 1 && !tcpMode) {
        std::cerr << "Usage: " << argv[0] << " [--server]\n";
        return 1;
    }

    // 서버 모드에서는 터미널 없이도 동작한다. 터미널이 있으면 키 제어도 함께 받는다.
    const bool keyboardEnabled = isatty(STDIN_FILENO);
    if (!tcpMode && !keyboardEnabled) {
        std::cerr << "A terminal is required for keyboard control.\n";
        return 1;
    }
    const int serverFd = tcpMode ? openControlServer() : -1;
    if (tcpMode && serverFd < 0) {
        std::cerr << "Cannot listen on TCP port " << CONTROL_PORT << ": "
                  << std::strerror(errno) << '\n';
        return 1;
    }

    if (gpioInitialise() < 0) {
        std::cerr << "pigpio initialization failed (try sudo).\n";
        if (serverFd >= 0) close(serverFd);
        return 1;
    }
    if (!configureGpio()) {
        std::cerr << "GPIO setup failed.\n";
        cleanupGpio();
        if (serverFd >= 0) close(serverFd);
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
    if (keyboardEnabled) {
        if (tcgetattr(STDIN_FILENO, &savedTerminal) != 0) {
            std::cerr << "Cannot read terminal settings.\n";
            cleanupGpio();
            if (serverFd >= 0) close(serverFd);
            return 1;
        }
        termios keyTerminal = savedTerminal;
        keyTerminal.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        keyTerminal.c_cc[VMIN] = 0;
        keyTerminal.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &keyTerminal) != 0) {
            std::cerr << "Could not enable single-key input.\n";
            cleanupGpio();
            if (serverFd >= 0) close(serverFd);
            return 1;
        }
    }

    if (keyboardEnabled)
        std::cout << "a/d: PAN  w/s: TILT  c: center  q: quit\n" << std::flush;
    if (tcpMode)
        std::cout << "PTZ TCP server listening on port " << CONTROL_PORT << '\n' << std::flush;

    std::vector<TcpClient> clients;
    while (!stopRequested) {
        // 키보드·새 TCP 연결·기존 클라이언트를 한 루프에서 처리한다.
        fd_set readFds;
        FD_ZERO(&readFds);
        int maxFd = -1;
        if (keyboardEnabled) {
            FD_SET(STDIN_FILENO, &readFds);
            maxFd = STDIN_FILENO;
        }
        if (serverFd >= 0) {
            FD_SET(serverFd, &readFds);
            maxFd = std::max(maxFd, serverFd);
        }
        for (const auto& client : clients) {
            FD_SET(client.fd, &readFds);
            maxFd = std::max(maxFd, client.fd);
        }
        timeval timeout{0, 100000}; // 최대 100ms 대기.
        const int ready = select(maxFd + 1, &readFds, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            std::cerr << "select failed: " << std::strerror(errno) << '\n';
            break;
        }
        if (ready == 0) continue;

        if (serverFd >= 0 && FD_ISSET(serverFd, &readFds)) {
            const int fd = accept(serverFd, nullptr, nullptr);
            if (fd >= FD_SETSIZE || clients.size() >= MAX_CLIENTS) {
                if (fd >= 0) close(fd);
            } else if (fd >= 0) {
                clients.push_back({fd, {}});
            }
        }
        for (auto it = clients.begin(); it != clients.end();) {
            if (FD_ISSET(it->fd, &readFds) && !receiveClient(*it)) {
                close(it->fd);
                it = clients.erase(it);
            } else {
                ++it;
            }
        }
        if (keyboardEnabled && FD_ISSET(STDIN_FILENO, &readFds)) {
            char key = 0;
            if (read(STDIN_FILENO, &key, 1) != 1) continue;
            switch (key) {
                case 'a': setPanAngle(currentPanAngle() - STEP_DEGREES); break;
                case 'd': setPanAngle(currentPanAngle() + STEP_DEGREES); break;
                case 'w': setTiltAngle(currentTiltAngle() + STEP_DEGREES); break;
                case 's': setTiltAngle(currentTiltAngle() - STEP_DEGREES); break;
                case 'c': centerServos(); break;
                case 'q': stopRequested = 1; break;
                default: break;
            }
        }
    }

    for (const auto& client : clients) close(client.fd);
    if (serverFd >= 0) close(serverFd);
    cleanupGpio();
    // 종료할 때 터미널 입력 설정을 원래대로 돌린다.
    if (keyboardEnabled) tcsetattr(STDIN_FILENO, TCSANOW, &savedTerminal);
    std::cout << "Servo pulses stopped.\n";
    return 0;
}
