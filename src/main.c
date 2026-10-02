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
#include "eta.h"
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
#ifdef LANG_FR
#define LANG_IS_FR 1
#else
#define LANG_IS_FR 0
#endif
/* what a line report names (log_entry.kind) */
#define LK_NONE 0
#define LK_DATA 1           /* cut data line: D number, chip, DQ     */
#define LK_ADDR 2           /* SDRAM address pin A0-A11              */
#define LK_BANK 3           /* SDRAM bank pin BA0/BA1                */
#define LK_ABIT 4           /* CPU address bit, no pin map known     */
#define LK_INFO 5           /* a line of text, no verdict            */
#define LK_ICROW 6          /* cartridge chips, up to 4 per row      */
#define IC_OK     0         /* LK_ICROW cell: SHA-1 matches          */
#define IC_BAD    1         /*   answers, wrong content              */
#define IC_ABS    2         /*   silent, or a mirror of another chip */
#define IC_NOHASH 3         /*   answers, not hashed (QUICK build)   */
#define COL_AMBER RGB565(31, 40, 0)
#define REPORT_GAP_MS 1000          /* >= 1 s between spoken reports */

#define CLIP_NONE   0xFFFFFFFFu

static u32 g_audio_ready;           /* sound RAM validated, AICA usable */
static u32 g_ic_valid;              /* identified board: IC tables apply */
static board_type g_board_type;     /* set by test_board() */
static u32 g_mie_port1;             /* MIE maple port + 1; 0 = none  */
u32 g_mie_prog;                     /* our Z80 program is resident   */

/* IC tables were extracted from the Naomi 1 BIOS; on any other board we
 * fall back to numbered positions instead of announcing wrong ICs. */
#define IC(table) (g_ic_valid ? (table) : 0)


/* ========================================================================
 * Operator menu: keys on the serial line, buttons on the board or the
 * cabinet. Both drive the same set of actions.
 * ===================================================================== */

#define ACT_NONE    0
#define ACT_CPU     1
#define ACT_VRAM    2
#define ACT_ARAM    3
#define ACT_DIMM    4
#define ACT_GAME    5
#define ACT_FLASH   6
#define ACT_JVS     7
#define ACT_PATTERN 8
#define ACT_COUNT   8

static const char *const menu_label[ACT_COUNT] = {
    S_M_CPU, S_M_VRAM, S_M_ARAM, S_M_DIMM, S_M_GAME, S_M_FLASH, S_M_JVS,
    S_M_PATTERN
};
static const char menu_key[ACT_COUNT] = { 'c', 'v', 's', 'd', 'g', 'f', 'j', 'm' };


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
        /* Only the board buttons end a soak run -- unless there are none to
         * press. The buttons exist only once the Z80 program answers in the
         * MIE; without it a loop started from the console could only be
         * stopped by a reset. So 'a' does it then, and only then. */
        if (in_loop) {
            if (!g_mie_prog && (ev.key == 'a' || ev.key == 'A'))
                progress_request_abort(ABORT_PLAIN);
            return;
        }
        if (ev.key == 'a' || ev.key == 'A') {
            progress_request_abort(ABORT_PLAIN);
            return;
        }
        for (u32 i = 0; i < ACT_COUNT; i++) {
            if (ev.key == menu_key[i]) {
                progress_request_abort(ABORT_ACT(i + 1));
                return;
            }
        }
        return;                         /* unknown key: ignored */
    }
    /* Either board button always stops, TEST or SERVICE alike. In a soak
     * run that is all it does; anywhere else it is also the way into the
     * menu, as it is once the report is up. */
    if (ev.kind == INPUT_SELECT || ev.kind == INPUT_CONFIRM)
        progress_request_abort(in_loop ? ABORT_PLAIN : ABORT_MENU);
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
/* T_SKIP: the test could not run, which is neither a pass nor a fault --
 * the screen says so in amber, the speech says why. */
typedef enum { T_OK = 0, T_FAIL = 1, T_SKIP = 2 } t_status;

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
    { CLIP_IC_16, "IC16" },         /* first 4 MiB, D0-D15 */
    { CLIP_IC_18, "IC18" },         /* first 4 MiB, D16-D31 */
    { CLIP_IC_20, "IC20" },         /* last 4 MiB, D0-D15 */
    { CLIP_IC_22, "IC22" },         /* last 4 MiB, D16-D31 */
};
static const comp_map tex1_comps[4] = {
    { CLIP_IC_17S, "IC17S" },
    { CLIP_IC_19S, "IC19S" },
    { CLIP_IC_21S, "IC21S" },
    { CLIP_IC_23S, "IC23S" },
};

/* EPR-23608C Naomi 2 TXB0/TXB1 tables, in BIOS result-bit order. The even
 * numbers are on the underside, as TEX1's are (designators read off a
 * Naomi 2 and checked against Sega's service manual). */
static const comp_map pvrb_comps[8] = {
    { CLIP_IC_111, "IC111" },  { CLIP_IC_113, "IC113" },
    { CLIP_IC_115, "IC115" },  { CLIP_IC_117, "IC117" },
    { CLIP_IC_112S, "IC112S" }, { CLIP_IC_114S, "IC114S" },
    { CLIP_IC_116S, "IC116S" }, { CLIP_IC_118S, "IC118S" },
};

/* EPR-23608C POLY table: bit 0/1 = even/odd 32-bit word of the lower
 * 16 MiB, bit 2/3 the same in the upper 16 MiB (see vram_mapping.h). */
static const comp_map elan_comps[4] = {
    { CLIP_IC_106, "IC106" },  { CLIP_IC_107, "IC107" },
    { CLIP_IC_108S, "IC108S" }, { CLIP_IC_109S, "IC109S" },
};


typedef struct {
    const char *name;               /* points into ROM */
    u32 clip;                       /* CLIP_xxx id or CLIP_NONE */
    t_status status;
    u32 detail;                     /* component bitmask (bit n = comp n+1) */
    const comp_map *comps;          /* NULL, or 4-entry position->IC table */
    u32 quiet_ok;                   /* screen+speech: only when it FAILS   */
    u32 pre;                        /* clip spoken first, or CLIP_NONE     */
    u32 kind;                       /* LK_*: what a line report names     */
    u32 line;                       /* D number, SDRAM pin, or CPU bit    */
    u32 dq;                         /* data line: the chip's own DQ pin   */
} log_entry;

static log_entry g_log[LOG_MAX];
static u32 g_log_n;
static u32 g_screen_ready;          /* TEX0 VRAM validated, display up */

/* The RTC reading, rendered once and kept: the screen needs it again after
 * any full repaint, and re-reading would cross G2 for nothing. Empty until
 * the RTC test has run, which is late in the suite. */
static char g_rtc_str[20];

/* Under the title, centred: 19 characters at 16 px. Costs no report row --
 * the rows start at y=48. */
static void screen_draw_date(void)
{
    if (g_screen_ready && g_rtc_str[0])
        fb_text(168, 28, g_rtc_str, COL_WHITE, FB_W);
}

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

    if (e->kind == LK_INFO) {
        fb_text(16, y, e->name, COL_WHITE, FB_W);
        return y + 20;
    }
    if (e->kind == LK_ICROW) {
        /* "IC22 OK   IC1 OK   IC2 HS   IC3 ABS": four columns of 152 px,
         * room for the longest name (IC12S) and the longest word (ABS) */
        for (u32 k = 0; k < e->dq; k++) {
            const char *nm = cartdb_ic_name[e->line + k];
            char cell[8];
            u32 n = 0;
            while (nm[n] && n < 7) {
                char ch = nm[n];
                cell[n++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
            }
            cell[n] = 0;
            u32 x = 16 + k * 152;
            u32 st = (e->detail >> (4 * k)) & 0xFu;
            fb_text(x, y, cell, COL_WHITE, x + 152);
            fb_text(x + 16 * (n + 1), y,
                    st == IC_OK ? S_SCR_OK : st == IC_BAD ? S_SCR_IC_BAD :
                    st == IC_ABS ? S_SCR_IC_ABS : S_SCR_IC_NOHASH,
                    st == IC_OK ? COL_GREEN : st == IC_NOHASH ? COL_WHITE
                                                               : COL_RED,
                    x + 152);
        }
        return y + 20;
    }
    if (e->status == T_SKIP) {
        /* right-aligned with OK and FAIL, one character in from the edge */
        u32 x = FB_W - 16 * (S_SCR_SKIP_LEN + 1);
        fb_text(16, y, e->name, COL_WHITE, x - 16);
        fb_text(x, y, S_SCR_SKIP, COL_AMBER, FB_W);
        return y + 20;
    }

    if (e->status == T_OK) {
        fb_text(16, y, e->name, COL_WHITE, FB_STATUS_X);
        fb_text(FB_W - 16 * 3, y, S_SCR_OK, COL_GREEN, FB_W);
        return y + 20;
    }

    /* A failure names its chips on its own line, in red just before FAIL,
     * rather than on a line of their own: a Naomi 2 report is one line
     * short of the screen already. The label gives way to them. */
    char chips[20];
    u32 n = 0;
    if (e->detail && e->comps && e->kind == LK_NONE) {
        const char *last = 0;
        u32 distinct = 0;
        u32 ncomps = e->comps == pvrb_comps ? 8u : 4u;
        for (u32 b = 0; b < ncomps; b++)
            if ((e->detail & (1u << b)) && e->comps[b].name != last)
                distinct++, last = e->comps[b].name;
        last = 0;
        for (u32 b = 0; b < ncomps; b++) {
            if (!(e->detail & (1u << b)) || e->comps[b].name == last)
                continue;
            last = e->comps[b].name;
            const char *s = e->comps[b].name;
            u32 len = 0;
            while (s[len])
                len++;
            if (n + len + (n ? 1 : 0) > 13) {   /* no room: how many */
                n = 0;
                chips[n++] = (char)('0' + distinct % 10);
                for (const char *t = S_ADDR_CHIPS; *t && n < 19; t++)
                    chips[n++] = *t;
                break;
            }
            if (n)
                chips[n++] = ' ';
            while (*s)
                chips[n++] = *s++;
        }
    }
    chips[n] = 0;
    u32 fail_x = FB_W - 16 * 6;
    u32 chips_x = n ? fail_x - 16 * (n + 1) : fail_x;
    /* A label too long for the chips beside it drops its parenthesis --
     * on the memories it lists the region's chips, which the red names
     * now say better -- rather than being cut mid-word. */
    const char *label = e->name;
    char lbuf[40];
    u32 llen = 0;
    while (label[llen])
        llen++;
    if (16 + llen * 16 > chips_x - 16) {
        u32 i = 0;
        while (label[i] && i < sizeof lbuf - 1 &&
               !(label[i] == ' ' && label[i + 1] == '('))
            lbuf[i] = label[i], i++;
        lbuf[i] = 0;
        label = lbuf;
    }
    fb_text(16, y, label, COL_WHITE, chips_x - 16);
    if (n)
        fb_text(chips_x, y, chips, COL_RED, fail_x);
    fb_text(fail_x, y, S_SCR_FAIL, COL_RED, FB_W);
    return y + 20;
}

/* Add the newest result to what is already on screen.
 *
 * This used to call screen_render, which clears all 640x480 and redraws
 * every line -- 153,600 uncached VRAM writes to add one line, on every
 * result. Nothing about an appended line requires that: its position depends
 * only on the lines before it, and those are already correct on screen. */
/* ---- the button hint ---------------------------------------------------
 * While the suite runs, once the MIE program reads the buttons: which ones
 * stop the suite and open the operator menu, in blue a blank line above
 * the progress label. It gives way for good when the report reaches its
 * line, and goes when the suite ends. */
#define COL_HINT    RGB565(10, 40, 31)

static u32 g_hint_on, g_hint_done;
static u32 g_keys_box_shown;            /* console reminder printed once */

/* The framed a/h reminder on the console, once per boot: as soon as the
 * board's buttons answer, and in any case before the long memory tests,
 * which is what 'a' is there to cut short -- with or without the MIE. */
static void keys_box(void)
{
    if (g_keys_box_shown)
        return;
    g_keys_box_shown = 1;
    scif_puts(S_KEYS_BOX);
}

static void hint_hide(void)
{
    if (g_hint_on && g_screen_ready)
        fb_fill_rows(fb_hint_y(), fb_hint_y() + 18, 0);
    g_hint_on = 0;
}

static void hint_retire(void)
{
    hint_hide();
    g_hint_done = 1;
}

static void hint_draw(void)
{
    if (!g_screen_ready || g_hint_done || !g_mie_prog)
        return;
    if (g_screen_y + 16 > fb_hint_y()) {    /* the report got there */
        hint_retire();
        return;
    }
    fb_text(16, fb_hint_y(), S_BUTTON_HINT, COL_HINT, FB_W);
    g_hint_on = 1;
}

static void screen_append(const log_entry *e)
{
    if (!g_screen_ready)
        return;
    if (g_hint_on && g_screen_y + 16 > fb_hint_y())
        hint_retire();                  /* this line lands on the hint */
    g_screen_y = screen_draw_entry(e, g_screen_y);
}

/* Rebuild the whole screen. Needed exactly twice: when the display first
 * comes up, and after the VRAM test has scribbled over the framebuffer. */
