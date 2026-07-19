# naomi-diag — BIOS de diagnostic pour SEGA Naomi

ROM de remplacement du BIOS (EPROM 2 Mo, M27C160) qui teste les composants
un par un et rapporte les résultats **sans jamais dépendre d'une RAM non
testée**.

## Stratégie de sortie (en cascade)

1. **SCIF (port série)** — seul canal 100 % interne au SH4 : fonctionne dès
   le reset, sans aucune mémoire externe. 115200 8N1.
2. **SDRAM principale** testée → si valide, utilisée pour les étapes suivantes.
3. **AICA / RAM son** testée → rapport vocal (WAV préenregistrés) diffusé en
   parallèle du SCIF, avec relecture des résultats déjà acquis. *(à venir)*
4. **VRAM PowerVR** testée → rapport à l'écran. *(à venir)*

Le journal des résultats est conservé en OC-RAM et rejouable sur chaque
nouveau canal de sortie qui devient disponible.

## Ce qui est implémenté (v0.2)

### Nouveau en v0.2 : audio AICA
- `aica.c` : ARM7 tenu en reset (reg 0x702C00 bit0), discipline FIFO G2
  (attente toutes les 8 écritures, réf. KallistiOS), test de la RAM son
  8 Mo (bus de données + 10 passes × 3 motifs avec CRC).
- Rapport vocal : clips **Piper** (TTS neuronal hors-ligne, voix française
  fr_FR-siwis-medium, PCM signé 16 bits 22050 Hz, slot AICA 0, OCT=-1)
  embarqués dans la ROM (`make audio` pour régénérer ; venv Piper dans
  `tools/.venv`, modèle dans `tools/voices/`). Une fois la
  RAM son validée, chaque résultat est annoncé **simultanément sur SCIF et
  haut-parleur**, en commençant par la relecture des résultats déjà acquis.
- Lecture **bloquante** : les tests attendent la fin du clip, puis ≥ 1 s de
  silence entre deux rapports (250 ms entre le nom d'un test et son verdict).
- `timer.c` : temporisations par TMU0 (50 MHz/4), sans RAM.
- Validation MAME : `-wavwrite` + analyse RMS → salves de parole aux bons
  instants, sans chevauchement.

### Nouveau en v0.7 : test du contenu de la cartouche (SHA1 par IC)
- Base embarquée des **192 jeux cartouche Naomi/Naomi 2 connus** (2298 IC),
  générée depuis `mame -listxml` (`tools/gen_cartdb.py` → `src/cartdb.h`,
  ~90 Ko de données : titre + {offset, taille, **SHA1**, sérigraphie IC}).
- Lecture PIO du ROM board (G1, 0x5F7000/04/08) en **mode linéaire brut**
  (bit 31 = auto-incrément, bit 29 = adressage linéaire des cartes M2 —
  sinon remappage par fenêtres 4 Mo ; bit 30 = 0 → contenu chiffré brut,
  identique aux dumps MAME). SHA-1 maison (`sha1.c`, 32 bits).
- Déroulé : en-tête cartouche affiché (magie « NAOMI » + titre ASCII) →
  identification du jeu par points de contrôle SHA1 sur la 1ʳᵉ IC →
  vérification **IC par IC** du jeu identifié → « <Titre> : ic22 GOOD,
  ic1 GOOD… » ou l'IC fautive nommée par sa sérigraphie.
- Validé sous MAME : Cannon Spike (carte M2 chiffrée) et Power Stone 2
  (non chiffrée) → identifiés, toutes IC GOOD ; détection BAD éprouvée
  (IC corrompue → FAIL + IC nommée). Absence de cartouche = normal.
- Garde-fou de build : refus si l'image dépasse 2 Mo (évite une
  troncature silencieuse). La ROM est à 98 % — la prochaine grosse
  addition imposera de compresser l'audio (ADPCM AICA, ÷4).

### Nouveau en v0.6b : auto-test du MIE (voie A — à retravailler)
- Commande 0x84 du noyau d'usine → réponse 0x85, mot de statut 0 = OK :
  le Z80 du MIE exécute son propre test ROM+RAM interne.
- **⚠ À RETRAVAILLER (voie B)** : ce test est une boîte noire SEGA — la
  couverture réelle et la sémantique fine du statut sont inconnues. Le
  plan cible est d'**uploader notre propre routine Z80** dans la RAM du
  MIE (commande 0x80, protocole documenté par libnaomi) appliquant nos
  motifs multi-passes (55/AA/PRNG) avec localisation précise, ce qui
  ouvrira du même coup la lecture de l'EEPROM des réglages (handler
  0x86) et le test JVS complet de la carte I/O 837-13551.

