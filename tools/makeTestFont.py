"""Writes tests/testFont.inc: a small font for the shaping tests, as a C array.

    python tools/makeTestFont.py      (needs fontTools: pip install fonttools)

The font ("Cascade Test") has what the tests look at, and nothing else:
- GSUB: liga f i -> f_i; smcp a -> a.sc; calt A -> A.alt after B (chained context, format 3);
  ccmp V U+FE0F -> V.emoji (U+FE0F passed over: it is matched as ignorable)
- GPOS: kern A V -100; mark acutecomb on A (anchor 250,700 on A, 0,0 on the mark)
- GDEF: classes (base, ligature, mark)
- fvar wght 100..900 (default 400) and gvar: at wght 900 glyph A is 200 units wider
- COLR / CPAL: heart (U+2764) is two layers, red and blue
- Devanagari (dev2): half ka (ka virama), the reph (ra virama), and the i matra, written before its
  consonant
- Arabic (arab): beh, alef and lam in their joined forms (isol, init, medi, fina: each its own width), lam alef by
  rlig, and fatha, a mark the letters join across
- for the other shapers: Thai KO KAI, SARA AA, NIKHAHIT and MAI EK (SARA AM split); Hangul KIYEOK, A and their syllable
  GA (jamo composed); Khmer KA, RO, COENG (pref: COENG RO), E and OE (split); Myanmar KA, E and MEDIAL RA (reordered);
  Javanese KA, RA, PANGKON (rphf: RA PANGKON, a repha) and TALING, a pre-base vowel, and the dotted circle; Tibetan DA,
  subjoined HA and DHA (ccmp splits DHA, blws joins it again); Sinhala KA, KOMBUVA, AELA-PILLA and O (KOMBUVA ...
  AELA-PILLA)
- calt on U+E000 U+E001 (ctxX ctxZ): a context calling a multiple substitution on the first (ctxX ctxY), then a single
  one on the second glyph of the input as it then is (ctxY, made ctxW; not ctxZ, which would be ctxW2)
And a second font ("Cascade Test Plain", kTestFontPlain) with no layout tables, whose Arabic letters are the Unicode
presentation forms: the joining done without the font's features.
Every glyph is a box (its advance wide, 700 high) but the mark, which is small.
"""
import os

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables import otTables
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.ttLib.tables.TupleVariation import TupleVariation
from fontTools.colorLib.builder import buildCOLR, buildCPAL

GLYPHS = [".notdef", "space", "A", "A.alt", "B", "V", "V.emoji", "a", "a.sc", "f", "i", "f_i", "acutecomb", "uniFE0F", "heart", "heart.l1", "heart.l2",
          "ka", "ra", "virama", "imatra", "ka.half", "reph",
          "beh", "beh.init", "beh.medi", "beh.fina", "alef", "alef.fina", "lam", "lam.init", "lam.medi", "lam.fina", "lam_alef",
          "lam_alef.fina", "fatha",
          "thKo", "thNikhahit", "thMaiEk", "thSaraAa", "hgK", "hgA", "hgGa", "kmKa", "kmE", "kmOe", "kmCoeng", "kmRo",
          "kmRoPref", "myKa", "myE", "myMedRa", "jvKa", "jvRa", "jvPangkon", "jvTaling", "jvRepha", "uni25CC", "tbDa",
          "tbSubHa", "tbDha", "snKa", "snKombuva", "snAaPilla", "snKo", "ctxX", "ctxY", "ctxZ", "ctxW", "ctxW2"]
WIDTHS = {"space": 300, "A": 600, "A.alt": 650, "B": 600, "V": 600, "V.emoji": 800, "a": 500, "a.sc": 550, "f": 300, "i": 250,
          "f_i": 520, "acutecomb": 0, "uniFE0F": 0, "heart": 800, "heart.l1": 800, "heart.l2": 800, ".notdef": 500,
          "ka": 600, "ra": 500, "virama": 0, "imatra": 300, "ka.half": 400, "reph": 0,
          "beh": 500, "beh.init": 400, "beh.medi": 300, "beh.fina": 450, "alef": 200, "alef.fina": 210, "lam": 520, "lam.init": 420,
          "lam.medi": 320, "lam.fina": 470, "lam_alef": 600, "lam_alef.fina": 610, "fatha": 0,
          "thKo": 600, "thNikhahit": 0, "thMaiEk": 0, "thSaraAa": 400, "hgK": 500, "hgA": 300, "hgGa": 900, "kmKa": 600,
          "kmE": 250, "kmOe": 260, "kmCoeng": 0, "kmRo": 550, "kmRoPref": 270, "myKa": 600, "myE": 240, "myMedRa": 280,
          "jvKa": 600, "jvRa": 560, "jvPangkon": 330, "jvTaling": 230, "jvRepha": 0, "uni25CC": 500, "tbDa": 600,
          "tbSubHa": 0, "tbDha": 610, "snKa": 600, "snKombuva": 220, "snAaPilla": 240, "snKo": 700, "ctxX": 500,
          "ctxY": 510, "ctxZ": 520, "ctxW": 530, "ctxW2": 540}
