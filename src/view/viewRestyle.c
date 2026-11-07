/* The pipeline before painting: styles computed again (all of them, or in place for state
 * changes), what changed found and laid out again (a text's block, a box in place, or the whole page),
 * and what must be painted again. */
#include "style/styleInternal.h"
#include "view/viewInternal.h"
#include "core/thread.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- pipeline ------------------------------------------------------------------------------------ */

static CscdStyleEnvironment environment(CscdView* v) {
    CscdStyleEnvironment env;
    memset(&env, 0, sizeof env);
    env.media = viewMediaOf(v);
    env.rootFontSize = 16 * env.media.textScale;
    env.loadImage = viewImageLoader;
    env.loadImageUser = v;
    return env;
}

/* the styles before a viewRestyle (for transitions): element -> a reference */
static void snapStyles(CscdNode* n, CscdU64Map* before) {
    CscdElement* e = cscdNodeAsElement(n);
    if (e && e->style) {
        bool inserted = false;
        CscdMapVal* val = cscdU64MapPut(before, CSCD_PKEY(e), &inserted);
        if (inserted) val->p = cscdRetain(e->style);
    }
    for (size_t i = 0; i < n->children.len; ++i) snapStyles(cscdNodeChild(n, i), before);
}

static void releaseSnap(CscdU64Map* before) {
    for (CscdU64MapSlot* s = NULL; (s = cscdU64MapNext(before, s));) cscdRelease(s->val.p);
    cscdU64MapFree(before);
}

static bool animationRunning(const CscdView* v, const CscdElement* e, const CscdStr* name) {
    for (size_t i = 0; i < v->animations.len; ++i)
        if (v->animations.items[i].e == e && cscdStrEq(&v->animations.items[i].a.name, name)) return true;
    return false;
}

static void startAnimation(CscdView* v, CscdElement* e, const CscdAnimation* a, double t) {
    if (!viewOn(v, CSCD_FEATURE_ANIMATIONS)) {
        /* without animations: one that ends shows its end (started that long ago), one that never does
         * is not run */
        if (a->iterations < 0) return;
        t -= a->delay + a->duration * a->iterations;
    }
    RunningAnimation r;
    r.e = e;
    r.a = *a;
    r.a.name = cscdStrCopy(&a->name);
    r.start = t;
    r.base = cscdStyleClone(e->style);
    cscdVecPush(&v->animations, r);
}

static bool isMoving(const CscdView* v, const CscdElement* e) {
    for (size_t i = 0; i < v->transitions.len; ++i) if (v->transitions.items[i].e == e) return true;
    for (size_t i = 0; i < v->animations.len; ++i) if (v->animations.items[i].e == e) return true;
    return false;
}

static void diffStyles(CscdView* v, CscdNode* n, const CscdU64Map* before, double t) {
    CscdElement* e = cscdNodeAsElement(n);
    if (e) {
        /* transitions and animations change the style in place: an element that has them gets a
         * style of its own, not one it shares with others */
        if (e->style && (e->style->transitions.len || e->style->animations.len || isMoving(v, e)) && cscdStyleEngineIsShared(v->styles, e->style)) {
            CscdComputedStyle* own = cscdStyleClone(e->style);
            cscdRelease(e->style);
            e->style = own;
        }
        const CscdMapVal* was = cscdU64MapGet(before, CSCD_PKEY(e));
        if (was && e->style) {
            const CscdComputedStyle* old = (const CscdComputedStyle*)was->p;
            if (e->style->transitions.len || v->transitions.len) viewStartTransitions(v, e, old, e->style);
            /* animations that appeared */
            for (size_t i = 0; i < e->style->animations.len; ++i) {
                const CscdAnimation* a = &e->style->animations.items[i];
                if (!animationRunning(v, e, &a->name)) startAnimation(v, e, a, t);
            }
        } else if (e->style) {
            for (size_t i = 0; i < e->style->animations.len; ++i) {
                const CscdAnimation* a = &e->style->animations.items[i];
                if (!animationRunning(v, e, &a->name)) startAnimation(v, e, a, t);
            }
        }
        /* animations that went away */
        for (size_t i = v->animations.len; i-- > 0;) {
            RunningAnimation* r = &v->animations.items[i];
            if (r->e != e || !e->style) continue;
            bool kept = false;
            for (size_t k = 0; k < e->style->animations.len && !kept; ++k) kept = cscdStrEq(&e->style->animations.items[k].name, &r->a.name);
            if (!kept) viewRemoveAnimation(v, i);
            else if (was && was->p != e->style) { /* (computed again: what it animates from is this) */
                cscdRelease(r->base);
                r->base = cscdStyleClone(e->style);
            }
        }
    }
    for (size_t i = 0; i < n->children.len; ++i) diffStyles(v, cscdNodeChild(n, i), before, t);
}