### Nouveau en v0.6 : EEPROM et puce de sécurité
- **93C46 « numéro de série »** (GPIO SH4, PDTRA : DI=b3, DO=b4, CS=b5,
  CLK=b2, PORTEN de BCR2 requis, PCTRA=0x450) : lecture directe des 128
  octets validée à l'octet près sous MAME + test de plausibilité du
  contenu (ASCII, ni tout-0 ni tout-1).
- **X76F100** (sécurité cartouche, bit-bang via BOARDID 0x5F7078/7C) :
  présence par response-to-reset (RTR lu : 0xAB540032) ; absence normale
  sans cartouche.
- **93C46 « réglages » derrière le MIE** : le firmware d'usine 315-6146
  n'a pas de handler 0x86 — le BIOS comme libnaomi uploadent un programme
  Z80 dans le MIE pour y accéder. Documenté, skip propre, travail futur
  (upload de code MIE). CRC SEGA implémenté et validé (sega_eeprom_crc,
  algo netboot, vérifié sur une EEPROM écrite par le BIOS).
- L'EEPROM de config du FPGA (EPC1064, IC31) n'est pas lisible par le CPU
  (chargement direct FPGA) : diagnostic par symptôme uniquement.

### Nouveau en v0.5 : support Naomi 2 validé, ROM universelle
- **Le BIOS Naomi 2 fait aussi 2 Mo (27C160, IC27)** : la même EPROM
  27C160 avec `naomi_diag.bin` fonctionne physiquement sur les deux
  générations — aucune variante d'image nécessaire.
- Détection affinée : signature Elan (0x08800000 = 0xE1AD0000 rev 0x12)
  **plus** test d'aliasing des VRAM (0xA4000000/0xA6000000 : miroir d'une
  même mémoire sur Naomi 1, deux VRAM distinctes sur Naomi 2).
- **Tables IC identiques** : le BIOS Naomi 2 (epr-23605c, tables à ROM
  0x5C800) affiche les mêmes désignateurs (IC29, IC35, IC9-12, IC16-22)
  → les annonces IC restent actives sur Naomi 2.
