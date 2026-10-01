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

install -Dm644 "$script_dir/mediamtx.yml" /etc/ptz-camera/mediamtx.yml
install -Dm755 "$script_dir/publish.sh" /usr/local/libexec/ptz-camera/publish.sh
install -Dm755 "$script_dir/watchdog.sh" /usr/local/libexec/ptz-camera/watchdog.sh
install -Dm644 "$script_dir/ptz-camera.service" /etc/systemd/system/ptz-camera.service
install -Dm644 "$script_dir/ptz-camera-watchdog.service" /etc/systemd/system/ptz-camera-watchdog.service
install -Dm644 "$script_dir/ptz-camera-watchdog.timer" /etc/systemd/system/ptz-camera-watchdog.timer

systemctl daemon-reload
systemctl enable ptz-camera.service
systemctl enable --now ptz-camera-watchdog.timer
systemctl try-restart ptz-camera.service

echo 'Camera service installed. Start it with: sudo systemctl start ptz-camera.service'
