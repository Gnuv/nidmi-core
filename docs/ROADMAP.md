# Roadmap `nidmi-core`

## Phase 0 - Foundation (immediat)

- [ ] extraire et compiler `MidiSender`
- [ ] extraire et compiler `MidiRouter`
- [ ] extraire RTP-MIDI
- [ ] definir structure de dossiers (`include/`, `src/`, `docs/`, `examples/`)

Resultat attendu: `nidmi-core` compilable, API minimale documentee.

## Phase 1 - Stabilisation transports

- [ ] integrer BLE MIDI de maniere modulaire
- [ ] integrer USB MIDI avec detection des cibles supportees
- [ ] **integrer UART MIDI (31250 baud, RX/TX)** — `UartMidiTransport`, voir `docs/UART_MIDI.md`
- [x] **interface reseau USB (CDC-NCM)** — `UsbNetService`, valide sur macOS ; Linux et Windows a faire
- [ ] uniformiser erreurs/retours d'etat (`CoreStatus`)
- [ ] ajouter exemple "send test notes"

Resultat attendu: comportement stable multi-transport.

## Phase 2 - Integration NiDMI

- [ ] brancher `NiDMI` sur `nidmi-core`
- [ ] supprimer la duplication de code transport dans `NiDMI`
- [ ] valider non-regression sur les flows existants
- [ ] tag `v0.1.0`

Resultat attendu: `NiDMI` devient premier consommateur officiel.

## Phase 3 - Integration NiDMI-Player

- [ ] creer squelette `NiDMI-Player`
- [ ] brancher envoi MIDI via `nidmi-core`
- [ ] valider Clock + Start/Stop/Continue
- [ ] tester compatibilite simultanee RTP/BLE/USB

Resultat attendu: base technique prete pour parser `.mid`.

## Phase 4 - Qualite et outillage

- [ ] tests unitaires sur `MidiRouter`
- [ ] tests integration par transport
- [ ] instrumentation debug (compteurs/messages)
- [ ] exemple de callback MIDI IN

Resultat attendu: robustesse et maintenance simplifiee.

## Hors scope `nidmi-core`

Les sujets ci-dessous doivent rester dans les applications:

- parser SMF (`.mid`)
- scheduler de playback
- UI web de transport et browser fichiers
- mapping capteurs -> messages MIDI

## Criteres de "done" v1.0

- API publique stable
- docs API et migration completes
- tests de non-regression sur projets consommateurs
- support valide des transports annonces pour les cibles visees
