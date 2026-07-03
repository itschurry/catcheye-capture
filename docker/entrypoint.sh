#!/bin/bash
set -e

echo "Starting entrypoint script..."
echo "Build type: $BUILD_TYPE"

for device in /dev/ttyUSB* /dev/ttyACM* /dev/pcanusb* /dev/input/js*; do
  if [ -e "$device" ]; then
    sudo chmod 666 "$device"
    echo "$device"
  fi
done

# ROS 환경 설정
source ~/ros_ws/ros_settings.sh
# battery can setup
# ros2 run battery_can can_setup.sh can0 250000

exec "$@"