/* roots: just their subtrees (false: it took the whole document) */
static bool restyleImpl(CscdView* v, CscdPtrVec* roots);

bool viewRestyle(CscdView* v, CscdPtrVec* roots) {
    const double t0 = cscdNowSeconds();
    uint64_t computed0, reused0, computed1, reused1;
    cscdStyleEngineCounts(v->styles, &computed0, &reused0);
    const bool r = restyleImpl(v, roots);
    cscdStyleEngineCounts(v->styles, &computed1, &reused1);
    v->stats.styleMs = (cscdNowSeconds() - t0) * 1000;
    v->stats.stylesComputed = (int)(computed1 - computed0);
    v->stats.stylesReused = (int)(reused1 - reused0);
    ++v->stats.styles;
    return r;
}

static bool restyleImpl(CscdView* v, CscdPtrVec* roots) {
    CscdStyleEnvironment env = environment(v);
    CscdU64Map before = {0};
    if (roots) {
        for (size_t i = 0; i < roots->len; ++i) snapStyles(&((CscdElement*)roots->items[i])->node, &before);
        for (size_t i = 0; i < roots->len; ++i)
            if (!cscdStyleEngineComputeSubtree(v->styles, (CscdElement*)roots->items[i], &env)) { /* the whole document after all */
                roots = NULL;
                releaseSnap(&before);
                memset(&before, 0, sizeof before);
                snapStyles(&v->doc->node, &before);
                cscdStyleEngineComputeAll(v->styles, v->doc, &env);
                break;
            }
    } else {
        snapStyles(&v->doc->node, &before);
        cscdStyleEngineComputeAll(v->styles, v->doc, &env);
    }
    double t = viewNow(v);
    if (roots) for (size_t i = 0; i < roots->len; ++i) diffStyles(v, &((CscdElement*)roots->items[i])->node, &before, t);
    else diffStyles(v, &v->doc->node, &before, t);
    releaseSnap(&before);
    viewTickAnimations(v, t);
    v->layoutDirty = true;
    if (roots) return true;
    v->styledVersion = v->doc->version;
    if (v->doc->structural) viewA11yStructure(v);
    v->doc->textChanged.len = 0;
    v->doc->paintChanged.len = 0;
    v->doc->structural = false;
    v->stateChanged.len = 0;
    v->styleDirty = false;
    return false;
}

void viewRestyleAll(CscdView* v) { viewRestyle(v, NULL); }

/* Where each element's boxes are (layout coordinates): element -> index in rects. */
typedef struct BoxRects { CscdU64Map at; CscdRectVec rects; } BoxRects;

static CscdRect* boxRectSlot(BoxRects* m, const CscdElement* e) {
    bool inserted = false;
    CscdMapVal* v = cscdU64MapPut(&m->at, CSCD_PKEY(e), &inserted);
    if (inserted) {
        v->u = m->rects.len;
        CscdRect z = {0, 0, 0, 0};
        cscdVecPush(&m->rects, z);
    }
    return &m->rects.items[v->u];
}

static void boxRects(const CscdLayoutBox* b, BoxRects* m) {
    if (b->element && !b->anonymous && !b->pseudo.len) {
        CscdRect r = cscdBoxBorderBox(b);
        for (size_t i = 0; i < b->inlineFrags.len; ++i) r = cscdRectUnite(r, b->inlineFrags.items[i].rect);
        CscdRect* known = boxRectSlot(m, b->element);
        *known = cscdRectUnite(*known, r);
    }
    for (size_t i = 0; i < b->children.len; ++i) boxRects(cscdBoxChild(b, i), m);
}

