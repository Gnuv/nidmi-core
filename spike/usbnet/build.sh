#!/usr/bin/env bash
# Compile (et flashe) le spike USB NCM. Cible S3 uniquement, usb_mode=0.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKETCH="$HERE/usbnet_spike"
OUT="$HERE/build"
FQBN="esp32:esp32:XIAO_ESP32S3:PSRAM=opi"

# usb_mode=0 -> USB-OTG/TinyUSB (requis pour MIDI et NCM)
# cdc_on_boot=0 -> pas de CDC-ACM : on garde les FIFO IN pour MIDI + NCM
PROPS=(
  --build-property "build.usb_mode=0"
  --build-property "build.cdc_on_boot=0"
)

case "${1:-compile}" in
  compile)
    arduino-cli compile --fqbn "$FQBN" "${PROPS[@]}" --warnings default \
      --output-dir "$OUT" "$SKETCH"
    echo
    echo "binaire : $OUT/usbnet_spike.ino.bin"
    ;;
  upload)
    PORT="${2:-}"
    if [ -z "$PORT" ]; then
      echo "usage: $0 upload /dev/cu.usbmodemXXXX" >&2
      echo "ports disponibles :" >&2
      arduino-cli board list >&2
      exit 1
    fi
    arduino-cli compile --fqbn "$FQBN" "${PROPS[@]}" --output-dir "$OUT" "$SKETCH"
    # --input-dir : reflashe exactement ce qui vient d'etre compile, sans
    # relancer une seconde compilation.
    arduino-cli upload --fqbn "$FQBN" -p "$PORT" --input-dir "$OUT" "$SKETCH"
    ;;
  flash)
    # Reflashe le dernier binaire sans recompiler.
    PORT="${2:-}"
    if [ -z "$PORT" ]; then
      echo "usage: $0 flash /dev/cu.usbmodemXXXX" >&2
      exit 1
    fi
    arduino-cli upload --fqbn "$FQBN" -p "$PORT" --input-dir "$OUT" "$SKETCH"
    ;;
  *)
    echo "usage: $0 [compile|upload <port>|flash <port>]" >&2
    exit 1
    ;;
esac
