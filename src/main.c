/* naomi-diag main: orchestrates the tests and the report.
 *
 * Output escalation (user spec):
 *   1. SCIF only (no RAM at all)
 *   2. SDRAM test; if at least one bank works, later stages may use it
 *   3. AICA/sound RAM test -> spoken reports on audio + SCIF simultaneously,
 *      starting with a spoken replay of the results acquired so far
 *   4. PowerVR VRAM test -> on-screen report (TODO)
 *
 * Audio discipline (user spec): every spoken report is blocking — the tests
 * pause until the clip has finished playing, and at least 1 s of silence
 * separates consecutive reports. */
#include "hw.h"
#include "scif.h"
#include "sdram.h"
#include "ramtest.h"
#include "timer.h"
#include "aica.h"
#include "pvr.h"
#include "periph.h"
#include "dimm.h"
#include "audio_clips.h"

#ifndef QUICK_TEST
#define QUICK_TEST 0
#endif

#define N_PASSES    10
#define REPORT_GAP_MS 1000          /* >= 1 s between spoken reports */

#define CLIP_NONE   0xFFFFFFFFu

static u32 g_audio_ready;           /* sound RAM validated, AICA usable */

/* ------------------------------------------------------------------ */
/* Replayable result log (lives in OC-RAM .bss, survives until reset) */
/* ------------------------------------------------------------------ */
#define LOG_MAX 32
typedef enum { T_OK = 0, T_FAIL = 1 } t_status;

/* numbered position -> silkscreen IC (see analysis/ADDRESS_MAP.md).
 * Mapping extracted from the original BIOS RAM TEST tables; the WORK
 * lane ORDER is a hypothesis until confirmed by a forced fault on real
 * hardware. Sound RAM is a single chip: every lane maps to IC29. */
typedef struct { u32 clip; const char *name; } comp_map;

static const comp_map work_comps[4] = {
    { CLIP_IC_16, "IC16" },         /* D0-D15,  even word */
    { CLIP_IC_18, "IC18" },         /* D16-D31, even word */
    { CLIP_IC_20, "IC20" },         /* D0-D15,  odd word  */
    { CLIP_IC_22, "IC22" },         /* D16-D31, odd word  */
};
static const comp_map aram_comps[4] = {
    { CLIP_IC_29, "IC29" }, { CLIP_IC_29, "IC29" },
    { CLIP_IC_29, "IC29" }, { CLIP_IC_29, "IC29" },
};
static const comp_map bios_comps[4] = {
    { CLIP_IC_27, "IC27" }, { CLIP_IC_27, "IC27" },
    { CLIP_IC_27, "IC27" }, { CLIP_IC_27, "IC27" },
};
static const comp_map tex0_comps[4] = {   /* lane order = hypothesis */
    { CLIP_IC_9,  "IC9"  },         /* D0-D15,  even word */
    { CLIP_IC_10, "IC10" },         /* D16-D31, even word */
    { CLIP_IC_11, "IC11" },         /* D0-D15,  odd word  */
    { CLIP_IC_12, "IC12" },         /* D16-D31, odd word  */
};
static const comp_map tex1_comps[4] = {   /* single 64Mbit chip */
    { CLIP_IC_35, "IC35" }, { CLIP_IC_35, "IC35" },
    { CLIP_IC_35, "IC35" }, { CLIP_IC_35, "IC35" },
};

typedef struct {
    const char *name;               /* points into ROM */
    u32 clip;                       /* CLIP_xxx id or CLIP_NONE */
    t_status status;
    u32 detail;                     /* component bitmask (bit n = comp n+1) */
    const comp_map *comps;          /* NULL, or 4-entry position->IC table */
} log_entry;

static log_entry g_log[LOG_MAX];
static u32 g_log_n;
static u32 g_screen_ready;          /* TEX0 VRAM validated, display up */

