"""Checks the Python binding: python bindings/python/testCascade.py (with CASCADE_LIB or the library
next to cascade.py)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cascade  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL", what)


check(cascade.version()[0] >= 1, "version")
with cascade.View() as view:
    view.load_html("<title>From Python</title><style>body{margin:0} #box{height:60px;overflow:auto} .row{height:20px}</style>"
                   "<p id=p>olá</p><button id=b>Go</button><input id=name value=Ann>"
                   "<template id=t><div class=row data-n='{{n}}'>Row {{n}}</div></template><div id=out></div><div id=box></div>")
    view.set_viewport(400, 300)
    view.update()
    check(view.title == "From Python", "title")
    check(view["#p"].text == "olá", "unicode text")
    clicks = []
    view["#b"].on("click", lambda ev: (clicks.append(ev.type), view["#p"].set_text("Clicked")))
    view["#b"].click()
    check(clicks == ["click"] and view["#p"].text == "Clicked", "events")
    check(view["#name"].value == "Ann", "value")
    view["#name"].set_value("Bob")
    check(view["#name"].value == "Bob", "set value")
    copy = view["#out"].append_copy(view["#t"])
    copy.fill(n=7)
    check(copy.text == "Row 7" and copy.attribute("data-n") == "7", "template copy")
    seen = []
    view.bind_list(view["#box"], view["#t"], 100000, 20, lambda row, i: (seen.append(i), row.fill(n=i)))
    view.update()
    rows = view.query_all("#box .row")
    check(3 <= len(rows) < 20 and rows[0].text == "Row 0", "list rows %d" % len(rows))
    check(view.feature(cascade.Feature.ANIMATIONS), "feature default")
    view.set_feature(cascade.Feature.ANIMATIONS, False)
    check(not view.feature(cascade.Feature.ANIMATIONS), "feature off")
    view["#out"].set_painter(lambda c, w, h: c.fill_rect(0, 0, w, h, 0xFF0000FF))
    w, h, pixels = view.paint()
    check(w == 400 and h == 300 and len(pixels) == w * h, "paint")
    check(view.stats["styles_computed"] >= 0, "stats")
    gone = view["#p"]
    gone.remove()
    try:
        gone.set_text("x")
        check(False, "a gone element raises")
    except cascade.CascadeError:
        pass

# touch, the edit menu and the clipboard
with cascade.View() as view:
    view.load_html("<style>body{margin:0} input{margin:0;width:300px;font-size:20px}</style><input id=f value='alpha beta'>")
    view.set_viewport(400, 200)
    view.update()
    view.paint()  # (taps find what was painted)
    clip, menus = [""], []
    view.set_clipboard(lambda t: clip.__setitem__(0, t), lambda: clip[0])
    view.on_edit_menu(lambda show, rect, actions: menus.append((show, rect, actions)))
    x, y, w, h = view["#f"].rect
    check(not view.edit_action(cascade.EditAction.COPY), "no field, no copy")
    for _ in range(2):  # a double tap selects the word
        view.touch_down(1, x + 12, y + h / 2)
        view.touch_up(1, x + 12, y + h / 2)
    check(view.selection_handles_shown, "handles after a double tap")
    check(menus and menus[-1][0] and cascade.EditAction.COPY in menus[-1][2] and menus[-1][1][2] > 0, "edit menu %r" % menus)
    check(cascade.EditAction.CUT in view.edit_actions, "can cut")
    check(view.edit_action(cascade.EditAction.COPY) and clip[0] == "alpha", "copy %r" % clip[0])
    check(view.edit_action(cascade.EditAction.SELECT_ALL) and view.edit_action(cascade.EditAction.CUT), "cut all")
    check(view["#f"].value == "" and clip[0] == "alpha beta", "cut")
    check(view.edit_action(cascade.EditAction.PASTE) and view["#f"].value == "alpha beta", "paste")
    view.hide_selection_handles()
    check(not view.selection_handles_shown, "handles hidden")
    check(view.copy_text("olá") and view.clipboard_text == "olá", "clipboard from code")
    check(not view.copy_image(1, 1, [0xFFFF0000]) and view.clipboard_files == [], "no pictures without a hook")
    view.touch_cancel()
print("python binding: %d failures" % failures)
sys.exit(1 if failures else 0)
