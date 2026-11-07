/* The keyboard and editing text: key events, moving the caret and the selection (characters, words,
 * lines), typing, deleting and indenting in fields, and focus moving with Tab. */
#include "style/styleInternal.h"
#include "view/viewInternal.h"

#include <stdio.h>
#include <stdlib.h>

size_t prevChar(CscdSv s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && ((unsigned char)s.ptr[i] & 0xC0) == 0x80) --i;
    return i;
}

size_t nextChar(CscdSv s, size_t i) {
    if (i >= s.len) return s.len;
    ++i;
    while (i < s.len && ((unsigned char)s.ptr[i] & 0xC0) == 0x80) ++i;
    return i;
}

size_t prevWord(CscdSv s, size_t i) {
    while (i > 0 && s.ptr[i - 1] == ' ') --i;
    while (i > 0 && s.ptr[i - 1] != ' ') i = prevChar(s, i);
    return i;
}

size_t nextWord(CscdSv s, size_t i) {
    while (i < s.len && s.ptr[i] != ' ') i = nextChar(s, i);
    while (i < s.len && s.ptr[i] == ' ') ++i;
    return i;
}

void viewEditInput(CscdView* v, CscdElement* e, CscdSv insert, int deleteDir, bool word) {
    v->markStart = v->markEnd = 0;
    CscdStr* val = &e->editValue;
    size_t a = cscdMinz(e->selAnchor, e->caret), z = cscdMaxz(e->selAnchor, e->caret);
    a = cscdMinz(a, val->len);
    z = cscdMinz(z, val->len);
    if (a == z && !insert.len && ((deleteDir < 0 && a == 0) || (deleteDir > 0 && z == val->len) || deleteDir == 0)) return;
    /* typing and deleting one character at a time are undone as runs */
    viewPushUndo(v, e, !insert.len ? 2 : (nextChar(insert, 0) == insert.len && !cscdSvEqC(insert, "\n") && a == z) ? 1 : 3);
    CscdSv cur = cscdStrSv(val);
    if (a == z && deleteDir < 0) a = word ? prevWord(cur, a) : prevChar(cur, a);
    else if (a == z && deleteDir > 0) z = word ? nextWord(cur, z) : nextChar(cur, z);
    CscdStr ins = {0};
    for (size_t i = 0; i < insert.len; ++i)
        if (!(cscdElementIs(e, "input") && insert.ptr[i] == '\n')) cscdStrAppendChar(&ins, insert.ptr[i]);
    if (cscdElementHasAttributeC(e, "maxlength")) {
        char* c = cscdSvDupC(cscdElementAttrC(e, "maxlength"));
        size_t maxLen = (size_t)atoi(c);
        cscdFree(c);
        size_t count = 0;
        for (size_t i = 0; i < val->len; i = nextChar(cur, i)) ++count;
        for (size_t i = a; i < z; i = nextChar(cur, i)) --count;
        CscdStr trimmed = {0};
        CscdSv is = cscdStrSv(&ins);
        for (size_t i = 0; i < is.len && count < maxLen; ++count) {
            size_t j = nextChar(is, i);
            cscdStrAppend(&trimmed, cscdSvSub(is, i, j - i));
            i = j;
        }
        cscdStrFree(&ins);
        ins = trimmed;
    }
    cscdStrErase(val, a, z - a);
    cscdStrInsert(val, a, cscdStrSv(&ins));
    e->caret = e->selAnchor = a + ins.len;
    cscdStrFree(&ins);
    e->valueDirty = true;
    v->caretTime = viewNow(v);
    cscdNodeMarkDirty(&e->node);
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &e);
    viewSendSimple(v, "input", e);
    if (e) viewEnsureCaretVisible(v, e);
    viewUnwatch(v, mark);
    cscdViewInvalidate(v);
}

void cscdViewTextInput(CscdView* v, CscdSv utf8) {
    CscdElement* f = v->doc->focused;
    if (!f || !isTextControl(f) || cscdElementHasAttributeC(f, "readonly") || cscdElementDisabled(f)) return;
    if (!utf8.len) return;
    cscdViewHideSelectionHandles(v);
    CscdSv s = cscdSvEqC(utf8, "\r") ? CSCD_SV("\n") : utf8;
    if ((unsigned char)s.ptr[0] < 0x20 && !cscdSvEqC(s, "\n") && !cscdSvEqC(s, "\t")) return;
    if (cscdSvEqC(s, "\n") && cscdElementIs(f, "input")) return;
    viewEditInput(v, f, s, 0, false);
}

