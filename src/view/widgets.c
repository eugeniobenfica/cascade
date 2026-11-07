/* The parts of a CscdView that are interface rather than document: popup menus (context menus, menu
 * bars, <select> lists), dialogs, popovers, tooltips, drag and drop, splitters, virtual lists, the
 * clipboard from the program, and the line layout and undo of text fields. Touch is touch.c's. */
#include "view/viewInternal.h"
#include "text/bidiInternal.h"

#include <ctype.h>
#include <stdio.h>


static void setPx(CscdElement* e, const char* prop, float v) {
    char b[32];
    snprintf(b, sizeof b, "%.2fpx", v);
    cscdElementSetStyle(e, cscdSv(prop), cscdSv(b));
}

static void setNum(CscdElement* e, const char* prop, float v) {
    char b[32];
    snprintf(b, sizeof b, "%g", v);
    cscdElementSetStyle(e, cscdSv(prop), cscdSv(b));
}

static bool inside(CscdElement* e, const CscdElement* ancestor) {
    for (; e; e = parentEl(e))
        if (e == ancestor) return true;
    return false;
}

static CscdElement* overlayParent(CscdDocument* d) {
    CscdElement* b = cscdDocumentBody(d);
    return b ? b : cscdDocumentElement(d);
}

static CscdColor colorOf(const CscdElement* e) { return e && e->style ? e->style->color : cscdRgb(0, 0, 0); }

static void strokeIcon(CscdCanvas* c, const CscdPath* p, const CscdElement* e, float width) {
    CscdStrokeStyle st = cscdStrokeStyle(width);
    st.cap = CSCD_CAP_ROUND;
    st.join = CSCD_JOIN_ROUND;
    CscdShader sh = cscdShaderSolid(colorOf(e));
    cscdCanvasStrokePathShader(c, p, &sh, &st);
    cscdShaderFini(&sh);
    cscdStrokeStyleFini(&st);
}

/* The check mark and the submenu arrow, drawn in the row's text color. */
static void paintCheck(void* user, CscdCanvas* c, float w, float h) {
    CscdPath p = {0};
    float s = cscdMinf(w, h);
    float x = (w - s) / 2, y = (h - s) / 2;
    cscdPathMoveTo(&p, x + s * 0.2f, y + s * 0.52f);
    cscdPathLineTo(&p, x + s * 0.42f, y + s * 0.74f);
    cscdPathLineTo(&p, x + s * 0.8f, y + s * 0.3f);
    strokeIcon(c, &p, (const CscdElement*)user, 1.6f);
    cscdPathFree(&p);
}

static void paintArrow(void* user, CscdCanvas* c, float w, float h) {
    CscdPath p = {0};
    float cx = w / 2, cy = h / 2;
    cscdPathMoveTo(&p, cx - 2, cy - 4);
    cscdPathLineTo(&p, cx + 2, cy);
    cscdPathLineTo(&p, cx - 2, cy + 4);
    strokeIcon(c, &p, (const CscdElement*)user, 1.5f);
    cscdPathFree(&p);
}

static bool hasSubmenu(const CscdMenuItem* m) { return m->submenu.len || m->fill.fn; }

static bool editable(const CscdElement* e) { return e && isTextControl(e) && !cscdElementHasAttributeC(e, "readonly") && !cscdElementDisabled(e); }

/* ---- menu items ---------------------------------------------------------------------------------- */

CscdMenuItem cscdMenuItem(CscdSv label, CscdActionFn fn, void* user, CscdSv shortcut) {
    CscdMenuItem m;
    memset(&m, 0, sizeof m);
    m.label = cscdStrDup(label);
    m.shortcut = cscdStrDup(shortcut);
    m.enabled = true;
    m.action.fn = fn;
    m.action.user = user;
    return m;
}

CscdMenuItem cscdMenuSeparator(void) {
    CscdMenuItem m;
    memset(&m, 0, sizeof m);
    m.separator = true;
    m.enabled = true;
    return m;
}

CscdMenuItem cscdMenuSub(CscdSv label, CscdMenu submenu) {
    CscdMenuItem m = cscdMenuItem(label, NULL, NULL, CSCD_SV(""));
    m.submenu = submenu;
    return m;
}

CscdMenuItem* cscdMenuAdd(CscdMenu* m, CscdMenuItem item) {
    cscdVecPush(m, item);
    return &cscdVecLast(m);
}

void cscdMenuItemFini(CscdMenuItem* it) {
    cscdStrFree(&it->label);
    cscdStrFree(&it->shortcut);
    cscdStrFree(&it->icon);
    cscdStrFree(&it->tooltip);
    cscdStrFree(&it->id);
    cscdMenuFree(&it->submenu);
    if (it->fill.freeUser && it->fill.user) it->fill.freeUser(it->fill.user);
    cscdActionFree(&it->action);
}

void cscdMenuFree(CscdMenu* m) {
    for (size_t i = 0; i < m->len; ++i) cscdMenuItemFini(&m->items[i]);
    cscdVecFree(m);
}

void openMenuFree(OpenMenu* m) {
    if (m->ownsItems) cscdMenuFree(&m->items); /* (a borrowed submenu's array stays with its item) */
    cscdVecFree(&m->rows);
    cscdFree(m);
}

/* ---- text field geometry ------------------------------------------------------------------------- */

void editLines(CscdSv text, const CscdFont* f, float width, bool wrap, float ls, EditLineVec* out) {
    out->len = 0;
    size_t pos = 0;
    for (;;) {
        size_t nl = cscdSvFindChar(text, '\n', pos);
        size_t pend = nl == CSCD_NPOS ? text.len : nl;
        if (!wrap || width <= 0) {
            EditLine l = {pos, pend};
            cscdVecPush(out, l);
        } else {
            size_t start = pos;
            do {
                size_t end = pend;
                if (cscdFontMeasure(cscdSvSub(text, start, pend - start), f, ls, 0) > width) {
                    size_t lastSpace = CSCD_NPOS;
                    for (size_t k = start; k < pend;) {
                        size_t kn = nextChar(text, k);
                        if (text.ptr[k] == ' ') lastSpace = k;
                        if (cscdFontMeasure(cscdSvSub(text, start, kn - start), f, ls, 0) > width) {
                            end = lastSpace != CSCD_NPOS && lastSpace > start ? lastSpace + 1 : cscdMaxz(k, nextChar(text, start));
                            break;
                        }
                        k = kn;
                    }
                }
                EditLine l = {start, end};
                cscdVecPush(out, l);
                start = end;
            } while (start < pend);
        }
        if (nl == CSCD_NPOS) break;
        pos = nl + 1;
    }
}

size_t editLineOf(const EditLineVec* lines, size_t pos) {
    size_t best = 0;
    for (size_t i = 0; i < lines->len; ++i)
        if (lines->items[i].start <= pos) best = i;
        else break;
    return best;
}

size_t editHit(CscdSv text, EditLine line, const CscdFont* f, float x, float ls) {
    size_t best = line.start;
    float bestD = 1e9f;
    for (size_t i = line.start;;) {
        float w = cscdFontMeasure(cscdSvSub(text, line.start, i - line.start), f, ls, 0);
        float d = fabsf(w - x);
        if (d < bestD) { bestD = d; best = i; }
        if (w > x || i >= line.end) break;
        i = nextChar(text, i);
    }
    return best;
}

float editLineHeight(const CscdComputedStyle* s, const CscdFont* f) {
    if (s->lineHeight < 0 && s->lineHeightFactor <= 0) return cscdFontAscent(f) + cscdFontDescent(f);
    return cscdStyleLineHeight(s, cscdFontAscent(f), cscdFontDescent(f));
}

bool editWraps(const CscdElement* e) { return !cscdSvIEqC(cscdElementAttrC(e, "wrap"), "off"); }

float editAlignShift(const CscdComputedStyle* s, float textWidth, float width) {
    if (textWidth >= width) return 0;
    CscdTextAlign a = s->textAlign;
    if (a == CSCD_ALIGN_START) a = s->rtl ? CSCD_ALIGN_RIGHT : CSCD_ALIGN_LEFT;
    else if (a == CSCD_ALIGN_END) a = s->rtl ? CSCD_ALIGN_LEFT : CSCD_ALIGN_RIGHT;
    if (a == CSCD_ALIGN_CENTER) return (width - textWidth) / 2;
    if (a == CSCD_ALIGN_RIGHT) return width - textWidth;
    return 0;
}

/* ---- a line of a text field as it shows: its bidirectional runs ---------------------------------- */

void editRuns(CscdSv text, EditLine line, const CscdFont* f, float ls, bool rtl, EditRunVec* out) {
    out->len = 0;
    const CscdSv s = cscdSvSub(text, line.start, line.end - line.start);
    bool needed = rtl;
    for (size_t i = 0; i < s.len && !needed; ++i) needed = (unsigned char)s.ptr[i] >= 0xD6; /* (no code point from U+0580 up: left to right) */
    if (!needed) {
        cscdVecPush(out, ((EditRun){line.start, line.end, false, 0, cscdFontMeasure(s, f, ls, 0)}));
        return;
    }
    CscdU32Vec cps = {0};
    CscdSizeVec at = {0}, order = {0};
    for (size_t i = 0; i < s.len;) {
        cscdVecPush(&at, line.start + i);
        cscdVecPush(&cps, cscdUtf8Next(s, &i));
    }
    cscdVecPush(&at, line.end);
    CscdBytes levels = {0};
    cscdVecResize(&levels, cps.len ? cps.len : 1);
    if (cps.len) {
        cscdBidiLevels(cps.items, cps.len, rtl ? 1 : 0, levels.items);
        cscdBidiLineLevels(cps.items, cps.len, rtl ? 1 : 0, levels.items);
    }
    cscdBidiVisualOrder(levels.items, cps.len, &order);
    /* runs: characters next to each other in the text and at the same level, left to right */
    float x = 0;
    for (size_t k = 0; k < order.len;) {
        const size_t first = order.items[k];
        const uint8_t level = levels.items[first];
        const bool r = level & 1;
        size_t lo = first, hi = first, n = 1;
        while (k + n < order.len) {
            const size_t c = order.items[k + n];
            if (levels.items[c] != level || c != (r ? lo - 1 : hi + 1)) break;
            if (r) lo = c; else hi = c;
            ++n;
        }
        EditRun run = {at.items[lo], at.items[hi + 1], r, x, 0};
        run.w = cscdFontMeasure(cscdSvSub(text, run.start, run.end - run.start), f, ls, 0);
        x += run.w;
        cscdVecPush(out, run);
        k += n;
    }
    if (!out->len) cscdVecPush(out, ((EditRun){line.start, line.end, rtl, 0, 0}));
    cscdVecFree(&cps);
    cscdVecFree(&at);
    cscdVecFree(&order);
    cscdVecFree(&levels);
}

