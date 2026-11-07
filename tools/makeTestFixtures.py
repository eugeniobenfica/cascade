"""Writes tests/testFixtures.inc: what the tests decode, as C arrays.

    python tools/makeTestFixtures.py      (needs fontTools and Pillow: pip install fonttools pillow)

- JPEGs of a 24 x 16 picture: baseline 4:4:4, baseline 4:2:0, progressive 4:2:0, with restart markers,
  and grayscale; each with what libjpeg decodes it to at some pixels, so that the decoder is checked
  against another one and not against itself
- "Cascade Pixels": an OpenType font with CFF outlines, curves on every letter, for the pixel proof:
  a page that uses it paints the same on a machine with no fonts of its own
- a font with an sbix picture and one with a CBDT picture, on the letter A (their tables are written
  here byte by byte: only the layout the specifications give, and nothing of the reader's)
"""
import io
import os
import struct

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables.DefaultTable import DefaultTable
from PIL import Image

W, H = 24, 16
SAMPLES = [(2, 2), (10, 3), (21, 2), (4, 9), (12, 8), (20, 13), (3, 14), (14, 14)]


def picture():
    img = Image.new("RGB", (W, H))
    for y in range(H):
        for x in range(W):
            if x < 8:
                c = (200 + y * 3, 40, 30)
            elif x < 16:
                c = (30, 90 + x * 6, 60 + y * 8)
            else:
                c = (x * 8, y * 12, 230)
            img.putpixel((x, y), c)
    return img


def jpeg(img, **options):
    out = io.BytesIO()
    img.save(out, "JPEG", **options)
    data = out.getvalue()
    decoded = Image.open(io.BytesIO(data)).convert("RGB")
    return data, [(x, y) + decoded.getpixel((x, y)) for x, y in SAMPLES]


def cff_font():
    letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
    order = [".notdef", "space", "period"] + ["g%02X" % ord(c) for c in letters]
    cmap = {0x20: "space", 0x2E: "period"}
    cmap.update({ord(c): "g%02X" % ord(c) for c in letters})
    strings, metrics = {}, {}
    for name in order:
        pen = T2CharStringPen(600, None)
        if name == "period":
            blob(pen, 300, 70, 70, 70, 0)
        elif name.startswith("g"):
            k = int(name[1:], 16)
            rx, ry = 180 + k * 7 % 90, 260 + k * 11 % 90
            blob(pen, 300, 350, rx, ry, k % 5 * 12)
            blob(pen, 300 + k % 7 * 8, 350, rx // 2, -(ry // 2), 0) # (the other way round: a hole)
        elif name == ".notdef":
            pen.moveTo((60, 0)); pen.lineTo((540, 0)); pen.lineTo((540, 700)); pen.lineTo((60, 700)); pen.closePath()
        strings[name] = pen.getCharString()
        metrics[name] = (600 if name != "space" else 300, 0)
    fb = FontBuilder(1000, isTTF=False)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupCFF("CascadePixels-Regular", {"FullName": "Cascade Pixels"}, strings, {})
    return finish(fb, metrics, "Cascade Pixels")


def blob(pen, cx, cy, rx, ry, lean):
    """A closed shape of four cubic curves; ry below zero turns it the other way."""
    k = 0.55
    down = cy - ry
    up = cy + ry
    pen.moveTo((cx + rx, cy))
    pen.curveTo((cx + rx, cy + ry * k), (cx + rx * k + lean, up), (cx + lean, up))
    pen.curveTo((cx - rx * k + lean, up), (cx - rx, cy + ry * k), (cx - rx, cy))
    pen.curveTo((cx - rx, cy - ry * k), (cx - rx * k - lean, down), (cx - lean, down))
    pen.curveTo((cx + rx * k - lean, down), (cx + rx, cy - ry * k), (cx + rx, cy))
    pen.closePath()


def finish(fb, metrics, family):
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": family, "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    fb.updateHead(created=0, modified=0)
    fb.font.recalcTimestamp = False
    out = io.BytesIO()
    fb.save(out)
    return out.getvalue()


def bitmap_font(family, tables):
    """A TrueType font of .notdef, space and an A with no outline; tables(png, glyphs) gives the rest."""
    order = [".notdef", "space", "A"]
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({0x20: "space", 0x41: "A"})
    fb.setupGlyf({name: TTGlyphPen(None).glyph() for name in order})
    png = io.BytesIO()
    Image.new("RGBA", (16, 16), (255, 0, 0, 255)).save(png, "PNG")
    for tag, data in tables(png.getvalue(), len(order)).items():
        table = DefaultTable(tag)
        table.data = data
        fb.font[tag] = table
    return finish(fb, {".notdef": (1000, 0), "space": (500, 0), "A": (1000, 0)}, family)


def sbix(png, glyphs):
    # a strike: ppem, ppi, each glyph's offset, the glyph's data (x, y, "png ", the picture)
    data = struct.pack(">hh4s", 0, 0, b"png ") + png
    first = 4 + 4 * (glyphs + 1)
    offsets = [first, first, first, first + len(data)] # (only glyph 2, the A, has data)
    strike = struct.pack(">HH", 16, 72) + b"".join(struct.pack(">I", o) for o in offsets) + data
    return {"sbix": struct.pack(">HHII", 1, 1, 1, 12) + strike}


def cbdt(png, glyphs):
    # format 17: small metrics (height, width, x, y, advance), the length, the picture
    glyph = struct.pack(">BBbbBI", 16, 16, 0, 16, 16, len(png)) + png
    table = struct.pack(">HH", 3, 0) + glyph
    # one strike, one index subtable of format 1 (offsets) for glyph 2
    sub = struct.pack(">HHI", 1, 17, 4) + struct.pack(">II", 0, len(glyph))
    array = struct.pack(">HHI", 2, 2, 8) + sub
    metrics = bytes(12)
    size = struct.pack(">IIII", 8 + 48, len(array), 1, 0) + metrics + metrics + struct.pack(">HHBBBB", 2, 2, 16, 16, 32, 1)
    return {"CBDT": table, "CBLC": struct.pack(">HHI", 3, 0, 1) + size + array}


def array(out, name, data):
    out.write("static const unsigned char %s[] = {" % name)
    out.write(",".join(str(b) for b in data))
    out.write("};\n")


def main():
    img = picture()
    jpegs = {
        "kJpeg444": jpeg(img, quality=92, subsampling=0),
        "kJpeg420": jpeg(img, quality=92, subsampling=2),
        "kJpegProgressive": jpeg(img, quality=92, subsampling=2, progressive=True),
        "kJpegRestart": jpeg(img, quality=92, subsampling=0, restart_marker_blocks=2),
        "kJpegGray": jpeg(img.convert("L"), quality=92),
    }
    path = os.path.join(os.path.dirname(__file__), "..", "tests", "testFixtures.inc")
    with open(path, "w", newline="\n") as out:
        out.write("/* Generated by tools/makeTestFixtures.py: do not edit. */\n")
        out.write("typedef struct JpegSample { int x, y, r, g, b; } JpegSample;\n")
        for name, (data, samples) in jpegs.items():
            array(out, name, data)
            out.write("static const JpegSample %sAt[] = {%s};\n" % (name, ",".join("{%d,%d,%d,%d,%d}" % s for s in samples)))
        array(out, "kPixelsFont", cff_font())
        array(out, "kSbixFont", bitmap_font("Cascade Sbix", sbix))
        array(out, "kCbdtFont", bitmap_font("Cascade Cbdt", cbdt))
    print("tests/testFixtures.inc: %d bytes" % os.path.getsize(path))


main()
