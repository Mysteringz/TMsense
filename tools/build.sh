#!/usr/bin/env bash
# Build (and optionally flash) TMnode with arduino-cli, no PlatformIO needed.
#   tools/build.sh                 build
#   tools/build.sh upload [port]   build and flash (default /dev/cu.usbserial-0001)
#
# arduino-cli wants a flat sketch folder, so src/ and include/ are copied into
# build/sketch/TMnode/ on every run. That copy is disposable -- never edit it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FQBN="esp32:esp32:heltec_wifi_lora_32_V3"
SKETCH="$ROOT/build/sketch/TMnode"
OUT="$ROOT/build/out"
rm -rf "$SKETCH" && mkdir -p "$SKETCH" "$OUT"
cp "$ROOT"/src/*.cpp "$ROOT"/include/*.h "$SKETCH/"
[ -f "$ROOT/include/node_config.h" ] || echo "note: no include/node_config.h - node boots unprovisioned (configure over serial)"
mv "$SKETCH/main.cpp" "$SKETCH/TMnode.ino"
arduino-cli compile --fqbn "$FQBN" --warnings default --output-dir "$OUT" "$SKETCH"
if [ "${1:-}" = "upload" ]; then
  arduino-cli upload --fqbn "$FQBN" -p "${2:-/dev/cu.usbserial-0001}" --input-dir "$OUT"
fi