static void screen_render(void)
{
    if (!g_screen_ready)
        return;
    fb_clear(0);
    fb_text(112, 8, "NAOMI DIAG ROM v" DIAG_VERSION, COL_TITLE, FB_W);
    screen_draw_date();
    u32 y = 48;
    for (u32 i = 0; i < g_log_n; i++)
        y = screen_draw_entry(&g_log[i], y);
    g_screen_y = y;
    g_hint_on = 0;                      /* the clear took it too */
    hint_draw();
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
/* 0..63, from the number clips: French composes 17-19 and 61 its own way,
 * but with 0-19 and the tens as clips only "et un" needs a rule. */
static void say_number(u32 n)
{
    if (n < 20) {
        say(CLIP_W_0 + n, 0);
        return;
    }
    u32 t = n / 10, u = n - t * 10;     /* constant divide: no libgcc */
    say(CLIP_W_20 + (t - 2), 0);
    if (u == 1 && LANG_IS_FR)
        say(CLIP_W_AND_ONE, 0);
    else if (u)
        say(CLIP_W_0 + u, 0);
}

static void say_entry(const log_entry *e)
{
    if (!g_audio_ready || e->clip == CLIP_NONE)
        return;
    /* "Line D, thirty seven, cut on, I C eleven S, D Q, five" */
    if (e->kind == LK_DATA) {
        say(CLIP_LINE_D, 0);
        say_number(e->line);
        say(CLIP_CUT_ON, 0);
        say(e->clip, 0);
        say(CLIP_DQ, 0);
        say_number(e->dq);
        progress_wait_ms(REPORT_GAP_MS);
        return;
    }
    /* "Address line A, five, cut on, I C ten" / "Bank line B A, one, ..."
     * / "Address bit, twelve, faulty on, I C twenty one"; every chip it
     * touched, or "cut, common to all chips" */
    if (e->kind == LK_ADDR || e->kind == LK_BANK || e->kind == LK_ABIT) {
        say(e->kind == LK_ADDR ? CLIP_ADDR_LINE :
            e->kind == LK_BANK ? CLIP_BANK_LINE : CLIP_ADDR_BIT, 0);
        say_number(e->line);
        if (e->kind != LK_ABIT && e->detail == 0xFu) {
            say(CLIP_CUT_ALL, 0);
        } else {
            say(e->kind == LK_ABIT ? CLIP_FAULTY_ON : CLIP_CUT_ON, 0);
            u32 spoken = CLIP_NONE;
            for (u32 i = 0; i < 8; i++) {
                if (!(e->detail & (1u << i)) || !e->comps)
                    continue;
                u32 c = e->comps[i].clip != CLIP_NONE ? e->comps[i].clip
                                                      : CLIP_NUM_1 + i;
                if (c != spoken)
                    say(c, 0);
                spoken = c;
            }
        }
        progress_wait_ms(REPORT_GAP_MS);
        return;
    }
    /* Same rule as the screen: an entry that gives up its line while it
     * passes gives up its clip too. The operator hears what is written in
     * front of them, and a bus test announcing itself with nothing to show
     * for it is the report contradicting the screen. Serial still carries
     * every line -- it is the full log, and it has no line budget. */
    if (e->quiet_ok && e->status == T_OK)
        return;
    /* Only the cartridge content skips so far, for want of CPU RAM:
     * "Game cartridge. content not checked, main memory. defective." */
    if (e->status == T_SKIP) {
        say(e->clip, 250);
        say(CLIP_CART_UNCHK, 250);
        say(CLIP_CPU_RAM, 250);
        say(CLIP_DEFECT, 250);
        progress_wait_ms(REPORT_GAP_MS);
        return;
    }
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
        u32 c = (e->comps && i < (e->comps == pvrb_comps ? 8u : 4u)) ? e->comps[i].clip : CLIP_NUM_1 + i;
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
/* A log_entry grew past what GCC copies inline, and a struct assignment
 * would then call memmove from libgcc, which this ROM does not link. */
static void log_store(const log_entry *e)
{
    if (g_log_n >= LOG_MAX)
        return;
    const u32 *src = (const u32 *)e;
    u32 *dst = (u32 *)&g_log[g_log_n++];
    for (u32 i = 0; i < sizeof(log_entry) / 4; i++)
        dst[i] = src[i];
}

static void log_result_q(const char *name, u32 clip, t_status st, u32 detail,
                         const comp_map *comps, u32 quiet_ok, u32 pre)
{
    /* The entry is built first and shown from here, stored or not. Once the
     * log was full the new result used to be dropped while the screen and
     * the speaker went on with g_log[g_log_n - 1] -- the previous result,
     * drawn and announced again in place of the real one. */
    log_entry e;
    e.name = name;
    e.clip = clip;
    e.status = st;
    e.detail = detail;
    e.comps = comps;
    e.quiet_ok = quiet_ok;
    e.pre = pre;
    e.kind = LK_NONE;
    e.line = 0;
    e.dq = 0;
    log_store(&e);

    scif_puts(name);
    scif_puts(st == T_OK ? S_SUF_OK : st == T_SKIP ? S_SUF_SKIP : S_SUF_FAIL);
    /* live multi-channel report: one line added, then speech */
    screen_append(&e);
    say_entry(&e);
}

static void log_result(const char *name, u32 clip, t_status st, u32 detail,
                       const comp_map *comps)
{
    log_result_q(name, clip, st, detail, comps, 0, CLIP_NONE);
}

/* Screen-only lines: serial already has the same facts in its own words,
 * and there is nothing to say out loud. */
static void log_screen_line(u32 kind, const char *name, u32 detail, u32 line,
                            u32 count)
{
    log_entry e;
    e.name = name;
    e.clip = CLIP_NONE;
    e.status = T_OK;
    e.detail = detail;
    e.comps = 0;
    e.quiet_ok = 0;
    e.pre = CLIP_NONE;
    e.kind = kind;
    e.line = line;
    e.dq = count;
    log_store(&e);
    screen_append(&e);
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
    const char *last = 0;
    for (u32 i = 0; i < 8; i++) {
        if (!(compmask & (1u << i)))
            continue;
        /* One line per chip, not per position: the sound RAM is a single
         * 16-bit chip answering for all four positions, and naming IC35 four
         * times reads as four faults. */
        if (comps && i < (comps == pvrb_comps ? 8u : 4u)) {
            if (comps[i].name == last)
                continue;
            last = comps[i].name;
        }
        scif_puts("  -> ");
        scif_puts(ramname);
        scif_puts(" ");
        scif_putdec(i + 1);
        if (comps && i < (comps == pvrb_comps ? 8u : 4u)) {
            scif_puts(" (");
            scif_puts(comps[i].name);
            scif_puts(")");
        }
        scif_puts((comps == tex0_comps || comps == tex1_comps ||
                   comps == pvrb_comps || comps == elan_comps)
                  ? S_VRAM_SUSPECT : S_DEFECTIVE);
    }
}

/* What a fault in one memory region is called and how it is broken down.
 * The boot tests and the looping tests on the menu both report through
 * this, so they cannot drift apart again: they did, and the loops ended up
 * calling every VRAM and sound RAM fault "CPU RAM n", with no IC names, and
 * without folding the sound RAM's two half-words onto its single chip. */
typedef struct {
    const char *group;              /* "CPU RAM", "VRAM TEX0", ...         */
    const comp_map *comps;          /* position -> IC, or NULL             */
    u32 fold16;                     /* one 16-bit chip behind a 32-bit bus */
} region_desc;

static void report_region(const region_desc *d, const ram_result *r)
{
    if (d->fold16)
        report_badbits_aram(r->badbits);
    else
        report_badbits(r->badbits);
    u32 is_vram = r->vram_chips || d->comps == tex0_comps || d->comps == tex1_comps ||
                  d->comps == pvrb_comps || d->comps == elan_comps;
    report_comps(d->group, is_vram ? r->vram_chips : ram_comp_mask(r), d->comps);
}

/* End of one test phase on the serial log. A phase cut short by an abort
 * used to end in "ok" like any other, which is a verdict nobody reached. */
static void phase_mark(const ram_result *r)
{
    if (progress_aborted())
        scif_puts(S_PH_STOPPED);
    else
        scif_puts(r->errors ? " ERR\n" : " ok\n");
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
    scif_puts(", model override ");
    scif_putdec(CFG_BOARD_MODEL);
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
     * time. See crc32_rom_block in src/ramtest_fast.S for why this loop is
     * hand-written; it runs from the relocated block when there is one. */
    u32 nq = n >> 2, done = 0;
    while (done < nq) {
        u32 q = nq - done;
        if (q > BIOS_CRC_CHUNK / 4u)
            q = BIOS_CRC_CHUNK / 4u;
        progress_tick(done << 2);
        crc = p_crc32_rom_block(rom + (done << 2), q, crc);
        done += q;
        if (progress_aborted()) {       /* a checksum cut short is not a bad EPROM */
            progress_end();
            return;
        }
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
static u32 g_maple_safe;            /* Maple DMA buffers sit in a qualified block */

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
        /* Stopped mid-scan, a block would come back with no error counted
         * and be taken as sound. Copying code into memory nobody finished
         * testing is exactly what this scan exists to prevent: leave the
         * loops in ROM and conclude nothing about the RAM. */
        if (progress_aborted())
            return;
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
        log_result(S_L_RELOC_SCAN, CLIP_CPU_RAM, T_FAIL,
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
    /* The Maple buffers move with it. They were left in the top block, which
     * is the one that failed whenever the scan settled on a lower one. */
    maple_set_buffers(win + 0x1800u);
    g_maple_safe = 1;

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
 * arranged. Judged on the physical address, so the answer does not depend
 * on which window the ROM was linked for: the boot EPROM is area 0, CPU RAM
 * starts at 0x0C000000. Comparing against 0xA0000000, as this did, called a
 * ROM copy "in RAM" in a P1-linked build. */
static void relocate_fast_loops(void)
{
    relocate_try();
    u32 at = (u32)p_ram_prng_verify_fast;
    scif_puts("  loops execute at ");
    scif_puthex(at);
    scif_puts((at & 0x1FFFFFFFu) >= 0x0C000000u ? S_RELOC_IN_RAM : S_RELOC_IN_ROM);
}



/* ---- cut data lines on the CPU RAM ------------------------------------
 * A failing cell test says which bits failed, not why. A cut (or shorted)
 * line fails that bit on essentially every word of its parity; a bad cell
 * fails it here and there. After a CPU RAM fault each failing bit is
 * probed on 64 addresses spread over the tested region, on the word parity
 * it failed on: all 64 written, then a decoy of the opposite polarity so a
 * floating line cannot just hold the last value driven, then all read
 * back -- once with the bit at 0, once at 1. Wrong on 60 or more in either
 * polarity is a line, not a cell.
 *
 * Named in the SH-4's 64-bit numbering, which is what the board traces
 * carry: an even word is D0-D31, an odd word D32-D63. */
#define LINE_SAMPLES    64u
#define LINE_THRESHOLD  60u
#define CUT_MAX         8u

static char g_cut_name[CUT_MAX][32];         /* shared by data and address reports */
static u32  g_cut_n;

static u32 line_probe(u32 base, u32 len, u32 odd, u32 bit, u32 set)
{
    volatile u32 *p = (volatile u32 *)base;
    u32 stride = (len >> 6) & ~7u;          /* 64 samples, 8-byte steps */
    u32 m = 1u << bit;
    u32 w0 = odd ? 1u : 0u;
    for (u32 k = 0; k < LINE_SAMPLES; k++) {
        u32 v = (0x5A5AA5A5u ^ (k * 0x01010101u)) & ~m;
        p[k * (stride >> 2) + w0] = set ? v | m : v;
    }
    p[w0 + 2] = set ? 0u : 0xFFFFFFFFu;     /* decoy, opposite polarity */
    u32 wrong = 0;
    for (u32 k = 0; k < LINE_SAMPLES; k++)
        if (((p[k * (stride >> 2) + w0] >> bit) & 1u) != set)
            wrong++;
    return wrong;
}

static void report_cut_lines(u32 base, u32 len, u32 bits_even, u32 bits_odd)
{
    if (len < LINE_SAMPLES * 64u)
        return;
    for (u32 odd = 0; odd < 2; odd++) {
        u32 bits = odd ? bits_odd : bits_even;
        for (u32 bit = 0; bit < 32 && bits; bit++) {
            if (!(bits & (1u << bit)))
                continue;
            bits &= ~(1u << bit);
            u32 f0 = line_probe(base, len, odd, bit, 0);
            u32 f1 = line_probe(base, len, odd, bit, 1);
            if (f0 < LINE_THRESHOLD && f1 < LINE_THRESHOLD)
                continue;                   /* cells, not the line */
            u32 line = bit + (odd ? 32u : 0u);
            u32 comp = (odd ? 2u : 0u) + (bit >= 16 ? 1u : 0u);

            /* serial: the detail, and how the line reads */
            scif_puts(S_CUT_SERIAL);
            scif_putdec(line);
            scif_puts(" (");
            scif_puts(work_comps[comp].name);
            scif_puts(", DQ");
            scif_putdec(bit & 15u);
            scif_puts(") : ");
            scif_puts(f0 >= LINE_THRESHOLD && f1 >= LINE_THRESHOLD ? S_CUT_FLOAT :
                      f0 >= LINE_THRESHOLD ? S_CUT_HIGH : S_CUT_LOW);
            scif_puts(" (");
            scif_putdec(f0);
            scif_puts("/64, ");
            scif_putdec(f1);
            scif_puts("/64)\n");

            if (g_cut_n >= CUT_MAX)
                continue;
            /* "Line D37 cut on IC11S DQ5": the SH-4 bus line, then the
             * chip and its own pin -- a name the log can keep */
            char *d = g_cut_name[g_cut_n++];
            u32 dq = bit & 15u;
            const char *parts[6] = { S_CUT_PRE, 0, S_CUT_MID,
                                     g_ic_valid ? work_comps[comp].name : "?",
                                     " DQ", 0 };
            u32 o = 0;
            for (u32 i = 0; i < 6; i++) {
                if (!parts[i]) {
                    u32 v = i == 1 ? line : dq;
                    if (v >= 10)
                        d[o++] = (char)('0' + v / 10);
                    d[o++] = (char)('0' + v % 10);
                    continue;
                }
                for (const char *s = parts[i]; *s && o < 31; s++)
                    d[o++] = *s;
            }
            d[o] = 0;

            log_entry e;
            e.name = d;
            e.clip = g_ic_valid ? work_comps[comp].clip : CLIP_NUM_1 + comp;
            e.status = T_FAIL;
            e.detail = 0;
            e.comps = 0;
            e.quiet_ok = 0;
            e.pre = CLIP_NONE;
            e.kind = LK_DATA;
            e.line = line;
            e.dq = dq;
            log_store(&e);
            scif_puts(d);
            scif_puts(S_SUF_FAIL);
            screen_append(&e);
            say_entry(&e);
        }
    }
}


/* ---- address lines -----------------------------------------------------
 * ram_addr_alias() finds, for each CPU byte-address bit, the chips on which
 * two addresses land on one cell. On the CPU RAM that bit is then named as
 * the SDRAM pin it travels on, from the SH-4's own multiplexing table
 * (Renesas SH7750 hardware manual, appendix F, for the MCR this ROM
 * programs): table 9 for 32 MB (AMX 2, four 1M x 16 x 4-bank chips),
 * table 13 for 16 MB (AMX 4). One pin carries a column bit and a row bit,
 * so both are gathered on it:
 *
 *   CPU bit   3..10   column, SDRAM A0..A7       (SH-4 pins A3..A10)
 *   CPU bit  11..20   row,    SDRAM A0..A9       (SH-4 pins A3..A12)
 *   CPU bit  21, 22   row,    SDRAM A10, A11     (32 MB; A10 on 16 MB)
 *   CPU bit  23, 24   bank,   SDRAM BA0, BA1     (32 MB; 22, 23 on 16 MB)
 *
 * Address lines are common to the four chips: a fault on all of them is
 * the shared trace or the SH-4 ball, on one of them that chip's own pin.
 * The other memories sit behind controllers whose multiplexing is not
 * documented; for them the CPU bit is named, and the chips it touched. */
#define PIN_NONE 0xFFu

static u32 sdram_pin(u32 b)
{
    u32 big = g_ram_size > 0x01000000u;
    if (b < 3)
        return PIN_NONE;
    if (b <= 10)
        return b - 3;                   /* column */
    if (b <= (big ? 22u : 21u))
        return b - 11;                  /* row */
    if (b == (big ? 23u : 22u))
        return 12;                      /* BA0 */
    if (b == (big ? 24u : 23u))
        return 13;                      /* BA1 */
    return PIN_NONE;
}

static u32 addr_chip_sdram(u32 addr, u32 l)
{
    u32 p = (addr & 4) ? 2u : 0u;
    return ((l & 0xFFFFu) ? 1u << p : 0) | ((l >> 16) ? 2u << p : 0);
}

static u32 addr_chip_vram(u32 addr, u32 l) { return vram_chip_mask(addr, l); }
static u32 addr_chip_one(u32 addr, u32 l)  { (void)addr; (void)l; return 1u; }

/* A chip that aliases on many address bits at once is dead or its data
 * lane is -- the cell and data tests report that; it is not four cut
 * address lines. Drop it from the address report. */
static void addr_drop_dead(u32 *byline, u32 n, u32 nchips)
{
    for (u32 c = 0; c < nchips; c++) {
        u32 hits = 0;
        for (u32 i = 0; i < n; i++)
            if (byline[i] & (1u << c))
                hits++;
        if (hits > 3) {
            for (u32 i = 0; i < n; i++)
                byline[i] &= ~(1u << c);
            scif_puts(S_ADDR_DEAD);
        }
    }
}

static void put_num(char *d, u32 *o, u32 v)
{
    if (v >= 10)
        d[(*o)++] = (char)('0' + v / 10);
    d[(*o)++] = (char)('0' + v % 10);
}

static void put_str(char *d, u32 *o, const char *s)
{
    while (*s && *o < 31)
        d[(*o)++] = *s++;
}

static void addr_report(u32 kind, u32 num, u32 chips, const comp_map *comps,
                        u32 ncomps, u32 clip)
{
    if (g_cut_n >= CUT_MAX || !chips)
        return;
    char *d = g_cut_name[g_cut_n++];
    u32 o = 0;
    put_str(d, &o, kind == LK_ADDR ? S_ADDR_PRE : kind == LK_BANK ? S_BANK_PRE
                                               : S_ABIT_PRE);
    put_num(d, &o, num);
    if (kind != LK_ABIT && chips == 0xFu) {
        put_str(d, &o, S_ADDR_ALL);     /* all four: the shared trace */
    } else {
        put_str(d, &o, kind == LK_ABIT ? S_ABIT_MID : S_CUT_MID);
        u32 named = 0;
        const char *last = 0;
        u32 distinct = 0;
        for (u32 i = 0; i < ncomps; i++)
            if ((chips & (1u << i)) && comps && comps[i].name != last)
                distinct++, last = comps[i].name;
        last = 0;
        for (u32 i = 0; i < ncomps && comps; i++) {
            if (!(chips & (1u << i)) || comps[i].name == last)
                continue;
            last = comps[i].name;
            if (distinct > 2) {         /* no room: say how many */
                put_num(d, &o, distinct);
                put_str(d, &o, S_ADDR_CHIPS);
                break;
            }
            if (named++)
                put_str(d, &o, "/");
            put_str(d, &o, comps[i].name);
        }
    }
    d[o] = 0;

    log_entry e;
    e.name = d;
    e.clip = clip;
    e.status = T_FAIL;
    e.detail = chips;
    e.comps = comps;
    e.quiet_ok = 0;
    e.pre = CLIP_NONE;
    e.kind = kind;
    e.line = num;
    e.dq = 0;
    log_store(&e);
    scif_puts(d);
    scif_puts(S_SUF_FAIL);
    screen_append(&e);
    say_entry(&e);
}

/* CPU RAM: which SDRAM pins alias, and on which chips. byline[0..13] =
 * A0..A11, BA0, BA1. Returns 1 if any. */
static u32 sdram_addr_scan(u32 byline[14], u32 out[32])
{
    ram_addr_alias(SDRAM_P2_BASE, g_ram_size, 3, 2, 0, addr_chip_sdram, out);
    for (u32 i = 0; i < 14; i++)
        byline[i] = 0;
    for (u32 b = 0; b < 32; b++) {
        u32 pin = sdram_pin(b);
        if (out[b] && pin != PIN_NONE)
            byline[pin] |= out[b];
    }
    addr_drop_dead(byline, 14, 4);
    u32 any = 0;
    for (u32 i = 0; i < 14; i++)
        any |= byline[i];
    return any != 0;
}

static void sdram_addr_report(const u32 byline[14], const u32 out[32])
{
    u32 big = g_ram_size > 0x01000000u;
    for (u32 pin = 0; pin < 14; pin++) {
        if (!byline[pin])
            continue;
        /* serial: the SDRAM pin, the SH-4 pin, and the CPU bits behind it */
        scif_puts(S_ADDR_SERIAL);
        if (pin < 12) {
            scif_puts("A");
            scif_putdec(pin);
            scif_puts(" (SH-4 A");
            scif_putdec(pin + 3);
        } else {
            scif_puts("BA");
            scif_putdec(pin - 12);
            scif_puts(" (SH-4 A");
            scif_putdec(pin - 12 + (big ? 15u : 14u));
        }
        scif_puts(")");
        scif_puts(S_ADDR_BITS);
        for (u32 b = 0; b < 32; b++) {
            if (!out[b] || sdram_pin(b) != pin)
                continue;
            scif_putc(' ');
            scif_putdec(b);
            scif_puts(b <= 10 ? S_ADDR_COL : pin >= 12 ? S_ADDR_BANK : S_ADDR_ROW);
        }
        scif_puts("\n");
        addr_report(pin < 12 ? LK_ADDR : LK_BANK, pin < 12 ? pin : pin - 12,
                    byline[pin], IC(work_comps), 4, CLIP_CPU_RAM);
    }
}

/* VRAM, Elan RAM, sound RAM: the CPU bit and the chips. Returns 1 if any. */
static u32 addr_bits_report(u32 base, u32 size, u32 passes, u32 g2,
                            addr_chip_fn chip, const comp_map *comps,
                            u32 ncomps, u32 clip)
{
    u32 out[32];
    ram_addr_alias(base, size, 2, passes, g2, chip, out);
    addr_drop_dead(out, 32, ncomps);
    u32 any = 0;
    for (u32 b = 0; b < 32; b++) {
        if (!out[b])
            continue;
        any = 1;
        addr_report(LK_ABIT, b, out[b], comps, ncomps, clip);
    }
    return any;
}

static u32 test_sdram_cells(void)
{
    u32 size = g_ram_size;
    scif_puts(S_SDRAM_HDR);

    /* data bus test runs at an even word address (A2=0): comps 1/2 */
    u32 bad = ram_test_databus(SDRAM_P2_BASE);
    u32 bus_bad = bad;
    u32 dbus_bad = bad;
    u32 comps = (bad & 0xFFFF ? 1u : 0) | (bad >> 16 ? 2u : 0);
    log_result_q(S_L_SDRAM_DBUS, CLIP_DATA_BUS, bad ? T_FAIL : T_OK, comps,
                 IC(work_comps), 1, CLIP_CPU_RAM);
    if (bad) {
        report_badbits(bad);
        report_comps(S_CG_CPU, comps, IC(work_comps));
    }

    bad = ram_test_addrbus(SDRAM_P2_BASE, size);
    /* the chip-by-chip walk covers the odd word too, which the classic one
     * never touches: either finding fails the address bus */
    u32 byline[14], aout[32];
    u32 alines = sdram_addr_scan(byline, aout);
    bus_bad |= bad | alines;
    log_result_q(S_L_SDRAM_ABUS, CLIP_ADDR_BUS, (bad || alines) ? T_FAIL : T_OK,
                 0, 0, 1, CLIP_CPU_RAM);
    if (bad) {
        scif_puts(S_BAD_ABITS);
        scif_puthex(bad);
        scif_puts("\n");
    }
    if (alines)
        sdram_addr_report(byline, aout);

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
    phase_mark(&res);

    phase_begin(S_P_SDRAM, 2, S_PH_1010, words);
    ram_test_pattern(SDRAM_P2_BASE, len, 0xAAAAAAAA, &res);
    progress_end();
    phase_mark(&res);

    phase_begin(S_P_SDRAM, 3, S_PH_PRNG, words);
    ram_test_prng(SDRAM_P2_BASE, len, 0xDEADBEEF ^ 0x9E3779B9u, &res);
    progress_end();
#if CFG_RAM_CRC
    scif_puts(" crc=");
    scif_puthex(res.crc_r);
#endif
    phase_mark(&res);

    if (progress_aborted())             /* partial run: no verdict either way */
        return 0;
    t_status st = res.errors ? T_FAIL : T_OK;
    log_result(S_L_SDRAM_CELL, CLIP_CPU_RAM, st,
               ram_comp_mask(&res), IC(work_comps));
    if (res.errors || (bus_bad & 0xFFFFFFFFu)) {
        if (res.errors) {
            report_intermittent(&res);
            report_badbits(res.badbits);
            report_comps(S_CG_CPU, ram_comp_mask(&res), IC(work_comps));
            report_fails(&res);
        }
        /* the data bus test runs on an even word: its bits are even ones */
        report_cut_lines(SDRAM_P2_BASE, len, res.badbits_e | dbus_bad,
                         res.badbits_o);
        if (res.errors)
            return 0;
    }
    /* Cells that hold their values say nothing about the wires that reach
     * them: with a dead address line every pattern still reads back, just
     * from the wrong cell. The Maple stage relies on this answer for its DMA
     * descriptors, so a bus fault makes the RAM unusable whatever the cell
     * test said. */
    return bus_bad ? 0 : size;
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

    /* Belt and braces. aica_init() already left the ARM7 in reset and
     * nothing has released it since, but the cell test below overwrites
     * sound RAM offset 0 -- the ARM's reset vector -- and an ARM left
     * running would execute the test pattern. Cheap enough to state twice
     * rather than depend on a promise made three functions away.
     *
     * Note the vocabulary: halt puts the ARM7 IN reset, park is the
     * opposite -- it writes a b . loop and RELEASES it. */
    aica_arm_halt();

    /* Address bus. Safe for the same reason, and it writes offset 0 too.
     * Cheap: 8 MB is 21 address lines, so the walk is a few hundred
     * accesses, not a sweep of the chip. */
    bad = aram_test_addrbus();
    log_result_q(S_L_ARAM_ABUS, CLIP_ADDR_BUS, bad ? T_FAIL : T_OK, 0, 0, 1,
                 CLIP_SOUND_RAM);
    if (bad) {
        scif_puts(S_BAD_ABITS);
        scif_puthex(bad);
        scif_puts("\n");
        /* one chip, IC35: which CPU bits alias, through the AICA */
        addr_bits_report(ARAM_P2_BASE, ARAM_SIZE, 1, 1, addr_chip_one,
                         IC(aram_comps), 4, CLIP_SOUND_RAM);
    }

    ram_result res;
    ram_result_clear(&res);
    u32 words = len >> 2;
    phase_begin(S_P_ARAM, 1, S_PH_0101, words);
    aram_test_pattern(0, len, 0x55555555, &res);
    progress_end();
    phase_mark(&res);

    phase_begin(S_P_ARAM, 2, S_PH_1010, words);
    aram_test_pattern(0, len, 0xAAAAAAAA, &res);
    progress_end();
    phase_mark(&res);

    phase_begin(S_P_ARAM, 3, S_PH_PRNG, words);
    aram_test_prng(0, len, 0xC0FFEE42 ^ 0x9E3779B9u, &res);
    progress_end();
#if CFG_RAM_CRC
    scif_puts(" crc=");
    scif_puthex(res.crc_r);
#endif
    phase_mark(&res);

    if (progress_aborted())             /* partial run: no verdict either way */
        return 0;

    /* a G2 bus that never drains aborts the write loops: the cell results
     * are meaningless then, and the bus is the actual fault to report */
    if (aica_g2_stalled()) {
        scif_puts(S_G2_STALL);
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
static u32 vram_diag_lines;
static u32 vram_diag_cpu_bits, vram_diag_data_bits;
static void report_vram_ic(u32 addr, u32 diff)
{
    if (!g_ic_valid || !vram_is_mapped(addr) || !diff) return;
    scif_puts(S_VRAM_BIOS_IC);
    for (u32 half = 0; half < 2; half++) {
        u32 bits = (diff >> (half * 16)) & 0xFFFFu;
        if (!bits) continue;
        scif_puts(" IC"); scif_putdec(vram_ic(addr, half * 16));
        scif_puts(" CPU D:");
        for (u32 bit = 0; bit < 16; bit++)
            if (bits & (1u << bit)) { scif_putc(' '); scif_putdec(half * 16 + bit); }
    }
    scif_puts("\n");
}
static void report_vram_address(const vram_address_event *e)
{
    /* Serial only: a diagnostic callback must never modify either GPU RAM. */
    if (e->alias) vram_diag_cpu_bits |= e->cpu_bits;
    else vram_diag_data_bits |= (e->expected ^ e->observed) |
                               (e->expected ^ e->reread) |
                               (e->peer_expected ^ e->peer_observed);
    if (vram_diag_lines++ >= 32) return;
    scif_puts(e->alias ? "  VRAM coupling candidate" : "  VRAM readback failure");
    scif_puts(" addr="); scif_puthex(e->first);
    scif_puts(" peer="); scif_puthex(e->second);
    scif_puts(" expected="); scif_puthex(e->expected);
    scif_puts(" observed="); scif_puthex(e->observed);
    scif_puts(" reread="); scif_puthex(e->reread);
    scif_puts(" peer_expected="); scif_puthex(e->peer_expected);
    scif_puts(" peer_observed="); scif_puthex(e->peer_observed);
    scif_puts(" xor="); scif_puthex(e->expected ^ e->observed);
    if (e->alias) {
        scif_puts(" CPU byte-address bits:");
        for (u32 bit = 2; bit < 32; bit++)
            if (e->cpu_bits & (1u << bit)) {
                scif_puts(" A"); scif_putdec(bit);
            }
    }
    scif_puts("\n");
    if (e->first == e->second)
        report_vram_ic(e->first, (e->expected ^ e->observed) |
                                (e->expected ^ e->reread));
}

static void diagnose_vram(u32 base, u32 size)
{
    vram_diag_lines = 0;
    vram_diag_cpu_bits = vram_diag_data_bits = 0;
    scif_puts(S_VRAM_DIAG);
    u32 failed = vram_address_diagnose(base, size, report_vram_address);
    scif_puts("  VRAM diagnostic failed pairs="); scif_putdec(failed);
    scif_puts(" events="); scif_putdec(vram_diag_lines);
    scif_puts(" (first 32 events printed)\n");
    scif_puts("  VRAM coupling CPU mask="); scif_puthex(vram_diag_cpu_bits);
    scif_puts(" readback data mask="); scif_puthex(vram_diag_data_bits);
    scif_puts("\n");
}

static void test_vram_region_body(const char *name, const char *dbus, const char *abus,
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
    if (vram_is_mapped(base)) c = vram_chip_mask(base, bad);
    log_result_q(dbus, CLIP_DATA_BUS, bad ? T_FAIL : T_OK, c, comps, 1, name_clip);
    if (bad) {
        report_badbits(bad);
        report_comps(name, c, comps);
        *ok_flag = 0;
        diagnose_vram(base, size);
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
        /* which CPU bits alias, and on which chips; the Elan RAM's chips
         * also depend on word parity, so it is walked on both */
        addr_bits_report(base, size, elan_is_mapped(base) ? 2u : 1u, 0,
                         addr_chip_vram, comps, comps == pvrb_comps ? 8u : 4u,
                         name_clip);
        *ok_flag = 0;
        diagnose_vram(base, size);
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
    u32 fbaddr = pvr_fb_addr();
    progress_screen_enable(base != VRAM_PVRB_BASE &&
                           !(base <= fbaddr && fbaddr < base + len));

    u32 words = len >> 2;
    phase_begin(S_P_VRAM, 1, S_PH_0101, words);
    vram_test_pattern(base, len, 0x55555555, &res);
    progress_end();
    phase_mark(&res);

    phase_begin(S_P_VRAM, 2, S_PH_1010, words);
    vram_test_pattern(base, len, 0xAAAAAAAA, &res);
    progress_end();
    phase_mark(&res);

    phase_begin(S_P_VRAM, 3, S_PH_PRNG, words);
    vram_test_prng(base, len, 0x7E0CBEEF ^ 0x9E3779B9u ^ base, &res);
    progress_end();
#if CFG_RAM_CRC
    scif_puts(" crc=");
    scif_puthex(res.crc_r);
#endif
    phase_mark(&res);

    /* this test scribbles over the framebuffer: repaint once it is done so
     * the screen is readable again */
    screen_render();
    progress_screen_enable(1);

    if (progress_aborted()) {           /* partial run: no verdict either way */
        *ok_flag = 0;
        return;
    }
    t_status st = res.errors ? T_FAIL : T_OK;
    u32 chips = vram_is_mapped(base) ? res.vram_chips : ram_comp_mask(&res);
    log_result(name, name_clip, st, chips, comps);
    if (res.errors) {
        if (res.unpinned && vram_is_mapped(base)) scif_puts(S_VRAM_UNLOCATED);
        else report_intermittent(&res);
        report_badbits(res.badbits);
        report_comps(name, chips, comps);
        report_fails(&res);
        *ok_flag = 0;
    }
}

/* One region, one block on the serial log: its bus tests, passes, verdict
 * and fault detail together, then a blank line before the next block --
 * whichever way the region ended. */
static void test_vram_region(const char *name, const char *dbus, const char *abus,
                             u32 base, u32 size,
                             const comp_map *comps, u32 name_clip,
                             u32 *ok_flag)
{
    test_vram_region_body(name, dbus, abus, base, size, comps, name_clip,
                          ok_flag);
    scif_puts("\n");
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
    /* the screen is usable if the bank holding the framebuffer is */
    return pvr_fb_on_tex1() ? tex1_ok : tex0_ok;
}

/* Naomi 2 only: the slave PVR's 16MB VRAM (32-bit path) and the Elan
 * T&L chip's 32MB RAM. IC designators for these are not known yet ->
 * numbered positions. */
static void test_naomi2_ram(void)
{
    if (g_board_type != BOARD_NAOMI2) {
        /* Explicitly report why these regions were not tested. */
        scif_puts(S_N2_SKIP);
        return;
    }
    scif_puts(S_N2_HDR);
    u32 ok = 1;
    pvr2_access access = pvr2_prepare();
    if (access != PVR2_READY) {
        log_result(access == PVR2_A_REFERENCE ? S_L_PVRA_REFERENCE : S_L_PVRB_ACCESS,
                   CLIP_NONE, T_FAIL, 0, 0);
        scif_puts(S_PVRB_SKIP);
        scif_putdec((u32)access);
        scif_puts("\n");
        if (access == PVR2_B_SILENT)
            scif_puts(S_PVRB_SILENT);
        if (access == PVR2_MAPPING || access == PVR2_A_REFERENCE ||
            access == PVR2_B_SILENT) {
            u32 count;
            const pvr2_probe_sample *samples = pvr2_probe_samples(&count);
            /* Print captured reads after restoration, without rerunning MMIO.
             * Four samples are the cells written alone (the probe stopped
             * there); eight are the two passes across all four windows. */
            for (u32 i = 0; i < count; i++) {
                if (count == 4) {
                    scif_puts("  PVR probe cell alone");
                } else {
                    scif_puts("  PVR probe pass=");
                    scif_putdec(i / 4);
                }
                scif_puts(" addr=");
                scif_puthex(samples[i].addr);
                scif_puts(" expected=");
                scif_puthex(samples[i].expected);
                scif_puts(" observed=");
                scif_puthex(samples[i].observed);
                scif_puts(" xor=");
                scif_puthex(samples[i].expected ^ samples[i].observed);
                if (samples[i].unreliable) {
                    scif_puts(" alone=");
                    scif_puthex(samples[i].unreliable);
                }
                scif_puts("\n");
                report_vram_ic(samples[i].addr, samples[i].expected ^ samples[i].observed);
            }
            if (access == PVR2_B_SILENT) {
                /* B writes were never shown to leave A alone: no more of them */
            } else if (access == PVR2_A_REFERENCE) {
                /* Diagnose the failing A half without assigning its fault to B. */
                for (u32 i = 0; i < count; i++)
                    if (samples[i].expected != samples[i].observed) {
                        diagnose_vram(samples[i].addr & ~0x007FFFFFu, 0x00800000u);
                        break;
                    }
            } else {
                diagnose_vram(VRAM_PVRB_BASE, VRAM_PVRB_SIZE);
            }
            screen_render();
        }
    } else {
        /* No framebuffer writes while exercising B, even after the sampled
         * independence check. An untested alias must not corrupt our
         * patterns. */
        u32 screen_ready = g_screen_ready;
        g_screen_ready = 0;
        test_vram_region(S_L_VRAM_B, S_L_VRAM_B_DBUS, S_L_VRAM_B_ABUS,
                         VRAM_PVRB_BASE, VRAM_PVRB_SIZE,
                         IC(pvrb_comps), CLIP_VRAM_B, &ok);
        g_screen_ready = screen_ready;
        screen_render();
    }

    scif_puts("\n");                  /* PVR-B's block ends here, however it went */

    /* The Elan RAM is the Elan's own memory, not a window onto either GPU's:
     * testing it writes no VRAM, so a doubt about PVR-B's windows is no
     * reason to leave it out. It needs the Elan itself, and the interface
     * control that routes its channel. */
    if (access == PVR2_NO_ELAN || access == PVR2_CONTROL) {
        scif_puts(S_ELAN_SKIP);
        return;
    }
    test_vram_region(S_L_ELAN, S_L_ELAN_DBUS, S_L_ELAN_ABUS,
                     ELAN_RAM_BASE, ELAN_RAM_SIZE,
                     IC(elan_comps), CLIP_ELAN, &ok);
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

    rtc_result rr;
    rtc_check(&rr);
    log_result(S_L_RTC, CLIP_RTC, rr.verdict ? T_FAIL : T_OK, 0, 0);
    /* both reads of each measurement, so a failure says what happened */
    for (u32 i = 0; i < rr.n; i += 2) {
        u32 d = rr.t[i + 1] - rr.t[i];
        scif_puts(S_RTC_READS);
        scif_putdec(rr.t[i]);
        scif_puts(" -> ");
        scif_putdec(rr.t[i + 1]);
        scif_puts(" (");
        if (d & 0x80000000u) {
            scif_puts("-");
            scif_putdec(0u - d);
        } else {
            scif_puts("+");
            scif_putdec(d);
        }
        scif_puts(" s)\n");
    }
    /* A raw counter tells the operator nothing; the date tells them when
     * this board was last on, and whether its battery still holds. */
    u32 rtcval = rr.t[rr.n - 1];
    rtc_fmt(rtcval, g_rtc_str);
    scif_puts(S_RTC_DATE);
    scif_puts(g_rtc_str);
    scif_puts(rr.verdict == RTC_OK ? S_RTC_TICK :
              rr.verdict == RTC_STUCK ? S_RTC_STUCK :
              rr.verdict == RTC_IRREGULAR ? S_RTC_IRREG : S_RTC_JUMP);
    screen_draw_date();

    /* Whether it ticks and whether it holds the right date are separate
     * questions. A date before 2026 cannot be today's: the battery went
     * flat at some point, or the clock was never set. Serial only while
     * it is fine; on screen and in speech when it is not. */
    u32 date_ok = rtc_year(rtcval) >= 2026u;
    log_result_q(S_L_RTC_DATE, CLIP_RTC_DATE, date_ok ? T_OK : T_FAIL,
                 0, 0, 1, CLIP_NONE);
    if (!date_ok)
        scif_puts(S_RTC_DATE_BAD);
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
static u32 udiv(u32 n, u32 d);

/* The probe's findings: every figure on serial, for the record of what a
 * real board does; the two directions and the speed on screen. */
static char g_dimmbuf[3][40];

static void dimm_dir_line(u32 slot, const char *label, u32 dir)
{
    u32 n = 0;
    for (const char *t = label; *t; t++)
        g_dimmbuf[slot][n++] = *t;
    for (const char *t = " GDDIR="; *t; t++)
        g_dimmbuf[slot][n++] = *t;
    g_dimmbuf[slot][n++] = (char)('0' + dir);
    g_dimmbuf[slot][n] = 0;
    log_result(g_dimmbuf[slot], CLIP_NONE, T_OK, 0, 0);
}

static void dimm_probe_report(const dimm_g1_probe_result *r)
{
    static const char *const what[6] = {
        S_DP_NOTRUN, S_DP_READ, S_DP_WRITE, S_DP_NONE, S_DP_TIMEOUT, S_DP_ODD
    };
    scif_puts(S_DP_HDR);
    if (!r->pio_flags) {
        scif_puts(S_DP_NOPIO);
        log_result(S_L_DP_PIO, CLIP_NONE, T_FAIL, 0, 0);
        return;
    }
    scif_puts(S_DP_PIO);
    scif_puthex(r->pio_flags);
    scif_puts(S_DP_AT);
    scif_puthex(r->addr);
    scif_puts("\n");
    for (u32 dir = 0; dir < 2; dir++) {
        scif_puts(S_DP_DIR);
        scif_putdec(dir);
        scif_puts(" : ");
        scif_puts(what[r->outcome[dir] < 6 ? r->outcome[dir] : 0]);
        scif_puts("\n");
    }
    if (r->restored)
        scif_puts(S_DP_RESTORED);
    if (r->restore_failed)
        scif_puts(S_DP_RESTORE_KO);
    if (r->dir_read != DP_UNKNOWN) {
        scif_puts(S_DP_COUNT);
        scif_puts(r->cnt_ok[0] ? "OK" : "--");
        scif_puts(" / ");
        scif_puts(r->cnt_ok[1] ? "OK" : "--");
        scif_puts("\n");
    }
    if (r->ticks_32k) {
        /* 32 KB in t ticks of 12.5 MHz: KB/s = 32 * 12500000 / t */
        scif_puts(S_DP_SPEED);
        scif_putdec(udiv(400000000u, r->ticks_32k));
        scif_puts(S_DP_KBS);
    }
    if (r->dir_read != DP_UNKNOWN)
        dimm_dir_line(0, S_L_DP_READ, r->dir_read);
    else
        log_result(S_L_DP_READ, CLIP_NONE, T_FAIL, 0, 0);
    if (r->dir_write != DP_UNKNOWN)
        dimm_dir_line(1, S_L_DP_WRITE, r->dir_write);
    else
        log_result(S_L_DP_WRITE, CLIP_NONE,
                   r->dir_read != DP_UNKNOWN ? T_SKIP : T_FAIL, 0, 0);
    if (r->restore_failed)
        log_result(S_L_DP_RESTORE, CLIP_NONE, T_FAIL, 0, 0);
}

/* SERVICE/START runs it, TEST (or any key but y/o) passes */
static u32 dimm_confirm_destructive(void)
{
    scif_puts(S_DIMM_ASK);
    u32 y = g_screen_y + 12;
    if (g_screen_ready) {
        fb_text(16, y, S_SCR_DIMM_ASK, COL_AMBER, FB_W);
        fb_text(16, fb_hint_y(), S_SCR_DIMM_ASK_HINT, COL_HINT, FB_W);
    }
    u32 go = 0;
    for (;;) {
        input_event ev;
        if (input_poll(&ev)) {
            if (ev.kind == INPUT_KEY) {
                go = ev.key == 'y' || ev.key == 'Y' || ev.key == 'o' ||
                     ev.key == 'O';
                break;
            }
            if (ev.kind == INPUT_CONFIRM) {
                go = 1;
                break;
            }
            if (ev.kind == INPUT_SELECT)
                break;
        }
        progress_heartbeat();
        delay_ms(20);
    }
    if (g_screen_ready)
        fb_fill_rows(y, fb_hint_y() + 18, 0);
    return go;
}

static void test_dimm(void)
{
    dimm_info di;
    g1_bus_init();
    dimm_probe(&di);

    /* A real Naomi 2 with a DIMM the original BIOS sees read all ones here,
     * mailbox and cartridge space alike, with the 0x1006 cycle. The BIOSes
     * pick 0x1006 or 0x1106 from a board-dependent test we do not have:
     * try the other branch (and the slower setup values both use first)
     * before calling the board absent, and say which one answered. */
    if (!di.present) {
        static const u16 tim[3] = { 0x1106, 0x1009, 0x1109 };
        for (u32 i = 0; i < 3 && !di.present; i++) {
            g1_set_board_timing(tim[i]);
            dimm_probe(&di);
            scif_puts(S_DIMM_TIMING);
            scif_puthex(tim[i]);
            scif_puts(di.present ? S_DIMM_TIM_YES : S_DIMM_TIM_NO);
        }
        if (!di.present)
            g1_set_board_timing(0x1006);
    }

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
    scif_puts(S_DIMM_SIG);
    scif_puthex(di.signature);
    scif_puts(di.latch ? S_DIMM_LATCH_OK : S_DIMM_LATCH_KO);

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

    /* What the G1 DMA does with this board, found on it: which SB_GDDIR
     * value reads and which writes, the count's unit, the speed. Leaves
     * the DIMM as it was. The cell test below needs both directions. */
    dimm_g1_probe_result pr;
    dimm_g1_probe(&pr);
    dimm_probe_report(&pr);
    if (pr.dir_read == DP_UNKNOWN || pr.dir_write == DP_UNKNOWN) {
        scif_puts(S_DIMM_NO_DIRS);
        log_result(S_L_DIMM_MEM_PAT, CLIP_NONE, T_SKIP, 0, 0);
        return;
    }
    /* It overwrites the game image: asked, not assumed. */
    if (!dimm_confirm_destructive()) {
        scif_puts(S_DIMM_SKIPPED);
        log_result(S_L_DIMM_MEM_PAT, CLIP_NONE, T_SKIP, 0, 0);
        return;
    }

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
    u32 mem_fail = 0, mem_ran = 0, aborted = 0;

    scif_puts(S_DIMM_MEM_HDR);
    for (u32 p = 0; p < 2; p++) {
        dimm_mem_result mr;
        progress_begin(S_P_DIMM, mspan);
        dimm_mem_test(mspan, patterns[p], &mr, pr.dir_write, pr.dir_read);
        progress_end();

        /* An operator abort stops dimm_mem_test between blocks, and what it
         * leaves behind looks like other things: no block at all reads as
         * "scratch RAM unusable", a partial run as a clean pass. Neither is
         * true, so say what happened and draw no conclusion from it. */
        if (progress_aborted()) {
            scif_puts(S_DIMM_MEM_ABORT);
            aborted = 1;
            if (mr.errors)              /* a fault found before the abort stands */
                mem_fail = 1;
            break;
        }

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
    if (mem_fail || (mem_ran && !aborted))
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
static const char *const g_fw_label[3] = { S_FW_V317, S_FW_V401, S_FW_V403 };
static char g_fwbuf[40], g_fwbuf2[40];  /* report lines, kept for repaints */
static void hex_into(char *d, u32 v, u32 ndigits);

/* JEDEC manufacturer, from the low byte of the read-ID answer (the DIMM's
 * flash is 16 bits wide and answers the byte code on both halves) */
static const char *flash_maker(u32 mfr)
{
    switch (mfr & 0xFF) {
    case 0x01: return "AMD/Spansion";
    case 0x04: return "Fujitsu";
    case 0x20: return "ST";
    case 0x89: return "Intel";
    case 0xC2: return "Macronix";
    case 0xDA: return "Winbond";
    default:   return 0;
    }
}

/* The version chooser, under the report lines: the three versions and
 * "cancel", TEST to move, SERVICE or START to pick, as in the menu. */
#define FW_CHOICES 4
static void fw_choice_draw(u32 y0, u32 sel)
{
    if (!g_screen_ready)
        return;
    fb_fill_rows(y0, fb_hint_y() + 18, 0);
    for (u32 i = 0; i < FW_CHOICES; i++) {
        u32 y = y0 + i * 22;
        const char *t = i < 3 ? g_fw_label[i] : S_FW_CANCEL;
        fb_text(16, y, i == sel ? ">" : " ", COL_TITLE, FB_W);
        fb_text(48, y, t, i == sel ? COL_TITLE : COL_WHITE, FB_W);
    }
    fb_text(16, fb_hint_y(), S_MENU_HINT, COL_HINT, FB_W);
}

static void test_dimm_flash(void)
{
    dimm_info di;
    g1_bus_init();
    dimm_probe(&di);
    scif_puts(S_FW_HDR);
    if (!di.present) {
        log_result(S_L_DIMM_ABSENT, CLIP_NONE, T_OK, 0, 0);
        return;
    }
    scif_puts(S_FW_IDING);
    dimm_flash_id fi;
    dimm_flash_identify(&fi);
    if (!fi.id_ok) {
        scif_puts(S_FW_NO_DIMM);
        log_result(S_L_FW_NOID, CLIP_NONE, T_FAIL, 0, 0);
        return;
    }
    scif_puts(S_FW_ID);
    scif_puthex(fi.mfr);
    scif_putc('/');
    scif_puthex(fi.dev);
    scif_puts("\n");
    {
        /* "Flash DIMM : Fujitsu 0004/22C4" */
        u32 n = 0;
        for (const char *t = S_SCR_FW_ID; *t; t++)
            g_fwbuf[n++] = *t;
        const char *mk = flash_maker(fi.mfr);
        if (mk) {
            while (*mk)
                g_fwbuf[n++] = *mk++;
            g_fwbuf[n++] = ' ';
        }
        hex_into(g_fwbuf + n, fi.mfr, 4);
        n += 4;
        g_fwbuf[n++] = '/';
        hex_into(g_fwbuf + n, fi.dev, 4);
        n += 4;
        g_fwbuf[n] = 0;
        log_screen_line(LK_INFO, g_fwbuf, 0, 0, 0);
    }

    /* the choice: board or cabinet buttons on screen, 1/2/3 on the console
     * (any other key cancels, as before) */
    scif_puts(S_FW_MENU);
    u32 y0 = g_screen_y + 12;
    u32 sel = 0;
    int pick = -1;
    fw_choice_draw(y0, sel);
    for (;;) {
        input_event ev;
        if (input_poll(&ev)) {
            if (ev.kind == INPUT_KEY) {
                if (ev.key >= '1' && ev.key <= '3')
                    pick = ev.key - '1';
                break;
            }
            if (ev.kind == INPUT_SELECT) {
                sel = (sel + 1) % FW_CHOICES;
                fw_choice_draw(y0, sel);
            } else if (ev.kind == INPUT_CONFIRM) {
                if (sel < 3)
                    pick = (int)sel;
                break;
            }
        }
        progress_heartbeat();
        delay_ms(20);
    }
    if (g_screen_ready)
        fb_fill_rows(y0, fb_hint_y() + 18, 0);
    if (pick < 0) {
        scif_puts(S_FW_ABORT);
        log_screen_line(LK_INFO, S_SCR_FW_CANCEL, 0, 0, 0);
        return;
    }
    scif_puts(S_FW_SEL);
    scif_puts(g_fw_ver[pick]);
    scif_puts("\n");
    scif_puts(S_FW_PENDING);
    {
        u32 n = 0;
        for (const char *t = S_SCR_FW_SEL; *t; t++)
            g_fwbuf2[n++] = *t;
        for (const char *t = g_fw_ver[pick]; *t; t++)
            g_fwbuf2[n++] = *t;
        g_fwbuf2[n] = 0;
        log_screen_line(LK_INFO, g_fwbuf2, 0, 0, 0);
    }
    /* amber and silent: nothing failed, nothing was written */
    log_result(S_L_FW_WRITE, CLIP_NONE, T_SKIP, 0, 0);
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
    scif_puts("MIE on maple port ");
    scif_putdec(mr.found_port);
    scif_puts(", resp cmd 0x");
    scif_puthex(mr.response_cmd);
    scif_puts("\nMIE version: \"");
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
/* The MIE input port, spelled out. SW1:1 selects the monitor frequency and
 * is the only one of the four whose meaning is documented; 2 to 4 are
 * reported as positions rather than given an invented meaning. All the bits
 * are active LOW, switches and buttons alike, so a closed switch reads 0.
 *
 * Serial only: it is context, not a verdict, and the CRT has no row to
 * spare for something that never fails. The raw byte is printed after it
 * because that is what you want when the decoding looks wrong. */
static void report_mie_inputs(u8 in5)
{
    scif_puts(S_DIP_HDR);
    scif_puts((in5 & 0x01u) ? S_DIP_31K : S_DIP_15K);
    for (u32 i = 1; i < 4; i++) {
        scif_puts("  ");
        scif_putdec(i + 1);
        scif_puts((in5 & (1u << i)) ? S_DIP_OFF : S_DIP_ON);
    }
    scif_puts(S_DIP_RAW);
    scif_puthex(in5);
    scif_puts(")\n");

    scif_puts(S_BTN_HDR);
    scif_puts((in5 & 0x10u) ? S_BTN_UP : S_BTN_DOWN);
    scif_puts(S_BTN_SVC);
    scif_puts((in5 & 0x20u) ? S_BTN_UP : S_BTN_DOWN);
    scif_puts("\n");
}

static void test_settings_eeprom(void)
{
    if (!g_mie_port1) {
        scif_puts(S_EEPROM_SKIP);
        return;
    }
    /* The MIE's factory firmware cannot read this EEPROM, and cannot read
     * the push buttons either -- it answers four Maple commands and none of
     * them is either. So a small Z80 program goes in first, and it stays
     * resident: from here on it is what serves the operator menu. */
    u32 bad = maple_mie_upload(g_mie_port1 - 1);
    if (bad) {
        scif_puts(S_MIE_UP_FAIL);
        scif_putdec(bad);
        scif_puts("\n");
        log_result(S_L_EEPROM_MIE, CLIP_EEPROM, T_FAIL, 0, 0);
        return;
    }
    scif_puts(S_MIE_UP_OK);

    /* Ask the program something before trusting it. Matching checksums say
     * the bytes arrived, not that the code runs: a MIE that took the upload
     * and then did nothing would leave the console polling a program that
     * never answers, four times a second, each poll spinning out a Maple
     * timeout. So the button path is armed by a reply, not by an upload.
     *
     * The DIP switches and the two front buttons sit on the MIE port the
     * EEPROM's data line shares, so this probe is also the report. */
    {
        /* Asked more than once. The program reads the whole EEPROM before it
         * starts answering -- about 13 ms at 16 MHz by its instruction
         * count, against the 20 ms the upload waits -- and a question that
         * arrives during that read goes unanswered. One try would turn a
         * slow MIE into "does not answer" and switch the buttons off for the
         * whole session. */
        u8 in5;
        u32 up = 0;
        for (u32 t = 0; t < 10 && !up; t++) {
            if (maple_mie_inputs(g_mie_port1 - 1, &in5) == 0)
                up = 1;
            else
                delay_ms(10);
        }
        if (up) {
            g_mie_prog = 1;
            hint_draw();                /* the buttons answer: say so */
            report_mie_inputs(in5);
            keys_box();                 /* and on the console */
        } else {
            scif_puts(S_MIE_NO_ANSWER);
        }
    }

    u8 ee[128];
    if (maple_eeprom_read(g_mie_port1 - 1, ee)) {
        scif_puts(S_EEPROM_MIE_NOTE);
        log_result(S_L_EEPROM_MIE, CLIP_EEPROM, T_FAIL, 0, 0);
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

/* Cartridge bytes hashed per call of the assembly rounds: small enough to
 * keep the progress bar moving, large enough that the call costs nothing. */
#define CART_HASH_CHUNK 0x2000u

/* Reading the cartridge through the PIO port costs two G1 bus cycles per
 * 32-bit word, about as long as the rounds that hash it. The G1 DMA fetches
 * the next chunk into one buffer while the rounds hash the previous one out
 * of the other, the way the DIMM test and the BIOS itself load data, so the
 * bus and the CPU work at the same time. Two 8 KB buffers in main RAM, 4 MB
 * in (the DIMM test's scratch, never in use at the same time), read by the
 * rounds through P2 so they see what the DMA wrote. */
#define CART_DMA_PHYS   0x0C400000u
#define CART_DMA_BUF(i) ((volatile u32 *)(0xA0000000u | \
                         (CART_DMA_PHYS + (i) * CART_HASH_CHUNK)))
static u32 g_cart_dma;          /* the DMA path was proven on this cartridge */
static u32 g_cart_t_dma, g_cart_t_pio;  /* one chunk each way, timer ticks */

static void cart_hash_pio(sha1_ctx *c, u32 off, u32 len, u32 pbase)
{
    cart_seek(off);
    for (u32 done = 0; done < len; done += CART_HASH_CHUNK) {
        progress_tick(pbase + done);
        if (((pbase + done) & 0x3FFFFF) == 0)
            scif_putc('.');
        sha1_update_cart(c, CART_HASH_CHUNK);
    }
}

/* Hash [off, off + len) into c, progress counted from pbase. */
static void cart_hash_range(sha1_ctx *c, u32 off, u32 len, u32 pbase)
{
    if (!g_cart_dma) {
        cart_hash_pio(c, off, len, pbase);
        return;
    }
    u32 cur = 0, done = 0;
    if (cart_dma_start(off, CART_DMA_PHYS, CART_HASH_CHUNK))
        goto lost;
    for (; done < len; done += CART_HASH_CHUNK) {
        progress_tick(pbase + done);
        if (((pbase + done) & 0x3FFFFF) == 0)
            scif_putc('.');
        if (cart_dma_wait())
            goto lost;
        if (done + CART_HASH_CHUNK < len)
            cart_dma_start(off + done + CART_HASH_CHUNK,
                           CART_DMA_PHYS + (cur ^ 1) * CART_HASH_CHUNK,
                           CART_HASH_CHUNK);
        sha1_update_buf(c, (const void *)CART_DMA_BUF(cur), CART_HASH_CHUNK);
        cur ^= 1;
    }
    return;
lost:
    /* a transfer never completed: finish this range, and the rest of the
     * cartridge, through the port -- slower, same digests */
    scif_puts(S_CART_DMA_LOST);
    g_cart_dma = 0;
    cart_hash_pio(c, off + done, len - done, pbase + done);
}

/* Before trusting the DMA path with a verdict: both buffers must hold what
 * is written to them, and 8 KB fetched by DMA must equal the same 8 KB read
 * through the port. Anything else -- a board that does not answer the DMA,
 * a buffer cell that fails -- and the check runs on the port alone. */
static u32 cart_dma_probe(u32 base)
{
    volatile u32 *b = CART_DMA_BUF(0);
    const u32 nw = 2 * CART_HASH_CHUNK / 4;
    for (u32 pass = 0; pass < 2; pass++) {
        u32 x = pass ? 0xFFFFFFFFu : 0;
        for (u32 i = 0; i < nw; i++)
            b[i] = (i * 0x9E3779B1u) ^ x;
        for (u32 i = 0; i < nw; i++)
            if (b[i] != ((i * 0x9E3779B1u) ^ x))
                return 0;
    }
    for (u32 i = 0; i < CART_HASH_CHUNK / 4; i++)
        b[i] = 0x5A5A5A5Au;
    u32 t0 = timer_ticks();
    if (cart_dma_start(base, CART_DMA_PHYS, CART_HASH_CHUNK) ||
        cart_dma_wait())
        return 0;
    g_cart_t_dma = timer_ticks() - t0;
    /* the port read is timed on its own, compared afterwards: the two
     * figures go in the log, for the real speed of both paths */
    u32 *pio = (u32 *)CART_DMA_BUF(1);
    t0 = timer_ticks();
    cart_seek(base);
    for (u32 i = 0; i < CART_HASH_CHUNK / 4; i++) {
        u32 lo = CART_ROM_DATA;
        pio[i] = lo | ((u32)CART_ROM_DATA << 16);
    }
    g_cart_t_pio = timer_ticks() - t0;
    for (u32 i = 0; i < CART_HASH_CHUNK / 4; i++)
        if (b[i] != pio[i])
            return 0;
    return 1;
}

static u32 sha1_ic(u32 offset, u32 size, u8 out[20])
{
    sha1_ctx c;
    sha1_init(&c);
    cart_hash_range(&c, offset, size, 0);
    progress_end();
    sha1_final(&c, out);
    return 0;
}

/* The chips of the game on screen, four to a row, each row drawn as soon as
 * it is full: the operator watches the verdicts come in. The chips of one
 * game are consecutive in the database, so a row is its first index, its
 * count and one 4-bit IC_* code per cell. */
static u32 g_icrow_base, g_icrow_n, g_icrow_det;

static void icrow_flush(void)
{
    if (g_icrow_n)
        log_screen_line(LK_ICROW, "", g_icrow_det, g_icrow_base, g_icrow_n);
    g_icrow_n = 0;
    g_icrow_det = 0;
}

static void icrow_add(u32 idx, u32 st)
{
    if (!g_icrow_n)
        g_icrow_base = idx;
    g_icrow_det |= st << (4 * g_icrow_n);
    if (++g_icrow_n == 4)
        icrow_flush();
}

/* "Jeu : <title>", kept for the repaints */
static char g_gamebuf[48];

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

/* base: the first chip's offset, so the sample is real data and not the
 * 0xFF of an empty socket 2F on a Namco board */
static void test_cart_pins(u32 base)
{
    cart_pin_stats st;
#if QUICK_TEST
    const u32 span = 0x00010000;        /* 64 KB sample */
#else
    const u32 span = 0x00100000;        /* 1 MB sample */
#endif
    progress_begin(S_P_CART_PINS, span);
    cart_pin_scan(base, span, &st);
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

/* Stream the first chip and snapshot the SHA-1 at every size a first chip
 * has in the database: the game, or 0xFFFFFFFF. */
static u32 cart_identify(u32 base)
{
    sha1_ctx c;
    u8 digest[20];
    u32 game = 0xFFFFFFFF;
    sha1_init(&c);
    u32 done = 0;
    scif_puts(S_IDENTIFYING);
    progress_begin(S_P_CART_ID, cartdb_first_sizes[CARTDB_NFIRST - 1]);
    for (u32 s = 0; s < CARTDB_NFIRST && game == 0xFFFFFFFF; s++) {
        u32 target = cartdb_first_sizes[s];
        cart_hash_range(&c, base + done, target - done, done);
        done = target;
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
            if (cartdb_ic_off[f] == base && cartdb_ic_size[f] == target &&
                sha1_eq(digest, cartdb_sha1[f])) {
                game = g;
                break;
            }
        }
    }
    progress_end();
    scif_puts("\n");
    return game;
}

static void test_cartridge(void)
{
    g1_bus_init();
    u32 cbase = cart_base();
    if (cbase == CART_NONE) {
        log_result(S_L_CART_ABSENT, CLIP_NONE, T_OK, 0, 0);
        say(CLIP_CART, 250);
        say(CLIP_ABSENT, REPORT_GAP_MS);
        return;
    }

    /* header peek: "NAOMI" magic + ASCII titles live in the first bytes */
    u8 hdr[80];
    cart_read(cbase, hdr, sizeof hdr);
    scif_puts(S_CART_HDR);
    for (u32 i = 0; i < 64; i++)
        scif_putc((hdr[i] >= 32 && hdr[i] < 127) ? (char)hdr[i] : '.');
    scif_puts("\"\n");

    /* identify: stream first IC, snapshot SHA1 at each known size */
    /* Identifying the game and checking its content means hashing the whole
     * cartridge: 132 MB for the median game in the database, 512 MB at most.
     * The rounds cost about 31 instructions per byte; from the EPROM, at the
     * 3.5 MIPS this ROM executes there, that is 9 s per megabyte -- twenty
     * minutes for the median game, well over an hour for the largest, against
     * about a minute run cached from CPU RAM. Without a proven CPU RAM block
     * the content check is left out and says so; the per-line bus test below
     * still runs, it only samples 1 MB. */
    if (!reloc_active()) {
        scif_puts(S_CART_NORELOC);
        log_result(S_L_CART_SKIP, CLIP_CART, T_SKIP, 0, 0);
        test_cart_pins(cbase);
        return;
    }

    g_cart_t_dma = g_cart_t_pio = 0;
    g_cart_dma = cart_dma_probe(cbase);
    scif_puts(g_cart_dma ? S_CART_DMA_ON : S_CART_DMA_OFF);
    if (g_cart_t_pio) {
        scif_puts(S_CART_8K_DMA);       /* 12.5 MHz ticks -> microseconds */
        scif_putdec(udiv(g_cart_t_dma * 4u, 50u));
        scif_puts(S_CART_8K_PIO);
        scif_putdec(udiv(g_cart_t_pio * 4u, 50u));
        scif_puts(S_CART_8K_US);
    }

    u32 game = cart_identify(cbase);
    if (game == 0xFFFFFFFF && g_cart_dma) {
        /* unknown through the DMA: before calling the content unknown,
         * make sure the DMA path is not the one misreading it */
        g_cart_dma = 0;
        scif_puts(S_CART_RECHECK);
        scif_puts("\n");
        game = cart_identify(cbase);
        if (game != 0xFFFFFFFF) {
            scif_puts(S_CART_DMA_WRONG);
            scif_puts("\n");
        }
    }
    u8 digest[20];

    if (game == 0xFFFFFFFF) {
        scif_puts(S_CART_NOTINDB);
        log_result(S_L_CART_UNKNOWN, CLIP_CART, T_FAIL, 0, 0);
        /* A dead or unstable data line corrupts every read, so the game
         * cannot be identified -- run the per-line test anyway: it tells
         * a bus/connector fault apart from a genuinely unknown cart. */
        test_cart_pins(cbase);
        return;
    }

    scif_puts(S_IDENTIFIED);
    scif_puts(cartdb_title[game]);
    scif_puts("\n");
    {
        u32 n = 0;
        for (const char *t = S_SCR_GAME; *t; t++)
            g_gamebuf[n++] = *t;
        for (u32 i = 0; cartdb_title[game][i] && i < 32 && n < 47; i++)
            g_gamebuf[n++] = cartdb_title[game][i];
        g_gamebuf[n] = 0;
        log_screen_line(LK_INFO, g_gamebuf, 0, 0, 0);
    }
    g_icrow_n = 0;
    g_icrow_det = 0;

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
            icrow_add(idx, IC_ABS);
            continue;                     /* no point hashing dead silence */
        }
        if (i >= nhash) {                 /* present, hashing skipped */
            scif_puts(S_PRESENT_ONLY);
            icrow_add(idx, IC_NOHASH);
            continue;
        }
        scif_putc(' ');
        progress_begin(cart_label(S_P_CART_IC, cartdb_ic_name[idx]),
                       cartdb_ic_size[idx]);
        sha1_ic(cartdb_ic_off[idx], cartdb_ic_size[idx], digest);
        u32 ok = sha1_eq(digest, cartdb_sha1[idx]);
        if (!ok && g_cart_dma) {
            /* a bad chip is a verdict on the cartridge: make sure it is not
             * the DMA path talking, by reading it again through the port */
            g_cart_dma = 0;
            scif_puts(S_CART_RECHECK);
            progress_begin(cart_label(S_P_CART_IC, cartdb_ic_name[idx]),
                           cartdb_ic_size[idx]);
            sha1_ic(cartdb_ic_off[idx], cartdb_ic_size[idx], digest);
            ok = sha1_eq(digest, cartdb_sha1[idx]);
            if (ok)
                scif_puts(S_CART_DMA_WRONG);    /* stays on the port */
            else
                g_cart_dma = 1;                 /* the chip, not the DMA */
        }
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
            icrow_add(idx, IC_ABS);
            continue;
        }
        if (nseen < CART_MAX_TRACK) {
            for (u32 b = 0; b < 20; b++)
                seen_digest[nseen][b] = digest[b];
            seen_idx[nseen] = idx;
            nseen++;
        }
        icrow_add(idx, ok ? IC_OK : IC_BAD);
    }
    icrow_flush();

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

    test_cart_pins(cbase);
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
    aica_init();                        /* ARM7 held in reset: the data bus
                                         * test below writes offset 0, which
                                         * is where a running ARM7 fetches */

    ram_result res;
    ram_result_clear(&res);
    u32 stalls = aica_g2_stalled();
    u32 bad = aram_test_databus();
    if (!bad) {
        aram_test_pattern(AUDIO_ZONE_OFF, AUDIO_ZONE_LEN, 0x55555555, &res);
        aram_test_pattern(AUDIO_ZONE_OFF, AUDIO_ZONE_LEN, 0xAAAAAAAA, &res);
        aram_test_prng(AUDIO_ZONE_OFF, AUDIO_ZONE_LEN, 0xA1CA5EED, &res);
    }
    /* The pattern tests give up without counting an error when the G2 FIFO
     * never drains -- they leave the verdict to their caller. Taking a
     * zero error count at face value here declared audio ready on a bus that
     * moves nothing. */
    if (aica_g2_stalled() != stalls) {
        bad = 1;
        scif_puts(S_G2_STALL);
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

/* The image area of one bank: its data bus, then the three patterns over
 * the 600 KB the framebuffer occupies. */
static t_status quick_fb_zone(u32 bank)
{
    ram_result res;
    ram_result_clear(&res);
    u32 bad = vram_test_databus(bank);
    if (!bad) {
        u32 fb = bank + FB_VRAM_OFFSET;
        vram_test_pattern(fb, FB_ZONE_LEN, 0x55555555, &res);
        vram_test_pattern(fb, FB_ZONE_LEN, 0xAAAAAAAA, &res);
        vram_test_prng(fb, FB_ZONE_LEN, 0x7EC0FFEE, &res);
    }
    if (res.errors)
        scif_puts("\n");               /* end the rescan's progress line */
    return (bad || res.errors) ? T_FAIL : T_OK;
}

static void quick_video_bringup(void)
{
    pvr_vram_enable();

    /* TEX0 first, where the image has always lived. If its image area
     * fails, the same area of TEX1 -- 8 MB further in the same window, on
     * the other four chips -- can carry the screen instead: a dead chip on
     * one bank does not take the display down with it. */
    t_status st = quick_fb_zone(VRAM_TEX0_BASE);
    log_result(S_L_VIDEO_QUICK, CLIP_NONE, st, 0, 0);
    if (st != T_OK) {
        st = quick_fb_zone(VRAM_TEX1_BASE);
        log_result(S_L_VIDEO_QUICK_T1, CLIP_NONE, st, 0, 0);
        if (st == T_OK) {
            pvr_fb_select_tex1(1);
            eta_video_retried();        /* two quick checks, not one */
        }
    }
    if (st == T_OK) {
        pvr_display_init();
        g_screen_ready = 1;             /* channel 2 live, in seconds */
        progress_phase(PH_VIDEO_ON);    /* green */
        screen_render();
        scif_puts(S_SCREEN_ONLINE);
        if (pvr_fb_on_tex1())
            scif_puts(S_SCREEN_TEX1);
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
 * CPU RAM uses word parity; VRAM uses 4 MiB halves in its 32-bit window.
 * The test suite knows a fault is on, say, D0-D15 of the even CPU word. Turning
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
            lane_beacon_phase(VRAM_TEX0_BASE, (k / 2) * 0x400000u + (k % 2) * 2, vram0_names[k], 0);
        for (u32 k = 0; k < 4; k++)
            lane_beacon_phase(VRAM_TEX1_BASE, (k / 2) * 0x400000u + (k % 2) * 2, vram1_names[k], 0);
    }
}
#endif  /* CFG_LANE_BEACON */




/* ---- looping memory tests -------------------------------------------- */

/* One or two regions, over and over, counting passes and accumulated
 * errors. Runs until the TEST button (or a when the buttons are not
 * available -- see diag_input_check): a stray byte on the console must not
 * end a soak.
 *
 * Every part is reported as itself: the video loop covers TEX0 and TEX1,
 * both banks, where it used to stop at TEX0 -- half the VRAM. */
typedef struct {
    u32 base, len;
    region_desc d;
    u32 clip;                       /* spoken name of the memory */
} loop_part;

/* What the screen shows of a soak run, per part: it accumulates, so a
 * fault seen once at 3 a.m. is still on screen in the morning. */
#define LOOP_MAX_PARTS 4
typedef struct {
    u32 errors;                     /* mismatching words, all passes */
    u32 chips;                      /* OR of the failing positions   */
    u32 first_pass;                 /* first pass that failed, 0 = none */
    u32 done;                       /* passes completed on this part */
} loop_stat;

static u32 dec_into(char *d, u32 v)
{
    char t[10];
    u32 n = 0, k = 0;
    do {
        u32 q = udiv(v, 10);
        t[n++] = (char)('0' + (v - q * 10));
        v = q;
    } while (v && n < 10);
    while (n)
        d[k++] = t[--n];
    d[k] = 0;
    return k;
}

/* "h:mm:ss", hours as many as it takes: a soak can run for days */
static void hms_into(char *d, u32 ms)
{
    u32 s = udiv(ms, 1000);
    u32 h = udiv(s, 3600);
    s -= h * 3600;
    u32 m = udiv(s, 60);
    s -= m * 60;
    u32 n = dec_into(d, h);
    d[n++] = ':';
    d[n++] = (char)('0' + udiv(m, 10));
    d[n++] = (char)('0' + m - udiv(m, 10) * 10);
    d[n++] = ':';
    d[n++] = (char)('0' + udiv(s, 10));
    d[n++] = (char)('0' + s - udiv(s, 10) * 10);
    d[n] = 0;
}

static u32 loop_chip_mask(const region_desc *d, const ram_result *r)
{
    u32 is_vram = r->vram_chips || d->comps == tex0_comps ||
                  d->comps == tex1_comps || d->comps == pvrb_comps ||
                  d->comps == elan_comps;
    return is_vram ? r->vram_chips : ram_comp_mask(r);
}

static void loop_screen(const loop_part *parts, u32 nparts, const char *what,
                        const loop_stat *st, u32 pass, u32 errors, u32 t0)
{
    if (!g_screen_ready)
        return;
    char buf[48];
    u32 ymax = fb_hint_y() - 4;
    fb_clear(0);
    fb_progress_invalidate();
    fb_text(16, 8, what, COL_TITLE, FB_W);

    u32 n = 0;
    for (const char *t = S_LP_PASS; *t; t++)
        buf[n++] = *t;
    dec_into(buf + n, pass);
    fb_text(16, 40, buf, COL_WHITE, 320);
    n = 0;
    for (const char *t = S_LP_TIME; *t; t++)
        buf[n++] = *t;
    hms_into(buf + n, timer_ms() - t0);
    fb_text(320, 40, buf, COL_WHITE, FB_W);
    n = 0;
    for (const char *t = S_LP_ERRS; *t; t++)
        buf[n++] = *t;
    dec_into(buf + n, errors);
    fb_text(16, 60, buf, errors ? COL_RED : COL_GREEN, FB_W);

    u32 y = 96;
    for (u32 k = 0; k < nparts && y < ymax; k++) {
        const loop_stat *s = &st[k];
        fb_text(16, y, parts[k].d.group, COL_WHITE, FB_W - 16 * 7);
        if (s->errors)
            fb_text(FB_W - 16 * 6, y, S_SCR_FAIL, COL_RED, FB_W);
        else if (s->done)
            fb_text(FB_W - 16 * 3, y, S_SCR_OK, COL_GREEN, FB_W);
        else
            fb_text(FB_W - 16 * 3, y, S_SCR_IC_NOHASH, COL_WHITE, FB_W);
        y += 20;
        if (!s->errors || y >= ymax)
            continue;
        /* "  1234 err., passe 3" then the chips, five to a row */
        n = 0;
        buf[n++] = ' ';
        buf[n++] = ' ';
        n += dec_into(buf + n, s->errors);
        for (const char *t = S_LP_FIRST; *t; t++)
            buf[n++] = *t;
        dec_into(buf + n, s->first_pass);
        fb_text(16, y, buf, COL_RED, FB_W);
        y += 20;
        const comp_map *c = parts[k].d.comps;
        u32 ncomps = c == pvrb_comps ? 8u : 4u;
        const char *last = 0;
        u32 inrow = 0;
        n = 0;
        for (u32 b = 0; b < ncomps && c; b++) {
            if (!(s->chips & (1u << b)) || c[b].name == last)
                continue;
            last = c[b].name;
            if (inrow == 5 && y < ymax) {
                buf[n] = 0;
                fb_text(16, y, buf, COL_RED, FB_W);
                y += 20;
                n = 0;
                inrow = 0;
            }
            buf[n++] = ' ';
            buf[n++] = ' ';
            for (const char *t = c[b].name; *t; t++)
                buf[n++] = *t;
            inrow++;
        }
        if (inrow && y < ymax) {
            buf[n] = 0;
            fb_text(16, y, buf, COL_RED, FB_W);
            y += 20;
        }
        y += 4;
    }
    fb_text(16, fb_hint_y(), g_mie_prog ? S_LP_HINT : S_LP_HINT_KEY,
            COL_HINT, FB_W);
}

/* The spoken report of a part's first failure, the words the boot suite
 * would use: "Video memory, I C sixteen, defective." */
static void loop_say(const loop_part *lp, u32 chips)
{
    log_entry e;
    e.name = lp->d.group;
    e.clip = lp->clip;
    e.status = T_FAIL;
    e.detail = chips;
    e.comps = lp->d.comps;
    e.quiet_ok = 0;
    e.pre = CLIP_NONE;
    e.kind = LK_NONE;
    e.line = 0;
    e.dq = 0;
    say_entry(&e);
}

static void loop_regions(const loop_part *parts, u32 nparts, const char *what)
{
    u32 pass = 0, errors = 0;
    loop_stat st[LOOP_MAX_PARTS];
    for (u32 k = 0; k < LOOP_MAX_PARTS; k++)
        st[k].errors = st[k].chips = st[k].first_pass = st[k].done = 0;
    if (nparts > LOOP_MAX_PARTS)
        nparts = LOOP_MAX_PARTS;
    u32 t0 = timer_ms();
    u32 bank0 = pvr_fb_on_tex1();       /* where the image goes back to */
    u32 retired = fb_progress_set_retired(0);
    progress_set_loop(1);
    while (!progress_aborted()) {
        pass++;
        scif_puts(S_LOOP_PASS);
        scif_putdec(pass);
        scif_puts(": ");
        for (u32 k = 0; k < nparts && !progress_aborted(); k++) {
            const loop_part *lp = &parts[k];
            ram_result res;
            ram_result_clear(&res);
            /* The image cannot stay in the bank about to be tested: the
             * patterns would overwrite it, and drawing would corrupt the
             * patterns. It moves to the other bank for the part, repainted
             * there -- a bad bank shows as a garbled screen, not a false
             * result. */
            if (g_screen_ready && (lp->base == VRAM_TEX0_BASE ||
                                   lp->base == VRAM_TEX1_BASE)) {
                u32 fa = pvr_fb_addr();
                if (lp->base <= fa && fa < lp->base + lp->len)
                    pvr_fb_set_bank(!pvr_fb_on_tex1());
            }
            loop_screen(parts, nparts, what, st, pass, errors, t0);
            u32 stalls = aica_g2_stalled();
            /* The framebuffer lives in TEX0. Drawing the bar into it while
             * TEX0 is under test would write into the pattern being verified
             * and report a fault that does not exist -- the boot test guards
             * against this, the loop did not. */
            u32 fbaddr = pvr_fb_addr();
            /* PVR-B's independence rests on a sample: draw nothing while
             * its whole range is being written, as the boot suite does. */
            progress_screen_enable(lp->base != VRAM_PVRB_BASE &&
                                   !(lp->base <= fbaddr && fbaddr < lp->base + lp->len));
            progress_begin(what, (lp->len >> 2) * 2);
            if (lp->base == ARAM_P2_BASE) {
                aram_test_pattern(0, lp->len, 0x55555555, &res);
                aram_test_pattern(0, lp->len, 0xAAAAAAAA, &res);
                aram_test_prng(0, lp->len, 0xC0FFEE42 ^ pass, &res);
            } else if (lp->base == SDRAM_P2_BASE) {
                ram_test_pattern(lp->base, lp->len, 0x55555555, &res);
                ram_test_pattern(lp->base, lp->len, 0xAAAAAAAA, &res);
                ram_test_prng(lp->base, lp->len, 0xDEADBEEF ^ pass, &res);
            } else {
                vram_test_pattern(lp->base, lp->len, 0x55555555, &res);
                vram_test_pattern(lp->base, lp->len, 0xAAAAAAAA, &res);
                vram_test_prng(lp->base, lp->len, 0x7E0CBEEF ^ pass ^ lp->base, &res);
            }
            progress_end();

            /* A stalled G2 bus makes the sound RAM loops give up without
             * counting anything; a pass of "0 errors" would be a lie. */
            if (aica_g2_stalled() != stalls) {
                scif_puts(S_G2_STALL);
                res.errors++;
            }
            errors += res.errors;
            if (!progress_aborted())
                st[k].done++;
            if (res.errors) {
                scif_puts("\n");       /* off the progress line */
                report_region(&lp->d, &res);
                u32 chips = loop_chip_mask(&lp->d, &res);
                u32 first = !st[k].errors;
                st[k].errors += res.errors;
                st[k].chips |= chips;
                if (first) {
                    st[k].first_pass = pass;
                    loop_screen(parts, nparts, what, st, pass, errors, t0);
                    loop_say(lp, chips);
                }
            }
        }
        scif_puts(S_LOOP_ERR);
        scif_putdec(errors);
        scif_puts("\n");
    }
    if (pvr_fb_on_tex1() != bank0)
        pvr_fb_set_bank(bank0);         /* the caller repaints the report */
    fb_progress_set_retired(retired);
    progress_screen_enable(1);
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
    /* The serial console must remain usable without a connected monitor.
     * Repeat the small menu after each selection change: plain terminals
     * and saved logs both retain the selected entry without ANSI escapes. */
    scif_puts("\n");
    scif_puts(S_MENU_TITLE);
    scif_puts(" -- ");
    scif_puts(S_MENU_HINT);
    scif_puts("\n");
    for (u32 i = 0; i < ACT_COUNT; i++) {
        scif_puts(i == sel ? "> [" : "  [");
        scif_putc(menu_key[i]);
        scif_puts("] ");
        scif_puts(menu_label[i]);
        scif_puts("\n");
    }
    if (!g_screen_ready)
        return;
    fb_clear(0);
    fb_text(16, 8, S_MENU_TITLE, COL_TITLE, FB_W);
    for (u32 i = 0; i < ACT_COUNT; i++) {
        u32 y = 64 + i * 28;
        fb_text(16, y, i == sel ? ">" : " ", COL_TITLE, FB_W);
        fb_text(48, y, menu_label[i], i == sel ? COL_TITLE : COL_WHITE, FB_W);
    }
    fb_text(16, 456, S_MENU_HINT, COL_WHITE, FB_W);
}

static void run_action(u32 act);
static void monitor_run(void);

/* TEST steps through the entries and wraps at the end; SERVICE runs the one
 * shown. A serial key jumps straight to its action without the menu. */
static void menu_run(void)
{
    u32 sel = 0;
    menu_draw(sel);

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
                } else if (ev.key == '!') {
                    monitor_run();
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

/* A menu action that reports gets a report of its own: the menu replaces the
 * boot report rather than piling onto it. Without this every press of g or
 * d appended five or six entries to the boot suite's two dozen, and a few
 * presses filled the log. The looping tests write nothing to the log and
 * leave the boot report where it is. */
static void report_restart(void)
{
    g_log_n = 0;
    screen_render();
}

/* ---- JVS I/O board --------------------------------------------------
 *
 * The JVS master runs in the MIE, inside the uploaded Z80 program: it
 * resets the bus, gives the board address 1, asks who it is and what it
 * has, then polls its inputs on its own. This side only reads the result,
 * 28 bytes at a time, so nothing here ever waits on the JVS bus. */

static void hex_into(char *d, u32 v, u32 ndigits);

/* Switches per player as the board declares them (feature 01, third
 * byte): START, SERVICE, four directions, then the buttons. Falls back
 * to the bytes it asked for when the list says nothing. */
static u32 jvs_switches(const u8 *info)
{
    for (u32 f = 0; f < 32 && info[JVSI_FEAT + f]; f += 4)
        if (info[JVSI_FEAT + f] == 1)
            return info[JVSI_FEAT + f + 2];
    return info[JVSI_SWBYTES] * 8u;
}

static u32 jvs_read(u8 *info, u32 nslices)
{
    for (u32 i = 0; i < nslices; i++)
        if (maple_jvs_info(g_mie_port1 - 1, i, info + i * 28))
            return 1;
    return 0;
}

/* JVS revisions are BCD: 0x13 is 1.3. */
static void put_bcd_rev(u32 v)
{
    scif_putdec((v >> 4) & 0xF);
    scif_putc('.');
    scif_putdec(v & 0xF);
}

static void test_jvs_io(void)
{
    if (!g_mie_prog) {
        scif_puts(S_JVS_SKIP);
        return;
    }
    scif_puts(S_JVS_HDR);

    /* The first answer takes two bus resets and their settling time, or a
     * timeout on the address when nobody is there: well under two seconds
     * either way. Three is the patience before calling the master stuck. */
    u8 info[JVSI_LEN];
    u32 state = 0;
    for (u32 t = 0; t < 60; t++) {
        if (maple_jvs_info(g_mie_port1 - 1, 0, info) == 0) {
            state = info[JVSI_STATE];
            if (state >= JVS_READY)
                break;
        }
        delay_ms(50);
        progress_heartbeat();
    }

    if (state == JVS_READY && jvs_read(info, 7) == 0) {
        info[JVSI_ID + 63] = 0;
        scif_puts(S_JVS_ID);
        scif_puts((const char *)&info[JVSI_ID]);
        scif_puts("\n");
        scif_puts(S_JVS_REV);
        put_bcd_rev(info[JVSI_CMDREV]);
        scif_puts(" / ");
        put_bcd_rev(info[JVSI_JVSREV]);
        scif_puts(" / ");
        put_bcd_rev(info[JVSI_COMMVER]);
        scif_puts("\n");
        scif_puts(S_JVS_FEAT);
        scif_putdec(info[JVSI_PLAYERS]);
        scif_puts(S_JVS_PLAYERS);
        scif_putdec(jvs_switches(info));
        scif_puts(S_JVS_BUTTONS);
        scif_putdec(info[JVSI_COINS]);
        scif_puts(S_JVS_COINS);
        scif_putdec(info[JVSI_ANACH]);
        scif_puts(S_JVS_ANALOG);
        scif_putdec(info[JVSI_ANABITS]);
        scif_puts(S_JVS_BITS);
        scif_puts(S_JVS_SENSE);
        scif_puthex(info[JVSI_SENSE]);
        scif_puts("\n");
        log_result(S_L_JVS_READY, CLIP_JVS_IO, T_OK, 0, 0);
        return;
    }

    scif_puts(S_JVS_SENSE);
    scif_puthex(info[JVSI_SENSE]);
    scif_puts("\n");
    if (state == JVS_NONE) {
        /* Nobody answered the address. A bench board with no I/O board
         * plugged in is the ordinary case, not a fault. */
        log_result(S_L_JVS_ABSENT, CLIP_NONE, T_OK, 0, 0);
        say(CLIP_JVS_IO, 250);
        say(CLIP_ABSENT, REPORT_GAP_MS);
        return;
    }
    if (state == JVS_ERROR) {
        scif_puts(S_JVS_ERRINFO);
        scif_putdec(info[JVSI_ERR]);
        scif_puts(S_JVS_ERRSTEP);
        scif_putdec(info[JVSI_FSTEP]);
        scif_puts("\n");
        scif_puts(S_JVS_ERRCODES);
        log_result(S_L_JVS_ERROR, CLIP_JVS_IO, T_FAIL, 0, 0);
        return;
    }
    log_result(S_L_JVS_STUCK, CLIP_JVS, T_FAIL, 0, 0);
}

/* ---- JVS input test screen ---------------------------------------------
 * Every input the board declared, live: pressed in green, released in grey.
 * The cabinet's own TEST is one of the inputs under test, so it cannot be
 * the way out; the board's push buttons and the serial console are. */

#define COL_DIM     RGB565(10, 20, 10)

static void jt_line_clear(u32 y)
{
    fb_fill_rows(y, y + 18, 0);
}

/* Draws a token and returns the next x. */
static u32 jt_tok(u32 x, u32 y, const char *s, u32 on)
{
    fb_text(x, y, s, on ? COL_GREEN : COL_DIM, FB_W);
    u32 n = 0;
    while (s[n])
        n++;
    return x + (n + 1) * 16;
}

static void jt_draw(const u8 *info, u32 full)
{
    static const char *const btn[16] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "10",
        "11", "12", "13", "14", "15", "16"
    };
    char buf[8];

    if (full) {
        fb_clear(0);
        fb_text(16, 8, S_JT_TITLE, COL_TITLE, FB_W);
        fb_text(16, 456, S_JT_QUIT, COL_WHITE, FB_W);
    }
    u32 state = info[JVSI_STATE];
    if (full || state != JVS_READY)
        jt_line_clear(40);
    if (state != JVS_READY) {
        fb_text(16, 40, state == JVS_NONE ? S_JT_NONE :
                        state == JVS_ERROR ? S_JT_ERR : S_JT_WAIT,
                state == JVS_ERROR ? COL_RED : COL_WHITE, FB_W);
        return;
    }
    if (full) {
        /* JVS names read "maker;board;version;comment" and often run past
         * one line: break at the last ';' that fits, two lines at most. */
        char id[2][37];
        const u8 *src = &info[JVSI_ID];
        jt_line_clear(58);
        for (u32 line = 0; line < 2 && *src; line++) {
            u32 n = 0, cut = 0;
            while (n < 36 && src[n]) {
                if (src[n] == ';')
                    cut = n + 1;
                n++;
            }
            if (src[n] && cut)
                n = cut;
            for (u32 i = 0; i < n; i++)
                id[line][i] = (char)src[i];
            id[line][n] = 0;
            fb_text(16, 40 + line * 18, id[line], COL_WHITE, FB_W);
            src += n;
        }
    }

    const u8 *sw = &info[JVSI_SW];
    u32 y = 88;
    jt_line_clear(y);
    u32 x = jt_tok(16, y, S_JT_SYS, 1);
    x = jt_tok(x, y, "TEST", sw[0] & 0x80);
    x = jt_tok(x, y, "TILT1", sw[0] & 0x40);
    x = jt_tok(x, y, "TILT2", sw[0] & 0x20);
    x = jt_tok(x, y, "TILT3", sw[0] & 0x10);

    /* Player byte 0: START SERVICE UP DOWN LEFT RIGHT B1 B2, then byte 1:
     * B3..B10, and B11 onwards in a third byte where a board has one. */
    u32 nbytes = info[JVSI_SWBYTES];
    u32 nsw = jvs_switches(info);
    if (nsw > nbytes * 8)
        nsw = nbytes * 8;
    u32 nbtn = nsw > 6 ? nsw - 6 : 0;
    if (nbtn > 16)
        nbtn = 16;
    for (u32 p = 0; p < info[JVSI_PLAYERS] && p < 4; p++) {
        const u8 *pb = &sw[1 + p * nbytes];
        y = 120 + p * 26;
        jt_line_clear(y);
        buf[0] = S_JT_PLAYER[0];
        buf[1] = (char)('1' + p);
        buf[2] = 0;
        x = jt_tok(16, y, buf, 1);
        x = jt_tok(x, y, "ST", pb[0] & 0x80);
        x = jt_tok(x, y, "SV", pb[0] & 0x40);
        x = jt_tok(x, y, S_JT_DIR_U, pb[0] & 0x20);
        x = jt_tok(x, y, S_JT_DIR_D, pb[0] & 0x10);
        x = jt_tok(x, y, S_JT_DIR_L, pb[0] & 0x08);
        x = jt_tok(x, y, S_JT_DIR_R, pb[0] & 0x04);
        for (u32 b = 0; b < nbtn && x < FB_W - 32; b++) {
            u32 bit = b + 6;                /* bit index from byte 0, MSB first */
            u32 on = pb[bit >> 3] & (0x80u >> (bit & 7));
            x = jt_tok(x, y, btn[b], on);
        }
    }

    y = 248;
    jt_line_clear(y);
    x = jt_tok(16, y, S_JT_COINS, 1);
    for (u32 c = 0; c < info[JVSI_COINS] && c < 4; c++) {
        const u8 *cb = &info[JVSI_COIN + c * 2];
        u32 v = ((u32)(cb[0] & 0x3F) << 8) | cb[1];
        buf[0] = (char)('1' + c);
        buf[1] = ':';
        buf[2] = (char)('0' + (v / 1000) % 10);
        buf[3] = (char)('0' + (v / 100) % 10);
        buf[4] = (char)('0' + (v / 10) % 10);
        buf[5] = (char)('0' + v % 10);
        buf[6] = (cb[0] & 0xC0) ? '!' : 0;  /* slot condition not normal */
        buf[7] = 0;
        x = jt_tok(x, y, buf, v != 0);
    }

    for (u32 row = 0; row < 2; row++) {
        y = 288 + row * 28;
        jt_line_clear(y);
        if (row * 4 >= info[JVSI_ANACH])
            continue;
        x = jt_tok(16, y, S_JT_ANA, 1);
        for (u32 c = row * 4; c < info[JVSI_ANACH] && c < row * 4 + 4; c++) {
            const u8 *ab = &info[JVSI_ANA + c * 2];
            buf[0] = (char)('1' + c);
            buf[1] = ':';
            hex_into(buf + 2, ((u32)ab[0] << 8) | ab[1], 4);
            buf[6] = 0;
            x = jt_tok(x, y, buf, 1);
        }
    }

    /* the raw switch bytes, for a board whose layout differs */
    y = 368;
    jt_line_clear(y);
    x = 16;
    for (u32 i = 0; i < 1 + info[JVSI_PLAYERS] * nbytes && i < 12; i++) {
        hex_into(buf, sw[i], 2);
        buf[2] = 0;
        x = jt_tok(x, y, buf, sw[i] != 0);
    }
}

/* The board's own TEST button, from the MIE port (active low): an input
 * under test like the others, since only SERVICE leaves this screen. */
static void jt_draw_board(u8 in5)
{
    u32 y = 222;
    jt_line_clear(y);
    u32 x = jt_tok(16, y, S_JT_BOARD, 1);
    jt_tok(x, y, "TEST", !(in5 & 0x10));
}

static void jvs_input_test(void)
{
    if (!g_mie_prog) {
        scif_puts(S_JVS_SKIP);
        return;
    }
    scif_puts(S_JT_SERIAL);

    u8 info[JVSI_LEN], shown[56];
    for (u32 i = 0; i < JVSI_LEN; i++)
        info[i] = 0;
    for (u32 i = 0; i < sizeof shown; i++)
        shown[i] = 0xFF;
    u32 have_id = 0, drawn_state = 0xFF;
    u8 in5 = 0x30, prev_in5 = 0xFF;     /* both board buttons released */
    u32 board_drawn = 0;

    for (;;) {
        /* Way out: any serial key, or a press of the board's SERVICE. The
         * board's TEST is one of the inputs under test, like the cabinet's:
         * it lights up here and leaves nothing. */
        if (scif_getc() >= 0)
            break;
        if (maple_mie_inputs(g_mie_port1 - 1, &in5) == 0) {
            if (prev_in5 != 0xFF && (prev_in5 & 0x20) && !(in5 & 0x20))
                break;                  /* SERVICE: 1 -> 0, active low */
            if (prev_in5 != 0xFF && ((prev_in5 ^ in5) & 0x10)) {
                scif_puts(S_JT_BOARD_SER);
                scif_puts(in5 & 0x10 ? S_BTN_UP : S_BTN_DOWN);
                scif_puts("\n");
            }
            if (g_screen_ready && (!board_drawn || ((prev_in5 ^ in5) & 0x10))) {
                jt_draw_board(in5);
                board_drawn = 1;
            }
            prev_in5 = in5;
        }

        if (jvs_read(info, 2) == 0) {
            u32 state = info[JVSI_STATE];
            if (state == JVS_READY && !have_id) {
                have_id = jvs_read(info, 7) == 0;
                drawn_state = 0xFF;
            }
            if (state != JVS_READY)
                have_id = 0;
            u32 full = state != drawn_state;
            drawn_state = state;
            if (g_screen_ready) {
                jt_draw(info, full);
                if (full)
                    jt_draw_board(in5);  /* a full repaint cleared it */
            }

            /* Serial: one line whenever a switch or a coin count moves.
             * Analog is left out; it would print on every jitter. */
            u32 changed = full;
            for (u32 i = JVSI_SW; i < JVSI_ANA; i++)
                if (shown[i - JVSI_SW] != info[i])
                    changed = 1;
            if (changed && state == JVS_READY) {
                scif_puts("  SW");
                for (u32 i = 0; i < 1u + info[JVSI_PLAYERS] * info[JVSI_SWBYTES] && i < 16; i++) {
                    scif_putc(' ');
                    char h[3];
                    hex_into(h, info[JVSI_SW + i], 2);
                    h[2] = 0;
                    scif_puts(h);
                }
                if (info[JVSI_COINS]) {
                    scif_puts("  COIN");
                    for (u32 c = 0; c < info[JVSI_COINS] && c < 4; c++) {
                        scif_putc(' ');
                        scif_putdec(((u32)(info[JVSI_COIN + c * 2] & 0x3F) << 8) |
                                    info[JVSI_COIN + c * 2 + 1]);
                    }
                }
                scif_puts("\n");
            }
            for (u32 i = JVSI_SW; i < JVSI_ANA; i++)
                shown[i - JVSI_SW] = info[i];
        }
        delay_ms(40);
        progress_heartbeat();
    }
    /* The press that ended the test must not also open the menu the
     * console goes back to: wait for the board buttons to be let go --
     * TEST included, which may be held while SERVICE is pressed. */
    for (u32 t = 0; t < 100; t++) {
        if (maple_mie_inputs(g_mie_port1 - 1, &in5) == 0 && (in5 & 0x30) == 0x30)
            break;
        delay_ms(20);
    }
    scif_puts(S_JT_END);
}

/* ---- video test pattern ---------------------------------------------
 * TEST (board or cabinet) or any serial key shows the next image; SERVICE,
 * START, 'a' or 'q' leaves. The border stops pulsing while an image is up:
 * it is part of the picture the operator is judging. */
static void test_pattern(void)
{
    static const char *const name[] = {
        S_PAT_0, S_PAT_1, S_PAT_2, S_PAT_3, S_PAT_4,
        S_PAT_5, S_PAT_6, S_PAT_7, S_PAT_8, S_PAT_9
    };
    if (!g_screen_ready) {
        scif_puts(S_PAT_NOSCREEN);
        return;
    }
    scif_puts(S_PAT_SERIAL);
    pvr_border(0);

    u32 n = 0, count = fb_pattern_count(), shown = 0xFF;
    u32 hint_until = 0;
    for (;;) {
        if (n != shown) {
            fb_pattern(n);
            shown = n;
            scif_puts("  ");
            scif_putdec(n + 1);
            scif_puts("/");
            scif_putdec(count);
            scif_puts(" ");
            scif_puts(name[n]);
            scif_puts("\n");
            /* the controls, on the first image only, for three seconds */
            if (n == 0) {
                fb_fill_rows(456, 474, 0);
                fb_text(16, 458, S_PAT_HINT, COL_WHITE, FB_W);
                hint_until = timer_ms() + 3000;
            }
        }
        if (hint_until && (int)(timer_ms() - hint_until) >= 0) {
            hint_until = 0;
            if (n == 0)
                fb_pattern(0);
        }

        input_event ev;
        if (input_poll(&ev)) {
            if (ev.kind == INPUT_CONFIRM)
                break;
            if (ev.kind == INPUT_KEY &&
                (ev.key == 'a' || ev.key == 'A' || ev.key == 'q' || ev.key == 'Q'))
                break;
            if (ev.kind == INPUT_SELECT || ev.kind == INPUT_KEY)
                n = n + 1 < count ? n + 1 : 0;  /* no libgcc: no runtime divide */
        }
        delay_ms(10);
        timer_ms();                     /* keeps the serial clock past its wrap */
    }
    scif_puts(S_PAT_END);
}

static void run_action(u32 act)
{
    progress_clear_abort();
    switch (act) {
    case ACT_CPU: {
        loop_part p = { SDRAM_P2_BASE,
                        g_reloc_win ? g_reloc_win - SDRAM_P2_BASE : g_ram_size,
                        { S_CG_CPU, IC(work_comps), 0 }, CLIP_CPU_RAM };
        loop_regions(&p, 1, S_M_CPU);
        break;
    }
    case ACT_VRAM: {
        loop_part p[4] = {
            { VRAM_TEX0_BASE, VRAM_TEX0_SIZE, { S_L_VRAM_TEX0, IC(tex0_comps), 0 }, CLIP_VRAM },
            { VRAM_TEX1_BASE, VRAM_TEX1_SIZE, { S_L_VRAM_TEX1, IC(tex1_comps), 0 }, CLIP_VRAM },
        };
        u32 n = 2;
        /* A Naomi 2 loops its other graphics memories too, under the same
         * conditions as the boot suite: PVR-B only once its windows have
         * been shown independent of PVR-A, the Elan RAM whenever the Elan
         * answers. Qualified again here: the suite may have been cut short
         * before it reached them. */
        if (g_board_type == BOARD_NAOMI2) {
            pvr2_access a = pvr2_prepare();
            if (a == PVR2_READY) {
                loop_part b = { VRAM_PVRB_BASE, VRAM_PVRB_SIZE,
                                { S_CG_PVRB, IC(pvrb_comps), 0 }, CLIP_VRAM_B };
                p[n++] = b;
            } else {
                scif_puts(S_PVRB_SKIP);
                scif_putdec((u32)a);
                scif_puts("\n");
            }
            if (a != PVR2_NO_ELAN && a != PVR2_CONTROL) {
                loop_part e = { ELAN_RAM_BASE, ELAN_RAM_SIZE,
                                { S_CG_ELAN, IC(elan_comps), 0 }, CLIP_ELAN };
                p[n++] = e;
            } else {
                scif_puts(S_ELAN_SKIP);
            }
        }
        loop_regions(p, n, S_M_VRAM);
        break;
    }
    case ACT_ARAM: {
        loop_part p = { ARAM_P2_BASE, ARAM_SIZE, { S_CG_SOUND, IC(aram_comps), 1 },
                        CLIP_SOUND_RAM };
        aica_arm_halt();                /* the loop writes offset 0: issue #1 */
        loop_regions(&p, 1, S_M_ARAM);
        aica_arm_park();
        break;
    }
    case ACT_DIMM:
        report_restart();
        test_dimm();
        break;
    case ACT_GAME:
        report_restart();
        test_x76();
        test_cartridge();
        break;
    case ACT_FLASH:
        report_restart();
        test_dimm_flash();
        break;
    case ACT_JVS:
        jvs_input_test();
        break;
    case ACT_PATTERN:
        test_pattern();
        break;
    default:
        break;
    }
}

/* Idle on the report until the operator asks for something. */
/* The report again, with the way to the operator menu on its last line. */
static void idle_screen(void)
{
    if (!g_screen_ready)
        return;
    fb_banner_reserve();
    screen_render();
    fb_banner(S_IDLE_BANNER);
}

/* ---- serial monitor ----------------------------------------------------
 * '!' on the console, from the menu or the idle screen. Reads and writes
 * any address and runs the G1 unlock in parts, so a hardware question can
 * be put to a real board without burning an EPROM for each guess. Lines:
 *   r b|w|l ADDR [N]    read N bytes/words/longs (hex), 8 per line
 *   w b|w|l ADDR VAL    write
 *   u [FLAGS]           G1 unlock, g1_unlock_v bits (default F)
 *   x                   X76F100 response-to-reset (board-ID register)
 *   d                   DIMM mailbox, signature and latch probe
 *   q                   back
 * Addresses are raw: give the P2 alias (A05F703C) for a register. A bad
 * one ends in the CPU exception report, as any other wild access would. */
static u32 mon_hex(const char **s, u32 *v)
{
    while (**s == ' ')
        (*s)++;
    u32 n = 0, r = 0;
    for (;;) {
        char c = **s;
        u32 d;
        if (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
        else break;
        r = (r << 4) | d;
        n++;
        (*s)++;
    }
    *v = r;
    return n != 0;
}

static u32 mon_line(char *buf, u32 max)
{
    u32 n = 0;
    for (;;) {
        input_event ev;
        if (input_poll(&ev) && ev.kind == INPUT_KEY) {
            int c = ev.key;
            if (c == '\r' || c == '\n') {
                scif_puts("\n");
                buf[n] = 0;
                return n;
            }
            if ((c == 8 || c == 127) && n) {
                n--;
                scif_puts("\b \b");
            } else if (c >= 32 && c < 127 && n < max - 1) {
                buf[n++] = (char)c;
                scif_putc((char)c);
            }
        }
        progress_heartbeat();
    }
}

static void monitor_run(void)
{
    char line[64];
    scif_puts(S_MON_HELP);
    for (;;) {
        scif_puts("mon> ");
        mon_line(line, sizeof line);
        const char *s = line;
        while (*s == ' ')
            s++;
        char cmd = *s ? *s++ : 0;
        if (cmd == 'q' || cmd == 'Q')
            return;
        if (cmd == 'r' || cmd == 'w') {
            while (*s == ' ')
                s++;
            char sz = *s ? *s++ : 0;
            u32 addr, v = 1;
            if ((sz != 'b' && sz != 'w' && sz != 'l') || !mon_hex(&s, &addr)) {
                scif_puts(S_MON_ERR);
                continue;
            }
            u32 step = sz == 'b' ? 1 : sz == 'w' ? 2 : 4;
            if (cmd == 'w') {
                if (!mon_hex(&s, &v)) {
                    scif_puts(S_MON_ERR);
                    continue;
                }
                if (step == 1) *(volatile u8 *)addr = (u8)v;
                else if (step == 2) *(volatile u16 *)addr = (u16)v;
                else *(volatile u32 *)addr = v;
                continue;
            }
            if (!mon_hex(&s, &v))
                v = 1;
            for (u32 i = 0; i < v; i++) {
                u32 a = addr + i * step;
                if ((i & 7) == 0) {
                    if (i)
                        scif_puts("\n");
                    scif_puthex(a);
                    scif_puts(":");
                }
                scif_putc(' ');
                if (step == 1) scif_puthex(*(volatile u8 *)a);
                else if (step == 2) scif_puthex(*(volatile u16 *)a);
                else scif_puthex(*(volatile u32 *)a);
            }
            scif_puts("\n");
        } else if (cmd == 'u' || cmd == 'U') {
            u32 f;
            if (!mon_hex(&s, &f))
                f = 0x31;
            p_g1_unlock_v(f);
            scif_puts(S_MON_DONE);
        } else if (cmd == 'x' || cmd == 'X') {
            scif_puthex(x76f100_rtr());
            scif_puts("\n");
        } else if (cmd == 'd' || cmd == 'D') {
            dimm_info di;
            dimm_probe(&di);
            scif_puthex(di.command); scif_putc(' ');
            scif_puthex(di.offsetl); scif_putc(' ');
            scif_puthex(di.paraml); scif_putc(' ');
            scif_puthex(di.paramh); scif_putc(' ');
            scif_puthex(di.status); scif_puts(" sig ");
            scif_puthex(di.signature);
            scif_puts(di.latch ? " latch OK\n" : " latch --\n");
        } else if (cmd) {
            scif_puts(S_MON_HELP);
        }
    }
}

static void console_idle(void)
{
    idle_screen();
    scif_puts(S_WAIT_TEST);
    for (;;) {
        input_event ev;
        if (input_poll(&ev)) {
            if (ev.kind == INPUT_SELECT || ev.kind == INPUT_CONFIRM) {
                menu_run();
                idle_screen();
                scif_puts(S_WAIT_TEST);
            } else if (ev.kind == INPUT_KEY) {
                if (ev.key == 'h' || ev.key == 'H') {
                    scif_puts(S_HELP);
                } else if (ev.key == '!') {
                    monitor_run();
                } else {
                    for (u32 i = 0; i < ACT_COUNT; i++) {
                        if (ev.key == menu_key[i]) {
                            run_action(i + 1);
                            idle_screen();
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

/* ------------------------------------------------------------------ */
/* CPU exception report.
 *
 * crt0.S has already printed the cause and the faulting address on serial --
 * it needs nothing, not even a stack, so it goes first. It then switches to
 * a fresh stack at the top of OC-RAM and calls this, which adds the screen
 * and then the speaker, each only if it had been brought up, and halts.
 *
 * The README used to say an exception restarted the ROM and reprinted the
 * banner. That was true before VBR was installed; since then the handler
 * reported on serial and stopped, and the screen and speaker said nothing.
 *
 * Nothing here may rely on state the exception could have broken beyond
 * what it has to: the report is a line of text and one clip. A fault inside
 * it vectors back into the handler, which sees g_exc_active and stops at the
 * serial report rather than recursing. */
u32 g_exc_active;                       /* read by crt0.S, set there too */

static void hex_into(char *d, u32 v, u32 ndigits)
{
    static const char hx[] = "0123456789ABCDEF";
    for (u32 i = 0; i < ndigits; i++)
        d[i] = hx[(v >> ((ndigits - 1 - i) * 4)) & 0xF];
}

void exc_report(u32 expevt, u32 spc)
{
    if (g_screen_ready) {
        char line[40];
        u32 n = 0;
        for (const char *t = S_EXC_SCREEN; *t && n < 20; t++)
            line[n++] = *t;
        hex_into(&line[n], expevt, 3);
        n += 3;
        line[n++] = ' ';
        line[n++] = 'P';
        line[n++] = 'C';
        line[n++] = ' ';
        hex_into(&line[n], spc, 8);
        n += 8;
        line[n] = 0;
        u32 ymax = fb_report_ymax();
        u32 y = g_screen_y + 20 <= ymax ? g_screen_y : ymax - 20;
        fb_text(16, y, line, COL_RED, FB_W);
    }
    pvr_border(0x00FF0000u);            /* solid red: halted, not pulsing */

    if (g_audio_ready)
        say(CLIP_CPU_EXC, 250);

    pvr_border(0x00FF0000u);            /* the speech wait pulses the border */
    for (;;)
        ;
}

/* The boot suite looks here between tests.
 *
 * A key or the TEST button is noticed in progress_tick, which only records a
 * request; the test running at the time stops at its next block and draws no
 * conclusion from the part it did, and the suite comes here instead of going
 * on to the next test. It does not resume -- the operator asked for something
 * else. 'a' goes to the report, TEST opens the menu, a menu key runs its
 * action.
 *
 * This did not exist. The request was recorded and nobody in the boot suite
 * ever read it: only the DIMM test and the soak loops looked, so neither a
 * key nor a button interrupted the tests the README said they did. */
static void suite_check_abort(void)
{
    u32 a = progress_aborted();
    if (!a)
        return;
    progress_clear_abort();
    progress_end();
    progress_screen_enable(1);
    progress_retire();                  /* nothing is being measured any more */
    progress_phase(PH_DONE);
    eta_finish();
    hint_retire();
    scif_puts(S_SUITE_STOPPED);
    screen_render();
    if (a >= ABORT_ACT(1) && a <= ABORT_ACT(ACT_COUNT)) {
        run_action(a - ABORT_ACT(0));
        screen_render();
    } else if (a == ABORT_MENU) {
        menu_run();
        screen_render();
    }
    console_idle();                     /* never returns */
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
    eta_start();
    quick_video_bringup();   /* screen first: richest channel, no replay */
    eta_step(ETA_AUDIO);
    quick_audio_bringup();   /* then audio, which replays the history */
    eta_set_audio(g_audio_ready);
    suite_check_abort();

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
    eta_step(ETA_BOARD);
    test_board();
    eta_set_board_n2(g_board_type == BOARD_NAOMI2);
    suite_check_abort();
    progress_phase(PH_BOARD_ID);        /* magenta */

    eta_step(ETA_RELOC);
    relocate_fast_loops();
    eta_set_reloc(reloc_active());
    eta_set_mie_early(g_maple_safe);
    suite_check_abort();

    /* Open the G1 bus to the cartridge or DIMM now, before the EPROM
     * checksum, from the relocated block when there is one (see g1_unlock
     * in ramtest_fast.S). Without it every ROM-board access reads 0xFFFF. */
    if (g1_open())
        scif_puts(!g1_devboot_present() ? S_G1_NODEVBOOT :
                  reloc_active() ? S_G1_UNLOCKED : S_G1_UNLOCKED_ROM);

    /* Only now the 2 MB ROM checksum: it is the single longest test in the
     * whole suite (4.2 M table steps), it must never run while the operator
     * is still staring at a black screen, and it waits for the relocation so
     * that, with a proven CPU RAM window, its loop runs cached from there and
     * only the data still comes off the EPROM. */
    eta_step(ETA_BIOS);
    test_bios_rom();
    suite_check_abort();

    /* The MIE stage, as early as it can run. Its DMA buffers need RAM, and
     * the block that holds them has just been qualified with the full
     * pattern suite; that is all the Maple bus needs. Running it here uploads
     * the Z80 program before the memory tests instead of after them, so the
     * board's TEST and SERVICE buttons can interrupt the long part of the
     * suite -- which is when an operator wants to. Without a qualified block
     * it stays where it was, behind the CPU RAM verdict. */
    eta_step(ETA_MIE);
    {
        /* the plan is known from here: board, loops, MIE stage, audio */
        char t[9];
        eta_format(t, eta_remaining_ms());
        scif_puts(S_ETA);
        scif_puts(t);
        scif_puts("\n");
    }
    if (g_maple_safe) {
        test_maple_mie(1);
        test_settings_eeprom();
        test_jvs_io();
    }
    suite_check_abort();

    progress_phase(PH_TESTING);         /* white: the long suite starts */

    /* The exhaustive memory tests: CPU RAM, then video, then sound.
     *
     * The order is free. The quick bring-ups have already proved the small
     * regions the screen and the speaker need, so every result is shown
     * and spoken as it lands whichever memory is under test -- which lets
     * the memory the rest of the board leans on go first. */
    keys_box();                         /* if the MIE did not answer */
    eta_step(ETA_SDRAM);
    u32 usable = test_sdram_cells();
    suite_check_abort();
    eta_step(ETA_VRAM);
    u32 vram_ok = test_vram();
    suite_check_abort();
    eta_step(ETA_ARAM);
    u32 aram_ok = test_aram();
    suite_check_abort();

    /* Naomi 2 extra memories (no-op on other boards) */
    eta_step(ETA_N2);
    test_naomi2_ram();
    suite_check_abort();

    /* peripheral stage: backup SRAM (non-destructive), RTC, DIMM, MIE */
    /* From here on nothing measures anything: the NVRAM, the RTC, the DIMM
     * probe, Maple and the EEPROMs all answer yes or no. A bar left up would
     * sit at whatever the last test put it, which says less than no bar at
     * all -- so it goes, and the report takes the rows it occupied. */
    progress_retire();

    eta_step(ETA_PERIPH);
    test_sram_rtc();
    suite_check_abort();
    if (!g_maple_safe) {
        test_maple_mie(usable);
        test_settings_eeprom();
        test_jvs_io();
    }
    suite_check_abort();
    eta_step(ETA_SEEPROM);
    test_serial_eeprom();
    suite_check_abort();
    eta_step(ETA_END);

    /* The loops have been executing out of CPU RAM on a board whose CPU RAM
     * is what we just spent the whole suite testing. A cell that passed the
     * 8 KB qualification and then decayed would have corrupted the code
     * producing every result since, so the block is re-read and compared
     * against the master copy in ROM before anything is summarised. */
    if (reloc_active()) {
        u32 off, expect, got;
        if (reloc_verify(&off, &expect, &got)) {
            /* Serial only while intact: the screen is a line short on a
             * Naomi 2, and this one only matters when it fails. */
            log_result_q(S_L_RELOC_CHK, CLIP_NONE, T_OK, 0, 0, 1, CLIP_NONE);
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
        if (g_log[i].kind == LK_INFO || g_log[i].kind == LK_ICROW)
            continue;                   /* screen-only lines */
        scif_puts(g_log[i].name);
        scif_puts(g_log[i].status == T_OK ? S_SUM_OK :
                  g_log[i].status == T_SKIP ? S_SUM_SKIP : S_SUM_FAIL);
    }
    if (usable)
        scif_puts(S_MAINRAM_OK);
    else
        scif_puts(S_MAINRAM_KO);
    if (aram_ok)
        scif_puts(S_ARAM_OK_MSG);
    if (vram_ok)
        scif_puts(S_VRAM_OK_MSG);

    eta_finish();
    hint_retire();
    scif_puts(S_COMPLETE);
    say(CLIP_TESTS_DONE, REPORT_GAP_MS);
    scif_flush();

#if CFG_LANE_BEACON
    lane_beacon();                      /* never returns */
#else
    console_idle();                     /* never returns */
#endif
}
