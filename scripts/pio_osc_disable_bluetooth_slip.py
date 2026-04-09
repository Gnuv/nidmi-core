# PlatformIO pre-script : la lib OSC CNMAT compile SLIPEncodedBluetoothSerial.cpp qui
# requiert BluetoothSerial (absent sur ESP32-S3). Renomme ce fichier pour l exclure du build.
import glob
import os

Import("env")  # noqa: F821


def _disable_osc_bt_slip():
    root = env.subst("${PROJECT_DIR}")
    pattern = os.path.join(root, ".pio", "libdeps", "*", "OSC", "SLIPEncodedBluetoothSerial.cpp")
    for path in glob.glob(pattern):
        disabled = path + ".disabled"
        if os.path.isfile(path) and not os.path.isfile(disabled):
            os.rename(path, disabled)
            print("[nidmi-core] OSC: desactive SLIPEncodedBluetoothSerial.cpp")


_disable_osc_bt_slip()
