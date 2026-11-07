/* cascade-tests: checks parsing, the cascade, layout geometry and input handling against the values
 * a browser computes for the same documents. */
#include "cascade/cascade.h"
#include "../src/text/bidiInternal.h" /* (internal: the algorithm on its own) */
#include "../src/view/viewInternal.h" /* (internal: viewSetInputAge, the platforms' touch times) */
#include "../src/core/thread.h"        /* (internal: the parallel loops) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);
static void sleepMs(int ms) { Sleep((unsigned long)ms); }
#else
#include <time.h>
static void sleepMs(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
#endif

static int failures = 0, checks = 0;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b) do { ++checks; double _a = (a), _b = (b); if (fabs(_a - _b) > 0.6) { ++failures; printf("FAIL %s:%d: %s = %g, expected %g\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

/* ---- helpers ---------------------------------------------------------------------------------------- */

/* Strings the checks compare, kept until a few more are asked for. */
static CscdStr kept[16];
static int keptAt;
static const char* keep(CscdStr s) {
    keptAt = (keptAt + 1) % 16;
    cscdStrFree(&kept[keptAt]);
    kept[keptAt] = s;
    return cscdStrC(&kept[keptAt]);
}
static const char* textOf(const CscdElement* e) {
    CscdStr t = {0};
    if (e) cscdNodeTextContent(&e->node, &t);
    return keep(t);
}
static const char* valueOf(const CscdElement* e) { return keep(cscdElementValue(e)); }
static bool eq(const char* a, const char* b) { return !strcmp(a, b); }

static CscdElement* byId(CscdView* v, const char* id) { return cscdViewGetElementById(v, cscdSv(id)); }
static CscdElement* query(CscdView* v, const char* s) { return cscdViewQuerySelector(v, cscdSv(s)); }
static size_t countOf(CscdView* v, const char* s) {
    CscdPtrVec all = {0};
    cscdViewQuerySelectorAll(v, cscdSv(s), &all);
    const size_t n = all.len;
    cscdVecFree(&all);
    return n;
}
static CscdElement* nth(CscdView* v, const char* s, size_t i) {
    CscdPtrVec all = {0};
    cscdViewQuerySelectorAll(v, cscdSv(s), &all);
    CscdElement* e = i < all.len ? (CscdElement*)all.items[i] : NULL;
    cscdVecFree(&all);
    return e;
}
static CscdRect rectOf(CscdView* v, const char* id) {
    CscdElement* e = byId(v, id);
    if (!e || !e->layout) return cscdRect(-1, -1, -1, -1);
    return cscdBoxBorderBox(e->layout);
}
static CscdRect viewRect(CscdView* v, CscdElement* e) { return cscdViewElementRect(v, e); }

static void layoutViewWH(CscdView* v, const char* html, float w, float h) {
    cscdViewLoadHTML(v, cscdSv(html), CSCD_SV(""));
    cscdViewSetViewport(v, w, h, 1);
    cscdViewUpdate(v);
}
static void layoutView(CscdView* v, const char* html) { layoutViewWH(v, html, 800, 600); }

static void frame(CscdView* v, CscdImage* img) {
    cscdViewUpdate(v);
    cscdViewPaint(v, img, false);
}

static CscdModifiers none(void) { CscdModifiers m = {false, false, false, false}; return m; }
static CscdModifiers shiftMod(void) { CscdModifiers m = {true, false, false, false}; return m; }
static CscdModifiers ctrlMod(void) { CscdModifiers m = {false, true, false, false}; return m; }
static void down(CscdView* v, float x, float y) { cscdViewMouseDown(v, x, y, 0, none(), 1); }
static void downButton(CscdView* v, float x, float y, int b) { cscdViewMouseDown(v, x, y, b, none(), 1); }
static void up(CscdView* v, float x, float y) { cscdViewMouseUp(v, x, y, 0, none()); }
static void upButton(CscdView* v, float x, float y, int b) { cscdViewMouseUp(v, x, y, b, none()); }
static void moveTo(CscdView* v, float x, float y) { cscdViewMouseMove(v, x, y, 0, none()); }
static void drag(CscdView* v, float x, float y) { cscdViewMouseMove(v, x, y, 1, none()); }
static void key(CscdView* v, CscdKey k) { cscdViewKeyDown(v, k, none(), CSCD_SV("")); }
static void keyMods(CscdView* v, CscdKey k, CscdModifiers m) { cscdViewKeyDown(v, k, m, CSCD_SV("")); }
static void keyNamed(CscdView* v, CscdKey k, const char* name) { cscdViewKeyDown(v, k, none(), cscdSv(name)); }
static void text(CscdView* v, const char* s) { cscdViewTextInput(v, cscdSv(s)); }

static void countEvent(void* user, CscdEvent* ev) { (void)ev; ++*(int*)user; }
static void on(CscdElement* e, const char* type, CscdEventFn fn, void* user) { cscdElementOn(e, cscdSv(type), fn, user); }

static bool imagesEqual(const CscdImage* a, const CscdImage* b) {
    return a->width == b->width && a->height == b->height && !memcmp(a->pixels, b->pixels, cscdImagePixelCount(a) * 4);
}

/* ---- parsing and the cascade ------------------------------------------------------------------------ */

static void testParser(void) {
    CscdDocument* doc = cscdParseHTML(CSCD_SV("<p>one<p>two<ul><li>a<li>b</ul><table><tr><td>x</table>&amp;&lt;&#x41;&eacute;"));
    CHECK(cscdDocumentBody(doc) != NULL);
    CHECK(cscdDocumentHead(doc) != NULL);
    CscdPtrVec all = {0};
    cscdDocumentQuerySelectorAll(doc, CSCD_SV("p"), &all);
    CHECK(all.len == 2);
    all.len = 0;
    cscdDocumentQuerySelectorAll(doc, CSCD_SV("li"), &all);
    CHECK(all.len == 2);
    cscdVecFree(&all);
    CHECK(cscdDocumentQuerySelector(doc, CSCD_SV("table > tbody > tr > td")) != NULL);
    CHECK(strstr(textOf(cscdDocumentBody(doc)), "&<A\xC3\xA9") != NULL);
    cscdDocumentFree(doc);
    CscdDocument* d2 = cscdParseHTML(CSCD_SV("<div id=a class='x y'><span>t</span></div>"));
    CscdElement* a = cscdDocumentGetElementById(d2, CSCD_SV("a"));
    CHECK(a && cscdElementHasClass(a, CSCD_SV("x")) && cscdElementHasClass(a, CSCD_SV("y")));
    CHECK(eq(keep(cscdElementInnerHTML(a)), "<span>t</span>"));
    cscdElementSetInnerHTML(a, CSCD_SV("<b>bold</b> text"));
    CHECK(cscdElementQuerySelector(a, CSCD_SV("b")) && eq(textOf(a), "bold text"));
    cscdDocumentFree(d2);
    /* a </head> before anything is open, then a <template> (the open elements were once left holding nothing) */
    CscdDocument* d3 = cscdParseHTML(CSCD_SV("</head><template><b>t</b></template><p>after"));
    CHECK(cscdDocumentBody(d3) && cscdDocumentQuerySelector(d3, CSCD_SV("p")) != NULL);
    cscdDocumentFree(d3);
    /* SVG path data with numbers after a Z: it ends there (it once went round for ever) */
    CscdPath path = {0};
    cscdParseSvgPath(CSCD_SV("M0 0 L10 0 L10 10 Z 5 5 L1 1"), &path);
    CHECK(path.verbs.len == 4);
    cscdPathFree(&path);
}

static void testSelectors(void) {
    CscdDocument* doc = cscdParseHTML(CSCD_SV("<ul id=l><li>1</li><li class=k>2</li><li>3</li><li data-v='a b'>4</li></ul><div><p></p><span></span></div>"));
#define ALL(s) (all.len = 0, cscdDocumentQuerySelectorAll(doc, CSCD_SV(s), &all), all.len)
#define ONE(s) cscdDocumentQuerySelector(doc, CSCD_SV(s))
    CscdPtrVec all = {0};
    CHECK(ALL("li:nth-child(odd)") == 2);
    CHECK(ALL("li:nth-child(2n+2)") == 2);
    CHECK(eq(textOf(ONE("li:first-child")), "1"));
    CHECK(eq(textOf(ONE("li:last-child")), "4"));
    CHECK(eq(textOf(ONE(".k + li")), "3"));
    CHECK(ALL(".k ~ li") == 2);
    CHECK(ONE("[data-v~=b]") != NULL);
    CHECK(ALL("li:not(.k)") == 3);
    CHECK(ALL(":is(p, span)") == 2);
    CHECK(ONE("div:has(> span)") != NULL);
    CHECK(ONE("p:empty") != NULL);
    CHECK(eq(textOf(ONE("#l > li.k")), "2"));
#undef ALL
#undef ONE
    cscdVecFree(&all);
    cscdDocumentFree(doc);
}

static void testCascade(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>\n"
                  "        :root { --c: rgb(10, 20, 30); --w: 10px; }\n"
                  "        p { color: red; } #i { color: blue; } .c { color: green !important; }\n"
                  "        div { width: calc(100px + 2 * var(--w)); color: var(--c); padding: 1em 2em; font-size: 10px; }\n"
                  "        span { color: inherit; margin: auto; }\n"
                  "        @media (max-width: 500px) { div { width: 1px; } }\n"
                  "      </style><p id=i class=c>t</p><div id=d><span id=s>x</span></div><p id=q style=\"color: rgb(1,2,3)\">y</p>");
#define C(id) byId(v, id)->style->color
    CHECK(cscdColorEq(C("i"), cscdRgb(0, 128, 0)));
    CHECK(cscdColorEq(C("d"), cscdRgb(10, 20, 30)));
    CHECK(cscdColorEq(C("s"), cscdRgb(10, 20, 30)));
    CHECK(cscdColorEq(C("q"), cscdRgb(1, 2, 3)));
#undef C
    NEAR(rectOf(v, "d").w, 120 + 40);
    NEAR(byId(v, "d")->style->padding[0].px, 10);
    cscdViewSetViewport(v, 400, 600, 1);
    cscdViewUpdate(v);
    NEAR(rectOf(v, "d").w, 1 + 40);
    cscdViewFree(v);
}

/* ---- layout ----------------------------------------------------------------------------------------- */

static void testBlockLayout(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} .a{margin:20px 0;height:10px} .b{margin:30px auto 0;width:200px;height:10px}\n"
                  "      .p{background:#eee} .c{margin-top:15px;height:5px}</style>\n"
                  "      <div class=a id=a></div><div class=b id=b></div><div class=p id=p><div class=c id=c></div></div>");
    NEAR(rectOf(v, "a").y, 20);           /* body margin 0, the first margin collapses with body / html */
    NEAR(rectOf(v, "b").y, 20 + 10 + 30); /* max(20, 30) between siblings */
    NEAR(rectOf(v, "b").x, 300);          /* auto margins center */
    NEAR(rectOf(v, "p").y, 70 + 15);      /* child margin collapses through the parent */
    NEAR(rectOf(v, "c").y, rectOf(v, "p").y);
    cscdViewFree(v);
}

static void testFlex(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} .f{display:flex;width:600px;gap:10px} .f div{height:20px}\n"
                  "      #x{flex:1} #y{flex:2} #z{width:100px;flex:none}\n"
                  "      .col{display:flex;flex-direction:column;height:300px} #g{flex:1} #h{height:50px}\n"
                  "      .c{display:flex;justify-content:center;align-items:center;width:200px;height:100px} #k{width:50px;height:20px}\n"
                  "      .w{display:flex;flex-wrap:wrap;width:250px} .w div{width:100px;height:10px}</style>\n"
                  "      <div class=f><div id=x></div><div id=y></div><div id=z></div></div>\n"
                  "      <div class=col><div id=g></div><div id=h></div></div>\n"
                  "      <div class=c><div id=k></div></div>\n"
                  "      <div class=w id=w><div id=w1></div><div id=w2></div><div id=w3></div></div>");
    NEAR(rectOf(v, "x").w, 160);
    NEAR(rectOf(v, "y").w, 320);
    NEAR(rectOf(v, "y").x, 170);
    NEAR(rectOf(v, "z").x, 500);
    NEAR(rectOf(v, "g").h, 250);
    NEAR(rectOf(v, "k").x, 75);
    NEAR(rectOf(v, "k").y - rectOf(v, "g").y, 300 + 40);
    NEAR(rectOf(v, "w3").y - rectOf(v, "w1").y, 10);
    NEAR(rectOf(v, "w").h, 20);
    cscdViewFree(v);
}

static void testGridPlacement(void) {
    /* an item with a definite column moves the auto-placement cursor (CSS Grid 8.5 step 4) */
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} .g{display:grid;width:300px;grid-template-columns:100px 100px 100px} .g div{height:20px} #b{grid-column:2 / 4}</style>\n"
                  "      <div class=g><div id=a></div><div id=b></div><div id=c></div><div id=d></div></div>");
    NEAR(rectOf(v, "b").x, 100);
    NEAR(rectOf(v, "b").y, 0);
    NEAR(rectOf(v, "c").x, 0);
    NEAR(rectOf(v, "c").y, 20);
    NEAR(rectOf(v, "d").x, 100);
    cscdViewFree(v);
}

static void testGrid(void) {
    testGridPlacement();
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} .g{display:grid;width:500px;grid-template-columns:100px 1fr 2fr;gap:10px}\n"
                  "      .g div{height:30px} #s{grid-column:1 / -1} .a{display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr));width:330px}</style>\n"
                  "      <div class=g><div id=a1></div><div id=a2></div><div id=a3></div><div id=s></div></div>\n"
                  "      <div class=a><div id=b1></div><div id=b2></div><div id=b3></div><div id=b4></div></div>");
    NEAR(rectOf(v, "a2").x, 110);
    NEAR(rectOf(v, "a2").w, 126.67);
    NEAR(rectOf(v, "a3").w, 253.33);
    NEAR(rectOf(v, "s").w, 500);
    NEAR(rectOf(v, "s").y, 40);
    NEAR(rectOf(v, "b1").w, 110);
    NEAR(rectOf(v, "b4").y - rectOf(v, "b1").y, 0 + rectOf(v, "b1").h);
    cscdViewFree(v);
}

static void testInline(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0;font:16px Arial} p{width:100px;margin:0} .c{text-align:center;width:300px}</style>\n"
                  "      <p id=p>aaa bbb ccc ddd eee fff ggg</p><div class=c id=c><span id=t>mid</span></div>");
    CscdElement* p = byId(v, "p");
    CHECK(p->layout && p->layout->lines.len >= 2);
    CscdRect t = cscdElementBoundingClientRect(byId(v, "t"));
    NEAR(t.x + t.w / 2, 150);
    cscdViewFree(v);
}

/* ---- events ----------------------------------------------------------------------------------------- */

static CscdView* rebuildView;
static int parentCalls, wrapCalls;
static void rebuildP(void* u, CscdEvent* ev) { (void)u; (void)ev; cscdElementSetInnerHTML(byId(rebuildView, "p"), CSCD_SV("<span>rebuilt</span>")); }
static void noop(void* u, CscdEvent* ev) { (void)u; (void)ev; }
static void rebuildWrap(void* u, CscdEvent* ev) {
    (void)u; (void)ev;
    ++wrapCalls;
    cscdElementSetInnerHTML(byId(rebuildView, "wrap"), CSCD_SV(""));
}
static char lastInput[64];
static void noteInput(void* u, CscdEvent* ev) {
    (void)u;
    snprintf(lastInput, sizeof lastInput, "%s", valueOf(ev->target));
}
static void setTrue(void* u, CscdEvent* ev) { (void)ev; *(bool*)u = true; }

static void testEvents(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} #b{width:100px;height:40px} #in{position:absolute;left:0;top:100px;width:200px;height:30px}</style>\n"
                  "      <div id=outer><button id=b>go</button></div><input id=in type=text value=\"ab\">\n"
                  "      <input id=cb type=checkbox style=\"position:absolute;left:300px;top:0\">");
    CscdImage img;
    cscdImageInit(&img, 800, 600);
    cscdViewPaint(v, &img, false);
    int clicks = 0, bubbled = 0;
    cscdElementOnClick(byId(v, "b"), countEvent, &clicks);
    cscdElementOnClick(byId(v, "outer"), countEvent, &bubbled);
    down(v, 10, 10);
    up(v, 10, 10);
    CHECK(clicks == 1 && bubbled == 1);
    /* a listener that rebuilds what the event is on its way through: the removed elements get no more
     * listeners, the ones still in the page do */
    {
        CscdView* r = cscdViewNew();
        rebuildView = r;
        parentCalls = wrapCalls = 0;
        layoutView(r, "<style>body{margin:0} #p button{width:100px;height:40px}</style><div id=wrap><div id=p><button id=k>x</button></div></div>");
        CscdImage ri;
        cscdImageInit(&ri, 400, 200);
        cscdViewPaint(r, &ri, false);
        cscdElementOnClick(byId(r, "k"), rebuildP, NULL);
        on(byId(r, "k"), "mouseup", noop, NULL);
        cscdElementOnClick(byId(r, "p"), countEvent, &parentCalls);
        cscdElementOnClick(byId(r, "wrap"), rebuildWrap, NULL);
        down(r, 10, 10);
        up(r, 10, 10);
        CHECK(parentCalls == 1 && wrapCalls == 1);
        CHECK(byId(r, "p") == NULL);
        cscdImageFini(&ri);
        cscdViewFree(r);
    }
    /* typing into a text field */
    CscdElement* in = byId(v, "in");
    lastInput[0] = 0;
    on(in, "input", noteInput, NULL);
    down(v, 150, 110);
    up(v, 150, 110);
    CHECK(cscdViewDocument(v)->focused == in);
    text(v, "c");
    CHECK(eq(valueOf(in), "abc"));
    CHECK(eq(lastInput, "abc"));
    key(v, CSCD_KEY_BACKSPACE);
    CHECK(eq(valueOf(in), "ab"));
    key(v, CSCD_KEY_HOME);
    text(v, "X");
    CHECK(eq(valueOf(in), "Xab"));
    /* checkbox toggles on click */
    cscdViewUpdate(v);
    cscdViewPaint(v, &img, false);
    CscdElement* cb = byId(v, "cb");
    bool changed = false;
    on(cb, "change", setTrue, &changed);
    down(v, 308, 8);
    up(v, 308, 8);
    CHECK(cscdElementChecked(cb) && changed);
    cscdImageFini(&img);
    cscdViewFree(v);
}

static void testTransition(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>#a{width:50px;height:50px;opacity:1;transition:opacity 1s linear} #a.off{opacity:0}</style><div id=a></div>");
    CscdElement* a = byId(v, "a");
    cscdElementAddClass(a, CSCD_SV("off"));
    cscdViewUpdate(v);
    /* (long enough that a busy machine sleeping late still lands in the middle) */
    sleepMs(250);
    cscdViewUpdate(v);
    float mid = a->style->opacity;
    CHECK(mid > 0.1f && mid < 0.9f);
    sleepMs(1000);
    cscdViewUpdate(v);
    NEAR(a->style->opacity, 0);
    cscdViewFree(v);
}

static void fillRow(void* user, CscdElement* row, size_t index) {
    (void)user;
    char buf[32];
    snprintf(buf, sizeof buf, "%zu", index);
    cscdElementFill(row, CSCD_SV("n"), cscdSv(buf));
}

/* Templates, copies, and a list of a hundred thousand rows that makes only the ones in sight. */
static void testTemplatesAndLists(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} #box{height:100px;overflow:auto} .row{height:20px}</style>"
                  "<template id=tpl><div class='row r{{n}}' data-n='{{n}}'>Item {{n}}</div><span>x</span></template>"
                  "<div id=box></div><p id=after>after</p>");
    CscdElement* tpl = byId(v, "tpl");
    /* the content is out of the document */
    CHECK(countOf(v, ".row") == 0 && countOf(v, "span") == 0);
    CHECK(cscdTemplateContent(tpl)->node.children.len == 2);
    CscdStr html = cscdElementInnerHTML(tpl);
    CHECK(strstr(cscdStrC(&html), "Item {{n}}") != NULL);
    cscdStrFree(&html);
    /* an instance, filled */
    CscdElement* one = cscdTemplateInstantiate(tpl);
    cscdElementFill(one, CSCD_SV("n"), CSCD_SV("7"));
    CHECK(eq(textOf(one), "Item 7") && eq(keep(cscdStrDup(cscdElementAttr(one, CSCD_SV("data-n")))), "7"));
    CHECK(cscdElementHasClass(one, CSCD_SV("r7")));
    cscdNodeAppendChild(&cscdDocumentBody(cscdViewDocument(v))->node, &one->node);
    cscdViewUpdate(v);
    CHECK(countOf(v, ".r7") == 1);
    CscdElement* copy = cscdElementClone(one, true);
    CHECK(eq(textOf(copy), "Item 7") && !copy->node.parent);
    cscdNodeDestroy(&copy->node);
    cscdElementRemove(one);
    /* the list */
    CscdElement* box = byId(v, "box");
    cscdViewBindList(v, box, tpl, 100000, 20, fillRow, NULL, NULL);
    cscdViewUpdate(v);
    const size_t made = countOf(v, ".row");
    CHECK(made >= 5 && made <= 16); /* (the five in sight and a few beyond) */
    CHECK(countOf(v, ".r0") == 1 && countOf(v, ".r99999") == 0);
    NEAR(rectOf(v, "after").y, 116); /* (the box keeps its height; then the paragraph's margin) */
    CscdImage img;
    cscdImageInit(&img, 800, 600);
    frame(v, &img); /* (where things are is known once painted) */
    cscdViewSetFeature(v, CSCD_FEATURE_SMOOTH_SCROLL, false);
    cscdViewWheel(v, 50, 50, 0, 1e7f, none());
    frame(v, &img);
    cscdImageFini(&img);
    CHECK(cscdViewListFirstShown(v, box) >= 49990);
    CHECK(countOf(v, ".r99999") == 1 && countOf(v, ".r0") == 0);
    CHECK(countOf(v, ".row") <= 16);
    cscdViewListSetCount(v, box, 3);
    cscdViewUpdate(v);
    CHECK(countOf(v, ".row") == 3);
    cscdViewFree(v);
}

/* The inspector: its keys, that it keeps the pointer from the page, and the rules it lists. */
static void testInspector(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} div{color:red} #b.c{color:blue} .c{width:100px;height:40px}</style><div id=b class=c>x</div>");
    CscdMatchedRuleVec rules = {0};
    cscdStyleEngineMatchedRules(cscdViewStyleEngine(v), byId(v, "b"), &rules);
    CHECK(rules.len >= 3);
    CHECK(rules.len && eq(cscdStrC(&cscdVecLast(&rules).selector->text), "#b.c")); /* (the winner last) */
    cscdVecFree(&rules);
    int clicks = 0;
    on(byId(v, "b"), "click", countEvent, &clicks);
    key(v, CSCD_KEY_F12); /* (off by default: nothing) */
    CHECK(!cscdViewInspecting(v));
    cscdViewSetFeature(v, CSCD_FEATURE_INSPECTOR, true);
    key(v, CSCD_KEY_F12);
    CHECK(cscdViewInspecting(v));
    CscdImage img;
    cscdImageInit(&img, 800, 600);
    frame(v, &img);
    down(v, 10, 10);
    up(v, 10, 10);
    CHECK(clicks == 0); /* (the inspector took it) */
    key(v, CSCD_KEY_ESCAPE);
    CHECK(!cscdViewInspecting(v));
    frame(v, &img);
    down(v, 10, 10);
    up(v, 10, 10);
    CHECK(clicks == 1);
    cscdImageFini(&img);
    cscdViewFree(v);
}

static void noteA11y(void* user, CscdElement* e, CscdAccessibleEvent event) {
    (void)e;
    ((int*)user)[event] += 1;
}

static const char* a11yName(CscdView* v, const char* id) {
    CscdStr s = {0};
    cscdAccessibleName(v, byId(v, id), &s);
    return keep(s);
}

/* What a screen reader is given: roles, names, states, the tree, actions and events. */
static void testAccessibility(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<div class=wrap><h2 id=h>Settings</h2>"
                  "<div><label for=name>Your name</label> <input id=name value=Ann></div>"
                  "<label><input type=checkbox id=cb checked> Dark mode</label>"
                  "<button id=b aria-label='Close window'>X</button><button id=b2>Save <span>now</span></button>"
                  "<img id=img alt='A cat'><input type=range id=r min=0 max=10 value=4>"
                  "<div id=plain><span>Some text</span></div><div aria-hidden=true><button id=hid>no</button></div>"
                  "<button id=off disabled>Off</button></div>");
    CHECK(cscdAccessibleRole(byId(v, "h")) == CSCD_ROLE_HEADING && cscdAccessibleLevel(byId(v, "h")) == 2);
    CHECK(cscdAccessibleRole(byId(v, "cb")) == CSCD_ROLE_CHECKBOX && cscdAccessibleRole(byId(v, "r")) == CSCD_ROLE_SLIDER);
    CHECK(cscdAccessibleRole(byId(v, "plain")) == CSCD_ROLE_NONE);
    CHECK(eq(a11yName(v, "name"), "Your name"));
    CHECK(eq(a11yName(v, "cb"), "Dark mode"));
    CHECK(eq(a11yName(v, "b"), "Close window"));
    CHECK(eq(a11yName(v, "b2"), "Save now"));
    CHECK(eq(a11yName(v, "img"), "A cat"));
    CscdStr val = {0};
    cscdAccessibleValue(v, byId(v, "name"), &val);
    CHECK(eq(cscdStrC(&val), "Ann"));
    cscdStrClear(&val);
    cscdAccessibleValue(v, byId(v, "r"), &val);
    CHECK(eq(cscdStrC(&val), "4"));
    cscdStrFree(&val);
    CHECK(cscdAccessibleState(v, byId(v, "cb")) & CSCD_A11Y_CHECKED);
    CHECK(cscdAccessibleState(v, byId(v, "off")) & CSCD_A11Y_DISABLED);
    /* the tree: plain containers left out, hidden parts too */
    CscdPtrVec kids = {0};
    cscdAccessibleChildren(v, NULL, &kids);
    bool sawPlain = false, sawHidden = false, sawSpan = false;
    for (size_t i = 0; i < kids.len; ++i) {
        CscdElement* k = kids.items[i];
        sawPlain = sawPlain || k == byId(v, "plain");
        sawHidden = sawHidden || k == byId(v, "hid");
        sawSpan = sawSpan || (cscdElementIs(k, "span") && eq(textOf(k), "Some text"));
    }
    CHECK(kids.len >= 8 && !sawPlain && !sawHidden && sawSpan);
    CHECK(cscdAccessibleParent(v, byId(v, "name")) == cscdAccessibleRoot(v));
    cscdVecFree(&kids);
    /* numbers that stay with their element */
    const uint32_t id = cscdAccessibleId(v, byId(v, "b2"));
    CHECK(id && cscdAccessibleId(v, byId(v, "b2")) == id && cscdAccessibleElement(v, id) == byId(v, "b2"));
    cscdElementRemove(byId(v, "b2"));
    CHECK(cscdAccessibleElement(v, id) == NULL);
    /* actions, and the events the window passes on */
    int events[4] = {0, 0, 0, 0};
    cscdViewOnAccessibleEvent(v, noteA11y, events);
    CHECK(cscdAccessibleDo(v, byId(v, "name"), CSCD_A11Y_FOCUS, CSCD_SV("")));
    CHECK(cscdAccessibleState(v, byId(v, "name")) & CSCD_A11Y_FOCUSED);
    CHECK(cscdAccessibleDo(v, byId(v, "name"), CSCD_A11Y_SET_VALUE, CSCD_SV("Bob")));
    CHECK(eq(valueOf(byId(v, "name")), "Bob"));
    CHECK(cscdAccessibleDo(v, byId(v, "cb"), CSCD_A11Y_ACTIVATE, CSCD_SV("")));
    CHECK(!(cscdAccessibleState(v, byId(v, "cb")) & CSCD_A11Y_CHECKED));
    CHECK(!cscdAccessibleDo(v, byId(v, "off"), CSCD_A11Y_ACTIVATE, CSCD_SV("")));
    CHECK(events[CSCD_A11Y_EVENT_FOCUS] >= 1 && events[CSCD_A11Y_EVENT_VALUE] >= 2);
    cscdElementAppendElement(cscdDocumentBody(cscdViewDocument(v)), CSCD_SV("p"));
    cscdViewUpdate(v);
    CHECK(events[CSCD_A11Y_EVENT_STRUCTURE] >= 1);
    cscdViewFree(v);
}

static uint32_t nthCodepoint(CscdSv s, int n) {
    size_t i = 0;
    uint32_t cp = 0;
    for (int k = 0; k <= n; ++k) {
        if (i >= s.len) return 0;
        cp = cscdUtf8Next(s, &i);
    }
    return cp;
}

