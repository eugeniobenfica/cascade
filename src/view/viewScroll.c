/* Scrolling: the page and boxes that scroll, by the wheel, by code or into view, smoothly or at once,
 * and the scroll events. */
#include "style/styleInternal.h"
#include "view/viewInternal.h"

#include <stdio.h>
#include <stdlib.h>

static void smoothScrollBy(CscdView* v, CscdElement* e, float dx, float dy) {
    if (!viewOn(v, CSCD_FEATURE_SMOOTH_SCROLL)) { viewScrollBy(v, e, dx, dy); return; }
    /* a wheel step glides there over a few frames (as browsers scroll) instead of jumping */
    if (!v->smooth.len) v->smoothTime = viewNow(v);
    v->dirty = true;
    viewNotifyInvalidate(v);
    for (size_t i = 0; i < v->smooth.len; ++i)
        if (v->smooth.items[i].e == e) {
            v->smooth.items[i].dx += dx;
            v->smooth.items[i].dy += dy;
            return;
        }
    SmoothScroll s = {e, dx, dy};
    cscdVecPush(&v->smooth, s);
}

static size_t findSmooth(const CscdView* v, const CscdElement* e) {
    for (size_t i = 0; i < v->smooth.len; ++i) if (v->smooth.items[i].e == e) return i;
    return CSCD_NPOS;
}

void viewAdvanceScrolls(CscdView* v, double t) {
    if (!v->smooth.len) return;
    const double dt = t - v->smoothTime < 0 ? 0 : t - v->smoothTime > 0.1 ? 0.1 : t - v->smoothTime;
    v->smoothTime = t;
    const float k = (float)(1 - exp(-dt / 0.045)); /* most of the way in about 0.1 s */
    for (size_t i = 0; i < v->smooth.len;) {
        SmoothScroll* s = &v->smooth.items[i];
        const bool last = fabsf(s->dx) < 0.5f && fabsf(s->dy) < 0.5f;
        const float sx = last ? s->dx : s->dx * k, sy = last ? s->dy : s->dy * k;
        CscdElement* e = s->e; /* (NULL: the page) */
        const float x0 = e ? e->scrollLeft : v->scrollX, y0 = e ? e->scrollTop : v->scrollY;
        s->dx -= sx;
        s->dy -= sy;
        const bool page = !e;
        const size_t mark = viewWatchMark(v);
        viewWatch(v, &e);
        viewScrollBy(v, e, sx, sy); /* (its scroll event may change the page: s is not used after) */
        viewUnwatch(v, mark);
        if (!page && !e) continue; /* (a listener removed it, and its scroll with it) */
        const bool moved = e ? e->scrollLeft != x0 || e->scrollTop != y0 : v->scrollX != x0 || v->scrollY != y0;
        size_t at = findSmooth(v, e);
        if (at == CSCD_NPOS) continue; /* removed meanwhile */
        if (last || !moved) cscdVecRemove(&v->smooth, at); /* there, or at an end */
        else ++at;
        i = at;
    }
}

static CscdElement* scrollableAt(CscdView* v, float x, float y, float dx, float dy, bool* contained) {
    CscdElement* e = cscdViewElementAt(v, x, y);
    *contained = false;
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
        if ((sx || sy) && s->overscroll) { /* (at its end: overscroll-behavior keeps the scroll from going further out) */
            *contained = true;
            return NULL;
        }
    }
    return NULL;
}

void cscdViewScrollTo(CscdView* v, float x, float y) {
    float maxY = cscdMaxf(0.f, cscdViewContentHeight(v) - v->vh);
    float maxX = v->root ? cscdMaxf(0.f, cscdRectRight(v->root->overflow) - v->vw) : 0;
    float nx = cscdClampf(x, 0.f, maxX), ny = cscdClampf(y, 0.f, maxY);
    if (nx == v->scrollX && ny == v->scrollY) return;
    v->scrollX = nx;
    v->scrollY = ny;
    cscdViewInvalidate(v);
}

