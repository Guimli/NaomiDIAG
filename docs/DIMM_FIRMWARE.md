# Firmware de la carte DIMM — état de la rétro-analyse

Source : `segadimm.zip` (jeu de ROM MAME), image `fpr23718.ic36` — 2 Mo.
Les autres versions (`fpr23905.ic36`, `401_203.bin`, …) partagent la même
structure.

## Acquis

**Processeur et système.** SH-4 exécutant **VxWorks** — « Copyright 1984-1998
Wind River Systems, Inc. » en clair dans l'image. Les symboles ont survécu.

**Base de liaison : `0x0C080000`.** Établie depuis l'amorceur : le code à
l'offset `0xbe` calcule `(0x0C080220 − 0x0C080000) + 0` pour sauter à l'offset
fichier `0x220`. Donc **offset fichier X ↔ adresse liée `0x0C080000 + X`**.
Vérifié : la chaîne `returnToNaomi` à l'offset `0x899DC` est référencée par la
valeur `0x0C1099DC`, à deux endroits.

L'image de 2 Mo contient **deux copies** du même code, décalées de `0x100000`.

**Séquence d'amorçage** (offset `0xbe`) :

```
FRQCR = 0x0E0A      horloge
CCR   = 0x0808      cache
MMUCR = 0
ICR   = 0x4080
r15   = 0x0C080000  pile
jsr   -> offset 0x220
```

**Fenêtre mailbox vue par le DIMM : `0xB4000014` à `0xB4000034`.**
Poignée de main sur le bit 0 de l'octet `0xB4000025` : le firmware y boucle
jusqu'à ce que la Naomi ait consommé la réponse. Les mots sortants sont
écrits en 32 bits à `0xB4000030` et `0xB4000034`.

**Format de réponse** (`returnToNaomi`, offset `0x9ee0`), quatre mots de 16
bits, soit exactement la disposition de la mailbox côté Naomi :

| Mot | Contenu |
|---|---|
| 0 | `((sid + 2) << 9) \| (stat ? 0xFF : 4)` |
| 1 | `param & 0x3F` |
| 2 | `valeur & 0xFFFF` |
| 3 | `valeur >> 16` |

Le champ d'identifiant en bits 9-14 correspond aux constantes de libnaomi
(`CONST_DIMM_COMMAND_MASK 0x7E00`).

**Pilote mémoire** — signature déduite des sites d'appel :

```
_drv_read_dimm (adresse, tampon, longueur, banc)   entrée 0x0C086240
_drv_write_dimm(adresse, tampon, longueur, banc)   entrée 0x0C085DC0
_drv_fill_dimm (…)                                  entrée 0x0C085BE0
```

appelés respectivement depuis 22, 23 et 2 endroits. Le firmware **sait donc
écrire sa propre mémoire**.

## Ce qui reste

**Le répartiteur de commandes entrantes n'est pas identifié.** Les 23
appelants de `_drv_write_dimm` sont tous internes au firmware pour ce qu'on
en sait ; rien n'établit qu'une commande émise par la Naomi y mène. Tant que
ce point n'est pas tranché, un test en écriture de la mémoire DIMM depuis la
Naomi reste hypothétique.

Prochaine étape : partir de la boucle qui lit `0xB4000014`/`0xB4000028` et
suivre le `switch` sur l'identifiant de commande jusqu'aux fonctions
appelées. Les cibles d'appel se retrouvent de façon fiable en collectant les
littéraux 32 bits chargés par `mov.l` puis consommés par un `jsr` — c'est la
méthode qui a donné les entrées ci-dessus.

## Voie fermée

**La trace dynamique sous MAME ne fonctionne pas.** Sur un vrai titre GD-ROM
(machine `naomigd`, firmware DIMM présent), avec interception de toute la
fenêtre G1 : sur soixante secondes émulées, `0x5F703C`-`0x5F704C` n'ont été
ni lus ni écrits. Une seule écriture sur `0x5F7070` au reset. MAME sert les
données du jeu sans jamais faire tourner le protocole. Le script
`mame/dimm_trace.lua` reste correct et servirait contre du matériel réel.