float editRunsWidth(const EditRunVec* runs) { return runs->len ? runs->items[runs->len - 1].x + runs->items[runs->len - 1].w : 0; }

/* The run a caret at pos is in: the one pos is inside of, else one it ends or starts. */
static const EditRun* runAt(const EditRunVec* runs, size_t pos) {
    for (size_t i = 0; i < runs->len; ++i)
        if (pos > runs->items[i].start && pos < runs->items[i].end) return &runs->items[i];
    for (size_t i = 0; i < runs->len; ++i)
        if (pos == runs->items[i].end) return &runs->items[i];
    for (size_t i = 0; i < runs->len; ++i)
        if (pos == runs->items[i].start) return &runs->items[i];
    return runs->len ? &runs->items[0] : NULL;
}

/* How far into a run (from its left) position pos is. */
static float runOffset(const EditRun* r, CscdSv text, const CscdFont* f, float ls, size_t pos) {
    pos = cscdMinz(cscdMaxz(pos, r->start), r->end);
    const float before = cscdFontMeasure(cscdSvSub(text, r->start, pos - r->start), f, ls, 0);
    return r->rtl ? r->w - before : before;
}

float editCaretX(const EditRunVec* runs, CscdSv text, const CscdFont* f, float ls, size_t pos) {
    const EditRun* r = runAt(runs, pos);
    return r ? r->x + runOffset(r, text, f, ls, pos) : 0;
}

size_t editRunsHit(const EditRunVec* runs, CscdSv text, const CscdFont* f, float ls, float x) {
    if (!runs->len) return 0;
    const EditRun* r = &runs->items[runs->len - 1];
    for (size_t i = 0; i < runs->len; ++i)
        if (x < runs->items[i].x + runs->items[i].w) { r = &runs->items[i]; break; }
    const float local = x - r->x;
    const EditLine line = {r->start, r->end};
    return editHit(text, line, f, r->rtl ? r->w - local : local, ls);
}

void editRunsSpan(const EditRunVec* runs, CscdSv text, const CscdFont* f, float ls, size_t a, size_t z, CscdFloatVec* out) {
    out->len = 0;
    for (size_t i = 0; i < runs->len; ++i) {
        const EditRun* r = &runs->items[i];
        const size_t ra = cscdMaxz(a, r->start), rz = cscdMinz(z, r->end);
        if (rz <= ra) continue;
        const float xa = runOffset(r, text, f, ls, ra), xz = runOffset(r, text, f, ls, rz);
        cscdVecPush(out, r->x + cscdMinf(xa, xz));
        cscdVecPush(out, r->x + cscdMaxf(xa, xz));
    }
}


void viewTextAreaRange(CscdView* v, CscdElement* e, float* maxX, float* maxY) {
    (void)v;
    *maxX = *maxY = 0;
    if (!e || !e->layout || !e->style) return;
    CscdLayoutBox* b = e->layout;
    CscdRect cb = cscdBoxContentBox(b);
    CscdFont f = cscdFontForStyle(e->style);
    CscdStr val = cscdElementValue(e);
    bool wrap = editWraps(e);
    EditLineVec lines = {0};
    editLines(cscdStrSv(&val), &f, cb.w, wrap, e->style->letterSpacing, &lines);
    float lh = editLineHeight(e->style, &f);
    *maxY = cscdMaxf(0.f, lines.len * lh - cb.h);
    if (!wrap) {
        float widest = 0;
        for (size_t i = 0; i < lines.len; ++i)
            widest = cscdMaxf(widest, cscdFontMeasure(cscdSvSub(cscdStrSv(&val), lines.items[i].start, lines.items[i].end - lines.items[i].start), &f,
                                                  e->style->letterSpacing, 0));
        *maxX = cscdMaxf(0.f, widest + 2 - cb.w);
    }
    cscdVecFree(&lines);
    cscdStrFree(&val);
}

size_t viewTextHit(CscdView* v, CscdElement* f, float x, float y) {
    if (!f || !f->layout || !f->style) return 0;
    CscdLayoutBox* b = f->layout;
    CscdRect r = cscdViewElementRect(v, f);
    CscdFont font = cscdFontForStyle(b->style);
    float ls = b->style->letterSpacing;
    float lx = x - (r.x + b->border[3] + b->padding[3]);
    float ly = y - (r.y + b->border[0] + b->padding[0]);
    CscdSv val = cscdStrSv(&f->editValue);
    if (cscdElementIs(f, "textarea")) {
        EditLineVec lines = {0};
        editLines(val, &font, cscdBoxContentBox(b).w, editWraps(f), ls, &lines);
        float lh = editLineHeight(b->style, &font);
        int i = cscdIntOf(floorf((ly + f->scrollTop) / cscdMaxf(1.f, lh)));
        i = cscdClampi(i, 0, (int)lines.len - 1);
        EditRunVec runs = {0};
        editRuns(val, lines.items[i], &font, ls, b->style->rtl, &runs);
        size_t hit = editRunsHit(&runs, val, &font, ls, lx + (editWraps(f) ? 0 : f->scrollLeft));
        cscdVecFree(&runs);
        cscdVecFree(&lines);
        return hit;
    }
    /* a single line field scrolls to keep the caret in view (as the painter does) */
    EditRunVec runs = {0};
    EditLine all = {0, val.len};
    editRuns(val, all, &font, ls, b->style->rtl, &runs);
    float caretX = editCaretX(&runs, val, &font, ls, cscdMinz(f->caret, val.len));
    float cw = cscdBoxContentBox(b).w;
    if (v->doc->focused == f && caretX > cw - 2) lx += caretX - cw + 2;
    lx -= editAlignShift(b->style, editRunsWidth(&runs), cw);
    const size_t hit = editRunsHit(&runs, val, &font, ls, lx);
    cscdVecFree(&runs);
    return hit;
}

void viewEnsureCaretVisible(CscdView* v, CscdElement* e) {
    if (!e || !cscdElementIs(e, "textarea") || !e->layout || !e->style) return;
    CscdLayoutBox* b = e->layout;
    CscdRect cb = cscdBoxContentBox(b);
    CscdFont f = cscdFontForStyle(e->style);
    CscdSv val = cscdStrSv(&e->editValue);
    bool wrap = editWraps(e);
    EditLineVec lines = {0};
    editLines(val, &f, cb.w, wrap, e->style->letterSpacing, &lines);
    float lh = editLineHeight(e->style, &f);
    size_t i = editLineOf(&lines, e->caret);
    float top = i * lh;
    if (top < e->scrollTop) e->scrollTop = top;
    else if (top + lh > e->scrollTop + cb.h) e->scrollTop = top + lh - cb.h;
    if (!wrap) {
        EditRunVec runs = {0};
        editRuns(val, lines.items[i], &f, e->style->letterSpacing, e->style->rtl, &runs);
        float x = editCaretX(&runs, val, &f, e->style->letterSpacing, e->caret);
        cscdVecFree(&runs);
        if (x < e->scrollLeft) e->scrollLeft = cscdMaxf(0.f, x - cb.w * 0.25f);
        else if (x > e->scrollLeft + cb.w - 2) e->scrollLeft = x - cb.w + cb.w * 0.25f;
    }
    cscdVecFree(&lines);
    float mx, my;
    viewTextAreaRange(v, e, &mx, &my);
    e->scrollTop = cscdClampf(e->scrollTop, 0.f, my);
    e->scrollLeft = cscdClampf(e->scrollLeft, 0.f, mx);
}

/* Where offset pos of a field's text shows, as a caret there would (relative to the viewport); line:
 * which line of a textarea it is on (0 in a single line field). */
CscdRect viewFieldPosRect(CscdView* v, CscdElement* e, size_t pos, size_t* line) {
    CscdLayoutBox* b = e->layout;
    CscdRect r = cscdViewElementRect(v, e);
    CscdRect cb = cscdRectTranslated(cscdBoxContentBox(b), r.x - b->x, r.y - b->y);
    CscdFont f = cscdFontForStyle(e->style);
    float ls = e->style->letterSpacing;
    CscdSv val = cscdStrSv(&e->editValue);
    pos = cscdMinz(pos, val.len);
    float h = cscdFontAscent(&f) + cscdFontDescent(&f);
    *line = 0;
    if (cscdElementIs(e, "textarea")) {
        EditLineVec lines = {0};
        editLines(val, &f, cb.w, editWraps(e), ls, &lines);
        float lh = editLineHeight(e->style, &f);
        size_t i = editLineOf(&lines, pos);
        EditRunVec runs = {0};
        editRuns(val, lines.items[i], &f, ls, e->style->rtl, &runs);
        float x = editCaretX(&runs, val, &f, ls, pos);
        cscdVecFree(&runs);
        cscdVecFree(&lines);
        *line = i;
        return cscdRect(cb.x + x - (editWraps(e) ? 0 : e->scrollLeft), cb.y + i * lh - e->scrollTop + (lh - h) / 2, 1, h);
    }
    EditRunVec runs = {0};
    EditLine all = {0, val.len};
    editRuns(val, all, &f, ls, e->style->rtl, &runs);
    float x = editCaretX(&runs, val, &f, ls, pos);
    /* (the field scrolls to keep the caret in view) */
    const float caretX = editCaretX(&runs, val, &f, ls, cscdMinz(e->caret, val.len));
    if (caretX > cb.w - 2) x -= caretX - (cb.w - 2);
    else x += editAlignShift(e->style, editRunsWidth(&runs), cb.w);
    cscdVecFree(&runs);
    return cscdRect(cb.x + x, cb.y + (cb.h - h) / 2, 1, h);
}

CscdRect cscdViewCaretRect(CscdView* v) {
    CscdElement* e = v->doc->focused;
    if (!e || !isTextControl(e) || !e->layout || !e->style) return cscdRect(0, 0, 0, 0);
    size_t line;
    return viewFieldPosRect(v, e, e->caret, &line);
}

