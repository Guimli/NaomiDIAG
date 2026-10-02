#include "eta.h"
#include "timer.h"
#include "pvr.h"

#ifndef QUICK_TEST
#define QUICK_TEST 0
#endif

/* -------------------------------------------------------------------------
 * Step durations, in milliseconds, WITHOUT the speech.
 *
 * Measured: the serial log of a real Naomi 2 running v0.16 with every
 * test working, attached to PR #3 ("after" log: 57600 baud, audio on,
 * loops relocated, MIE answering, a JVS I/O board present). Its
 * timestamps give each step with the speech included; one spoken result
 * costs about SPEECH_MS (clip + the one-second gap), taken out here and
 * added back at run time only when audio is on.
 *
 *   step                 from -> to (s)        total   base + results
 *   quick VRAM check       0.03 ->   14.56     14.5    14.5, no audio yet
 *   quick sound + replay  16.71 ->   31.59     14.9     7.4 + 2
 *   board + BIOS CRC      31.59 ->   39.24      7.6     3.9 + 1
 *   relocation            39.24 ->   76.07     36.8    29.4 + 2
 *   (v0.18 runs the BIOS CRC after the relocation, its loop cached: the
 *   3.9 s above split into 0.3 s of board identification and 3.6 s of CRC
 *   from ROM; cached, only the 2 MB of EPROM data remain: 1.06 s measured
 *   on a real Naomi 2, 2026-10-02 -- relocation 35.18 s, CRC OK 36.23 s)
 *   MIE, EEPROM, JVS      76.07 ->   95.34     19.3     4.5 + 4
 *   CPU RAM               95.34 ->  147.15     51.8    48.1 + 1
 *   VRAM TEX0 + TEX1     147.15 ->  199.83     52.7    45.3 + 2
 *                        (TEX1 passes take 8.9 s, twice TEX0's 4.1 s)
 *   sound RAM            199.83 ->  702.37    502.5   498.8 + 1
 *   PVR-B + Elan RAM     702.37 ->  793.68     91.3    83.9 + 2
 *   (since v0.18 the PVR-B and Elan RAM run before the sound RAM; the
 *   step durations are unchanged)
 *   NVRAM + RTC          793.68 ->  807.27     13.6     6.2 + 2
 *   serial EEPROM        807.27 ->  821.85     14.6    10.9 + 1
 *   relocated-code check 821.85 ->  821.93      0.1     0.5
 *   Naomi 2 total                             13:42   (this table: 13:37)
 *
 * The Naomi 1 has the same steps but the PVR-B and Elan RAM. The first
 * version of this table came from the PR #2 log, which had a fault on
 * TEX1, a MIE that did not answer and no Naomi 2 memories tested; it was
 * a minute short.
 *
 * Loops from ROM (no CPU RAM block for them): the quick VRAM check runs
 * from ROM on every board, 3 passes over 600 KB in 14.5 s, which is
 * 7.9 s per megabyte-pass -- about 14 times the cached rate. Instruction
 * fetch from the EPROM then dominates, so the same rate is applied to every
 * memory the relocated loops would have tested:
 *   CPU RAM   32 MB x 3 = 96 MB-passes  ->  755 s
 *   VRAM      16 MB x 3                 ->  378 s
 *   PVR-B     16 MB x 3                 ->  378 s
 *   Elan RAM  32 MB x 3                 ->  755 s
 * The sound RAM loops are never relocated (G2 paces them): unchanged. The
 * relocation scan itself tries up to 128 blocks from ROM, about 24 s, and
 * the MIE stage cannot run without a block.
 * ---------------------------------------------------------------------- */

#define SPEECH_MS   3700u

#if QUICK_TEST
/* 1 MB per memory instead of the whole of it */
#define Q(ms, mb)   ((ms) / (mb))
#else
#define Q(ms, mb)   (ms)
#endif

typedef struct {
    u32 ms;         /* loops relocated (cached) */
    u32 rom_ms;     /* loops from ROM */
    u8  speech;     /* spoken results */
    u8  phases;     /* progress phases, for tracking a memory step */
    u8  fast;       /* runs the relocatable loops: rescaled by measurement */
} step_est;

