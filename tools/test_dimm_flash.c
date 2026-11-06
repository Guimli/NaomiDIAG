/* PC test of src/dimm_flash.c against a simulated AMD Am29LV160DT (top boot,
 * x16) behind the G1 PIO. Build/run: tools/test_dimm_flash.sh
 * The model accepts only the exact AMD command cycles (word addresses 0x555 /
 * 0x2AA), programs by AND (1 -> 0 only), erases by sector with a busy period
 * that returns status (DQ7 = 0, DQ6 toggling), and counts every write. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dimm_flash.h"

#define FL_SIZE 0x200000u
static u16 fl[FL_SIZE / 2];
static u16 regH, regL;
static int flash_mode, cyc, idmode, busy, toggle;
static u32 busy_addr; static u16 busy_val;
static u32 ms;
static long nwrites, erases_slot[2];
static int fail_sector = -1;          /* force a never-ending erase */

static u32 cur(void) { return ((u32)(regH & 0x1F) << 16) | regL; }
static void sector_of(u32 a, u32 *s, u32 *n) {
    if (a < 0x1F0000) { *s = a & ~0xFFFFu; *n = 0x10000; }
    else if (a < 0x1F8000) { *s = 0x1F0000; *n = 0x8000; }
    else if (a < 0x1FA000) { *s = 0x1F8000; *n = 0x2000; }
    else if (a < 0x1FC000) { *s = 0x1FA000; *n = 0x2000; }
    else { *s = 0x1FC000; *n = 0x4000; } }

void hw_w32(u32 a, u32 v) { if (a == 0xA05F7000u) flash_mode = (v == DIMM_FLASH_MODE); }
u32 hw_ticks_ms(void) { return ms++; }
void hw_w16(u32 a, u16 v) {
    if (a == 0xA05F7004u) { regL = v; return; }
    if (a == 0xA05F7000u) { regH = v; if ((v & 0x3000) != 0x3000) { fprintf(stderr, "drapeaux absents\n"); exit(2);} return; }
    if (a != 0xA05F7008u || !flash_mode) { fprintf(stderr, "ecriture hors mode flash\n"); exit(2); }
    u32 ad = cur(), wa = (ad >> 1) & 0x7FF; u8 c = (u8)v;
    if (busy) return;                 /* AMD : commandes ignorees pendant une operation */
    if (cyc == 10) {                  /* 4e cycle de programmation : une DONNEE, jamais une commande */
        fl[ad / 2] &= v; nwrites++; busy = 3; busy_addr = ad; busy_val = fl[ad / 2]; cyc = 0; return; }
    if (c == 0xF0) { cyc = 0; idmode = 0; return; }
    switch (cyc) {
    case 0: case 3: if (wa == 0x555 && c == 0xAA) { cyc++; return; } break;
    case 1: case 4: if (wa == 0x2AA && c == 0x55) { cyc++; return; } break;
    case 2: if (wa == 0x555 && c == 0x90) { idmode = 1; cyc = 0; return; }
            if (wa == 0x555 && c == 0xA0) { cyc = 10; return; }
            if (wa == 0x555 && c == 0x80) { cyc = 3; return; } break;
    case 5: if (c == 0x30) { u32 s, n; sector_of(ad, &s, &n);
                for (u32 i = 0; i < n; i += 2) fl[(s + i) / 2] = 0xFFFF;
                erases_slot[s >= 0x100000]++;
                busy = (fail_sector == (int)s) ? 1000000000 : 50; busy_addr = s; busy_val = 0xFFFF; cyc = 0; return; }
            if (c == 0x10) { fprintf(stderr, "CHIP ERASE interdit\n"); exit(3); } break;
    }
    fprintf(stderr, "sequence invalide cyc=%d @%06X v=%04X\n", cyc, ad, v); exit(2);
}
u16 hw_r16(u32 a) {
    if (a != 0xA05F7008u || !flash_mode) return 0xFFFF;
    u32 ad = cur();
    if (busy) { busy--; toggle ^= 0x4040; return (u16)((~busy_val & 0x8080) | toggle); }
    if (idmode) return (ad & 2) ? 0x22C4 : 0x0004;
    return fl[ad / 2];
}

