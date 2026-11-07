/* Touch: a finger's gestures as the view takes them (taps, drags that scroll or are the page's, flings,
 * pinches, long presses, touch-action) and selecting text by touch (the selection's handles, the edit
 * menu, the magnifier). The platforms hand touches over with cscdViewTouch*. */
#include "view/viewInternal.h"

#include <math.h>

static void fillTouches(CscdEvent* ev, const CscdTouchVec* all, CscdTouchPoint changed) {
    cscdStrSetC(&ev->pointerType, "touch");
    cscdVecCopy(&ev->touches, all);
    ev->changedTouches.len = 0;
    cscdVecPush(&ev->changedTouches, changed);
    ev->x = changed.x;
    ev->y = changed.y;
}

/* ---- touch ----------------------------------------------------------------------------------------- */

static size_t findTouch(const CscdView* v, int id) {
    for (size_t i = 0; i < v->touches.len; ++i) if (v->touches.items[i].id == id) return i;
    return CSCD_NPOS;
}

static void touchPoints(const CscdView* v, CscdTouchVec* all) {
    for (size_t i = 0; i < v->touches.len; ++i) {
        const TouchState* s = &v->touches.items[i];
        CscdTouchPoint p = {s->id, s->x, s->y};
        cscdVecPush(all, p);
    }
}

static void touchEvent(CscdView* v, const char* type, const CscdTouchVec* all, CscdTouchPoint changed, CscdElement* target, bool* prevented) {
    CscdEvent ev;
    cscdEventInit(&ev, cscdSv(type));
    fillTouches(&ev, all, changed);
    viewDispatch(v, &ev, target);
    if (prevented) *prevented = ev.defaultPrevented;
    cscdEventFini(&ev);
}

/* (touch selection, below) */
static bool startHandleDrag(CscdView* v, int id, float x, float y);
static void dragHandle(CscdView* v, float x, float y);
static void endHandleDrag(CscdView* v, bool menu, bool moved);
static void tapHandles(CscdView* v, CscdElement* before, size_t caretBefore, bool collapsedBefore, bool menuBefore);
static bool longPressField(CscdView* v, float x, float y);

/* when the touch being handled happened */
static double touchNow(const CscdView* v) { return viewNow(v) - v->inputAge; }

void viewSetInputAge(CscdView* v, double seconds) { v->inputAge = seconds > 0 && seconds < 1 ? seconds : 0; }

/* the gestures touch-action leaves to the view over an element (its own and its ancestors') */
static unsigned touchActions(const CscdElement* e) {
    unsigned allowed = CSCD_TA_PAN_X | CSCD_TA_PAN_Y | CSCD_TA_PINCH;
    for (; e; e = parentEl(e))
        if (e->style && e->style->touchAction) allowed &= e->style->touchAction;
    return allowed;
}

typedef struct LongPress { CscdView* v; int id; } LongPress;

static void longPress(void* user) {
    LongPress* lp = (LongPress*)user;
    CscdView* v = lp->v;
    v->longPressTimer = 0;
    if (v->touchMode != TOUCH_PENDING || !v->touches.len || v->touches.items[0].id != lp->id) return;
    TouchState s = v->touches.items[0];
    v->touchMode = TOUCH_DONE;
    if (longPressField(v, s.x, s.y)) return; /* (a text field's word, with handles and the edit menu) */
    CscdModifiers none = {0};
    cscdViewMouseMove(v, s.x, s.y, 0, none);
    cscdViewMouseDown(v, s.x, s.y, 2, none, 1);
    cscdViewMouseUp(v, s.x, s.y, 2, none);
}

