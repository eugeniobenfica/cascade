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
        float maxY = cscdMaxf(0.f, (float)b->overflow.h - pb.h), maxX = cscdMaxf(0.f, (float)b->overflow.w - pb.w);
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

/* (whether what is drawn between layout places x0..x1, y0..y1 lands on device pixels the same way however far it is
 * scrolled: the canvas's floats hold its device coordinates, the page's places times the scale, exactly below 2^17 px;
 * further down a long page they round, and a frame moved by whole pixels is not the one painted there) */
static bool exactOnDevice(const CscdView* v, double x0, double y0, double x1, double y1) {
    const double lim = 131072.0 / v->dpr;
    return x0 > -lim && y0 > -lim && x1 < lim && y1 < lim;
}

void cscdViewScrollTo(CscdView* v, float x, float y) {
    float maxY = cscdMaxf(0.f, cscdViewContentHeight(v) - v->vh);
    float maxX = v->root ? cscdMaxf(0.f, (float)cscdBoxRectRight(v->root->overflow) - v->vw) : 0;
    float nx = cscdClampf(x, 0.f, maxX), ny = cscdClampf(y, 0.f, maxY);
    if (nx == v->scrollX && ny == v->scrollY) return;
    /* Moved by whole device pixels, a page that moves whole: the last frame is moved, and only what comes into view
     * (and the scrollbars) is painted */
    const float dpr = v->dpr, ox = v->scrollX * dpr, oy = v->scrollY * dpr, px = nx * dpr, py = ny * dpr;
    const int sx = (int)lroundf(ox - px), sy = (int)lroundf(oy - py);
    const bool whole = ox == roundf(ox) && oy == roundf(oy) && px == roundf(px) && py == roundf(py);
    const bool blit = whole && viewOn(v, CSCD_FEATURE_PARTIAL_REPAINT) && v->incrementalValid && !v->fullPaint && !v->shiftPending &&
                      !v->damage.len && !v->dragActive && !v->inspecting && (float)abs(sy) < v->vh * dpr / 2 &&
                      (float)abs(sx) < v->vw * dpr / 2 && !v->frameTransformed && paintPageMovesWhole(v) &&
                      exactOnDevice(v, cscdMinf(v->scrollX, nx), cscdMinf(v->scrollY, ny), cscdMaxf(v->scrollX, nx) + v->vw,
                                    cscdMaxf(v->scrollY, ny) + v->vh);
    v->scrollX = nx;
    v->scrollY = ny;
    if (!blit) {
        cscdViewInvalidate(v);
        return;
    }
    v->shiftX = sx;
    v->shiftY = sy;
    v->shiftArea = cscdIRect(0, 0, cscdIntOf(ceilf(v->vw * dpr)), cscdIntOf(ceilf(v->vh * dpr)));
    v->shiftPending = true;
    const float mx = abs(sx) / dpr + 1, my = abs(sy) / dpr + 1; /* (a pixel more: the strips are cut on whole pixels) */
    /* (strips that touch without overlapping: overlapping damage merges into the box around it) */
    const float top = sy > 0 ? my : 0, bottom = sy < 0 ? my : 0;
    const float hbar = (float)cscdBoxRectRight(v->root->overflow) > v->vw + 0.5f ? 12.f : 0.f; /* (a page wider than the window) */
    if (top > 0) viewAddDamage(v, cscdRect(0, 0, v->vw, top));
    if (bottom > 0) viewAddDamage(v, cscdRect(0, v->vh - bottom, v->vw, bottom));
    if (hbar > 0) viewAddDamage(v, cscdRect(0, v->vh - hbar, v->vw, hbar));
    const float y0 = top, y1 = v->vh - cscdMaxf(bottom, hbar);
    if (sx < 0) viewAddDamage(v, cscdRect(v->vw - mx, y0, mx, y1 - y0));
    if (sx > 0) viewAddDamage(v, cscdRect(0, y0, mx, y1 - y0));
    viewAddDamage(v, cscdRect(v->vw - 12, y0, 12, y1 - y0)); /* (the page's scrollbar: its thumb moved) */
    v->hitsStale = true;
    v->dirty = true;
    viewNotifyInvalidate(v);
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

/* The box of e scrolled from (ox, oy): moved by whole device pixels, a box whose inside is its last frame moved
 * (paintBoxScrollsWhole), that frame is moved inside it and only the rest of the box is painted (what came into view,
 * its scrollbars, its edges). False: not so. */
static bool scrolledByMoving(CscdView* v, CscdElement* e, float ox, float oy) {
    const CscdLayoutBox* b = e->layout;
    const float dpr = v->dpr;
    const float px = e->scrollLeft * dpr, py = e->scrollTop * dpr, qx = ox * dpr, qy = oy * dpr;
    if (!b || px != roundf(px) || py != roundf(py) || qx != roundf(qx) || qy != roundf(qy) || v->frameTransformed) return false;
    if (!viewOn(v, CSCD_FEATURE_PARTIAL_REPAINT) || !v->incrementalValid || v->fullPaint || v->shiftPending || v->dragActive || v->inspecting) return false;
    /* the inside of its padding box, its scrollbars aside, on whole device pixels in the window */
    const CscdRect box = cscdViewElementRect(v, e);
    CscdRect in = cscdRect(box.x + b->border[3], box.y + b->border[0], box.w - b->border[1] - b->border[3], box.h - b->border[0] - b->border[2]);
    in.w -= 12;
    in.h -= 12;
    in = cscdRectIntersect(in, cscdRect(0, 0, v->vw - 12, v->vh - 12));
    const int x0 = (int)ceilf(in.x * dpr), y0 = (int)ceilf(in.y * dpr), x1 = (int)floorf(cscdRectRight(in) * dpr), y1 = (int)floorf(cscdRectBottom(in) * dpr);
    const int sx = (int)lroundf(qx - px), sy = (int)lroundf(qy - py);
    if (x1 - x0 <= abs(sx) || y1 - y0 <= abs(sy)) return false;
    for (size_t i = 0; i < v->damage.len; ++i) /* (a repaint not made yet would be moved as it is) */
        if (!cscdRectEmpty(cscdRectIntersect(v->damage.items[i], box))) return false;
    const CscdRect area = cscdRect(x0 / dpr, y0 / dpr, (x1 - x0) / dpr, (y1 - y0) / dpr);
    if (!paintBoxScrollsWhole(v, b, area)) return false;
    /* (its contents' places, scrolled either way, on device pixels exactly) */
    if (!exactOnDevice(v, b->x - cscdMaxf(ox, e->scrollLeft), b->y - cscdMaxf(oy, e->scrollTop),
                       b->x + b->overflow.w + b->w, b->y + b->overflow.h + b->h))
        return false;
    v->shiftX = sx;
    v->shiftY = sy;
    v->shiftArea = cscdIRect(x0, y0, x1 - x0, y1 - y0);
    v->shiftPending = true;
    /* what stays: the area moved, within it; the rest of the box (a pixel more around it) in four bands that only touch */
    const int k0 = cscdMaxi(x0, x0 + sx), k1 = cscdMini(x1, x1 + sx), l0 = cscdMaxi(y0, y0 + sy), l1 = cscdMini(y1, y1 + sy);
    const CscdRect keep = cscdRect(k0 / dpr, l0 / dpr, (k1 - k0) / dpr, (l1 - l0) / dpr);
    const CscdRect all = cscdRect(box.x - 1, box.y - 1, box.w + 2, box.h + 2);
    viewAddDamage(v, cscdRect(all.x, all.y, all.w, keep.y - all.y));
    viewAddDamage(v, cscdRect(all.x, cscdRectBottom(keep), all.w, cscdRectBottom(all) - cscdRectBottom(keep)));
    viewAddDamage(v, cscdRect(all.x, keep.y, keep.x - all.x, keep.h));
    viewAddDamage(v, cscdRect(cscdRectRight(keep), keep.y, cscdRectRight(all) - cscdRectRight(keep), keep.h));
    v->hitsStale = true;
    v->dirty = true;
    viewNotifyInvalidate(v);
    return true;
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
        maxY = cscdMaxf(0.f, (float)b->overflow.h - pb.h);
        maxX = cscdMaxf(0.f, (float)b->overflow.w - pb.w);
    }
    float ny = cscdClampf(e->scrollTop + dy, 0.f, maxY), nx = cscdClampf(e->scrollLeft + dx, 0.f, maxX);
    if (ny == e->scrollTop && nx == e->scrollLeft) return;
    const float ox = e->scrollLeft, oy = e->scrollTop;
    e->scrollTop = ny;
    e->scrollLeft = nx;
    const bool moved = scrolledByMoving(v, e, ox, oy); /* (before the event: what it changes is painted after the move) */
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &e);
    viewSendSimple(v, "scroll", e);
    if (e && !moved) viewScrolled(v, e);
    viewUnwatch(v, mark);
}
