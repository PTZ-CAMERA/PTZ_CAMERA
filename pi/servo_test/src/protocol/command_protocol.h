#pragma once

#include "network/tcp_server.h"

namespace ptz {
// 개행을 제거한 명령을 해석하고 서보 제어/Qt 상태 알림으로 연결한다.
void handleTcpCommand(TcpClient& source, std::vector<TcpClient>& clients, std::string command);
void cameraPublisherDisconnected(const TcpClient& client, const std::vector<TcpClient>& clients);
bool hasCameraPublisher();
void publishTrackingChanges(const std::vector<TcpClient>& clients);
} // namespace ptz
