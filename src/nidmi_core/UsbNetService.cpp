#include "UsbNetService.h"

#if NIDMI_USB_NET_SUPPORTED

#include <string.h>

#include <atomic>

#include "class/net/net_device.h"
#include "esp32-hal-tinyusb.h"
#include "device/usbd_pvt.h"   // usbd_defer_func : executer DANS la tache USB
#include "device/dcd.h"        // DCD_EVENT_COUNT : types d'evenements, pour le releve

#include <esp_event.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_defaults.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>   // les trames recues et la pile de usbnet_rx vont en PSRAM
#include <esp_netif_net_stack.h>   // esp_netif_get_netif_impl : le netif lwIP, pour l'ARP
#include <lwip/etharp.h>
#include <lwip/tcpip.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>   // xTaskCreatePinnedToCoreWithCaps
#include <mdns.h>

namespace {

// --- Identite -------------------------------------------------------------
// Deux MAC distinctes : une pour le netif ESP32, une annoncee a l'hote.
// Les faire identiques casse l'ARP sur le lien.
uint8_t s_devMac[6] = {0};
uint8_t s_hostMac[6] = {0};
char s_devMacStr[18] = {0};   // aa:bb:cc:dd:ee:ff
char s_hostMacStr[13] = {0};  // AABBCCDDEEFF — format impose par NCM
char s_ifName[40] = "NiDMI USB Network";

// --- Etat -----------------------------------------------------------------
esp_netif_t* s_netif = nullptr;
esp_netif_driver_base_t s_driverBase = {};
bool s_interfaceEnabled = false;
bool s_started = false;
bool s_linkUp = false;
bool s_manageMdns = true;
bool s_mdnsAnnounced = false;
uint32_t s_lastAnnounce = 0;
uint8_t s_announcesLeft = 0;

nidmi_core::UsbNetStep s_lastStep = nidmi_core::UsbNetStep::NotRegistered;
nidmi_core::UsbNetStats s_stats;

// --- Chemin RX ------------------------------------------------------------
// tud_network_recv_cb s'execute dans la task usbd. On copie la trame, on la
// pousse dans une queue, et une task dediee la remet a lwIP — puis DEMANDE a
// la task usbd de renouveler (voir « TOUT APPEL AU PILOTE… » plus bas).
struct RxFrame {
  void* buf;
  uint16_t len;
};
QueueHandle_t s_rxQueue = nullptr;
TaskHandle_t s_rxTask = nullptr;

// --- Chemin TX ------------------------------------------------------------
// Pas de tampon a nous : la tache usbd lit la trame DANS le tampon de lwIP,
// pendant que la tache reseau l'attend (voir usbnetTransmit).
SemaphoreHandle_t s_txMutex = nullptr;
SemaphoreHandle_t s_txDone = nullptr;

constexpr TickType_t kTxWaitTicks = pdMS_TO_TICKS(100);

/* ── RELEVE : LA FILE D'EVENEMENTS DE TINYUSB ─────────────────────────────
 * Tout passe par une file de 16 evenements (CFG_TUD_TASK_QUEUE_SZ, fige dans
 * la lib precompilee) : fins de transfert deposees par l'interruption, ET nos
 * appels differes. Les deux n'y entrent pas de la meme facon (osal_freertos.h) :
 * depuis une tache, le depot ATTEND une place ; depuis l'interruption, il
 * ECHOUE en silence si la file est pleine — et une fin de transfert perdue
 * laisse le pilote croire un transfert en cours, pour toujours.
 * `_usbd_qdef` est global dans usbd.c (OSAL_QUEUE_DEF) et la file est creee
 * statiquement dans son `sq` : son adresse EST la poignee de la file. */
extern "C" osal_queue_def_t _usbd_qdef;
// L'etat du pilote NCM, pour le releve : notre copie du pilote l'expose
// (tinyusb/ncm_device.c).
extern "C" const void* nidmi_ncm_etat(size_t* taille);
// ... et sa trace : les 16 derniers evenements (activations, notifications).
extern "C" uint32_t nidmi_ncm_evenements(uint32_t* ms, char* quoi, uint8_t* val, uint32_t* total);
extern "C" bool nidmi_ncm_reseau_actif(void);
volatile uint32_t s_evt[DCD_EVENT_COUNT] = {0};   // deposes avec succes, par type
volatile uint32_t s_evtIsr = 0;                   // dont depuis l'interruption
volatile uint16_t s_queueMax = 0;                 // remplissage maximal vu

volatile uint32_t s_deferPosted = 0;
volatile uint32_t s_deferSlow = 0;                // depots ayant attendu > 1 ms
volatile uint32_t s_deferMaxUs = 0;

void defer(osal_task_func_t func, void* param) {
  const int64_t t0 = esp_timer_get_time();
  usbd_defer_func(func, param, false);
  const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
  s_deferPosted++;
  if (dt > 1000) {
    s_deferSlow++;
  }
  if (dt > s_deferMaxUs) {
    s_deferMaxUs = dt;
  }
}

/* ── LA TACHE USB, EPINGLEE AU COEUR DE L'INTERRUPTION ────────────────────
 * Le pilote du controleur (dcd_dwc2.c, TinyUSB 0.20, mode esclave) ecrit la
 * FIFO d'emission depuis DEUX fils : la tache qui lance un transfert
 * (dcd_edpt_xfer -> edpt_schedule_packets -> epin_write_tx_fifo) et
 * l'interruption (handle_epin_slave). La tache se protege par une section
 * critique — qui ne masque l'interruption QUE sur son propre coeur ; et
 * l'interruption, elle, ne prend aucun verrou pour les points d'acces.
 * Or la tache usbd d'Arduino est creee SANS coeur (xTaskCreate) : quand elle
 * s'execute sur l'autre coeur que l'interruption, les deux ecrivent en meme
 * temps. Vu deux fois : l'hote a UN datagramme de moins que la carte n'en a
 * emis. Et le releve des registres montre le bloc en cours avec son dernier
 * paquet « ecrit » (XFRSIZ 0) mais absent de la FIFO : l'hote le reclame, la
 * carte repond NAK, pour toujours.
 *
 * Remede, eprouve (MESURES §147) : la tache usbd est REMPLACEE par une copie
 * epinglee au coeur de l'interruption. Sa section critique masque alors bien
 * l'interruption. Avant : lien mort a 93 puis 1 666 datagrammes ; apres :
 * 23 483 sans une expiree, sur la meme epreuve.
 *
 * Ce qui reste d'un autre coeur — les ecritures MIDI des autres taches — ne
 * lance que des transferts d'UN paquet : la tache l'ecrit en entier, la FIFO
 * de ce point d'acces n'a pas de second ecrivain, et DIEPEMPMSK n'est pas
 * touche. Seuls les transferts de plusieurs paquets (les NTB) exposaient la
 * course, et ils ne partent que de la tache usbd. */
volatile uint32_t s_coeurIsr = 0;          // masque des coeurs ou l'interruption a ete vue
volatile uint32_t s_coeurUsbd[2] = {0, 0};  // nos appels differes executes, par coeur
volatile int8_t s_coeurEpingle = -1;       // coeur de la tache usbd epinglee, -1 : flottante
bool s_epinglageDemande = false;
TaskHandle_t s_usbdEpinglee = nullptr;

// Pile et TCB STATIQUES : la creation ne peut pas manquer de place, il n'y a
// donc pas de chemin d'echec a gerer. En RAM interne, pas en PSRAM : c'est la
// tache la plus prioritaire du coeur de l'audio, elle ne doit pas attendre la
// PSRAM a chaque commutation. 3 072 o : elle en utilise 1 016 au plus, releve
// apres le demarrage (enumeration comprise) puis sous la charge du §82 par le
// cable (MESURES §150) ; les 4 096 d'Arduino laissaient 3 080 o jamais touches.
StackType_t s_usbdPile[3072];   // en octets sous ESP-IDF
StaticTask_t s_usbdTcb;

inline void compterCoeur() { s_coeurUsbd[xPortGetCoreID() & 1]++; }

/* ── LA RELANCE DE L'ENUMERATION ──────────────────────────────────────────
 * Deconnexion logicielle (le controleur retire sa resistance de tirage : l'hote
 * voit un depart), puis reconnexion par update() apres kRelanceMs : l'hote voit
 * une arrivee, remet le bus a zero et enumere de nouveau — MIDI compris. Les
 * deux appels passent par la tache usbd, comme tout appel au pilote. */
constexpr uint32_t kRelanceMs = 500;
volatile bool s_relanceEnCours = false;
uint32_t s_relanceDebut = 0;
uint32_t s_relances = 0;
void deconnecterDansUsbd(void*) { tud_disconnect(); }
void connecterDansUsbd(void*) { tud_connect(); }

void boucleUsbd(void*) {
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // l'ancienne tache s'est retiree
  for (;;) {
    tud_task();
  }
}

// S'execute DANS la tache usbd d'Arduino, entre deux evenements : rien n'est a
// moitie traite. Cree la remplacante, epinglee au coeur de l'interruption, lui
// passe la main, et se supprime.
void epinglerUsbd(void* coeur) {
  const BaseType_t c = (BaseType_t)(intptr_t)coeur;
  s_usbdEpinglee = xTaskCreateStaticPinnedToCore(boucleUsbd, "usbd", sizeof(s_usbdPile), nullptr,
                                                 configMAX_PRIORITIES - 1, s_usbdPile, &s_usbdTcb, c);
  if (s_usbdEpinglee == nullptr) {
    return;   // impossible avec des tampons fournis ; on garde la tache flottante
  }
  s_coeurEpingle = (int8_t)c;
  xTaskNotifyGive(s_usbdEpinglee);
  vTaskDelete(nullptr);
}

/* ── TOUT APPEL AU PILOTE NCM S'EXECUTE DANS LA TACHE USB ─────────────────
 * Le lien mourait en silence — sous charge (MESURES §140), puis a vide des la
 * montee (§143) : compteurs figes, aucun rejet, les deux bouts le disant monte.
 *
 * La cause, lue dans ncm_device.c (TinyUSB 0.20.x, et identique en 0.21.0) :
 * tud_network_recv_renew() est une boucle gardee contre la RE-ENTREE par deux
 * simples booleens, `_active` et `_process_again` — un garde ecrit pour qu'une
 * fonction se rappelle elle-meme DANS LE MEME FIL (TinyUSB #2711). Ici, deux
 * taches l'appelaient : la tache usbd, a chaque NTB recu (netd_xfer_cb), et la
 * tache RX, apres chaque trame. Un booleen n'est pas un verrou. Scenario qui
 * gele tout : la tache RX est dans la boucle (`_active` vrai) ; un NTB arrive,
 * la tache usbd le range, appelle renew, voit `_active` et REND LA MAIN SANS
 * RELANCER DE RECEPTION, en comptant sur l'autre ; or l'autre a deja relu
 * `_process_again` et sort. Plus personne ne relance la reception. Jamais.
 *
 * Meme famille cote emission (tud_network_can_xmit/xmit, appeles depuis la
 * tache lwIP, pendant que la tache usbd libere les NTB envoyes).
 *
 * Le pilote suppose UN SEUL FIL. On le lui rend : usbd_defer_func() fait
 * executer la fonction dans la tache usbd, a la suite de ses propres
 * evenements. Le garde a booleens redevient ce pour quoi il a ete ecrit. */
void renewInUsbd(void*) {
  compterCoeur();
  tud_network_recv_renew();
}

/* L'emission reste SYNCHRONE pour lwIP : la tache reseau demande a la tache
 * usbd d'emettre la trame, et attend sa reponse. La trame n'est plus recopiee
 * dans un tampon a nous (1 514 o de RAM interne, et une copie par trame) :
 * tud_network_xmit la recopie une seule fois, directement du tampon de lwIP
 * dans le NTB.
 *
 * Ce tampon n'appartient a lwIP que le temps de usbnetTransmit. D'ou un
 * contrat, porte par s_txEtat : la tache usbd ne touche au tampon qu'apres
 * avoir fait passer la demande de DEMANDE a EN_COURS ; l'emetteur qui renonce
 * (tache usbd muette 100 ms) fait passer DEMANDE a ANNULE — si c'est trop
 * tard (EN_COURS : la copie est commencee), il attend qu'elle finisse, ce qui
 * est borne. Un appel differe reste dans la file apres une annulation : il
 * trouvera ANNULE (et ne lira rien) ou la demande SUIVANTE (et la servira). */
enum : uint32_t { TX_LIBRE, TX_DEMANDE, TX_EN_COURS, TX_FINI, TX_ANNULE };
std::atomic<uint32_t> s_txEtat{TX_LIBRE};
const void* s_txSrc = nullptr;   // publies avant TX_DEMANDE, lus apres EN_COURS
uint16_t s_txLen = 0;
volatile bool s_txOk = false;
void xmitInUsbd(void*) {
  compterCoeur();
  uint32_t attendu = TX_DEMANDE;
  if (!s_txEtat.compare_exchange_strong(attendu, TX_EN_COURS)) {
    return;   // annulee, ou deja servie par un appel precedent : rien a lire
  }
  bool ok = false;
  if (tud_network_can_xmit(s_txLen)) {
    tud_network_xmit(const_cast<void*>(s_txSrc), s_txLen);   // xmit_cb recopie la trame, ici meme
    ok = true;
  }
  s_txOk = ok;
  s_txEtat.store(TX_FINI);
  xSemaphoreGive(s_txDone);
}

void deriveMacs() {
  if (s_devMacStr[0]) {
    return;
  }
  esp_read_mac(s_devMac, ESP_MAC_ETH);
  memcpy(s_hostMac, s_devMac, 6);
  // Meme OUI, dernier octet decale : deux stations distinctes sur le lien.
  s_hostMac[5] = (uint8_t)(s_hostMac[5] ^ 0x01);
  snprintf(s_devMacStr, sizeof(s_devMacStr), "%02x:%02x:%02x:%02x:%02x:%02x", s_devMac[0], s_devMac[1],
           s_devMac[2], s_devMac[3], s_devMac[4], s_devMac[5]);
  snprintf(s_hostMacStr, sizeof(s_hostMacStr), "%02X%02X%02X%02X%02X%02X", s_hostMac[0], s_hostMac[1],
           s_hostMac[2], s_hostMac[3], s_hostMac[4], s_hostMac[5]);
}

// --- Descripteur ----------------------------------------------------------
bool s_descriptorLoaded = false;

extern "C" uint16_t nidmi_usbnet_load_descriptor(uint8_t* dst, uint8_t* itf) {
  if (s_descriptorLoaded) {
    return 0;
  }
  s_descriptorLoaded = true;

  deriveMacs();

  uint8_t strIndex = tinyusb_add_string_descriptor(s_ifName);
  uint8_t macIndex = tinyusb_add_string_descriptor(s_hostMacStr);

  // L'ORDRE COMPTE, et c'est celui-ci qui est valide sur macOS : notification
  // d'abord, paire de donnees en duplex ensuite. On obtient
  //   MIDI IN1/OUT1, NCM notif 0x82, NCM data 0x83/0x03
  //
  // Ne pas « optimiser » cet ordre. L'avoir inverse pour tenter de laisser de
  // la place a un CDC produit notif 0x83 / data 0x82-0x02, et dans cette
  // disposition macOS lie bien AppleUSBNCMData et cree l'interface, mais
  // n'active jamais l'alternate setting 1 : `status: inactive`, aucun bail,
  // zero trame. Le CDC ne rentre de toute facon pas (voir docs/USB_NET.md).
  uint8_t epNotif = tinyusb_get_free_in_endpoint();
  TU_VERIFY(epNotif != 0);
  uint8_t epData = tinyusb_get_free_duplex_endpoint();
  TU_VERIFY(epData != 0);

  uint8_t descriptor[TUD_CDC_NCM_DESC_LEN] = {
    TUD_CDC_NCM_DESCRIPTOR(*itf, strIndex, macIndex, (uint8_t)(0x80 | epNotif), 64, epData,
                           (uint8_t)(0x80 | epData), CFG_TUD_ENDOINT_SIZE, CFG_TUD_NET_MTU)
  };
  *itf += 2;  // interface de controle + interface de donnees
  memcpy(dst, descriptor, TUD_CDC_NCM_DESC_LEN);
  return TUD_CDC_NCM_DESC_LEN;
}

// --- Glue esp_netif -------------------------------------------------------

esp_err_t usbnetTransmit(void* h, void* buffer, size_t len) {
  (void)h;
  if (!s_linkUp || len == 0 || len > CFG_TUD_NET_MTU) {
    return ESP_ERR_INVALID_STATE;
  }
  if (xSemaphoreTake(s_txMutex, kTxWaitTicks) != pdTRUE) {
    s_stats.txTimeouts++;
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t result = ESP_ERR_TIMEOUT;
  s_txSrc = buffer;
  s_txLen = (uint16_t)len;
  const TickType_t deadline = xTaskGetTickCount() + kTxWaitTicks;
  do {
    xSemaphoreTake(s_txDone, 0);                  // purge un eventuel reliquat
    s_txEtat.store(TX_DEMANDE);
    defer(xmitInUsbd, nullptr);                   // dans la tache usbd
    if (xSemaphoreTake(s_txDone, kTxWaitTicks) != pdTRUE) {
      // La tache usbd n'a pas repondu. Pas commencee : on annule, elle ne
      // lira jamais ce tampon. Commencee : une copie bornee, on l'attend.
      uint32_t attendu = TX_DEMANDE;
      if (!s_txEtat.compare_exchange_strong(attendu, TX_ANNULE)) {
        xSemaphoreTake(s_txDone, portMAX_DELAY);
        if (s_txOk) {
          result = ESP_OK;
        }
      }
      break;
    }
    if (s_txOk) {
      result = ESP_OK;
      break;
    }
    vTaskDelay(1);   // tous les NTB d'emission pleins : laisser partir un transfert
  } while (xTaskGetTickCount() < deadline);

  if (result == ESP_OK) {
    s_stats.txFrames++;
  } else {
    s_stats.txTimeouts++;
  }
  xSemaphoreGive(s_txMutex);
  return result;
}

void usbnetFreeRxBuffer(void* h, void* buffer) {
  (void)h;
  free(buffer);
}

esp_err_t usbnetPostAttach(esp_netif_t* netif, esp_netif_iodriver_handle h) {
  esp_netif_driver_ifconfig_t ifconfig = {};
  ifconfig.handle = h;
  ifconfig.transmit = usbnetTransmit;
  ifconfig.driver_free_rx_buffer = usbnetFreeRxBuffer;
  esp_err_t err = esp_netif_set_driver_config(netif, &ifconfig);
  if (err == ESP_OK) {
    s_netif = netif;
  }
  return err;
}

void rxTask(void* arg) {
  (void)arg;
  RxFrame frame;
  for (;;) {
    if (xQueueReceive(s_rxQueue, &frame, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (frame.buf != nullptr) {
      if (s_netif == nullptr) {
        free(frame.buf);
        s_stats.rxDropped++;
      } else {
        // esp_netif_receive prend possession du buffer : lwIP le liberera via
        // usbnetFreeRxBuffer, y compris sur ses chemins d'erreur. Ne jamais
        // free() ici apres l'appel, ce serait un double free.
        esp_netif_receive(s_netif, frame.buf, frame.len, frame.buf);
        s_stats.rxFrames++;
      }
    }
    defer(renewInUsbd, nullptr);   // PAS d'ici : dans la tache usbd
  }
}

}  // namespace

// --- Callbacks TinyUSB (surchargent les weak du core) ----------------------

/**
 * Contrat NCM. ⚠ Ce commentaire affirmait que la valeur de retour etait
 * IGNOREE : c'etait vrai d'une ancienne version de ncm_device.c. Dans celle que
 * compile le core Arduino (TinyUSB 0.20.1), elle est LUE : `true` fait avancer
 * le pilote au datagramme suivant, `false` le lui fait garder. On rend `true` :
 * la trame est copiee, ou jetee et comptee.
 *
 * Ce callback s'execute TOUJOURS dans la tache usbd : il n'est appele que par
 * tud_network_recv_renew(), et celui-ci ne l'est plus que depuis la tache usbd
 * (netd_xfer_cb, ou renewInUsbd via usbd_defer_func). L'appel a renew ci-dessous
 * est donc une re-entree DANS LE MEME FIL — celle que le garde du pilote sait
 * traiter.
 */
extern "C" bool tud_network_recv_cb(const uint8_t* src, uint16_t size) {
  RxFrame frame = {nullptr, 0};

  if (size > 0 && s_rxQueue != nullptr) {
    // En PSRAM : une trame reste allouee jusqu'a ce que lwIP (et le serveur
    // web, pour une requete) l'ait lue ; en RAM interne, ces allocations de
    // toutes tailles se glissaient entre les blocs du tas du plus gros bloc.
    // lwIP range deja les siennes en PSRAM (CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP).
    // free() les rend, d'ou qu'elles viennent.
    void* buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf != nullptr) {
      memcpy(buf, src, size);
      frame.buf = buf;
      frame.len = size;
    } else {
      s_stats.rxDropped++;
    }
  }

  // Une entree nulle vaut « rien a livrer, renouvelle quand meme ».
  if (s_rxQueue == nullptr || xQueueSend(s_rxQueue, &frame, 0) != pdTRUE) {
    free(frame.buf);
    s_stats.rxDropped++;
    // Dernier recours dans la task usbd. La recursion bornee est moins grave
    // qu'un RX bloque.
    tud_network_recv_renew();
  }
  return true;
}

extern "C" uint16_t tud_network_xmit_cb(uint8_t* dst, void* ref, uint16_t arg) {
  // Appele par tud_network_xmit(), dans xmitInUsbd : c'est lui qui signale.
  memcpy(dst, ref, arg);
  return arg;
}

/** Appele par TinyUSB apres chaque depot REUSSI dans sa file (surcharge le weak
 *  de usbd.c). Depuis l'interruption aussi : rien d'autre que compter. */
extern "C" void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
  (void)rhport;
  if (eventid < DCD_EVENT_COUNT) {
    s_evt[eventid]++;
  }
  QueueHandle_t q = (QueueHandle_t)&_usbd_qdef.sq;
  UBaseType_t n;
  if (in_isr) {
    s_evtIsr++;
    s_coeurIsr |= 1u << xPortGetCoreID();
    n = uxQueueMessagesWaitingFromISR(q);
  } else {
    n = uxQueueMessagesWaiting(q);
  }
  if (n > s_queueMax) {
    s_queueMax = (uint16_t)n;
  }
}

namespace nidmi_core {

UsbNetService::UsbNetService() {
  enableInterface();
}

bool UsbNetService::enableInterface() {
  if (s_interfaceEnabled) {
    return true;
  }
  // Pas de deriveMacs() ici : appele depuis un constructeur global, on serait
  // en initialisation statique. Les MAC sont derivees paresseusement, dans le
  // callback de descripteur (execute a USB.begin()) et dans les accesseurs.
  //
  // USB_INTERFACE_CUSTOM : seul slot du core Arduino pour une classe qu'il
  // n'expose pas lui-meme.
  if (tinyusb_enable_interface(USB_INTERFACE_CUSTOM, TUD_CDC_NCM_DESC_LEN, nidmi_usbnet_load_descriptor) !=
      ESP_OK) {
    return false;
  }
  s_interfaceEnabled = true;
  return true;
}

bool UsbNetService::begin(const UsbNetConfig& cfg) {
  if (s_started) {
    return true;
  }
  s_lastStep = UsbNetStep::NotRegistered;
  if (!s_interfaceEnabled) {
    log_e("UsbNetService::enableInterface() doit etre appele avant USB.begin()");
    return false;
  }

  strlcpy(s_ifName, cfg.interfaceName, sizeof(s_ifName));
  s_manageMdns = cfg.manageMdns;

  s_lastStep = UsbNetStep::SyncAlloc;
  s_txMutex = xSemaphoreCreateMutex();
  s_txDone = xSemaphoreCreateBinary();
  s_rxQueue = xQueueCreate(8, sizeof(RxFrame));
  if (s_txMutex == nullptr || s_txDone == nullptr || s_rxQueue == nullptr) {
    return false;
  }

  // Arduino ne les appelle que via WiFi.begin() ; ici le WiFi peut rester eteint.
  s_lastStep = UsbNetStep::NetifInit;
  if (esp_netif_init() != ESP_OK) {
    return false;
  }
  s_lastStep = UsbNetStep::EventLoop;
  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return false;
  }

