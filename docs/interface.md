# The interface

What an application needs beyond a page: events, forms, menus, dialogs, popovers, tooltips,
drag and drop, touch, templates and long lists, timers and threads, accessibility, the inspector,
and the features a view can turn off.

## Finding elements

`cscdViewGet(view, selector)` returns the element a selector names — `"#save"`, `".status"`,
`"main > h1"`, or a bare id (`"save"`). It never returns NULL: when nothing matches it reports
`no element matches "#svae"` (see [Getting started](gettingStarted.md#when-something-is-missing))
and returns a detached element, so a typo is a message, not a crash. `cscdViewGetElementById` and
`cscdViewQuerySelector` return NULL as on the web.

```c
CscdElement* status = cscdViewGet(ui, CSCD_SV("#status"));
cscdElementSetText(status, CSCD_SV("Saved"));
cscdElementAddClass(status, CSCD_SV("ok"));
cscdElementSetStyle(status, CSCD_SV("opacity"), CSCD_SV("1"));
```

To build content from data, `cscdElementAppendHTML` parses and appends (returning the first element
it made), `cscdElementAppendElement` makes one element, and `cscdElementRemove` takes an element out:

```c
CscdElement* list = cscdViewGet(ui, CSCD_SV("#tasks"));
for (size_t i = 0; i < count; ++i) {
    CscdStr html = cscdStrDupC("<li class=task>");
    cscdEscapeHTML(cscdSv(tasks[i].title), false, &html);
    cscdStrAppendC(&html, "</li>");
    CscdElement* li = cscdElementAppendHTML(list, cscdStrSv(&html));
    cscdElementOnClick(li, openTask, &tasks[i]);
    cscdStrFree(&html);
}
```

## Events

Events are the web's, with capture and bubbling. A listener is a function, its user data, and
(with `cscdElementAddEventListener`) what frees that data when the listener goes:

```c
static void onMouseDown(void* user, CscdEvent* e) {
    if (e->button == 2) return;               /* 0 left, 1 middle, 2 right */
    if (e->shift) { /* ... */ }
    float x = e->offsetX, y = e->offsetY;      /* relative to the element's border box */
    cscdEventStopPropagation(e);
    cscdEventPreventDefault(e);
}

cscdElementOn(el, CSCD_SV("mousedown"), onMouseDown, app);
```

For form controls there are listeners that get the value itself:

```c
static void preview(void* user, CscdSv value) { /* every change */ }
static void setWrap(void* user, bool on) { /* a checkbox */ }

cscdElementOnInput(cscdViewGet(ui, CSCD_SV("#name")), preview, app, NULL);
cscdElementOnChange(cscdViewGet(ui, CSCD_SV("#size")), resizeTo, app, NULL);  /* once committed */
cscdElementOnChecked(cscdViewGet(ui, CSCD_SV("#wrap")), setWrap, app, NULL);
```

| Kind | Events |
| --- | --- |
| Mouse | `click`, `dblclick`, `contextmenu`, `mousedown`, `mouseup`, `mousemove`, `mouseover`, `mouseout`, `mouseenter`, `mouseleave`, `wheel` |
| Keyboard | `keydown`, `keyup` (`e->key`: `"a"`, `"Enter"`, `"ArrowLeft"`…) |
| Forms | `input`, `change`, `submit`, `focus`, `blur`, `toggle` (`<details>`, popovers) |
| Drag and drop | `dragstart`, `drag`, `dragend`, `dragenter`, `dragover`, `dragleave`, `drop` |
| Touch | `touchstart`, `touchmove`, `touchend` |
| Others | `scroll`, `close` and `cancel` (dialogs) |

A handler may change the page freely, even remove the element the event is on its way through:
the elements that went get no more listeners, and their memory waits until the event is done.
`cscdElementClick(e)` is a click as the user's: the listeners, then what a click does (a checkbox
checks, a `<details>` opens, a form submits). `contextmenu` also comes from the Menu key and
Shift+F10.

## Forms and focus

Text fields and text areas have a caret, selection, undo and redo, the clipboard and word
movement (Ctrl+arrows). Checkboxes, radios (by `name`), range sliders, `<select>` (its options
open as a menu), labels and `<details>` work as on the web. Tab and Shift+Tab move the focus;
`cscdViewFocus(view, element)` sets it.

**Code fields**: `<textarea code>` indents with Tab / Shift+Tab and keeps the indentation on
Enter; `line-numbers` adds a gutter; `wrap="off"` scrolls sideways. `cscdElementSetHighlighter`
colors the text as it is typed.

**Input methods** (Chinese, Japanese, Korean): the composition shows at the caret, underlined.

**On phones** the on-screen keyboard follows the field: `type` (`email`, `url`, `tel`, `number`,
`search`, `password`), `inputmode`, `enterkeyhint`, `autocapitalize` and `autocorrect` /
`spellcheck` choose its kind, its action key and its corrections, as in mobile browsers
(`cscdViewInputTraits` says what the focused field asks for). The keyboard edits the field's whole
text: `cscdViewGetTextState` and `cscdViewSetTextState` give and take the text, the selection and
the part still being composed, and `cscdViewTextRangeRect` / `cscdViewTextOffsetAt` say where text
is. The windows use them; a host of its own (a game engine's surface) can too.

## The system's settings and the app's life

The window passes the system's theme, text size and "reduce motion" to the view
(`cscdViewSetSystemPreferences`): `prefers-color-scheme` and `prefers-reduced-motion` follow them,
the default canvas turns dark when the page's `color-scheme` names `dark`, and a larger text size
scales the root's default font size and the font-size keywords (`rem` and `em` follow; sizes in
`px` stay). What the app sets itself (`cscdViewSetDarkMode`, `cscdViewSetFeature`) wins.

```c
static void onApp(void* user, CscdAppEvent e) {
    if (e == CSCD_APP_BACKGROUND) saveDraft(user); /* (a phone may end a background app) */
}
cscdWindowOnAppEvent(win, onApp, app);
cscdWindowOpenUrl(win, "https://example.com");
cscdWindowPickFiles(win, "image/*,.pdf", true, onFiles, app); /* onFiles(app, &paths) */
```

## Saving files, the menu bar and the tray

`cscdWindowSaveFile` opens the system's dialog to save a file: the user chooses where, and the
callback writes it there (`path` is NULL when they cancelled). On phones the path is a file of the
app's own, which the system then copies where the user chose; on iOS the callback writes first and
the place is chosen after.

```c
static bool writeReport(void* user, const char* path) {
    return path && saveReportTo(user, path); /* true: written */
}
cscdWindowSaveFile(win, "report.pdf", ".pdf", writeReport, app);
```

On macOS, `cscdWindowSetMenuBar` puts the app's menus in the menu bar at the top of the screen. They
are the same `CscdMenu`s as the page's popup menus: each item of the menu given is a menu of the
bar, with submenus, checks, separators, disabled items and shortcuts (`"Ctrl+S"` is Command-S there).
Elsewhere it returns false: put the menu bar in the page with `cscdViewBindMenuBar`, so one program
does both.

```c
CscdMenu file = {0};
cscdMenuAdd(&file, cscdMenuItem(CSCD_SV("Save"), save, app, CSCD_SV("Ctrl+S")));
CscdMenu bar = {0};
cscdMenuAdd(&bar, cscdMenuSub(CSCD_SV("File"), file));
if (!cscdWindowSetMenuBar(win, bar)) bindPageMenuBar(ui); /* (not macOS) */
```

An icon in the tray (the notification area on Windows, a status item in the menu bar on macOS) is
a `CscdTray`: a click runs its action, a right click opens its menu. With `cscdWindowHide` and an
`onClose` that hides the window instead of closing it, the app lives in the tray. There is no tray on
Linux yet, nor on phones: `cscdTrayNew` returns NULL there.

```c
CscdTray* tray = cscdTrayNew(win, icon, "My app");    /* icon: a CscdImage, 32x32 is plenty */
cscdTrayOnClick(tray, cscdAction(showWindow, win));
CscdMenu menu = {0};
cscdMenuAdd(&menu, cscdMenuItem(CSCD_SV("Quit"), quit, app, CSCD_SV("")));
cscdTraySetMenu(tray, menu);
/* ... */
cscdTrayFree(tray);                                    /* before the window */
```

## Popup menus

Menus are elements in the page (`div.cscd-menu`), so CSS styles them. Items take a label, a
shortcut, an icon, a check mark, a tooltip, a separator or a submenu, which can be filled as it
opens (`item.fill`). The keyboard works: arrows, Enter, Esc and letters. A menu given to the view
becomes the view's: it frees the items, and runs each one's `action.freeUser` when it closes.

```c
static void duplicate(void* user) { /* ... */ }

CscdMenu menu = {0};
cscdMenuAdd(&menu, cscdMenuItem(CSCD_SV("Duplicate"), duplicate, app, CSCD_SV("Ctrl+D")));
CscdMenuItem* snap = cscdMenuAdd(&menu, cscdMenuItem(CSCD_SV("Snap to Grid"), toggleSnap, app, CSCD_SV("")));
snap->checkable = true;
snap->checked = app->snapping;
cscdMenuAdd(&menu, cscdMenuSeparator());

cscdViewShowMenu(ui, menu, e->x, e->y, 0);          /* at the pointer (in a contextmenu handler) */
cscdViewShowMenuAt(ui, button, otherMenu);           /* under a button */
cscdViewBindMenuBar(ui, fileEntry, fillFileMenu, app, NULL); /* a menu bar entry */
```

`cscdViewOnMenuItem` decorates each row as it is made: paint its icon slot, for example.

## Dialogs

`<dialog>` works as on the web. `cscdViewShowModal` centers it over a dimmed page that takes no
input while it is open; Esc sends `cancel`, then closes it. `cscdViewCloseDialog(view, dialog,
value)` sets its `returnvalue` and sends `close`, and a `<form method="dialog">` closes its dialog
with the submitting button's value.

```html
<dialog id="ask">
  <form method="dialog">
    <p>Discard the changes?</p>
    <button value="discard">Discard</button>
    <button value="cancel">Cancel</button>
  </form>
</dialog>
```

```c
static void onAskClosed(void* user, CscdEvent* e) {
    if (cscdSvEqC(cscdElementAttrC(e->currentTarget, "returnvalue"), "discard")) discard(user);
}

CscdElement* ask = cscdViewGet(ui, CSCD_SV("#ask"));
cscdElementOn(ask, CSCD_SV("close"), onAskClosed, app);
cscdViewShowModal(ui, ask);
```

## Popovers and tooltips

Elements with the `popover` attribute open with `cscdViewShowPopover(view, popover, anchor)`, under
the anchor or above it when there is no room. A press outside or Esc hides them, and they get
`toggle` when they open and close.

The `title` attribute shows as a tooltip (`div.cscd-tooltip`) after a short hover
(`cscdViewSetTooltipDelay`).

## Drag and drop

Elements with `draggable="true"` start a drag. The source gets `dragstart`, `drag` and `dragend`;
targets get `dragenter`, `dragover`, `dragleave` and `drop`, and accept by calling
`cscdEventPreventDefault` in `dragover`. A `CscdDataTransfer` carries the data
(`cscdDataTransferSetData`, `cscdDataTransferGetData`), and a picture of the element follows the
pointer (`cscdDataTransferSetDragImage` replaces it).

Files and text dragged in from other applications arrive the same way, in
`e->dataTransfer->files` and `cscdDataTransferGetData(e->dataTransfer, CSCD_SV("text/plain"))`.

## Touch

`touchstart`, `touchmove` and `touchend` reach the page. Unless it calls `cscdEventPreventDefault`
on `touchstart`, one finger scrolls (with momentum), a tap clicks, a long press opens the context
menu, and two fingers pan and pinch (as wheel and Ctrl+wheel events, as browsers do).

## Splitters

An element with a `splitter` attribute resizes the elements on either side of it when dragged,
along its parent's flex direction. It gets `input` while dragged and `change` after.

## A sheet under every page

A sheet that every page of a program shares goes under the document's own with
`cscdViewSetBaseStyleSheet`; it stays when the view loads another page.

## Clipboard

Text fields use the system clipboard by themselves. A window connects the view to it
(`cscdViewSetClipboardHooks`): text, a picture, and the files copied in the file manager. Without a
window, give the view your own hooks. The program reaches it through them too:

```c
cscdViewCopyText(ui, CSCD_SV("#3390ff"));
cscdViewCopyImage(ui, chart);                 /* a CscdImage, as a PNG */
CscdStrVec files = {0};
cscdViewClipboardFiles(ui, &files);           /* on phones, copies (a picture as a PNG file) */
```

On touch screens a text field's selection gets handles and an edit menu (the system's, or the
view's own). A window of your own gives the view its menu with `cscdViewOnEditMenu` (and its
magnifier with `cscdViewOnMagnifier`), and calls `cscdViewEditAction` with what the user chose.