bool viewContextKey(CscdView* v) {
    /* the Menu key or Shift+F10: a context menu for the focused element (at its caret, if it has one) */
    CscdElement* target = v->doc->focused ? v->doc->focused : v->doc->hovered ? v->doc->hovered : cscdDocumentBody(v->doc);
    if (!target) return false;
    const bool text = isTextControl(target);
    CscdRect r = text ? cscdViewCaretRect(v) : cscdViewElementRect(v, target);
    CscdEvent c;
    cscdEventInit(&c, CSCD_SV("contextmenu"));
    c.x = text ? r.x : r.x + cscdMinf(r.w / 2, 24.f);
    c.y = text ? cscdRectBottom(r) : r.y + cscdMinf(r.h / 2, 16.f);
    c.button = 2;
    viewDispatch(v, &c, target);
    cscdEventFini(&c);
    return true;
}

static void keyEvent(CscdEvent* ev, const char* type, CscdModifiers m, CscdSv keyName) {
    cscdEventInit(ev, cscdSv(type));
    cscdStrSet(&ev->key, keyName);
    ev->shift = m.shift; ev->ctrl = m.ctrl; ev->alt = m.alt; ev->meta = m.meta;
}

static void collectFocusable(CscdNode* n, CscdPtrVec* all) {
    CscdElement* e = cscdNodeAsElement(n);
    if (e) {
        if (!e->layout && !cscdElementIs(e, "html") && !cscdElementIs(e, "body") && e->style && e->style->display == CSCD_DISPLAY_NONE) return;
        if (isFocusable(e) && e->layout) cscdVecPush(all, e);
    }
    for (size_t i = 0; i < n->children.len; ++i) collectFocusable(cscdNodeChild(n, i), all);
}

/* code fields (<textarea code>): Tab indents, Shift+Tab outdents */
static void indentLines(CscdView* v, CscdElement* f, CscdModifiers m) {
    CscdStr* val = &f->editValue;
    size_t a = cscdMinz(f->selAnchor, f->caret), z = cscdMaxz(f->selAnchor, f->caret);
    size_t ls = a == 0 ? CSCD_NPOS : cscdSvRFindChar(cscdSvSub(cscdStrSv(val), 0, a), '\n');
    ls = ls == CSCD_NPOS || a == 0 ? 0 : ls + 1;
    if (a == z && !m.shift) {
        char spaces[5] = "    ";
        spaces[4 - (a - ls) % 4] = 0;
        viewEditInput(v, f, cscdSv(spaces), 0, false);
        return;
    }
    /* every line the selection touches */
    viewPushUndo(v, f, 3);
    size_t lastEnd = z > a && val->ptr[z - 1] == '\n' ? z - 1 : z;
    CscdSizeVec starts = {0};
    for (size_t p = ls;;) {
        cscdVecPush(&starts, p);
        size_t nl = cscdSvFindChar(cscdStrSv(val), '\n', p);
        if (nl == CSCD_NPOS || nl >= lastEnd) break;
        p = nl + 1;
    }
    long dA = 0, dZ = 0;
    for (size_t k = starts.len; k-- > 0;) {
        size_t p = starts.items[k];
        if (m.shift) {
            size_t n = 0;
            while (n < 4 && p + n < val->len && val->ptr[p + n] == ' ') ++n;
            cscdStrErase(val, p, n);
            if (p < a) dA -= (long)cscdMinz(n, a - p);
            if (p < z) dZ -= (long)cscdMinz(n, z - p);
        } else {
            cscdStrInsert(val, p, CSCD_SV("    "));
            if (p <= a) dA += 4;
            dZ += 4;
        }
    }
    cscdVecFree(&starts);
    size_t na = (size_t)(((long)a + dA) < 0 ? 0 : (long)a + dA), nz = (size_t)(((long)z + dZ) < 0 ? 0 : (long)z + dZ);
    bool forward = f->selAnchor <= f->caret;
    f->selAnchor = cscdMinz(forward ? na : nz, val->len);
    f->caret = cscdMinz(forward ? nz : na, val->len);
    f->valueDirty = true;
    cscdNodeMarkDirty(&f->node);
    viewSendSimple(v, "input", f);
    cscdViewInvalidate(v);
}

