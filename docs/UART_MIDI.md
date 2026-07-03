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

#### Circuit MIDI OUT standard (boucle de courant, resistances)

Le circuit "classique" ne necessite aucun composant actif cote emetteur : la
boucle de courant est fermee par l’**opto-coupleur du recepteur** (l’autre
appareil), pas par l’ESP32. TX pilote juste un transistor logique interne au
MCU qui tire la ligne a la masse.

```text
                 +5V (ou +3V3, voir note)
                   |
                  [R1]  220 ohm
                   |
   DIN-5 pin 4 o---+---------------------o  vers MIDI IN de l’appareil
                                             (+5V via R1, alimente la LED
                                              de l’opto-coupleur recepteur)

   ESP32 TX ---[R2]--- DIN-5 pin 5 o------o  vers MIDI IN de l’appareil
              220 ohm                        (cathode LED opto recepteur)

   DIN-5 pin 2 : blindage / GND (relie au GND si cable blinde ; sinon NC)
```

- **Pin 4** (+5 V via R1 = 220 Ω) et **pin 5** (signal, via R2 = 220 Ω depuis
  TX) sont les deux seules broches actives d’une sortie MIDI DIN-5 standard.
  Pins 1 et 3 ne sont pas utilisees ; pin 2 = masse/blindage (optionnelle).
- Fonctionnement : TX au repos = **HIGH** -> pas de courant. Un bit de start
  (TX -> LOW) tire le courant a travers R1, la LED de l’opto du recepteur,
  R2, jusqu’a TX -> ~5 mA en boucle, conforme a la spec MIDI 1.0.
- **Sortie TRS (type A/B)** : meme principe, brochage different (voir specs
  MIDI Association "TRS MIDI" — le point important reste R1/R2 = 220 Ω et le
  sens du courant).

**Alimentation R1 : 5 V vs 3,3 V**

- **5 V** (valeurs ci-dessus) : conforme au calcul de reference de la norme
  MIDI (~5 mA), compatible avec le plus grand nombre de recepteurs, y compris
  les vieux opto-coupleurs peu sensibles (6N138 d’origine, etc.). A privilegier
  si la carte expose un rail 5 V (ex. `5V`/`VUSB` sur beaucoup de cartes ESP32,
  disponible seulement si alimentation par USB).
- **3,3 V uniquement** (pas de rail 5 V disponible) : reduire R1+R2 pour
  conserver un courant suffisant, p. ex. **R1 = R2 = 100 Ω** (~5 mA sous 3,3 V
  en tenant compte de la chute de tension de la LED). Fonctionne avec la
  plupart des opto-coupleurs modernes et rapides (6N137, PC900, TLP2361...)
  mais moins garanti avec du materiel MIDI ancien/exotique — **tester** avec
  le materiel cible avant de figer la valeur.
- Dans tous les cas : **resistances en serie obligatoires**, ne jamais relier
  TX directement a la broche signal sans R2 (destruction possible du GPIO ou
  hors-norme en courant).

#### Circuit MIDI IN (recepteur, cote ESP32) — si RX cable

- Symetrique : c’est ici que l’**opto-coupleur** (6N138 ou equivalent rapide,
  ex. 6N137/HCPL-0631 pour un slew rate correct a 31250 baud) isole
  galvaniquement l’entree.
- Cablage type : DIN-5 pin 4 -> resistance courante (220 Ω, cote emetteur
  distant) -> pin 5 -> travers la LED de l’opto (avec une diode de protection
  anti-inversion en serie, ex. 1N4148) -> pin 2 (masse). Le phototransistor de
  l’opto ferme le circuit RX cote ESP32 (souvent avec pull-up + inverseur
  logique selon le composant).
- Ne pas improviser sans schema de reference : reprendre un circuit MIDI IN
  publie (ex. specs MIDI Association, ou circuits Arduino MIDI Shield connus)
  plutot que de recalculer les valeurs a la main.

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
