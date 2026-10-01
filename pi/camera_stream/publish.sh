#!/usr/bin/env bash
set -euo pipefail

# Bullseye's libcamera-vid captures and encodes H.264. FFmpeg only remuxes it
# into RTSP; it does not re-encode the video.
/usr/bin/libcamera-vid --nopreview --timeout 0 \
  --width 1280 --height 720 --framerate 30 \
  --codec h264 --inline --output - |
  /usr/bin/ffmpeg -hide_banner -loglevel warning \
    -f h264 -framerate 30 -i pipe:0 \
    -an -c:v copy -f rtsp -rtsp_transport tcp \
    rtsp://127.0.0.1:8554/cam