typedef struct TextKeys { CscdView* v; CscdElement* f; CscdModifiers m; bool area; } TextKeys;

static void moveCaret(TextKeys* T, size_t pos) {
    CscdElement* f = T->f;
    f->caret = cscdMinz(pos, f->editValue.len);
    if (!T->m.shift) f->selAnchor = f->caret;
    T->v->caretTime = viewNow(T->v);
    viewEnsureCaretVisible(T->v, f);
    cscdViewInvalidate(T->v);
}

/* the lines as drawn (a textarea wraps), for Home / End / Up / Down / Page keys */
static void drawnLines(TextKeys* T, EditLineVec* out) {
    CscdElement* f = T->f;
    CscdFont font = f->style ? cscdFontForStyle(f->style) : cscdFontDefault();
    float w = f->layout ? cscdBoxContentBox(f->layout).w : 0;
    editLines(cscdStrSv(&f->editValue), &font, w, T->area && editWraps(f), f->style ? f->style->letterSpacing : 0, out);
}

static void moveVertical(TextKeys* T, int delta) {
    CscdElement* f = T->f;
    if (!f->style) return;
    EditLineVec ls = {0};
    drawnLines(T, &ls);
    CscdFont font = cscdFontForStyle(f->style);
    float lsp = f->style->letterSpacing;
    CscdSv val = cscdStrSv(&f->editValue);
    size_t i = editLineOf(&ls, f->caret);
    EditRunVec runs = {0};
    editRuns(val, ls.items[i], &font, lsp, f->style->rtl, &runs);
    float x = editCaretX(&runs, val, &font, lsp, f->caret);
    long j = (long)i + delta;
    if (j < 0) moveCaret(T, 0);
    else if (j >= (long)ls.len) moveCaret(T, val.len);
    else {
        editRuns(val, ls.items[j], &font, lsp, f->style->rtl, &runs);
        moveCaret(T, editRunsHit(&runs, val, &font, lsp, x));
    }
    cscdVecFree(&runs);
    cscdVecFree(&ls);
}

