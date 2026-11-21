"""Writes src/text/useData.inc: the Universal Shaping Engine's category of every code point, as ranges; and the
canonical decompositions and combining classes of the Brahmic scripts' characters (its shaper decomposes them).

    python tools/genUse.py path/to/ucd

The directory holds the Unicode Character Database's IndicSyllabicCategory.txt, IndicPositionalCategory.txt,
DerivedCoreProperties.txt and ArabicShaping.txt (https://www.unicode.org/Public/<version>/ucd/), of the same version as
this Python's unicodedata. The categories are Microsoft's USE ones, derived from those properties as HarfBuzz derives
them (its gen-use-table.py), with its corrections to the data: B a base, H a halant, VAbv a vowel above, and so on.
"""
import os
import sys
import unicodedata

# (the order of the enum in fontUse.c)
CATEGORIES = ["O", "B", "N", "GB", "CGJ", "FAbv", "FBlw", "FPst", "FMAbv", "FMBlw", "FMPst", "MAbv", "MBlw", "MPst", "MPre",
              "CMAbv", "CMBlw", "SUB", "CS", "H", "HVM", "HN", "IS", "Sk", "ZWNJ", "R", "SMAbv", "SMBlw", "VAbv", "VBlw", "VPst",
              "VPre", "VMAbv", "VMBlw", "VMPst", "VMPre", "WJ"]

POSITIONS = {
    "F": {"Abv": ["Top"], "Blw": ["Bottom"], "Pst": ["Right"]},
    "M": {"Abv": ["Top"], "Blw": ["Bottom", "Bottom_And_Left", "Bottom_And_Right"], "Pst": ["Right"],
          "Pre": ["Left", "Top_And_Bottom_And_Left"]},
    "CM": {"Abv": ["Top"], "Blw": ["Bottom", "Overstruck"]},
    "V": {"Abv": ["Top", "Top_And_Bottom", "Top_And_Bottom_And_Right", "Top_And_Right"],
          "Blw": ["Bottom", "Overstruck", "Bottom_And_Right"], "Pst": ["Right"],
          "Pre": ["Left", "Top_And_Left", "Top_And_Left_And_Right", "Left_And_Right"]},
    "VM": {"Abv": ["Top"], "Blw": ["Bottom", "Overstruck"], "Pst": ["Right"], "Pre": ["Left"]},
    "SM": {"Abv": ["Top"], "Blw": ["Bottom"]},
    "FM": {"Abv": ["Top"], "Blw": ["Bottom"], "Pst": ["Not_Applicable"]},
}


def read(path, into):
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = [x.strip() for x in line.split(";")]
            a, _, b = parts[0].partition("..")
            for cp in range(int(a, 16), int(b or a, 16) + 1):
                into(cp, parts)


