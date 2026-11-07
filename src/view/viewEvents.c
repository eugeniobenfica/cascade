/* Events into the page: dispatching them (capture and bubbling), hover and active, the focus, the
 * mouse, and the default actions of links, buttons and form controls. */
#include "style/styleInternal.h"
#include "view/viewInternal.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- events -------------------------------------------------------------------------------------- */

static bool listenerStillThere(const CscdElement* e, const CscdListener* l) {
    for (size_t i = 0; i < e->listeners.len; ++i) {
        const CscdListener* o = &e->listeners.items[i];
        if (o->handler.fn == l->handler.fn && o->handler.user == l->handler.user && o->capture == l->capture) return true;
    }
    return false;
}

static void callListeners(CscdElement* e, CscdEvent* ev, bool capture) {
    /* a copy: a listener may add or remove listeners (those it removes are not called) */
    const size_t n = e->listeners.len;
    if (!n) return;
    CscdListener* ls = cscdMalloc(n * sizeof *ls);
    memcpy(ls, e->listeners.items, n * sizeof *ls);
    for (size_t i = 0; i < n; ++i) {
        if (ls[i].capture != capture || !cscdStrEq(&ls[i].type, &ev->type)) continue;
        if (i && !listenerStillThere(e, &ls[i])) continue;
        ls[i].handler.fn(ls[i].handler.user, ev);
    }
    cscdFree(ls);
}

void viewDispatch(CscdView* v, CscdEvent* ev, CscdElement* target) {
    if (!target) return;
    ev->target = target;
    if (v->bindings.len && (cscdStrEqC(&ev->type, "input") || cscdStrEqC(&ev->type, "change"))) viewBindingsFromPage(v, target);
    CscdPtrVec path = {0};
    for (CscdElement* e = target; e; e = parentEl(e)) cscdVecPush(&path, e);
    CscdDocument* doc = v->doc;
    cscdDocumentBeginDispatch(doc);
    /* the elements a listener removes on the way get no more listeners called */
    const size_t mark = viewWatchMark(v);
    for (size_t i = 0; i < path.len; ++i) viewWatch(v, (CscdElement**)&path.items[i]);
    /* capture */
    for (size_t i = path.len; i-- > 0 && !ev->propagationStopped;) {
        CscdElement* e = (CscdElement*)path.items[i];
        if (!e) continue;
        ev->currentTarget = e;
        callListeners(e, ev, true);
    }
    const bool bubbles = !cscdStrEqC(&ev->type, "mouseenter") && !cscdStrEqC(&ev->type, "mouseleave") && !cscdStrEqC(&ev->type, "focus") &&
                         !cscdStrEqC(&ev->type, "blur");
    for (size_t i = 0; i < path.len; ++i) {
        if (ev->propagationStopped) break;
        CscdElement* e = (CscdElement*)path.items[i];
        if (!e) {
            if (!bubbles) break;
            continue;
        }
        ev->currentTarget = e;
        CscdRect r = cscdElementBoundingClientRect(e);
        ev->offsetX = ev->x - (r.x - v->scrollX);
        ev->offsetY = ev->y - (r.y - v->scrollY);
        callListeners(e, ev, false);
        if (!bubbles) break;
    }
    CscdElement* still = (CscdElement*)path.items[0]; /* (NULL when a listener removed it) */
    viewUnwatch(v, mark);
    cscdVecFree(&path);
    cscdDocumentEndDispatch(doc);
    if (still) viewA11yNotify(v, cscdStrSv(&ev->type), still);
}

static void sendMouseEvent(CscdView* v, const char* type, CscdElement* target) {
    CscdEvent ev;
    cscdEventInit(&ev, cscdSv(type));
    ev.x = v->mouseX;
    ev.y = v->mouseY;
    viewDispatch(v, &ev, target);
    cscdEventFini(&ev);
}

/* The chain of e and its ancestors. */
static void chainOf(CscdElement* e, CscdPtrVec* out) {
    for (CscdElement* a = e; a; a = parentEl(a)) cscdVecPush(out, a);
}

