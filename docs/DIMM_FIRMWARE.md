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

appelés respectivement depuis 22, 27 et 2 endroits. Le firmware sait donc
écrire sa propre mémoire — sa **SDRAM**, pas sa flash : l'argument de banc
(1, 2, 3 → fenêtres `0xB5000000`+) est contrôlé et produit la chaîne
`Illegal bank(_drv_write_dimm)` hors bornes. Ce sont des copies mémoire, rien
de plus (voir la conclusion en fin de document).

## Le répartiteur de commandes

Trouvé, et il n'a pas la forme attendue : **il n'y a pas de grand `switch`**.

**Lecture** (`0x0C085460`) — les quatre registres entrants `0xB4000014`,
`18`, `1C`, `20` sont recopiés en 16 bits dans un tampon à `0x0C1242EC`.

**Répartition** (`0x0C085340`) :

```
jsr  0x0C085460              ; lit les 4 mots
r0 = (commande >> 13) & 3    ; deux bits seulement
r1 = [0x0C1238AC + r0*4]     ; table de 4 gestionnaires
if (r1) jsr @r1              ; appel avec le tampon
jsr  0x0C085300              ; acquittement
```

La table est remplie à l'exécution par `0x0C0851C0`
(`table[index] = gestionnaire`, avec contrôle de borne). Deux familles
seulement sont inscrites dans ce firmware :

| Famille | Gestionnaire | Action |
|---|---|---|
| 0 | `0x0C0854C0` | `msgQSend` de **8 octets** vers la file `0x0C113CF4` |
| 1 | `0x0C089E80` | efface le bit 15, `msgQSend` de **2 octets** vers `0x0C1252C4` |
| 2, 3 | — | non inscrites |

Autrement dit, le premier niveau n'est qu'un **démultiplexage sur deux bits
vers des files de messages VxWorks**. L'identifiant de commande proprement
dit — les bits 9-14, masque `0x7E00` de libnaomi — est interprété par les
**tâches qui consomment ces files**, pas par le répartiteur.

**Sortie** (`0x0C0853A0`) : attente sur le bit 0 de `0xB4000025`, puis
écriture des mots dans `0xB4000014`+.

## Les consommateurs des deux files

`msgQReceive` est `0x0C0E1FC0` (signature VxWorks `(qid, buf, maxBytes,
timeout)`, retour `-1` = ERROR). Toutes ses références du firmware ont été
relevées ; cinq portent sur la file A, deux sur la file B.

### File A (`0x0C113CF4`, messages de 8 octets) — sens DIMM → Naomi

Les consommateurs de la file A ne sont pas un répartiteur : ce sont quatre
**émetteurs** qui construisent une commande, l'envoient par `0x0C0853A0`,
puis attendent la **réponse** de la Naomi sur la file A. Le délai d'attente
vient de `[0x0C113D0C]`, et chacun a sa chaîne d'erreur :

| Fonction | Chaîne en cas d'expiration |
|---|---|
| `0x0C0855C0` | `PEEK timeout` |
| `0x0C085740` | `POKE timeout` |
| `0x0C0858A0` | `CONTROL_READ timeout` |
| `0x0C085980` | `SET_BASE_ADDRESS timeout` |

Le format du paquet de 8 octets est identique pour les quatre, quatre mots
de 16 bits :

```
w0 = 0x8000 | (cmd << 9) | ((adresse >> 16) & 0x1FF)
w1 = adresse & 0xFFFF
w2 = donnée basse   (POKE) ou 0
w3 = donnée haute   (POKE / SET_BASE_ADDRESS) ou 0
```

Les identifiants, enfin nommés :

| `cmd` | Commande | Taille |
|---|---|---|
| 1 | `CONTROL_READ` | — (mot brut `0x8200`) |
| 3 | `SET_BASE_ADDRESS` | adresse 32 bits en w2/w3 (mot brut `0x8600`) |
| 4 / 5 / 6 | `PEEK` | 8 / 16 / 32 bits |
| 8 / 9 / 10 | `POKE` | 8 / 16 / 32 bits |

Le **bit 15 est le bit de sens** : mis à 1, la commande vient du DIMM et la
Naomi doit la servir. C'est cohérent avec le gestionnaire de famille 1
(`0x0C089E80`) qui *efface* le bit 15 avant de poster sur la file B.

Deux fonctions utilitaires complètent l'ensemble : `0x0C085500` attend que la
file A soit vide (`msgQNumMsgs` = `0x0C0E21E0`, temporisation `0x0C085AC0`)
et `0x0C085560` la vide entièrement.

**Conséquence directe pour NaomiDIAG** : `PEEK`/`POKE` ne sont pas des
services offerts par le DIMM. Ce sont des requêtes que le DIMM adresse à la
Naomi, servies par le BIOS d'origine. Une ROM de diagnostic ne peut pas s'en
servir pour lire ou écrire la mémoire du DIMM.

### File B (`0x0C1252C4`, messages de 2 octets) — sens Naomi → DIMM

C'est la bonne direction, et c'est ici que tout se joue. La tâche
`SocketControl` (`0x0C08BDA0`) crée la file par `msgQCreate(64, 2, 1)`,
inscrit `0x0C089E80` comme gestionnaire de famille 1 via
`0x0C0851C0(1, …)`, lance la tâche `SocketAction` (`0x0C089FA0`), puis
boucle sur `msgQReceive(fileB, buf, 2, WAIT_FOREVER)`.

Le tri du mot de 16 bits reçu :

