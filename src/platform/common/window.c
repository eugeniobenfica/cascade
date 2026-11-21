#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* (clock_gettime's process clock under a strict C standard) */
#endif
/* What the native windows share: construction, the posted-function queue, the frame (update, paint,
 * present, and the software fallback), the title taken from the document, and the run loop. The
 * platforms (windowWin32.c, windowX11.c, windowCocoa.m) supply the window itself and its events. */
#include "platform/common/platformInternal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

CscdWindowOptions cscdWindowOptions(void) {
    CscdWindowOptions o;
    memset(&o, 0, sizeof o);
    o.title = "cascade";
    o.width = 1024;
    o.height = 720;
    o.backend = CSCD_BACKEND_AUTO;
    o.resizable = true;
    o.minWidth = 200;
    o.minHeight = 150;
    o.visible = true;
    o.liveReload = -1;
    return o;
}

CscdWindowOptions* cscdWindowOptionsNew(void) {
    CscdWindowOptions* o = CSCD_NEW(CscdWindowOptions);
    *o = cscdWindowOptions();
    return o;
}
void cscdWindowOptionsFree(CscdWindowOptions* o) {
    if (!o) return;
    cscdFree(o->ownTitle_);
    cscdFree(o->ownPage_);
    cscdFree(o);
}
static char* optionsCopy(char** own, const char* s) {
    cscdFree(*own);
    *own = NULL;
    if (!s) return NULL;
    const size_t n = strlen(s) + 1;
    *own = (char*)cscdMalloc(n);
    memcpy(*own, s, n);
    return *own;
}
void cscdWindowOptionsSetTitle(CscdWindowOptions* o, const char* title) { o->title = optionsCopy(&o->ownTitle_, title); }
void cscdWindowOptionsSetSize(CscdWindowOptions* o, int width, int height) {
    o->width = width;
    o->height = height;
}
void cscdWindowOptionsSetMinSize(CscdWindowOptions* o, int width, int height) {
    o->minWidth = width;
    o->minHeight = height;
}
void cscdWindowOptionsSetBackend(CscdWindowOptions* o, CscdBackend backend) { o->backend = backend; }
void cscdWindowOptionsSetResizable(CscdWindowOptions* o, bool resizable) { o->resizable = resizable; }
void cscdWindowOptionsSetDarkTitleBar(CscdWindowOptions* o, bool dark) { o->darkTitleBar = dark; }
void cscdWindowOptionsSetVisible(CscdWindowOptions* o, bool visible) { o->visible = visible; }
void cscdWindowOptionsSetPage(CscdWindowOptions* o, const char* page) { o->page = optionsCopy(&o->ownPage_, page); }
void cscdWindowOptionsSetLiveReload(CscdWindowOptions* o, int liveReload) { o->liveReload = liveReload; }
void cscdWindowOptionsSetPresenter(CscdWindowOptions* o, CscdPresenterFn presenter, void* user) {
    o->presenter = presenter;
    o->presenterUser = user;
}

static void viewInvalidated(void* user) {
    /* invalidated from inside the loop (a drag, a timer, a resize the system runs itself): the loop
     * must come round, once a frame */
    CscdWindow* w = (CscdWindow*)user;
    if (w->dirty) return;
    w->dirty = true;
    if (w->hwnd) windowWake(w);
}

CscdPresenter* windowPresenterNew(CscdWindow* w, void* surface, CscdBackend preferred, int width, int height) {
    CscdPresenter* p = w->makePresenter ? w->makePresenter(w->makePresenterUser, surface, width, height) : NULL;
    return p ? p : cscdPresenterNew(surface, preferred, width, height);
}

CscdWindow* cscdWindowNew(const CscdWindowOptions* opts) {
    CscdWindowOptions defaults = cscdWindowOptions();
    if (!opts) opts = &defaults;
    CscdWindow* w = CSCD_NEW(CscdWindow);
    w->view = cscdViewNew();
    w->dirty = true;
    w->dpr = 1;
    cscdMutexInit(&w->postMutex);
    w->requested = opts->backend;
    w->makePresenter = opts->presenter;
    w->makePresenterUser = opts->presenterUser;
    w->gpuApplied = opts->backend != CSCD_BACKEND_SOFTWARE;
    w->titleFromDocument = !opts->title || !opts->title[0];
    windowCreateNative(w, opts);
    if (!w->hwnd) {
        /* (the page still loads: the program finds its elements, and cscdWindowRun returns) */
        fprintf(stderr, "cascade: the window could not be opened\n");
        if (opts->page && opts->page[0]) cscdViewLoadFile(w->view, cscdSv(opts->page));
        return w;
    }
    cscdViewOnInvalidate(w->view, viewInvalidated, w);
    /* debug builds: live reload and the inspector, unless the program or CASCADE_FEATURES says otherwise */
    if (opts->liveReload >= 0) cscdViewSetFeature(w->view, CSCD_FEATURE_LIVE_RELOAD, opts->liveReload != 0);
    else if (cscdAppDebugBuild() && !cscdFeatureInEnvironment(CSCD_FEATURE_LIVE_RELOAD)) cscdViewSetFeature(w->view, CSCD_FEATURE_LIVE_RELOAD, true);
    if (cscdAppDebugBuild() && !cscdFeatureInEnvironment(CSCD_FEATURE_INSPECTOR)) cscdViewSetFeature(w->view, CSCD_FEATURE_INSPECTOR, true);
    if (opts->page && opts->page[0]) cscdViewLoadFile(w->view, cscdSv(opts->page));
    if (opts->visible) cscdWindowShow(w);
    return w;
}

