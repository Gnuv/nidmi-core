# Spike — interface reseau USB (CDC-NCM) + USB-MIDI sur ESP32-S3

Code **jetable**, hors de `src/`. Objectif : valider les etapes 1 a 3 avant
d'ecrire un vrai `UsbNetService` dans `nidmi-core`. Si le spike passe, on
refactorise `NetBootstrap` et on ajoute le service ; sinon on jette ce dossier.

Le WiFi n'est jamais demarre dans ce sketch. Tout ce qui repond passe par le cable.

## Ce qui a ete verifie avant d'ecrire une ligne

| Point | Constat |
|---|---|
| Classe NCM compilee dans le core Arduino 3.3.1 | oui — `CONFIG_TINYUSB_NCM_ENABLED=y`, `ncm_device.c.obj` present dans `libarduino_tinyusb.a` |
| RNDIS / ECM disponibles | **non** — aucun `ecm_rndis_device.c.obj`. NCM est la seule classe reseau sans rebuild des libs IDF |
| Budget endpoints S3 | 6 EP, 5 IN, 4 FIFO IN utilisables hors CDC. MIDI 1 + NCM 2 = **3 / 4** |
| Hook pour une classe non exposee | `tinyusb_enable_interface(USB_INTERFACE_CUSTOM, ...)` — meme mecanisme que `USBMIDI.cpp` |
| Glue reseau | `esp_netif` + `_g_esp_netif_netstack_default_eth` linkables ; `dhcpserver` et `mdns_register_netif()` presents |
| Exclusion C3 | `SOC_USB_OTG_SUPPORTED` absent sur C3 — garde par capacite, pas par nom de puce |

## Deux pieges trouves en lisant les sources

**`tud_network_recv_renew()` rappelle directement `tud_network_recv_cb()`**
(`ncm_device.c:228-242`) pour le datagramme suivant du NTB. Renouveler depuis le
callback recurse d'un cran par datagramme — d'ou la queue + `rxTask`.

**NCM ignore la valeur de retour de `tud_network_recv_cb()`** (`ncm_device.c:241`),
contrairement a ECM/RNDIS (`ecm_rndis_device.c:370`) qui represente la trame si
on rend `false`. Donc il faut **exactement un renew par appel, meme quand on
jette la trame** : en oublier un fige le RX definitivement. C'est le bug le plus
facile a introduire ici.

## Build

```bash
./spike/usbnet/build.sh compile                      # -> spike/usbnet/build/
./spike/usbnet/build.sh upload /dev/cu.usbmodemXXXX  # compile puis flashe
./spike/usbnet/build.sh flash  /dev/cu.usbmodemXXXX  # reflashe sans recompiler
```

