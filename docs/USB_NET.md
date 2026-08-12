# Interface reseau sur cable USB (CDC-NCM)

`nidmi_core::UsbNetService` donne a l'ESP32-S3 un second netif porte par le
cable USB, **en parallele de l'USB-MIDI**, sur le meme connecteur.

Interet : tout ce qui est reseau dans l'application continue de fonctionner
sans modification — serveur HTTP, WebSocket, OSC/UDP, RTP-MIDI, OTA, mDNS —
mais par le cable, WiFi eteint.

## Cibles

| Cible | Support |
|---|---|
| ESP32-S3, `usb_mode=0` | oui |
| ESP32-S3, `usb_mode=1` (HW CDC/JTAG) | non — TinyUSB inactif |
| ESP32-C3 | non — pas d'USB-OTG |

L'exclusion est faite par `SOC_USB_OTG_SUPPORTED`, une capacite, pas un nom de
puce. L'API reste appelable partout et rend `false` : pas de `#ifdef` chez
l'appelant. `UsbNetService::available()` est `constexpr`.

## Cablage des appels

L'ordre n'est pas negociable : TinyUSB assemble son descripteur une seule fois,
a `USB.begin()`.

```cpp
#include <nidmi_core.h>
#include <USB.h>
#include <USBMIDI.h>

static USBMIDI usbMidi;                  // pose son descripteur au constructeur
static nidmi_core::UsbNetService usbNet;

void setup() {
  usbNet.enableInterface();              // AVANT USB.begin()
  USB.productName("NiDMI");
  USB.usbClass(TUSB_CLASS_MISC);         // composite IAD : exige par Windows
  USB.usbSubClass(MISC_SUBCLASS_COMMON);
  USB.usbProtocol(MISC_PROTOCOL_IAD);
  USB.begin();

  usbNet.begin();                        // APRES USB.begin()

  // mDNS APRES usbNet.begin() : mdns_init() a besoin d'esp_netif_init() et de
  // la boucle d'evenements par defaut, mis en place par begin(). Dans l'autre
  // ordre l'init echoue et le .local ne resout jamais.
  MDNS.begin("nidmi");
}

void loop() {
  usbNet.update();                       // non bloquant, indispensable
}
```

`update()` n'est pas optionnel : il porte l'annonce de lien vers l'hote et
l'activation mDNS, qui doivent etre repetees (voir plus bas).

## Configuration

```cpp
nidmi_core::UsbNetConfig cfg;
cfg.ip = "192.168.7.1";        // sous-reseau distinct de l'AP WiFi (192.168.4.x)
cfg.dhcpServer = true;
cfg.advertiseRouter = false;   // NE PAS activer
usbNet.begin(cfg);
```

**`advertiseRouter` doit rester a `false`.** A `true`, le bail DHCP annonce une
passerelle et l'hote peut prendre le lien USB comme route par defaut : brancher
l'instrument couperait l'acces Internet de la machine. Par defaut la passerelle
vaut `0.0.0.0` et ni routeur ni DNS ne sont offerts.

## OSC sur le lien USB

```cpp
osc.setInterface(nidmi_core::OscNetInterface::USB);
osc.setUsbBroadcastAddress(usbNet.broadcastAddress().c_str());
```

L'adresse de diffusion doit etre fournie : contrairement a l'AP WiFi dont le
sous-reseau est fixe, celui du lien USB est configurable.

## Contraintes de plateforme

**Budget d'endpoints — trois classes maximum.** Le S3 offre 6 endpoints dont 4
FIFO IN reellement utilisables. La configuration validee est :

```
MIDI IN1/OUT1     NCM data IN2/OUT2 (duplex)     NCM notif IN3
```

L'ordre d'allocation compte : le service prend la **paire de donnees en duplex
d'abord**, la notification ensuite. Dans l'autre sens la notification occupe
l'index 2 et les bulk finissent desapparies, ce que macOS refuse — l'interface
apparait mais reste `inactive`, alternate setting 0, zero trame.

**Ajouter un CDC-ACM au composite ne fonctionne pas.** La comptabilite du core
l'autorise (`tinyusb_has_available_fifos()` tolere 5 endpoints IN quand CDC est
charge, en considerant que sa notification 0x85 ne consomme pas de FIFO), et
l'allocation reussit. Mais a l'usage l'hote enumere un peripherique **sans
aucune interface** : `bNumConfigurations = 1`, sessionID stable, pas de boot
loop, et rien d'instancie. Quatre classes sont hors budget en pratique.

> Consequence pratique : pas de console serie USB, et pas de flash par
> auto-reset. Le passage en mode download reste manuel (BOOT + rebranchement).

**mDNS.** `CONFIG_MDNS_MAX_INTERFACES` vaut 3 dans les libs Arduino et les trois
slots sont pris par les interfaces predefinies STA / AP / ETH.
`mdns_register_netif()` echoue donc pour toute interface enregistree a
l'execution, et ce n'est pas reglable sans reconstruire les libs IDF. Le netif
prend donc la cle `ETH_DEF` pour occuper le slot ETH predefini.

> **Consequence : sur cette pile, ce netif USB et un vrai Ethernet ne peuvent
> pas coexister sous mDNS.** Sans effet sur les cibles NiDMI actuelles.

**Pas de RNDIS/ECM.** Les libs Arduino ne compilent que `ncm_device` ; il n'y a
pas d'`ecm_rndis_device`. Aucun repli possible sans reconstruire les libs.

## Deux annonces qui doivent etre repetees

Mesure faite pendant le spike (`spike/usbnet/`), les deux se manifestent de la
meme facon : rien ne fonctionne, et rien ne le signale.

**Annonce de lien.** Emise une seule fois sur la transition de `tud_mounted()`,
elle se perd si l'hote n'a pas fini de se configurer. L'hote laisse alors
l'interface de donnees sur l'alternate setting 0 (`bNumEndpoints = 0`) et aucun
trafic n'est possible. Cote macOS : `status: inactive`, aucun bail DHCP.
`update()` la re-affirme chaque seconde.

**Activation mDNS.** Emise depuis `begin()`, elle ne prend pas : le composant
veut un netif deja monte et adresse, ce qui n'arrive qu'a l'activation de
l'interface de donnees par l'hote. `update()` la declenche a la montee du lien,
puis re-annonce quelques fois.

## Diagnostic

Sans CDC il n'y a pas de console : `lastStep()` rend l'etape atteinte par
`begin()` (`UsbNetStep`), exploitable via une LED, du MIDI ou une page HTTP.
`stats()` expose les compteurs de trames.

| Symptome | Piste |
|---|---|
| aucun peripherique USB | descripteur refuse — budget d'endpoints |
| MIDI seul, pas de reseau | `enableInterface()` a echoue |
| interface hote presente, `inactive` | annonce de lien perdue — `update()` appele ? |
| IP repond, `.local` non | mDNS : cle `ETH_DEF`, activation a la montee du lien |
| `txTimeouts` non nul | lien non monte cote hote, ou alt 1 non selectionne |

## Validation

Mesure sur macOS 26.5 / XIAO ESP32S3 : bail DHCP en 2 s, ping 20/20 a
0.6-1.2 ms, route par defaut intacte, HTTP par IP et par nom, et une requete
HTTP arrivee par NCM declenchant une note sur l'USB-MIDI du meme cable.

Linux et Windows restent a valider. Protocole complet dans
`spike/usbnet/README.md` (branche `feat/usb-ncm-spike`).