void cscdViewTouchDown(CscdView* v, int id, float x, float y) {
    /* a finger on a selection handle drags it (the page does not see that touch) */
    if (!v->touches.len && v->handlesFor && startHandleDrag(v, id, x, y)) return;
    if (v->touchMode == TOUCH_HANDLE) return; /* (a second finger while one drags a handle) */
    double t = touchNow(v);
    CscdElement* target = cscdViewElementAt(v, x, y);
    TouchState ts = {id, x, y, x, y, x, y, t, target};
    cscdVecPush(&v->touches, ts);
    cscdViewHideTooltip(v);
    CscdTouchVec all = {0};
    touchPoints(v, &all);
    CscdTouchPoint changed = {id, x, y};
    bool prevented = false;
    touchEvent(v, "touchstart", &all, changed, target, &prevented);
    cscdVecFree(&all);
    if (prevented) v->touchMode = TOUCH_PAGE;
    if (v->touchMode == TOUCH_PAGE) return;
    if (v->touches.len == 1) {
        if (v->momentumTimer) { cscdViewClearTimer(v, v->momentumTimer); v->momentumTimer = 0; }
        v->touchMode = TOUCH_PENDING;
        v->touchVX = v->touchVY = v->panAccX = v->panAccY = 0;
        v->touchLastT = t;
        LongPress* lp = CSCD_NEW(LongPress);
        lp->v = v;
        lp->id = id;
        CscdAction a = {longPress, lp, cscdFree};
        v->longPressTimer = cscdViewSetTimeout(v, a, 550);
    } else if (v->touches.len == 2 && (touchActions(v->touches.items[0].target) & CSCD_TA_PINCH)) {
        if (v->longPressTimer) { cscdViewClearTimer(v, v->longPressTimer); v->longPressTimer = 0; }
        CscdModifiers none = {0};
        if (v->touchMode == TOUCH_MOUSE) cscdViewMouseUp(v, v->touches.items[0].x, v->touches.items[0].y, 0, none);
        v->touchMode = TOUCH_PINCH;
        const TouchState* a = &v->touches.items[0];
        const TouchState* b = &v->touches.items[1];
        v->pinchDist = cscdHypotf(b->x - a->x, b->y - a->y);
        v->pinchX = (a->x + b->x) / 2;
        v->pinchY = (a->y + b->y) / 2;
    }
}

static CscdElement* scrollableFrom(CscdView* v, float x, float y, float dx, float dy) {
    /* (as view.c's: the element under the point that can scroll that way) */
    CscdElement* e = cscdViewElementAt(v, x, y);
    for (CscdElement* a = e; a; a = parentEl(a)) {
        CscdLayoutBox* b = a->layout;
        if (!b || !b->style || !b->parent) continue;
        const CscdComputedStyle* s = b->style;
        CscdRect pb = cscdBoxPaddingBox(b);
        float maxY = cscdMaxf(0.f, b->overflow.h - pb.h), maxX = cscdMaxf(0.f, b->overflow.w - pb.w);
        bool sy = (s->overflowY == CSCD_OVERFLOW_AUTO || s->overflowY == CSCD_OVERFLOW_SCROLL) && maxY > 0.5f;
        bool sx = (s->overflowX == CSCD_OVERFLOW_AUTO || s->overflowX == CSCD_OVERFLOW_SCROLL) && maxX > 0.5f;
        if (cscdElementIs(a, "textarea")) {
            viewTextAreaRange(v, a, &maxX, &maxY);
            sy = maxY > 0.5f;
            sx = maxX > 0.5f;
        }
        if ((sy && dy != 0 && ((dy > 0 && a->scrollTop < maxY) || (dy < 0 && a->scrollTop > 0))) ||
            (sx && dx != 0 && ((dx > 0 && a->scrollLeft < maxX) || (dx < 0 && a->scrollLeft > 0))))
            return a;
    }
    return NULL;
}

static void setLast(CscdView* v, int id, float x, float y) {
    size_t i = findTouch(v, id);
    if (i != CSCD_NPOS) {
        v->touches.items[i].lastX = x;
        v->touches.items[i].lastY = y;
    }
}

