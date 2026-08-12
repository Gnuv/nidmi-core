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

ESPTOOL="$HOME/Library/Arduino15/packages/esp32/tools/esptool_py/4.5.1/esptool"

usb_product() { ioreg -r -c IOUSBHostDevice -w 0 2>/dev/null | sed -n 's/.*"USB Product Name" = "\(.*\)"/\1/p' | head -1; }
any_port()    { ls /dev/cu.usbmodem* 2>/dev/null | head -1; }
in_rom()      { [ "$(usb_product)" = "USB JTAG_serial debug unit" ]; }

# Amene la carte en mode download sans toucher au bouton BOOT.
#
# Le firmware doit embarquer un CDC (NIDMI_CDC=1) : USBCDC::_onLineState()
# appelle alors usb_persist_restart(RESTART_BOOTLOADER) sur le motif DTR/RTS.
# Le piege est qu'en repartant, la carte enumere sa ROM USB-Serial-JTAG sous un
# NOM DE PORT DIFFERENT — esptool tient l'ancien et echoue sur "Device not
# configured". On declenche donc le reset, puis on attend le nouveau port.
enter_download() {
  if in_rom; then
    echo "deja en mode download."
    return 0
  fi
  local port; port="$(any_port)"
  if [ -z "$port" ]; then
    echo "Aucun port serie : le firmware actuel n'a pas de CDC." >&2
    echo "Passer la carte en bootloader a la main (BOOT + rebranchement)," >&2
    echo "puis reflasher avec NIDMI_CDC=1 pour ne plus avoir a le refaire." >&2
    return 1
  fi
  echo "declenchement du reset via $port ..."
  # Echec attendu : le port disparait pendant la sequence de reset.
  "$ESPTOOL" --port "$port" --before default_reset --after no_reset chip_id >/dev/null 2>&1 || true
  for _ in $(seq 1 20); do
    if in_rom && [ -n "$(any_port)" ]; then
      echo "mode download atteint sur $(any_port)"
      return 0
    fi
    sleep 1
  done
  echo "La carte n'est pas passee en mode download." >&2
  return 1
}

do_flash() {
  local port="${1:-}"
  if [ -z "$port" ]; then
    enter_download || exit 1
    port="$(any_port)"
  fi
  arduino-cli upload --fqbn "$FQBN" -p "$port" --input-dir "$OUT" "$SKETCH"
  echo
  echo "Flash termine. Si la carte reste en ROM, debrancher/rebrancher :"
  echo "esptool ne sort pas le S3 du mode download par lui-meme."
}

case "${1:-compile}" in
  compile)
    do_compile --warnings default
    echo
    echo "binaire : $OUT/usbnet_spike.ino.bin"
    ;;
  upload)
    do_compile
    do_flash "${2:-}"
    ;;
  flash)
    # Reflashe le dernier binaire sans recompiler.
    do_flash "${2:-}"
    ;;
  *)
    echo "usage: $0 [compile|upload <port>|flash <port>]" >&2
    exit 1
    ;;
esac
