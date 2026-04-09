# nidmi-core

`nidmi-core` est la couche commune pour les projets de l'ecosysteme NiDMI.

Objectif: factoriser l'infrastructure MIDI et reseau deja stable, afin que plusieurs applications partagent la meme base technique sans dupliquer le code.

## Vision

`nidmi-core` doit rester une bibliotheque:

- petite (API compacte)
- stable (contrats clairs, versionnee semantiquement)
- portable (ESP32-C3/S3 en priorite)
- neutre applicativement (pas de logique "capteurs" ni "lecteur .mid" integre)

## Projets cibles

- `NiDMI`: application capteurs/actuateurs -> MIDI temps reel
- `NiDMI-Player`: lecteur de fichiers MIDI (`.mid`) -> sorties MIDI temps reel
- **Lecteurs de sons multi-ESP** (optionnel): chaque appareil joue l'audio avec sa propre stack (decodeur + I2S) ; `nidmi-core` sert a la **sync / controle** (ex. RTP-MIDI clock, start/stop, ou conventions OSC dans l'app). Voir `docs/USE_CASE_AUDIO_SYNC.md`.

`NiDMI` et `NiDMI-Player` dependent de `nidmi-core` pour les transports MIDI et les services reseau de base. Un **lecteur de sons** peut aussi dependre du core pour la **sync** (MIDI reseau ou UART), en ajoutant sa propre stack **audio** (decodeur, I2S).

## Ce que contient `nidmi-core`

- Interface d'emission MIDI (`MidiSender`)
- Routage multi-sorties (`MidiRouter`)
- Transports MIDI supportes:
  - RTP-MIDI (AppleMIDI)
  - BLE MIDI
  - USB MIDI (selon carte/cible)
  - **MIDI serie (UART RX/TX, 31250 baud)** — DIN / TRS vers materiels classiques
- **OSC UDP** (CNMAT) : `OscUdpService` — envoi / reception, modes interface (AP / STA / les deux), aligne sur les usages type NiDMI (unicast / broadcast). Voir `docs/OSC_PLATFORMIO.md` pour PlatformIO (script excluant le SLIP Bluetooth de la lib sur ESP32-S3).
- **Pont RTP-MIDI ↔ OSC** : `MidiOscRouter` — adresses du type `{prefix}/midi/noteon`, `noteoff`, `cc` avec trois entiers `(data1, data2, canal)` pour rester compatible avec les conventions NiDMI sans dependre du depot NiDMI.
- Services systeme transverses:
  - configuration runtime des transports
  - boucle `update()` non bloquante
  - hooks/callbacks de reception MIDI (optionnels ; `RtpMidiService` utilise des callbacks statiques vers une instance active pour la MIDI Library)
- **Plateforme ESP32-S3** : `nidmi_platform::Esp32UsbCdc` — init du **CDC** (port serie USB TinyUSB), separe du MIDI USB ; voir `docs/USB_CDC.md`.

## Ce que `nidmi-core` ne contient pas

- decodeur / lecture **audio** (PCM, fichiers sons, I2S) — voir `docs/USE_CASE_AUDIO_SYNC.md` pour combiner un lecteur avec ce core
- gestion des composants capteurs (`ComponentManager`, processors, registry UI)
- logique de lecture/parsing de fichiers MIDI (SMF)
- UI metier (config capteurs, transport player, browser de fichiers)

## Documentation

- Demarrage rapide: `docs/QUICKSTART.md`
- Architecture: `docs/ARCHITECTURE.md`
- USB CDC (ESP32-S3): `docs/USB_CDC.md`
- MIDI UART (RX/TX): `docs/UART_MIDI.md`
- Lecteur de sons + sync multi-ESP: `docs/USE_CASE_AUDIO_SYNC.md`
- OSC + PlatformIO (ESP32-S3): `docs/OSC_PLATFORMIO.md`
- Contrat API: `docs/API.md`
- Migration depuis `NiDMI`: `docs/MIGRATION_FROM_NIDMI.md`
- Versioning et releases: `docs/RELEASES.md`
- Roadmap: `docs/ROADMAP.md`

## Structure recommandee

```text
nidmi-core/
  README.md
  library.properties
  src/nidmi_core/
    Version.h
    Version.cpp
    NetBootstrap.h
    NetBootstrap.cpp
    RtpMidiService.h
    RtpMidiService.cpp
    OscUdpService.h / .cpp
    MidiOscRouter.h / .cpp
    UartMidiTransport.h
  docs/
    QUICKSTART.md
    ARCHITECTURE.md
    UART_MIDI.md
    USE_CASE_AUDIO_SYNC.md
    API.md
    MIGRATION_FROM_NIDMI.md
    RELEASES.md
    ROADMAP.md
  src/
    midi/
    network/
    platform/
  examples/
```

## Principes de conception

- API orientee interfaces (`MidiSender`) pour separer "quoi envoyer" de "comment transporter"
- aucune allocation dynamique dans les chemins critiques temps reel si possible
- appels non bloquants dans `loop()`
- responsabilites strictement separees entre:
  - `core transport`
  - `application metier`

## Statut

Ce depot est initialise avec la documentation fondatrice. La migration du code existant depuis `NiDMI` est decrite dans `docs/MIGRATION_FROM_NIDMI.md`.

## Test materiel (sans toucher NiDMI)

Le projet **`nidmi-player`** (`../nidmi-player`) utilise ce depot : **point d’acces WiFi + mDNS + RTP-MIDI** (`RtpMidiService`, `netBeginSoftAp`) et envoi de notes de test. Voir le README du player pour SSID / mot de passe.
