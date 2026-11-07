/* cascade gallery: an HTML / CSS interface driven from C.
 *   cascade-gallery [--vulkan | --opengl | --software] */
#include "cascade/cascade.h"
#include "cascade/window.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h> /* (--screenshot) */
#endif

typedef struct App {
    CscdWindow* win;
    CscdView* ui;
    int clicks;
    double t0;
    CscdElement* canvas;
    CscdElement* list;
    CscdElement* input;
    const char* screenshot;
} App;

/* (cscdViewGet: never NULL; a missing element is reported, and changing it does nothing) */
static CscdElement* byId(App* app, const char* id) { return cscdViewGet(app->ui, cscdSv(id)); }

static void setTextf(CscdElement* e, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    cscdElementSetText(e, cscdSv(buf));
}

/* ---- tabs ------------------------------------------------------------------------------------------ */

static void onTab(void* user, CscdEvent* ev) {
    App* app = (App*)user;
    CscdElement* tab = ev->currentTarget;
    CscdPtrVec all = {0};
    cscdViewQuerySelectorAll(app->ui, CSCD_SV(".tab"), &all);
    for (size_t i = 0; i < all.len; ++i) cscdElementSetClass(all.items[i], CSCD_SV("active"), all.items[i] == tab);
    all.len = 0;
    CscdStr want = cscdStrf("page-%.*s", (int)cscdElementAttrC(tab, "data-page").len, cscdElementAttrC(tab, "data-page").ptr);
    cscdViewQuerySelectorAll(app->ui, CSCD_SV(".page"), &all);
    for (size_t i = 0; i < all.len; ++i) cscdElementSetClass(all.items[i], CSCD_SV("hidden"), !cscdSvEq(cscdElementId(all.items[i]), cscdStrSv(&want)));
    cscdStrFree(&want);
    cscdVecFree(&all);
}

static void onTheme(void* user, CscdEvent* ev) {
    App* app = (App*)user;
    cscdElementSetClass(cscdDocumentBody(cscdViewDocument(app->ui)), CSCD_SV("dark"), cscdElementChecked(ev->currentTarget));
}

static void onClicker(void* user, CscdEvent* ev) {
    App* app = (App*)user;
    ++app->clicks;
    setTextf(ev->currentTarget, "Clicked %d %s", app->clicks, app->clicks == 1 ? "time" : "times");
    cscdElementSetText(byId(app, "status"), CSCD_SV("Button clicked"));
}

static void onOpacity(void* user, CscdEvent* ev) {
    App* app = (App*)user;
    CscdStr v = cscdElementValue(ev->currentTarget);
    setTextf(byId(app, "opacityValue"), "%s%%", cscdStrC(&v));
    cscdStrFree(&v);
}

static void onChip(void* user, CscdEvent* ev) {
    (void)user;
    cscdElementToggleClass(ev->currentTarget, CSCD_SV("active"));
}

static void onTreeItem(void* user, CscdEvent* ev) {
    App* app = (App*)user;
    CscdElement* item = ev->currentTarget;
    CscdPtrVec all = {0};
    cscdViewQuerySelectorAll(app->ui, CSCD_SV(".tree .item"), &all);
    for (size_t i = 0; i < all.len; ++i) cscdElementSetClass(all.items[i], CSCD_SV("selected"), all.items[i] == item);
    cscdVecFree(&all);
    CscdStr text = cscdStrDupC("Selected ");
    cscdNodeTextContent(&item->node, &text);
    cscdElementSetText(byId(app, "status"), cscdStrSv(&text));
    cscdStrFree(&text);
}

/* ---- a small to-do list built with DOM calls ------------------------------------------------------ */

static void refreshCount(App* app) {
    CscdPtrVec items = {0};
    cscdElementQuerySelectorAll(app->list, CSCD_SV("li"), &items);
    setTextf(byId(app, "count"), "%zu", items.len);
    cscdVecFree(&items);
}

static void onTaskChecked(void* user, CscdEvent* ev) {
    CscdElement* li = (CscdElement*)user;
    cscdElementSetClass(li, CSCD_SV("done"), cscdElementChecked(ev->currentTarget));
}

typedef struct RemoveTask { App* app; CscdElement* li; } RemoveTask;

static void removeTask(void* user) {
    RemoveTask* r = (RemoveTask*)user;
    cscdElementRemove(r->li);
    refreshCount(r->app);
}

static void onTaskRemove(void* user, CscdEvent* ev) {
    App* app = (App*)user;
    RemoveTask* r = CSCD_NEW(RemoveTask);
    r->app = app;
    r->li = cscdNodeParentElement(&ev->currentTarget->node);
    CscdAction a = {removeTask, r, cscdFree};
    cscdViewSetTimeout(app->ui, a, 0);
}