void cscdViewTouchMove(CscdView* v, int id, float x, float y) {
    size_t i = findTouch(v, id);
    if (i == CSCD_NPOS) return;
    v->touches.items[i].x = x;
    v->touches.items[i].y = y;
    if (v->touchMode == TOUCH_HANDLE) {
        const TouchState* s = &v->touches.items[i];
        const float tx = x - s->startX, ty = y - s->startY;
        if (!v->handleMoved && tx * tx + ty * ty < 64) return; /* (a tap on the handle, so far) */
        v->handleMoved = true;
        dragHandle(v, x, y);
        return;
    }
    CscdTouchVec all = {0};
    touchPoints(v, &all);
    CscdTouchPoint changed = {id, x, y};
    touchEvent(v, "touchmove", &all, changed, v->touches.items[i].target, NULL);
    cscdVecFree(&all);
    i = findTouch(v, id);
    if (i == CSCD_NPOS || v->touchMode == TOUCH_PAGE || v->touchMode == TOUCH_DONE) {
        setLast(v, id, x, y);
        return;
    }
    double t = touchNow(v);
    TouchState* it = &v->touches.items[i];
    float dx = x - it->lastX, dy = y - it->lastY;
    CscdModifiers none = {0};
    if (v->touchMode == TOUCH_PENDING) {
        float tx = x - it->startX, ty = y - it->startY;
        if (tx * tx + ty * ty < 64) return;
        if (v->longPressTimer) { cscdViewClearTimer(v, v->longPressTimer); v->longPressTimer = 0; }
        it = &v->touches.items[i];
        /* a slider or a splitter under the finger takes a drag along it; the page scrolls with the rest */
        const bool across = fabsf(tx) >= fabsf(ty);
        bool control = !(touchActions(it->target) & (across ? CSCD_TA_PAN_X : CSCD_TA_PAN_Y)); /* (touch-action keeps that pan from the view) */
        for (CscdElement* a = it->target; a && !control; a = parentEl(a)) {
            if (cscdElementHasAttributeC(a, "splitter")) control = true;
            else if (across && cscdElementIs(a, "input") && cscdSvIEqC(cscdElementAttrC(a, "type"), "range") && !cscdElementDisabled(a)) control = true;
        }
        CscdElement* sc = control ? NULL : scrollableFrom(v, it->startX, it->startY, -tx, -ty);
        bool doc = false;
        if (!sc && !control && !across) { /* (the page scrolls up and down: a drag mostly sideways is not a scroll of it) */
            float maxY = cscdMaxf(0.f, cscdViewContentHeight(v) - v->vh);
            doc = v->root && v->root->style && v->root->style->overflowY != CSCD_OVERFLOW_HIDDEN &&
                  ((ty < 0 && v->scrollY < maxY) || (ty > 0 && v->scrollY > 0));
        }
        if (sc || doc) {
            v->touchMode = TOUCH_PAN;
            v->touchScroll = sc;
            v->touchScrollDoc = doc;
            dx = tx;
            dy = ty;
        } else {
            v->touchMode = TOUCH_MOUSE;
            float sx = it->startX, sy = it->startY;
            cscdViewMouseMove(v, sx, sy, 0, none);
            cscdViewMouseDown(v, sx, sy, 0, none, 1);
            cscdViewMouseMove(v, x, y, 1, none);
            setLast(v, id, x, y);
            return;
        }
    }
    switch (v->touchMode) {
    case TOUCH_PAN: {
        if (!v->touchScroll && !v->touchScrollDoc) break;
        viewScrollBy(v, v->touchScroll, -dx, -dy);
        /* the velocity over at least 8 ms: moves that come together (a batch from the system) do not
         * divide by an instant */
        v->panAccX += -dx;
        v->panAccY += -dy;
        const float dt = (float)(t - v->touchLastT);
        if (dt >= 0.008f) {
            const float k = dt > 0.05f ? 1.f : 0.7f; /* (after a pause, the velocity starts over) */
            v->touchVX = k * (v->panAccX / dt) + (1 - k) * v->touchVX;
            v->touchVY = k * (v->panAccY / dt) + (1 - k) * v->touchVY;
            v->panAccX = v->panAccY = 0;
            v->touchLastT = t;
        }
        break;
    }
    case TOUCH_MOUSE: cscdViewMouseMove(v, x, y, 1, none); break;
    case TOUCH_PINCH:
        if (v->touches.len >= 2) {
            const TouchState a = v->touches.items[0], b = v->touches.items[1];
            float d = cscdHypotf(b.x - a.x, b.y - a.y);
            float cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
            v->touchWheel = true;
            if (v->pinchDist > 1 && fabsf(d / v->pinchDist - 1) > 0.01f) {
                CscdModifiers m = {0};
                m.ctrl = true;
                cscdViewWheel(v, cx, cy, 0, -100.f * cscdLogf(d / v->pinchDist) / cscdLogf(1.25f), m);
                v->pinchDist = d;
            }
            if (cx != v->pinchX || cy != v->pinchY) cscdViewWheel(v, cx, cy, -(cx - v->pinchX), -(cy - v->pinchY), none);
            v->touchWheel = false;
            v->pinchX = cx;
            v->pinchY = cy;
        }
        break;
    default: break;
    }
    setLast(v, id, x, y);
}

