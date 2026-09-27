/* Naomi 2 CPU access setup. No display/audio callbacks may run during the
 * scratch probe: the two GPU windows have not yet been proven independent. */
#include "pvr.h"

/* Override only in the host MMIO regression harness. */
#ifndef PVR2_READ
#define PVR2_READ(a) REG32(a)
#define PVR2_WRITE(a, v) (REG32(a) = (v))
#endif

static pvr2_probe_sample samples[8];
static u32 sample_count;

const pvr2_probe_sample *pvr2_probe_samples(u32 *count)
{
    *count = sample_count;
    return samples;
}

static u32 probe_offset(u32 offset)
{
    const u32 addr[4] = { VRAM_TEX0_BASE + offset, VRAM_TEX1_BASE + offset,
                         VRAM_PVRB_BASE + offset,
                         VRAM_PVRB_BASE + 0x00800000u + offset };
    const u32 marker[4] = { 0x13579BDF, 0x2468ACE0, 0xA5963CC3, 0x5A69C33C };
    u32 saved[4], failed = 0;
    sample_count = 0;
    for (u32 i = 0; i < 4; i++)
        saved[i] = PVR2_READ(addr[i]);
    /* Opposite write orders expose one-way broadcast as well as mirrors.
     * Read every location only after all four have distinct contents. */
    for (u32 pass = 0; pass < 2; pass++) {
        for (u32 n = 0; n < 4; n++) {
            u32 i = pass ? 3 - n : n;
            PVR2_WRITE(addr[i], marker[i] ^ (pass ? 0xFFFFFFFFu : 0));
        }
        for (u32 i = 0; i < 4; i++) {
            pvr2_probe_sample *s = &samples[sample_count++];
            s->addr = addr[i];
            s->expected = marker[i] ^ (pass ? 0xFFFFFFFFu : 0);
            s->observed = PVR2_READ(addr[i]);
            if (s->observed != s->expected)
                failed |= i < 2 ? 1u : 2u;
        }
    }
    /* Restore B first, A last, preserving the visible framebuffer if B
     * aliases A. Probe offsets are outside the active framebuffer. */
    for (u32 n = 4; n; n--)
        PVR2_WRITE(addr[n - 1], saved[n - 1]);
    return failed;
}

pvr2_access pvr2_prepare(void)
{
    sample_count = 0;
    if (PVR2_READ(0xA8800000u) != 0xE1AD0000u)
        return PVR2_NO_ELAN;

    /* IFCTL: bit 2 enables CLXB, bit 1 assigns channel 2 to Elan,
     * bit 0 broadcasts CS1 writes. Independent tests require bit 0 clear. */
    u32 ctl = (PVR2_READ(0xA8800010u) | 6u) & ~1u;
    PVR2_WRITE(0xA8800010u, ctl);
    if ((PVR2_READ(0xA8800010u) & 7u) != 6u)
        return PVR2_CONTROL;
    PVR2_WRITE(0xA8800014u, 0x2029u);

    if (PVR2_READ(0xA25F8000u) != 0x17FD11DBu)
        return PVR2_NO_PVR;
    PVR2_WRITE(0xA25F8008u, 0);
    PVR2_WRITE(0xA25F80A4u, 0x1Fu);
    PVR2_WRITE(0xA25F80A8u, 0x15D1C951u);
    PVR2_WRITE(0xA25F80A0u, 0x20u);

    /* Same 32-bit windows used by the actual test, including both halves.
     * These samples are an access preflight, not a substitute for RAM tests. */
    const u32 offsets[] = { 0, 4, 0x1000, 0x007FFFFC };
    for (u32 i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        u32 failed = probe_offset(offsets[i]);
        if (failed)
            return (failed & 2) ? PVR2_MAPPING : PVR2_A_REFERENCE;
    }
    return PVR2_READY;
}
