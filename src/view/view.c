/* CscdView: making and freeing it, features, invalidating and what must be painted again, the
 * viewport and the system's preferences, the frame (cscdViewUpdate, cscdViewPaint), timers and
 * queries. The rest of the view is in viewLoad.c (loading), viewAnimate.c (transitions and
 * animations), viewRestyle.c (styles and layout again), viewScroll.c, viewEvents.c (events, the
 * mouse, the focus, default actions), viewKeys.c (the keyboard, editing), widgets.c (menus, dialogs,
 * text fields' lines, drags) and touch.c. */
#include "style/styleInternal.h"
#include "view/viewInternal.h"
#include "core/thread.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
__declspec(dllimport) void __stdcall OutputDebugStringA(const char*);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long*);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long*);
#else
#include <time.h>
#endif


static double monotonicSeconds(void) {
#ifdef _WIN32
    long long c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c / (double)f;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

double viewNow(const CscdView* v) { return monotonicSeconds() - v->epoch; }

void cscdActionFree(CscdAction* a) {
    if (a->freeUser && a->user) a->freeUser(a->user);
    a->fn = NULL;
    a->user = NULL;
    a->freeUser = NULL;
}

void viewNotifyInvalidate(CscdView* v) {
    if (v->onInvalidate) v->onInvalidate(v->onInvalidateUser);
}

void viewClearHits(CscdView* v) {
    v->hits.len = 0;
    cscdU64MapClear(&v->hitsGone);
}

/* ---- watched pointers ---------------------------------------------------------------------------- */

void viewWatch(CscdView* v, CscdElement** e) { cscdVecPush(&v->watched, e); }
size_t viewWatchMark(const CscdView* v) { return v->watched.len; }
void viewUnwatch(CscdView* v, size_t mark) { v->watched.len = mark; }

/* ---- events into the view ------------------------------------------------------------------------ */

static void docClick(void* user, CscdElement* e) {
    CscdView* v = (CscdView*)user;
    CscdEvent c;
    cscdEventInit(&c, CSCD_SV("click"));
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &e); /* (a click handler may remove it) */
    viewDispatch(v, &c, e);
    if (!c.defaultPrevented && e) {
        c.target = e;
        viewDefaultAction(v, &c);
    }
    viewUnwatch(v, mark);
    cscdEventFini(&c);
}

static void docNodeRemoved(void* user, CscdNode* n) { viewNodeRemoved((CscdView*)user, n); }

void viewHookDocument(CscdView* v) {
    v->doc->onNodeRemoved = docNodeRemoved;
    v->doc->onNodeRemovedUser = v;
    v->doc->onClick = docClick;
    v->doc->onClickUser = v;
}

void viewUnhookDocument(CscdView* v) {
    if (!v->doc) return;
    v->doc->onNodeRemoved = NULL;
    v->doc->onClick = NULL;
}

static void defaultWarning(void* user, CscdSv message) {
    (void)user;
    CscdStr line = cscdStrf("cascade: %.*s\n", (int)message.len, message.ptr);
    fputs(cscdStrC(&line), stderr);
#ifdef _WIN32
    OutputDebugStringA(cscdStrC(&line));
#endif
    cscdStrFree(&line);
}

/* ---- features ------------------------------------------------------------------------------------ */

static const char* kFeatureNames[CSCD_FEATURE_COUNT] = {
    "paint-threads", "partial-repaint", "animations", "smooth-scroll", "tooltips", "live-reload", "accessibility",
    "inspector", "stats", "gpu", "warn-files", "warn-css", "warn-selectors",
};

const char* cscdFeatureName(CscdFeature f) { return f < CSCD_FEATURE_COUNT ? kFeatureNames[f] : ""; }

/* The defaults, changed by CASCADE_FEATURES ("+stats,-gpu"); named: the features it mentions. */
static uint32_t defaultFeatures(uint32_t* named) {
    uint32_t on = ~0u & ((1u << CSCD_FEATURE_COUNT) - 1);
    on &= ~(1u << CSCD_FEATURE_LIVE_RELOAD) & ~(1u << CSCD_FEATURE_INSPECTOR) & ~(1u << CSCD_FEATURE_STATS);
    if (named) *named = 0;
    const char* env = getenv("CASCADE_FEATURES");
    for (CscdSv rest = cscdSv(env); rest.len;) {
        size_t comma = cscdSvFindC(rest, ",", 0);
        CscdSv item = cscdSvTrim(cscdSvSub(rest, 0, comma == CSCD_NPOS ? rest.len : comma));
        rest = comma == CSCD_NPOS ? CSCD_SV("") : cscdSvFrom(rest, comma + 1);
        bool value = true;
        if (item.len && (item.ptr[0] == '+' || item.ptr[0] == '-')) { value = item.ptr[0] == '+'; item = cscdSvFrom(item, 1); }
        for (int f = 0; f < CSCD_FEATURE_COUNT; ++f)
            if (cscdSvEqC(item, kFeatureNames[f])) {
                on = value ? on | (1u << f) : on & ~(1u << f);
                if (named) *named |= 1u << f;
            }
    }
    return on;
}