Artefacts dans `spike/usbnet/build/` (ignore par git) :
`usbnet_spike.ino.bin` (577 ko, l'application seule) et
`usbnet_spike.ino.merged.bin` (8 Mo, bootloader + partitions + app, a flasher
a l'offset 0 si tu passes par esptool directement).

`usb_mode=0` + `cdc_on_boot=0` : USB-OTG/TinyUSB, pas de CDC-ACM (on garde les
FIFO IN pour MIDI + NCM).

### Passer la carte en mode bootloader

Consequence directe de `cdc_on_boot=0` : **il n'y a pas de port serie USB quand
le firmware tourne**, donc pas d'auto-reset possible pour le flash. Sur le
XIAO S3 :

1. maintenir **BOOT** enfonce
2. brancher le cable (ou appuyer sur **RESET** si deja branche)
3. relacher BOOT

Le S3 expose alors son USB-Serial-JTAG ROM et un `/dev/cu.usbmodemXXXX`
apparait. Verifier avec `arduino-cli board list`, puis flasher. Apres le flash,
un RESET fait repartir le firmware — et le port disparait, c'est normal.

### Recuperer les logs

Sans CDC, `Serial` sort sur **UART0 = D6 (TX, GPIO43) / D7 (RX, GPIO44)** : il
faut un adaptateur USB-TTL pour l'etape 1. Des que l'etape 2 fonctionne, le
meme journal est lisible sur `http://192.168.7.1/log`.

## Protocole de test

### Etape 1 — enumeration du composite

Aucun serie necessaire, le descripteur suffit :

```bash
system_profiler SPUSBDataType | grep -A 25 NiDMI
```

Attendu :
- le peripherique `NiDMI` apparait
- deux fonctions distinctes : MIDI **et** une interface reseau CDC-NCM
- `bDeviceClass` = 239 (0xEF, Misc/IAD)

Le MIDI doit rester visible dans **Audio MIDI Setup** (Fenetre → Studio MIDI).
S'il a disparu, le composite a echoue meme si NCM marche.

### Etape 2 — netif, DHCP, ping

Reglages Systeme → Reseau : une nouvelle interface apparait. Elle doit recevoir
une IP en `192.168.7.x`.

```bash
ifconfig | grep -B 3 192.168.7      # l'hote a bien un bail
ping -c 3 192.168.7.1               # l'ESP32 repond
netstat -rn | grep default          # INCHANGE : pas de route par defaut via USB
```

La troisieme commande est la plus importante : le bail DHCP n'annonce **ni
routeur ni DNS** (`ESP_NETIF_ROUTER_SOLICITATION_ADDRESS` et
`ESP_NETIF_DOMAIN_NAME_SERVER` a 0, passerelle `0.0.0.0`). Si une route par
defaut apparait via le lien USB, l'utilisateur perd son acces Internet des
qu'il branche l'instrument — c'est bloquant.

### Etape 3 — HTTP et mDNS

```bash
curl http://192.168.7.1/status
dns-sd -B _http._tcp                # NiDMI USB spike doit apparaitre
curl http://nidmi-usb.local/status
```

`nidmi-usb.local` teste specifiquement `mdns_register_netif()` : un netif a
`if_key` custom n'est pas gere automatiquement par le composant mdns. Symptome
si l'appel manque : l'IP repond, le `.local` non.

### Coexistence des deux classes

`http://192.168.7.1/midi` envoie une note C4 sur l'USB-MIDI. Une requete HTTP
arrivee par NCM qui declenche du MIDI sur le meme cable, c'est la preuve de bout
en bout que les deux classes cohabitent.

## Etat

- [x] compile sans warning (`arduino-cli` 1.0.4, core esp32 3.3.1, S3)
- [x] **etape 1 — enumeration macOS** : composite complet
- [x] **etape 2 — netif + DHCP + ping macOS** : bail obtenu, 0 % de perte
- [x] **etape 3a — HTTP par IP** : `curl http://192.168.7.1/status` repond
- [ ] etape 3b — mDNS : correctif en attente de validation
- [x] **coexistence MIDI + NCM** : HTTP par NCM declenche une note USB-MIDI
- [ ] Linux
- [ ] Windows (go / no-go : pas de repli RNDIS sans rebuild des libs)

### Mesures macOS 26.5 (Darwin 25.5.0), 12 aout 2026

```
peripherique  NiDMI  VID 0x2886  PID 0x0056  bDeviceClass 239/2/1 (IAD)
  itf 0  classe 1 / 1     Audio Control
  itf 1  classe 1 / 3     MIDIStreaming     -> CoreMIDI: port visible
  itf 2  classe 2 / 13    CDC-NCM control
  itf 3  classe 10 / 0/1  CDC-Data (NTB)

en14      192.168.7.2 par DHCP, status active
ping      4/4, 0.815 / 1.026 / 1.540 ms
route     defaut inchangee sur en0 — le lien USB ne detourne rien
http      /status repond, rx 165 trames / 0 rejetee / 0 timeout TX
midi      HTTP par NCM -> 0x90 0x3c 0x64 puis 0x80 0x3c 0x00 sur le port USB-MIDI
```

### Deux courses trouvees a l'execution

**Annonce de lien.** `usbNcmUpdate()` conditionnait `tud_network_link_state()`
au succes du netif, et ne l'emettait qu'une fois, sur la transition de
`tud_mounted()`. Si l'hote n'avait pas fini de configurer, la notification
etait perdue : pas de porteur, interface de donnees laissee sur **alt 0**
(`bNumEndpoints = 0`), aucun trafic possible. Symptome cote macOS :
`status: inactive` et aucun bail. Corrige en gatant sur l'enregistrement du
descripteur seul et en re-affirmant l'annonce chaque seconde.

**Activation mDNS.** `mdns_netif_action(ENABLE_IP4)` emis dans `setup()` ne
prend pas : `dns-sd -B _http._tcp` ne voit pas le service sur l'interface du
lien USB. Le composant mdns veut un netif deja monte et adresse, ce qui
n'arrive qu'a l'activation de l'interface de donnees par l'hote. Deplace a la
montee du lien, avec quelques re-annonces espacees.

Les deux ont la meme forme : un evenement unique emis trop tot, perdu sans
aucun diagnostic. C'est le motif a retenir pour `UsbNetService`.

## Points a surveiller au runtime

- `rx dropped` non nul sur `/status` : queue RX trop courte ou heap sous pression
- `tx timeout` non nul : `tud_network_can_xmit()` jamais pret — lien pas monte
  cote hote, ou alt setting 1 non selectionne
- pile de `rxTask` (4096) : a re-mesurer avec `uxTaskGetStackHighWaterMark` si
  quelque chose se comporte bizarrement
