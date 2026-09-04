/* AICA (sound) support, SH4 side only — the ARM7 is held in reset the
 * whole time, so nothing depends on sound RAM contents but our own data.
 *
 * References: KallistiOS spu.c/g2bus.c (G2 FIFO discipline, ARM reset at
 * reg 0x2C00 bit0), MAME aica.cpp (slot register layout). */
#include "aica.h"
#include "ramtest.h"
#include "progress.h"
#include "timer.h"

#define AICA_REG(off)   REG32(0xA0700000u + (off))
#define AICA_ARMRST     AICA_REG(0x2C00)
#define AICA_MVOL       AICA_REG(0x2800)
#define G2_FIFO_STAT    REG32(0xA05F688Cu)

#define SLOT(ch, off)   REG32(0xA0700000u + (u32)(ch) * 0x80 + (off))

#define VOICE_ARAM_OFF  0x00010000u     /* clip landing zone in sound RAM */
#define VOICE_RATE      22050u

/* Wait until the G2 bus has drained, using the mask the original Naomi BIOS
 * itself polls at SB_FFST (see its loop: mov #49,rN / mov.l @rM,rK /
 * tst rN,rK / bf). 0x31 = bits 0, 4 and 5; bit 4 is the SH-4 -> G2 write
 * buffer. We previously polled 0x20 alone, i.e. we never watched that write
 * buffer at all, so a long run of back-to-back writes overran the G2 FIFO
 * and locked the bus on real hardware -- the sound RAM cell test froze right
 * after the data bus test, which writes too few words to overrun anything.
 * MAME models no FIFO, so it never showed this.
 *
 * Bounded, and a timeout is recorded rather than ignored: a G2 bus that
 * never goes idle is itself a fault worth reporting, and grinding through
 * millions of words at 0x8000 wasted reads each would just look like a
 * freeze -- exactly the symptom we are removing. */
static u32 g2_stalled;

static int g2_fifo_wait(void)
{
    for (u32 i = 0; i < 0x8000u; i++)
        if (!(G2_FIFO_STAT & 0x31))
            return 1;
    g2_stalled++;
    return 0;
}

void aica_g2_wait(void)     { g2_fifo_wait(); }
u32  aica_g2_stalled(void)  { return g2_stalled; }

void aica_init(void)
{
    g2_fifo_wait();
    /* Write 1, do not read-modify-write. The original BIOS writes this
     * register outright (mov #1,rN / mov.l rN,@rM) and so should we: a
     * read of an AICA register goes out over G2 and back, and building the
     * value we write out of whatever comes back gains nothing. */
    AICA_ARMRST = 1;                    /* ARM7 stopped: ARAM is all ours */
    g2_fifo_wait();

    /* MEM8MB matters on Naomi and cannot matter on a Dreamcast: this board
     * carries 8 MB of sound RAM where the Dreamcast has 2, and bit 9 is what
     * tells the AICA to address the larger part. Every reference for driving
     * the AICA from the SH-4 is Dreamcast code, where the bit is meaningless
     * and therefore absent -- which is exactly the kind of difference an
     * emulator does not reproduce. */
    AICA_MVOL = 0x0200 | 0x000F;        /* MEM8MB | master volume max */
    g2_fifo_wait();
    /* silence all 64 slots (KYONB=0 + KYONEX flush) */
    for (int ch = 0; ch < 64; ch++) {
        SLOT(ch, 0x00) = 0x0000;
        if ((ch & 7) == 7)
            g2_fifo_wait();
    }
    SLOT(0, 0x00) = 0x8000;             /* KYONEX: apply */
    g2_fifo_wait();
}

/* ---- sound RAM tests (same patterns as SDRAM, G2 FIFO aware) ---- */

u32 aram_test_databus(void)
{
    volatile u32 *p = (volatile u32 *)ARAM_P2_BASE;
    u32 bad = 0;
    for (u32 bit = 1; bit != 0; bit <<= 1) {
        g2_fifo_wait();
        *p = bit;
        g2_fifo_wait();
        bad |= *p ^ bit;
        g2_fifo_wait();
        *p = ~bit;
        g2_fifo_wait();
        bad |= *p ^ ~bit;
    }
    return bad;
}

