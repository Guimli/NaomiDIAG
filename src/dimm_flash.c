/* DIMM flash access over the G1 PIO. See dimm_flash.h.
 *
 * Read side ported from Sega's DIMM FIRMWARE UPDATE program (Naomi side,
 * slot 0 of FW_Netdimm_403.bin): flash-mode control words, address encoding,
 * AMD read-ID. Write side:
 *   - word program: the updater's own sequence, per word
 *       base+0xAAA <- AAAA, base+0x554 <- 5555, base+0xAAA <- A0A0, addr <- data
 *     then read back until the word matches (updater 0x0C02E5C4);
 *   - erase: SECTOR erase, as the DIMM firmware's own flash routine does it
 *     (4.01, 0x0C087040): AA/55/80/AA/55 then 0x30 at the sector address,
 *     the 64 KB block at 0x1F0000 being the four boot sectors of a top-boot
 *     part. The updater itself uses CHIP erase (0x10), which wipes both slots;
 *     sector erase is what makes a one-slot update possible.
 * Command addresses are (addr & 0xFFFE0000) + 0xAAA / 0x554, as the updater
 * computes them; byte addresses, the flash being 16 bits wide.
 *
 * Hardware access goes through pio_w16 / pio_r16 / pio_w32 so the whole
 * driver runs on the PC against a simulated flash (tools/test_dimm_flash.c). */
#include "dimm_flash.h"

#ifdef HOST_TEST
void hw_w16(u32 a, u16 v);
u16  hw_r16(u32 a);
void hw_w32(u32 a, u32 v);
u32  hw_ticks_ms(void);
#define pio_w16(a, v)   hw_w16((a), (v))
#define pio_r16(a)      hw_r16(a)
#define pio_w32(a, v)   hw_w32((a), (v))
#define now_ms()        hw_ticks_ms()
#else
#include "timer.h"
#define pio_w16(a, v)   (REG16(a) = (v))
#define pio_r16(a)      REG16(a)
#define pio_w32(a, v)   (REG32(a) = (v))
static u32 now_ms(void) { return timer_ticks() / (TIMER_HZ / 1000u); }
#endif

#define PIO_OFFSETH 0xA05F7000u
#define PIO_OFFSETL 0xA05F7004u
#define PIO_DATA    0xA05F7008u

/* Set the flash address on the G1 PIO (flash mode): low word first, then the
 * high word with the flash-access flags, as the updater does every access. */
static void fl_addr(u32 addr)
{
    pio_w16(PIO_OFFSETL, (u16)addr);
    pio_w16(PIO_OFFSETH, (u16)((addr >> 16) | DIMM_FLASH_ADDRFLAG));
}

static void fl_write16(u32 addr, u16 val)
{
    fl_addr(addr);
    pio_w16(PIO_DATA, val);
}

u16 dimm_flash_read16(u32 addr)
{
    fl_addr(addr);
    return pio_r16(PIO_DATA);
}

static void dimm_flash_enter(void)
{
    pio_w32(PIO_OFFSETH, DIMM_FLASH_MODE);     /* setup1: flash access mode */
}

static void dimm_flash_exit(void)
{
    pio_w32(PIO_OFFSETH, 0);                   /* back to normal G1 access  */
}

static void fl_cmd(u32 at, u16 cmd)
{
    u32 base = at & 0xFFFE0000u;
    fl_write16(base + 0xAAA, 0xAAAA);
    fl_write16(base + 0x554, 0x5555);
    fl_write16(base + 0xAAA, cmd);
}

static void fl_reset(void)
{
    fl_write16(0, 0xF0F0);
}

void dimm_flash_identify(dimm_flash_id *fi)
{
    fi->entered = 0;
    fi->mfr = 0xFFFF;
    fi->dev = 0xFFFF;
    fi->id_ok = 0;
    fi->layout_ok = 0;

    dimm_flash_enter();
    fi->entered = 1;
    fl_reset();
    fl_cmd(0, 0x9090);                          /* AMD read-ID */
    fi->mfr = dimm_flash_read16(0x000000);
    fi->dev = dimm_flash_read16(0x000002);
    fl_reset();
    dimm_flash_exit();

    /* A window that answers all-ones at both words is not a flash in ID mode
     * (no DIMM, mode switch refused, or a stuck bus). */
    fi->id_ok = !(fi->mfr == 0xFFFF && fi->dev == 0xFFFF);
    /* The sector map below is the top-boot 16 Mbit x16 one (AMD Am29LV160DT,
     * Fujitsu MBM29LV160TE: device 0x22C4). Anything else is not written. */
    fi->layout_ok = fi->id_ok && (fi->mfr & 0xFF) != 0xFF &&
                    ((fi->mfr & 0xFF) == 0x01 || (fi->mfr & 0xFF) == 0x04) &&
                    fi->dev == 0x22C4;
}

