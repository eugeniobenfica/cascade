"""Writes src/text/brotliData.inc: the tables of the Brotli format (RFC 7932) the decoder (src/text/brotli.c) needs.

    python tools/genBrotli.py path/to/dictionary.bin

dictionary.bin is Brotli's static dictionary (RFC 7932 Appendix A; google/brotli's c/common/dictionary.bin), 122,784
bytes. It goes in zlib-compressed (the library inflates it the first time a stream refers to it). The context lookup
tables (Section 7.1) and the 121 word transforms (Appendix B) are written out here; each table is checked against the
CRC-32 the RFC gives for it.
"""
import os
import sys
import zlib

LUT0 = [
    0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 0, 0, 4, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    8, 12, 16, 12, 12, 20, 12, 16, 24, 28, 12, 12, 32, 12, 36, 12,
    44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 32, 32, 24, 40, 28, 12,
    12, 48, 52, 52, 52, 48, 52, 52, 52, 48, 52, 52, 52, 52, 52, 48,
    52, 52, 52, 52, 52, 48, 52, 52, 52, 52, 52, 24, 12, 28, 12, 12,
    12, 56, 60, 60, 60, 56, 60, 60, 60, 56, 60, 60, 60, 60, 60, 56,
    60, 60, 60, 60, 60, 56, 60, 60, 60, 60, 60, 24, 12, 28, 12, 0,
] + [0, 1] * 32 + [2, 3] * 32
LUT1 = [0] * 32 + [
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
    1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
    1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 0,
] + [0] * 96 + [2] * 32
LUT2 = [0] + [1] * 15 + [2] * 48 + [3] * 64 + [4] * 64 + [5] * 48 + [6] * 15 + [7]

# (prefix, elementary transform, suffix): 0 Identity, 1 FermentFirst, 2 FermentAll, 3..11 OmitFirst1..9, 12..20 OmitLast1..9
I, FF, FA = 0, 1, 2
OF = lambda k: 2 + k
OL = lambda k: 11 + k
TRANSFORMS = [
    (b"", I, b""), (b"", I, b" "), (b" ", I, b" "), (b"", OF(1), b""), (b"", FF, b" "), (b"", I, b" the "), (b" ", I, b""),
    (b"s ", I, b" "), (b"", I, b" of "), (b"", FF, b""), (b"", I, b" and "), (b"", OF(2), b""), (b"", OL(1), b""),
    (b", ", I, b" "), (b"", I, b", "), (b" ", FF, b" "), (b"", I, b" in "), (b"", I, b" to "), (b"e ", I, b" "),
    (b"", I, b"\""), (b"", I, b"."), (b"", I, b"\">"), (b"", I, b"\n"), (b"", OL(3), b""), (b"", I, b"]"),
    (b"", I, b" for "), (b"", OF(3), b""), (b"", OL(2), b""), (b"", I, b" a "), (b"", I, b" that "), (b" ", FF, b""),
    (b"", I, b". "), (b".", I, b""), (b" ", I, b", "), (b"", OF(4), b""), (b"", I, b" with "), (b"", I, b"'"),
    (b"", I, b" from "), (b"", I, b" by "), (b"", OF(5), b""), (b"", OF(6), b""), (b" the ", I, b""), (b"", OL(4), b""),
    (b"", I, b". The "), (b"", FA, b""), (b"", I, b" on "), (b"", I, b" as "), (b"", I, b" is "), (b"", OL(7), b""),
    (b"", OL(1), b"ing "), (b"", I, b"\n\t"), (b"", I, b":"), (b" ", I, b". "), (b"", I, b"ed "), (b"", OF(9), b""),
    (b"", OF(7), b""), (b"", OL(6), b""), (b"", I, b"("), (b"", FF, b", "), (b"", OL(8), b""), (b"", I, b" at "),
    (b"", I, b"ly "), (b" the ", I, b" of "), (b"", OL(5), b""), (b"", OL(9), b""), (b" ", FF, b", "), (b"", FF, b"\""),
    (b".", I, b"("), (b"", FA, b" "), (b"", FF, b"\">"), (b"", I, b"=\""), (b" ", I, b"."), (b".com/", I, b""),
    (b" the ", I, b" of the "), (b"", FF, b"'"), (b"", I, b". This "), (b"", I, b","), (b".", I, b" "), (b"", FF, b"("),
    (b"", FF, b"."), (b"", I, b" not "), (b" ", I, b"=\""), (b"", I, b"er "), (b" ", FA, b" "), (b"", I, b"al "),
    (b" ", FA, b""), (b"", I, b"='"), (b"", FA, b"\""), (b"", FF, b". "), (b" ", I, b"("), (b"", I, b"ful "),
    (b" ", FF, b". "), (b"", I, b"ive "), (b"", I, b"less "), (b"", FA, b"'"), (b"", I, b"est "), (b" ", FF, b"."),
    (b"", FA, b"\">"), (b" ", I, b"='"), (b"", FF, b","), (b"", I, b"ize "), (b"", FA, b"."), (b"\xc2\xa0", I, b""),
    (b" ", I, b","), (b"", FF, b"=\""), (b"", FA, b"=\""), (b"", I, b"ous "), (b"", FA, b", "), (b"", FF, b"='"),
    (b" ", FF, b","), (b" ", FA, b"=\""), (b" ", FA, b", "), (b"", FA, b","), (b"", FA, b"("), (b"", FA, b". "),
    (b" ", FA, b"."), (b"", FA, b"='"), (b" ", FA, b". "), (b" ", FF, b"=\""), (b" ", FA, b"='"), (b" ", FF, b"='"),
]


