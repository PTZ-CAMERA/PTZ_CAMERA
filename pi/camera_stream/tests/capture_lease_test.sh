#!/usr/bin/env bash
# 격리된 MediaMTX + 합성 H.264 + 가짜 추적 상태로 reader 생명주기를 검증한다.
# 실제 카메라, 모델, GPIO, 설치된 서비스 설정을 사용하지 않는다.
set -euo pipefail
lease_exe=$1
fixture_exe=$2
media_exe=$3
ffmpeg_exe=$4
curl_exe=$5
case_dir=$(mktemp -d /tmp/ptz-lease-test.XXXXXX)
media_pid=''
fixture_pid=''
cleanup() {
  for process in "$media_pid" "$fixture_pid"; do
    if [[ -n "$process" ]]; then kill "$process" 2>/dev/null || true; wait "$process" 2>/dev/null || true; fi
  done
  if [[ -f "$case_dir/media.log" ]]; then cat "$case_dir/media.log"; fi
  for name in media.log fixture.log state config.yml; do
    if [[ -f "$case_dir/$name" ]]; then unlink "$case_dir/$name"; fi
  done
  rmdir "$case_dir"
}
trap cleanup EXIT
printf 'OFF\n' > "$case_dir/state"
"$fixture_exe" 18500 "$case_dir/state" > "$case_dir/fixture.log" 2>&1 &
fixture_pid=$!
cat > "$case_dir/config.yml" <<YAML
rtspAddress: 127.0.0.1:18554
rtspTransports: [tcp]
rtmp: false
srt: false
moq: false
webrtc: false
hls: false
metrics: true
metricsAddress: 127.0.0.1:19998
paths:
  cam:
    source: publisher
    runOnInit: $lease_exe --control-port 18500 --rtsp-url rtsp://127.0.0.1:18554/cam
    runOnInitRestart: true
    runOnDemand: $ffmpeg_exe -nostdin -hide_banner -loglevel error -re -f lavfi -i color=c=black:s=320x180:r=10 -c:v libx264 -pix_fmt yuv420p -preset ultrafast -tune zerolatency -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:18554/cam
    runOnDemandStartTimeout: 10s
    runOnDemandCloseAfter: 1s
YAML
"$media_exe" "$case_dir/config.yml" > "$case_dir/media.log" 2>&1 &
media_pid=$!
readers() {
  "$curl_exe" -fsS --max-time 1 http://127.0.0.1:19998/metrics |
    awk '/^paths_readers\{.*name="cam"/ { count += $NF } END { print count+0 }'
}
wait_count() {
  local expected=$1
  for ((trial=0; trial<60; ++trial)); do
    kill -0 "$media_pid" "$fixture_pid"
    if [[ "$(readers 2>/dev/null || true)" == "$expected" ]]; then return 0; fi
    sleep 0.2
  done
  echo "Expected readers=$expected" >&2
  return 1
}
wait_count 0
sleep 0.5
[[ "$(readers)" == 0 ]]
printf 'ON\n' > "$case_dir/state"
wait_count 1
# 일반 시청자 없이 내부 reader 하나로 합성 publisher가 켜져야 한다.
"$curl_exe" -fsS --max-time 1 http://127.0.0.1:19998/metrics | awk '
  /^paths\{.*name="cam".*state="ready"/ { found=1 } END { exit !found }'
printf 'OFF\n' > "$case_dir/state"
wait_count 0
# 제어 서비스 단절도 lease 해제와 재연결에 반영되어야 한다.
printf 'ON\n' > "$case_dir/state"
wait_count 1
kill "$fixture_pid"
wait "$fixture_pid"
fixture_pid=''
wait_count 0
echo 'PASS idle / tracking without viewer / OFF / control disconnect'
