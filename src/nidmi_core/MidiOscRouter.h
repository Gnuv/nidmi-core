#pragma once

#include "OscUdpService.h"
#include "RtpMidiService.h"
#include <Arduino.h>

namespace nidmi {

/**
 * Pont RTP-MIDI <-> OSC UDP : conventions d adresses (prefixe par defaut "/nidmi") :
 *   {prefix}/midi/noteon  : 3 int32 (note, velocity, canal)
 *   {prefix}/midi/noteoff : idem
 *   {prefix}/midi/cc      : (cc, valeur, canal)
 *
 * Les messages MIDI entrants (RTP) sont emis en OSC ; les paquets OSC vers ces adresses
 * sont convertis en MIDI sortant (RTP).
 */
class MidiOscRouter {
public:
  MidiOscRouter(RtpMidiService& rtp, OscUdpService& osc);

  void setAddressPrefix(const char* prefix);
  void wire();
  void unwire();
  bool isWired() const { return wired_; }

private:
  RtpMidiService& rtp_;
  OscUdpService& osc_;
  String prefix_;
  bool wired_ = false;
};

}  // namespace nidmi
