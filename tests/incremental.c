/* cascade-incremental-tests: the oracle of the light paths. Random changes and input go to a page; after each round
 * the view is brought up to date and painted over its last frame, as a window does (styles computed in place for a
 * hover, a text's block laid out again, an attribute repainted, a box scrolled, only the damage painted), and
 * viewCheckIncremental compares that with the whole pipeline from scratch: the boxes of every element and every pixel
 * must be the same. A path that forgets something shows here first.
 *   cascade-incremental-tests [first seed] [seeds] [rounds] */
#include "cascade/cascade.h"
#include "../src/view/viewInternal.h" /* (internal: the oracle) */

#include <stdio.h>
#include <stdlib.h>

static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 20); }

static const char kPage[] =
    "<style>"
    "body { margin: 0; font: 14px sans-serif; background: #fafafa; }"
    ".bar { display: flex; gap: 6px; padding: 6px; background: #eee; }"
    ".bar button { padding: 4px 10px; border: 1px solid #999; border-radius: 6px; background: white; }"
    ".bar button:hover { background: #ddd; }"                  /* hover that only paints */
    ".bar button:active { background: #bbb; color: white; }"
    ".grow:hover { padding: 6px 14px; }"                        /* hover that moves things */
    ".card { margin: 6px; padding: 8px; border-radius: 10px; background: white; box-shadow: 0 2px 6px rgba(0,0,0,.3); }"
    ".card:hover { box-shadow: 0 4px 14px rgba(0,0,0,.4); }"
    ".card:hover .hint { color: #8b5cf6; }"
    ".grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 4px; }"
    ".cell { padding: 4px; background: #e8e8ff; }"
    ".cell:nth-child(2n) { background: #ffe8e8; }"
    ".on { background: #8b5cf6; color: white; }"
    ".wide { width: 160px; }"
    ".scroll { height: 70px; overflow: auto; border: 1px solid #ccc; margin: 6px; }"
    ".scroll p { margin: 4px; }"
    ".sticky { position: sticky; top: 0; background: #ffd; }"
    ".turn { transform: rotate(3deg); display: inline-block; }"
    ".fade { opacity: .6; }"
    "#count { display: inline-block; min-width: 40px; }"
    "</style>"
    "<div class=bar><button id=b1>One</button><button id=b2 class=grow>Two</button><button id=b3>Three</button>"
    "<span id=count>0</span><progress id=pr value=20 max=100></progress></div>"
    "<div class=card id=c1><b>Title</b> <span class=hint id=h1>hint</span><p id=t1>Some text in a card.</p></div>"
    "<div class=grid id=g><div class=cell>a</div><div class=cell>b</div><div class=cell>c</div>"
    "<div class=cell>d</div><div class=cell id=e5>e</div><div class=cell>f</div></div>"
    "<div class=scroll id=sc><div class=sticky>head</div><p id=s1>one</p><p>two</p><p>three</p><p>four</p><p>five</p><p>six</p></div>"
    "<div class=card id=c2><span class=turn id=tr>turned</span> <span class=fade id=fd>faded</span></div>";

static const char* const kTexts[] = {"0", "12", "345", "6789", "short", "a longer piece of text that wraps", "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", ""};
static const char* const kIds[] = {"#count", "#h1", "#t1", "#e5", "#s1", "#tr", "#fd", "#b3"};
static const char* const kClasses[] = {"on", "wide", "fade", "grow"};

static CscdElement* any(CscdView* v) {
    CscdPtrVec all = {0};
    cscdViewQuerySelectorAll(v, CSCD_SV("body *"), &all);
    CscdElement* e = all.len ? (CscdElement*)all.items[rnd() % all.len] : NULL;
    cscdVecFree(&all);
    return e;
}