/* ---- the focused field as input methods see it ----------------------------------------------------- */

CscdElement* viewFocusedField(CscdView* v) {
    CscdElement* e = v->doc->focused;
    return e && isTextControl(e) && e->valueDirty ? e : NULL;
}

/* i moved back to the start of the character it falls in */
static size_t charStart(CscdSv s, size_t i) {
    i = cscdMinz(i, s.len);
    while (i > 0 && i < s.len && ((unsigned char)s.ptr[i] & 0xC0) == 0x80) --i;
    return i;
}

bool cscdViewGetTextState(CscdView* v, CscdTextState* out) {
    memset(out, 0, sizeof *out);
    CscdElement* e = viewFocusedField(v);
    if (!e) return false;
    out->text = cscdStrCopy(&e->editValue);
    out->selStart = cscdMinz(e->selAnchor, e->editValue.len);
    out->selEnd = cscdMinz(e->caret, e->editValue.len);
    if (v->markEnd > v->markStart && v->markEnd <= e->editValue.len) {
        out->composeStart = v->markStart;
        out->composeEnd = v->markEnd;
    }
    return true;
}

void cscdViewSetTextState(CscdView* v, const CscdTextState* s) {
    CscdElement* e = viewFocusedField(v);
    if (!e || cscdElementHasAttributeC(e, "readonly") || cscdElementDisabled(e)) return;
    /* a single line field keeps to one line: the offsets past a line break it drops move back */
    CscdStr text = {0};
    size_t sel[4] = {s->selStart, s->selEnd, s->composeStart, s->composeEnd};
    const bool oneLine = cscdElementIs(e, "input");
    for (size_t i = 0; i < s->text.len; ++i) {
        const char c = s->text.ptr[i];
        if (oneLine && (c == '\n' || c == '\r')) {
            for (int k = 0; k < 4; ++k)
                if (sel[k] > text.len) --sel[k];
            continue;
        }
        cscdStrAppendChar(&text, c);
    }
    const CscdSv t = cscdStrSv(&text);
    for (int k = 0; k < 4; ++k) sel[k] = charStart(t, sel[k]);
    const bool changed = !cscdStrEq(&text, &e->editValue);
    if (changed) {
        cscdViewHideSelectionHandles(v); /* (typing takes them away) */
        viewPushUndo(v, e, 1);
        cscdStrFree(&e->editValue);
        e->editValue = text;
        e->valueDirty = true;
        cscdNodeMarkDirty(&e->node);
    } else cscdStrFree(&text);
    e->selAnchor = sel[0];
    e->caret = sel[1];
    v->markStart = sel[2] < sel[3] ? sel[2] : 0;
    v->markEnd = sel[2] < sel[3] ? sel[3] : 0;
    cscdStrClear(&v->composition);
    v->caretTime = viewNow(v);
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &e);
    if (changed) viewSendSimple(v, "input", e);
    if (e) viewEnsureCaretVisible(v, e);
    viewUnwatch(v, mark);
    cscdViewInvalidate(v);
}

void cscdTextStateFini(CscdTextState* s) { cscdStrFree(&s->text); }

CscdRect cscdViewTextRangeRect(CscdView* v, size_t start, size_t end) {
    CscdElement* e = viewFocusedField(v);
    if (!e || !e->layout || !e->style) return cscdRect(0, 0, 0, 0);
    const size_t a = cscdMinz(start, end), z = cscdMaxz(start, end);
    size_t la, lz;
    const CscdRect ra = viewFieldPosRect(v, e, a, &la);
    const CscdRect rz = viewFieldPosRect(v, e, z, &lz);
    if (la == lz) {
        const float x0 = cscdMinf(ra.x, rz.x), x1 = cscdMaxf(ra.x, rz.x);
        return cscdRect(x0, ra.y, cscdMaxf(1.f, x1 - x0), ra.h);
    }
    /* (to the end of its first line) */
    CscdLayoutBox* b = e->layout;
    const CscdRect r = cscdViewElementRect(v, e);
    const CscdRect cb = cscdRectTranslated(cscdBoxContentBox(b), r.x - b->x, r.y - b->y);
    return cscdRect(ra.x, ra.y, cscdMaxf(1.f, cscdRectRight(cb) - ra.x), ra.h);
}

size_t cscdViewTextOffsetAt(CscdView* v, float x, float y) {
    CscdElement* e = viewFocusedField(v);
    return e ? viewTextHit(v, e, x, y) : 0;
}

CscdInputTraits cscdViewInputTraits(CscdView* v) {
    CscdInputTraits t = {CSCD_INPUT_NONE, CSCD_ENTER_DEFAULT, CSCD_CAPITALIZE_NONE, false};
    CscdElement* e = v->doc->focused;
    if (!e || !isTextControl(e) || cscdElementDisabled(e) || cscdElementHasAttributeC(e, "readonly")) return t;
    const bool area = cscdElementIs(e, "textarea");
    const CscdSv type = area ? CSCD_SV("") : cscdElementHasAttributeC(e, "type") ? cscdElementAttrC(e, "type") : CSCD_SV("text");
    static const struct { const char* name; CscdInputKind kind; } kTypes[] = {
        {"password", CSCD_INPUT_PASSWORD}, {"email", CSCD_INPUT_EMAIL}, {"url", CSCD_INPUT_URL}, {"tel", CSCD_INPUT_TEL},
        {"search", CSCD_INPUT_SEARCH}, {"number", CSCD_INPUT_DECIMAL}};
    t.kind = area ? CSCD_INPUT_MULTILINE : CSCD_INPUT_TEXT;
    for (size_t i = 0; i < CSCD_COUNT(kTypes); ++i)
        if (cscdSvIEqC(type, kTypes[i].name)) t.kind = kTypes[i].kind;
    if (t.kind != CSCD_INPUT_PASSWORD && cscdElementHasAttributeC(e, "inputmode")) {
        static const struct { const char* name; CscdInputKind kind; } kModes[] = {
            {"none", CSCD_INPUT_NONE}, {"decimal", CSCD_INPUT_DECIMAL}, {"numeric", CSCD_INPUT_NUMBER}, {"tel", CSCD_INPUT_TEL},
            {"search", CSCD_INPUT_SEARCH}, {"email", CSCD_INPUT_EMAIL}, {"url", CSCD_INPUT_URL}};
        const CscdSv mode = cscdElementAttrC(e, "inputmode");
        if (cscdSvIEqC(mode, "text")) t.kind = area ? CSCD_INPUT_MULTILINE : CSCD_INPUT_TEXT;
        for (size_t i = 0; i < CSCD_COUNT(kModes); ++i)
            if (cscdSvIEqC(mode, kModes[i].name)) t.kind = kModes[i].kind;
    }
    if (t.kind == CSCD_INPUT_SEARCH) t.enter = CSCD_ENTER_SEARCH;
    static const struct { const char* name; CscdEnterKey key; } kEnter[] = {
        {"enter", CSCD_ENTER_ENTER}, {"done", CSCD_ENTER_DONE}, {"go", CSCD_ENTER_GO}, {"next", CSCD_ENTER_NEXT},
        {"previous", CSCD_ENTER_PREVIOUS}, {"search", CSCD_ENTER_SEARCH}, {"send", CSCD_ENTER_SEND}};
    const CscdSv hint = cscdElementAttrC(e, "enterkeyhint");
    for (size_t i = 0; i < CSCD_COUNT(kEnter); ++i)
        if (cscdSvIEqC(hint, kEnter[i].name)) t.enter = kEnter[i].key;
    /* words that are typed: corrected and capitalized; addresses, numbers and passwords: as typed */
    const bool prose = t.kind == CSCD_INPUT_TEXT || t.kind == CSCD_INPUT_MULTILINE || t.kind == CSCD_INPUT_SEARCH;
    t.autocorrect = prose;
    if (cscdSvIEqC(cscdElementAttrC(e, "spellcheck"), "false") || cscdSvIEqC(cscdElementAttrC(e, "autocorrect"), "off")) t.autocorrect = false;
    else if (cscdSvIEqC(cscdElementAttrC(e, "autocorrect"), "on") && t.kind != CSCD_INPUT_PASSWORD) t.autocorrect = true;
    t.capitalize = prose && t.kind != CSCD_INPUT_SEARCH ? CSCD_CAPITALIZE_SENTENCES : CSCD_CAPITALIZE_NONE;
    const CscdSv cap = cscdElementAttrC(e, "autocapitalize");
    if (cscdSvIEqC(cap, "off") || cscdSvIEqC(cap, "none")) t.capitalize = CSCD_CAPITALIZE_NONE;
    else if (cscdSvIEqC(cap, "on") || cscdSvIEqC(cap, "sentences")) t.capitalize = CSCD_CAPITALIZE_SENTENCES;
    else if (cscdSvIEqC(cap, "words")) t.capitalize = CSCD_CAPITALIZE_WORDS;
    else if (cscdSvIEqC(cap, "characters")) t.capitalize = CSCD_CAPITALIZE_CHARACTERS;
    return t;
}

void cscdViewSetComposition(CscdView* v, CscdSv utf8) {
    if (cscdSvEq(cscdStrSv(&v->composition), utf8)) return;
    cscdStrSet(&v->composition, utf8);
    v->caretTime = viewNow(v);
    cscdViewInvalidate(v);
}

/* ---- undo in text fields ------------------------------------------------------------------------- */

static void pushState(UndoStateVec* s, const CscdElement* e) {
    UndoState st = {cscdStrCopy(&e->editValue), e->caret, e->selAnchor};
    cscdVecPush(s, st);
}

void viewPushUndo(CscdView* v, CscdElement* e, int kind) {
    bool inserted = false;
    CscdMapVal* slot = cscdU64MapPut(&v->undo, CSCD_PKEY(e), &inserted);
    if (inserted) slot->p = CSCD_NEW(UndoStack);
    UndoStack* u = (UndoStack*)slot->p;
    double t = viewNow(v);
    bool coalesce = kind != 3 && kind == u->lastKind && t - u->lastT < 1.0 && u->undo.len;
    u->lastT = t;
    u->lastKind = kind;
    for (size_t i = 0; i < u->redo.len; ++i) cscdStrFree(&u->redo.items[i].value);
    u->redo.len = 0;
    if (coalesce) return;
    pushState(&u->undo, e);
    if (u->undo.len > 300) {
        cscdStrFree(&u->undo.items[0].value);
        cscdVecRemove(&u->undo, 0);
    }
}

