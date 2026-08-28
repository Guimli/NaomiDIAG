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
#include "maple.h"
#include "board.h"
#include "cart.h"
#include "sha1.h"
#include "version.inc"
#include "strings.h"
#include "audio_clips.h"
#include "cartdb.h"
#include "progress.h"

#ifndef QUICK_TEST
#define QUICK_TEST 0
#endif

#ifndef N_PASSES
#define N_PASSES    1               /* see PASSES in the Makefile */
#endif
#define REPORT_GAP_MS 1000          /* >= 1 s between spoken reports */

#define CLIP_NONE   0xFFFFFFFFu

static u32 g_audio_ready;           /* sound RAM validated, AICA usable */
static u32 g_ic_valid;              /* identified board: IC tables apply */
static board_type g_board_type;     /* set by test_board() */
static u32 g_mie_port1;             /* MIE maple port + 1; 0 = none  */

/* IC tables were extracted from the Naomi 1 BIOS; on any other board we
 * fall back to numbered positions instead of announcing wrong ICs. */
#define IC(table) (g_ic_valid ? (table) : 0)

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
    fb_text(112, 8, "NAOMI DIAG ROM v" DIAG_VERSION, COL_TITLE, FB_W);
    u32 y = 48;
    for (u32 i = 0; i < g_log_n && y < FB_REPORT_YMAX; i++) {
        const log_entry *e = &g_log[i];
        fb_text(16, y, e->name, COL_WHITE, FB_STATUS_X);
        if (e->status == T_OK) {
            fb_text(FB_W - 16 * 3, y, S_SCR_OK, COL_GREEN, FB_W);
        } else {
            fb_text(FB_W - 16 * 6, y, S_SCR_FAIL, COL_RED, FB_W);
            if (e->detail && e->comps) {
                y += 20;
                u32 x = 32;
                const char *last = 0;
                for (u32 b = 0; b < 4 && x < FB_W - 80; b++) {
                    if (!(e->detail & (1u << b)) || e->comps[b].name == last)
                        continue;
                    last = e->comps[b].name;
                    fb_text(x, y, e->comps[b].name, COL_RED, FB_W);
                    x += 16 * 5;
                }
            }
        }
        y += 20;
    }
    /* the repaint above cleared the whole screen: put the bar back whole */
    fb_progress_invalidate();
    fb_progress(progress_label(), progress_pct());
}

/* "<base> pass k/10" assembled in OC-RAM: the ROM allows no writable .data,
 * and the progress bar needs to name which of the ten passes is running. */
static char g_passbuf[64];

static const char *pass_label(const char *base, u32 pass)
{
    u32 i = 0;
    while (base[i] && i < 40) {
        g_passbuf[i] = base[i];
        i++;
    }
    const char *suf = S_P_PASS;
    for (u32 j = 0; suf[j] && i < 52; j++)
        g_passbuf[i++] = suf[j];
    if (pass >= 10)
        g_passbuf[i++] = (char)('0' + pass / 10);
    g_passbuf[i++] = (char)('0' + pass % 10);
    g_passbuf[i++] = '/';
    if (N_PASSES >= 10)
        g_passbuf[i++] = (char)('0' + N_PASSES / 10);
    g_passbuf[i++] = (char)('0' + N_PASSES % 10);
    g_passbuf[i] = 0;
    return g_passbuf;
}

static void say(u32 clip, u32 gap_ms)
{
    if (g_audio_ready && clip != CLIP_NONE)
        aica_say(audio_clips[clip].pcm, audio_clips[clip].len, gap_ms);
}