static void momentum(void* user) {
    CscdView* v = (CscdView*)user;
    /* by the time that passed (frames come at the screen's rate, or late): the same glide on any screen */
    const double now = viewNow(v);
    const float dt = (float)cscdClampf((float)(now - v->momentumT), 0.f, 0.05f);
    v->momentumT = now;
    if ((!v->touchScroll && !v->touchScrollDoc) || cscdHypotf(v->touchVX, v->touchVY) < 20) {
        cscdViewClearTimer(v, v->momentumTimer);
        v->momentumTimer = 0;
        return;
    }
    viewScrollBy(v, v->touchScroll, v->touchVX * dt, v->touchVY * dt);
    const float decay = powf(0.95f, dt / 0.016f);
    v->touchVX *= decay;
    v->touchVY *= decay;
}

void cscdViewTouchUp(CscdView* v, int id, float x, float y) {
    size_t i = findTouch(v, id);
    if (i == CSCD_NPOS) return;
    if (v->touchMode == TOUCH_HANDLE) {
        cscdVecRemove(&v->touches, i);
        if (!v->touches.len) v->touchMode = TOUCH_NONE;
        endHandleDrag(v, true, v->handleMoved);
        return;
    }
    CscdElement* target = v->touches.items[i].target;
    cscdVecRemove(&v->touches, i);
    CscdTouchVec all = {0};
    touchPoints(v, &all);
    CscdTouchPoint changed = {id, x, y};
    touchEvent(v, "touchend", &all, changed, target, NULL);
    cscdVecFree(&all);
    if (v->longPressTimer) { cscdViewClearTimer(v, v->longPressTimer); v->longPressTimer = 0; }
    double t = touchNow(v);
    CscdModifiers none = {0};
    switch (v->touchMode) {
    case TOUCH_PENDING: {
        v->tapClicks = t - v->lastTap < 0.35 ? v->tapClicks + 1 : 1;
        v->lastTap = t;
        CscdElement* before = v->doc->focused;
        const size_t caretBefore = before ? before->caret : 0;
        const bool collapsedBefore = before && before->caret == before->selAnchor;
        const bool menuBefore = v->editMenuShown, menusOpen = v->menus.len != 0;
        const size_t mark = viewWatchMark(v);
        viewWatch(v, &before);
        cscdViewMouseMove(v, x, y, 0, none);
        cscdViewMouseDown(v, x, y, 0, none, v->tapClicks);
        cscdViewMouseUp(v, x, y, 0, none);
        if (!menusOpen) tapHandles(v, before, caretBefore, collapsedBefore, menuBefore); /* (a tap on a menu chose from it) */
        viewUnwatch(v, mark);
        break;
    }
    case TOUCH_PAN:
        if (cscdHypotf(v->touchVX, v->touchVY) > 150 && t - v->touchLastT < 0.1) {
            v->momentumT = t;
            v->momentumTimer = cscdViewSetInterval(v, cscdAction(momentum, v), 8);
        }
        break;
    case TOUCH_MOUSE: cscdViewMouseUp(v, x, y, 0, none); break;
    default: break;
    }
    if (!v->touches.len) v->touchMode = TOUCH_NONE;
    else if (v->touchMode == TOUCH_PINCH) v->touchMode = TOUCH_DONE;
}

void cscdViewTouchCancel(CscdView* v) {
    if (v->longPressTimer) { cscdViewClearTimer(v, v->longPressTimer); v->longPressTimer = 0; }
    if (v->touchMode == TOUCH_HANDLE) {
        v->touches.len = 0;
        v->touchMode = TOUCH_NONE;
        endHandleDrag(v, false, v->handleMoved);
        return;
    }
    CscdModifiers none = {0};
    if (v->touchMode == TOUCH_MOUSE && v->touches.len) cscdViewMouseUp(v, v->touches.items[0].x, v->touches.items[0].y, 0, none);
    CscdTouchVec noTouches = {0};
    for (size_t i = 0; i < v->touches.len; ++i) {
        const TouchState s = v->touches.items[i];
        CscdTouchPoint changed = {s.id, s.x, s.y};
        touchEvent(v, "touchcancel", &noTouches, changed, s.target, NULL);
    }
    v->touches.len = 0;
    v->touchMode = TOUCH_NONE;
}