/* ---- sectors of a top-boot 2 MB part, byte addresses ------------------ */
u32 dimm_flash_sectors(u32 slot, u32 *start, u32 *size, u32 max)
{
    u32 n = 0;
    u32 lo = slot ? 0x100000u : 0;
    u32 hi = lo + DIMM_SLOT_SIZE;
    for (u32 a = lo; a < hi && n < max; ) {
        u32 sz;
        if (a < 0x1F0000u)       sz = 0x10000;
        else if (a < 0x1F8000u)  sz = 0x8000;
        else if (a < 0x1FC000u)  sz = 0x2000;
        else                     sz = 0x4000;
        start[n] = a;
        size[n] = sz;
        n++;
        a += sz;
    }
    return n;
}

/* Wait for an erase or program to finish at addr: the word reads `want`
 * (0xFFFF after an erase). DQ5 set while still busy is the part's own
 * "exceeded timing limits"; re-read once then, as the data sheet asks. */
static int fl_wait(u32 addr, u16 want, u32 timeout_ms)
{
    u32 t0 = now_ms();
    for (;;) {
        u16 v = dimm_flash_read16(addr);
        if (v == want)
            return DF_OK;
        if (v & 0x2020) {
            v = dimm_flash_read16(addr);
            if (v == want)
                return DF_OK;
            if ((v & 0x2020) && (dimm_flash_read16(addr) != v))
                return DF_ERR_DQ5;   /* still toggling after the limit */
        }
        if (now_ms() - t0 > timeout_ms)
            return DF_ERR_TIMEOUT;
    }
}

static int fl_erase_sector(u32 sec)
{
    fl_cmd(sec, 0x8080);
    u32 base = sec & 0xFFFE0000u;
    fl_write16(base + 0xAAA, 0xAAAA);
    fl_write16(base + 0x554, 0x5555);
    fl_write16(sec, 0x3030);
    return fl_wait(sec, 0xFFFF, DF_ERASE_TIMEOUT_MS);
}

static int fl_program_word(u32 addr, u16 val)
{
    fl_cmd(addr, 0xA0A0);
    fl_write16(addr, val);
    return fl_wait(addr, val, DF_PROG_TIMEOUT_MS);
}

static void cb(df_progress p, u32 phase, u32 done, u32 total)
{
    if (p)
        p(phase, done, total);
}

/* CRC-32 (0xEDB88320, init 0xFFFFFFFF, no final XOR) -- the DIMM boot loader's
 * slot check: a good slot gives 0 over its whole 1 MB. */
