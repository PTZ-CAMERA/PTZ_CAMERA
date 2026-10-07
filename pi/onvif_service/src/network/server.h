#pragma once
#include "protocol/service.h"
#include <csignal>
namespace onvif {
std::string interfaceAddress(const std::string& name);
int runServer(const Config& config,const std::string& bindAddress,volatile std::sig_atomic_t& stop);
} // namespace onvif
