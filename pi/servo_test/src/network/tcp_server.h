#pragma once

#include <string>
#include <vector>
#include <sys/select.h>

namespace ptz {
struct TcpClient {
    int fd;
    std::string buffer; // TCP는 줄 단위로 도착하지 않으므로 연결마다 누적한다.
    std::string peer;
    bool publisher = false;
    bool observer = false;
};

struct TcpServer {
    int fd = -1;
    std::vector<TcpClient> clients;
};
int openControlServer();
int connectLocalServer();
void addServerReadFds(const TcpServer& server, fd_set& readFds, int& maxFd);
void processServerReadFds(TcpServer& server, const fd_set& readFds);
void closeControlServer(TcpServer& server);
void reply(int fd, const std::string& message);
void broadcastQt(const std::vector<TcpClient>& clients, const std::string& line);
} // namespace ptz