static void boxRectsFree(BoxRects* m) {
    cscdU64MapFree(&m->at);
    cscdVecFree(&m->rects);
}

/* The block a changed text lies in, with the ink of all its text (layout coordinates). */
static const CscdLayoutBox* textBlock(const CscdElement* e) {
    const CscdLayoutBox* b = e->layout;
    while (b && (b->kind == CSCD_BOX_INLINE || b->kind == CSCD_BOX_TEXT) && b->parent) b = b->parent;
    return b;
}

static CscdRect textInk(const CscdLayoutBox* b) {
    CscdRect r = cscdBoxBorderBox(b);
    for (size_t i = 0; i < b->textFrags.len; ++i) {
        const CscdTextFrag* f = &b->textFrags.items[i];
        r = cscdRectUnite(r, cscdRect(f->x, f->top, f->width, f->height));
    }
    for (size_t i = 0; i < b->inlineFrags.len; ++i) r = cscdRectUnite(r, b->inlineFrags.items[i].rect);
    const CscdComputedStyle* s = b->style;
    if (s) {
        /* what is drawn outside the box: its outline and its shadows */
        float out = s->outlineStyle != CSCD_BORDER_NONE ? cscdMaxf(0.f, s->outlineWidth + s->outlineOffset) : 0;
        for (size_t i = 0; i < s->boxShadow.len; ++i) {
            const CscdShadow* sh = &s->boxShadow.items[i];
            if (!sh->inset) out = cscdMaxf(out, cscdMaxf(fabsf(sh->x), fabsf(sh->y)) + sh->blur + cscdMaxf(0.f, sh->spread));
        }
        if (out > 0) r = cscdRectUnite(r, cscdRect(r.x - out, r.y - out, r.w + 2 * out, r.h + 2 * out));
    }
    for (size_t i = 0; i < b->children.len; ++i) r = cscdRectUnite(r, textInk(cscdBoxChild(b, i)));
    return r;
}

/* what a box and its contents draw, in viewport px */
static CscdRect drawnArea(CscdView* v, const CscdLayoutBox* b) {
    /* from layout coordinates to the viewport: the page's and the scrolled boxes' offsets */
    CscdPoint d = {-v->scrollX, -v->scrollY};
    for (CscdLayoutBox* p = b->parent; p; p = p->parent)
        if (p->style && cscdStyleClipsOverflow(p->style) && p->element && p->parent) { d.x -= p->element->scrollLeft; d.y -= p->element->scrollTop; }
    if (b->style && b->style->position == CSCD_POS_FIXED) d = cscdPoint(0, 0);
    const CscdRect r = textInk(b); /* its box, its outline and shadows, the text in it */
    return cscdRect(r.x + d.x - 2, r.y + d.y - 2, r.w + 4, r.h + 4);
}

static void noteDrawn(CscdView* v, const CscdLayoutBox* b, BoxRects* wasAt) {
    if (b->element && !b->anonymous && !b->pseudo.len) {
        CscdRect* w = boxRectSlot(wasAt, b->element);
        *w = cscdRectUnite(*w, drawnArea(v, b));
    }
    for (size_t i = 0; i < b->children.len; ++i) noteDrawn(v, cscdBoxChild(b, i), wasAt);
}

static CscdRect areaOf(CscdView* v, const CscdElement* e) { return e->layout ? drawnArea(v, e->layout) : cscdRect(0, 0, 0, 0); }