/* Arabic letters take the forms their neighbours ask for (in reading order). */
static void testArabicJoining(void) {
    CscdStr out = {0};
    CscdU32Vec map = {0};
    CHECK(!cscdArabicJoin(CSCD_SV("plain text"), &out, &map));
    /* beh beh beh: initial, medial, final */
    CHECK(cscdArabicJoin(CSCD_SV("\xD8\xA8\xD8\xA8\xD8\xA8"), &out, &map));
    CHECK(nthCodepoint(cscdStrSv(&out), 0) == 0xFE91 && nthCodepoint(cscdStrSv(&out), 1) == 0xFE92 && nthCodepoint(cscdStrSv(&out), 2) == 0xFE90);
    CHECK(map.len == out.len && map.items[0] == 0 && map.items[3] == 2); /* (each byte back to its letter) */
    /* alef does not join what follows: beh alef beh -> initial, final, isolated */
    cscdStrClear(&out);
    map.len = 0;
    cscdArabicJoin(CSCD_SV("\xD8\xA8\xD8\xA7\xD8\xA8"), &out, &map);
    CHECK(nthCodepoint(cscdStrSv(&out), 0) == 0xFE91 && nthCodepoint(cscdStrSv(&out), 1) == 0xFE8E && nthCodepoint(cscdStrSv(&out), 2) == 0xFE8F);
    /* lam alef: one ligature */
    cscdStrClear(&out);
    map.len = 0;
    cscdArabicJoin(CSCD_SV("\xD9\x84\xD8\xA7"), &out, &map);
    CHECK(nthCodepoint(cscdStrSv(&out), 0) == 0xFEFB && nthCodepoint(cscdStrSv(&out), 1) == 0);
    cscdStrFree(&out);
    cscdVecFree(&map);
}

static void countWarning(void* user, CscdSv message) { (void)message; ++*(int*)user; }

/* The features an application turns off. */
static void testFeatures(void) {
    CscdView* v = cscdViewNew();
    CHECK(cscdViewFeature(v, CSCD_FEATURE_ANIMATIONS) && !cscdViewFeature(v, CSCD_FEATURE_STATS));
    CHECK(eq(cscdFeatureName(CSCD_FEATURE_WARN_CSS), "warn-css"));
    int warnings = 0;
    cscdViewOnWarning(v, countWarning, &warnings);
    /* warnings by kind */
    cscdViewSetFeature(v, CSCD_FEATURE_WARN_SELECTORS, false);
    cscdViewSetFeature(v, CSCD_FEATURE_WARN_CSS, false);
    layoutView(v, "<style>#a{colr:red;width:50px;height:50px;opacity:1;transition:opacity 1s linear} #a.off{opacity:0}"
                  " @media (prefers-reduced-motion: reduce) { #a{height:20px} }</style><div id=a title=tip></div>");
    cscdViewGet(v, CSCD_SV("#nothing"));
    CHECK(warnings == 0);
    cscdViewSetFeature(v, CSCD_FEATURE_WARN_SELECTORS, true);
    cscdViewGet(v, CSCD_SV("#nothing"));
    CHECK(warnings == 1);
    /* without animations: the new style at once, and prefers-reduced-motion */
    NEAR(rectOf(v, "a").h, 50);
    cscdViewSetFeature(v, CSCD_FEATURE_ANIMATIONS, false);
    CscdElement* a = byId(v, "a");
    cscdElementAddClass(a, CSCD_SV("off"));
    cscdViewUpdate(v);
    NEAR(a->style->opacity, 0);
    NEAR(rectOf(v, "a").h, 20);
    /* without tooltips */
    cscdViewSetFeature(v, CSCD_FEATURE_TOOLTIPS, false);
    cscdViewSetTooltipDelay(v, 0);
    CscdImage img;
    cscdImageInit(&img, 800, 600);
    moveTo(v, 10, 10);
    sleepMs(5);
    frame(v, &img);
    CHECK(!query(v, ".cscd-tooltip"));
    cscdImageFini(&img);
    cscdViewFree(v);
}

static void testImageCodec(void) {
    CscdImage img;
    cscdImageInit(&img, 3, 2);
    cscdImageRow(&img, 0)[0] = cscdColorPremul(cscdRgb(255, 0, 0));
    cscdImageRow(&img, 1)[2] = cscdColorPremul(cscdRgba(0, 0, 255, 128));
    CscdBytes png = {0};
    cscdEncodePNG(&img, &png);
    CscdImage* back = cscdDecodeImage(png.items, png.len);
    CHECK(back && back->width == 3 && back->height == 2);
    CHECK(back && cscdImageRow(back, 0)[0] == cscdImageRow(&img, 0)[0]);
    CHECK(back && (cscdImageRow(back, 1)[2] >> 24) == 128);
    cscdRelease(back);
    cscdVecFree(&png);
    cscdImageFini(&img);
}

/* Data from elsewhere that is broken or hostile: refused, without reading past it or allocating
 * what its headers claim. */
static void testBrokenImages(void) {
    /* 120 bytes of zlib that make 100000 zeros */
    uint8_t bomb[120];
    memset(bomb, 0, sizeof bomb);
    memcpy(bomb, "\x78\xda\xed\xc1\x31\x01\x00\x00\x00\xc2\xa0\xf5\x4f\x6d\x0d\x0f\xa0", 17);
    memcpy(bomb + 113, "\x80\x57\x03\x86\xaf\x00\x01", 7);
    CscdBytes out = {0};
    CHECK(cscdInflateZlib(bomb, sizeof bomb, &out) && out.len == 100000);
    out.len = 0;
    CHECK(!cscdInflateZlibLimit(bomb, sizeof bomb, &out, 1000));
    out.len = 0;
    CHECK(cscdInflateZlibLimit(bomb, sizeof bomb, &out, 100000) && out.len == 100000);
    cscdVecFree(&out);
    /* a WebP whose RIFF size says it holds nothing, not even its own header */
    static const uint8_t emptyRiff[24] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' '};
    CHECK(cscdDecodeImage(emptyRiff, sizeof emptyRiff) == NULL);
    /* a PNG whose IHDR is too short, at the very end of the data */
    static const uint8_t shortIhdr[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 0, 'I', 'H', 'D', 'R', 0, 0, 0, 0};
    CHECK(!cscdDecodeImage(shortIhdr, sizeof shortIhdr));
    /* a JPEG Huffman table of more codes than there can be (4080), and one cut short */
    uint8_t jpeg[2 + 4 + 17 + 8] = {0xFF, 0xD8, 0xFF, 0xC4, 0, 4 + 17 - 2 + 8, 0x00};
    memset(jpeg + 7, 255, 16);
    CHECK(!cscdDecodeImage(jpeg, sizeof jpeg));
    /* an 8-bit BMP whose palette is said to be far past the end: its pixels come out black */
    uint8_t bmp[58] = {'B', 'M'};
    bmp[10] = 54;               /* (the pixels) */
    bmp[14] = 0xFF; bmp[17] = 0x7F; /* (the info header's size: the palette after it) */
    bmp[18] = 1; bmp[22] = 1;   /* 1 x 1 */
    bmp[28] = 8;                /* bits per pixel */
    bmp[54] = 7;
    CscdImage* b = cscdDecodeImage(bmp, sizeof bmp);
    CHECK(b && b->width == 1 && cscdImageRow(b, 0)[0] == 0xFF000000u);
    /* a file whose name is not ASCII (paths are UTF-8, whatever the system's code page) */
    const char* name = "cascade-t\xC3\xA9st-\xE6\x97\xA5.png";
    CHECK(cscdSavePNG(cscdSv(name), b));
    CscdImage* again = cscdLoadImage(cscdSv(name));
    CHECK(again && again->width == 1 && cscdImageRow(again, 0)[0] == 0xFF000000u);
    cscdRelease(again);
#ifdef _WIN32
    _wremove(L"cascade-tést-日.png");
#else
    remove(name);
#endif
    cscdRelease(b);
}

/* ---- menus ------------------------------------------------------------------------------------------ */

static char ran[16];
static void setRan(void* user) { snprintf(ran, sizeof ran, "%s", (const char*)user); }
static CscdMenuItem item(const char* label, const char* runs) { return cscdMenuItem(cscdSv(label), runs ? setRan : NULL, (void*)runs, CSCD_SV("")); }

static CscdView* menuView;
static void contextMenu(void* u, CscdEvent* e) {
    (void)u;
    CscdMenu m = {0};
    cscdMenuAdd(&m, cscdMenuItem(CSCD_SV("Cut"), setRan, "cut", CSCD_SV("Ctrl+X")));
    cscdMenuAdd(&m, cscdMenuSeparator());
    CscdMenuItem off = item("Off", "off");
    off.enabled = false;
    cscdMenuAdd(&m, off);
    CscdMenu sub = {0};
    cscdMenuAdd(&sub, item("Deep", "deep"));
    cscdMenuAdd(&m, cscdMenuSub(CSCD_SV("More"), sub));
    cscdViewShowMenu(menuView, m, e->x, e->y, 0);
}
static void fileMenu(void* u, CscdMenu* out) { (void)u; cscdMenuAdd(out, item("New", NULL)); }
static void editMenu(void* u, CscdMenu* out) {
    (void)u;
    cscdMenuAdd(out, item("Undo", NULL));
    cscdMenuAdd(out, item("Redo", NULL));
}

static void testMenus(void) {
    CscdView* v = cscdViewNew();
    menuView = v;
    layoutView(v, "<style>body{margin:0} #bar{display:flex;height:24px} #bar span{padding:0 10px}\n"
                  "      #area{position:absolute;left:0;top:100px;width:300px;height:200px}\n"
                  "      #sel{position:absolute;left:400px;top:20px;width:120px}</style>\n"
                  "      <div id=bar><span id=file>File</span><span id=edit>Edit</span></div>\n"
                  "      <div id=area title=\"The area\"></div>\n"
                  "      <select id=sel><option value=a>Alpha</option><option value=b>Beta</option><option value=c>Gamma</option></select>");
    CscdImage img;
    cscdImageInit(&img, 800, 600);
    frame(v, &img);
    /* a context menu: the click on a row runs its action, once the event is over */
    ran[0] = 0;
    on(byId(v, "area"), "contextmenu", contextMenu, NULL);
    downButton(v, 50, 150, 2);
    upButton(v, 50, 150, 2);
    CHECK(cscdViewMenuOpen(v));
    frame(v, &img);
    CHECK(countOf(v, ".cscd-menu .cscd-menu-item") == 3);
    CHECK(countOf(v, ".cscd-menu-separator") == 1);
    CscdRect menu = viewRect(v, query(v, ".cscd-menu"));
    CHECK(menu.x >= 49 && menu.y >= 149);
    /* the disabled row does nothing */
    CscdRect off = viewRect(v, nth(v, ".cscd-menu .cscd-menu-item", 1));
    moveTo(v, off.x + 20, off.y + 5);
    down(v, off.x + 20, off.y + 5);
    up(v, off.x + 20, off.y + 5);
    frame(v, &img);
    CHECK(cscdViewMenuOpen(v) && !ran[0]);
    /* hovering a submenu row opens it; its row runs */
    CscdRect more = viewRect(v, nth(v, ".cscd-menu .cscd-menu-item", 2));
    moveTo(v, more.x + 20, more.y + 5);
    frame(v, &img);
    CHECK(countOf(v, ".cscd-menu") == 2);
    CscdElement* deep = cscdElementQuerySelector(nth(v, ".cscd-menu", 1), CSCD_SV(".cscd-menu-item"));
    CscdRect dr = viewRect(v, deep);
    CHECK(dr.x >= cscdRectRight(more) - 4);
    moveTo(v, dr.x + 10, dr.y + 5);
    down(v, dr.x + 10, dr.y + 5);
    up(v, dr.x + 10, dr.y + 5);
    CHECK(!cscdViewMenuOpen(v));
    frame(v, &img);
    CHECK(eq(ran, "deep"));
    CHECK(countOf(v, ".cscd-menu") == 0);
    /* keyboard: Down, Enter */
    CscdMenu two = {0};
    cscdMenuAdd(&two, item("One", "one"));
    cscdMenuAdd(&two, item("Two", "two"));
    cscdViewShowMenu(v, two, 10, 10, 0);
    key(v, CSCD_KEY_DOWN);
    key(v, CSCD_KEY_DOWN);
    key(v, CSCD_KEY_ENTER);
    frame(v, &img);
    CHECK(eq(ran, "two"));
    /* a click outside closes the menu without reaching the page */
    int pageClicks = 0;
    cscdElementOnClick(byId(v, "area"), countEvent, &pageClicks);
    CscdMenu one = {0};
    cscdMenuAdd(&one, item("One", NULL));
    cscdViewShowMenu(v, one, 600, 400, 0);
    frame(v, &img);
    down(v, 50, 150);
    up(v, 50, 150);
    CHECK(!cscdViewMenuOpen(v) && pageClicks == 0);
    /* menu bar: press opens, hovering the next entry switches */
    cscdViewBindMenuBar(v, byId(v, "file"), fileMenu, NULL, NULL);
    cscdViewBindMenuBar(v, byId(v, "edit"), editMenu, NULL, NULL);
    frame(v, &img);
    CscdRect fr = viewRect(v, byId(v, "file")), er = viewRect(v, byId(v, "edit"));
    down(v, fr.x + 5, 10);
    up(v, fr.x + 5, 10);
    frame(v, &img);
    CHECK(cscdViewMenuOpen(v) && cscdElementHasClass(byId(v, "file"), CSCD_SV("cscd-menu-open")));
    moveTo(v, er.x + 5, 10);
    frame(v, &img);
    CHECK(countOf(v, ".cscd-menu .cscd-menu-item") == 2);
    CscdRect mr = viewRect(v, query(v, ".cscd-menu"));
    NEAR(mr.x, er.x);
    NEAR(mr.y, cscdRectBottom(er));
    key(v, CSCD_KEY_ESCAPE);
    CHECK(!cscdViewMenuOpen(v));
    /* <select>: a list under the field; picking changes the value */
    int changes = 0;
    on(byId(v, "sel"), "change", countEvent, &changes);
    frame(v, &img);
    CscdRect sr = viewRect(v, byId(v, "sel"));
    down(v, sr.x + 10, sr.y + 5);
    up(v, sr.x + 10, sr.y + 5);
    frame(v, &img);
    CHECK(cscdViewMenuOpen(v) && query(v, ".cscd-select-menu"));
    CHECK(countOf(v, ".cscd-select-menu .cscd-menu-item") == 3 && cscdElementHasClass(nth(v, ".cscd-select-menu .cscd-menu-item", 0), CSCD_SV("checked")));
    CscdRect gr = viewRect(v, nth(v, ".cscd-select-menu .cscd-menu-item", 2));
    CHECK(viewRect(v, query(v, ".cscd-select-menu")).w >= sr.w - 0.5f);
    moveTo(v, gr.x + 10, gr.y + 5);
    down(v, gr.x + 10, gr.y + 5);
    up(v, gr.x + 10, gr.y + 5);
    frame(v, &img);
    CHECK(eq(valueOf(byId(v, "sel")), "c") && changes == 1);
    /* tooltips: after the hover delay */
    cscdViewSetTooltipDelay(v, 0);
    moveTo(v, 100, 200);
    sleepMs(5);
    frame(v, &img);
    CscdElement* tip = query(v, ".cscd-tooltip");
    CHECK(tip && eq(textOf(tip), "The area"));
    down(v, 100, 200);
    CHECK(!query(v, ".cscd-tooltip"));
    up(v, 100, 200);
    cscdImageFini(&img);
    cscdViewFree(v);
}

/* ---- drag and drop ---------------------------------------------------------------------------------- */

static CscdStrVec dragLog;
static char got[32];
static void logStr(const char* s) { cscdVecPush(&dragLog, cscdStrDupC(s)); }
static void onDragStart(void* u, CscdEvent* e) {
    (void)u;
    logStr("start");
    cscdDataTransferSetData(e->dataTransfer, CSCD_SV("text/x-item"), CSCD_SV("42"));
}
static void onDragEnd(void* u, CscdEvent* e) {
    (void)u;
    CscdStr s = cscdStrf("end:%s", cscdStrC(&e->dataTransfer->dropEffect));
    cscdVecPush(&dragLog, s);
}
static void onDragEnter(void* u, CscdEvent* e) { (void)u; (void)e; logStr("enter"); }
static void onDragLeave(void* u, CscdEvent* e) { (void)u; (void)e; logStr("leave"); }
static void onDragOver(void* u, CscdEvent* e) {
    (void)u;
    if (cscdDataTransferHasType(e->dataTransfer, CSCD_SV("text/x-item"))) cscdEventPreventDefault(e);
}
static void onDrop(void* u, CscdEvent* e) {
    (void)u;
    CscdSv d = cscdDataTransferGetData(e->dataTransfer, CSCD_SV("text/x-item"));
    snprintf(got, sizeof got, "%.*s", (int)d.len, d.ptr);
}
static void onFilesOver(void* u, CscdEvent* e) {
    (void)u;
    if (cscdDataTransferHasType(e->dataTransfer, CSCD_SV("Files"))) cscdEventPreventDefault(e);
}
static CscdStrVec droppedFiles;
static void onFilesDrop(void* u, CscdEvent* e) {
    (void)u;
    cscdStrVecCopy(&droppedFiles, &e->dataTransfer->files);
}
static const char* lastLog(void) { return dragLog.len ? cscdStrC(&dragLog.items[dragLog.len - 1]) : ""; }

static void testDragDrop(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} #src{width:80px;height:40px} #dst{position:absolute;left:200px;top:0;width:100px;height:100px}\n"
                  "      #no{position:absolute;left:400px;top:0;width:100px;height:100px}</style>\n"
                  "      <div id=src draggable=true>drag me</div><div id=dst></div><div id=no></div>");
    CscdImage img;
    cscdImageInit(&img, 800, 600);
    frame(v, &img);
    int clicks = 0;
    CscdElement *src = byId(v, "src"), *dst = byId(v, "dst");
    on(src, "dragstart", onDragStart, NULL);
    on(src, "dragend", onDragEnd, NULL);
    cscdElementOnClick(src, countEvent, &clicks);
    on(dst, "dragenter", onDragEnter, NULL);
    on(dst, "dragleave", onDragLeave, NULL);
    on(dst, "dragover", onDragOver, NULL);
    on(dst, "drop", onDrop, NULL);
    down(v, 10, 10);
    drag(v, 12, 11); /* below the threshold: no drag yet */
    CHECK(!cscdViewDragging(v));
    drag(v, 30, 20);
    CHECK(cscdViewDragging(v));
    drag(v, 450, 50); /* a target that does not accept */
    CHECK(cscdViewCursor(v) == CSCD_CURSOR_NOT_ALLOWED);
    drag(v, 250, 50);
    CHECK(cscdViewCursor(v) != CSCD_CURSOR_NOT_ALLOWED);
    frame(v, &img); /* the picture of the source follows the pointer */
    up(v, 250, 50);
    CHECK(!cscdViewDragging(v));
    CHECK(eq(got, "42"));
    CHECK(clicks == 0);
    CHECK(dragLog.len >= 3 && cscdStrEqC(&dragLog.items[0], "start") && cscdStrEqC(&dragLog.items[1], "enter") && eq(lastLog(), "end:move"));
    /* from another app: files */
    CscdDataTransfer* dt = cscdDataTransferNew();
    cscdVecPush(&dt->files, cscdStrDupC("C:/a.png"));
    cscdVecPush(&dt->files, cscdStrDupC("C:/b.mov"));
    cscdElementRemoveEventListeners(dst, CSCD_SV("dragover"));
    on(dst, "dragover", onFilesOver, NULL);
    cscdElementRemoveEventListeners(dst, CSCD_SV("drop"));
    on(dst, "drop", onFilesDrop, NULL);
    cscdViewExternalDragEnter(v, dt, 450, 50);
    cscdRelease(dt);
    CHECK(!cscdViewExternalDragOver(v, 460, 50));
    CHECK(cscdViewExternalDragOver(v, 250, 50));
    CHECK(cscdViewExternalDrop(v, 250, 50));
    CHECK(droppedFiles.len == 2 && cscdStrEqC(&droppedFiles.items[1], "C:/b.mov"));
    /* Esc cancels a drag */
    cscdStrVecFree(&dragLog);
    down(v, 10, 10);
    drag(v, 40, 30);
    keyNamed(v, CSCD_KEY_ESCAPE, "Escape");
    CHECK(!cscdViewDragging(v) && eq(lastLog(), "end:none"));
    up(v, 40, 30);
    cscdStrVecFree(&dragLog);
    cscdStrVecFree(&droppedFiles);
    cscdImageFini(&img);
    cscdViewFree(v);
}

/* ---- touch, splitters, text editing ----------------------------------------------------------------- */

static float zoom, panX;
static void onFirstClick(void* u, CscdEvent* e) {
    ++*(int*)u;
    CHECK(e->button == 0);
}
static void onPadWheel(void* u, CscdEvent* e) {
    (void)u;
    if (e->ctrl) zoom += e->deltaY;
    else panX += e->deltaX;
    cscdEventPreventDefault(e);
}

static void testTouch(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} #list{height:100px;overflow:auto} #list div{height:40px} #pad{position:absolute;left:300px;top:0;width:200px;height:200px}</style>\n"
                  "      <div id=list><div id=first>1</div><div>2</div><div>3</div><div>4</div><div>5</div></div><div id=pad></div>");
    CscdImage img;
    cscdImageInit(&img, 800, 600);
    frame(v, &img);
    int clicks = 0, downs = 0, ctxs = 0;
    zoom = panX = 0;
    cscdElementOnClick(byId(v, "first"), onFirstClick, &clicks);
    CscdElement* pad = byId(v, "pad");
    on(pad, "mousedown", countEvent, &downs);
    on(pad, "contextmenu", countEvent, &ctxs);
    on(pad, "wheel", onPadWheel, NULL);
    /* a tap clicks */
    cscdViewTouchDown(v, 1, 20, 20);
    cscdViewTouchUp(v, 1, 20, 20);
    CHECK(clicks == 1);
    /* a swipe scrolls the list, not clicks */
    cscdViewTouchDown(v, 1, 20, 90);
    cscdViewTouchMove(v, 1, 20, 70);
    cscdViewTouchMove(v, 1, 20, 40);
    cscdViewTouchUp(v, 1, 20, 40);
    CscdElement* list = byId(v, "list");
    CHECK(list->scrollTop > 40 && clicks == 1);
    /* a drag where nothing scrolls is a mouse drag */
    cscdViewTouchDown(v, 2, 350, 50);
    cscdViewTouchMove(v, 2, 380, 60);
    CHECK(downs == 1);
    cscdViewTouchUp(v, 2, 380, 60);
    /* a long press opens the context menu */
    cscdViewTouchDown(v, 3, 350, 50);
    sleepMs(600);
    cscdViewUpdate(v);
    CHECK(ctxs == 1);
    cscdViewTouchUp(v, 3, 350, 50);
    /* two fingers: pinch zooms (ctrl+wheel), moving together pans */
    cscdViewTouchDown(v, 4, 350, 100);
    cscdViewTouchDown(v, 5, 400, 100);
    cscdViewTouchMove(v, 5, 450, 100);
    CHECK(zoom < -50);
    panX = 0;
    cscdViewTouchMove(v, 4, 330, 100);
    cscdViewTouchMove(v, 5, 430, 100);
    CHECK(panX > 0);
    cscdViewTouchUp(v, 4, 330, 100);
    cscdViewTouchUp(v, 5, 430, 100);
    cscdImageFini(&img);
    cscdViewFree(v);
}

typedef struct MenuLog { int shows, hides; unsigned actions; CscdRect around; } MenuLog;
static void logMenu(void* user, bool show, CscdRect around, unsigned actions) {
    MenuLog* l = (MenuLog*)user;
    if (!show) { ++l->hides; return; }
    ++l->shows;
    l->actions = actions;
    l->around = around;
}
static int magnified;
static void logMagnifier(void* user, bool show, float x, float y) { (void)user; (void)x; (void)y; if (show) ++magnified; }
static CscdStr fakeClip;
static void fakeClipSet(void* user, CscdSv text) { (void)user; cscdStrSet(&fakeClip, text); }
static void fakeClipGet(void* user, CscdStr* out) { (void)user; cscdStrAppend(out, cscdStrSv(&fakeClip)); }

/* a slider follows a finger dragged along it, even on a page that scrolls and with a little of the
 * drag up or down; moves that come in one batch do not make a wild fling */
static void testTouchSlider(void) {
    CscdView* v = cscdViewNew();
    layoutViewWH(v, "<style>body{margin:0} #r{display:block;margin:20px;width:300px;height:24px} #tall{height:2000px}</style>"
                    "<input id=r type=range min=0 max=100 value=10><div id=tall></div>", 400, 600);
    CscdImage img;
    cscdImageInit(&img, 400, 600);
    frame(v, &img);
    int inputs = 0;
    on(byId(v, "r"), "input", countEvent, &inputs);
    const CscdRect r = rectOf(v, "r");
    const float y = r.y + r.h / 2, x0 = r.x + r.w * 0.1f;
    cscdViewTouchDown(v, 1, x0, y);
    cscdViewTouchMove(v, 1, x0 + 40, y + 6);
    cscdViewTouchMove(v, 1, x0 + 150, y + 12);
    cscdViewTouchUp(v, 1, x0 + 150, y + 12);
    CscdStr val = cscdElementValue(byId(v, "r"));
    CHECK(inputs > 0 && atoi(val.ptr ? val.ptr : "0") >= 55 && cscdViewScrollY(v) == 0);
    cscdStrFree(&val);
    /* up and down over the slider: the page scrolls, the slider stays */
    cscdViewTouchDown(v, 2, x0, y + 200);
    cscdViewTouchMove(v, 2, x0 + 5, y + 120);
    cscdViewTouchMove(v, 2, x0 + 8, y + 60);
    cscdViewTouchUp(v, 2, x0 + 8, y + 60);
    CHECK(cscdViewScrollY(v) > 100);
    /* a fling whose moves all come at once: no velocity from an instant */
    cscdViewScrollTo(v, 0, 0);
    cscdViewTouchDown(v, 3, 200, 500);
    for (int i = 1; i <= 10; ++i) cscdViewTouchMove(v, 3, 200, 500 - 30.f * i);
    cscdViewTouchUp(v, 3, 200, 200);
    const float after = cscdViewScrollY(v);
    sleepMs(300);
    for (int i = 0; i < 20; ++i) cscdViewUpdate(v);
    CHECK(after > 250 && cscdViewScrollY(v) - after < 600);
    cscdImageFini(&img);
    cscdViewFree(v);
}

/* touch-action keeps gestures from the view (the drag goes to the page as the mouse); touches' own times */
static void testTouchAction(void) {
    CscdView* v = cscdViewNew();
    layoutViewWH(v, "<style>body{margin:0} div{height:200px} #none{touch-action:none} #py{touch-action:pan-y}"
                    " #px{touch-action:pan-x} #tall{height:2000px}</style>"
                    "<div id=none></div><div id=py></div><div id=px></div><div id=tall></div>", 400, 600);
    CscdImage img;
    cscdImageInit(&img, 400, 600);
    frame(v, &img);
    int downs = 0;
    on(byId(v, "none"), "mousedown", countEvent, &downs);
    on(byId(v, "px"), "mousedown", countEvent, &downs);
    /* none: up and down is the page's drag, not a scroll */
    cscdViewTouchDown(v, 1, 200, 150);
    cscdViewTouchMove(v, 1, 202, 100);
    cscdViewTouchMove(v, 1, 204, 40);
    cscdViewTouchUp(v, 1, 204, 40);
    CHECK(cscdViewScrollY(v) == 0 && downs == 1);
    /* pan-x: up and down neither */
    cscdViewTouchDown(v, 2, 200, 550);
    cscdViewTouchMove(v, 2, 202, 500);
    cscdViewTouchUp(v, 2, 202, 500);
    CHECK(cscdViewScrollY(v) == 0 && downs == 2);
    /* pan-y: the page scrolls */
    cscdViewTouchDown(v, 3, 200, 390);
    cscdViewTouchMove(v, 3, 202, 330);
    cscdViewTouchMove(v, 3, 204, 250);
    cscdViewTouchUp(v, 3, 204, 250);
    CHECK(cscdViewScrollY(v) > 100);
    CHECK(byId(v, "none")->style->touchAction == CSCD_TA_SET && byId(v, "py")->style->touchAction == (CSCD_TA_SET | CSCD_TA_PAN_Y));
    /* a fling's moves taken together, each with its own time (as a platform tells them): the speed is the
     * finger's, 300 px in 100 ms, so the page glides on */
    cscdViewScrollTo(v, 0, 0);
    viewSetInputAge(v, 0.1);
    cscdViewTouchDown(v, 4, 200, 340); /* (over pan-y) */
    for (int i = 1; i <= 10; ++i) {
        viewSetInputAge(v, 0.1 - 0.01 * i);
        cscdViewTouchMove(v, 4, 200, 340 - 30.f * i);
    }
    viewSetInputAge(v, 0);
    cscdViewTouchUp(v, 4, 200, 40);
    const float after = cscdViewScrollY(v);
    sleepMs(150);
    for (int i = 0; i < 5; ++i) cscdViewUpdate(v);
    CHECK(cscdViewScrollY(v) - after > 100);
    cscdImageFini(&img);
    cscdViewFree(v);
}

/* a progress bar's value set each frame paints just the bar, unless a selector can see it */
static void testProgressRepaint(void) {
    CscdView* v = cscdViewNew();
    layoutViewWH(v, "<style>body{margin:0} progress{display:block;width:200px;height:10px} p{height:400px}</style>"
                    "<progress id=p value=0.1 max=1></progress><p>tall</p>", 400, 600);
    CscdImage img;
    cscdImageInit(&img, 400, 600);
    frame(v, &img);
    cscdViewPaint(v, &img, true); /* (from here on, each frame paints only what changed) */
    CscdElement* p = byId(v, "p");
    cscdElementSetAttribute(p, CSCD_SV("value"), CSCD_SV("0.9"));
    cscdViewPaint(v, &img, true);
    CscdFrameStats st = cscdViewFrameStats(v);
    CHECK(st.partialPaint && st.paintedArea < 0.1 * 400 * 600);
    const uint32_t filled = cscdImageRow(&img, 5)[150], track = cscdImageRow(&img, 5)[195];
    CHECK(filled != track); /* (the bar reaches 90% now) */
    /* a selector that tests the value: styles computed again, the page painted */
    cscdViewAddStyleSheet(v, CSCD_SV("progress[value=\"0.5\"] + p { height: 10px }"), CSCD_SV(""));
    frame(v, &img);
    cscdElementSetAttribute(p, CSCD_SV("value"), CSCD_SV("0.5"));
    cscdViewUpdate(v);
    CHECK(rectOf(v, "p").h > 0 && cscdRectBottom(cscdViewElementRect(v, cscdViewQuerySelector(v, CSCD_SV("p")))) < 100);
    cscdImageFini(&img);
    cscdViewFree(v);
}