bool cscdFeatureInEnvironment(CscdFeature f) {
    uint32_t named;
    defaultFeatures(&named);
    return f < CSCD_FEATURE_COUNT && ((named >> f) & 1u);
}


static void setFeature(CscdView* v, CscdFeature f, bool on);

void cscdViewSetFeature(CscdView* v, CscdFeature f, bool on) {
    if (f < CSCD_FEATURE_COUNT) v->featuresByApp |= 1u << f;
    setFeature(v, f, on);
}

static void setFeature(CscdView* v, CscdFeature f, bool on) {
    if (f >= CSCD_FEATURE_COUNT || viewOn(v, f) == on) return;
    v->features = on ? v->features | (1u << f) : v->features & ~(1u << f);
    switch (f) {
    case CSCD_FEATURE_LIVE_RELOAD: viewSetLiveReload(v, on); break;
    case CSCD_FEATURE_ANIMATIONS: v->styleDirty = true; break; /* (prefers-reduced-motion) */
    case CSCD_FEATURE_TOOLTIPS: if (!on) cscdViewHideTooltip(v); break;
    default: break;
    }
    cscdViewInvalidate(v);
}

bool cscdViewFeature(const CscdView* v, CscdFeature f) { return f < CSCD_FEATURE_COUNT && viewOn(v, f); }

CscdView* cscdViewNew(void) {
    CscdView* v = CSCD_NEW(CscdView);
    v->epoch = monotonicSeconds();
    v->textScale = 1;
    v->doc = cscdDocumentNew();
    viewHookDocument(v);
    v->styles = cscdStyleEngineNew();
    v->onWarning = defaultWarning;
    v->tooltipDelayMs = 650;
    v->vw = 800;
    v->vh = 600;
    v->dpr = 1;
    v->layoutDirty = v->styleDirty = v->dirty = v->fullPaint = true;
    v->recordHits = true;
    v->cursor = CSCD_CURSOR_DEFAULT;
    v->mouseX = v->mouseY = -1;
    v->downClicks = 1;
    v->nextTimer = 1;
    v->ghostScale = 1;
    v->splitRow = true;
    v->features = defaultFeatures(NULL);
    if (viewOn(v, CSCD_FEATURE_LIVE_RELOAD)) {
        v->features &= ~(1u << CSCD_FEATURE_LIVE_RELOAD);
        cscdViewSetFeature(v, CSCD_FEATURE_LIVE_RELOAD, true);
    }
    return v;
}

static void undoStackFree(UndoStack* u) {
    for (size_t i = 0; i < u->undo.len; ++i) cscdStrFree(&u->undo.items[i].value);
    for (size_t i = 0; i < u->redo.len; ++i) cscdStrFree(&u->redo.items[i].value);
    cscdVecFree(&u->undo);
    cscdVecFree(&u->redo);
    cscdFree(u);
}

void viewClearUndo(CscdView* v) {
    for (CscdU64MapSlot* s = NULL; (s = cscdU64MapNext(&v->undo, s));) undoStackFree((UndoStack*)s->val.p);
    cscdU64MapClear(&v->undo);
}

void viewClearTransitions(CscdView* v) {
    for (size_t i = 0; i < v->transitions.len; ++i) {
        RunningTransition* r = &v->transitions.items[i];
        cscdStrFree(&r->prop);
        cscdRelease(r->from);
        cscdRelease(r->to);
    }
    v->transitions.len = 0;
}

void viewClearAnimations(CscdView* v) {
    for (size_t i = 0; i < v->animations.len; ++i) {
        cscdStrFree(&v->animations.items[i].a.name);
        cscdRelease(v->animations.items[i].base);
    }
    v->animations.len = 0;
}

void viewClearMenus(CscdView* v) {
    for (size_t i = 0; i < v->menus.len; ++i) openMenuFree((OpenMenu*)v->menus.items[i]);
    v->menus.len = 0;
    for (size_t i = 0; i < v->menuBar.len; ++i)
        if (v->menuBar.items[i].freeUser) v->menuBar.items[i].freeUser(v->menuBar.items[i].user);
    v->menuBar.len = 0;
}

static void clearTimers(CscdView* v) {
    for (size_t i = 0; i < v->timers.len; ++i) cscdActionFree(&v->timers.items[i].fn);
    v->timers.len = 0;
}