static void addTask(App* app, CscdSv text) {
    if (!text.len) return;
    CscdElement* li = cscdElementAppendElement(app->list, CSCD_SV("li"));
    CscdElement* box = cscdElementAppendElement(li, CSCD_SV("input"));
    cscdElementSetAttribute(box, CSCD_SV("type"), CSCD_SV("checkbox"));
    cscdElementSetText(cscdElementAppendElement(li, CSCD_SV("span")), text);
    CscdElement* x = cscdElementAppendElement(li, CSCD_SV("button"));
    cscdElementAddClass(x, CSCD_SV("x"));
    cscdElementSetText(x, CSCD_SV("\xC3\x97"));
    cscdElementOn(box, CSCD_SV("change"), onTaskChecked, li);
    cscdElementOnClick(x, onTaskRemove, app);
    refreshCount(app);
}

static void addFromInput(App* app) {
    CscdStr v = cscdElementValue(app->input);
    addTask(app, cscdStrSv(&v));
    cscdStrFree(&v);
    cscdElementSetValue(app->input, CSCD_SV(""));
}

static void onTaskKey(void* user, CscdEvent* ev) {
    if (cscdStrEqC(&ev->key, "Enter")) addFromInput((App*)user);
}

static void onAddTask(void* user, CscdEvent* ev) {
    (void)ev;
    addFromInput((App*)user);
}

/* ---- custom painting: an animated waveform and keyframes, the kind of thing Skia used to draw ---- */

static void paintTimeline(void* user, CscdCanvas* c, float w, float h) {
    App* app = (App*)user;
    const float t0 = (float)app->t0;
    cscdCanvasFillRect(c, cscdRect(0, 0, w, h), cscdRgb(24, 25, 31));
    for (int i = 0; i <= 20; ++i) {
        float x = roundf(w * i / 20.f) + 0.5f;
        cscdCanvasDrawLine(c, cscdPoint(x, 0), cscdPoint(x, h), cscdRgba(255, 255, 255, i % 5 ? 12 : 30), 1, CSCD_CAP_BUTT);
    }
    CscdPath wave = {0};
    for (int i = 0; i <= 400; ++i) {
        float x = w * i / 400.f;
        float y = h / 2 + sinf(i * 0.05f + t0 * 3) * h * 0.25f * sinf(i * 0.008f + 0.5f);
        if (i == 0) cscdPathMoveTo(&wave, x, y);
        else cscdPathLineTo(&wave, x, y);
    }
    CscdStrokeStyle st = cscdStrokeStyle(2);
    st.join = CSCD_JOIN_ROUND;
    CscdGradientStop stops[2] = {{0, cscdRgb(139, 124, 246)}, {1, cscdRgb(196, 181, 253)}};
    CscdShader sh = cscdShaderLinear(cscdPoint(0, 0), cscdPoint(w, 0), stops, 2);
    cscdCanvasStrokePathShader(c, &wave, &sh, &st);
    cscdShaderFini(&sh);
    cscdStrokeStyleFini(&st);
    cscdPathFree(&wave);
    for (int k = 0; k < 6; ++k) {
        float x = 40 + k * (w - 80) / 5;
        CscdPath d = {0};
        cscdPathMoveTo(&d, x, h - 30);
        cscdPathLineTo(&d, x + 7, h - 23);
        cscdPathLineTo(&d, x, h - 16);
        cscdPathLineTo(&d, x - 7, h - 23);
        cscdPathClose(&d);
        cscdCanvasFillPath(c, &d, cscdRgb(253, 203, 110), CSCD_FILL_NONZERO);
        cscdPathFree(&d);
    }
    float px = fmodf(t0 * 80, w);
    cscdCanvasFillRect(c, cscdRect(px, 0, 2, h), cscdRgb(225, 112, 85));
}

static void tick(void* user) {
    App* app = (App*)user;
    app->t0 += 1 / 30.0;
    if (!cscdElementClosest(app->canvas, CSCD_SV(".hidden"))) cscdViewInvalidate(app->ui);
    char buf[32];
    snprintf(buf, sizeof buf, "%f", fmod(app->t0 / 8, 1.0));
    cscdElementSetAttribute(byId(app, "progress"), CSCD_SV("value"), cscdSv(buf));
}

/* ---- --screenshot out.png [page] ------------------------------------------------------------------- */

static void takeScreenshot(void* user) {
    App* app = (App*)user;
#ifdef _WIN32
    /* captures what the presenter put on screen, then exits (used to test the backends) */
    HWND hwnd = (HWND)cscdWindowNativeHandle(app->win);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC wdc = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(wdc);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = rc.right;
    bi.bmiHeader.biHeight = -rc.bottom;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(hwnd, mem, 3 /* PW_CLIENTONLY | PW_RENDERFULLCONTENT */);
    CscdImage img;
    cscdImageInit(&img, rc.right, rc.bottom);
    memcpy(img.pixels, bits, (size_t)rc.right * rc.bottom * 4);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, wdc);
