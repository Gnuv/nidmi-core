# OSC (CNMAT) et PlatformIO

La dependance **`cnmat/OSC`** inclut des fichiers sources pour Bluetooth serie (`SLIPEncodedBluetoothSerial.cpp`) qui ne compilent pas sur **ESP32-S3** (pas de `BluetoothSerial.h` dans ce contexte).

## Solution

Dans le `platformio.ini` du firmware (ex. `nidmi-player`), ajouter **avant** la compilation :

```ini
[env:...]
extra_scripts =
  pre:../nidmi-core/scripts/pio_osc_disable_bluetooth_slip.py
```

Ajuster le chemin relatif si le depot `nidmi-core` n est pas a cote du projet.

Le script renomme une fois `SLIPEncodedBluetoothSerial.cpp` en `.cpp.disabled` dans le dossier de la lib dans `.pio/libdeps/`.

## NiDMI (futur)

Lorsque NiDMI consommera `nidmi-core` + OSC via PlatformIO, utiliser le meme `extra_scripts` sans modifier le code applicatif NiDMI autrement que la configuration PIO.
