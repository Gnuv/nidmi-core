#include "UsbNcmNet.h"

#if NIDMI_USB_NCM_SUPPORTED

#include <string.h>

#include "esp32-hal-tinyusb.h"
#include "class/net/net_device.h"

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

// --- Identite -------------------------------------------------------------
// Deux MAC distinctes : une pour le netif ESP32, une annoncee a l'hote via le
// descripteur. Les faire identiques casse l'ARP sur le lien.
uint8_t s_devMac[6] = {0};
uint8_t s_hostMac[6] = {0};
char s_devMacStr[18] = {0};   // aa:bb:cc:dd:ee:ff
char s_hostMacStr[13] = {0};  // AABBCCDDEEFF — format impose par NCM (iMACAddress)
char s_ifDescription[40] = "NiDMI USB Network";

// --- Etat -----------------------------------------------------------------
esp_netif_t* s_netif = nullptr;
esp_netif_driver_base_t s_driverBase = {};
bool s_linkUp = false;
bool s_started = false;
nidmi_spike::UsbNcmStep s_lastStep = nidmi_spike::UsbNcmStep::NotRegistered;

nidmi_spike::UsbNcmStats s_stats = {0, 0, 0, 0};

// --- Chemin RX ------------------------------------------------------------
// tud_network_recv_cb s'execute dans la task usbd. On copie la trame, on la
// pousse dans une queue, et une task dediee la remet a lwIP puis appelle
// tud_network_recv_renew(). Renouveler directement dans le callback ferait
// recurser recv_renew -> recv_cb autant de fois qu'il y a de datagrammes dans
// le NTB NCM courant.
struct RxFrame {
  void* buf;
  uint16_t len;
};
QueueHandle_t s_rxQueue = nullptr;
TaskHandle_t s_rxTask = nullptr;

// --- Chemin TX ------------------------------------------------------------
// usbnet_transmit s'execute dans la task tcpip et doit rendre la main
// seulement quand TinyUSB a fini de lire le buffer. On recopie dans un buffer
// possede, et on attend le semaphore rendu par tud_network_xmit_cb.
uint8_t s_txBuf[CFG_TUD_NET_MTU];
SemaphoreHandle_t s_txMutex = nullptr;
SemaphoreHandle_t s_txDone = nullptr;

constexpr TickType_t kTxWaitTicks = pdMS_TO_TICKS(100);