/* ---- touch selection: handles and the edit menu ----------------------------------------------------- */

#define HANDLE_RADIUS 7.f
#define HANDLE_REACH 22.f /* (a finger's width around the knob) */

static bool handlesValid(const CscdView* v) {
    const CscdElement* f = v->handlesFor;
    return f && f == v->doc->focused && isTextControl(f) && f->valueDirty && f->layout && f->style;
}

/* the knob of a handle, under the foot of the caret at pos (false: scrolled out of the field) */
static bool handleKnob(CscdView* v, CscdElement* f, size_t pos, CscdRect* caret, CscdPoint* knob) {
    size_t line;
    *caret = viewFieldPosRect(v, f, pos, &line);
    const CscdRect r = cscdViewElementRect(v, f);
    const float footY = cscdRectBottom(*caret);
    if (caret->x < r.x - 1 || caret->x > cscdRectRight(r) + 1 || footY < r.y || caret->y > cscdRectBottom(r)) return false;
    *knob = cscdPoint(caret->x, footY + HANDLE_RADIUS + 1);
    return true;
}

/* the handle a finger at (x, y) takes: 1 the selection's start, 2 its end, 3 the caret's; 0 none */
static int handleAt(CscdView* v, float x, float y) {
    if (!handlesValid(v)) return 0;
    CscdElement* f = v->handlesFor;
    const size_t a = cscdMinz(f->selAnchor, f->caret), z = cscdMaxz(f->selAnchor, f->caret);
    int best = 0;
    float bestD = HANDLE_REACH;
    for (int k = 0; k < (a == z ? 1 : 2); ++k) {
        CscdRect caret;
        CscdPoint p;
        if (!handleKnob(v, f, k ? z : a, &caret, &p)) continue;
        if (y <= caret.y + caret.h / 2) continue; /* (above, the finger is on the text) */
        const float d = cscdHypotf(x - p.x, y - p.y);
        if (d < bestD) { best = a == z ? 3 : k + 1; bestD = d; }
    }
    return best;
}

/* where the menu goes: around the selection, or at the caret (clipped to the field's box) */
static CscdRect selectionBox(CscdView* v, CscdElement* f) {
    const size_t a = cscdMinz(f->selAnchor, f->caret), z = cscdMaxz(f->selAnchor, f->caret);
    size_t la, lz;
    const CscdRect ra = viewFieldPosRect(v, f, a, &la), rz = viewFieldPosRect(v, f, z, &lz);
    const float x0 = cscdMinf(ra.x, rz.x), y0 = cscdMinf(ra.y, rz.y);
    CscdRect box = cscdRect(x0, y0, cscdMaxf(cscdRectRight(ra), cscdRectRight(rz)) - x0, cscdMaxf(cscdRectBottom(ra), cscdRectBottom(rz)) - y0);
    const CscdRect r = cscdViewElementRect(v, f);
    if (la != lz) box = cscdRect(r.x, box.y, r.w, box.h); /* (several lines: the field's width) */
    const CscdRect clipped = cscdRectIntersect(box, r);
    return cscdRectEmpty(clipped) ? box : clipped;
}

unsigned cscdViewEditActions(CscdView* v) {
    CscdElement* f = viewFocusedField(v);
    if (!f) return 0;
    const bool ro = cscdElementHasAttributeC(f, "readonly") || cscdElementDisabled(f);
    const bool password = cscdElementIs(f, "input") && cscdSvIEqC(cscdElementAttrC(f, "type"), "password");
    const size_t a = cscdMinz(f->selAnchor, f->caret), z = cscdMaxz(f->selAnchor, f->caret), len = f->editValue.len;
    unsigned bits = 0;
    if (z > a && !password && v->setClipboard) bits |= CSCD_EDIT_BIT(CSCD_EDIT_COPY) | (ro ? 0 : CSCD_EDIT_BIT(CSCD_EDIT_CUT));
    if (!ro && v->getClipboard) bits |= CSCD_EDIT_BIT(CSCD_EDIT_PASTE);
    if (len && (a > 0 || z < len)) bits |= CSCD_EDIT_BIT(CSCD_EDIT_SELECT_ALL);
    return bits;
}