void cscdViewScrollIntoView(CscdView* v, CscdElement* e) {
    if (!e || !e->layout) return;
    CscdRect r = cscdBoxBorderBox(e->layout);
    float to = v->scrollY;
    if (r.y < v->scrollY) to = r.y;
    else if (cscdRectBottom(r) > v->scrollY + v->vh) to = cscdRectBottom(r) - v->vh;
    if (to == v->scrollY) return;
    if (v->root && v->root->style && v->root->style->smoothScroll) smoothScrollBy(v, NULL, 0, to - v->scrollY); /* (scroll-behavior: smooth) */
    else cscdViewScrollTo(v, v->scrollX, to);
}

void cscdViewWheel(CscdView* v, float x, float y, float dx, float dy, CscdModifiers m) {
    cscdViewHideTooltip(v);
    if (v->menus.len) {
        /* a long menu scrolls; the rest of the page waits until it closes */
        for (size_t i = v->menus.len; i-- > 0;) {
            OpenMenu* om = (OpenMenu*)v->menus.items[i];
            if (om->box && cscdRectContains(cscdViewElementRect(v, om->box), x, y)) { viewScrollBy(v, om->box, dx, dy); break; }
        }
        return;
    }
    if (viewBlockedByModal(v, cscdViewElementAt(v, x, y))) return;
    CscdEvent ev;
    cscdEventInit(&ev, CSCD_SV("wheel"));
    ev.x = x; ev.y = y; ev.deltaX = dx; ev.deltaY = dy;
    if (v->touchWheel) cscdStrSetC(&ev.pointerType, "touch");
    ev.shift = m.shift; ev.ctrl = m.ctrl; ev.alt = m.alt;
    viewDispatch(v, &ev, cscdViewElementAt(v, x, y));
    const bool prevented = ev.defaultPrevented;
    cscdEventFini(&ev);
    if (prevented) return;
    if (m.shift && dx == 0) { dx = dy; dy = 0; }
    bool contained;
    CscdElement* s = scrollableAt(v, x, y, dx, dy, &contained);
    if (s) {
        smoothScrollBy(v, s, dx, dy);
        return;
    }
    if (contained) return;
    cscdViewScrollTo(v, v->scrollX + dx, v->scrollY + dy);
    if (cscdDocumentBody(v->doc)) viewSendSimple(v, "scroll", cscdDocumentBody(v->doc));
}

void viewScrolled(CscdView* v, CscdElement* e) {
    /* what it contains moved inside its box: that box is painted again, and the hit areas found again */
    const CscdRect r = viewPaintArea(v, e);
    if (cscdRectEmpty(r)) {
        cscdViewInvalidate(v);
        return;
    }
    viewAddDamage(v, cscdRect(r.x - 1, r.y - 1, r.w + 2, r.h + 2));
    v->hitsStale = true;
    v->dirty = true;
    viewNotifyInvalidate(v);
}

void viewScrollBy(CscdView* v, CscdElement* e, float dx, float dy) {
    if (!e) {
        cscdViewScrollTo(v, v->scrollX + dx, v->scrollY + dy);
        return;
    }
    CscdLayoutBox* b = e->layout;
    if (!b) return;
    float maxX, maxY;
    if (cscdElementIs(e, "textarea")) viewTextAreaRange(v, e, &maxX, &maxY);
    else {
        CscdRect pb = cscdBoxPaddingBox(b);
        maxY = cscdMaxf(0.f, b->overflow.h - pb.h);
        maxX = cscdMaxf(0.f, b->overflow.w - pb.w);
    }
    float ny = cscdClampf(e->scrollTop + dy, 0.f, maxY), nx = cscdClampf(e->scrollLeft + dx, 0.f, maxX);
    if (ny == e->scrollTop && nx == e->scrollLeft) return;
    e->scrollTop = ny;
    e->scrollLeft = nx;
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &e);
    viewSendSimple(v, "scroll", e);
    if (e) viewScrolled(v, e);
    viewUnwatch(v, mark);
}