static void clearImages(CscdView* v) {
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&v->images, s));) cscdRelease(s->val.p);
    cscdStrMapClear(&v->images);
    for (size_t i = 0; i < v->playing.len; ++i) cscdRelease(v->playing.items[i].anim);
    v->playing.len = 0;
}

void viewClearSources(CscdView* v) {
    for (size_t i = 0; i < v->sources.len; ++i) cscdStrFree(&v->sources.items[i].path);
    v->sources.len = 0;
    for (size_t i = 0; i < v->sheetMedia.len; ++i) cscdStrFree(&v->sheetMedia.items[i].query);
    v->sheetMedia.len = 0;
}

void cscdViewFree(CscdView* v) {
    if (!v) return;
    viewFreeLists(v);
    viewA11yFree(v);
    cscdVecFree(&v->paintDamage);
    viewUnhookDocument(v);
    viewClearMenus(v);
    cscdVecFree(&v->menus);
    cscdVecFree(&v->menuBar);
    clearTimers(v);
    cscdVecFree(&v->timers);
    viewClearTransitions(v);
    cscdVecFree(&v->transitions);
    viewClearAnimations(v);
    cscdVecFree(&v->animations);
    viewClearUndo(v);
    cscdU64MapFree(&v->undo);
    cscdLayoutBoxFree(v->root);
    cscdStyleEngineFree(v->styles);
    for (size_t i = 0; i < v->authorSheets.len; ++i) cscdRelease(v->authorSheets.items[i]);
    cscdVecFree(&v->authorSheets);
    if (v->baseSheet) cscdRelease(v->baseSheet);
    cscdStrVecFree(&v->authorSheetFiles);
    viewClearSources(v);
    cscdVecFree(&v->sources);
    cscdVecFree(&v->sheetMedia);
    cscdStrMapFree(&v->unknownProperties);
    clearImages(v);
    cscdStrMapFree(&v->images);
    cscdVecFree(&v->playing);
    viewBindingsClear(v);
    cscdVecFree(&v->bindings);
    cscdDocumentFree(v->doc);
    for (size_t i = 0; i < v->missing.len; ++i) cscdNodeDestroy(&((CscdElement*)v->missing.items[i])->node);
    cscdVecFree(&v->missing);
    cscdVecFree(&v->watched);
    cscdVecFree(&v->damage);
    cscdVecFree(&v->stateChanged);
    cscdVecFree(&v->smooth);
    cscdVecFree(&v->hits);
    cscdU64MapFree(&v->hitsGone);
    cscdVecFree(&v->touches);
    cscdVecFree(&v->popovers);
    cscdVecFree(&v->modals);
    cscdVecFree(&v->modalReturnFocus);
    cscdRelease(v->dragData);
    cscdRelease(v->dragGhost);
    cscdStrFree(&v->composition);
    cscdFree(v);
}

void cscdViewInvalidate(CscdView* v) {
    v->dirty = v->fullPaint = true;
    viewNotifyInvalidate(v);
}

CscdRect viewPaintArea(CscdView* v, CscdElement* e) {
    /* where the element is drawn, unless a transform or a sticky offset moves it from its box */
    bool plain = e && e->layout;
    for (CscdLayoutBox* b = plain ? e->layout : NULL; b; b = b->parent)
        if (b->style && (b->style->transform.len || b->style->position == CSCD_POS_STICKY)) plain = false;
    return plain ? cscdViewElementRect(v, e) : cscdRect(0, 0, 0, 0);
}

void viewAddDamage(CscdView* v, CscdRect r) {
    /* separate areas stay separate (each is painted on its own); overlapping ones merge */
    CscdRect a = r;
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < v->damage.len; ++i)
            if (!cscdRectEmpty(cscdRectIntersect(v->damage.items[i], a))) {
                a = cscdRectUnite(a, v->damage.items[i]);
                cscdVecRemove(&v->damage, i);
                merged = true;
                break;
            }
    }
    cscdVecPush(&v->damage, a);
    if (v->damage.len > 8) { /* many small ones: one pass over all of them */
        CscdRect all = {0, 0, 0, 0};
        for (size_t i = 0; i < v->damage.len; ++i) all = cscdRectUnite(all, v->damage.items[i]);
        v->damage.len = 0;
        cscdVecPush(&v->damage, all);
    }
}

void cscdViewInvalidateElement(CscdView* v, CscdElement* e) {
    const CscdRect r = viewPaintArea(v, e);
    if (cscdRectEmpty(r)) {
        cscdViewInvalidate(v);
        return;
    }
    viewAddDamage(v, cscdRect(r.x - 2, r.y - 2, r.w + 4, r.h + 4)); /* (antialiased edges) */
    v->dirty = true;
    viewNotifyInvalidate(v);
}