  static esp_netif_ip_info_t ipInfo;
  ipInfo.ip.addr = esp_ip4addr_aton(cfg.ip);
  ipInfo.netmask.addr = esp_ip4addr_aton(cfg.netmask);
  ipInfo.gw.addr = cfg.advertiseRouter ? ipInfo.ip.addr : 0;

  static esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
  // Cle ETH_DEF, et pas une cle propre : CONFIG_MDNS_MAX_INTERFACES vaut 3
  // dans les libs Arduino et les trois slots sont deja pris par les
  // interfaces predefinies STA / AP / ETH. mdns_register_netif() echoue donc
  // pour toute interface enregistree a l'execution, et ce n'est pas reglable
  // sans reconstruire les libs IDF. En prenant la cle du slot ETH predefini,
  // le composant mdns nous resout via esp_netif_get_handle_from_ifkey().
  //
  // CONTRAINTE : sur cette pile, ce netif USB et un vrai Ethernet ne peuvent
  // pas coexister sous mDNS.
  base.if_key = "ETH_DEF";
  base.if_desc = "usb_ncm";
  base.route_prio = 10;  // sous le WiFi : jamais l'interface par defaut cote ESP
  base.flags = (esp_netif_flags_t)((cfg.dhcpServer ? ESP_NETIF_DHCP_SERVER : 0) | ESP_NETIF_FLAG_AUTOUP);
  base.ip_info = &ipInfo;
  base.get_ip_event = 0;
  base.lost_ip_event = 0;

