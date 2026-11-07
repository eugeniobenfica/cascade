/* A presenter of the app's in a real window: what CscdWindowOptions.presenter promises. Only with a
 * screen, so only when asked for (CASCADE_TEST_WINDOW). The presenter here wraps cascade's software
 * one, which also proves that the surface the window hands over is what cscdPresenterNew takes. */
#include "cascade/cascade.h"
#include "cascade/window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

typedef struct Host {
    int made, freed, presented, resized;
    int failNext;                   /* the next present says the device is lost */
    int width, height;              /* as last told */
    int frameWidth, frameHeight;    /* of the last frame */
    uint32_t emptyPixel;            /* the frame's top left corner */
    bool refuse;                    /* the host makes nothing: cascade's own presenter comes */
    bool sawPartial;
} Host;

typedef struct Wrapper { CscdPresenter base; CscdPresenter* inner; Host* host; } Wrapper;

static CscdBackend wrapperBackend(const CscdPresenter* p) { (void)p; return CSCD_BACKEND_OPENGL; }
static const char* wrapperDescription(const CscdPresenter* p) { (void)p; return "the app's"; }
static bool wrapperPresent(CscdPresenter* bp, const CscdImage* frame) {
    Wrapper* p = (Wrapper*)bp;
    Host* h = p->host;
    if (h->failNext) { h->failNext = 0; return false; }
    ++h->presented;
    h->frameWidth = frame->width;
    h->frameHeight = frame->height;
    h->emptyPixel = cscdImageRow(frame, 0)[0];
    h->sawPartial = h->sawPartial || bp->partial;
    return cscdPresenterPresent(p->inner, frame);
}
static void wrapperResize(CscdPresenter* bp, int width, int height) {
    Wrapper* p = (Wrapper*)bp;
    ++p->host->resized;
    p->host->width = width;
    p->host->height = height;
    cscdPresenterResize(p->inner, width, height);
}
static void wrapperDestroy(CscdPresenter* bp) {
    Wrapper* p = (Wrapper*)bp;
    ++p->host->freed;
    cscdPresenterFree(p->inner);
    free(p);
}
static const CscdPresenterVtbl wrapperTable = {wrapperBackend, wrapperPresent, wrapperResize, wrapperDescription, wrapperDestroy};

static CscdPresenter* makePresenter(void* user, void* surface, int width, int height) {
    Host* h = user;
    CHECK(surface != NULL && width > 0 && height > 0);
    ++h->made;
    h->width = width;
    h->height = height;
    if (h->refuse) return NULL;
    Wrapper* p = calloc(1, sizeof *p);
    p->base.vt = &wrapperTable;
    p->inner = cscdPresenterNew(surface, CSCD_BACKEND_SOFTWARE, width, height);
    p->host = h;
    return &p->base;
}

/* pumps until the host has seen `presented` frames; false after five seconds of none */
static bool waitFor(CscdWindow* w, const Host* h, int presented) {
    const time_t end = time(NULL) + 5;
    while (h->presented < presented && time(NULL) < end) {
        cscdWindowPump(w, false);
        cscdViewInvalidate(cscdWindowView(w));
    }
    return h->presented >= presented;
}

static CscdWindow* openWindow(Host* h) {
    CscdWindowOptions o = cscdWindowOptions();
    o.title = "presenter";
    o.width = 320;
    o.height = 200;
    o.presenter = makePresenter;
    o.presenterUser = h;
    return cscdWindowNew(&o);
}

static void testPresenter(void) {
    Host h = {0};
    CscdWindow* w = openWindow(&h);
    CHECK(cscdWindowNativeHandle(w) != NULL);
    CHECK(h.made == 1 && h.freed == 0);
    CscdView* v = cscdWindowView(w);
    cscdViewLoadHTML(v, CSCD_SV("<style>html,body{margin:0;background:transparent}#a{position:absolute;left:40px;top:40px;width:40px;height:40px;background:#7c3aed}</style><div id=a></div>"), CSCD_SV(""));
    cscdViewSetTransparent(v, true);
    CHECK(waitFor(w, &h, 2));
    CHECK(h.frameWidth == h.width && h.frameHeight == h.height);
    CHECK(h.emptyPixel == 0); /* the app's picture shows there */
    CHECK(strcmp(cscdWindowBackendDescription(w), "the app's") == 0);

    /* the GPU feature does not take the presenter away */
    cscdViewSetFeature(v, CSCD_FEATURE_GPU, false);
    const int before = h.presented;
    CHECK(waitFor(w, &h, before + 2));
    CHECK(h.made == 1 && h.freed == 0);
    cscdViewSetFeature(v, CSCD_FEATURE_GPU, true);

    /* a lost device: the presenter goes, and the app is asked for a new one */
    h.failNext = 1;
    const int shown = h.presented;
    CHECK(waitFor(w, &h, shown + 2));
    CHECK(h.made == 2 && h.freed == 1);

    /* the size follows */
    const int resized = h.resized;
    cscdWindowResize(w, 400, 260); /* (on some platforms, before this returns) */
    const time_t end = time(NULL) + 5;
    while (h.resized == resized && time(NULL) < end) cscdWindowPump(w, false);
    CHECK(h.resized > resized);
    const int after = h.presented;
    CHECK(waitFor(w, &h, after + 2));
    CHECK(h.frameWidth == h.width && h.frameHeight == h.height);

    cscdWindowFree(w);
    CHECK(h.made == h.freed); /* each one destroyed, with the window */
}

static void testRefused(void) {
    Host h = {0};
    h.refuse = true;
    CscdWindow* w = openWindow(&h);
    CHECK(h.made == 1);
    CHECK(strcmp(cscdWindowBackendDescription(w), "none") != 0); /* a window always shows */
    cscdWindowFree(w);
    CHECK(h.freed == 0);
}

int main(void) {
    if (!getenv("CASCADE_TEST_WINDOW")) {
        puts("no window asked for (CASCADE_TEST_WINDOW)");
        return 0;
    }
    testPresenter();
    testRefused();
    if (!failures) puts("ok");
    return failures ? 1 : 0;
}
