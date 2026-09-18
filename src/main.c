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

/* hand-written CRC-32 of the boot EPROM, src/crc32_fast.S */
u32 crc32_rom_block(const u32 *src, u32 nquads, u32 crc);
#include "timer.h"
#include "aica.h"
#include "pvr.h"
#include "periph.h"
#include "dimm.h"
#include "dimm_flash.h"
#include "maple.h"
#include "board.h"
#include "cart.h"
#include "sha1.h"
#include "config.h"
#include "version.inc"
#include "strings.h"
#include "audio_clips.h"
#include "cartdb.h"
#include "progress.h"
#include "reloc.h"
#include "input.h"

#ifndef QUICK_TEST
#define QUICK_TEST 0
#endif

/* A memory test is three phases, always: 0101 over the region, then 1010,
 * then a pseudo-random stream checked against a CRC held in a register.
 * They are numbered 1/3, 2/3, 3/3 in the report and each drives the progress
 * bar from 0 to 100%, so the operator can see which of the three is running
 * and how far it has got. */
#define N_PHASES    3
#define REPORT_GAP_MS 1000          /* >= 1 s between spoken reports */

#define CLIP_NONE   0xFFFFFFFFu

static u32 g_audio_ready;           /* sound RAM validated, AICA usable */
static u32 g_ic_valid;              /* identified board: IC tables apply */
static board_type g_board_type;     /* set by test_board() */
static u32 g_mie_port1;             /* MIE maple port + 1; 0 = none  */

/* IC tables were extracted from the Naomi 1 BIOS; on any other board we
 * fall back to numbered positions instead of announcing wrong ICs. */
#define IC(table) (g_ic_valid ? (table) : 0)


/* ========================================================================
 * Operator console: keys on the serial line, buttons on the board or the
 * cabinet. Both drive the same set of actions.
 * ===================================================================== */

#define ACT_NONE    0
#define ACT_CPU     1
#define ACT_VRAM    2
#define ACT_ARAM    3
#define ACT_DIMM    4
#define ACT_GAME    5
#define ACT_FLASH   6
#define ACT_COUNT   6

static const char *const menu_label[ACT_COUNT] = {
    S_M_CPU, S_M_VRAM, S_M_ARAM, S_M_DIMM, S_M_GAME, S_M_FLASH
};
static const char menu_key[ACT_COUNT] = { 'c', 'v', 's', 'd', 'g', 'f' };


/* Called from progress_tick, once per test block. Decides whether what the
 * operator did should stop the test, and what should happen next. */
void diag_input_check(u32 in_loop)
{
    input_event ev;
    if (!input_poll(&ev))
        return;

    if (ev.kind == INPUT_KEY) {
        if (ev.key == 'h' || ev.key == 'H') {
            scif_puts(S_HELP);          /* help never interrupts */
            return;
        }
        if (in_loop)                    /* only TEST ends a soak run */
            return;
        if (ev.key == 'a' || ev.key == 'A') {
            progress_request_abort(ABORT_PLAIN);
            return;
        }
        for (u32 i = 0; i < ACT_COUNT; i++) {
            if (ev.key == menu_key[i]) {
                progress_request_abort(i + 1);
                return;
            }
        }
        return;                         /* unknown key: ignored */
    }
    if (ev.kind == INPUT_SELECT)        /* TEST / PSW1 always stops */
        progress_request_abort(ABORT_PLAIN);
}

/* ------------------------------------------------------------------ */
/* Replayable result log (lives in OC-RAM .bss, survives until reset) */
/* ------------------------------------------------------------------ */
/* A Naomi 1 boot suite fills 24 of these, a Naomi 2 thirty, and a menu
 * action (cartridge, DIMM) adds several more on top -- entries past the end
 * are dropped silently, so the ceiling needs real headroom, not two spare
 * slots. The array lives in the 4 KB OC-RAM .bss; 48 entries cost 1.3 KB of
 * it and the linker refuses anything that does not fit. */
#define LOG_MAX 48
typedef enum { T_OK = 0, T_FAIL = 1 } t_status;

/* numbered position -> silkscreen IC (see analysis/ADDRESS_MAP.md).
 * Mapping extracted from the original BIOS RAM TEST tables; the WORK
 * lane ORDER is a hypothesis until confirmed by a forced fault on real
 * hardware. Sound RAM is a single chip: every lane maps to IC35. */
typedef struct { u32 clip; const char *name; } comp_map;

/* Lane order. IC10 on position 2 is PROVEN: a board this ROM reported as
 * IC10 was repaired by replacing IC10 alone and the fault went away. That
 * rules out the reversed order and the even/odd swap, the two plausible
 * alternatives, and leaves ascending numbering. Positions 1, 3 and 4 follow
 * from that ordering rather than from their own measurement -- the lane
 * beacon at the end of the run is there to settle them individually. */
static const comp_map work_comps[4] = {
    { CLIP_IC_9,  "IC9"   },        /* D0-D15,  even word              */
    { CLIP_IC_10, "IC10"  },        /* D16-D31, even word -- CONFIRMED */
    { CLIP_IC_11, "IC11S" },        /* D0-D15,  odd word               */
    { CLIP_IC_12, "IC12S" },        /* D16-D31, odd word               */
};
/* Sound RAM is IC35 and the backup NVRAM is IC29 -- confirmed on a real
 * board. Both were wrong here until now, and the mistake is worth recording:
 * the IC NUMBERS are genuine, read from the original BIOS RAM TEST screens
 * which print "IC%02d GOOD/BAD", but which number belonged to which region
 * was inferred from the order they appear in, and that inference was wrong.
 * Naming the wrong part on a diagnostic sends someone to desolder a good
 * chip, so anything still resting on that inference is now marked as such. */
static const comp_map aram_comps[4] = {
    { CLIP_IC_35, "IC35" }, { CLIP_IC_35, "IC35" },
    { CLIP_IC_35, "IC35" }, { CLIP_IC_35, "IC35" },
};
static const comp_map bios_comps[4] = {
    { CLIP_IC_27, "IC27" }, { CLIP_IC_27, "IC27" },
    { CLIP_IC_27, "IC27" }, { CLIP_IC_27, "IC27" },
};
/* TEX0 and TEX1, read out of the original BIOS rather than guessed.
 *
 * The BIOS RAM TEST carries a table at ROM offset 0x5C484: for each region,
 * a count followed by the IC numbers it prints. In the order of its region
 * names at 0x59548 -- BACK, AICA, WORK, TEX0, TEX1 -- it reads
 *
 *   BACK 1 -> IC29        AICA 1 -> IC35
 *   WORK 4 -> IC9,10,11,12
 *   TEX0 4 -> IC16,18,20,22        TEX1 4 -> IC17,19,21,23
 *
 * The first three are independently confirmed on a real board: IC29 is the
 * NVRAM, IC35 the sound RAM, IC9-12 the CPU RAM, and IC10 was proven to be
 * position 2 by a repair. Three matches out of three is what makes the last
 * two trustworthy -- reading this table by itself is exactly what produced
 * the wrong map before, when the region each entry belonged to was inferred
 * from display order instead of from the table's own structure.
 *
 * TEX0 is the four chips on top of the board, TEX1 the four underneath. */
