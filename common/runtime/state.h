#pragma once
namespace edge {
enum class CameraState { Stopped, Starting, Running, Error };
enum class StreamState { Stopped, Streaming, Error };
inline const char* cameraStateName(CameraState s) {
    switch(s) { case CameraState::Stopped:return "STOPPED"; case CameraState::Starting:return "STARTING";
    case CameraState::Running:return "RUNNING"; case CameraState::Error:return "ERROR"; }
    return "ERROR";
}
inline const char* streamStateName(StreamState s) {
    switch(s) { case StreamState::Stopped:return "STOPPED"; case StreamState::Streaming:return "STREAMING"; case StreamState::Error:return "ERROR"; }
    return "ERROR";
}
}