bool viewUndoEdit(CscdView* v, CscdElement* e, bool redo) {
    CscdMapVal* slot = cscdU64MapGet(&v->undo, CSCD_PKEY(e));
    if (!slot) return false;
    UndoStack* u = (UndoStack*)slot->p;
    UndoStateVec* from = redo ? &u->redo : &u->undo;
    UndoStateVec* to = redo ? &u->undo : &u->redo;
    if (!from->len) return false;
    pushState(to, e);
    UndoState s = cscdVecPop(from);
    cscdStrFree(&e->editValue);
    e->editValue = s.value;
    e->caret = cscdMinz(s.caret, s.value.len);
    e->selAnchor = cscdMinz(s.anchor, s.value.len);
    e->valueDirty = true;
    u->lastKind = 0;
    cscdNodeMarkDirty(&e->node);
    v->caretTime = viewNow(v);
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &e);
    viewSendSimple(v, "input", e);
    if (e) viewEnsureCaretVisible(v, e);
    viewUnwatch(v, mark);
    cscdViewInvalidate(v);
    return true;
}

/* ---- menus ----------------------------------------------------------------------------------------- */

void cscdViewShowMenu(CscdView* v, CscdMenu items, float x, float y, float minWidth) {
    cscdViewCloseMenus(v);
    if (!items.len) {
        cscdMenuFree(&items);
        return;
    }
    viewOpenMenu(v, items, cscdRect(x, y, 0, 0), false, minWidth, -1, NULL);
}

void cscdViewShowMenuAt(CscdView* v, CscdElement* anchor, CscdMenu items) {
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &anchor); /* (it may be in the menu that closes) */
    cscdViewCloseMenus(v);
    viewUnwatch(v, mark);
    if (!items.len || !anchor) {
        cscdMenuFree(&items);
        return;
    }
    viewLayoutNow(v);
    viewOpenMenu(v, items, cscdViewElementRect(v, anchor), false, 0, -1, NULL);
}

void cscdViewBindMenuBar(CscdView* v, CscdElement* entry, CscdMenuBarFn items, void* user, CscdFreeFn freeUser) {
    for (size_t i = 0; i < v->menuBar.len; ++i) {
        MenuBarEntry* b = &v->menuBar.items[i];
        if (b->entry != entry) continue;
        if (b->freeUser) b->freeUser(b->user);
        b->fn = items;
        b->user = user;
        b->freeUser = freeUser;
        return;
    }
    MenuBarEntry b = {entry, items, user, freeUser};
    cscdVecPush(&v->menuBar, b);
}

void viewOpenMenuBar(CscdView* v, CscdElement* entry) {
    cscdViewCloseMenus(v);
    for (size_t i = 0; i < v->menuBar.len; ++i) {
        MenuBarEntry* b = &v->menuBar.items[i];
        if (b->entry != entry) continue;
        CscdMenu items = {0};
        b->fn(b->user, &items);
        if (!items.len) {
            cscdMenuFree(&items);
            return;
        }
        viewLayoutNow(v);
        viewOpenMenu(v, items, cscdViewElementRect(v, entry), false, 0, -1, entry);
        v->menuBarOpen = entry;
        cscdElementAddClass(entry, CSCD_SV("cscd-menu-open"));
        return;
    }
}

static CscdElement* appendSpan(CscdElement* row, const char* cls) {
    CscdElement* s = cscdElementAppendElement(row, CSCD_SV("span"));
    cscdElementSetAttribute(s, CSCD_SV("class"), cscdSv(cls));
    return s;
}

/* owned: the menu's items are its own (else they belong to the parent item: a submenu) */
static void openMenuEx(CscdView* v, CscdMenu items, bool owned, CscdRect anchor, bool beside, float minWidth, int parentRow, CscdElement* owner) {
    CscdElement* host = overlayParent(v->doc);
    if (!host) {
        if (owned) cscdMenuFree(&items);
        return;
    }
    cscdViewHideTooltip(v);
    OpenMenu* m = CSCD_NEW(OpenMenu);
    m->items = items;
    m->highlighted = -1;
    m->parentRow = parentRow;
    m->owner = owner;
    m->ownsItems = owned;
    CscdElement* box = cscdElementAppendElement(host, CSCD_SV("div"));
    cscdElementSetAttribute(box, CSCD_SV("class"), CSCD_SV("cscd-menu"));
    cscdElementSetStyle(box, CSCD_SV("left"), CSCD_SV("0px"));
    cscdElementSetStyle(box, CSCD_SV("top"), CSCD_SV("0px"));
    if (minWidth > 0) setPx(box, "min-width", minWidth);
    for (size_t i = 0; i < m->items.len; ++i) {
        const CscdMenuItem* it = &m->items.items[i];
        CscdElement* row = cscdElementAppendElement(box, CSCD_SV("div"));
        cscdVecPush(&m->rows, row);
        if (it->separator) {
            cscdElementSetAttribute(row, CSCD_SV("class"), CSCD_SV("cscd-menu-separator"));
            continue;
        }
        CscdStr cls = cscdStrDupC("cscd-menu-item");
        if (!it->enabled) cscdStrAppendC(&cls, " disabled");
        if (it->checkable) cscdStrAppendC(&cls, it->checked ? " checkable checked" : " checkable");
        if (hasSubmenu(it)) cscdStrAppendC(&cls, " has-submenu");
        cscdElementSetAttribute(row, CSCD_SV("class"), cscdStrSv(&cls));
        cscdStrFree(&cls);
        if (it->id.len) cscdElementSetAttribute(row, CSCD_SV("data-id"), cscdStrSv(&it->id));
        if (it->tooltip.len) cscdElementSetAttribute(row, CSCD_SV("title"), cscdStrSv(&it->tooltip));
        CscdElement* check = appendSpan(row, "cscd-menu-check");
        if (it->checkable && it->checked) {
            CscdPainter p = {paintCheck, check, NULL};
            cscdElementSetPainter(check, p);
        }
        CscdElement* icon = appendSpan(row, "cscd-menu-icon");
        if (it->icon.len) cscdElementSetAttribute(icon, CSCD_SV("data-icon"), cscdStrSv(&it->icon));
        CscdElement* label = appendSpan(row, "cscd-menu-label");
        cscdElementSetText(label, cscdStrSv(&it->label));
        CscdElement* keys = appendSpan(row, "cscd-menu-shortcut");
        cscdElementSetText(keys, cscdStrSv(&it->shortcut));
        CscdElement* arrow = appendSpan(row, "cscd-menu-arrow");
        if (hasSubmenu(it)) {
            CscdPainter p = {paintArrow, arrow, NULL};
            cscdElementSetPainter(arrow, p);
        }
        if (v->onMenuItem) v->onMenuItem(v->onMenuItemUser, row, it);
    }
    /* the check and icon columns only when a row uses them (as QMenu) */
    bool icons = false, checks = false;
    for (size_t i = 0; i < m->items.len; ++i) {
        icons = icons || m->items.items[i].icon.len;
        checks = checks || m->items.items[i].checkable;
    }
    if (!icons) cscdElementAddClass(box, CSCD_SV("no-icons"));
    if (!checks) cscdElementAddClass(box, CSCD_SV("no-checks"));
    m->box = box;
    cscdVecPush(&v->menus, m); /* (in the list before the layout: removals during it find it) */
    viewLayoutNow(v);
    if (!m->box) {
        cscdViewInvalidate(v);
        return;
    }
    CscdRect r = cscdViewElementRect(v, box);
    float w = r.w, h = r.h, nx, ny;
    if (beside) {
        nx = cscdRectRight(anchor) - 2;
        ny = anchor.y - (box->layout ? box->layout->padding[0] + box->layout->border[0] : 0);
        if (nx + w > v->vw) nx = cscdMaxf(0.f, anchor.x - w + 2);
        if (ny + h > v->vh) ny = cscdMaxf(0.f, v->vh - h);
    } else {
        nx = anchor.x;
        ny = cscdRectBottom(anchor);
        if (nx + w > v->vw) nx = cscdMaxf(0.f, v->vw - w);
        if (ny + h > v->vh) ny = anchor.y - h >= 0 ? anchor.y - h : cscdMaxf(0.f, v->vh - h);
    }
    if (h > v->vh) {
        setPx(box, "max-height", v->vh);
        cscdElementSetStyle(box, CSCD_SV("overflow-y"), CSCD_SV("auto"));
        ny = 0;
    }
    setPx(box, "left", nx);
    setPx(box, "top", ny);
    cscdViewInvalidate(v);
}

void viewOpenMenu(CscdView* v, CscdMenu items, CscdRect anchor, bool beside, float minWidth, int parentRow, CscdElement* owner) {
    openMenuEx(v, items, true, anchor, beside, minWidth, parentRow, owner);
}

static OpenMenu* menuAt(const CscdView* v, size_t level) { return (OpenMenu*)v->menus.items[level]; }

void viewCloseMenusFrom(CscdView* v, size_t level) {
    while (v->menus.len > level) {
        OpenMenu* m = (OpenMenu*)cscdVecPop(&v->menus);
        if (m->owner) cscdElementRemoveClass(m->owner, CSCD_SV("cscd-menu-open"));
        if (m->box && m->box->node.parent) {
            CscdElement* box = m->box;
            m->box = NULL;
            cscdElementRemove(box);
        }
        openMenuFree(m);
    }
    cscdViewInvalidate(v);
}

void cscdViewCloseMenus(CscdView* v) {
    viewCloseMenusFrom(v, 0);
    v->menuBarOpen = NULL;
}

