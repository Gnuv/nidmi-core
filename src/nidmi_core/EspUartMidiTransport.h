/**
 * Implementation ESP32 (Arduino HardwareSerial) du contrat UartMidiTransport.
 * MIDI physique (DIN 5 broches ou TRS type A/B) en sortie sur une paire de
 * broches UART materiel. Voir docs/UART_MIDI.md pour le cablage (niveau
 * courant/optocoupleur cote recepteur).
 */
#pragma once

#include "UartMidiTransport.h"
#include <HardwareSerial.h>

namespace nidmi_core {

class EspUartMidiTransport : public UartMidiTransport {
public:
  EspUartMidiTransport() = default;

  bool begin(const UartMidiConfig& cfg) override;
  void update() override;
  bool isReady() const override { return ready_; }

  void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) override;
  void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override;
  void sendControlChange(uint8_t channel, uint8_t control, uint8_t value) override;
  void sendProgramChange(uint8_t channel, uint8_t program) override;
  void sendPitchBend(uint8_t channel, int bend) override;
  void sendAftertouch(uint8_t channel, uint8_t pressure) override;
  void sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) override;
  void sendClock() override;
  void sendStart() override;
  void sendStop() override;
  void sendContinue() override;

private:
  HardwareSerial* serial_ = nullptr;
  bool ready_ = false;
  bool rxEnabled_ = false;

  void writeStatusData(uint8_t status, uint8_t channel, uint8_t d1, uint8_t d2, uint8_t len);
};

}  // namespace nidmi_core
