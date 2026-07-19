/* Maple bus scan: send a Device Request (command 1) to the primary
 * device address of each port and collect the response.
 * Register/protocol references: KallistiOS maple driver, MAME maple-dc. */
#include "maple.h"
#include "timer.h"
#include "scif.h"

#define SB_MDSTAR   REG32(0xA05F6C04)   /* DMA descriptor table (phys)   */
#define SB_MDTSEL   REG32(0xA05F6C10)   /* trigger: 0 = software         */
#define SB_MDEN     REG32(0xA05F6C14)   /* DMA enable                    */
#define SB_MDST     REG32(0xA05F6C18)   /* DMA start / busy              */
#define SB_MSYS     REG32(0xA05F6C80)   /* timeout / bitrate             */
#define SB_MDAPRO   REG32(0xA05F6C8C)   /* address protection window     */

/* scratch area in validated main RAM (P2 view / physical for the DMA) */
#define MAPLE_DESC_P2   0xAC0FF000u
#define MAPLE_DESC_PHY  0x0C0FF000u
#define MAPLE_RX_P2     0xAC0FF100u
#define MAPLE_RX_PHY    0x0C0FF100u

static u32 maple_txn(u32 port, u32 cmd, u32 nwords, const u32 *payload)
{
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

u32 maple_eeprom_read(u32 port, u8 *out128)
{
    volatile u32 *rx = (volatile u32 *)MAPLE_RX_P2;
    u32 pay;

    pay = 0x00000001;                       /* start EEPROM -> MIE read */
    u32 hdr = maple_txn(port, 0x86, 1, &pay);
    scif_puts("  [mie] start-read resp hdr ");
    scif_puthex(hdr);
    scif_puts(" w1 ");
    scif_puthex(rx[1]);
    scif_puts("\n");

    for (u32 tries = 0; tries < 50; tries++) {
        delay_ms(10);
        pay = 0x00000003;                   /* fetch read result */
        hdr = maple_txn(port, 0x86, 1, &pay);
        if (tries < 1) {
            scif_puts("  [mie] fetch resp hdr ");
            scif_puthex(hdr);
            scif_puts(" w1 ");
            scif_puthex(rx[1]);
            scif_puts("\n");
        }
        if ((hdr & 0xFF) == 0x87 && ((hdr >> 24) & 0xFF) >= 32) {
            /* fetch response: 32 payload words = the 128 EEPROM bytes */
            const volatile u8 *src = (const volatile u8 *)&rx[1];
            for (u32 i = 0; i < 128; i++)
                out128[i] = src[i];
            return 0;
        }
    }
    return 1;
}