void viewWarn(CscdView* v, CscdSv message) {
    if (v->onWarning) v->onWarning(v->onWarningUser, message);
}

void viewWarnf(CscdView* v, CscdFeature kind, const char* fmt, ...) {
    if (!v->onWarning || !viewOn(v, kind)) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    v->onWarning(v->onWarningUser, cscdSv(buf));
}

void cscdViewSetViewport(CscdView* v, float w, float h, float dpr) {
    if (w == v->vw && h == v->vh && dpr == v->dpr) return;
    v->vw = cscdMaxf(1.f, w);
    v->vh = cscdMaxf(1.f, h);
    v->dpr = dpr > 0 ? dpr : 1;
    /* media queries and viewport units; a window resized with neither only lays out again */
    const CscdMediaContext media = viewMediaOf(v);
    bool sheets = false; /* (a <style media> or <link media> that holds now, or no longer) */
    for (size_t i = 0; i < v->sheetMedia.len && !sheets; ++i)
        sheets = cscdEvalMediaQuery(cscdStrSv(&v->sheetMedia.items[i].query), &media) != v->sheetMedia.items[i].held;
    if (sheets) viewCollectSheets(v);
    if (sheets || v->styleDirty || cscdStyleEngineMediaChanges(v->styles, &media)) v->styleDirty = true;
    else v->layoutDirty = true;
    for (size_t i = 0; i < v->modals.len; ++i) viewPlaceModal(v, (CscdElement*)v->modals.items[i]);
    cscdViewInvalidate(v);
}

static void setDark(CscdView* v, bool dark) {
    if (dark == v->dark) return;
    v->dark = dark;
    v->styleDirty = true;
    cscdViewInvalidate(v);
}

void cscdViewSetDarkMode(CscdView* v, bool dark) {
    v->darkByApp = true;
    setDark(v, dark);
}

bool viewDark(const CscdView* v) {
    if (v->darkByApp || !v->systemDark) return v->dark;
    return v->root && v->root->style && (v->root->style->colorScheme & 2);
}

void cscdViewSetSystemPreferences(CscdView* v, CscdSystemPreferences p) {
    if (p.dark != v->systemDark) {
        v->systemDark = p.dark;
        if (!v->darkByApp) {
            v->styleDirty = true;
            cscdViewInvalidate(v);
        }
    }
    if (!((v->featuresByApp >> CSCD_FEATURE_ANIMATIONS) & 1u) && !cscdFeatureInEnvironment(CSCD_FEATURE_ANIMATIONS))
        setFeature(v, CSCD_FEATURE_ANIMATIONS, !p.reducedMotion);
    const float scale = p.textScale > 0 ? cscdClampf(p.textScale, 0.5f, 4.f) : 1;
    if (scale != v->textScale) {
        v->textScale = scale;
        v->styleDirty = true;
        cscdViewInvalidate(v);
    }
}

CscdSystemPreferences cscdViewSystemPreferences(const CscdView* v) {
    CscdSystemPreferences p = {v->darkByApp ? v->dark : v->systemDark, !viewOn(v, CSCD_FEATURE_ANIMATIONS), v->textScale};
    return p;
}


static bool focusedIsTextField(const CscdView* v) {
    const CscdElement* f = v->doc->focused;
    return f && (cscdElementIs(f, "input") || cscdElementIs(f, "textarea"));
}

bool cscdViewUpdate(CscdView* v) {
    double t = viewNow(v);
    /* timers */
    for (size_t guard = 0; guard < 64; ++guard) {
        size_t best = CSCD_NPOS;
        for (size_t i = 0; i < v->timers.len; ++i)
            if (best == CSCD_NPOS || v->timers.items[i].due < v->timers.items[best].due) best = i;
        if (best == CSCD_NPOS || v->timers.items[best].due > t) break;
        Timer tm = v->timers.items[best];
        if (tm.interval > 0) {
            v->timers.items[best].due = t + tm.interval / 1000.0;
            cscdActionCall(&tm.fn);
        } else {
            cscdVecRemove(&v->timers, best);
            cscdActionCall(&tm.fn);
            cscdActionFree(&tm.fn);
        }
    }
    viewAdvanceScrolls(v, t);
    if (v->playing.len && viewAdvanceImages(v, t)) v->dirty = v->fullPaint = true;
    if (v->bindings.len) viewApplyBindings(v);
    if (v->doc->version != v->styledVersion && !viewRepaintAttributes(v) && !viewRelayoutText(v)) v->styleDirty = true;
    if (!v->styleDirty) viewRestyleStates(v);
    if (v->styleDirty) viewRestyle(v, NULL);
    else if (v->transitions.len || v->animations.len) viewTickAnimations(v, t);
    if (v->layoutDirty) { viewRelayout(v); v->dirty = v->fullPaint = true; viewCenterModals(v); }
    if (v->lists.len && viewUpdateLists(v)) {
        /* the rows of a list that scrolled (or whose box changed size), in this same frame */
        if (v->doc->version != v->styledVersion && !viewRepaintAttributes(v) && !viewRelayoutText(v)) v->styleDirty = true;
        if (v->styleDirty) viewRestyle(v, NULL);
        if (v->layoutDirty) { viewRelayout(v); v->dirty = v->fullPaint = true; }
    }
    if (v->transitions.len || v->animations.len) v->dirty = v->fullPaint = true;
    viewHandlesCheck(v);
    if (focusedIsTextField(v)) {
        /* caret blink */
        bool shown = cscdViewCaretVisible(v);
        if (shown != v->caretShown) { v->caretShown = shown; v->dirty = v->fullPaint = true; }
    }
    return v->dirty;
}