static void note_fail(ram_result *r, u32 addr, u32 exp, u32 got)
{
    r->errors++;
    r->badbits |= exp ^ got;
    if (addr & 4)
        r->badbits_o |= exp ^ got;
    else
        r->badbits_e |= exp ^ got;
    if (r->nfails < RAM_MAX_FAILS) {
        r->fail_addr[r->nfails] = addr;
        r->fail_exp [r->nfails] = exp;
        r->fail_got [r->nfails] = got;
        r->nfails++;
    }
}

void aram_test_pattern(u32 off, u32 len, u32 pattern, ram_result *r)
{
    volatile u32 *p = (volatile u32 *)(ARAM_P2_BASE + off);
    u32 n = len >> 2;
    for (u32 i = 0; i < n; i++) {
        if ((i & 7) == 0 && !g2_fifo_wait())
            return;                     /* G2 stalled: reported by caller */
        if ((i & 1023) == 0)
            progress_tick(i);
        p[i] = pattern;
    }
    for (u32 i = 0; i < n; i++) {
        if ((i & 1023) == 0)
            progress_tick(n + i);
        u32 got = p[i];
        if (got != pattern)
            note_fail(r, ARAM_P2_BASE + off + (i << 2), pattern, got);
    }
}



static inline u32 xorshift32(u32 x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

void aram_test_prng(u32 off, u32 len, u32 seed, ram_result *r)
{
    volatile u32 *p = (volatile u32 *)(ARAM_P2_BASE + off);
    u32 n = len >> 2;
    register u32 crc_w = 0xFFFFFFFF;
    register u32 crc_r = 0xFFFFFFFF;
    u32 x = seed ? seed : 1;

    for (u32 i = 0; i < n; i++) {
        x = xorshift32(x);
        if ((i & 7) == 0 && !g2_fifo_wait())
            return;                     /* G2 stalled: reported by caller */
        if ((i & 1023) == 0)
            progress_tick(i);
        p[i] = x;
        crc_w = crc32_word(crc_w, x);
    }
    x = seed ? seed : 1;
    for (u32 i = 0; i < n; i++) {
        x = xorshift32(x);
        if ((i & 1023) == 0)
            progress_tick(n + i);
        u32 got = p[i];
        crc_r = crc32_word(crc_r, got);
        if (got != x)
            note_fail(r, ARAM_P2_BASE + off + (i << 2), x, got);
    }
    r->crc_w = ~crc_w;
    r->crc_r = ~crc_r;
    if (crc_w != crc_r && r->errors == 0)
        note_fail(r, ARAM_P2_BASE + off, crc_w, crc_r);
}

/* A plain square wave, generated here rather than played from a recorded
 * clip. It isolates the analog chain: if this is audible, the AICA, the
 * board's output stage and the cabinet wiring all work and any remaining
 * silence belongs to the clips or their timing. If it is not, the fault is
 * upstream of anything the diagnostic can say with a voice.
 *
 * It loops out of a 500-sample buffer -- ten whole periods at 22050 Hz, so
 * the loop point falls on a zero crossing and does not click. */
#define TONE_ARAM_OFF   0x00008000u
#define TONE_SAMPLES    500u
#define TONE_PERIOD     50u

void aica_tone(u32 ms)
{
    volatile u16 *dst = (volatile u16 *)(ARAM_P2_BASE + TONE_ARAM_OFF);
    for (u32 i = 0; i < TONE_SAMPLES; i++) {
        if ((i & 7) == 0 && !g2_fifo_wait())
            return;
        u32 phase = i % TONE_PERIOD;
        dst[i] = (u16)(phase < TONE_PERIOD / 2 ? 8000 : (u16)-8000);
    }

    g2_fifo_wait();
    u32 sa = TONE_ARAM_OFF;
    SLOT(0, 0x00) = (u32)(0x0200 | ((sa >> 16) & 0x7F));  /* LPCTL: loop */
    SLOT(0, 0x04) = sa & 0xFFFF;
    SLOT(0, 0x08) = 0;                       /* LSA */
    SLOT(0, 0x0C) = TONE_SAMPLES - 1;        /* LEA */
    SLOT(0, 0x10) = 0x001F;                  /* AR max */
    SLOT(0, 0x14) = 0x001F;                  /* RR max, DL=0 */
    g2_fifo_wait();
    SLOT(0, 0x18) = (u32)(0xF << 11);        /* OCT=-1: 22050 Hz */
    SLOT(0, 0x1C) = 0;
    SLOT(0, 0x20) = 0;
    SLOT(0, 0x24) = 0x0F00;                  /* DISDL max, pan centre */
    SLOT(0, 0x28) = 0;                       /* TL 0: no attenuation */
    g2_fifo_wait();
    SLOT(0, 0x00) = (u32)(0x8000 | 0x4000 | 0x0200 | ((sa >> 16) & 0x7F));
    g2_fifo_wait();

    progress_wait_ms(ms);

    g2_fifo_wait();
    SLOT(0, 0x00) = (u32)(0x8000 | 0x0200 | ((sa >> 16) & 0x7F));  /* key off */
    g2_fifo_wait();
}

/* ---- speech ---- */


void aica_say(const signed char *pcm, u32 len, u32 gap_ms)
{
    u32 samples = len >> 1;          /* 16-bit PCM (PCMS=0) */
    if (samples > 0xFFF0) {
        samples = 0xFFF0;            /* LEA is 16-bit: max ~2.9 s per clip */
        len = samples << 1;
    }
    /* copy clip into sound RAM, 8 words per FIFO window */
    volatile u32 *dst = (volatile u32 *)(ARAM_P2_BASE + VOICE_ARAM_OFF);
    const u32 *src = (const u32 *)pcm;
    u32 nw = (len + 3) >> 2;
    for (u32 i = 0; i < nw; i++) {
        if ((i & 7) == 0)
            g2_fifo_wait();
        dst[i] = src[i];
    }

    g2_fifo_wait();
    /* key off + settings: PCMS=0 (16-bit LE), no loop, SA = clip address */
    u32 sa = VOICE_ARAM_OFF;
    SLOT(0, 0x00) = (u32)((sa >> 16) & 0x7F);
    SLOT(0, 0x04) = sa & 0xFFFF;
    SLOT(0, 0x08) = 0;                       /* LSA */
    SLOT(0, 0x0C) = samples;                 /* LEA in samples (16-bit) */
    SLOT(0, 0x10) = 0x001F;                  /* AR max, D1R=D2R=0 */
    SLOT(0, 0x14) = 0x001F;                  /* RR max, DL=0 */
    g2_fifo_wait();
    SLOT(0, 0x18) = (u32)(0xF << 11);        /* OCT=-1: 44100/2 = 22050 Hz */
    SLOT(0, 0x1C) = 0;                       /* no LFO */
    SLOT(0, 0x20) = 0;                       /* no DSP send */
    SLOT(0, 0x24) = 0x0F00;                  /* DISDL max, pan center */
    SLOT(0, 0x28) = 0;                       /* TL 0 = full level */
    g2_fifo_wait();
    /* key on */
    SLOT(0, 0x00) = (u32)(0x8000 | 0x4000 | ((sa >> 16) & 0x7F));
    g2_fifo_wait();

    /* blocking wait: clip duration + user-mandated inter-report gap.
     * ms = samples * (1000/22050); 2972/65536 = 0.045349 (no libgcc div) */
    u32 dur_ms = (samples * 2972u) >> 16;
    progress_wait_ms(dur_ms + 60);

    g2_fifo_wait();
    SLOT(0, 0x00) = (u32)(0x8000 | ((sa >> 16) & 0x7F)); /* key off */
    g2_fifo_wait();
    progress_wait_ms(gap_ms);
}
