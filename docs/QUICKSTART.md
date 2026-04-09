# Quickstart

Ce guide decrit une integration rapide de `nidmi-core` dans une application ESP32.

## 1. Ajouter la dependance

Choisir une methode:

- submodule Git dans le projet applicatif
- dependance PlatformIO/Arduino pointee sur un tag `nidmi-core`

Toujours pinner une version (`vX.Y.Z`), pas `main`.

## 2. Initialiser le routeur MIDI

```cpp
#include "MidiRouter.h"

MidiRouter g_router;

void setup() {
  CoreConfig cfg;
  cfg.enableRtpMidi = true;
  cfg.enableBleMidi = false;
  cfg.enableUsbMidi = true;
  cfg.enableUartMidi = true;
  cfg.uartMidi.enable = true;
  cfg.uartMidi.uartIndex = 1;
  cfg.uartMidi.txPin = 17;   // exemple: adapter a ta carte
  cfg.uartMidi.rxPin = 16;   // -1 si emission seule
  cfg.sessionName = "MyDevice";

  g_router.begin(cfg);
}
```

Details UART (31250 baud, cablage): `docs/UART_MIDI.md`.

## 3. Mettre a jour dans la boucle

```cpp
void loop() {
  g_router.update();

  // logique metier
  // ...
}
```

## 4. Envoyer des messages MIDI

```cpp
g_router.sendNoteOn(1, 60, 100);
g_router.sendNoteOff(1, 60, 0);
g_router.sendControlChange(1, 1, 64);
```

## 5. Bonnes pratiques

- garder `update()` tres frequente
- eviter les appels bloquants dans `loop()`
- centraliser l'envoi MIDI via une seule instance `MidiSender`

## 6. Depannage rapide

- aucun message ne sort:
  - verifier `CoreConfig` (transport active)
  - verifier readiness via `getStatus()`
- comportement instable:
  - reduire logs verbeux
  - verifier charge CPU et delais de boucle