/* full-screen render of the whole log (screen = 3rd report channel) */
static void screen_render(void)
{
    if (!g_screen_ready)
        return;
    fb_clear(0);
    fb_text(112, 8, "NAOMI DIAG ROM v0.3", COL_TITLE);
    u32 y = 48;
    for (u32 i = 0; i < g_log_n && y < FB_H - 20; i++) {
        const log_entry *e = &g_log[i];
        fb_text(16, y, e->name, COL_WHITE);
        if (e->status == T_OK) {
            fb_text(FB_W - 16 * 3, y, "OK", COL_GREEN);
        } else {
            fb_text(FB_W - 16 * 5, y, "FAIL", COL_RED);
            if (e->detail && e->comps) {
                y += 20;
                u32 x = 32;
                const char *last = 0;
                for (u32 b = 0; b < 4 && x < FB_W - 80; b++) {
                    if (!(e->detail & (1u << b)) || e->comps[b].name == last)
                        continue;
                    last = e->comps[b].name;
                    fb_text(x, y, e->comps[b].name, COL_RED);
                    x += 16 * 5;
                }
            }
        }
        y += 20;
    }
}

static void say(u32 clip, u32 gap_ms)
{
    if (g_audio_ready && clip != CLIP_NONE)
        aica_say(audio_clips[clip].pcm, audio_clips[clip].len, gap_ms);
}

/* speak one result. OK: "<name> test réussi". FAIL with located
 * components: one full report per component, using the silkscreen IC
 * name when the mapping is known ("Mémoire principale, I C seize,
 * défectueuse") and the position number otherwise. Duplicate IC clips
 * (single-chip RAMs) are only spoken once. */
static void say_entry(const log_entry *e)
{
    if (!g_audio_ready || e->clip == CLIP_NONE)
        return;
    if (e->status == T_OK) {
        say(e->clip, 250);
        say(CLIP_OK, 250);
        delay_ms(REPORT_GAP_MS);
        return;
    }
    if (e->detail == 0) {                   /* fault w/o component info */
        say(e->clip, 250);
        say(CLIP_FAIL, 250);
        delay_ms(REPORT_GAP_MS);
        return;
    }
    u32 spoken = CLIP_NONE;
    for (u32 i = 0; i < 8; i++) {
        if (!(e->detail & (1u << i)))
            continue;
        u32 c = (e->comps && i < 4) ? e->comps[i].clip : CLIP_NUM_1 + i;
        if (c == spoken)
            continue;
        spoken = c;
        say(e->clip, 250);
        say(c, 250);
        say(CLIP_DEFECT, 250);
        delay_ms(REPORT_GAP_MS);
    }
}

static void log_result(const char *name, u32 clip, t_status st, u32 detail,
                       const comp_map *comps)
{
    if (g_log_n < LOG_MAX) {
        g_log[g_log_n].name = name;
        g_log[g_log_n].clip = clip;
        g_log[g_log_n].status = st;
        g_log[g_log_n].detail = detail;
        g_log[g_log_n].comps = comps;
        g_log_n++;
    }
    scif_puts(name);
    scif_puts(st == T_OK ? " ........ OK\n" : " ........ FAIL\n");
    /* live multi-channel report: screen refresh, then speech */
    screen_render();
    if (g_log_n)
        say_entry(&g_log[g_log_n - 1]);
}

/* ------------------------------------------------------------------ */
static void report_badbits(u32 badbits)
{
    scif_puts("  bad data bits mask: ");
    scif_puthex(badbits);
    scif_puts("\n");
}

/* located components: position number + silkscreen IC name when known */
static void report_comps(const char *ramname, u32 compmask,
                         const comp_map *comps)
{
    for (u32 i = 0; i < 8; i++) {
        if (!(compmask & (1u << i)))
            continue;
        scif_puts("  -> ");
        scif_puts(ramname);
        scif_puts(" ");
        scif_putdec(i + 1);
        if (comps && i < 4) {
            scif_puts(" (");
            scif_puts(comps[i].name);
            scif_puts(")");
        }
        scif_puts(" DEFECTIVE\n");
    }
}

static void report_fails(const ram_result *r)
{
    for (u32 i = 0; i < r->nfails; i++) {
        scif_puts("  @");
        scif_puthex(r->fail_addr[i]);
        scif_puts(" wrote ");
        scif_puthex(r->fail_exp[i]);
        scif_puts(" read ");
        scif_puthex(r->fail_got[i]);
        scif_puts("\n");
    }
    if (r->errors > r->nfails) {
        scif_puts("  ... ");
        scif_putdec(r->errors);
        scif_puts(" errors total\n");
    }
}

