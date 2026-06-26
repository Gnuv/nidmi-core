#include "NetBootstrap.h"
#include <ESPmDNS.h>
#include <WiFi.h>

namespace nidmi_core {

bool netBeginSoftAp(const char* apSsid, const char* apPass, const char* mdnsHostname) {
  if (!apSsid || !apPass || !mdnsHostname) {
    return false;
  }

  WiFi.mode(WIFI_MODE_AP);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  const IPAddress apIp(192, 168, 4, 1);
  const IPAddress apGw(192, 168, 4, 1);
  const IPAddress apSn(255, 255, 255, 0);
  WiFi.softAPConfig(apIp, apGw, apSn);
  WiFi.softAP(apSsid, apPass, 1);
  delay(100);

  if (!MDNS.begin(mdnsHostname)) {
    return false;
  }
  return true;
}

}  // namespace nidmi_core