static const comp_map tex0_comps[4] = {
    { CLIP_IC_16, "IC16" },         /* D0-D15,  even word */
    { CLIP_IC_18, "IC18" },         /* D16-D31, even word */
    { CLIP_IC_20, "IC20" },         /* D0-D15,  odd word  */
    { CLIP_IC_22, "IC22" },         /* D16-D31, odd word  */
};
/* No spoken clips exist for these four: the voice falls back to the position
 * number rather than announcing a chip it cannot name. */
static const comp_map tex1_comps[4] = {
    { CLIP_NONE, "IC17S" },
    { CLIP_NONE, "IC19S" },
    { CLIP_NONE, "IC21S" },
    { CLIP_NONE, "IC23S" },
};


typedef struct {
    const char *name;               /* points into ROM */
    u32 clip;                       /* CLIP_xxx id or CLIP_NONE */
    t_status status;
    u32 detail;                     /* component bitmask (bit n = comp n+1) */
    const comp_map *comps;          /* NULL, or 4-entry position->IC table */
    u32 quiet_ok;                   /* screen+speech: only when it FAILS   */
    u32 pre;                        /* clip spoken first, or CLIP_NONE     */
} log_entry;

static log_entry g_log[LOG_MAX];
static u32 g_log_n;
static u32 g_screen_ready;          /* TEX0 VRAM validated, display up */

/* full-screen render of the whole log (screen = 3rd report channel) */
/* Where the next report line goes. The report is append-only, so an added
 * result does not need the screen rebuilt: it needs one line drawn. */
static u32 g_screen_y;

/* Draw one entry at y and return the y the next one starts at, or 0 if it
 * did not fit. Shared by the append path and the full repaint so the two
 * cannot lay the screen out differently. */
static u32 screen_draw_entry(const log_entry *e, u32 y)
{
    /* The screen holds fewer lines than the suite produces results, so the
     * bus tests give up their line while they pass. They still run and still
     * print on serial; a failure takes the line back, because that is when
     * it is worth the space. say_entry() follows the same rule, so screen
     * and speech never disagree. */
    if (e->quiet_ok && e->status == T_OK)
        return y;
    if (y >= fb_report_ymax())
        return y;

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
    return y + 20;
}

/* Add the newest result to what is already on screen.
 *
 * This used to call screen_render, which clears all 640x480 and redraws
 * every line -- 153,600 uncached VRAM writes to add one line, on every
 * result. Nothing about an appended line requires that: its position depends
 * only on the lines before it, and those are already correct on screen. */
static void screen_append(void)
{
    if (!g_screen_ready || !g_log_n)
        return;
    g_screen_y = screen_draw_entry(&g_log[g_log_n - 1], g_screen_y);
}

/* Rebuild the whole screen. Needed exactly twice: when the display first
 * comes up, and after the VRAM test has scribbled over the framebuffer. */
static void screen_render(void)
{
    if (!g_screen_ready)
        return;
    fb_clear(0);
    fb_text(112, 8, "NAOMI DIAG ROM v" DIAG_VERSION, COL_TITLE, FB_W);
    u32 y = 48;
    for (u32 i = 0; i < g_log_n; i++)
        y = screen_draw_entry(&g_log[i], y);
    g_screen_y = y;
    /* the clear above took the bar with it: put it back whole */
    fb_progress_full(progress_label(), progress_pct());
}

/* "<base> pass k/10" assembled in OC-RAM: the ROM allows no writable .data,
 * and the progress bar needs to name which of the ten passes is running. */
static char g_passbuf[64];

static const char *pass_label(const char *base, u32 phase)
{
    u32 i = 0;
    while (base[i] && i < 40) {
        g_passbuf[i] = base[i];
        i++;
    }
    const char *suf = S_P_PASS;
    for (u32 j = 0; suf[j] && i < 52; j++)
        g_passbuf[i++] = suf[j];
    g_passbuf[i++] = (char)('0' + phase);
    g_passbuf[i++] = '/';
    g_passbuf[i++] = (char)('0' + N_PHASES);
    g_passbuf[i] = 0;
    return g_passbuf;
}

/* The parity of the failing word is always known -- the verify loops keep one
 * difference mask per address parity -- so a fault names exactly one chip,
 * whether the re-scan could reproduce it or not. */
static void report_intermittent(const ram_result *r)
{
    if (r->unpinned)
        scif_puts(S_INTERMITTENT);
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
    /* Same rule as the screen: an entry that gives up its line while it
     * passes gives up its clip too. The operator hears what is written in
     * front of them, and a bus test announcing itself with nothing to show
     * for it is the report contradicting the screen. Serial still carries
     * every line -- it is the full log, and it has no line budget. */
    if (e->quiet_ok && e->status == T_OK)
        return;
    /* The bus tests share their clip between memories -- "data bus" belongs
     * to the CPU RAM and to the sound RAM alike, and "address bus" names no
     * memory at all. The screen line says which one ("SDRAM address bus");
     * the speech did not, so a bus fault announced itself as "address bus,
     * test failed" and the operator had to guess which memory. Name it. */
    if (e->pre != CLIP_NONE)
        say(e->pre, 250);
    if (e->status == T_OK) {
        say(e->clip, 250);
        say(CLIP_OK, 250);
        progress_wait_ms(REPORT_GAP_MS);
        return;
    }
    if (e->detail == 0) {                   /* fault w/o component info */
        say(e->clip, 250);
        say(CLIP_FAIL, 250);
        progress_wait_ms(REPORT_GAP_MS);
        return;
    }
    u32 spoken = CLIP_NONE;
    for (u32 i = 0; i < 8; i++) {
        if (!(e->detail & (1u << i)))
            continue;
        u32 c = (e->comps && i < 4) ? e->comps[i].clip : CLIP_NUM_1 + i;
        if (c == CLIP_NONE)
            c = CLIP_NUM_1 + i;   /* no clip for this chip: say the position */
        if (c == spoken)
            continue;
        spoken = c;
        say(e->clip, 250);
        say(c, 250);
        say(CLIP_DEFECT, 250);
        progress_wait_ms(REPORT_GAP_MS);
    }
}

/* quiet_ok: the screen and the speech report this result only if it FAILS.
 * Serial reports it either way -- it is the screen that is short of lines,
 * and the spoken report follows the screen so the two agree. */
static void log_result_q(const char *name, u32 clip, t_status st, u32 detail,
                         const comp_map *comps, u32 quiet_ok, u32 pre)
{
    if (g_log_n < LOG_MAX) {
        g_log[g_log_n].name = name;
        g_log[g_log_n].clip = clip;
        g_log[g_log_n].status = st;
        g_log[g_log_n].detail = detail;
        g_log[g_log_n].comps = comps;
        g_log[g_log_n].quiet_ok = quiet_ok;
        g_log[g_log_n].pre = pre;
        g_log_n++;
    }
    scif_puts(name);
    scif_puts(st == T_OK ? S_SUF_OK : S_SUF_FAIL);
    /* live multi-channel report: one line added, then speech */
    screen_append();
    if (g_log_n)
        say_entry(&g_log[g_log_n - 1]);
}

