# Naomi — carte d'adresses et correspondance IC (sérigraphie)

Sources : désassemblage du BIOS epr-21576h (tables du RAM TEST à ROM
0x5C470-0x5C560), captures d'écran du menu de test exécuté sous MAME
(naomi-diag/snap/), driver MAME naomi.cpp/dc.cpp, liste de composants du
PCB 837-13544 dans l'en-tête MAME. Pour la Naomi 2 : relevé sur une carte
837-14009, recoupé avec le manuel de service Sega et les tables du BIOS
EPR-23608C (voir [Naomi 2](#naomi-2-837-14009--désignateurs-relevés)).

## Correspondance région ↔ puces (officielle, affichée par le BIOS)

| Région / rôle | Adresse SH4 (P2) | Puces (sérigraphie) | Notes |
|---|---|---|---|
| SH-4 (CPU) | — | **IC1** | dissipateur sans ventilateur |
| GPU (HOLLY) | — | **IC15** | dissipateur + ventilateur |
| WORK (RAM CPU) | 0xAC000000, 32 Mo | **IC9, IC10, IC11S, IC12S** | 4× HM5264165 (64 Mbit) |
| VRAM TEX0 | 0xA5000000, 8 Mo | **IC16, IC18, IC20, IC22** (dessus) | 4× µPD4516161 (16 Mbit) |
| VRAM TEX1 | 0xA5800000, 8 Mo | **IC17S, IC19S, IC21S, IC23S** (dessous) | 4× µPD4516161 (16 Mbit) |
| ROM BIOS | 0xA0000000, 2 Mo | **IC27** | 27C160 |
| Puce son (AICA) | 0xA0700000 | **IC33** | |
| RAM son | 0xA0800000, 8 Mo | **IC35** | |
| NVRAM sauvegarde | 0xA0200000 | **IC29** | |
| FPGA Altera Flex EPF8452AQC160-3 | — | **IC30** | |
| EEPROM série 93C46 | GPIO SH-4 | **IC31** | |

> Les désignations terminées par **S** sont au **verso** du PCB.

Les groupes WORK et TEX ainsi que les rôles IC29/IC35 ont été corrigés après relevé physique. Les anciennes déductions fondées sur le seul ordre d'affichage ne sont plus utilisées.

- Le test RAM du BIOS affiche ces numéros via le format `IC%02d GOOD/BAD`
  (chaînes ROM 0x59508-0x59530, régions nommées ROM 0x59550+ : AICA, WORK,
  TEX0, TEX1 (+BACK), tables de numéros IC à ROM 0x5C484-0x5C4C8).
- Écrans capturés : `naomi-diag/snap/naomi/*.png` (liste complète tous GOOD :
  IC29, IC35, IC9-12, IC16/18/20/22, IC17/19/21/23).

## Naomi 2 (837-14009) — désignateurs relevés

Relevé sur une Naomi 2 réelle, puis comparé au manuel de
service Sega NAOMI 2 : toutes les désignations concordent. IC29 et IC46 ont
été confirmés une seconde fois sur la carte. La Naomi 2 reprend les
désignateurs de la Naomi 1 pour les parties communes et ajoute le second
GPU (PVR-B) et l'Elan.

| Rôle | Adresse SH4 (P2) | Puces (sérigraphie) | Notes |
|---|---|---|---|
| SH-4 (CPU) | — | **IC1** | |
| RAM CPU (WORK) | 0xAC000000, 32 Mo | **IC9, IC10, IC11S, IC12S** | voies : parité du mot, comme sur Naomi 1 |
| PowerVR 1 (PVR-A, maître) | registres 0xA05F8000 | **IC15** | |
| VRAM PVR-A TEX0 | 0xA5000000, 8 Mo | **IC16, IC18, IC20, IC22** (dessus) | |
| VRAM PVR-A TEX1 | 0xA5800000, 8 Mo | **IC17S, IC19S, IC21S, IC23S** (dessous) | IC21S confirmé par coupure de DQ9 |
| PowerVR 2 (PVR-B, esclave) | registres 0xA25F8000 | **IC110** | fenêtres fermées au démarrage, ouvertes par l'Elan |
| VRAM PVR-B TXB0 | 0xA7000000, 8 Mo | **IC111, IC113, IC115, IC117** (dessus) | numéros impairs d'abord, table BIOS |
| VRAM PVR-B TXB1 | 0xA7800000, 8 Mo | **IC112S, IC114S, IC116S, IC118S** (dessous) | |
| Elan (T&L) | registres 0xA8800000 | **IC105** | ID `E1AD0000`, confirmé sur carte |
| RAM Elan (POLY) | 0xAA000000, 32 Mo | **IC106, IC107, IC108S, IC109S** | voies : parité du mot + moitié de 16 Mio |
| ROM BIOS | 0xA0000000, 2 Mo | **IC27** | 27C160 |
| NVRAM sauvegarde | 0xA0200000 | **IC29** | D43256BGU |
| Puce son (AICA) | 0xA0700000 | **IC33** | |
| RAM son | 0xA0800000, 8 Mo | **IC35** | |
| FPGA Altera EPF8452AQC160-3 | — | **IC30** | |
| EEPROM série 315-6188 (93C46) | GPIO SH-4 | **IC31** | |
| MIE 315-6146 (Z80) | bus Maple | **IC45** | |
| SRAM D43256BGU | — | **IC46** | 32 Ko près du MIE ; rôle non vérifié |
| 315-6269 | — | **IC7** | rôle non vérifié |
| EPROM 315-6268 | — | *aucune sérigraphie* | |

Les voies de chaque région Naomi 2 sont détaillées plus bas :
[Correspondance exploitable](#correspondance-exploitable) pour TEX et TXB,
[RAM Elan (POLY)](#ram-elan-poly--chaîne-de-calcul) pour l'Elan. Le suffixe
**S** désigne le verso. Les numéros viennent du relevé ; seule
l'association d'un numéro à une voie de données vient du BIOS.

## Identification des deux gros circuits (relevé carte réelle)

Les deux grands boîtiers sont sous dissipateur ; impossible de lire leur
sérigraphie. L'arithmétique les distingue, à partir de tailles que la ROM
mesure elle-même :

| Boîtier | RAM autour | Total | Rôle |
|---|---|---|---|
| dissipateur **sans** ventilateur | 4× HM5264165 (64 Mbit) | 32 Mo | **SH-4** (RAM principale détectée : 32 Mo) |
| dissipateur **avec** ventilateur | 8× µPD4516161 (16 Mbit) | 16 Mo | **HOLLY** (VRAM testée : TEX0 8 Mo + TEX1 8 Mo) |

4 puces × 16 bits = le bus 64 bits du SH-4. 8 puces × 16 bits = deux bancs de
64 bits, soit TEX0 et TEX1 — **TEX1 est donc quatre puces, pas une seule**,
ce que ce document affirmait à tort.

## Voies de RAM CPU (WORK)

Le bus SDRAM SH-4 est large de 64 bits. Il utilise la parité du mot de
32 bits, contrairement à la fenêtre VRAM 32 bits décrite plus bas.

| Position | Voie CPU | IC | Validation |
| --- | --- | --- | --- |
| 1 | D0–D15, mot pair | IC9 | Ordre déduit, mesure individuelle manquante |
| 2 | D16–D31, mot pair | IC10 | Réparation réussie par remplacement d'IC10 seul |
| 3 | D0–D15, mot impair | IC11S | Ordre déduit, mesure individuelle manquante |
| 4 | D16–D31, mot impair | IC12S | Ordre déduit, mesure individuelle manquante |

La balise de voies permet de vérifier chaque attribution sur la carte.
La validation d'IC10 ne remplace pas une mesure propre aux trois autres IC.

## Fait notable

Le RAM TEST du BIOS d'origine n'a **pas détecté** une panne injectée
(D5 collé à 0, mots pairs, 16 Mo hauts) que naomi-diag détecte et localise
(61 525 erreurs). Le test SEGA écrit/vérifie par petits blocs sans passes
croisées — le nôtre est plus exigeant.

## Adresses périphériques (SH4, physiques — préfixer 0xA0000000 pour P2)

| Périphérique | Adresse | Notes |
|---|---|---|
| BIOS ROM | 0x00000000 | 2 Mo |
| SRAM sauvegarde | 0x00200000 | 32 Ko (map MAME `sram`) |
| Registres HOLLY/SB | 0x005F6800-0x005F7CFF | maple 0x005F6C00, G1 0x005F7400, G2 0x005F7800, PVR-DMA 0x005F7C00 |
| Registres PVR (TA/CORE) | 0x005F8000 | |
| Registres AICA | 0x00700000-0x00707FFF | reset ARM7 : 0x00702C00 bit0 ; volume : 0x00702800 |
| RTC AICA | 0x00710000-0x0071000F | |
| RAM son (G2) | 0x00800000-0x00FFFFFF | 8 Mo |
| VRAM accès 64 bits | 0x04000000 | 8 Mo (TEX) |
| VRAM accès 32 bits | 0x05000000 | 8+8 Mo (TEX0/TEX1) |
| Registres PVR-B (Naomi 2) | 0x025F8000 | ouverts après IFCTL de l'Elan |
| VRAM PVR-B accès 64 bits (Naomi 2) | 0x06000000 | même mémoire que la fenêtre 32 bits |
| VRAM PVR-B accès 32 bits (Naomi 2) | 0x07000000 | 8+8 Mo (TXB0/TXB1) |
| Registres Elan (Naomi 2) | 0x08800000 | ID +0, IFCTL +0x10 (bits 1-2 : canal et CLXB, bit 0 : diffusion), rafraîchissement +0x14 |
| RAM Elan (Naomi 2) | 0x0A000000-0x0BFFFFFF | 32 Mo |
| RAM principale | 0x0C000000-0x0DFFFFFF | 32 Mo |
| Cartouche/ROM board (G1) | 0x10000000 zone DMA | via registres G1 5F74xx |

## Mapping des erreurs VRAM extrait du BIOS EPR-23608C

### Image et méthode

Image fournie par l'utilisateur : `epr-23608c.ic27`, 2 097 152 octets.
SHA-1 : `25ef957ec1c58fdaff5e89102002bca6c38832c5`.
Analyse statique SH-4 little-endian, incluant la production du masque,
sa réduction et l'indexation des noms affichés. Aucun test dynamique de ce
BIOS dans MAME n'a été effectué pour cette analyse.

Les offsets ci-dessous sont des offsets **dans le fichier ROM**. Dans le
second bloc, les pointeurs internes se résolvent avec
`offset_ROM = pointeur - 0x0C000000 + 0x188000`.
Le premier bloc contient cinq régions, le second huit dont POLY et TXB0/1.

### Tables du bloc Naomi 2

| Descripteur ROM | Nom SEGA | Début | Fin exclusive | Table IC ROM | IC, dans l'ordre des bits de résultat |
| --- | --- | --- | --- | --- | --- |
| 1F4790 | BACK | A0200000 | A0208000 | 1F4708 | 29 |
| 1F47B0 | AICA | A0800000 | A1000000 | 1F4710 | 35 |
| 1F47D0 | WORK | AC010000 | AE000000 | 1F4718 | 9, 10, 11, 12 |
| 1F47F0 | TEX0 | A5000000 | A5800000 | 1F472C | 16, 18, 20, 22 |
| 1F4810 | TEX1 | A5800000 | A6000000 | 1F4740 | 17, 19, 21, 23 |
| 1F4830 | POLY | AA000000 | ABFF0000 | 1F4754 | 106, 107, 108, 109 |
| 1F4850 | TXB0 | A7000000 | A7800000 | 1F4768 | 111, 113, 115, 117 |
| 1F4870 | TXB1 | A7800000 | A8000000 | 1F477C | 112, 114, 116, 118 |

Chaque table commence par un compteur 32 bits, puis les numéros d'IC.
Chaque descripteur occupe 32 octets : nom, début, fin, champ nul, taille
de bloc, deux paramètres, pointeur vers la table IC. Les blocs TEX font
0x1000 octets. La fonction d'affichage à 1AD234 sélectionne BAD/GOOD
en testant les bits successifs du résultat et parcourt la table IC.

### Chaîne de calcul démontrée pour TEX0/1 et TXB0/1

1. La boucle à 1AD5BE appelle la fonction interne 0C0281BC, soit ROM
   **1B01BC**, avec l'adresse du bloc et sa taille.
2. Cette fonction écrit des rotations de 01010101, lit et calcule le XOR.
   À **1B0228–1B024A**, elle réduit les quatre octets du XOR en quatre
   bits. La table **1B0298** contient `{4, 000000FF, 8}`. Le poids de
   ces quatre bits tourne de quatre positions à chaque mot de 32 bits.
3. Le résultat passe par **1AD1C8**, appelé à **1AD5D4**. Cette routine
   compare la position du bloc au milieu de la région de 8 Mio.
4. **1AD120** réduit les bits avec `33333333` et `CCCCCCCC` : ces masques
   réunissent respectivement les deux octets bas et les deux octets hauts
   de chaque mot. Le déplacement circulaire par mot ne change pas cette
   distinction.
5. Dans la seconde moitié de la région, **1AD1F4** décale le résultat de
   deux bits. Les valeurs 1, 2, 4, 8 sélectionnent donc quatre IC distincts.

Il ne faut pas confondre le masque intermédiaire du BIOS avec un XOR de
données brut. La règle équivalente pour nos lectures directes est :

```text
slot = (offset_dans_region_8_Mio >= 0x400000 ? 2 : 0)
     + (bit_donnee_CPU >= 16 ? 1 : 0)
IC = table_IC[slot]
```

### Correspondance exploitable

| Plage de mots 32 bits | D0–D15 | D16–D31 |
| --- | --- | --- |
| A5000000–A53FFFFC | IC16 | IC18 |
| A5400000–A57FFFFC | IC20 | IC22 |
| A5800000–A5BFFFFC | IC17 | IC19 |
| A5C00000–A5FFFFFC | IC21 | IC23 |
| A7000000–A73FFFFC | IC111 | IC113 |
| A7400000–A77FFFFC | IC115 | IC117 |
| A7800000–A7BFFFFC | IC112 | IC114 |
| A7C00000–A7FFFFFC | IC116 | IC118 |

**Le cas A5FFFFFC / XOR 00000300 correspond à IC21 selon le BIOS SEGA.**
C'est une attribution de voie, pas une preuve que le silicium de la RAM
est responsable plutôt que ses connexions ou le contrôleur.
La table n'identifie pas les broches physiques d'adresse et ne démontre
pas l'absence de permutation entre bits CPU et broches DQ.

### Intégration et vérification

`vram_mapping.h` remplace la parité du mot par la moitié de 4 Mio et la
moitié du mot de données. Les erreurs localisées de toutes les passes
accumulent les IC concernés, même au-delà des huit détails conservés.
Une erreur non relocalisée conserve plusieurs candidats couvrant la plage
testée. Les diagnostics TEX et les traces PVR annoncent l'association BIOS.
La balise VRAM utilise désormais des offsets séparés de 4 Mio pour ses
voies 3/4. La RAM CPU garde son propre mécanisme.

Les codes d'accès 1–6 de NaomiDIAG sont nos codes de qualification ; ils
ne sont pas des codes SEGA. Le BIOS utilise ici des bits de résultat par
IC et des textes BAD/GOOD.

### RAM Elan (POLY) : chaîne de calcul

La boucle de dispatch à **1AD4E8** choisit la réduction selon l'index de
région (r13) : BACK (0) → 0C02801C, AICA (1) → 0C0280F4 sans réduction,
WORK (2) → 0C0281BC puis **1AD120**, **POLY (5)** → 0C0281BC puis
**1AD1FE**, les régions TEX/TXB → 0C0281BC puis 1AD1C8.

- 0C0281BC (ROM **1B01BC**) est le même accumulateur que pour TEX : pour le
  mot d'indice i, l'erreur de l'octet k pose le bit `4*(i mod 8) + k`.
- **1AD1FE** est le sélecteur de moitié de 1AD1C8, mais il appelle
  **1AD174** au lieu de 1AD120. 1AD174 réduit avec `0F0F0F0F` → bit 0 et
  `F0F0F0F0` → bit 1. Les quartets pairs sont les mots pairs, les quartets
  impairs les mots impairs : le bit 0 désigne un **mot 32 bits pair entier**,
  le bit 1 un **mot impair entier**, quel que soit l'octet fautif.
- Au-delà de la moitié de la région, le résultat est décalé de deux bits.

Avec la table POLY `106, 107, 108, 109` :

| Plage | Mots pairs (`addr & 4 = 0`) | Mots impairs |
| --- | --- | --- |
| AA000000–AAFFFFFC | IC106 | IC107 |
| AB000000–ABFFFFFC | IC108S | IC109S |

Le BIOS teste AA000000–ABFF0000 et coupe donc à AAFF8000 ; la ROM coupe
à la frontière physique de 16 Mio. Chaque puce porte un mot de 32 bits :
la position du bit de données ne change pas l'attribution. Le suffixe S
(verso) vient du relevé sur carte. `tools/test_vram_mapping.c` recalcule
cette réduction pour les 32 bits et les huit positions de mot.

Au passage : WORK (2) passe par 1AD120 sans sélecteur de moitié ni parité,
donc le BIOS Naomi 2 ne peut désigner que IC9 (D0–D15) ou IC10 (D16–D31) ;
IC11 et IC12 de sa table ne sont jamais marqués. NaomiDIAG garde pour la
RAM CPU la parité du mot, validée par la réparation d'IC10.

Reproduction, avec le BIOS utilisateur conservé dans un dossier ignoré :

```sh
python3 tools/inspect_bios_mapping.py roms/epr-23608c.ic27
sh tools/test_pvr2.sh
```

Le script valide le SHA-1, extrait les huit descripteurs et vérifie que
l'accumulateur d'erreurs et le sélecteur de moitié sont identiques dans
les deux blocs. Les tests comparent la règle intégrée au calcul du masque
reconstitué pour les 32 bits, les huit positions de mot et les quatre
régions vidéo. Le BIOS et son désassemblage restent exclus de Git.

## Brochage fourni : TSOP-II à 50 broches

Ces correspondances sont transcrites de l'image fournie par l'utilisateur.
La référence exacte de la RAM et le câblage de la carte restent à confirmer.

| Signal RAM | Broche |
| --- | --- |
| A0, A1, A2, A3 | 21, 22, 23, 24 |
| A4, A5, A6, A7 | 27, 28, 29, 30 |
| A8, A9, A10 | 31, 32, 20 |
| A11 (sélection de banque) | 19 |
| DQ0, DQ1, DQ2, DQ3 | 2, 3, 5, 6 |
| DQ4, DQ5, DQ6, DQ7 | 8, 9, 11, 12 |
| DQ8, DQ9, DQ10, DQ11 | 39, 40, 42, 43 |
| DQ12, DQ13, DQ14, DQ15 | 45, 46, 48, 49 |
| LDQM, UDQM | 14, 36 |
| /WE, /CAS, /RAS, /CS | 15, 16, 17, 18 |
| CLK, CKE | 35, 34 |

Selon ce pinout, A0–A10 servent à l'adresse de ligne, A0–A7 à celle de
colonne et A11 à la banque. Un bit d'adresse CPU ne peut donc pas être
converti directement en une broche A de cette RAM sans connaître le
décodage du contrôleur et l'organisation des puces.

L'ancien XOR `00000300` désigne les bits de données CPU D8 et D9.
Si le câblage de la voie concernée les relie à DQ8/DQ9, les broches à
examiner sont 39 et 40. Cette correspondance de voie n'est pas prouvée
par le pinout seul. Le numéro d'IC annoncé vient du calcul du BIOS SEGA,
et ne constitue pas une preuve de panne interne de cette puce.

## Sources complémentaires et mesures physiques restantes

Le [relevé MAME de la Naomi 2](https://github.com/mamedev/mame/blob/master/src/mame/sega/naomi.cpp)
décrit la carte 837-14009-01 / 171-8082C et des RAM Hynix HY57V161610DTC-8
(16 Mbit, deux banques de 512 K mots ×16, TSOP-II 50).
Le [relevé RetroSix](https://retrosix.wiki/wiki/hardware-overview-sega-naomi-2)
associe cette famille aux groupes IC16–IC23 et IC111–IC118, sans établir
l'ordre électrique des voies. Vérifier la référence montée et la révision du PCB.
La [fiche Hynix, page 2](https://pdf.dzsc.com/HY5/HY57V161610DTC-6.pdf)
documente le composant, pas les connexions du PCB ; A10 sert aussi à
l'auto-précharge selon la commande.

Les fenêtres VRAM 32 et 64 bits sont deux vues de la même mémoire.
La [description Dreamcast de Marcus Comstedt](https://mc.pp.se/dc/pvr.html)
explique leur organisation différente, mais ses 8 Mio ne prouvent pas à eux
seuls le câblage Naomi 2. Le modèle
[Flycast `pvr_map32`](https://github.com/flyinghead/flycast/blob/master/core/hw/pvr/pvr_mem.cpp)
donne, pour 16 Mio :

```text
offset64 = (offset32 & 0x800003)
         | ((offset32 & 0x3FFFFC) << 1)
         | ((offset32 & 0x400000) >> 20)
```

Ce modèle est une piste pour les mesures croisées, pas une netlist vérifiée.
Il associe par exemple A5000004 à A4000008 et A5400000 à A4000004.
Pour compléter le mapping physique, il reste à :

- vérifier les vues 32/64 bits sur plusieurs offsets faisant varier les bits
  2, 22 et 23, en sauvegardant les cellules et en suspendant l'affichage ;
- observer les sélections et masquages des voies de chaque quart de 4 Mio,
  sans conclure à partir de CLK ou de commandes partagées seules ;
- relever les connexions entre bits CPU et DQ pour chaque IC ;
- capturer ACTIVE et READ/WRITE en faisant varier un bit CPU à la fois,
  pour distinguer ligne, colonne et banque sur les broches A0–A11.

Consigner les résultats séparément pour PVR-A, PVR-B et chaque révision de carte.
Les instructions d'interprétation des résultats TEX figurent dans le
[README français](../README.fr.md#diagnostic-tex-et-pvr-ab) et le
[README anglais](../README.md#tex-and-pvr-ab-diagnostics).
