# Mise à jour OTA (firmware par Wi‑Fi)

## Rôle

`nidmi::platform::OtaUpdate` encapsule l’API Arduino **`Update`** sur ESP32 : écriture du binaire dans la **partition OTA** de rechange, validation, redémarrage.

La bibliothèque **ne fournit pas** de route HTTP ni d’interface web : l’application (ex. NiDMI avec **ESPAsyncWebServer**) enregistre un handler (POST multipart ou body brut) et transmet les octets à `write()`.

## Prérequis flash

- Table de partitions avec **au moins deux slots d’application** (OTA) ou équivalent documenté par Espressif.
- Taille du `.bin` ≤ `OtaUpdate::maxFirmwareBytes()` (espace libre côté partition cible).

## Usage typique (taille connue)

```cpp
#include <nidmi_platform/OtaUpdate.h>

nidmi::platform::OtaUpdate ota;

bool startOta(size_t totalBytes) {
  if (totalBytes > nidmi::platform::OtaUpdate::maxFirmwareBytes()) {
    return false;
  }
  return ota.begin(totalBytes);
}

bool chunk(const uint8_t* data, size_t len) {
  return ota.write(data, len);
}

bool finish() {
  return ota.end(true);  // redémarre si succès
}
```

## Taille inconnue

`beginWithUnknownSize()` utilise `UPDATE_SIZE_UNKNOWN` si le core Arduino-ESP32 est ≥ 2.0. Sinon, préférer une taille connue (en-tête `Content-Length`, ou taille du fichier côté client).

## Erreurs

En cas d’échec, `errorMessage()` retourne une chaîne fournie par `Update.errorString()` (ou un message court interne).

## Sécurité

À l’usage : protéger l’endpoint d’upload (token, mot de passe, réseau isolé). Ce module ne gère pas l’authentification.
