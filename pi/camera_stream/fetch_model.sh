#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}" )" && pwd)
model_dir="$script_dir/models"
mkdir -p "$model_dir"

# The source is the author's MIT-licensed, pretrained VOC MobileNet-SSD.
# Pinning the commit and SHA-256 prevents a moving upstream branch from silently
# changing the model that was checked with this C++ detector.
source_base=https://raw.githubusercontent.com/chuanqi305/MobileNet-SSD/bb17b6c3eef36d80be441ae8e5339be66e8e3b7a

fetch() {
  local filename=$1 expected=$2 source=$3
  local target="$model_dir/$filename"
  if [[ -f "$target" ]] && printf '%s  %s\n' "$expected" "$target" | sha256sum -c --status; then
    return
  fi
  local temporary
  temporary=$(mktemp "$model_dir/.download.XXXXXX")
  trap 'rm -f -- "$temporary"' RETURN
  curl --fail --location --silent --show-error --retry 3 --max-time 180 \
    --output "$temporary" "$source_base/$source"
  printf '%s  %s\n' "$expected" "$temporary" | sha256sum -c --status
  mv -- "$temporary" "$target"
  trap - RETURN
}

fetch deploy.prototxt \
  2d180f723b3109e21f8287f6b3c691390d07b60eed998327cd3259ffa0e50608 \
  deploy.prototxt
fetch mobilenet_iter_73000.caffemodel \
  52eed8be80522c152a17fb56740de705b79881bde1a167e0e747310523685fc7 \
  mobilenet_iter_73000.caffemodel

curl --fail --location --silent --show-error --retry 3 --max-time 30 \
  --output "$model_dir/LICENSE" "$source_base/LICENSE"
echo "MobileNet-SSD model files ready in $model_dir"
