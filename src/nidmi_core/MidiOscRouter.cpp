#include "MidiOscRouter.h"
#include <OSCMessage.h>

namespace nidmi {

MidiOscRouter::MidiOscRouter(RtpMidiService& rtp, OscUdpService& osc) : rtp_(rtp), osc_(osc) {
  prefix_ = "/nidmi";
}

void MidiOscRouter::setAddressPrefix(const char* prefix) {
  prefix_ = prefix ? prefix : "/nidmi";
}

void MidiOscRouter::wire() {
  if (wired_) {
    return;
  }

  String base = prefix_;
  while (base.length() > 1 && base.endsWith("/")) {
    base.remove(base.length() - 1);
  }

  rtp_.setMidiInputHooks(
      [this, base](uint8_t ch, uint8_t n, uint8_t v) {
        String p = base + "/midi/noteon";
        osc_.sendMidiTriplet(p.c_str(), n, v, ch);
      },
      [this, base](uint8_t ch, uint8_t n, uint8_t v) {
        String p = base + "/midi/noteoff";
        osc_.sendMidiTriplet(p.c_str(), n, v, ch);
      },
      [this, base](uint8_t ch, uint8_t cc, uint8_t val) {
        String p = base + "/midi/cc";
        osc_.sendMidiTriplet(p.c_str(), cc, val, ch);
      });

  osc_.setReceiveCallback([this, base](const char* addr, OSCMessage& msg) {
    if (msg.size() < 3 || !msg.isInt(0) || !msg.isInt(1) || !msg.isInt(2)) {
      return;
    }
    const int x0 = msg.getInt(0);
    const int x1 = msg.getInt(1);
    const int x2 = msg.getInt(2);
    const String a = addr;
    const String nb = base + "/midi/noteon";
    const String nf = base + "/midi/noteoff";
    const String nc = base + "/midi/cc";

    if (a == nb) {
      rtp_.sendNoteOn(static_cast<uint8_t>(x2), static_cast<uint8_t>(x0), static_cast<uint8_t>(x1));
    } else if (a == nf) {
      rtp_.sendNoteOff(static_cast<uint8_t>(x2), static_cast<uint8_t>(x0), static_cast<uint8_t>(x1));
    } else if (a == nc) {
      rtp_.sendControlChange(static_cast<uint8_t>(x2), static_cast<uint8_t>(x0), static_cast<uint8_t>(x1));
    }
  });

  wired_ = true;
}

void MidiOscRouter::unwire() {
  if (!wired_) {
    return;
  }
  rtp_.clearMidiInputHooks();
  osc_.setReceiveCallback(nullptr);
  wired_ = false;
}

}  // namespace nidmi