void viewSetHover(CscdView* v, CscdElement* e) {
    if (v->doc->hovered == e) return;
    CscdPtrVec oldChain = {0}, newChain = {0};
    chainOf(v->doc->hovered, &oldChain);
    chainOf(e, &newChain);
    for (size_t i = 0; i < oldChain.len; ++i) ((CscdElement*)oldChain.items[i])->state &= ~(uint32_t)CSCD_STATE_HOVER;
    for (size_t i = 0; i < newChain.len; ++i) ((CscdElement*)newChain.items[i])->state |= CSCD_STATE_HOVER;
    CscdElement* old = v->doc->hovered;
    v->doc->hovered = e;
    /* the elements whose :hover changed: just their styles again (viewRestyleStates) */
    for (size_t i = 0; i < oldChain.len; ++i)
        if (!viewContainsPtr(&newChain, oldChain.items[i])) cscdVecPush(&v->stateChanged, oldChain.items[i]);
    for (size_t i = 0; i < newChain.len; ++i)
        if (!viewContainsPtr(&oldChain, newChain.items[i])) cscdVecPush(&v->stateChanged, newChain.items[i]);
    const size_t mark = viewWatchMark(v);
    for (size_t i = 0; i < oldChain.len; ++i) viewWatch(v, (CscdElement**)&oldChain.items[i]);
    for (size_t i = 0; i < newChain.len; ++i) viewWatch(v, (CscdElement**)&newChain.items[i]);
    viewWatch(v, &old);
    viewWatch(v, &e);
    if (old) {
        sendMouseEvent(v, "mouseout", old);
        for (size_t i = 0; i < oldChain.len; ++i) {
            CscdElement* a = (CscdElement*)oldChain.items[i];
            if (!a || viewContainsPtr(&newChain, a)) continue;
            sendMouseEvent(v, "mouseleave", a);
        }
    }
    if (e) {
        sendMouseEvent(v, "mouseover", e);
        for (size_t i = 0; i < newChain.len; ++i) {
            CscdElement* a = (CscdElement*)newChain.items[i];
            if (!a || viewContainsPtr(&oldChain, a)) continue;
            sendMouseEvent(v, "mouseenter", a);
        }
    }
    viewUnwatch(v, mark);
    cscdVecFree(&oldChain);
    cscdVecFree(&newChain);
    v->dirty = true;
    viewNotifyInvalidate(v);
}

void viewSetActive(CscdView* v, CscdElement* e) {
    if (v->doc->active == e) return;
    CscdPtrVec oldChain = {0}, newChain = {0};
    chainOf(v->doc->active, &oldChain);
    chainOf(e, &newChain);
    for (size_t i = 0; i < oldChain.len; ++i) ((CscdElement*)oldChain.items[i])->state &= ~(uint32_t)CSCD_STATE_ACTIVE;
    v->doc->active = e;
    for (size_t i = 0; i < newChain.len; ++i) ((CscdElement*)newChain.items[i])->state |= CSCD_STATE_ACTIVE;
    for (size_t i = 0; i < oldChain.len; ++i)
        if (!viewContainsPtr(&newChain, oldChain.items[i])) cscdVecPush(&v->stateChanged, oldChain.items[i]);
    for (size_t i = 0; i < newChain.len; ++i)
        if (!viewContainsPtr(&oldChain, newChain.items[i])) cscdVecPush(&v->stateChanged, newChain.items[i]);
    cscdVecFree(&oldChain);
    cscdVecFree(&newChain);
    v->dirty = true;
    viewNotifyInvalidate(v);
}

bool isFocusable(const CscdElement* e) {
    if (!e) return false;
    if (cscdElementHasAttributeC(e, "disabled")) return false;
    if (cscdElementIs(e, "input") && cscdSvIEqC(cscdElementAttrC(e, "type"), "hidden")) return false;
    if (cscdElementIs(e, "input") || cscdElementIs(e, "textarea") || cscdElementIs(e, "select") || cscdElementIs(e, "button")) return true;
    if (cscdElementIs(e, "a") && cscdElementHasAttributeC(e, "href")) return true;
    if (cscdElementHasAttributeC(e, "tabindex")) {
        char* c = cscdSvDupC(cscdElementAttrC(e, "tabindex"));
        const int ti = atoi(c);
        cscdFree(c);
        if (ti >= 0) return true;
    }
    if (cscdElementHasAttributeC(e, "contenteditable")) return true;
    if (cscdElementIs(e, "summary")) return true;
    return false;
}