static void popupsHideAll(CscdWindow* w) {
    for (size_t i = w->popups.len; i-- > 0;) platformPopupHide(w, w->popups.items[i].native); /* (top first) */
    w->popups.len = 0;
}

/* The view's popups in windows of their own: the area they may take (the screen's), each painted and shown where it
 * goes, those that closed hidden. */
static void syncPopups(CscdWindow* w) {
    CscdIRect area;
    if (!platformPopupArea(w, &area) || w->dpr <= 0) {
        if (w->popups.len) popupsHideAll(w);
        cscdViewSetPopupArea(w->view, cscdRect(0, 0, 0, 0));
        return;
    }
    const float dpr = w->dpr;
    cscdViewSetPopupArea(w->view, cscdRect(area.x / dpr, area.y / dpr, area.w / dpr, area.h / dpr));
    CscdPopup list[16];
    const size_t n = cscdMinz(cscdViewPopups(w->view, list, 16), 16);
    /* (those gone first, the top one first: a popup under another may not go before it) */
    for (size_t i = w->popups.len; i-- > 0;) {
        bool open = false;
        for (size_t j = 0; j < n; ++j) open = open || list[j].id == w->popups.items[i].id;
        if (!open) {
            platformPopupHide(w, w->popups.items[i].native);
            cscdVecRemove(&w->popups, i);
        }
    }
    for (size_t j = 0; j < n; ++j) {
        const CscdRect r = list[j].rect;
        const int x = cscdIntOf(floorf(r.x * dpr)), y = cscdIntOf(floorf(r.y * dpr));
        const CscdIRect at = cscdIRect(x, y, cscdIntOf(ceilf(cscdRectRight(r) * dpr)) - x, cscdIntOf(ceilf(cscdRectBottom(r) * dpr)) - y);
        if (at.w <= 0 || at.h <= 0) continue;
        CscdImage img;
        cscdImageInit(&img, at.w, at.h);
        if (cscdViewPaintPopup(w->view, list[j].id, &img)) {
            WindowPopup* p = NULL;
            for (size_t i = 0; i < w->popups.len; ++i) if (w->popups.items[i].id == list[j].id) p = &w->popups.items[i];
            if (!p) {
                WindowPopup fresh = {list[j].id, NULL, at};
                cscdVecPush(&w->popups, fresh);
                p = &w->popups.items[w->popups.len - 1];
            }
            p->rect = at;
            p->native = platformPopupShow(w, p->native, at, &img, !list[j].tooltip);
        }
        cscdImageFini(&img);
    }
}

void cscdWindowFree(CscdWindow* w) {
    if (!w) return;
    popupsHideAll(w);
    cscdVecFree(&w->popups);
    cscdPresenterFree(w->presenter);
    w->presenter = NULL;
    if (w->native) windowDestroyNative(w);
    cscdViewFree(w->view);
    cscdImageFini(&w->frame);
    windowBackdropForget(w);
    cscdStrFree(&w->lastTitle);
    for (size_t i = 0; i < w->posted.len; ++i) cscdActionFree(&w->posted.items[i]);
    cscdVecFree(&w->posted);
    cscdMutexDestroy(&w->postMutex);
    cscdFree(w);
}

CscdView* cscdWindowView(CscdWindow* w) { return w->view; }
bool cscdWindowIsOpen(const CscdWindow* w) { return w->hwnd != NULL; }
bool cscdWindowIsFullscreen(const CscdWindow* w) { return w->fullscreen; }
void* cscdWindowNativeHandle(const CscdWindow* w) { return w->hwnd; }
void* cscdWindowNativeDisplay(const CscdWindow* w) { return w->display; }
float cscdWindowDevicePixelRatio(const CscdWindow* w) { return w->dpr; }
void cscdWindowOnClose(CscdWindow* w, CscdCloseFn fn, void* user) { w->onClose = fn; w->onCloseUser = user; }
void cscdWindowOnFrame(CscdWindow* w, CscdActionFn fn, void* user) { w->onFrame = fn; w->onFrameUser = user; }
void cscdWindowOnAppEvent(CscdWindow* w, CscdAppEventFn fn, void* user) { w->onAppEvent = fn; w->onAppEventUser = user; }

void windowAppEvent(CscdWindow* w, CscdAppEvent event) {
    if (event == CSCD_APP_BACKGROUND || event == CSCD_APP_FOREGROUND) {
        const bool background = event == CSCD_APP_BACKGROUND;
        if (background == w->background) return;
        w->background = background;
        if (!background) w->dirty = true;
        if (background) {
            /* menus and tooltips do not outlive the app's turn in front */
            cscdViewCloseMenus(w->view);
            cscdViewHideTooltip(w->view);
        }
    }
    if (w->onAppEvent) w->onAppEvent(w->onAppEventUser, event);
}