static void paintPage(CscdView* v, CscdCanvas* c) {
    cscdCanvasSave(c);
    if (v->root) {
        cscdCanvasTranslate(c, -v->scrollX, -v->scrollY);
        paintRoot(v, c, v->root);
    }
    cscdCanvasRestore(c);
    /* document scrollbar */
    float ch = cscdViewContentHeight(v);
    if (ch > v->vh + 0.5f && v->root && v->root->style->overflowY != CSCD_OVERFLOW_HIDDEN && v->root->style->scrollbarWidth != 2) {
        float trackH = v->vh - 4;
        float th = cscdMaxf(24.f, trackH * v->vh / ch);
        float ty = 2 + (trackH - th) * (v->scrollY / (ch - v->vh));
        CscdRRect r = cscdRRect(cscdRect(v->vw - 9, ty, 6, th));
        for (int i = 0; i < 4; ++i) r.rx[i] = r.ry[i] = 3;
        cscdCanvasSave(c);
        const CscdComputedStyle* rs = v->root->style;
        const int k = viewDark(v) ? 255 : 0;
        cscdCanvasFillRRect(c, &r, rs && rs->scrollbarThumbSet ? rs->scrollbarThumb : cscdRgba(k, k, k, 90));
        cscdCanvasRestore(c);
    }
    viewPaintHandles(v, c); /* (a selection made by touch) */
    /* what is being dragged follows the pointer */
    if (v->dragActive && v->dragGhost && !cscdImageEmpty(v->dragGhost)) {
        cscdCanvasSave(c);
        cscdCanvasSetGlobalAlpha(c, 0.72f);
        const CscdImage* g = v->dragGhost;
        cscdCanvasDrawSharedImage(c, v->dragGhost, cscdRect(0, 0, (float)g->width, (float)g->height),
                                cscdRect(v->mouseX - v->ghostX, v->mouseY - v->ghostY, g->width * v->ghostScale, g->height * v->ghostScale), true);
        cscdCanvasRestore(c);
    }
    v->dirty = false;
}

static void paintCanvas(CscdView* v, CscdCanvas* c) {
    viewClearHits(v);
    v->hitsStale = false;
    /* the page paints all of the target, unless its edge falls inside a device pixel */
    const bool whole = fmodf(v->vw * v->dpr, 1.f) == 0 && fmodf(v->vh * v->dpr, 1.f) == 0;
    if (!v->transparent && (!v->root || !whole)) cscdCanvasClear(c, viewDark(v) ? cscdRgb(18, 18, 18) : cscdRgb(255, 255, 255));
    paintPage(v, c);
    viewInspectorPaint(v, c);
    v->dirty = false;
}

static void notePaint(CscdView* v, double t0, double area, bool partial) {
    if (area <= 0) return; /* (nothing changed: the last real paint stays) */
    v->stats.paintMs = (cscdNowSeconds() - t0) * 1000;
    v->stats.paintedArea = area;
    v->stats.partialPaint = partial;
    ++v->stats.paints;
}

void cscdViewPaintCanvas(CscdView* v, CscdCanvas* c) {
    cscdViewUpdate(v);
    const double t0 = cscdNowSeconds();
    paintCanvas(v, c);
    notePaint(v, t0, (double)v->vw * v->vh * v->dpr * v->dpr, false);
}

CscdFrameStats cscdViewFrameStats(const CscdView* v) { return v->stats; }

bool cscdViewPaintDamage(const CscdView* v, CscdIRectVec* out) {
    if (v->paintDamageAll) return false;
    cscdVecAppend(out, v->paintDamage.items, v->paintDamage.len);
    return true;
}

void cscdViewSetTransparent(CscdView* v, bool on) {
    if (v->transparent == on) return;
    v->transparent = on;
    v->fullPaint = true;
    cscdViewInvalidate(v);
}