  esp_netif_config_t netifConfig = {};
  netifConfig.base = &base;
  netifConfig.driver = nullptr;
  netifConfig.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;

  s_lastStep = UsbNetStep::NetifNew;
  esp_netif_t* netif = esp_netif_new(&netifConfig);
  if (netif == nullptr) {
    return false;
  }

  s_lastStep = UsbNetStep::NetifAttach;
  s_driverBase.post_attach = usbnetPostAttach;
  s_driverBase.netif = netif;
  if (esp_netif_attach(netif, &s_driverBase) != ESP_OK) {
    esp_netif_destroy(netif);
    return false;
  }

  esp_netif_set_mac(netif, s_devMac);

  if (cfg.dhcpServer) {
    esp_netif_dhcps_stop(netif);  // les options ne se posent qu'a l'arret
    const uint8_t offer = cfg.advertiseRouter ? 1 : 0;
    esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, (void*)&offer,
                           sizeof(offer));
    esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, (void*)&offer,
                           sizeof(offer));
  }

  esp_netif_action_start(netif, nullptr, 0, nullptr);
  if (cfg.dhcpServer) {
    esp_netif_dhcps_start(netif);
  }

  s_lastStep = UsbNetStep::RxTask;
  const BaseType_t coeurRx = (cfg.rxCore == 0 || cfg.rxCore == 1) ? (BaseType_t)cfg.rxCore : tskNO_AFFINITY;
  // Pile en PSRAM : 4 096 o de RAM interne rendus. La tache ne fait que passer
  // les trames a lwIP (1 196 o de pile au plus, MESURES §150), sous le MIDI et
  // les capteurs ; elle n'ecrit jamais la flash (seule contrainte d'une pile en
  // PSRAM : ne pas tourner cache coupe). Jamais supprimee — sinon ce serait par
  // vTaskDeleteWithCaps.
  if (xTaskCreatePinnedToCoreWithCaps(rxTask, "usbnet_rx", 4096, nullptr, cfg.rxPriority, &s_rxTask, coeurRx,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    return false;
  }

  s_lastStep = UsbNetStep::Ok;
  s_started = true;
  return true;
}

