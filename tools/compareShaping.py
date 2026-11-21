"""Compares cascade's shaping with HarfBuzz's, on a font and texts of a script (a developer's check, not a test).

    python tools/compareShaping.py path/to/cascade-shape [script ...]

Needs uharfbuzz (pip install uharfbuzz). For each script below: its font (the system's; a collection's as "file.ttc:index"), texts made of its syllables
(random, seeded: consonants, what joins under or after them, the vowels and marks) and some words, each shaped by both;
a text whose glyphs (and clusters) differ is printed, with how many of each script's texts agree.
"""
import os
import random
import subprocess
import sys
import unicodedata

import uharfbuzz as hb

FONTS = "C:/Windows/Fonts" if os.name == "nt" else "/usr/share/fonts"


def r(a, b):
    """The assigned characters from a to b."""
    return [c for c in range(a, b + 1) if unicodedata.category(chr(c)) != "Cn"]


# script: (font file, rtl, syllable parts: lists of code points a syllable takes 0..n of, in order, and words)
SCRIPTS = {
    "thai": ("LeelawUI.ttf", False,
             [(r(0x0E40, 0x0E44), 0, 1), (r(0x0E01, 0x0E2E), 1, 1), ([0x0E31] + r(0x0E34, 0x0E3A), 0, 1), (r(0x0E48, 0x0E4C), 0, 1),
              ([0x0E30, 0x0E32, 0x0E33, 0x0E45], 0, 1)],
             ["0E01 0E33", "0E19 0E49 0E33", "0E01 0E34 0E48 0E19", "0E2A 0E27 0E31 0E2A 0E14 0E35"]),
    "lao": ("LeelawUI.ttf", False,
            [(r(0x0EC0, 0x0EC4), 0, 1), (r(0x0E81, 0x0EAE), 1, 1), ([0x0EB1] + r(0x0EB4, 0x0EBC), 0, 1), (r(0x0EC8, 0x0ECB), 0, 1),
             ([0x0EB0, 0x0EB2, 0x0EB3], 0, 1)],
            ["0E81 0EB3", "0E99 0EC9 0EB3", "0EAA 0EB0 0E9A 0EB2 0E8D 0E94 0EB5"]),
    "khmer": ("LeelawUI.ttf", False,
              [(r(0x1780, 0x17A2), 1, 1), ([0x17D2], 0, 1), (r(0x1780, 0x17A2), 0, 0), (r(0x17B6, 0x17C5), 0, 1), (r(0x17C6, 0x17C8), 0, 1)],
              ["1781 17D2 1798 17C2 179A", "179F 17D2 179A 17B8", "1780 17D2 179A 17C4 1798", "1796 17D2 179A 17C7"]),
    "myanmar": ("mmrtext.ttf", False,
                [(r(0x1000, 0x1021), 1, 1), ([0x1039], 0, 1), (r(0x1000, 0x1021), 0, 0), ([0x103B], 0, 1), ([0x103C], 0, 1), ([0x103D], 0, 1),
                 ([0x103E], 0, 1), ([0x1031], 0, 1), ([0x102D, 0x102E, 0x1032], 0, 1), ([0x102F, 0x1030], 0, 1), ([0x102B, 0x102C], 0, 1),
                 ([0x1036], 0, 1), ([0x1037], 0, 1), ([0x103A], 0, 1), ([0x1038], 0, 1)],
                ["1019 103C 1014 103A 1019 102C", "1000 103B 1031 102C 1004 103A 1038", "1004 103A 1039 1002 102B"]),
    "tibetan": ("himalaya.ttf", False,
                [(r(0x0F40, 0x0F6C), 1, 1), (r(0x0F90, 0x0FBC), 0, 2), (r(0x0F71, 0x0F7D) + [0x0F80], 0, 1), ([0x0F7E, 0x0F82, 0x0F83], 0, 1)],
                ["0F56 0F7C 0F51 0F0B", "0F66 0F90 0F7A 0F51", "0F56 0F66 0F92 0F7C 0F58 0F66"]),
    "hangul": ("malgun.ttf", False,
               [(r(0x1100, 0x1112), 1, 1), (r(0x1161, 0x1175), 1, 1), (r(0x11A8, 0x11C2), 0, 1)],
               ["1112 1161 11AB 1100 1173 11AF", "1100 1161", "110B 1175 11AF"]),
    "javanese": ("javatext.ttf", False,
                 [(r(0xA984, 0xA9B2), 1, 1), ([0xA9C0], 0, 1), (r(0xA984, 0xA9B2), 0, 0), (r(0xA9BD, 0xA9BF), 0, 1), (r(0xA9B4, 0xA9BC), 0, 1),
                  (r(0xA980, 0xA983), 0, 1)],
                 ["A9A0 A9BA A9A4 A9C0", "A98F A9C0 A9B2 A9BC"]),
    "sinhala": ("Nirmala.ttc:0", False,
                [(r(0x0D9A, 0x0DC6), 1, 1), ([0x0DCA], 0, 1), (r(0x0D9A, 0x0DC6), 0, 0),
                 ([0x0DCF] + r(0x0DD0, 0x0DD4) + [0x0DD6] + r(0x0DD8, 0x0DDF), 0, 1), ([0x0D82, 0x0D83], 0, 1)],
                ["0DC1 0DCA 0DBB 0DD3", "0D9A 0DCA 0DC2 0DD9"]),
    "devanagari": ("Nirmala.ttc:0", False,
                   [(r(0x0915, 0x0939), 1, 1), ([0x094D], 0, 1), (r(0x0915, 0x0939), 0, 0), (r(0x093E, 0x094C), 0, 1),
                    ([0x0901, 0x0902], 0, 1)],
                   ["0915 094D 0937 093F", "0930 094D 0915 0940", "0926 094D 0935 093E"]),
    "telugu": ("Nirmala.ttc:0", False,
               [(r(0x0C15, 0x0C39), 1, 1), ([0x0C4D], 0, 1), (r(0x0C15, 0x0C39), 0, 0), (r(0x0C3E, 0x0C4C), 0, 1),
                ([0x0C02], 0, 1)],
               ["0C15 0C4D 0C37 0C3F", "0C38 0C4D 0C24 0C41"]),
    "kannada": ("Nirmala.ttc:0", False,
                [(r(0x0C95, 0x0CB9), 1, 1), ([0x0CCD], 0, 1), (r(0x0C95, 0x0CB9), 0, 0), (r(0x0CBE, 0x0CCC), 0, 1),
                 ([0x0C82], 0, 1)],
                ["0CA8 0CCD 0CA8 0CBE", "0C95 0CCD 0CB7 0CBF"]),
    "newtailue": ("ntailu.ttf", False,
                  [(r(0x1980, 0x19AB), 1, 1), (r(0x19B0, 0x19C0), 0, 1), (r(0x19C1, 0x19C7), 0, 1), ([0x19C8, 0x19C9], 0, 1)],
                  ["1991 19B1 19C8", "19B5 1980 19C1"]),
}


