#!/usr/bin/env bash
set -euo pipefail

# Do not undo an intentional service stop.
if ! /usr/bin/systemctl is-active --quiet ptz-camera.service; then
  exit 0
fi

# A probe is itself an RTSP viewer: check real viewers first, or the watchdog
# would repeatedly wake the on-demand camera when nobody is watching.
metrics=$(/usr/bin/curl --fail --silent --show-error --max-time 3 \
  http://127.0.0.1:9998/metrics)
reader_count=$(printf '%s\n' "$metrics" | /usr/bin/awk \
  '/^paths_readers\{.*name="cam"/ { count += $NF } END { print count+0 }')
if (( reader_count == 0 )); then
  exit 0
fi

has_frame() {
  /usr/bin/timeout 12 /usr/bin/ffmpeg -nostdin -hide_banner -loglevel error \
    -rtsp_transport tcp -i rtsp://127.0.0.1:8554/cam \
    -frames:v 1 -f null - >/dev/null 2>&1
}

if has_frame; then
  exit 0
fi

# Give a starting publisher one more chance before interrupting the stream.
/usr/bin/sleep 5
if has_frame; then
  exit 0
fi

echo 'Camera stream has no decodable frame; restarting ptz-camera.service'
/usr/bin/systemctl restart ptz-camera.service
