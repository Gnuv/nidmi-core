/**
 * SPIKE nidmi-core — USB-MIDI + interface reseau USB (CDC-NCM) sur ESP32-S3.
 *
 * Valide les etapes 1 a 3 avant tout ajout dans nidmi-core :
 *   1. le composite MIDI+NCM enumere, le MIDI reste visible cote hote
 *   2. netif + DHCP : l'hote obtient une IP, le ping passe
 *   3. HTTP + mDNS : http://nidmi-usb.local repond via le cable
 *
 * Le WiFi n'est jamais demarre : tout ce qui repond ici passe par l'USB.
 *
 * Build : ./spike/usbnet/build.sh
 */

#include <Arduino.h>

#include "UsbNcmNet.h"

#if NIDMI_USB_NCM_SUPPORTED

#include <ESPmDNS.h>
#include <USB.h>
#include <USBMIDI.h>
#include <WebServer.h>
#include <mdns.h>

using namespace nidmi_spike;

static USBMIDI usbMidi;
static WebServer server(80);

static const char* kHostname = "nidmi-usb";

// Journal circulaire : avec usb_mode=0 il n'y a pas de CDC, le port serie sort
// sur UART0 (D6/D7). /log evite d'avoir a brancher un adaptateur USB-TTL une
// fois que l'etape 2 fonctionne.
static String logBuffer;

static void logLine(const String& line) {
  Serial.println(line);
  logBuffer += line;
  logBuffer += '\n';
  if (logBuffer.length() > 4000) {
    logBuffer.remove(0, logBuffer.length() - 4000);
  }
}

static String statusText() {
  const UsbNcmStats stats = usbNcmStats();
  esp_netif_ip_info_t ip = {};
  if (usbNcmNetif() != nullptr) {
    esp_netif_get_ip_info(usbNcmNetif(), &ip);
  }

  String out;
  out += "link      : ";
  out += usbNcmIsLinkUp() ? "up" : "down";
  out += "\nmac dev   : ";
  out += usbNcmDeviceMac();
  out += "\nmac host  : ";
  out += usbNcmHostMac();
  out += "\nip        : " + IPAddress(ip.ip.addr).toString();
  out += "\nnetmask   : " + IPAddress(ip.netmask.addr).toString();
  out += "\ngateway   : " + IPAddress(ip.gw.addr).toString() + "  (0.0.0.0 attendu)";
  out += "\nhostname  : ";
  out += kHostname;
  out += ".local\nrx frames : " + String(stats.rxFrames);
  out += "\nrx dropped: " + String(stats.rxDropped);
  out += "\ntx frames : " + String(stats.txFrames);
  out += "\ntx timeout: " + String(stats.txTimeouts);
  out += "\nheap free : " + String(ESP.getFreeHeap());
  out += "\nuptime    : " + String(millis() / 1000) + " s\n";
  return out;
}

static void handleRoot() {
  String page = F(
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>NiDMI USB spike</title>"
    "<style>body{font:14px/1.5 system-ui,sans-serif;margin:2rem;max-width:40rem}"
    "pre{background:#f4f4f5;padding:1rem;overflow-x:auto}"
    "@media(prefers-color-scheme:dark){body{background:#18181b;color:#e4e4e7}pre{background:#27272a}}</style>"
    "<h1>NiDMI &mdash; spike USB NCM</h1>"
    "<p>Cette page arrive par le c&acirc;ble USB. Le WiFi n'a jamais &eacute;t&eacute; d&eacute;marr&eacute;.</p><pre>");
  page += statusText();
  page += F("</pre><p><a href=/log>journal</a> &middot; <a href=/midi>envoyer une note MIDI</a></p>");
  server.send(200, "text/html; charset=utf-8", page);
}

static void handleStatus() {
  server.send(200, "text/plain; charset=utf-8", statusText());
}

static void handleLog() {
  server.send(200, "text/plain; charset=utf-8", logBuffer);
}

// Preuve que les deux classes cohabitent : une requete HTTP arrivee par NCM
// declenche une note sur l'interface USB-MIDI du meme cable.
static void handleMidi() {
  usbMidi.noteOn(60, 100, 1);
  delay(120);
  usbMidi.noteOff(60, 0, 1);
  logLine("HTTP -> note MIDI C4 envoyee sur USB-MIDI");
  server.send(200, "text/plain; charset=utf-8", "note C4 envoyee sur USB-MIDI\n");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  logLine("");
  logLine("=== spike USB NCM + USB MIDI ===");

  // Ordre impose : les descripteurs doivent tous etre enregistres avant
  // USB.begin(). Le constructeur d'USBMIDI a deja pose le sien.
  if (!usbNcmEnableInterface()) {
    logLine("ERREUR: enregistrement du descripteur NCM refuse");
    return;
  }
  logLine("descripteur NCM enregistre");

  USB.productName("NiDMI");
  USB.manufacturerName("NiDMI");
  // Composite avec IAD : sans cette triplette, Windows refuse de lier les
  // interfaces NCM. Sans effet visible sur macOS et Linux.
  USB.usbClass(TUSB_CLASS_MISC);
  USB.usbSubClass(MISC_SUBCLASS_COMMON);
  USB.usbProtocol(MISC_PROTOCOL_IAD);
  USB.begin();
  logLine("USB.begin() ok");

  UsbNcmConfig cfg;
  cfg.ifDescription = "NiDMI USB Network";
  cfg.ip = "192.168.7.1";
  cfg.netmask = "255.255.255.0";
  if (!usbNcmBegin(cfg)) {
    logLine("ERREUR: usbNcmBegin() a echoue");
    return;
  }
  logLine(String("netif usb up, ip 192.168.7.1, mac dev ") + usbNcmDeviceMac());
  logLine(String("mac annoncee a l'hote: ") + usbNcmHostMac());

  // Etape 3 : mDNS. Un netif a if_key custom n'est pas gere automatiquement
  // par le composant mdns — sans ces deux appels le serveur repond en IP mais
  // nidmi-usb.local ne resout pas.
  if (mdns_init() == ESP_OK) {
    mdns_hostname_set(kHostname);
    mdns_instance_name_set("NiDMI USB spike");
    if (mdns_register_netif(usbNcmNetif()) == ESP_OK) {
      mdns_netif_action(usbNcmNetif(), (mdns_event_actions_t)(MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4));
      logLine("mdns: netif usb enregistre");
    } else {
      logLine("ERREUR: mdns_register_netif a echoue");
    }
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
  } else {
    logLine("ERREUR: mdns_init a echoue");
  }

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/log", handleLog);
  server.on("/midi", handleMidi);
  server.begin();
  logLine("http: en ecoute sur :80");
  logLine("attendu: http://192.168.7.1/ et http://nidmi-usb.local/");
}

void loop() {
  usbNcmUpdate();
  server.handleClient();

  static bool lastLink = false;
  const bool link = usbNcmIsLinkUp();
  if (link != lastLink) {
    lastLink = link;
    logLine(link ? "lien USB monte" : "lien USB tombe");
  }

  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    const UsbNcmStats stats = usbNcmStats();
    logLine(String("rx=") + stats.rxFrames + " drop=" + stats.rxDropped + " tx=" + stats.txFrames +
            " txto=" + stats.txTimeouts);
  }
  delay(2);
}

#else  // cible sans USB-OTG (ESP32-C3) ou compilee en usb_mode=1

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Ce spike demande un ESP32-S3 compile en usb_mode=0 (USB-OTG).");
}

void loop() {
  delay(1000);
}

#endif
