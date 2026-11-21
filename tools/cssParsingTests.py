"""Runs css-parsing-tests (https://github.com/SimonSapin/css-parsing-tests) on cascade's CSS parsing.

    python tools/cssParsingTests.py path/to/cascade-css-parsing path/to/css-parsing-tests [--known FILE]

The files cascade's parsing can answer: component_value_list.json and one_component_value.json (the tokenizer and the
component values), declaration_list.json and one_declaration.json (declarations; the at-rules a list of declarations
may hold are not kept by cascade, so cases with one are passed over), the color_*.json files (compared as RGBA, each
channel within one unit, alpha within 1/255; a result written as lab(), lch(), oklab(), oklch() or color(srgb) is
turned into sRGB to compare, and passed over outside its gamut, which CSS maps into it and cascade clips) and An+B.json (through :nth-child()). A number's representation is not compared (the tokenizer keeps its
value). --known: the cases known to fail ("file.json:N", one a line, "#" comments); a case listed that passes is an
error too. Prints the failures and a count; exits 1 when a case fails that is not known to.
"""
import json
import os
import re
import subprocess
import sys

MODES = {"component_value_list.json": "values", "one_component_value.json": "one", "declaration_list.json": "decls",
         "one_declaration.json": "decls", "An+B.json": "anb"}


def strip_repr(v):
    """The representation of numbers left out (index 1 of number, percentage, dimension), and the parse errors at the
    end of the input (["error", "eof-in-string"]...), which cascade does not record."""
    if isinstance(v, list):
        v = [x for x in v if not (isinstance(x, list) and len(x) == 2 and x[0] == "error" and str(x[1]).startswith("eof-in"))]
        if len(v) >= 4 and v[0] in ("number", "percentage", "dimension") and isinstance(v[1], str):
            return [v[0]] + [strip_repr(x) for x in v[2:]]
        return [strip_repr(x) for x in v]
    if isinstance(v, float) and v == int(v):
        return int(v)
    return v


def srgb_gamma(c):
    return 12.92 * c if abs(c) <= 0.0031308 else (1 if c > 0 else -1) * (1.055 * abs(c) ** (1 / 2.4) - 0.055)


def from_linear(r, g, b):
    """Linear sRGB to 0..255, or None outside the gamut (CSS maps those into it; cascade clips)."""
    if min(r, g, b) < -1e-4 or max(r, g, b) > 1 + 1e-4:
        return None
    return [255 * srgb_gamma(min(max(x, 0), 1)) for x in (r, g, b)]


def oklab_to_rgb(L, a, b):
    l_, m_, s_ = L + 0.3963377774 * a + 0.2158037573 * b, L - 0.1055613458 * a - 0.0638541728 * b, L - 0.0894841775 * a - 1.2914855480 * b
    l, m, s = l_ ** 3, m_ ** 3, s_ ** 3
    return from_linear(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                       -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s)


def lab_to_rgb(L, a, b):
    """CIE Lab (D50) to sRGB, through XYZ D50 and the Bradford adaptation to D65."""
    k, e = 24389 / 27, 216 / 24389
    fy = (L + 16) / 116
    fx, fz = fy + a / 500, fy - b / 200
    x = fx ** 3 if fx ** 3 > e else (116 * fx - 16) / k
    y = ((L + 16) / 116) ** 3 if L > k * e else L / k
    z = fz ** 3 if fz ** 3 > e else (116 * fz - 16) / k
    X, Y, Z = x * 0.3457 / 0.3585, y, z * (1 - 0.3457 - 0.3585) / 0.3585
    X65 = 0.9554734527042182 * X - 0.023098536874261423 * Y + 0.0632593086610217 * Z
    Y65 = -0.028369706963208136 * X + 1.0099954580058226 * Y + 0.021041398966943008 * Z
    Z65 = 0.012314001688319899 * X - 0.020507696433477912 * Y + 1.3303659366080753 * Z
    return from_linear(3.2409699419045226 * X65 - 1.537383177570094 * Y65 - 0.4986107602930034 * Z65,
                       -0.9692436362808796 * X65 + 1.8759675015077202 * Y65 + 0.04155505740717559 * Z65,
                       0.05563007969699366 * X65 - 0.20397695888897652 * Y65 + 1.0569715142428786 * Z65)