static void menuAction(CscdView* v, CscdEditAction a) { cscdViewEditAction(v, a); }
static void menuCut(void* v) { menuAction((CscdView*)v, CSCD_EDIT_CUT); }
static void menuCopy(void* v) { menuAction((CscdView*)v, CSCD_EDIT_COPY); }
static void menuPaste(void* v) { menuAction((CscdView*)v, CSCD_EDIT_PASTE); }
static void menuSelectAll(void* v) { menuAction((CscdView*)v, CSCD_EDIT_SELECT_ALL); }

static void hideEditMenu(CscdView* v) {
    if (!v->editMenuShown) return;
    v->editMenuShown = false;
    if (v->onEditMenu) v->onEditMenu(v->onEditMenuUser, false, cscdRect(0, 0, 0, 0), 0);
    else cscdViewCloseMenus(v); /* (the view's own) */
}

static void showEditMenu(CscdView* v) {
    CscdElement* f = v->handlesFor;
    const unsigned actions = cscdViewEditActions(v);
    if (!handlesValid(v) || !actions) { hideEditMenu(v); return; }
    const CscdRect box = selectionBox(v, f);
    v->editMenuShown = true;
    v->editMenuRect = box;
    v->editMenuActions = actions;
    if (v->onEditMenu) {
        v->onEditMenu(v->onEditMenuUser, true, box, actions);
        return;
    }
    /* no system menu: the view's own, under the selection */
    static const struct { CscdEditAction a; const char* label; CscdActionFn fn; } kItems[] = {
        {CSCD_EDIT_CUT, "Cut", menuCut}, {CSCD_EDIT_COPY, "Copy", menuCopy}, {CSCD_EDIT_PASTE, "Paste", menuPaste},
        {CSCD_EDIT_SELECT_ALL, "Select All", menuSelectAll}};
    CscdMenu items = {0};
    for (size_t i = 0; i < CSCD_COUNT(kItems); ++i)
        if (actions & CSCD_EDIT_BIT(kItems[i].a)) cscdMenuAdd(&items, cscdMenuItem(cscdSv(kItems[i].label), kItems[i].fn, v, CSCD_SV("")));
    cscdViewCloseMenus(v);
    viewOpenMenu(v, items, cscdRect(box.x, box.y, box.w, box.h + 2 * HANDLE_RADIUS + 2), false, 0, -1, NULL);
}

static void hideLoneCaret(void* user) {
    CscdView* v = (CscdView*)user;
    v->handleTimer = 0;
    CscdElement* f = v->handlesFor;
    if (f && !v->handleDragged && !v->editMenuShown && f->selAnchor == f->caret) cscdViewHideSelectionHandles(v);
}

void viewShowHandles(CscdView* v, CscdElement* f, bool menu) {
    if (!f || f != v->doc->focused || !isTextControl(f)) return;
    v->handlesFor = f;
    if (v->handleTimer) { cscdViewClearTimer(v, v->handleTimer); v->handleTimer = 0; }
    if (menu) showEditMenu(v);
    else hideEditMenu(v);
    if (f->selAnchor == f->caret && !v->editMenuShown) v->handleTimer = cscdViewSetTimeout(v, cscdAction(hideLoneCaret, v), 4000);
    cscdViewInvalidate(v);
}

void cscdViewHideSelectionHandles(CscdView* v) {
    if (v->handleTimer) { cscdViewClearTimer(v, v->handleTimer); v->handleTimer = 0; }
    if (v->handleDragged && v->onMagnifier) v->onMagnifier(v->onMagnifierUser, false, 0, 0);
    v->handleDragged = 0;
    hideEditMenu(v);
    if (v->handlesFor) {
        v->handlesFor = NULL;
        cscdViewInvalidate(v);
    }
}

bool cscdViewSelectionHandlesShown(const CscdView* v) { return handlesValid(v); }

void viewHandlesCheck(CscdView* v) {
    if (!v->handlesFor && !v->editMenuShown) return;
    if (v->editMenuShown && !v->onEditMenu && !v->menus.len) v->editMenuShown = false; /* (the view's own menu closed itself) */
    if (!handlesValid(v)) { cscdViewHideSelectionHandles(v); return; }
    /* the system's menu follows the selection (scrolling, the keyboard coming up) */
    if (v->editMenuShown && v->onEditMenu && !v->handleDragged) {
        const CscdRect box = selectionBox(v, v->handlesFor);
        const unsigned actions = cscdViewEditActions(v);
        const CscdRect o = v->editMenuRect;
        if (actions != v->editMenuActions || fabsf(box.x - o.x) > 0.5f || fabsf(box.y - o.y) > 0.5f || fabsf(box.w - o.w) > 0.5f ||
            fabsf(box.h - o.h) > 0.5f)
            showEditMenu(v);
    }
}