/* lays out; repaints just there when nothing moved */
static void relayoutAround(CscdView* v, const CscdPtrVec* blocks) {
    CscdRect changed = {0, 0, 0, 0};
    for (size_t i = 0; i < blocks->len; ++i) changed = cscdRectUnite(changed, areaOf(v, (const CscdElement*)blocks->items[i]));
    BoxRects before = {0}, after = {0}, wasAt = {0};
    boxRects(v->root, &before);
    /* where the boxes are, before, in the viewport (for those that move) */
    noteDrawn(v, v->root, &wasAt);
    viewRelayout(v);
    boxRects(v->root, &after);
    /* a few boxes moving or resizing (a counter's width pushing a spacer) are painted again where
     * they were and where they are; many of them, or most of the page: all of it */
    bool all = before.at.len != after.at.len;
    CscdPtrVec moved = {0};
    for (CscdU64MapSlot* s = NULL; !all && (s = cscdU64MapNext(&before.at, s));) {
        const CscdMapVal* n = cscdU64MapGet(&after.at, s->key);
        if (!n) { all = true; break; }
        const CscdRect a = before.rects.items[s->val.u], b = after.rects.items[n->u];
        if (fabsf(b.x - a.x) > 0.01f || fabsf(b.y - a.y) > 0.01f || fabsf(b.w - a.w) > 0.01f || fabsf(b.h - a.h) > 0.01f) {
            cscdVecPush(&moved, (void*)(uintptr_t)s->key);
            if (moved.len > 32) all = true;
        }
    }
    for (size_t i = 0; i < blocks->len; ++i) changed = cscdRectUnite(changed, areaOf(v, (const CscdElement*)blocks->items[i]));
    CscdRectVec areas = {0};
    cscdVecPush(&areas, changed);
    for (size_t i = 0; i < moved.len && !all; ++i) {
        const CscdElement* e = (const CscdElement*)moved.items[i];
        for (const CscdLayoutBox* p = e->layout; p && !all; p = p->parent)
            if (p->style && (p->style->transform.len || p->style->position == CSCD_POS_STICKY)) all = true;
        cscdVecPush(&areas, cscdRectUnite(*boxRectSlot(&wasAt, e), areaOf(v, e)));
    }
    float covered = 0;
    for (size_t i = 0; i < areas.len; ++i) {
        const CscdRect a = cscdRectIntersect(areas.items[i], cscdRect(0, 0, v->vw, v->vh));
        if (!cscdRectEmpty(a)) covered += a.w * a.h;
    }
    if (all || covered > 0.6f * v->vw * v->vh) {
        v->dirty = v->fullPaint = true;
        viewCenterModals(v);
    } else {
        for (size_t i = 0; i < areas.len; ++i) viewAddDamage(v, areas.items[i]);
        v->dirty = true;
        viewRecordHits(v); /* the old ones point into the layout that went */
    }
    cscdVecFree(&areas);
    cscdVecFree(&moved);
    boxRectsFree(&before);
    boxRectsFree(&after);
    boxRectsFree(&wasAt);
}

/* only texts changed: a new layout, and just them repainted when nothing moved */
/* Only attributes their painting reads changed (a progress bar's value, set each frame): those
 * elements are painted again, and nothing is restyled or laid out, unless a selector or attr() could
 * see the change. (A removed element made the change structural: none is reached here.) */
bool viewRepaintAttributes(CscdView* v) {
    CscdDocument* d = v->doc;
    if (!d->paintChanged.len || d->structural || d->textChanged.len || !v->root || v->styleDirty || v->layoutDirty) return false;
    static const char* const kNames[] = {"value", "max", "min", "low", "high", "optimum"};
    for (size_t i = 0; i < CSCD_COUNT(kNames); ++i)
        if (cscdStyleEngineSelectsAttribute(v->styles, cscdSv(kNames[i]))) return false;
    for (size_t i = 0; i < d->paintChanged.len; ++i)
        if (cscdSvContains(cscdElementAttrC((CscdElement*)d->paintChanged.items[i], "style"), "attr(")) return false;
    v->styledVersion = d->version;
    for (size_t i = 0; i < d->paintChanged.len; ++i) {
        CscdElement* e = (CscdElement*)d->paintChanged.items[i];
        if (e->layout) cscdViewInvalidateElement(v, e);
    }
    d->paintChanged.len = 0;
    return true;
}

bool viewRelayoutText(CscdView* v) {
    CscdDocument* d = v->doc;
    if (d->structural || !d->textChanged.len || d->paintChanged.len || !v->root || v->styleDirty || v->layoutDirty) return false;
    /* each text's block, where it is drawn; a transform or a sticky offset: the usual way */
    CscdPtrVec blocks = {0};
    for (size_t i = 0; i < d->textChanged.len; ++i) {
        const CscdElement* e = (const CscdElement*)d->textChanged.items[i];
        const CscdLayoutBox* b = e->layout ? textBlock(e) : NULL;
        bool usual = !b || !b->element;
        for (const CscdLayoutBox* p = b; p && !usual; p = p->parent)
            if (p->style && (p->style->transform.len || p->style->position == CSCD_POS_STICKY)) usual = true;
        if (usual) {
            cscdVecFree(&blocks);
            return false;
        }
        cscdVecPush(&blocks, b->element);
    }
    v->styledVersion = d->version;
    d->textChanged.len = 0;
    relayoutAround(v, &blocks);
    cscdVecFree(&blocks);
    return true;
}

