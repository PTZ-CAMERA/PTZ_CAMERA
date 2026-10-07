#pragma once
#include "xml/xml.h"
namespace onvif::client {
std::unique_ptr<Node> call(const std::string& url,const std::string& operation);
const Node& required(const Node& node,const std::string& ns,const std::string& local);
} // namespace onvif::client