def parse_color(s):
    """An expected color as [r, g, b, a] (0..255), or None when written in a space cascade does not keep."""
    if s is None:
        return "null"
    if not isinstance(s, str):  # (color_functions_5's pairs of color() serializations)
        return None
    m = re.fullmatch(r"(rgba?|lab|lch|oklab|oklch|color)\(([^)]*)\)", s)
    if not m:
        return None
    space, body = m.group(1), m.group(2)
    if space not in ("rgb", "rgba"):
        nums = body.replace("/", " ").split()
        if space == "color":
            if not nums or nums[0] != "srgb":
                return None
            nums = nums[1:]
        if "none" in nums:
            return None
        v = [float(x) for x in nums]
        alpha = v[3] if len(v) > 3 else 1.0
        import math
        if space == "color":
            rgb = [255 * x for x in v[:3]] if all(0 <= x <= 1 for x in v[:3]) else None
        elif space in ("lab", "lch"):
            L, a, b = v[0], v[1], v[2]
            if space == "lch":
                a, b = v[1] * math.cos(math.radians(v[2])), v[1] * math.sin(math.radians(v[2]))
            rgb = lab_to_rgb(L, a, b)
        else:
            L, a, b = v[0], v[1], v[2]
            if space == "oklch":
                a, b = v[1] * math.cos(math.radians(v[2])), v[1] * math.sin(math.radians(v[2]))
            rgb = oklab_to_rgb(L, a, b)
        return None if rgb is None else rgb + [alpha * 255]
    parts = [float(x) for x in body.replace("/", ",").split(",")]
    a = parts[3] if len(parts) > 3 else 1.0
    return [parts[0], parts[1], parts[2], a * 255]


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    args = sys.argv[1:]
    known = []
    if "--known" in args:
        k = args.index("--known")
        with open(args[k + 1], encoding="utf-8") as f:
            known = [line.split("#", 1)[0].strip() for line in f]
        known = [x for x in known if x]
        del args[k:k + 2]
    tool, root = args[0], args[1]
    cases = []  # (id, mode, input, expected)
    for name in sorted(os.listdir(root)):
        if not name.endswith(".json"):
            continue
        mode = "color" if name.startswith("color_") else MODES.get(name)
        if not mode:
            continue
        data = json.load(open(os.path.join(root, name), encoding="utf-8"))
        for i in range(0, len(data) - 1, 2):
            cases.append(("%s:%d" % (name, i // 2 + 1), mode, data[i], data[i + 1], name))
    inp = "".join("%s %s\n" % (mode, text.encode("utf-8").hex()) for _, mode, text, _, _ in cases)
    out = subprocess.run([tool], input=inp, capture_output=True, text=True, encoding="utf-8").stdout.splitlines()
    passed = failed = known_failed = skipped = 0
    stale = []
    for k, (cid, mode, text, want, name) in enumerate(cases):
        got = json.loads(out[k]) if k < len(out) else "(no output)"
        if mode == "color":
            w = parse_color(want)
            if w is None:
                skipped += 1
                continue
            ok = (w == "null" and got is None) or (
                w != "null" and got is not None and all(abs(a - b) <= 1 for a, b in zip(got[:3], w[:3])) and abs(got[3] - w[3]) <= 1)
        elif name == "one_declaration.json":
            want = [want] if isinstance(want, list) and want and want[0] == "declaration" else []
            ok = strip_repr(got) == strip_repr(want)
        elif mode == "decls":
            if any(isinstance(x, list) and x and x[0] == "at-rule" for x in want):
                skipped += 1
                continue
            want = [x for x in want if isinstance(x, list) and x and x[0] == "declaration"]
            ok = strip_repr(got) == strip_repr(want)
        else:
            ok = strip_repr(got) == strip_repr(want)
        if ok:
            passed += 1
            if cid in known:
                stale.append(cid)
        elif cid in known:
            known_failed += 1
        else:
            failed += 1
            if failed <= 12 or os.environ.get("CASCADE_CSS_LIST"):
                print("FAIL %s %r\n  want %s\n  got  %s" % (cid, text, json.dumps(want, ensure_ascii=False)[:300],
                                                            json.dumps(got, ensure_ascii=False)[:300]))
    for cid in stale:
        print("PASS %s: listed as failing (take it off the list)" % cid)
    print("css-parsing-tests: %d of %d cases pass (%d failing as known, %d not; %d passed over)" %
          (passed, passed + failed + known_failed, known_failed, failed, skipped))
    return 1 if failed or stale else 0


if __name__ == "__main__":
    sys.exit(main())
