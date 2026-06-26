#include <nidmi_platform/OtaUpdate.h>

#include <Update.h>

#if defined(ARDUINO_ARCH_ESP32)
#define NIDMI_PLATFORM_OTA_ESP32 1
#include <esp_arduino_version.h>
#endif

namespace nidmi_core {
namespace platform {

namespace {

#if NIDMI_PLATFORM_OTA_ESP32
#if !defined(UPDATE_SIZE_UNKNOWN)
#define UPDATE_SIZE_UNKNOWN 0xFFFFFFFFU
#endif
#endif

}  // namespace

OtaUpdate::~OtaUpdate() {
  abort();
}

size_t OtaUpdate::maxFirmwareBytes() {
#if NIDMI_PLATFORM_OTA_ESP32
  return ESP.getFreeSketchSpace();
#else
  return 0;
#endif
}

bool OtaUpdate::begin(size_t firmwareSize) {
  abort();
  errMsg_ = "";

  if (firmwareSize == 0) {
    errMsg_ = "taille nulle";
    return false;
  }

#if NIDMI_PLATFORM_OTA_ESP32
  if (firmwareSize > ESP.getFreeSketchSpace()) {
    errMsg_ = "firmware trop volumineux pour la partition OTA";
    return false;
  }

  if (!Update.begin(firmwareSize, U_FLASH)) {
    setErrorFromUpdate();
    return false;
  }
  active_ = true;
  written_ = 0;
  return true;
#else
  errMsg_ = "OTA non supportee sur cette cible";
  return false;
#endif
}

bool OtaUpdate::beginWithUnknownSize() {
  abort();
  errMsg_ = "";

#if NIDMI_PLATFORM_OTA_ESP32
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(2, 0, 0)
  if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    setErrorFromUpdate();
    return false;
  }
  active_ = true;
  written_ = 0;
  return true;
#else
  errMsg_ = "beginWithUnknownSize requiert Arduino-ESP32 >= 2.0";
  return false;
#endif
#else
  errMsg_ = "OTA non supportee sur cette cible";
  return false;
#endif
}

bool OtaUpdate::write(const uint8_t* data, size_t len) {
  if (!active_ || !data || len == 0) {
    if (!active_) {
      errMsg_ = "session inactive";
    }
    return false;
  }

#if NIDMI_PLATFORM_OTA_ESP32
  const size_t w = Update.write(const_cast<uint8_t*>(data), len);
  if (w != len) {
    setErrorFromUpdate();
    abort();
    return false;
  }
  written_ += w;
  return true;
#else
  return false;
#endif
}

bool OtaUpdate::end(bool reboot) {
  if (!active_) {
    errMsg_ = "session inactive";
    return false;
  }

#if NIDMI_PLATFORM_OTA_ESP32
  if (!Update.end(true)) {
    setErrorFromUpdate();
    active_ = false;
    return false;
  }
  active_ = false;
  written_ = 0;
  if (reboot) {
    delay(100);
    ESP.restart();
  }
  return true;
#else
  return false;
#endif
}

void OtaUpdate::abort() {
#if NIDMI_PLATFORM_OTA_ESP32
  if (active_) {
    Update.abort();
  }
#endif
  active_ = false;
  written_ = 0;
}

void OtaUpdate::setErrorFromUpdate() {
#if NIDMI_PLATFORM_OTA_ESP32
  errMsg_ = Update.errorString();
  if (errMsg_.length() == 0) {
    errMsg_ = "erreur Update";
  }
#else
  errMsg_ = "erreur";
#endif
}

}  // namespace platform
}  // namespace nidmi_core