## Timers, frames and threads

Timers run on the interface's thread, inside `cscdViewUpdate`. The timer takes the action: its
`freeUser` runs when the timer is cleared, or after a timeout has run.

```c
int id = cscdViewSetInterval(ui, cscdAction(tick, app), 16);
cscdViewSetTimeout(ui, cscdAction(hideToast, app), 3000);
cscdViewClearTimer(ui, id);
```

`cscdWindowOnFrame(win, fn, user)` runs once per frame before painting, for animation loops and live
data.

Work in other threads hands its results to the interface with `cscdWindowPost`, which runs a
function on the window's thread and may be called from any thread:

```c
typedef struct Result { CscdWindow* win; char text[256]; } Result;

static void showResult(void* user) {
    Result* r = user;
    cscdElementSetText(cscdViewGet(cscdWindowView(r->win), CSCD_SV("#out")), cscdSv(r->text));
}

/* in the worker thread */
Result* r = malloc(sizeof *r);
r->win = win;
longComputation(r->text);
CscdAction a = {showResult, r, free};
cscdWindowPost(win, a);
```

The DOM, the view and the canvas belong to the interface's thread; reach them only from there.

## Templates and long lists

A `<template>` holds a piece of interface that is not shown: its content stays out of the document
(not styled, laid out, or found by queries) until copies of it are made.