def check(name, data, length, crc):
    got = zlib.crc32(bytes(data)) & 0xFFFFFFFF
    if len(data) != length or got != crc:
        sys.exit("%s: %d bytes, CRC-32 0x%08x; RFC 7932 says %d, 0x%08x" % (name, len(data), got, length, crc))


def c_bytes(b):
    return '"%s"' % "".join(chr(c) if 0x20 <= c < 0x7F and c not in (0x22, 0x5C, 0x3F) else "\\%03o" % c for c in b)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    dictionary = open(sys.argv[1], "rb").read()
    check("dictionary", dictionary, 122784, 0x5136CB04)
    check("Lut0", LUT0, 256, 0x8E91EFB7)
    check("Lut1", LUT1, 256, 0xD01A32F4)
    check("Lut2", LUT2, 256, 0x0DD7A0D6)
    seq = b"".join(p + b"\0" + bytes([t]) + s + b"\0" for p, t, s in TRANSFORMS)
    check("transforms", seq, 648, 0x3D965F81)
    z = zlib.compress(dictionary, 9)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "src", "text", "brotliData.inc")
    lines = ["/* Generated by tools/genBrotli.py from RFC 7932's tables and dictionary: do not edit. */"]
    for name, table in (("kLut0", LUT0), ("kLut1", LUT1), ("kLut2", LUT2)):
        lines.append("static const uint8_t %s[256] = {" % name)
        lines += ["    " + ", ".join(str(v) for v in table[i:i + 32]) + "," for i in range(0, 256, 32)]
        lines.append("};")
    lines.append("/* the word transforms: prefix, its length, the elementary transform, suffix, its length */")
    lines.append("static const Transform kTransforms[121] = {")
    for p, t, s in TRANSFORMS:
        lines.append("    {%s, %d, %d, %s, %d}," % (c_bytes(p), len(p), t, c_bytes(s), len(s)))
    lines.append("};")
    lines.append("/* the static dictionary (%d bytes), zlib-compressed */" % len(dictionary))
    lines.append("static const uint8_t kDictionaryZ[%d] = {" % len(z))
    lines += ["    " + ", ".join("0x%02x" % v for v in z[i:i + 24]) + "," for i in range(0, len(z), 24)]
    lines.append("};")
    with open(out, "w", encoding="utf-8", newline="") as f:
        f.write("".join(line + chr(10) for line in lines))
    print("%s: dictionary %d bytes as %d" % (out, len(dictionary), len(z)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