static const step_est est[ETA_STEPS] = {
    [ETA_VIDEO]   = { 14500, 14500, 0, 3, 0 },
    [ETA_AUDIO]   = {  7400,  7400, 2, 3, 0 },
    [ETA_BOARD]   = {   300,   300, 1, 1, 0 },
    [ETA_RELOC]   = { 29400, 24000, 2, 0, 0 },
    [ETA_BIOS]    = {  1100,  3600, 0, 0, 0 },
    [ETA_MIE]     = {  4500,     0, 4, 0, 0 },
    [ETA_SDRAM]   = { Q(48100, 32), Q(755000, 32), 1, 3, 1 },
    [ETA_VRAM]    = { Q(45300, 8),  Q(378000, 8),  2, 6, 1 },
    [ETA_ARAM]    = { Q(498800, 8), Q(498800, 8),  1, 3, 0 },
    [ETA_N2]      = { Q(83900, 24), Q(1133000, 24), 2, 6, 1 },
    [ETA_PERIPH]  = {  6200,  6200, 2, 0, 0 },
    [ETA_SEEPROM] = { 10900, 10900, 1, 0, 0 },
    [ETA_END]     = {   500,   500, 0, 0, 0 },
};

static u32 g_on;                /* counting down */
static u32 g_step;              /* current step */
static u32 g_step_t0;           /* its start, ms */
static u32 g_phases_seen;       /* progress phases begun in this step */
static u32 g_n2, g_rom, g_mie_late, g_audio;
static u32 g_scale;             /* fast-loop steps, x/256; 0 = not measured */
static u32 g_video_twice;       /* TEX1 fallback: two quick checks */
static u32 g_romk;              /* ROM execution speed vs the table, x/256
                                   (set by eta_start: no .data in this ROM) */
static u32 g_last_draw, g_last_ms, g_drawn;

/* no libgcc: shift-and-subtract */
static u32 udiv(u32 a, u32 b)
{
    if (!b)
        return 0;
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1u);
        if (r >= b) {
            r -= b;
            q |= 1u << i;
        }
    }
    return q;
}

static u32 step_ms(u32 s)
{
    const step_est *e = &est[s];
    u32 ms = g_rom ? e->rom_ms : e->ms;
    if (s == ETA_N2 && !g_n2)
        return 0;
    if (s == ETA_MIE && g_rom)
        return 0;                       /* no block: the stage is skipped */
    if (e->fast && g_scale)
        ms = udiv(ms, 256) * g_scale;
    else if (e->fast && g_rom)
        ms = udiv(ms, 256) * g_romk;
    u32 sp = e->speech;
    if (s == ETA_PERIPH && g_mie_late)
        ms += est[ETA_MIE].ms, sp += est[ETA_MIE].speech;
    if (s == ETA_N2 && !g_n2)
        sp = 0;
#if !NO_AUDIO
    if (g_audio)
        ms += sp * SPEECH_MS;
#endif
    return ms;
}

void eta_start(void)
{
    g_on = 1;
    g_step = ETA_VIDEO;
    g_step_t0 = timer_ms();
    g_phases_seen = 0;
    g_scale = 0;
    g_romk = 256;
    g_video_twice = 0;
    g_drawn = 0;
}

void eta_finish(void)
{
    if (g_drawn)
        fb_fill_rect(FB_W - 16 * 8, 8, 16 * 8, 16, 0);
    g_on = 0;
    g_drawn = 0;
}

void eta_set_board_n2(u32 n2)     { g_n2 = n2; }
void eta_set_mie_early(u32 early) { g_mie_late = !early && !g_rom; }
void eta_set_audio(u32 on)        { g_audio = on; }
void eta_video_retried(void)      { g_video_twice = 1; }

void eta_set_reloc(u32 cached)
{
    g_rom = !cached;
}

void eta_phase(void)
{
    g_phases_seen++;
}

static u32 clamp_k(u32 k)
{
    if (k < 64)
        return 64;                      /* 4x faster at most... */
    if (k > 1024)
        return 1024;                    /* ...or 4x slower: a guard, not a model */
    return k;
}