static void change(CscdView* v, char* log, size_t logSize) {
    const CscdModifiers none = {0};
    const float x = (float)(rnd() % 420), y = (float)(rnd() % 320);
    char num[16];
    const unsigned what = rnd() % 16;
    CscdElement* e = NULL;
    switch (what) {
    case 0: case 1: case 2: /* the text path */
        e = cscdViewQuerySelector(v, cscdSv(kIds[rnd() % CSCD_COUNT(kIds)]));
        if (e) cscdElementSetText(e, cscdSv(kTexts[rnd() % CSCD_COUNT(kTexts)]));
        break;
    case 3: /* the attribute path */
        snprintf(num, sizeof num, "%u", rnd() % 120);
        e = cscdViewQuerySelector(v, CSCD_SV("#pr"));
        if (e) cscdElementSetAttribute(e, CSCD_SV("value"), cscdSv(num));
        break;
    case 4: case 5: case 6: case 7: /* the state path: hover */
        cscdViewMouseMove(v, x, y, 0, none);
        break;
    case 8: /* :active */
        cscdViewMouseDown(v, x, y, 0, none, 1);
        if (rnd() % 2) cscdViewUpdate(v);
        cscdViewMouseUp(v, x, y, 0, none);
        break;
    case 9: /* the scroll path */
        cscdViewWheel(v, x, y, 0, (float)((int)(rnd() % 120) - 40), none);
        break;
    case 10: cscdViewMouseLeave(v); break;
    case 11: /* the whole pipeline: a class */
        e = any(v);
        if (e) cscdElementToggleClass(e, cscdSv(kClasses[rnd() % CSCD_COUNT(kClasses)]));
        break;
    case 12: /* a style */
        e = any(v);
        snprintf(num, sizeof num, "%upx", rnd() % 30);
        if (e) cscdElementSetStyle(e, CSCD_SV("margin-left"), cscdSv(num));
        break;
    case 13: /* an element more, or one less */
        e = any(v);
        if (e && rnd() % 2) {
            CscdElement* i = cscdElementAppendElement(e, CSCD_SV("i"));
            if (i) cscdElementSetText(i, CSCD_SV("new"));
        } else if (e && e->node.parent && cscdNodeIsElement(e->node.parent) && !cscdElementIs(e, "body")) cscdElementRemove(e);
        break;
    case 14: /* a painter's element */
        e = any(v);
        if (e) cscdViewInvalidateElement(v, e);
        break;
    default: /* the viewport (whole device pixels, as a window's) */
        cscdViewSetViewport(v, (float)(int)((380 + rnd() % 60) * v->dpr) / v->dpr, 300, v->dpr);
        break;
    }
    const size_t at = strlen(log);
    if (at + 48 < logSize) snprintf(log + at, logSize - at, " %u", what);
}

static int failures;

static void run(uint64_t seed, int rounds, float dpr) {
    rs = 88172645463325252ull ^ (seed * 0x9E3779B97F4A7C15ull);
    CscdView* v = cscdViewNew();
    cscdViewOnWarning(v, NULL, NULL);
    cscdViewSetFeature(v, CSCD_FEATURE_ANIMATIONS, false);
    cscdViewSetFeature(v, CSCD_FEATURE_SMOOTH_SCROLL, false);
    cscdViewSetFeature(v, CSCD_FEATURE_TOOLTIPS, false);
    cscdViewSetViewport(v, 400, 300, dpr);
    cscdViewLoadHTML(v, (CscdSv){kPage, sizeof kPage - 1}, CSCD_SV("."));
    CscdImage frame;
    memset(&frame, 0, sizeof frame);
    char log[4096] = "";
    for (int i = 0; i < rounds; ++i) {
        /* the frame follows the viewport, as a window's does (a new size: painted whole) */
        const int w = (int)ceilf(v->vw * dpr), h = (int)ceilf(v->vh * dpr);
        bool whole = false;
        if (frame.width != w || frame.height != h) {
            cscdImageFini(&frame);
            cscdImageInit(&frame, w, h);
            whole = true;
        }
        cscdViewUpdate(v);
        cscdViewPaint(v, &frame, !whole);
        if (i > 0) {
            CscdStr why = {0};
            CscdImage light;
            cscdImageInit(&light, frame.width, frame.height);
            memcpy(light.pixels, frame.pixels, (size_t)frame.width * frame.height * 4);
            if (!viewCheckIncremental(v, &frame, &why)) {
                ++failures;
                printf("FAIL seed %llu dpr %g round %d: %s\n  changes:%s\n", (unsigned long long)seed, dpr, i, cscdStrC(&why), log);
                if (getenv("CASCADE_ORACLE_DUMP")) { /* the two frames, to compare, and the page */
                    CscdStr html = cscdElementOuterHTML(cscdViewQuerySelector(v, CSCD_SV("body")));
                    printf("%s\n", cscdStrC(&html));
                    cscdStrFree(&html);
                    char name[64];
                    snprintf(name, sizeof name, "oracle-%llu-light.png", (unsigned long long)seed);
                    cscdSavePNG(cscdSv(name), &light);
                    snprintf(name, sizeof name, "oracle-%llu-scratch.png", (unsigned long long)seed);
                    cscdSavePNG(cscdSv(name), &frame);
                }
                cscdImageFini(&light);
                cscdStrFree(&why);
                break;
            }
            cscdImageFini(&light);
            cscdStrFree(&why);
        }
        log[0] = 0;
        const unsigned n = 1 + rnd() % 3;
        for (unsigned k = 0; k < n; ++k) change(v, log, sizeof log);
    }
    cscdImageFini(&frame);
    cscdViewFree(v);
}

int main(int argc, char** argv) {
    const int first = argc > 1 ? atoi(argv[1]) : 1, seeds = argc > 2 ? atoi(argv[2]) : 6, rounds = argc > 3 ? atoi(argv[3]) : 200;
    cscdSetPaintThreads(1);
    for (int s = first; s < first + seeds; ++s) run((uint64_t)s, rounds, s % 3 == 2 ? 1.5f : 1.f);
    printf("%d seeds of %d rounds, %d failed\n", seeds, rounds, failures);
    return failures ? 1 : 0;
}