bool isTextControl(const CscdElement* e) {
    if (!e) return false;
    if (cscdElementIs(e, "textarea")) return true;
    if (!cscdElementIs(e, "input")) return false;
    CscdSv t = cscdElementHasAttributeC(e, "type") ? cscdElementAttrC(e, "type") : CSCD_SV("text");
    static const char* other[] = {"checkbox", "radio", "range", "button", "submit", "reset", "color", "hidden", "image", "file"};
    for (size_t i = 0; i < CSCD_COUNT(other); ++i) if (cscdSvIEqC(t, other[i])) return false;
    return true;
}

void viewSendSimple(CscdView* v, const char* type, CscdElement* target) {
    CscdEvent ev;
    cscdEventInit(&ev, cscdSv(type));
    viewDispatch(v, &ev, target);
    cscdEventFini(&ev);
}

void cscdViewFocus(CscdView* v, CscdElement* e) {
    if (v->doc->focused == e) return;
    CscdElement* old = v->doc->focused;
    v->markStart = v->markEnd = 0; /* (a composition ends with its field's focus) */
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &e);
    if (old) {
        old->state &= ~(uint32_t)(CSCD_STATE_FOCUS | CSCD_STATE_FOCUS_VISIBLE);
        viewWatch(v, &old);
        viewSendSimple(v, "blur", old);
        if (old && isTextControl(old) && old->valueDirty) viewSendSimple(v, "change", old);
    }
    v->doc->focused = e;
    if (e) {
        e->state |= CSCD_STATE_FOCUS;
        if (isTextControl(e)) {
            e->state |= CSCD_STATE_FOCUS_VISIBLE;
            if (!e->valueDirty) {
                CscdStr val = cscdElementValue(e);
                cscdStrFree(&e->editValue);
                e->editValue = val;
                e->valueDirty = true;
            }
            e->caret = e->selAnchor = e->editValue.len;
        }
        viewSendSimple(v, "focus", e);
    }
    viewUnwatch(v, mark);
    v->caretTime = viewNow(v);
    v->styleDirty = true;
    cscdViewInvalidate(v);
}

void cscdViewBlur(CscdView* v) { cscdViewFocus(v, NULL); }

float viewAttrNumber(const CscdElement* e, const char* name, float def) {
    if (!cscdElementHasAttributeC(e, name)) return def;
    char* c = cscdSvDupC(cscdElementAttrC(e, name));
    float f = (float)atof(c);
    cscdFree(c);
    return f;
}

