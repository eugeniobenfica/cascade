"""Writes tests/layoutFixtures.inc: small pages of boxes, and where Chrome lays each of their elements out.

    python tools/layoutFixtures.py [path/to/chrome-or-edge]

As Taffy's gentest does: each fixture is a page of boxes (no text: fonts differ from one machine to another) laid out
by a Chromium browser (Microsoft Edge by default, headless), which writes the border box of every element in the body
(document order) into the page; those boxes are the expected ones. Each page holds its boxes in one of 800x600 (the
browser's window does not decide them). cascade-tests lays out the same pages and compares (tests/tests.c: testLayoutFixtures), within half a pixel. A fixture cascade does not lay
out as Chrome does is listed in tests/layoutFixtures.known, with why.
"""
import json
import os
import subprocess
import sys
import tempfile

D = "div"


def box(style="", *children, tag=D):
    return '<%s style="%s">%s</%s>' % (tag, style, "".join(children), tag)


FIXTURES = {
    # ---- block ----
    "block_auto_width": box("padding:10px;border:5px solid", box("height:20px"), box("height:30px;margin:0 40px")),
    "block_percent_width": box("width:400px", box("width:50%;height:10px"), box("width:25%;padding-left:10%;height:10px")),
    "block_margin_auto_center": box("width:500px", box("width:200px;height:10px;margin:0 auto"), box("width:200px;height:10px;margin-left:auto")),
    "block_border_box": box("width:300px;padding:20px;border:10px solid;box-sizing:border-box;height:100px", box("height:20px")),
    "block_min_max": box("width:300px", box("width:500px;max-width:200px;height:10px"), box("width:50px;min-width:120px;height:10px"),
                         box("height:5px;min-height:40px"), box("height:90px;max-height:30px")),
    "block_margin_collapse_siblings": box("width:200px", box("height:10px;margin-bottom:20px"), box("height:10px;margin-top:30px"),
                                          box("height:10px;margin-top:-10px")),
    "block_margin_collapse_parent": box("width:200px;margin-top:10px", box("margin-top:25px", box("height:10px;margin-top:15px"))),
    "block_margin_no_collapse_padding": box("width:200px;padding-top:1px", box("height:10px;margin-top:25px")),
    "block_percent_height": box("width:200px;height:300px", box("height:50%", box("height:50%")), box("height:10%")),
    "block_aspect_ratio": box("width:300px", box("aspect-ratio:2/1"), box("width:100px;aspect-ratio:1"),
                              box("height:40px;width:auto;aspect-ratio:3/1;display:inline-block")),
    "block_negative_margins": box("width:300px", box("height:20px;margin:0 -20px"), box("height:20px;margin-top:-5px;margin-left:30px")),
    "block_inline_block": box("width:300px", box("display:inline-block;width:100px;height:40px"), box("display:inline-block;width:150px;height:20px;vertical-align:top"),
                              box("display:inline-block;width:120px;height:30px")),
    "block_overflow_bfc": box("width:300px", box("float:left;width:100px;height:50px"), box("overflow:hidden;height:30px")),
    "block_floats": box("width:300px", box("float:left;width:100px;height:40px"), box("float:right;width:80px;height:60px"),
                        box("float:left;width:150px;height:20px"), box("clear:both;height:10px")),
    # ---- positioning ----
    "abs_insets": box("position:relative;width:400px;height:300px", box("position:absolute;left:10px;top:20px;width:50px;height:50px"),
                      box("position:absolute;right:10px;bottom:20px;width:50px;height:50px"),
                      box("position:absolute;left:10px;right:10px;top:100px;height:20px"),
                      box("position:absolute;top:10%;bottom:10%;left:50%;width:10px")),
    "abs_percent_size": box("position:relative;width:400px;height:200px;padding:10px", box("position:absolute;width:50%;height:25%;left:0;top:0")),
    "abs_auto_static": box("position:relative;width:300px;padding:15px", box("height:30px"), box("position:absolute;width:40px;height:40px")),
    "abs_margin_auto": box("position:relative;width:400px;height:200px", box("position:absolute;left:0;right:0;width:100px;height:50px;margin:0 auto")),
    "relative_offsets": box("width:300px", box("position:relative;left:20px;top:10px;height:30px"), box("position:relative;right:15px;bottom:5px;height:30px")),
    # ---- flex ----
    "flex_grow": box("display:flex;width:400px;height:50px", box("flex-grow:1"), box("flex-grow:2"), box("width:100px")),
    "flex_shrink": box("display:flex;width:300px;height:50px", box("width:200px"), box("width:200px;flex-shrink:2"), box("width:100px;flex-shrink:0")),
    "flex_basis": box("display:flex;width:500px;height:20px", box("flex:1 1 100px"), box("flex:2 1 50px"), box("flex:0 0 25%")),
    "flex_column": box("display:flex;flex-direction:column;width:200px;height:300px", box("height:50px"), box("flex:1"), box("flex:2;margin:10px")),
    "flex_wrap": box("display:flex;flex-wrap:wrap;width:300px", box("width:120px;height:30px"), box("width:120px;height:40px"),
                     box("width:120px;height:20px"), box("width:200px;height:10px")),
    "flex_wrap_reverse": box("display:flex;flex-wrap:wrap-reverse;width:300px;height:200px", box("width:200px;height:30px"),
                             box("width:200px;height:40px")),
    "flex_justify_between": box("display:flex;justify-content:space-between;width:400px;height:20px", box("width:50px"), box("width:50px"), box("width:50px")),
    "flex_justify_around": box("display:flex;justify-content:space-around;width:400px;height:20px", box("width:50px"), box("width:50px"), box("width:50px")),
    "flex_justify_evenly": box("display:flex;justify-content:space-evenly;width:400px;height:20px", box("width:50px"), box("width:50px"), box("width:50px")),
    "flex_justify_center_end": box("width:400px", box("display:flex;justify-content:center;height:20px", box("width:50px"), box("width:70px")),
                                   box("display:flex;justify-content:flex-end;height:20px", box("width:50px"), box("width:70px"))),
    "flex_align_items": box("width:400px", box("display:flex;align-items:center;height:100px", box("width:50px;height:20px"), box("width:50px;height:60px"), box("width:50px")),
                            box("display:flex;align-items:flex-end;height:100px", box("width:50px;height:20px"), box("width:50px;height:60px"))),
    "flex_align_self": box("display:flex;height:100px;width:300px", box("width:50px;align-self:flex-start;height:10px"), box("width:50px;align-self:center;height:10px"),
                           box("width:50px;align-self:flex-end;height:10px"), box("width:50px")),
    "flex_align_content": box("display:flex;flex-wrap:wrap;align-content:space-between;width:200px;height:200px", box("width:150px;height:30px"),
                              box("width:150px;height:30px"), box("width:150px;height:30px")),
    "flex_gap": box("display:flex;flex-wrap:wrap;gap:10px 20px;width:300px", box("width:100px;height:30px"), box("width:100px;height:30px"),
                    box("width:100px;height:30px"), box("width:100px;height:30px")),
    "flex_order": box("display:flex;width:300px;height:20px", box("width:50px;order:3"), box("width:60px;order:1"), box("width:70px;order:2")),
    "flex_row_reverse": box("display:flex;flex-direction:row-reverse;width:300px;height:20px", box("width:50px"), box("width:60px")),
    "flex_column_reverse": box("display:flex;flex-direction:column-reverse;width:100px;height:200px", box("height:50px"), box("height:60px")),
    "flex_margin_auto": box("display:flex;width:400px;height:100px", box("width:50px;margin-left:auto"), box("width:50px;margin:auto")),
    "flex_min_max": box("display:flex;width:400px;height:20px", box("flex:1;max-width:50px"), box("flex:1;min-width:250px"), box("flex:1")),
    "flex_nested": box("display:flex;width:400px;height:200px", box("flex:1;display:flex;flex-direction:column", box("flex:1"), box("height:40px")), box("width:100px")),
    "flex_percent_children": box("display:flex;width:400px;height:200px", box("width:25%;height:50%"), box("flex:1;height:10%")),
    "flex_padding_border": box("display:flex;width:400px;height:100px;padding:10px;border:5px solid", box("flex:1;padding:5px"), box("flex:1;border:3px solid")),
    "flex_stretch_column": box("display:flex;flex-direction:column;width:300px", box("height:20px"), box("height:20px;width:100px"), box("height:20px;align-self:center;width:50px")),
    "flex_aspect_ratio": box("display:flex;width:400px", box("width:100px;aspect-ratio:2"), box("flex:1;aspect-ratio:4/1;align-self:flex-start")),
    # ---- grid ----
    "grid_fixed": box("display:grid;grid-template-columns:100px 200px;grid-template-rows:50px 30px", box(), box(), box(), box()),
    "grid_fr": box("display:grid;grid-template-columns:1fr 2fr 1fr;width:400px;grid-auto-rows:20px", box(), box(), box(), box()),
    "grid_repeat_minmax": box("display:grid;grid-template-columns:repeat(3, minmax(50px, 1fr));width:300px;grid-auto-rows:20px", box(), box(), box(), box(), box()),
    "grid_auto_fill": box("display:grid;grid-template-columns:repeat(auto-fill, 70px);width:300px;grid-auto-rows:20px", box(), box(), box(), box(), box()),
    "grid_gap": box("display:grid;grid-template-columns:100px 100px;gap:10px 30px;grid-auto-rows:20px", box(), box(), box(), box()),
    "grid_placement": box("display:grid;grid-template-columns:repeat(4, 50px);grid-template-rows:repeat(3, 20px)", box("grid-column:2/4;grid-row:1"),
                          box("grid-column:1;grid-row:2/4"), box("grid-column:span 2;grid-row:3"), box()),
    "grid_areas": box("display:grid;grid-template-areas:'a a b' 'c d b';grid-template-columns:50px 60px 70px;grid-template-rows:20px 30px",
                      box("grid-area:b"), box("grid-area:a"), box("grid-area:d"), box("grid-area:c")),
    "grid_dense": box("display:grid;grid-template-columns:repeat(3, 50px);grid-auto-flow:dense;grid-auto-rows:20px", box("grid-column:span 2"),
                      box("grid-column:span 2"), box()),
    "grid_align": box("display:grid;grid-template-columns:100px 100px;grid-template-rows:60px;justify-items:center;align-items:end",
                      box("width:30px;height:20px"), box("width:40px;height:10px;justify-self:end;align-self:start")),
    "grid_content_align": box("display:grid;grid-template-columns:50px 50px;grid-template-rows:20px;width:300px;height:100px;justify-content:space-between;align-content:center",
                              box(), box()),
    "grid_auto_rows_content": box("display:grid;grid-template-columns:100px;grid-auto-rows:minmax(20px, auto)", box("height:50px"), box()),
    "grid_percent_tracks": box("display:grid;grid-template-columns:25% 75%;width:400px;grid-template-rows:30px", box(), box()),
}

