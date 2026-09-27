/* Exercise the production Maple transport with enforced DMA bounds. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HW_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
static u32 regs[0x100/4], isterr, memory[128], ticks, mode, transfers, accesses;
static u32 base, protection_seen;
static volatile u32 *reg(u32 addr)
{
    if (addr == 0xA05F6908) return &isterr;
    assert(addr >= 0xA05F6C00 && addr < 0xA05F6D00);
    return &regs[(addr - 0xA05F6C00)/4];
}
static volatile u32 *buffer(u32 addr)
{
    assert(addr >= base && addr < base + sizeof memory);
    accesses++;
    return &memory[(addr-base)/4];
}
#define REG32(a) (*reg(a))
#define MAPLE_BUFFER(a) buffer(a)
#include "../src/maple.c"

u32 timer_ticks(void)
{
    ticks += TIMER_HZ / 1000;
    if ((SB_MDST & 1) && mode != 1) {
        transfers++;
        protection_seen = SB_MDAPRO;
        u32 lo = ((SB_MDAPRO >> 8) & 0x7F) + 0x80;
        u32 hi = (SB_MDAPRO & 0x7F) + 0x80;
        assert((SB_MDAPRO >> 16) == 0x6155);
        assert((SB_MDSTAR >> 20) >= lo);
        assert(((memory[1] + 255) >> 20) <= hi);
        assert(SB_MDEN == 1);
        assert(memory[0] & 0x80000000u);
        u32 port = (memory[0] >> 16) & 3;
        if (mode == 2) memory[64] = 0xFFFFFFFF;
        else if (mode == 3) { /* DMA completed without writing the sentinel. */ }
        else if (mode == 4 && !port) memory[64] = 0x000000FD;
        else if (mode == 5 && !port) memory[64] = 0x00000083;
        else if (mode == 6) {
            assert((memory[2] & 0xFF) == 0xE4);
            assert((memory[2] >> 24) == 1 && memory[3] == 3);
            memory[64] = 0x070000E5;
            for (u32 i = 0; i < 7; i++) memory[65 + i] = 0x11223300 + i;
        }
        else {
            memory[64] = 0x01000083;
            memory[65] = 0x0045494D; /* MIE */
        }
        SB_MDST = 0;
    }
    return ticks;
}
void delay_ms(u32 ms) { (void)ms; }
void scif_puts(const char *s) { assert(s); }
void scif_puthex(u32 n) { (void)n; }
void scif_putdec(u32 n) { (void)n; }

static void setup(u32 addr, u32 fault)
{
    memset(regs, 0, sizeof regs);
    memset(memory, 0, sizeof memory);
    base = addr; mode = fault; ticks = transfers = accesses = 0;
    isterr = 0x1234; /* Must remain untouched by diagnosis. */
    maple_set_buffers(addr);
}
static void check_scan(u32 addr, u32 expected)
{
    maple_result r;
    setup(addr, 0);
    maple_scan(&r);
    assert(r.found_port == 0 && r.response_cmd == 0x83);
    assert(!strcmp(r.id, "MIE "));
    assert(protection_seen == expected && transfers == 1);
    assert(isterr == 0x1234);
}
int main(void)
{
    check_scan(0xAC0FF000, 0x61554040);
    check_scan(0xACFFF800, 0x61554F4F);
    check_scan(0xADFFD800, 0x61555F5F);
    check_scan(0xACFFFF00, 0x61554F50); /* RX crosses the 16 MiB boundary. */
    maple_result r;
    setup(0xAC0FF000, 0);
    g_desc_p2 = g_rx_p2 = 0; /* First-use fallback must precede RX pointer creation. */
    maple_scan(&r);
    assert(r.found_port == 0);
    setup(0xADFFD800, 6);
    u8 info[28];
    assert(maple_jvs_info(0, 3, info) == 0);
    assert(!memcmp(info, &memory[65], sizeof info));
    assert(protection_seen == 0x61555F5F);
    setup(0xADFFD800, 2);
    assert(maple_jvs_info(0, 3, info) == 1);
    setup(0xADFFD800, 1);
    ticks = 0xFFFF0000u; /* Deadline subtraction across timer wrap. */
    assert(maple_txn(0, 0x82, 0, 0) == 0xFFFFFFFF);
    assert(!strcmp(g_transaction_status, "dma-timeout") && SB_MDEN == 0);
    u32 old_accesses = accesses;
    assert(maple_txn(1, 0x82, 0, 0) == 0xFFFFFFFF);
    assert(!strcmp(g_transaction_status, "busy-timeout") && accesses == old_accesses);
    for (u32 fault = 2; fault <= 5; fault++) {
        setup(0xADFFD800, fault);
        maple_scan(&r);
        assert(r.found_port == (fault < 4 ? 0xFFFFFFFFu : 1));
        if (fault == 2) assert(!strcmp(g_transaction_status, "no-response"));
        if (fault == 3) assert(!strcmp(g_transaction_status, "rx-unchanged"));
    }
    setup(0xADFFFF00, 0); /* Reservation exceeds the physical RAM window. */
    assert(maple_txn(0, 0x82, 0, 0) == 0xFFFFFFFF && !accesses);
    setup(0xADFFD804, 0); /* Descriptor must be 32-byte aligned. */
    assert(maple_txn(0, 0x82, 0, 0) == 0xFFFFFFFF && !accesses);
    setup(0xADFFD800, 0);
    assert(maple_txn(0, 0x82, 62, 0) == 0xFFFFFFFF && !accesses);
    puts("Maple: DMA bounds, high RAM, fallback, timeouts and response checks passed");
    return 0;
}