/* What a transparent page leaves empty must be empty in the target too. (Straight into the pixels:
 * a recording canvas draws later, over this.) */
static void clearPixels(CscdImage* target, int x0, int y0, int x1, int y1) {
    x0 = cscdMaxi(x0, 0);
    y0 = cscdMaxi(y0, 0);
    x1 = cscdMini(x1, target->width);
    y1 = cscdMini(y1, target->height);
    if (x1 > x0)
        for (int y = y0; y < y1; ++y) memset(cscdImageRow(target, y) + x0, 0, (size_t)(x1 - x0) * sizeof(uint32_t));
}

void cscdViewPaint(CscdView* v, CscdImage* target, bool incremental) {
    cscdViewUpdate(v);
    const double t0 = cscdNowSeconds();
    v->paintDamage.len = 0;
    v->paintDamageAll = true;
    CscdRectVec areas = {0};
    for (size_t i = 0; i < v->damage.len; ++i) {
        CscdRect a = cscdRectIntersect(v->damage.items[i], cscdRect(0, 0, v->vw, v->vh));
        if (!cscdRectEmpty(a)) cscdVecPush(&areas, a);
    }
    if ((!viewOn(v, CSCD_FEATURE_PARTIAL_REPAINT) || v->inspecting) && incremental && v->dirty) v->fullPaint = true;
    const bool partial = incremental && v->incrementalValid && !v->fullPaint && areas.len;
    /* A frame with enough to paint is recorded, then drawn by bands of rows on several threads (the
     * same pixels as drawn at once). Painters and hit areas run once, while recording. */
    float painted = v->vw * v->vh;
    if (partial) {
        painted = 0;
        for (size_t i = 0; i < areas.len; ++i) painted += areas.items[i].w * areas.items[i].h;
    }
    const bool banded = viewOn(v, CSCD_FEATURE_PAINT_THREADS) && cscdPaintThreads() > 1 && painted * v->dpr * v->dpr >= 200000;
    CscdDisplayList* list = banded ? cscdDisplayListNew() : NULL;
    CscdCanvas* c = banded ? cscdCanvasNewRecording(target, list) : cscdCanvasNew(target);
    cscdCanvasScale(c, v->dpr, v->dpr);
    if (partial) {
        /* only what invalidate(element) asked for, each area on whole device pixels; the hit areas
         * recorded with the page stay, unless contents scrolled */
        if (v->hitsStale) viewRecordHits(v);
        v->hitsStale = false;
        v->recordHits = false;
        const float dpr = v->dpr;
        for (size_t i = 0; i < areas.len; ++i) {
            const CscdRect area = areas.items[i];
            const float x0 = floorf(area.x * dpr) / dpr, y0 = floorf(area.y * dpr) / dpr;
            const CscdRect d = cscdRect(x0, y0, ceilf(cscdRectRight(area) * dpr) / dpr - x0, ceilf(cscdRectBottom(area) * dpr) / dpr - y0);
            const int dx0 = cscdIntOf(floorf(area.x * dpr)), dy0 = cscdIntOf(floorf(area.y * dpr));
            cscdVecPush(&v->paintDamage, cscdIRect(dx0, dy0, cscdIntOf(ceilf(cscdRectRight(area) * dpr)) - dx0, cscdIntOf(ceilf(cscdRectBottom(area) * dpr)) - dy0));
            cscdCanvasSave(c);
            cscdCanvasClipRect(c, d);
            if (v->transparent) clearPixels(target, dx0, dy0, cscdIntOf(ceilf(cscdRectRight(area) * dpr)), cscdIntOf(ceilf(cscdRectBottom(area) * dpr)));
            else cscdCanvasFillRect(c, d, viewDark(v) ? cscdRgb(18, 18, 18) : cscdRgb(255, 255, 255));
            paintPage(v, c);
            cscdCanvasRestore(c);
        }
        v->recordHits = true;
        v->dirty = false;
        v->paintDamageAll = false;
    } else if (incremental && v->incrementalValid && !v->fullPaint) {
        v->dirty = false; /* nothing that shows changed (or only outside the viewport) */
        painted = 0;
        v->paintDamageAll = false;
    } else {
        if (v->transparent) clearPixels(target, 0, 0, target->width, target->height);
        paintCanvas(v, c);
    }
    cscdCanvasFree(c); /* (a recording canvas notes the layers it closes as it goes) */
    if (banded) {
        cscdDisplayListReplayInto(list, target, 0);
        cscdDisplayListFree(list);
    }
    v->incrementalValid = incremental;
    v->fullPaint = false;
    v->damage.len = 0;
    cscdVecFree(&areas);
    notePaint(v, t0, (double)painted * v->dpr * v->dpr, partial);
}

