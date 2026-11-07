#!/usr/bin/env python3
"""check_lang.py <lang> -- checks src/lang/<lang>.h against en.h, the base.

- the same S_* keys, no more, no fewer;
- on-screen strings fit: a result label (S_L_*) within 38 columns, the
  progress and menu labels within 40, FAIL/SKIP short enough to leave room;
- S_SCR_OK is 2 columns: the OK column is fixed (FB_W - 3 cells);
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
    if cols(cur.get("S_SCR_OK", "OK")) != 2:
        err.append(f"S_SCR_OK must be 2 columns: {cur.get('S_SCR_OK')!r}")
    for k in ("S_SCR_FAIL", "S_SCR_SKIP"):
        if k in cur and cols(cur[k]) > 12:
            err.append(f"{k} too long ({cols(cur[k])} columns)")
    if nums.get("NUM_JOIN", "0") not in ("0", "1", "2") or nums.get("NUM_UNIT_FIRST", "0") not in ("0", "1"):
        err.append(f"bad number rule {nums}")
    for e in err:
        print(f"{lang}: {e}")
    print(f"{lang}: {len(cur)} strings, {'OK' if not err else str(len(err)) + ' problem(s)'}")
    sys.exit(1 if err else 0)


main()