/* ------------------------------------------------------------------ */
/* BIOS EPROM (IC27) self-test: CRC32 of the whole ROM but its last 4
 * bytes, where the build system stored the expected value. Catches worn
 * EPROM cells and oxidised DIP42 socket contacts. Runs from ROM with the
 * OC-RAM stack only. */
static const u32 bios_crc4tab[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
};

static void test_bios_rom(void)
{
    const volatile u32 *rom = (const volatile u32 *)0xA0000000;
    u32 n = (0x00200000 - 4) >> 2;
    register u32 crc = 0xFFFFFFFF;
    for (u32 i = 0; i < n; i++) {
        crc ^= rom[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 4) ^ bios_crc4tab[crc & 0xF];
    }
    crc = ~crc;
    u32 expect = rom[n];
    t_status st = (crc == expect) ? T_OK : T_FAIL;
    log_result("BIOS ROM (IC27) CRC32", CLIP_BIOS, st, st == T_FAIL ? 1 : 0,
               bios_comps);
    if (st == T_FAIL) {
        scif_puts("  computed ");
        scif_puthex(crc);
        scif_puts(" expected ");
        scif_puthex(expect);
        scif_puts("\n");
    }
}

/* ------------------------------------------------------------------ */
static u32 test_sdram(void)
{
    scif_puts("\nSDRAM init (BSC values from original BIOS)...\n");
    u32 size = sdram_init();
    scif_puts("SDRAM detected size: ");
    scif_putdec(size >> 20);
    scif_puts(" MB\n");

    /* data bus test runs at an even word address (A2=0): comps 1/2 */
    u32 bad = ram_test_databus(SDRAM_P2_BASE);
    u32 comps = (bad & 0xFFFF ? 1u : 0) | (bad >> 16 ? 2u : 0);
    log_result("SDRAM data bus", CLIP_DATA_BUS, bad ? T_FAIL : T_OK, comps,
                work_comps);
    if (bad) {
        report_badbits(bad);
        report_comps("CPU RAM", comps, work_comps);
    }

    bad = ram_test_addrbus(SDRAM_P2_BASE, size);
    log_result("SDRAM address bus", CLIP_ADDR_BUS, bad ? T_FAIL : T_OK, 0, 0);
    if (bad) {
        scif_puts("  bad address bits mask: ");
        scif_puthex(bad);
        scif_puts("\n");
    }

#if QUICK_TEST
    u32 len = 0x00100000;
    scif_puts("QUICK build: testing first 1MB only\n");
#else
    u32 len = size;
#endif

    ram_result res;
    ram_result_clear(&res);
    for (u32 pass = 0; pass < N_PASSES; pass++) {
        scif_puts("pass ");
        scif_putdec(pass + 1);
        scif_puts("/10: 5555");
        ram_test_pattern(SDRAM_P2_BASE, len, 0x55555555, &res);
        scif_puts(" AAAA");
        ram_test_pattern(SDRAM_P2_BASE, len, 0xAAAAAAAA, &res);
        scif_puts(" PRNG");
        u32 seed = 0xDEADBEEF ^ (0x9E3779B9u * (pass + 1));
        ram_test_prng(SDRAM_P2_BASE, len, seed, &res);
        scif_puts(" crc=");
        scif_puthex(res.crc_r);
        scif_puts(res.errors ? " ERR\n" : " ok\n");
    }

    t_status st = res.errors ? T_FAIL : T_OK;
    log_result("SDRAM cell test (10 passes)", CLIP_CPU_RAM, st,
               ram_comp_mask(&res), work_comps);
    if (res.errors) {
        report_badbits(res.badbits);
        report_comps("CPU RAM", ram_comp_mask(&res), work_comps);
        report_fails(&res);
        return 0;
    }
    return size;
}

