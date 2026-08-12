/**
 * SPIKE — interface reseau USB (CDC-NCM) coexistant avec USB-MIDI sur ESP32-S3.
 *
 * Code jetable : valide les etapes 1-3 avant d'ecrire un vrai UsbNetService
 * dans nidmi-core. Ne pas dependre de cette API.
 *
 * Cible : ESP32-S3 en usb_mode=0 (USB-OTG / TinyUSB). Le C3 n'a pas d'USB-OTG
 * et est exclu a la compilation par SOC_USB_OTG_SUPPORTED.
 */
#pragma once

#include <Arduino.h>
#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_ENABLED && CONFIG_TINYUSB_NCM_ENABLED
#define NIDMI_USB_NCM_SUPPORTED 1
#else
#define NIDMI_USB_NCM_SUPPORTED 0
#endif

#if NIDMI_USB_NCM_SUPPORTED
#include "esp_netif.h"
#endif

namespace nidmi_spike {

struct UsbNcmConfig {
  /** Nom de l'interface reseau cote hote. */
  const char* ifDescription = "NiDMI USB Network";
  /** IP de l'ESP32 sur le lien USB. Sous-reseau distinct de l'AP WiFi. */
  const char* ip = "192.168.7.1";
  const char* netmask = "255.255.255.0";
  /**
   * Bail DHCP servi a l'hote. Ni routeur ni DNS ne sont annonces :
   * l'hote ne doit jamais router son trafic Internet vers l'ESP32.
   */
  bool dhcpServer = true;
};

/** Vrai si la cible supporte NCM (compile-time). */
constexpr bool usbNcmSupported() {
  return NIDMI_USB_NCM_SUPPORTED != 0;
}

/**
 * Enregistre le descripteur NCM aupres de TinyUSB.
 * IMPERATIF : appeler AVANT USB.begin(), comme le constructeur d'USBMIDI.
 */
bool usbNcmEnableInterface();

/** Cree le netif, demarre le serveur DHCP. A appeler APRES USB.begin(). */
bool usbNcmBegin(const UsbNcmConfig& cfg = UsbNcmConfig());

/** A appeler dans loop() : suit l'etat du lien USB (mount / unmount). */
void usbNcmUpdate();

bool usbNcmIsLinkUp();

/** MAC cote ESP32 (netif). */
const char* usbNcmDeviceMac();
/** MAC cote hote, transmise via le descripteur iMACAddress. */
const char* usbNcmHostMac();

/** Compteurs de diagnostic. */
struct UsbNcmStats {
  uint32_t rxFrames;
  uint32_t rxDropped;
  uint32_t txFrames;
  uint32_t txTimeouts;
};
UsbNcmStats usbNcmStats();

#if NIDMI_USB_NCM_SUPPORTED
esp_netif_t* usbNcmNetif();
#endif

}  // namespace nidmi_spike
