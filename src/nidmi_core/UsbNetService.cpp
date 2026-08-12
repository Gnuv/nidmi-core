#include "UsbNetService.h"

#if NIDMI_USB_NET_SUPPORTED

#include <string.h>

#include "class/net/net_device.h"
#include "esp32-hal-tinyusb.h"

#include <esp_event.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_defaults.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
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
uint32_t s_lastLinkAssert = 0;
uint32_t s_lastAnnounce = 0;
uint8_t s_announcesLeft = 0;

nidmi_core::UsbNetStep s_lastStep = nidmi_core::UsbNetStep::NotRegistered;
nidmi_core::UsbNetStats s_stats;

// --- Chemin RX ------------------------------------------------------------
// tud_network_recv_cb s'execute dans la task usbd. On copie la trame, on la
// pousse dans une queue, et une task dediee la remet a lwIP puis renouvelle.
// Renouveler dans le callback ferait recurser recv_renew -> recv_cb autant de
// fois qu'il y a de datagrammes dans le NTB NCM courant.
struct RxFrame {
  void* buf;
  uint16_t len;
};
QueueHandle_t s_rxQueue = nullptr;
TaskHandle_t s_rxTask = nullptr;

// --- Chemin TX ------------------------------------------------------------
uint8_t s_txBuf[CFG_TUD_NET_MTU];
SemaphoreHandle_t s_txMutex = nullptr;
SemaphoreHandle_t s_txDone = nullptr;

constexpr TickType_t kTxWaitTicks = pdMS_TO_TICKS(100);

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

  // L'ORDRE COMPTE. On alloue d'abord la paire de donnees en duplex, la
  // notification ensuite.
  //
  // Mesure : avec la notification en premier, elle prend IN2, et le duplex ne
  // trouve plus d'index libre des deux cotes des que CDC est actif (il reserve
  // OUT3/IN4/IN5). On retombe alors sur des bulk desapparies (IN3/OUT2), et
  // macOS refuse d'activer l'alternate setting 1 : interface presente,
  // `status: inactive`, zero trame, alors que le peripherique annonce
  // pourtant son lien.
  //
  // En prenant le duplex d'abord, l'index 2 est libre des deux cotes dans les
  // deux configurations :
  //   sans CDC : MIDI IN1/OUT1, NCM data IN2/OUT2, NCM notif IN3
  //   avec CDC : idem + CDC OUT3/IN4/IN5, soit 5 IN dont 4 FIFO reels
  uint8_t epData = tinyusb_get_free_duplex_endpoint();
  uint8_t epIn = epData;
  uint8_t epOut = epData;
  if (epData == 0) {
    // Repli si un jour la configuration ne laisse plus d'index duplex libre.
    // Non valide cote hote : voir la mesure ci-dessus.
    epIn = tinyusb_get_free_in_endpoint();
    epOut = tinyusb_get_free_out_endpoint();
  }
  TU_VERIFY(epIn != 0 && epOut != 0);

  uint8_t epNotif = tinyusb_get_free_in_endpoint();
  TU_VERIFY(epNotif != 0);

  uint8_t descriptor[TUD_CDC_NCM_DESC_LEN] = {
    TUD_CDC_NCM_DESCRIPTOR(*itf, strIndex, macIndex, (uint8_t)(0x80 | epNotif), 64, epOut,
                           (uint8_t)(0x80 | epIn), CFG_TUD_ENDOINT_SIZE, CFG_TUD_NET_MTU)
  };
  *itf += 2;  // interface de controle + interface de donnees
  memcpy(dst, descriptor, TUD_CDC_NCM_DESC_LEN);
  return TUD_CDC_NCM_DESC_LEN;
}

// --- Glue esp_netif -------------------------------------------------------

esp_err_t usbnetTransmit(void* h, void* buffer, size_t len) {
  (void)h;
  if (!s_linkUp || len == 0 || len > sizeof(s_txBuf)) {
    return ESP_ERR_INVALID_STATE;
  }
  if (xSemaphoreTake(s_txMutex, kTxWaitTicks) != pdTRUE) {
    s_stats.txTimeouts++;
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t result = ESP_ERR_TIMEOUT;
  const TickType_t deadline = xTaskGetTickCount() + kTxWaitTicks;
  while (xTaskGetTickCount() < deadline) {
    if (tud_network_can_xmit((uint16_t)len)) {
      memcpy(s_txBuf, buffer, len);
      xSemaphoreTake(s_txDone, 0);  // purge un eventuel reliquat
      tud_network_xmit(s_txBuf, (uint16_t)len);
      // xmit_cb peut etre synchrone ou differe selon la version de TinyUSB :
      // on attend la recopie dans les deux cas plutot que de parier.
      if (xSemaphoreTake(s_txDone, kTxWaitTicks) == pdTRUE) {
        result = ESP_OK;
      }
      break;
    }
    vTaskDelay(1);
  }

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
    tud_network_recv_renew();
  }
}

}  // namespace

// --- Callbacks TinyUSB (surchargent les weak du core) ----------------------

/**
 * Contrat NCM, verifie dans ncm_device.c : la valeur de retour est IGNOREE
 * (contrairement a ECM/RNDIS qui represente la trame si on rend false), et
 * tud_network_recv_renew() rappelle directement tud_network_recv_cb() pour le
 * datagramme suivant du NTB. Donc exactement un renew par appel, y compris
 * quand on jette la trame : en oublier un fige le RX definitivement.
 */
extern "C" bool tud_network_recv_cb(const uint8_t* src, uint16_t size) {
  RxFrame frame = {nullptr, 0};

  if (size > 0 && s_rxQueue != nullptr) {
    void* buf = malloc(size);
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
  memcpy(dst, ref, arg);
  if (s_txDone != nullptr) {
    xSemaphoreGive(s_txDone);
  }
  return arg;
}

extern "C" void tud_network_init_cb(void) {
  s_linkUp = false;
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
  if (xTaskCreate(rxTask, "usbnet_rx", 4096, nullptr, 12, &s_rxTask) != pdPASS) {
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

  const bool mounted = tud_mounted();
  if (mounted != s_linkUp) {
    s_linkUp = mounted;
    tud_network_link_state(0, mounted);

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

  // Certains hotes n'activent l'interface de donnees qu'apres avoir recu la
  // notification NETWORK_CONNECTION. Emise une seule fois, elle se perd si
  // l'hote n'a pas fini de se configurer, et personne ne la relance.
  const uint32_t now = millis();
  if (s_linkUp && now - s_lastLinkAssert > 1000) {
    s_lastLinkAssert = now;
    tud_network_link_state(0, true);
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

}  // namespace nidmi_core

#endif
