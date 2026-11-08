/* Draws docs/assets/screens/quickStart.png and quickStartRunning.png: the README's quick start (its
 * page, its sheet and its click handler, the same text) as its window shows it, before and after a
 * click on Start, at twice the pixels. Built against cascade and run from the repository's root:
 *
 *   cc -std=c11 -Iinclude docs/assets/src/quickStart.c build/libcascade.a -lm -o quickStart && ./quickStart
 *
 * (On Windows, add the system libraries the SDK's CMake package lists.) cscdSavePNG writes stored
 * zlib blocks, so the files come out large: recompress them losslessly (oxipng, optipng) before they
 * go into the repository. */
#include "cascade/cascade.h"

#include <stdio.h>
#include <string.h>

/* the README's ui/index.html and ui/app.css, as they are there */
static const char kIndex[] =
    "<!-- ui/index.html -->\n"
    "<link rel=\"stylesheet\" href=\"app.css\">\n"
    "<main class=\"timer\">\n"
    "  <div class=\"ring\"><h1>25:00</h1></div>\n"
    "  <button id=\"start\">Start</button>\n"
    "</main>\n";

static const char kAppCss[] =
    "/* ui/app.css */\n"
    "body   { margin: 0; background: #111116; color: #f4f3f8; font-family: sans-serif; }\n"
    ".timer { display: grid; place-items: center; gap: 28px; padding: 48px; }\n"
    ".ring  { width: 220px; aspect-ratio: 1; border-radius: 50%; border: 8px solid #26262f;\n"
    "         display: grid; place-items: center; }\n"
    ".ring h1 { margin: 0; font-size: 52px; }\n"
    "#start { background: #8b5cf6; color: white; border: 0; border-radius: 999px;\n"
    "         padding: 12px 40px; font-size: 16px; }\n"
    ".running .ring  { border-color: #8b5cf6; }          /* main.c switches this class */\n"
    ".running #start { background: #26262f; }\n";

static bool pages(void* user, CscdSv path, CscdStr* out) {
    (void)user;
    if (!cscdSvEnds(path, CSCD_SV("app.css"))) return false;
    cscdStrSet(out, cscdSvMake(kAppCss, sizeof kAppCss - 1));
    return true;
}

/* the README's handler */
static void onStart(void* user, CscdEvent* ev) {
    (void)ev;
    CscdView* ui = user;
    bool running = cscdElementToggleClass(cscdViewGet(ui, CSCD_SV(".timer")), CSCD_SV("running"));
    cscdElementSetText(cscdViewGet(ui, CSCD_SV("#start")), running ? CSCD_SV("Pause") : CSCD_SV("Start"));
}

static bool save(CscdView* view, int width, int height, int scale, const char* path) {
    cscdViewSetViewport(view, (float)width, (float)height, (float)scale);
    cscdViewUpdate(view);
    CscdImage img;
    cscdImageInit(&img, width * scale, height * scale);
    cscdViewPaint(view, &img, false);
    const bool saved = cscdSavePNG(cscdSvMake(path, strlen(path)), &img);
    cscdImageFini(&img);
    if (!saved) fprintf(stderr, "cannot write %s\n", path);
    return saved;
}

int main(void) {
    const int width = 480, height = 440, scale = 2;
    cscdAppSetResourceLoader(pages, NULL);
    CscdView* view = cscdViewNew();
    cscdViewLoadHTML(view, cscdSvMake(kIndex, sizeof kIndex - 1), CSCD_SV("ui"));
    cscdElementOnClick(cscdViewGet(view, CSCD_SV("#start")), onStart, view);
    bool ok = save(view, width, height, scale, "docs/assets/screens/quickStart.png");
    cscdElementClick(cscdViewGet(view, CSCD_SV("#start")));   /* what a click does: the handler runs */
    ok = save(view, width, height, scale, "docs/assets/screens/quickStartRunning.png") && ok;
    cscdViewFree(view);
    return ok ? 0 : 1;
}
