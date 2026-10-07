#pragma once
#include "protocol/service.h"
namespace onvif {
// 표준 ONVIF PTZ와 분리된 카메라 확장 JSON 인터페이스. HTTP Digest는 기존 서버가 처리한다.
Response cameraControl(const Config& config,const std::string& body);
}