def syllables(parts, rng, n):
    out = []
    for _ in range(n):
        s = []
        for i, (cps, lo, hi) in enumerate(parts):
            if hi == 0:  # (the consonant after a joiner, when there is one)
                if s and s[-1] in parts[i - 1][0]:
                    s.append(rng.choice(cps))
                continue
            for _k in range(rng.randint(lo, hi)):
                s.append(rng.choice(cps))
        # (Myanmar: an asat kills a consonant, in a syllable with no vowel; HarfBuzz marks it broken elsewhere)
        if 0x1032 in s:  # (AI takes no vowel below)
            s = [c for c in s if c not in (0x102F, 0x1030)]
        if 0x103A in s and any(0x102B <= c <= 0x1036 for c in s):
            s = [c for c in s if c != 0x103A]
        out.append(s)
    return out


def harfbuzz(face_bytes, index, texts, rtl):
    face = hb.Face(face_bytes, index)
    font = hb.Font(face)
    upem = face.upem
    font.scale = (upem, upem)
    lines = []
    for cps in texts:
        buf = hb.Buffer()
        buf.add_codepoints(cps)
        buf.guess_segment_properties()
        if rtl:
            buf.direction = "rtl"
        hb.shape(font, buf)
        x = 0
        items = []
        for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
            items.append((info.codepoint, info.cluster, x + pos.x_offset, pos.y_offset, pos.x_advance))
            x += pos.x_advance
        lines.append(items)
    return lines


def cascade(tool, path, index, texts, rtl):
    inp = "\n".join(" ".join("%X" % c for c in t) for t in texts) + "\n"
    res = subprocess.run([tool, path, "--index", str(index)] + (["--rtl"] if rtl else []), input=inp, capture_output=True, text=True, check=True)
    lines = []
    for line in res.stdout.splitlines():
        items = []
        for tok in line.split():
            g, _, rest = tok.partition(":")
            if g == "?":
                items.append((-1, int(rest), 0, 0, 0))
                continue
            c, _, pos = rest.partition("@")
            xy, _, adv = pos.partition("+")
            x, _, y = xy.rpartition(",")
            items.append((int(g), int(c), int(x), int(y), int(adv)))
        lines.append(items)
    return lines


def main():
    tool = sys.argv[1]
    wanted = sys.argv[2:] or list(SCRIPTS)
    rng = random.Random(1)
    total_ok = total = 0
    for name in wanted:
        fontfile, rtl, parts, words = SCRIPTS[name]
        fontfile, _, index = fontfile.partition(":")  # (a collection's font: "file.ttc:index")
        index = int(index or 0)
        path = os.path.join(FONTS, fontfile)
        if not os.path.exists(path):
            print("%s: no %s" % (name, fontfile))
            continue
        texts = [[int(x, 16) for x in w.split()] for w in words]
        for _ in range(150):
            texts.append([c for s in syllables(parts, rng, rng.randint(1, 3)) for c in s])
        data = open(path, "rb").read()
        want = harfbuzz(data, index, texts, rtl)
        got = cascade(tool, path, index, texts, rtl)
        ok = 0
        shown = 0
        for t, w, g in zip(texts, want, got):
            same_glyphs = [x[0] for x in w] == [x[0] for x in g]
            same_places = same_glyphs and all(abs(a[2] - b[2]) <= 1 and abs(a[3] - b[3]) <= 1 for a, b in zip(w, g))
            if same_places:
                ok += 1
            elif shown < 6:
                shown += 1
                print("  %s %s\n    harfbuzz %s\n    cascade  %s%s" % (name, " ".join("%04X" % c for c in t),
                      " ".join("%d:%d" % (x[0], x[1]) for x in w), " ".join("%d:%d" % (x[0], x[1]) for x in g),
                      "" if not same_glyphs else "\n    (places: %s vs %s)" % ([(x[2], x[3]) for x in w], [(x[2], x[3]) for x in g])))
        print("%s: %d of %d texts as HarfBuzz shapes them" % (name, ok, len(texts)))
        total_ok += ok
        total += len(texts)
    print("all: %d of %d" % (total_ok, total))


if __name__ == "__main__":
    main()