static bool counterVecEq(const CscdCounterVec* a, const CscdCounterVec* b) {
    if (a->len != b->len) return false;
    for (size_t i = 0; i < a->len; ++i)
        if (!cscdStrEq(&a->items[i].name, &b->items[i].name) || a->items[i].value != b->items[i].value) return false;
    return true;
}

/* Two styles lay out the same boxes: they differ at most in how those are painted (colors,
 * backgrounds, shadows, outlines, radii...) and not in what decides the stacking or the hit areas. */
static bool sameLayout(const CscdComputedStyle* a, const CscdComputedStyle* b) {
    for (int i = 0; i < 4; ++i)
        if (!cscdLenEq(a->margin[i], b->margin[i]) || !cscdLenEq(a->padding[i], b->padding[i]) || a->border[i].width != b->border[i].width ||
            a->border[i].style != b->border[i].style)
            return false;
#define EQ(f) (a->f == b->f)
#define LEQ(f) cscdLenEq(a->f, b->f)
    return EQ(display) && EQ(originalDisplay) && EQ(position) && EQ(floating) && EQ(clear) && EQ(boxSizing) && LEQ(width) && LEQ(height) &&
           LEQ(minWidth) && LEQ(minHeight) && LEQ(maxWidth) && LEQ(maxHeight) && LEQ(top) && LEQ(right) && LEQ(bottom) && LEQ(left) &&
           EQ(aspectRatio) && EQ(zIndex) && EQ(zAuto) && EQ(overflowX) && EQ(overflowY) && EQ(visibility) && (a->opacity < 1) == (b->opacity < 1) &&
           !a->transform.len && !b->transform.len && EQ(perspective) && EQ(preserve3d) && EQ(backfaceHidden) &&
           cscdBackdropAny(&a->backdrop) == cscdBackdropAny(&b->backdrop) && (a->masks.len == 0) == (b->masks.len == 0) && EQ(pointerEvents) &&
           EQ(appearanceNone) && EQ(blend) && (a->blur > 0) == (b->blur > 0) && EQ(objectFit) && cscdStrEq(&a->clipPath, &b->clipPath) &&
           cscdFontDescEq(&a->font, &b->font) && EQ(lineHeight) && EQ(lineHeightFactor) && EQ(letterSpacing) && EQ(wordSpacing) && EQ(textAlign) &&
           LEQ(textIndent) && EQ(whiteSpace) && EQ(textTransform) && EQ(textOverflow) && EQ(wordBreak) && EQ(overflowWrap) && EQ(verticalAlign) &&
           LEQ(verticalAlignLength) && EQ(tabSize) && EQ(listStyle) && EQ(listInside) && cscdStrEq(&a->listImage, &b->listImage) && EQ(lineClamp) &&
           EQ(rtl) && EQ(flexDirection) && EQ(flexWrap) && EQ(justifyContent) && EQ(alignItems) && EQ(alignSelf) && EQ(alignContent) &&
           EQ(justifyItems) && EQ(justifySelf) && EQ(flexGrow) && EQ(flexShrink) && LEQ(flexBasis) && EQ(order) && LEQ(rowGap) && LEQ(columnGap) &&
           cscdTrackListEq(&a->gridColumns, &b->gridColumns) && cscdTrackListEq(&a->gridRows, &b->gridRows) &&
           cscdTrackVecEq(&a->gridAutoColumns, &b->gridAutoColumns) && cscdTrackVecEq(&a->gridAutoRows, &b->gridAutoRows) && EQ(gridAutoFlow) &&
           cscdGridLineEq(&a->gridColumnStart, &b->gridColumnStart) && cscdGridLineEq(&a->gridColumnEnd, &b->gridColumnEnd) &&
           cscdGridLineEq(&a->gridRowStart, &b->gridRowStart) && cscdGridLineEq(&a->gridRowEnd, &b->gridRowEnd) &&
           cscdStrVecEq(&a->gridAreas, &b->gridAreas) && cscdStrEq(&a->gridArea, &b->gridArea) && EQ(borderCollapse) && EQ(borderSpacingX) &&
           EQ(borderSpacingY) && EQ(hasContent) && cscdStrEq(&a->content, &b->content) && cscdStrEq(&a->contentImage, &b->contentImage) &&
           EQ(containerType) && counterVecEq(&a->counterReset, &b->counterReset) && counterVecEq(&a->counterSet, &b->counterSet) &&
           counterVecEq(&a->counterIncrement, &b->counterIncrement) && EQ(textAlignLast) && EQ(alignLastSet) && EQ(isolate) && EQ(tableFixed) &&
           EQ(captionBottom) && cscdStrVecEq(&a->containerNames, &b->containerNames);
#undef EQ
#undef LEQ
}