CMAP = {0x20: "space", 0x41: "A", 0x42: "B", 0x56: "V", 0x61: "a", 0x66: "f", 0x69: "i", 0x301: "acutecomb", 0xFE0F: "uniFE0F", 0x2764: "heart",
        0x915: "ka", 0x930: "ra", 0x94D: "virama", 0x93F: "imatra", 0x628: "beh", 0x627: "alef", 0x644: "lam", 0x64E: "fatha",
        0xE01: "thKo", 0xE4D: "thNikhahit", 0xE48: "thMaiEk", 0xE32: "thSaraAa", 0x1100: "hgK", 0x1161: "hgA",
        0xAC00: "hgGa", 0x1780: "kmKa", 0x17C1: "kmE", 0x17BE: "kmOe", 0x17D2: "kmCoeng", 0x179A: "kmRo",
        0x1000: "myKa", 0x1031: "myE", 0x103C: "myMedRa", 0xA98F: "jvKa", 0xA9AB: "jvRa", 0xA9C0: "jvPangkon",
        0xA9BA: "jvTaling", 0x25CC: "uni25CC", 0xF51: "tbDa", 0xFB7: "tbSubHa", 0xF52: "tbDha", 0xD9A: "snKa",
        0xDD9: "snKombuva", 0xDCF: "snAaPilla", 0xDDC: "snKo", 0xE000: "ctxX", 0xE001: "ctxZ"}

# the plain font: the letters and their presentation forms (isolated, final, initial, medial; lam alef, alone and final)
PLAIN = {0x20: ("space", 300), 0x628: ("beh", 500), 0xFE8F: ("behIsol", 500), 0xFE90: ("behFina", 450), 0xFE91: ("behInit", 400),
         0xFE92: ("behMedi", 300), 0x627: ("alef", 200), 0xFE8D: ("alefIsol", 200), 0xFE8E: ("alefFina", 210), 0x644: ("lam", 520),
         0xFEDD: ("lamIsol", 520), 0xFEDE: ("lamFina", 470), 0xFEDF: ("lamInit", 420), 0xFEE0: ("lamMedi", 320),
         0xFEFB: ("lamAlef", 600), 0xFEFC: ("lamAlefFina", 610)}


def box(w, h, x0=0, y0=0):
    pen = TTGlyphPen(None)
    if w > 0 and h > 0:
        pen.moveTo((x0, y0))
        pen.lineTo((x0, y0 + h))
        pen.lineTo((x0 + w, y0 + h))
        pen.lineTo((x0 + w, y0))
        pen.closePath()
    return pen.glyph()


