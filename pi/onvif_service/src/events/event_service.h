#pragma once
#include "protocol/service.h"
#include "xml/xml.h"
namespace onvif {
Response eventOperation(const Config& config,const std::string& path,const Node& operation);
} // namespace onvif