typedef struct InPlace {
    CscdView* v;
    CSCD_VEC(struct Repoint { CscdLayoutBox* b; CscdComputedStyle* s; }) repoint; /* s: a reference */
    CscdU64Map generated; /* element -> bits: a ::before box, an ::after box */
    CscdU64Map boxed;
} InPlace;

static bool inPlaceWalk(InPlace* P, CscdLayoutBox* b) {
    if (b->anonymous || !b->element || !b->style) return false;
    CscdElement* e = b->element;
    CscdComputedStyle* s = NULL;
    if (!b->pseudo.len) s = e->style;
    else if (cscdStrEqC(&b->pseudo, "marker")) {
        if (!cscdElementIs(e, "summary")) s = cscdStyleEnginePseudoStyle(P->v->styles, e, CSCD_SV("marker"));
        if (!s) s = e->style;
    } else {
        s = cscdStyleEnginePseudoStyle(P->v->styles, e, cscdStrSv(&b->pseudo));
        CscdMapVal* g = cscdU64MapPut(&P->generated, CSCD_PKEY(e), NULL);
        g->u |= cscdStrEqC(&b->pseudo, "before") ? 1 : cscdStrEqC(&b->pseudo, "after") ? 2 : 0;
    }
    if (!s) return false;
    if (s != b->style) {
        if (!sameLayout(b->style, s)) return false;
        struct Repoint rp = {b, (CscdComputedStyle*)cscdRetain(s)};
        cscdVecPush(&P->repoint, rp);
    }
    if (!b->pseudo.len) cscdU64MapPut(&P->boxed, CSCD_PKEY(e), NULL);
    for (size_t i = 0; i < b->children.len; ++i) if (!inPlaceWalk(P, cscdBoxChild(b, i))) return false;
    return true;
}

/* every element: laid out the same, its box (if any) seen, its generated boxes as they were */
static bool inPlaceCheck(InPlace* P, CscdElement* e, const CscdU64Map* was) {
    const CscdMapVal* w = cscdU64MapGet(was, CSCD_PKEY(e));
    const CscdComputedStyle* old = w ? (const CscdComputedStyle*)w->p : NULL;
    if (!old || !e->style) {
        if (old || e->style) return false;
    } else if (old != e->style && !sameLayout(old, e->style)) return false;
    if (e->layout && !cscdU64MapGet(&P->boxed, CSCD_PKEY(e))) return false;
    const CscdMapVal* g = cscdU64MapGet(&P->generated, CSCD_PKEY(e));
    const uint64_t bits = g ? g->u : 0;
    if ((cscdStyleEnginePseudoStyle(P->v->styles, e, CSCD_SV("before")) != NULL) != ((bits & 1) != 0) ||
        (cscdStyleEnginePseudoStyle(P->v->styles, e, CSCD_SV("after")) != NULL) != ((bits & 2) != 0))
        return false;
    for (size_t i = 0; i < e->node.children.len; ++i) {
        CscdElement* ce = cscdNodeAsElement(cscdNodeChild(&e->node, i));
        if (ce && !inPlaceCheck(P, ce, was)) return false;
    }
    return true;
}

/* The subtrees' new styles lay out as the old ones did (only colors, shadows... changed): the boxes
 * take them and just those areas are repainted. False: they need a layout. */