static u8 img[DIMM_SLOT_SIZE];
static void make_image(u32 seed, int bootcode) {
    srand(seed);
    for (u32 i = 0; i < DIMM_SLOT_SIZE - 4; i++) img[i] = (i % 7) ? (u8)rand() : 0xFF;
    if (bootcode) { img[0] = 0x0E; img[1] = 0xD0; }
    u32 c = dimm_crc32_update(0xFFFFFFFFu, img, DIMM_SLOT_SIZE - 4);
    /* last word: makes the whole-slot CRC (no final XOR) come out 0 */
    for (int i = 0; i < 4; i++) img[DIMM_SLOT_SIZE - 4 + i] = (u8)(c >> (8 * i));
}
static int check(const char *what, int cond) { printf("  %-62s %s\n", what, cond ? "OK" : "ECHEC"); return !cond; }

int main(void) {
    int bad = 0; df_report r;
    for (u32 i = 0; i < FL_SIZE / 2; i++) fl[i] = (u16)(i * 2654435761u >> 7);   /* contenu d'origine */
    u16 *before = malloc(sizeof fl); memcpy(before, fl, sizeof fl);

    puts("1. slot 1, image valide");
    make_image(1, 0);
    bad |= check("CRC de l'image (fin d'image auto-corrigee) == 0", dimm_crc32_update(0xFFFFFFFFu, img, DIMM_SLOT_SIZE) == 0);
    int e = dimm_flash_write_slot(1, img, 0, 0, &r);
    printf("  (err=%d etape=%u adr=%06X lu=%04X attendu=%04X)\n", e, r.stage, r.addr, r.got, r.want);
    bad |= check("retour DF_OK", e == DF_OK);
    bad |= check("slot 1 identique a l'image", !memcmp((u8 *)fl + 0x100000, img, DIMM_SLOT_SIZE));
    bad |= check("slot 0 jamais touche (aucun effacement, contenu intact)", erases_slot[0] == 0 && !memcmp(fl, before, 0x100000));
    bad |= check("19 secteurs effaces (15 x 64K + 32K + 8K + 8K + 16K)", erases_slot[1] == 19);
    bad |= check("CRC du slot 1 relu depuis la flash == 0", dimm_flash_slot_crc(1, 0) == 0);

    puts("2. slot 0 refuse sans autorisation");
    memcpy(before, fl, sizeof fl);
    e = dimm_flash_write_slot(0, img, 0, 0, &r);
    bad |= check("DF_ERR_SLOT0_LOCKED, flash intacte", e == DF_ERR_SLOT0_LOCKED && !memcmp(fl, before, sizeof fl));

    puts("3. slot 0 avec une image qui n'est pas un amorceur DIMM");
    e = dimm_flash_write_slot(0, img, DF_ALLOW_SLOT0, 0, &r);
    bad |= check("DF_ERR_IMAGE_SLOT0, flash intacte", e == DF_ERR_IMAGE_SLOT0 && !memcmp(fl, before, sizeof fl));

    puts("4. image au CRC faux");
    img[1234] ^= 1;
    e = dimm_flash_write_slot(1, img, 0, 0, &r);
    bad |= check("DF_ERR_IMAGE_CRC, flash intacte", e == DF_ERR_IMAGE_CRC && !memcmp(fl, before, sizeof fl));

    puts("5. slot 0 autorise, image amorceur valide");
    make_image(2, 1); erases_slot[0] = erases_slot[1] = 0;
    memcpy(before, fl, sizeof fl);
    e = dimm_flash_write_slot(0, img, DF_ALLOW_SLOT0, 0, &r);
    printf("  (err=%d etape=%u adr=%06X lu=%04X attendu=%04X)\n", e, r.stage, r.addr, r.got, r.want);
    bad |= check("retour DF_OK", e == DF_OK);
    bad |= check("slot 0 identique, slot 1 intact", !memcmp(fl, img, DIMM_SLOT_SIZE) && !memcmp((u8 *)fl + 0x100000, (u8 *)before + 0x100000, DIMM_SLOT_SIZE));
    bad |= check("16 secteurs de 64K effaces, aucun dans le slot 1", erases_slot[0] == 16 && erases_slot[1] == 0);

    puts("6. effacement qui ne finit jamais (secteur 0x130000)");
    make_image(3, 0); fail_sector = 0x130000;
    e = dimm_flash_write_slot(1, img, 0, 0, &r);
    bad |= check("DF_ERR_TIMEOUT a 0x130000, en phase d'effacement", e == DF_ERR_TIMEOUT && r.addr == 0x130000 && r.stage == DF_PH_ERASE);
    printf("  ecritures de mots au total : %ld\n", nwrites);
    puts(bad ? "RESULTAT : ECHEC" : "RESULTAT : tous les tests passent");
    return bad;
}