static void testTouchSelection(void) {
    CscdView* v = cscdViewNew();
    layoutViewWH(v, "<style>body{margin:0} input{margin:20px;width:300px;font-size:20px}</style><input id=f value=\"hello brave world\">", 400, 200);
    CscdImage img;
    cscdImageInit(&img, 400, 200);
    frame(v, &img);
    MenuLog log = {0};
    magnified = 0;
    cscdViewOnEditMenu(v, logMenu, &log);
    cscdViewOnMagnifier(v, logMagnifier, NULL);
    cscdViewSetClipboardHooks(v, fakeClipSet, fakeClipGet, NULL, NULL, NULL);
    CscdElement* f = byId(v, "f");
    cscdViewFocus(v, f);
    frame(v, &img);
    const CscdRect inBrave = cscdViewTextRangeRect(v, 8, 8);
    cscdViewBlur(v);
    /* a long press selects the word there, with handles and the edit menu */
    cscdViewTouchDown(v, 1, inBrave.x, inBrave.y + inBrave.h / 2);
    sleepMs(600);
    cscdViewUpdate(v);
    cscdViewTouchUp(v, 1, inBrave.x, inBrave.y + inBrave.h / 2);
    CHECK(cscdViewDocument(v)->focused == f && f->selAnchor == 6 && f->caret == 11);
    CHECK(cscdViewSelectionHandlesShown(v) && log.shows == 1);
    CHECK((log.actions & CSCD_EDIT_BIT(CSCD_EDIT_COPY)) && (log.actions & CSCD_EDIT_BIT(CSCD_EDIT_CUT)) && (log.actions & CSCD_EDIT_BIT(CSCD_EDIT_PASTE)));
    CHECK(log.around.w > 10 && log.around.y >= 20);
    /* the handles are drawn under the selection's ends */
    frame(v, &img);
    const CscdRect end = cscdViewTextRangeRect(v, 11, 11);
    const float knobY = cscdRectBottom(end) + 8;
    const uint32_t px = cscdImageRow(&img, (int)knobY)[(int)end.x];
    CHECK((px & 0xFFu) > 200 && ((px >> 16) & 0xFFu) < 120); /* (blue, BGRA) */
    /* dragging the end handle takes the selection to the end; the magnifier follows, the menu waits */
    const CscdRect last = cscdViewTextRangeRect(v, 17, 17);
    cscdViewTouchDown(v, 2, end.x, knobY);
    CHECK(log.hides == 1);
    cscdViewTouchMove(v, 2, last.x + 8, knobY);
    CHECK(f->selAnchor == 6 && f->caret == 17 && magnified > 0);
    cscdViewTouchUp(v, 2, last.x + 8, knobY);
    CHECK(log.shows == 2);
    /* the menu's actions */
    CHECK(cscdViewEditAction(v, CSCD_EDIT_COPY) && cscdStrEqC(&fakeClip, "brave world"));
    CHECK(cscdViewEditAction(v, CSCD_EDIT_SELECT_ALL) && f->selAnchor == 0 && f->caret == 17);
    CHECK(!(cscdViewEditActions(v) & CSCD_EDIT_BIT(CSCD_EDIT_SELECT_ALL)));
    CHECK(cscdViewEditAction(v, CSCD_EDIT_PASTE) && cscdStrEqC(&f->editValue, "brave world"));
    CHECK(!cscdViewSelectionHandlesShown(v));
    /* a tap shows the caret's handle; a tap on the caret again asks for the menu */
    const int shows = log.shows;
    const CscdRect at3 = cscdViewTextRangeRect(v, 3, 3);
    sleepMs(400); /* (not a double tap) */
    cscdViewTouchDown(v, 3, at3.x, at3.y + at3.h / 2);
    cscdViewTouchUp(v, 3, at3.x, at3.y + at3.h / 2);
    CHECK(cscdViewSelectionHandlesShown(v) && log.shows == shows && f->caret == 3);
    sleepMs(400);
    cscdViewTouchDown(v, 4, at3.x, at3.y + at3.h / 2);
    cscdViewTouchUp(v, 4, at3.x, at3.y + at3.h / 2);
    CHECK(log.shows == shows + 1 && (log.actions & CSCD_EDIT_BIT(CSCD_EDIT_PASTE)) && !(log.actions & CSCD_EDIT_BIT(CSCD_EDIT_COPY)));
    /* a key takes them away */
    const CscdModifiers none = {0};
    cscdViewKeyDown(v, CSCD_KEY_RIGHT, none, CSCD_SV("ArrowRight"));
    CHECK(!cscdViewSelectionHandlesShown(v) && log.hides >= 2);
    /* a password is not copied */
    cscdElementSetAttribute(f, CSCD_SV("type"), CSCD_SV("password"));
    CHECK(cscdViewEditAction(v, CSCD_EDIT_SELECT_ALL));
    CHECK(!cscdViewEditAction(v, CSCD_EDIT_COPY) && !cscdViewEditAction(v, CSCD_EDIT_CUT));
    /* the clipboard from code */
    CHECK(cscdViewCopyText(v, CSCD_SV("from code")) && cscdStrEqC(&fakeClip, "from code"));
    CscdStr got = {0};
    cscdViewClipboardText(v, &got);
    CHECK(cscdStrEqC(&got, "from code"));
    CHECK(!cscdViewCopyImage(v, &img)); /* (no picture hook) */
    cscdStrFree(&got);
    cscdStrFree(&fakeClip);
    cscdImageFini(&img);
    cscdViewFree(v);
}

static void testSplitter(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} #row{display:flex;width:600px;height:100px} #a,#c{flex:1} #b{width:6px} #c{min-width:100px}</style>\n"
                  "      <div id=row><div id=a></div><div id=b splitter></div><div id=c></div></div>");
    CscdImage img;
    cscdImageInit(&img, 800, 600);
    frame(v, &img);
    int changes = 0;
    on(byId(v, "b"), "change", countEvent, &changes);
    NEAR(rectOf(v, "a").w, 297);
    moveTo(v, 300, 50);
    CHECK(cscdViewCursor(v) == CSCD_CURSOR_COL_RESIZE);
    down(v, 300, 50);
    drag(v, 200, 50);
    cscdViewUpdate(v);
    NEAR(rectOf(v, "a").w, 197);
    NEAR(rectOf(v, "c").w, 397);
    drag(v, 590, 50); /* c keeps its min-width */
    cscdViewUpdate(v);
    NEAR(rectOf(v, "c").w, 100);
    up(v, 590, 50);
    CHECK(changes == 1);
    /* the window grows: the panels keep their proportion */
    cscdElementSetStyle(byId(v, "row"), CSCD_SV("width"), CSCD_SV("1200px"));
    cscdViewUpdate(v);
    NEAR(rectOf(v, "c").w, 201); /* 100 of 594 px, now of 1194 */
    cscdImageFini(&img);
    cscdViewFree(v);
}

static void highlightFirstTwo(void* u, CscdSv s, CscdTextSpanVec* out) {
    (void)u;
    if (s.len > 2) {
        CscdTextSpan sp = {0, 2, cscdRgb(255, 0, 0), true};
        cscdVecPush(out, sp);
    }
}

static void testTextEditing(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} textarea{width:300px;height:60px;font:16px monospace;line-height:20px;padding:0;border:0}</style>\n"
                  "      <textarea id=t code>if (a) {</textarea>");
    CscdImage img;
    cscdImageInit(&img, 800, 600);
    frame(v, &img);
    CscdElement* t = byId(v, "t");
    cscdViewFocus(v, t);
    /* auto-indent after a brace, Tab to the next stop */
    key(v, CSCD_KEY_ENTER);
    CHECK(eq(valueOf(t), "if (a) {\n    "));
    text(v, "x");
    key(v, CSCD_KEY_ENTER);
    CHECK(eq(valueOf(t), "if (a) {\n    x\n    "));
    /* undo the runs back, redo again */
    keyMods(v, CSCD_KEY_Z, ctrlMod());
    CHECK(eq(valueOf(t), "if (a) {\n    x"));
    keyMods(v, CSCD_KEY_Z, ctrlMod());
    keyMods(v, CSCD_KEY_Z, ctrlMod());
    CHECK(eq(valueOf(t), "if (a) {"));
    keyMods(v, CSCD_KEY_Y, ctrlMod());
    CHECK(eq(valueOf(t), "if (a) {\n    "));
    /* Up and Down move by line, keeping the column */
    cscdElementSetValue(t, CSCD_SV("abcdef\nab\nabcdef"));
    cscdViewFocus(v, NULL);
    cscdViewFocus(v, t);
    t->caret = t->selAnchor = 5;
    key(v, CSCD_KEY_DOWN);
    CHECK(t->caret == 9);
    key(v, CSCD_KEY_DOWN);
    CHECK(t->caret == 12);
    keyMods(v, CSCD_KEY_UP, shiftMod());
    CHECK(t->caret == 9 && t->selAnchor == 12);
    /* indent the selected lines, then outdent them */
    t->selAnchor = 0;
    t->caret = 9;
    key(v, CSCD_KEY_TAB);
    CHECK(eq(valueOf(t), "    abcdef\n    ab\nabcdef"));
    keyMods(v, CSCD_KEY_TAB, shiftMod());
    CHECK(eq(valueOf(t), "abcdef\nab\nabcdef"));
    /* a click puts the caret on the line under it */
    frame(v, &img);
    down(v, 2, 45);
    up(v, 2, 45);
    CHECK(t->caret == 10);
    /* highlighting: the spans come back for the painter */
    CscdHighlighter hl = {highlightFirstTwo, NULL, NULL};
    cscdElementSetHighlighter(t, hl);
    CscdStr val = cscdElementValue(t);
    CHECK(cscdElementHighlights(t, cscdStrSv(&val))->len == 1);
    cscdStrFree(&val);
    frame(v, &img);
    /* input method composition: shown, not in the value until committed */
    cscdViewSetComposition(v, CSCD_SV("ni"));
    frame(v, &img);
    CHECK(eq(valueOf(t), "abcdef\nab\nabcdef"));
    CscdRect cr = cscdViewCaretRect(v);
    CHECK(cr.h > 0 && cr.y >= 39);
    cscdViewSetComposition(v, CSCD_SV(""));
    text(v, "\xE4\xBD\xA0");
    {
        const char* now = valueOf(t);
        const char* at = strstr(now, "\xE4\xBD\xA0");
        CHECK(at && at - now == 10);
    }
    /* a long text scrolls with the wheel */
    CscdStr many = {0};
    for (int i = 0; i < 20; ++i) cscdStrAppendC(&many, "line\n");
    cscdElementSetValue(t, cscdStrSv(&many));
    cscdStrFree(&many);
    frame(v, &img);
    cscdViewWheel(v, 10, 10, 0, 100, none());
    /* it glides there over a few frames, and lands exactly */
    CHECK(t->scrollTop < 100);
    for (int i = 0; i < 40 && cscdViewAnimating(v); ++i) {
        sleepMs(10);
        cscdViewUpdate(v);
    }
    CHECK(t->scrollTop > 50);
    cscdImageFini(&img);
    cscdViewFree(v);
}

/* ---- the keyboard in forms and controls, the base sheet ----------------------------------------------- */

static void testKeyboardAndBaseSheet(void) {
    CscdView* v = cscdViewNew();
    CscdImage img;
    cscdImageInit(&img, 800, 600);
    int warnings = 0;
    cscdViewOnWarning(v, countWarning, &warnings);
    /* the base sheet: under the document's own, kept from page to page, its misspellings warned about */
    cscdViewSetBaseStyleSheet(v, CSCD_SV("p{color:#112233;colr:red} #b{color:#445566}"), CSCD_SV(""));
    CHECK(warnings == 1);
    layoutView(v, "<style>#b{color:#778899}</style><p id=a>a</p><p id=b>b</p>");
    CHECK(byId(v, "a")->style->color.r == 0x11 && byId(v, "b")->style->color.r == 0x77);
    layoutView(v, "<p id=a>again</p>");
    CHECK(byId(v, "a")->style->color.r == 0x11);
    cscdViewSetBaseStyleSheet(v, CSCD_SV("p{color:#aabbcc}"), CSCD_SV(""));
    cscdViewUpdate(v);
    CHECK(byId(v, "a")->style->color.r == 0xaa);

    /* Enter in a field presses the form's default button: a dialog form closes with its value */
    layoutView(v, "<style>body{margin:0} dialog{width:300px;height:100px;padding:0;border:0}</style>"
                  "<dialog id=d><form method=dialog><input id=name value=x><button id=ok value=ok>OK</button></form></dialog>");
    CscdElement* d = byId(v, "d");
    int okClicks = 0;
    cscdElementOnClick(byId(v, "ok"), countEvent, &okClicks);
    cscdViewShowModal(v, d);
    frame(v, &img);
    CHECK(cscdViewDocument(v)->focused == byId(v, "name"));
    key(v, CSCD_KEY_ENTER);
    CHECK(okClicks == 1 && !cscdElementHasAttributeC(d, "open") && cscdSvEqC(cscdElementAttrC(d, "returnvalue"), "ok"));

    /* data-focus="keyboard": Tab reaches it, a click does not, Esc leaves it; Tab goes on from there */
    layoutView(v, "<style>body{margin:0} div{width:100px;height:40px}</style>"
                  "<div id=one tabindex=0 data-focus=keyboard></div><div id=two tabindex=0 data-focus=keyboard></div>"
                  "<div id=three tabindex=0></div>");
    frame(v, &img);
    CscdRect one = viewRect(v, byId(v, "one"));
    down(v, one.x + 5, one.y + 5);
    up(v, one.x + 5, one.y + 5);
    CHECK(!cscdViewDocument(v)->focused);
    key(v, CSCD_KEY_TAB);
    CHECK(cscdViewDocument(v)->focused == byId(v, "one"));
    key(v, CSCD_KEY_TAB);
    CHECK(cscdViewDocument(v)->focused == byId(v, "two"));
    keyNamed(v, CSCD_KEY_ESCAPE, "Escape");
    CHECK(!cscdViewDocument(v)->focused);
    key(v, CSCD_KEY_TAB);
    CHECK(cscdViewDocument(v)->focused == byId(v, "three"));
    /* a new page: Tab starts from its top (not from the third place, where it was) */
    cscdViewFocus(v, NULL);
    layoutView(v, "<button id=first>1</button><button>2</button><button>3</button><button>4</button><button>5</button>");
    key(v, CSCD_KEY_TAB);
    CHECK(cscdViewDocument(v)->focused == byId(v, "first"));
    cscdImageFini(&img);
    cscdViewFree(v);
}

/* ---- dialogs, images, popovers ---------------------------------------------------------------------- */

static void testDialogs(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} #page{width:200px;height:50px} dialog{width:300px;height:100px;padding:0;border:0}</style>\n"
                  "      <button id=page>page</button>\n"
                  "      <dialog id=d><form method=dialog><input id=name value=x><button id=ok value=ok>OK</button><button id=no value=cancel>Cancel</button></form></dialog>");
    CscdImage img;
    cscdImageInit(&img, 800, 600);
    frame(v, &img);
    CscdElement* d = byId(v, "d");
    CHECK(!d->layout); /* closed: not shown */
    int pageClicks = 0, closes = 0;
    cscdElementOnClick(byId(v, "page"), countEvent, &pageClicks);
    on(d, "close", countEvent, &closes);
    cscdViewShowModal(v, d);
    frame(v, &img);
    CscdRect r = viewRect(v, d);
    NEAR(r.x, 250);
    NEAR(r.y, 250);
    CHECK(cscdViewDocument(v)->focused == byId(v, "name"));
    /* the page under it takes no clicks */
    down(v, 20, 20);
    up(v, 20, 20);
    CHECK(pageClicks == 0 && cscdElementHasAttributeC(d, "open"));
    /* Tab stays inside */
    key(v, CSCD_KEY_TAB);
    key(v, CSCD_KEY_TAB);
    key(v, CSCD_KEY_TAB);
    CHECK(cscdViewDocument(v)->focused == byId(v, "name"));
    /* its form's button closes it with its value */
    CscdRect ok = viewRect(v, byId(v, "ok"));
    down(v, ok.x + 2, ok.y + 2);
    up(v, ok.x + 2, ok.y + 2);
    CHECK(!cscdElementHasAttributeC(d, "open") && cscdSvEqC(cscdElementAttrC(d, "returnvalue"), "ok") && closes == 1);
    /* Esc cancels */
    cscdViewShowModal(v, d);
    frame(v, &img);
    keyNamed(v, CSCD_KEY_ESCAPE, "Escape");
    CHECK(!cscdElementHasAttributeC(d, "open") && !cscdElementAttrC(d, "returnvalue").len && closes == 2);
    /* grown while open: centered again */
    cscdViewShowModal(v, d);
    frame(v, &img);
    cscdElementSetStyle(d, CSCD_SV("height"), CSCD_SV("300px"));
    frame(v, &img);
    NEAR(viewRect(v, d).y, 150);
    cscdViewCloseDialog(v, d, CSCD_SV(""));
    CHECK(!cscdViewModalDialog(v));
    frame(v, &img);
    down(v, 20, 20);
    up(v, 20, 20);
    CHECK(pageClicks == 1);
    cscdImageFini(&img);
    cscdViewFree(v);
}

static void countPaint(void* user, CscdCanvas* c, float w, float h) { (void)c; (void)w; (void)h; ++*(int*)user; }

static void testAppImages(void) {
    /* an image the app provides under its own scheme, in a page loaded from a folder */
    CscdView* v = cscdViewNew();
    CscdImage* thumb = cscdImageNew(40, 30);
    cscdViewSetImage(v, CSCD_SV("thumb:7"), thumb);
    cscdRelease(thumb);
    cscdViewLoadHTML(v, CSCD_SV("<style>body{margin:0}</style><img id=i src=\"thumb:7\">"), CSCD_SV("C:/some/folder"));
    cscdViewSetViewport(v, 800, 600, 1);
    cscdViewUpdate(v);
    NEAR(rectOf(v, "i").w, 40);
    NEAR(rectOf(v, "i").h, 30);
    cscdViewFree(v);
    /* ::selection colors a field's selected text */
    CscdView* w = cscdViewNew();
    layoutView(w, "<style>body{margin:0} input{font:16px sans-serif} ::selection{background:#ff0000}</style><input id=f value=\"hello\">");
    CscdElement* f = byId(w, "f");
    cscdViewFocus(w, f);
    f->selAnchor = 0;
    f->caret = 5;
    CscdImage img;
    cscdImageInit(&img, 200, 60);
    cscdViewPaint(w, &img, false);
    cscdImageFini(&img);
    const CscdComputedStyle* sel = cscdStyleEnginePseudoStyle(cscdViewStyleEngine(w), f, CSCD_SV("selection"));
    CHECK(sel && cscdColorEq(sel->backgroundColor, cscdRgb(255, 0, 0)));
    cscdViewFree(w);
    /* an element's own painting stays hidden with it (visibility: hidden) */
    CscdView* h = cscdViewNew();
    layoutView(h, "<style>body{margin:0} div{width:20px;height:20px}</style><div id=a></div><div id=b style=\"visibility:hidden\"></div>");
    int painted = 0;
    CscdPainter p = {countPaint, &painted, NULL};
    cscdElementSetPainter(byId(h, "a"), p);
    cscdElementSetPainter(byId(h, "b"), p);
    CscdImage out;
    cscdImageInit(&out, 40, 60);
    cscdViewPaint(h, &out, false);
    cscdImageFini(&out);
    CHECK(painted == 1);
    cscdViewFree(h);
}

static void testPopovers(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} #b{width:80px;height:20px} [popover]{width:100px;height:60px;padding:0;border:0}</style>\n"
                  "      <button id=b>pick</button><div id=p popover><button id=in>x</button></div><div id=out style=\"height:300px\"></div>");
    CscdImage img;
    cscdImageInit(&img, 800, 600);
    frame(v, &img);
    CscdElement* p = byId(v, "p");
    int toggles = 0;
    on(p, "toggle", countEvent, &toggles);
    cscdViewShowPopover(v, p, byId(v, "b"));
    frame(v, &img);
    CHECK(cscdViewPopoverOpen(v, p) && toggles == 1);
    NEAR(viewRect(v, p).y, cscdRectBottom(viewRect(v, byId(v, "b"))) + 4);
    /* a press inside keeps it, outside hides it */
    CscdRect in = viewRect(v, byId(v, "in"));
    down(v, in.x + 2, in.y + 2);
    up(v, in.x + 2, in.y + 2);
    CHECK(cscdViewPopoverOpen(v, p));
    down(v, 400, 400);
    up(v, 400, 400);
    CHECK(!cscdViewPopoverOpen(v, p) && toggles == 2);
    /* Esc hides it */
    cscdViewShowPopover(v, p, byId(v, "b"));
    keyNamed(v, CSCD_KEY_ESCAPE, "Escape");
    CHECK(!cscdViewPopoverOpen(v, p));
    cscdImageFini(&img);
    cscdViewFree(v);
}

/* ---- the canvas ------------------------------------------------------------------------------------- */

static int red(const CscdImage* img, int x, int y) { return (int)((img->pixels[(size_t)y * img->width + x] >> 16) & 255); }

/* Clips: a mask covers only its own area, and what it lets through is the shape. */
static void testClips(void) {
    CscdImage clipped, filled;
    cscdImageInit(&clipped, 200, 120);
    cscdImageInit(&filled, 200, 120);
    CscdRRect rr = cscdRRect(cscdRect(20.3f, 10.6f, 150.2f, 90.5f));
    for (int i = 0; i < 4; ++i) rr.rx[i] = rr.ry[i] = 12.5f;
    {
        CscdCanvas* c = cscdCanvasNew(&clipped);
        cscdCanvasClipRRect(c, &rr);
        cscdCanvasFillRect(c, cscdRect(0, 0, 200, 120), cscdRgb(255, 0, 0));
        cscdCanvasFree(c);
    }
    {
        CscdCanvas* c = cscdCanvasNew(&filled);
        cscdCanvasFillRRect(c, &rr, cscdRgb(255, 0, 0));
        cscdCanvasFree(c);
    }
    int worst = 0;
    for (int y = 0; y < 120; ++y)
        for (int x = 0; x < 200; ++x) worst = cscdMaxi(worst, abs(red(&clipped, x, y) - red(&filled, x, y)));
    CHECK(worst <= 2);
    CHECK(red(&clipped, 100, 50) == 255 && red(&clipped, 21, 11) == 0 && red(&clipped, 5, 5) == 0);
    /* nested: a rectangle, then a path inside it; and drawing outside the clip leaves the pixels alone */
    CscdImage nested;
    cscdImageInit(&nested, 100, 100);
    {
        CscdCanvas* c = cscdCanvasNew(&nested);
        cscdCanvasClipRect(c, cscdRect(10.5f, 10.5f, 60, 60));
        CscdPath tri = {0};
        cscdPathMoveTo(&tri, 0, 0);
        cscdPathLineTo(&tri, 100, 0);
        cscdPathLineTo(&tri, 0, 100);
        cscdPathClose(&tri);
        cscdCanvasClipPath(c, &tri, CSCD_FILL_NONZERO);
        cscdPathFree(&tri);
        cscdCanvasFillRect(c, cscdRect(0, 0, 100, 100), cscdRgb(255, 0, 0));
        cscdCanvasFree(c);
    }
    CHECK(red(&nested, 20, 20) == 255);
    CHECK(red(&nested, 60, 60) == 0); /* past the diagonal */
    CHECK(red(&nested, 5, 20) == 0 && red(&nested, 20, 80) == 0);
    cscdImageFini(&clipped);
    cscdImageFini(&filled);
    cscdImageFini(&nested);
}

/* ---- repainting part of the page -------------------------------------------------------------------- */

static int colorA, paintsA, paintsB;
static void painterA(void* u, CscdCanvas* c, float w, float h) {
    (void)u;
    ++paintsA;
    cscdCanvasFillRect(c, cscdRect(10, 10, w - 20, h - 20), cscdRgb(colorA, 0, 0));
}
static void painterB(void* u, CscdCanvas* c, float w, float h) {
    (void)u; (void)w; (void)h;
    ++paintsB;
    cscdCanvasFillCircle(c, cscdPoint(50, 40), 20, cscdRgb(0, 200, 0));
}

/* invalidate(element) paints just that element again, and the result is the page as a whole repaint
 * would make it. */
static void translucentPainter(void* user, CscdCanvas* c, float w, float h) {
    cscdCanvasFillRect(c, cscdRect(0, 0, w, h), cscdRgba(255, 0, 0, *(int*)user));
}

/* A page over the host's picture: what it leaves empty stays empty, whatever was in the target, and
 * nothing builds up from one partial paint to the next. */
static void testTransparentView(void) {
    static const char page[] = "<style>html,body{margin:0;background:transparent}"
                               "#solid{position:absolute;left:10px;top:10px;width:50px;height:50px;background:#7c3aed}"
                               "#half{position:absolute;left:100px;top:10px;width:50px;height:50px;background:rgba(0,0,255,.5)}"
                               "#big{position:absolute;left:0;top:100px;width:800px;height:400px}</style>"
                               "<div id=solid></div><div id=half></div><div id=big></div>";
    CscdView* v = cscdViewNew();
    cscdViewSetTransparent(v, true);
    CscdImage fr, whole;
    cscdImageInit(&fr, 800, 600);
    cscdImageInit(&whole, 800, 600);
    memset(fr.pixels, 0xAB, cscdImagePixelCount(&fr) * 4);
    cscdViewSetViewport(v, 800, 600, 1);
    cscdViewPaint(v, &fr, false); /* (no page yet) */
    CHECK(cscdImageRow(&fr, 300)[400] == 0);

    cscdViewLoadHTML(v, cscdSv(page), CSCD_SV(""));
    int alpha = 128;
    CscdPainter painter = {translucentPainter, &alpha, NULL};
    cscdElementSetPainter(byId(v, "big"), painter);
    memset(fr.pixels, 0xAB, cscdImagePixelCount(&fr) * 4);
    cscdViewPaint(v, &fr, true);
    CHECK(cscdImageRow(&fr, 5)[5] == 0);                /* empty, over what the target held */
    CHECK(cscdImageRow(&fr, 30)[30] >> 24 == 255);      /* opaque stays opaque */
    const uint32_t half = cscdImageRow(&fr, 30)[120];
    CHECK(half >> 24 >= 127 && half >> 24 <= 128 && (half & 255) <= half >> 24 && ((half >> 16) & 255) == 0); /* premultiplied */

    /* partial paints, large enough to go by bands, give what a whole one gives */
    for (int i = 0; i < 5; ++i) {
        cscdViewInvalidateElement(v, byId(v, "big"));
        cscdViewPaint(v, &fr, true);
    }
    CHECK(cscdViewFrameStats(v).partialPaint);
    CHECK(cscdImageRow(&fr, 300)[400] >> 24 >= 127 && cscdImageRow(&fr, 300)[400] >> 24 <= 128);
    cscdViewPaint(v, &whole, false);
    CHECK(imagesEqual(&fr, &whole));
    cscdViewSetFeature(v, CSCD_FEATURE_PAINT_THREADS, false);
    alpha = 40;
    cscdViewInvalidateElement(v, byId(v, "big"));
    cscdViewPaint(v, &fr, true);
    cscdViewPaint(v, &whole, false);
    CHECK(imagesEqual(&fr, &whole));
    cscdViewSetFeature(v, CSCD_FEATURE_PAINT_THREADS, true);

    /* the dark canvas stays out too, and a size that ends inside a device pixel */
    cscdViewSetDarkMode(v, true);
    cscdViewPaint(v, &fr, false);
    CHECK(cscdImageRow(&fr, 5)[5] == 0);
    cscdViewSetDarkMode(v, false);
    cscdViewSetViewport(v, 533, 300, 1.5f);
    cscdViewPaint(v, &fr, false);
    CHECK(cscdImageRow(&fr, 5)[5] == 0);
    cscdViewSetViewport(v, 800, 600, 1);

    /* off, the canvas comes back at once, in a frame that was only being patched */
    cscdViewPaint(v, &fr, true);
    cscdViewSetTransparent(v, false);
    cscdViewPaint(v, &fr, true);
    CHECK(cscdImageRow(&fr, 5)[5] == 0xFFFFFFFFu);
    cscdImageFini(&fr);
    cscdImageFini(&whole);
    cscdViewFree(v);
}