void cscdViewMouseMove(CscdView* v, float x, float y, int buttons, CscdModifiers m) {
    v->mouseX = x;
    v->mouseY = y;
    v->buttons = buttons;
    if (viewInspectorMouse(v, 0, x, y)) return;
    if (v->splitter) { viewSplitterMove(v, x, y); return; }
    if (v->dragActive) { viewDragMoveTo(v, x, y); return; }
    if (v->dragSource && (buttons & 1) && cscdHypotf(x - v->dragStartX, y - v->dragStartY) > 4) {
        viewBeginDrag(v, x, y);
        if (v->dragActive) return;
    }
    if (!(buttons & 1)) v->dragSource = NULL;
    if (viewMenuMouse(v, 0, x, y, 0)) return;
    if (v->scrollDrag && v->scrollDrag->layout) {
        CscdLayoutBox* b = v->scrollDrag->layout;
        CscdRect pb = cscdBoxPaddingBox(b);
        float maxY = cscdMaxf(0.f, b->overflow.h - pb.h);
        float th = cscdMaxf(20.f, (pb.h - 4) * pb.h / (pb.h + maxY));
        float delta = (y - v->scrollDragStart) * maxY / cscdMaxf(1.f, pb.h - 4 - th);
        v->scrollDrag->scrollTop = cscdClampf(v->scrollDragOrigin + delta, 0.f, maxY);
        cscdViewInvalidate(v);
        return;
    }
    if (v->dragRange) {
        CscdElement* range = v->dragRange;
        CscdRect r = cscdViewElementRect(v, range);
        float mn = viewAttrNumber(range, "min", 0), mx = viewAttrNumber(range, "max", 100);
        float step = viewAttrNumber(range, "step", 1);
        float t = cscdClampf((x - r.x - 8) / cscdMaxf(1.f, r.w - 16), 0.f, 1.f);
        float val = mn + (mx - mn) * t;
        if (step > 0) val = mn + roundf((val - mn) / step) * step;
        char buf[32];
        snprintf(buf, sizeof buf, "%g", val);
        CscdStr cur = cscdElementValue(range);
        if (!cscdStrEqC(&cur, buf)) {
            cscdElementSetValue(range, cscdSv(buf));
            viewSendSimple(v, "input", range);
        }
        cscdStrFree(&cur);
        cscdViewInvalidate(v);
        return;
    }
    CscdElement* hit = cscdViewElementAt(v, x, y);
    if (viewBlockedByModal(v, hit)) hit = (CscdElement*)cscdVecLast(&v->modals);
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &hit);
    viewSetHover(v, hit);
    viewTooltipMove(v, hit);
    CscdEvent ev;
    cscdEventInit(&ev, CSCD_SV("mousemove"));
    ev.x = x; ev.y = y; ev.buttons = buttons;
    ev.shift = m.shift; ev.ctrl = m.ctrl; ev.alt = m.alt;
    viewDispatch(v, &ev, hit);
    cscdEventFini(&ev);
    /* text selection by dragging in a text control */
    CscdElement* f = v->doc->focused;
    if (v->selecting && f && isTextControl(f) && (buttons & 1) && f->layout) {
        f->caret = viewTextHit(v, f, x, y);
        viewEnsureCaretVisible(v, f);
        cscdViewInvalidate(v);
    }
    /* cursor */
    CscdCursor c = CSCD_CURSOR_DEFAULT;
    for (CscdElement* a = hit; a; a = parentEl(a)) {
        if (cscdElementHasAttributeC(a, "splitter") && (!a->style || a->style->cursor == CSCD_CURSOR_AUTO)) {
            CscdElement* p = parentEl(a);
            c = p && p->style && cscdStyleIsColumn(p->style) ? CSCD_CURSOR_ROW_RESIZE : CSCD_CURSOR_COL_RESIZE;
            break;
        }
        if (a->style && a->style->cursor != CSCD_CURSOR_AUTO) { c = (CscdCursor)a->style->cursor; break; }
        if (isTextControl(a)) { c = CSCD_CURSOR_TEXT; break; }
    }
    if (c == CSCD_CURSOR_DEFAULT && hit && hit->style && hit->style->cursor == CSCD_CURSOR_AUTO) {
        /* over text */
        CscdPoint d = {x * v->dpr, y * v->dpr};
        for (size_t i = v->hits.len; i-- > 0;) {
            const CscdHitItem* it = &v->hits.items[i];
            if (!cscdRectContains(it->clip, d.x, d.y)) continue;
            CscdMatrix inv;
            if (!cscdMatInvert(it->m, &inv)) continue;
            CscdPoint l = cscdMatMap(inv, d);
            if (!cscdRectContains(it->rect, l.x, l.y)) continue;
            if (it->text && hit->style->userSelect != CSCD_US_NONE) c = CSCD_CURSOR_TEXT;
            break;
        }
    }
    viewUnwatch(v, mark);
    v->cursor = c;
}

void cscdViewMouseLeave(CscdView* v) {
    cscdViewHideTooltip(v);
    v->tipFor = NULL;
    if (v->dragActive || v->splitter) return;
    viewSetHover(v, NULL);
    v->mouseX = v->mouseY = -1;
}

