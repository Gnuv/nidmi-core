#include <nidmi_platform/Esp32UsbCdc.h>

#include <cstring>

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV) || defined(ARDUINO_ESP32S3)
#define NIDMI_PLATFORM_ESP32S3_USB 1
#include <sdkconfig.h>
#endif

#if NIDMI_PLATFORM_ESP32S3_USB && defined(CONFIG_TINYUSB_CDC_ENABLED) && CONFIG_TINYUSB_CDC_ENABLED
#include <USB.h>
#include <USBCDC.h>
#if !ARDUINO_USB_MODE && !ARDUINO_USB_CDC_ON_BOOT
static USBCDC nidmiPlatformUsbCdc;
#endif
#endif

namespace nidmi {
namespace platform {

namespace {

#if NIDMI_PLATFORM_ESP32S3_USB && defined(CONFIG_TINYUSB_CDC_ENABLED) && CONFIG_TINYUSB_CDC_ENABLED
void sanitizeProductName(const char* in, char* out, size_t outSz) {
  if (!out || outSz == 0) {
    return;
  }
  out[0] = '\0';
  if (!in) {
    strncpy(out, "nidmi", outSz - 1);
    out[outSz - 1] = '\0';
    return;
  }
  size_t n = 0;
  while (in[n] && n < outSz - 1) {
    char c = in[n];
    if (c == '\n' || c == '\r' || c == '\t') {
      break;
    }
    out[n] = c;
    ++n;
  }
  out[n] = '\0';
  if (n == 0) {
    strncpy(out, "nidmi", outSz - 1);
    out[outSz - 1] = '\0';
  }
}
#endif

}  // namespace

bool Esp32UsbCdc::isTargetSupported() {
#if NIDMI_PLATFORM_ESP32S3_USB
  return true;
#else
  return false;
#endif
}

bool Esp32UsbCdc::isOtgTinyUsbProfile() {
#if !NIDMI_PLATFORM_ESP32S3_USB
  return false;
#elif !defined(CONFIG_TINYUSB_CDC_ENABLED) || !CONFIG_TINYUSB_CDC_ENABLED
  return false;
#elif ARDUINO_USB_MODE
  return false;
#elif ARDUINO_USB_CDC_ON_BOOT
  return false;
#else
  return true;
#endif
}

bool Esp32UsbCdc::begin(const UsbCdcConfig& config) {
  if (active_) {
    return true;
  }

#if !NIDMI_PLATFORM_ESP32S3_USB
  (void)config;
  return false;
#elif !defined(CONFIG_TINYUSB_CDC_ENABLED) || !CONFIG_TINYUSB_CDC_ENABLED
  (void)config;
  return false;
#elif ARDUINO_USB_MODE
  (void)config;
  active_ = true;
  return true;
#elif ARDUINO_USB_CDC_ON_BOOT
  (void)config;
  active_ = true;
  return true;
#else
  char nameBuf[33];
  sanitizeProductName(config.productName, nameBuf, sizeof(nameBuf));
  nidmiPlatformUsbCdc.begin();
  USB.productName(nameBuf);
  USB.begin();
  active_ = true;
  return true;
#endif
}

}  // namespace platform
}  // namespace nidmi
