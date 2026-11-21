"""Runs Unicode's text rendering tests (https://github.com/unicode-org/text-rendering-tests) on cascade's shaping.

    python tools/textRenderingTests.py path/to/cascade-shape path/to/text-rendering-tests [--known FILE] [PREFIX ...]
    python tools/textRenderingTests.py --harfbuzz path/to/text-rendering-tests [PREFIX ...]

(--harfbuzz: HarfBuzz's shaping instead, through uharfbuzz: which cases the reference shaper passes. --known: the
cases known to fail, one a line, a case's id or the start of ids ("MORX-"), "#" and what follows a comment; those
failing count as known, and a case listed that passes is an error too, so that the list stays what fails.)

Needs fontTools (pip install fonttools). Each test case (testcases/*.html: a text, a font, and the glyphs it should be
drawn with, by name or index, at their places, as SVG) whose id starts with one of the prefixes (all of them when none is given) is shaped
by cascade-shape; it passes when the glyphs that draw something are the expected ones, each within a unit of its
place (the SVGs' units: thousandths of an em). The cases of a variable font's instance (ft:var) are passed over: cascade-shape shapes the default instance.
Prints each failure and a count; exits 1 when a case failed (one not known to).
"""
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as etree

from fontTools.pens.boundsPen import BoundsPen
from fontTools.ttLib import TTFont

FT = "{https://github.com/OpenType/fonttest}"
XLINK = "{http://www.w3.org/1999/xlink}"
RTL = re.compile("[֐-ࣿיִ-﷿ﹰ-﻿\U00010800-\U00010FFF\U0001E800-\U0001EFFF]")


def expected_glyphs(td):
    """The glyphs the case's SVG draws: (glyph name or "gidN", x, y), each referring to "#<case id>.<glyph>"."""
    prefix = "#" + td.attrib[FT + "id"] + "."
    glyphs = []
    for use in td.iter("use"):
        href = use.attrib.get(XLINK + "href", "")
        if href.startswith(prefix):
            glyphs.append((href[len(prefix):], round(float(use.attrib.get("x", 0))), round(float(use.attrib.get("y", 0)))))
    return glyphs


def shape_harfbuzz(path, texts):
    """The texts shaped by HarfBuzz, as cascade-shape prints them."""
    import uharfbuzz as hb
    font = hb.Font(hb.Face(open(path, "rb").read()))
    lines = []
    for text in texts:
        buf = hb.Buffer()
        buf.add_str(text)
        buf.guess_segment_properties()
        hb.shape(font, buf)
        x, out = 0, []
        for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
            out.append("%d:%d@%d,%d+%d" % (info.codepoint, info.cluster, x + pos.x_offset, pos.y_offset, pos.x_advance))
            x += pos.x_advance
        lines.append(" ".join(out))
    return lines


class Inks:
    """Which glyphs of a font draw something (the expected SVGs leave out the empty ones)."""

    def __init__(self, path):
        self.font = TTFont(path, lazy=True)
        self.order = self.font.getGlyphOrder()
        self.gs = self.font.getGlyphSet()
        self.cache = {}
        self.upem = self.font["head"].unitsPerEm
        self.ids = {name: i for i, name in enumerate(self.order)}

    def gid(self, glyph):
        """A glyph's index, from its name or "gidN"."""
        if glyph in self.ids:
            return self.ids[glyph]
        return int(glyph[3:]) if glyph.startswith("gid") and glyph[3:].isdigit() else -2

    def inked(self, gid):
        if gid not in self.cache:
            pen = BoundsPen(self.gs)
            if 0 <= gid < len(self.order):
                self.gs[self.order[gid]].draw(pen)
            self.cache[gid] = pen.bounds is not None
        return self.cache[gid]


def main():
    args = sys.argv[1:]
    known = []
    if "--known" in args:
        k = args.index("--known")
        with open(args[k + 1], encoding="utf-8") as f:
            known = [line.split("#", 1)[0].strip() for line in f]
        known = [x for x in known if x]
        del args[k:k + 2]
    if len(args) < 2:
        print(__doc__)
        return 2
    tool, root = args[0], args[1]
    harfbuzz = tool == "--harfbuzz"
    prefixes = args[2:]
    is_known = lambda cid: any(cid == x or (x.endswith(("-", "/")) and cid.startswith(x)) for x in known)
    cases = []  # (id, font, text, expected)
    tests = os.path.join(root, "testcases")
    for name in sorted(os.listdir(tests)):
        if not name.endswith(".html") or (prefixes and not any(name.startswith(p) for p in prefixes)):
            continue
        doc = etree.parse(os.path.join(tests, name))
        for td in doc.iter():
            if td.attrib.get("class") != "expected" or FT + "render" not in td.attrib or FT + "var" in td.attrib:
                continue
            cases.append((td.attrib[FT + "id"], td.attrib[FT + "font"], td.attrib[FT + "render"], expected_glyphs(td)))
    passed = failed = known_failed = 0
    stale = []
    by_font = {}
    for case in cases:
        by_font.setdefault((case[1], bool(RTL.search(case[2]))), []).append(case)
    for (fontname, rtl), group in sorted(by_font.items()):
        path = os.path.join(root, "fonts", fontname)
        inks = Inks(path)
        inp = "".join(" ".join("%X" % ord(c) for c in text) + "\n" for _, _, text, _ in group)
        if harfbuzz:
            lines = shape_harfbuzz(path, [text for _, _, text, _ in group])
        else:
            res = subprocess.run([tool, path] + (["--rtl"] if rtl else []), input=inp, capture_output=True, text=True)
            lines = res.stdout.splitlines() if res.returncode == 0 else []
        for k, (cid, _, text, want) in enumerate(group):
            want = [(inks.gid(g), x, y) for g, x, y in want]  # (the glyphs that draw nothing: on neither side)
            got = []
            if k < len(lines):
                for tok in lines[k].split():
                    g, _, rest = tok.partition(":")
                    if g == "?":
                        got.append((-1, 0, 0))
                        continue
                    _, _, pos = rest.partition("@")
                    xy, _, _adv = pos.partition("+")
                    x, _, y = xy.rpartition(",")
                    if inks.inked(int(g)):  # (the SVGs' places: in thousandths of an em)
                        got.append((int(g), round(int(x) * 1000 / inks.upem), round(int(y) * 1000 / inks.upem)))
            want = [w for w in want if w[0] < 0 or inks.inked(w[0])]
            ok = k < len(lines) and len(got) == len(want) and all(
                a[0] == b[0] and abs(a[1] - b[1]) <= 1 and abs(a[2] - b[2]) <= 1 for a, b in zip(got, want))
            if ok:
                passed += 1
                if is_known(cid) and cid in known:
                    stale.append(cid)
            elif is_known(cid):
                known_failed += 1
            else:
                failed += 1
                print("FAIL %s %s\n  want %s\n  got  %s" % (cid, " ".join("%04X" % ord(c) for c in text),
                      " ".join("%d@%d,%d" % w for w in want), " ".join("%d@%d,%d" % g for g in got)))
    for cid in stale:
        print("PASS %s: listed as failing (take it off the list)" % cid)
    print("%d of %d cases pass (%d failing as known, %d not)" % (passed, passed + failed + known_failed, known_failed, failed))
    return 1 if failed or stale else 0


if __name__ == "__main__":
    sys.exit(main())