void windowSystemPreferences(CscdWindow* w, CscdSystemPreferences p) {
    const CscdSystemPreferences was = cscdViewSystemPreferences(w->view);
    cscdViewSetSystemPreferences(w->view, p);
    const CscdSystemPreferences now = cscdViewSystemPreferences(w->view);
    if (was.dark != now.dark || was.reducedMotion != now.reducedMotion || was.textScale != now.textScale) w->dirty = true;
}

bool cscdWindowOpenUrl(CscdWindow* w, const char* url) { return w->hwnd && url && url[0] && platformOpenUrl(w, url); }
bool cscdWindowShareText(CscdWindow* w, const char* text) { return w->hwnd && text && platformShareText(w, text); }
void cscdWindowHaptic(CscdWindow* w, CscdHaptic kind) { if (w->hwnd) platformHaptic(w, kind); }

bool cscdWindowPickFiles(CscdWindow* w, const char* accept, bool multiple, CscdPickFilesFn fn, void* user) {
    if (!w->hwnd || !fn || w->pickFn) return false; /* (one picker at a time) */
    w->pickFn = fn;
    w->pickUser = user;
    if (platformPickFiles(w, accept ? accept : "", multiple)) return true;
    w->pickFn = NULL;
    w->pickUser = NULL;
    return false;
}

bool cscdWindowHide(CscdWindow* w) {
    if (!w->hwnd) return false;
    if (!w->shown) return true;
    if (!platformHide(w)) return false;
    w->shown = false;
    return true;
}

bool cscdWindowSetMenuBar(CscdWindow* w, CscdMenu menu) {
    if (w->hwnd && platformSetMenuBar(w, &menu)) return true;
    cscdMenuFree(&menu);
    return false;
}

void windowMenuChosen(CscdWindow* w, const CscdMenuItem* item) {
    if (!item || !item->enabled || item->separator) return;
    cscdActionCall(&item->action);
    if (w) {
        w->dirty = true;
        windowWake(w);
    }
}

CscdTray* cscdTrayNew(CscdWindow* w, const CscdImage* icon, const char* tooltip) {
    if (!w->hwnd) return NULL;
    CscdTray* t = cscdCalloc(1, sizeof *t);
    t->w = w;
    if (platformTrayCreate(t, icon, tooltip ? tooltip : "")) return t;
    cscdFree(t);
    return NULL;
}

void cscdTraySetIcon(CscdTray* t, const CscdImage* icon) { if (t && icon) platformTraySet(t, icon, NULL); }
void cscdTraySetTooltip(CscdTray* t, const char* tooltip) { if (t) platformTraySet(t, NULL, tooltip ? tooltip : ""); }

void cscdTraySetMenu(CscdTray* t, CscdMenu menu) {
    if (!t) {
        cscdMenuFree(&menu);
        return;
    }
    cscdMenuFree(&t->menu);
    t->menu = menu;
}

void cscdTrayOnClick(CscdTray* t, CscdAction action) {
    if (!t) {
        cscdActionFree(&action);
        return;
    }
    cscdActionFree(&t->click);
    t->click = action;
}

void cscdTrayFree(CscdTray* t) {
    if (!t) return;
    platformTrayDestroy(t);
    cscdMenuFree(&t->menu);
    cscdActionFree(&t->click);
    cscdFree(t);
}

void windowTrayClicked(CscdTray* t) {
    cscdActionCall(&t->click);
    t->w->dirty = true;
    windowWake(t->w);
}

bool cscdWindowSaveFile(CscdWindow* w, const char* name, const char* accept, CscdSaveFileFn fn, void* user) {
    if (!w->hwnd || !fn || w->saveFn) return false; /* (one dialog at a time) */
    w->saveFn = fn;
    w->saveUser = user;
    if (platformSaveFile(w, name ? name : "", accept ? accept : "")) return true;
    w->saveFn = NULL;
    w->saveUser = NULL;
    return false;
}

bool windowSaveChosen(CscdWindow* w, const char* path) {
    CscdSaveFileFn fn = w->saveFn;
    void* user = w->saveUser;
    w->saveFn = NULL;
    w->saveUser = NULL;
    if (path && !path[0]) path = NULL;
    const bool wrote = fn && fn(user, path) && path;
    w->dirty = true;
    return wrote;
}

