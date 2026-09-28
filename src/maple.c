/* Maple bus scan: send a MIE version request (0x82) to the primary
 * device address of each port and collect the response.
 * Register/protocol references: KallistiOS maple driver, MAME maple-dc. */
#include "maple.h"
#include "mie_prog.h"
#include "timer.h"
#include "scif.h"

#define SB_MDSTAR   REG32(0xA05F6C04)   /* DMA descriptor table (phys)   */
#define SB_MDTSEL   REG32(0xA05F6C10)   /* trigger: 0 = software         */
#define SB_MDEN     REG32(0xA05F6C14)   /* DMA enable                    */
#define SB_MDST     REG32(0xA05F6C18)   /* DMA start / busy              */
#define SB_MSYS     REG32(0xA05F6C80)   /* timeout / bitrate             */
#define SB_MDAPRO   REG32(0xA05F6C8C)   /* address protection window     */
#define SB_ISTERR   REG32(0xA05F6908)   /* latched system-bus errors     */

/* Override memory access only in the host regression harness. */
#ifndef MAPLE_BUFFER
#define MAPLE_BUFFER(a) ((volatile u32 *)(uintptr_t)(a))
#endif
#define MAPLE_WAIT_TICKS (TIMER_HZ / 10u)

static u32 g_dma_protection;
static const char *g_transaction_status;
static u32 g_error_before, g_error_after, g_last_mdst;

static u32 maple_wait_idle(void)
{
    u32 start = timer_ticks();
    while (SB_MDST & 1u)
        if ((u32)(timer_ticks() - start) >= MAPLE_WAIT_TICKS)
            return 0;
    return 1;
}

static u32 maple_finish(const char *status, u32 header)
{
    g_transaction_status = status;
    g_last_mdst = SB_MDST;
    g_error_after = SB_ISTERR;
    return header;
}

/* scratch area in validated main RAM (P2 view / physical for the DMA) */
/* Maple DMA descriptors and receive buffer live in main RAM, which is also
 * what the CPU RAM test writes patterns over. Polling the buttons during that
 * test therefore DMAs into the region under test and the test reads back
 * corruption -- phantom faults on a perfectly good board.
 *
 * So the buffers are moved into the reserved block at the top of RAM, the one
 * the cell test already stops below. Defaults kept for the boot path, before
 * the reservation is known. */
/* Zero-initialised on purpose: this ROM allows no writable .data, so the
 * fallback is applied on first use rather than by an initialiser. */
static u32 g_desc_p2, g_desc_phy, g_rx_p2, g_rx_phy;

void maple_set_buffers(u32 p2_base)
{
    g_desc_p2  = p2_base;
    g_desc_phy = p2_base & 0x1FFFFFFFu;
    g_rx_p2    = p2_base + 0x100u;
    g_rx_phy   = (p2_base & 0x1FFFFFFFu) + 0x100u;
}

#define MAPLE_DESC_P2   g_desc_p2
#define MAPLE_DESC_PHY  g_desc_phy
#define MAPLE_RX_P2     g_rx_p2
#define MAPLE_RX_PHY    g_rx_phy

