# USB CDC (port série sur câble USB) — `nidmi_platform`

## Rôle

La couche **`nidmi::platform::Esp32UsbCdc`** centralise l’initialisation du **CDC** (Communications Device Class) sur **ESP32-S3** lorsque le firmware utilise **USB-OTG + TinyUSB** avec un profil où l’application contrôle `USB.begin()` (typiquement **`cdc_on_boot=0`** dans le script / menu Arduino).

Ce module ne gère **pas** le MIDI USB : il ne crée pas d’interface `USBMIDI`. Il évite en revanche de disperser des `USBCDC` / `USB.begin()` dans plusieurs classes sans règle commune.

## Contrat

1. **Un seul propriétaire** de `USB.begin()` pour un profil USB donné au démarrage (composite CDC+MIDI est une autre décision, prise au niveau application ou futur `UsbMidiTransport`).
2. **`Esp32UsbCdc::begin()`** est **idempotent** : plusieurs appels sont sans effet après le premier succès.
3. Les profils suivants sont reconnus au **compile-time** :
   - **HWCDC** (`ARDUINO_USB_MODE` / JTAG) : pas de TinyUSB pour la console ; `begin()` réussit sans appeler `USB.begin()` TinyUSB.
   - **`cdc_on_boot=1`** : TinyUSB déjà démarré par le framework ; `begin()` ne rappelle pas `USB.begin()`.
   - **USB-OTG TinyUSB manuel** : enregistrement `USBCDC`, `USB.productName()`, puis `USB.begin()`.

## Intégration

```cpp
#include <nidmi_platform/Esp32UsbCdc.h>

void setup() {
  nidmi::platform::Esp32UsbCdc cdc;
  nidmi::platform::UsbCdcConfig cfg;
  cfg.productName = "mon-app";
  if (nidmi::platform::Esp32UsbCdc::isTargetSupported()) {
    cdc.begin(cfg);
  }
}
```

Le chemin d’include suit la convention Arduino de cette lib : `#include <nidmi_platform/Esp32UsbCdc.h>` une fois le dossier `src/nidmi_platform` exposé par la bibliothèque (voir `library.properties`).

## Build

- Cible : **esp32** (S3 pour le chemin CDC TinyUSB).
- **`CONFIG_TINYUSB_CDC_ENABLED`** : si désactivé, `begin()` retourne `false` (pas de pile CDC TinyUSB disponible).

Voir aussi `docs/ARCHITECTURE.md` (couche Platform).
