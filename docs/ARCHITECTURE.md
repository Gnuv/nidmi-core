# Architecture `nidmi-core`

## Objectif architectural

Fournir une base commune pour transporter des messages MIDI vers plusieurs sorties, avec une API unique pour les applications.

## Vue d'ensemble

```text
Application (NiDMI / NiDMI-Player)
        |
        v
     MidiSender (interface)
        |
        v
     MidiRouter (fan-out + policy)
      /    |    |    \
     v     v    v     v
 RTP-MIDI BLE USB  UART (RX/TX)
```

## Couches

### 1) Couche Interface

- `MidiSender`
- Contrat unique d'emission:
  - notes
  - control changes
  - program change
  - pitch bend
  - aftertouch
  - transport/clock

Role: permettre a la logique applicative d'emettre du MIDI sans connaitre la pile transport.

### 2) Couche Routage

- `MidiRouter` implemente `MidiSender`
- Duplique les messages vers les transports actives
- Centralise les flags runtime:
  - `rtpEnabled`
  - `bleEnabled`
  - `usbEnabled`
  - `uartEnabled` (MIDI serie 31250 baud sur UART hardware)

Role: etre le "point unique de sortie MIDI".

### 3) Couche Transport

- `RtpMidiTransport`
- `BleMidiTransport`
- `UsbMidiTransport`
- `UartMidiTransport` (MIDI DIN / TRS via **UART** `RX`/`TX`, 31250 baud)

Chaque transport expose un contrat uniforme:

- `begin(config)`
- `update()`
- `isReady()`
- `sendXxx(...)`

Role: encapsuler les details bas niveau de chaque backend.

### 4) Couche Platform

- detection de capacites cible (ex: USB natif ESP32-S3)
- **interface reseau USB** : `UsbNetService` (CDC-NCM) — expose un second netif
  porte par le cable, exclu a la compilation hors ESP32-S3 par
  `SOC_USB_OTG_SUPPORTED`. Voir `docs/USB_NET.md`
- wrappers HAL (si necessaire) pour limiter la dependance directe aux specifics carte
- **OTA firmware** : `OtaUpdate` — ecriture via `Update` (ESP32), sans HTTP ; voir `docs/OTA.md`

Role: isoler les variations hardware et simplifier la portabilite.

## Regles de responsabilite

- `core` ne connait pas les "features metier" (capteurs, parser SMF, UI)
- un transport ne connait pas les autres transports
- `MidiRouter` ne parse pas de fichiers, ne gere pas de stockage
- aucun transport ne doit bloquer la boucle principale

## Threading / temps reel

`nidmi-core` doit rester compatible avec:

- boucle unique (`loop`) sans RTOS dedie
- ou task dediee MIDI (FreeRTOS) selon l'application

Contraintes:

- appels transport rapides et predicibles
- limiter les allocations dans les chemins frequents
- horloge de scheduling laissee a l'application metier (ex: player)

## Erreurs et observabilite

Approche recommandee:

- codes retour booleens simples sur les fonctions critiques
- niveaux de logs optionnels (`ERROR`, `WARN`, `INFO`, `DEBUG`)
- compteurs internes utiles au diagnostic:
  - messages envoyes
  - messages abandonnes
  - etat de readiness par transport

## Frontieres avec les apps

### `NiDMI`

- lit les capteurs
- decide quels messages MIDI emettre
- appelle `MidiSender`

### `NiDMI-Player`

- parse les fichiers `.mid`
- calcule le timing des evenements
- appelle `MidiSender`

### Lecteur de sons multi-ESP (exemple)

- decode et joue l'audio localement (hors `nidmi-core`)
- utilise `nidmi-core` pour RTP-MIDI (clock, transport) et/ou d'autres transports documentes

Voir `docs/USE_CASE_AUDIO_SYNC.md`.

Dans tous ces cas, la couche transport MIDI est `nidmi-core` ; le son PCM reste applicatif.
