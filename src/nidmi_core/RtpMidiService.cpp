#include "RtpMidiService.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <AppleMIDI.h>
#include <ESPmDNS.h>
#include <functional>

USING_NAMESPACE_APPLEMIDI

// Port 5004 par defaut (modifiable uniquement en changeant la macro si besoin d un autre port).
APPLEMIDI_CREATE_INSTANCE(WiFiUDP, NdmMidi, "nidmi-core", 5004);

namespace nidmi_core {

RtpMidiService* RtpMidiService::callbackTarget_ = nullptr;

void RtpMidiService::staticNoteOn(byte channel, byte note, byte velocity) {
  if (callbackTarget_) {
    callbackTarget_->handleNoteOn_(channel, note, velocity);
  }
}

void RtpMidiService::staticNoteOff(byte channel, byte note, byte velocity) {
  if (callbackTarget_) {
    callbackTarget_->handleNoteOff_(channel, note, velocity);
  }
}

void RtpMidiService::staticControlChange(byte channel, byte cc, byte value) {
  if (callbackTarget_) {
    callbackTarget_->handleControlChange_(channel, cc, value);
  }
}

/* Pas de trace par message : une ligne sur le port serie par note (35 o a
 * 115 200 bauds, ~3 ms) bloquait la tache qui lisait des qu'un accord
 * remplissait la FIFO de l'UART — sur le chemin meme du MIDI (MESURES §172 du
 * depot nidmi). L'entree se voit a la LED d'activite de l'app. */
void RtpMidiService::handleNoteOn_(uint8_t channel, uint8_t note, uint8_t velocity) {
  if (hookNoteOn_) {
    hookNoteOn_(channel, note, velocity);
  }
}

void RtpMidiService::handleNoteOff_(uint8_t channel, uint8_t note, uint8_t velocity) {
  if (hookNoteOff_) {
    hookNoteOff_(channel, note, velocity);
  }
}

void RtpMidiService::handleControlChange_(uint8_t channel, uint8_t cc, uint8_t value) {
  if (hookControlChange_) {
    hookControlChange_(channel, cc, value);
  }
}

void RtpMidiService::installMidiInputHandlers_() {
  callbackTarget_ = this;
  NdmMidi.setHandleNoteOn(staticNoteOn);
  NdmMidi.setHandleNoteOff(staticNoteOff);
  NdmMidi.setHandleControlChange(staticControlChange);
}

void RtpMidiService::setMidiInputHooks(MidiThreeByteFn noteOn, MidiThreeByteFn noteOff, MidiThreeByteFn controlChange) {
  hookNoteOn_ = std::move(noteOn);
  hookNoteOff_ = std::move(noteOff);
  hookControlChange_ = std::move(controlChange);
  if (started_) {
    installMidiInputHandlers_();
  }
}

void RtpMidiService::clearMidiInputHooks() {
  hookNoteOn_ = nullptr;
  hookNoteOff_ = nullptr;
  hookControlChange_ = nullptr;
  if (started_) {
    installMidiInputHandlers_();
  }
}

bool RtpMidiService::begin(const char* sessionName, uint16_t port) {
  (void)port;
  if (port != 5004) {
    // La macro APPLEMIDI_CREATE_INSTANCE fige le port a la compilation.
  }

  AppleNdmMidi.begin();
  AppleNdmMidi.setName(sessionName ? sessionName : "nidmi-core");

  MDNS.addService("apple-midi", "udp", AppleNdmMidi.getPort());

  AppleNdmMidi.setHandleConnected([](const APPLEMIDI_NAMESPACE::ssrc_t&, const char* name) {
    Serial.printf("[RTP-MIDI] connecte: %s\n", name ? name : "?");
    if (callbackTarget_) callbackTarget_->connected_ = true;
  });
  AppleNdmMidi.setHandleDisconnected([](const APPLEMIDI_NAMESPACE::ssrc_t&) {
    Serial.println("[RTP-MIDI] deconnecte");
    if (callbackTarget_) callbackTarget_->connected_ = false;
  });

  installMidiInputHandlers_();

  started_ = true;
  port_ = AppleNdmMidi.getPort();
  return true;
}

void RtpMidiService::stop() {
  if (!started_) {
    return;
  }
  AppleNdmMidi.end();
  if (callbackTarget_ == this) {
    callbackTarget_ = nullptr;
  }
  started_ = false;
}

/* TOUT ce qui est arrive, pas un message : read() n'en decode qu'un, si bien
 * qu'un accord arrive dans un seul paquet RTP s'etalait sur autant d'appels —
 * un tour de boucle par note. Borne a 32 : un flot ne monopolise pas
 * l'appelant. Rend le nombre de messages traites. */
int RtpMidiService::update() {
  if (!started_) {
    return 0;
  }
  int n = 0;
  while (n < 32 && NdmMidi.read()) {
    n++;
  }
  return n;
}

void RtpMidiService::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
  if (!started_) {
    return;
  }
  NdmMidi.sendNoteOn(note, velocity, channel);
}

void RtpMidiService::sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
  if (!started_) {
    return;
  }
  NdmMidi.sendNoteOff(note, velocity, channel);
}

void RtpMidiService::sendControlChange(uint8_t channel, uint8_t control, uint8_t value) {
  if (!started_) {
    return;
  }
  NdmMidi.sendControlChange(control, value, channel);
}

void RtpMidiService::sendProgramChange(uint8_t channel, uint8_t program) {
  if (!started_) {
    return;
  }
  NdmMidi.sendProgramChange(program, channel);
}

void RtpMidiService::sendPitchBend(uint8_t channel, int bend) {
  if (!started_) {
    return;
  }
  NdmMidi.sendPitchBend(bend, channel);
}

void RtpMidiService::sendAftertouch(uint8_t channel, uint8_t pressure) {
  if (!started_) {
    return;
  }
  NdmMidi.sendAfterTouch(pressure, channel);
}

void RtpMidiService::sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) {
  if (!started_) {
    return;
  }
  if (channel < 1 || channel > 16) {
    return;
  }
  NdmMidi.sendAfterTouch(note & 0x7F, pressure & 0x7F, channel);
}

void RtpMidiService::sendClock() {
  if (!started_) {
    return;
  }
  NdmMidi.sendClock();
}

void RtpMidiService::sendStart() {
  if (!started_) {
    return;
  }
  NdmMidi.sendStart();
}

void RtpMidiService::sendStop() {
  if (!started_) {
    return;
  }
  NdmMidi.sendStop();
}

void RtpMidiService::sendContinue() {
  if (!started_) {
    return;
  }
  NdmMidi.sendContinue();
}

}  // namespace nidmi_core