- Attentes SCIF bornées (jamais de blocage sur la console série ; requis
  aussi par l'émulation SCIF partielle de MAME ≥ 0.288).
- Validation MAME 0.288 : driver `naomi` (Naomi 1) et machine `clubk2k3`
  (vraie config Naomi 2 : Elan + 2× PVR) — cascade complète OK sur les
  deux, détection correcte de part et d'autre.

### Nouveau en v0.4 : bus Maple + MIE (JVS)
- `maple.c` : transaction DMA Maple complète (descripteurs en SDRAM
  validée, registres 0xA05F6C04-6C8C) ; requête de version MIE (commande
  propriétaire 0x82, réf. libnaomi) → réponse 0x83 du **vrai firmware
  315-6146** avec sa chaîne d'identité « 315-6149 COPYRIGHT SEGA ».
  Teste d'un coup : moteur DMA Maple du HOLLY, liaison, Z80 MIE, firmware.
  Validé sous MAME avec le dump 315-6146 authentique.

### Nouveau en v0.3c : carte DIMM (bus G1) et ventilateur
- `dimm.c` : sonde **lecture seule** de la mailbox DIMM (0xA05F703C-704C,
  protocole documenté dans MAME naomigd.cpp) : absence détectée par
  lecture 0xFFFF (config cartouche = normal, annoncé « Carte DIMM,
  absente »), présence → dump des 5 registres bruts sur SCIF + contrôle
  du handshake.
- **Ventilateur** : le tachymètre du ventilateur DIMM est câblé sur le
  firmware de la DIMM uniquement — la carte mère ne le voit jamais
  directement (vérifié : aucune chaîne « fan » dans tout le BIOS, le test
  DIMM du BIOS n'affiche que GOOD/BAD/NOT FOUND/TIMEOUT). Une panne
  ventilateur remonte comme code d'erreur de boot DIMM, visible dans le
  dump de la mailbox. Le ventilateur carte mère n'a pas de tachymètre.

### Nouveau en v0.3b : SRAM sauvegarde + RTC
- `periph.c` : test **non destructif** de la SRAM de sauvegarde 32 Ko
  (0xA0200000, 2× 62256) — sauvegarde/motifs 55/AA/PRNG/restauration
  vérifiée, octet par octet, 10 passes ; localisation par lane octet
  pair/impair (positions 1/2, désignateurs IC en attente de relevé).
- Test **RTC AICA** (0xA0710000) non destructif : le compteur (secondes
  depuis 1950) doit avancer entre deux lectures espacées de 2,2 s (TMU).
  Pas d'écriture : l'heure de la borne est préservée.

### Nouveau en v0.3 : VRAM + écran, auto-test BIOS
- `pvr.c` : activation du contrôleur VRAM (valeurs BIOS), test des deux
  régions **TEX0 (IC9-12)** et **TEX1 (IC35)** avec la même suite 10×3+CRC,
  puis **affichage VGA 640×480 RGB565** (timings BIOS 31 kHz) : le rapport
  complet s'affiche à l'écran (fonte 8×8 domaine public ×2, OK vert /
  FAIL rouge + IC incriminés), re-rendu à chaque nouveau résultat.
  Le framebuffer vit en TEX0, donc toujours testé avant usage.
- Auto-test **BIOS EPROM (IC27)** : CRC32 de la ROM stocké dans les 4
  derniers octets par `tools/patch_crc.py` au build.
- Ordre de la cascade : SCIF → cache SH4 → BIOS → SDRAM → RAM son/audio →
  VRAM/écran.

## Historique v0.1

- `crt0.S` : reset → MMU off → cache opérande en mode RAM (2×4 Ko,
  CCR=0x9A9 ORA+OIX) → init SCIF → bannière → **auto-test OC-RAM** →
  pile en OC-RAM → C.
- `sdram.c` : init BSC/SDRAM avec les valeurs exactes du BIOS d'origine
  (désassemblage epr-21576h @0xA0000440), détection 16/32 Mo par miroir.
- `ramtest.c` : test bus de données (walking ones), test bus d'adresses,
  puis 10 passes × (0x55555555, 0xAAAAAAAA, flux pseudo-aléatoire xorshift32
  avec **CRC32 tenu en registre CPU** comparé écriture/lecture, graine
  différente à chaque passe). Accès en P2 (non caché) uniquement.
- Localisation de panne par **composant numéroté** : le bus SDRAM est large
  de 64 bits, un accès 32 bits touche la moitié basse ou haute selon A2 —
  4 positions distinguables électriquement :
  | # | lanes | mot 32 bits |
  |---|-------|-------------|
  | 1 | D0-D15  | pair (A2=0) |
  | 2 | D16-D31 | pair (A2=0) |
  | 3 | D0-D15  | impair (A2=1) |
  | 4 | D16-D31 | impair (A2=1) |
  Annonce vocale et SCIF avec la **sérigraphie officielle** extraite des
  tables du RAM TEST du BIOS d'origine : « Mémoire principale, I C seize,
  défectueuse » / `CPU RAM 1 (IC16) DEFECTIVE`. Correspondance : positions
  1-4 → IC16/IC18/IC20/IC22 (ordre des lanes = hypothèse à confirmer par
  panne forcée sur vraie carte), RAM son → IC29, VRAM → IC9-12 + IC35
  (voir analysis/ADDRESS_MAP.md). Pannes simulables sous MAME avec
  `fault_inject.lua` + `-nodrc`.
- Également : 8 premières adresses en erreur, comptage total, CRC par passe.

## Build

```sh
make            # naomi_diag.bin (2 Mo, prêt à graver)
make QUICK=1    # variante rapide (1 Mo par passe) pour l'émulateur
make dis        # désassemblage de contrôle
```

Toolchain : paquets Debian `gcc-sh-elf`, `binutils-sh-elf`.

## Test sous MAME

```sh
make mame-rom   # construit ../roms_diag/naomi.zip (BIOS remplacé + dummies MIE/JVS)
make run-mame   # boot + console SCIF capturée par scif_tap.lua
```

- La sortie série est capturée par un tap Lua sur SCFTDR2 (bus 64 bits :
  extraction par lane).
- `fault_inject.lua` : validation négative (bit D5 collé à 0 sur 64 Ko) —
  nécessite `-nodrc` car la SDRAM est en fastram côté DRC.

## Vrai hardware

- Graver `naomi_diag.bin` sur M27C160 (voir doc JinGasa pour l'effacement UV
  et le brochage).
- Câble série sur les pins SCIF : voir `JinGasa/doc` (CN1/CN8/CN11/CN16,
  niveaux 3,3 V — adaptateur USB-série 3,3 V obligatoire, jamais RS-232 ±12 V).
- Un échec du test OC-RAM ⇒ SH4 défectueux (message puis halt).
- Une exception CPU ⇒ redémarrage de la ROM (bannière répétée = crash loop,
  c'est un signal de diagnostic en soi).

## TODO

- Rapport par IC : mapper les lanes D0-D31 sur les références sérigraphiées
  des puces SDRAM de la carte 837-13544. Pas de schémas publics trouvés
  (recherche 07/2026) ; à relever sur une carte réelle (photo sérigraphie).
  SRAM de sauvegarde = IC29 (62256) d'après les logs de réparation.
- Test VRAM PowerVR (mêmes motifs) puis affichage écran.
- RTC, flash sauvegarde, bus G1 (DIMM : statut ventilateur !), JVS via MIE.
- Affiner le test bus d'adresses (ne pas sur-rapporter quand une cellule de
  la fenêtre testée est défectueuse).
