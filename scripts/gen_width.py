#!/usr/bin/env python3
"""Regenerate src/tui/width.gen.inc, the display-width table of the TUI
(ARCHITECTURE.md section 16.2.4).

Sources (unicode.org, version UNICODE below):
  ucd/EastAsianWidth.txt               wide (W, F) and ambiguous (A)
  ucd/DerivedCoreProperties.txt        Grapheme_Extend, Default_Ignorable
  ucd/extracted/DerivedGeneralCategory.txt   Mn, Me, Mc, Cf, Cc
  ucd/auxiliary/GraphemeBreakProperty.txt    ZWJ, Regional_Indicator, V, T
  ucd/emoji/emoji-data.txt             Extended_Pictographic, Emoji_Presentation
  ucd/DerivedAge.txt                   what terminals with Unicode 9 tables know

Each code point gets:
  best  the Unicode answer (0, 1 or 2)            -> w_best, for scrollback
  max   the most any terminal is known to use     -> w_max, for the band
  cls   0 base, 1 extend (joins the previous cluster), 2 ZWJ, 3 regional
        indicator
  pict  Extended_Pictographic (for VS16 and ZWJ sequences)

Uncertain cases (max > best): East Asian ambiguous, pictographs that
default to text presentation, regional indicators, and zero-width marks
newer than Unicode 9 (older tables draw them one column wide), and format
characters (some terminals draw them). Unassigned
code points count 1, at most 2.

The output is one sorted uint32 per range: the first code point in bits
0-20 and the properties above it; a range runs up to the next one's start.
It is committed, so builds stay offline; run `make width` to refresh it.
"""
import re
import sys
import urllib.request

UNICODE = "17.0.0"
BASE = f"https://www.unicode.org/Public/{UNICODE}/ucd/"
OUT = "src/tui/width.gen.inc"
MAXCP = 0x110000


def fetch(path):
    with urllib.request.urlopen(BASE + path) as r:
        return r.read().decode("utf-8")


def ranges(text, want=None):
    """(lo, hi, value) for each data line; value is the second field."""
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(";")]
        if want and f[1] not in want:
            continue
        a, _, b = f[0].partition("..")
        yield int(a, 16), int(b or a, 16), f[1]


def missing_defaults(text):
    """@missing lines of EastAsianWidth.txt: default values for unlisted ranges."""
    for m in re.finditer(r"#\s*@missing:\s*([0-9A-F]+)\.\.([0-9A-F]+);\s*(\w+)", text):
        yield int(m.group(1), 16), int(m.group(2), 16), m.group(3)


def main():
    eaw_txt = fetch("EastAsianWidth.txt")
    core = fetch("DerivedCoreProperties.txt")
    gc_txt = fetch("extracted/DerivedGeneralCategory.txt")
    gb_txt = fetch("auxiliary/GraphemeBreakProperty.txt")
    emoji = fetch("emoji/emoji-data.txt")
    age_txt = fetch("DerivedAge.txt")

    eaw = ["N"] * MAXCP
    for lo, hi, v in missing_defaults(eaw_txt):
        for c in range(lo, hi + 1):
            eaw[c] = v
    for lo, hi, v in ranges(eaw_txt):
        for c in range(lo, hi + 1):
            eaw[c] = v
    gc = ["Cn"] * MAXCP
    for lo, hi, v in ranges(gc_txt):
        for c in range(lo, hi + 1):
            gc[c] = v
    extend = bytearray(MAXCP)
    ignorable = bytearray(MAXCP)
    for lo, hi, v in ranges(core, {"Grapheme_Extend", "Default_Ignorable_Code_Point"}):
        for c in range(lo, hi + 1):
            if v == "Grapheme_Extend":
                extend[c] = 1
            else:
                ignorable[c] = 1
    gb = [""] * MAXCP
    for lo, hi, v in ranges(gb_txt):
        for c in range(lo, hi + 1):
            gb[c] = v
    pict = bytearray(MAXCP)
    epres = bytearray(MAXCP)
    for lo, hi, v in ranges(emoji, {"Extended_Pictographic", "Emoji_Presentation"}):
        for c in range(lo, hi + 1):
            if v == "Extended_Pictographic":
                pict[c] = 1
            else:
                epres[c] = 1
    age = [None] * MAXCP
    for lo, hi, v in ranges(age_txt):
        major, minor = (int(x) for x in v.split("."))
        for c in range(lo, hi + 1):
            age[c] = (major, minor)

    def props(c):
        g = gc[c]
        if g == "Cn" and not pict[c]:  # unassigned
            if eaw[c] in ("W", "F"):  # reserved CJK blocks default to wide
                return 2, 2, 0, 0
            return 1, 2, 0, 0
        cls = 0
        if gb[c] == "ZWJ":
            cls = 2
        elif gb[c] == "Regional_Indicator":
            cls = 3
        elif extend[c] or gb[c] in ("Extend", "SpacingMark", "V", "T"):
            cls = 1
        if g in ("Mn", "Me", "Cc") or gb[c] in ("V", "T") or cls == 2:
            best = 0
        elif g == "Cf" and gb[c] != "Prepend" and c != 0xAD:
            best = 0  # format characters; prepended concatenation marks show
        elif ignorable[c] and g != "Lo":
            best = 0
        elif eaw[c] in ("W", "F") or epres[c]:
            best = 2
        else:
            best = 1
        mx = best
        if best == 1 and (eaw[c] == "A" or pict[c] or cls == 3):
            mx = 2
        if best == 0 and g != "Cc" and age[c] and age[c] > (9, 0):
            mx = 1
        if best == 0 and (g == "Cf" or ignorable[c]) and c not in (0x200D, 0xFE0E, 0xFE0F):
            mx = 1  # some terminals draw format characters
        return best, mx, cls, pict[c]

    out = []
    prev = None
    for c in range(MAXCP):
        p = props(c)
        if p != prev:
            best, mx, cls, pc = p
            out.append(c | best << 21 | mx << 23 | cls << 25 | pc << 27)
            prev = p

    with open(OUT, "w") as f:
        f.write(f"/* Generated by scripts/gen_width.py from Unicode {UNICODE}. Do not edit.\n")
        f.write(" * Each entry: first code point (bits 0-20), best width (21-22), max width\n")
        f.write(" * (23-24), cluster class (25-26), Extended_Pictographic (27). */\n")
        f.write(f"#define TUI_UNICODE \"{UNICODE}\"\n")
        f.write("static const uint32_t tui_wtab[] = {\n")
        for i in range(0, len(out), 6):
            f.write("    " + ", ".join(f"0x{x:08x}" for x in out[i:i + 6]) + ",\n")
        f.write("};\n")
    print(f"{OUT}: {len(out)} ranges", file=sys.stderr)


if __name__ == "__main__":
    main()
