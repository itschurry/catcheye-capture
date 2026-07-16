#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CATCHEYE_CAPTURE_PATH="$(cd -- "$SCRIPT_DIR/.." && pwd)"
CAPTURE_DIR="${CATCHEYE_CAPTURE_DIR:-/home/user/catcheye-capture/captures}"
RECORDING_DIR="${CATCHEYE_CAPTURE_RECORDING_DIR:-/home/user/catcheye-capture/recordings}"
CAMERA_PROPERTIES="${CATCHEYE_CAPTURE_CAMERA_PROPERTIES:-$CATCHEYE_CAPTURE_PATH/config/camera_properties.json}"
GPIO_CHIP="${CATCHEYE_CAPTURE_GPIO_CHIP:-/dev/gpiochip4}"
TRIGGER_GPIO="${CATCHEYE_CAPTURE_TRIGGER_GPIO:-23}"
COMPLETE_GPIO="${CATCHEYE_CAPTURE_COMPLETE_GPIO:-24}"
HEARTBEAT_LED_GPIO="${CATCHEYE_CAPTURE_HEARTBEAT_LED_GPIO:-13}"

exec "$CATCHEYE_CAPTURE_PATH/bin/catcheye-capture" \
  --camera \
  --camera-pipeline "libcamerasrc ! video/x-raw,width=2304,height=1296,framerate=15/1,format=NV12 ! queue leaky=downstream max-size-buffers=1 ! videoflip method=vertical-flip" \
  --ws \
  --gpio-chip "$GPIO_CHIP" \
  --trigger-gpio "$TRIGGER_GPIO" \
  --complete-gpio "$COMPLETE_GPIO" \
  --complete-pulse-ms 1000 \
  --heartbeat-led-gpio "$HEARTBEAT_LED_GPIO" \
  --capture-dir "$CAPTURE_DIR" \
  --recording-dir "$RECORDING_DIR" \
  --camera-properties "$CAMERA_PROPERTIES"