void viewHighlightMenuRow(CscdView* v, size_t level, int row, bool openSub) {
    if (level >= v->menus.len) return;
    OpenMenu* m = menuAt(v, level);
    bool subOpen = level + 1 < v->menus.len && menuAt(v, level + 1)->parentRow == row;
    if (m->highlighted == row && (subOpen || !openSub)) return;
    if (m->highlighted >= 0 && m->highlighted < (int)m->rows.len) cscdElementRemoveClass((CscdElement*)m->rows.items[m->highlighted], CSCD_SV("highlighted"));
    m->highlighted = row;
    viewCloseMenusFrom(v, level + 1);
    if (row < 0 || row >= (int)m->rows.len) return;
    cscdElementAddClass((CscdElement*)m->rows.items[row], CSCD_SV("highlighted"));
    CscdMenuItem* it = &m->items.items[row];
    if (openSub && it->enabled && hasSubmenu(it)) {
        CscdMenu sub = {0};
        bool owned = true;
        if (it->fill.fn) it->fill.fn(it->fill.user, &sub);
        else {
            sub = it->submenu; /* (borrowed: the item keeps it) */
            owned = false;
        }
        if (!sub.len) {
            CscdMenuItem none = cscdMenuItem(CSCD_SV("(empty)"), NULL, NULL, CSCD_SV(""));
            none.enabled = false;
            memset(&sub, 0, sizeof sub);
            cscdMenuAdd(&sub, none);
            owned = true;
        }
        viewLayoutNow(v);
        openMenuEx(v, sub, owned, cscdViewElementRect(v, (CscdElement*)m->rows.items[row]), true, 0, row, NULL);
    }
    cscdViewInvalidate(v);
}

void viewActivateMenuRow(CscdView* v, size_t level, int row) {
    if (level >= v->menus.len || row < 0 || row >= (int)menuAt(v, level)->items.len) return;
    CscdMenuItem* it = &menuAt(v, level)->items.items[row];
    if (!it->enabled || it->separator) return;
    if (hasSubmenu(it)) {
        viewHighlightMenuRow(v, level, row, true);
        if (level + 1 < v->menus.len) {
            /* keyboard: into the submenu, on its first enabled row */
            OpenMenu* sub = menuAt(v, level + 1);
            for (size_t i = 0; i < sub->items.len; ++i)
                if (!sub->items.items[i].separator && sub->items.items[i].enabled) { viewHighlightMenuRow(v, level + 1, (int)i, false); break; }
        }
        return;
    }
    /* the action leaves the item (the menus go) for a timer that runs it once they are gone */
    CscdAction fn = it->action;
    memset(&it->action, 0, sizeof it->action);
    cscdViewCloseMenus(v);
    if (fn.fn) cscdViewSetTimeout(v, fn, 0);
    else cscdActionFree(&fn);
}

bool viewMenuMouse(CscdView* v, int kind, float x, float y, int button) {
    /* a menu bar entry under the pointer */
    CscdElement* hit = cscdViewElementAt(v, x, y);
    CscdElement* entry = NULL;
    for (size_t i = 0; i < v->menuBar.len; ++i)
        if (inside(hit, v->menuBar.items[i].entry)) entry = v->menuBar.items[i].entry;
    if (!v->menus.len) {
        if (kind == 1 && button == 0 && entry) {
            viewOpenMenuBar(v, entry);
            v->menuSwallowUp = true;
            return true;
        }
        if (kind == 2 && v->menuSwallowUp) {
            v->menuSwallowUp = false;
            return true;
        }
        return false;
    }
    for (size_t i = 0; i < v->menus.len; ++i)
        if (!menuAt(v, i)->box) {
            cscdViewCloseMenus(v);
            return kind != 0;
        }
    int level = -1, row = -1;
    for (int i = (int)v->menus.len - 1; i >= 0; --i)
        if (cscdRectContains(cscdViewElementRect(v, menuAt(v, (size_t)i)->box), x, y)) { level = i; break; }
    if (level >= 0) {
        OpenMenu* m = menuAt(v, (size_t)level);
        for (size_t j = 0; j < m->rows.len; ++j)
            if (!m->items.items[j].separator && inside(hit, (CscdElement*)m->rows.items[j])) { row = (int)j; break; }
    }
    v->cursor = CSCD_CURSOR_DEFAULT;
    if (kind == 0) {
        if (level >= 0) {
            viewSetHover(v, hit);
            viewTooltipMove(v, hit);
            if (row >= 0) viewHighlightMenuRow(v, (size_t)level, row, true);
        } else if (entry && v->menuBarOpen && entry != v->menuBarOpen) {
            viewOpenMenuBar(v, entry);
        } else {
            cscdViewHideTooltip(v);
            /* off the menus: the deepest menu loses its highlight unless it opened a submenu */
            OpenMenu* last = (OpenMenu*)cscdVecLast(&v->menus);
            if (last->highlighted >= 0 && !hasSubmenu(&last->items.items[last->highlighted])) viewHighlightMenuRow(v, v->menus.len - 1, -1, false);
        }
        return true;
    }
    if (kind == 1) {
        cscdViewHideTooltip(v);
        if (level >= 0) return true;
        v->menuSwallowUp = true;
        if (entry && entry != v->menuBarOpen) viewOpenMenuBar(v, entry);
        else cscdViewCloseMenus(v);
        return true;
    }
    /* release */
    v->menuSwallowUp = false;
    if (level >= 0 && row >= 0) {
        if (button == 0 || button == 2) viewActivateMenuRow(v, (size_t)level, row);
    }
    return true;
}

static void menuStep(CscdView* v, size_t level, int dir) {
    OpenMenu* m = menuAt(v, level);
    int n = (int)m->items.len;
    if (!n) return;
    int i = m->highlighted;
    for (int tries = 0; tries < n; ++tries) {
        i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
        if (!m->items.items[i].separator && m->items.items[i].enabled) break;
    }
    viewHighlightMenuRow(v, level, i, false);
    CscdElement* rowEl = (CscdElement*)m->rows.items[i];
    if (m->box && rowEl->layout) {
        /* keep the row in view in a scrolling menu */
        CscdRect rr = cscdBoxBorderBox(rowEl->layout), bb = m->box->layout ? cscdBoxPaddingBox(m->box->layout) : rr;
        if (rr.y - m->box->scrollTop < bb.y) m->box->scrollTop = rr.y - bb.y;
        else if (cscdRectBottom(rr) - m->box->scrollTop > cscdRectBottom(bb)) m->box->scrollTop = cscdRectBottom(rr) - cscdRectBottom(bb);
    }
}

/* the menu bar entry next to the open one (dir: +1 or -1), opened on its first row */
static void menuBarStep(CscdView* v, int dir) {
    const size_t n = v->menuBar.len;
    for (size_t i = 0; i < n; ++i)
        if (v->menuBar.items[i].entry == v->menuBarOpen) {
            viewOpenMenuBar(v, v->menuBar.items[(i + n + (size_t)dir) % n].entry);
            if (v->menus.len) {
                menuAt(v, 0)->highlighted = -1;
                viewMenuKey(v, CSCD_KEY_DOWN, CSCD_SV(""));
            }
            break;
        }
}

bool viewMenuKey(CscdView* v, CscdKey k, CscdSv keyName) {
    if (!v->menus.len) return false;
    size_t level = v->menus.len - 1;
    OpenMenu* m = menuAt(v, level);
    switch (k) {
    case CSCD_KEY_DOWN: menuStep(v, level, 1); return true;
    case CSCD_KEY_UP: menuStep(v, level, -1); return true;
    case CSCD_KEY_HOME: m->highlighted = -1; menuStep(v, level, 1); return true;
    case CSCD_KEY_END: m->highlighted = -1; menuStep(v, level, -1); return true;
    case CSCD_KEY_ESCAPE:
        if (level > 0) viewCloseMenusFrom(v, level);
        else cscdViewCloseMenus(v);
        return true;
    case CSCD_KEY_RIGHT:
        if (m->highlighted >= 0 && hasSubmenu(&m->items.items[m->highlighted])) { viewActivateMenuRow(v, level, m->highlighted); return true; }
        if (v->menuBarOpen) menuBarStep(v, 1);
        return true;
    case CSCD_KEY_LEFT:
        if (level > 0) { viewCloseMenusFrom(v, level); return true; }
        if (v->menuBarOpen) menuBarStep(v, -1);
        return true;
    case CSCD_KEY_ENTER: case CSCD_KEY_SPACE:
        if (m->highlighted >= 0) viewActivateMenuRow(v, level, m->highlighted);
        return true;
    default: break;
    }
    /* a letter: the next row starting with it */
    if (keyName.len == 1 && isalnum((unsigned char)keyName.ptr[0])) {
        int n = (int)m->items.len;
        for (int t = 1; t <= n; ++t) {
            int i = ((m->highlighted < 0 ? -1 : m->highlighted) + t + n) % n;
            const CscdStr* l = &m->items.items[i].label;
            if (!m->items.items[i].separator && m->items.items[i].enabled && l->len &&
                tolower((unsigned char)l->ptr[0]) == tolower((unsigned char)keyName.ptr[0])) {
                viewHighlightMenuRow(v, level, i, false);
                break;
            }
        }
    }
    return true;
}

/* ---- dialogs --------------------------------------------------------------------------------------- */

void viewPlaceModal(CscdView* v, CscdElement* dialog) {
    /* centered in the viewport (and again when the viewport changes) */
    cscdElementSetStyle(dialog, CSCD_SV("left"), CSCD_SV("0px"));
    cscdElementSetStyle(dialog, CSCD_SV("top"), CSCD_SV("0px"));
    viewLayoutNow(v);
    CscdRect r = cscdViewElementRect(v, dialog);
    setPx(dialog, "left", cscdMaxf(0.f, roundf((v->vw - r.w) / 2)));
    setPx(dialog, "top", cscdMaxf(0.f, roundf((v->vh - r.h) / 2)));
    cscdViewInvalidate(v);
}

static CscdElement* firstFocusable(CscdElement* e) {
    for (size_t i = 0; i < e->node.children.len; ++i) {
        CscdElement* c = cscdNodeAsElement(cscdNodeChild(&e->node, i));
        if (!c) continue;
        if (isFocusable(c) && c->layout) return c;
        CscdElement* d = firstFocusable(c);
        if (d) return d;
    }
    return NULL;
}

