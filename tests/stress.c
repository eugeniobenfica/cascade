/* cascade-stress: random changes to a page and random input, with listeners that remove elements,
 * rebuild panels and replace the page while their events travel. It checks nothing itself: it is
 * here for AddressSanitizer (CASCADE_SANITIZE), which sees what a change left dangling.
 *   cascade-stress [first seed] [seeds] [rounds] */
#include "cascade/cascade.h"
#include <stdio.h>
#include <stdlib.h>
static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 20); }
static CscdView* V;
static int loadInHandler;
static const char* const kHtml =
 "<style>div{padding:4px} .a{display:flex} .b{display:grid} .c{position:absolute;left:9px} .d{transition:width .1s} .e{overflow:auto;height:40px}"
 " dialog{width:90px} [popover]{width:50px} .f::before{content:'x'} :hover{outline:1px solid} :focus{color:red} .g{animation:k .1s infinite}@keyframes k{to{opacity:.5}}</style>"
 "<div id=r><div class=a><button id=b1>one</button><input id=i1 value=hello><textarea id=t1>abc\ndef</textarea></div>"
 "<div class=b><select id=s1><option>1<option>2</select><input type=checkbox id=c1><input type=range id=g1></div>"
 "<div class=e><p>text <b>bold</b> <a href=#x>link</a></p><p>more</p><p>more</p><p>more</p><p>more</p></div>"
 "<dialog id=d1><form method=dialog><button value=ok>ok</button></form></dialog><div id=p1 popover>pop</div>"
 "<details><summary>s</summary>x</details><table><tr><td>1<td>2</table><div draggable=true id=dr>drag</div><div contenteditable id=ce>edit</div>"
 "<ul><li>a<li>b<li>c</ul><div id=lst class=e></div><template id=tp><div>{{x}}</div></template></div>";