void UsbNetService::update() {
  // Gate sur l'enregistrement du descripteur, PAS sur s_started : l'annonce de
  // lien est une affaire purement USB. La conditionner au succes du netif
  // faisait qu'un echec cote reseau privait l'hote de porteur, le laissait sur
  // l'alternate setting 0, et interdisait tout trafic — sans aucun symptome
  // exploitable.
  if (!s_interfaceEnabled) {
    return;
  }

  if (s_relanceEnCours && millis() - s_relanceDebut >= kRelanceMs) {
    s_relanceEnCours = false;
    defer(connecterDansUsbd, nullptr);
  }

  /* L'ETAT DU LIEN NCM APPARTIENT AU PILOTE — on n'y touche pas.
   * netd_init() le remet a « monte » a CHAQUE reset du bus (lu dans la lib
   * compilee : `s8i 1` a l'octet 53 de ncm_interface), et le pilote l'annonce
   * de lui-meme quand l'hote active l'interface de donnees (alt 1) : VITESSE,
   * puis CONNECTE. Nous annoncions « coupe » au demontage : execute APRES le
   * reset du bus, cela ecrasait le « monte » du pilote, et si l'hote activait
   * l'interface avant notre « monte », il recevait CONNECTE = 0, repassait en
   * alt 0 — ou la 0.20 refuse toute annonce — et ne revenait jamais : en10
   * `inactive`, lien de l'hote 1, vitesse 0 (MESURES §147). Nos re-annonces
   * periodiques, elles, ne faisaient rien : le pilote rend la main quand
   * l'etat ne change pas. */
  const bool mounted = tud_mounted();
  if (mounted != s_linkUp) {
    s_linkUp = mounted;

    if (s_started && s_netif != nullptr) {
      if (mounted) {
        esp_netif_action_connected(s_netif, nullptr, 0, nullptr);
      } else {
        esp_netif_action_disconnected(s_netif, nullptr, 0, nullptr);
      }

      // mDNS seulement maintenant : emise depuis begin(), l'activation ne
      // prend pas, le netif n'etant ni monte ni joignable tant que l'hote n'a
      // pas active l'interface de donnees NCM.
      if (s_manageMdns) {
        if (mounted) {
          mdns_netif_action(s_netif,
                            (mdns_event_actions_t)(MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4));
          s_mdnsAnnounced = true;
          s_announcesLeft = 5;
          s_lastAnnounce = millis();
        } else {
          mdns_netif_action(s_netif, MDNS_EVENT_DISABLE_IP4);
          s_mdnsAnnounced = false;
          s_announcesLeft = 0;
        }
      }
    }
  }

  const uint32_t now = millis();

  // Epingler la tache usbd des que le coeur de l'interruption est CONNU (vu,
  // et unique) — aux premiers evenements de l'enumeration, bien avant le
  // premier NTB.
  if (!s_epinglageDemande && s_coeurIsr != 0 && (s_coeurIsr & (s_coeurIsr - 1)) == 0) {
    s_epinglageDemande = true;
    defer(epinglerUsbd, (void*)(intptr_t)(s_coeurIsr == 1u ? 0 : 1));
  }

  // Meme raison cote mDNS : la pile de l'hote peut n'etre prete qu'apres le
  // bail DHCP.
  if (s_mdnsAnnounced && s_announcesLeft > 0 && now - s_lastAnnounce > 3000) {
    s_lastAnnounce = now;
    s_announcesLeft--;
    mdns_netif_action(s_netif, MDNS_EVENT_ANNOUNCE_IP4);
  }
}