static void testPartialRepaint(void) {
    CscdView* v = cscdViewNew();
    cscdViewSetViewport(v, 300, 200, 1);
    cscdViewLoadHTML(v, CSCD_SV("<body style='margin:0;background:#202020'><div id='a' style='width:100px;height:80px'></div>"
                            "<div id='b' style='margin-top:20px;width:100px;height:80px;background:#304050'></div><p>text</p></body>"),
                   CSCD_SV(""));
    colorA = paintsA = paintsB = 0;
    CscdPainter pa = {painterA, NULL, NULL}, pb = {painterB, NULL, NULL};
    cscdElementSetPainter(byId(v, "a"), pa);
    cscdElementSetPainter(byId(v, "b"), pb);
    CscdImage fr, whole;
    cscdImageInit(&fr, 300, 200);
    cscdImageInit(&whole, 300, 200);
    cscdViewPaint(v, &fr, true);
    CHECK(paintsA == 1 && paintsB == 1);
    colorA = 255;
    cscdViewInvalidateElement(v, byId(v, "a"));
    cscdViewPaint(v, &fr, true);
    CHECK(paintsA == 2 && paintsB == 1); /* b is not asked again */
    cscdViewPaint(v, &whole, false);
    CHECK(imagesEqual(&fr, &whole));
    /* after painting elsewhere, the next frame is a whole one */
    colorA = 128;
    cscdViewInvalidateElement(v, byId(v, "a"));
    const int before = paintsB;
    cscdViewPaint(v, &fr, true);
    CHECK(paintsB == before + 1);
    /* a change of the page itself paints everything */
    cscdElementSetAttribute(byId(v, "b"), CSCD_SV("style"), CSCD_SV("margin-top:20px;width:100px;height:80px;background:#506070"));
    cscdViewPaint(v, &fr, true);
    cscdViewPaint(v, &whole, false);
    CHECK(imagesEqual(&fr, &whole));
    /* hit testing still knows the page after a partial paint */
    cscdViewInvalidateElement(v, byId(v, "a"));
    cscdViewPaint(v, &fr, true);
    CHECK(cscdViewElementAt(v, 50, 140) == byId(v, "b"));
    cscdImageFini(&fr);
    cscdImageFini(&whole);
    cscdViewFree(v);
}

static int painted;
static void paintGreen(void* u, CscdCanvas* c, float w, float h) {
    (void)u;
    ++painted;
    cscdCanvasFillRect(c, cscdRect(0, 0, w, h), cscdRgb(0, 90, 0));
}
static void paintYellow(void* u, CscdCanvas* c, float w, float h) {
    (void)u;
    ++painted;
    cscdCanvasFillRect(c, cscdRect(0, 0, w, h), cscdRgb(200, 200, 0));
}

/* A text that changes and moves nothing (a clock, a counter): only its block is painted again, and the
 * page still answers clicks where it is. */
static void testTextRepaint(void) {
    CscdView* v = cscdViewNew();
    cscdViewSetViewport(v, 300, 200, 1);
    cscdViewLoadHTML(v, CSCD_SV("<body style='margin:0;background:#202020;color:#fff'><span id='t' style='display:inline-block;width:120px'>00:01</span>"
                            "<div id='c' style='margin-top:12px;width:100px;height:60px'></div><button id='b'>Go</button></body>"),
                   CSCD_SV(""));
    painted = 0;
    int clicks = 0;
    CscdPainter p = {paintGreen, NULL, NULL};
    cscdElementSetPainter(byId(v, "c"), p);
    cscdElementOnClick(byId(v, "b"), countEvent, &clicks);
    CscdImage fr, whole;
    cscdImageInit(&fr, 300, 200);
    cscdImageInit(&whole, 300, 200);
    cscdViewPaint(v, &fr, true);
    const int first = painted;
    cscdElementSetText(byId(v, "t"), CSCD_SV("00:02"));
    cscdViewPaint(v, &fr, true);
    CHECK(painted == first); /* the painted block below is not asked again */
    cscdViewPaint(v, &whole, false);
    CHECK(imagesEqual(&fr, &whole));
    CscdRect r = viewRect(v, byId(v, "b"));
    cscdElementSetText(byId(v, "t"), CSCD_SV("00:03"));
    cscdViewPaint(v, &fr, true);
    down(v, r.x + 5, r.y + 5);
    up(v, r.x + 5, r.y + 5);
    CHECK(clicks == 1);
    /* a text that grows its box moves what follows: everything is painted */
    cscdElementSetAttribute(byId(v, "t"), CSCD_SV("style"), CSCD_SV(""));
    cscdViewPaint(v, &fr, true);
    const int before = painted;
    cscdElementSetText(byId(v, "t"), CSCD_SV("a much longer text than before, wrapping the line further"));
    cscdViewPaint(v, &fr, true);
    CHECK(painted == before + 1);
    cscdViewPaint(v, &whole, false);
    CHECK(imagesEqual(&fr, &whole));
    cscdImageFini(&fr);
    cscdImageFini(&whole);
    cscdViewFree(v);
}

/* :hover changes only the hovered elements' styles; with nothing moved, only they are painted again, as
 * a whole repaint would paint them. A scroll repaints its box and finds things again. */
static void testStateRepaint(void) {
    CscdView* v = cscdViewNew();
    cscdViewSetViewport(v, 300, 300, 1);
    cscdViewLoadHTML(v, CSCD_SV("<style>html,body{height:100%} body{margin:0;background:#202020} .b{width:120px;height:30px;background:#333;margin:4px}"
                            " .b:hover{background:#4a6} .row:hover .icon{background:#a46} .icon{width:10px;height:10px}"
                            " .list{height:80px;overflow:auto} .list div{height:30px}</style>"
                            "<div class=b id=a></div><div class='b row' id=r><div class=icon></div></div><div id=p style='width:50px;height:20px'></div>"
                            "<div class=list id=l><div>1</div><div>2</div><div>3</div><div>4</div><div>5</div></div>"),
                   CSCD_SV(""));
    painted = 0;
    CscdPainter pp = {paintYellow, NULL, NULL};
    cscdElementSetPainter(byId(v, "p"), pp);
    CscdImage fr, whole;
    cscdImageInit(&fr, 300, 300);
    cscdImageInit(&whole, 300, 300);
    moveTo(v, 290, 290); /* on the page already (entering it changes the root's :hover) */
    cscdViewPaint(v, &fr, true);
    const int first = painted;
    CscdRect a = viewRect(v, byId(v, "a")), r = viewRect(v, byId(v, "r"));
    moveTo(v, a.x + 5, a.y + 5);
    cscdViewPaint(v, &fr, true);
    CHECK(painted == first); /* the painted block is not asked again */
    cscdViewPaint(v, &whole, false);
    CHECK(imagesEqual(&fr, &whole));
    moveTo(v, r.x + 5, r.y + 5); /* the row and its icon */
    cscdViewPaint(v, &fr, true);
    cscdViewPaint(v, &whole, false);
    CHECK(imagesEqual(&fr, &whole));
    CHECK(cscdNodeFirstElementChild(&byId(v, "r")->node)->style->backgroundColor.r == 0xaa);
    /* a scrolled list: its box again, and the clicks land on what is there now */
    CscdRect l = viewRect(v, byId(v, "l"));
    moveTo(v, l.x + 5, l.y + 5);
    cscdViewPaint(v, &fr, true);
    cscdViewWheel(v, l.x + 5, l.y + 5, 0, 40, none());
    for (int i = 0; i < 40 && cscdViewAnimating(v); ++i) {
        sleepMs(10);
        cscdViewPaint(v, &fr, true);
    }
    CHECK(byId(v, "l")->scrollTop > 30);
    cscdViewPaint(v, &whole, false);
    CHECK(imagesEqual(&fr, &whole));
    CscdElement* under = cscdViewElementAt(v, l.x + 5, l.y + 5);
    CHECK(under && eq(textOf(under), "2"));
    cscdImageFini(&fr);
    cscdImageFini(&whole);
    cscdViewFree(v);
}

/* A picture only scaled takes a quick path; it draws what the image pattern draws. */
static void testScaledImage(void) {
    CscdImage* pic = cscdImageNew(37, 23);
    for (int y = 0; y < 23; ++y)
        for (int x = 0; x < 37; ++x) cscdImageRow(pic, y)[x] = cscdColorPremul(cscdRgba(x * 7, y * 11, (x * y) % 255, 200 + (x + y) % 55));
    for (int s = 0; s < 2; ++s) {
        const bool smooth = s == 0;
        CscdImage fast, pattern;
        cscdImageInit(&fast, 160, 120);
        cscdImageInit(&pattern, 160, 120);
        const CscdRect dst = {10.3f, 7.6f, 121.4f, 83.2f};
        {
            CscdCanvas* c = cscdCanvasNew(&fast);
            cscdCanvasScale(c, 1.25f, 1.25f);
            cscdCanvasSetGlobalAlpha(c, 0.8f);
            cscdCanvasDrawImage(c, pic, dst, smooth);
            cscdCanvasFree(c);
        }
        {
            CscdCanvas* c = cscdCanvasNew(&pattern);
            cscdCanvasScale(c, 1.25f, 1.25f);
            cscdCanvasSetGlobalAlpha(c, 0.8f);
            CscdShader sh = cscdShaderImage(pic, dst, false, false);
            sh.smooth = smooth;
            cscdCanvasFillRectShader(c, dst, &sh);
            cscdShaderFini(&sh);
            cscdCanvasFree(c);
        }
        /* inside (a partly covered edge row must not drop to rounding) */
        int worst = 0;
        for (int y = 11; y < 112; ++y)
            for (int x = 14; x < 159; ++x) {
                const uint32_t f = cscdImageRow(&fast, y)[x], q = cscdImageRow(&pattern, y)[x];
                for (int ch = 0; ch < 32; ch += 8) worst = cscdMaxi(worst, abs((int)((f >> ch) & 255) - (int)((q >> ch) & 255)));
            }
        CHECK(worst <= 2);
        cscdImageFini(&fast);
        cscdImageFini(&pattern);
    }
    cscdRelease(pic);
}

/* Elements with the same rules share one computed style, kept between restyles; whatever changes them
 * still shows. */
static void testStyleSharing(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>li{color:#111} .k{color:#222} body.dark li{color:#eee} .w{width:50vw;height:10px} .t{opacity:1;transition:opacity 1s linear}"
                  " .t.off{opacity:0} .b{width:40px;height:20px;background:#333} .b:hover{background:#4a6} .g{width:40px;height:20px} .g:hover{width:90px}</style>"
                  "<ul><li id=a>1</li><li id=b>2</li><li id=c class=k>3</li></ul><div id=w class=w></div>"
                  "<div id=t1 class=t></div><div id=t2 class=t></div><div id=hb class=b></div><div id=hg class=g></div>");
    CscdElement *a = byId(v, "a"), *b = byId(v, "b"), *c = byId(v, "c");
    CHECK(a->style == b->style);
    CHECK(a->style != c->style && c->style->color.r == 0x22);
    CscdComputedStyle* shared = (CscdComputedStyle*)cscdRetain(a->style);
    CscdElement* body = cscdDocumentBody(cscdViewDocument(v));
    cscdElementAddClass(body, CSCD_SV("dark"));
    cscdViewUpdate(v);
    CHECK(a->style->color.r == 0xee && c->style->color.r == 0xee);
    cscdElementRemoveClass(body, CSCD_SV("dark"));
    cscdViewUpdate(v);
    CHECK(a->style == shared); /* the style from before, kept */
    /* viewport units follow the viewport; the other styles stay */
    cscdViewSetViewport(v, 600, 600, 1);
    cscdViewUpdate(v);
    NEAR(rectOf(v, "w").w, 300);
    CHECK(a->style == shared);
    cscdRelease(shared);
    /* a transition changes its element's style, not the one its twin shares */
    CscdElement *t1 = byId(v, "t1"), *t2 = byId(v, "t2");
    cscdElementAddClass(t1, CSCD_SV("off"));
    cscdViewUpdate(v);
    sleepMs(250);
    cscdViewUpdate(v);
    CHECK(t1->style->opacity < 0.9f);
    NEAR(t2->style->opacity, 1);
    /* a hover that only paints keeps the layout; one that resizes lays out again */
    CscdImage fr;
    cscdImageInit(&fr, 600, 600);
    cscdViewPaint(v, &fr, true); /* (where things are comes from painting) */
    moveTo(v, 590, 590);       /* on the page first (entering it changes the root's :hover) */
    cscdViewPaint(v, &fr, true);
    CscdLayoutBox* root = cscdViewRootBox(v);
    CscdRect hb = viewRect(v, byId(v, "hb")), hg = viewRect(v, byId(v, "hg"));
    moveTo(v, hb.x + 5, hb.y + 5);
    cscdViewUpdate(v);
    CscdElement* bEl = byId(v, "hb");
    CHECK(bEl->style->backgroundColor.g == 0xaa && bEl->layout->style == bEl->style);
    CHECK(cscdViewRootBox(v) == root);
    moveTo(v, hg.x + 5, hg.y + 5);
    cscdViewUpdate(v);
    NEAR(rectOf(v, "hg").w, 90);
    CHECK(bEl->style->backgroundColor.g == 0x33 && bEl->layout->style == bEl->style);
    cscdImageFini(&fr);
    cscdViewFree(v);
}

/* ---- the application's side ------------------------------------------------------------------------- */

static CscdStrVec warnings;
static void noteWarning(void* u, CscdSv m) {
    (void)u;
    cscdVecPush(&warnings, cscdStrDup(m));
}

static void writeFile(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

/* Application files: found next to the program, style sheets read again when they change. */
static void testAppFiles(void) {
    CscdStr dir = cscdExecutableDir();
    CHECK(dir.len != 0);
    CscdStr same = cscdAppFindFile(CSCD_SV("no/such/file.html"));
    CHECK(cscdStrEqC(&same, "no/such/file.html"));
    cscdStrFree(&same);
    CscdStr css = cscdStrf("%s/live-reload-test.css", cscdStrC(&dir));
    writeFile(cscdStrC(&css), "#x{width:10px;height:10px}");
    CscdView* v = cscdViewNew();
    cscdStrVecFree(&warnings);
    cscdViewOnWarning(v, noteWarning, NULL);
    cscdViewLoadHTML(v, CSCD_SV("<link rel=stylesheet href=live-reload-test.css><link rel=stylesheet href=missing.css><div id=x></div>"), cscdStrSv(&dir));
    cscdViewSetViewport(v, 200, 200, 1);
    cscdViewUpdate(v);
    NEAR(rectOf(v, "x").w, 10);
    CHECK(warnings.len == 1 && strstr(cscdStrC(&warnings.items[0]), "missing.css") != NULL);
    cscdViewSetFeature(v, CSCD_FEATURE_LIVE_RELOAD, true);
    sleepMs(50);
    writeFile(cscdStrC(&css), "#x{width:30px;height:10px}");
    sleepMs(400);
    cscdViewUpdate(v);
    NEAR(rectOf(v, "x").w, 30);
    cscdViewSetFeature(v, CSCD_FEATURE_LIVE_RELOAD, false);
    remove(cscdStrC(&css));
    cscdViewFree(v);
    cscdStrFree(&css);
    cscdStrFree(&dir);
    cscdStrVecFree(&warnings);
}

/* The program's own loader: files that are on no disk (built into the program) */
static CscdBytes embeddedPng;
static int loaderCalls;
static bool embedded(void* user, CscdSv path, CscdStr* out) {
    (void)user;
    ++loaderCalls;
    if (cscdSvEq(path, CSCD_SV("embedded/index.html")))
        cscdStrSetC(out, "<link rel=stylesheet href=app.css><div id=x></div><img id=i src=dot.png>");
    else if (cscdSvEq(path, CSCD_SV("embedded/app.css"))) cscdStrSetC(out, "@import 'more.css'; #x{height:10px}");
    else if (cscdSvEq(path, CSCD_SV("embedded/more.css"))) cscdStrSetC(out, "#x{width:42px}");
    else if (cscdSvEq(path, CSCD_SV("embedded/dot.png"))) cscdStrAppend(out, (CscdSv){(const char*)embeddedPng.items, embeddedPng.len});
    else return false;
    return true;
}

static void testResourceLoader(void) {
    CscdImage dot;
    cscdImageInit(&dot, 3, 2);
    cscdEncodePNG(&dot, &embeddedPng);
    cscdImageFini(&dot);
    cscdAppSetResourceLoader(embedded, NULL);
    CscdView* v = cscdViewNew();
    CHECK(cscdViewLoadFile(v, CSCD_SV("embedded/index.html")));
    cscdViewSetViewport(v, 200, 200, 1);
    cscdViewUpdate(v);
    NEAR(rectOf(v, "x").w, 42);
    NEAR(rectOf(v, "x").h, 10);
    NEAR(rectOf(v, "i").w, 3);
    cscdViewFree(v);
    CscdStr none = {0};
    CHECK(!cscdReadFile(CSCD_SV("embedded/nothing.css"), &none)); /* not the loader's, not on disk */
    cscdStrFree(&none);
    cscdAppSetResourceLoader(NULL, NULL);
    const int calls = loaderCalls;
    CscdStr s = {0};
    CHECK(!cscdReadFile(CSCD_SV("embedded/app.css"), &s) && loaderCalls == calls);
    cscdStrFree(&s);
    cscdVecFree(&embeddedPng);
}

/* The library's parallel loops: each index once, whatever the threads; a loop inside a loop runs
 * on its thread alone (no deadlock); a call that is a once's waits for it. */
typedef struct LoopCount { volatile int hits[1000]; volatile int nested; } LoopCount;
static void countInner(void* ctx, int i) { (void)i; cscdAtomicAdd(&((LoopCount*)ctx)->nested, 1); }
static void countIndex(void* ctx, int i) {
    LoopCount* c = (LoopCount*)ctx;
    cscdAtomicAdd(&c->hits[i], 1);
    if (i % 100 == 0) cscdParallelFor(10, 4, countInner, c);
}
static CscdOnce slowOnce = CSCD_ONCE_INIT;
static volatile int onceRuns;
static void slowOnceFn(void* arg) {
    (void)arg;
    cscdAtomicAdd(&onceRuns, 1);
    double end = cscdNowSeconds() + 0.02;
    while (cscdNowSeconds() < end) {}
}
static void callSlowOnce(void* ctx, int i) {
    (void)i;
    cscdCallOnceArg(&slowOnce, slowOnceFn, NULL);
    cscdAtomicAdd((volatile int*)ctx, 1);
}

static void testParallelLoops(void) {
    static LoopCount c;
    for (int threads = 1; threads <= 8; threads *= 2) {
        memset((void*)&c, 0, sizeof c);
        cscdParallelFor(1000, threads, countIndex, &c);
        bool once = true;
        for (int i = 0; i < 1000; ++i) once = once && c.hits[i] == 1;
        CHECK(once);
        CHECK(c.nested == 100);
    }
    volatile int after = 0;
    cscdParallelFor(8, 8, callSlowOnce, (void*)&after);
    CHECK(onceRuns == 1 && after == 8);
    cscdParallelFor(0, 4, countIndex, &c); /* (nothing to do) */
    CHECK(cscdHardwareThreads() >= 1);
}

static char typed[32];
static bool ticked;
static void onTyped(void* u, CscdSv value) {
    (void)u;
    snprintf(typed, sizeof typed, "%.*s", (int)value.len, value.ptr);
}
static void onTicked(void* u, bool on) { (void)u; ticked = on; }

/* The conveniences: cscdViewGet never NULL, handlers of values. */
static void testApi(void) {
    CscdView* v = cscdViewNew();
    cscdStrVecFree(&warnings);
    cscdViewOnWarning(v, noteWarning, NULL);
    layoutView(v, "<input id=name value=a><input id=ok type=checkbox><ul id=list></ul><p id=status>-</p>");
    CscdElement* status = cscdViewGet(v, CSCD_SV("#status"));
    CHECK(status == byId(v, "status") && cscdViewGet(v, CSCD_SV("status")) == status);
    cscdElementSetText(status, CSCD_SV("Saved"));
    cscdElementAddClass(status, CSCD_SV("ok"));
    cscdElementSetStyle(status, CSCD_SV("color"), CSCD_SV("red"));
    CHECK(eq(textOf(status), "Saved") && cscdElementHasClass(status, CSCD_SV("ok")) && eq(keep(cscdElementGetStyle(status, CSCD_SV("color"))), "red"));
    CscdElement* nothing = cscdViewGet(v, CSCD_SV("#statsu"));
    cscdElementSetText(nothing, CSCD_SV("x")); /* goes nowhere, does not crash */
    cscdElementAddClass(nothing, CSCD_SV("y"));
    CHECK(warnings.len == 1 && strstr(cscdStrC(&warnings.items[0]), "#statsu") != NULL);
    int clicks = 0;
    cscdElementOnClick(status, countEvent, &clicks);
    cscdElementClick(status);
    CHECK(clicks == 1);
    typed[0] = 0;
    ticked = false;
    cscdElementOnInput(cscdViewGet(v, CSCD_SV("#name")), onTyped, NULL, NULL);
    cscdElementOnChecked(cscdViewGet(v, CSCD_SV("#ok")), onTicked, NULL, NULL);
    CscdRect r = viewRect(v, byId(v, "name"));
    CscdImage fr;
    cscdImageInit(&fr, 800, 600);
    cscdViewPaint(v, &fr, false);
    cscdImageFini(&fr);
    down(v, r.x + r.w - 2, r.y + 4);
    up(v, r.x + r.w - 2, r.y + 4);
    text(v, "bc");
    CHECK(eq(typed, "abc"));
    cscdElementClick(cscdViewGet(v, CSCD_SV("#ok")));
    CHECK(ticked);
    CscdElement* li = cscdElementAppendHTML(cscdViewGet(v, CSCD_SV("#list")), CSCD_SV("<li class=row>one</li><li>two</li>"));
    CHECK(li && cscdElementHasClass(li, CSCD_SV("row")) && countOf(v, "#list li") == 2);
    cscdElementRemove(li);
    CHECK(countOf(v, "#list li") == 1);
    cscdViewFree(v);
    cscdStrVecFree(&warnings);
}

/* 3D transforms: perspective foreshortens (and clicks follow it), back faces can hide. */
static void test3DTransforms(void) {
    CscdView* v = cscdViewNew();
    layoutViewWH(v, "<style>body{margin:0;background:#fff} .stage{perspective:400px;width:300px;height:200px;margin:50px}"
                    " .card{width:200px;height:100px;margin:50px;background:#f00} #turned{transform:rotateY(75deg)}"
                    " #back{transform:rotateY(180deg);backface-visibility:hidden} #flat{transform:rotateX(60deg);background:#00f}</style>"
                    "<div class=stage><div class=card id=turned></div></div><div class=stage><div class=card id=back></div></div>"
                    "<div class=card id=flat></div>",
                 800, 900);
    CscdElement *turned = byId(v, "turned"), *back = byId(v, "back"), *flat = byId(v, "flat");
    CHECK(turned->style->transform.len == 1 && cscdTransformIs3D(&turned->style->transform.items[0]));
    NEAR(cscdNodeParentElement(&turned->node)->style->perspective, 400);
    CscdImage img;
    cscdImageInit(&img, 800, 900);
    cscdViewPaint(v, &img, false);
    CscdRect t = viewRect(v, turned), b = viewRect(v, back), f = viewRect(v, flat);
#define PIX(x, y) (cscdImageRow(&img, (int)(y))[(int)(x)] & 0xFFFFFF)
    /* turned 75 degrees: a narrow band around its center line */
    CHECK(cscdViewElementAt(v, t.x + t.w / 2, t.y + t.h / 2) == turned);
    CHECK(cscdViewElementAt(v, t.x + 8, t.y + t.h / 2) != turned);
    CHECK(PIX(t.x + t.w / 2, t.y + t.h / 2) == 0xFF0000);
    CHECK(PIX(t.x + 8, t.y + t.h / 2) == 0xFFFFFF);
    /* its back turned and hidden: nothing drawn, nothing to click */
    CHECK(cscdViewElementAt(v, b.x + b.w / 2, b.y + b.h / 2) != back);
    CHECK(PIX(b.x + b.w / 2, b.y + b.h / 2) == 0xFFFFFF);
#undef PIX
    /* rotateX without a perspective flattens: half the height, around the middle */
    CHECK(cscdViewElementAt(v, f.x + f.w / 2, f.y + f.h / 2) == flat);
    CHECK(cscdViewElementAt(v, f.x + f.w / 2, f.y + 10) != flat);
    cscdImageFini(&img);
    cscdViewFree(v);
}

/* @container: rules and cq units follow the container's size, which follows the layout. */
static void testContainerQueries(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} .c{container-type:inline-size} .w{width:500px} .n{width:200px}"
                  " .item{width:10px;height:10px} @container (min-width: 300px) { .item{width:50cqw} }"
                  " .badge{display:block;height:10px} .p{container:panel / inline-size;width:320px}"
                  " @container panel (width > 250px) { .badge{height:30px} } .shrink{display:inline-block;container-type:inline-size}</style>"
                  "<div class='c w'><div class=item id=wide></div></div><div class='c n'><div class=item id=narrow></div></div>"
                  "<div class=p><span class=badge id=badge></span></div><div class=shrink id=shrink><span>long text inside</span></div>");
    NEAR(rectOf(v, "wide").w, 250);   /* 50cqw of 500px */
    NEAR(rectOf(v, "narrow").w, 10);  /* the condition does not hold */
    NEAR(rectOf(v, "badge").h, 30);   /* the named container */
    NEAR(rectOf(v, "shrink").w, 0);   /* size containment: sized as if empty */
    /* resizing the container: styles follow it */
    cscdElementSetStyle(cscdNodeParentElement(&byId(v, "narrow")->node), CSCD_SV("width"), CSCD_SV("400px"));
    cscdViewUpdate(v);
    NEAR(rectOf(v, "narrow").w, 200);
    cscdViewFree(v);
}

/* Resizing the view: styles are computed again only when a media query or viewport units depend on
 * the size; otherwise the page is only laid out again. */
static void testResize(void) {
    CscdView* v = cscdViewNew();
    layoutViewWH(v, "<style>body{margin:0} #a{height:10px} #p{width:50%;height:10px} @media (max-width: 500px) { #a{height:40px} }</style>"
                    "<div id=a></div><div id=p></div>", 800, 600);
    NEAR(rectOf(v, "a").h, 10);
    NEAR(rectOf(v, "p").w, 400);
    cscdViewSetViewport(v, 600, 600, 1); /* (no query changes: laid out again) */
    cscdViewUpdate(v);
    NEAR(rectOf(v, "a").h, 10);
    NEAR(rectOf(v, "p").w, 300);
    cscdViewSetViewport(v, 400, 600, 1); /* the query holds now */
    cscdViewUpdate(v);
    NEAR(rectOf(v, "a").h, 40);
    NEAR(rectOf(v, "p").w, 200);
    cscdViewSetViewport(v, 450, 300, 1);
    cscdViewUpdate(v);
    NEAR(rectOf(v, "a").h, 40);
    cscdViewSetViewport(v, 900, 300, 1); /* and no longer */
    cscdViewUpdate(v);
    NEAR(rectOf(v, "a").h, 10);
    /* viewport units follow every size */
    layoutViewWH(v, "<style>body{margin:0} #u{width:10vw;height:5vh} #s{width:7px;height:7px}</style><div id=u></div><div id=s></div>", 800, 600);
    NEAR(rectOf(v, "u").w, 80);
    cscdViewSetViewport(v, 500, 200, 1);
    cscdViewUpdate(v);
    NEAR(rectOf(v, "u").w, 50);
    NEAR(rectOf(v, "u").h, 10);
    NEAR(rectOf(v, "s").y, 10);
    cscdViewSetViewport(v, 500, 200, 2); /* another resolution */
    cscdViewUpdate(v);
    NEAR(rectOf(v, "u").w, 50);
    /* a sheet with a media attribute comes and goes with it */
    layoutViewWH(v, "<style>body{margin:0} #m{height:10px}</style><style media='(max-width: 500px)'>#m{height:25px}</style><div id=m></div>", 800, 600);
    NEAR(rectOf(v, "m").h, 10);
    cscdViewSetViewport(v, 400, 600, 1);
    cscdViewUpdate(v);
    NEAR(rectOf(v, "m").h, 25);
    cscdViewSetViewport(v, 700, 600, 1);
    cscdViewUpdate(v);
    NEAR(rectOf(v, "m").h, 10);
    cscdViewFree(v);
}

static void appendWarning(void* user, CscdSv message) {
    CscdStr* all = (CscdStr*)user;
    cscdStrAppend(all, message);
    cscdStrAppendChar(all, '\n');
}

/* Unknown properties are reported (with what they probably meant), tabular digits, and what frames cost. */
static void testDiagnostics(void) {
    CscdView* v = cscdViewNew();
    CscdStr warnings = {0};
    cscdViewOnWarning(v, appendWarning, &warnings);
    layoutView(v, "<style>body{margin:0} p{colr:red;widht:3px;-webkit-whatever:1;--mine:2;color:blue} q{colr:red}"
                  " .n{display:inline-block;font-size:20px} .t{font-variant-numeric:tabular-nums}</style>"
                  "<p>x</p><span class=n id=a>1111</span><br><span class=n id=b>0000</span><br>"
                  "<div class=t><span class=n id=c>1111</span><br><span class=n id=d>0000</span></div>");
    CHECK(cscdSvFind(cscdStrSv(&warnings), CSCD_SV("colr (in a <style>): ignored; did you mean color?"), 0) != CSCD_NPOS);
    CHECK(cscdSvFind(cscdStrSv(&warnings), CSCD_SV("did you mean width?"), 0) != CSCD_NPOS);
    CHECK(cscdSvFind(cscdStrSv(&warnings), CSCD_SV("whatever"), 0) == CSCD_NPOS); /* (vendor prefixes, custom properties: quiet) */
    CHECK(cscdSvFind(cscdStrSv(&warnings), CSCD_SV("mine"), 0) == CSCD_NPOS);
    CHECK(cscdSvFind(cscdStrSv(&warnings), CSCD_SV("colr"), cscdSvFind(cscdStrSv(&warnings), CSCD_SV("colr"), 0) + 1) == CSCD_NPOS); /* once */
    /* tabular digits are all as wide as the widest (and inherited) */
    NEAR(rectOf(v, "c").w, rectOf(v, "d").w);
    CHECK(rectOf(v, "c").w >= rectOf(v, "a").w - 0.01f);
    /* frame statistics */
    CscdFrameStats st = cscdViewFrameStats(v);
    CHECK(st.styles >= 1 && st.layouts >= 1);
    CHECK(st.stylesComputed > 0);
    const uint64_t styles = st.styles;
    cscdViewSetViewport(v, 700, 600, 1); /* a resize nothing in the styles depends on: no style pass */
    cscdViewUpdate(v);
    st = cscdViewFrameStats(v);
    CHECK(st.styles == styles && st.layouts >= 2);
    CscdImage img;
    cscdImageInit(&img, 700, 600);
    cscdViewPaint(v, &img, false);
    st = cscdViewFrameStats(v);
    CHECK(st.paints == 1 && !st.partialPaint);
    NEAR(st.paintedArea, 700.0 * 600.0);
    cscdImageFini(&img);
    cscdStrFree(&warnings);
    cscdViewFree(v);
}