bool cscdViewAnimating(const CscdView* v) {
    if (v->smooth.len) return true;
    if (v->transitions.len || v->animations.len) return true;
    return focusedIsTextField(v);
}

int cscdViewNextTimerMs(const CscdView* v) {
    double t = viewNow(v);
    double best = -1;
    for (size_t i = 0; i < v->timers.len; ++i) {
        double d = v->timers.items[i].due - t;
        if (d < 0) d = 0;
        if (best < 0 || d < best) best = d;
    }
    if (v->transitions.len || v->animations.len || v->smooth.len) best = best < 0 ? 0.016 : (best < 0.016 ? best : 0.016);
    if (viewOn(v, CSCD_FEATURE_ANIMATIONS))
        for (size_t i = 0; i < v->playing.len; ++i) {
            double d = v->playing.items[i].due - t;
            if (d == INFINITY) continue;
            if (d < 0) d = 0;
            if (best < 0 || d < best) best = d;
        }
    if (focusedIsTextField(v)) {
        double phase = fmod(t - v->caretTime, 0.5);
        double d = 0.5 - phase;
        best = best < 0 ? d : (best < d ? best : d);
    }
    return best < 0 ? -1 : (int)ceil(best * 1000);
}

bool cscdViewCaretVisible(const CscdView* v) { return fmod(viewNow(v) - v->caretTime, 1.0) < 0.5; }

int cscdViewSetTimeout(CscdView* v, CscdAction fn, int ms) {
    Timer t = {v->nextTimer, viewNow(v) + ms / 1000.0, 0, fn};
    cscdVecPush(&v->timers, t);
    viewNotifyInvalidate(v);
    return v->nextTimer++;
}

int cscdViewSetInterval(CscdView* v, CscdAction fn, int ms) {
    Timer t = {v->nextTimer, viewNow(v) + ms / 1000.0, cscdMaxi(1, ms), fn};
    cscdVecPush(&v->timers, t);
    viewNotifyInvalidate(v);
    return v->nextTimer++;
}

void cscdViewClearTimer(CscdView* v, int id) {
    for (size_t i = v->timers.len; i-- > 0;)
        if (v->timers.items[i].id == id) {
            Timer t = v->timers.items[i];
            cscdVecRemove(&v->timers, i);
            cscdActionFree(&t.fn);
        }
}

float cscdViewContentHeight(const CscdView* v) {
    if (!v->root) return 0;
    return cscdMaxf(cscdRectBottom(v->root->overflow), v->root->y + v->root->h + v->root->margin[2]);
}

/* ---- queries ------------------------------------------------------------------------------------- */

CscdElement* cscdViewElementAt(CscdView* v, float x, float y) {
    CscdPoint d = {x * v->dpr, y * v->dpr};
    for (size_t i = v->hits.len; i-- > 0;) {
        const CscdHitItem* it = &v->hits.items[i];
        if (!cscdRectContains(it->clip, d.x, d.y)) continue;
        CscdPoint p = d;
        if (it->projective) { /* back through the perspective, onto the image the box was painted in */
            const float* h = it->h;
            const float a = h[4] * h[8] - h[5] * h[7], b = h[5] * h[6] - h[3] * h[8], c = h[3] * h[7] - h[4] * h[6];
            const float det = h[0] * a + h[1] * b + h[2] * c;
            if (fabsf(det) < 1e-12f) continue;
            const float i0 = a / det, i1 = (h[2] * h[7] - h[1] * h[8]) / det, i2 = (h[1] * h[5] - h[2] * h[4]) / det;
            const float i3 = b / det, i4 = (h[0] * h[8] - h[2] * h[6]) / det, i5 = (h[2] * h[3] - h[0] * h[5]) / det;
            const float i6 = c / det, i7 = (h[1] * h[6] - h[0] * h[7]) / det, i8 = (h[0] * h[4] - h[1] * h[3]) / det;
            const float w = i6 * d.x + i7 * d.y + i8;
            if (w <= 0) continue;
            p = cscdPoint((i0 * d.x + i1 * d.y + i2) / w, (i3 * d.x + i4 * d.y + i5) / w);
        }
        CscdMatrix inv;
        if (!cscdMatInvert(it->m, &inv)) continue;
        CscdPoint l = cscdMatMap(inv, p);
        if (!cscdRectContains(it->rect, l.x, l.y)) continue;
        if (!cscdU64MapGet(&v->hitsGone, CSCD_PKEY(it->element))) return it->element;
    }
    CscdElement* body = cscdDocumentBody(v->doc);
    return body ? body : cscdDocumentElement(v->doc);
}

