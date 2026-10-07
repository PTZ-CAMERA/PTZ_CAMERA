#pragma once
#include "protocol/service.h"
#include "xml/xml.h"
namespace onvif {
inline constexpr const char* POSITION_SPACE="http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace";
inline constexpr const char* TRANSLATION_SPACE="http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationGenericSpace";
inline constexpr const char* VELOCITY_SPACE="http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace";
std::string ptzConfiguration(const std::string& tag);
std::string ptzCapabilities();
std::string ptzOperation(const Config& config,const Node& operation);
} // namespace onvif
