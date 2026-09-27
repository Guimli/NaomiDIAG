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

static u32 popcount(u32 x)
{
    u32 n = 0;
    for (; x; x &= x - 1)
        n++;
    return n;
}

static void record(u32 addr, u32 expected, u32 observed, u32 unreliable)
{
    if (sample_count >= sizeof(samples) / sizeof(samples[0]))
        return;
    pvr2_probe_sample *s = &samples[sample_count++];
    s->addr = addr;
    s->expected = expected;
    s->observed = observed;
    s->unreliable = unreliable;
}

/* A cell is judged alone before anything is compared across windows: write
 * it, read it back at once, with the marker and its complement, twice. The
 * bits that fail there are that cell's own fault -- a cut data line, a dead
 * chip -- and say nothing about whether the windows are independent. They
 * used to: one broken line on IC21 failed the A reference and skipped every
 * PVR-B and Elan test on a board whose B side was never in question.
 *
 * Returns the bits that cannot be trusted at this cell. */
static u32 probe_cell(u32 addr, u32 marker)
{
    u32 bad = 0;
    for (u32 k = 0; k < 4; k++) {
        u32 v = marker ^ ((k & 1) ? 0xFFFFFFFFu : 0);
        PVR2_WRITE(addr, v);
        bad |= PVR2_READ(addr) ^ v;
    }
    return bad;
}

/* More than half a word failing alone leaves too few bits to see another
 * window's marker through. Nothing is concluded from such a cell. */
#define PROBE_MAX_UNRELIABLE 16u

static pvr2_access probe_offset(u32 offset)
{
    const u32 addr[4] = { VRAM_TEX0_BASE + offset, VRAM_TEX1_BASE + offset,
                         VRAM_PVRB_BASE + offset,
                         VRAM_PVRB_BASE + 0x00800000u + offset };
    const u32 marker[4] = { 0x13579BDF, 0x2468ACE0, 0xA5963CC3, 0x5A69C33C };
    u32 saved[4], unreliable[4];
    pvr2_access verdict = PVR2_READY;
    sample_count = 0;
    for (u32 i = 0; i < 4; i++)
        saved[i] = PVR2_READ(addr[i]);

    for (u32 i = 0; i < 4; i++) {
        unreliable[i] = probe_cell(addr[i], marker[i]);
        if (popcount(unreliable[i]) > PROBE_MAX_UNRELIABLE && verdict == PVR2_READY)
            verdict = i < 2 ? PVR2_A_REFERENCE : PVR2_B_SILENT;
    }
    if (verdict != PVR2_READY) {
        for (u32 i = 0; i < 4; i++)
            record(addr[i], marker[i], marker[i] ^ unreliable[i], unreliable[i]);
        goto restore;
    }

    /* Opposite write orders expose one-way broadcast as well as mirrors.
     * Read every location only after all four have distinct contents, and
     * look only at the bits each cell held on its own: a change there was
     * made by a write to another window. */
    for (u32 pass = 0; pass < 2; pass++) {
        for (u32 n = 0; n < 4; n++) {
            u32 i = pass ? 3 - n : n;
            PVR2_WRITE(addr[i], marker[i] ^ (pass ? 0xFFFFFFFFu : 0));
        }
        for (u32 i = 0; i < 4; i++) {
            u32 expected = marker[i] ^ (pass ? 0xFFFFFFFFu : 0);
            u32 observed = PVR2_READ(addr[i]);
            record(addr[i], expected, observed, unreliable[i]);
            if ((observed ^ expected) & ~unreliable[i])
                verdict = PVR2_MAPPING;
        }
    }

restore:
    /* Restore B first, A last, preserving the visible framebuffer if B
     * aliases A. Probe offsets are outside the active framebuffer. */
    for (u32 n = 4; n; n--)
        PVR2_WRITE(addr[n - 1], saved[n - 1]);
    return verdict;
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
        pvr2_access a = probe_offset(offsets[i]);
        if (a != PVR2_READY)
            return a;
    }
    return PVR2_READY;
}