void viewPaintHandles(CscdView* v, CscdCanvas* c) {
    if (!handlesValid(v)) return;
    CscdElement* f = v->handlesFor;
    CscdColor color = cscdRgb(51, 144, 255);
    const CscdComputedStyle* sel = cscdStyleEnginePseudoStyle(v->styles, f, CSCD_SV("selection"));
    if (sel && sel->backgroundColor.a) color = sel->backgroundColor;
    color.a = 255;
    const size_t a = cscdMinz(f->selAnchor, f->caret), z = cscdMaxz(f->selAnchor, f->caret);
    for (int k = 0; k < (a == z ? 1 : 2); ++k) {
        CscdRect caret;
        CscdPoint p;
        if (!handleKnob(v, f, k ? z : a, &caret, &p)) continue;
        /* a selection's ends get a stem the line's height; the caret has its own */
        if (a != z) cscdCanvasFillRect(c, cscdRect(caret.x - 1, caret.y, 2, caret.h + 1), color);
        cscdCanvasFillCircle(c, p, HANDLE_RADIUS, color);
    }
}

/* a dragged handle moves its end of the selection (the other stays: they never cross) */
static void dragHandle(CscdView* v, float x, float y) {
    CscdElement* f = v->handlesFor;
    if (!handlesValid(v)) return;
    const CscdSv val = cscdStrSv(&f->editValue);
    size_t line;
    const CscdRect now = viewFieldPosRect(v, f, f->caret, &line);
    const float footX = x - v->handleGrabX, footY = y - v->handleGrabY;
    size_t at = viewTextHit(v, f, footX, footY - now.h / 2);
    const size_t a = cscdMinz(f->selAnchor, f->caret), z = cscdMaxz(f->selAnchor, f->caret);
    if (v->handleDragged == 1) {
        if (at >= z) at = z ? prevChar(val, z) : 0;
        f->selAnchor = z;
        f->caret = at;
    } else if (v->handleDragged == 2) {
        if (at <= a) at = nextChar(val, a);
        f->selAnchor = a;
        f->caret = at;
    } else f->selAnchor = f->caret = at;
    v->caretTime = viewNow(v);
    viewEnsureCaretVisible(v, f);
    cscdViewInvalidate(v);
    if (v->onMagnifier) {
        const CscdRect r = viewFieldPosRect(v, f, f->caret, &line);
        v->onMagnifier(v->onMagnifierUser, true, r.x, r.y + r.h / 2);
    }
}

static bool startHandleDrag(CscdView* v, int id, float x, float y) {
    const int which = handleAt(v, x, y);
    if (!which) return false;
    CscdElement* f = v->handlesFor;
    const size_t a = cscdMinz(f->selAnchor, f->caret), z = cscdMaxz(f->selAnchor, f->caret);
    CscdRect caret;
    CscdPoint knob;
    handleKnob(v, f, which == 2 ? z : a, &caret, &knob);
    /* the dragged end becomes the caret: it is the one kept in sight */
    if (which == 1) { f->selAnchor = z; f->caret = a; }
    else if (which == 2) { f->selAnchor = a; f->caret = z; }
    v->handleDragged = which;
    v->handleGrabX = x - caret.x;
    v->handleGrabY = y - cscdRectBottom(caret);
    v->handleMenuBefore = v->editMenuShown;
    v->handleMoved = false;
    if (v->handleTimer) { cscdViewClearTimer(v, v->handleTimer); v->handleTimer = 0; }
    hideEditMenu(v);
    TouchState ts = {id, x, y, x, y, x, y, viewNow(v), f};
    cscdVecPush(&v->touches, ts);
    v->touchMode = TOUCH_HANDLE;
    return true; /* (the selection moves once the finger does) */
}

/* a handle let go: the menu for a selection; a handle tapped without dragging shows the menu, or
 * hides it when it showed */
