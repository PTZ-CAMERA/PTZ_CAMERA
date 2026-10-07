#pragma once
#include <string>
#include <functional>
#include <memory>
namespace onvif {
class EventBroker;
inline constexpr const char* SOAP="http://www.w3.org/2003/05/soap-envelope";
inline constexpr const char* DEVICE="http://www.onvif.org/ver10/device/wsdl";
inline constexpr const char* MEDIA="http://www.onvif.org/ver10/media/wsdl";
inline constexpr const char* PTZ="http://www.onvif.org/ver20/ptz/wsdl";
inline constexpr const char* EVENTS="http://www.onvif.org/ver10/events/wsdl";
inline constexpr const char* WSNT="http://docs.oasis-open.org/wsn/b-2";
inline constexpr const char* TOPICS="urn:ptz-camera:events";
inline constexpr const char* TYPES="http://www.onvif.org/ver10/schema";
inline constexpr const char* DISCOVERY="http://schemas.xmlsoap.org/ws/2005/04/discovery";
struct Config {
    std::string interface="eth0", ip, uuid;
    unsigned port=8080, rtspPort=8554, servoPort=5000;
    std::function<std::string(const std::string&)> servoRequest;
    bool discovery=true, autoAddress=false, authRequired=false;
    std::string credentialsFile;
    std::shared_ptr<EventBroker> events;
    unsigned requestId=0;
    std::string baseUrl() const;
    std::string eventsUrl() const;
    std::string deviceUrl() const;
    std::string mediaUrl() const;
    std::string ptzUrl() const;
    std::string streamUrl() const;
};
struct Response { int status;std::string xml;bool deferred=false; };
Response soapResponse(const Config& config,const std::string& path,const std::string& xml);
std::string probeResponse(const Config& config,const std::string& xml);
} // namespace onvif