bool UsbNetService::isLinkUp() const {
  return s_linkUp;
}

UsbNetStep UsbNetService::lastStep() const {
  return s_lastStep;
}

UsbNetStats UsbNetService::stats() const {
  return s_stats;
}

namespace {
ip4_addr_t s_cibleSonde;   // ecrite avant de poster, lue dans la tache lwIP

bool bailHote(esp_ip4_addr_t* ip) {
  if (!s_started || s_netif == nullptr) {
    return false;
  }
  esp_netif_pair_mac_ip_t paire = {};
  memcpy(paire.mac, s_hostMac, sizeof(paire.mac));
  if (esp_netif_dhcps_get_clients_by_mac(s_netif, 1, &paire) != ESP_OK || paire.ip.addr == 0) {
    return false;
  }
  if (ip != nullptr) {
    *ip = paire.ip;
  }
  return true;
}

// etharp_request() n'est pas reentrant : il s'execute dans la tache lwIP.
void sonderDansLwip(void*) {
  struct netif* n = (struct netif*)esp_netif_get_netif_impl(s_netif);
  if (n != nullptr) {
    etharp_request(n, &s_cibleSonde);
  }
}
}  // namespace

bool UsbNetService::hoteConnu() const {
  return bailHote(nullptr);
}

bool UsbNetService::relancerEnumeration() {
  if (!s_interfaceEnabled || s_relanceEnCours) {
    return false;
  }
  s_relanceDebut = millis();
  s_relanceEnCours = true;
  s_relances++;
  defer(deconnecterDansUsbd, nullptr);
  return true;
}