void eta_step(u32 step)
{
    u32 now = timer_ms();
    /* The quick VRAM check always runs from ROM: its duration says how fast
     * this board executes the loops from the EPROM, which is what every
     * memory step costs if no CPU RAM block takes them. */
    if (g_step == ETA_VIDEO && step > ETA_VIDEO)
        g_romk = clamp_k(udiv((now - g_step_t0) >> (g_video_twice ? 1 : 0),
                              udiv(est[ETA_VIDEO].ms, 256)));
    /* The CPU RAM test is the first step that runs the relocatable loops
     * over a known size: how long it really took against the estimate
     * rescales the later ones (VRAM, Naomi 2), cached or from ROM alike. */
    if (g_step == ETA_SDRAM && step > ETA_SDRAM) {
        u32 sp = 0;
#if !NO_AUDIO
        if (g_audio)
            sp = est[ETA_SDRAM].speech * SPEECH_MS;
#endif
        u32 took = now - g_step_t0;
        took = took > sp ? took - sp : 1;
        u32 base = g_rom ? est[ETA_SDRAM].rom_ms : est[ETA_SDRAM].ms;
        g_scale = clamp_k(udiv(took, udiv(base, 256) ? udiv(base, 256) : 1));
    }
    g_step = step;
    g_step_t0 = now;
    g_phases_seen = 0;
}

u32 eta_remaining_ms(void)
{
    if (!g_on)
        return 0;
    u32 now = timer_ms();
    u32 elapsed = now - g_step_t0;
    u32 cur = step_ms(g_step);
    u32 left;
    if (elapsed >= cur)
        left = udiv(cur, 20);           /* running late: never show zero */
    else
        left = cur - elapsed;
    for (u32 s = g_step + 1; s < ETA_STEPS; s++)
        left += step_ms(s);
    return left;
}

/* Called from the heartbeat. Inside a memory step the progress of its
 * passes gives a better answer than the table once a few percent are done:
 * the time spent so far, stretched over the fraction left. */
void eta_tick(u32 screen_ok, u32 pct)
{
    if (!g_on)
        return;
    u32 now = timer_ms();
    if (g_drawn && (u32)(now - g_last_draw) < 1000u)
        return;
    g_last_draw = now;

    u32 left = eta_remaining_ms();
    const step_est *e = &est[g_step];
    if (e->phases && g_phases_seen) {
        u32 done_pm = udiv(((g_phases_seen - 1) * 100u + pct) * 10u, e->phases);
        u32 elapsed = now - g_step_t0;
        if (done_pm >= 50 && done_pm < 1000 && elapsed > 5000) {
            u32 cur_left = udiv(elapsed, done_pm) * (1000 - done_pm);
            u32 cur = step_ms(g_step);
            u32 table_left = elapsed < cur ? cur - elapsed : udiv(cur, 20);
            left = left - table_left + cur_left;
        }
    }
    /* never climb by less than a few seconds: jitter, not information */
    if (g_drawn && left > g_last_ms && left - g_last_ms < 5000)
        left = g_last_ms;
    g_last_ms = left;

    if (!screen_ok)
        return;
    char buf[9];
    u32 n = eta_format(buf, left);
    fb_fill_rect(FB_W - 16 * 8, 8, 16 * 8, 16, 0);
    fb_text(FB_W - 16 * n - 8, 8, buf, COL_WHITE, FB_W);
    g_drawn = 1;
}

/* ms -> "m:ss", "mm:ss" or "h:mm:ss"; returns the length */
u32 eta_format(char *buf, u32 ms)
{
    u32 s = udiv(ms + 999, 1000);
    u32 h = udiv(s, 3600);
    s -= h * 3600;
    u32 m = udiv(s, 60);
    s -= m * 60;
    u32 n = 0;
    if (h) {
        buf[n++] = (char)('0' + (h > 9 ? 9 : h));
        buf[n++] = ':';
    }
    if (h || m >= 10)
        buf[n++] = (char)('0' + udiv(m, 10));
    buf[n++] = (char)('0' + (m - udiv(m, 10) * 10));
    buf[n++] = ':';
    buf[n++] = (char)('0' + udiv(s, 10));
    buf[n++] = (char)('0' + (s - udiv(s, 10) * 10));
    buf[n] = 0;
    return n;
}