static u32 maple_txn(u32 port, u32 cmd, u32 nwords, const u32 *payload)
{
    if (!g_desc_p2)
        maple_set_buffers(0xAC0FF000u);     /* before anything reserved one */

    g_error_before = SB_ISTERR;
    g_dma_protection = 0;
    /* Validate the entire 512-byte reservation before touching it. */
    if ((MAPLE_DESC_P2 & 0xE000001Fu) != 0xA0000000u ||
        MAPLE_DESC_PHY < 0x0C000000u || MAPLE_DESC_PHY > 0x0DFFFE00u ||
        port > 3 || nwords > 61 || (nwords && !payload))
        return maple_finish("invalid-request", 0xFFFFFFFFu);

    /* Never overwrite buffers that an earlier DMA might still be using. */
    if (!maple_wait_idle()) {
        SB_MDEN = 0;
        return maple_finish("busy-timeout", 0xFFFFFFFFu);
    }
    SB_MDEN = 0;
    /* MDAPRO uses inclusive 1 MiB bounds, encoded as (physical >> 20)-0x80.
     * 0x6155404F only covered the first 16 MiB, excluding our high-RAM buffers.
     * Cover both buffers, including a reservation crossing a MiB boundary. */
    u32 low = (MAPLE_DESC_PHY >> 20) - 0x80u;
    u32 high = ((MAPLE_RX_PHY + 0xFFu) >> 20) - 0x80u;
    g_dma_protection = 0x61550000u | (low << 8) | high;
    SB_MDAPRO = g_dma_protection;

    volatile u32 *desc = MAPLE_BUFFER(MAPLE_DESC_P2);
    volatile u32 *rx   = MAPLE_BUFFER(MAPLE_RX_P2);

    for (int i = 0; i < 64; i++)
        rx[i] = 0xDEADDEAD;

    /* dst = port main device (port<<6|0x20), src = host */
    u32 dst = (port << 6) | 0x20;
    u32 src = (port << 6);
    desc[0] = 0x80000000 | (port << 16) | nwords;   /* last, frame words-1 */
    desc[1] = MAPLE_RX_PHY;
    desc[2] = (nwords << 24) | (src << 16) | (dst << 8) | (cmd & 0xFF);
    for (u32 i = 0; i < nwords; i++)
        desc[3 + i] = payload[i];

    SB_MDTSEL = 0;
    SB_MSYS   = 0xC3500000;                     /* 50000 timeout, 2 Mbps */
    SB_MDSTAR = MAPLE_DESC_PHY;
    SB_MDEN   = 1;
    SB_MDST   = 1;

    if (!maple_wait_idle()) {
        SB_MDEN = 0;
        return maple_finish("dma-timeout", 0xFFFFFFFFu);
    }
    delay_ms(2);
    u32 header = rx[0];
    return maple_finish(header == 0xDEADDEADu ? "rx-unchanged" :
                        (header & 0xFFu) == 0xFFu ? "no-response" : "reply", header);
}

void maple_scan(maple_result *mr)
{
    mr->found_port = 0xFFFFFFFF;
    mr->response_cmd = 0xFF;
    mr->func_codes = 0;
    for (u32 i = 0; i < sizeof mr->id; i++)
        mr->id[i] = 0;

    for (u32 port = 0; port < 4; port++) {
        /* MIE protocol (libnaomi): 0x82 = version request -> 0x83 */
        u32 hdr = maple_txn(port, 0x82, 0, 0);
        u32 cmd = hdr & 0xFF;
        if (hdr == 0xFFFFFFFF || hdr == 0xDEADDEAD || cmd != 0x83 ||
            !(hdr >> 24) || (hdr >> 24) > 63) {
            /* Preserve raw status: ISTERR may include older/unrelated faults.
             * Do not clear global error bits owned by other diagnostics. */
            scif_puts("  Maple port="); scif_putdec(port);
            scif_puts(" status=");
            scif_puts(cmd == 0xFF || hdr == 0xDEADDEAD ?
                      g_transaction_status : "invalid-version-reply");
            scif_puts(" header="); scif_puthex(hdr);
            scif_puts(" desc="); scif_puthex(MAPLE_DESC_PHY);
            scif_puts(" rx="); scif_puthex(MAPLE_RX_PHY);
            scif_puts(" mdapro="); scif_puthex(g_dma_protection);
            scif_puts(" mdst="); scif_puthex(g_last_mdst);
            scif_puts(" isterr_before="); scif_puthex(g_error_before);
            scif_puts(" isterr_after="); scif_puthex(g_error_after);
            scif_puts("\n");
            continue;                            /* no device / timeout  */
        }
        volatile u32 *rx = MAPLE_BUFFER(MAPLE_RX_P2);
        mr->found_port = port;
        mr->response_cmd = cmd;
        mr->func_codes = rx[1];
        /* 0x83 response: ASCII version string in the payload words */
        const volatile u8 *blk = (const volatile u8 *)&rx[1];
        u32 words = (hdr >> 24) & 0xFF;
        u32 nbytes = words * 4;
        if (nbytes > sizeof mr->id - 1)
            nbytes = sizeof mr->id - 1;
        u32 o = 0;
        for (u32 i = 0; i < nbytes; i++) {
            u8 c = blk[i];
            mr->id[o++] = (c >= 32 && c < 127) ? (char)c : ' ';
        }
        mr->id[o] = 0;
        return;
    }
}

u32 maple_mie_selftest(u32 port, u32 *status)
{
    volatile u32 *rx = MAPLE_BUFFER(MAPLE_RX_P2);
    *status = 0xFFFFFFFF;
    for (u32 tries = 0; tries < 100; tries++) {
        u32 hdr = maple_txn(port, 0x84, 0, 0);
        if ((hdr & 0xFF) == 0x85) {
            if (((hdr >> 24) & 0xFF) != 1)
                return 1;                   /* malformed response */
            *status = rx[1];
            return (*status == 0) ? 0 : 1;  /* all-zero word = pass */
        }
        delay_ms(20);                       /* test still running */
    }
    return 1;
}

