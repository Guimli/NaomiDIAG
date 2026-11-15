#!/usr/bin/env python3
"""check_lang.py <lang> -- checks src/lang/<lang>.h against en.h, the base.

- the same S_* keys, no more, no fewer;
- on-screen strings fit: a result label (S_L_*) within 38 columns, the
  progress and menu labels within 40, FAIL/SKIP short enough to leave room;
- S_SCR_OK is 2 columns: the OK column is fixed (FB_W - 3 cells);
- the strings src/main.c copies into fixed buffers fit them in bytes (a kana
  or hanzi is 3 bytes of UTF-8 for one column);
- the number rule (NUM_UNIT_FIRST / NUM_JOIN) is valid.
Columns are characters (one 16-px cell each, ASCII or not)."""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src", "lang")


def parse(path):
    text = open(path, encoding="utf-8").read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = text.replace("\\\n", " ")
    d = {}
    for m in re.finditer(r"^#define\s+(S_\w+)\s+(.*)$", text, re.M):
        lits = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2))
        d[m.group(1)] = "".join(re.sub(r"\\(.)", lambda e: {"n": "\n", "r": "\r"}.get(e.group(1), e.group(1)), x) for x in lits)
    nums = dict(re.findall(r"^#define\s+(NUM_UNIT_FIRST|NUM_JOIN)\s+(\d+)", text, re.M))
    return d, nums


def main():
    lang = sys.argv[1]
    base, _ = parse(os.path.join(SRC, "en.h"))
    cur, nums = parse(os.path.join(SRC, lang + ".h"))
    err = []
    miss = sorted(set(base) - set(cur))
    extra = sorted(set(cur) - set(base))
    if miss:
        err.append(f"missing keys: {' '.join(miss)}")
    if extra:
        err.append(f"extra keys: {' '.join(extra)}")
    cols = lambda s: len(s.strip("\n"))
    for k, v in cur.items():
        if "\n" in v.strip("\n"):
            continue                    # multi-line: serial text
        lim = 38 if k.startswith("S_L_") else 40 if k.startswith(("S_P_", "S_M_", "S_SCR_")) else None
        if lim and cols(v) > lim:
            err.append(f"{k}: {cols(v)} columns > {lim}: {v!r}")
    # A result label shares its line with OK (FB_STATUS_X: 33 columns) or
    # FAIL (38 - len(FAIL) columns, chips aside). One too long drops its
    # " (...)" part first (fit_label in src/main.c): what is left must fit.
    room = min(33, 38 - cols(cur.get("S_SCR_FAIL", "FAIL")))
    for k, v in cur.items():
        if k.startswith("S_L_"):
            core = v.split(" (")[0] if " (" in v else v
            if cols(core) > room:
                err.append(f"{k}: {cols(core)} columns without its parenthesis > {room}: {v!r}")
    if cols(cur.get("S_SCR_OK", "OK")) != 2:
        err.append(f"S_SCR_OK must be 2 columns: {cur.get('S_SCR_OK')!r}")
    for k in ("S_SCR_FAIL", "S_SCR_SKIP"):
        if k in cur and cols(cur[k]) > 12:
            err.append(f"{k} too long ({cols(cur[k])} columns)")
    # Strings that src/main.c copies into fixed byte buffers: in bytes, not
    # columns (a kana or hanzi is 3 bytes of UTF-8 for one column). Past
    # these, a copy overflows its buffer or is cut in the middle of a
    # character. Each entry: the keys whose lengths add up, the limit, where.
    b = lambda k: len(cur.get(k, "").encode("utf-8"))
    budgets = [((k,), 40, "pass_label base") for k in cur if k.startswith("S_P_")]
    budgets += [
        (("S_P_PASS",), 12, "pass_label suffix"),
        (("S_P_CART_IC",), 24, "cart_label"),
        (("S_ADDR_CHIPS",), 18, "chips[24] on screen"),
        (("S_SCR_PIN",), 8, "chips[24] on screen"),
        (("S_SCR_NICS",), 8, "chips[24] on screen"),
        (("S_L_DP_READ",), 31, "g_dimmbuf[40] + GDDIR=n"),
        (("S_L_DP_WRITE",), 31, "g_dimmbuf[40] + GDDIR=n"),
        (("S_DH_FW", "S_DH_MB"), 28, "g_dimmbuf[40] with version and size"),
        (("S_DIMM_SIZE_UNK",), 39, "detail[40]"),
        (("S_DH_MB", "S_SCR_DIMM_ETA"), 27, "detail[40] with size and h:mm:ss"),
        (("S_SCR_FW_S1",), 19, "fw menu label prefix"),
        (("S_SCR_GAME",), 15, "g_gamebuf[48] with the title"),
        (("S_LP_PASS",), 30, "loop screen buf[48]"),
        (("S_LP_TIME",), 30, "loop screen buf[48]"),
        (("S_LP_ERRS",), 30, "loop screen buf[48]"),
        (("S_LP_FIRST",), 33, "loop screen buf[48] with two numbers"),
        (("S_EXC_SCREEN",), 20, "exception line, cut at 20 bytes"),
        (("S_SCR_FW_EXPECT",), 30, "g_fw_expect[40], cut at 30 bytes, then the version"),
    ]
    pl = cur.get("S_JT_PLAYER", "P")
    if len(pl) != 1 or ord(pl) > 0x7F:
        err.append(f"S_JT_PLAYER must be one ASCII letter (only its first byte is drawn): {pl!r}")
    dirs = [cur.get(f"S_JT_DIR_{d}") for d in "UDLR"]
    if len(set(dirs)) != 4:
        err.append(f"S_JT_DIR_U/D/L/R must differ: {dirs}")
    for keys, lim, where in budgets:
        n = sum(b(k) for k in keys)
        if n > lim:
            err.append(f"{'+'.join(keys)}: {n} bytes > {lim} ({where})")
    # (the report names, g_cut_name[32], are cut by put_str at 31 bytes on a
    # character boundary: a long one loses its end, never overflows)
    if nums.get("NUM_JOIN", "0") not in ("0", "1", "2") or nums.get("NUM_UNIT_FIRST", "0") not in ("0", "1"):
        err.append(f"bad number rule {nums}")
    for e in err:
        print(f"{lang}: {e}")
    print(f"{lang}: {len(cur)} strings, {'OK' if not err else str(len(err)) + ' problem(s)'}")
    sys.exit(1 if err else 0)


main()
