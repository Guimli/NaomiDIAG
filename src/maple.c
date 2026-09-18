/* Maple bus scan: send a Device Request (command 1) to the primary
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

    volatile u32 *desc = (volatile u32 *)MAPLE_DESC_P2;
    volatile u32 *rx   = (volatile u32 *)MAPLE_RX_P2;

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
    SB_MDEN   = 1;
    SB_MSYS   = 0xC3500000;                     /* 50000 timeout, 2 Mbps */
    SB_MDAPRO = 0x6155404F;                     /* allow whole RAM       */
    SB_MDSTAR = MAPLE_DESC_PHY;
    SB_MDST   = 1;

    for (u32 spin = 0; spin < 1000000; spin++)
        if (!(SB_MDST & 1))
            break;
    delay_ms(2);
    return rx[0];
}

void maple_scan(maple_result *mr)
{
    volatile u32 *rx = (volatile u32 *)MAPLE_RX_P2;

    mr->found_port = 0xFFFFFFFF;
    mr->response_cmd = 0xFF;
    mr->func_codes = 0;
    for (u32 i = 0; i < sizeof mr->id; i++)
        mr->id[i] = 0;

    for (u32 port = 0; port < 4; port++) {
        /* MIE protocol (libnaomi): 0x82 = version request -> 0x83 */
        u32 hdr = maple_txn(port, 0x82, 0, 0);
        u32 cmd = hdr & 0xFF;
        if (hdr == 0xFFFFFFFF || hdr == 0xDEADDEAD || cmd == 0xFF)
            continue;                            /* no device / timeout  */
        mr->found_port = port;
        mr->response_cmd = cmd;
        mr->func_codes = rx[1];
        /* 0x83 response: ASCII version string in the payload words */
        const volatile u8 *blk = (const volatile u8 *)(MAPLE_RX_P2 + 4);
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
    volatile u32 *rx = (volatile u32 *)MAPLE_RX_P2;
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
    volatile u32 *rx = (volatile u32 *)MAPLE_RX_P2;
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
    volatile u32 *rx = (volatile u32 *)MAPLE_RX_P2;

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
    volatile u32 *rx = (volatile u32 *)MAPLE_RX_P2;
    u32 pay = 0;
    u32 hdr = maple_txn(port, 0xE2, 1, &pay);
    if ((hdr & 0xFF) != 0xE3)
        return 1;
    *state = (u8)(rx[1] & 0xFF);
    return 0;
}

/* Read the JVS control state through the MIE: command 0x86 with subcommand
 * 0x15, answered by 0x87 with subresponse 0x16 and 0x0E payload words.
 *
 * The protocol shape is documented (DragonMinded/netboot, docs/naomi.md);
 * the BIT POSITIONS of TEST, SERVICE and the per-player buttons inside those
 * words are NOT -- that document says outright that they were never mapped
 * and that the way to find them is to print the response and press things.
 * So this returns the raw words and the mapping is established by
 * measurement, not by assumption. Returns 0 on success. */
u32 maple_jvs_read(u32 port, u32 out[14])
{
    volatile u32 *rx = (volatile u32 *)MAPLE_RX_P2;
    u32 pay = 0x00000015;
    u32 hdr = maple_txn(port, 0x86, 1, &pay);

    if ((hdr & 0xFF) != 0x87)
        return 1;
    for (u32 i = 0; i < 14; i++)
        out[i] = rx[1 + i];
    return 0;
}

/* Ask the JVS I/O board at `addr` for its switch state. The MIE relays it on
 * the JVS bus and holds the reply until the next 0x15 read, so this is sent
 * one poll ahead of the read that collects it.
 *
 * Subcommand 0x27 with a twelve-byte body, the shape libnaomi uses. The 0x77
 * in the second byte is described there as a GPIO direction that these
 * packets carry "for some reason"; it is reproduced rather than reasoned
 * about. Returns 0 if the MIE accepted it. */
u32 maple_jvs_request(u32 port, u32 addr)
{
    u32 pay[3];
    pay[0] = 0x00007727u;                       /* 0x27, 0x77, 0, 0 */
    pay[1] = (addr & 0xFFu) << 16 | 0x01000000u;/* 0, 0, addr, 1    */
    pay[2] = 0x00000000u;
    return (maple_txn(port, 0x86, 3, pay) & 0xFF) == 0x87 ? 0 : 1;
}