void cscdViewShowModal(CscdView* v, CscdElement* dialog) {
    if (!dialog || viewContainsPtr(&v->modals, dialog)) return;
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &dialog); /* (it may be in the menu that closes, or a drag's listeners may remove it) */
    cscdViewCloseMenus(v);
    cscdViewHideTooltip(v);
    if (v->dragActive) cscdViewCancelDrag(v);
    viewUnwatch(v, mark);
    if (!dialog) return;
    cscdElementSetAttribute(dialog, CSCD_SV("open"), CSCD_SV(""));
    cscdElementAddClass(dialog, CSCD_SV("cscd-modal"));
    cscdVecPush(&v->modals, dialog);
    cscdVecPush(&v->modalReturnFocus, v->doc->focused);
    viewPlaceModal(v, dialog);
    /* the focus goes in: [autofocus], else the first field or button */
    CscdElement* first = cscdElementQuerySelector(dialog, CSCD_SV("[autofocus]"));
    if (!first) first = firstFocusable(dialog);
    viewWatch(v, &first);
    cscdViewFocus(v, first);
    if (first && isTextControl(first)) {
        first->selAnchor = 0;
        first->caret = first->editValue.len;
    }
    viewUnwatch(v, mark);
}

void cscdViewShowDialog(CscdView* v, CscdElement* dialog) {
    if (!dialog) return;
    cscdElementSetAttribute(dialog, CSCD_SV("open"), CSCD_SV(""));
    cscdViewInvalidate(v);
}

void cscdViewCloseDialog(CscdView* v, CscdElement* dialog, CscdSv returnValue) {
    if (!dialog || !cscdElementHasAttributeC(dialog, "open")) return;
    cscdElementRemoveAttribute(dialog, CSCD_SV("open"));
    cscdElementRemoveClass(dialog, CSCD_SV("cscd-modal"));
    cscdElementSetAttribute(dialog, CSCD_SV("returnvalue"), returnValue);
    CscdElement* back = NULL;
    for (size_t i = 0; i < v->modals.len; ++i)
        if (v->modals.items[i] == dialog) {
            back = (CscdElement*)v->modalReturnFocus.items[i];
            cscdVecRemove(&v->modals, i);
            cscdVecRemove(&v->modalReturnFocus, i);
            break;
        }
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &dialog);
    if (v->doc->focused && inside(v->doc->focused, dialog)) cscdViewFocus(v, back);
    if (dialog) viewSendSimple(v, "close", dialog);
    viewUnwatch(v, mark);
    cscdViewInvalidate(v);
}

bool viewBlockedByModal(const CscdView* v, CscdElement* target) {
    if (!v->modals.len) return false;
    const CscdElement* top = (const CscdElement*)cscdVecLast(&v->modals);
    for (CscdElement* e = target; e; e = parentEl(e))
        if (e == top || cscdElementHasClass(e, CSCD_SV("cscd-menu")) || cscdElementHasClass(e, CSCD_SV("cscd-tooltip")) ||
            cscdElementHasClass(e, CSCD_SV("cscd-popover-open")))
            return false;
    return true;
}

/* ---- popovers -------------------------------------------------------------------------------------- */

static void toggleEvent(CscdView* v, CscdElement* p, const char* state) {
    CscdEvent ev;
    cscdEventInit(&ev, CSCD_SV("toggle"));
    cscdStrSetC(&ev.key, state);
    viewDispatch(v, &ev, p);
    cscdEventFini(&ev);
}

void cscdViewShowPopover(CscdView* v, CscdElement* p, CscdElement* anchor) {
    if (!p) return;
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &p); /* (either may be in the menu that closes) */
    viewWatch(v, &anchor);
    cscdViewHideTooltip(v);
    cscdViewCloseMenus(v);
    viewUnwatch(v, mark);
    if (!p) return;
    if (!cscdViewPopoverOpen(v, p)) cscdVecPush(&v->popovers, p);
    cscdElementAddClass(p, CSCD_SV("cscd-popover-open"));
    cscdElementSetStyle(p, CSCD_SV("left"), CSCD_SV("0px"));
    cscdElementSetStyle(p, CSCD_SV("top"), CSCD_SV("0px"));
    viewLayoutNow(v);
    CscdRect r = cscdViewElementRect(v, p);
    CscdRect a = anchor ? cscdViewElementRect(v, anchor) : cscdRect(v->mouseX, v->mouseY, 0, 0);
    const float gap = anchor ? 4.f : 0.f;
    float x = a.x, y = cscdRectBottom(a) + gap;
    if (x + r.w > v->vw - 4) x = cscdMaxf(4.f, v->vw - 4 - r.w);
    if (y + r.h > v->vh - 4) y = a.y - r.h - gap >= 4 ? a.y - r.h - gap : cscdMaxf(4.f, v->vh - 4 - r.h);
    setPx(p, "left", x);
    setPx(p, "top", y);
    toggleEvent(v, p, "open");
    cscdViewInvalidate(v);
}

void cscdViewHidePopover(CscdView* v, CscdElement* p) {
    size_t at = CSCD_NPOS;
    for (size_t i = 0; i < v->popovers.len; ++i) if (v->popovers.items[i] == p) { at = i; break; }
    if (at == CSCD_NPOS) return;
    cscdVecRemove(&v->popovers, at);
    cscdElementRemoveClass(p, CSCD_SV("cscd-popover-open"));
    toggleEvent(v, p, "closed");
    cscdViewInvalidate(v);
}

bool cscdViewPopoverOpen(const CscdView* v, const CscdElement* p) { return viewContainsPtr(&v->popovers, p); }

void viewLightDismiss(CscdView* v, CscdElement* hit) {
    /* a press outside the popovers hides them, from the top down to the one it is in */
    while (v->popovers.len) {
        CscdElement* top = (CscdElement*)cscdVecLast(&v->popovers);
        if (inside(hit, top)) return;
        bool inMenu = false;
        for (CscdElement* e = hit; e; e = parentEl(e)) inMenu = inMenu || cscdElementHasClass(e, CSCD_SV("cscd-menu"));
        if (inMenu) return;
        cscdViewHidePopover(v, top);
    }
}

/* ---- tooltips -------------------------------------------------------------------------------------- */

void cscdViewHideTooltip(CscdView* v) {
    if (v->tipTimer) {
        cscdViewClearTimer(v, v->tipTimer);
        v->tipTimer = 0;
    }
    if (v->tip) {
        CscdElement* t = v->tip;
        v->tip = NULL;
        cscdElementRemove(t);
        cscdViewInvalidate(v);
    }
}

typedef struct TipShow { CscdView* v; CscdElement* t; } TipShow;

static void showTooltip(void* user) {
    TipShow* s = (TipShow*)user;
    CscdView* v = s->v;
    CscdElement* t = s->t;
    v->tipTimer = 0;
    if (v->tipFor != t || v->dragActive || v->buttons || v->mouseX < 0) return;
    CscdElement* host = overlayParent(v->doc);
    if (!host) return;
    CscdElement* tip = cscdElementAppendElement(host, CSCD_SV("div"));
    cscdElementSetAttribute(tip, CSCD_SV("class"), CSCD_SV("cscd-tooltip"));
    cscdElementSetText(tip, cscdElementAttrC(t, "title"));
    cscdElementSetStyle(tip, CSCD_SV("left"), CSCD_SV("0px"));
    cscdElementSetStyle(tip, CSCD_SV("top"), CSCD_SV("0px"));
    v->tip = tip;
    viewLayoutNow(v);
    if (!v->tip) return;
    CscdRect r = cscdViewElementRect(v, tip);
    float x = v->mouseX + 2, y = v->mouseY + 22;
    if (x + r.w > v->vw - 2) x = cscdMaxf(2.f, v->vw - 2 - r.w);
    if (y + r.h > v->vh - 2) y = cscdMaxf(2.f, v->mouseY - r.h - 6);
    setPx(tip, "left", x);
    setPx(tip, "top", y);
    cscdViewInvalidate(v);
}

void viewTooltipMove(CscdView* v, CscdElement* hit) {
    CscdElement* t = NULL;
    for (CscdElement* a = hit; a; a = parentEl(a))
        if (cscdElementHasAttributeC(a, "title")) { t = a; break; }
    if (t && !cscdElementAttrC(t, "title").len) t = NULL;
    if (t == v->tipFor) return;
    cscdViewHideTooltip(v);
    v->tipFor = t;
    if (!t || v->buttons || v->dragActive || !viewOn(v, CSCD_FEATURE_TOOLTIPS)) return;
    TipShow* s = CSCD_NEW(TipShow);
    s->v = v;
    s->t = t;
    CscdAction a = {showTooltip, s, cscdFree};
    v->tipTimer = cscdViewSetTimeout(v, a, v->tooltipDelayMs);
}

/* ---- drag and drop --------------------------------------------------------------------------------- */

/* sends a drag event; returns whether it was cancelled (preventDefault) */
static bool fireDrag(CscdView* v, const char* type, CscdElement* e, float x, float y, CscdDataTransfer* dt) {
    if (!e) return false;
    CscdEvent ev;
    cscdEventInit(&ev, cscdSv(type));
    ev.x = x;
    ev.y = y;
    ev.buttons = v->dragExternal ? 0 : 1;
    ev.dataTransfer = (CscdDataTransfer*)cscdRetain(dt);
    viewDispatch(v, &ev, e);
    const bool prevented = ev.defaultPrevented;
    cscdEventFini(&ev);
    return prevented;
}

