/**
 * [학습 안내] 가짜 pigpio로 실제 제어 코드와 로그 연결 검증
 * 실제 키/버튼/명령 처리기를 사용하고 gpioServo만 메모리 출력으로 대체한다.
 * 클램프, 궤적/만료/Stop, 자동 추적 위임, 외부 IPC 거부, 종료와 부모 이벤트 ID를 확인한다.
 * 실물 PWM 파형이나 기계적 움직임은 검증하지 않는다. 이 파일의 pigpio 정의는 테스트에만 연결한다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
// 실제 GPIO 대신 아래 pigpio 함수를 연결한다. 모터/버튼 회로를 건드리지 않는다.
#include "performance/performance_log.h"
#include "servo/servo_control.h"
#include "button/center_button.h"
#include "protocol/command_protocol.h"
#include "keyboard/keyboard_control.h"
#include <pigpio.h>
#include <array>
#include "metadata/detection_metadata.h"
#include <cassert>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace {
std::array<unsigned, 54> pulses{};
bool terminated = false;
} // namespace
/**
 * 테스트 전용 pigpio 대체 함수다. 실제 /dev/mem/GPIO 초기화를 하지 않는다.
 */
int gpioInitialise() {
    return 0;
}
/**
 * 테스트의 종료 호출 여부를 기록한다. 실제 pigpio 프로세스를 종료하지 않는다.
 */
void gpioTerminate() {
    terminated = true;
}
/**
 * 테스트에서는 설정 성공만 흉내 낸다. 실제 핀 모드를 바꾸지 않는다.
 */
int gpioSetMode(unsigned, unsigned) {
    return 0;
}
/**
 * 테스트에서는 반환 계약만 대체한다. 실제 pull-up 저항 검증은 아니다.
 */
int gpioSetPullUpDown(unsigned, unsigned) {
    return 0;
}
/**
 * 테스트용 설정 대체다. 실제 하드웨어 필터 파형을 검증하지 않는다.
 */
int gpioGlitchFilter(unsigned, unsigned) {
    return 0;
}
/**
 * 테스트에서는 callback 등록 API를 대체하고 callback 함수 자체를 직접 호출해 동작을 확인한다.
 */
int gpioSetAlertFuncEx(unsigned, gpioAlertFuncEx_t, void *) {
    return 0;
}
/**
 * 가짜 GPIO 배열에 요청 펄스를 저장해 이후 assert로 확인한다. 실물 출력 없이 제어 코드를 검증한다.
 */
int gpioServo(unsigned gpio, unsigned pulse) {
    pulses.at(gpio) = pulse;
    return 0;
}

/**
 * 테스트의 입력/실행/검증/정리를 순서대로 수행한다. assert 실패는 해당 계약이 깨졌다는 뜻이며 실물 성능 인증이 아니다.
 */