/* the keys of a text field; false: not one it takes */
static void textFieldKey(CscdView* v, CscdElement* f, CscdKey k, CscdModifiers m) {
    TextKeys T = {v, f, m, cscdElementIs(f, "textarea")};
    CscdStr* val = &f->editValue;
    const bool ro = cscdElementHasAttributeC(f, "readonly") || cscdElementDisabled(f);
    const bool area = T.area;
    switch (k) {
    case CSCD_KEY_BACKSPACE: if (!ro) viewEditInput(v, f, CSCD_SV(""), -1, m.ctrl); return;
    case CSCD_KEY_DELETE: if (!ro) viewEditInput(v, f, CSCD_SV(""), 1, m.ctrl); return;
    case CSCD_KEY_LEFT:
        if (!m.shift && f->selAnchor != f->caret) { moveCaret(&T, cscdMinz(f->selAnchor, f->caret)); return; }
        moveCaret(&T, m.ctrl ? prevWord(cscdStrSv(val), f->caret) : prevChar(cscdStrSv(val), f->caret));
        return;
    case CSCD_KEY_RIGHT:
        if (!m.shift && f->selAnchor != f->caret) { moveCaret(&T, cscdMaxz(f->selAnchor, f->caret)); return; }
        moveCaret(&T, m.ctrl ? nextWord(cscdStrSv(val), f->caret) : nextChar(cscdStrSv(val), f->caret));
        return;
    case CSCD_KEY_HOME: {
        if (!area || m.ctrl) { moveCaret(&T, 0); return; }
        EditLineVec ls = {0};
        drawnLines(&T, &ls);
        const EditLine l = ls.items[editLineOf(&ls, f->caret)];
        cscdVecFree(&ls);
        /* code: first to the indentation, then to the line start */
        size_t indent = l.start;
        while (indent < l.end && val->ptr[indent] == ' ') ++indent;
        moveCaret(&T, cscdElementHasAttributeC(f, "code") && f->caret != indent ? indent : l.start);
        return;
    }
    case CSCD_KEY_END: {
        if (!area || m.ctrl) { moveCaret(&T, val->len); return; }
        EditLineVec ls = {0};
        drawnLines(&T, &ls);
        size_t i = editLineOf(&ls, f->caret);
        size_t end = ls.items[i].end;
        if (i + 1 < ls.len && ls.items[i + 1].start == end && end > ls.items[i].start) end = prevChar(cscdStrSv(val), end); /* a wrapped line: before its last space */
        cscdVecFree(&ls);
        moveCaret(&T, end);
        return;
    }
    case CSCD_KEY_UP: if (area) moveVertical(&T, -1); return;
    case CSCD_KEY_DOWN: if (area) moveVertical(&T, 1); return;
    case CSCD_KEY_PAGE_UP: case CSCD_KEY_PAGE_DOWN:
        if (area && f->style && f->layout) {
            CscdFont font = cscdFontForStyle(f->style);
            int page = cscdMaxi(1, (int)(cscdBoxContentBox(f->layout).h / cscdMaxf(1.f, editLineHeight(f->style, &font))) - 1);
            moveVertical(&T, k == CSCD_KEY_PAGE_UP ? -page : page);
        }
        return;
    case CSCD_KEY_ENTER:
        if (area) {
            if (ro) return;
            /* code: the new line keeps the indentation of the one it splits */
            CscdStr ins = cscdStrDupC("\n");
            if (cscdElementHasAttributeC(f, "code")) {
                size_t a = cscdMinz(f->selAnchor, f->caret);
                size_t ls = a == 0 ? CSCD_NPOS : cscdSvRFindChar(cscdSvSub(cscdStrSv(val), 0, a), '\n');
                ls = ls == CSCD_NPOS || a == 0 ? 0 : ls + 1;
                size_t n = ls;
                while (n < a && (val->ptr[n] == ' ' || val->ptr[n] == '\t')) ++n;
                cscdStrAppend(&ins, cscdSvSub(cscdStrSv(val), ls, n - ls));
                if (a > ls && (val->ptr[a - 1] == '{' || val->ptr[a - 1] == '(' || val->ptr[a - 1] == '[')) cscdStrAppendC(&ins, "    ");
            }
            viewEditInput(v, f, cscdStrSv(&ins), 0, false);
            cscdStrFree(&ins);
            return;
        }
        {
            const size_t mark = viewWatchMark(v);
            viewWatch(v, &f);
            viewSendSimple(v, "change", f);
            if (f) viewSubmitForm(v, f);
            viewUnwatch(v, mark);
        }
        return;
    case CSCD_KEY_A: if (m.ctrl) { f->selAnchor = 0; f->caret = val->len; cscdViewInvalidate(v); } return;
    case CSCD_KEY_C: case CSCD_KEY_X:
        if (m.ctrl && f->selAnchor != f->caret && v->setClipboard && !cscdSvIEqC(cscdElementAttrC(f, "type"), "password")) {
            size_t a = cscdMinz(f->selAnchor, f->caret), z = cscdMaxz(f->selAnchor, f->caret);
            v->setClipboard(v->clipboardUser, cscdSvSub(cscdStrSv(val), a, z - a));
            if (k == CSCD_KEY_X && !ro) viewEditInput(v, f, CSCD_SV(""), 0, false);
        }
        return;
    case CSCD_KEY_V:
        if (m.ctrl && v->getClipboard && !ro) {
            CscdStr text = {0};
            v->getClipboard(v->clipboardUser, &text);
            viewEditInput(v, f, cscdStrSv(&text), 0, false);
            cscdStrFree(&text);
        }
        return;
    case CSCD_KEY_Z: if (m.ctrl && !ro) viewUndoEdit(v, f, m.shift); return;
    case CSCD_KEY_Y: if (m.ctrl && !ro) viewUndoEdit(v, f, true); return;
    default: return;
    }
}