/* Upload our Z80 program into the MIE and start it.
 *
 * Maple command 0x80, as the MIE's boot ROM implements it: payload word 0
 * carries the destination address in its upper two bytes -- high byte at
 * offset 2, low at offset 3 -- and words 1..6 carry 24 bytes of code. The
 * MIE answers 0x81 with one word whose low byte is the sum of the 28
 * payload bytes, so every packet is checked rather than hoped for. A packet
 * whose first two payload bytes are 0xFF 0xFF means "run it", and the MIE
 * jumps to 0x8010.
 *
 * From that point the MIE no longer runs its factory dispatcher: this
 * program owns the Maple interface until the board is reset.
 *
 * Returns 0 on success, or the 1-based number of the packet that failed. */
u32 maple_mie_upload(u32 port)
{
    volatile u32 *rx = MAPLE_BUFFER(MAPLE_RX_P2);
    u32 pay[7];

    for (u32 off = 0; off < MIE_PROG_LEN; off += 24) {
        u32 addr = MIE_PROG_ORG + off;
        const u8 *src = &mie_prog[off];
        u32 left = MIE_PROG_LEN - off;
        if (left > 24)
            left = 24;

        /* word 0: bytes 0,1 free (they only mean something as FFFF),
         * byte 2 = address high, byte 3 = address low */
        pay[0] = ((addr & 0xFF00u) << 8) | ((addr & 0x00FFu) << 24);
        u8 *pb = (u8 *)&pay[1];
        for (u32 i = 0; i < 24; i++)
            pb[i] = (i < left) ? src[i] : 0;

        u32 sum = 0;
        const u8 *all = (const u8 *)pay;
        for (u32 i = 0; i < 28; i++)
            sum += all[i];

        u32 hdr = maple_txn(port, 0x80, 7, pay);
        if ((hdr & 0xFF) != 0x81 || (rx[1] & 0xFF) != (sum & 0xFF))
            return (off / 24) + 1;
    }

    pay[0] = 0x0000FFFFu;                   /* bytes 0,1 = FF FF: execute */
    for (u32 i = 1; i < 7; i++)
        pay[i] = 0;
    maple_txn(port, 0x80, 7, pay);          /* no reply: the MIE jumps away */
    delay_ms(20);                           /* it reads the whole EEPROM first */
    return 0;
}

/* Read the 128-byte settings EEPROM through the uploaded program: command
 * 0xE0, payload byte 0 = which 28-byte slice, answered by 0xE1. */
u32 maple_eeprom_read(u32 port, u8 *out128)
{
    volatile u32 *rx = MAPLE_BUFFER(MAPLE_RX_P2);

    for (u32 blk = 0; blk < 5; blk++) {
        u32 pay = blk;
        u32 hdr = maple_txn(port, 0xE0, 1, &pay);
        if ((hdr & 0xFF) != 0xE1)
            return 1;
        const volatile u8 *src = (const volatile u8 *)&rx[1];
        for (u32 i = 0; i < 28; i++) {
            u32 d = blk * 28 + i;
            if (d < 128)
                out128[d] = src[i];
        }
    }
    return 0;
}

/* The MIE input port, live: DIP SW1:1-4 in bits 0-3, TEST in bit 4,
 * SERVICE1/2 in bits 5-6, all active low. Command 0xE2 -> 0xE3. */
u32 maple_mie_inputs(u32 port, u8 *state)
{
    volatile u32 *rx = MAPLE_BUFFER(MAPLE_RX_P2);
    u32 pay = 0;
    u32 hdr = maple_txn(port, 0xE2, 1, &pay);
    if ((hdr & 0xFF) != 0xE3)
        return 1;
    *state = (u8)(rx[1] & 0xFF);
    return 0;
}

/* One 28-byte slice of the JVS info block: command 0xE4, payload byte 0 =
 * slice index, answered by 0xE5 with seven words. The JVS master in the
 * MIE fills the block on its own; this only reads it, so it never waits
 * on the JVS bus. */
u32 maple_jvs_info(u32 port, u32 idx, u8 *out28)
{
    volatile u32 *rx = MAPLE_BUFFER(MAPLE_RX_P2);
    u32 pay = idx;
    u32 hdr = maple_txn(port, 0xE4, 1, &pay);
    if ((hdr & 0xFF) != 0xE5)
        return 1;
    const volatile u8 *src = (const volatile u8 *)&rx[1];
    for (u32 i = 0; i < 28; i++)
        out28[i] = src[i];
    return 0;
}