static void endHandleDrag(CscdView* v, bool menu, bool moved) {
    CscdElement* f = v->handlesFor;
    if (v->onMagnifier && moved) v->onMagnifier(v->onMagnifierUser, false, 0, 0);
    v->handleDragged = 0;
    if (!handlesValid(v)) return;
    viewShowHandles(v, f, menu && (moved ? f->selAnchor != f->caret : !v->handleMenuBefore));
}

/* after a tap: a double tap's word gets handles and the menu; a tap on the caret where it already
 * was asks for the menu (a second time, hides it); any other tap in a field shows the caret's handle */
static void tapHandles(CscdView* v, CscdElement* before, size_t caretBefore, bool collapsedBefore, bool menuBefore) {
    CscdElement* f = viewFocusedField(v);
    if (!f) { cscdViewHideSelectionHandles(v); return; }
    if (v->tapClicks >= 2 && f->selAnchor != f->caret) {
        /* the word, as a long press selects it: without the space a double click takes after it */
        const CscdSv val = cscdStrSv(&f->editValue);
        if (v->tapClicks == 2 && f->caret > f->selAnchor)
            while (f->caret > f->selAnchor + 1 && f->caret <= val.len && val.ptr[f->caret - 1] == ' ') --f->caret;
        viewShowHandles(v, f, true);
        return;
    }
    const bool again = f == before && collapsedBefore && f->selAnchor == f->caret && f->caret == caretBefore;
    viewShowHandles(v, f, again && !menuBefore);
}

/* a long press on a text field: the page's contextmenu first; unless it takes it, the word there */
static bool longPressField(CscdView* v, float x, float y) {
    CscdElement* f = cscdViewElementAt(v, x, y);
    while (f && !isTextControl(f)) f = parentEl(f);
    if (!f || cscdElementDisabled(f)) return false;
    CscdEvent c;
    cscdEventInit(&c, CSCD_SV("contextmenu"));
    c.x = x;
    c.y = y;
    c.button = 2;
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &f);
    viewDispatch(v, &c, f);
    const bool prevented = c.defaultPrevented;
    cscdEventFini(&c);
    if (f && !prevented) cscdViewFocus(v, f);
    viewUnwatch(v, mark);
    if (!f || prevented || v->doc->focused != f || !f->layout) return true;
    const CscdSv val = cscdStrSv(&f->editValue);
    const size_t at = viewTextHit(v, f, x, y);
    f->selAnchor = prevWord(val, at);
    f->caret = nextWord(val, at);
    while (f->caret > f->selAnchor && val.ptr[f->caret - 1] == ' ') --f->caret; /* (the word, without the space after it) */
    v->caretTime = viewNow(v);
    viewShowHandles(v, f, true);
    return true;
}

bool cscdViewEditAction(CscdView* v, CscdEditAction act) {
    if (!(cscdViewEditActions(v) & CSCD_EDIT_BIT(act))) return false;
    CscdElement* f = viewFocusedField(v);
    const size_t a = cscdMinz(f->selAnchor, f->caret), z = cscdMaxz(f->selAnchor, f->caret);
    switch (act) {
    case CSCD_EDIT_CUT: case CSCD_EDIT_COPY:
        v->setClipboard(v->clipboardUser, cscdSvSub(cscdStrSv(&f->editValue), a, z - a));
        if (act == CSCD_EDIT_CUT) {
            cscdViewHideSelectionHandles(v);
            viewEditInput(v, f, CSCD_SV(""), 0, false);
        } else hideEditMenu(v);
        break;
    case CSCD_EDIT_PASTE: {
        CscdStr text = {0};
        v->getClipboard(v->clipboardUser, &text);
        cscdViewHideSelectionHandles(v);
        if (text.len) viewEditInput(v, f, cscdStrSv(&text), 0, false);
        cscdStrFree(&text);
        break;
    }
    case CSCD_EDIT_SELECT_ALL:
        f->selAnchor = 0;
        f->caret = f->editValue.len;
        v->caretTime = viewNow(v);
        if (v->handlesFor == f) viewShowHandles(v, f, true);
        cscdViewInvalidate(v);
        break;
    }
    return true;
}

void cscdViewOnEditMenu(CscdView* v, CscdEditMenuFn fn, void* user) {
    hideEditMenu(v);
    v->onEditMenu = fn;
    v->onEditMenuUser = user;
}

void cscdViewOnMagnifier(CscdView* v, CscdMagnifierFn fn, void* user) {
    v->onMagnifier = fn;
    v->onMagnifierUser = user;
}
