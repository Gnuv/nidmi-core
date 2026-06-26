#pragma once

#include <Arduino.h>

namespace nidmi_core {

/**
 * Point d'acces WiFi + mDNS (hostname .local).
 * A appeler avant RtpMidiService::begin (AppleMIDI enregistre le service sur mDNS existant).
 */
bool netBeginSoftAp(const char* apSsid, const char* apPass, const char* mdnsHostname);

}  // namespace nidmi_core