static CscdRect boxRect(const CscdLayoutBox* b) {
    CscdRect r = cscdBoxBorderBox(b);
    if (b->kind == CSCD_BOX_INLINE && b->inlineFrags.len) {
        r = b->inlineFrags.items[0].rect;
        for (size_t i = 0; i < b->inlineFrags.len; ++i) r = cscdRectUnite(r, b->inlineFrags.items[i].rect);
    }
    return r;
}

CscdRect cscdViewElementRect(CscdView* v, CscdElement* e) {
    if (!e || !e->layout) return cscdRect(0, 0, 0, 0);
    CscdLayoutBox* b = e->layout;
    CscdRect r = boxRect(b);
    /* scroll offsets of ancestors */
    float dx = -v->scrollX, dy = -v->scrollY;
    for (CscdLayoutBox* p = b->parent; p; p = p->parent)
        if (p->style && cscdStyleClipsOverflow(p->style) && p->element && p->parent) { dx -= p->element->scrollLeft; dy -= p->element->scrollTop; }
    if (b->style && b->style->position == CSCD_POS_FIXED) { dx = 0; dy = 0; }
    return cscdRectTranslated(r, dx, dy);
}

CscdRect cscdElementBoundingClientRect(const CscdElement* e) { return e->layout ? boxRect(e->layout) : cscdRect(0, 0, 0, 0); }
float cscdElementScrollWidth(const CscdElement* e) { return e->layout ? cscdMaxf(e->layout->overflow.w, cscdBoxPaddingBox(e->layout).w) : 0; }
float cscdElementScrollHeight(const CscdElement* e) { return e->layout ? cscdMaxf(e->layout->overflow.h, cscdBoxPaddingBox(e->layout).h) : 0; }

/* ---- small accessors ----------------------------------------------------------------------------- */

void cscdViewOnWarning(CscdView* v, CscdWarningFn fn, void* user) { v->onWarning = fn; v->onWarningUser = user; }
void cscdViewOnNodeRemoved(CscdView* v, CscdNodeFn fn, void* user) { v->onNodeRemoved = fn; v->onNodeRemovedUser = user; }
void cscdViewOnDocumentReplaced(CscdView* v, CscdActionFn fn, void* user) { v->onDocumentReplaced = fn; v->onDocumentReplacedUser = user; }
void cscdViewOnInvalidate(CscdView* v, CscdActionFn fn, void* user) { v->onInvalidate = fn; v->onInvalidateUser = user; }
void cscdViewOnMenuItem(CscdView* v, CscdMenuItemFn fn, void* user) { v->onMenuItem = fn; v->onMenuItemUser = user; }
void cscdViewSetClipboardHooks(CscdView* v, CscdSetClipboardFn set, CscdGetClipboardFn get, CscdSetClipboardImageFn setImage,
                             CscdGetClipboardFilesFn getFiles, void* user) {
    v->setClipboard = set;
    v->getClipboard = get;
    v->setClipboardImage = setImage;
    v->getClipboardFiles = getFiles;
    v->clipboardUser = user;
}
CscdDocument* cscdViewDocument(CscdView* v) { return v->doc; }
CscdElement* cscdViewGetElementById(CscdView* v, CscdSv id) { return cscdDocumentGetElementById(v->doc, id); }
CscdElement* cscdViewQuerySelector(CscdView* v, CscdSv selector) { return cscdDocumentQuerySelector(v->doc, selector); }
void cscdViewQuerySelectorAll(CscdView* v, CscdSv selector, CscdPtrVec* out) { cscdDocumentQuerySelectorAll(v->doc, selector, out); }
CscdStr cscdViewTitle(const CscdView* v) { return cscdDocumentTitle(v->doc); }
float cscdViewWidth(const CscdView* v) { return v->vw; }
float cscdViewHeight(const CscdView* v) { return v->vh; }
float cscdViewDevicePixelRatio(const CscdView* v) { return v->dpr; }
bool cscdViewNeedsPaint(const CscdView* v) { return v->dirty; }
CscdCursor cscdViewCursor(const CscdView* v) { return v->cursor; }
bool cscdViewMenuOpen(const CscdView* v) { return v->menus.len != 0; }
CscdElement* cscdViewModalDialog(const CscdView* v) { return v->modals.len ? (CscdElement*)cscdVecLast(&v->modals) : NULL; }
void cscdViewSetTooltipDelay(CscdView* v, int ms) { v->tooltipDelayMs = ms; }
bool cscdViewDragging(const CscdView* v) { return v->dragActive; }
float cscdViewScrollX(const CscdView* v) { return v->scrollX; }
float cscdViewScrollY(const CscdView* v) { return v->scrollY; }
CscdLayoutBox* cscdViewRootBox(CscdView* v) { return v->root; }
CscdStyleEngine* cscdViewStyleEngine(CscdView* v) { return v->styles; }
