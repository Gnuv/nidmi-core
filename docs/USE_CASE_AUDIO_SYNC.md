# Cas d'usage : lecteur de sons multi-ESP avec `nidmi-core`

## Positionnement

`nidmi-core` **ne decode pas l'audio** (pas de WAV/MP3, pas d'I2S, pas de buffers PCM).  
En revanche, un **projet de lecteur de sons** sur ESP32 peut **dependre de `nidmi-core`** pour la partie **transport MIDI** et, selon ton architecture, la **synchronisation** entre plusieurs appareils sur le reseau.

## Ce que le lecteur audio fait lui-meme

Sur chaque ESP :

- lecture de fichiers ou flux (SD, LittleFS, reseau)
- decodeur adapte (selon format)
- sortie **I2S** / DAC vers ampli ou HP

Cette pile reste **hors** `nidmi-core` (autre bibliotheque ou code applicatif).

## Ce que `nidmi-core` apporte dans ce scenario

- **RTP-MIDI** : emettre ou recevoir **MIDI Clock**, **Start / Stop / Continue**, notes/CC si besoin — utile pour aligner des **sequences** ou des **declenchements** entre ESP.
- **UART MIDI** : relier du materiel classique (DIN/TRS) en parallele.
- **BLE / USB MIDI** : selon le produit.

L'application **combine** deux couches :

```text
[ Moteur audio local ]     <- fichiers, I2S, volume
        +
[ nidmi-core :: MidiRouter ]  <- sync reseau / MIDI filaire
```

## Synchronisation multi-ESP

Approches courantes :

1. **Un ESP maitre** envoie l'**horloge MIDI** (et eventuellement Start/Stop) ; les autres **esclaves** avancent leur logique de lecture (ou declenchent des samples) au rythme des ticks.
2. **OSC** (si integre dans ton app ou une couche voisine) : messages applicatifs (`/play`, `/bpm`, timestamps) — le **contrat** entre appareils est a definir dans ton projet ; ce n'est pas standardise comme le MIDI Clock.

Limites a anticiper : **jitter WiFi**, precision au **tick MIDI** (pas a l'echantillon audio). Pour beaucoup de usages concert / installation, c'est suffisant.

## Resume

| Role | Outil |
|------|--------|
| Son (PCM) | Stack audio dediee par projet |
| Sync / controle reseau ou MIDI filaire | `nidmi-core` |

Un **lecteur de sons multi-ESP synchro** est donc un **cas d'usage valide** : `nidmi-core` comme dependance pour la **couche MIDI/transports**, pas comme lecteur audio tout-en-un.