void formatMacColon(const uint8_t mac[6], char* out) {
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void formatMacNcm(const uint8_t mac[6], char* out) {
  snprintf(out, 13, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void deriveMacs() {
  if (s_devMacStr[0]) {
    return;
  }
  esp_read_mac(s_devMac, ESP_MAC_ETH);
  memcpy(s_hostMac, s_devMac, 6);
  // Meme OUI, dernier octet decale : deux stations distinctes sur le lien.
  s_hostMac[5] = (uint8_t)(s_hostMac[5] ^ 0x01);
  formatMacColon(s_devMac, s_devMacStr);
  formatMacNcm(s_hostMac, s_hostMacStr);
}

// --- Descripteur ----------------------------------------------------------
bool s_descriptorLoaded = false;
bool s_interfaceEnabled = false;

extern "C" uint16_t tusb_ncm_load_descriptor(uint8_t* dst, uint8_t* itf) {
  if (s_descriptorLoaded) {
    return 0;
  }
  s_descriptorLoaded = true;

  deriveMacs();

  uint8_t strIndex = tinyusb_add_string_descriptor(s_ifDescription);
  uint8_t macIndex = tinyusb_add_string_descriptor(s_hostMacStr);

  // NCM : 1 endpoint IN interrupt (notification) + 1 bulk IN + 1 bulk OUT.
  //
  // Allocation SEPAREE des deux bulk, pas via tinyusb_get_free_duplex_endpoint().
  // Le duplex impose le meme index en entree et en sortie, ce qui ne passe plus
  // des que CDC est actif : CDC reserve OUT3 / IN4 / IN5, MIDI prend le 1, la
  // notification le 2, et il ne reste aucun index libre des deux cotes.
  // En allouant separement on obtient IN3 / OUT2 et tout rentre :
  //   MIDI IN1/OUT1, NCM notif IN2, NCM data IN3/OUT2, CDC OUT3/IN4/IN5
  //   soit 4 FIFO IN reels (IN5 n'en consomme pas) = la limite exacte du S3.
  // C'est ce qui rend possible une console CDC, donc le flash par auto-reset.
  uint8_t epNotif = tinyusb_get_free_in_endpoint();
  TU_VERIFY(epNotif != 0);
  uint8_t epIn = tinyusb_get_free_in_endpoint();
  TU_VERIFY(epIn != 0);
  uint8_t epOut = tinyusb_get_free_out_endpoint();
  TU_VERIFY(epOut != 0);

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
      // on attend la recopie dans les deux cas plutot que de parier sur l'un.
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
    // Autorise TinyUSB a nous livrer le datagramme suivant. Un renew par
    // trame acceptee, jamais plus.
    tud_network_recv_renew();
  }
}

}  // namespace

// --- Callbacks TinyUSB (surchargent les weak du core) ----------------------

/**
 * Contrat NCM, verifie dans ncm_device.c : la valeur de retour est IGNOREE
 * (contrairement a ECM/RNDIS qui represente la trame si on rend false), et
 * tud_network_recv_renew() rappelle directement tud_network_recv_cb() pour le
 * datagramme suivant du NTB. Deux consequences :
 *   - exactement un renew par appel, y compris quand on jette la trame ;
 *     en oublier un fige le RX definitivement ;
 *   - renouveler ici meme ferait recurser d'un cran par datagramme du NTB,
 *     d'ou le passage par la queue et rxTask.
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
    // Dernier recours dans la task usbd. Le renew ne peut pas etre saute :
    // la recursion bornee est moins grave qu'un RX bloque.
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
  // Le lien repart de zero a chaque reconfiguration par l'hote.
  s_linkUp = false;
}

namespace nidmi_spike {

bool usbNcmEnableInterface() {
  if (s_interfaceEnabled) {
    return true;
  }
  deriveMacs();
  // USB_INTERFACE_CUSTOM est le seul slot libre du core pour une classe que
  // l'Arduino core n'expose pas lui-meme.
  if (tinyusb_enable_interface(USB_INTERFACE_CUSTOM, TUD_CDC_NCM_DESC_LEN, tusb_ncm_load_descriptor) != ESP_OK) {
    return false;
  }
  s_interfaceEnabled = true;
  return true;
}

bool usbNcmBegin(const UsbNcmConfig& cfg) {
  if (s_started) {
    return true;
  }
  s_lastStep = UsbNcmStep::NotRegistered;
  if (!s_interfaceEnabled) {
    log_e("usbNcmEnableInterface() doit etre appele avant USB.begin()");
    return false;
  }

  strlcpy(s_ifDescription, cfg.ifDescription, sizeof(s_ifDescription));

  s_lastStep = UsbNcmStep::SyncAlloc;
  s_txMutex = xSemaphoreCreateMutex();
  s_txDone = xSemaphoreCreateBinary();
  s_rxQueue = xQueueCreate(8, sizeof(RxFrame));
  if (s_txMutex == nullptr || s_txDone == nullptr || s_rxQueue == nullptr) {
    return false;
  }

  // Arduino ne les appelle que via WiFi.begin() ; ici on n'utilise pas le WiFi.
  s_lastStep = UsbNcmStep::NetifInit;
  esp_err_t err = esp_netif_init();
  if (err != ESP_OK) {
    return false;
  }
  s_lastStep = UsbNcmStep::EventLoop;
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return false;
  }

  static esp_netif_ip_info_t ipInfo;
  ipInfo.ip.addr = esp_ip4addr_aton(cfg.ip);
  ipInfo.netmask.addr = esp_ip4addr_aton(cfg.netmask);
  // Passerelle nulle et volontaire : sans option router dans le bail DHCP,
  // l'hote ne peut pas prendre le lien USB comme route par defaut et perdre
  // son acces Internet.
  ipInfo.gw.addr = 0;

  static esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
  // Mesure : avec une cle custom ("USB_NCM"), mdns_register_netif() echoue.
  // CONFIG_MDNS_MAX_INTERFACES=3 dans les libs Arduino, et les trois slots
  // sont deja pris par les interfaces predefinies STA / AP / ETH — il n'en
  // reste aucun pour une interface enregistree a l'execution, et ce n'est pas
  // reglable sans reconstruire les libs IDF.
  // On prend donc la cle du slot ETH predefini, que le composant mdns resout
  // via esp_netif_get_handle_from_ifkey(). Corollaire a retenir : sur cette
  // pile, un netif USB et un vrai Ethernet ne peuvent pas coexister sous mDNS.
  base.if_key = "ETH_DEF";
  base.if_desc = "usb_ncm";
  base.route_prio = 10;  // sous le WiFi : jamais l'interface par defaut cote ESP
  base.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);
  base.ip_info = &ipInfo;
  base.get_ip_event = 0;
  base.lost_ip_event = 0;

  esp_netif_config_t netifConfig = {};
  netifConfig.base = &base;
  netifConfig.driver = nullptr;
  netifConfig.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;

  s_lastStep = UsbNcmStep::NetifNew;
  esp_netif_t* netif = esp_netif_new(&netifConfig);
  if (netif == nullptr) {
    return false;
  }

  s_lastStep = UsbNcmStep::NetifAttach;
  s_driverBase.post_attach = usbnetPostAttach;
  s_driverBase.netif = netif;
  if (esp_netif_attach(netif, &s_driverBase) != ESP_OK) {
    esp_netif_destroy(netif);
    return false;
  }

  esp_netif_set_mac(netif, s_devMac);

  if (cfg.dhcpServer) {
    esp_netif_dhcps_stop(netif);  // les options ne se posent qu'a l'arret
    uint8_t off = 0;
    esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &off, sizeof(off));
    esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &off, sizeof(off));
  }

  esp_netif_action_start(netif, nullptr, 0, nullptr);
  if (cfg.dhcpServer) {
    esp_netif_dhcps_start(netif);
  }

  s_lastStep = UsbNcmStep::RxTask;
  if (xTaskCreate(rxTask, "usbncm_rx", 4096, nullptr, 12, &s_rxTask) != pdPASS) {
    return false;
  }

  s_lastStep = UsbNcmStep::Ok;
  s_started = true;
  return true;
}

UsbNcmStep usbNcmLastStep() {
  return s_lastStep;
}

void usbNcmUpdate() {
  // Volontairement gate sur l'enregistrement du descripteur, PAS sur
  // s_started : l'annonce de lien est une affaire purement USB. La lier au
  // succes du netif faisait qu'un echec cote reseau laissait l'hote sans
  // porteur, donc sur alt 0, donc sans aucun trafic — et sans moyen de le voir.
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
    }
  }

  // Certains hotes n'activent l'interface de donnees (alt 1) qu'apres avoir
  // recu la notification NETWORK_CONNECTION. Si elle part avant que l'hote
  // ait fini de configurer, elle est perdue et personne ne relance : on la
  // re-affirme tant que le lien est cense etre monte.
  static uint32_t lastAssert = 0;
  if (s_linkUp && millis() - lastAssert > 1000) {
    lastAssert = millis();
    tud_network_link_state(0, true);
  }
}

bool usbNcmIsLinkUp() {
  return s_linkUp;
}

const char* usbNcmDeviceMac() {
  deriveMacs();
  return s_devMacStr;
}

const char* usbNcmHostMac() {
  deriveMacs();
  return s_hostMacStr;
}

UsbNcmStats usbNcmStats() {
  return s_stats;
}

esp_netif_t* usbNcmNetif() {
  return s_netif;
}

}  // namespace nidmi_spike

#else  // !NIDMI_USB_NCM_SUPPORTED

namespace nidmi_spike {

bool usbNcmEnableInterface() {
  return false;
}
bool usbNcmBegin(const UsbNcmConfig&) {
  return false;
}
UsbNcmStep usbNcmLastStep() {
  return UsbNcmStep::NotRegistered;
}
void usbNcmUpdate() {}
bool usbNcmIsLinkUp() {
  return false;
}
const char* usbNcmDeviceMac() {
  return "";
}
const char* usbNcmHostMac() {
  return "";
}
UsbNcmStats usbNcmStats() {
  return {0, 0, 0, 0};
}

}  // namespace nidmi_spike

#endif