/* speak one result. OK: "<name> test passed". FAIL with located
 * components: one full report per component, using the silkscreen IC
 * name when the mapping is known ("Main memory, I C sixteen, defective")
 * and the position number otherwise. Duplicate IC clips (single-chip
 * RAMs) are only spoken once. */
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
    scif_puts(st == T_OK ? S_SUF_OK : S_SUF_FAIL);
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
        scif_puts(S_DEFECTIVE);
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
/* Board identification: read-only, first thing after the cache test. */
static void test_board(void)
{
    board_info b;
    board_detect(&b);
    g_board_type = b.type;

    scif_puts("Board probe: SH4 ver ");
    scif_puthex(b.sh4_ver);
    scif_puts(", HOLLY id ");
    scif_puthex(b.holly_id);
    scif_puts(" rev ");
    scif_puthex(b.holly_rev);
    scif_puts(", Elan id ");
    scif_puthex(b.elan_id);
    scif_puts(", dual PVR ");
    scif_putdec(b.dual_pvr);
    scif_puts("\n");

    switch (b.type) {
    case BOARD_NAOMI1:
        g_ic_valid = 1;
        log_result(S_L_BOARD_N1, CLIP_NAOMI1, T_OK, 0, 0);
        break;
    case BOARD_NAOMI2:
        /* the Naomi 2 BIOS (epr-23605c, tables at ROM 0x5C800) uses the
         * exact same RAM TEST IC designators as the Naomi 1 for all the
         * regions we test -> the IC tables apply here too */
        g_ic_valid = 1;
        log_result(S_L_BOARD_N2, CLIP_NAOMI2, T_OK, 0, 0);
        break;
    default:
        log_result(S_L_BOARD_UNK, CLIP_BOARD_UNK, T_FAIL, 0, 0);
        break;
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
    /* read through P1: cached, so one 32-byte line fill serves 8 words
     * instead of 8 separate EPROM cycles. Correct for a checksum -- the
     * content is read-only and the cache was invalidated at reset. */
    const volatile u32 *rom = (const volatile u32 *)0x80000000;
    u32 n = (0x00200000 - 4) >> 2;
    register u32 crc = 0xFFFFFFFF;
    progress_begin(S_P_BIOS, n);
    for (u32 i = 0; i < n; i++) {
        if ((i & 1023) == 0)
            progress_tick(i);
        crc ^= rom[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 4) ^ bios_crc4tab[crc & 0xF];
    }
    progress_end();
    crc = ~crc;
    u32 expect = rom[n];
    t_status st = (crc == expect) ? T_OK : T_FAIL;
    log_result(S_L_BIOS, CLIP_BIOS, st, st == T_FAIL ? 1 : 0,
               IC(bios_comps));
    if (st == T_FAIL) {
        scif_puts("  computed ");
        scif_puthex(crc);
        scif_puts(" expected ");
        scif_puthex(expect);
        scif_puts("\n");
    }
}

/* ------------------------------------------------------------------ */
static u32 g_ram_size;

/* Fast: configure the bus/SDRAM controller (BSC) and detect 16/32MB.
 * Done EARLY, before the audio/video stages, because the AICA and PVR
 * buses rely on the BSC being set up — and because it takes only a few
 * milliseconds. The slow part (the 10-pass cell test) is deferred to
 * test_sdram_cells() so the screen and audio come up first. */
static void sdram_setup(void)
{
    scif_puts(S_SDRAM_INIT);
    g_ram_size = sdram_init();
    scif_puts(S_SDRAM_SIZE);
    scif_putdec(g_ram_size >> 20);
    scif_puts(S_MB);
}

/* Slow (~1 min on 32MB): the actual CPU-RAM cell test. Runs after the
 * screen and audio are live so the machine never looks frozen. */
static u32 test_sdram_cells(void)
{
    u32 size = g_ram_size;

    /* data bus test runs at an even word address (A2=0): comps 1/2 */
    u32 bad = ram_test_databus(SDRAM_P2_BASE);
    u32 comps = (bad & 0xFFFF ? 1u : 0) | (bad >> 16 ? 2u : 0);
    log_result(S_L_SDRAM_DBUS, CLIP_DATA_BUS, bad ? T_FAIL : T_OK, comps,
                IC(work_comps));
    if (bad) {
        report_badbits(bad);
        report_comps(S_CG_CPU, comps, IC(work_comps));
    }

    bad = ram_test_addrbus(SDRAM_P2_BASE, size);
    log_result(S_L_SDRAM_ABUS, CLIP_ADDR_BUS, bad ? T_FAIL : T_OK, 0, 0);
    if (bad) {
        scif_puts("  bad address bits mask: ");
        scif_puthex(bad);
        scif_puts("\n");
    }

#if QUICK_TEST
    u32 len = 0x00100000;
    scif_puts(S_QUICK);
#else
    u32 len = size;
#endif

    ram_result res;
    ram_result_clear(&res);
    for (u32 pass = 0; pass < N_PASSES; pass++) {
        scif_puts(S_PASS);
        scif_putdec(pass + 1);
        scif_puts("/");
        scif_putdec(N_PASSES);
        scif_puts(": 5555");
        progress_begin(pass_label(S_P_SDRAM, pass + 1), (len >> 2) * 2);
        ram_test_pattern(SDRAM_P2_BASE, len, 0x55555555, &res);
        scif_puts(" AAAA");
        progress_begin(pass_label(S_P_SDRAM, pass + 1), (len >> 2) * 2);
        ram_test_pattern(SDRAM_P2_BASE, len, 0xAAAAAAAA, &res);
        scif_puts(" PRNG");
        u32 seed = 0xDEADBEEF ^ (0x9E3779B9u * (pass + 1));
        progress_begin(pass_label(S_P_SDRAM, pass + 1), (len >> 2) * 2);
        ram_test_prng(SDRAM_P2_BASE, len, seed, &res);
        progress_end();
        scif_puts(" crc=");
        scif_puthex(res.crc_r);
        scif_puts(res.errors ? " ERR\n" : " ok\n");
    }

    t_status st = res.errors ? T_FAIL : T_OK;
    log_result(S_L_SDRAM_CELL, CLIP_CPU_RAM, st,
               ram_comp_mask(&res), IC(work_comps));
    if (res.errors) {
        report_badbits(res.badbits);
        report_comps(S_CG_CPU, ram_comp_mask(&res), IC(work_comps));
        report_fails(&res);
        return 0;
    }
    return size;
}

/* ------------------------------------------------------------------ */
static u32 test_aram(void)
{
    scif_puts(S_AICA_HDR);
    aica_init();

    u32 bad = aram_test_databus();
    u32 comps = (bad & 0xFFFF ? 1u : 0) | (bad >> 16 ? 2u : 0);
    log_result(S_L_ARAM_DBUS, CLIP_DATA_BUS, bad ? T_FAIL : T_OK, comps,
                IC(aram_comps));
    if (bad) {
        report_badbits(bad);
        report_comps(S_CG_SOUND, comps, IC(aram_comps));
    }

#if QUICK_TEST
    u32 len = 0x00100000;
    scif_puts(S_QUICK);
#else
    u32 len = ARAM_SIZE;
#endif

    ram_result res;
    ram_result_clear(&res);
    for (u32 pass = 0; pass < N_PASSES; pass++) {
        scif_puts(S_PASS);
        scif_putdec(pass + 1);
        scif_puts("/");
        scif_putdec(N_PASSES);
        scif_puts(": 5555");
        progress_begin(pass_label(S_P_ARAM, pass + 1), (len >> 2) * 2);
        aram_test_pattern(0, len, 0x55555555, &res);
        scif_puts(" AAAA");
        progress_begin(pass_label(S_P_ARAM, pass + 1), (len >> 2) * 2);
        aram_test_pattern(0, len, 0xAAAAAAAA, &res);
        scif_puts(" PRNG");
        u32 seed = 0xC0FFEE42 ^ (0x9E3779B9u * (pass + 1));
        progress_begin(pass_label(S_P_ARAM, pass + 1), (len >> 2) * 2);
        aram_test_prng(0, len, seed, &res);
        progress_end();
        scif_puts(" crc=");
        scif_puthex(res.crc_r);
        scif_puts(res.errors ? " ERR\n" : " ok\n");
    }

    /* a G2 bus that never drains aborts the write loops: the cell results
     * are meaningless then, and the bus is the actual fault to report */
    if (aica_g2_stalled()) {
        scif_puts("  G2 bus never went idle: sound RAM result is void\n");
        log_result(S_L_ARAM_CELL, CLIP_SOUND_RAM, T_FAIL, 0, IC(aram_comps));
        return 0;
    }

    t_status st = res.errors ? T_FAIL : T_OK;
    log_result(S_L_ARAM_CELL, CLIP_SOUND_RAM, st,
               ram_comp_mask(&res), IC(aram_comps));
    if (res.errors) {
        report_badbits(res.badbits);
        report_comps(S_CG_SOUND, ram_comp_mask(&res), IC(aram_comps));
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
    /* the framebuffer lives inside TEX0: while that region is under test,
     * the bar must not be drawn into it -- it would overwrite the pattern
     * being verified and report a fault that does not exist. */
    u32 fbaddr = VRAM_TEX0_BASE + FB_VRAM_OFFSET;
    progress_screen_enable(!(base <= fbaddr && fbaddr < base + len));

    for (u32 pass = 0; pass < N_PASSES; pass++) {
        progress_begin(pass_label(S_P_VRAM, pass + 1), (len >> 2) * 2);
        vram_test_pattern(base, len, 0x55555555, &res);
        progress_begin(pass_label(S_P_VRAM, pass + 1), (len >> 2) * 2);
        vram_test_pattern(base, len, 0xAAAAAAAA, &res);
        u32 seed = 0x7E0CBEEF ^ (0x9E3779B9u * (pass + 1)) ^ base;
        progress_begin(pass_label(S_P_VRAM, pass + 1), (len >> 2) * 2);
        vram_test_prng(base, len, seed, &res);
        progress_end();
        scif_putc('.');
        /* this test scribbles over the framebuffer: repaint after every
         * pass so the screen stays readable (and shows progress) */
        screen_render();
    }
    progress_screen_enable(1);
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
    scif_puts(S_PVR_HDR);
    pvr_vram_enable();
    u32 tex0_ok = 1, tex1_ok = 1;
    test_vram_region(g_ic_valid ? S_L_VRAM_TEX0_IC : S_L_VRAM_TEX0, VRAM_TEX0_BASE, VRAM_TEX0_SIZE,
                     IC(tex0_comps), CLIP_VRAM, &tex0_ok);
    test_vram_region(g_ic_valid ? S_L_VRAM_TEX1_IC : S_L_VRAM_TEX1, VRAM_TEX1_BASE, VRAM_TEX1_SIZE,
                     IC(tex1_comps), CLIP_VRAM, &tex1_ok);
    return tex0_ok;                     /* the framebuffer lives in TEX0 */
}

/* Naomi 2 only: the slave PVR's 16MB VRAM (32-bit path) and the Elan
 * T&L chip's 32MB RAM. IC designators for these are not known yet ->
 * numbered positions. */
static void test_naomi2_ram(void)
{
    if (g_board_type != BOARD_NAOMI2)
        return;
    scif_puts(S_N2_HDR);
    u32 ok = 1;
    pvr2_vram_enable();
    test_vram_region(S_L_VRAM_B, VRAM_PVRB_BASE, VRAM_PVRB_SIZE,
                     0, CLIP_VRAM_B, &ok);
    elan_init();
    test_vram_region(S_L_ELAN, ELAN_RAM_BASE, ELAN_RAM_SIZE,
                     0, CLIP_ELAN, &ok);
}

/* ------------------------------------------------------------------ */
static void test_sram_rtc(void)
{
    scif_puts(S_PERIPH_HDR);

    ram_result res;
    u32 mask = sram_test(&res, N_PASSES);
    /* positions 1/2 = even/odd byte lane; IC designators pending the
     * user's silkscreen readout -> spoken as numbered positions */
    log_result(S_L_BACKSRAM, CLIP_SRAM,
               mask ? T_FAIL : T_OK, mask, 0);
    if (mask) {
        report_badbits(res.badbits);
        report_comps(S_CG_SRAM, mask, 0);
        report_fails(&res);
    }

    u32 rtcval = 0;
    u32 bad = rtc_test(&rtcval);
    log_result(S_L_RTC, CLIP_RTC, bad ? T_FAIL : T_OK, 0, 0);
    scif_puts(S_RTC_COUNTER);
    scif_puthex(rtcval);
    scif_puts(bad ? S_RTC_STUCK : S_RTC_TICK);
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
        log_result(S_L_DIMM_ABSENT, CLIP_NONE, T_OK, 0, 0);
        say(CLIP_DIMM, 250);
        say(CLIP_ABSENT, REPORT_GAP_MS);
        return;
    }
    /* present: sanity — the handshake bits must not be all stuck low */
    t_status st = (di.status == 0x0000) ? T_FAIL : T_OK;
    log_result(S_L_DIMM_PRESENT, CLIP_NONE, st, 0, 0);
    say(CLIP_DIMM, 250);
    say(CLIP_PRESENT, 250);
    say(st == T_OK ? CLIP_OK : CLIP_FAIL, REPORT_GAP_MS);
}

/* ------------------------------------------------------------------ */
/* Maple bus + MIE (315-6146): a Device Request must be answered by the
 * MIE's Z80. Needs validated main RAM for the DMA descriptors. */
static void test_maple_mie(u32 ram_ok)
{
    if (!ram_ok) {
        scif_puts(S_MAPLE_SKIP);
        return;
    }
    maple_result mr;
    maple_scan(&mr);

    if (mr.found_port == 0xFFFFFFFF) {
        log_result(S_L_MIE_NORESP, CLIP_JVS, T_FAIL, 0, 0);
        return;
    }
    scif_puts("  MIE on maple port ");
    scif_putdec(mr.found_port);
    scif_puts(", resp cmd 0x");
    scif_puthex(mr.response_cmd);
    scif_puts("\n  MIE version: \"");
    scif_puts(mr.id);
    scif_puts("\"\n");
    /* 0x83 = version response from the 315-6146 firmware */
    if (mr.response_cmd == 0x83)
        g_mie_port1 = mr.found_port + 1;
    log_result(S_L_MIE, CLIP_JVS,
               mr.response_cmd == 0x83 ? T_OK : T_FAIL, 0, 0);

    /* factory self-test: the Z80 checks its own ROM/RAM (status 0 = ok).
     * Black-box coverage — to be reworked with an uploaded Z80 test
     * program (see README). */
    if (mr.response_cmd == 0x83) {
        u32 st_word;
        u32 bad = maple_mie_selftest(mr.found_port, &st_word);
        scif_puts("  MIE self-test status word: ");
        scif_puthex(st_word);
        scif_puts("\n");
        log_result(S_L_MIE_SELFTEST, CLIP_JVS,
                   bad ? T_FAIL : T_OK, 0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Settings EEPROM (93C46 behind the MIE): read all 128 bytes through
 * the MIE protocol, then verify SEGA CRCs and the duplicated copies of
 * the system area. Read-only. */
static void test_settings_eeprom(void)
{
    if (!g_mie_port1) {
        scif_puts(S_EEPROM_SKIP);
        return;
    }
    u8 ee[128];
    if (maple_eeprom_read(g_mie_port1 - 1, ee)) {
        /* Stock 315-6146 firmware has no 0x86 handler: reading this
         * EEPROM needs a code upload into the MIE (as the BIOS does).
         * Not a fault -> reported as a documented skip. */
        scif_puts(S_EEPROM_MIE_NOTE);
        return;
    }
    u16 stored1 = (u16)(ee[0] | (ee[1] << 8));
    u16 calc1   = sega_eeprom_crc(&ee[2], 16);
    u16 stored2 = (u16)(ee[18] | (ee[19] << 8));
    u16 calc2   = sega_eeprom_crc(&ee[20], 16);
    u32 mirror_ok = 1;
    for (u32 i = 0; i < 18; i++)
        if (ee[i] != ee[18 + i])
            mirror_ok = 0;

    scif_puts("\nSettings EEPROM: crc1 ");
    scif_puthex(stored1);
    scif_puts(stored1 == calc1 ? " ok" : " BAD");
    scif_puts(", crc2 ");
    scif_puthex(stored2);
    scif_puts(stored2 == calc2 ? " ok" : " BAD");
    scif_puts(mirror_ok ? ", copies match\n" : ", copies DIFFER\n");

    t_status st = (stored1 == calc1 && stored2 == calc2 && mirror_ok)
                      ? T_OK : T_FAIL;
    log_result(S_L_EEPROM_MIE, CLIP_EEPROM, st, 0, 0);
}

/* Serial-number 93C46 (SH4 GPIO): direct read + content plausibility
 * (the factory content is ASCII; all-0/all-1 = dead chip or open line). */
static void test_serial_eeprom(void)
{
    u8 ee[128];
    serial_eeprom_read(ee);

    u32 printable = 0, zeros = 0, ones = 0;
    for (u32 i = 0; i < 128; i++) {
        if (ee[i] >= 32 && ee[i] < 127)
            printable++;
        if (ee[i] == 0x00)
            zeros++;
        if (ee[i] == 0xFF)
            ones++;
    }
    scif_puts("\nSerial EEPROM (93C46 on SH4 GPIO): \"");
    for (u32 i = 0; i < 24; i++)
        scif_putc((ee[i] >= 32 && ee[i] < 127) ? (char)ee[i] : '.');
    scif_puts("\" printable ");
    scif_putdec(printable);
    scif_puts("/128\n");

    t_status st = (zeros == 128 || ones == 128 || printable < 16)
                      ? T_FAIL : T_OK;
    log_result(S_L_EEPROM_GPIO, CLIP_EEPROM, st, 0, 0);
}

/* X76F100 cart security chip: presence via response-to-reset. Absence is
 * normal without a cartridge (e.g. DIMM setups). */
static void test_x76(void)
{
    u32 rtr = x76f100_rtr();
    scif_puts("X76F100 response-to-reset: ");
    scif_puthex(rtr);
    scif_puts("\n");
    if (rtr == 0x00000000 || rtr == 0xFFFFFFFF) {
        log_result(S_L_X76_ABSENT, CLIP_NONE,
                   T_OK, 0, 0);
        say(CLIP_X76, 250);
        say(CLIP_ABSENT, REPORT_GAP_MS);
        return;
    }
    log_result(S_L_X76_PRESENT, CLIP_NONE, T_OK, 0, 0);
    say(CLIP_X76, 250);
    say(CLIP_PRESENT, 250);
    say(CLIP_OK, REPORT_GAP_MS);
}

/* ------------------------------------------------------------------ */
/* Cartridge content test: identify the game by streaming SHA-1
 * checkpoints over the first IC, then verify every IC of the matched
 * game against the embedded database (per-IC SHA1s from MAME). */

static u32 sha1_ic(u32 offset, u32 size, u8 out[20])
{
    sha1_ctx c;
    u8 buf[512];
    sha1_init(&c);
    cart_seek(offset);
    for (u32 done = 0; done < size; done += sizeof buf) {
        for (u32 i = 0; i < sizeof buf; i += 2) {
            u16 w = CART_ROM_DATA;
            buf[i] = (u8)w;
            buf[i + 1] = (u8)(w >> 8);
        }
        sha1_update(&c, buf, sizeof buf);
        if ((done & 0x3FFFFF) == 0)
            scif_putc('.');
    }
    sha1_final(&c, out);
    return 0;
}

static u32 sha1_eq(const u8 *a, const u8 *b)
{
    for (u32 i = 0; i < 20; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* Per-data-line health of the cartridge bus. Two things are looked at:
 *  - the share of 1s each line reads over a large sample: a line that
 *    never toggles (0% or 100%) is stuck -- broken trace, dead
 *    transceiver output, bent connector pin;
 *  - mismatches between two reads of the very same addresses: any
 *    difference means the line is unstable, the classic signature of a
 *    tired bus transceiver or an oxidised edge connector. A checksum
 *    alone cannot tell that apart from genuinely wrong ROM data. */
/* unsigned divide, shift-and-subtract: this ROM links no libgcc, so the
 * compiler's __udivsi3 helper is not available */
static u32 udiv(u32 n, u32 d)
{
    if (!d)
        return 0;
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= 1u << i;
        }
    }
    return q;
}

static void test_cart_pins(void)
{
    cart_pin_stats st;
#if QUICK_TEST
    const u32 span = 0x00010000;        /* 64 KB sample */
#else
    const u32 span = 0x00100000;        /* 1 MB sample */
#endif
    cart_pin_scan(0, span, &st);

    scif_puts(S_PINS_HDR);
    scif_puts(S_PINS_SAMPLED);
    scif_putdec(st.words);
    scif_puts("\n");

    u32 faults = 0;
    for (u32 b = 0; b < 16; b++) {
        u32 pct = udiv(st.ones[b] * 100u, st.words);
        scif_puts(S_PIN);
        scif_putdec(b);
        scif_puts(S_PIN_ONES);
        scif_putdec(pct);
        scif_puts(S_PIN_PCT);
        if (st.words && (st.ones[b] == 0 || st.ones[b] == st.words)) {
            scif_puts(S_PIN_STUCK);     /* never toggles */
            faults++;
        }
        if (st.flaky[b]) {
            scif_puts(S_PIN_FLAKY);     /* differs between two reads */
            scif_putdec(st.flaky[b]);
            faults++;
        }
        scif_puts("\n");
    }
    if (!faults)
        scif_puts(S_PINS_OK);

    log_result(S_L_CART_PINS, CLIP_DATA_BUS, faults ? T_FAIL : T_OK, 0, 0);
}

static void test_cartridge(void)
{
    g1_bus_init();
    if (!cart_present()) {
        log_result(S_L_CART_ABSENT, CLIP_NONE, T_OK, 0, 0);
        say(CLIP_CART, 250);
        say(CLIP_ABSENT, REPORT_GAP_MS);
        return;
    }

    /* header peek: "NAOMI" magic + ASCII titles live in the first bytes */
    u8 hdr[80];
    cart_read(0, hdr, sizeof hdr);
    scif_puts(S_CART_HDR);
    for (u32 i = 0; i < 64; i++)
        scif_putc((hdr[i] >= 32 && hdr[i] < 127) ? (char)hdr[i] : '.');
    scif_puts("\"\n");

    /* identify: stream first IC, snapshot SHA1 at each known size */
    sha1_ctx c;
    u8 buf[512], digest[20];
    u32 game = 0xFFFFFFFF;
    sha1_init(&c);
    cart_seek(0);
    u32 done = 0;
    scif_puts(S_IDENTIFYING);
    for (u32 s = 0; s < CARTDB_NFIRST && game == 0xFFFFFFFF; s++) {
        u32 target = cartdb_first_sizes[s];
        while (done < target) {
            for (u32 i = 0; i < sizeof buf; i += 2) {
                u16 w = CART_ROM_DATA;
                buf[i] = (u8)w;
                buf[i + 1] = (u8)(w >> 8);
            }
            sha1_update(&c, buf, sizeof buf);
            done += sizeof buf;
            if ((done & 0x3FFFFF) == 0)
                scif_putc('.');
        }
        sha1_ctx snap;
        {
            const u8 *s = (const u8 *)&c;
            u8 *d = (u8 *)&snap;
            for (u32 i = 0; i < sizeof c; i++)
                d[i] = s[i];
        }
        sha1_final(&snap, digest);
        for (u32 g = 0; g < CARTDB_NGAMES; g++) {
            u32 f = cartdb_game_first[g];
            if (cartdb_ic_off[f] == 0 && cartdb_ic_size[f] == target &&
                sha1_eq(digest, cartdb_sha1[f])) {
                game = g;
                break;
            }
        }
    }
    scif_puts("\n");

    if (game == 0xFFFFFFFF) {
        scif_puts(S_CART_NOTINDB);
        log_result(S_L_CART_UNKNOWN, CLIP_CART, T_FAIL, 0, 0);
        /* A dead or unstable data line corrupts every read, so the game
         * cannot be identified -- run the per-line test anyway: it tells
         * a bus/connector fault apart from a genuinely unknown cart. */
        test_cart_pins();
        return;
    }

    scif_puts(S_IDENTIFIED);
    scif_puts(cartdb_title[game]);
    scif_puts("\n");

    /* verify every IC of the identified game: first that the chip answers
     * at all (a missing or unseated EPROM is a different fault from bad
     * content), then its SHA-1 */
    u32 nics = cartdb_game_nics[game];
    u32 base = cartdb_game_first[game];
    u32 bad = 0, missing = 0;
    /* Presence is probed on EVERY chip the game needs (16 samples each,
     * cheap). Only the slow SHA-1 hashing is trimmed on QUICK builds. */
#if QUICK_TEST
    const u32 nhash = (nics > 2) ? 2 : nics;
#else
    const u32 nhash = nics;
#endif

    /* keep every digest so identical chips can be spotted: an empty
     * socket often mirrors a neighbouring chip through address decoding
     * instead of reading back 0xFF, which the presence probe cannot see */
#define CART_MAX_TRACK 32
    u8  seen_digest[CART_MAX_TRACK][20];
    u32 seen_idx[CART_MAX_TRACK];
    u32 nseen = 0;

    for (u32 i = 0; i < nics; i++) {
        u32 idx = base + i;
        scif_puts("  ");
        scif_puts(cartdb_ic_name[idx]);   /* name already includes "ic" */
        if (!cart_ic_responds(cartdb_ic_off[idx], cartdb_ic_size[idx])) {
            scif_puts(S_NO_RESPONSE);
            missing++;
            continue;                     /* no point hashing dead silence */
        }
        if (i >= nhash) {                 /* present, hashing skipped */
            scif_puts(S_PRESENT_ONLY);
            continue;
        }
        scif_putc(' ');
        sha1_ic(cartdb_ic_off[idx], cartdb_ic_size[idx], digest);
        u32 ok = sha1_eq(digest, cartdb_sha1[idx]);
        scif_puts(ok ? S_GOOD : S_BAD);
        if (!ok)
            bad++;

        /* alias check: same bytes as an earlier chip = one of them is
         * not really there (only meaningful when the content is wrong) */
        u32 aliased = 0xFFFFFFFF;
        for (u32 s = 0; s < nseen; s++) {
            if (sha1_eq(digest, seen_digest[s])) {
                aliased = seen_idx[s];
                break;
            }
        }
        if (aliased != 0xFFFFFFFF && !ok) {
            scif_puts(S_ALIAS_A);
            scif_puts(cartdb_ic_name[aliased]);
            scif_puts(S_ALIAS_B);
            missing++;                    /* chip is effectively absent */
        } else if (nseen < CART_MAX_TRACK) {
            for (u32 b = 0; b < 20; b++)
                seen_digest[nseen][b] = digest[b];
            seen_idx[nseen] = idx;
            nseen++;
        }
    }

    /* affirmative count: how many of the chips this game needs answered */
    scif_puts(S_ROMSET_A);
    scif_putdec(nics - missing);
    scif_puts(S_ROMSET_B);
    scif_putdec(nics);
    scif_puts(S_ROMSET_C);
    if (missing) {
        scif_puts(S_CART_NMISS_A);
        scif_putdec(missing);
        scif_puts(S_CART_NMISS_B);
    }
    log_result(S_L_CART_PRESENCE, CLIP_CART, missing ? T_FAIL : T_OK, 0, 0);

    if (bad) {
        scif_puts(S_CART_NDEF_A);
        scif_putdec(bad);
        scif_puts(S_CART_NDEF_B);
    }
    log_result(S_L_CART_CONTENT, CLIP_CART,
               (bad || missing) ? T_FAIL : T_OK, 0, 0);

    test_cart_pins();
}

/* ------------------------------------------------------------------ */
/* Fast channel bring-up.
 *
 * The exhaustive memory tests take minutes on real hardware (the ROM runs
 * uncached straight from the EPROM), so waiting for them before lighting
 * up a channel leaves the operator staring at a dead machine. Instead we
 * prove ONLY the small region each channel actually uses -- the clip
 * landing zone for audio, the framebuffer for video -- which takes
 * seconds, and bring the channel up right away. The full memory is still
 * tested exhaustively straight afterwards, with its results reported on
 * the channels now available.
 *
 * Rule 1 is preserved: nothing is used before being proven; we simply
 * prove the part we need first. */

static void audio_replay_log(void);   /* defined below */

#define AUDIO_ZONE_OFF  0x00010000u     /* clip landing zone in sound RAM */
#define AUDIO_ZONE_LEN  0x00020000u     /* 128 KB: longest clip fits */
#define FB_ZONE_LEN     ((u32)FB_W * FB_H * 2)   /* 640x480 RGB565 */

static void quick_audio_bringup(void)
{
    aica_init();                        /* ARM7 held in reset */

    ram_result res;
    ram_result_clear(&res);
    u32 bad = aram_test_databus();
    if (!bad) {
        aram_test_pattern(AUDIO_ZONE_OFF, AUDIO_ZONE_LEN, 0x55555555, &res);
        aram_test_pattern(AUDIO_ZONE_OFF, AUDIO_ZONE_LEN, 0xAAAAAAAA, &res);
        aram_test_prng(AUDIO_ZONE_OFF, AUDIO_ZONE_LEN, 0xA1CA5EED, &res);
    }
    t_status st = (bad || res.errors) ? T_FAIL : T_OK;
    log_result(S_L_AUDIO_QUICK, CLIP_NONE, st, 0, 0);
    if (st == T_OK) {
        g_audio_ready = 1;
        progress_phase(PH_AUDIO_ON);    /* yellow */              /* channel 3 live, in seconds */
        audio_replay_log();
    }
}

static void quick_video_bringup(void)
{
    pvr_vram_enable();

    ram_result res;
    ram_result_clear(&res);
    u32 bad = vram_test_databus(VRAM_TEX0_BASE);
    u32 fb = VRAM_TEX0_BASE + FB_VRAM_OFFSET;
    if (!bad) {
        vram_test_pattern(fb, FB_ZONE_LEN, 0x55555555, &res);
        vram_test_pattern(fb, FB_ZONE_LEN, 0xAAAAAAAA, &res);
        vram_test_prng(fb, FB_ZONE_LEN, 0x7EC0FFEE, &res);
    }
    t_status st = (bad || res.errors) ? T_FAIL : T_OK;
    log_result(S_L_VIDEO_QUICK, CLIP_NONE, st, 0, 0);
    if (st == T_OK) {
        pvr_display_init();
        g_screen_ready = 1;             /* channel 2 live, in seconds */
        progress_phase(PH_VIDEO_ON);    /* green */
        screen_render();
        scif_puts(S_SCREEN_ONLINE);
    } else {
        progress_phase(PH_FAILED);      /* red: no usable framebuffer */
    }
}

/* spoken replay of everything acquired before audio came up */
static void audio_replay_log(void)
{
    scif_puts(S_AUDIO_ONLINE);
    say(CLIP_AUDIO_OK, REPORT_GAP_MS);
    for (u32 i = 0; i < g_log_n; i++)
        say_entry(&g_log[i]);
}

/* ------------------------------------------------------------------ */
void cmain(void)
{
    timer_init();

    /* Video timings FIRST, before anything can go wrong. This touches no
     * memory at all -- the PowerVR paints the border colour straight from a
     * register -- so the screen turns from black to blue within a fraction
     * of a second of power-up, and every phase from here on repaints it in
     * its own colour. A board that stops mid-diagnostic therefore leaves
     * the colour of the phase it died in on the screen, which is the only
     * output a Naomi with no serial cable can give us. */
    pvr_video_on();
    progress_phase(PH_ROM_ALIVE);       /* blue */

    /* Enable the SH-4 DMA controller. The original BIOS does this early and
     * JinGasa documents why: without it the Maple bus does not work. We only
     * declared the register until now, never wrote it. */
    DMAOR = 0x00008201;
    scif_puts(S_SCIF_UP);

    /* crt0 already proved OC-RAM works, put it in the log.
     * A failure here means the SH4 itself (IC designator TBD) is dead —
     * crt0 reports it on SCIF and halts before ever reaching this point. */
    log_result(S_L_OCRAM, CLIP_CPU_CACHE, T_OK, 0, 0);

    /* Bus controller FIRST, exactly like the original BIOS at 0xA0000440.
     * Until this runs, area 0 (the boot EPROM) uses the reset-default wait
     * states -- the slowest the chip offers -- and everything that reads
     * ROM crawls. Measured on real hardware: a plain two-instruction loop
     * ran orders of magnitude slower before this was programmed. The 2 MB
     * BIOS CRC below used to run at that crippled speed. */
    sdram_setup();
    progress_phase(PH_BUS_READY);       /* cyan */

    /* light up the audio and video channels within seconds, by proving
     * only the region each one needs (see quick_*_bringup above) */
    quick_video_bringup();   /* screen first: richest channel, no replay */
    quick_audio_bringup();   /* then audio, which replays the history */

    /* Only now the 2 MB ROM checksum: it is the single longest test in the
     * whole suite (4.2 M table steps) and it must never run while the
     * operator is still staring at a black screen. */
    test_bios_rom();

    /* Board identification runs only now: it probes addresses whose
     * behaviour on real hardware is less certain than plain memory, so if
     * it ever misbehaves the operator already has screen and audio up to
     * see how far the diagnostic got. */
    test_board();
    progress_phase(PH_BOARD_ID);        /* magenta */

    progress_phase(PH_TESTING);         /* white: the long suite starts */

    /* now the exhaustive memory tests, reported live on those channels */
    u32 aram_ok = test_aram();
    u32 vram_ok = test_vram();

    /* now the slow CPU-RAM cell test, with screen + audio already live:
     * each result is shown/spoken as it lands, SCIF prints pass progress */
    u32 usable = test_sdram_cells();

    /* Naomi 2 extra memories (no-op on other boards) */
    test_naomi2_ram();

    /* peripheral stage: backup SRAM (non-destructive), RTC, DIMM, MIE */
    test_sram_rtc();
    test_dimm();
    test_maple_mie(usable);
    test_settings_eeprom();
    test_serial_eeprom();
    test_x76();
    test_cartridge();

    progress_end();
    progress_phase(PH_DONE);            /* grey: suite finished */

    scif_puts(S_SUMMARY);
    for (u32 i = 0; i < g_log_n; i++) {
        scif_puts(g_log[i].name);
        scif_puts(g_log[i].status == T_OK ? S_SUM_OK : S_SUM_FAIL);
    }
    if (usable)
        scif_puts(S_MAINRAM_OK);
    else
        scif_puts(S_MAINRAM_KO);
    if (aram_ok)
        scif_puts(S_ARAM_OK_MSG);
    if (vram_ok)
        scif_puts(S_VRAM_OK_MSG);

    scif_puts(S_COMPLETE);
    say(CLIP_TESTS_DONE, REPORT_GAP_MS);
    scif_flush();
    for (;;)
        ;
}