void viewBeginDrag(CscdView* v, float x, float y) {
    CscdElement* src = v->dragSource;
    CscdDataTransfer* dt = cscdDataTransferNew();
    CscdEvent ev;
    cscdEventInit(&ev, CSCD_SV("dragstart"));
    ev.x = v->dragStartX;
    ev.y = v->dragStartY;
    ev.buttons = 1;
    ev.dataTransfer = (CscdDataTransfer*)cscdRetain(dt);
    viewDispatch(v, &ev, src);
    const bool prevented = ev.defaultPrevented;
    cscdEventFini(&ev);
    if (prevented || !v->dragSource) {
        v->dragSource = NULL;
        cscdRelease(dt);
        return;
    }
    src = v->dragSource;
    cscdViewHideTooltip(v);
    cscdRelease(v->dragData);
    v->dragData = dt;
    v->dragActive = true;
    v->dragExternal = false;
    v->dragOver = NULL;
    v->dropAllowed = false;
    v->selecting = false;
    viewSetActive(v, NULL);
    v->pressed = NULL;
    cscdRelease(v->dragGhost);
    v->dragGhost = NULL;
    if (dt->dragImage) {
        v->dragGhost = (CscdImage*)cscdRetain(dt->dragImage);
        v->ghostX = dt->imageX;
        v->ghostY = dt->imageY;
        v->ghostScale = 1;
    } else {
        /* a picture of the element as it is drawn now */
        CscdRect r = cscdViewElementRect(v, src);
        const float dpr = v->dpr;
        int W = cscdIntOf(ceilf(v->vw * dpr)), H = cscdIntOf(ceilf(v->vh * dpr));
        CscdImage full;
        cscdImageInit(&full, W, H);
        cscdViewPaint(v, &full, false);
        v->dirty = v->fullPaint = true;
        int x0 = cscdClampi(cscdIntOf(floorf(r.x * dpr)), 0, W), y0 = cscdClampi(cscdIntOf(floorf(r.y * dpr)), 0, H);
        int x1 = cscdClampi(cscdIntOf(ceilf(cscdRectRight(r) * dpr)), 0, W), y1 = cscdClampi(cscdIntOf(ceilf(cscdRectBottom(r) * dpr)), 0, H);
        if (x1 > x0 && y1 > y0) {
            CscdImage* g = cscdImageNew(x1 - x0, y1 - y0);
            for (int yy = y0; yy < y1; ++yy) memcpy(cscdImageRow(g, yy - y0), cscdImageRow(&full, yy) + x0, (size_t)(x1 - x0) * sizeof(uint32_t));
            v->dragGhost = g;
        }
        cscdImageFini(&full);
        v->ghostX = v->dragStartX - x0 / dpr;
        v->ghostY = v->dragStartY - y0 / dpr;
        v->ghostScale = 1 / dpr;
    }
    viewDragMoveTo(v, x, y);
}

void viewDragMoveTo(CscdView* v, float x, float y) {
    v->mouseX = x;
    v->mouseY = y;
    CscdDataTransfer* dt = (CscdDataTransfer*)cscdRetain(v->dragData);
    if (!dt) return;
    CscdElement* t = cscdViewElementAt(v, x, y);
    if (v->dragSource && !v->dragExternal) fireDrag(v, "drag", v->dragSource, x, y, dt);
    if (t != v->dragOver) {
        CscdElement* old = v->dragOver;
        v->dragOver = t;
        const size_t mark = viewWatchMark(v);
        viewWatch(v, &t);
        fireDrag(v, "dragleave", old, x, y, dt);
        fireDrag(v, "dragenter", t, x, y, dt);
        viewUnwatch(v, mark);
    }
    cscdStrSetC(&dt->dropEffect, v->dragExternal || cscdStrEqC(&dt->effectAllowed, "copy") ? "copy" : "move");
    bool ok = fireDrag(v, "dragover", v->dragOver, x, y, dt);
    if (!ok && editable(v->dragOver) && cscdDataTransferHasType(dt, CSCD_SV("text/plain"))) ok = true; /* text fields take text */
    v->dropAllowed = ok && !cscdStrEqC(&dt->dropEffect, "none");
    v->cursor = !v->dropAllowed ? CSCD_CURSOR_NOT_ALLOWED : cscdStrEqC(&dt->dropEffect, "copy") ? CSCD_CURSOR_COPY : CSCD_CURSOR_DEFAULT;
    cscdRelease(dt);
    cscdViewInvalidate(v);
}

void viewEndDrag(CscdView* v, bool drop) {
    CscdDataTransfer* dt = (CscdDataTransfer*)cscdRetain(v->dragData);
    CscdElement* target = v->dragOver;
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &target);
    bool dropped = false;
    if (dt && drop && v->dropAllowed && target) {
        const bool prevented = fireDrag(v, "drop", target, v->mouseX, v->mouseY, dt);
        dropped = true;
        if (!prevented && target && v->dragOver == target && editable(target) && cscdDataTransferHasType(dt, CSCD_SV("text/plain"))) {
            cscdViewFocus(v, target);
            if (target) viewEditInput(v, target, cscdDataTransferGetData(dt, CSCD_SV("text/plain")), 0, false);
        }
    } else if (dt && target) {
        fireDrag(v, "dragleave", target, v->mouseX, v->mouseY, dt);
    }
    if (dt && v->dragSource && !v->dragExternal) {
        if (!dropped) cscdStrSetC(&dt->dropEffect, "none");
        fireDrag(v, "dragend", v->dragSource, v->mouseX, v->mouseY, dt);
    }
    viewUnwatch(v, mark);
    cscdRelease(dt);
    v->dragActive = v->dragExternal = v->dropAllowed = false;
    v->dragSource = v->dragOver = NULL;
    cscdRelease(v->dragData);
    v->dragData = NULL;
    cscdRelease(v->dragGhost);
    v->dragGhost = NULL;
    v->cursor = CSCD_CURSOR_DEFAULT;
    cscdViewInvalidate(v);
}

void cscdViewCancelDrag(CscdView* v) {
    if (v->dragActive) viewEndDrag(v, false);
    v->dragSource = NULL;
}

void cscdViewExternalDragEnter(CscdView* v, CscdDataTransfer* data, float x, float y) {
    if (v->dragActive) viewEndDrag(v, false);
    cscdViewHideTooltip(v);
    cscdViewCloseMenus(v);
    cscdRelease(v->dragData);
    v->dragData = (CscdDataTransfer*)cscdRetain(data);
    v->dragActive = v->dragExternal = true;
    v->dragSource = v->dragOver = NULL;
    cscdRelease(v->dragGhost);
    v->dragGhost = NULL;
    viewDragMoveTo(v, x, y);
}

bool cscdViewExternalDragOver(CscdView* v, float x, float y) {
    if (!v->dragActive || !v->dragExternal) return false;
    viewDragMoveTo(v, x, y);
    return v->dropAllowed;
}

void cscdViewExternalDragLeave(CscdView* v) {
    if (v->dragActive && v->dragExternal) viewEndDrag(v, false);
}

bool cscdViewExternalDrop(CscdView* v, float x, float y) {
    if (!v->dragActive || !v->dragExternal) return false;
    viewDragMoveTo(v, x, y);
    bool ok = v->dropAllowed;
    viewEndDrag(v, true);
    return ok;
}

/* ---- the clipboard, from the program ---------------------------------------------------------------- */

bool cscdViewCopyText(CscdView* v, CscdSv text) {
    if (!v->setClipboard) return false;
    v->setClipboard(v->clipboardUser, text);
    return true;
}

bool cscdViewCopyImage(CscdView* v, const CscdImage* image) {
    if (!v->setClipboardImage || !image || image->width <= 0 || image->height <= 0) return false;
    v->setClipboardImage(v->clipboardUser, image);
    return true;
}

void cscdViewClipboardText(CscdView* v, CscdStr* out) {
    if (v->getClipboard) v->getClipboard(v->clipboardUser, out);
}

void cscdViewClipboardFiles(CscdView* v, CscdStrVec* out) {
    if (v->getClipboardFiles) v->getClipboardFiles(v->clipboardUser, out);
}

/* ---- splitters ------------------------------------------------------------------------------------- */

static bool shown(const CscdElement* e) { return e && e->layout && e->style && e->style->display != CSCD_DISPLAY_NONE; }

bool viewSplitterDown(CscdView* v, CscdElement* hit, float x, float y) {
    CscdElement* s = NULL;
    for (CscdElement* a = hit; a; a = parentEl(a))
        if (cscdElementHasAttributeC(a, "splitter")) { s = a; break; }
    if (!s) return false;
    CscdElement* parent = parentEl(s);
    if (!parent || !parent->style) return false;
    CscdElement* prev = cscdNodePreviousElementSibling(&s->node);
    while (prev && !shown(prev)) prev = cscdNodePreviousElementSibling(&prev->node);
    CscdElement* next = cscdNodeNextElementSibling(&s->node);
    while (next && !shown(next)) next = cscdNodeNextElementSibling(&next->node);
    if (!prev || !next) return false;
    v->splitRow = !cscdStyleIsColumn(parent->style);
    /* the panels that share the room keep proportions as flex-grow in pixels */
    for (size_t i = 0; i < parent->node.children.len; ++i) {
        CscdElement* c = cscdNodeAsElement(cscdNodeChild(&parent->node, i));
        if (!shown(c) || cscdElementHasAttributeC(c, "splitter") || c->style->flexGrow <= 0) continue;
        CscdRect r = cscdBoxBorderBox(c->layout);
        setNum(c, "flex-grow", cscdMaxf(1.f, v->splitRow ? r.w : r.h));
        cscdElementSetStyle(c, CSCD_SV("flex-basis"), CSCD_SV("0px"));
    }
    CscdRect pr = cscdBoxBorderBox(prev->layout), nr = cscdBoxBorderBox(next->layout);
    v->splitter = s;
    v->splitPrev = prev;
    v->splitNext = next;
    v->splitPrevSize = v->splitRow ? pr.w : pr.h;
    v->splitNextSize = v->splitRow ? nr.w : nr.h;
    v->splitStart = v->splitRow ? x : y;
    v->cursor = v->splitRow ? CSCD_CURSOR_COL_RESIZE : CSCD_CURSOR_ROW_RESIZE;
    return true;
}

static float splitMin(const CscdView* v, const CscdElement* e) {
    const CscdLength l = v->splitRow ? e->style->minWidth : e->style->minHeight;
    return cscdMaxf(24.f, cscdLenIsFixed(l) ? l.px : 0.f);
}

static void splitApply(CscdElement* e, float size) {
    if (e->style->flexGrow > 0) setNum(e, "flex-grow", cscdMaxf(1.f, size));
    else setPx(e, "flex-basis", size);
}

void viewSplitterMove(CscdView* v, float x, float y) {
    if (!v->splitter || !v->splitPrev || !v->splitNext) { v->splitter = NULL; return; }
    float d = (v->splitRow ? x : y) - v->splitStart;
    float total = v->splitPrevSize + v->splitNextSize;
    float np = cscdClampf(v->splitPrevSize + d, splitMin(v, v->splitPrev), cscdMaxf(splitMin(v, v->splitPrev), total - splitMin(v, v->splitNext)));
    float nn = total - np;
    splitApply(v->splitPrev, np);
    splitApply(v->splitNext, nn);
    v->cursor = v->splitRow ? CSCD_CURSOR_COL_RESIZE : CSCD_CURSOR_ROW_RESIZE;
    viewSendSimple(v, "input", v->splitter);
    cscdViewInvalidate(v);
}

