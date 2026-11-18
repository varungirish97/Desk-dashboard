#!/usr/bin/env bash
# Build and flash the desk dashboard.
#   ./flash.sh              USB: compile, upload, then open the serial monitor
#   ./flash.sh /dev/cu.X    USB, with a specific port
#   ./flash.sh ota          Wi-Fi: compile and upload to desk-dashboard.local
#   ./flash.sh ota <ip>     Wi-Fi, to a specific IP address
set -e
FQBN="esp32:esp32:esp32:PartitionScheme=min_spiffs"   # ESP32 Dev Module, 1.9 MB app partition (keeps OTA)
cd "$(dirname "$0")"

if [ "$1" = "ota" ]; then
  HOST="${2:-desk-dashboard.local}"
  PASS=$(sed -n 's/^#define OTA_PASSWORD "\(.*\)".*/\1/p' secrets.h)
  ESPOTA=$(ls ~/Library/Arduino15/packages/esp32/hardware/esp32/*/tools/espota.py 2>/dev/null | tail -n1)
  [ -z "$ESPOTA" ] && { echo "espota.py not found (is the esp32 core installed?)"; exit 1; }

  arduino-cli compile --fqbn "$FQBN" --output-dir build .
  echo "Uploading over Wi-Fi to $HOST ..."
  python3 "$ESPOTA" -i "$HOST" -p 3232 ${PASS:+-a "$PASS"} -f build/desk_dashboard.ino.bin -r
  echo "Done. The board restarts with the new firmware."
  exit 0
fi

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
