#!/usr/bin/env bash
# 실제 카메라를 사용하지 않는 pipeline 시험. 운영 포트/설정과 분리한다.
set -euo pipefail
fixture=$1
media=$2
case_dir=$(mktemp -d /tmp/ptz-capture-test.XXXXXX)
media_pid=''
cleanup() {
  if [[ -n "$media_pid" ]]; then kill "$media_pid" 2>/dev/null || true; wait "$media_pid" 2>/dev/null || true; fi
  cat "$case_dir/media.log"
  unlink "$case_dir/media.log"
  unlink "$case_dir/config.yml"
  rmdir "$case_dir"
}
trap cleanup EXIT
cat > "$case_dir/config.yml" <<'YAML'
rtspAddress: 127.0.0.1:18555
rtspTransports: [tcp]
rtmp: false
srt: false
moq: false
webrtc: false
hls: false
paths:
  cam:
    source: publisher
YAML
"$media" "$case_dir/config.yml" > "$case_dir/media.log" 2>&1 &
media_pid=$!
sleep 0.2
kill -0 "$media_pid"
"$fixture" synthetic