void windowAcceptExtensions(const char* accept, CscdStrVec* out) {
    static const struct { const char* type; const char* extensions; } kTypes[] = {
        {"image/*", "png jpg jpeg gif webp bmp svg heic heif avif tif tiff ico"}, {"image/png", "png"}, {"image/jpeg", "jpg jpeg"},
        {"image/gif", "gif"}, {"image/webp", "webp"}, {"image/svg+xml", "svg"}, {"image/bmp", "bmp"},
        {"text/*", "txt csv md html htm css xml json"}, {"text/plain", "txt"}, {"text/csv", "csv"}, {"text/html", "html htm"},
        {"application/json", "json"}, {"application/pdf", "pdf"}, {"application/zip", "zip"},
        {"audio/*", "mp3 wav ogg oga flac m4a aac opus"}, {"video/*", "mp4 m4v mov webm mkv avi"},
        {"font/*", "ttf otf woff woff2 ttc"}};
    CscdSv rest = cscdSv(accept ? accept : "");
    while (rest.len) {
        size_t comma = cscdSvFindChar(rest, ',', 0);
        const CscdSv item = cscdSvTrim(cscdSvSub(rest, 0, comma == CSCD_NPOS ? rest.len : comma));
        rest = comma == CSCD_NPOS ? CSCD_SV("") : cscdSvSub(rest, comma + 1, rest.len - comma - 1);
        if (!item.len) continue;
        if (item.ptr[0] == '.') {
            CscdStr e = cscdStrDup(cscdSvSub(item, 1, item.len - 1));
            for (size_t i = 0; i < e.len; ++i) e.ptr[i] = (char)tolower((unsigned char)e.ptr[i]);
            cscdVecPush(out, e);
            continue;
        }
        for (size_t t = 0; t < CSCD_COUNT(kTypes); ++t) {
            if (!cscdSvIEqC(item, kTypes[t].type)) continue;
            CscdSv list = cscdSv(kTypes[t].extensions);
            while (list.len) {
                size_t sp = cscdSvFindChar(list, ' ', 0);
                cscdVecPush(out, cscdStrDup(cscdSvSub(list, 0, sp == CSCD_NPOS ? list.len : sp)));
                list = sp == CSCD_NPOS ? CSCD_SV("") : cscdSvSub(list, sp + 1, list.len - sp - 1);
            }
        }
    }
}

void windowFilesPicked(CscdWindow* w, const CscdStrVec* paths) {
    CscdPickFilesFn fn = w->pickFn;
    void* user = w->pickUser;
    w->pickFn = NULL;
    w->pickUser = NULL;
    if (fn) fn(user, paths);
    w->dirty = true;
}

void cscdWindowPost(CscdWindow* w, CscdAction fn) {
    cscdMutexLock(&w->postMutex);
    cscdVecPush(&w->posted, fn);
    cscdMutexUnlock(&w->postMutex);
    if (w->hwnd) windowWake(w);
}

bool windowHasPosted(CscdWindow* w) {
    cscdMutexLock(&w->postMutex);
    const bool any = w->posted.len != 0;
    cscdMutexUnlock(&w->postMutex);
    return any;
}

void windowRunPosted(CscdWindow* w) {
    cscdMutexLock(&w->postMutex);
    CscdAction* now = w->posted.items;
    const size_t n = w->posted.len;
    memset(&w->posted, 0, sizeof w->posted);
    cscdMutexUnlock(&w->postMutex);
    for (size_t i = 0; i < n; ++i) {
        cscdActionCall(&now[i]);
        cscdActionFree(&now[i]);
    }
    cscdFree(now);
}

CscdBackend cscdWindowBackend(const CscdWindow* w) { return w->presenter ? cscdPresenterBackend(w->presenter) : CSCD_BACKEND_SOFTWARE; }
void cscdWindowSetBackend(CscdWindow* w, CscdBackend backend) {
    w->requested = backend;
    w->trial = 0; /* (Auto: tried again as the next frame shows) */
    w->gpuApplied = backend != CSCD_BACKEND_SOFTWARE;
    if (w->presenter) {
        cscdPresenterFree(w->presenter);
        w->presenter = windowPresenterNew(w, windowSurface(w), backend, w->clientW, w->clientH);
    }
    cscdImageFini(&w->frame); /* (the next frame is painted whole: a new presenter has nothing to update) */
    memset(&w->frame, 0, sizeof w->frame);
    w->dirty = true;
}

const char* cscdWindowBackendDescription(const CscdWindow* w) { return w->presenter ? cscdPresenterDescription(w->presenter) : "none"; }

/* The stats panel, drawn over the frame for the presenter; what it covers is kept in under and put
 * back afterwards (the frame stays the page's: only what changes is painted into it). */
static CscdIRect drawStats(CscdWindow* w, CscdImage* under) {
    static CscdStrVec families;
    if (!families.len) {
        cscdVecPush(&families, cscdStrDupC("Segoe UI"));
        cscdVecPush(&families, cscdStrDupC("sans-serif"));
    }
    const CscdFrameStats st = cscdViewFrameStats(w->view);
    char text[160];
    snprintf(text, sizeof text, "style %.1f ms (%d new, %d reused)  layout %.1f ms  paint %.1f ms%s", st.styleMs, st.stylesComputed,
             st.stylesReused, st.layoutMs, st.paintMs, st.partialPaint ? " (part)" : "");
    const float dpr = w->dpr > 0 ? w->dpr : 1;
    const CscdFont font = cscdFontMatch(&families, 11 * dpr, 400, false);
    CscdCanvas* c = cscdCanvasNew(&w->frame);
    const float tw = cscdCanvasMeasureText(c, cscdSv(text), &font), pad = 6 * dpr, h = 11 * dpr + 2 * pad;
    const int x0 = cscdMaxi(0, cscdIntOf(floorf(w->frame.width - tw - 2 * pad - 6 * dpr))), y0 = (int)(6 * dpr);
    const CscdIRect r = cscdIRect(x0, y0, cscdMini(w->frame.width - x0, cscdIntOf(ceilf(tw + 2 * pad))), cscdMini(w->frame.height - y0, cscdIntOf(ceilf(h))));
    if (r.w <= 0 || r.h <= 0) { cscdCanvasFree(c); return cscdIRect(0, 0, 0, 0); }
    cscdImageInit(under, r.w, r.h);
    for (int y = 0; y < r.h; ++y) memcpy(cscdImageRow(under, y), cscdImageRow(&w->frame, r.y + y) + r.x, (size_t)r.w * 4);
    CscdRRect box = cscdRRect(cscdRect((float)r.x, (float)r.y, (float)r.w, (float)r.h));
    for (int i = 0; i < 4; ++i) box.rx[i] = box.ry[i] = 4 * dpr;
    cscdCanvasFillRRect(c, &box, cscdRgba(0, 0, 0, 190));
    cscdCanvasDrawText(c, cscdSv(text), r.x + pad, r.y + pad + cscdFontAscent(&font) * 0.9f, &font, cscdRgb(255, 255, 255));
    cscdCanvasFree(c);
    return r;
}

