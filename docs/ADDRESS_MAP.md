# Naomi — carte d'adresses et correspondance IC (sérigraphie)

Sources : désassemblage du BIOS epr-21576h (tables du RAM TEST à ROM
0x5C470-0x5C560), captures d'écran du menu de test exécuté sous MAME
(naomi-diag/snap/), driver MAME naomi.cpp/dc.cpp, liste de composants du
PCB 837-13544 dans l'en-tête MAME.

## Correspondance région ↔ puces (officielle, affichée par le BIOS)

| Région / rôle | Adresse SH4 (P2) | Puces (sérigraphie) | Notes |
|---|---|---|---|
| SH-4 (CPU) | — | **IC1** | dissipateur sans ventilateur |
| GPU (HOLLY) | — | **IC15** | dissipateur + ventilateur |
| WORK (RAM CPU) | 0xAC000000, 32 Mo | **IC9, IC10, IC11S, IC12S** | 4× HM5264165 (64 Mbit) |
| VRAM GPU | 0xA5000000 / 0xA5800000, 2× 8 Mo | **IC16, IC18, IC20, IC22** (dessus) et **IC17S, IC19S, IC21S, IC23S** (dessous) | 8× µPD4516161 (16 Mbit) ; quel groupe = TEX0 et lequel = TEX1 : **inconnu** |
| ROM BIOS | 0xA0000000, 2 Mo | **IC27** | 27C160 |
| Puce son (AICA) | 0xA0700000 | **IC33** | |
| RAM son | 0xA0800000, 8 Mo | **IC35** | |
| NVRAM sauvegarde | 0xA0200000 | **IC29** | |
| FPGA Altera Flex EPF8452AQC160-3 | — | **IC30** | |
| EEPROM série 93C46 | GPIO SH-4 | **IC31** | |

> Les désignations terminées par **S** sont au **verso** du PCB.

> **Relevé physique (2026-09-09).** Ce tableau vient de la carte, plus d'une
> déduction. La version précédente inversait les deux groupes de RAM : elle
> donnait IC9-12 à la VRAM et IC16/18/20/22 à la RAM CPU, alors que c'est
> exactement l'inverse. Ces numéros venaient bien des écrans RAM TEST du BIOS,
> mais leur association à une région était déduite de l'ordre d'affichage —
> déduction fausse, comme l'étaient déjà IC29 et IC35.

> **Correction (relevé sur carte réelle).** IC35 est la RAM son et IC29 la
> NVRAM ; ce tableau affirmait l'inverse. Les *numéros* sont authentiques —
> ils viennent des écrans RAM TEST du BIOS d'origine — mais l'association
> d'un numéro à une région était déduite de leur ordre d'affichage, et cette
> déduction était fausse. Tout ce qui repose encore sur elle (TEX0 = IC9-12,
> WORK = IC16/18/20/22) partage la même origine et n'est donc pas plus sûr
> tant qu'un relevé physique ne l'a pas confirmé.

- Le test RAM du BIOS affiche ces numéros via le format `IC%02d GOOD/BAD`
  (chaînes ROM 0x59508-0x59530, régions nommées ROM 0x59550+ : AICA, WORK,
  TEX0, TEX1 (+BACK), tables de numéros IC à ROM 0x5C484-0x5C4C8).
- Écrans capturés : `naomi-diag/snap/naomi/*.png` (liste complète tous GOOD :
  IC29, IC35, IC9-12, IC16/18/20/22, IC17/19/21/23).

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

## Ordre des lanes dans WORK (hypothèse à confirmer)

Bus SDRAM SH4 = 64 bits, 4 puces ×16 bits. Ordre d'affichage BIOS
IC16→IC18→IC20→IC22 = très probablement ordre électrique croissant :

| naomi-diag | Lanes | Hypothèse IC |
|---|---|---|
| CPU RAM 1 | D0-D15, mot pair | IC16 |
| CPU RAM 2 | D16-D31, mot pair | IC18 |
| CPU RAM 3 | D0-D15, mot impair | IC20 |
| CPU RAM 4 | D16-D31, mot impair | IC22 |

Confirmation prévue : panne forcée sur une puce identifiée d'une vraie
Naomi (plan utilisateur). Les masques de bits par puce existent aussi dans
le code BIOS (pool 0xA00251E6 : 0x33333333/0xCCCCCCCC, 0x000F/0x00F0/
0x0F00/0xF000…) — désassemblage à approfondir si besoin.

## Désignateurs IC — ce qui reste inconnu

Tout ce qui figure dans le tableau ci-dessus a été relevé sur la carte. Il
reste deux points, et aucun ne se devine :

1. **Quel groupe de quatre RAM GPU répond à TEX0** (0xA5000000) et lequel à
   TEX1 (0xA5800000). Le partage recto / verso est plausible, pas établi.
2. **L'ordre des voies** à l'intérieur de chaque groupe de quatre. Le code
   suppose l'ordre croissant des numéros — IC9 sur D0-D15 du mot pair, IC10
   sur D16-D31, IC11S et IC12S sur le mot impair — et cette hypothèse n'est
   pas vérifiée.

Les tests concernés affichent le **numéro de position** dans le banc, qui est
exact, plutôt qu'une désignation devinée.

Les anciennes déductions tirées du plan d'implantation de `naomi.cpp` (MAME)
sont retirées : elles décrivaient six RAM vidéo et deux 62256, là où la carte
en porte huit et une seule NVRAM.

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
| RAM principale | 0x0C000000-0x0DFFFFFF | 32 Mo |
| Cartouche/ROM board (G1) | 0x10000000 zone DMA | via registres G1 5F74xx |