void cscdViewMouseDown(CscdView* v, float x, float y, int button, CscdModifiers m, int clicks) {
    v->mouseX = x;
    v->mouseY = y;
    if (viewInspectorMouse(v, 1, x, y)) return;
    cscdViewHideTooltip(v);
    if (v->dragActive) return;
    if (viewMenuMouse(v, 1, x, y, button)) return;
    if (v->touchMode == TOUCH_NONE && v->handlesFor) cscdViewHideSelectionHandles(v); /* (a mouse, not a finger) */
    if (v->popovers.len) viewLightDismiss(v, cscdViewElementAt(v, x, y));
    /* scrollbar thumbs */
    for (CscdElement* a = cscdViewElementAt(v, x, y); a; a = parentEl(a)) {
        CscdLayoutBox* b = a->layout;
        if (!b || !b->parent || !b->style || !cscdStyleClipsOverflow(b->style)) continue;
        CscdRect r = cscdViewElementRect(v, a);
        CscdRect pb = cscdRectTranslated(cscdBoxPaddingBox(b), r.x - b->x, r.y - b->y);
        float maxY = cscdMaxf(0.f, b->overflow.h - cscdBoxPaddingBox(b).h);
        if (maxY > 0 && x >= cscdRectRight(pb) - 10 && x <= cscdRectRight(pb) && y >= pb.y && y <= cscdRectBottom(pb)) {
            v->scrollDrag = a;
            v->scrollDragStart = y;
            v->scrollDragOrigin = a->scrollTop;
            return;
        }
    }
    CscdElement* hit = cscdViewElementAt(v, x, y);
    if (viewBlockedByModal(v, hit)) return; /* the page under a modal dialog */
    v->pressed = hit;
    viewSetActive(v, hit);
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &hit);
    CscdEvent ev;
    cscdEventInit(&ev, CSCD_SV("mousedown"));
    ev.x = x; ev.y = y; ev.button = button; ev.buttons = 1 << button;
    ev.detail = clicks;
    v->downClicks = clicks;
    ev.shift = m.shift; ev.ctrl = m.ctrl; ev.alt = m.alt;
    viewDispatch(v, &ev, hit);
    const bool prevented = ev.defaultPrevented;
    cscdEventFini(&ev);
    if (prevented || (button == 0 && viewSplitterDown(v, hit, x, y))) {
        viewUnwatch(v, mark);
        return;
    }
    if (button == 0) {
        /* a drag starts once the pointer moves a few pixels */
        v->dragSource = NULL;
        for (CscdElement* a = hit; a; a = parentEl(a))
            if (cscdSvIEqC(cscdElementAttrC(a, "draggable"), "true") && !cscdElementDisabled(a)) { v->dragSource = a; break; }
        if (v->dragSource && isTextControl(hit)) v->dragSource = NULL;
        v->dragStartX = x;
        v->dragStartY = y;
    }
    CscdElement* f = hit;
    /* data-focus=keyboard: reached by Tab, never by a click, so the keys stay the program's after one */
    while (f && (!isFocusable(f) || cscdSvEqC(cscdElementAttrC(f, "data-focus"), "keyboard"))) f = parentEl(f);
    if (f && cscdElementIs(f, "label")) f = NULL;
    viewWatch(v, &f);
    cscdViewFocus(v, f);
    if (f && isTextControl(f) && f->layout) {
        /* caret from the click position */
        const CscdSv val = cscdStrSv(&f->editValue);
        size_t best = viewTextHit(v, f, x, y);
        if (clicks >= 2) { f->selAnchor = prevWord(val, best); f->caret = nextWord(val, best); }
        else { f->caret = best; if (!m.shift) f->selAnchor = best; }
        if (clicks >= 3) { f->selAnchor = 0; f->caret = val.len; }
        v->selecting = true;
        v->caretTime = viewNow(v);
    }
    if (hit && cscdElementIs(hit, "input") && cscdSvIEqC(cscdElementAttrC(hit, "type"), "range") && !cscdElementDisabled(hit)) {
        v->dragRange = hit;
        cscdViewMouseMove(v, x, y, 1, m);
    }
    viewUnwatch(v, mark);
    v->lastClick = viewNow(v);
    cscdViewInvalidate(v);
}

