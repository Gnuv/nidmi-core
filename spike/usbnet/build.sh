#!/usr/bin/env bash
# Compile (et flashe) le spike USB NCM. Cible S3 uniquement, usb_mode=0.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKETCH="$HERE/usbnet_spike"
LIB="$(cd "$HERE/../.." && pwd)"   # le banc exerce nidmi-core, pas une copie
OUT="$HERE/build"
FQBN="esp32:esp32:XIAO_ESP32S3:PSRAM=opi"

# usb_mode=0 -> USB-OTG/TinyUSB (requis pour MIDI et NCM)
#
# cdc_on_boot : NIDMI_CDC=1 ajoute un CDC-ACM au composite. Ca tient tout juste
# dans le budget d'endpoints (4 FIFO IN, la limite du S3) et ca apporte deux
# choses decisives pour iterer : une console serie, et surtout l'auto-reset au
# flash (USBCDC::_onLineState -> usb_persist_restart) — plus besoin du bouton
# BOOT. Le mettre a 0 pour valider la config finale sans CDC.
CDC="${NIDMI_CDC:-1}"
PROPS=(
  --build-property "build.usb_mode=0"
  --build-property "build.cdc_on_boot=$CDC"
)
echo "cdc_on_boot=$CDC  (NIDMI_CDC=0 pour compiler sans console ni auto-reset)"

# arduino-cli ne reconstruit pas le core quand seul cdc_on_boot change : on
# obtient un "undefined reference to USBSerial" au link, parce que USBCDC.cpp
# reste celui compile avec l'ancienne valeur. On force --clean uniquement a la
# bascule, pour ne pas payer une reconstruction complete a chaque build.
STAMP="$OUT/.cdc_mode"
mkdir -p "$OUT"
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$CDC" ]; then
  echo "  (bascule de cdc_on_boot -> reconstruction complete du core)"
  PROPS+=(--clean)
fi

# Le temoin n'est ecrit qu'apres une compilation reussie. L'ecrire en amont le
# desynchronisait du cache des que la commande ne compilait pas (flash), et le
# --clean suivant sautait : retour du "undefined reference to USBSerial".
do_compile() {
  arduino-cli compile --fqbn "$FQBN" "${PROPS[@]}" "$@" \
    --library "$LIB" --output-dir "$OUT" "$SKETCH"
  echo "$CDC" > "$STAMP"
}

case "${1:-compile}" in
  compile)
    do_compile --warnings default
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
    do_compile
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