#else
    /* paints the view as the window shows it, then exits (smoke test) */
    CscdImage img;
    cscdImageInit(&img, (int)(cscdViewWidth(app->ui) * cscdViewDevicePixelRatio(app->ui)), (int)(cscdViewHeight(app->ui) * cscdViewDevicePixelRatio(app->ui)));
    cscdViewPaint(app->ui, &img, false);
#endif
    for (size_t i = 0; i < cscdImagePixelCount(&img); ++i) img.pixels[i] |= 0xFF000000u;
    cscdSavePNG(cscdSv(app->screenshot), &img);
    cscdImageFini(&img);
    printf("%s\n", cscdWindowBackendDescription(app->win));
    cscdWindowClose(app->win);
}

static void each(App* app, const char* selector, CscdEventFn fn) {
    CscdPtrVec all = {0};
    cscdViewQuerySelectorAll(app->ui, cscdSv(selector), &all);
    for (size_t i = 0; i < all.len; ++i) cscdElementOnClick(all.items[i], fn, app);
    cscdVecFree(&all);
}

int main(int argc, char** argv) {
    CscdBackend backend = CSCD_BACKEND_AUTO;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--vulkan")) backend = CSCD_BACKEND_VULKAN;
        else if (!strcmp(argv[i], "--opengl")) backend = CSCD_BACKEND_OPENGL;
        else if (!strcmp(argv[i], "--software")) backend = CSCD_BACKEND_SOFTWARE;
    }
    CscdWindowOptions opts = cscdWindowOptions();
    opts.title = "";              /* the page's <title> */
    opts.width = 1180;
    opts.height = 760;
    opts.backend = backend;
    opts.darkTitleBar = true;
    opts.page = "ui/index.html"; /* next to the program (in debug builds: in the sources, live reloaded) */
    App app;
    memset(&app, 0, sizeof app);
    app.win = cscdWindowNew(&opts);
    app.ui = cscdWindowView(app.win);

    cscdElementSetText(byId(&app, "backend"), cscdSv(cscdWindowBackendDescription(app.win)));
    fprintf(stderr, "cascade-gallery: presenting with %s\n", cscdWindowBackendDescription(app.win)); /* (the log says which: phones, CI) */
    each(&app, ".tab", onTab);
    cscdElementOn(byId(&app, "theme"), CSCD_SV("change"), onTheme, &app);
    cscdElementOnClick(byId(&app, "clicker"), onClicker, &app);
    cscdElementOn(byId(&app, "opacity"), CSCD_SV("input"), onOpacity, &app);
    each(&app, ".chip", onChip);
    each(&app, ".tree .item", onTreeItem);

    app.list = byId(&app, "tasks");
    app.input = byId(&app, "task");
    addTask(&app, CSCD_SV("Port the timeline panel"));
    addTask(&app, CSCD_SV("Replace QPainter calls"));
    cscdElementOn(app.input, CSCD_SV("keydown"), onTaskKey, &app);
    cscdElementOnClick(byId(&app, "addTask"), onAddTask, &app);

    /* swatches generated from C */
    CscdElement* sw = byId(&app, "swatches");
    for (int i = 0; i < 18; ++i) {
        CscdElement* s = cscdElementAppendElement(sw, CSCD_SV("div"));
        cscdElementAddClass(s, CSCD_SV("swatch"));
        char buf[64];
        snprintf(buf, sizeof buf, "hsl(%d, 65%%, 55%%)", i * 20);
        cscdElementSetStyle(s, CSCD_SV("background"), cscdSv(buf));
        setTextf(s, "%d\xC2\xB0", i * 20);
    }

    app.canvas = byId(&app, "timeline");
    CscdPainter painter = {paintTimeline, &app, NULL};
    cscdElementSetPainter(app.canvas, painter);
    cscdViewSetInterval(app.ui, cscdAction(tick, &app), 33);

    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--screenshot")) continue;
        app.screenshot = argv[i + 1];
        const char* page = i + 2 < argc ? argv[i + 2] : NULL;
        if (page) {
            CscdPtrVec tabs = {0};
            cscdViewQuerySelectorAll(app.ui, CSCD_SV(".tab"), &tabs);
            for (size_t k = 0; k < tabs.len; ++k)
                if (cscdSvEqC(cscdElementAttrC(tabs.items[k], "data-page"), page)) cscdElementClick(tabs.items[k]);
            cscdVecFree(&tabs);
        }
#ifdef _WIN32
        cscdViewSetTimeout(app.ui, cscdAction(takeScreenshot, &app), 1500);
#else
        cscdViewSetTimeout(app.ui, cscdAction(takeScreenshot, &app), 3000); /* (open long enough for the test to see the window on the screen) */
#endif
    }
    int code = cscdWindowRun(app.win);
    cscdWindowFree(app.win);
    return code;
}