PAGE = ('<!doctype html><html><head><style>body{margin:0}</style></head><body><div style="width:800px;height:600px">%s</div>'
        '</body></html>')
PROBE = ('<script>document.body.setAttribute("data-r", JSON.stringify([...document.body.querySelectorAll("*")]'
         '.filter(e => e.tagName != "SCRIPT").map(e => { const r = e.getBoundingClientRect(); return [r.x, r.y, r.width, r.height]; })));'
         '</script>')


def c_string(s):
    return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')


def main():
    browser = sys.argv[1] if len(sys.argv) > 1 else "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"
    out = ["/* Generated by tools/layoutFixtures.py (Chromium, headless, 800x600): do not edit. */",
           "/* each: a name, a page, how many elements its body holds, then each one's border box (x, y, width, height) */"]
    tmp = tempfile.mkdtemp()
    for name, body in FIXTURES.items():
        page = PAGE % body
        path = os.path.join(tmp, name + ".html")
        open(path, "w", encoding="utf-8").write(PAGE % (body + PROBE))
        dom = subprocess.run([browser, "--headless=new", "--disable-gpu", "--window-size=800,600", "--dump-dom",
                              "file:///" + path.replace("\\", "/")], capture_output=True, text=True, encoding="utf-8").stdout
        start = dom.find('data-r="')
        if start < 0:
            sys.exit("%s: no layout from the browser" % name)
        rects = json.loads(dom[start + 8:dom.find('"', start + 8)].replace("&quot;", '"'))
        nums = ", ".join("%.6g" % v for r in rects for v in r)
        out.append("{%s, %s, %d, (const float[]){%s}}," % (c_string(name), c_string(page), len(rects), nums))
        print("%s: %d elements" % (name, len(rects)))
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(root, "tests", "layoutFixtures.inc"), "w", encoding="utf-8", newline="") as f:
        f.write("".join(line + chr(10) for line in out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