bool UsbNetService::relanceEnCours() const {
  return s_relanceEnCours;
}

uint32_t UsbNetService::relances() const {
  return s_relances;
}

bool UsbNetService::reseauActif() const {
  return s_interfaceEnabled && nidmi_ncm_reseau_actif();
}

bool UsbNetService::sonderHote() {
  esp_ip4_addr_t ip = {};
  if (!s_linkUp || !bailHote(&ip)) {
    return false;
  }
  s_cibleSonde.addr = ip.addr;
  return tcpip_try_callback(sonderDansLwip, nullptr) == ERR_OK;
}

IPAddress UsbNetService::localIp() const {
  if (s_netif == nullptr) {
    return IPAddress((uint32_t)0);
  }
  esp_netif_ip_info_t info = {};
  if (esp_netif_get_ip_info(s_netif, &info) != ESP_OK) {
    return IPAddress((uint32_t)0);
  }
  return IPAddress(info.ip.addr);
}

String UsbNetService::broadcastAddress() const {
  if (s_netif == nullptr) {
    return String();
  }
  esp_netif_ip_info_t info = {};
  if (esp_netif_get_ip_info(s_netif, &info) != ESP_OK) {
    return String();
  }
  const uint32_t bcast = (info.ip.addr & info.netmask.addr) | ~info.netmask.addr;
  return IPAddress(bcast).toString();
}