/* subgrid: a nested grid that uses its parent's tracks, and sizes them with its own items. */
static void testSubgrid(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{margin:0} .cards{display:grid;grid-template-columns:100px 100px;row-gap:0}"
                  " .card{display:grid;grid-row:span 2;grid-template-rows:subgrid;row-gap:0}"
                  " .t{height:20px} .tall{height:60px} .b{height:10px}"
                  " .form{display:grid;grid-template-columns:auto 1fr;width:300px} .row{display:grid;grid-column:span 2;grid-template-columns:subgrid}"
                  " .l1{width:40px;height:10px} .l2{width:90px;height:10px}</style>"
                  "<div class=cards><div class=card><div class=t></div><div class=b id=b1></div></div>"
                  "<div class=card><div class=tall></div><div class=b id=b2></div></div></div>"
                  "<div class=form><div class=row><div class=l1></div><div id=f1>x</div></div><div class=row><div class=l2></div><div id=f2>y</div></div></div>");
    /* the second rows line up: both bodies start below the taller title */
    NEAR(rectOf(v, "b1").y, 60);
    NEAR(rectOf(v, "b2").y, 60);
    /* the fields of both rows start after the widest label */
    NEAR(rectOf(v, "f1").x, 90);
    NEAR(rectOf(v, "f2").x, 90);
    cscdViewFree(v);
}

/* GIF (built here: a palette, a transparent index, LZW codes with clears) and lossless WebP. */
static void testMoreImageFormats(void) {
    /* 3 x 2 pixels, indices into a 4-color palette; index 3 is transparent */
    const uint8_t idx[6] = {0, 1, 2, 3, 1, 0};
    static const uint8_t head[] = {'G', 'I', 'F', '8', '9', 'a', 3, 0, 2, 0, 0x81, 0, 0,
                                   255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9,
                                   0x21, 0xF9, 4, 1, 0, 0, 3, 0,
                                   0x2C, 0, 0, 0, 0, 3, 0, 2, 0, 0, 2};
    CscdBytes gif = {0};
    cscdVecAppend(&gif, head, sizeof head);
    {
        /* codes of 3 bits (min code size 2): a clear before every two indices keeps them at 3 bits */
        CscdBytes bytes = {0};
        uint32_t acc = 0;
        int n = 0;
#define PUT(code) do { acc |= (uint32_t)(code) << n; n += 3; while (n >= 8) { cscdVecPush(&bytes, (uint8_t)acc); acc >>= 8; n -= 8; } } while (0)
        for (int i = 0; i < 6; ++i) {
            if (i % 2 == 0) PUT(4);
            PUT(idx[i]);
        }
        PUT(5);
#undef PUT
        if (n) cscdVecPush(&bytes, (uint8_t)acc);
        cscdVecPush(&gif, (uint8_t)bytes.len);
        cscdVecAppend(&gif, bytes.items, bytes.len);
        cscdVecPush(&gif, 0);
        cscdVecPush(&gif, 0x3B);
        cscdVecFree(&bytes);
    }
    CscdImage* g = cscdDecodeImage(gif.items, gif.len);
    CHECK(g && g->width == 3 && g->height == 2);
    if (g) {
        CHECK(cscdImageRow(g, 0)[0] == 0xFFFF0000u && cscdImageRow(g, 0)[1] == 0xFF00FF00u && cscdImageRow(g, 0)[2] == 0xFF0000FFu);
        CHECK(cscdImageRow(g, 1)[0] == 0 && cscdImageRow(g, 1)[1] == 0xFF00FF00u && cscdImageRow(g, 1)[2] == 0xFFFF0000u);
    }
    cscdRelease(g);
    cscdVecFree(&gif);
    /* 6 x 4, lossless (made with libwebp from a YUV picture: orange top-left, blue, blended edges) */
    static const uint8_t webp[] = {0x52, 0x49, 0x46, 0x46, 0x5a, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x4c, 0x4d, 0x00, 0x00, 0x00,
        0x2f, 0x05, 0xc0, 0x00, 0x00, 0x3f, 0xa0, 0x16, 0x00, 0x02, 0xe6, 0x0b, 0x20, 0xea, 0xd8, 0x62, 0x5a, 0x1a, 0x64, 0x02, 0x16, 0xff, 0x3e, 0xb7,
        0xd6, 0x6a, 0x88, 0x24, 0x0c, 0x32, 0x11, 0x8c, 0xd3, 0x4c, 0x78, 0x01, 0x34, 0x50, 0xe1, 0x27, 0xf3, 0xfc, 0x87, 0xae, 0x98, 0xb9, 0x17, 0x17,
        0x13, 0x03, 0xd5, 0x1c, 0x18, 0x14, 0x02, 0x40, 0xc3, 0xa4, 0x10, 0x42, 0x06, 0x41, 0xec, 0xa0, 0x0c, 0x61, 0xfe, 0x18, 0x45, 0xf4, 0x3f, 0xe0,
        0xa5, 0xe6, 0xaf, 0x2b, 0x01, 0x00};
    CscdImage* w = cscdDecodeImage(webp, sizeof webp);
    CHECK(w && w->width == 6 && w->height == 4);
    if (w) {
        /* (what libwebp decodes it to) */
        CHECK(cscdImageRow(w, 0)[0] == 0xFFFF8801u && cscdImageRow(w, 0)[3] == 0xFFA05407u && cscdImageRow(w, 1)[0] == 0xFFDA8E41u);
        CHECK(cscdImageRow(w, 2)[3] == 0xFF4D619Au && cscdImageRow(w, 2)[4] == 0xFF3C64BBu && cscdImageRow(w, 3)[5] == 0xFF3266CBu);
    }
    cscdRelease(w);
}

/* Lossy WebP with alpha, animated WebP and animated GIF (made with libwebp and ffmpeg; the pixels
 * expected are what libwebp and browsers decode them to), and an animation playing in a page. */
static void testAnimatedImages(void) {
    static const uint8_t lossyAlpha[] = {
        0x52, 0x49, 0x46, 0x46, 0x36, 0x01, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x58, 0x0a, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
        0x13, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x41, 0x4c, 0x50, 0x48, 0x14, 0x00, 0x00, 0x00, 0x05, 0x0f, 0x30, 0x0d, 0x11, 0x11, 0x42, 0x48, 0x40, 0x4c,
        0xf0, 0xff, 0x2c, 0xdd, 0x82, 0x88, 0xfe, 0x87, 0x31, 0x00, 0x56, 0x50, 0x38, 0x20, 0xfc, 0x00, 0x00, 0x00, 0x50, 0x06, 0x00, 0x9d, 0x01, 0x2a,
        0x14, 0x00, 0x0c, 0x00, 0x3e, 0xd1, 0x54, 0xa3, 0x4b, 0xa8, 0x24, 0xa3, 0x21, 0xb0, 0x08, 0x01, 0x00, 0x1a, 0x09, 0x6c, 0x00, 0x9d, 0x32, 0x84,
        0x70, 0x15, 0xc6, 0x79, 0x23, 0xbf, 0xaa, 0xbe, 0x94, 0x1e, 0xc8, 0x64, 0x44, 0x9b, 0xb2, 0x47, 0x03, 0xb8, 0x00, 0x09, 0x6c, 0x5c, 0x9e, 0x74,
        0xda, 0xa2, 0x6a, 0xe2, 0xb0, 0x00, 0xfe, 0xcd, 0xd8, 0x60, 0xf0, 0xea, 0x71, 0xbc, 0x12, 0xb4, 0x73, 0x9c, 0xa0, 0xd4, 0xf9, 0xf5, 0xb6, 0x2c,
        0x84, 0xb2, 0x8f, 0x1a, 0x0d, 0x94, 0x23, 0x24, 0xf2, 0x61, 0x0f, 0x07, 0xed, 0x8a, 0x4d, 0xaf, 0x80, 0xdf, 0x39, 0xdc, 0x27, 0x15, 0xdb, 0x73,
        0xf9, 0xd6, 0x80, 0xbc, 0xad, 0xcf, 0xcd, 0x44, 0x68, 0x7b, 0x89, 0x20, 0x4f, 0x37, 0xb6, 0xfc, 0xfb, 0x9f, 0xa3, 0x0f, 0x84, 0x47, 0x20, 0x08,
        0x95, 0xf6, 0xf7, 0xfe, 0x9a, 0xc0, 0xfb, 0x54, 0x7f, 0xbb, 0xd9, 0xe2, 0xbe, 0x2f, 0x06, 0x29, 0x9f, 0xc9, 0xbe, 0x25, 0x18, 0x10, 0xba, 0xf5,
        0x1d, 0x84, 0x58, 0xed, 0x2a, 0x6d, 0x89, 0xbc, 0xd1, 0xdd, 0xa1, 0xe0, 0x91, 0xe3, 0x89, 0x8e, 0xae, 0x3f, 0xc4, 0x2b, 0x0b, 0x90, 0x2c, 0xd1,
        0xba, 0xa0, 0x71, 0x80, 0x02, 0xe0, 0x3d, 0xf7, 0x8c, 0x9f, 0xfd, 0x7d, 0x1d, 0x66, 0xbc, 0xf9, 0xf6, 0x44, 0x7c, 0xbc, 0xcf, 0x3e, 0x11, 0x4e,
        0x3d, 0x49, 0xcf, 0x2e, 0xa4, 0x97, 0x69, 0x66, 0x28, 0x41, 0xed, 0x65, 0x5a, 0xcf, 0xef, 0xce, 0x98, 0xca, 0x6e, 0xbf, 0x6f, 0x3f, 0xe1, 0x3f,
        0xfe, 0xd6, 0x19, 0x7f, 0x60, 0xf5, 0x1c, 0xbe, 0xb5, 0xda, 0xbb, 0x1b, 0x3f, 0xfb, 0xfc, 0x47, 0xf3, 0x7f, 0xec, 0xe4, 0xce, 0x4c, 0xe8, 0xe3,
        0x80, 0xf0, 0x02, 0xe0, 0x00, 0x00,
    };
    static const uint8_t animWebp[] = {
        0x52, 0x49, 0x46, 0x46, 0x90, 0x01, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x58, 0x0a, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x41, 0x4e, 0x49, 0x4d, 0x06, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x41, 0x4e, 0x4d, 0x46,
        0x92, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x02, 0x56, 0x50, 0x38, 0x20,
        0x7a, 0x00, 0x00, 0x00, 0x90, 0x02, 0x00, 0x9d, 0x01, 0x2a, 0x08, 0x00, 0x08, 0x00, 0x02, 0xc0, 0x4c, 0x25, 0xb0, 0x02, 0x74, 0x32, 0x90, 0x3f,
        0x59, 0xba, 0xd0, 0x01, 0xbd, 0xf8, 0xbf, 0x76, 0x00, 0x00, 0xfe, 0xe6, 0xe3, 0x9e, 0x60, 0xde, 0x76, 0x84, 0x4b, 0x5d, 0xee, 0xc9, 0x9d, 0xd9,
        0xff, 0xe1, 0x9e, 0x11, 0xb6, 0xc0, 0xc9, 0xa9, 0x94, 0xa2, 0xa0, 0x52, 0xf8, 0x5d, 0xa4, 0xc4, 0x5a, 0x3e, 0xfb, 0xc0, 0x35, 0x8f, 0x0a, 0xbb,
        0x80, 0xb8, 0xa8, 0x32, 0x40, 0x13, 0x30, 0xdf, 0xfd, 0xe0, 0x39, 0xc3, 0x13, 0xee, 0x1b, 0x41, 0xfb, 0xff, 0xfa, 0x38, 0xe0, 0x13, 0xff, 0xfd,
        0xe2, 0xe3, 0xfc, 0x10, 0xfd, 0xa2, 0x60, 0xcf, 0x2f, 0xdd, 0xff, 0xeb, 0x86, 0x04, 0x19, 0xf3, 0xff, 0x8c, 0xfd, 0xfa, 0x02, 0xb3, 0x1d, 0xcf,
        0xf8, 0x95, 0xf7, 0xb8, 0x00, 0x00, 0x41, 0x4e, 0x4d, 0x46, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x07, 0x00, 0x00, 0x01,
        0x00, 0x00, 0xc8, 0x00, 0x00, 0x00, 0x56, 0x50, 0x38, 0x20, 0x48, 0x00, 0x00, 0x00, 0x74, 0x02, 0x00, 0x9d, 0x01, 0x2a, 0x08, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x4c, 0x25, 0xa8, 0x02, 0x74, 0x92, 0x00, 0xf2, 0x00, 0xc9, 0x00, 0x05, 0x0e, 0xee, 0x46, 0xa0, 0x00, 0xfe, 0xb6, 0xed, 0xbc, 0xc7,
        0x88, 0xfb, 0x0c, 0x5b, 0xdb, 0x18, 0x80, 0x0d, 0xff, 0xe1, 0x1d, 0x3e, 0x08, 0x7f, 0xed, 0x6c, 0x7f, 0xe1, 0x1d, 0x3e, 0x08, 0x7c, 0x75, 0xda,
        0x5c, 0x45, 0xff, 0xff, 0xc4, 0x89, 0x34, 0xb8, 0x8b, 0xff, 0x80, 0x60, 0x00, 0x00, 0x41, 0x4e, 0x4d, 0x46, 0x62, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x03, 0x00, 0x00, 0x07, 0x00, 0x00, 0x01, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x00, 0x56, 0x50, 0x38, 0x20, 0x4a, 0x00, 0x00, 0x00, 0xb4, 0x02,
        0x00, 0x9d, 0x01, 0x2a, 0x08, 0x00, 0x02, 0x00, 0x00, 0x00, 0x4c, 0x25, 0xa8, 0x02, 0x74, 0x92, 0x00, 0xf2, 0x00, 0xc9, 0x00, 0xfd, 0x00, 0x05,
        0x2a, 0x8e, 0x46, 0xa0, 0x00, 0xfe, 0xaf, 0xcd, 0x4c, 0xe8, 0x9f, 0x2d, 0xad, 0x3e, 0xb5, 0xb4, 0x08, 0xff, 0xdf, 0x26, 0x4f, 0x08, 0x3f, 0xc7,
        0x83, 0xff, 0x79, 0x32, 0x78, 0x41, 0xcc, 0x37, 0x20, 0xb8, 0x1c, 0x17, 0xff, 0xf9, 0xb9, 0x25, 0xf0, 0xf0, 0x38, 0x2f, 0xf2, 0xb0, 0x00, 0x00,
    };
    static const uint8_t animGif[] = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x08, 0x00, 0x08, 0x00, 0xf7, 0x1f, 0x31, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x48, 0x00, 0x00, 0x6c, 0x00,
        0x00, 0x90, 0x00, 0x00, 0xb4, 0x00, 0x00, 0xd8, 0x00, 0x00, 0xfc, 0x00, 0x00, 0x00, 0x24, 0x00, 0x24, 0x24, 0x00, 0x48, 0x24, 0x00, 0x6c, 0x24,
        0x00, 0x90, 0x24, 0x00, 0xb4, 0x24, 0x00, 0xd8, 0x24, 0x00, 0xfc, 0x24, 0x00, 0x00, 0x48, 0x00, 0x24, 0x48, 0x00, 0x48, 0x48, 0x00, 0x6c, 0x48,
        0x00, 0x90, 0x48, 0x00, 0xb4, 0x48, 0x00, 0xd8, 0x48, 0x00, 0xfc, 0x48, 0x00, 0x00, 0x6c, 0x00, 0x24, 0x6c, 0x00, 0x48, 0x6c, 0x00, 0x6c, 0x6c,
        0x00, 0x90, 0x6c, 0x00, 0xb4, 0x6c, 0x00, 0xd8, 0x6c, 0x00, 0xfc, 0x6c, 0x00, 0x00, 0x90, 0x00, 0x24, 0x90, 0x00, 0x48, 0x90, 0x00, 0x6c, 0x90,
        0x00, 0x90, 0x90, 0x00, 0xb4, 0x90, 0x00, 0xd8, 0x90, 0x00, 0xfc, 0x90, 0x00, 0x00, 0xb4, 0x00, 0x24, 0xb4, 0x00, 0x48, 0xb4, 0x00, 0x6c, 0xb4,
        0x00, 0x90, 0xb4, 0x00, 0xb4, 0xb4, 0x00, 0xd8, 0xb4, 0x00, 0xfc, 0xb4, 0x00, 0x00, 0xd8, 0x00, 0x24, 0xd8, 0x00, 0x48, 0xd8, 0x00, 0x6c, 0xd8,
        0x00, 0x90, 0xd8, 0x00, 0xb4, 0xd8, 0x00, 0xd8, 0xd8, 0x00, 0xfc, 0xd8, 0x00, 0x00, 0xfc, 0x00, 0x24, 0xfc, 0x00, 0x48, 0xfc, 0x00, 0x6c, 0xfc,
        0x00, 0x90, 0xfc, 0x00, 0xb4, 0xfc, 0x00, 0xd8, 0xfc, 0x00, 0xfc, 0xfc, 0x00, 0x00, 0x00, 0x55, 0x24, 0x00, 0x55, 0x48, 0x00, 0x55, 0x6c, 0x00,
        0x55, 0x90, 0x00, 0x55, 0xb4, 0x00, 0x55, 0xd8, 0x00, 0x55, 0xfc, 0x00, 0x55, 0x00, 0x24, 0x55, 0x24, 0x24, 0x55, 0x48, 0x24, 0x55, 0x6c, 0x24,
        0x55, 0x90, 0x24, 0x55, 0xb4, 0x24, 0x55, 0xd8, 0x24, 0x55, 0xfc, 0x24, 0x55, 0x00, 0x48, 0x55, 0x24, 0x48, 0x55, 0x48, 0x48, 0x55, 0x6c, 0x48,
        0x55, 0x90, 0x48, 0x55, 0xb4, 0x48, 0x55, 0xd8, 0x48, 0x55, 0xfc, 0x48, 0x55, 0x00, 0x6c, 0x55, 0x24, 0x6c, 0x55, 0x48, 0x6c, 0x55, 0x6c, 0x6c,
        0x55, 0x90, 0x6c, 0x55, 0xb4, 0x6c, 0x55, 0xd8, 0x6c, 0x55, 0xfc, 0x6c, 0x55, 0x00, 0x90, 0x55, 0x24, 0x90, 0x55, 0x48, 0x90, 0x55, 0x6c, 0x90,
        0x55, 0x90, 0x90, 0x55, 0xb4, 0x90, 0x55, 0xd8, 0x90, 0x55, 0xfc, 0x90, 0x55, 0x00, 0xb4, 0x55, 0x24, 0xb4, 0x55, 0x48, 0xb4, 0x55, 0x6c, 0xb4,
        0x55, 0x90, 0xb4, 0x55, 0xb4, 0xb4, 0x55, 0xd8, 0xb4, 0x55, 0xfc, 0xb4, 0x55, 0x00, 0xd8, 0x55, 0x24, 0xd8, 0x55, 0x48, 0xd8, 0x55, 0x6c, 0xd8,
        0x55, 0x90, 0xd8, 0x55, 0xb4, 0xd8, 0x55, 0xd8, 0xd8, 0x55, 0xfc, 0xd8, 0x55, 0x00, 0xfc, 0x55, 0x24, 0xfc, 0x55, 0x48, 0xfc, 0x55, 0x6c, 0xfc,
        0x55, 0x90, 0xfc, 0x55, 0xb4, 0xfc, 0x55, 0xd8, 0xfc, 0x55, 0xfc, 0xfc, 0x55, 0x00, 0x00, 0xaa, 0x24, 0x00, 0xaa, 0x48, 0x00, 0xaa, 0x6c, 0x00,
        0xaa, 0x90, 0x00, 0xaa, 0xb4, 0x00, 0xaa, 0xd8, 0x00, 0xaa, 0xfc, 0x00, 0xaa, 0x00, 0x24, 0xaa, 0x24, 0x24, 0xaa, 0x48, 0x24, 0xaa, 0x6c, 0x24,
        0xaa, 0x90, 0x24, 0xaa, 0xb4, 0x24, 0xaa, 0xd8, 0x24, 0xaa, 0xfc, 0x24, 0xaa, 0x00, 0x48, 0xaa, 0x24, 0x48, 0xaa, 0x48, 0x48, 0xaa, 0x6c, 0x48,
        0xaa, 0x90, 0x48, 0xaa, 0xb4, 0x48, 0xaa, 0xd8, 0x48, 0xaa, 0xfc, 0x48, 0xaa, 0x00, 0x6c, 0xaa, 0x24, 0x6c, 0xaa, 0x48, 0x6c, 0xaa, 0x6c, 0x6c,
        0xaa, 0x90, 0x6c, 0xaa, 0xb4, 0x6c, 0xaa, 0xd8, 0x6c, 0xaa, 0xfc, 0x6c, 0xaa, 0x00, 0x90, 0xaa, 0x24, 0x90, 0xaa, 0x48, 0x90, 0xaa, 0x6c, 0x90,
        0xaa, 0x90, 0x90, 0xaa, 0xb4, 0x90, 0xaa, 0xd8, 0x90, 0xaa, 0xfc, 0x90, 0xaa, 0x00, 0xb4, 0xaa, 0x24, 0xb4, 0xaa, 0x48, 0xb4, 0xaa, 0x6c, 0xb4,
        0xaa, 0x90, 0xb4, 0xaa, 0xb4, 0xb4, 0xaa, 0xd8, 0xb4, 0xaa, 0xfc, 0xb4, 0xaa, 0x00, 0xd8, 0xaa, 0x24, 0xd8, 0xaa, 0x48, 0xd8, 0xaa, 0x6c, 0xd8,
        0xaa, 0x90, 0xd8, 0xaa, 0xb4, 0xd8, 0xaa, 0xd8, 0xd8, 0xaa, 0xfc, 0xd8, 0xaa, 0x00, 0xfc, 0xaa, 0x24, 0xfc, 0xaa, 0x48, 0xfc, 0xaa, 0x6c, 0xfc,
        0xaa, 0x90, 0xfc, 0xaa, 0xb4, 0xfc, 0xaa, 0xd8, 0xfc, 0xaa, 0xfc, 0xfc, 0xaa, 0x00, 0x00, 0xff, 0x24, 0x00, 0xff, 0x48, 0x00, 0xff, 0x6c, 0x00,
        0xff, 0x90, 0x00, 0xff, 0xb4, 0x00, 0xff, 0xd8, 0x00, 0xff, 0xfc, 0x00, 0xff, 0x00, 0x24, 0xff, 0x24, 0x24, 0xff, 0x48, 0x24, 0xff, 0x6c, 0x24,
        0xff, 0x90, 0x24, 0xff, 0xb4, 0x24, 0xff, 0xd8, 0x24, 0xff, 0xfc, 0x24, 0xff, 0x00, 0x48, 0xff, 0x24, 0x48, 0xff, 0x48, 0x48, 0xff, 0x6c, 0x48,
        0xff, 0x90, 0x48, 0xff, 0xb4, 0x48, 0xff, 0xd8, 0x48, 0xff, 0xfc, 0x48, 0xff, 0x00, 0x6c, 0xff, 0x24, 0x6c, 0xff, 0x48, 0x6c, 0xff, 0x6c, 0x6c,
        0xff, 0x90, 0x6c, 0xff, 0xb4, 0x6c, 0xff, 0xd8, 0x6c, 0xff, 0xfc, 0x6c, 0xff, 0x00, 0x90, 0xff, 0x24, 0x90, 0xff, 0x48, 0x90, 0xff, 0x6c, 0x90,
        0xff, 0x90, 0x90, 0xff, 0xb4, 0x90, 0xff, 0xd8, 0x90, 0xff, 0xfc, 0x90, 0xff, 0x00, 0xb4, 0xff, 0x24, 0xb4, 0xff, 0x48, 0xb4, 0xff, 0x6c, 0xb4,
        0xff, 0x90, 0xb4, 0xff, 0xb4, 0xb4, 0xff, 0xd8, 0xb4, 0xff, 0xfc, 0xb4, 0xff, 0x00, 0xd8, 0xff, 0x24, 0xd8, 0xff, 0x48, 0xd8, 0xff, 0x6c, 0xd8,
        0xff, 0x90, 0xd8, 0xff, 0xb4, 0xd8, 0xff, 0xd8, 0xd8, 0xff, 0xfc, 0xd8, 0xff, 0x00, 0xfc, 0xff, 0x24, 0xfc, 0xff, 0x48, 0xfc, 0xff, 0x6c, 0xfc,
        0xff, 0x90, 0xfc, 0xff, 0xb4, 0xfc, 0xff, 0xd8, 0xfc, 0xff, 0xfc, 0xfc, 0xff, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53, 0x43, 0x41, 0x50, 0x45,
        0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x04, 0x14, 0x00, 0x1f, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x08,
        0x00, 0x00, 0x08, 0x35, 0x00, 0x01, 0x1c, 0xc0, 0xf1, 0x03, 0xd8, 0x31, 0x7c, 0xff, 0x04, 0x1e, 0x03, 0xf6, 0x03, 0xc7, 0x81, 0x84, 0xf8, 0x16,
        0x36, 0x3c, 0x00, 0x00, 0x40, 0x44, 0x86, 0x0e, 0x2b, 0x5e, 0x9c, 0xa8, 0x51, 0x62, 0xc6, 0x03, 0x37, 0x78, 0xe0, 0xc1, 0x07, 0x8d, 0xd8, 0xa1,
        0x03, 0x2f, 0x76, 0xe0, 0x20, 0x39, 0xec, 0x58, 0x40, 0x00, 0x21, 0xf9, 0x04, 0x05, 0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x06, 0x00, 0x08,
        0x00, 0x02, 0x00, 0x00, 0x08, 0x13, 0x00, 0x1f, 0xfc, 0xd0, 0x01, 0x00, 0x1e, 0x30, 0x63, 0x00, 0x04, 0xea, 0xc0, 0x65, 0xb0, 0xd8, 0xa1, 0x80,
        0x00, 0x00, 0x21, 0xf9, 0x04, 0x05, 0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x06, 0x00, 0x08, 0x00, 0x02, 0x00, 0x00, 0x08, 0x14, 0x00, 0x3f,
        0xf8, 0xc0, 0x81, 0x0b, 0x5d, 0xb0, 0x63, 0x47, 0x3e, 0x00, 0xc0, 0x01, 0x00, 0xdc, 0xc1, 0x23, 0x01, 0x01, 0x00, 0x3b,
    };
    CscdImage* a = cscdDecodeImage(lossyAlpha, sizeof lossyAlpha);
    CHECK(a && a->width == 20 && a->height == 12);
    if (a) {
        CHECK(cscdImageRow(a, 0)[0] == 0x00000000u && cscdImageRow(a, 3)[5] == 0x41041103u);
        CHECK(cscdImageRow(a, 11)[19] == 0xF7F77D00u && cscdImageRow(a, 6)[10] == 0x82120301u);
    }
    cscdRelease(a);
    CscdAnimatedImage* w = cscdDecodeAnimatedImage(animWebp, sizeof animWebp);
    CHECK(w && w->frameCount == 3 && w->loops == 2 && w->durations[1] == 200);
    if (w && w->frameCount == 3) {
        CHECK(cscdImageRow(w->frames[0], 4)[4] == 0xFFA8FF77u && cscdImageRow(w->frames[0], 7)[7] == 0xFFC516E5u);
        CHECK(cscdImageRow(w->frames[1], 7)[7] == 0xFFE203D0u && cscdImageRow(w->frames[2], 7)[7] == 0xFFE802A6u);
    }
    cscdRelease(w);
    CscdAnimatedImage* g = cscdDecodeAnimatedImage(animGif, sizeof animGif);
    CHECK(g && g->frameCount == 3 && g->loops == 0 && g->durations[0] == 200);
    if (g && g->frameCount == 3) {
        CHECK(cscdImageRow(g->frames[0], 7)[7] == 0xFFFC00FFu && cscdImageRow(g->frames[1], 7)[7] == 0xFFFC00AAu);
        CHECK(cscdImageRow(g->frames[2], 7)[7] == 0xFFFC0055u && cscdImageRow(g->frames[2], 4)[4] == 0xFFFCFC00u);
    }
    cscdRelease(g);

    /* in a page: the image's pixels follow the frames as time passes */
    CscdView* v = cscdViewNew();
    layoutView(v, "<img id=i style='display:block;width:8px;height:8px;image-rendering:pixelated' src='data:image/gif;base64,R0lGODlhCAAIAPcfMQAAACQAAEgAAGwAAJAAALQAANgAAPwAAAAkACQkAEgkAGwkAJAkALQkANgkAPwkAABIACRIAEhIAGxIAJBIALRIANhIAPxIAABsACRsAEhsAGxsAJBsALRsANhsAPxsAACQACSQAEiQAGyQAJCQALSQANiQAPyQAAC0ACS0AEi0AGy0AJC0ALS0ANi0APy0AADYACTYAEjYAGzYAJDYALTYANjYAPzYAAD8ACT8AEj8AGz8AJD8ALT8ANj8APz8AAAAVSQAVUgAVWwAVZAAVbQAVdgAVfwAVQAkVSQkVUgkVWwkVZAkVbQkVdgkVfwkVQBIVSRIVUhIVWxIVZBIVbRIVdhIVfxIVQBsVSRsVUhsVWxsVZBsVbRsVdhsVfxsVQCQVSSQVUiQVWyQVZCQVbSQVdiQVfyQVQC0VSS0VUi0VWy0VZC0VbS0Vdi0Vfy0VQDYVSTYVUjYVWzYVZDYVbTYVdjYVfzYVQD8VST8VUj8VWz8VZD8VbT8Vdj8Vfz8VQAAqiQAqkgAqmwAqpAAqrQAqtgAqvwAqgAkqiQkqkgkqmwkqpAkqrQkqtgkqvwkqgBIqiRIqkhIqmxIqpBIqrRIqthIqvxIqgBsqiRsqkhsqmxsqpBsqrRsqthsqvxsqgCQqiSQqkiQqmyQqpCQqrSQqtiQqvyQqgC0qiS0qki0qmy0qpC0qrS0qti0qvy0qgDYqiTYqkjYqmzYqpDYqrTYqtjYqvzYqgD8qiT8qkj8qmz8qpD8qrT8qtj8qvz8qgAA/yQA/0gA/2wA/5AA/7QA/9gA//wA/wAk/yQk/0gk/2wk/5Ak/7Qk/9gk//wk/wBI/yRI/0hI/2xI/5BI/7RI/9hI//xI/wBs/yRs/0hs/2xs/5Bs/7Rs/9hs//xs/wCQ/ySQ/0iQ/2yQ/5CQ/7SQ/9iQ//yQ/wC0/yS0/0i0/2y0/5C0/7S0/9i0//y0/wDY/yTY/0jY/2zY/5DY/7TY/9jY//zY/wD8/yT8/0j8/2z8/5D8/7T8/9j8//z8/yH/C05FVFNDQVBFMi4wAwEAAAAh+QQEFAAfACwAAAAACAAIAAAINQABHMDxA9gxfP8EHgP2A8eBhPgWNjwAAEBEhg4rXpyoUWLGAzd44MEHjdihAy924CA57FhAACH5BAUUAAAALAAABgAIAAIAAAgTAB/80AEAHjBjAATqwGWw2KGAAAAh+QQFFAAAACwAAAYACAACAAAIFAA/+MCBC12wY0c+AMABANzBIwEBADs='>");
    CscdImage img;
    cscdImageInit(&img, 800, 600);
    frame(v, &img);
    const CscdRect r = rectOf(v, "i");
    /* (a block, so on whole pixels whatever the platform's font) */
    const int px = (int)r.x + 7, py = (int)r.y + 7;
    const uint32_t before = cscdImageRow(&img, py)[px];
    CHECK(before == 0xFFFC00FFu);
    if (before != 0xFFFC00FFu) fprintf(stderr, "  the image at %g,%g %gx%g, its pixel %08X\n", r.x, r.y, r.w, r.h, before);
    CHECK(cscdViewNextTimerMs(v) >= 0 && cscdViewNextTimerMs(v) <= 200);
    sleepMs(230);
    CHECK(cscdViewUpdate(v));
    frame(v, &img);
    CHECK(cscdImageRow(&img, py)[px] == 0xFFFC00AAu);
    cscdImageFini(&img);
    cscdViewFree(v);
}