static void log_result(const char *name, u32 clip, t_status st, u32 detail,
                       const comp_map *comps)
{
    log_result_q(name, clip, st, detail, comps, 0, CLIP_NONE);
}

/* Sound RAM is ONE 16-bit chip behind the AICA, not four chips on a 64-bit
 * bus like the CPU RAM. A 32-bit access from the SH-4 crosses G2 as one
 * transfer and the AICA splits it into two 16-bit cycles on its own RAM bus,
 * so every physical data line carries both halves of the word. A single
 * stuck line therefore shows up twice in a 32-bit mask -- at bit n and at
 * bit n+16 -- which reads as two faults where the board has one. Folding the
 * halves together names the line that actually exists. */
static void report_badbits_aram(u32 badbits)
{
    scif_puts(S_ARAM_LINES);
    scif_puthex((badbits | (badbits >> 16)) & 0xFFFFu);
    scif_puts("\n");
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
/* Words per progress_tick: the bar, the border heartbeat and the operator's
 * key/button poll all hang off it, so it has to keep running through the
 * longest test of the suite. 1024 words is ~0.2 % of the ROM. */
#define BIOS_CRC_CHUNK  1024u

static void test_bios_rom(void)
{
    /* read through P1: cached, so one 32-byte line fill serves 8 words
     * instead of 8 separate EPROM cycles. Correct for a checksum -- the
     * content is read-only and the cache was invalidated at reset. */
    const u32 *rom = (const u32 *)0x80000000;
    u32 n = (0x00200000 - 4) >> 2;
    u32 crc = 0xFFFFFFFF;
    progress_begin(S_P_BIOS, n);
    /* crc32_rom_block eats groups of four words; the odd tail (n is 0x7FFFF,
     * not a multiple of four) goes through the same table one word at a
     * time. See src/crc32_fast.S for why this loop is hand-written. */
    u32 nq = n >> 2, done = 0;
    while (done < nq) {
        u32 q = nq - done;
        if (q > BIOS_CRC_CHUNK / 4u)
            q = BIOS_CRC_CHUNK / 4u;
        progress_tick(done << 2);
        crc = crc32_rom_block(rom + (done << 2), q, crc);
        done += q;
    }
    for (u32 i = nq << 2; i < n; i++)
        crc = crc32_word(crc, rom[i]);
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
/* Move the memory-test loops into CPU RAM and run them cached.
 *
 * Uncached from the boot EPROM each of those loops pays a bus cycle per
 * instruction; from RAM the loop body sits in the instruction cache and runs
 * at core speed. Nothing else moves, and the memory under test is still
 * addressed through P2, so the data path stays uncached and the test keeps
 * the coverage it has.
 *
 * Which 8 KB block: the four CPU RAM chips are interleaved by data lane, not
 * by address range -- IC9/IC10 carry the even words, IC11S/IC12S the odd ones,
 * sixteen bits of the sixty-four each. Every block therefore spans all four,
 * and no choice of address can dodge a chip that is dead across its range.
 * What moving the window does buy is immunity to a LOCALIZED fault, a bad row
 * or column inside one chip, which is the common case. So the blocks are
 * scanned downward from the top and the first sound one is taken.
 *
 * Every candidate gets the full pattern and pseudo-random suite before
 * anything is copied into it, and a candidate that fails is reported rather
 * than quietly skipped -- it is real broken memory. Everything scanned sits
 * above the block finally chosen, and the main CPU RAM test then covers
 * everything below it, so no region goes unexamined.
 *
 * If no block passes, the pointers keep addressing the ROM copies: a board
 * whose CPU RAM is unusable still gets a full, slow diagnostic, which is
 * exactly the board that needs one. */
static u32 g_reloc_win;             /* P2 address of the block in use, 0 = none */

/* "pass n/3 <what>" on serial, and the same on the progress bar. */
static void phase_begin(const char *label, u32 phase, const char *what,
                        u32 words)
{
    scif_puts(S_PASS);
    scif_putdec(phase);
    scif_puts("/3 ");
    scif_puts(what);
    progress_begin(pass_label(label, phase), words * 2);
}

static void relocate_try(void)
{
    /* The top block is reserved whatever happens next: the Maple DMA buffers
     * go there, clear of every pattern the cell test writes. Polling the
     * buttons during a memory test would otherwise DMA into the region under
     * test, and the test would report faults that are its own doing. */
    g_reloc_win = SDRAM_P2_BASE + g_ram_size - RELOC_WINDOW;
    maple_set_buffers(g_reloc_win + 0x1800u);

#if RELOC
    if (g_ram_size < 0x00100000u) {
        log_result(S_L_RELOC, CLIP_NONE, T_FAIL, 0, 0);
        scif_puts(S_RELOC_ROM);
        return;
    }

    /* A chip dead across its whole range shows on the data bus test in
     * thirty-two accesses. Without this check we would scan 128 blocks --
     * a megabyte of futile testing from the EPROM -- before the operator
     * learned anything, and every one of them would fail for the same
     * reason. Decide it here and go straight to the ROM copies. */
    if (ram_test_databus(SDRAM_P2_BASE)) {
        log_result(S_L_RELOC, CLIP_NONE, T_FAIL, 0, 0);
        scif_puts(S_RELOC_ROM);
        return;
    }

    ram_result scan;                /* accumulates every bad block found */
    ram_result_clear(&scan);
    u32 nbad = 0;
    u32 win = 0;                    /* the block the loops are copied into */

    for (u32 k = 0; k < RELOC_TRIES; k++) {
        u32 cand = SDRAM_P2_BASE + g_ram_size - (k + 2) * RELOC_WINDOW;
        ram_result res;
        ram_result_clear(&res);
        ram_test_pattern(cand, RELOC_WINDOW, 0x55555555, &res);
        ram_test_pattern(cand, RELOC_WINDOW, 0xAAAAAAAA, &res);
        ram_test_prng(cand, RELOC_WINDOW, 0x5EED1234, &res);
        if (!res.errors) {
            win = cand;
            break;
        }
        nbad++;
        scan.errors += res.errors;
        scan.badbits |= res.badbits;
        scan.badbits_e |= res.badbits_e;
        scan.badbits_o |= res.badbits_o;
    }

    if (nbad) {                     /* bad memory is a finding, not a detour */
        scif_puts("  ");
        scif_putdec(nbad);
        scif_puts(" bad 8KB block(s) at the top of CPU RAM\n");
        log_result(S_L_RELOC_SCAN, CLIP_SOUND_RAM, T_FAIL,
                   ram_comp_mask(&scan), IC(work_comps));
        report_badbits(scan.badbits);
        report_comps(S_CG_CPU, ram_comp_mask(&scan), IC(work_comps));
    }

    if (!win) {
        log_result(S_L_RELOC, CLIP_NONE, T_FAIL, 0, 0);
        scif_puts(S_RELOC_ROM);
        return;
    }
    g_reloc_win = win;          /* the cell test now stops below this block */

    /* the first execution of relocated code happens inside reloc_install.
     * Flag it on the border first: if this board refuses cached execution
     * from RAM the way it refused it from ROM, the screen stops on this
     * colour and says so without a serial cable. */
    progress_phase(PH_RELOC);
    u32 ok = reloc_install(win);
    log_result(S_L_RELOC, CLIP_NONE, ok ? T_OK : T_FAIL, 0, 0);
    if (!ok) {
        scif_puts(S_RELOC_ROM);
    }
#endif
}

/* One report, whatever happened: the address is read back from the pointer
 * that will really be called, not from a flag saying what should have been
 * arranged. 0x8C/0x8D is cached CPU RAM, 0xA0 is the boot EPROM. */
static void relocate_fast_loops(void)
{
    relocate_try();
    scif_puts("  loops execute at ");
    scif_puthex((u32)p_ram_prng_verify_fast);
    scif_puts((u32)p_ram_prng_verify_fast < 0xA0000000u
              ? S_RELOC_IN_RAM : S_RELOC_IN_ROM);
}


static u32 test_sdram_cells(void)
{
    u32 size = g_ram_size;
    scif_puts(S_SDRAM_HDR);

    /* data bus test runs at an even word address (A2=0): comps 1/2 */
    u32 bad = ram_test_databus(SDRAM_P2_BASE);
    u32 comps = (bad & 0xFFFF ? 1u : 0) | (bad >> 16 ? 2u : 0);
    log_result_q(S_L_SDRAM_DBUS, CLIP_DATA_BUS, bad ? T_FAIL : T_OK, comps,
                 IC(work_comps), 1, CLIP_CPU_RAM);
    if (bad) {
        report_badbits(bad);
        report_comps(S_CG_CPU, comps, IC(work_comps));
    }

    bad = ram_test_addrbus(SDRAM_P2_BASE, size);
    log_result_q(S_L_SDRAM_ABUS, CLIP_ADDR_BUS, bad ? T_FAIL : T_OK, 0, 0, 1,
                 CLIP_CPU_RAM);
    if (bad) {
        scif_puts(S_BAD_ABITS);
        scif_puthex(bad);
        scif_puts("\n");
    }

#if QUICK_TEST
    u32 len = 0x00100000;
    scif_puts(S_QUICK);
#else
    /* the top window holds the relocated test loops and was already tested
     * in full before they were copied there; testing it again would mean
     * overwriting the code we are executing */
    u32 len = g_reloc_win ? g_reloc_win - SDRAM_P2_BASE : size;
#endif

    ram_result res;
    ram_result_clear(&res);
    u32 words = len >> 2;
    phase_begin(S_P_SDRAM, 1, S_PH_0101, words);
    ram_test_pattern(SDRAM_P2_BASE, len, 0x55555555, &res);
    progress_end();
    scif_puts(res.errors ? " ERR\n" : " ok\n");

    phase_begin(S_P_SDRAM, 2, S_PH_1010, words);
    ram_test_pattern(SDRAM_P2_BASE, len, 0xAAAAAAAA, &res);
    progress_end();
    scif_puts(res.errors ? " ERR\n" : " ok\n");

    phase_begin(S_P_SDRAM, 3, S_PH_PRNG, words);
    ram_test_prng(SDRAM_P2_BASE, len, 0xDEADBEEF ^ 0x9E3779B9u, &res);
    progress_end();
#if CFG_RAM_CRC
    scif_puts(" crc=");
    scif_puthex(res.crc_r);
#endif
    scif_puts(res.errors ? " ERR\n" : " ok\n");

    t_status st = res.errors ? T_FAIL : T_OK;
    log_result(S_L_SDRAM_CELL, CLIP_CPU_RAM, st,
               ram_comp_mask(&res), IC(work_comps));
    if (res.errors) {
        report_intermittent(&res);
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
    log_result_q(S_L_ARAM_DBUS, CLIP_DATA_BUS, bad ? T_FAIL : T_OK, comps,
                 IC(aram_comps), 1, CLIP_SOUND_RAM);
    if (bad) {
        report_badbits_aram(bad);
        report_comps(S_CG_SOUND, comps, IC(aram_comps));
    }

#if QUICK_TEST
    u32 len = 0x00100000;
    scif_puts(S_QUICK);
#else
    u32 len = ARAM_SIZE;
#endif

    /* The cell test overwrites sound RAM offset 0, which is the ARM's reset
     * vector: leave it running and it would execute the test pattern. */
    aica_arm_halt();

    /* Address bus. Runs with the ARM already parked because it writes offset
     * 0 too, and it is cheap: 8 MB is 21 address lines, so the walk is a few
     * hundred accesses, not a sweep of the chip. */
    bad = aram_test_addrbus();
    log_result_q(S_L_ARAM_ABUS, CLIP_ADDR_BUS, bad ? T_FAIL : T_OK, 0, 0, 1,
                 CLIP_SOUND_RAM);
    if (bad) {
        scif_puts(S_BAD_ABITS);
        scif_puthex(bad);
        scif_puts("\n");
    }

    ram_result res;
    ram_result_clear(&res);
    u32 words = len >> 2;
    phase_begin(S_P_ARAM, 1, S_PH_0101, words);
    aram_test_pattern(0, len, 0x55555555, &res);
    progress_end();
    scif_puts(res.errors ? " ERR\n" : " ok\n");

    phase_begin(S_P_ARAM, 2, S_PH_1010, words);
    aram_test_pattern(0, len, 0xAAAAAAAA, &res);
    progress_end();
    scif_puts(res.errors ? " ERR\n" : " ok\n");

    phase_begin(S_P_ARAM, 3, S_PH_PRNG, words);
    aram_test_prng(0, len, 0xC0FFEE42 ^ 0x9E3779B9u, &res);
    progress_end();
#if CFG_RAM_CRC
    scif_puts(" crc=");
    scif_puthex(res.crc_r);
#endif
    scif_puts(res.errors ? " ERR\n" : " ok\n");

    /* a G2 bus that never drains aborts the write loops: the cell results
     * are meaningless then, and the bus is the actual fault to report */
    if (aica_g2_stalled()) {
        scif_puts("  G2 bus never went idle: sound RAM result is void\n");
        log_result(S_L_ARAM_CELL, CLIP_SOUND_RAM, T_FAIL, 0, IC(aram_comps));
        return 0;
    }

    aica_arm_park();                    /* back to the state a Naomi boots in */

    t_status st = res.errors ? T_FAIL : T_OK;
    log_result(S_L_ARAM_CELL, CLIP_SOUND_RAM, st,
               ram_comp_mask(&res), IC(aram_comps));
    if (res.errors) {
        report_intermittent(&res);
        report_badbits_aram(res.badbits);
        report_comps(S_CG_SOUND, ram_comp_mask(&res), IC(aram_comps));
        report_fails(&res);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
static void test_vram_region(const char *name, const char *dbus, const char *abus,
                             u32 base, u32 size,
                             const comp_map *comps, u32 name_clip,
                             u32 *ok_flag)
{
    ram_result res;
    ram_result_clear(&res);

    /* The bus results get their own log entries, as the CPU RAM and the
     * sound RAM ones do. They were folded into the region result before,
     * which meant a healthy board never showed that the VRAM bus had been
     * tested at all -- the one memory whose bus said nothing either way. */
    u32 bad = vram_test_databus(base);
    u32 c = (bad & 0xFFFF ? 1u : 0) | (bad >> 16 ? 2u : 0);
    log_result_q(dbus, CLIP_DATA_BUS, bad ? T_FAIL : T_OK, c, comps, 1, name_clip);
    if (bad) {
        report_badbits(bad);
        report_comps(name, c, comps);
        *ok_flag = 0;
        return;
    }

    /* Address bus. vram_test_databus() is byte-for-byte ram_test_databus(),
     * so this window takes a plain pointer and the SDRAM walk applies to it
     * unchanged -- no VRAM-specific variant needed. */
    bad = ram_test_addrbus(base, size);
    log_result_q(abus, CLIP_ADDR_BUS, bad ? T_FAIL : T_OK, 0, 0, 1, name_clip);
    if (bad) {
        scif_puts(S_BAD_ABITS);
        scif_puthex(bad);
        scif_puts("\n");
        *ok_flag = 0;
        return;
    }

#if QUICK_TEST
    (void)size;
#endif

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

    u32 words = len >> 2;
    phase_begin(S_P_VRAM, 1, S_PH_0101, words);
    vram_test_pattern(base, len, 0x55555555, &res);
    progress_end();
    scif_puts("\n");

    phase_begin(S_P_VRAM, 2, S_PH_1010, words);
    vram_test_pattern(base, len, 0xAAAAAAAA, &res);
    progress_end();
    scif_puts("\n");

    phase_begin(S_P_VRAM, 3, S_PH_PRNG, words);
    vram_test_prng(base, len, 0x7E0CBEEF ^ 0x9E3779B9u ^ base, &res);
    progress_end();
#if CFG_RAM_CRC
    scif_puts(" crc=");
    scif_puthex(res.crc_r);
#endif
    scif_puts(res.errors ? " ERR\n" : " ok\n");

    /* this test scribbles over the framebuffer: repaint once it is done so
     * the screen is readable again */
    screen_render();
    progress_screen_enable(1);
    scif_putc('\n');

    t_status st = res.errors ? T_FAIL : T_OK;
    log_result(name, name_clip, st, ram_comp_mask(&res), comps);
    if (res.errors) {
        report_intermittent(&res);
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
    test_vram_region(g_ic_valid ? S_L_VRAM_TEX0_IC : S_L_VRAM_TEX0,
                     S_L_VRAM_T0_DBUS, S_L_VRAM_T0_ABUS,
                     VRAM_TEX0_BASE, VRAM_TEX0_SIZE,
                     IC(tex0_comps), CLIP_VRAM, &tex0_ok);
    test_vram_region(g_ic_valid ? S_L_VRAM_TEX1_IC : S_L_VRAM_TEX1,
                     S_L_VRAM_T1_DBUS, S_L_VRAM_T1_ABUS,
                     VRAM_TEX1_BASE, VRAM_TEX1_SIZE,
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
    test_vram_region(S_L_VRAM_B, S_L_VRAM_B_DBUS, S_L_VRAM_B_ABUS,
                     VRAM_PVRB_BASE, VRAM_PVRB_SIZE,
                     0, CLIP_VRAM_B, &ok);
    elan_init();
    test_vram_region(S_L_ELAN, S_L_ELAN_DBUS, S_L_ELAN_ABUS,
                     ELAN_RAM_BASE, ELAN_RAM_SIZE,
                     0, CLIP_ELAN, &ok);
}

/* ------------------------------------------------------------------ */
static void test_sram_rtc(void)
{
    scif_puts(S_PERIPH_HDR);

    ram_result res;
    u32 mask = sram_test(&res);
    /* positions 1/2 = even/odd byte lane; IC designators pending the
     * user's silkscreen readout -> spoken as numbered positions */
    log_result(S_L_BACKSRAM, CLIP_SRAM,
               mask ? T_FAIL : T_OK, mask, 0);
    if (mask) {
        report_intermittent(&res);
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
/* Full DIMM board check, in two parts.
 *
 * (1) Non-destructive, always: read every block TWICE and compare. A cell
 * that will not hold, a marginal bus or a dying board shows up as a
 * difference between two reads of the same addresses -- exactly the failure a
 * content checksum cannot tell from bad data. That is cart_pin_scan, already
 * validated, run over the DIMM's contents with a per-data-line report.
 *
 * (2) Destructive, operator-initiated: a real write/read/verify of the DIMM
 * SDRAM. The DIMM presents its DRAM to the Naomi as a ROM board, but the same
 * GD-DMA engine that loads a game can also write it back (SB_GDDIR=1) -- a
 * path reversed from the DIMM-aware BIOS, not present in MAME's model. So we
 * push the requested patterns over G1-DMA, read them back and CRC them. This
 * overwrites the loaded game image, which is why it belongs to this
 * operator-chosen test and nowhere in the automatic sweep. It cannot touch
 * the DIMM's firmware (that runs from the board's own flash, not this SDRAM). */
static void test_dimm(void)
{
    dimm_info di;
    g1_bus_init();
    dimm_probe(&di);

    scif_puts(S_DIMM_HDR);
    scif_puts(S_DIMM_REGS);
    scif_puthex(di.command);
    scif_putc(' ');
    scif_puthex(di.offsetl);
    scif_putc(' ');
    scif_puthex(di.paraml);
    scif_putc(' ');
    scif_puthex(di.paramh);
    scif_putc(' ');
    scif_puthex(di.status);
    scif_puts("\n");

    if (!di.present) {
        log_result(S_L_DIMM_ABSENT, CLIP_NONE, T_OK, 0, 0);
        return;
    }
    log_result(S_L_DIMM_PRESENT, CLIP_NONE, T_OK, 0, 0);

    /* Read-twice-and-compare over as much of the board as the operator is
     * willing to wait for; TEST or a key stops it at the next block. */
#if QUICK_TEST
    const u32 span = 0x00040000;
#else
    const u32 span = 0x01000000;        /* 16 MB, abortable */
#endif
    cart_pin_stats st;
    progress_begin(S_P_DIMM, span);
    cart_pin_scan(0, span, &st);
    progress_end();

    scif_puts(S_PINS_SAMPLED);
    scif_putdec(st.words);
    scif_puts("\n");

    u32 unstable = 0;
    for (u32 b = 0; b < 16; b++) {
        if (st.flaky[b]) {
            scif_puts(S_PIN);
            scif_putdec(b);
            scif_puts(S_PIN_FLAKY);
            scif_putdec(st.flaky[b]);
            scif_puts("\n");
            unstable++;
        }
    }
    log_result(S_L_DIMM_MEM, CLIP_NONE, unstable ? T_FAIL : T_OK, 0, 0);

    /* Destructive cell test: write the requested patterns over the DIMM
     * SDRAM via G1-DMA and read them back with a CRC. Overwrites the game
     * image, hence gated behind this operator-initiated test. dimm_mem_test
     * checks its own system-RAM scratch, so it is safe to call here without
     * threading the main-RAM verdict. */
#if QUICK_TEST
    const u32 mspan = 0x00040000;       /* 256 KB */
#else
    const u32 mspan = 0x00400000;       /* 4 MB, abortable at each block */
#endif
    static const u32 patterns[2] = { 0x01010101u, 0x10101010u };
    u32 mem_fail = 0, mem_ran = 0;

    scif_puts(S_DIMM_MEM_HDR);
    for (u32 p = 0; p < 2; p++) {
        dimm_mem_result mr;
        progress_begin(S_P_DIMM, mspan);
        dimm_mem_test(mspan, patterns[p], &mr);
        progress_end();

        scif_puts(S_DIMM_MEM_PAT);
        scif_puthex(patterns[p]);
        scif_puts("\n");

        if (mr.timeout) {               /* present but DMA never completed */
            scif_puts(S_DIMM_MEM_TIMEOUT);
            mem_fail = 1;
            break;                      /* the second pattern would only stall too */
        }
        if (mr.blocks == 0) {           /* scratch RAM bad: same both passes */
            scif_puts(S_DIMM_SCRATCH_BAD);
            break;
        }
        mem_ran = 1;

        scif_puts(S_DIMM_MEM_BLOCKS);
        scif_putdec(mr.blocks);
        scif_puts("\n");
        scif_puts(S_DIMM_MEM_CRC);
        scif_puthex(mr.crc_w ^ 0xFFFFFFFFu);
        scif_putc('/');
        scif_puthex(mr.crc_r ^ 0xFFFFFFFFu);
        scif_puts("\n");
        scif_puts(S_DIMM_MEM_ERRS);
        scif_putdec(mr.errors);
        scif_puts("\n");

        if (mr.errors) {
            scif_puts(S_DIMM_MEM_BADBITS);
            scif_puthex(mr.badbits);
            scif_puts("\n");
            scif_puts(S_DIMM_MEM_FIRST);
            scif_puthex(mr.first_addr);
            scif_puts(S_DIMM_MEM_EXPGOT);
            scif_puthex(mr.first_exp);
            scif_putc('/');
            scif_puthex(mr.first_got);
            scif_puts("\n");
            mem_fail = 1;
        }
    }
    if (mem_ran || mem_fail)
        log_result(S_L_DIMM_MEM_PAT, CLIP_NONE, mem_fail ? T_FAIL : T_OK, 0, 0);
}

/* ------------------------------------------------------------------ */
/* DIMM firmware update: identify the flash, then let the operator pick a
 * version. Reversed from Sega's own DIMM FIRMWARE UPDATE program; the flash
 * is driven over the G1 PIO (see dimm_flash.c). Only the non-destructive
 * identify + the version menu are wired: the write path is a single AMD
 * chip-erase + full-image reprogram, which wipes the recovery slot too, so
 * it stays disarmed until validated on real hardware. */
static const char *const g_fw_ver[3] = { "3.17", "4.01", "4.03" };

static void test_dimm_flash(void)
{
    dimm_info di;
    g1_bus_init();
    dimm_probe(&di);

    scif_puts(S_FW_HDR);
    if (!di.present) {
        scif_puts(S_FW_NO_DIMM);
        return;
    }

    scif_puts(S_FW_IDING);
    dimm_flash_id fi;
    dimm_flash_identify(&fi);
    if (!fi.id_ok) {
        scif_puts(S_FW_NO_DIMM);
        return;
    }
    scif_puts(S_FW_ID);
    scif_puthex(fi.mfr);
    scif_putc('/');
    scif_puthex(fi.dev);
    scif_puts("\n");

    /* version selection on the serial console (board buttons cancel) */
    scif_puts(S_FW_MENU);
    int sel = -1;
    for (;;) {
        input_event ev;
        if (input_poll(&ev)) {
            if (ev.kind == INPUT_KEY) {
                if (ev.key >= '1' && ev.key <= '3')
                    sel = ev.key - '1';
                break;
            }
            if (ev.kind == INPUT_SELECT || ev.kind == INPUT_CONFIRM)
                break;
        }
        progress_heartbeat();
    }

    if (sel < 0) {
        scif_puts(S_FW_ABORT);
        return;
    }
    scif_puts(S_FW_SEL);
    scif_puts(g_fw_ver[sel]);
    scif_puts("\n");
    scif_puts(S_FW_PENDING);
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
        input_set_mie_port(g_mie_port1);
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

/* "<what> <ic>" for the bar, in its own buffer: each chip opens a new bar, so
 * the previous label is finished with by the time this is rewritten. */
static char g_cartbuf[48];

static const char *cart_label(const char *what, const char *icname)
{
    u32 i = 0;
    while (what[i] && i < 24) {
        g_cartbuf[i] = what[i];
        i++;
    }
    if (icname) {
        g_cartbuf[i++] = ' ';
        for (u32 j = 0; icname[j] && i < 44; j++)
            g_cartbuf[i++] = icname[j];
    }
    g_cartbuf[i] = 0;
    return g_cartbuf;
}

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
        progress_tick(done);
        if ((done & 0x3FFFFF) == 0)
            scif_putc('.');
    }
    progress_end();
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
    progress_begin(S_P_CART_PINS, span);
    cart_pin_scan(0, span, &st);
    progress_end();

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
    progress_begin(S_P_CART_ID, cartdb_first_sizes[CARTDB_NFIRST - 1]);
    for (u32 s = 0; s < CARTDB_NFIRST && game == 0xFFFFFFFF; s++) {
        u32 target = cartdb_first_sizes[s];
        while (done < target) {
            progress_tick(done);
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
    progress_end();
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
        progress_begin(cart_label(S_P_CART_IC, cartdb_ic_name[idx]),
                       cartdb_ic_size[idx]);
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

#if !NO_AUDIO
static void audio_replay_log(void);   /* defined below */
#endif

#define AUDIO_ZONE_OFF  0x00010000u     /* clip landing zone in sound RAM */
#define AUDIO_ZONE_LEN  0x00020000u     /* 128 KB: was sized for 16-bit
                                         * clips; ADPCM needs a quarter of
                                         * that, so it now proves more
                                         * sound RAM than speech requires */
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
        progress_phase(PH_AUDIO_ON);    /* yellow */
#if !NO_AUDIO
        g_audio_ready = 1;
        audio_replay_log();             /* spoken replay of the report so far */
#endif
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
#if !NO_AUDIO
static void audio_replay_log(void)
{
    scif_puts(S_AUDIO_ONLINE);
    say(CLIP_AUDIO_OK, REPORT_GAP_MS);
    for (u32 i = 0; i < g_log_n; i++)
        say_entry(&g_log[i]);
}
#endif

/* ------------------------------------------------------------------ */

#if CFG_LANE_BEACON
/* ------------------------------------------------------------------------
 * Lane beacon: which silkscreen chip carries which 16-bit slice of the bus.
 *
 * The test suite knows a fault is on, say, D0-D15 of the even word. Turning
 * that into a designator needs a link between an electrical lane and a
 * physical package, and that link cannot be inferred -- inferring it from
 * the order the original BIOS prints IC numbers is exactly what got the map
 * wrong three times over.
 *
 * It can be measured, though, and without touching the CPU. The SH-4 can
 * write 16 bits at a time, and on a 64-bit bus a halfword write asserts the
 * byte mask of exactly one 16-bit slice, so exactly one chip is selected:
 *
 *     offset +0 -> D0-D15    +2 -> D16-D31    +4 -> D32-D47    +6 -> D48-D63
 *
 * Hammering one offset in a loop therefore pulses the DQM pins of one chip
 * and leaves the other three masked. Put a scope on LDQM or UDQM of each RAM
 * -- they are TSOP and reachable, unlike anything under a heatsink -- and the
 * one that moves during a given phase is that group's chip. The same applies
 * to the eight GPU RAM chips: the offset picks the chip within a bank, the
 * base address picks TEX0 or TEX1.
 *
 * Runs after the report, forever, because by then there is nothing left to
 * disturb and the operator needs time with a probe. */
static void lane_beacon_phase(u32 base, u32 offset, const char *what,
                              u32 halfword)
{
    scif_puts(S_BEACON_ON);
    scif_puts(what);
    scif_puts("\n");
    log_result(what, CLIP_NONE, T_OK, 0, 0);

    /* Two ways of lighting up one 16-bit slice, because the two memories
     * hang off different buses.
     *
     * CPU RAM is on the SH-4's own bus, so a 16-bit write asserts the byte
     * mask of exactly one slice: one chip sees DQM pulse and its data lines
     * move, the other three stay masked and quiet. Both pins discriminate.
     *
     * VRAM is not on that bus -- it belongs to the graphics chip, which
     * mediates every access -- so the SH-4's byte masks say nothing about
     * which VRAM chip is selected. There the write is 32 bits wide with only
     * the targeted half toggling and the other half held constant: the
     * chip carrying the moving half shows activity on its data pins while
     * its neighbour sits still, whatever the graphics chip does with byte
     * enables. Probe DQ rather than DQM for those. */
    for (u32 rep = 0; rep < 4000; rep++) {
        if (halfword) {
            volatile u16 *p = (volatile u16 *)(base + offset);
            for (u32 i = 0; i < 64; i++) {
                p[i * 4] = 0xAAAA;
                p[i * 4] = 0x5555;
            }
        } else {
            /* offset 0 or 4 selects the 32-bit word; 0 or 2 within it says
             * which half moves and which is pinned to a constant */
            volatile u32 *p = (volatile u32 *)(base + (offset & 4));
            u32 lo = (offset & 2) == 0;
            for (u32 i = 0; i < 64; i++) {
                p[i * 2] = lo ? 0x0000AAAAu : 0xAAAA0000u;
                p[i * 2] = lo ? 0x00005555u : 0x55550000u;
            }
        }
        if ((rep & 0x7F) == 0)
            progress_heartbeat();
    }
}

static void lane_beacon(void)
{
    static const char *const cpu_names[4] = {
        S_BEACON_CPU1, S_BEACON_CPU2, S_BEACON_CPU3, S_BEACON_CPU4
    };
    static const char *const vram0_names[4] = {
        S_BEACON_T0_1, S_BEACON_T0_2, S_BEACON_T0_3, S_BEACON_T0_4
    };
    static const char *const vram1_names[4] = {
        S_BEACON_T1_1, S_BEACON_T1_2, S_BEACON_T1_3, S_BEACON_T1_4
    };

    scif_puts(S_BEACON_HDR);
    for (;;) {
        g_log_n = 0;                    /* one phase on screen at a time */
        for (u32 k = 0; k < 4; k++)
            lane_beacon_phase(SDRAM_P2_BASE, k * 2, cpu_names[k], 1);
        for (u32 k = 0; k < 4; k++)
            lane_beacon_phase(VRAM_TEX0_BASE, k * 2, vram0_names[k], 0);
        for (u32 k = 0; k < 4; k++)
            lane_beacon_phase(VRAM_TEX1_BASE, k * 2, vram1_names[k], 0);
    }
}
#endif  /* CFG_LANE_BEACON */


#if CFG_JVS_MAP
/* Print the MIE's control words whenever any bit changes, and say which bits
 * moved. Press TEST, press START, read the answer off the serial console --
 * this is how the mapping gets established, since no public source carries
 * it. Runs after the report, forever. */
static void jvs_map_aid(void)
{
    u32 prev[14], cur[14];
    for (u32 i = 0; i < 14; i++)
        prev[i] = 0;
    if (!g_mie_port1) {
        scif_puts(S_JVS_NO_MIE);
        return;
    }
    scif_puts(S_JVS_MAP_HDR);
    for (;;) {
        if (maple_jvs_read(g_mie_port1 - 1, cur) == 0) {
            u32 changed = 0;
            for (u32 i = 0; i < 14; i++)
                if (cur[i] != prev[i])
                    changed = 1;
            if (changed) {
                for (u32 i = 0; i < 14; i++) {
                    scif_puthex(cur[i]);
                    scif_putc(i == 13 ? '\n' : ' ');
                }
                for (u32 i = 0; i < 14; i++) {
                    u32 d = cur[i] ^ prev[i];
                    if (!d)
                        continue;
                    scif_puts("  mot ");
                    scif_putdec(i);
                    scif_puts(" bits ");
                    scif_puthex(d);
                    scif_puts("\n");
                    prev[i] = cur[i];
                }
            }
        }
        delay_ms(30);
        progress_heartbeat();
    }
}
#endif


/* ---- looping memory tests -------------------------------------------- */

/* One region, over and over, counting passes and accumulated errors. Runs
 * until the TEST button; a stray byte on the console must not end a soak. */
static void loop_region(u32 base, u32 len, const char *what)
{
    u32 pass = 0, errors = 0;
    progress_set_loop(1);
    while (!progress_aborted()) {
        ram_result res;
        ram_result_clear(&res);
        pass++;

        scif_puts(S_LOOP_PASS);
        scif_putdec(pass);
        scif_puts(": ");
        progress_begin(what, (len >> 2) * 2);
        if (base == ARAM_P2_BASE) {
            aram_test_pattern(0, len, 0x55555555, &res);
            aram_test_pattern(0, len, 0xAAAAAAAA, &res);
            aram_test_prng(0, len, 0xC0FFEE42 ^ pass, &res);
        } else if (base == SDRAM_P2_BASE) {
            ram_test_pattern(base, len, 0x55555555, &res);
            ram_test_pattern(base, len, 0xAAAAAAAA, &res);
            ram_test_prng(base, len, 0xDEADBEEF ^ pass, &res);
        } else {
            vram_test_pattern(base, len, 0x55555555, &res);
            vram_test_pattern(base, len, 0xAAAAAAAA, &res);
            vram_test_prng(base, len, 0x7E0CBEEF ^ pass, &res);
        }
        progress_end();

        errors += res.errors;
        scif_puts(S_LOOP_ERR);
        scif_putdec(errors);
        scif_puts("\n");
        if (res.errors) {
            report_badbits(res.badbits);
            report_comps(S_CG_CPU, ram_comp_mask(&res),
                         IC(base == SDRAM_P2_BASE ? work_comps : 0));
        }
    }
    progress_set_loop(0);
    progress_clear_abort();
    scif_puts(S_ABORTED);
}

/* ---- the menu -------------------------------------------------------- */

/* Replaces the report: the screen is short of lines and a menu overlaid on
 * results would be readable as neither. The report is rebuilt on the way
 * out, from the log, which is still intact. */
static void menu_draw(u32 sel)
{
    if (!g_screen_ready)
        return;
    fb_clear(0);
    fb_text(16, 8, S_MENU_TITLE, COL_TITLE, FB_W);
    for (u32 i = 0; i < ACT_COUNT; i++) {
        u32 y = 64 + i * 28;
        fb_text(16, y, i == sel ? ">" : " ", COL_TITLE, FB_W);
        fb_text(48, y, menu_label[i], i == sel ? COL_TITLE : COL_WHITE, FB_W);
    }
}

static void run_action(u32 act);

/* TEST steps through the entries and wraps at the end; START runs the one
 * shown. A serial key jumps straight to its action without the menu. */
static void menu_run(void)
{
    u32 sel = 0;
    menu_draw(sel);
    scif_puts(S_MENU_TITLE);
    scif_puts("\n");

    for (;;) {
        input_event ev;
        if (input_poll(&ev)) {
            if (ev.kind == INPUT_SELECT) {
                sel = (sel + 1) % ACT_COUNT;
                menu_draw(sel);
            } else if (ev.kind == INPUT_CONFIRM) {
                run_action(sel + 1);
                return;
            } else if (ev.kind == INPUT_KEY) {
                if (ev.key == 'h' || ev.key == 'H') {
                    scif_puts(S_HELP);
                } else {
                    for (u32 i = 0; i < ACT_COUNT; i++) {
                        if (ev.key == menu_key[i]) {
                            run_action(i + 1);
                            return;
                        }
                    }
                }
            }
        }
        progress_heartbeat();
        delay_ms(20);
    }
}

static void run_action(u32 act)
{
    progress_clear_abort();
    switch (act) {
    case ACT_CPU:
        loop_region(SDRAM_P2_BASE,
                    g_reloc_win ? g_reloc_win - SDRAM_P2_BASE : g_ram_size,
                    S_M_CPU);
        break;
    case ACT_VRAM:
        loop_region(VRAM_TEX0_BASE, VRAM_TEX0_SIZE, S_M_VRAM);
        break;
    case ACT_ARAM:
        aica_arm_halt();
        loop_region(ARAM_P2_BASE, ARAM_SIZE, S_M_ARAM);
        aica_arm_park();
        break;
    case ACT_DIMM:
        test_dimm();
        break;
    case ACT_GAME:
        test_x76();
        test_cartridge();
        break;
    case ACT_FLASH:
        test_dimm_flash();
        break;
    default:
        break;
    }
}

/* Idle on the report until the operator asks for something. */
static void console_idle(void)
{
    scif_puts(S_WAIT_TEST);
    for (;;) {
        input_event ev;
        if (input_poll(&ev)) {
            if (ev.kind == INPUT_SELECT || ev.kind == INPUT_CONFIRM) {
                menu_run();
                screen_render();
                scif_puts(S_WAIT_TEST);
            } else if (ev.kind == INPUT_KEY) {
                if (ev.key == 'h' || ev.key == 'H') {
                    scif_puts(S_HELP);
                } else {
                    for (u32 i = 0; i < ACT_COUNT; i++) {
                        if (ev.key == menu_key[i]) {
                            run_action(i + 1);
                            screen_render();
                            scif_puts(S_WAIT_TEST);
                            break;
                        }
                    }
                }
            }
        }
        progress_heartbeat();
        delay_ms(20);
    }
}

void cmain(void)
{
    timer_init();
    reloc_init();

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

    /* Board identification, before the ROM checksum for two reasons.
     *
     * It sets g_ic_valid, which is what lets every later result name a
     * silkscreen designator -- including the EPROM's own. With the checksum
     * first, IC(bios_comps) was still 0 when it logged, so a worn EPROM was
     * reported as a bad checksum and nothing else, which is precisely the
     * case where the operator most wants to be told IC27.
     *
     * And it takes milliseconds while the checksum takes the longest minute
     * of the suite, so the board type reaches the screen and the speaker
     * straight away instead of after it.
     *
     * The caution it used to carry still holds and is still satisfied: it
     * probes addresses whose behaviour on real hardware is less certain
     * than plain memory, so it must run with screen and audio already up --
     * which the two bring-ups above have just done. */
    test_board();
    progress_phase(PH_BOARD_ID);        /* magenta */

    /* Only now the 2 MB ROM checksum: it is the single longest test in the
     * whole suite (4.2 M table steps) and it must never run while the
     * operator is still staring at a black screen. */
    test_bios_rom();

    relocate_fast_loops();

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
    /* From here on nothing measures anything: the NVRAM, the RTC, the DIMM
     * probe, Maple and the EEPROMs all answer yes or no. A bar left up would
     * sit at whatever the last test put it, which says less than no bar at
     * all -- so it goes, and the report takes the rows it occupied. */
    progress_retire();

    test_sram_rtc();
    test_maple_mie(usable);
    test_settings_eeprom();
    test_serial_eeprom();

    /* The loops have been executing out of CPU RAM on a board whose CPU RAM
     * is what we just spent the whole suite testing. A cell that passed the
     * 8 KB qualification and then decayed would have corrupted the code
     * producing every result since, so the block is re-read and compared
     * against the master copy in ROM before anything is summarised. */
    if (reloc_active()) {
        u32 off, expect, got;
        if (reloc_verify(&off, &expect, &got)) {
            log_result(S_L_RELOC_CHK, CLIP_NONE, T_OK, 0, 0);
        } else {
            log_result(S_L_RELOC_CHK, CLIP_NONE, T_FAIL, 0, 0);
            scif_puts("  relocated code changed at offset ");
            scif_puthex(off);
            scif_puts(": wrote ");
            scif_puthex(expect);
            scif_puts(", read ");
            scif_puthex(got);
            scif_puts("\n");
            scif_puts(S_RELOC_SUSPECT);
        }
    }

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

#if CFG_JVS_MAP
    jvs_map_aid();                      /* never returns */
#endif
#if CFG_LANE_BEACON
    lane_beacon();                      /* never returns */
#else
    console_idle();                     /* never returns */
#endif
}