```c
if ((msg & 0xFF00) == 0xFF00)        handler_FFxx(msg);      /* 0x0C08BCC0 */
else if ((msg & 0xC000) == 0x8000)   handler_reponse(msg);   /* 0x0C08BA60 */
else {
    cmd = (msg >> 9) & 0x3F;
    if (cmd == 16)                   /* accepté, sans effet */ ;
    else if (cmd <= 18)              naomi_command(msg);     /* 0x0C08B900 */
    else if (cmd < 16) erreur("socket error: undefined NAOMI command [%04x] was sent.");
    else               erreur("socket error: undefined message [%04x] was sent.");
}
```

**Le jeu de commandes acceptées depuis la Naomi se réduit à trois valeurs** :
`cmd` = 16, 17, 18, c'est-à-dire les mots bruts `0x2000`, `0x2200`, `0x2400`.
Et `0x2000` est accepté sans rien faire. Tout le reste est rejeté avec une
chaîne d'erreur explicite.

### Le mot de la boîte aux lettres n'est qu'une sonnette

`naomi_command` (`0x0C08B900`) révèle le vrai mécanisme :

```c
void naomi_command(u16 msg)
{
    u32 slot = msg & 0xFF;                       /* octet de poids faible */
    u32 fb[8];
    _drv_read_dimm(socket_base + (slot << 12), fb, 32, 3);
    /* trace : "fb[0] %08X" … "fb[7] %08X" */
    if (fb[0] > 19) { erreur("socket error"); repondre(…); return; }
    handler = table_0x0C113D78[fb[0]];
    handler(msg, fb);
}
```

`socket_base` (`[0x0C124098]`) est fixé par `SocketControl` à
`taille_DIMM - 0x800000`. Le mot posté dans la boîte aux lettres ne porte
donc **aucun paramètre** : il désigne seulement un créneau de 4 Ko dans la
SDRAM du DIMM, où la Naomi a préalablement écrit un bloc de 32 octets —
huit longs `fb[0..7]`. `fb[0]` est l'appel demandé, et il indexe une table
de **20 gestionnaires**, présente en clair dans la ROM à `0x0C113D78` :

```
[ 0] 0x0C08A240   [ 5] 0x0C08B600   [10] 0x0C08AAE0   [15] 0x0C08AF40
[ 1] 0x0C08A840   [ 6] 0x0C08A740   [11] 0x0C08A420   [16] 0x0C08B4C0
[ 2] 0x0C08A560   [ 7] 0x0C08B0E0   [12] 0x0C08B200   [17] 0x0C08B580
[ 3] 0x0C08B360   [ 8] 0x0C08A660   [13] 0x0C08B4A0   [18] 0x0C08B700
[ 4] 0x0C08A9A0   [ 9] 0x0C08AC60   [14] 0x0C08ADE0   [19] 0x0C08B800
```

Les chaînes de diagnostic (`SocketControl`, `SocketAction`,
`socket error: …`) ne laissent guère de doute sur la nature de ces vingt
appels : c'est un **mandataire de sockets BSD**, l'interface réseau que les
jeux en ligne utilisent à travers la carte DIMM. Rien d'un service de
diagnostic.

## Réponse à la question posée

**Aucune commande émise par la Naomi ne mène à une reprogrammation du
DIMM**, et la question était mal posée au départ : `_drv_write_dimm`
(`0x0C085DC0`) et `_drv_read_dimm` (`0x0C086240`) **ne programment pas de
flash**. Ce sont des accesseurs de la SDRAM du DIMM, avec un argument de
banque (1, 2 ou 3 → fenêtres `0xB5000000`+) et un contrôle de borne qui
produit la chaîne `Illegal bank(_drv_write_dimm)`. Leurs 27 appelants sont
de simples copies mémoire.

Le graphe d'appels construit depuis les points d'entrée côté Naomi
(`SocketControl`, `SocketAction`, `naomi_command`, les 20 gestionnaires)
couvre 374 fonctions. Les gestionnaires 7, 12 et 15 appellent bien
`_drv_write_dimm` — pour déposer les données reçues du réseau dans la
mémoire du jeu, ce qui est exactement leur rôle.

**Conclusion pour NaomiDIAG.** La boîte aux lettres du DIMM n'offre pas de
service exploitable par une ROM de diagnostic :

- pas de commande d'identité ni de version ;
- pas de test mémoire ;
- pas de reflashage ;
- les trois seules commandes acceptées exigent d'avoir déjà écrit un bloc de
  32 octets dans la SDRAM du DIMM — ce qu'on ne peut faire qu'à travers le
  protocole GD-ROM/DIMM de chargement, hors de portée d'un test matériel.

Le test DIMM de NaomiDIAG reste donc ce qu'il est aujourd'hui : un vidage de
la boîte aux lettres et un contrôle de stabilité de lecture
(`cart_pin_scan`). C'est le maximum atteignable sans réimplémenter le
chargeur de jeu complet, et ce n'est pas l'objet du projet.

## Méthode

Les cibles d'appel se retrouvent de façon fiable en collectant les littéraux
32 bits chargés par `mov.l @(disp,PC)` puis consommés par un `jsr` — c'est la
méthode qui a donné toutes les entrées ci-dessus, après l'échec d'une
détection par prologues. Le même balayage, complété par les `bsr` et par une
attribution de chaque site d'appel à la dernière entrée de fonction connue,
donne un graphe d'appels suffisant pour trancher une question de
joignabilité.

## Voie fermée

**La trace dynamique sous MAME ne fonctionne pas.** Sur un vrai titre GD-ROM
(machine `naomigd`, firmware DIMM présent), avec interception de toute la
fenêtre G1 : sur soixante secondes émulées, `0x5F703C`-`0x5F704C` n'ont été
ni lus ni écrits. Une seule écriture sur `0x5F7070` au reset. MAME sert les
données du jeu sans jamais faire tourner le protocole. Le script
`mame/dimm_trace.lua` reste correct et servirait contre du matériel réel.