static bool restyleInPlace(CscdView* v, const CscdPtrVec* roots, const CscdU64Map* was) {
    InPlace P;
    memset(&P, 0, sizeof P);
    P.v = v;
    bool ok = true;
    for (size_t i = 0; i < roots->len && ok; ++i) {
        CscdElement* r = (CscdElement*)roots->items[i];
        ok = r->layout && inPlaceWalk(&P, r->layout);
    }
    for (size_t i = 0; i < roots->len && ok; ++i) ok = inPlaceCheck(&P, (CscdElement*)roots->items[i], was);
    if (ok) {
        /* painted again where they were drawn and where they are drawn now (a shadow may have grown) */
        CscdRectVec areas = {0};
        for (size_t i = 0; i < roots->len; ++i) cscdVecPush(&areas, drawnArea(v, ((CscdElement*)roots->items[i])->layout));
        for (size_t i = 0; i < P.repoint.len; ++i) {
            CscdLayoutBox* b = P.repoint.items[i].b;
            cscdRelease(b->style);
            b->style = P.repoint.items[i].s;
            P.repoint.items[i].s = NULL;
        }
        for (size_t i = 0; i < roots->len; ++i) areas.items[i] = cscdRectUnite(areas.items[i], drawnArea(v, ((CscdElement*)roots->items[i])->layout));
        v->layoutDirty = false;
        v->dirty = true;
        float covered = 0;
        for (size_t i = 0; i < areas.len; ++i) {
            const CscdRect a = cscdRectIntersect(areas.items[i], cscdRect(0, 0, v->vw, v->vh));
            if (!cscdRectEmpty(a)) covered += a.w * a.h;
        }
        if (covered > 0.6f * v->vw * v->vh) v->fullPaint = true;
        else for (size_t i = 0; i < areas.len; ++i) viewAddDamage(v, areas.items[i]);
        cscdVecFree(&areas);
    }
    for (size_t i = 0; i < P.repoint.len; ++i) cscdRelease(P.repoint.items[i].s);
    cscdVecFree(&P.repoint);
    cscdU64MapFree(&P.generated);
    cscdU64MapFree(&P.boxed);
    return ok;
}

bool viewContainsPtr(const CscdPtrVec* v, const void* p) {
    for (size_t i = 0; i < v->len; ++i) if (v->items[i] == p) return true;
    return false;
}

/* only :hover / :active changed: just those subtrees, repainted when nothing moved */
void viewRestyleStates(CscdView* v) {
    if (!v->stateChanged.len) return;
    /* only when nothing else changed, and a state cannot reach beyond an element's own subtree */
    if (v->styleDirty || v->layoutDirty || !v->root || v->doc->version != v->styledVersion || cscdStyleEngineStatesReachSiblings(v->styles)) {
        v->styleDirty = true;
        return;
    }
    CscdPtrVec changed = v->stateChanged;
    memset(&v->stateChanged, 0, sizeof v->stateChanged);
    /* the highest ones (their descendants are computed with them), drawn where their box is */
    CscdPtrVec roots = {0};
    bool usual = false;
    for (size_t i = 0; i < changed.len && !usual; ++i) {
        CscdElement* e = (CscdElement*)changed.items[i];
        bool under = false;
        for (CscdElement* a = parentEl(e); a && !under; a = parentEl(a)) under = viewContainsPtr(&changed, a);
        if (under || viewContainsPtr(&roots, e)) continue;
        if (!parentEl(e) || (e->layout && cscdRectEmpty(viewPaintArea(v, e)))) usual = true; /* the root, or transformed: the usual way */
        else cscdVecPush(&roots, e);
    }
    cscdVecFree(&changed);
    if (usual) {
        v->styleDirty = true;
        cscdVecFree(&roots);
        return;
    }
    /* the styles they had, to see whether the new ones lay out the same */
    CscdU64Map was = {0};
    for (size_t i = 0; i < roots.len; ++i) snapStyles(&((CscdElement*)roots.items[i])->node, &was);
    if (viewRestyle(v, &roots) && !restyleInPlace(v, &roots, &was)) relayoutAround(v, &roots);
    releaseSnap(&was);
    cscdVecFree(&roots);
}