```html
<template id=row><div class="row" data-id="{{id}}"><b>{{name}}</b> <span>{{size}}</span></div></template>
<div id=files style="height: 400px; overflow: auto"></div>
```

```c
CscdElement* copy = cscdTemplateInstantiate(cscdViewGet(ui, CSCD_SV("#row"))); /* the caller's */
cscdElementFill(copy, CSCD_SV("name"), CSCD_SV("notes.txt"));                    /* every {{name}} */
cscdNodeAppendChild(&cscdViewGet(ui, CSCD_SV("#files"))->node, &copy->node);
```

`cscdElementClone(e, deep)` copies any element (its tag and attributes, and with `deep` what it
holds; not its listeners or painter).

A list of many items makes only the rows that show, a few more on each side, and makes the others
as the list scrolls; a hundred thousand rows cost what twenty do:

```c
static void fillFile(void* user, CscdElement* row, size_t i) {
    const File* f = &((App*)user)->files[i];
    cscdElementFill(row, CSCD_SV("name"), cscdSv(f->name));
    cscdElementFill(row, CSCD_SV("size"), cscdSv(f->size));
}

cscdViewBindList(ui, cscdViewGet(ui, CSCD_SV("#files")), cscdViewGet(ui, CSCD_SV("#row")),
                  app->fileCount, 28 /* px a row */, fillFile, app, NULL);
cscdViewListSetCount(ui, list, app->fileCount); /* the items changed */
cscdViewListRefresh(ui, list);                  /* the rows shown filled again */
```

