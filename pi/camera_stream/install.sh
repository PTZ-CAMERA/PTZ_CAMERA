#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
  echo 'Run this installer with sudo.' >&2
  exit 1
fi

if [[ ! -x /usr/local/bin/mediamtx ]]; then
  echo 'Install the MediaMTX arm64 binary at /usr/local/bin/mediamtx first.' >&2
  exit 1
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# 기존 사용자 설정을 덮어쓰지 않고 첫 설치에만 공통 기본값을 배치한다.
if [[ ! -e /etc/ptz-camera/camera.json ]]; then
  install -Dm644 "$script_dir/../../edge-camera/config/camera.json" /etc/ptz-camera/camera.json
fi

"$script_dir/fetch_model.sh"

cmake -S "$script_dir" -B "$script_dir/build"
cmake --build "$script_dir/build" -j2

if [[ -s /etc/ptz-camera/access.conf ]]; then
  if [[ ! -x /usr/local/libexec/ptz-onvif/configure_access ]]; then
    echo 'Install the ONVIF authentication tools before reinstalling the camera service.' >&2
    exit 1
  fi
  /usr/local/libexec/ptz-onvif/configure_access --render-media \
    /etc/ptz-camera/access.conf "$script_dir/mediamtx.yml" /etc/ptz-camera/mediamtx.yml
else
  install -Dm644 "$script_dir/mediamtx.yml" /etc/ptz-camera/mediamtx.yml
fi
install -Dm755 "$script_dir/build/ptz_capture_lease" /usr/local/libexec/ptz-camera/ptz_capture_lease
install -Dm755 "$script_dir/build/ptz_camera_publisher" /usr/local/libexec/ptz-camera/ptz_camera_publisher
install -Dm644 "$script_dir/models/deploy.prototxt" /usr/local/share/ptz-camera/deploy.prototxt
install -Dm644 "$script_dir/models/mobilenet_iter_73000.caffemodel" /usr/local/share/ptz-camera/mobilenet_iter_73000.caffemodel
install -Dm644 "$script_dir/models/LICENSE" /usr/local/share/ptz-camera/MobileNet-SSD-LICENSE
"$script_dir/build/ptz_camera_publisher" --check-model
install -Dm755 "$script_dir/watchdog.sh" /usr/local/libexec/ptz-camera/watchdog.sh
install -Dm644 "$script_dir/ptz-camera.service" /etc/systemd/system/ptz-camera.service
install -Dm644 "$script_dir/ptz-camera-watchdog.service" /etc/systemd/system/ptz-camera-watchdog.service
install -Dm644 "$script_dir/ptz-camera-watchdog.timer" /etc/systemd/system/ptz-camera-watchdog.timer

systemctl daemon-reload
systemctl enable ptz-camera.service
systemctl enable --now ptz-camera-watchdog.timer
systemctl try-restart ptz-camera.service

echo 'Camera service installed. Start it with: sudo systemctl start ptz-camera.service'