void cscdViewKeyDown(CscdView* v, CscdKey k, CscdModifiers m, CscdSv keyName) {
    if (viewInspectorKey(v, k, m, keyName)) return;
    cscdViewHideTooltip(v);
    if (k != CSCD_KEY_SHIFT && k != CSCD_KEY_CONTROL && k != CSCD_KEY_ALT && k != CSCD_KEY_META) cscdViewHideSelectionHandles(v);
    if (v->dragActive) {
        if (k == CSCD_KEY_ESCAPE) cscdViewCancelDrag(v);
        return;
    }
    if (viewMenuKey(v, k, keyName)) return;
    if (cscdSvEqC(keyName, "ContextMenu") || (k == CSCD_KEY_F10 && m.shift)) { viewContextKey(v); return; }
    if (v->modals.len && k == CSCD_KEY_ESCAPE && !v->composition.len) {
        CscdElement* d = (CscdElement*)cscdVecLast(&v->modals);
        CscdEvent c;
        cscdEventInit(&c, CSCD_SV("cancel"));
        const size_t mark = viewWatchMark(v);
        viewWatch(v, &d);
        viewDispatch(v, &c, d);
        if (!c.defaultPrevented && d) cscdViewCloseDialog(v, d, CSCD_SV(""));
        viewUnwatch(v, mark);
        cscdEventFini(&c);
        return;
    }
    CscdElement* f = v->doc->focused;
    CscdEvent ev;
    keyEvent(&ev, "keydown", m, keyName);
    viewDispatch(v, &ev, f ? f : cscdDocumentBody(v->doc));
    const bool prevented = ev.defaultPrevented;
    cscdEventFini(&ev);
    if (prevented) return;
    /* Esc hides the top popover (after the page heard the key) */
    if (v->popovers.len && k == CSCD_KEY_ESCAPE && !v->composition.len) {
        cscdViewHidePopover(v, (CscdElement*)cscdVecLast(&v->popovers));
        return;
    }
    if (f != v->doc->focused) return;
    /* Esc hands the keys back to the program */
    if (k == CSCD_KEY_ESCAPE && f && cscdSvEqC(cscdElementAttrC(f, "data-focus"), "keyboard")) {
        cscdViewFocus(v, NULL);
        return;
    }
    if (k == CSCD_KEY_TAB && f && cscdElementIs(f, "textarea") && cscdElementHasAttributeC(f, "code") && !cscdElementHasAttributeC(f, "readonly") && !m.ctrl) {
        indentLines(v, f, m);
        return;
    }
    if (k == CSCD_KEY_TAB) {
        /* focus navigation in document order */
        CscdPtrVec all = {0};
        collectFocusable(v->modals.len ? &((CscdElement*)cscdVecLast(&v->modals))->node : &v->doc->node, &all);
        if (!all.len) return;
        size_t at = CSCD_NPOS;
        for (size_t i = 0; i < all.len; ++i) if (all.items[i] == f) { at = i; break; }
        /* nothing focused (a panel made again, Esc): on from where Tab was, not from the top */
        if (at == CSCD_NPOS && v->tabPlace) at = (v->tabPlace - 1) % all.len;
        size_t to;
        if (at == CSCD_NPOS) to = m.shift ? all.len - 1 : 0;
        else if (m.shift) to = at == 0 ? all.len - 1 : at - 1;
        else to = at + 1 == all.len ? 0 : at + 1;
        CscdElement* next = (CscdElement*)all.items[to];
        v->tabPlace = to + 1;
        cscdVecFree(&all);
        const size_t mark = viewWatchMark(v);
        viewWatch(v, &next);
        cscdViewFocus(v, next);
        if (next) {
            next->state |= CSCD_STATE_FOCUS_VISIBLE;
            cscdViewScrollIntoView(v, next);
        }
        viewUnwatch(v, mark);
        return;
    }
    if (f && isTextControl(f)) {
        textFieldKey(v, f, k, m);
        return;
    }
    /* a menu of the bar opens from the keyboard too, on its first row */
    if (f && (k == CSCD_KEY_ENTER || k == CSCD_KEY_SPACE || k == CSCD_KEY_DOWN))
        for (size_t i = 0; i < v->menuBar.len; ++i)
            if (v->menuBar.items[i].entry == f) {
                viewOpenMenuBar(v, f);
                viewMenuKey(v, CSCD_KEY_DOWN, CSCD_SV(""));
                return;
            }
    if (f && (k == CSCD_KEY_ENTER || k == CSCD_KEY_SPACE)) {
        if (cscdElementIs(f, "button") || (cscdElementIs(f, "input") && !isTextControl(f)) || cscdElementIs(f, "a") || cscdElementIs(f, "summary") ||
            cscdElementHasAttributeC(f, "tabindex")) {
            if (!(cscdElementIs(f, "input") && cscdSvIEqC(cscdElementAttrC(f, "type"), "range"))) {
                CscdEvent c;
                cscdEventInit(&c, CSCD_SV("click"));
                c.target = f;
                const size_t mark = viewWatchMark(v);
                viewWatch(v, &f);
                viewDispatch(v, &c, f);
                if (!c.defaultPrevented && f) {
                    c.target = f;
                    viewDefaultAction(v, &c);
                }
                viewUnwatch(v, mark);
                cscdEventFini(&c);
                return;
            }
        }
    }
    if (f && cscdElementIs(f, "input") && cscdSvIEqC(cscdElementAttrC(f, "type"), "range") &&
        (k == CSCD_KEY_LEFT || k == CSCD_KEY_RIGHT || k == CSCD_KEY_UP || k == CSCD_KEY_DOWN)) {
        float mn = viewAttrNumber(f, "min", 0), mx = viewAttrNumber(f, "max", 100);
        float step = viewAttrNumber(f, "step", 1);
        if (step <= 0) step = 1;
        CscdStr cur = cscdElementValue(f);
        float val = !cur.len ? (mn + mx) / 2 : (float)atof(cscdStrC(&cur));
        cscdStrFree(&cur);
        val = cscdClampf(val + ((k == CSCD_KEY_LEFT || k == CSCD_KEY_DOWN) ? -step : step), mn, mx);
        char buf[32];
        snprintf(buf, sizeof buf, "%g", val);
        cscdElementSetValue(f, cscdSv(buf));
        viewInputAndChange(v, f);
        return;
    }
    if (f && cscdElementIs(f, "select") && (k == CSCD_KEY_UP || k == CSCD_KEY_DOWN)) {
        CscdPtrVec opts = {0};
        cscdElementQuerySelectorAll(f, CSCD_SV("option"), &opts);
        if (!opts.len) return;
        CscdStr cur = cscdElementValue(f);
        size_t idx = 0;
        for (size_t i = 0; i < opts.len; ++i) {
            CscdStr ov = cscdElementValue((CscdElement*)opts.items[i]);
            if (cscdStrEq(&ov, &cur)) idx = i;
            cscdStrFree(&ov);
        }
        cscdStrFree(&cur);
        if (k == CSCD_KEY_UP && idx > 0) --idx;
        if (k == CSCD_KEY_DOWN && idx + 1 < opts.len) ++idx;
        CscdStr nv = cscdElementValue((CscdElement*)opts.items[idx]);
        cscdElementSetValue(f, cscdStrSv(&nv));
        cscdStrFree(&nv);
        cscdVecFree(&opts);
        viewSendSimple(v, "change", f);
        return;
    }
    /* document scrolling */
    float step = 40;
    switch (k) {
    case CSCD_KEY_DOWN: cscdViewScrollTo(v, v->scrollX, v->scrollY + step); break;
    case CSCD_KEY_UP: cscdViewScrollTo(v, v->scrollX, v->scrollY - step); break;
    case CSCD_KEY_PAGE_DOWN: case CSCD_KEY_SPACE: cscdViewScrollTo(v, v->scrollX, v->scrollY + v->vh * 0.9f); break;
    case CSCD_KEY_PAGE_UP: cscdViewScrollTo(v, v->scrollX, v->scrollY - v->vh * 0.9f); break;
    case CSCD_KEY_HOME: if (!f) cscdViewScrollTo(v, v->scrollX, 0); break;
    case CSCD_KEY_END: if (!f) cscdViewScrollTo(v, v->scrollX, cscdViewContentHeight(v)); break;
    case CSCD_KEY_ESCAPE: if (f) cscdViewBlur(v); break;
    default: break;
    }
}

void cscdViewKeyUp(CscdView* v, CscdKey k, CscdModifiers m, CscdSv keyName) {
    (void)k;
    CscdEvent ev;
    keyEvent(&ev, "keyup", m, keyName);
    CscdElement* f = v->doc->focused;
    viewDispatch(v, &ev, f ? f : cscdDocumentBody(v->doc));
    cscdEventFini(&ev);
}
