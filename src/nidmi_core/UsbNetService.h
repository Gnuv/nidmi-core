/**
 * Interface reseau sur le cable USB (CDC-NCM), coexistant avec l'USB-MIDI.
 *
 * Donne a l'ESP32-S3 un second netif porte par l'USB. Tout ce qui est reseau
 * dans l'application continue de fonctionner sans modification : serveur HTTP,
 * WebSocket, OSC/UDP, RTP-MIDI, OTA, mDNS — mais par le cable, WiFi eteint.
 *
 * Cible : ESP32-S3 en usb_mode=0 (USB-OTG / TinyUSB). L'ESP32-C3 n'a pas
 * d'USB-OTG et est exclu a la compilation ; l'API reste appelable et rend
 * false partout, pour eviter des #ifdef chez l'appelant.
 *
 * Voir docs/USB_NET.md pour le cablage des appels et les contraintes mesurees.
 */
#pragma once

#include <Arduino.h>
#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_ENABLED && CONFIG_TINYUSB_NCM_ENABLED
#define NIDMI_USB_NET_SUPPORTED 1
#else
#define NIDMI_USB_NET_SUPPORTED 0
#endif

#if NIDMI_USB_NET_SUPPORTED
#include <esp_netif.h>
#endif

namespace nidmi_core {

struct UsbNetConfig {
  /** Nom de l'interface tel que l'hote l'affiche. */
  const char* interfaceName = "NiDMI USB Network";

  /** Adresse de l'ESP32 sur le lien USB. Garder un sous-reseau distinct de l'AP WiFi. */
  const char* ip = "192.168.7.1";
  const char* netmask = "255.255.255.0";

  /** Sert un bail a l'hote. Sans lui, l'hote reste en link-local. */
  bool dhcpServer = true;

  /**
   * Annonce une passerelle et un DNS dans le bail. Laisser false.
   * A true, l'hote peut prendre le lien USB comme route par defaut et perdre
   * son acces Internet des qu'on branche l'instrument.
   */
  bool advertiseRouter = false;

  /**
   * Active et annonce mDNS sur le lien USB a la montee du lien.
   * L'application doit avoir initialise mDNS avant (MDNS.begin / mdns_init).
   */
  bool manageMdns = true;
};

/** Etapes de begin(), pour diagnostiquer sans console serie. */
enum class UsbNetStep : uint8_t {
  Ok = 0,
  NotRegistered = 1,  // enableInterface() pas appele, ou refuse
  SyncAlloc = 2,
  NetifInit = 3,
  EventLoop = 4,
  NetifNew = 5,
  NetifAttach = 6,
  RxTask = 7,
};

struct UsbNetStats {
  uint32_t rxFrames = 0;
  uint32_t rxDropped = 0;
  uint32_t txFrames = 0;
  uint32_t txTimeouts = 0;
};

/**
 * Une seule instance : il n'y a qu'un peripherique USB, et les callbacks
 * TinyUSB sont globaux. Meme approche que RtpMidiService avec ses callbacks
 * statiques vers l'instance active.
 */
class UsbNetService {
public:
  /**
   * Le constructeur enregistre le descripteur NCM, comme celui d'USBMIDI.
   *
   * Ce n'est pas un detail de style : avec `cdc_on_boot=1` le core appelle
   * USB.begin() depuis main() AVANT setup(), et tout descripteur enregistre
   * dans setup() est ignore en silence — le peripherique enumere sans
   * l'interface reseau. Declarer le service en global est donc la seule forme
   * correcte dans toutes les configurations :
   *
   *     static nidmi_core::UsbNetService usbNet;   // portee globale
   */
  UsbNetService();

  /** Vrai si la cible peut faire du NCM (decide a la compilation). */
  static constexpr bool available() { return NIDMI_USB_NET_SUPPORTED != 0; }

  /**
   * Enregistre le descripteur NCM aupres de TinyUSB. Idempotent.
   * Deja fait par le constructeur ; utile seulement pour verifier le resultat.
   */
  bool enableInterface();

  /** Cree le netif et demarre le serveur DHCP. Apres USB.begin(). */
  bool begin(const UsbNetConfig& cfg = UsbNetConfig());

  /**
   * A appeler dans loop(). Epingle la tache usbd au coeur de l'interruption
   * USB des qu'il est connu (sans quoi l'emission peut se figer pour
   * toujours, voir UsbNetService.cpp), suit le montage USB pour le netif et
   * declenche l'activation mDNS au bon moment. N'annonce PAS l'etat du lien :
   * il appartient au pilote NCM. Non bloquant.
   */
  void update();

  bool isLinkUp() const;
  UsbNetStep lastStep() const;
  UsbNetStats stats() const;

  /**
   * Releve de diagnostic, en JSON, LECTURE SEULE : remplissage de la file
   * d'evenements de TinyUSB (maximum atteint, evenements deposes par type),
   * temps passe a y deposer nos appels, etat des points d'acces NCM vu par
   * TinyUSB, et registres du controleur USB pour chaque point d'acces.
   * Sert a dire, quand un sens du lien se fige, si le materiel a fini son
   * transfert (evenement perdu en route) ou s'il attend encore l'hote.
   */
  String diagJson() const;

  /**
   * Vrai si l'hote a un bail de notre serveur DHCP (retrouve par sa MAC, celle
   * qu'on lui annonce) : c'est l'adresse que sonderHote() interroge.
   */
  bool hoteConnu() const;

  /**
   * Sonde l'hote : une requete ARP vers l'adresse louee. Un hote vivant repond
   * toujours a l'ARP — pare-feu furtif compris — et sa reponse compte dans
   * stats().rxFrames. C'est une preuve de vie qu'on PROVOQUE au lieu de
   * l'attendre : au repos, un Mac se tait jusqu'a une minute (MESURES §148).
   * Faux si rien n'est parti : service arrete, lien bas, aucun bail.
   */
  bool sonderHote();

  /** Adresse de l'ESP32 sur le lien, 0.0.0.0 si le service n'est pas demarre. */
  IPAddress localIp() const;
  /** Adresse de diffusion du lien, ex. "192.168.7.255". Vide si non demarre. */
  String broadcastAddress() const;

  /** MAC du cote ESP32. */
  const char* deviceMac() const;
  /** MAC annoncee a l'hote (descripteur iMACAddress). */
  const char* hostMac() const;

#if NIDMI_USB_NET_SUPPORTED
  esp_netif_t* netif() const;
#endif
};

}  // namespace nidmi_core