/* Right-to-left text in fields: the caret, clicks and the selection by the runs as they show. */
static void testBidiFields(void) {
    CscdView* v = cscdViewNew();
    /* (Hebrew alef bet gimel) */
    layoutView(v, "<input id=r dir=rtl style='direction: rtl; width: 300px; font-size: 20px' value='\xD7\x90\xD7\x91\xD7\x92'>"
                  "<input id=m style='width: 300px; font-size: 20px' value='ab \xD7\x90\xD7\x91 cd'>");
    CscdElement* r = byId(v, "r");
    cscdViewFocus(v, r);
    r->caret = r->selAnchor = 0;
    const CscdRect start = cscdViewCaretRect(v);
    r->caret = r->selAnchor = 6;
    const CscdRect end = cscdViewCaretRect(v);
    CHECK(start.x > end.x); /* (the first letter on the right) */
    const CscdRect box = rectOf(v, "r");
    CHECK(start.x > box.x + box.w / 2); /* (right aligned: direction's start) */
    /* mixed: "ab " then the Hebrew word, its letters right to left */
    CscdElement* m = byId(v, "m");
    cscdViewFocus(v, m);
    m->caret = m->selAnchor = 5; /* (after alef, before bet: alef shows at the word's right) */
    const float afterAlef = cscdViewCaretRect(v).x;
    m->caret = m->selAnchor = 7; /* (after bet: the word's left) */
    const float afterBet = cscdViewCaretRect(v).x;
    m->caret = m->selAnchor = 2;
    const float afterB = cscdViewCaretRect(v).x;
    m->caret = m->selAnchor = 9;
    const float afterC = cscdViewCaretRect(v).x;
    CHECK(afterB < afterBet && afterBet < afterAlef && afterAlef < afterC);
    cscdViewFree(v);
}

/* OpenType shaping, variations and color glyphs, on a font made for it (tools/makeTestFont.py;
 * the results expected are HarfBuzz's for the same font). */
#include "testFont.inc"

static const CscdShapedGlyph* shapedVisible(const CscdShapedGlyphVec* v, size_t i) {
    for (size_t k = 0; k < v->len; ++k)
        if (v->items[k].glyph && i-- == 0) return &v->items[k];
    return NULL;
}

static void testOpenType(void) {
    CscdFontFace* face = cscdFontFaceLoad(NULL, kTestFont, sizeof kTestFont, 0);
    CHECK(face != NULL);
    if (!face) return;
    CscdFont font = cscdFontDefault();
    font.face = face;
    font.size = 1000; /* (one px a font unit) */
    CscdShapedGlyphVec out = {0};
    /* a ligature, and none when ligatures are off */
    cscdFontShape(CSCD_SV("fi"), &font, 0, 0, &out);
    CHECK(out.len == 1 && out.items[0].glyph == 11 && out.items[0].advance == 520);
    font.noLigatures = true;
    cscdFontShape(CSCD_SV("fi"), &font, 0, 0, &out);
    CHECK(out.len == 2 && out.items[0].glyph == 9 && out.items[1].glyph == 10);
    font.noLigatures = false;
    /* a chained contextual substitution: A after B */
    cscdFontShape(CSCD_SV("BA A"), &font, 0, 0, &out);
    CHECK(out.len == 4 && out.items[1].glyph == 3 && out.items[3].glyph == 2);
    /* kerning; none with font-kerning: none */
    cscdFontShape(CSCD_SV("AV"), &font, 0, 0, &out);
    CHECK(out.len == 2 && out.items[0].advance == 500 && out.items[1].x == 500);
    CscdFontSettings settings;
    memset(&settings, 0, sizeof settings);
    settings.noKerning = true;
    font.settings = cscdFontSettingsIntern(&settings);
    cscdFontShape(CSCD_SV("AV"), &font, 0, 0, &out);
    CHECK(out.len == 2 && out.items[1].x == 600);
    /* a feature asked for (smcp), interned once */
    memset(&settings, 0, sizeof settings);
    settings.features[settings.featureCount++] = (CscdFontTagValue){cscdFontTag("smcp"), 1};
    font.settings = cscdFontSettingsIntern(&settings);
    CHECK(font.settings == cscdFontSettingsIntern(&settings));
    cscdFontShape(CSCD_SV("a"), &font, 0, 0, &out);
    CHECK(out.len == 1 && out.items[0].glyph == 8 && out.items[0].advance == 550);
    font.settings = NULL;
    /* a mark on its base, by their anchors */
    cscdFontShape(CSCD_SV("A\xCC\x81"), &font, 0, 0, &out);
    CHECK(out.len == 2 && out.items[1].glyph == 12 && out.items[1].x == 250 && out.items[1].y == -700 && out.items[1].advance == 0);
    CHECK(cscdFontMeasure(CSCD_SV("A\xCC\x81"), &font, 0, 0) == 600);
    /* the variation selector passed over (the sequence forms), then showing nothing */
    cscdFontShape(CSCD_SV("V\xEF\xB8\x8F"), &font, 0, 0, &out);
    CHECK(shapedVisible(&out, 0) && shapedVisible(&out, 0)->glyph == 6 && !shapedVisible(&out, 1));
    /* right to left: shaped in reading order, out as shown (kerned the same) */
    cscdFontShapeRtl(CSCD_SV("AV"), &font, 0, 0, &out);
    CHECK(out.len == 2 && out.items[0].glyph == 5 && out.items[0].x == 0 && out.items[1].glyph == 2 && out.items[1].x == 600);
    cscdFontShapeRtl(CSCD_SV("A\xCC\x81"), &font, 0, 0, &out);
    CHECK(out.len == 2 && out.items[0].glyph == 12 && out.items[0].x == 250 && out.items[1].x == 0);

    /* Devanagari: the i matra before its consonant, a half form, the reph after the base */
    cscdFontShape(CSCD_SV("\xE0\xA4\x95\xE0\xA4\xBF"), &font, 0, 0, &out);
    CHECK(out.len == 2 && out.items[0].glyph == 20 && out.items[1].glyph == 17);
    cscdFontShape(CSCD_SV("\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\x95"), &font, 0, 0, &out);
    CHECK(out.len == 2 && out.items[0].glyph == 17 && out.items[1].glyph == 22);
    cscdFontShape(CSCD_SV("\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\x95"), &font, 0, 0, &out);
    CHECK(out.len == 2 && out.items[0].glyph == 21 && out.items[1].glyph == 17);
    cscdFontShape(CSCD_SV("\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\x95\xE0\xA4\xBF"), &font, 0, 0, &out);
    CHECK(out.len == 3 && out.items[0].glyph == 20 && out.items[1].glyph == 17 && out.items[2].glyph == 22);

    /* a variable font: the axis, an instance (kept: the same values give the same face) */
    uint32_t tg;
    float lo, def, hi;
    CHECK(cscdFontFaceAxisCount(face) == 1 && cscdFontFaceAxis(face, 0, &tg, &lo, &def, &hi) && tg == cscdFontTag("wght") && lo == 100 && def == 400 && hi == 900);
    const CscdFontTagValue bold = {cscdFontTag("wght"), 900}, half = {cscdFontTag("wght"), 650};
    CscdFontFace* heavy = cscdFontFaceVary(face, &bold, 1);
    CHECK(heavy != face && heavy == cscdFontFaceVary(face, &bold, 1) && heavy->weight == 900);
    CHECK(cscdFontFaceVary(face, NULL, 0) == face);
    CHECK(cscdFontFaceAdvance(heavy, 2) == 800 && cscdFontFaceAdvance(face, 2) == 600);
    CHECK(cscdFontFaceAdvance(cscdFontFaceVary(face, &half, 1), 2) == 700);
    CscdPath p = {0};
    CHECK(cscdFontFaceOutline(heavy, 2, &p));
    const CscdRect bb = cscdPathBounds(&p);
    CHECK(bb.x == 0 && bb.w == 800 && bb.h == 700);
    cscdPathFree(&p);
    CscdFont wide = cscdFontWithSettings(font, 900, NULL);
    CHECK(wide.face == heavy);

    /* color: COLR layers in CPAL's colors */
    CHECK(cscdFontFaceHasColor(face));
    CscdColorLayer layers[4];
    CHECK(cscdFontFaceColorLayers(face, 14, layers, 4) == 2 && layers[0].glyph == 15 && layers[0].argb == 0xFFFF0000u && layers[1].argb == 0xFF0000FFu);
    CHECK(cscdFontFaceColorLayers(face, 2, layers, 4) == 0);
    /* painted: the layers' colors, not the text's */
    CscdImage img;
    cscdImageInit(&img, 40, 40);
    CscdCanvas* c = cscdCanvasNew(&img);
    CscdFont small = font;
    small.size = 20;
    CscdGlyph g = {14, 2, 30, &small};
    CscdShader black = cscdShaderSolid(cscdRgb(0, 0, 0));
    cscdCanvasDrawGlyphs(c, &g, 1, &black);
    CHECK(cscdImageRow(&img, 25)[10] == 0xFF0000FFu); /* (the blue layer, on top) */
    cscdCanvasFree(c);
    cscdImageFini(&img);
    cscdVecFree(&out);
    cscdFontFaceFree(face);
}

/* the text of e's ::before box */
static const char* generated(CscdView* v, const char* id) {
    CscdStr t = {0};
    CscdLayoutBox* b = byId(v, id)->layout;
    for (size_t i = 0; i < b->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(b, i);
        if (!cscdStrEqC(&c->pseudo, "before")) continue;
        for (size_t k = 0; k < c->children.len; ++k) cscdStrAppend(&t, cscdStrSv(&cscdBoxChild(c, k)->text));
    }
    return keep(t);
}

/* The CSS properties that were once accepted without effect: hyphens, quotes, zoom, shape-outside,
 * contain, content-visibility, unicode-bidi, empty-cells; and a clip whose first point is not its
 * top-left corner. */
static void testMoreCss(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<div id=h style='width:60px;font-size:16px'>aaaaa\xC2\xAD" "bbbbb\xC2\xAD" "ccccc</div>"
                  "<div id=hn style='width:60px;font-size:16px;hyphens:none'>aaaaa\xC2\xAD" "bbbbb</div>"
                  "<p><q id=q1>x <q id=q2>y</q></q> <span style='quotes:\"<\" \">\"'><q id=q3>z</q></span></p>"
                  "<div id=z style='zoom:2;width:50px;height:10px;border:1px solid'></div>"
                  "<div style='zoom:2'><div id=zf style='font-size:8px;width:10px'>a</div></div>"
                  "<div style='width:300px;font-size:16px'><div style='float:left;width:100px;height:100px;shape-outside:circle(50%)'></div>"
                  "<span id=wrap>aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa aaa</span></div>"
                  "<div id=cs style='contain:size;display:inline-block'>wide content here</div>"
                  "<div id=cv style='content-visibility:hidden;height:20px'><p>hidden</p></div>"
                  "<p id=bo style='unicode-bidi:bidi-override;direction:rtl'>ab</p>");
    /* a soft hyphen breaks the line and shows as a hyphen; not with hyphens: none */
    const CscdLayoutBox* ht = cscdBoxChild(byId(v, "h")->layout, 0);
    CHECK(ht->textFrags.len >= 2 && cscdStrSv(&ht->textFrags.items[0].text).ptr[ht->textFrags.items[0].text.len - 1] == '-');
    const CscdLayoutBox* hn = cscdBoxChild(byId(v, "hn")->layout, 0);
    CHECK(hn->textFrags.len == 1);
    /* quotes: nested, and the element's own */
    CHECK(eq(generated(v, "q1"), "\xE2\x80\x9C") && eq(generated(v, "q2"), "\xE2\x80\x98") && eq(generated(v, "q3"), "<"));
    /* zoom: lengths, borders and font sizes */
    NEAR(rectOf(v, "z").w, 104);
    CHECK(byId(v, "zf")->style->font.size == 16);
    /* shape-outside: the first line runs closer to the circle's top than the middle ones */
    const CscdLayoutBox* wt = cscdBoxChild(byId(v, "wrap")->layout, 0);
    CHECK(wt->textFrags.len >= 3 && wt->textFrags.items[0].x < wt->textFrags.items[2].x - 2);
    /* size containment: as if empty */
    NEAR(rectOf(v, "cs").w, 0);
    /* content-visibility: hidden: its box, none of its contents */
    CHECK(byId(v, "cv")->layout->children.len == 0);
    NEAR(rectOf(v, "cv").h, 20);
    /* bidi-override: Latin letters right to left */
    CHECK(cscdBoxChild(byId(v, "bo")->layout, 0)->textFrags.items[0].rtl);
    cscdViewFree(v);

    /* writing-mode: vertical lines as long as the box is tall, the box as wide as they are deep */
    v = cscdViewNew();
    layoutView(v, "<div id=vr style='writing-mode:vertical-rl;height:100px;font-size:16px;display:inline-block'>aaa bbb ccc ddd eee fff ggg hhh</div>"
                  "<div id=va style='writing-mode:vertical-rl;font-size:16px;display:inline-block'>ab</div>");
    const CscdRect vr = rectOf(v, "vr"), va = rectOf(v, "va");
    NEAR(vr.h, 100);
    CHECK(vr.w > 30 && vr.w < 100); /* (two or more columns) */
    CHECK(va.h > 5 && va.h < 40 && va.w > 10 && va.w < 30); /* (one short column) */
    cscdViewFree(v);

    /* a clip path whose first point is not at the top-left (its bounds were once cut to one row) */
    CscdPath p = {0};
    CHECK(cscdBasicShapePath(CSCD_SV("polygon(100% 0, 100% 100%, 0 100%)"), cscdRect(0, 0, 40, 40), &p));
    CscdImage img;
    cscdImageInit(&img, 40, 40);
    CscdCanvas* c = cscdCanvasNew(&img);
    cscdCanvasClipPath(c, &p, CSCD_FILL_NONZERO);
    cscdCanvasFillRect(c, cscdRect(0, 0, 40, 40), cscdRgb(255, 0, 0));
    CHECK(cscdImageRow(&img, 35)[35] != 0 && cscdImageRow(&img, 5)[5] == 0);
    cscdCanvasFree(c);
    cscdImageFini(&img);
    cscdPathFree(&p);
}

/* Data binding: the page follows the model (text, shown, classes, attributes, repeated items, only
 * what changed touched) and fields set the model as they are typed in. */
static int bindChanges;
static void onBindChange(void* user, CscdModel* m, CscdSv name) {
    (void)user;
    (void)m;
    if (cscdSvEqC(name, "title")) ++bindChanges;
}

static void testBinding(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<div id=app><h1 id=t data-text=title></h1><p id=s data-show=busy>working</p><p id=n data-hide=busy>idle</p>"
                  "<a id=l data-attr-href=link data-class-on=busy>x</a><input id=f data-value=title>"
                  "<ul id=list data-each=items><template><li data-text=name data-class-done=done></li></template></ul></div>");
    CscdModel* m = cscdModelNew();
    cscdModelSet(m, CSCD_SV("title"), CSCD_SV("Hello"));
    cscdModelSetBool(m, CSCD_SV("busy"), false);
    cscdModelSet(m, CSCD_SV("link"), CSCD_SV("page.html"));
    CscdModel* a = cscdModelListAdd(m, CSCD_SV("items"));
    cscdModelSet(a, CSCD_SV("name"), CSCD_SV("first"));
    CscdModel* b = cscdModelListAdd(m, CSCD_SV("items"));
    cscdModelSet(b, CSCD_SV("name"), CSCD_SV("second"));
    cscdModelSetBool(b, CSCD_SV("done"), true);
    cscdModelOnChange(m, onBindChange, NULL);
    cscdViewBind(v, byId(v, "app"), m);
    cscdViewUpdate(v);
    CHECK(eq(textOf(byId(v, "t")), "Hello") && eq(valueOf(byId(v, "f")), "Hello"));
    CHECK(cscdElementHasAttributeC(byId(v, "s"), "hidden") && !cscdElementHasAttributeC(byId(v, "n"), "hidden"));
    CHECK(cscdSvEqC(cscdElementAttrC(byId(v, "l"), "href"), "page.html") && !cscdElementHasClass(byId(v, "l"), CSCD_SV("on")));
    CHECK(countOf(v, "#list li") == 2 && eq(textOf(nth(v, "#list li", 1)), "second") && cscdElementHasClass(nth(v, "#list li", 1), CSCD_SV("done")));
    CscdElement* firstLi = nth(v, "#list li", 0);
    CHECK(cscdViewModelOf(v, firstLi) == a);
    /* changes: only those touched; an item added, one moved, one removed */
    cscdModelSetBool(m, CSCD_SV("busy"), true);
    CscdModel* c = cscdModelListInsert(m, CSCD_SV("items"), 0);
    cscdModelSet(c, CSCD_SV("name"), CSCD_SV("zero"));
    cscdModelListRemove(m, CSCD_SV("items"), 2); /* (second) */
    cscdViewUpdate(v);
    CHECK(!cscdElementHasAttributeC(byId(v, "s"), "hidden") && cscdElementHasClass(byId(v, "l"), CSCD_SV("on")));
    CHECK(countOf(v, "#list li") == 2 && eq(textOf(nth(v, "#list li", 0)), "zero") && eq(textOf(nth(v, "#list li", 1)), "first"));
    CHECK(nth(v, "#list li", 1) == firstLi); /* (the first item's copy kept) */
    cscdModelListMove(m, CSCD_SV("items"), 1, 0);
    cscdViewUpdate(v);
    CHECK(nth(v, "#list li", 0) == firstLi && eq(textOf(nth(v, "#list li", 1)), "zero"));
    /* the page sets the model: typed into the field */
    CscdElement* f = byId(v, "f");
    cscdViewFocus(v, f);
    f->caret = f->selAnchor = f->editValue.len;
    cscdViewTextInput(v, CSCD_SV("!"));
    CHECK(cscdSvEqC(cscdModelGet(m, CSCD_SV("title")), "Hello!") && bindChanges == 1);
    cscdViewUpdate(v);
    CHECK(eq(textOf(byId(v, "t")), "Hello!"));
    /* unbound: the page stays as it is */
    cscdViewUnbind(v, byId(v, "app"));
    cscdModelSet(m, CSCD_SV("title"), CSCD_SV("gone"));
    cscdViewUpdate(v);
    CHECK(eq(textOf(byId(v, "t")), "Hello!"));
    cscdRelease(m);
    cscdViewFree(v);
}

/* CSS counters in generated content. */
static void testCounters(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>body{counter-reset:ch} h2{counter-increment:ch;counter-reset:sec} h3{counter-increment:sec}"
                  " h3::before{content:counter(ch) '.' counter(sec) ' '} h2::before{content:counter(ch, upper-roman) ' '}"
                  " ol{counter-reset:it;list-style:none} li::before{counter-increment:it;content:counters(it, '.') ')'}</style>"
                  "<h2 id=a>A</h2><h3 id=a1>x</h3><h3 id=a2>y</h3><h2 id=b>B</h2><h3 id=b1>z</h3>"
                  "<ol><li>one<ol><li id=inner>in</li></ol></li></ol>");
    CHECK(eq(generated(v, "a"), "I "));
    CHECK(eq(generated(v, "b"), "II "));
    CHECK(eq(generated(v, "a2"), "1.2 "));
    CHECK(eq(generated(v, "b1"), "2.1 "));
    CHECK(eq(generated(v, "inner"), "1.1)"));
    cscdViewFree(v);
}

static const char* markerOf(CscdView* v, const char* id) {
    CscdLayoutBox* b = byId(v, id)->layout;
    for (size_t i = 0; i < b->children.len; ++i)
        if (cscdBoxChild(b, i)->isMarker || cscdStrEqC(&cscdBoxChild(b, i)->pseudo, "marker")) return cscdStrC(&cscdBoxChild(b, i)->text);
    return "";
}

static char* repeated(const char* s, int n, const char* tail) {
    const size_t len = strlen(s);
    char* out = (char*)malloc(len * (size_t)n + strlen(tail) + 1);
    for (int i = 0; i < n; ++i) memcpy(out + len * (size_t)i, s, len);
    strcpy(out + len * (size_t)n, tail);
    return out;
}

/* Siblings found by the position a node remembers: right after insertions and removals before it. */
static void testSiblingPositions(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>li:nth-child(2){color:red}</style><ul id=u><li id=a>a</li><li id=b>b</li><li id=c>c</li><li id=d>d</li></ul>");
    CscdElement *u = byId(v, "u"), *b = byId(v, "b"), *c = byId(v, "c"), *d = byId(v, "d");
    CHECK(cscdElementMatches(b, CSCD_SV(":nth-child(2)")));
    CscdElement* z = cscdDocumentCreateElement(cscdViewDocument(v), CSCD_SV("li"));
    cscdNodeInsertBefore(&u->node, &z->node, u->node.children.items[0]);
    CHECK(cscdElementMatches(b, CSCD_SV(":nth-child(3)")));
    CHECK(cscdElementMatches(z, CSCD_SV(":first-child")));
    CHECK(cscdNodePreviousElementSibling(&c->node) == b);
    cscdNodeRemoveChild(&u->node, &b->node);
    CHECK(cscdNodePreviousElementSibling(&c->node) == byId(v, "a"));
    CHECK(cscdNodeNextElementSibling(&c->node) == d);
    CHECK(cscdElementMatches(d, CSCD_SV(":last-child")) && cscdElementMatches(d, CSCD_SV(":nth-child(4)")));
    CHECK(cscdNodeIndexInParent(&d->node) == 3);
    cscdNodeDestroy(&b->node);
    cscdViewFree(v);
}

/* A positioned box without offsets stays where the flow would have put it, also when what holds it is
 * laid out twice (flex items: measured, then laid out for good) and the page is laid out again. */
static void testStaticPositionRelayout(void) {
    static const char* pages[] = {
        "<div style='display:flex'><div style='flex:1;height:80px'>a</div><div style='flex:1'><div id=e style='position:absolute'>e</div>t</div></div>",
        "<div style='display:flex'><div style='flex:1;height:80px'>a</div><div style='flex:1'><p style='margin:0;height:20px'>p</p><div id=e style='position:absolute'>e</div></div></div>",
        "<div style='display:flex'><div style='flex:1;height:80px'>a</div><div style='flex:1;display:flex'><div style='flex:1'><div id=e style='position:fixed'>e</div>t</div></div></div>",
    };
    static const float wantY[] = {0, 20, 0};
    for (int i = 0; i < 3; ++i) {
        CscdView* v = cscdViewNew();
        char html[600];
        snprintf(html, sizeof html, "<style>body{margin:0}</style>%s", pages[i]);
        layoutViewWH(v, html, 400, 300);
        NEAR(cscdViewElementRect(v, byId(v, "e")).x, 200);
        NEAR(cscdViewElementRect(v, byId(v, "e")).y, wantY[i]);
        cscdViewSetViewport(v, 600, 300, 1);
        cscdViewUpdate(v);
        NEAR(cscdViewElementRect(v, byId(v, "e")).x, 300);
        cscdViewFree(v);
    }
}

/* Percentage widths on cells size their columns (auto table layout), as browsers do. */
static void testTablePercentColumns(void) {
    static const struct { const char* a; const char* b; const char* table; float wa, wb; } cases[] = {
        {"width:30%", "", "width:400px", 120, 280},             /* the share, the rest to the others */
        {"width:25%", "width:25%", "width:400px", 200, 200},     /* all percentages, room left: they grow */
        {"width:70%", "width:50%", "width:400px", 280, 120},     /* over 100%: the later column gets what is left */
        {"width:50%", "", "", 32, 32},                           /* an auto table wide enough for the share */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        CscdView* v = cscdViewNew();
        char html[512];
        snprintf(html, sizeof html,
                 "<style>body{margin:0}td{padding:0}</style><table style='%s;border-spacing:0'><tr><td id=a style='%s'>"
                 "<div style='width:32px'></div></td><td id=b style='%s'>b</td></tr></table>",
                 cases[i].table, cases[i].a, cases[i].b);
        layoutView(v, html);
        NEAR(cscdViewElementRect(v, byId(v, "a")).w, cases[i].wa);
        NEAR(cscdViewElementRect(v, byId(v, "b")).w, cases[i].wb);
        cscdViewFree(v);
    }
}

/* Hostile documents stay within bounds: nesting depth, nested rules and selectors and values, var()s
 * that grow without end, list numbers past what a style can write. */
/* What the on-screen keyboards of phones use: the focused field's whole text state, the keyboard its
 * attributes ask for, and the system's theme, motion and text size. */
