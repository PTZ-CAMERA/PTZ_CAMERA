/**
 * [학습 안내] 시청자 없는 자동 추적의 캡처 생명주기
 * MediaMTX runOnInit에서 실행된다. 서보의 TRACK:LEASE를 조회하고 추적이 필요할 때만
 * loopback RTSP reader를 하나 유지한다. reader가 생기면 기존 runOnDemand가 publisher를 연다.
 * 이 프로그램은 프레임을 디코딩/분석하거나 GPIO를 열지 않는다.
 * 추적 종료/서보 단절/프로세스 종료 시 reader를 닫고 기존 종료 유예 시간을 적용받는다.
 */
extern "C" {
#include <libavformat/avformat.h>
}
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include "config/app_config.h"
#include <charconv>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
using Clock = std::chrono::steady_clock;
Clock::time_point ioDeadline;
int leaseFd = -1;
unsigned short controlPort = 5000;
std::string rtspUrl = "rtsp://127.0.0.1:8554/cam";
bool leaseActive = false;
Clock::time_point nextCheck;
bool trackingRequested();
void refreshLease() {
    if (Clock::now() < nextCheck) return;
    leaseActive = trackingRequested();
    nextCheck = Clock::now() + std::chrono::milliseconds(250);
}
int interrupt(void*) {
    if (stopping || Clock::now() >= ioDeadline) return 1;
    // RTSP 연결이 열리는 도중에도 추적 종료/제어 단절을 검사해 16초를 기다리지 않는다.
    refreshLease();
    return !leaseActive;
}

// 별도의 짧은 조회 연결을 사용한다. CAMERA:HELLO를 보내 publisher 상태를 오염시키지 않는다.
bool trackingRequested() {
    if (leaseFd < 0) {
        leaseFd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (leaseFd < 0) return false;
        timeval timeout{0,200000};
        setsockopt(leaseFd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        setsockopt(leaseFd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        sockaddr_in address{};
        address.sin_family=AF_INET; address.sin_port=htons(controlPort);
        address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if (connect(leaseFd,reinterpret_cast<sockaddr*>(&address),sizeof(address)) != 0) {
            close(leaseFd); leaseFd=-1; return false;
        }
    }
    const std::string request="TRACK:LEASE\n";
    std::size_t sent=0;
    while (sent<request.size()) {
        const auto n=send(leaseFd,request.data()+sent,request.size()-sent,MSG_NOSIGNAL);
        if (n<=0) break;
        sent+=static_cast<std::size_t>(n);
    }
    std::string input;
    while (sent==request.size() && input.size()<64 && input.find('\n')==std::string::npos) {
        char bytes[64]; const auto n=recv(leaseFd,bytes,sizeof(bytes),0);
        if (n<=0) break;
        input.append(bytes,static_cast<std::size_t>(n));
    }
    if (input != "TRACK:ON\n" && input != "TRACK:OFF\n") {
        close(leaseFd); leaseFd=-1; return false;
    }
    return input == "TRACK:ON\n";
}
}
int main(int argc, char** argv) {
    try { edge::loadDefaultConfig(); } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
    controlPort=static_cast<unsigned short>(edge::appConfig().controlPort);
    rtspUrl="rtsp://127.0.0.1:"+std::to_string(edge::appConfig().rtspPort)+"/"+edge::appConfig().rtspPath;
    // 테스트/진단 주소는 loopback으로 제한한다. 운영 기본값과 서비스 경로는 유지한다.
    for (int i=1; i<argc; i+=2) {
        if (i+1>=argc) return 1;
        const std::string key=argv[i], value=argv[i+1];
        if (key=="--control-port") {
            unsigned port=0;
            const auto parsed=std::from_chars(value.data(),value.data()+value.size(),port);
            if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || port==0 || port>65535) return 1;
            controlPort=static_cast<unsigned short>(port);
        } else if (key=="--rtsp-url" && value.rfind("rtsp://127.0.0.1:",0)==0 &&
                   value.find('@')==std::string::npos && value.find_first_of("\r\n") == std::string::npos) {
            rtspUrl=value;
        } else return 1;
    }
    std::signal(SIGINT,stop); std::signal(SIGTERM,stop);
    avformat_network_init();
    AVFormatContext* reader=nullptr;
    AVPacket* packet=av_packet_alloc();
    if (!packet) return 1;
    nextCheck=Clock::now();
    auto retry=Clock::now();
    while (!stopping) {
        refreshLease();
        if (!leaseActive) {
            if (reader) {
                avformat_close_input(&reader);
                std::cout << "Tracking capture lease released\n" << std::flush;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (!reader) {
            if (Clock::now()<retry) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue;
            }
            reader=avformat_alloc_context();
            if (!reader) break;
            reader->interrupt_callback={interrupt,nullptr};
            // 기존 on-demand 시작 제한은 15초다. 개방 중에도 종료 신호는 interrupt가 처리한다.
            ioDeadline=Clock::now()+std::chrono::seconds(16);
            AVDictionary* options=nullptr;
            av_dict_set(&options,"rtsp_transport","tcp",0);
            const int result=avformat_open_input(&reader,rtspUrl.c_str(),nullptr,&options);
            av_dict_free(&options);
            if (result<0) {
                avformat_close_input(&reader);
                retry=Clock::now()+std::chrono::seconds(1); continue;
            }
            std::cout << "Tracking capture lease acquired\n" << std::flush;
        }
        ioDeadline=Clock::now()+std::chrono::seconds(1);
        const int result=av_read_frame(reader,packet);
        av_packet_unref(packet); // 압축 packet은 버린다. 디코더/추론 중복 실행 없음.
        if (result<0) {
            avformat_close_input(&reader);
            retry=Clock::now()+std::chrono::seconds(1);
        }
    }
    avformat_close_input(&reader);
    av_packet_free(&packet);
    if (leaseFd >= 0) close(leaseFd);
    avformat_network_deinit();
    return 0;
}
