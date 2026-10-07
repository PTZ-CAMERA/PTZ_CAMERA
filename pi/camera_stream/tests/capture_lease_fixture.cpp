/**
 * [학습 안내] 캡처 생명주기 시험용 가짜 제어 서버
 * 테스트 전용 loopback 포트에서 TRACK:LEASE만 응답한다. 상태 파일의 ON/OFF를 읽는다.
 * 실제 서보/GPIO/ONVIF 서비스와 독립적으로 실행한다.
 */
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <csignal>
#include <fstream>
#include <string>
namespace {
volatile std::sig_atomic_t stopping=0;
void stop(int) { stopping=1; }
}
int main(int argc,char** argv) {
    if (argc!=3) return 1;
    std::signal(SIGTERM,stop); std::signal(SIGINT,stop);
    const int server=socket(AF_INET,SOCK_STREAM,0);
    int reuse=1; setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    sockaddr_in address{};
    address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    address.sin_port=htons(static_cast<unsigned short>(std::stoi(argv[1])));
    if (bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0 || listen(server,1)<0) return 2;
    int client=-1;
    std::string pending;
    while (!stopping) {
        fd_set fds; FD_ZERO(&fds); FD_SET(server,&fds);
        if (client>=0) FD_SET(client,&fds);
        timeval timeout{0,100000};
        const int ready=select((client>server?client:server)+1,&fds,nullptr,nullptr,&timeout);
        if (ready<=0) continue;
        if (FD_ISSET(server,&fds)) {
            const int accepted=accept(server,nullptr,nullptr);
            if (client>=0) close(client);
            client=accepted; pending.clear(); continue;
        }
        if (client>=0 && FD_ISSET(client,&fds)) {
            char bytes[128]; const auto n=recv(client,bytes,sizeof(bytes),0);
            if (n<=0) { close(client); client=-1; continue; }
            pending.append(bytes,static_cast<std::size_t>(n));
            std::size_t end;
            while ((end=pending.find('\n'))!=std::string::npos) {
                const auto command=pending.substr(0,end); pending.erase(0,end+1);
                std::ifstream file(argv[2]); std::string state; file>>state;
                const std::string reply=command=="TRACK:LEASE" ?
                    (state=="ON" ? "TRACK:ON\n" : "TRACK:OFF\n") : "ERR\n";
                if (send(client,reply.data(),reply.size(),MSG_NOSIGNAL)!=static_cast<ssize_t>(reply.size())) {
                    close(client); client=-1; break;
                }
            }
        }
    }
    if (client>=0) close(client);
    close(server);
}