static void testPhoneInput(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<input id=t value='caf'><textarea id=a>one\ntwo</textarea><input id=m type=email enterkeyhint=send>"
                  "<input id=n inputmode=numeric autocapitalize=words><input id=r readonly value=x><input id=p type=password>"
                  "<p id=x style='font-size:medium'>x</p><p id=y style='font-size:20px'>y</p>");
    CscdTextState st;
    CHECK(!cscdViewGetTextState(v, &st));
    CscdElement* t = byId(v, "t");
    int inputs = 0;
    on(t, "input", countEvent, &inputs);
    cscdViewFocus(v, t);
    CHECK(cscdViewGetTextState(v, &st) && eq(cscdStrC(&st.text), "caf") && st.selStart == 3 && st.selEnd == 3 && st.composeStart == st.composeEnd);
    cscdTextStateFini(&st);
    /* composing "é" after "caf", then committing it */
    CscdTextState s = {cscdStrDupC("caf\xC3\xA9"), 5, 5, 3, 5};
    cscdViewSetTextState(v, &s);
    CHECK(eq(valueOf(t), "caf\xC3\xA9") && inputs == 1);
    CHECK(cscdViewGetTextState(v, &st) && st.composeStart == 3 && st.composeEnd == 5 && st.selEnd == 5);
    cscdTextStateFini(&st);
    s.composeStart = s.composeEnd = 0;
    cscdViewSetTextState(v, &s);
    CHECK(inputs == 1); /* (the same text: no input event) */
    CHECK(cscdViewGetTextState(v, &st) && st.composeStart == st.composeEnd);
    cscdTextStateFini(&st);
    /* offsets inside a character, and a line break, in a single line field */
    cscdStrSet(&s.text, CSCD_SV("a\nb\xC3\xA9"));
    s.selStart = 0;
    s.selEnd = 4; /* (inside the é once the line break goes) */
    cscdViewSetTextState(v, &s);
    CHECK(eq(valueOf(t), "ab\xC3\xA9") && cscdViewGetTextState(v, &st) && st.selStart == 0 && st.selEnd == 2);
    cscdTextStateFini(&st);
    cscdTextStateFini(&s);
    CHECK(cscdViewInputTraits(v).kind == CSCD_INPUT_TEXT && cscdViewInputTraits(v).autocorrect &&
          cscdViewInputTraits(v).capitalize == CSCD_CAPITALIZE_SENTENCES);
    /* where the text is: the caret's box, a range's, the offset under a point */
    cscdViewUpdate(v);
    const CscdRect caret = cscdViewCaretRect(v), range = cscdViewTextRangeRect(v, 0, 2);
    CHECK(range.w > 4 && fabsf(range.y - caret.y) < 0.5f);
    CHECK(cscdViewTextOffsetAt(v, range.x + 1, range.y + range.h / 2) == 0);
    /* the keyboards the fields ask for */
    cscdViewFocus(v, byId(v, "a"));
    CHECK(cscdViewInputTraits(v).kind == CSCD_INPUT_MULTILINE);
    const CscdRect lines = cscdViewTextRangeRect(v, 1, 6); /* (from the first line into the second) */
    CHECK(lines.w > 4);
    cscdViewFocus(v, byId(v, "m"));
    CscdInputTraits it = cscdViewInputTraits(v);
    CHECK(it.kind == CSCD_INPUT_EMAIL && it.enter == CSCD_ENTER_SEND && !it.autocorrect && it.capitalize == CSCD_CAPITALIZE_NONE);
    cscdViewFocus(v, byId(v, "n"));
    it = cscdViewInputTraits(v);
    CHECK(it.kind == CSCD_INPUT_NUMBER && it.capitalize == CSCD_CAPITALIZE_WORDS);
    cscdViewFocus(v, byId(v, "r"));
    CHECK(cscdViewInputTraits(v).kind == CSCD_INPUT_NONE);
    cscdViewFocus(v, byId(v, "p"));
    CHECK(cscdViewInputTraits(v).kind == CSCD_INPUT_PASSWORD && !cscdViewInputTraits(v).autocorrect);
    cscdViewBlur(v);
    CHECK(cscdViewInputTraits(v).kind == CSCD_INPUT_NONE);
    /* the system's preferences: dark, less motion, bigger text (keywords and the root's default, not px) */
    CscdSystemPreferences p = {true, true, 1.5f};
    cscdViewSetSystemPreferences(v, p);
    cscdViewUpdate(v);
    CHECK(cscdViewSystemPreferences(v).dark && !cscdViewFeature(v, CSCD_FEATURE_ANIMATIONS));
    NEAR(byId(v, "x")->style->font.size, 24);
    NEAR(byId(v, "y")->style->font.size, 20);
    /* what the app chose stays */
    cscdViewSetDarkMode(v, false);
    p.textScale = 1;
    cscdViewSetSystemPreferences(v, p);
    cscdViewUpdate(v);
    CHECK(!cscdViewSystemPreferences(v).dark);
    NEAR(byId(v, "x")->style->font.size, 16);
    cscdViewFree(v);
    /* the page's background, for what a phone shows around it: body's when the root has none, over
     * white; half transparent, mixed */
    v = cscdViewNew();
    layoutView(v, "<style>body{background:#204060}</style><p>x</p>");
    CscdColor c = cscdViewCanvasColor(v);
    CHECK(c.r == 0x20 && c.g == 0x40 && c.b == 0x60 && c.a == 255);
    layoutView(v, "<style>html{background:rgba(0,0,0,.5)}</style><p>x</p>");
    c = cscdViewCanvasColor(v);
    CHECK(c.r == 127 && c.g == 127 && c.b == 127);
    cscdViewFree(v);
}

static void testLimits(void) {
    CscdView* v = cscdViewNew();
    char* deep = repeated("<div>", 2000, "<p id=last>x</p>");
    layoutView(v, deep);
    free(deep);
    int depth = 0;
    for (const CscdNode* n = &byId(v, "last")->node; n; n = n->parent) ++depth;
    CHECK(depth > 500 && depth < 520);

    char* rules = repeated("p{", 5000, "color:red");
    char* doc = (char*)malloc(strlen(rules) + 64);
    sprintf(doc, "<style>%s</style><p id=p>x</p>", rules);
    layoutView(v, doc);
    free(rules);
    free(doc);
    CHECK(byId(v, "p") != NULL);

    char* is = repeated(":is(", 100, "p");
    char* is2 = (char*)malloc(strlen(is) + 300);
    sprintf(is2, "<style>%s", is);
    for (int i = 0; i < 100; ++i) strcat(is2, ")");
    strcat(is2, "{color:red}</style><p id=p>x</p>");
    layoutView(v, is2);
    free(is);
    free(is2);
    CHECK(byId(v, "p")->style->color.r == 0); /* (too deep: not a selector) */

    CscdStr vars = cscdStrDupC("<style>:root{");
    for (int i = 0; i < 40; ++i) cscdStrAppendf(&vars, "--v%d:var(--v%d) var(--v%d);", i, i + 1, i + 1);
    cscdStrAppendC(&vars, "--v40:1px}p{margin-left:var(--v0)}</style><p id=p>x</p>");
    layoutView(v, cscdStrC(&vars));
    cscdStrFree(&vars);
    NEAR(byId(v, "p")->layout->margin[3], 0); /* (invalid at computed-value time) */

    layoutView(v, "<ol type=I start=3998><li id=r1>a<li id=r2>b<li id=r3>c</ol><ol style='list-style:lower-alpha' start=0><li id=z>z</ol>"
                  "<ol start=2147483647><li id=big>a<li id=big2>b</ol>");
    CHECK(eq(markerOf(v, "r1"), "MMMCMXCVIII. "));
    CHECK(eq(markerOf(v, "r3"), "4000. "));
    CHECK(eq(markerOf(v, "z"), "0. "));
    CHECK(eq(markerOf(v, "big2"), "1000000000. "));

    layoutView(v, "<div style='display:grid'><div id=g style='grid-column:1/99999999'>x</div></div>");
    CHECK(byId(v, "g")->layout != NULL);
    cscdViewFree(v);
}

static bool gray(uint32_t p) {
    const int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
    return abs(r - g) < 3 && abs(g - b) < 3;
}

/* backdrop-filter filters what is behind a box; mask-image fades a box by its alpha. */
static void testBackdropAndMask(void) {
    CscdView* v = cscdViewNew();
    layoutViewWH(v, "<style>body{margin:0;background:#fff} .stripes{position:absolute;left:0;top:0;width:200px;height:100px;"
                    "background:linear-gradient(90deg,#f00 50%,#00f 50%)} #g{position:absolute;left:0;top:0;width:200px;height:100px;"
                    "backdrop-filter:grayscale(1)} #m{position:absolute;left:0;top:120px;width:100px;height:100px;background:#000;"
                    "mask-image:linear-gradient(#000, transparent)}</style><div class=stripes></div><div id=g></div><div id=m></div>",
                 300, 300);
    CscdImage img;
    cscdImageInit(&img, 300, 300);
    cscdViewPaint(v, &img, false);
    CHECK(gray(cscdImageRow(&img, 50)[20]) && gray(cscdImageRow(&img, 50)[180])); /* the red and blue under it went gray */
    CHECK(((cscdImageRow(&img, 50)[20] >> 16) & 255) != ((cscdImageRow(&img, 50)[180] >> 16) & 255)); /* (still two different grays) */
    const int top = cscdImageRow(&img, 122)[50] & 255, bottom = cscdImageRow(&img, 216)[50] & 255; /* black fading into white */
    CHECK(top < 30 && bottom > 220);
    cscdImageFini(&img);
    cscdViewFree(v);
}

/* text fill and stroke, image-rendering, table-layout: fixed, caption-side, text-align-last. */
static void testSmallerProperties(void) {
    CscdView* v = cscdViewNew();
    CscdImage* checker = cscdImageNew(2, 1);
    checker->pixels[0] = 0xFF000000u;
    checker->pixels[1] = 0xFFFFFFFFu;
    cscdViewSetImage(v, CSCD_SV("checker.png"), checker);
    cscdRelease(checker);
    layoutView(v, "<style>body{margin:0;background:#fff} #t{font:900 60px sans-serif;-webkit-text-fill-color:transparent;-webkit-text-stroke:2px #f00}"
                  " img{display:block;width:40px;height:20px;image-rendering:pixelated}"
                  " table{width:300px;table-layout:fixed;border-spacing:0} td{padding:0} caption{caption-side:bottom}"
                  " p{width:200px;text-align-last:right;margin:0}</style>"
                  "<div id=t>O</div><img id=i src=checker.png><table id=tab><caption id=cap>c</caption><tr><td id=c1 style='width:50px'>x</td>"
                  "<td id=c2>a long cell that would be wide</td><td id=c3>y</td></tr></table><p><span id=last>end</span></p>");
    CscdImage img;
    cscdImageInit(&img, 400, 400);
    cscdViewPaint(v, &img, false);
    /* the O: red outline, nothing filled inside it */
    CscdRect t = viewRect(v, byId(v, "t"));
    bool isRed = false;
    for (int x = 0; x < 60 && !isRed; ++x) {
        const uint32_t p = cscdImageRow(&img, (int)(t.y + t.h / 2))[x];
        isRed = ((p >> 16) & 255) > 200 && (p & 255) < 80;
    }
    CHECK(isRed);
    /* pixelated: the two source pixels stay black and white, no gray in between */
    CscdRect i = viewRect(v, byId(v, "i"));
    const uint32_t mid = cscdImageRow(&img, (int)i.y + 10)[19] & 0xFFFFFF, mid2 = cscdImageRow(&img, (int)i.y + 10)[20] & 0xFFFFFF;
    CHECK((mid == 0 && mid2 == 0xFFFFFF) || (mid == 0xFFFFFF && mid2 == 0));
    NEAR(rectOf(v, "c1").w, 50);
    NEAR(rectOf(v, "c2").w, 125);
    NEAR(rectOf(v, "c3").w, 125);
    CHECK(rectOf(v, "cap").y > rectOf(v, "c2").y);
    CscdRect last = viewRect(v, byId(v, "last"));
    NEAR(last.x + last.w, 200);
    cscdImageFini(&img);
    cscdViewFree(v);
}

/* ---- bidirectional text ----------------------------------------------------------------------------- */

/* the levels of text (UTF-8) in a paragraph at level, as a string of digits */
static const char* bidiOf(const char* textUtf8, int level) {
    CscdU32Vec cps = {0};
    CscdSv s = cscdSv(textUtf8);
    for (size_t i = 0; i < s.len;) cscdVecPush(&cps, cscdUtf8Next(s, &i));
    CscdBytes lv = {0};
    cscdVecResize(&lv, cps.len);
    cscdBidiLevels(cps.items, cps.len, level, lv.items);
    CscdStr out = {0};
    for (size_t i = 0; i < lv.len; ++i) cscdStrAppendChar(&out, (char)('0' + lv.items[i]));
    cscdVecFree(&cps);
    cscdVecFree(&lv);
    return keep(out);
}

static void testBidi(void) {
    /* Hebrew in English: the Hebrew word at 1, the space before it stays with the English */
    CHECK(eq(bidiOf("ab \xD7\x90\xD7\x91 cd", 0), "00011000"));
    /* digits after Hebrew: EN at 2, and the space between them goes right to left (N1) */
    CHECK(eq(bidiOf("\xD7\x90 12", 0), "1122"));
    /* Arabic letters make following digits Arabic numbers (W2), at 2 as well */
    CHECK(eq(bidiOf("\xD8\xA7 1", 0), "112"));
    /* a right-to-left paragraph: English at 2, the period between them at 1 */
    CHECK(eq(bidiOf("ab.\xD7\x90", 1), "2211"));
    /* explicit embedding and isolate (the controls take the level before them) */
    CHECK(eq(bidiOf("a\xE2\x80\xAB" "b\xE2\x80\xAC" "c", 0), "00220"));
    CHECK(eq(bidiOf("a\xE2\x81\xA7" "b\xE2\x81\xA9" "c", 0), "00200"));
    /* brackets around Hebrew in English take the Hebrew's direction only when it is all inside (N0) */
    CHECK(eq(bidiOf("a (\xD7\x90) b", 0), "0001000"));
    CHECK(eq(bidiOf("\xD7\x90 (\xD7\x91) \xD7\x92", 0), "1111111"));
    /* the line's trailing spaces go back to the paragraph level (L1) */
    {
        const uint32_t cps[3] = {0x5D0, 0x20, 0x20};
        uint8_t lv[3] = {1, 1, 1};
        cscdBidiLineLevels(cps, 3, 0, lv);
        CHECK(lv[0] == 1 && lv[1] == 0 && lv[2] == 0);
    }
    /* L2: runs at odd levels reversed, nested ones put back */
    {
        const uint8_t lv[8] = {0, 0, 1, 1, 2, 2, 1, 0};
        CscdSizeVec order = {0};
        cscdBidiVisualOrder(lv, 8, &order);
        const size_t want[8] = {0, 1, 6, 4, 5, 3, 2, 7};
        CHECK(order.len == 8 && !memcmp(order.items, want, sizeof want));
        cscdVecFree(&order);
    }
    CHECK(cscdBidiMirror('(') == ')' && cscdBidiMirror(0x00BB) == 0x00AB && cscdBidiMirror('a') == 'a');
    CHECK(cscdBidiClass(0x05D0) == CSCD_BIDI_R && cscdBidiClass(0x0661) == CSCD_BIDI_AN && cscdBidiClass('x') == CSCD_BIDI_L);
    {
        const uint32_t latin[3] = {'h', 'i', 0xE9};
        CHECK(!cscdBidiNeeded(latin, 3));
    }

    CscdView* v = cscdViewNew();
    /* a Hebrew paragraph, right to left: the first word at the right edge, the second to its left */
    layoutView(v, "<style>body{margin:0;font-size:20px} p{margin:0;width:400px}</style>"
                  "<p dir=rtl><span id=a>\xD7\x90\xD7\x91\xD7\x92</span> <span id=b>\xD7\x93\xD7\x94\xD7\x95</span></p>");
    CscdRect a = viewRect(v, byId(v, "a")), b = viewRect(v, byId(v, "b"));
    CHECK(a.x > b.x);
    NEAR(cscdRectRight(a), 400);
    CHECK(cscdBoxChild(byId(v, "a")->layout, 0)->textFrags.items[0].rtl);
    /* Hebrew words inside English: shown in reverse, between the English words */
    layoutView(v, "<style>body{margin:0;font-size:20px}</style>"
                  "<p><span id=e1>abc</span> <span id=h1>\xD7\x90\xD7\x91</span> <span id=h2>\xD7\x92\xD7\x93</span> <span id=e2>xyz</span></p>");
    CscdRect e1 = viewRect(v, byId(v, "e1")), h1 = viewRect(v, byId(v, "h1")), h2 = viewRect(v, byId(v, "h2")), e2 = viewRect(v, byId(v, "e2"));
    CHECK(cscdRectRight(e1) <= h2.x + 0.5f && cscdRectRight(h2) < h1.x && cscdRectRight(h1) <= e2.x + 0.5f);
    CHECK(!cscdBoxChild(byId(v, "e1")->layout, 0)->textFrags.items[0].rtl);
    /* English only: nothing moves (and no text is marked right to left) */
    layoutView(v, "<style>body{margin:0;font-size:20px}</style><p><span id=x>one</span> <span id=y>two</span></p>");
    CHECK(viewRect(v, byId(v, "x")).x < viewRect(v, byId(v, "y")).x);
    cscdViewFree(v);
}

/* ---- what the decoders read, against what other decoders make of it (tools/makeTestFixtures.py) ------ */

#include "testFixtures.inc"

static void checkJpeg(const unsigned char* data, size_t size, const JpegSample* at, size_t n, int tolerance) {
    CscdImage* img = cscdDecodeImage(data, size);
    CHECK(img && img->width == 24 && img->height == 16);
    if (!img) return;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t p = cscdImageRow(img, at[i].y)[at[i].x];
        const int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        CHECK((p >> 24) == 255);
        ++checks;
        if (abs(r - at[i].r) > tolerance || abs(g - at[i].g) > tolerance || abs(b - at[i].b) > tolerance) {
            ++failures;
            printf("FAIL jpeg at %d,%d: %d %d %d, libjpeg %d %d %d\n", at[i].x, at[i].y, r, g, b, at[i].r, at[i].g, at[i].b);
        }
    }
    cscdRelease(img);
}

/* (a little apart from libjpeg: its transforms are not ours, nor is how 4:2:0 color is spread) */
#define JPEG(name, tolerance) checkJpeg(name, sizeof name, name##At, sizeof name##At / sizeof name##At[0], tolerance)

static void testJpeg(void) {
    JPEG(kJpeg444, 4);
    JPEG(kJpeg420, 24);
    JPEG(kJpegProgressive, 24);
    JPEG(kJpegRestart, 4);
    JPEG(kJpegGray, 4);
    /* progressive and baseline hold the same picture */
    CscdImage* a = cscdDecodeImage(kJpeg420, sizeof kJpeg420);
    CscdImage* b = cscdDecodeImage(kJpegProgressive, sizeof kJpegProgressive);
    CHECK(a && b);
    if (a && b) {
        int worst = 0;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 24; ++x) {
                const int d = abs((int)((cscdImageRow(a, y)[x] >> 8) & 255) - (int)((cscdImageRow(b, y)[x] >> 8) & 255));
                if (d > worst) worst = d;
            }
        CHECK(worst <= 4);
    }
    cscdRelease(a);
    cscdRelease(b);
    /* cut short and damaged: a picture or nothing, never a crash */
    for (size_t n = 0; n < sizeof kJpegProgressive; n += 37) {
        CscdImage* cut = cscdDecodeImage(kJpegProgressive, n);
        if (cut) cscdRelease(cut);
    }
    uint8_t broken[sizeof kJpeg420];
    memcpy(broken, kJpeg420, sizeof broken);
    for (size_t i = 30; i < sizeof broken; i += 11) broken[i] ^= 0x5A;
    CscdImage* bad = cscdDecodeImage(broken, sizeof broken);
    if (bad) cscdRelease(bad);
}

/* Outlines in CFF, and what cascade does not read. */
static void testCffFont(void) {
    CscdFontFace* f = cscdFontFaceLoad(NULL, kPixelsFont, sizeof kPixelsFont, 0);
    CHECK(f != NULL);
    if (!f) return;
    const uint16_t a = cscdFontFaceGlyphIndex(f, 'A');
    CHECK(a != 0 && cscdFontFaceGlyphIndex(f, 0x3B1) == 0);
    NEAR(cscdFontFaceAdvance(f, a), 600);
    NEAR(cscdFontFaceAdvance(f, cscdFontFaceGlyphIndex(f, ' ')), 300);
    CscdPath p = {0};
    CHECK(cscdFontFaceOutline(f, a, &p));
    CHECK(p.verbs.len > 8); /* two closed shapes of four curves */
    const CscdRect r = cscdPathBounds(&p);
    /* (the letter's numbers in tools/makeTestFixtures.py) */
    const float rx = 180 + 0x41 * 7 % 90, ry = 260 + 0x41 * 11 % 90;
    CHECK(fabs(r.w - 2 * rx) < 30 && fabs(r.h - 2 * ry) < 2);
    cscdPathFree(&p);
    cscdFontFaceFree(f);
    /* WOFF2 is not read: its files are turned away */
    static const uint8_t woff2[64] = {'w', 'O', 'F', '2', 0, 1, 0, 0};
    CHECK(cscdFontFaceLoad(NULL, woff2, sizeof woff2, 0) == NULL);
}

/* What the pages below read. */
static bool fixtures(void* user, CscdSv path, CscdStr* out) {
    (void)user;
#define SERVE(name, data) if (cscdSvEq(path, CSCD_SV("fixtures/" name))) { cscdStrAppend(out, (CscdSv){(const char*)data, sizeof data}); return true; }
    SERVE("pixels.otf", kPixelsFont)
    SERVE("sbix.ttf", kSbixFont)
    SERVE("cbdt.ttf", kCbdtFont)
    SERVE("photo.jpg", kJpeg444)
    SERVE("progressive.jpg", kJpegProgressive)
#undef SERVE
    return false;
}

static void loadFixturePage(CscdView* v, const char* html, float w, float h, float scale) {
    cscdViewLoadHTML(v, cscdSv(html), CSCD_SV("fixtures"));
    cscdViewSetViewport(v, w, h, scale);
    cscdViewUpdate(v);
}

static int countPixels(const CscdImage* img, uint32_t color, int tolerance) {
    int n = 0;
    for (int y = 0; y < img->height; ++y)
        for (int x = 0; x < img->width; ++x) {
            const uint32_t p = cscdImageRow(img, y)[x];
            if (abs((int)((p >> 16) & 255) - (int)((color >> 16) & 255)) <= tolerance && abs((int)((p >> 8) & 255) - (int)((color >> 8) & 255)) <= tolerance &&
                abs((int)(p & 255) - (int)(color & 255)) <= tolerance) ++n;
        }
    return n;
}

/* Fonts of pictures: the letter A is a red square in each. */
static void testBitmapFonts(void) {
    cscdAppSetResourceLoader(fixtures, NULL);
    static const char* const pages[2] = {
        "<style>@font-face{font-family:F;src:url(sbix.ttf)} body{margin:0;background:#fff;font:32px F;line-height:40px}</style><div>A</div>",
        "<style>@font-face{font-family:F;src:url(cbdt.ttf)} body{margin:0;background:#fff;font:32px F;line-height:40px}</style><div>A</div>"};
    for (int i = 0; i < 2; ++i) {
        CscdView* v = cscdViewNew();
        loadFixturePage(v, pages[i], 100, 50, 1);
        CscdImage img;
        cscdImageInit(&img, 100, 50);
        cscdViewPaint(v, &img, false);
        const int red = countPixels(&img, 0xFF0000u, 8);
        CHECK(red > 32 * 32 * 3 / 4 && red <= 34 * 34); /* (16 pixels, drawn at 32) */
        cscdImageFini(&img);
        cscdViewFree(v);
    }
    cscdAppSetResourceLoader(NULL, NULL);
}

static void testSvg(void) {
    CscdView* v = cscdViewNew();
    layoutViewWH(v, "<style>body{margin:0;background:#fff} svg{display:block}</style>"
                    "<svg width=200 height=200 viewBox='0 0 100 100'>"
                    "<rect x=10 y=10 width=30 height=30 fill='#f00'/><circle cx=70 cy=25 r=15 fill='#0f0'/>"
                    "<path d='M10 70 A20 20 0 0 1 50 70 Z' fill='#00f'/>"
                    "<g transform='translate(60 50)'><rect width=20 height=20 fill='#f0f' opacity='.5'/></g>"
                    "<line x1=0 y1=90 x2=100 y2=90 stroke='#000' stroke-width=4 stroke-dasharray='10 10'/>"
                    "</svg>", 200, 200);
    CscdImage img;
    cscdImageInit(&img, 200, 200);
    cscdViewPaint(v, &img, false);
    const uint32_t* row = cscdImageRow(&img, 50);
    CHECK((row[50] & 0xFFFFFF) == 0xFF0000u && (row[140] & 0xFFFFFF) == 0x00FF00u); /* viewBox doubles it */
    CHECK((row[10] & 0xFFFFFF) == 0xFFFFFFu);
    CHECK((cscdImageRow(&img, 130)[60] & 0xFFFFFF) == 0x0000FFu); /* in the arc's half circle */
    CHECK((cscdImageRow(&img, 95)[25] & 0xFFFFFF) == 0xFFFFFFu);  /* over its top */
    const uint32_t half = cscdImageRow(&img, 120)[140] & 0xFFFFFF; /* moved by the group, half opaque */
    CHECK(((half >> 16) & 255) == 255 && ((half >> 8) & 255) > 120 && ((half >> 8) & 255) < 135);
    row = cscdImageRow(&img, 180);
    CHECK((row[10] & 0xFFFFFF) == 0 && (row[30] & 0xFFFFFF) == 0xFFFFFFu && (row[50] & 0xFFFFFF) == 0); /* dashes 20 pixels long */
    cscdImageFini(&img);
    cscdViewFree(v);
}

static void testBlur(void) {
    /* a blurred path: dark in the middle, fading over its edge, nothing far from it */
    CscdImage img;
    cscdImageInit(&img, 100, 100);
    CscdCanvas* c = cscdCanvasNew(&img);
    cscdCanvasFillRect(c, cscdRect(0, 0, 100, 100), cscdRgb(255, 255, 255));
    CscdPath p = {0};
    cscdPathMoveTo(&p, 30, 30);
    cscdPathLineTo(&p, 70, 30);
    cscdPathLineTo(&p, 70, 70);
    cscdPathLineTo(&p, 30, 70);
    cscdPathClose(&p);
    cscdCanvasDrawBlurredPath(c, &p, 8, cscdRgb(0, 0, 0));
    cscdPathFree(&p);
    cscdCanvasFree(c);
    const uint32_t* row = cscdImageRow(&img, 50);
    CHECK((row[50] & 255) < 30 && (row[30] & 255) > 90 && (row[30] & 255) < 170 && (row[5] & 255) == 255);
    CHECK((row[22] & 255) > (row[30] & 255) && (row[30] & 255) > (row[38] & 255));
    cscdImageFini(&img);
    /* filter and shadows */
    CscdView* v = cscdViewNew();
    layoutViewWH(v, "<style>body{margin:0;background:#fff} div{position:absolute;width:40px;height:40px}"
                    "#f{left:30px;top:30px;background:#000;filter:blur(6px)} #s{left:130px;top:30px;background:#fff;box-shadow:0 0 12px #000}</style>"
                    "<div id=f></div><div id=s></div>", 220, 100);
    cscdImageInit(&img, 220, 100);
    cscdViewPaint(v, &img, false);
    row = cscdImageRow(&img, 50);
    CHECK((row[50] & 255) < 40 && (row[30] & 255) > 90 && (row[30] & 255) < 170 && (row[5] & 255) == 255); /* the filter */
    CHECK((row[150] & 255) == 255 && (row[126] & 255) < 220 && (row[100] & 255) == 255);                   /* the shadow, outside the box */
    cscdImageFini(&img);
    cscdViewFree(v);
}

static void testKeyframes(void) {
    CscdView* v = cscdViewNew();
    layoutView(v, "<style>@keyframes fade{from{opacity:1}to{opacity:0}} @keyframes grow{0%{width:10px}50%{width:110px}100%{width:10px}}"
                  "#a{width:50px;height:50px;animation:fade 1s linear forwards} #b{height:50px;width:10px;animation:grow 1s linear}"
                  "#c{width:50px;height:50px;opacity:1;animation:fade 1s linear 10s}</style><div id=a></div><div id=b></div><div id=c></div>");
    CscdElement* a = byId(v, "a");
    /* (long enough that a busy machine sleeping late still lands in the middle) */
    sleepMs(250);
    cscdViewUpdate(v);
    CHECK(a->style->opacity > 0.1f && a->style->opacity < 0.9f);
    CHECK(rectOf(v, "b").w > 15 && rectOf(v, "b").w < 110);
    NEAR(byId(v, "c")->style->opacity, 1); /* its delay holds it */
    sleepMs(1000);
    cscdViewUpdate(v);
    NEAR(a->style->opacity, 0); /* forwards: it stays where it ended */
    NEAR(rectOf(v, "b").w, 10); /* and without it, back to the style */
    cscdViewFree(v);
}

static void testTextLines(void) {
    cscdAppSetResourceLoader(fixtures, NULL);
    CscdView* v = cscdViewNew();
    loadFixturePage(v, "<style>@font-face{font-family:P;src:url(pixels.otf)} body{margin:0;background:#fff;color:#000;font:20px/30px P}"
                       "div{position:absolute;left:0;white-space:nowrap} #plain{top:0} #under{top:30px;text-decoration:underline;text-decoration-color:#f00}"
                       "#through{top:60px;text-decoration:line-through;text-decoration-color:#00f}"
                       "#cut{top:90px;width:100px;overflow:hidden;text-overflow:ellipsis}</style>"
                       "<div id=plain>Hello</div><div id=under>Hello</div><div id=through>Hello</div><div id=cut>Hello Hello Hello Hello</div>",
                    300, 120, 1);
    NEAR(rectOf(v, "plain").w, 60); /* five letters of 0.6em: the page's font, not one of the system's */
    CscdImage img;
    cscdImageInit(&img, 300, 120);
    cscdViewPaint(v, &img, false);
    int red = 0, blue = 0, past = 0;
    for (int x = 0; x < 60; ++x) {
        for (int y = 30; y < 60; ++y) { const uint32_t p = cscdImageRow(&img, y)[x]; red += ((p >> 16) & 255) > (p & 255) + 40; }
        for (int y = 60; y < 90; ++y) { const uint32_t p = cscdImageRow(&img, y)[x]; blue += (p & 255) > ((p >> 16) & 255) + 40; }
    }
    for (int x = 100; x < 300; ++x)
        for (int y = 90; y < 120; ++y) past += (cscdImageRow(&img, y)[x] & 0xFFFFFF) != 0xFFFFFFu;
    CHECK(red >= 30 && blue >= 30); /* (the lines, softened at their edges) */
    CHECK(past == 0);
    cscdImageFini(&img);
    cscdViewFree(v);
    cscdAppSetResourceLoader(NULL, NULL);
}

