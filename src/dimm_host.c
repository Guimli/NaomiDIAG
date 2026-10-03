/* The BIOS's side of the DIMM's PEEK/POKE requests: see dimm_host.h. */
#include "dimm_host.h"
#include "dimm.h"           /* DIMM_* mailbox registers, SB_GDST */
#include "timer.h"
#include "scif.h"

/* The part of the BIOS work area the DIMM uses: its status and information
 * words from 0x0C01FC00 and its text screen from 0x0C01FD00. In OC-RAM,
 * not CPU RAM: the session outlives the DIMM test, and the operator may
 * loop on the CPU RAM afterwards -- the DIMM's writes must not land in
 * memory under test. Its other accesses (0x0C01F240, the BIOS's request
 * word, chiefly) read 0 and write nowhere. */
#define SH_BASE     0x0C01FC00u
#define SH_SIZE     0x00000200u
static u32 g_shadow[SH_SIZE / 4];
#define SH(off)     ((volatile u8 *)g_shadow + (off))

#define W_MEM       0x0C01FC04u     /* memory word, posted by the DIMM    */
#define W_REQ       0x0C01FC08u     /* test requests, read by the DIMM    */
#define W_VER       0x0C01FC0Cu     /* status / firmware version word     */

/* The status byte of every answer: what the BIOS keeps at 0x0C01FF00,
 * 2 in test mode and 3 in game mode. 2 is what makes the DIMM run its
 * request loop rather than load a game. */
#define BIOS_TEST_MODE  2u

/* STATUS (0x5F704C): bit 0 = interrupt to the DIMM, bit 8 = interrupt to
 * the Naomi, each raised by writing 0 and released by writing 1. */
#define ST_TO_DIMM  0x0001u
#define ST_TO_NAOMI 0x0100u

static u32 g_active;
static u32 g_base;                  /* SET_BASE_ADDRESS; the DIMM sends 0x0C000000 */
static u32 g_got;
static u32 g_count, g_refused;

u32 dimm_host_active(void)   { return g_active; }
u32 dimm_host_got(void)      { return g_got; }
u32 dimm_host_requests(void) { return g_count; }
u32 dimm_host_refused(void)  { return g_refused; }
void dimm_host_stop(void)    { g_active = 0; }

/* Wait for the DIMM to have taken the previous interrupt. The BIOS waits
 * without a limit; a tenth of a second is far more than the DIMM's
 * interrupt handler needs, and a dead DIMM must not hang the menu. */
static void wait_dimm_free(void)
{
    u32 t0 = timer_ticks();
    while (!(DIMM_STATUS & ST_TO_DIMM))
        if (timer_ticks() - t0 > TIMER_HZ / 10)
            break;
}

/* Raise the interrupt to the DIMM. With the newer acknowledge (memory
 * word bit 29) the BIOS clears bit 8 in the same write, as copied here. */
static void kick_dimm(void)
{
    u32 newer = dimm_host_word(W_MEM) & 0x20000000u;
    DIMM_STATUS = (u16)(DIMM_STATUS & (newer ? 0xFEFEu : 0xFFFEu));
}

void dimm_host_start(void)
{
    for (u32 i = 0; i < SH_SIZE; i++)
        *SH(i) = 0;
    g_base = 0x0C000000u;
    g_got = g_count = g_refused = 0;
    g_active = 1;

    /* The message the BIOS sends at boot: command 0x1E00 | its mode, the
     * other three registers clear. The DIMM's loader waits for any message
     * at all after it has reset the Naomi; this is the one it gets. */
    wait_dimm_free();
    DIMM_COMMAND = (u16)(0x1E00u | BIOS_TEST_MODE);
    DIMM_OFFSETL = 0;
    DIMM_PARAML  = 0;
    DIMM_PARAMH  = 0;
    kick_dimm();
}

u32 dimm_host_word(u32 addr)
{
    u32 off = addr - SH_BASE;
    if (off > SH_SIZE - 4 || (off & 3))
        return 0;
    volatile u8 *p = SH(off);
    return p[0] | (p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

u32 dimm_host_size(void)
{
    if (!(g_got & DH_GOT_MEM))
        return 0;
    u32 mb = (dimm_host_word(W_MEM) & 0xFFFu) + 16;
    return mb << 20;
}

/* where a request points in the shadow, or 0 if it does not */
static volatile u8 *shadow_at(u32 addr, u32 size)
{
    u32 off = addr - SH_BASE;
    if (off >= SH_SIZE || off + size > SH_SIZE || (off & (size - 1)))
        return 0;
    return SH(off);
}

void dimm_host_service(void)
{
    if (!g_active)
        return;
    if (DIMM_STATUS & ST_TO_NAOMI)
        return;                         /* nothing pending */
    if (SB_GDST & 1)
        return;                         /* G1 DMA running: the BIOS waits too */

    u32 cmd = DIMM_COMMAND, off = DIMM_OFFSETL;
    u32 pl = DIMM_PARAML, ph = DIMM_PARAMH;
    u32 status = BIOS_TEST_MODE, data = 0;

    if (cmd & 0x8000u) {
        u32 type = (cmd >> 9) & 0x3Fu;
        u32 addr = g_base + (((cmd & 0x1FFu) << 16) | off);
        u32 size = 0, write = 0;
        switch (type) {
        case 0:                         /* NOP */
            break;
        case 1:                         /* CONTROL_READ: 0x0C01FF04, 0 here */
            break;
        case 3:                         /* SET_BASE_ADDRESS */
            g_base = (ph << 16) | pl;
            break;
        case 4: size = 1; break;        /* PEEK 8/16/32 */
        case 5: size = 2; break;
        case 6: size = 4; break;
        case 8: size = 1; write = 1; break;     /* POKE 8/16/32 */
        case 9: size = 2; write = 1; break;
        case 10: size = 4; write = 1; break;
        default:
            status = 0xFF;              /* what the BIOS answers */
            g_refused++;
            break;
        }
        if (size) {
            volatile u8 *p = shadow_at(addr, size);
            if (!p) {
                g_refused++;            /* outside the work area: ignored */
            } else if (write) {
                u32 v = (ph << 16) | pl;
                for (u32 i = 0; i < size; i++)
                    p[i] = (u8)(v >> (8 * i));
                if (size == 4 && addr == W_MEM)
                    g_got |= DH_GOT_MEM;
                if (size == 4 && addr == W_VER)
                    g_got |= DH_GOT_VER;
            } else {
                for (u32 i = 0; i < size; i++)
                    data |= (u32)p[i] << (8 * i);
                if (addr == W_REQ)
                    g_got |= DH_GOT_IDLE;
            }
        }
    }

    /* The answer, in the registers the request came in. */
    wait_dimm_free();
    DIMM_COMMAND = (u16)((cmd & 0x7E00u) | status);
    DIMM_OFFSETL = 0;
    DIMM_PARAML  = (u16)data;
    DIMM_PARAMH  = (u16)(data >> 16);
    DIMM_STATUS  = (u16)(DIMM_STATUS | ST_TO_NAOMI);    /* ours, taken */
    kick_dimm();
    g_count++;
    g_got |= DH_GOT_ANY;
}