const char* UsbNetService::deviceMac() const {
  deriveMacs();
  return s_devMacStr;
}

const char* UsbNetService::hostMac() const {
  deriveMacs();
  return s_hostMacStr;
}

esp_netif_t* UsbNetService::netif() const {
  return s_netif;
}

/* Registres du controleur USB (Synopsys DWC2) de l'ESP32-S3, en LECTURE seule.
 * Base et decalages : portable/synopsys/dwc2/dwc2_esp32.h et dwc2_type.h. On
 * n'y lit ni GRXSTSP (lire la file de reception la DEPILE) ni les FIFO. */
namespace {
constexpr uintptr_t kDwc2Base = 0x60080000UL;
inline uint32_t dwc2(uint32_t off) {
  return *(volatile uint32_t*)(kDwc2Base + off);
}
void hex(String& j, const char* cle, uint32_t v, bool virgule = true) {
  char b[32];
  snprintf(b, sizeof(b), "\"%s\":\"%08lx\"", cle, (unsigned long)v);
  if (virgule) {
    j += ',';
  }
  j += b;
}
void ep(String& j, uint8_t addr) {
  j += "{\"busy\":";
  j += usbd_edpt_busy(0, addr) ? "true" : "false";
  j += ",\"stall\":";
  j += usbd_edpt_stalled(0, addr) ? "true" : "false";
  j += '}';
}
}  // namespace

