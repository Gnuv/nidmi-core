#pragma once

#include <Arduino.h>

namespace nidmi {
namespace platform {

/**
 * Configuration du périphérique USB **CDC** (port série virtuel sur câble USB).
 * Indépendante du MIDI USB : même pile TinyUSB, mais responsabilité isolée ici.
 */
struct UsbCdcConfig {
  /** Nom produit USB (énumération). Chaîne courte recommandée (≤ 32 caractères). */
  const char* productName = "nidmi";
};

/**
 * Démarrage **CDC seul** sur ESP32-S3 (USB-OTG / TinyUSB), lorsque l’application
 * contrôle l’init USB (typiquement `cdc_on_boot=0`).
 *
 * Règle : un seul propriétaire de `USB.begin()` pour un profil donné au boot.
 * Ne pas combiner avec une autre classe qui appellerait `USB.begin()` sans coordination.
 */
class Esp32UsbCdc {
 public:
  Esp32UsbCdc() = default;

  /** True sur cible S3 avec support USB device attendu par le build. */
  static bool isTargetSupported();

  /**
   * True si le firmware est en mode **USB-OTG TinyUSB** utilisable pour enregistrer CDC
   * (pas le mode HWCDC `ARDUINO_USB_MODE==1` où TinyUSB n’est pas utilisé pour la console).
   */
  static bool isOtgTinyUsbProfile();

  /**
   * Initialise l’interface CDC + pile USB (ordre : enregistrement CDC puis `USB.begin()`).
   * @return false si non applicable, profil incompatible, ou échec.
   */
  bool begin(const UsbCdcConfig& config = UsbCdcConfig{});

  bool isActive() const { return active_; }

 private:
  bool active_ = false;
};

}  // namespace platform
}  // namespace nidmi
