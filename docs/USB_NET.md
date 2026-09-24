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

`update()` n'est pas optionnel : il epingle la tache USB au coeur de
l'interruption (voir « Deux coeurs »), et porte l'activation mDNS, qui doit
etre repetee (voir plus bas).

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
MIDI IN1/OUT1     NCM notif 0x82     NCM data 0x83/0x03 (duplex)
```

**L'ordre d'allocation compte, et il ne faut pas y toucher** : notification
d'abord, paire de donnees en duplex ensuite. Inverser cet ordre — ce qui semble
anodin, et permettrait en theorie de liberer un index pour un CDC — donne
notif `0x83` et data `0x82`/`0x02`. Dans cette disposition macOS lie pourtant
`AppleUSBNCMControl` et `AppleUSBNCMData`, cree bien l'interface reseau, mais
n'active **jamais** l'alternate setting 1 : `status: inactive`, `bNumEndpoints
= 0` sur l'interface de donnees, aucun bail, zero trame. Aucun message
d'erreur nulle part.

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

## Deux coeurs : la tache USB epinglee au coeur de l'interruption

**Sans cela, l'emission se fige pour toujours sous charge.** Le pilote du
controleur (`dcd_dwc2.c`, TinyUSB 0.20, mode esclave — la 0.21 ne change rien)
ecrit la FIFO d'emission depuis deux fils : la tache qui lance un transfert
(`dcd_edpt_xfer` → `epin_write_tx_fifo`) et l'interruption
(`handle_epin_slave`). La tache se protege par une section critique, qui ne
masque l'interruption que **sur son propre coeur** ; l'interruption ne prend
aucun verrou pour les points d'acces. Or le core Arduino cree la tache `usbd`
**sans coeur** (`xTaskCreate`) : releve, elle s'executait 69 fois sur 118 sur
le coeur 0 quand l'interruption est sur le coeur 1.

Symptome, releve dans les registres : le bloc en cours d'emission a son dernier
paquet compte comme ecrit (`DIEPTSIZ` : 1 paquet, 0 octet) mais la FIFO est
vide ; l'hote reclame (jetons IN), la carte repond NAK, indefiniment. L'hote a
toujours UN datagramme de moins que la carte n'en a emis.

Seuls les transferts de plusieurs paquets exposent la course — les NTB. Les
ecritures MIDI (un paquet) venues d'autres taches ne l'exposent pas : la tache
ecrit le paquet en entier, sans second ecrivain sur cette FIFO.

`update()` remplace donc la tache `usbd` par une copie epinglee au coeur de
l'interruption, des qu'il est connu (vu par `tud_event_hook_cb`). Le
remplacement se fait DANS l'ancienne tache, entre deux evenements. Eprouve :
lien mort a 93 puis 1 666 datagrammes avant ; 23 483 sans une expiree apres,
sur la meme charge (six requetes paralleles, 20 s, deux passages).

## L'etat du lien appartient au pilote

**Ne pas appeler `tud_network_link_state()`.** `netd_init()` remet le lien a
« monte » a chaque reset du bus, et le pilote l'annonce de lui-meme a
l'activation de l'interface de donnees (alt 1) : VITESSE, puis CONNECTE.

Annoncer « coupe » au demontage, comme le faisait cette bibliotheque, cree une
course : execute apres le reset du bus, l'appel ecrase le « monte » du pilote ;
si l'hote active l'interface avant l'annonce « monte » suivante, il recoit
CONNECTE = 0 et repasse en alt 0 — ou la 0.20 refuse toute annonce. Cote
macOS : `status: inactive`, `IOLinkStatus` 1, vitesse 0, pour toujours. Vu une
fois sur quatre demarrages ; c'est le seul chemin par lequel le pilote emet
CONNECTE = 0, et il rend compte de tout l'etat releve (alt 0, notifications
« faites », lien a 1 cote carte), mais il n'a pas ete reproduit a volonte. Les
re-annonces periodiques, elles, ne faisaient rien : le pilote rend la main
quand l'etat ne change pas.

## Le pilote NCM, embarque en source

Ce service apporte sa propre copie du pilote (`src/nidmi_core/tinyusb/ncm_device.c`) :
TinyUSB master `2b9a77862`, le commit exact de la lib precompilee du core
Arduino 3.3.5 (`versions.txt` du paquet `esp32-arduino-libs`). Ses symboles
sont definis avant que l'editeur de liens ne parcoure l'archive : le membre
precompile n'est plus extrait (verifie dans le `.map`). Changements, tous
marques « NiDMI : » dans le source :

- **NTB de 2 048 o** au lieu de 3 200 : 2 304 o de RAM interne rendus, debit
  inchange (~13 000 datagrammes par 20 s de charge) ;
- **reactivation par l'hote** (amont fef11cd + c06dc87) : sur alt 0, l'etat des
  notifications revient a VITESSE ; et **aucune notification en alt 0** — sans
  cette garde, une notification en vol au moment de la desactivation relancait
  VITESSE puis CONNECTE en alt 0, et l'activation suivante n'annoncait rien ;
- **validation des NTB recus** (amont 02ffd90 + 6d697c6) : le bloc NDP doit
  tenir dans le NTB recu, et la position du premier NDP se compare a
  `sizeof(nth16_t)` (le code comparait a la taille d'un pointeur) ;
- **requetes de classe** que la 0.20 refusait, portees de la 0.21 :
  SetEthernetPacketFilter, GetNtbInputSize, SetNtbInputSize (forme a 4 octets ;
  la norme rend les deux dernieres obligatoires) ;
- `nidmi_ncm_etat()` et `nidmi_ncm_evenements()` : l'etat du pilote et la trace
  de ses 16 derniers evenements (activations, notifications, requetes, refus),
  pour le releve — plus d'adresse lue dans l'ELF et passee a la compilation.

**Ce que macOS fait vraiment, releve par la trace** (MESURES §153) : a
l'enumeration, GetNtbParameters, alt 1, VITESSE, CONNECTE — le lien monte. Sur
`ifconfig en10 down` : alt 0. Sur `ifconfig en10 up` : alt 1 puis **alt 0 une
milliseconde plus tard**, avant meme d'avoir lu la notification VITESSE — et
plus rien. Ce n'est pas une reaction a ce que la carte envoie : des
notifications envoyees en alt 0 ne le ramenent pas non plus. Dans ce cas,
seule une nouvelle enumeration rend le lien.

**`relancerEnumeration()` la fait sans debrancher** : deconnexion logicielle
(depuis la tache usbd), reconnexion 500 ms plus tard (par `update()`). Mesure :
le cable revient en ~3 s, lien et MIDI compris. Elle coupe AUSSI le MIDI USB
le temps de la relance — un geste manuel, jamais automatique (le firmware NiDMI
l'expose dans Reglages → Carte → Reseau, « Relancer le cable »).
`reseauActif()` dit si l'hote utilise le reseau du cable (interface de donnees
en alt 1) : faux avec le bus monte, c'est le cas de la relance. La bascule
« cable prioritaire » garde la carte joignable par le WiFi entre-temps.

## Preuve de vie : `sonderHote()`

`isLinkUp()` dit que l'USB est configure, pas que le lien vit : il a ete vu
vrai sur un lien mort. La preuve de vie, ce sont des trames recues
(`stats().rxFrames` qui bouge). Mais au repos un Mac se tait jusqu'a une minute
sur ce lien (22 trames en 5 min, ecarts de 50 a 61 s) : attendre qu'il parle
rend la preuve lente, ou fausse.

`sonderHote()` la provoque : une requete ARP vers l'adresse que notre serveur
DHCP a louee a l'hote, retrouvee par la MAC qu'on lui annonce. Un hote vivant
repond toujours a l'ARP — pare-feu furtif compris — et sa reponse fait bouger
`rxFrames`. `hoteConnu()` dit s'il y a un bail, donc quelqu'un a sonder. C'est
ce que la bascule « cable prioritaire » du firmware NiDMI utilise pour couper
le WiFi quand le cable vit, et le rallumer des qu'il se tait.

## Une annonce qui doit etre repetee

**Activation mDNS.** Emise depuis `begin()`, elle ne prend pas : le composant
veut un netif deja monte et adresse, ce qui n'arrive qu'a l'activation de
l'interface de donnees par l'hote. `update()` la declenche a la montee du lien,
puis re-annonce quelques fois.

## Memoire

Sur un ESP32-S3 le chiffre qui decide est le plus gros bloc contigu de RAM
interne. Ce que le lien y prend, et ce qu'il n'y prend plus (MESURES §150 du
depot de l'app) :

| | RAM interne |
|---|---|
| pile de la tache `usbd` epinglee (statique) + TCB | 3 072 + 352 o — elle en utilise ~1 000 |
| tampons NTB du pilote (2 × 2 048, notre copie) | 4 112 o (6 416 dans la lib precompilee) |
| tampon d'emission | **aucun** : la trame est recopiee une seule fois, du tampon de lwIP dans le NTB, par la tache `usbd` pendant que la tache reseau attend |
| trames recues | **aucune** : copiees en PSRAM jusqu'a ce que lwIP les ait lues |
| pile de `usbnet_rx` | **aucune** : 4 096 o en PSRAM (seul le TCB reste interne) |

La tache `usbd` garde sa pile en RAM interne : c'est la plus prioritaire du
coeur de l'audio, elle ne doit pas attendre la PSRAM a chaque commutation.

**Le cout le plus lourd n'est pas ici.** La lib TinyUSB precompilee du core
Arduino lie toutes ses classes (MSC, DFU, pile hote, CDC, video...) avec leurs
tampons statiques, ~12,6 ko, sans qu'aucune interface ne les annonce.
L'application les ecarte par des pilotes vides en symboles faibles (firmware :
`src/network/UsbClassesAbsentes.c`) ; les vrais pilotes reviennent d'eux-memes
quand une classe sert — le vrai NCM reste lie, puisque ce service appelle
`tud_network_xmit`.

## Diagnostic

Sans CDC il n'y a pas de console : `lastStep()` rend l'etape atteinte par
`begin()` (`UsbNetStep`), exploitable via une LED, du MIDI ou une page HTTP.
`stats()` expose les compteurs de trames.

| Symptome | Piste |
|---|---|
| aucun peripherique USB | descripteur refuse — budget d'endpoints |
| MIDI seul, pas de reseau | `enableInterface()` a echoue |
| interface hote presente, `inactive` | `reseauActif()` faux, trace du pilote en alt 0 : macOS a desactive de lui-meme ; `relancerEnumeration()` |
| trafic qui s'arrete sous charge, `txTimeouts` qui monte | tache `usbd` non epinglee — `update()` appele ? |
| IP repond, `.local` non | mDNS : cle `ETH_DEF`, activation a la montee du lien |
| `txTimeouts` non nul | lien non monte cote hote, ou alt 1 non selectionne |

## Validation

Mesure du code de cette bibliotheque sur macOS 26.5 / XIAO ESP32S3 :

```
bail DHCP   192.168.7.2 en 2 s, en14 active
ping        10/10, 0.799 / 0.922 / 1.278 ms
route       defaut inchangee sur en0
http        /status par IP et par nom, 200
mdns        dns-sd voit "NiDMI USB" sur if 23 (le lien USB)
midi        HTTP par NCM -> 0x90 3c 64 / 0x80 3c 00 sur le port USB-MIDI
compteurs   step 0 (Ok), 103 trames RX, 0 rejetee, 36 TX, 0 timeout
```

Linux et Windows restent a valider. Protocole complet dans
`spike/usbnet/README.md` (branche `feat/usb-ncm-spike`).