def category(u, uisc, uipc, gc, di, ajt):
    base = (uisc in ("Number", "Consonant", "Consonant_Head_Letter", "Tone_Letter", "Vowel_Independent")
            or ajt in ("C", "D", "L", "R", "T")
            or (gc == "Lo" and uisc in ("Avagraha", "Bindu", "Consonant_Final", "Consonant_Medial", "Consonant_Subjoined",
                                        "Vowel", "Vowel_Dependent")))
    sym_mod = 0x1B6B <= u <= 0x1B73
    cgj = uisc == "Joiner" or (di and gc in ("Mc", "Me", "Mn"))
    base_other = uisc == "Consonant_Placeholder" or u in (0x2015, 0x2022, 0x25FB, 0x25FC, 0x25FD, 0x25FE)
    wj = (di and u not in (0x115F, 0x1160, 0x3164, 0xFFA0, 0x1BCA0, 0x1BCA1, 0x1BCA2, 0x1BCA3) and uisc == "Other"
          and not cgj)
    if base or gc == "Cn":  # (an unassigned code point: HarfBuzz keeps what follows with it, as after a base)
        return "B"
    if uisc == "Brahmi_Joining_Number":
        return "N"
    if base_other:
        return "GB"
    if cgj:
        return "CGJ"
    if (uisc == "Consonant_Final" and gc != "Lo") or uisc == "Consonant_Succeeding_Repha":
        c = "F"
    elif uisc == "Syllable_Modifier":
        c = "FM"
    elif (uisc == "Consonant_Medial" and gc != "Lo") or uisc == "Consonant_Initial_Postfixed":
        c = "M"
    elif uisc in ("Nukta", "Gemination_Mark", "Consonant_Killer") and not sym_mod:
        c = "CM"
    elif uisc == "Consonant_Subjoined" and gc != "Lo":
        return "SUB"
    elif uisc == "Consonant_With_Stacker":
        return "CS"
    elif u == 0x0DCA:
        return "HVM"
    elif u == 0x1A60:
        return "Sk"
    elif uisc == "Virama":
        return "H"
    elif uisc == "Number_Joiner":
        return "HN"
    elif uisc == "Invisible_Stacker":
        return "IS"
    elif uisc == "Non_Joiner":
        return "ZWNJ"
    elif uisc in ("Consonant_Preceding_Repha", "Consonant_Prefixed"):
        return "R"
    elif sym_mod:
        c = "SM"
    elif uisc == "Pure_Killer" or (gc != "Lo" and uisc in ("Vowel", "Vowel_Dependent")):
        c = "V"
    elif uisc in ("Tone_Mark", "Cantillation_Mark", "Register_Shifter", "Visarga") or (gc != "Lo" and uisc == "Bindu"):
        c = "VM"
    elif wj:
        return "WJ"
    else:
        return "O"
    for suffix, places in POSITIONS[c].items():
        if uipc in places:
            return c + suffix
    # (a position the model has no place for: what HarfBuzz falls back on)
    return c + ("Blw" if c in ("CM", "SM") else "Abv")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    d = sys.argv[1]
    uisc, uipc, di, ajt = {}, {}, set(), {}
    read(os.path.join(d, "IndicSyllabicCategory.txt"), lambda cp, p: uisc.__setitem__(cp, p[1]))
    read(os.path.join(d, "IndicPositionalCategory.txt"), lambda cp, p: uipc.__setitem__(cp, p[1]))
    read(os.path.join(d, "DerivedCoreProperties.txt"), lambda cp, p: di.add(cp) if p[1] == "Default_Ignorable_Code_Point" else None)
    read(os.path.join(d, "ArabicShaping.txt"), lambda cp, p: ajt.__setitem__(cp, p[2]))
    # HarfBuzz's corrections to the data (gen-use-table.py)
    for u in (0x0F18, 0x0F19, 0x0F3E, 0x0F3F):
        uisc[u] = "Vowel_Dependent"
    uisc[0x1CED] = "Tone_Mark"
    uisc[0x1A7F], uipc[0x1A7F] = "Consonant_Final", "Bottom"
    for u in (0x11302, 0x11303, 0x114C1, 0x1CF8, 0x1CF9):
        uipc[u] = "Top"
    for u in range(0x1CE2, 0x1CE9):
        uisc[u] = "Cantillation_Mark"
    uisc[0x11134] = "Gemination_Mark"
    # (Tibetan AA, written below, comes before the vowels above in the canonical order, its combining class the lowest:
    # HarfBuzz takes them in that order, which the model's, below after above, would break)
    uipc[0x0F71] = "Top"
    cats = []
    for u in range(0x110000):
        gc = unicodedata.category(chr(u))
        c = CATEGORIES.index(category(u, uisc.get(u, "Other"), uipc.get(u, "Not_Applicable"), gc, u in di, ajt.get(u)))
        cats.append(c | (0x80 if gc in ("Mn", "Mc", "Me") else 0))  # (and whether Unicode calls it a mark)
    ranges = []
    start, cur = 0, cats[0]
    for u in range(1, 0x110000):
        if cats[u] != cur:
            ranges.append((start, cur))
            start, cur = u, cats[u]
    ranges.append((start, cur))
    # the canonical decompositions of the Brahmic scripts' characters (one level: {character, first, second, whether
    # the two compose back to it}), and the combining classes of their marks
    brahmic = lambda u: u in uisc or u in uipc
    decomp, ccc = [], {}
    for u in range(0x110000):
        d = unicodedata.decomposition(chr(u))
        if not d or d.startswith("<"):
            continue
        parts = [int(x, 16) for x in d.split()]
        if not (brahmic(u) or brahmic(parts[0])) or 0xAC00 <= u <= 0xD7A3:
            continue
        a, b = parts[0], parts[1] if len(parts) > 1 else 0
        composes = b != 0 and unicodedata.normalize("NFC", chr(a) + chr(b)) == chr(u)
        decomp.append((u, a, b, composes))
    for u in range(0x110000):
        if (brahmic(u) or any(u in (x[1], x[2]) for x in decomp)) and unicodedata.combining(chr(u)):
            ccc[u] = unicodedata.combining(chr(u))
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "src", "text", "useData.inc")
    lines = ["/* Generated by tools/genUse.py from Unicode %s: do not edit. */" % unicodedata.unidata_version,
             "/* first << 8 | category (| 0x80: a Unicode mark): each range runs to the next one's first. */",
             "static const uint32_t kUse[] = {"]
    for i in range(0, len(ranges), 8):
        lines.append(" ".join("0x%X," % (a << 8 | c) for a, c in ranges[i:i + 8]))
    lines += ["};", "/* {character, first, second (0: none), whether first and second compose back to it} */",
              "static const uint32_t kUseDecompositions[][4] = {"]
    lines += ["{0x%X, 0x%X, 0x%X, %d}," % (u, a, b, c) for u, a, b, c in decomp]
    lines += ["};", "/* character << 8 | canonical combining class (the marks with one) */",
              "static const uint32_t kUseCombiningClasses[] = {"]
    keys = sorted(ccc)
    for i in range(0, len(keys), 8):
        lines.append(" ".join("0x%X," % (u << 8 | ccc[u]) for u in keys[i:i + 8]))
    lines.append("};")
    with open(out, "w", encoding="utf-8", newline="") as f:
        f.write("".join(line + chr(10) for line in lines))
    print("%s: %d ranges, %d decompositions, %d combining classes (Unicode %s)" % (out, len(ranges), len(decomp), len(ccc),
                                                                                  unicodedata.unidata_version))
    return 0


if __name__ == "__main__":
    sys.exit(main())