static void restoreUnderStats(CscdWindow* w, CscdImage* under, CscdIRect r) {
    for (int y = 0; y < r.h; ++y) memcpy(cscdImageRow(&w->frame, r.y + y) + r.x, cscdImageRow(under, y), (size_t)r.w * 4);
    cscdImageFini(under);
}

void cscdWindowSetTitle(CscdWindow* w, const char* title) {
    w->titleFromDocument = false;
    if (w->hwnd) windowShowTitle(w, title);
}

/* ---- around the page, on phones: the backdrop and the back gesture ---------------------------------- */

#ifdef CSCD_PLATFORM_MOBILE
/* The color that rules a row of the frame (most of 32 samples), or else fallback. */
static uint32_t rowColor(const CscdImage* f, int y, uint32_t fallback) {
    const uint32_t* row = cscdImageRow(f, y);
    uint32_t samples[32];
    for (int i = 0; i < 32; ++i) samples[i] = row[(int)((long)f->width * (2 * i + 1) / 64)] | 0xFF000000u;
    int best = 0, bestCount = 0;
    for (int i = 0; i < 32; ++i) {
        int count = 0;
        for (int k = 0; k < 32; ++k) count += samples[k] == samples[i];
        if (count > bestCount) { best = i; bestCount = count; }
    }
    return bestCount * 10 >= 32 * 6 ? samples[best] : fallback;
}