A row scrolled away is dropped, and the next item gets a fresh copy: handle the rows' events on the
list itself (`event->target` is the row element), not on each row.

## Data binding

`cascade/bind.h` keeps the page in step with a model of named values, through attributes, with no
code for each element:

```html
<div id=app>
  <h1 data-text=title></h1>
  <input data-value=title>                       <!-- typing sets the model -->
  <p data-show=busy>Working…</p>
  <ul data-each=tasks>
    <template><li data-text=name data-class-done=done></li></template>
  </ul>
</div>
```

```c
CscdModel* m = cscdModelNew();
cscdModelSet(m, CSCD_SV("title"), CSCD_SV("Today"));
CscdModel* t = cscdModelListAdd(m, CSCD_SV("tasks"));
cscdModelSet(t, CSCD_SV("name"), CSCD_SV("Write the report"));
cscdViewBind(ui, cscdViewGet(ui, CSCD_SV("#app")), m);
...
cscdModelSetBool(t, CSCD_SV("done"), true); /* shows at the next update */
```

`data-text`, `data-value` and `data-checked` (both ways), `data-show` / `data-hide`, `data-class-x`,
`data-attr-x` and `data-each` (a `<template>` repeated for each item of a list) take a name, `!name`
for the test turned around. The page follows the model at the view's next update and touches only
what changed: a list keeps the copies of the items it still has (moved into the new order), makes
the new ones and removes the others. `cscdModelOnChange` tells what the page changed;
`cscdViewModelOf` gives the item a copy shows (for its events).

