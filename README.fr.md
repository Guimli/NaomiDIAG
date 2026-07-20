# NaomiDiag

ROM de BIOS de diagnostic pour les cartes d'arcade **SEGA Naomi** et
**Naomi 2**. Elle remplace le BIOS d'origine dans le support IC27 et teste
les composants de la carte un par un, en rapportant les résultats sur trois
canaux — **série (SCIF)**, **écran (VGA)** et **voix** — sans jamais
dépendre d'une mémoire dont le bon fonctionnement n'a pas encore été prouvé.

Disponible en **anglais** et en **français** (`NaomiDIAG_EN.bin` /
`NaomiDIAG_FR.bin`), texte et voix localisés.

> English: see [README.md](README.md).

## Pourquoi trois canaux de sortie, dans cet ordre

Le port série SCIF est la seule sortie entièrement interne au processeur
SH-4 : il fonctionne dès le reset **sans aucune RAM externe**, c'est donc le
canal primaire, toujours disponible. Le son et la vidéo ont besoin de leurs
propres mémoires (RAM son derrière l'AICA, VRAM derrière le PowerVR) ; elles
sont donc testées d'abord et, une fois validées, deviennent des canaux de
rapport supplémentaires. Le long test de la RAM principale se déroule alors
avec l'audio et l'écran déjà actifs, si bien que la machine ne paraît jamais
figée pendant ce test.

## Ce qui est testé

Dans l'ordre :

1. **Cœur SH-4** — le cache est configuré en RAM interne et auto-testé ;
   il sert aussi de pile/`.bss` à toute la ROM (aucune RAM externe utilisée
   avant d'être validée).
2. **Identification de la carte** — Naomi 1 ou Naomi 2 (signature Elan +
   aliasing VRAM) ; la bonne table de sérigraphies IC est choisie, sinon on
   utilise des positions numérotées sur une carte inconnue.
3. **EPROM BIOS (IC27)** — auto-contrôle CRC32.
4. **RAM son** (AICA, 8 Mo, bus G2) — puis l'audio devient un canal.
5. **VRAM** (PowerVR TEX0 = IC9-12, TEX1 = IC35) — puis l'écran de rapport
   VGA s'active.
6. **RAM CPU principale** (SDRAM, 16/32 Mo, IC16/18/20/22) — bus de données,
   bus d'adresses, puis **10 passes de `0x55555555`, `0xAAAAAAAA` et un flux
   pseudo-aléatoire (graine différente par passe) dont le CRC32 est conservé
   dans un registre CPU et comparé écriture/lecture**. Toute cellule
   défectueuse condamne la puce (et donc toute la RAM entrelacée), qui n'est
   plus utilisée ensuite.
7. **Naomi 2 uniquement** — VRAM du PVR esclave (16 Mo) et RAM Elan (32 Mo).
8. **NVRAM de sauvegarde** (2× 62256) — test non destructif (sauvegarde/
   restauration).
9. **RTC** (AICA) — vérification non destructive de l'avance de l'horloge.
10. **Carte DIMM** (mailbox G1) — présence et cohérence de la mailbox.
11. **Bus Maple / MIE** (Z80 315-6146) — requête de version + auto-test
    d'usine.
12. **EEPROM des réglages** (93C46 via MIE) — base posée (nécessite un
    upload de code Z80 ; documenté).
13. **EEPROM numéro de série** (93C46 sur GPIO du SH-4) — lecture + contrôle
    du contenu.
14. **Sécurité cartouche** (X76F100) — présence par response-to-reset.
15. **Contenu cartouche** — identifie le jeu dans une base embarquée de tous
    les jeux cartouche Naomi/Naomi 2 connus (192 jeux, 2298 IC) et vérifie
    chaque puce ROM par SHA-1, en nommant l'IC fautive par sa sérigraphie.

Les pannes RAM sont rapportées par composant : masque de bits, lanes de
données concernées, et désignateur IC sérigraphié (ex.
`RAM CPU 1 (IC16) DEFECTUEUX`).

## Compilation

Chaîne d'outils : paquets Debian `gcc-sh-elf` / `binutils-sh-elf`.

```sh
make LANG=EN            # -> NaomiDIAG_EN.bin (texte + voix anglais)
make LANG=FR            # -> NaomiDIAG_FR.bin (texte + voix français)
make LANG=FR QUICK=1    # 1 Mo par passe RAM, pour l'émulateur
```

Une image de 2 Mo est produite, prête à graver sur une EPROM 27C160 (IC27).
Le build refuse toute image dépassant 2 Mo (pas de troncature silencieuse).

La même image 27C160 fonctionne sur Naomi 1 et Naomi 2 (les deux utilisent
un BIOS de 2 Mo) ; la carte est détectée à l'exécution.

Régénérer les sources générées (rarement nécessaire, versionnées) :

```sh
make audio     # ré-génère les clips vocaux (nécessite le venv Piper + sox)
make cartdb    # reconstruit la base SHA-1 cartouche depuis `mame -listxml`
```

## Test sous MAME

```sh
make LANG=FR QUICK=1 mame-rom
mame naomi -rompath ../roms_diag -autoboot_script scif_tap.lua
```

`scif_tap.lua` capture la FIFO d'émission SCIF du SH-4 et affiche la console
série (MAME ne câble pas le SCIF). Les scripts `*_fault*.lua` injectent des
pannes RAM pour les tests négatifs. Note : la RAM principale est en fastram
sous le DRC, l'injection de panne y nécessite `-nodrc`.

## Notes pour le vrai matériel

- Gravez `NaomiDIAG_xx.bin` sur une 27C160 (IC27). La sortie série est sur
  les broches SCIF en logique 3,3 V — utilisez un adaptateur USB-série
  3,3 V, jamais des niveaux RS-232.
- Un échec du test cache signifie que le SH-4 lui-même est mort : c'est
  rapporté sur SCIF puis la ROM s'arrête.
- Une exception CPU redémarre la ROM (la bannière se réaffiche) — une
  bannière qui se répète est en soi un signal de diagnostic.
- Le ventilateur de la carte DIMM n'est surveillé que par le firmware DIMM ;
  la carte mère ne peut pas le lire directement. Le ventilateur de la carte
  mère n'a pas de tachymètre.

## État et limites

- Les désignateurs IC des puces RAM proviennent des tables du RAM TEST du
  BIOS d'origine ; l'**ordre exact lane→IC** est une hypothèse à confirmer
  par panne forcée sur vrai matériel.
- Les désignateurs du SH-4, du HOLLY, de l'AICA et des deux 62256 sont
  encore inconnus (le BIOS ne les affiche jamais) ; à relever sur une carte.
- La lecture de l'EEPROM des réglages et le test JVS complet de la carte
  I/O nécessitent un upload de code Z80 dans le MIE (travail futur).
- MAME modélise fidèlement les bus numériques mais pas la mécanique des
  lecteurs ni l'analogique ; le vrai matériel reste le juge final.

## Crédits

Initié à partir du projet de BIOS Naomi minimal
[JinGasa](https://github.com/Tchan0/JinGasa). Détails au niveau registre
recoupés avec MAME et libnaomi. Clips vocaux générés avec
[Piper](https://github.com/rhasspy/piper).
