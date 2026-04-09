# MIDI serie (UART RX / TX) dans `nidmi-core`

## Objectif

Exposer le MIDI **physique** (connecteur DIN 5 broches ou sortie 3,5 mm TRS Type A/B) via **UART** sur l’ESP32, en complement des transports deja prevus (RTP-MIDI, BLE, USB).

Ce transport est **obligatoire** pour beaucoup de materiels (syntheseurs, boites a rythmes, interfaces) qui n’ont que du MIDI filaire.

## Norme et debit

- Debit standard MIDI: **31250 baud**, **8N1** (8 bits, pas de parite, 1 bit stop).
- Protocole: octets MIDI tels qu’emis par `MidiSender` (messages complets, pas de SysEx dans la version minimale si non specifie).

## Role dans l’architecture

`UartMidiTransport` est un **transport de plus** dans le fan-out du `MidiRouter` (meme contrat que RTP/BLE/USB):

- `begin(UartMidiConfig)`
- `update()` (drain entree si reception activee)
- `sendXxx(...)` (emission sur TX)

NiDMI et NiDMI-Player n’implementent pas le filtrage metier ici: ils appellent uniquement `MidiSender`.

## Configuration materielle (ESP32)

### Sortie (TX)

- Broche **TX** du UART choisi vers l’entree MIDI de l’equipement (souvent via **convertisseur de niveau** 3,3 V -> courant conforme MIDI; schema classique avec optocoupleur cote **reception** de l’autre appareil, ici on est emetteur).
- Respecter le cablage de la fiche DIN (pin 5 = courant, pin 4 = +5 V via resistance, etc.) selon ton interface.

### Entree (RX) — optionnelle

- Broche **RX** pour recevoir du MIDI entrant (sync, notes pour LEDs, etc.).
- Cote ESP32, l’entree MIDI standard utilise souvent un **opto-coupleur** (6N138, etc.) pour isoler galvaniquement.

### Choix du UART

- `Serial`, `Serial1`, `Serial2` selon carte et broches libres.
- Eviter les conflits avec la console USB (`Serial` sur certaines cartes), le flash, ou d’autres peripheriques.

### Debouncing / charge

- Ne pas surcharger le buffer: `update()` doit lire `available()` sans bloquer longtemps.

## API prevue (contrat)

Voir `docs/API.md` (`UartMidiConfig`, `enableUartMidi`, `setUartMidiEnabled`).

Encapsulation recommandee:

```text
src/midi/UartMidiTransport.cpp
  - encodage des messages MIDI en octets (running status gere avec prudence)
  - ecriture non bloquante sur `HardwareSerial`
```

## Reception MIDI (IN)

Si `rxPin` est configure:

- Parser les octets entrants (machine a etats MIDI 1.0).
- Option A: callbacks utilisateur (`MidiInputCallbacks`) branches sur le routeur.
- Option B: file legere d’evenements consommee par l’application.

SysEx: soit ignore dans v1, soit buffer limite avec garde anti-depassement.

## Limites et bonnes pratiques

- **Pas de melange** de logique capteur dans ce module: uniquement transport.
- Tester **TX seul** avant RX (moins de risques de cablage).
- Documenter les broches dans la config NVS ou JSON cote application.

## Integration NiDMI (plus tard)

Lors du refactor vers `nidmi-core`, ajouter dans `MidiRouter` les memes appels `sendXxx` que pour RTP/BLE/USB lorsque `uartEnabled` est vrai, et appeler `uart.update()` dans `MidiRouter::update()`.

## References

- MIDI 1.0 — Electrical Specification (courant de boucle, 31250 baud).
- ESP32 Arduino `HardwareSerial` (`begin(31250, SERIAL_8N1, rxPin, txPin)` selon API).
