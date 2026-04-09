# Migration depuis `NiDMI` vers `nidmi-core`

Ce plan vise une extraction progressive sans regression fonctionnelle.

## Strategie

Approche en 2 phases:

1. extraire le minimum viable (`MidiSender`, `MidiRouter`, transports)
2. stabiliser l'API puis brancher `NiDMI-Player`

Ne pas extraire "tout d'un coup".

## Perimetre initial a extraire

- `src/midi/MidiSender.h`
- `src/midi/MidiRouter.*`
- classes de transport associees:
  - RTP-MIDI
  - Bluetooth MIDI
  - USB MIDI
  - **UART / serie 31250** (`UartMidiTransport`) — a ajouter dans `nidmi-core` (pas encore dans NiDMI), puis brancher dans `MidiRouter`
- portions de `ServerCore` strictement necessaires aux transports (si dependance forte)

## Perimetre a laisser dans `NiDMI`

- `ComponentManager`
- `ComponentRegistry`
- `ProcessorRegistry`
- logique pin/GPIO/mux capteurs
- API web metier capteurs

## Etapes detaillees

## Etape 1 - Baseline

- figer un commit fonctionnel de `NiDMI` (reference de comparaison)
- definir des scenarios de test manuels:
  - Note On/Off via RTP
  - CC via BLE
  - envoi USB sur cible compatible

## Etape 2 - Extraction minimale

- copier les fichiers cibles dans `nidmi-core`
- corriger includes et paths
- creer une facade publique (`include/nidmi_core/...`)

Livrable: `nidmi-core` compile seul.

## Etape 3 - Rebranchement de `NiDMI`

- remplacer les includes locaux par includes `nidmi-core`
- remplacer les instances internes par la facade core
- valider que le comportement reste identique

Livrable: `NiDMI` fonctionne sans changement fonctionnel visible.

## Etape 4 - Stabilisation API

- geler les signatures publiques
- documenter les invariants et limites
- tagger `nidmi-core` en `v0.1.0`

## Etape 5 - Bootstrap `NiDMI-Player`

- creer app minimale qui depend de `nidmi-core`
- envoyer un jeu de messages de test sans parser `.mid`
- valider RTP/BLE/USB

## Etape 6 - Ajout moteur player

- parser SMF
- scheduler timing
- transport controls
- UI dediee player

## Checklist anti-regression

- [ ] Build `NiDMI` OK
- [ ] Build `NiDMI-Player` OK
- [ ] NoteOn/NoteOff conformes sur les 3 transports (si actifs)
- [ ] Clock/Start/Stop/Continue non regresses
- [ ] pas de blocage dans `loop()`/task

## Risques connus

- dependances circulaires historiques entre `ServerCore` et `MidiRouter`
- divergences de comportement entre cartes (C3 vs S3, USB natif)
- code conditionnel compile-time fragile

## Mitigations

- introduire des interfaces de transport fines
- isoler les macros hardware dans une couche `platform`
- tester d'abord RTP (chemin le plus stable), puis BLE/USB
