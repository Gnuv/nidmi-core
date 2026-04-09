#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace nidmi {
namespace platform {

/**
 * Encapsule la mise à jour du firmware applicatif (OTA) via l’API Arduino `Update`
 * sur ESP32 (partition OTA). À utiliser avec un flux d’octets (HTTP upload, etc.).
 *
 * Ne dépend pas du serveur web : l’application enregistre les handlers HTTP et
 * délègue les chunks à `write()`.
 */
class OtaUpdate {
 public:
  OtaUpdate() = default;
  ~OtaUpdate();

  OtaUpdate(const OtaUpdate&) = delete;
  OtaUpdate& operator=(const OtaUpdate&) = delete;

  /** Espace disponible pour un futur firmware (partition OTA cible), en octets. */
  static size_t maxFirmwareBytes();

  /**
   * Démarre une session d’écriture. `firmwareSize` doit être la taille totale du `.bin`.
   * Utiliser `beginWithUnknownSize()` seulement si le core Arduino le supporte (streaming).
   */
  bool begin(size_t firmwareSize);

  /**
   * Variante pour taille inconnue à l’avance (`UPDATE_SIZE_UNKNOWN`) si supportée par le core.
   * Sinon retourne false.
   */
  bool beginWithUnknownSize();

  /** Écrit un bloc contigu. Retourne false si échec (voir `errorMessage()`). */
  bool write(const uint8_t* data, size_t len);

  size_t writtenBytes() const { return written_; }

  /**
   * Finalise l’image et valide. Si `reboot` est true, redémarre immédiatement (ne revient pas).
   */
  bool end(bool reboot = true);

  /** Annule une session en cours (buffer incomplet, erreur réseau, etc.). */
  void abort();

  bool active() const { return active_; }

  /** Dernier message d’erreur lisible (ou chaîne vide). */
  const char* errorMessage() const { return errMsg_.c_str(); }

 private:
  void setErrorFromUpdate();

  bool active_ = false;
  size_t written_ = 0;
  String errMsg_;
};

}  // namespace platform
}  // namespace nidmi