void viewSplitterUp(CscdView* v) {
    CscdElement* s = v->splitter;
    v->splitter = v->splitPrev = v->splitNext = NULL;
    if (!s) return;
    viewSendSimple(v, "change", s);
}

/* ---- removed nodes --------------------------------------------------------------------------------- */

/* ---- virtual lists --------------------------------------------------------------------------------- */

enum { LIST_OVERSCAN = 4 }; /* rows made beyond each edge, so that a wheel step shows rows already made */

static VirtualList* findList(const CscdView* v, const CscdElement* box) {
    for (size_t i = 0; i < v->lists.len; ++i) if (v->lists.items[i]->box == box) return v->lists.items[i];
    return NULL;
}

static void freeList(VirtualList* l) {
    cscdVecFree(&l->rows);
    cscdNodeDestroy(&l->proto->node);
    if (l->freeUser) l->freeUser(l->user);
    cscdFree(l);
}

static bool updateList(CscdView* v, VirtualList* l, bool all) {
    CscdElement* box = l->box;
    const float h = box->layout ? cscdBoxPaddingBox(box->layout).h : v->vh;
    const float rh = l->rowHeight;
    /* (fewer items than before: back in range) */
    const float most = cscdMaxf(0.f, (float)l->count * rh - h);
    if (box->scrollTop > most) box->scrollTop = most;
    size_t first = (size_t)cscdMaxf(0.f, floorf(box->scrollTop / rh) - LIST_OVERSCAN);
    size_t end = (size_t)cscdMaxf(0.f, ceilf((box->scrollTop + h) / rh) + LIST_OVERSCAN);
    if (first > l->count) first = l->count;
    if (end > l->count) end = l->count;
    if (!all && first == l->first && end == l->first + l->rows.len) return false;
    /* the rows still in range stay; the others go, and the new items get fresh copies */
    CscdPtrVec rows = {0};
    for (size_t i = first; i < end; ++i) {
        CscdElement* row = NULL;
        if (!all && i >= l->first && i < l->first + l->rows.len && l->rows.items[i - l->first]) {
            row = (CscdElement*)l->rows.items[i - l->first];
            l->rows.items[i - l->first] = NULL;
        } else {
            row = cscdElementClone(l->proto, true);
            cscdElementSetStyle(row, CSCD_SV("position"), CSCD_SV("absolute"));
            cscdElementSetStyle(row, CSCD_SV("left"), CSCD_SV("0"));
            cscdElementSetStyle(row, CSCD_SV("right"), CSCD_SV("0"));
            setPx(row, "top", (float)i * rh);
            setPx(row, "height", rh);
            if (l->fill) l->fill(l->user, row, i);
            cscdNodeAppendChild(&l->box->node, &row->node);
        }
        cscdVecPush(&rows, row);
    }
    CscdPtrVec old = l->rows;
    l->rows = rows;
    l->first = first;
    for (size_t i = 0; i < old.len; ++i)
        if (old.items[i]) cscdElementRemove((CscdElement*)old.items[i]);
    cscdVecFree(&old);
    return true;
}

bool viewUpdateLists(CscdView* v) {
    bool changed = false;
    for (size_t i = 0; i < v->lists.len; ++i) changed = updateList(v, v->lists.items[i], false) || changed;
    return changed;
}

void viewFreeLists(CscdView* v) {
    for (size_t i = 0; i < v->lists.len; ++i) freeList(v->lists.items[i]);
    cscdVecFree(&v->lists);
}

void cscdViewBindList(CscdView* v, CscdElement* box, CscdElement* row, size_t count, float rowHeight, CscdListFillFn fill,
                      void* user, CscdFreeFn freeUser) {
    if (!box || !row) {
        if (freeUser) freeUser(user);
        return;
    }
    cscdViewUnbindList(v, box);
    VirtualList* l = CSCD_NEW(VirtualList);
    l->box = box;
    l->proto = cscdElementIs(row, "template") ? cscdTemplateInstantiate(row) : cscdElementClone(row, true);
    if (!l->proto) l->proto = cscdElementNew(CSCD_SV("div"));
    l->count = count;
    l->rowHeight = rowHeight > 0 ? rowHeight : 1;
    l->fill = fill;
    l->user = user;
    l->freeUser = freeUser;
    /* the rows are placed in box; the spacer gives it the height of them all */
    if (!box->style || box->style->position == CSCD_POS_STATIC) cscdElementSetStyle(box, CSCD_SV("position"), CSCD_SV("relative"));
    l->spacer = cscdElementAppendElement(box, CSCD_SV("div"));
    cscdElementSetAttribute(l->spacer, CSCD_SV("aria-hidden"), CSCD_SV("true"));
    setPx(l->spacer, "height", (float)count * l->rowHeight);
    cscdVecPush(&v->lists, l);
    updateList(v, l, true);
}

void cscdViewListSetCount(CscdView* v, CscdElement* box, size_t count) {
    VirtualList* l = findList(v, box);
    if (!l) return;
    l->count = count;
    if (l->spacer) setPx(l->spacer, "height", (float)count * l->rowHeight);
    updateList(v, l, true);
}

void cscdViewListRefresh(CscdView* v, CscdElement* box) {
    VirtualList* l = findList(v, box);
    if (l) updateList(v, l, true);
}

void cscdViewUnbindList(CscdView* v, CscdElement* box) {
    for (size_t i = 0; i < v->lists.len; ++i)
        if (v->lists.items[i]->box == box) {
            freeList(v->lists.items[i]);
            cscdVecRemove(&v->lists, i);
            return;
        }
}

size_t cscdViewListFirstShown(const CscdView* v, const CscdElement* box) {
    const VirtualList* l = findList(v, box);
    if (!l) return 0;
    const size_t at = (size_t)floorf(box->scrollTop / l->rowHeight);
    return at < l->count ? at : l->count;
}

void viewNodeRemoved(CscdView* v, CscdNode* n) {
    if (v->bindings.len) viewBindingsNodeRemoved(v, n);
    CscdElement* e = (CscdElement*)n; /* (compared as a pointer only) */
    if (v->hits.len && cscdNodeIsElement(n)) {
        bool inserted; /* (until the next paint the hits still name it: a pointer event comes first) */
        cscdU64MapPut(&v->hitsGone, CSCD_PKEY(e), &inserted);
    }
    if (v->inspected == e) v->inspected = NULL;
    if (v->handlesFor == e) v->handlesFor = NULL; /* (the menu goes at the next update) */
    viewA11yForget(v, e);
    for (size_t i = v->lists.len; i-- > 0;) {
        VirtualList* l = v->lists.items[i];
        if (l->box == e) { /* (its rows go with it) */
            freeList(l);
            cscdVecRemove(&v->lists, i);
            continue;
        }
        if (l->spacer == e) l->spacer = NULL;
        for (size_t k = 0; k < l->rows.len; ++k)
            if (l->rows.items[k] == e) l->rows.items[k] = NULL;
    }
    for (size_t i = 0; i < v->watched.len; ++i)
        if (*v->watched.items[i] == e) *v->watched.items[i] = NULL;
    for (size_t i = v->stateChanged.len; i-- > 0;)
        if (v->stateChanged.items[i] == e) cscdVecRemove(&v->stateChanged, i);
    for (size_t i = v->smooth.len; i-- > 0;)
        if (v->smooth.items[i].e == e) cscdVecRemove(&v->smooth, i);
    if (v->pressed == e) v->pressed = NULL;
    if (v->dragRange == e) v->dragRange = NULL;
    if (v->scrollDrag == e) v->scrollDrag = NULL;
    for (size_t i = v->transitions.len; i-- > 0;)
        if (v->transitions.items[i].e == e) {
            RunningTransition* r = &v->transitions.items[i];
            cscdStrFree(&r->prop);
            cscdRelease(r->from);
            cscdRelease(r->to);
            cscdVecRemove(&v->transitions, i);
        }
    for (size_t i = v->animations.len; i-- > 0;)
        if (v->animations.items[i].e == e) {
            cscdStrFree(&v->animations.items[i].a.name);
            cscdVecRemove(&v->animations, i);
        }
    for (size_t i = 0; i < v->menus.len; ++i) {
        OpenMenu* m = menuAt(v, i);
        if (m->box == e) m->box = NULL;
        if (m->owner == e) m->owner = NULL;
    }
    for (size_t i = v->menuBar.len; i-- > 0;)
        if (v->menuBar.items[i].entry == e) {
            if (v->menuBar.items[i].freeUser) v->menuBar.items[i].freeUser(v->menuBar.items[i].user);
            cscdVecRemove(&v->menuBar, i);
        }
    if (v->menuBarOpen == e) v->menuBarOpen = NULL;
    if (v->tip == e) v->tip = NULL;
    if (v->tipFor == e) v->tipFor = NULL;
    if (v->dragSource == e) v->dragSource = NULL;
    if (v->dragOver == e) v->dragOver = NULL;
    if (v->touchScroll == e) v->touchScroll = NULL;
    for (size_t i = 0; i < v->touches.len; ++i)
        if (v->touches.items[i].target == e) v->touches.items[i].target = NULL;
    if (v->splitter == e || v->splitPrev == e || v->splitNext == e) v->splitter = v->splitPrev = v->splitNext = NULL;
    CscdMapVal old;
    if (cscdU64MapRemove(&v->undo, CSCD_PKEY(e), &old)) {
        UndoStack* u = (UndoStack*)old.p;
        for (size_t i = 0; i < u->undo.len; ++i) cscdStrFree(&u->undo.items[i].value);
        for (size_t i = 0; i < u->redo.len; ++i) cscdStrFree(&u->redo.items[i].value);
        cscdVecFree(&u->undo);
        cscdVecFree(&u->redo);
        cscdFree(u);
    }
    for (size_t i = v->popovers.len; i-- > 0;)
        if (v->popovers.items[i] == e) cscdVecRemove(&v->popovers, i);
    for (size_t i = 0; i < v->modals.len;) {
        if (v->modals.items[i] == e) {
            cscdVecRemove(&v->modals, i);
            cscdVecRemove(&v->modalReturnFocus, i);
            continue;
        }
        if (v->modalReturnFocus.items[i] == e) v->modalReturnFocus.items[i] = NULL;
        ++i;
    }
    if (v->onNodeRemoved) v->onNodeRemoved(v->onNodeRemovedUser, n);
}
