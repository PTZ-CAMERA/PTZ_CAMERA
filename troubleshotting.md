# Troubleshooting log

Record each new problem investigated in this project here: symptom, cause,
checks, change, and verification. Add a new dated entry for each incident.

## 2026-10-02 — `jq` package download returned 404 during on-demand setup

- **Symptom:** `apt-get install jq` failed because the Bullseye security repository returned 404 for its indexed `jq` and `libjq1` package versions. `apt-get update` did not resolve it.
- **Change:** Avoided the extra package. The on-demand watchdog reads MediaMTX's localhost-only Prometheus metrics with the already installed `curl` and `awk` tools.
- **Verification:** MediaMTX exposed `paths_readers{name="cam",readerType="",state="notReady"} 0` while idle and a reader count of 1 during an RTSP or WebRTC viewing session.

## 2026-10-02 — GStreamer hardware H.264 encoder failed during C++ migration

- **Symptom:** `libcamerasrc → videoconvert → v4l2h264enc` started but then reported `Failed to process frame` from `GstV4l2VideoEnc` on this Pi 4. A direct NV12 path failed format negotiation.
- **Check:** The same 1280×720, 30fps camera stream worked with `videoconvert → x264enc`; the `rtspclientsink` package was missing and was installed as `gstreamer1.0-rtsp`.
- **Cause:** The V4L2 encoder path could not process these GStreamer camera buffers in the tested configuration. The exact driver/buffer incompatibility is not yet established.
- **Change:** Added a C++17 GStreamer publisher using `libcamerasrc`, `videoconvert`, `x264enc`, `h264parse`, and `rtspclientsink`; MediaMTX now starts this executable. The old `libcamera-vid | ffmpeg` publishing script was removed.
- **Verification:** CMake build passed. MediaMTX accepted the local RTSP publisher; FFmpeg decoded 151 frames in a 5-second stream sample. RTSP reported H.264 Constrained Baseline, 1280×720, 30fps, and the WebRTC page returned HTTP 200. The publisher used approximately 115% CPU during this check; remote browser playback remains to be checked.

## 2026-10-01 — Intermittent magenta/pink video frames

- **Symptom:** The PC stream occasionally shows a pink image with horizontal corruption.
- **Isolation:** Stopped `ptz-camera.service` and recorded 20 seconds directly from `libcamera-vid` to `/tmp/ptz_camera_20s.h264` (593 frames). Frames 295 (9.84 s) and 530 (17.67 s) visibly contain magenta corruption. This occurred before MediaMTX, WebRTC, the LAN, or Qt could affect the image.
- **Other checks:** A separate 12-second MJPEG capture (353 frames) showed no obvious color outliers, but this short sample cannot rule out an intermittent camera-side issue. `vcgencmd get_throttled` was `0x0`, the Pi Ethernet interface showed no RX/TX errors, and no servo process was running during inspection.
- **Conclusion:** The fault is on the Pi capture/encode side, not solely in network delivery or PC playback. The exact point (CSI cable/connector, camera, ISP, or H.264 encoding) is not yet established.
- **Change/verification:** No capture settings changed. The camera service was restarted after the direct tests and RTSP again reported H.264 1280×720. Inspect the ribbon/connector with power removed, then repeat a direct capture after reseating; if the artifact remains, compare longer MJPEG and H.264 captures to separate camera input from encoder behavior.

## 2026-10-01 — Camera page loads but live video stops

- **Symptom:** The PC WebRTC player lost video. The `ptz-camera.service` process was still active, but RTSP `/cam` returned 404 because no stream was available.
- **Evidence:** The IMX219 remained visible in `libcamera-vid --list-cameras`. MediaMTX logged a local RTSP publisher `i/o timeout` at 02:47:04, followed by `no stream is available on path 'cam'` for PC WebRTC requests. Pi power reported `throttled=0x0`; no camera or voltage errors appeared in recent kernel logs. A standalone 5-second camera capture produced a 5.9 MB H.264 file.
- **Cause:** The camera-to-MediaMTX publishing pipeline stopped delivering video frames while its parent service stayed active. The underlying reason for the stall is not yet confirmed; the available checks do not establish a cable or power fault.
- **Change:** Restarted `ptz-camera.service`. Added `pi/camera_stream/watchdog.sh` and a systemd timer that checks for a decodable RTSP frame every 30 seconds, retries once, then restarts the camera service if both checks fail. The timer skips an intentionally stopped camera service.
- **Verification:** After restart, RTSP decoded a frame and reported H.264 1280×720; the WebRTC page returned HTTP 200. The timer ran successfully on a healthy stream. PC playback after the change still needs confirmation.

## 2026-10-01 — Servo movement stops before 0°/180°

- **Symptom:** The servos appear to stop around the middle portion of their travel.
- **Cause:** The current application deliberately clamps PAN to 30–150° and TILT to 40–140° in `pi/servo_test/src/main.cpp`. These values came from the original control requirements; they are software limits, not a measured limit of the installed servos.
- **Check:** `angleToPulseWidth()` maps 0–180° to 1000–2000 µs, but `setPanAngle()` and `setTiltAngle()` clamp their inputs before calling it.
- **Initial finding:** At the time of inspection, PAN was 30–150° and TILT was 40–140°.
- **Follow-up change:** At the user's request to extend PAN slightly, PAN was changed to 25–155°; TILT remains 40–140°.
- **Verification:** The CMake build passed. Physical clearance and maximum travel remain unverified. A `servo_test` process was already running, so that process continues using the previous limits until restarted.

## 2026-10-01 — `servo_test` failed to initialize pigpio

- **Symptom:** `sudo ./build/servo_test` printed `initInitialise: bind to port 8888 failed (Address already in use)` and `pigpio initialization failed`.
- **Cause:** The MediaMTX camera service's HLS listener and pigpio both used TCP port 8888. `ss -lntp` showed `mediamtx` listening on 8888.
- **Change:** Set `hlsAddress: :8891` in `pi/camera_stream/mediamtx.yml`, updated the HLS URL in `pi/camera_stream/README.md`, and restarted `ptz-camera.service`.
- **Verification:** Port 8888 was free; a small C program successfully called `gpioInitialise()` and `gpioTerminate()`. The camera RTSP stream still reported H.264 at 1280×720, and the WebRTC and HLS pages both returned HTTP 200. The user still needs to retry the full servo program.
