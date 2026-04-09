/**
 * Contrat du transport MIDI serie (UART RX/TX, 31250 baud 8N1).
 * Specification: docs/UART_MIDI.md — implementation Arduino dans src/ (HardwareSerial).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace nidmi {

struct UartMidiConfig {
  bool enable = false;
  uint8_t uartIndex = 1;
  int8_t rxPin = -1;
  int8_t txPin = -1;
  std::size_t rxBufferSize = 256;
};

/**
 * Interface transport UART; implementation concrete liee a ESP32 Arduino (Serial1/2).
 */
class UartMidiTransport {
public:
  virtual ~UartMidiTransport() = default;

  virtual bool begin(const UartMidiConfig& cfg) = 0;
  virtual void update() = 0;
  virtual bool isReady() const = 0;

  virtual void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) = 0;
  virtual void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) = 0;
  virtual void sendControlChange(uint8_t channel, uint8_t control, uint8_t value) = 0;
  virtual void sendProgramChange(uint8_t channel, uint8_t program) = 0;
  virtual void sendPitchBend(uint8_t channel, int bend) = 0;
  virtual void sendAftertouch(uint8_t channel, uint8_t pressure) = 0;
  virtual void sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) = 0;
  virtual void sendClock() = 0;
  virtual void sendStart() = 0;
  virtual void sendStop() = 0;
  virtual void sendContinue() = 0;
};

}  // namespace nidmi