/* ---- the same pixels everywhere ----------------------------------------------------------------------
 * The promise of docs/principles.md, kept here: each page below is painted, its pixels hashed, and the
 * hash compared with tests/pixelsExpected.inc. cascade-tests runs on every platform CI has (MinGW, MSVC,
 * GCC and Clang, x86 and ARM, the iOS simulator, the Android emulator), each against the same hashes: a
 * platform that paints one pixel differently fails. The pages use only the font and the pictures above,
 * nothing of the machine's. When a change means to change pixels, the test prints the new lines; with
 * CASCADE_PIXELS_DIR set it also saves each page as a PNG, to look at before taking them. */

#define PIXEL_FONT "@font-face{font-family:P;src:url(pixels.otf)} *{font-family:P} html{background:#fff;color:#123}"

typedef struct PixelPage { const char* name; int width, height; const char* html; } PixelPage;

static const PixelPage kPixelPages[] = {
    {"boxes", 320, 240, "<style>" PIXEL_FONT "body{margin:0} div{position:absolute}"
        "#a{left:10px;top:10px;width:90px;height:60px;background:linear-gradient(35deg,#e33,#fc3 40%,#39f);border-radius:14px 30px}"
        "#b{left:120px;top:14px;width:70px;height:50px;border:5px dashed #274;border-radius:50%}"
        "#c{left:210px;top:10px;width:90px;height:70px;background:radial-gradient(circle at 30% 30%,#fff,#a2f);box-shadow:3px 4px 9px #0007,inset 0 0 6px #fff}"
        "#d{left:20px;top:100px;width:80px;height:80px;background:conic-gradient(#f00,#ff0,#0f0,#0ff,#00f,#f0f,#f00);transform:rotate(17deg) scale(.9)}"
        "#e{left:130px;top:100px;width:60px;height:60px;background:#08c;opacity:.55;outline:2px solid #c40;outline-offset:3px}"
        "#f{left:215px;top:105px;width:80px;height:80px;border:7px double #753;background:repeating-linear-gradient(45deg,#eee 0 6px,#bcd 6px 12px)}"
        "#g{left:130px;top:190px;width:170px;height:34px;background:oklch(70% .15 200);clip-path:polygon(0 0,100% 20%,85% 100%,10% 80%)}"
        "</style><div id=a></div><div id=b></div><div id=c></div><div id=d></div><div id=e></div><div id=f></div><div id=g></div>"},
    {"text", 320, 240, "<style>" PIXEL_FONT "body{margin:8px;font-size:15px;line-height:1.3}"
        "h1{font-size:34px;margin:0;letter-spacing:-1px} p{margin:4px 0} .b{font-weight:700} .i{font-style:italic} .s{font-size:9.5px}"
        ".sh{text-shadow:1px 2px 3px #e33} .t{transform:rotate(-4deg) translateX(3.5px)} .u{text-decoration:underline wavy #c2c}"
        ".g{background:linear-gradient(90deg,#e33,#33e);-webkit-background-clip:text;color:transparent;font-size:26px}</style>"
        "<h1>Same pixels</h1><p class=b>Bold made from the regular</p><p class=i>Leaning the same way</p><p class=s>Small 0123456789</p>"
        "<p class=sh>Shadow under letters</p><p class=t>Turned a little</p><p class=u>Wavy line</p><div class=g>Gradient</div>"},
    {"images", 320, 240, "<style>" PIXEL_FONT "body{margin:0} img,div{position:absolute}"
        "#a{left:8px;top:8px;width:120px;height:80px} #b{left:140px;top:8px;width:37px;height:53px;image-rendering:pixelated}"
        "#c{left:190px;top:8px;width:120px;height:80px;object-fit:cover;border-radius:40px}"
        "#d{left:8px;top:100px;width:150px;height:60px;background:url(photo.jpg) 3px 5px/33px 21px repeat}"
        "#e{left:170px;top:100px;width:13px;height:9px} #f{left:8px;top:170px;width:300px;height:60px;background:url(progressive.jpg) center/cover;transform:skewX(-8deg)}"
        "</style><img id=a src=photo.jpg><img id=b src=photo.jpg><img id=c src=progressive.jpg><div id=d></div><img id=e src=progressive.jpg><div id=f></div>"},
    {"effects", 320, 240, "<style>" PIXEL_FONT "body{margin:0} div{position:absolute}"
        "#bg{inset:0;background:repeating-linear-gradient(60deg,#f55 0 14px,#5af 14px 28px)}"
        "#a{left:20px;top:20px;width:110px;height:80px;backdrop-filter:blur(5px) saturate(.4);background:#fff4;border-radius:12px}"
        "#b{left:160px;top:20px;width:110px;height:80px;background:#2a6;filter:blur(3px) hue-rotate(40deg)}"
        "#c{left:20px;top:125px;width:110px;height:90px;background:#329;mask-image:radial-gradient(circle,#000 40%,transparent 70%)}"
        "#d{left:170px;top:125px;width:90px;height:90px;background:linear-gradient(#fd2,#d24);transform:perspective(200px) rotateY(38deg) rotateX(12deg)}"
        "#e{left:120px;top:100px;width:70px;height:50px;background:#fc0;mix-blend-mode:multiply}"
        "</style><div id=bg></div><div id=a></div><div id=b></div><div id=c></div><div id=d></div><div id=e></div>"},
    {"svg", 320, 240, "<style>" PIXEL_FONT "body{margin:0} svg{display:block}</style><svg width=320 height=240 viewBox='0 0 160 120'>"
        "<path d='M10 60 C30 5 60 115 80 60 S130 5 150 60' fill='none' stroke='#c33' stroke-width='3' stroke-linecap='round'/>"
        "<path d='M20 100 A25 15 20 1 1 70 100 Z' fill='#39c' stroke='#134' stroke-width='1.5' stroke-linejoin='round'/>"
        "<circle cx='115' cy='90' r='20' fill='none' stroke='#2a5' stroke-width='4' stroke-dasharray='7 3 2 3'/>"
        "<g transform='rotate(12 80 25) scale(.8)' opacity='.7'><rect x='70' y='8' width='50' height='26' rx='7' fill='#e93'/></g>"
        "<polygon points='130,10 150,20 142,42 118,38' fill='#94c' fill-rule='evenodd'/></svg>"},
    {"layout", 320, 240, "<style>" PIXEL_FONT "body{margin:6px;font-size:12px} .g{display:grid;grid-template-columns:1fr 2fr 1fr;gap:4px}"
        ".g>div,.f>div{background:#dfe;border:1px solid #485;padding:3px} .f{display:flex;gap:5px;margin:6px 0;align-items:center;justify-content:space-between}"
        "table{border-collapse:collapse;width:100%} td{border:1px solid #669;padding:2px 5px;text-align:center} tr:nth-child(2n){background:#eef}"
        ".fl{float:left;width:60px;height:34px;background:#fca;margin-right:6px} ol{margin:4px 0}</style>"
        "<div class=g><div>One</div><div>Two is wider</div><div>Three</div><div style='grid-column:span 2'>Four</div><div>Five</div></div>"
        "<div class=f><div>Left</div><div style='flex:1'>Grows</div><div>Right<br>Two</div></div>"
        "<table><tr><td>a<td>b<td>c<tr><td colspan=2>wide<td>d</table>"
        "<div class=fl></div><p>Text beside a float goes around it and under it when the line is long enough to wrap.</p><ol><li>First<li>Second</ol>"},
    {"controls", 320, 240, "<style>" PIXEL_FONT "body{margin:8px;font-size:13px} input,button,select,progress,textarea{display:block;margin:5px 0}</style>"
        "<button>Press</button><input value='Some text'><input type=checkbox checked><input type=radio checked>"
        "<input type=range value=30><progress value=.6></progress><select><option>Choice</select><textarea rows=2>Two lines</textarea>"},
};

static const struct { const char* name; float scale; uint64_t hash; } kPixelHashes[] = {
#include "pixelsExpected.inc"
};

static uint64_t hashPixels(const CscdImage* img) {
    uint64_t h = 14695981039346656037ull; /* FNV-1a */
    for (int y = 0; y < img->height; ++y) {
        const uint32_t* row = cscdImageRow(img, y);
        for (int x = 0; x < img->width; ++x)
            for (int s = 0; s < 32; s += 8) h = (h ^ ((row[x] >> s) & 255)) * 1099511628211ull;
    }
    return h;
}

static uint64_t paintPixelPage(const PixelPage* page, float scale, int threads, const char* saveAs) {
    cscdSetPaintThreads(threads);
    CscdView* v = cscdViewNew();
    loadFixturePage(v, page->html, (float)page->width, (float)page->height, scale);
    CscdImage img;
    cscdImageInit(&img, (int)(page->width * scale), (int)(page->height * scale));
    cscdViewPaint(v, &img, false);
    const uint64_t h = hashPixels(&img);
    if (saveAs) cscdSavePNG(cscdSv(saveAs), &img);
    cscdImageFini(&img);
    cscdViewFree(v);
    return h;
}

/* What painting keeps: within its limits, and what is costly stays when a scroll makes many new ones. */
static void testPaintCache(void) {
    const CscdPaintCacheLimits was = cscdPaintCacheLimits();
    CscdImage kept, none;
    cscdImageInit(&kept, 700, 500);
    cscdImageInit(&none, 700, 500);
    CscdCanvas* c = cscdCanvasNew(&kept);
    const CscdColor black = cscdRgba(0, 0, 0, 160);
    const CscdRRect large = cscdRRect(cscdRect(150, 120, 400, 260));
    cscdPaintCacheClear();
    CscdPaintCacheLimits l = was;
    l.shadowCount = 64;
    cscdPaintCacheSetLimits(l);
    CHECK(cscdPaintCacheLimits().shadowCount == 64);
    const CscdPaintCacheStats start = cscdPaintCacheStats();
    CHECK(start.shadowCount == 0 && start.shadowBytes == 0);
    cscdCanvasDrawBoxShadow(c, &large, 0, 12, 40, 0, black, false);
    cscdCanvasDrawBoxShadow(c, &large, 0, 12, 40, 0, black, false);
    CscdPaintCacheStats s = cscdPaintCacheStats();
    CHECK(s.shadowMisses == start.shadowMisses + 1 && s.shadowHits == start.shadowHits + 1 && s.shadowCount == 1);
    /* a page scrolling smoothly: small shadows at new fractions of a pixel, the large one every frame */
    for (int i = 0; i < 2000; ++i) {
        const CscdRRect small = cscdRRect(cscdRect(20, 20 + (float)i * .013f, 60, 24));
        cscdCanvasDrawBoxShadow(c, &small, 0, 2, 3, 0, black, false);
        if (i % 30 == 0) cscdCanvasDrawBoxShadow(c, &large, 0, 12, 40, 0, black, false);
    }
    const CscdPaintCacheStats full = cscdPaintCacheStats();
    CHECK(full.shadowCount <= 64 && full.shadowEvictions > 0 && full.shadowBytes > 0);
    cscdCanvasDrawBoxShadow(c, &large, 0, 12, 40, 0, black, false);
    s = cscdPaintCacheStats();
    CHECK(s.shadowMisses == full.shadowMisses); /* still there */
    /* a small shadow with no blur is not kept: it has nothing costly */
    const CscdRRect sharp = cscdRRect(cscdRect(40, 40, 30, 20));
    cscdCanvasDrawBoxShadow(c, &sharp, 0, 2, 0, 0, black, false);
    CHECK(cscdPaintCacheStats().shadowCount == s.shadowCount);
    cscdCanvasFree(c);
    /* nothing kept at all: the same pixels */
    cscdImageFini(&kept);
    cscdImageInit(&kept, 700, 500);
    c = cscdCanvasNew(&kept);
    cscdCanvasDrawBoxShadow(c, &large, 0, 12, 40, 0, black, false);
    cscdCanvasFree(c);
    l.shadowBytes = 0;
    cscdPaintCacheSetLimits(l);
    CHECK(cscdPaintCacheStats().shadowCount == 0);
    c = cscdCanvasNew(&none);
    cscdCanvasDrawBoxShadow(c, &large, 0, 12, 40, 0, black, false);
    cscdCanvasFree(c);
    CHECK(cscdPaintCacheStats().shadowCount == 0);
    bool same = true;
    for (int y = 0; y < 500 && same; ++y) same = !memcmp(cscdImageRow(&kept, y), cscdImageRow(&none, y), 700 * 4);
    CHECK(same);
    /* the glyphs' limit has a floor; a lower one drops the oldest, never all of them */
    l = was;
    l.glyphCount = 1;
    cscdPaintCacheSetLimits(l);
    CHECK(cscdPaintCacheLimits().glyphCount == 256);
    cscdPaintCacheClear();
    CHECK(cscdPaintCacheStats().glyphCount == 0);
    cscdPaintCacheSetLimits(was);
    cscdImageFini(&kept);
    cscdImageFini(&none);
}

static void testPixels(void) {
    cscdAppSetResourceLoader(fixtures, NULL);
    const char* dir = getenv("CASCADE_PIXELS_DIR");
    static const float scales[] = {1, 1.5f, 2, 4};
    for (size_t p = 0; p < sizeof kPixelPages / sizeof kPixelPages[0]; ++p)
        for (size_t s = 0; s < sizeof scales / sizeof scales[0]; ++s) {
            char file[512] = "";
            if (dir) snprintf(file, sizeof file, "%s/%s-%g.png", dir, kPixelPages[p].name, scales[s]);
            const uint64_t h = paintPixelPage(&kPixelPages[p], scales[s], 1, dir ? file : NULL);
            uint64_t expected = 0;
            for (size_t i = 0; i < sizeof kPixelHashes / sizeof kPixelHashes[0]; ++i)
                if (kPixelHashes[i].name && !strcmp(kPixelHashes[i].name, kPixelPages[p].name) && kPixelHashes[i].scale == scales[s]) expected = kPixelHashes[i].hash;
            ++checks;
            if (h != expected) {
                ++failures;
                printf("FAIL pixels: {\"%s\", %gf, 0x%016llxull},\n", kPixelPages[p].name, scales[s], (unsigned long long)h);
            }
            /* painted by bands on several threads: the same */
            ++checks;
            if (paintPixelPage(&kPixelPages[p], scales[s], 4, NULL) != h) {
                ++failures;
                printf("FAIL pixels: %s at %g differs on four threads\n", kPixelPages[p].name, scales[s]);
            }
        }
    cscdSetPaintThreads(0);
    cscdAppSetResourceLoader(NULL, NULL);
}

/* ---- what stays behind when elements go ------------------------------------------------------------ */

static CscdView* goneView;
static int goneRuns;

static void removeTarget(void* user, CscdEvent* ev) { (void)user; cscdElementRemove(ev->target); }
static void clickOtherThenRemove(void* user, CscdEvent* ev) {
    cscdElementClick((CscdElement*)user); /* (a click inside a click) */
    cscdElementRemove(ev->target);
}
static void countRuns(void* user, CscdEvent* ev) { (void)user; (void)ev; ++goneRuns; }
static void replacePage(void* user, CscdEvent* ev) {
    (void)user; (void)ev;
    cscdViewLoadHTML(goneView, CSCD_SV("<p id=new>new</p>"), CSCD_SV("."));
}

static void paintOnce(CscdView* v) {
    CscdImage img;
    cscdViewUpdate(v);
    cscdImageInit(&img, 200, 120);
    cscdViewPaint(v, &img, false);
    cscdImageFini(&img);
}

static void clickAt(CscdView* v, float x, float y) {
    const CscdModifiers none = {0};
    cscdViewMouseDown(v, x, y, 0, none, 1);
    cscdViewMouseUp(v, x, y, 0, none);
}

static void testElementsGone(void) {
    CscdView* v = cscdViewNew();
    goneView = v;
    const CscdModifiers none = {0};
    cscdViewOnWarning(v, NULL, NULL);
    cscdViewSetViewport(v, 200, 120, 1);

    /* a pointer event that comes before the next paint does not find what left the page */
    cscdViewLoadHTML(v, CSCD_SV("<style>body{margin:0}div{height:40px}</style><div id=a></div><div id=b></div>"), CSCD_SV("."));
    paintOnce(v);
    CscdElement* a = byId(v, "a");
    CHECK(cscdViewElementAt(v, 5, 5) == a);
    cscdElementRemove(a);
    CHECK(cscdViewElementAt(v, 5, 5) != a);
    cscdViewMouseMove(v, 5, 5, 0, none);
    cscdViewTouchDown(v, 1, 5, 5);
    cscdViewTouchUp(v, 1, 5, 5);
    paintOnce(v);
    CHECK(cscdViewElementAt(v, 5, 5) == byId(v, "b")); /* (the page moved up) */

    /* a click on an element that clicks another and removes itself: no default action on it */
    cscdViewLoadHTML(v, CSCD_SV("<style>body{margin:0}button{display:block;height:40px;width:100px}</style>"
                                "<form><button id=a></button></form><button id=b type=button></button>"), CSCD_SV("."));
    paintOnce(v);
    cscdElementOn(byId(v, "b"), CSCD_SV("click"), countRuns, NULL);
    cscdElementOn(byId(v, "a"), CSCD_SV("click"), clickOtherThenRemove, byId(v, "b"));
    goneRuns = 0;
    clickAt(v, 5, 5);
    CHECK(goneRuns == 1 && !byId(v, "a"));

    /* a listener replaces the page it is called from */
    cscdViewLoadHTML(v, CSCD_SV("<style>body{margin:0}div{height:40px}</style><section><div id=a></div></section>"), CSCD_SV("."));
    paintOnce(v);
    cscdElementOn(byId(v, "a"), CSCD_SV("click"), replacePage, NULL);
    cscdElementOn(query(v, "section"), CSCD_SV("click"), countRuns, NULL);
    goneRuns = 0;
    clickAt(v, 5, 5);
    CHECK(byId(v, "new") && goneRuns == 0);
    paintOnce(v);

    /* what the inspector and screen readers held of the page before */
    cscdViewLoadHTML(v, CSCD_SV("<button id=a>a</button>"), CSCD_SV("."));
    paintOnce(v);
    cscdViewInspect(v, byId(v, "a"));
    const uint32_t id = cscdAccessibleId(v, byId(v, "a"));
    CHECK(cscdAccessibleElement(v, id) == byId(v, "a"));
    cscdViewLoadHTML(v, CSCD_SV("<p>another page</p>"), CSCD_SV("."));
    CHECK(cscdAccessibleElement(v, id) == NULL);
    paintOnce(v);
    cscdViewInspect(v, NULL);

    /* an anchor inside the menu that closes to make room */
    CscdMenu m = {0}, next = {0};
    cscdMenuAdd(&m, cscdMenuItem(CSCD_SV("a"), NULL, NULL, CSCD_SV("")));
    cscdMenuAdd(&next, cscdMenuItem(CSCD_SV("b"), NULL, NULL, CSCD_SV("")));
    cscdViewShowMenu(v, m, 5, 5, 0);
    paintOnce(v);
    CscdElement* row = query(v, ".cscd-menu-item");
    CHECK(row != NULL);
    cscdViewShowMenuAt(v, row, next);
    CHECK(!cscdViewMenuOpen(v));
    CscdMenu third = {0};
    cscdMenuAdd(&third, cscdMenuItem(CSCD_SV("c"), NULL, NULL, CSCD_SV("")));
    cscdViewShowMenu(v, third, 5, 5, 0);
    paintOnce(v);
    cscdViewShowPopover(v, query(v, ".cscd-menu-item"), NULL);

    /* a box whose scroll is on its way leaves the page */
    cscdViewLoadHTML(v, CSCD_SV("<style>body{margin:0}#s{height:60px;overflow:auto}#s div{height:400px}</style><div id=s><div></div></div>"), CSCD_SV("."));
    paintOnce(v);
    cscdElementOn(byId(v, "s"), CSCD_SV("scroll"), removeTarget, NULL);
    cscdViewWheel(v, 5, 5, 0, 60, none);
    for (int i = 0; i < 4; ++i) { sleepMs(20); cscdViewUpdate(v); }
    CHECK(!byId(v, "s"));
    cscdViewFree(v);
}

/* ---- what a page or a font can ask for ------------------------------------------------------------- */

static uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t* p) { return be16(p) << 16 | be16(p + 2); }

/* Selectors that must look further after a failure, and ones that must stop. */
static void testSelectorSearch(void) {
    CscdView* v = cscdViewNew();
    cscdViewOnWarning(v, NULL, NULL);
    cscdViewLoadHTML(v, CSCD_SV("<a><b><b><c id=q></c></b></b></a>"
                                "<x><p><y></y><u></u><z id=s></z></p></x>"
                                "<k><m><n></n><o><r id=t></r></o></m></k>"
                                "<g><h></h><i></i><j><l id=w></l></j></g>"), CSCD_SV("."));
    static const struct { const char* selector; const char* id; bool yes; } cases[] = {
        {"a > b c", "q", true},     /* (the nearer b's parent is no a: the farther b's is) */
        {"a > b > c", "q", false},
        {"a b b c", "q", true},
        {"a b b b c", "q", false},
        {"x y ~ z", "s", true},     /* (a sibling further back, then an ancestor) */
        {"x u + z", "s", true},
        {"x y + z", "s", false},
        {"q y ~ z", "s", false},
        {"p > y ~ z", "s", true},
        {"k > m n + o r", "t", true},
        {"k n + o > r", "t", true},
        {"m > n ~ o r", "t", true},
        {"g h ~ j l", "w", true},   /* (a sibling of an ancestor) */
        {"g h + j l", "w", false},
        {"g h + i + j > l", "w", true},
        {"g > h ~ i ~ j l", "w", true},
        {"z h ~ j l", "w", false},
    };
    for (size_t i = 0; i < CSCD_COUNT(cases); ++i) {
        const bool got = cscdElementMatches(byId(v, cases[i].id), cscdSv(cases[i].selector));
        if (got != cases[i].yes) printf("selector: %s\n", cases[i].selector);
        CHECK(got == cases[i].yes);
    }
    cscdViewFree(v);
}

/* Attributes in the numbers no tag has: the same answers, and no time for each one against all. */
static void testManyAttributes(void) {
    CscdStr html = {0};
    cscdStrAppendC(&html, "<html a=1 b=2><body id=b class=first");
    for (int i = 0; i < 40; ++i) { char a[32]; snprintf(a, sizeof a, " x%d=%d x%d=dup", i, i, i); cscdStrAppendC(&html, a); }
    cscdStrAppendC(&html, "><body class=second z=1 id=other>");
    CscdView* v = cscdViewNew();
    cscdViewLoadHTML(v, cscdStrSv(&html), CSCD_SV("."));
    CscdElement* body = cscdDocumentBody(cscdViewDocument(v));
    CHECK(eq(keep(cscdStrDup(cscdElementAttrC(body, "x39"))), "39"));
    CHECK(eq(keep(cscdStrDup(cscdElementAttrC(body, "id"))), "b")); /* (the first says, and a second adds) */
    CHECK(eq(keep(cscdStrDup(cscdElementAttrC(body, "z"))), "1"));
    CHECK(cscdElementHasClass(body, CSCD_SV("first")) && !cscdElementHasClass(body, CSCD_SV("second")));
    CHECK(body->attributes.len == 43);
    cscdViewFree(v);
    cscdStrFree(&html);
}

static void testHostileRequests(void) {
    CscdView* v = cscdViewNew();
    cscdViewOnWarning(v, NULL, NULL);
    cscdViewSetViewport(v, 200, 120, 1);
    /* counter() that names no counter */
    cscdViewLoadHTML(v, CSCD_SV("<style>a::before{content:counter(,) counters(,,,)}</style><a>x</a>"), CSCD_SV("."));
    paintOnce(v);
    CHECK(eq(textOf(query(v, "a")), "x"));
    /* numbers beyond any int, the same everywhere */
    CHECK(cscdIntOf(1e30f) == 1 << 28 && cscdIntOf(-1e30f) == -(1 << 28) && cscdIntOf(NAN) == 0 && cscdIntOf(-2.f) == -2);
    CHECK(cscdIntOfDouble(1e30) == 2147483647 && cscdIntOfDouble(-1e30) == -2147483647 - 1 && cscdIntOfDouble(NAN) == 0);
    cscdViewLoadHTML(v, CSCD_SV("<div style='z-index:1e30;order:-1e30;width:1e30px;box-shadow:0 0 1e30px red'>x</div>"
                                "<style>a{counter-reset:x 1e30}</style><a>x</a>"), CSCD_SV("."));
    paintOnce(v);
    /* roman numerals stop at 3999: past it a counter is written in decimal, not in millions of Ms */
    cscdViewLoadHTML(v, CSCD_SV("<style>#a{counter-reset:x 4000}#b{counter-reset:x 3999}#c{counter-reset:x 1e30}"
                                "a::before{content:counter(x,upper-roman)}</style><a id=a></a><a id=b></a><a id=c></a>"), CSCD_SV("."));
    cscdViewUpdate(v);
    CHECK(eq(generated(v, "a"), "4000"));
    CHECK(eq(generated(v, "b"), "MMMCMXCIX"));
    CHECK(eq(generated(v, "c"), "2147483647"));
    /* dashes finer than anything that shows, along lines as long as float reaches (these never ended) */
    static const char* const dashed[] = {
        "<svg width=99 height=99><line x2=1e30 y2=1e30 stroke=red stroke-dasharray=1 /></svg>",
        "<svg width=99 height=99><line x2=100000 stroke=red stroke-dasharray=0.00001 /></svg>",
        "<svg width=99 height=99><line x2=99 stroke=red stroke-dasharray='-1 2 -3 5' /></svg>",
        "<svg width=99 height=99><line x2=1e39 stroke=red stroke-dasharray='1e39 1e39' /></svg>",
    };
    for (size_t i = 0; i < CSCD_COUNT(dashed); ++i) {
        cscdViewLoadHTML(v, cscdSv(dashed[i]), CSCD_SV("."));
        paintOnce(v);
    }
    cscdViewFree(v);

    /* extension subtables that point at themselves: shaping comes back, with nothing substituted */
    uint8_t* font = (uint8_t*)cscdMalloc(sizeof kTestFont);
    memcpy(font, kTestFont, sizeof kTestFont);
    uint32_t gsub = 0;
    for (uint32_t i = 0; i < be16(font + 4); ++i)
        if (!memcmp(font + 12 + 16 * i, "GSUB", 4)) gsub = be32(font + 12 + 16 * i + 8);
    CHECK(gsub != 0);
    const uint32_t list = gsub + be16(font + gsub + 8);
    for (uint32_t i = 0; i < be16(font + list); ++i) {
        const uint32_t lookup = list + be16(font + list + 2 + 2 * i);
        uint8_t* sub = font + lookup + be16(font + lookup + 6);
        font[lookup] = 0;
        font[lookup + 1] = 7;
        memcpy(sub, "\0\1\0\7\0\0\0\0", 8); /* format 1, of type 7, 0 bytes on */
    }
    CscdFontFace* f = cscdFontFaceLoad(NULL, font, sizeof kTestFont, 0);
    CHECK(f != NULL);
    if (f) {
        CscdFont use = cscdFontDefault();
        use.face = f;
        CscdShapedGlyphVec out = {0};
        cscdFontShape(CSCD_SV("fi"), &use, 0, 0, &out);
        CHECK(out.len == 2);
        cscdVecFree(&out);
        cscdFontFaceFree(f);
    }
    cscdFree(font);
}

int main(void) {
    testParser();
    testSelectors();
    testCascade();
    testBlockLayout();
    testFlex();
    testGrid();
    testInline();
    testEvents();
    testTransition();
    testFeatures();
    testTemplatesAndLists();
    testInspector();
    testAccessibility();
    testArabicJoining();
    testImageCodec();
    testBrokenImages();
    testAnimatedImages();
    testOpenType();
    testBidiFields();
    testMoreCss();
    testBinding();
    testMenus();
    testDragDrop();
    testTouch();
    testTouchSelection();
    testProgressRepaint();
    testTouchSlider();
    testTouchAction();
    testSplitter();
    testTextEditing();
    testDialogs();
    testKeyboardAndBaseSheet();
    testAppImages();
    testPopovers();
    testClips();
    testPartialRepaint();
    testTransparentView();
    testTextRepaint();
    testStateRepaint();
    testScaledImage();
    testStyleSharing();
    testAppFiles();
    testResourceLoader();
    testParallelLoops();
    testApi();
    test3DTransforms();
    testContainerQueries();
    testResize();
    testDiagnostics();
    testSubgrid();
    testMoreImageFormats();
    testCounters();
    testBackdropAndMask();
    testSmallerProperties();
    testBidi();
    testSiblingPositions();
    testStaticPositionRelayout();
    testTablePercentColumns();
    testLimits();
    testPhoneInput();
    testJpeg();
    testCffFont();
    testBitmapFonts();
    testSvg();
    testBlur();
    testKeyframes();
    testTextLines();
    testPaintCache();
    testPixels();
    testElementsGone();
    testSelectorSearch();
    testManyAttributes();
    testHostileRequests();
    for (int i = 0; i < 16; ++i) cscdStrFree(&kept[i]);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
