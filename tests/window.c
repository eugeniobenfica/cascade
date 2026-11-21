/* A presenter of the app's in a real window: what CscdWindowOptions.presenter promises. Only with a
 * screen, so only when asked for (CASCADE_TEST_WINDOW). The presenter here wraps cascade's software
 * one, which also proves that the surface the window hands over is what cscdPresenterNew takes. */
#include "cascade/cascade.h"
#include "cascade/window.h"

#include "platform/common/platformInternal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

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
    CscdWindowOptions* o = cscdWindowOptionsNew();
    cscdWindowOptionsSetTitle(o, "presenter");
    cscdWindowOptionsSetSize(o, 320, 200);
    cscdWindowOptionsSetPresenter(o, makePresenter, h);
    CscdWindow* w = cscdWindowNew(o);
    cscdWindowOptionsFree(o);
    return w;
}

/* the options a program sets through functions (the struct is opaque to it): strings copied, NULL as NULL */
static void testOptions(void) {
    CscdWindowOptions* o = cscdWindowOptionsNew();
    CHECK(!strcmp(o->title, "cascade") && o->width == 1024 && o->visible && o->liveReload == -1);
    char title[16] = "Notes";
    cscdWindowOptionsSetTitle(o, title);
    title[0] = 'X';
    cscdWindowOptionsSetPage(o, "ui/index.html");
    cscdWindowOptionsSetPage(o, NULL);
    cscdWindowOptionsSetSize(o, 960, 640);
    cscdWindowOptionsSetMinSize(o, 300, 200);
    cscdWindowOptionsSetVisible(o, false);
    cscdWindowOptionsSetLiveReload(o, 0);
    CHECK(!strcmp(o->title, "Notes") && !o->page && o->width == 960 && o->height == 640 && o->minWidth == 300 &&
          o->minHeight == 200 && !o->visible && o->liveReload == 0);
    cscdWindowOptionsFree(o);
    cscdWindowOptionsFree(NULL);
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

/* A menu at the window's corner, in a window of its own past the client area where the platform has them; a press
 * in that window runs its row. */
static int chosen;
static void choose(void* user) { chosen = (int)(intptr_t)user; }

static void testPopups(void) {
    Host h = {0};
    CscdWindow* w = openWindow(&h);
    CscdView* v = cscdWindowView(w);
    cscdViewLoadHTML(v, CSCD_SV("<style>body{margin:0;background:#fff}</style><p>page</p>"), CSCD_SV(""));
    CHECK(waitFor(w, &h, 1));
    CscdIRect area;
    if (!platformPopupArea(w, &area)) {
        puts("popups: the platform keeps them in the window");
        cscdWindowFree(w);
        return;
    }
    CscdMenu m = {0};
    cscdMenuAdd(&m, cscdMenuItem(CSCD_SV("First"), choose, (void*)(intptr_t)1, CSCD_SV("")));
    cscdMenuAdd(&m, cscdMenuItem(CSCD_SV("Second"), choose, (void*)(intptr_t)2, CSCD_SV("")));
    cscdViewShowMenu(v, m, 300, 190, 0);
    CHECK(waitFor(w, &h, h.presented + 1));
    CHECK(w->popups.len == 1 && w->popups.items[0].native != NULL);
    if (w->popups.len != 1) {
        cscdWindowFree(w);
        return;
    }
    const CscdIRect r = w->popups.items[0].rect;
    CHECK(r.x + r.w > w->clientW && r.y + r.h > w->clientH); /* (past the client area, not pushed in) */
    CscdElement* second = NULL;
    CscdPtrVec rows = {0};
    cscdViewQuerySelectorAll(v, CSCD_SV(".cscd-menu-item"), &rows);
    CHECK(rows.len == 2);
    second = rows.len == 2 ? (CscdElement*)rows.items[1] : NULL;
    cscdVecFree(&rows);
#ifdef _WIN32
    HWND popup = FindWindowExW(NULL, NULL, L"cascade-popup", NULL);
    CHECK(popup && IsWindowVisible(popup) && GetWindow(popup, GW_OWNER) == (HWND)cscdWindowNativeHandle(w));
    RECT pr;
    POINT origin = {0, 0};
    ClientToScreen((HWND)cscdWindowNativeHandle(w), &origin);
    CHECK(popup && GetWindowRect(popup, &pr) && pr.left == origin.x + r.x && pr.top == origin.y + r.y && pr.right - pr.left == r.w);
    if (popup && second) {
        /* a click on its second row, in the popup's own coordinates */
        const CscdRect sr = cscdViewElementRect(v, second);
        const int lx = (int)((sr.x + 10) * w->dpr) - r.x, ly = (int)((sr.y + sr.h / 2) * w->dpr) - r.y;
        chosen = 0;
        SendMessageW(popup, WM_MOUSEMOVE, 0, MAKELPARAM(lx, ly));
        SendMessageW(popup, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(lx, ly));
        SendMessageW(popup, WM_LBUTTONUP, 0, MAKELPARAM(lx, ly));
        const time_t end = time(NULL) + 5;
        while ((chosen == 0 || w->popups.len) && time(NULL) < end) {
            cscdWindowPump(w, false);
            cscdViewInvalidate(v);
        }
        CHECK(chosen == 2);
        CHECK(w->popups.len == 0 && !IsWindow(popup)); /* (closed with the menu) */
    }
#else
    /* (X11, macOS: the windows go with the menu) */
    (void)second;
    cscdViewCloseMenus(v);
    const time_t end = time(NULL) + 5;
    while (w->popups.len && time(NULL) < end) {
        cscdWindowPump(w, false);
        cscdViewInvalidate(v);
    }
    CHECK(w->popups.len == 0);
#endif
    cscdWindowFree(w);
}

/* The system's clipboard, format by format, there and back (only when asked for: it replaces what the user copied). */
static void testClipboard(void) {
    Host h = {0};
    CscdWindow* w = openWindow(&h);
    CscdView* v = cscdWindowView(w);
    CHECK(waitFor(w, &h, 1));
    /* text */
    CHECK(cscdViewCopyText(v, CSCD_SV("plain \xC3\xA9t\xC3\xA9")));
    CscdStr s = {0};
    cscdViewClipboardText(v, &s);
    CHECK(cscdStrEqC(&s, "plain \xC3\xA9t\xC3\xA9"));
    cscdStrClear(&s);
    /* HTML, with its text */
    CHECK(cscdViewCopyHtml(v, CSCD_SV("<b>bold</b> \xC3\xA9"), CSCD_SV("bold \xC3\xA9")));
    cscdViewClipboardHtml(v, &s);
    CHECK(cscdStrEqC(&s, "<b>bold</b> \xC3\xA9"));
    cscdStrClear(&s);
    cscdViewClipboardText(v, &s);
    CHECK(cscdStrEqC(&s, "bold \xC3\xA9"));
    cscdStrClear(&s);
    /* an image: an opaque pixel and a half transparent one come back as they went */
    CscdImage img, back;
    cscdImageInit(&img, 3, 2);
    cscdImageRow(&img, 0)[0] = 0xFF2040C0u;
    cscdImageRow(&img, 1)[2] = 0x80400000u; /* (premultiplied: half of 0x80 red) */
    CHECK(cscdViewCopyImage(v, &img));
    if (cscdViewClipboardImage(v, &back)) {
        CHECK(back.width == 3 && back.height == 2);
        CHECK(cscdImageRow(&back, 0)[0] == 0xFF2040C0u);
        const uint32_t p = cscdImageRow(&back, 1)[2];
        CHECK((p >> 24) == 0x80 && ((p >> 16) & 255) >= 0x3E && ((p >> 16) & 255) <= 0x42);
        cscdImageFini(&back);
    } else {
        CHECK(!"the image came back");
    }
    cscdImageFini(&img);
    /* files */
    CscdStrVec files = {0}, got = {0};
    cscdVecPush(&files, cscdStrDupC(
#ifdef _WIN32
        "C:\\cascade test\\a b.txt"
#else
        "/tmp/cascade test/a b.txt"
#endif
        ));
    CHECK(cscdViewCopyFiles(v, &files));
    cscdViewClipboardFiles(v, &got);
    CHECK(got.len == 1 && got.len == files.len && cscdStrEq(&got.items[0], &files.items[0]));
    for (size_t i = 0; i < files.len; ++i) cscdStrFree(&files.items[i]);
    for (size_t i = 0; i < got.len; ++i) cscdStrFree(&got.items[i]);
    cscdVecFree(&files);
    cscdVecFree(&got);
    cscdStrFree(&s);
    cscdWindowFree(w);
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
    testOptions();
    if (!getenv("CASCADE_TEST_WINDOW")) {
        puts("no window asked for (CASCADE_TEST_WINDOW)");
        return failures ? 1 : 0;
    }
    testPresenter();
    testPopups();
    if (getenv("CASCADE_TEST_CLIPBOARD")) testClipboard();
    testRefused();
    if (!failures) puts("ok");
    return failures ? 1 : 0;
}
