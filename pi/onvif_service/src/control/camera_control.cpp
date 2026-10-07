/**
 * [학습 안내] 카메라 전용 tracking_set/get과 metadata_get
 * VMS 요청을 검증한 뒤 기존 loopback bridge와 event broker를 재사용한다.
 * GPIO/카메라를 열지 않고 ONVIF 표준 동작 이름의 의미를 바꾸지 않는다.
 * HTTP 주소는 /camera/control, 인증은 ONVIF 서버와 같은 Digest다.
 */
#include "control/camera_control.h"
#include "config/app_config.h"
#include "events/event_broker.h"
#include "ptz/servo_bridge.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <stdexcept>
namespace onvif {
Response cameraControl(const Config& config,const std::string& body) {
    using Json=nlohmann::json;
    try {
        if (body.size()>4096) return {413,"{\"type\":\"error\",\"error\":\"request_too_large\"}"};
        const auto request=Json::parse(body);
        if (!request.is_object()||!request.contains("type")||!request["type"].is_string()||
            !request.contains("cameraId")||!request["cameraId"].is_string()||
            request["cameraId"]!=edge::appConfig().cameraId) throw std::invalid_argument("Invalid camera request");
        for (auto it=request.begin();it!=request.end();++it)
            if (it.key()!="type"&&it.key()!="cameraId"&&it.key()!="data"&&it.key()!="requestId")
                throw std::invalid_argument("Unknown request field");
        if (request.contains("requestId")&&(!request["requestId"].is_string()||request["requestId"].get<std::string>().size()>64))
            throw std::invalid_argument("Invalid requestId");
        const auto rpc=[&](const std::string& command) {
            const auto reply=config.servoRequest ? config.servoRequest(command) : servoRpc(config.servoPort,command);
            if (reply.rfind("ERR",0)==0) throw BackendError("Camera backend rejected request");
            return reply;
        };
        const auto type=request["type"].get<std::string>();
        if (type=="tracking_set") {
            if (!request.contains("data")||!request["data"].is_object()||request["data"].size()!=1||
                !request["data"].contains("enabled")||!request["data"]["enabled"].is_boolean())
                throw std::invalid_argument("enabled boolean required");
            const auto command=request["data"]["enabled"].get<bool>() ? "ONVIF:TRACK" : "ONVIF:TRACK_OFF";
            if (rpc(command)!="OK") throw BackendError("Invalid camera acknowledgement");
        } else if (type!="tracking_get"&&type!="metadata_get") throw std::invalid_argument("Unsupported camera operation");
        Json data;
        if (type=="metadata_get") {
            if (!config.events) throw BackendError("Metadata unavailable");
            data=Json::parse(config.events->snapshotJson());
        } else {
            int requested=0,allowed=0,fault=0,moving=0,pan=0,tilt=0;
            unsigned long long generation=0;
            char mode[16]{},extra=0;
            if (std::sscanf(rpc("ONVIF:RUNTIME").c_str(),"RUNTIME:%d,%d,%15[^,],%llu,%d,%d,%d,%d%c",
                &requested,&allowed,mode,&generation,&fault,&moving,&pan,&tilt,&extra)!=8)
                throw BackendError("Invalid runtime response");
            data={{"enabled",requested!=0},{"autoAllowed",allowed!=0},{"ptzMode",mode},
                  {"generation",std::to_string(generation)},{"fault",fault!=0},{"moving",moving!=0},
                  {"pan",pan},{"tilt",tilt}};
        }
        Json response={{"cameraId",edge::appConfig().cameraId},{"type",type+"_result"},{"data",data}};
        if (request.contains("requestId")) response["requestId"]=request["requestId"];
        return {200,response.dump()};
    } catch (const BackendError&) {
        return {500,"{\"type\":\"error\",\"error\":\"backend_unavailable\"}"};
    } catch (const std::exception&) {
        return {400,"{\"type\":\"error\",\"error\":\"invalid_request\"}"};
    }
}
} // namespace onvif