def main():
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(GLYPHS)
    fb.setupCharacterMap(CMAP)
    glyphs = {}
    for g in GLYPHS:
        if g == "acutecomb":
            glyphs[g] = box(100, 100, -50, 0)
        elif g == "fatha":
            glyphs[g] = box(100, 100, -50, 800)
        elif g in ("thNikhahit", "thMaiEk"):
            glyphs[g] = box(100, 100, -150, 800)
        elif WIDTHS[g] == 0:
            glyphs[g] = box(0, 0)
        else:
            glyphs[g] = box(WIDTHS[g], 700)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({g: (WIDTHS[g], 0) for g in GLYPHS})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "Cascade Test", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    fb.setupFvar([("wght", 100, 400, 900, "Weight")], [])
    # at wght 900 (normalized 1): A's right side 200 units further (its two right points and the advance's phantom point)
    a = fb.font["glyf"]["A"]
    coords = list(a.coordinates)
    deltas = [(200, 0) if x == 600 else (0, 0) for x, y in coords] + [(0, 0), (200, 0), (0, 0), (0, 0)]
    fb.setupGvar({"A": [TupleVariation({"wght": (0, 1.0, 1.0)}, deltas)]})
    addOpenTypeFeaturesFromString(fb.font, """
        languagesystem DFLT dflt;
        languagesystem latn dflt;
        languagesystem dev2 dflt;
        languagesystem arab dflt;
        languagesystem thai dflt;
        languagesystem khmr dflt;
        languagesystem mym2 dflt;
        languagesystem java dflt;
        languagesystem tibt dflt;
        languagesystem sinh dflt;
        @bases = [A A.alt B V V.emoji a a.sc f i heart ka ra imatra ka.half reph virama beh beh.init beh.medi beh.fina alef alef.fina
                  lam lam.init lam.medi lam.fina];
        table GDEF { GlyphClassDef @bases, [f_i lam_alef lam_alef.fina], [acutecomb fatha thNikhahit thMaiEk], ; } GDEF;
        feature ccmp { sub V uniFE0F by V.emoji; } ccmp;
        feature liga { sub f i by f_i; } liga;
        feature smcp { sub a by a.sc; } smcp;
        feature calt { sub B A' by A.alt; } calt;
        feature kern { pos A V -100; } kern;
        feature rphf { script dev2; sub ra virama by reph; } rphf;
        feature half { script dev2; sub ka virama by ka.half; } half;
        feature init { script arab; sub beh by beh.init; sub lam by lam.init; } init;
        feature medi { script arab; sub beh by beh.medi; sub lam by lam.medi; } medi;
        feature fina { script arab; sub beh by beh.fina; sub alef by alef.fina; sub lam by lam.fina; } fina;
        feature rlig { script arab; sub lam.init alef.fina by lam_alef; sub lam.medi alef.fina by lam_alef.fina; } rlig;
        feature pref { script khmr; sub kmCoeng kmRo by kmRoPref; } pref;
        lookup ctxAdd { sub ctxX by ctxX ctxY; } ctxAdd;
        lookup ctxChange { sub ctxY by ctxW; sub ctxZ by ctxW2; } ctxChange;
        feature calt { sub ctxX' lookup ctxAdd ctxZ' lookup ctxChange; } calt;
        feature rphf { script java; sub jvRa jvPangkon by jvRepha; } rphf;
        feature ccmp { script tibt; sub tbDha by tbDa tbSubHa; } ccmp;
        feature blws { script tibt; sub tbDa tbSubHa by tbDha; } blws;
        markClass acutecomb <anchor 0 0> @TOP;
        feature mark { pos base A <anchor 250 700> mark @TOP; } mark;
    """)
    fb.font["COLR"] = buildCOLR({"heart": [("heart.l1", 0), ("heart.l2", 1)]})
    fb.font["CPAL"] = buildCPAL([[(1.0, 0.0, 0.0, 1.0), (0.0, 0.0, 1.0, 1.0)]])
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "testFont.ttf")
    fb.save(path)
    data = open(path, "rb").read()
    os.remove(path)
    lines = ", ".join("0x%02x" % b for b in data)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "testFont.inc")
    with open(out, "w", newline="\n") as f:
        f.write("/* Generated by tools/makeTestFont.py: do not edit. */\nstatic const unsigned char kTestFont[] = {%s};\n" % lines)
    print("%d bytes" % len(data))
    plain()


def plain():
    order = [".notdef"] + [g for g, _ in PLAIN.values()]
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({cp: g for cp, (g, _) in PLAIN.items()})
    widths = {".notdef": 500}
    widths.update({g: w for g, w in PLAIN.values()})
    fb.setupGlyf({g: box(widths[g], 700) if g != "space" else box(0, 0) for g in order})
    fb.setupHorizontalMetrics({g: (widths[g], 0) for g in order})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "Cascade Test Plain", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "testFontPlain.ttf")
    fb.save(path)
    data = open(path, "rb").read()
    os.remove(path)
    lines = ", ".join("0x%02x" % b for b in data)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "testFontPlain.inc")
    with open(out, "w", newline="\n") as f:
        f.write("/* Generated by tools/makeTestFont.py: do not edit. */\nstatic const unsigned char kTestFontPlain[] = {%s};\n" % lines)
    print("plain: %d bytes" % len(data))


main()
