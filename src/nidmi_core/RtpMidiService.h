#pragma once

#include <Arduino.h>
#include <functional>

namespace nidmi {

/**
 * RTP-MIDI (AppleMIDI) : emission et reception via WiFi.
 * Appeler netBeginSoftAp() avant begin(), puis update() dans loop().
 *
 * Hooks MIDI entrant : a definir avant begin() si besoin (ex. routage vers OSC).
 */
class RtpMidiService {
public:
  RtpMidiService() = default;

  using MidiThreeByteFn = std::function<void(uint8_t channel, uint8_t a, uint8_t b)>;

  void setMidiInputHooks(MidiThreeByteFn noteOn, MidiThreeByteFn noteOff, MidiThreeByteFn controlChange);
  void clearMidiInputHooks();

  bool begin(const char* sessionName, uint16_t port = 5004);
  void stop();
  void update();

  void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
  void sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
  void sendControlChange(uint8_t channel, uint8_t control, uint8_t value);
  void sendProgramChange(uint8_t channel, uint8_t program);
  void sendPitchBend(uint8_t channel, int bend);
  void sendAftertouch(uint8_t channel, uint8_t pressure);
  void sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure);
  void sendClock();
  void sendStart();
  void sendStop();
  void sendContinue();

  bool isReady() const { return started_; }
  uint16_t port() const { return port_; }

private:
  bool started_ = false;
  uint16_t port_ = 5004;

  MidiThreeByteFn hookNoteOn_;
  MidiThreeByteFn hookNoteOff_;
  MidiThreeByteFn hookControlChange_;

  void installMidiInputHandlers_();
  void handleNoteOn_(uint8_t channel, uint8_t note, uint8_t velocity);
  void handleNoteOff_(uint8_t channel, uint8_t note, uint8_t velocity);
  void handleControlChange_(uint8_t channel, uint8_t cc, uint8_t value);

  static void staticNoteOn(byte channel, byte note, byte velocity);
  static void staticNoteOff(byte channel, byte note, byte velocity);
  static void staticControlChange(byte channel, byte cc, byte value);

  static RtpMidiService* callbackTarget_;
};

}  // namespace nidmi
