/**
 * Banc de test de nidmi_core::UsbNetService — USB-MIDI + interface reseau USB
 * (CDC-NCM) sur ESP32-S3.
 *
 * Ce sketch n'embarque plus sa propre pile : il exerce directement le code de
 * la bibliotheque, pour que ce qui est valide ici soit ce qui sera livre.
 *
 * Build : ./spike/usbnet/build.sh          (avec CDC : console + auto-reset)
 *         NIDMI_CDC=0 ./spike/usbnet/build.sh   (config finale, sans CDC)
 */

#include <Arduino.h>
#include <nidmi_core.h>

#if NIDMI_USB_NET_SUPPORTED

#include <ESPmDNS.h>
#include <USB.h>
#include <USBMIDI.h>
#include <WebServer.h>

// Portee globale, imperativement : avec cdc_on_boot=1 le core appelle
// USB.begin() avant setup(), et un descripteur enregistre dans setup() est
// ignore en silence. Les constructeurs des deux objets posent le leur.
static USBMIDI usbMidi;
static nidmi_core::UsbNetService usbNet;

static WebServer server(80);
static const char* kHostname = "nidmi-usb";

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
  const nidmi_core::UsbNetStats stats = usbNet.stats();
  String out;
  out += "link      : ";
  out += usbNet.isLinkUp() ? "up" : "down";
  out += "\nstep      : " + String((int)usbNet.lastStep()) + "  (0 = Ok)";
  out += "\nmac dev   : ";
  out += usbNet.deviceMac();
  out += "\nmac host  : ";
  out += usbNet.hostMac();
  out += "\nip        : " + usbNet.localIp().toString();
  out += "\nbroadcast : " + usbNet.broadcastAddress();
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
    "<title>NiDMI USB</title>"
    "<style>body{font:14px/1.5 system-ui,sans-serif;margin:2rem;max-width:40rem}"
    "pre{background:#f4f4f5;padding:1rem;overflow-x:auto}"
    "@media(prefers-color-scheme:dark){body{background:#18181b;color:#e4e4e7}pre{background:#27272a}}</style>"
    "<h1>NiDMI &mdash; UsbNetService</h1>"
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
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // eteinte (active a l'etat bas)
  Serial.begin(115200);
  delay(200);
  logLine("");
  logLine("=== banc de test UsbNetService ===");
  logLine(String("descripteur NCM enregistre: ") + (usbNet.enableInterface() ? "oui" : "NON"));

#if !ARDUINO_USB_CDC_ON_BOOT
  // Sans CDC au boot, USB.begin() nous revient : on peut encore nommer le
  // peripherique et annoncer le composite IAD. Avec cdc_on_boot=1 le core l'a
  // deja fait, ces reglages seraient sans effet.
  USB.productName("NiDMI");
  USB.manufacturerName("NiDMI");
  USB.usbClass(TUSB_CLASS_MISC);
  USB.usbSubClass(MISC_SUBCLASS_COMMON);
  USB.usbProtocol(MISC_PROTOCOL_IAD);
  USB.begin();
  logLine("USB.begin() appele par le sketch");
#else
  logLine("USB deja demarre par le core (cdc_on_boot=1)");
#endif

  nidmi_core::UsbNetConfig cfg;
  cfg.interfaceName = "NiDMI USB Network";
  cfg.ip = "192.168.7.1";
  if (!usbNet.begin(cfg)) {
    logLine(String("ERREUR: UsbNetService::begin() a echoue, step=") + String((int)usbNet.lastStep()));
  } else {
    logLine(String("netif up, ip ") + usbNet.localIp().toString() + ", mac dev " + usbNet.deviceMac());
  }

  // mDNS APRES begin() : mdns_init() a besoin d'esp_netif_init() et de la
  // boucle d'evenements par defaut, que begin() met en place. Dans l'autre
  // ordre l'init echoue et le .local ne resout jamais.
  if (mdns_init() == ESP_OK) {
    mdns_hostname_set(kHostname);
    mdns_instance_name_set("NiDMI USB");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    logLine("mdns initialise");
  } else {
    logLine("ERREUR: mdns_init a echoue");
  }

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/log", handleLog);
  server.on("/midi", handleMidi);
  server.begin();
  logLine("http: en ecoute sur :80");
}

void loop() {
  usbNet.update();
  server.handleClient();

  static bool lastLink = false;
  const bool link = usbNet.isLinkUp();
  if (link != lastLink) {
    lastLink = link;
    logLine(link ? "lien USB monte" : "lien USB tombe");
  }

  // Telemetrie par USB-MIDI, canal 16 — le seul canal sortant quand le
  // firmware est compile sans CDC.
  //   CC 20 = UsbNetStep   CC 21 = lien   CC 22/23 = trames RX (7 bits)
  //   CC 24 = rejetees     CC 25 = TX
  static uint32_t lastReport = 0;
  if (millis() - lastReport > 1000) {
    lastReport = millis();
    const nidmi_core::UsbNetStats stats = usbNet.stats();
    usbMidi.controlChange(20, (uint8_t)usbNet.lastStep(), 16);
    usbMidi.controlChange(21, link ? 1 : 0, 16);
    usbMidi.controlChange(22, (uint8_t)((stats.rxFrames >> 7) & 0x7F), 16);
    usbMidi.controlChange(23, (uint8_t)(stats.rxFrames & 0x7F), 16);
    usbMidi.controlChange(24, (uint8_t)(stats.rxDropped & 0x7F), 16);
    usbMidi.controlChange(25, (uint8_t)(stats.txFrames & 0x7F), 16);
  }

  // Meme information en clignotement, si le MIDI est muet lui aussi.
  static uint32_t ledAt = 0;
  static uint8_t ledPhase = 0;
  const uint8_t pulses = (uint8_t)usbNet.lastStep() * 2;
  if (millis() - ledAt > (ledPhase > pulses ? 900u : 150u)) {
    ledAt = millis();
    if (ledPhase > pulses) {
      ledPhase = 0;
    }
    digitalWrite(LED_BUILTIN, (ledPhase % 2 == 0) ? LOW : HIGH);
    ledPhase++;
  }

  delay(2);
}

#else  // cible sans USB-OTG (ESP32-C3) ou compilee en usb_mode=1

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Ce banc demande un ESP32-S3 compile en usb_mode=0 (USB-OTG).");
}

void loop() {
  delay(1000);
}

#endif
