# API `nidmi-core` (contrat cible)

Ce document decrit le contrat public recommande pour stabiliser l'usage de `nidmi-core`.

## Principes

- API compacte
- noms explicites
- comportement non bloquant
- separer la config, l'initialisation et la boucle d'update

## Types de base

### MIDI serie (UART)

Transport pour connectique **DIN** ou **TRS** via UART materiel ESP32 (debit **31250 baud**, 8N1). Detail materiel et integration: `docs/UART_MIDI.md`.

```cpp
struct UartMidiConfig {
  bool enable = false;
  /** UART Arduino: typiquement 1 ou 2 (eviter le port deja utilise par la console). */
  uint8_t uartIndex = 1;
  /** -1 ou GPIO numerique invalide = broche non utilisee (TX seul ou RX seul). */
  int8_t rxPin = -1;
  int8_t txPin = -1;
  /** Taille buffer lecture (octets), si reception activee. */
  size_t rxBufferSize = 256;
};
```

### Configuration globale du core

```cpp
struct CoreConfig {
  bool enableRtpMidi = true;
  bool enableBleMidi = false;
  bool enableUsbMidi = false;
  /** MIDI filaire 31250 baud sur UART (RX/TX). */
  bool enableUartMidi = false;
  UartMidiConfig uartMidi;

  const char* sessionName = "NiDMI";
  uint16_t rtpPort = 5004;
};

struct CoreStatus {
  bool rtpReady = false;
  bool bleReady = false;
  bool usbReady = false;
  /** Pret si UART configure et `begin()` reussi. */
  bool uartReady = false;
};
```

## Interface `MidiSender`

```cpp
class MidiSender {
public:
  virtual ~MidiSender() = default;

  virtual void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) = 0;
  virtual void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) = 0;
  virtual void sendControlChange(uint8_t channel, uint8_t cc, uint8_t value) = 0;
  virtual void sendProgramChange(uint8_t channel, uint8_t program) = 0;
  virtual void sendPitchBend(uint8_t channel, int16_t bend) = 0;
  virtual void sendAfterTouch(uint8_t channel, uint8_t pressure) = 0;
  virtual void sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) = 0;

  virtual void sendClock() = 0;
  virtual void sendStart() = 0;
  virtual void sendStop() = 0;
  virtual void sendContinue() = 0;
};
```

## Classe `MidiRouter`

```cpp
class MidiRouter final : public MidiSender {
public:
  bool begin(const CoreConfig& config);
  void update();

  void setRtpEnabled(bool enabled);
  void setBleEnabled(bool enabled);
  void setUsbEnabled(bool enabled);
  void setUartEnabled(bool enabled);
  /** Reconfiguration UART (pins, index); peut necessiter un `begin()` ulterieur. */
  void setUartConfig(const UartMidiConfig& cfg);

  CoreStatus getStatus() const;

  // Impl. MidiSender
  void sendNoteOn(uint8_t ch, uint8_t note, uint8_t vel) override;
  void sendNoteOff(uint8_t ch, uint8_t note, uint8_t vel) override;
  void sendControlChange(uint8_t ch, uint8_t cc, uint8_t val) override;
  void sendProgramChange(uint8_t ch, uint8_t program) override;
  void sendPitchBend(uint8_t ch, int16_t bend) override;
  void sendAfterTouch(uint8_t ch, uint8_t pressure) override;
  void sendKeyPressure(uint8_t ch, uint8_t note, uint8_t pressure) override;
  void sendClock() override;
  void sendStart() override;
  void sendStop() override;
  void sendContinue() override;
};
```

## Contrats de comportement

- `begin(...)` initialise les transports actives et retourne `false` en cas d'echec global critique.
- `update()` doit etre appele frequemment depuis la boucle principale ou une task dediee (inclut `UartMidiTransport::update()` pour lire le RX si configure).
- Les `sendXxx(...)` ignorent silencieusement les transports desactives/non prets.
- Aucun `sendXxx(...)` ne doit faire de blocage long.
- **UART**: si `enableUartMidi` est vrai mais pins invalides pour l’ESP32 cible, `uartReady` reste `false` et l’emission sur UART est ignoree (comportement defini, pas d’undefined behavior).

## Validation des parametres

Recommandations:

- `channel` accepte `1..16` (ou `0..15` en interne, mais contrat externe coherent et documente)
- clamping explicite pour les valeurs MIDI hors bornes
- `pitch bend` borne a `[-8192, 8191]`

## Hooks de reception (optionnel)

Pour les apps qui ont besoin d'entree MIDI:

```cpp
struct MidiInputCallbacks {
  void (*onNoteOn)(uint8_t ch, uint8_t note, uint8_t vel) = nullptr;
  void (*onNoteOff)(uint8_t ch, uint8_t note, uint8_t vel) = nullptr;
  void (*onControlChange)(uint8_t ch, uint8_t cc, uint8_t val) = nullptr;
};
```

`MidiRouter` peut exposer `setInputCallbacks(const MidiInputCallbacks&)`.

## Exemple d'usage (NiDMI)

```cpp
MidiRouter router;
CoreConfig cfg;
cfg.enableRtpMidi = true;
cfg.enableBleMidi = true;
cfg.enableUsbMidi = true;
cfg.sessionName = "NiDMI";

router.begin(cfg);

// Dans le manager capteurs
router.sendControlChange(1, 74, sensorValue);
```

## Exemple d'usage (NiDMI-Player)

```cpp
MidiRouter router;
router.begin(CoreConfig{});

// Dans l'engine de lecture
if (event.type == MidiEventType::NoteOn) {
  router.sendNoteOn(event.channel, event.note, event.velocity);
}
```

## Stabilite API

- Tout changement de signature publique -> version majeure
- Ajout non cassant (nouvelle methode avec valeur par defaut) -> mineure
- Correctifs internes sans impact API -> patch
