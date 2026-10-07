/** [학습 안내] 표준 PTZ와 분리된 추적 스위치의 검증. 실제 GPIO는 사용하지 않는다. */
#include "control/camera_control.h"
#include "events/event_broker.h"
#include "config/app_config.h"
#include <nlohmann/json.hpp>
#include <cassert>
int main() {
    onvif::Config c;
    c.events=std::make_shared<onvif::EventBroker>();
    std::string command;
    c.servoRequest=[&](const std::string& input) {
        command=input;
        return input=="ONVIF:RUNTIME" ? "RUNTIME:1,0,MANUAL,7,0,0,95,90" : "OK";
    };
    auto response=onvif::cameraControl(c,R"({"type":"tracking_set","cameraId":"CAM01","data":{"enabled":true},"requestId":"r1"})");
    assert(response.status==200);
    const auto j=nlohmann::json::parse(response.xml);
    assert(j["data"]["enabled"]==true && j["data"]["autoAllowed"]==false && j["requestId"]=="r1");
    // OFF 명령를 받은 사실은 callback에서 따로 검사한다.
    bool off=false;
    c.servoRequest=[&](const std::string& input) {
        if (input=="ONVIF:TRACK_OFF") off=true;
        return input=="ONVIF:RUNTIME" ? "RUNTIME:0,0,MANUAL,8,0,0,95,90" : "OK";
    };
    assert(onvif::cameraControl(c,R"({"type":"tracking_set","cameraId":"CAM01","data":{"enabled":false}})").status==200 && off);
    assert(onvif::cameraControl(c,R"({"type":"tracking_set","cameraId":"OTHER","data":{"enabled":true}})").status==400);
    assert(onvif::cameraControl(c,R"({"type":"tracking_set","cameraId":"CAM01","data":{"enabled":1}})").status==400);
    assert(onvif::cameraControl(c,R"({"type":"tracking_set","cameraId":"CAM01","data":{"enabled":true,"extra":1}})").status==400);
    assert(onvif::cameraControl(c,"{").status==400);
    assert(onvif::cameraControl(c,std::string(4097,' ')).status==413);
    assert(onvif::cameraControl(c,R"({"type":"metadata_get","cameraId":"CAM01"})").status==200);
    c.servoRequest=[](const std::string&){ return "ERR unavailable"; };
    assert(onvif::cameraControl(c,R"({"type":"tracking_get","cameraId":"CAM01"})").status==500);
}