void viewRecordHits(CscdView* v) {
    /* a canvas the size of the device that draws nothing: the painter only notes where things are */
    CscdImage bounds;
    memset(&bounds, 0, sizeof bounds);
    bounds.width = cscdIntOf(ceilf(v->vw * v->dpr));
    bounds.height = cscdIntOf(ceilf(v->vh * v->dpr));
    CscdCanvas* c = cscdCanvasNew(&bounds);
    cscdCanvasSetDryRun(c, true);
    cscdCanvasScale(c, v->dpr, v->dpr);
    viewClearHits(v);
    cscdCanvasSave(c);
    if (v->root) {
        cscdCanvasTranslate(c, -v->scrollX, -v->scrollY);
        paintRoot(v, c, v->root);
    }
    cscdCanvasRestore(c);
    cscdCanvasFree(c);
}

static void containerSizes(const CscdLayoutBox* b, CscdU64Map* sizes) {
    if (b->element && !b->anonymous && !b->pseudo.len && b->style && b->style->containerType) {
        CscdMapVal* s = cscdU64MapPut(sizes, CSCD_PKEY(b->element), NULL);
        s->f[0] = cscdBoxContentW(b);
        s->f[1] = cscdBoxContentH(b);
    }
    for (size_t i = 0; i < b->children.len; ++i) containerSizes(cscdBoxChild(b, i), sizes);
}

void viewRelayout(CscdView* v) {
    const double t0 = cscdNowSeconds();
    CscdViewport vp = {v->vw, v->vh};
    cscdLayoutBoxFree(v->root);
    v->root = cscdLayoutDocument(v->doc, v->styles, vp, viewImageLoader, v);
    /* container queries: styles that ask about a container's size are worked out again with the size
     * this layout gave it (containment keeps that size from depending on them) */
    for (int pass = 0; pass < 4 && cscdStyleEngineHasContainers(v->styles); ++pass) {
        CscdU64Map sizes = {0};
        if (v->root) containerSizes(v->root, &sizes);
        if (!cscdStyleEngineSetContainerSizes(v->styles, &sizes)) break;
        viewRestyle(v, NULL);
        cscdLayoutBoxFree(v->root);
        v->root = cscdLayoutDocument(v->doc, v->styles, vp, viewImageLoader, v);
    }
    v->layoutDirty = false;
    /* keep scroll offsets in range */
    float maxY = cscdMaxf(0.f, cscdViewContentHeight(v) - v->vh);
    v->scrollY = cscdClampf(v->scrollY, 0.f, maxY);
    float maxX = v->root ? cscdMaxf(0.f, cscdRectRight(v->root->overflow) - v->vw) : 0;
    v->scrollX = cscdClampf(v->scrollX, 0.f, maxX);
    v->stats.layoutMs = (cscdNowSeconds() - t0) * 1000;
    ++v->stats.layouts;
}

void viewLayoutNow(CscdView* v) {
    if (v->doc->version != v->styledVersion && !viewRepaintAttributes(v) && !viewRelayoutText(v)) v->styleDirty = true;
    if (!v->styleDirty) viewRestyleStates(v);
    if (v->styleDirty) viewRestyle(v, NULL);
    if (v->layoutDirty) { viewRelayout(v); v->dirty = v->fullPaint = true; }
}

void viewCenterModals(CscdView* v) {
    /* a modal dialog stays centered when its contents change its size */
    for (size_t i = 0; i < v->modals.len; ++i) {
        CscdElement* d = (CscdElement*)v->modals.items[i];
        if (!d->layout) continue;
        CscdRect r = cscdViewElementRect(v, d);
        const float x = cscdMaxf(0.f, roundf((v->vw - r.w) / 2)), y = cscdMaxf(0.f, roundf((v->vh - r.h) / 2));
        if (fabsf(r.x - x) < 0.5f && fabsf(r.y - y) < 0.5f) continue;
        char buf[32];
        snprintf(buf, sizeof buf, "%.2fpx", x);
        cscdElementSetStyle(d, CSCD_SV("left"), cscdSv(buf));
        snprintf(buf, sizeof buf, "%.2fpx", y);
        cscdElementSetStyle(d, CSCD_SV("top"), cscdSv(buf));
        viewRestyle(v, NULL);
        viewRelayout(v);
    }
}