static void pick(CscdPtrVec* all) {
    all->len = 0;
    cscdViewQuerySelectorAll(V, CSCD_SV("body *"), all);
}
static void mutate(int depth);
static void onEvent(void* user, CscdEvent* ev) {
    (void)user;
    static int nested;
    if (nested > 2) return;
    ++nested;
    switch (rnd() % 12) {
    case 0: if (ev->target) cscdElementRemove(ev->target); break;
    case 1: if (ev->target) cscdElementSetInnerHTML(ev->target, CSCD_SV("<i>new</i><input value=q>")); break;
    case 2: if (ev->target && ev->target->node.parent && cscdNodeIsElement(ev->target->node.parent)) cscdElementSetText((CscdElement*)ev->target->node.parent, CSCD_SV("gone")); break;
    case 3: cscdEventStopPropagation(ev); break;
    case 4: cscdEventPreventDefault(ev); break;
    case 5: mutate(1); break;
    case 6: if (loadInHandler) cscdViewLoadHTML(V, cscdSv(kHtml), CSCD_SV(".")); break;
    case 7: cscdViewBlur(V); break;
    case 8: { CscdElement* b = cscdViewQuerySelector(V, CSCD_SV("body")); if (b) cscdElementClick(b); break; }
    case 9: cscdViewCloseMenus(V); break;
    default: break;
    }
    --nested;
}
static const char* const kEvents[] = {"click","mousedown","mouseup","mousemove","mouseenter","mouseleave","keydown","keyup","input","change","focus","blur","focusin","focusout","wheel","scroll","contextmenu","dragstart","drop","close","cancel","toggle","touchstart","touchend","dblclick","select"};
static void listen(CscdElement* e) { for (size_t i = 0; i < sizeof kEvents / sizeof *kEvents; ++i) if (rnd() % 3 == 0) cscdElementOn(e, cscdSv(kEvents[i]), onEvent, NULL); }
static void timerFn(void* u) { (void)u; mutate(1); }
static void fill(void* u, CscdElement* row, size_t i) { (void)u; char b[32]; snprintf(b, 32, "%zu", i); cscdElementFill(row, CSCD_SV("x"), cscdSv(b)); }
static void menuPick(void* u) { (void)u; mutate(1); }
static void mutate(int depth) {
    CscdPtrVec all = {0};
    pick(&all);
    if (!all.len) { cscdVecFree(&all); cscdViewLoadHTML(V, cscdSv(kHtml), CSCD_SV(".")); return; }
    CscdElement* e = all.items[rnd() % all.len];
    CscdElement* o = all.items[rnd() % all.len];
    CscdStr h;
    switch (rnd() % 25) {
    case 0: cscdElementRemove(e); break;
    case 1: cscdElementSetInnerHTML(e, CSCD_SV("<div class=a><input value=z><b>x</b></div>")); break;
    case 2: cscdElementSetText(e, CSCD_SV("text \xD8\xB3\xD9\x84")); break;
    case 3: cscdElementAddClass(e, CSCD_SV("c")); break;
    case 4: cscdElementToggleClass(e, CSCD_SV("g")); break;
    case 5: cscdElementSetStyle(e, CSCD_SV("width"), CSCD_SV("33px")); break;
    case 6: cscdElementSetAttribute(e, CSCD_SV("hidden"), CSCD_SV("")); break;
    case 7: cscdElementRemoveAttribute(e, CSCD_SV("hidden")); break;
    case 8: { /* move */
        CscdNode* n; bool loop = false;
        for (n = &o->node; n; n = n->parent) if (n == &e->node) loop = true;
        if (!loop && e->node.parent) { CscdNode* c = cscdNodeRemoveChild(e->node.parent, &e->node); if (c) cscdNodeAppendChild(&o->node, c); }
        break; }
    case 9: { CscdElement* c = cscdElementClone(e, true); if (c) cscdNodeAppendChild(&o->node, &c->node); break; }
    case 10: cscdViewFocus(V, e); break;
    case 11: listen(e); break;
    case 12: cscdElementRemoveEventListeners(e, CSCD_SV("click")); break;
    case 13: cscdViewShowModal(V, e); break;
    case 14: cscdViewCloseDialog(V, e, CSCD_SV("x")); break;
    case 15: cscdViewShowPopover(V, e, o); break;
    case 16: cscdViewHidePopover(V, e); break;
    case 17: if (depth < 2) cscdViewSetTimeout(V, cscdAction(timerFn, NULL), 0); break;
    case 18: cscdViewScrollIntoView(V, e); break;
    case 19: h = cscdElementInnerHTML(e); cscdStrFree(&h); break;
    case 20: cscdElementSetValue(e, CSCD_SV("val")); break;
    case 21: cscdViewInspect(V, rnd() % 2 ? e : NULL); break;
    case 22: { CscdMenu m = {0}; cscdMenuAdd(&m, cscdMenuItem(CSCD_SV("a"), menuPick, NULL, CSCD_SV(""))); cscdMenuAdd(&m, cscdMenuSeparator()); cscdViewShowMenuAt(V, e, m); break; }
    case 23: { CscdElement* tp = cscdViewQuerySelector(V, CSCD_SV("#tp")); CscdElement* l = cscdViewQuerySelector(V, CSCD_SV("#lst"));
        if (tp && l && cscdNodeFirstElementChild(&tp->node)) cscdViewBindList(V, l, cscdNodeFirstElementChild(&tp->node), 1000, 18, fill, NULL, NULL); break; }
    default: { CscdElement* l = cscdViewQuerySelector(V, CSCD_SV("#lst")); if (l) cscdViewListSetCount(V, l, rnd() % 50); break; }
    }
    cscdVecFree(&all);
}
static void input(CscdImage* img) {
    const CscdModifiers none = {0}, ctrl = {.ctrl = true}, shift = {.shift = true};
    const float x = (float)(rnd() % 400), y = (float)(rnd() % 300);
    switch (rnd() % 18) {
    case 0: cscdViewMouseMove(V, x, y, 0, none); break;
    case 1: cscdViewMouseDown(V, x, y, 0, none, 1); cscdViewMouseUp(V, x, y, 0, none); break;
    case 2: cscdViewMouseDown(V, x, y, 0, none, 2); cscdViewMouseMove(V, x + 40, y, 1, none); cscdViewMouseUp(V, x + 40, y, 0, none); break;
    case 3: cscdViewMouseDown(V, x, y, 2, none, 1); cscdViewMouseUp(V, x, y, 2, none); break;
    case 4: cscdViewWheel(V, x, y, 0, 60, none); break;
    case 5: cscdViewKeyDown(V, CSCD_KEY_TAB, rnd() % 2 ? shift : none, CSCD_SV("Tab")); break;
    case 6: cscdViewTextInput(V, CSCD_SV("ab")); break;
    case 7: cscdViewKeyDown(V, CSCD_KEY_BACKSPACE, none, CSCD_SV("Backspace")); break;
    case 8: cscdViewKeyDown(V, CSCD_KEY_Z, ctrl, CSCD_SV("z")); break;
    case 9: cscdViewKeyDown(V, CSCD_KEY_ENTER, none, CSCD_SV("Enter")); break;
    case 10: cscdViewKeyDown(V, CSCD_KEY_ESCAPE, none, CSCD_SV("Escape")); break;
    case 11: cscdViewTouchDown(V, 1, x, y); cscdViewTouchMove(V, 1, x, y + 50); cscdViewTouchUp(V, 1, x, y + 50); break;
    case 12: cscdViewTouchDown(V, 1, x, y); cscdViewTouchUp(V, 1, x, y); cscdViewTouchDown(V, 1, x, y); cscdViewTouchUp(V, 1, x, y); break;
    case 13: cscdViewSetComposition(V, CSCD_SV("ka")); break;
    case 14: cscdViewEditAction(V, (CscdEditAction)(rnd() % 4)); break;
    case 15: cscdViewMouseLeave(V); break;
    case 16: cscdViewUpdate(V); cscdViewPaint(V, img, rnd() % 2); break;
    default: { CscdTextState t = {0}; if (cscdViewGetTextState(V, &t)) { t.selStart = rnd() % 9; cscdViewSetTextState(V, &t); cscdTextStateFini(&t); } break; }
    }
}
static void run(uint64_t seed, int rounds, bool replaceInListeners) {
    rs = 88172645463325252ull ^ (seed * 0x9E3779B97F4A7C15ull);
    loadInHandler = replaceInListeners;
    V = cscdViewNew();
    cscdViewOnWarning(V, NULL, NULL);
    cscdViewSetViewport(V, 400, 300, 1);
    cscdViewLoadHTML(V, cscdSv(kHtml), CSCD_SV("."));
    CscdImage img;
    cscdImageInit(&img, 400, 300);
    cscdViewUpdate(V);
    CscdPtrVec all = {0};
    pick(&all);
    for (size_t i = 0; i < all.len; ++i) listen(all.items[i]);
    cscdVecFree(&all);
    for (int i = 0; i < rounds; ++i) {
        if (rnd() % 3 == 0) mutate(0); else input(&img);
        if (i % 7 == 0) { cscdViewUpdate(V); cscdViewPaint(V, &img, false); }
        if (i % 500 == 499) cscdViewLoadHTML(V, cscdSv(kHtml), CSCD_SV("."));
    }
    cscdImageFini(&img);
    cscdViewFree(V);
}

int main(int argc, char** argv) {
    const int first = argc > 1 ? atoi(argv[1]) : 1, seeds = argc > 2 ? atoi(argv[2]) : 8, rounds = argc > 3 ? atoi(argv[3]) : 1500;
    for (int s = first; s < first + seeds; ++s) run((uint64_t)s, rounds, s % 2 == 0);
    printf("%d seeds of %d rounds\n", seeds, rounds);
    return 0;
}