/* ------------------------------------------------------------------ */
static u32 test_aram(void)
{
    scif_puts("\nAICA: ARM7 held in reset, testing sound RAM (8MB, G2 bus)...\n");
    aica_init();

    u32 bad = aram_test_databus();
    u32 comps = (bad & 0xFFFF ? 1u : 0) | (bad >> 16 ? 2u : 0);
    log_result("Sound RAM data bus", CLIP_DATA_BUS, bad ? T_FAIL : T_OK, comps,
                aram_comps);
    if (bad) {
        report_badbits(bad);
        report_comps("SOUND RAM", comps, aram_comps);
    }

#if QUICK_TEST
    u32 len = 0x00100000;
    scif_puts("QUICK build: testing first 1MB only\n");
#else
    u32 len = ARAM_SIZE;
#endif

    ram_result res;
    ram_result_clear(&res);
    for (u32 pass = 0; pass < N_PASSES; pass++) {
        scif_puts("pass ");
        scif_putdec(pass + 1);
        scif_puts("/10: 5555");
        aram_test_pattern(0, len, 0x55555555, &res);
        scif_puts(" AAAA");
        aram_test_pattern(0, len, 0xAAAAAAAA, &res);
        scif_puts(" PRNG");
        u32 seed = 0xC0FFEE42 ^ (0x9E3779B9u * (pass + 1));
        aram_test_prng(0, len, seed, &res);
        scif_puts(" crc=");
        scif_puthex(res.crc_r);
        scif_puts(res.errors ? " ERR\n" : " ok\n");
    }

    t_status st = res.errors ? T_FAIL : T_OK;
    log_result("Sound RAM cell test (10 passes)", CLIP_SOUND_RAM, st,
               ram_comp_mask(&res), aram_comps);
    if (res.errors) {
        report_badbits(res.badbits);
        report_comps("SOUND RAM", ram_comp_mask(&res), aram_comps);
        report_fails(&res);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
static void test_vram_region(const char *name, u32 base, u32 size,
                             const comp_map *comps, u32 name_clip,
                             u32 *ok_flag)
{
    ram_result res;
    ram_result_clear(&res);
#if QUICK_TEST
    (void)size;
#endif

    u32 bad = vram_test_databus(base);
    if (bad) {
        u32 c = (bad & 0xFFFF ? 1u : 0) | (bad >> 16 ? 2u : 0);
        log_result(name, name_clip, T_FAIL, c, comps);
        report_badbits(bad);
        report_comps(name, c, comps);
        *ok_flag = 0;
        return;
    }

#if QUICK_TEST
    u32 len = 0x00100000;
#else
    u32 len = size;
#endif
    for (u32 pass = 0; pass < N_PASSES; pass++) {
        vram_test_pattern(base, len, 0x55555555, &res);
        vram_test_pattern(base, len, 0xAAAAAAAA, &res);
        u32 seed = 0x7E0CBEEF ^ (0x9E3779B9u * (pass + 1)) ^ base;
        vram_test_prng(base, len, seed, &res);
        scif_putc('.');
    }
    scif_putc('\n');

    t_status st = res.errors ? T_FAIL : T_OK;
    log_result(name, name_clip, st, ram_comp_mask(&res), comps);
    if (res.errors) {
        report_badbits(res.badbits);
        report_comps(name, ram_comp_mask(&res), comps);
        report_fails(&res);
        *ok_flag = 0;
    }
}

static u32 test_vram(void)
{
    scif_puts("\nPVR: enabling VRAM controller, testing texture RAM...\n");
    pvr_vram_enable();
    u32 tex0_ok = 1, tex1_ok = 1;
    test_vram_region("VRAM TEX0 (IC9-12)", VRAM_TEX0_BASE, VRAM_TEX0_SIZE,
                     tex0_comps, CLIP_VRAM, &tex0_ok);
    test_vram_region("VRAM TEX1 (IC35)", VRAM_TEX1_BASE, VRAM_TEX1_SIZE,
                     tex1_comps, CLIP_VRAM, &tex1_ok);
    return tex0_ok;                     /* the framebuffer lives in TEX0 */
}

/* ------------------------------------------------------------------ */
static void test_sram_rtc(void)
{
    scif_puts("\nBackup SRAM (2x 62256, non-destructive) + AICA RTC...\n");

    ram_result res;
    u32 mask = sram_test(&res, N_PASSES);
    /* positions 1/2 = even/odd byte lane; IC designators pending the
     * user's silkscreen readout -> spoken as numbered positions */
    log_result("Backup SRAM (non-destructive)", CLIP_SRAM,
               mask ? T_FAIL : T_OK, mask, 0);
    if (mask) {
        report_badbits(res.badbits);
        report_comps("SRAM", mask, 0);
        report_fails(&res);
    }

    u32 rtcval = 0;
    u32 bad = rtc_test(&rtcval);
    log_result("RTC (AICA, must tick)", CLIP_RTC, bad ? T_FAIL : T_OK, 0, 0);
    scif_puts("  RTC counter: ");
    scif_puthex(rtcval);
    scif_puts(bad ? " (stuck or implausible)\n" : " (ticking)\n");
}

/* ------------------------------------------------------------------ */
/* DIMM board (G1). Absence is a normal configuration, not a failure.
 * When present: the mailbox must not look stuck; any abnormal state
 * (e.g. a fan/boot error latched by the DIMM firmware) shows up in the
 * raw registers dumped on the SCIF. */
static void test_dimm(void)
{
    dimm_info di;
    dimm_probe(&di);

    scif_puts("\nDIMM board (G1 mailbox) raw regs: cmd=");
    scif_puthex(di.command);
    scif_puts(" off=");
    scif_puthex(di.offsetl);
    scif_puts(" pl=");
    scif_puthex(di.paraml);
    scif_puts(" ph=");
    scif_puthex(di.paramh);
    scif_puts(" st=");
    scif_puthex(di.status);
    scif_puts("\n");

    if (!di.present) {
        log_result("DIMM board: not present", CLIP_NONE, T_OK, 0, 0);
        say(CLIP_DIMM, 250);
        say(CLIP_ABSENT, REPORT_GAP_MS);
        return;
    }
    /* present: sanity — the handshake bits must not be all stuck low */
    t_status st = (di.status == 0x0000) ? T_FAIL : T_OK;
    log_result("DIMM board: present, mailbox", CLIP_NONE, st, 0, 0);
    say(CLIP_DIMM, 250);
    say(CLIP_PRESENT, 250);
    say(st == T_OK ? CLIP_OK : CLIP_FAIL, REPORT_GAP_MS);
}

/* spoken replay of everything acquired before audio came up */
static void audio_replay_log(void)
{
    scif_puts("Audio online: replaying acquired results on speaker...\n");
    say(CLIP_AUDIO_OK, REPORT_GAP_MS);
    for (u32 i = 0; i < g_log_n; i++)
        say_entry(&g_log[i]);
}

/* ------------------------------------------------------------------ */
void cmain(void)
{
    timer_init();
    scif_puts("SCIF console up, 115200 8N1\n");

    /* crt0 already proved OC-RAM works, put it in the log.
     * A failure here means the SH4 itself (IC designator TBD) is dead —
     * crt0 reports it on SCIF and halts before ever reaching this point. */
    log_result("CPU OC-RAM (cache as RAM)", CLIP_CPU_CACHE, T_OK, 0, 0);

    test_bios_rom();

    u32 usable = test_sdram();

    /* audio stage: test sound RAM, then it becomes an output channel */
    u32 aram_ok = test_aram();
    if (aram_ok) {
        g_audio_ready = 1;
        audio_replay_log();
    }

    /* video stage: test VRAM, then the screen becomes an output channel
     * (the framebuffer sits in TEX0, validated just before) */
    u32 vram_ok = test_vram();
    if (vram_ok) {
        pvr_display_init();
        g_screen_ready = 1;
        screen_render();
        scif_puts("Screen online: report displayed on VGA output.\n");
    }

    /* peripheral stage: backup SRAM (non-destructive), RTC, DIMM board */
    test_sram_rtc();
    test_dimm();

    scif_puts("\n==== SUMMARY ====\n");
    for (u32 i = 0; i < g_log_n; i++) {
        scif_puts(g_log[i].name);
        scif_puts(g_log[i].status == T_OK ? ": OK\n" : ": FAIL\n");
    }
    if (usable)
        scif_puts("Main RAM usable.\n");
    else
        scif_puts("Main RAM NOT usable -> staying in OC-RAM only mode.\n");
    if (aram_ok)
        scif_puts("Sound RAM usable, audio reports active.\n");
    if (vram_ok)
        scif_puts("VRAM usable, on-screen report active.\n");

    scif_puts("\n*** DIAG COMPLETE (v0.3: SCIF + OC-RAM + BIOS + SDRAM + AICA + VRAM) ***\n");
    say(CLIP_TESTS_DONE, REPORT_GAP_MS);
    scif_flush();
    for (;;)
        ;
}
