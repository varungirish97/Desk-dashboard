#!/usr/bin/env bash
# Build, upload and open the serial monitor for this sketch.
# Usage: ./flash.sh            (auto-detects the board's port)
#        ./flash.sh /dev/cu.X  (use a specific port)
set -e
FQBN="esp32:esp32:esp32:PartitionScheme=min_spiffs"   # ESP32 Dev Module, 1.9 MB app partition (keeps OTA)
cd "$(dirname "$0")"

PORT="${1:-$(ls /dev/cu.usbserial-* /dev/cu.wchusbserial* 2>/dev/null | head -n1)}"
if [ -z "$PORT" ]; then
  echo "No board found. Check the USB-C cable (some are power-only) and run: arduino-cli board list"
  exit 1
fi
echo "Using port: $PORT"

arduino-cli compile --fqbn "$FQBN" .
# The CH340 USB chip is unreliable at 921600 baud, so try 460800, then fall back to 115200
if ! arduino-cli upload --fqbn "$FQBN,UploadSpeed=460800" -p "$PORT" .; then
  echo "Upload failed at 460800 baud, retrying at 115200..."
  arduino-cli upload --fqbn "$FQBN,UploadSpeed=115200" -p "$PORT" .
fi
echo "Opening serial monitor (Ctrl+C to exit)..."
arduino-cli monitor -p "$PORT" -c baudrate=115200
