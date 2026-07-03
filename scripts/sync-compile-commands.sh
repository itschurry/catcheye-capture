#!/usr/bin/env bash
set -euo pipefail

PROFILE="${1:-release}"
host_arch="$(uname -m)"
case "$host_arch" in
  x86_64) default_arch="amd64" ;;
  aarch64) default_arch="arm64" ;;
  *) default_arch="$host_arch" ;;
esac
ARCH="${2:-$default_arch}"
CONTAINER_WORKDIR="${CATCHEYE_CAPTURE_CONTAINER_WORKDIR:-/home/user/catcheye-capture}"
HOST_WORKDIR="${CATCHEYE_CAPTURE_HOST_WORKDIR:-$(pwd -P)}"

case "$ARCH:$PROFILE" in
  # amd64:debug) BUILD_DIR="build/debug-amd64" ;;
  # amd64:release) BUILD_DIR="build/release-amd64" ;;
  # arm64:debug) BUILD_DIR="build/debug-arm64" ;;
  # arm64:release) BUILD_DIR="build/release-arm64" ;;
  amd64:debug) BUILD_DIR="build/debug" ;;
  amd64:release) BUILD_DIR="build/release" ;;
  arm64:debug) BUILD_DIR="build/debug" ;;
  arm64:release) BUILD_DIR="build/release" ;;
  *) echo "unknown profile: $PROFILE" >&2; exit 2 ;;
esac

SOURCE="$BUILD_DIR/compile_commands.json"
TARGET="build/compile_commands.json"

if [[ ! -f "$SOURCE" ]]; then
  echo "missing compile commands: $SOURCE" >&2
  exit 1
fi

mkdir -p build
rm -f "$TARGET"
python3 - "$SOURCE" "$TARGET" "$CONTAINER_WORKDIR" "$HOST_WORKDIR" <<'PY'
import json
import sys
from pathlib import Path

source = Path(sys.argv[1])
target = Path(sys.argv[2])
container_root = sys.argv[3].rstrip("/")
host_root = sys.argv[4].rstrip("/")

data = json.loads(source.read_text())
for item in data:
    for key in ("directory", "command", "file", "output"):
        value = item.get(key)
        if isinstance(value, str):
            item[key] = value.replace(container_root, host_root)

target.write_text(json.dumps(data, indent=2) + "\n")
PY

if [[ -L compile_commands.json ]]; then
  rm -f compile_commands.json
elif [[ -e compile_commands.json ]]; then
  echo "unexpected root compile_commands.json: remove it before syncing" >&2
  exit 1
fi

echo "synced compile_commands.json for host: $HOST_WORKDIR"