String UsbNetService::diagJson() const {
  QueueHandle_t q = (QueueHandle_t)&_usbd_qdef.sq;
  String j = "{\"file\":{\"taille\":" + String(_usbd_qdef.depth);
  j += ",\"maintenant\":" + String((unsigned)uxQueueMessagesWaiting(q));
  j += ",\"max\":" + String((unsigned)s_queueMax);
  j += ",\"par_type\":[";
  for (int i = 0; i < DCD_EVENT_COUNT; ++i) {
    if (i) {
      j += ',';
    }
    j += String((unsigned long)s_evt[i]);
  }
  j += "],\"depuis_interruption\":" + String((unsigned long)s_evtIsr) + "}";
  j += ",\"depots\":{\"n\":" + String((unsigned long)s_deferPosted);
  j += ",\"lents\":" + String((unsigned long)s_deferSlow);
  j += ",\"max_us\":" + String((unsigned long)s_deferMaxUs) + "}";
  j += ",\"coeurs\":{\"interruption\":" + String((unsigned long)s_coeurIsr);
  j += ",\"usbd\":[" + String((unsigned long)s_coeurUsbd[0]) + "," + String((unsigned long)s_coeurUsbd[1]) + "]";
  j += ",\"epingle\":" + String((int)s_coeurEpingle) + "}";
  j += ",\"monte\":";
  j += tud_mounted() ? "true" : "false";
  j += ",\"suspendu\":";
  j += tud_suspended() ? "true" : "false";
  j += ",\"ep83\":";
  ep(j, 0x83);
  j += ",\"ep03\":";
  ep(j, 0x03);
  j += ",\"ep82\":";
  ep(j, 0x82);
  j += ",\"reg\":{";
  hex(j, "gintsts", dwc2(0x014), false);
  hex(j, "gintmsk", dwc2(0x018));
  hex(j, "grxfsiz", dwc2(0x024));
  hex(j, "gnptxfsiz", dwc2(0x028));
  hex(j, "gnptxsts", dwc2(0x02C));
  hex(j, "dctl", dwc2(0x804));
  hex(j, "dsts", dwc2(0x808));
  hex(j, "diepmsk", dwc2(0x810));
  hex(j, "doepmsk", dwc2(0x814));
  hex(j, "daint", dwc2(0x818));
  hex(j, "daintmsk", dwc2(0x81C));
  hex(j, "diepempmsk", dwc2(0x834));
  // Par point d'acces IN n : [ctl, int, tsiz, txfsts, taille de sa FIFO]
  j += ",\"in\":[";
  for (uint32_t n = 0; n < 5; ++n) {
    const uint32_t b = 0x900 + 0x20 * n;
    char l[80];
    snprintf(l, sizeof(l), "%s[\"%08lx\",\"%08lx\",\"%08lx\",\"%08lx\",\"%08lx\"]", n ? "," : "",
             (unsigned long)dwc2(b), (unsigned long)dwc2(b + 0x08), (unsigned long)dwc2(b + 0x10),
             (unsigned long)dwc2(b + 0x18), (unsigned long)(n ? dwc2(0x104 + 4 * (n - 1)) : dwc2(0x028)));
    j += l;
  }
  // Par point d'acces OUT n : [ctl, int, tsiz]
  j += "],\"out\":[";
  for (uint32_t n = 0; n < 4; ++n) {
    const uint32_t b = 0xB00 + 0x20 * n;
    char l[48];
    snprintf(l, sizeof(l), "%s[\"%08lx\",\"%08lx\",\"%08lx\"]", n ? "," : "", (unsigned long)dwc2(b),
             (unsigned long)dwc2(b + 0x08), (unsigned long)dwc2(b + 0x10));
    j += l;
  }
  j += "]}";
  // L'etat du pilote NCM (`ncm_interface`), expose par notre copie du pilote
  // (tinyusb/ncm_device.c, nidmi_ncm_etat). Le lecteur valide la zone : elle
  // doit commencer par les points d'acces 83 03 82. Suivi du tampon
  // d'emission en cours, s'il pointe en DRAM : ses 32 premiers octets (NTH16 +
  // debut de NDP16 — longueur du bloc et datagrammes).
  size_t taille = 0;
  const uint8_t* p = (const uint8_t*)nidmi_ncm_etat(&taille);
  auto dump = [&j](const uint8_t* a, int n) {
    for (int i = 0; i < n; ++i) {
      char b[3];
      snprintf(b, sizeof(b), "%02x", a[i]);
      j += b;
    }
  };
  j += ",\"ncm\":\"";
  dump(p, (int)taille);
  j += "\"";
  uintptr_t enCours;
  memcpy(&enCours, p + 36, sizeof(enCours));   // xmit_tinyusb_ntb
  if (enCours >= 0x3FC88000UL && enCours + 32 <= 0x3FD00000UL) {
    j += ",\"ntb_emission\":\"";
    dump((const uint8_t*)enCours, 32);
    j += "\"";
  }
  // La trace du pilote : [ms, quoi, valeur], du plus ancien au plus recent.
  uint32_t evMs[16];
  char evQuoi[16];
  uint8_t evVal[16];
  uint32_t evTotal = 0;
  const uint32_t nEv = nidmi_ncm_evenements(evMs, evQuoi, evVal, &evTotal);
  j += ",\"ncm_evts_total\":" + String((unsigned long)evTotal) + ",\"ncm_evts\":[";
  for (uint32_t i = 0; i < nEv; ++i) {
    if (i) j += ',';
    j += "[" + String((unsigned long)evMs[i]) + ",\"" + String(evQuoi[i]) + "\"," + String((unsigned)evVal[i]) + "]";
  }
  j += "],\"maintenant_ms\":" + String((unsigned long)xTaskGetTickCount());
  j += "}";
  return j;
}

}  // namespace nidmi_core

#else  // cible sans USB-OTG (ESP32-C3) ou compilee en usb_mode=1

namespace nidmi_core {

UsbNetService::UsbNetService() {}
bool UsbNetService::enableInterface() {
  return false;
}
bool UsbNetService::begin(const UsbNetConfig&) {
  return false;
}
void UsbNetService::update() {}
bool UsbNetService::isLinkUp() const {
  return false;
}
UsbNetStep UsbNetService::lastStep() const {
  return UsbNetStep::NotRegistered;
}
UsbNetStats UsbNetService::stats() const {
  return UsbNetStats();
}
bool UsbNetService::hoteConnu() const {
  return false;
}
bool UsbNetService::sonderHote() {
  return false;
}
bool UsbNetService::relancerEnumeration() {
  return false;
}
bool UsbNetService::relanceEnCours() const {
  return false;
}
uint32_t UsbNetService::relances() const {
  return 0;
}
bool UsbNetService::reseauActif() const {
  return false;
}
IPAddress UsbNetService::localIp() const {
  return IPAddress((uint32_t)0);
}
String UsbNetService::broadcastAddress() const {
  return String();
}
const char* UsbNetService::deviceMac() const {
  return "";
}
const char* UsbNetService::hostMac() const {
  return "";
}
String UsbNetService::diagJson() const {
  return String("{}");
}

}  // namespace nidmi_core

#endif