static uint32_t argbOf(CscdColor c) { return 0xFF000000u | (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | c.b; }

/* The color under a bar at a row of the frame: the row's own, when what is drawn there stays put; when
 * it scrolls (in a box that scrolls, or with the page), that box's background, so items passing under
 * the bar do not tint it. */
static uint32_t edgeColor(CscdWindow* w, int y, uint32_t canvas) {
    const float x = (float)w->clientW / w->dpr / 2, cy = ((float)y + 0.5f) / w->dpr;
    CscdElement* e = cscdViewElementAt(w->view, x, cy);
    bool pageScrolls = cscdViewContentHeight(w->view) > cscdViewHeight(w->view) + 0.5f;
    for (CscdElement* a = e; a; a = cscdNodeParentElement(&a->node)) {
        const CscdComputedStyle* s = a->style;
        if (!s) continue;
        if (s->position == CSCD_POS_FIXED || s->position == CSCD_POS_STICKY) { pageScrolls = false; break; } /* (it stays) */
        if (a != e && (s->overflowY == CSCD_OVERFLOW_AUTO || s->overflowY == CSCD_OVERFLOW_SCROLL))
            return s->backgroundColor.a == 255 ? argbOf(s->backgroundColor) : canvas;
    }
    return pageScrolls ? canvas : rowColor(&w->frame, y, canvas);
}

/* a column of the frame as (first row, color) runs */
static void columnRuns(const CscdImage* f, int x, CscdU32Vec* out) {
    out->len = 0;
    uint32_t last = 0;
    for (int y = 0; y < f->height; ++y) {
        const uint32_t c = cscdImageRow(f, y)[x] | 0xFF000000u;
        if (out->len && c == last) continue;
        cscdVecPush(out, (uint32_t)y);
        cscdVecPush(out, c);
        last = c;
    }
}

static bool runsEq(const CscdU32Vec* a, const CscdU32Vec* b) {
    return a->len == b->len && (!a->len || !memcmp(a->items, b->items, a->len * sizeof(uint32_t)));
}

/* What the system shows around the page, from the frame's edges: told when it changed. */
static void tellBackdrop(CscdWindow* w) {
    const CscdImage* f = &w->frame;
    if (f->width <= 0 || f->height <= 0) return;
    const uint32_t canvas = argbOf(cscdViewCanvasColor(w->view));
    CscdWindowBackdrop now = {edgeColor(w, 0, canvas), edgeColor(w, f->height - 1, canvas), {0}, {0}};
    columnRuns(f, 0, &now.left);
    columnRuns(f, f->width - 1, &now.right);
    CscdWindowBackdrop* was = &w->backdrop;
    if (now.top == was->top && now.bottom == was->bottom && runsEq(&now.left, &was->left) && runsEq(&now.right, &was->right)) {
        cscdVecFree(&now.left);
        cscdVecFree(&now.right);
        return;
    }
    cscdVecFree(&was->left);
    cscdVecFree(&was->right);
    *was = now;
    windowSetBackdrop(w, was);
}
#endif

#ifdef __ANDROID__
static bool claimsSideDrags(const CscdElement* e) {
    if (cscdElementHasAttributeC(e, "splitter")) return true;
    if (cscdElementIs(e, "input") && cscdSvIEqC(cscdElementAttrC(e, "type"), "range") && !cscdElementDisabled(e)) return true;
    const CscdComputedStyle* s = e->style;
    return s && s->touchAction && !(s->touchAction & CSCD_TA_PAN_X);
}

static void findSideDrags(CscdNode* n, CscdPtrVec* out, int depth) {
    if (depth > 256) return;
    for (size_t i = 0; i < n->children.len; ++i) {
        CscdElement* e = cscdNodeAsElement(cscdNodeChild(n, i));
        if (!e || !e->style) continue; /* (not shown: nor its children) */
        if (claimsSideDrags(e)) cscdVecPush(out, e); /* (its children's drags are its own) */
        else findSideDrags(&e->node, out, depth + 1);
    }
}

/* Where a drag sideways is the page's (sliders, splitters, touch-action without pan-x), near enough to
 * the screen's sides for the back gesture to take it: told to the system. The candidates are found
 * again when the document changes (removing a node changes it: they cannot dangle); their places, after
 * each frame. */
static void tellSideDrags(CscdWindow* w) {
    CscdDocument* d = cscdViewDocument(w->view);
    if (d->version != w->sideDragsVersion) {
        w->sideDragsVersion = d->version;
        w->sideDrags.len = 0;
        findSideDrags(&d->node, &w->sideDrags, 0);
    }
    CscdIRectVec now = {0};
    const float dpr = w->dpr, edge = 48 * dpr; /* (the gesture's zone is narrower) */
    for (size_t i = 0; i < w->sideDrags.len; ++i) {
        const CscdRect r = cscdViewElementRect(w->view, (CscdElement*)w->sideDrags.items[i]);
        const int x0 = cscdMaxi(0, cscdIntOf(floorf(r.x * dpr))), y0 = cscdMaxi(0, cscdIntOf(floorf(r.y * dpr)));
        const int x1 = cscdMini(w->clientW, cscdIntOf(ceilf(cscdRectRight(r) * dpr))), y1 = cscdMini(w->clientH, cscdIntOf(ceilf(cscdRectBottom(r) * dpr)));
        if (x1 <= x0 || y1 <= y0 || (x0 > edge && x1 < w->clientW - edge)) continue;
        cscdVecPush(&now, cscdIRect(x0, y0, x1 - x0, y1 - y0));
    }
    const bool same = w->sideDragsTold && now.len == w->sideDragRects.len &&
                      (!now.len || !memcmp(now.items, w->sideDragRects.items, now.len * sizeof(CscdIRect)));
    if (!same) windowSetGestureExclusion(w, now.items, now.len);
    cscdVecFree(&w->sideDragRects);
    w->sideDragRects = now;
    w->sideDragsTold = true;
}
#endif

/* what was told to the system around the page: told again (a new activity) or let go of */
void windowBackdropForget(CscdWindow* w) {
    w->backdrop.top = w->backdrop.bottom = 0;
    cscdVecFree(&w->backdrop.left);
    cscdVecFree(&w->backdrop.right);
    w->sideDragsTold = false;
    w->sideDragsVersion = 0;
    cscdVecFree(&w->sideDrags);
    cscdVecFree(&w->sideDragRects);
}

/* --- CSCD_BACKEND_AUTO: which engine shows this window most cheaply on this computer -----------------------------------
 * As the window first shows, each engine there is shows a few frames, whole (as when everything moves: playing a video);
 * the processor time each present takes, for the whole program (a driver's own threads count too), is summed. The
 * cheapest is kept. Not wall time: OpenGL waits for the screen's refresh there, which costs nothing. */
#define TRIAL_FRAMES 6
/* (OpenGL first: on Windows a window that has had a Vulkan swap chain no longer takes OpenGL) */
static const CscdBackend TrialOrder[] = {CSCD_BACKEND_OPENGL, CSCD_BACKEND_VULKAN, CSCD_BACKEND_SOFTWARE};

static double processorTime(void) {
#ifdef _WIN32
    ULONG64 cycles = 0; /* (cycles: only compared with each other) */
    QueryProcessCycleTime(GetCurrentProcess(), &cycles);
    return (double)cycles;
#else
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

static void trialUse(CscdWindow* w, CscdBackend b) {
    cscdPresenterFree(w->presenter);
    w->presenter = windowPresenterNew(w, windowSurface(w), b, w->clientW, w->clientH);
    cscdImageFini(&w->frame); /* (painted whole for the new presenter) */
    memset(&w->frame, 0, sizeof w->frame);
    w->dirty = true;
}

static void trialStep(CscdWindow* w, double cost) {
    const int k = w->trial - 1;
    if (w->trialFrames++ > 0) w->trialCost += cost; /* (the first frame of each: setting up) */
    if (w->trialFrames <= TRIAL_FRAMES) {
        w->dirty = true;
        return;
    }
    if (w->presenter && cscdPresenterBackend(w->presenter) == TrialOrder[k] && (w->bestCost < 0 || w->trialCost < w->bestCost)) {
        w->bestCost = w->trialCost;
        w->best = TrialOrder[k];
    }
    w->trialFrames = 0;
    w->trialCost = 0;
    if (k + 1 < (int)CSCD_COUNT(TrialOrder)) {
        w->trial = k + 2;
        trialUse(w, TrialOrder[k + 1]);
        return;
    }
    w->trial = -1;
    w->requested = w->bestCost >= 0 ? w->best : CSCD_BACKEND_SOFTWARE;
    trialUse(w, w->requested);
}

bool cscdWindowBackendSettled(const CscdWindow* w) { return w->trial <= 0 && (w->trial < 0 || w->requested != CSCD_BACKEND_AUTO); }

void windowRender(CscdWindow* w) {
    if (!w->hwnd || w->rendering || w->background || w->clientW <= 0 || w->clientH <= 0) return;
    w->rendering = true;
    w->dirty = false;
    if (w->onFrame) w->onFrame(w->onFrameUser);
    cscdViewUpdate(w->view);
    if (w->titleFromDocument) {
        CscdStr t = cscdViewTitle(w->view);
        if (t.len && !cscdStrEq(&t, &w->lastTitle)) {
            cscdStrSet(&w->lastTitle, cscdStrSv(&t));
            windowShowTitle(w, cscdStrC(&t));
        }
        cscdStrFree(&t);
    }
#ifdef _WIN32
    /* On Windows a window that has had a Vulkan swap chain shows nothing else after it, neither OpenGL nor the system's
     * software path (the window stayed on its last frame): when Auto's first engine is Vulkan, it is kept, untried. */
    if (!w->trial && w->requested == CSCD_BACKEND_AUTO && w->presenter && cscdPresenterBackend(w->presenter) == CSCD_BACKEND_VULKAN) w->trial = -1;
#endif
    if (!w->trial && w->requested == CSCD_BACKEND_AUTO && w->presenter && !w->makePresenter && w->shown && cscdViewFeature(w->view, CSCD_FEATURE_GPU)) {
        w->trial = 1;
        w->bestCost = -1.0;
        trialUse(w, TrialOrder[0]);
    }
    /* the GPU feature: the presenter follows it when it changes (a GPU one, or the system's software path) */
    const bool gpu = cscdViewFeature(w->view, CSCD_FEATURE_GPU) && w->requested != CSCD_BACKEND_SOFTWARE;
    if (w->presenter && !w->makePresenter && gpu != w->gpuApplied) {
        w->gpuApplied = gpu;
        const bool onGpu = cscdPresenterBackend(w->presenter) != CSCD_BACKEND_SOFTWARE;
        if (gpu != onGpu) {
            cscdPresenterFree(w->presenter);
            w->presenter = windowPresenterNew(w, windowSurface(w), gpu ? w->requested : CSCD_BACKEND_SOFTWARE, w->clientW, w->clientH);
        }
    }
    bool fresh = w->frame.width != w->clientW || w->frame.height != w->clientH;
    if (fresh) {
        cscdImageFini(&w->frame);
        cscdImageInit(&w->frame, w->clientW, w->clientH);
    }
    cscdViewPaint(w->view, &w->frame, !fresh); /* (the frame keeps what was painted: only what changed is painted again) */
    syncPopups(w);
#ifdef CSCD_PLATFORM_MOBILE
    tellBackdrop(w);
#endif
#ifdef __ANDROID__
    tellSideDrags(w);
#endif
    CscdImage under;
    memset(&under, 0, sizeof under);
    const CscdIRect stats = cscdViewFeature(w->view, CSCD_FEATURE_STATS) ? drawStats(w, &under) : cscdIRect(0, 0, 0, 0);
    /* what changed since the frame shown last: the GPU presenters upload only that (with where the
     * stats panel is now and was before) */
    CscdIRectVec damage = {0};
    if (w->presenter) {
        w->presenter->partial = !fresh && cscdViewPaintDamage(w->view, &damage) && w->trial <= 0; /* (on trial: whole frames) */
        if (stats.w > 0) cscdVecPush(&damage, stats);
        if (w->lastOverlay.w > 0) cscdVecPush(&damage, w->lastOverlay);
        w->presenter->damage = damage.items;
        w->presenter->damageCount = damage.len;
    }
    w->lastOverlay = stats;
    const double before = w->trial > 0 ? processorTime() : 0.0;
    if (w->presenter && !cscdPresenterPresent(w->presenter, &w->frame)) {
        /* the GPU path failed (device lost): fall back to software (an app makes its own again) */
        cscdPresenterFree(w->presenter);
        w->presenter = windowPresenterNew(w, windowSurface(w), CSCD_BACKEND_SOFTWARE, w->clientW, w->clientH);
        if (w->presenter) cscdPresenterPresent(w->presenter, &w->frame); /* (none without a surface) */
    }
    if (stats.w > 0) restoreUnderStats(w, &under, stats);
    if (w->presenter) {
        w->presenter->partial = false;
        w->presenter->damage = NULL;
        w->presenter->damageCount = 0;
    }
    cscdVecFree(&damage);
    if (w->trial > 0) trialStep(w, processorTime() - before);
    w->rendering = false;
}

int cscdWindowRun(CscdWindow* w) {
    while (cscdWindowPump(w, true)) {}
    return 0;
}

/* ---- the clipboard as X11 and Wayland hand it out ---------------------------------------------------------------- */

void clipDataClear(ClipData* d) {
    for (size_t i = 0; i < d->len; ++i) {
        cscdStrFree(&d->items[i].mime);
        cscdVecFree(&d->items[i].data);
    }
    d->len = 0;
}

void clipDataFree(ClipData* d) {
    clipDataClear(d);
    cscdVecFree(d);
}

const ClipFormat* clipDataFind(const ClipData* d, const char* mime) {
    for (size_t i = 0; i < d->len; ++i)
        if (cscdStrEqC(&d->items[i].mime, mime)) return &d->items[i];
    return NULL;
}

static void clipDataAdd(ClipData* d, const char* mime, const void* bytes, size_t n) {
    ClipFormat f;
    memset(&f, 0, sizeof f);
    f.mime = cscdStrDupC(mime);
    if (n) cscdVecAppend(&f.data, (const uint8_t*)bytes, n);
    cscdVecPush(d, f);
}

static void clipDataAddText(ClipData* d, CscdSv text) {
    static const char* const kText[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"};
    for (size_t i = 0; i < CSCD_COUNT(kText); ++i) clipDataAdd(d, kText[i], text.ptr, text.len);
}

void clipDataSetText(ClipData* d, CscdSv text) {
    clipDataClear(d);
    clipDataAddText(d, text);
}

void clipDataSetHtml(ClipData* d, CscdSv html, CscdSv text) {
    clipDataClear(d);
    clipDataAdd(d, "text/html", html.ptr, html.len);
    clipDataAddText(d, text);
}

void clipDataSetImage(ClipData* d, const CscdImage* image) {
    clipDataClear(d);
    CscdBytes png = {0};
    cscdEncodePNG(image, &png);
    clipDataAdd(d, "image/png", png.items, png.len);
    cscdVecFree(&png);
}

void clipDataSetFiles(ClipData* d, const CscdStrVec* paths) {
    clipDataClear(d);
    CscdStr uris = {0}, gnome = {0}, text = {0};
    cscdStrAppendC(&gnome, "copy");
    for (size_t i = 0; i < paths->len; ++i) {
        CscdStr uri = {0};
        cscdStrAppendC(&uri, "file://");
        const CscdStr* p = &paths->items[i];
        for (size_t k = 0; k < p->len; ++k) {
            const unsigned char c = (unsigned char)p->ptr[k];
            if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') cscdStrAppendChar(&uri, (char)c);
            else {
                char hex[4];
                snprintf(hex, sizeof hex, "%%%02X", c);
                cscdStrAppendC(&uri, hex);
            }
        }
        cscdStrAppend(&uris, cscdStrSv(&uri));
        cscdStrAppendC(&uris, "\r\n");
        cscdStrAppendChar(&gnome, '\n');
        cscdStrAppend(&gnome, cscdStrSv(&uri));
        if (i) cscdStrAppendChar(&text, '\n');
        cscdStrAppend(&text, cscdStrSv(p));
        cscdStrFree(&uri);
    }
    clipDataAdd(d, "text/uri-list", uris.ptr, uris.len);
    clipDataAdd(d, "x-special/gnome-copied-files", gnome.ptr, gnome.len);
    clipDataAddText(d, cscdStrSv(&text));
    cscdStrFree(&uris);
    cscdStrFree(&gnome);
    cscdStrFree(&text);
}

static int hexDigit(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

void clipFilePaths(CscdSv list, CscdStrVec* out) {
    size_t start = 0;
    while (start < list.len) {
        size_t end = cscdSvFindChar(list, '\n', start);
        if (end == CSCD_NPOS) end = list.len;
        CscdSv line = cscdSvSub(list, start, end - start);
        start = end + 1;
        while (line.len && (line.ptr[line.len - 1] == '\r' || line.ptr[line.len - 1] == '\0')) --line.len;
        if (!cscdSvStartsC(line, "file://")) continue;
        const size_t slash = cscdSvFindChar(line, '/', 7); /* past the host name, usually empty */
        if (slash == CSCD_NPOS) continue;
        CscdStr path = {0};
        for (size_t i = slash; i < line.len; ++i) {
            if (line.ptr[i] == '%' && i + 2 < line.len && isxdigit((unsigned char)line.ptr[i + 1]) && isxdigit((unsigned char)line.ptr[i + 2])) {
                cscdStrAppendChar(&path, (char)(hexDigit(line.ptr[i + 1]) * 16 + hexDigit(line.ptr[i + 2])));
                i += 2;
            } else cscdStrAppendChar(&path, line.ptr[i]);
        }
        cscdVecPush(out, path);
    }
}
