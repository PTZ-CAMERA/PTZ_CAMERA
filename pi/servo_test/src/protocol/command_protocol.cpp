/**
 * [학습 안내] Pi 내부 명령의 검증과 제어 연결
 * ONVIF 게이트웨이 RPC, 카메라 등록/자동 이동, 로컬 키 명령과 내부 관측 알림을 처리한다.
 * 명령은 main 입력 루프에서 순서대로 처리하고 GPIO 공유 상태는 servo_control이 보호한다.
 * 외부 사용자 프로토콜은 ONVIF다. 이 줄 기반 규약은 Pi 내부 IPC 용도로만 남긴다.
 * 사용 API의 선언은 include된 헤더에서, 전체 데이터 흐름은 docs/architecture.md에서 확인한다.
 */
#include "performance/performance_log.h"
#include "protocol/command_protocol.h"
#include "servo/servo_control.h"
#include "config.h"
#include "metadata/detection_metadata.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string_view>

namespace ptz {
namespace {
int cameraPublishers = 0;
std::string latestDetection = "NO_DETECTION\n";
std::string latestStreamState="CAMERA:STREAM:STOPPED\n";
std::string latestCameraState="CAMERA:STATE:STOPPED\n",latestTargetState="TARGET:NO_TARGET\n";

/**
 * 자동 추적의 두 정수 보정값을 끝까지 파싱하고 ±3도 범위를 확인한다. 뒤의 쓰레기 문자열도 허용하지 않는다.
 */
std::optional<std::pair<int, int>> parseMove(std::string_view input) {
    const auto separator = input.find(',');
    if (separator == std::string_view::npos)
        return std::nullopt;
    int pan = 0;
    int tilt = 0;
    const auto p = std::from_chars(input.data(), input.data() + separator, pan);
    const auto t = std::from_chars(input.data() + separator + 1, input.data() + input.size(), tilt);
    if (p.ec != std::errc{} || p.ptr != input.data() + separator || t.ec != std::errc{} ||
        t.ptr != input.data() + input.size() || pan < -3 || pan > 3 || tilt < -3 || tilt > 3)
        return std::nullopt;
    return std::pair{pan, tilt};
}

} // namespace

/**
 * 줄 단위 내부 명령을 검증해 제어/등록/관측 동작으로 전달한다. HTTP 인증은 게이트웨이가, GPIO/상태 잠금은 servo_control이 담당한다.
 */
void handleTcpCommand(TcpClient &source, std::vector<TcpClient> &clients, std::string command) {
    // Qt NetworkClient::sendLine()은 명령 뒤에 '\n'을 붙인다.
    if (!command.empty() && command.back() == '\r')
        command.pop_back();
    if (command.empty())
        return;
    perf::Scope commandTiming("tcp_command", command);
    const bool local = source.peer.rfind("127.0.0.1:", 0) == 0;
    if (command.rfind("TRACK:", 0) != 0 && command != "CAMERA:HELLO" && command.rfind("AUTO:", 0) != 0)
        std::cout << "TCP command from " << source.peer << ": " << command << std::endl;

    // ONVIF 게이트웨이 전용 내부 RPC. 외부 Qt는 인증된 HTTP PTZ를 사용한다.
    if (command.rfind("ONVIF:", 0) == 0) {
        if (!local) {
            reply(source.fd, "ERR local gateway only\n");
            return;
        }
        const bool wasTracking = isTrackingEnabled();
        bool ok = false;
        double x = 0, y = 0;
        int timeout = 0;
        char extra = 0;
        if (command == "ONVIF:WATCH") {
            source.observer = true;
            reply(source.fd, cameraPublishers > 0 ? "CAMERA:CONNECTED\n" : "CAMERA:DISCONNECTED\n");
            reply(source.fd, isTrackingRequested() ? "TRACK:ON\n" : "TRACK:OFF\n");
            reply(source.fd,runtimeStateLine()+"\n");
            reply(source.fd,latestCameraState);
            reply(source.fd,latestStreamState);
            reply(source.fd,latestTargetState);
            reply(source.fd, latestDetection);
            return;
        } else if (command == "ONVIF:RUNTIME") {
            reply(source.fd,runtimeStateLine()+"\n"); return;
        } else if (command == "ONVIF:TRACK_OFF") {
            setTrackingEnabled(false); ok=true;
        } else if (command == "ONVIF:TRACK") {
            ok = startAutoTracking(false);
        } else if (command.rfind("ONVIF:MOVE_TRACK:", 0) == 0 &&
                   std::sscanf(command.c_str() + std::string("ONVIF:MOVE_TRACK:").size(),
                               "%lf,%lf%c", &x, &y, &extra) == 2) {
            ok = startAutoTracking(true, x, y);
        } else if (command == "ONVIF:STATUS") {
            const auto state = servoMotionStatus();
            reply(source.fd, "STATE:" + std::to_string(state.pan) + "," +
                                 std::to_string(state.tilt) + "," + std::to_string(state.moving) +
                                 "," + std::to_string(state.fault) + "\n");
            return;
        } else if (command == "ONVIF:STOP") {
            stopServoMotion();
            ok = true;
        } else if (command.rfind("ONVIF:ABS:", 0) == 0 &&
                   std::sscanf(command.c_str() + 10, "%lf,%lf%c", &x, &y, &extra) == 2 &&
                   x >= PAN_MIN_ANGLE && x <= PAN_MAX_ANGLE && y >= TILT_MIN_ANGLE &&
                   y <= TILT_MAX_ANGLE) {
            ok = startTargetMove(x, y, false);
        } else if (command.rfind("ONVIF:REL:", 0) == 0 &&
                   std::sscanf(command.c_str() + 10, "%lf,%lf%c", &x, &y, &extra) == 2 &&
                   std::abs(x) <= PAN_MAX_ANGLE - PAN_MIN_ANGLE &&
                   std::abs(y) <= TILT_MAX_ANGLE - TILT_MIN_ANGLE) {
            ok = startTargetMove(x, y, true);
        } else if (command.rfind("ONVIF:VEL:", 0) == 0 &&
                   std::sscanf(command.c_str() + 10, "%lf,%lf,%d%c", &x, &y, &timeout, &extra) ==
                       3) {
            ok = startVelocityMove(x, y, timeout);
        }
        if (ok && wasTracking && !isTrackingEnabled()) {
            latestDetection = "NO_DETECTION\n";
            broadcastQt(clients, latestDetection);
        }
        reply(source.fd, ok ? "OK\n" : "ERR invalid motion\n");
        return;
    }
    if (command == "CAMERA:HELLO" && local) {
        if (!source.publisher) {
            source.publisher = true;
            if (++cameraPublishers == 1) {
                latestCameraState="CAMERA:STATE:STARTING\n";
                broadcastQt(clients, "CAMERA:CONNECTED\n"); broadcastQt(clients,latestCameraState);
            }
        }
        reply(source.fd, "CAMERA:READY\n");
        return;
    }
    if (command.rfind("CAMERA:STREAM:",0)==0 && local && source.publisher) {
        const auto state=command.substr(14);
        if (state=="STREAMING"||state=="STOPPED"||state=="ERROR") {
            latestStreamState=command+"\n"; broadcastQt(clients,latestStreamState);
        }
        return;
    }
    if (command.rfind("CAMERA:STATE:",0)==0 && local && source.publisher) {
        const auto state=command.substr(13);
        if (state=="STARTING"||state=="RUNNING"||state=="STOPPED"||state=="ERROR") {
            latestCameraState=command+"\n"; broadcastQt(clients,latestCameraState);
        }
        return;
    }
    if (command == "TRACK:SNAPSHOT" && local) {
        const auto state = trackingState();
        reply(source.fd, std::string(state.first ? "TRACK:ON:" : "TRACK:OFF:") +
                         std::to_string(state.second) + "\n");
        return;
    }
    if (command == "TRACK:LEASE" && local) {
        // 이동 후 추적 예약도 캡처가 필요하다. keeper는 publisher로 등록하지 않는다.
        reply(source.fd, isTrackingRequested() ? "TRACK:ON\n" : "TRACK:OFF\n");
        return;
    }
    if (command == "TRACK:STATUS" && local) {
        reply(source.fd, isTrackingEnabled() ? "TRACK:ON\n" : "TRACK:OFF\n");
        return;
    }
    if (command == "TRACK:ON" || command == "TRACK:OFF") {
        if (!local) {
            reply(source.fd, "ERR use ONVIF tracking\n");
            return;
        }
        setTrackingEnabled(command == "TRACK:ON");
        if (command == "TRACK:OFF")
            broadcastQt(clients, "NO_DETECTION\n");
        return;
    }
    if (command.rfind("AUTO:", 0) == 0) {
        if (!local || !source.publisher)
            return;
        if (command.rfind("AUTO:TARGET:",0)==0) {
            const auto value=std::string_view(command).substr(12);
            const auto comma=value.find(',');
            if (comma==std::string_view::npos) return;
            const auto state=value.substr(0,comma);
            if (state!="NO_TARGET"&&state!="ACQUIRED"&&state!="TRACKING"&&state!="LOST"&&state!="ERROR") return;
            std::uint64_t generation=0;
            const auto r=std::from_chars(value.data()+comma+1,value.data()+value.size(),generation);
            if (r.ec!=std::errc{}||r.ptr!=value.data()+value.size()||generation!=trackingGeneration()) return;
            latestTargetState="TARGET:"+std::string(state)+"\n";
            broadcastQt(clients,latestTargetState);
            if (state=="LOST"||state=="ERROR") stopAutomaticMotion(generation);
            return;
        }
        if (command.rfind("AUTO:MOVE",0)==0 && !isTrackingEnabled()) return;
        if (command.rfind("AUTO:MOVE2:", 0) == 0) {
            const std::string_view value(command.data()+11, command.size()-11);
            const auto last = value.rfind(',');
            const auto move = last == std::string_view::npos ? std::nullopt : parseMove(value.substr(0,last));
            std::uint64_t generation = 0;
            if (!move || last == std::string_view::npos) { reply(source.fd,"ERR invalid auto move\n"); return; }
            const auto r = std::from_chars(value.data()+last+1,value.data()+value.size(),generation);
            if (r.ec != std::errc{} || r.ptr != value.data()+value.size()) {
                reply(source.fd,"ERR invalid generation\n"); return;
            }
            moveServosAutomatically(move->first,move->second,generation);
            return;
        }
        if (command.rfind("AUTO:DETECT2:", 0) == 0) {
            auto metadata = decodeDetection(std::string_view(command).substr(13));
            const auto state = trackingState();
            if (!metadata) { reply(source.fd,"ERR invalid detection\n"); return; }
            // 정상적인 수동 전환 경쟁으로 폐기한 결과에는 오류 응답을 끼워 넣지 않는다.
            if (metadata->generation != static_cast<std::int64_t>(state.second)) return;
            const auto angles = currentAngles();
            metadata->panAngle = angles.first; metadata->tiltAngle = angles.second;
            latestDetection = "DETECT2:" + encodeDetection(*metadata) + "\n";
            broadcastQt(clients,latestDetection);
            return;
        }
        if (command.rfind("AUTO:MOVE:", 0) == 0) {
            const auto move = parseMove(std::string_view(command).substr(10));
            if (!move) {
                reply(source.fd, "ERR invalid auto move\n");
                return;
            }
            moveServosAutomatically(move->first, move->second);
            return;
        }
        if (command.rfind("AUTO:DETECT:", 0) == 0) {
            if (!isTrackingEnabled()) return;
            float confidence = 0;
            int x = 0, y = 0, ex = 0, ey = 0;
            char extra = 0;
            if (std::sscanf(command.c_str() + 12, "%f,%d,%d,%d,%d%c", &confidence, &x, &y, &ex, &ey,
                            &extra) != 5 ||
                !std::isfinite(confidence) || confidence < 0 || confidence > 1 || x < 0 ||
                x > 1280 || y < 0 || y > 720) {
                reply(source.fd, "ERR invalid detection\n");
                return;
            }
            const auto [pan, tilt] = currentAngles();
            latestDetection = "DETECT:person," + std::to_string(confidence) + "," +
                              std::to_string(x) + "," + std::to_string(y) + "," +
                              std::to_string(ex) + "," + std::to_string(ey) + "," +
                              std::to_string(pan) + "," + std::to_string(tilt) + "\n";
            broadcastQt(clients, latestDetection);
            return;
        }
        if (command == "AUTO:NO_DETECTION") {
            latestDetection = "NO_DETECTION\n";
            broadcastQt(clients, latestDetection);
            return;
        }
    }
    if (command == "PTZ:LEFT" || command == "PTZ:RIGHT" || command == "PTZ:UP" ||
        command == "PTZ:DOWN" || command == "PTZ:CENTER") {
        if (!local) {
            reply(source.fd, "ERR use authenticated ONVIF PTZ\n");
            return;
        }
        // Pi의 로컬 키 클라이언트는 기존 명령을 사용한다.
        // 수동 명령이 자동 제어를 즉시 해제한다.
        if (command == "PTZ:LEFT")
            setPanAngle(currentPanAngle() - STEP_DEGREES);
        else if (command == "PTZ:RIGHT")
            setPanAngle(currentPanAngle() + STEP_DEGREES);
        else if (command == "PTZ:UP")
            setTiltAngle(currentTiltAngle() + STEP_DEGREES);
        else if (command == "PTZ:DOWN")
            setTiltAngle(currentTiltAngle() - STEP_DEGREES);
        else
            centerServos();
    } else {
        std::cerr << "Invalid TCP command: " << command << '\n';
        reply(source.fd, "ERR unknown command\n");
        return;
    }
    // 현재 Qt 파서는 PTZ 응답을 지원하지 않으므로 정상 명령에는 응답하지 않는다.
    // 처리 결과는 Pi의 각도 로그에서 확인할 수 있다.
}

/**
 * 현재 등록된 송출기 연결이 하나 이상인지 반환한다. CSI 센서가 물리적으로 연결됐다는 판정은 아니다.
 */
bool hasCameraPublisher() {
    return cameraPublishers > 0;
}

/**
 * 마지막 publisher가 끊기면 송출 종료와 탐지 없음 상태를 관측자에게 알린다.
 */
void cameraPublisherDisconnected(const TcpClient &client, const std::vector<TcpClient> &clients) {
    if (client.publisher && --cameraPublishers == 0) {
        if (latestCameraState!="CAMERA:STATE:ERROR\n") latestCameraState="CAMERA:STATE:STOPPED\n";
        latestTargetState="TARGET:NO_TARGET\n";
        broadcastQt(clients,latestCameraState); broadcastQt(clients,latestTargetState);
        latestDetection = "NO_DETECTION\n";
        broadcastQt(clients, "CAMERA:DISCONNECTED\n");
        broadcastQt(clients, latestDetection);
    }
}
/**
 * 입력 루프가 실제 추적 상태 변화를 감지해 ONVIF 관측 채널에 보낸다. 예약 이동 완료/버튼 전환도 같은 경로로 알려진다.
 */
void publishTrackingChanges(const std::vector<TcpClient> &clients) {
    static bool last = false;
    const bool current = isTrackingRequested();
    static std::string lastRuntime;
    static auto nextRuntime=std::chrono::steady_clock::time_point{};
    const auto runtime=runtimeStateLine();
    const auto now=std::chrono::steady_clock::now();
    if (runtime!=lastRuntime && now>=nextRuntime) {
        broadcastQt(clients,runtime+"\n"); lastRuntime=runtime;
        nextRuntime=now+std::chrono::milliseconds(1000/edge::appConfig().tracking.metadataHz);
    }
    if (current == last) return;
    last = current;
    broadcastQt(clients, current ? "TRACK:ON\n" : "TRACK:OFF\n");
    if (!current) {
        latestTargetState="TARGET:NO_TARGET\n"; broadcastQt(clients,latestTargetState);
        latestDetection = "NO_DETECTION\n";
        broadcastQt(clients, latestDetection);
    }
}
} // namespace ptz