void cscdViewMouseUp(CscdView* v, float x, float y, int button, CscdModifiers m) {
    v->mouseX = x;
    v->mouseY = y;
    if (viewInspectorMouse(v, 2, x, y)) return;
    v->selecting = false;
    if (v->splitter) { viewSplitterUp(v); viewSetActive(v, NULL); v->pressed = NULL; return; }
    if (v->dragActive) {
        if (!v->dragExternal) viewEndDrag(v, true);
        viewSetActive(v, NULL);
        v->pressed = NULL;
        return;
    }
    v->dragSource = NULL;
    if (viewMenuMouse(v, 2, x, y, button)) { viewSetActive(v, NULL); v->pressed = NULL; return; }
    if (viewBlockedByModal(v, cscdViewElementAt(v, x, y)) && !v->pressed) return;
    if (v->scrollDrag) { v->scrollDrag = NULL; return; }
    if (v->dragRange) {
        viewSendSimple(v, "change", v->dragRange);
        v->dragRange = NULL;
    }
    CscdElement* hit = cscdViewElementAt(v, x, y);
    CscdEvent ev;
    cscdEventInit(&ev, CSCD_SV("mouseup"));
    ev.x = x; ev.y = y; ev.button = button;
    ev.shift = m.shift; ev.ctrl = m.ctrl; ev.alt = m.alt;
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &hit); /* a mouseup listener may remove it */
    viewDispatch(v, &ev, hit);
    cscdEventFini(&ev);
    CscdElement* pressed = v->pressed;
    viewWatch(v, &pressed);
    viewSetActive(v, NULL);
    v->pressed = NULL;
    /* click goes to the nearest common ancestor of press and release targets */
    if (pressed && hit) {
        CscdElement* common = NULL;
        for (CscdElement* a = hit; a && !common; a = parentEl(a))
            for (CscdElement* b = pressed; b; b = parentEl(b)) if (a == b) { common = a; break; }
        if (common) {
            CscdEvent c;
            cscdEventInit(&c, button == 2 ? CSCD_SV("contextmenu") : CSCD_SV("click"));
            c.x = x; c.y = y; c.button = button;
            c.detail = v->downClicks;
            c.shift = m.shift; c.ctrl = m.ctrl; c.alt = m.alt;
            viewWatch(v, &common); /* a click handler may remove it */
            viewDispatch(v, &c, common);
            if (!c.defaultPrevented && button == 0 && common) {
                c.target = common;
                viewDefaultAction(v, &c);
            }
            cscdEventFini(&c);
            if (button == 0 && v->downClicks == 2 && common) {
                CscdEvent d;
                cscdEventInit(&d, CSCD_SV("dblclick"));
                d.x = x; d.y = y; d.detail = 2;
                d.shift = m.shift; d.ctrl = m.ctrl; d.alt = m.alt;
                viewDispatch(v, &d, common);
                cscdEventFini(&d);
            }
        }
    }
    viewUnwatch(v, mark);
    cscdViewInvalidate(v);
}

typedef struct SelectChoice { CscdView* v; CscdElement* e; CscdStr value; } SelectChoice;

static void selectChoiceFree(void* p) {
    SelectChoice* s = (SelectChoice*)p;
    cscdStrFree(&s->value);
    cscdFree(s);
}

static void selectChoose(void* user) {
    SelectChoice* s = (SelectChoice*)user;
    CscdStr cur = cscdElementValue(s->e);
    const bool same = cscdStrEq(&cur, &s->value);
    cscdStrFree(&cur);
    if (same) return;
    cscdElementSetValue(s->e, cscdStrSv(&s->value));
    CscdView* v = s->v;
    CscdElement* e = s->e;
    viewSendSimple(v, "input", e);
    viewSendSimple(v, "change", e);
    cscdViewInvalidate(v);
}

void viewInputAndChange(CscdView* v, CscdElement* e) {
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &e);
    viewSendSimple(v, "input", e);
    if (e) viewSendSimple(v, "change", e);
    viewUnwatch(v, mark);
}

