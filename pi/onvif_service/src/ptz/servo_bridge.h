#pragma once
#include <stdexcept>
#include <string>
namespace onvif {
struct BackendError : std::runtime_error { using std::runtime_error::runtime_error; };
// Pi 내부 전용 RPC. 외부 HTTP 요청이 GPIO를 직접 소유하지 않게 한다.
std::string servoRpc(unsigned port,const std::string& command);
} // namespace onvif
