#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
  echo 'Run this installer with sudo.' >&2
  exit 1
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# 기존 사용자 설정을 덮어쓰지 않고 첫 설치에만 공통 기본값을 배치한다.
if [[ ! -e /etc/ptz-camera/camera.json ]]; then
  install -Dm644 "$script_dir/../../edge-camera/config/camera.json" /etc/ptz-camera/camera.json
fi
cmake -S "$script_dir" -B "$script_dir/build"
cmake --build "$script_dir/build" -j2
ctest --test-dir "$script_dir/build" --output-on-failure

install -Dm755 "$script_dir/build/servo_test" /usr/local/libexec/ptz-servo/servo_test
install -Dm644 "$script_dir/ptz-servo.service" /etc/systemd/system/ptz-servo.service
systemctl daemon-reload
systemctl enable ptz-servo.service
systemctl restart ptz-servo.service

echo 'Servo loopback IPC service installed on port 5000.'
