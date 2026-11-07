/* Draws docs/assets/screens/quickStart.png: the README's quick start (its page, its sheet and its
 * painter, the same text) as its window shows it, at twice the pixels. Built against cascade and run
 * from the repository's root:
 *
 *   cc -std=c11 -Iinclude docs/assets/src/quickStart.c build/libcascade.a -lm -o quickStart && ./quickStart
 *
 * (On Windows, add the system libraries the SDK's CMake package lists.) cscdSavePNG writes stored
 * zlib blocks, so the file comes out large: recompress it losslessly (oxipng, optipng) before it goes
 * into the repository. */
#include "cascade/cascade.h"

#include <stdio.h>
#include <string.h>

/* the README's ui/index.html and ui/app.css, as they are there */
static const char kIndex[] =
    "<!-- ui/index.html -->\n"
    "<link rel=\"stylesheet\" href=\"app.css\">\n"
    "<main>\n"
    "  <h1>Hello, cascade</h1>\n"
    "  <button id=\"save\" class=\"accent\">Save</button>\n"
    "  <p id=\"status\">Nothing saved yet.</p>\n"
    "  <div id=\"chart\"></div>\n"
    "</main>\n";

static const char kAppCss[] =
    "/* ui/app.css */\n"
    "main   { max-width: 480px; margin: 40px auto; font-family: sans-serif; }\n"
    ".accent { background: #8b5cf6; color: white; border: 0; border-radius: 6px; padding: 8px 16px; }\n"
    "#chart { height: 120px; }                 /* a painter draws in the box the page gives it */\n";

static bool pages(void* user, CscdSv path, CscdStr* out) {
    (void)user;
    if (!cscdSvEnds(path, CSCD_SV("app.css"))) return false;
    cscdStrSet(out, cscdSvMake(kAppCss, sizeof kAppCss - 1));
    return true;
}

/* the README's painter */
static void paintChart(void* user, CscdCanvas* c, float w, float h) {
    (void)user;
    CscdPath wave = {0};
    cscdPathMoveTo(&wave, 0, h / 2);
    cscdPathCubicTo(&wave, w / 3, 0, w * 2 / 3, h, w, h / 2);
    cscdCanvasStrokePath(c, &wave, cscdRgb(139, 92, 246), 2);
    cscdPathFree(&wave);
}

int main(void) {
    const int width = 560, height = 400, scale = 2;
    cscdAppSetResourceLoader(pages, NULL);
    CscdView* view = cscdViewNew();
    cscdViewLoadHTML(view, cscdSvMake(kIndex, sizeof kIndex - 1), CSCD_SV("ui"));
    CscdPainter chart = {paintChart, NULL, NULL};
    cscdElementSetPainter(cscdViewGet(view, CSCD_SV("#chart")), chart);
    cscdViewSetViewport(view, width, height, scale);
    CscdImage img;
    cscdImageInit(&img, width * scale, height * scale);
    cscdViewPaint(view, &img, false);
    const bool saved = cscdSavePNG(CSCD_SV("docs/assets/screens/quickStart.png"), &img);
    cscdImageFini(&img);
    cscdViewFree(view);
    if (!saved) fprintf(stderr, "cannot write docs/assets/screens/quickStart.png\n");
    return saved ? 0 : 1;
}
