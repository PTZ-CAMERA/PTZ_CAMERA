#!/usr/bin/env bash
set -euo pipefail
# 소프트웨어 부하 시험이며 GPIO·실제 카메라·RTSP 포트에 접근하지 않는다.
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
seconds=${1:-180}
repeats=${2:-3}
warmup=${3:-5}
if [[ ! $seconds =~ ^[0-9]+$ || ! $repeats =~ ^[0-9]+$ || ! $warmup =~ ^[0-9]+$ ]] ||
   (( seconds <= warmup || repeats < 1 )); then
    echo 'Usage: run_suite.sh [seconds > warmup] [repeats >= 1] [warmup seconds]' >&2
    exit 1
fi
cmake -S "$script_dir" -B "$script_dir/build"
cmake --build "$script_dir/build" -j2
run_dir="$script_dir/results/$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$run_dir"
{
    date -Is
    sha256sum "$script_dir"/build/pipeline_benchmark "$script_dir"/build/resource_sampler "$script_dir"/build/summarize
    uname -a
    printf 'seconds=%s repeats=%s warmup=%s\n' "$seconds" "$repeats" "$warmup"
    git -C "$script_dir" rev-parse HEAD
    git -C "$script_dir" status --short
    sha256sum "$script_dir"/*.cpp "$script_dir"/../../common/performance/*.h "$script_dir"/../../pi/camera_stream/src/person_detector.cpp
    pkg-config --modversion gstreamer-1.0 opencv4
    sha256sum /usr/local/share/ptz-camera/deploy.prototxt /usr/local/share/ptz-camera/mobilenet_iter_73000.caffemodel
    systemctl is-active ptz-camera.service ptz-servo.service || true
    vcgencmd get_throttled 2>/dev/null || true
} > "$run_dir/conditions.txt"
trial_pid=''
sampler_pid=''
cleanup() {
    if [[ -n $trial_pid ]]; then kill "$trial_pid" 2>/dev/null || true; fi
    if [[ -n $sampler_pid ]]; then kill "$sampler_pid" 2>/dev/null || true; fi
}
trap cleanup EXIT INT TERM
for ((repeat=1; repeat<=repeats; ++repeat)); do
    # 순서에 따른 온도 편향을 줄이기 위해 반복마다 순서를 뒤집는다.
    cases=(off async1 async2 async3 sync2 fifo2)
    if (( repeat % 2 == 0 )); then cases=(fifo2 sync2 async3 async2 async1 off); fi
    for case_name in "${cases[@]}"; do
        mode=async; threads=2; policy=latest
        case "$case_name" in
            off) mode=off;;
            async1) threads=1;;
            async3) threads=3;;
            sync2) mode=sync;;
            fifo2) policy=fifo;;
        esac
        trial_dir="$run_dir/${repeat}-${case_name}"
        mkdir -p "$trial_dir"
        printf 'Running %s repeat %s (%s seconds)\n' "$case_name" "$repeat" "$seconds"
        vcgencmd get_throttled > "$trial_dir/throttled-before.txt" 2>/dev/null || true
        PTZ_PERF_LOG="$trial_dir/events.csv" "$script_dir/build/pipeline_benchmark" \
            --mode "$mode" --threads "$threads" --policy "$policy" --seconds "$seconds" \
            > "$trial_dir/runtime.txt" 2>&1 &
        trial_pid=$!
        "$script_dir/build/resource_sampler" --pid "$trial_pid" --seconds "$((seconds + 5))" \
            --output "$trial_dir/resources.csv" &
        sampler_pid=$!
        wait "$trial_pid"
        trial_pid=''
        wait "$sampler_pid"
        sampler_pid=''
        vcgencmd get_throttled > "$trial_dir/throttled-after.txt" 2>/dev/null || true
        "$script_dir/build/summarize" "$trial_dir/events.csv" "$trial_dir/resources.csv" "$warmup" \
            > "$trial_dir/summary.md"
        cat "$trial_dir/runtime.txt"
    done
done
printf 'Results: %s\n' "$run_dir"