int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    setenv("PTZ_PERF_LOG", argv[1], 1);
    using namespace ptz;
    assert(initializeGpio());
    centerServos();
    assert(pulses[18] == 1500 && pulses[19] == 1500);
    assert(angleToPulseWidth(-1) == 1000 && angleToPulseWidth(181) == 2000);
    setPanAngle(999);
    setTiltAngle(-999);
    assert(currentPanAngle() == 155 && currentTiltAngle() == 40);

    int connection[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, connection) == 0);
    std::vector<TcpClient> clients{{connection[0], {}, "127.0.0.1:10000", false}};
    auto &camera = clients.front();
    handleTcpCommand(camera, clients, "PTZ:CENTER");
    handleTcpCommand(camera, clients, "CAMERA:HELLO");
    handleTcpCommand(camera, clients, "TRACK:ON");
    handleTcpCommand(camera, clients, "AUTO:MOVE:1,-1");
    assert(currentPanAngle() == 91 && currentTiltAngle() == 89);
    handleTcpCommand(camera, clients, "AUTO:MOVE:4,0");
    assert(currentPanAngle() == 91 && currentTiltAngle() == 89);
    handleTcpCommand(camera, clients, "PTZ:RIGHT");
    assert(currentPanAngle() == 96 && !isTrackingEnabled());

    // 실제 내부 parser까지 통과시켜 v2 세대 검증과 WATCH metadata 전달을 확인한다.
    handleTcpCommand(camera,clients,"TRACK:ON");
    const auto oldGeneration=trackingGeneration();
    handleTcpCommand(camera,clients,"TRACK:OFF");
    handleTcpCommand(camera,clients,"TRACK:ON");
    const auto generation=trackingGeneration();
    const auto before=currentAngles();
    handleTcpCommand(camera,clients,"AUTO:MOVE2:1,-1,"+std::to_string(oldGeneration));
    assert(currentAngles()==before);
    handleTcpCommand(camera,clients,"AUTO:MOVE2:1,-1,"+std::to_string(generation));
    assert(currentPanAngle()==before.first+1 && currentTiltAngle()==before.second-1);
    DetectionMetadata d;
    d.confidence=.8; d.x=640; d.y=360; d.boxX=500; d.boxY=200;
    d.boxWidth=280; d.boxHeight=320; d.frameId=7; d.framePtsNs=123456;
    d.captureUnixMs=1000; d.analysisUnixMs=1300; d.streamEpoch=999; d.generation=generation;
    handleTcpCommand(camera,clients,"AUTO:DETECT2:"+encodeDetection(d));
    handleTcpCommand(camera,clients,"ONVIF:WATCH");
    char messages[4096]{};
    const auto count=recv(connection[1],messages,sizeof(messages),MSG_DONTWAIT);
    assert(count>0 && std::string(messages,count).find("DETECT2:")!=std::string::npos);
    camera.observer=false;

    setTrackingEnabled(true);
    handleCenterButton(17, 0, 1000000, nullptr);
    assert(currentPanAngle() == 90 && currentTiltAngle() == 90 && !isTrackingEnabled());
    setPanAngle(100);
    setTrackingEnabled(true);
    handleCenterButton(17, 0, 1040000, nullptr);
    assert(currentPanAngle() == 100 && isTrackingEnabled());
    handleCenterButton(17, 0, 1300000, nullptr);
    assert(currentPanAngle() == 90 && !isTrackingEnabled());
    volatile std::sig_atomic_t stop = 0;
    handleLocalKey('w', stop);
    assert(currentTiltAngle() == 95);
    handleLocalKey('q', stop);
    assert(stop == 1);
    // 실제 핸들러/컨트롤러를 사용하되 gpioServo는 위의 가짜 출력이다.
    handleTcpCommand(camera, clients, "ONVIF:ABS:100,90");
    assert(servoMotionStatus().moving && !isTrackingEnabled());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (servoMotionStatus().moving && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        updateServoMotion();
    }
    assert(currentPanAngle() == 100 && currentTiltAngle() == 90 && !servoMotionStatus().moving);
    handleTcpCommand(camera, clients, "ONVIF:VEL:30,0,100");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    updateServoMotion();
    assert(!servoMotionStatus().moving); // 만료된 명령으로 추가 이동하지 않는다.
    handleTcpCommand(camera, clients, "ONVIF:VEL:30,0,1000");
    handleTcpCommand(camera, clients, "ONVIF:STOP");
    const auto held = currentAngles();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    updateServoMotion();
    assert(currentAngles() == held && !servoMotionStatus().moving);
    handleTcpCommand(camera, clients, "ONVIF:ABS:155,140");
    setTrackingEnabled(true);
    assert(!servoMotionStatus().moving);
    setTrackingEnabled(false);
    handleTcpCommand(camera, clients, "ONVIF:TRACK");
    assert(isTrackingEnabled());
    handleTcpCommand(camera, clients, "ONVIF:STOP");
    assert(!isTrackingEnabled());
    handleTcpCommand(camera, clients, "ONVIF:MOVE_TRACK:100,90");
    assert(isTrackingEnabled()); // 목표가 현재 위치면 바로 위임.
    stopServoMotion();
    camera.peer = "192.168.0.50:10000";
    handleTcpCommand(camera, clients, "PTZ:LEFT");
    handleTcpCommand(camera, clients, "ONVIF:ABS:25,40");
    assert(currentAngles() == held && !servoMotionStatus().moving);
    camera.peer = "127.0.0.1:10000";
    cleanupGpio();
    assert(pulses[18] == 0 && pulses[19] == 0 && terminated);
    close(connection[0]);
    close(connection[1]);
    perf::CsvLog::instance().flush();

    std::ifstream file(argv[1]);
    std::string line;
    std::getline(file, line);
    assert(line.rfind("start_ns,end_ns,pid,tid,event_id,parent_id,event", 0) == 0);
    std::set<unsigned long long> roots;
    std::vector<unsigned long long> gpioParents;
    bool ignored = false;
    while (std::getline(file, line)) {
        std::istringstream row(line);
        std::vector<std::string> fields;
        std::string field;
        for (int i = 0; i < 10 && std::getline(row, field, ','); ++i)
            fields.push_back(field);
        assert(fields.size() == 10);
        assert(std::stoll(fields[1]) >= std::stoll(fields[0]));
        assert(std::stoi(fields[2]) == getpid());
        if (fields[6] == "tcp_command" || fields[6] == "button_callback" ||
            fields[6] == "keyboard_command" || fields[6] == "servo_motion_tick")
            roots.insert(std::stoull(fields[4]));
        if (fields[6] == "gpio_write" && fields[5] != "0")
            gpioParents.push_back(std::stoull(fields[5]));
        if (fields[6] == "button_debounce_ignored")
            ignored = true;
    }
    assert(!gpioParents.empty() && ignored);
    for (auto parent : gpioParents)
        assert(roots.count(parent) == 1);
    return 0;
}