u32 dimm_crc32_update(u32 crc, const u8 *p, u32 n)
{
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

void dimm_flash_check_words(u32 *w0, u32 *w1)
{
    dimm_flash_enter();
    fl_reset();
    *w0 = dimm_flash_read16(0x0FFFFC) | (u32)dimm_flash_read16(0x0FFFFE) << 16;
    *w1 = dimm_flash_read16(0x1FFFFC) | (u32)dimm_flash_read16(0x1FFFFE) << 16;
    dimm_flash_exit();
}

u32 dimm_flash_slot_crc(u32 slot, df_progress p)
{
    u32 base = slot ? 0x100000u : 0;
    u32 crc = 0xFFFFFFFFu;
    dimm_flash_enter();
    fl_reset();
    for (u32 o = 0; o < DIMM_SLOT_SIZE; o += 2) {
        u16 w = dimm_flash_read16(base + o);
        u8 b[2] = { (u8)w, (u8)(w >> 8) };
        crc = dimm_crc32_update(crc, b, 2);
        if ((o & 0xFFFF) == 0)
            cb(p, DF_PH_READ, o, DIMM_SLOT_SIZE);
    }
    dimm_flash_exit();
    return crc;
}

int dimm_flash_write_slot(u32 slot, const u8 *img, u32 flags, df_progress p,
                          df_report *rep)
{
    u32 st[24], sz[24];
    rep->stage = DF_PH_CHECK;
    rep->addr = 0;
    rep->got = rep->want = 0;

    if (slot > 1)
        return rep->err = DF_ERR_ARG;
    if (slot == 0 && !(flags & DF_ALLOW_SLOT0))
        return rep->err = DF_ERR_SLOT0_LOCKED;
    /* the image must already be a valid slot: CRC-32 over 1 MB == 0 */
    if (dimm_crc32_update(0xFFFFFFFFu, img, DIMM_SLOT_SIZE) != 0)
        return rep->err = DF_ERR_IMAGE_CRC;
    /* slot 0 holds the reset vector and the boot loader: a Naomi program (an
     * updater, a game) or anything else that does not start like the DIMM
     * firmware's boot code must never land there */
    if (slot == 0 && !(img[0] == 0x0E && img[1] == 0xD0))
        return rep->err = DF_ERR_IMAGE_SLOT0;

    dimm_flash_id fi;
    dimm_flash_identify(&fi);
    if (!fi.layout_ok) {
        rep->got = ((u32)fi.mfr << 16) | fi.dev;
        return rep->err = DF_ERR_CHIP;
    }

    u32 ns = dimm_flash_sectors(slot, st, sz, 24);
    u32 base = slot ? 0x100000u : 0;
    int e = DF_OK;

    dimm_flash_enter();
    fl_reset();

    /* 1. erase the slot's sectors, and only them */
    rep->stage = DF_PH_ERASE;
    for (u32 i = 0; i < ns && e == DF_OK; i++) {
        cb(p, DF_PH_ERASE, i, ns);
        rep->addr = st[i];
        e = fl_erase_sector(st[i]);
    }
    /* 2. blank check */
    if (e == DF_OK) {
        rep->stage = DF_PH_BLANK;
        for (u32 o = 0; o < DIMM_SLOT_SIZE; o += 2) {
            if ((o & 0xFFFF) == 0)
                cb(p, DF_PH_BLANK, o, DIMM_SLOT_SIZE);
            u16 v = dimm_flash_read16(base + o);
            if (v != 0xFFFF) {
                rep->addr = base + o;
                rep->got = v;
                rep->want = 0xFFFF;
                e = DF_ERR_BLANK;
                break;
            }
        }
    }
    /* 3. program, skipping the words that stay erased */
    if (e == DF_OK) {
        rep->stage = DF_PH_PROGRAM;
        for (u32 o = 0; o < DIMM_SLOT_SIZE; o += 2) {
            if ((o & 0xFFFF) == 0)
                cb(p, DF_PH_PROGRAM, o, DIMM_SLOT_SIZE);
            u16 w = (u16)(img[o] | (img[o + 1] << 8));
            if (w == 0xFFFF)
                continue;
            e = fl_program_word(base + o, w);
            if (e != DF_OK) {
                rep->addr = base + o;
                rep->want = w;
                rep->got = dimm_flash_read16(base + o);
                break;
            }
        }
    }
    /* 4. verify every word and the slot CRC, from the flash */
    if (e == DF_OK) {
        rep->stage = DF_PH_VERIFY;
        u32 crc = 0xFFFFFFFFu;
        for (u32 o = 0; o < DIMM_SLOT_SIZE; o += 2) {
            if ((o & 0xFFFF) == 0)
                cb(p, DF_PH_VERIFY, o, DIMM_SLOT_SIZE);
            u16 v = dimm_flash_read16(base + o);
            u16 w = (u16)(img[o] | (img[o + 1] << 8));
            if (v != w) {
                rep->addr = base + o;
                rep->got = v;
                rep->want = w;
                e = DF_ERR_VERIFY;
                break;
            }
            u8 b[2] = { (u8)v, (u8)(v >> 8) };
            crc = dimm_crc32_update(crc, b, 2);
        }
        if (e == DF_OK && crc != 0) {
            rep->got = crc;
            e = DF_ERR_FLASH_CRC;
        }
    }
    fl_reset();
    dimm_flash_exit();
    if (e == DF_OK)
        rep->stage = DF_PH_DONE;
    cb(p, rep->stage, 1, 1);
    return rep->err = e;
}