## Accessibility

A window offers the page to the system's screen readers: UI Automation on Windows (Narrator, NVDA,
JAWS), NSAccessibility on macOS (VoiceOver). Elements get their role from `role="..."` or their tag
(buttons, links, fields, checkboxes, sliders, headings, lists, tables...), their name from
`aria-label`, `aria-labelledby`, a `<label>`, `alt` or their text, and their states (checked,
disabled, expanded, focused...) from the page; plain containers are left out and their contents
lifted to their parent. Readers can press buttons, toggle checkboxes, type into fields and move
sliders, and they hear about focus and value changes as they happen.

Write the page as for the web and it reads well: real `<button>`s, `<label for>` on fields, `alt` on
pictures, `aria-label` on buttons that only show an icon. `cscdAccessibleRole`, `Name`, `State`,
`Children` and the rest give the same tree to a host with its own accessibility bridge.
`CSCD_FEATURE_ACCESSIBILITY` turns it off. (Linux's AT-SPI is not there yet.)

## The inspector

With `CSCD_FEATURE_INSPECTOR` on (debug builds turn it on in a window), F12 or Ctrl+Shift+I shows
the boxes of the element under the pointer (margin, border, padding, content) and a panel with its
size, its computed style and the rules that apply to it, the one that wins first. A click pins an
element; Esc closes. `cscdViewInspect(ui, element)` opens it on one from code.

## Features to turn off

A view's behaviour is made of features, each of which an application can turn off (or on):
`cscdViewSetFeature(ui, CSCD_FEATURE_ANIMATIONS, false)`.

| Feature | Default | What it does |
| --- | --- | --- |
| `PAINT_THREADS` | on | large frames painted by bands on several threads |
| `PARTIAL_REPAINT` | on | only invalidated elements painted again |
| `ANIMATIONS` | on | transitions and animations; off, styles change at once and `prefers-reduced-motion` holds |
| `SMOOTH_SCROLL` | on | wheel steps glide |
| `TOOLTIPS` | on | `title` shown after a hover |
| `LIVE_RELOAD` | off (debug: on) | style sheet files read again when they change |
| `ACCESSIBILITY` | on | the page offered to screen readers |
| `INSPECTOR` | off (debug: on) | F12 shows boxes, style and rules |
| `STATS` | off | the window shows what the last frame cost |
| `GPU` | on | the window presents through Vulkan or OpenGL (off: the system's software path) |
| `WARN_FILES`, `WARN_CSS`, `WARN_SELECTORS` | on | the warnings of each kind |

`CASCADE_FEATURES` in the environment changes the defaults without rebuilding:
`CASCADE_FEATURES=+stats,-gpu,+inspector ./app`.
