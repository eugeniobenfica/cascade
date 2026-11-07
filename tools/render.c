/* cascade-render: renders an HTML file to a PNG without a window.
 *   cascade-render page.html out.png [width] [height] [dpr]
 * A height of 0 renders the full page. */
#include "cascade/cascade.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

static double millis(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }

int main(int argc, char** argv) {
#ifdef _WIN32
    /* the arguments as UTF-8, the library's paths (main's are in the system's code page) */
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv && wargc == argc) {
        for (int i = 0; i < argc; ++i) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
            char* a = (char*)malloc(n > 0 ? (size_t)n : 1);
            if (a && WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, a, n, NULL, NULL) > 0) argv[i] = a; /* (kept until the end) */
        }
    }
    if (wargv) LocalFree(wargv);
#endif
    if (argc < 3) {
        fprintf(stderr, "usage: cascade-render page.html out.png [width=800] [height=0] [dpr=1]\n");
        return 1;
    }
    float w = argc > 3 ? (float)atof(argv[3]) : 800;
    float h = argc > 4 ? (float)atof(argv[4]) : 0;
    float dpr = argc > 5 ? (float)atof(argv[5]) : 1;
    double t0 = millis();
    CscdView* view = cscdViewNew();
    if (!cscdViewLoadFile(view, cscdSv(argv[1]))) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        cscdViewFree(view);
        return 1;
    }
    cscdViewSetViewport(view, w, h > 0 ? h : 600, dpr);
    cscdViewUpdate(view);
    if (h <= 0) {
        h = cscdMaxf(1.f, ceilf(cscdViewContentHeight(view)));
        cscdViewSetViewport(view, w, h, dpr);
        cscdViewUpdate(view);
    }
    double t1 = millis();
    CscdImage img;
    cscdImageInit(&img, (int)ceilf(w * dpr), (int)ceilf(h * dpr));
    cscdViewPaint(view, &img, false);
    double t2 = millis();
    int status = 0;
    if (!cscdSavePNG(cscdSv(argv[2]), &img)) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        status = 1;
    } else {
        const CscdFrameStats st = cscdViewFrameStats(view);
        printf("%s: %dx%d, load and layout %.1f ms (last style pass %.1f ms: %d computed, %d reused; last layout %.1f ms), paint %.1f ms\n",
               argv[2], img.width, img.height, t1 - t0, st.styleMs, st.stylesComputed, st.stylesReused, st.layoutMs, t2 - t1);
    }
    cscdImageFini(&img);
    cscdViewFree(view);
    return status;
}