void viewDefaultAction(CscdView* v, CscdEvent* ev) {
    for (CscdElement* e = ev->target; e; e = parentEl(e)) {
        if (cscdElementDisabled(e)) return;
        if (cscdElementIs(e, "input")) {
            CscdSv type = cscdElementAttrC(e, "type");
            if (cscdSvIEqC(type, "checkbox")) {
                cscdElementSetChecked(e, !cscdElementChecked(e));
                viewInputAndChange(v, e);
                return;
            }
            if (cscdSvIEqC(type, "radio")) {
                if (!cscdElementChecked(e)) {
                    cscdElementSetChecked(e, true);
                    viewInputAndChange(v, e);
                }
                return;
            }
            if (cscdSvIEqC(type, "submit")) { viewSubmitForm(v, e); return; }
            return;
        }
        if (cscdElementIs(e, "button")) {
            CscdSv type = cscdElementHasAttributeC(e, "type") ? cscdElementAttrC(e, "type") : CSCD_SV("submit");
            if (cscdSvIEqC(type, "submit")) viewSubmitForm(v, e);
            return;
        }
        if (cscdElementIs(e, "label")) {
            CscdElement* control = NULL;
            if (cscdElementHasAttributeC(e, "for")) control = cscdDocumentGetElementById(v->doc, cscdElementAttrC(e, "for"));
            else control = cscdElementQuerySelector(e, CSCD_SV("input, select, textarea, button"));
            if (control && control != ev->target) {
                if (isTextControl(control)) cscdViewFocus(v, control);
                else {
                    CscdEvent c;
                    cscdEventInit(&c, CSCD_SV("click"));
                    const size_t mark = viewWatchMark(v);
                    viewWatch(v, &control);
                    viewDispatch(v, &c, control);
                    if (control) {
                        c.target = control;
                        viewDefaultAction(v, &c);
                    }
                    viewUnwatch(v, mark);
                    cscdEventFini(&c);
                }
            }
            return;
        }
        if (cscdElementIs(e, "summary")) {
            CscdElement* det = parentEl(e);
            if (det && cscdElementIs(det, "details")) {
                if (cscdElementHasAttributeC(det, "open")) cscdElementRemoveAttribute(det, CSCD_SV("open"));
                else cscdElementSetAttribute(det, CSCD_SV("open"), CSCD_SV(""));
                viewSendSimple(v, "toggle", det);
            }
            return;
        }
        if (cscdElementIs(e, "select")) {
            /* the options in a popup list under the field */
            CscdPtrVec opts = {0};
            cscdElementQuerySelectorAll(e, CSCD_SV("option"), &opts);
            if (!opts.len) return;
            CscdStr val = cscdElementValue(e);
            CscdMenu items = {0};
            for (size_t i = 0; i < opts.len; ++i) {
                CscdElement* o = (CscdElement*)opts.items[i];
                CscdStr text = {0};
                cscdNodeTextContent(&o->node, &text);
                CscdStr ov = cscdElementValue(o);
                SelectChoice* sc = CSCD_NEW(SelectChoice);
                sc->v = v;
                sc->e = e;
                sc->value = ov;
                CscdMenuItem it = cscdMenuItem(cscdSvTrim(cscdStrSv(&text)), selectChoose, sc, CSCD_SV(""));
                it.action.freeUser = selectChoiceFree;
                it.checkable = true;
                it.checked = cscdStrEq(&ov, &val);
                it.enabled = !cscdElementDisabled(o);
                cscdMenuAdd(&items, it);
                cscdStrFree(&text);
            }
            cscdViewCloseMenus(v);
            viewLayoutNow(v);
            CscdRect r = cscdViewElementRect(v, e);
            viewOpenMenu(v, items, r, false, r.w, -1, NULL);
            if (v->menus.len && ((OpenMenu*)v->menus.items[0])->box) {
                cscdElementAddClass(((OpenMenu*)v->menus.items[0])->box, CSCD_SV("cscd-select-menu"));
                for (size_t i = 0; i < opts.len; ++i) {
                    CscdStr ov = cscdElementValue((CscdElement*)opts.items[i]);
                    if (cscdStrEq(&ov, &val)) viewHighlightMenuRow(v, 0, (int)i, false);
                    cscdStrFree(&ov);
                }
            }
            cscdStrFree(&val);
            cscdVecFree(&opts);
            return;
        }
    }
}

void viewSubmitForm(CscdView* v, CscdElement* submitter) {
    CscdElement* form = NULL;
    for (CscdElement* f = submitter; f && !form; f = parentEl(f))
        if (cscdElementIs(f, "form")) form = f;
    if (!form) return;
    /* Enter in a field stands for the form's default button, the first that submits (HTML's implicit submission): a
     * <form method=dialog> then closes with that button's value, not with nothing. */
    const bool isButton = cscdElementIs(submitter, "button") || cscdSvIEqC(cscdElementAttrC(submitter, "type"), "submit");
    if (!isButton) {
        CscdElement* button = cscdElementQuerySelector(form, CSCD_SV("button:not([type=button]):not([type=reset]):not([disabled]), input[type=submit]:not([disabled])"));
        if (button) {
            cscdElementClick(button);
            return;
        }
    }
    CscdEvent s;
    cscdEventInit(&s, CSCD_SV("submit"));
    s.target = submitter;
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &form);
    viewWatch(v, &submitter);
    viewDispatch(v, &s, form);
    const bool prevented = s.defaultPrevented;
    cscdEventFini(&s);
    if (!prevented && form && cscdSvIEqC(cscdElementAttrC(form, "method"), "dialog")) {
        /* <form method=dialog>: its dialog closes with the button's value */
        CscdStr value = cscdStrDup(submitter ? cscdElementAttrC(submitter, "value") : CSCD_SV(""));
        for (CscdElement* d = form; d; d = parentEl(d))
            if (cscdElementIs(d, "dialog")) { cscdViewCloseDialog(v, d, cscdStrSv(&value)); break; }
        cscdStrFree(&value);
    }
    viewUnwatch(v, mark);
}
