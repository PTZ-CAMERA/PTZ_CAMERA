#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
  echo 'Run this installer with sudo.' >&2
  exit 1
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cmake -S "$script_dir" -B "$script_dir/build"
cmake --build "$script_dir/build" -j2

install -Dm755 "$script_dir/build/servo_test" /usr/local/libexec/ptz-servo/servo_test
install -Dm644 "$script_dir/ptz-servo.service" /etc/systemd/system/ptz-servo.service
systemctl daemon-reload
systemctl enable ptz-servo.service
systemctl restart ptz-servo.service

echo 'Servo TCP service installed on port 5000.'
