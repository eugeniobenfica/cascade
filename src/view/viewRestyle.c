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
    if (v->doc->structural || v->doc->attrChanged.len) viewA11yStructure(v); /* (an aria- attribute, a role...) */
    v->doc->textChanged.len = 0;
    v->doc->paintChanged.len = 0;
    cscdDocumentClearAttrChanges(v->doc);
    cscdDocumentClearNodeChanges(v->doc);
    v->doc->structural = false;
    v->stateChanged.len = 0;
    v->styleDirty = false;
    return false;
}

void viewRestyleAll(CscdView* v) { viewRestyle(v, NULL); }

/* Where each element's boxes are (layout coordinates): element -> index in rects; for a box that scrolls, what it
 * holds and where it is scrolled to (its scrollbars: x, y the scroll offsets, w, h the overflow's size); and how its
 * boxes look, as one number: the styles they were given (its own and its ::before, ::after, ::marker), and where its
 * text and its inline pieces are (a line centered in a box that kept its size moves when a neighbor grows). Kept (kept):
 * the styles stay alive, so that one freed meanwhile cannot come back at the same address and look the same. */
typedef struct BoxRects {
    CscdU64Map at;
    CscdRectVec rects, scrolls;
    CSCD_VEC(uint64_t) looks;
    CscdPtrVec kept;
} BoxRects;

static CscdRect* boxRectSlot(BoxRects* m, const CscdElement* e) {
    bool inserted = false;
    CscdMapVal* v = cscdU64MapPut(&m->at, CSCD_PKEY(e), &inserted);
    if (inserted) {
        v->u = m->rects.len;
        CscdRect z = {0, 0, 0, 0};
        cscdVecPush(&m->rects, z);
        cscdVecPush(&m->scrolls, z);
        cscdVecPush(&m->looks, 14695981039346656037ull);
    }
    return &m->rects.items[v->u];
}

static uint64_t mixFloat(uint64_t h, float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    return (h ^ bits) * 1099511628211ull;
}

static uint64_t mixFrags(uint64_t h, const CscdLayoutBox* b) {
    for (size_t i = 0; i < b->textFrags.len; ++i) {
        const CscdTextFrag* f = &b->textFrags.items[i];
        h = mixFloat(mixFloat(mixFloat(mixFloat(h, f->x), f->baseline), f->width), f->justify);
    }
    for (size_t i = 0; i < b->inlineFrags.len; ++i) {
        const CscdRect r = b->inlineFrags.items[i].rect;
        h = mixFloat(mixFloat(mixFloat(mixFloat(h, r.x), r.y), r.w), r.h);
    }
    return h;
}

static void boxRectsWalk(const CscdLayoutBox* b, BoxRects* m, bool keep) {
    if (b->element && b->anonymous) { /* (its pieces are the element's: an anonymous block holds its lines) */
        CscdRect* known = boxRectSlot(m, b->element);
        const size_t at = (size_t)(known - m->rects.items);
        m->looks.items[at] = mixFrags(m->looks.items[at], b);
    }
    if (b->element && !b->anonymous) {
        CscdRect* known = boxRectSlot(m, b->element);
        const size_t at = (size_t)(known - m->rects.items);
        m->looks.items[at] = mixFrags(m->looks.items[at], b);
        if (!b->pseudo.len) {
            CscdRect r = cscdBoxBorderBox(b);
            for (size_t i = 0; i < b->inlineFrags.len; ++i) r = cscdRectUnite(r, b->inlineFrags.items[i].rect);
            *known = cscdRectUnite(*known, r);
            if (b->style && cscdStyleClipsOverflow(b->style) && b->parent)
                m->scrolls.items[at] = cscdRect(b->element->scrollLeft, b->element->scrollTop, b->overflow.w, b->overflow.h);
        }
        m->looks.items[at] = (m->looks.items[at] ^ (uint64_t)(uintptr_t)b->style) * 1099511628211ull;
        if (keep && b->style) cscdVecPush(&m->kept, cscdRetain(b->style));
    }
    for (size_t i = 0; i < b->children.len; ++i) boxRectsWalk(cscdBoxChild(b, i), m, keep);
}

static void boxRects(const CscdLayoutBox* b, BoxRects* m) { boxRectsWalk(b, m, false); }

static void boxRectsFree(BoxRects* m) {
    cscdU64MapFree(&m->at);
    cscdVecFree(&m->rects);
    cscdVecFree(&m->scrolls);
    cscdVecFree(&m->looks);
    for (size_t i = 0; i < m->kept.len; ++i) cscdRelease(m->kept.items[i]);
    cscdVecFree(&m->kept);
}

/* The block a changed text lies in, with the ink of all its text (layout coordinates). */
static const CscdLayoutBox* textBlock(const CscdElement* e) {
    const CscdLayoutBox* b = e->layout;
    while (b && (b->kind == CSCD_BOX_INLINE || b->kind == CSCD_BOX_TEXT) && b->parent) b = b->parent;
    return b;
}

/* what a box draws itself (not its children): its box, its text, its outline and shadows */
static CscdRect ownInk(const CscdLayoutBox* b) {
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
        /* a blur reaches 3 sigmas (sigma: half the blur radius) and a device pixel past the shape (the canvas's pad):
         * the blur radius alone left the faint tail of a grown shadow on screen */
        for (size_t i = 0; i < s->boxShadow.len; ++i) {
            const CscdShadow* sh = &s->boxShadow.items[i];
            if (!sh->inset) out = cscdMaxf(out, cscdMaxf(fabsf(sh->x), fabsf(sh->y)) + 1.5f * sh->blur + 1 + cscdMaxf(0.f, sh->spread));
        }
        for (size_t i = 0; i < s->textShadow.len; ++i) {
            const CscdShadow* sh = &s->textShadow.items[i];
            out = cscdMaxf(out, cscdMaxf(fabsf(sh->x), fabsf(sh->y)) + 1.5f * sh->blur + 1);
        }
        if (out > 0) r = cscdRectUnite(r, cscdRect(r.x - out, r.y - out, r.w + 2 * out, r.h + 2 * out));
    }
    return r;
}

static CscdRect textInk(const CscdLayoutBox* b) {
    CscdRect r = ownInk(b);
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

/* drawnArea of every element, in one pass (each box's ink is its own and its children's): d is the offset of the page's
 * and the scrolled ancestors' scrolling above b; returns b's ink */
static CscdRect noteDrawnWalk(const CscdLayoutBox* b, CscdPoint d, BoxRects* wasAt) {
    CscdRect ink = ownInk(b);
    CscdPoint inner = d;
    if (b->style && cscdStyleClipsOverflow(b->style) && b->element && b->parent) {
        inner.x -= b->element->scrollLeft;
        inner.y -= b->element->scrollTop;
    }
    for (size_t i = 0; i < b->children.len; ++i) ink = cscdRectUnite(ink, noteDrawnWalk(cscdBoxChild(b, i), inner, wasAt));
    if (b->element && !b->anonymous && !b->pseudo.len) {
        const CscdPoint at = b->style && b->style->position == CSCD_POS_FIXED ? cscdPoint(0, 0) : d;
        CscdRect* w = boxRectSlot(wasAt, b->element);
        *w = cscdRectUnite(*w, cscdRect(ink.x + at.x - 2, ink.y + at.y - 2, ink.w + 4, ink.h + 4));
    }
    return ink;
}

static void noteDrawn(CscdView* v, const CscdLayoutBox* b, BoxRects* wasAt) { noteDrawnWalk(b, cscdPoint(-v->scrollX, -v->scrollY), wasAt); }

/* painted beyond its box by something above it (a transform, a sticky offset, a filter that spreads it, a backdrop
 * that reads it): its area alone is not enough */
static bool drawnElsewhere(const CscdLayoutBox* b) {
    for (const CscdLayoutBox* p = b; p; p = p->parent)
        if (p->style && (p->style->transform.len || p->style->position == CSCD_POS_STICKY || p->style->blur > 0 || cscdBackdropAny(&p->style->backdrop)))
            return true;
    return false;
}

static CscdRect areaOf(CscdView* v, const CscdElement* e) { return e->layout ? drawnArea(v, e->layout) : cscdRect(0, 0, 0, 0); }

/* ---- the damage of a relayout, from what it made or laid out again ---------------------------------------------------
 * The layout notes, on each element box it made again or laid out again, what that element's own boxes drew before
 * (prevInk, prevLook), and on a box it took where it was, as it was, that nothing in it changed (keptIn). So only the
 * boxes the layout touched are looked at: one whose own boxes look otherwise is painted again where it was and where
 * it is, and what was kept is passed over with all it holds. */

typedef struct Damage {
    CscdView* v;
    uint64_t epoch; /* (of the layout just made) */
    CscdRectVec* areas;
    int count;
    bool all;
} Damage;

static void damageWalk(Damage* D, const CscdLayoutBox* b, CscdPoint d) {
    if (D->all) return;
    if (b->keptIn == D->epoch) {
        if (b->x == b->keptX && b->y == b->keptY) return; /* (as it was, where it was) */
        /* as it was, shifted (a flex or grid parent places its items after laying them out): where it was and is */
        if (drawnElsewhere(b) || ++D->count > 32) {
            D->all = true;
            return;
        }
        const CscdPoint at = b->style && b->style->position == CSCD_POS_FIXED ? cscdPoint(0, 0) : d;
        const CscdRect now = textInk(b), was = cscdRectTranslated(now, b->keptX - b->x, b->keptY - b->y);
        const CscdRect r = cscdRectUnite(was, now);
        cscdVecPush(D->areas, cscdRect(r.x + at.x - 2, r.y + at.y - 2, r.w + 4, r.h + 4));
        return;
    }
    CscdPoint inner = d;
    if (b->style && cscdStyleClipsOverflow(b->style) && b->element && b->parent) {
        inner.x -= b->element->scrollLeft;
        inner.y -= b->element->scrollTop;
    }
    if (cscdBoxIsElements(b)) {
        if (!b->hasPrev) { /* (a box made with nothing before it: the layout said so too) */
            D->all = true;
            return;
        }
        if (cscdBoxOwnLook(b) != b->prevLook) {
            if (drawnElsewhere(b) || ++D->count > 32) {
                D->all = true;
                return;
            }
            const CscdPoint at = b->style && b->style->position == CSCD_POS_FIXED ? cscdPoint(0, 0) : d;
            const CscdRect r = cscdRectUnite(b->prevInk, cscdBoxOwnInk(b));
            cscdVecPush(D->areas, cscdRect(r.x + at.x - 2, r.y + at.y - 2, r.w + 4, r.h + 4));
        }
    }
    for (size_t i = 0; i < b->children.len; ++i) {
        const CscdLayoutBox* c = cscdBoxChild(b, i);
        if (cscdBoxIsElements(c) || c->anonymous) damageWalk(D, c, inner); /* (texts and pseudo elements: their element's own) */
    }
}

static void relayoutAroundWhole(CscdView* v, const CscdPtrVec* blocks);

/* Lays out; repaints only what changed: the elements whose own boxes look otherwise than before (a text that changed,
 * a box that moved, a class that only changed colors), found from what the layout touched; many of them, or most of
 * the page: all of it. */
static void relayoutAround(CscdView* v, const CscdPtrVec* blocks) {
    if (!viewLayoutReuse || !v->root || cscdStyleEngineHasContainers(v->styles) || v->layoutDpr != v->dpr) {
        relayoutAroundWhole(v, blocks); /* (each layout from nothing, or several in one: element by element, before and after) */
        return;
    }
    const float heightWas = cscdViewContentHeight(v), scrollXWas = v->scrollX, scrollYWas = v->scrollY;
    viewRelayout(v);
    bool all = v->layoutBoxesChanged || v->scrollX != scrollXWas || v->scrollY != scrollYWas || !v->root;
    CscdRectVec areas = {0};
    if (!all) {
        Damage D = {v, v->doc->layoutEpoch - 1, &areas, 0, false};
        damageWalk(&D, v->root, cscdPoint(-v->scrollX, -v->scrollY));
        all = D.all;
    }
    if (!all && cscdViewContentHeight(v) != heightWas) cscdVecPush(&areas, cscdRect(v->vw - 12, 0, 12, v->vh)); /* (the page's scrollbar) */
    float covered = 0;
    for (size_t i = 0; i < areas.len && !all; ++i) {
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
}

/* The same, comparing every element's boxes before and after (when layouts are made from nothing, or several at once). */
static void relayoutAroundWhole(CscdView* v, const CscdPtrVec* blocks) {
    CscdRect changed = {0, 0, 0, 0};
    for (size_t i = 0; i < blocks->len; ++i) changed = cscdRectUnite(changed, areaOf(v, (const CscdElement*)blocks->items[i]));
    BoxRects before = {0}, after = {0}, wasAt = {0};
    boxRectsWalk(v->root, &before, true);
    /* (the page's own scrollbar: its thumb follows the content's height, and where the page is scrolled to) */
    const float heightWas = cscdViewContentHeight(v), scrollXWas = v->scrollX, scrollYWas = v->scrollY;
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
        const CscdRect sa = before.scrolls.items[s->val.u], sb = after.scrolls.items[n->u];
        /* (a box that scrolls and holds more or less now, or is scrolled elsewhere: its scrollbars changed) */
        if (fabsf(b.x - a.x) > 0.01f || fabsf(b.y - a.y) > 0.01f || fabsf(b.w - a.w) > 0.01f || fabsf(b.h - a.h) > 0.01f ||
            fabsf(sb.x - sa.x) > 0.01f || fabsf(sb.y - sa.y) > 0.01f || fabsf(sb.w - sa.w) > 0.01f || fabsf(sb.h - sa.h) > 0.01f ||
            before.looks.items[s->val.u] != after.looks.items[n->u]) {
            cscdVecPush(&moved, (void*)(uintptr_t)s->key);
            if (moved.len > 32) all = true;
        }
    }
    for (size_t i = 0; i < blocks->len; ++i) changed = cscdRectUnite(changed, areaOf(v, (const CscdElement*)blocks->items[i]));
    CscdRectVec areas = {0};
    cscdVecPush(&areas, changed);
    if (v->scrollX != scrollXWas || v->scrollY != scrollYWas) all = true; /* (the page moved: kept in range as it shrank) */
    else if (cscdViewContentHeight(v) != heightWas) cscdVecPush(&areas, cscdRect(v->vw - 12, 0, 12, v->vh));
    for (size_t i = 0; i < moved.len && !all; ++i) {
        const CscdElement* e = (const CscdElement*)moved.items[i];
        if (e->layout && drawnElsewhere(e->layout)) all = true;
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
    if (!d->paintChanged.len || d->structural || d->textChanged.len || d->attrChanged.len || !v->root || v->styleDirty || v->layoutDirty)
        return false;
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
    if (d->structural || !d->textChanged.len || d->paintChanged.len || d->attrChanged.len || !v->root || v->styleDirty || v->layoutDirty)
        return false;
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

/* Only attributes that matter through styles changed (a class toggled, an inline style, data-state): the styles are
 * computed again and the page laid out, and what is painted again is what moved or now looks different, not the whole
 * window. */
/* the classes in one list and not in the other (either way: a class that came, and one that went) */
static unsigned classesReach(const CscdStyleEngine* se, CscdSv from, CscdSv to) {
    unsigned bits = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const CscdSv a = pass ? to : from, b = pass ? from : to;
        for (size_t i = 0; i < a.len;) {
            while (i < a.len && cscdIsSpace(a.ptr[i])) ++i;
            size_t j = i;
            while (j < a.len && !cscdIsSpace(a.ptr[j])) ++j;
            if (j > i) {
                const CscdSv cls = cscdSvSub(a, i, j - i);
                bool inB = false;
                for (size_t k = 0; k < b.len && !inB;) {
                    while (k < b.len && cscdIsSpace(b.ptr[k])) ++k;
                    size_t l = k;
                    while (l < b.len && !cscdIsSpace(b.ptr[l])) ++l;
                    inB = l > k && cscdSvEq(cscdSvSub(b, k, l - k), cls);
                    k = l;
                }
                if (!inB) bits |= cscdStyleEngineReach(se, CSCD_REACH_CLASS, cls);
            }
            i = j;
        }
    }
    return bits;
}

/* how far one change reaches (CSCD_REACH_*) */
static unsigned changeReach(const CscdView* v, const struct CscdAttrChange* c) {
    const CscdStyleEngine* se = v->styles;
    const CscdSv name = cscdStrSv(&c->name), now = cscdElementAttr(c->element, name);
    unsigned bits = cscdStyleEngineReach(se, CSCD_REACH_ATTRIBUTE, name); /* ([class~=x], [data-state=on]...) */
    if (cscdStyleEngineSelectsAttribute(se, name)) bits |= CSCD_REACH_SELF; /* (attr() in a value: its own style) */
    if (cscdSvEqC(name, "style")) bits |= CSCD_REACH_SELF;
    else if (cscdSvEqC(name, "class")) bits |= classesReach(se, cscdStrSv(&c->old), now);
    else if (cscdSvEqC(name, "id")) bits |= cscdStyleEngineReach(se, CSCD_REACH_ID, cscdStrSv(&c->old)) | cscdStyleEngineReach(se, CSCD_REACH_ID, now);
    return bits;
}

/* whether n is in the document (not in a subtree cut off from it, nor a template's content) */
static bool connected(const CscdNode* n, const CscdDocument* d) {
    for (; n; n = n->parent)
        if (n == &d->node) return true;
    return false;
}

static void addRoot(CscdPtrVec* roots, CscdElement* e, bool* all) {
    if (!e) return;
    if (!parentEl(e)) *all = true; /* (the root element: the whole document) */
    else if (!viewContainsPtr(roots, e)) cscdVecPush(roots, e);
}

/* the highest ones (those under another are styled with it) */
static void keepHighest(CscdPtrVec* roots) {
    size_t w = 0;
    for (size_t i = 0; i < roots->len; ++i) {
        CscdElement* e = (CscdElement*)roots->items[i];
        bool under = false;
        for (const CscdElement* a = parentEl(e); a && !under; a = parentEl(a)) under = viewContainsPtr(roots, a);
        if (!under) roots->items[w++] = e;
    }
    roots->len = w;
}

/* Elements came in, went, or changed themselves (an attribute layout reads, a value, a check), with perhaps attributes,
 * texts and painted values besides. What the rules can see of each is styled again: a new element and what it holds;
 * its parent's children when a rule sees places among siblings (+, ~, :nth-child...); nothing for one gone, unless so;
 * the whole page only with :has(). The layout that follows takes the boxes of what did not change. */
bool viewRestyleStructure(CscdView* v) {
    CscdDocument* d = v->doc;
    if (!d->structural || d->nodeChangesFull || !v->root || v->styleDirty || v->stateChanged.len) return false;
    const unsigned reach = cscdStyleEngineStructureReach(v->styles);
    if (reach & CSCD_REACH_ALL) return false;
    const bool siblings = (reach & CSCD_REACH_SIBLINGS) != 0;
    CscdPtrVec roots = {0};
    bool all = false;
    for (size_t i = 0; i < d->nodeChanges.len && !all; ++i) {
        const CscdNodeChange* c = &d->nodeChanges.items[i];
        if (!connected(c->node, d)) continue;
        CscdElement* e = cscdNodeAsElement(c->node);
        CscdElement* parent = c->node->parent && cscdNodeIsElement(c->node->parent) ? (CscdElement*)c->node->parent : NULL;
        if (c->kind == CSCD_CHANGE_INSERTED) {
            addRoot(&roots, siblings ? parent : e, &all); /* (a text: nothing, its parent's style stands) */
            if (parent && cscdStyleEngineEmptyMatters(v->styles, parent)) addRoot(&roots, parent, &all); /* (:empty no more) */
        } else if (c->kind == CSCD_CHANGE_CHILDREN) addRoot(&roots, e && (siblings || cscdStyleEngineEmptyMatters(v->styles, e)) ? e : NULL, &all);
        else addRoot(&roots, siblings ? parent : e, &all);
        if (c->kind != CSCD_CHANGE_INSERTED && !e) all = true; /* (the document itself) */
    }
    for (size_t i = 0; i < d->attrChanges.len && !all; ++i) {
        const CscdAttrChange* c = &d->attrChanges.items[i];
        if (!connected(&c->element->node, d)) continue;
        const unsigned bits = changeReach(v, c);
        if (bits & CSCD_REACH_ALL) all = true;
        else if (bits) addRoot(&roots, bits & CSCD_REACH_SIBLINGS ? parentEl(c->element) : c->element, &all);
    }
    if (all) {
        cscdVecFree(&roots);
        return false;
    }
    keepHighest(&roots);
    const bool whole = roots.len && !viewRestyle(v, &roots); /* (false: it styled the whole document after all) */
    cscdVecFree(&roots);
    if (whole) return true;
    /* (as a whole restyle leaves the document) */
    v->styledVersion = d->version;
    viewA11yStructure(v);
    d->textChanged.len = 0;
    d->paintChanged.len = 0;
    cscdDocumentClearAttrChanges(d);
    cscdDocumentClearNodeChanges(d);
    d->structural = false;
    v->stateChanged.len = 0;
    v->styleDirty = false;
    v->layoutDirty = true;
    return true;
}

/* Only attributes that matter through styles changed (a class toggled, an inline style, data-state). What the rules
 * say each change reaches is styled again: the element and its subtree, or its parent's for one that reaches following
 * siblings, or nothing at all when no rule names it; the whole page when one reaches anywhere (:has()). Then the page is
 * laid out, and what is painted again is what moved or now looks different. */
bool viewRestyleAttributes(CscdView* v) {
    CscdDocument* d = v->doc;
    if (!d->attrChanged.len || d->structural || d->textChanged.len || d->paintChanged.len || !v->root || v->styleDirty || v->layoutDirty ||
        v->stateChanged.len)
        return false;
    CscdPtrVec roots = {0};
    bool all = false;
    for (size_t i = 0; i < d->attrChanges.len && !all; ++i) {
        const struct CscdAttrChange* c = &d->attrChanges.items[i];
        const unsigned bits = changeReach(v, c);
        if (!bits) continue;
        CscdElement* root = c->element;
        if (bits & CSCD_REACH_ALL) all = true;
        else if (bits & CSCD_REACH_SIBLINGS) root = parentEl(c->element);
        if (!root || !parentEl(root)) all = true; /* (the root element: the whole document) */
        else if (!viewContainsPtr(&roots, root)) cscdVecPush(&roots, root);
    }
    if (all) {
        cscdVecFree(&roots);
        viewRestyle(v, NULL);
    } else {
        /* (the highest ones: those under another are styled with it) */
        size_t w = 0;
        for (size_t i = 0; i < roots.len; ++i) {
            CscdElement* e = (CscdElement*)roots.items[i];
            bool under = false;
            for (const CscdElement* a = parentEl(e); a && !under; a = parentEl(a)) under = viewContainsPtr(&roots, a);
            if (!under) roots.items[w++] = e;
        }
        roots.len = w;
        const bool whole = roots.len && !viewRestyle(v, &roots); /* (false: it styled the whole document after all) */
        cscdVecFree(&roots);
        if (!whole) {
            /* (as a whole restyle leaves the document: styled as it now is) */
            v->styledVersion = d->version;
            viewA11yStructure(v); /* (an aria- attribute, a role...) */
            cscdDocumentClearAttrChanges(d);
            v->styleDirty = false;
            if (!w) return true; /* (no rule names what changed: nothing to style, lay out or paint) */
            v->layoutDirty = true;
        }
    }
    const CscdPtrVec none = {0};
    relayoutAround(v, &none);
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
        /* (what they draw now: a shadow may have grown; the rest of the page's inks stay) */
        for (size_t i = 0; i < P.repoint.len; ++i) paintInkChanged(v, P.repoint.items[i].b);
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
    /* (only those a rule's :hover or :active could match: the root coming under the pointer matters to no ".row:hover") */
    size_t kept = 0;
    for (size_t i = 0; i < changed.len; ++i)
        if (cscdStyleEngineStateMatters(v->styles, (const CscdElement*)changed.items[i])) changed.items[kept++] = changed.items[i];
    changed.len = kept;
    if (!kept) {
        cscdVecFree(&changed);
        return;
    }
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

/* the boxes that scroll kept in range when what they hold shrank: past the end, the content showed a gap and the
 * thumb was drawn below the box, where nothing painted over it again */
static void clampScrolls(CscdView* v, CscdLayoutBox* b) {
    CscdElement* e = b->element;
    if (e && !b->anonymous && !b->pseudo.len && (e->scrollTop != 0 || e->scrollLeft != 0) && b->parent) {
        float maxX, maxY;
        if (cscdElementIs(e, "textarea")) viewTextAreaRange(v, e, &maxX, &maxY);
        else {
            const CscdRect pb = cscdBoxPaddingBox(b);
            maxY = cscdMaxf(0.f, b->overflow.h - pb.h);
            maxX = cscdMaxf(0.f, b->overflow.w - pb.w);
        }
        e->scrollTop = cscdClampf(e->scrollTop, 0.f, maxY);
        e->scrollLeft = cscdClampf(e->scrollLeft, 0.f, maxX);
    }
    for (size_t i = 0; i < b->children.len; ++i) clampScrolls(v, cscdBoxChild(b, i));
}

bool viewLayoutReuse = true;

/* a layout of the page: from the last one (the boxes of what did not change taken from it, then the rest of it freed),
 * or from nothing */
static void layoutFrom(CscdView* v, bool reuse) {
    CscdViewport vp = {v->vw, v->vh};
    CscdLayoutBox* old = v->root;
    reuse = reuse && old && viewLayoutReuse && v->layoutDpr == v->dpr;
    bool changed = true;
    v->root = cscdLayoutDocumentFrom(v->doc, v->styles, vp, viewImageLoader, v, reuse ? old : NULL, &changed);
    v->layoutBoxesChanged = v->layoutBoxesChanged || changed;
    if (reuse) cscdLayoutBoxFreeReplaced(old);
    else cscdLayoutBoxFree(old);
    v->layoutDpr = v->dpr;
}

void viewRelayout(CscdView* v) {
    const double t0 = cscdNowSeconds();
    v->layoutBoxesChanged = false;
    layoutFrom(v, true);
    /* container queries: styles that ask about a container's size are worked out again with the size
     * this layout gave it (containment keeps that size from depending on them) */
    for (int pass = 0; pass < 4 && cscdStyleEngineHasContainers(v->styles); ++pass) {
        CscdU64Map sizes = {0};
        if (v->root) containerSizes(v->root, &sizes);
        if (!cscdStyleEngineSetContainerSizes(v->styles, &sizes)) break;
        viewRestyle(v, NULL);
        layoutFrom(v, true);
    }
    v->layoutDirty = false;
    v->inkValid = false;
    if (v->root) clampScrolls(v, v->root);
    /* keep scroll offsets in range */
    float maxY = cscdMaxf(0.f, cscdViewContentHeight(v) - v->vh);
    v->scrollY = cscdClampf(v->scrollY, 0.f, maxY);
    float maxX = v->root ? cscdMaxf(0.f, cscdRectRight(v->root->overflow) - v->vw) : 0;
    v->scrollX = cscdClampf(v->scrollX, 0.f, maxX);
    v->stats.layoutMs = (cscdNowSeconds() - t0) * 1000;
    ++v->stats.layouts;
}

/* ---- the oracle of the incremental paths -----------------------------------------------------------------------
 * What the light paths left (styles in place, a text's block laid out again, damage painted over the last frame)
 * against what the whole pipeline makes from nothing: every element's boxes where they are, and every pixel. */

static void describeElement(CscdStr* out, const CscdElement* e) {
    cscdStrAppend(out, cscdStrSv(&e->tag));
    const CscdSv id = cscdElementAttrC(e, "id"), cls = cscdElementAttrC(e, "class");
    if (id.len) cscdStrAppendf(out, "#%.*s", CSCD_SVF(id));
    if (cls.len) cscdStrAppendf(out, ".%.*s", CSCD_SVF(cls));
}

bool viewCheckIncremental(CscdView* v, CscdImage* frame, CscdStr* why) {
    BoxRects light = {0}, full = {0};
    if (v->root) boxRects(v->root, &light);
    v->styleDirty = true;
    viewRestyle(v, NULL);
    const bool reuse = viewLayoutReuse;
    viewLayoutReuse = false; /* (from scratch: nothing taken from the layout under test) */
    viewRelayout(v);
    viewLayoutReuse = reuse;
    v->dirty = v->fullPaint = true;
    if (v->root) boxRects(v->root, &full);
    bool same = light.at.len == full.at.len;
    if (!same) cscdStrAppendf(why, "%u elements with boxes, from scratch %u; ", (unsigned)light.at.len, (unsigned)full.at.len);
    for (CscdU64MapSlot* s = NULL; (s = cscdU64MapNext(&full.at, s));) {
        const CscdMapVal* l = cscdU64MapGet(&light.at, s->key);
        const CscdRect b = full.rects.items[s->val.u];
        const CscdRect a = l ? light.rects.items[l->u] : cscdRect(NAN, NAN, NAN, NAN);
        if (l && fabsf(a.x - b.x) <= 0.01f && fabsf(a.y - b.y) <= 0.01f && fabsf(a.w - b.w) <= 0.01f && fabsf(a.h - b.h) <= 0.01f) continue;
        if (same) {
            describeElement(why, (const CscdElement*)(uintptr_t)s->key);
            cscdStrAppendf(why, " at %g,%g %gx%g, from scratch %g,%g %gx%g; ", a.x, a.y, a.w, a.h, b.x, b.y, b.w, b.h);
        }
        same = false;
    }
    boxRectsFree(&light);
    boxRectsFree(&full);
    /* the frame the damage was painted over, against one painted whole */
    CscdImage fresh;
    cscdImageInit(&fresh, frame->width, frame->height);
    cscdViewPaint(v, &fresh, false);
    for (int y = 0; y < frame->height && same; ++y) {
        const uint32_t *a = cscdImageRow(frame, y), *b = cscdImageRow(&fresh, y);
        for (int x = 0; x < frame->width; ++x)
            if (a[x] != b[x]) {
                cscdStrAppendf(why, "pixel %d,%d is %08x, from scratch %08x; ", x, y, (unsigned)a[x], (unsigned)b[x]);
                same = false;
                break;
            }
    }
    for (int y = 0; y < frame->height; ++y) memcpy(cscdImageRow(frame, y), cscdImageRow(&fresh, y), (size_t)frame->width * 4);
    cscdImageFini(&fresh);
    v->incrementalValid = true; /* (frame is what was painted whole: the next paint goes over it, damage only) */
    return same;
}

void viewLayoutNow(CscdView* v) {
    if (v->doc->version != v->styledVersion && !viewRepaintAttributes(v) && !viewRelayoutText(v) && !viewRestyleAttributes(v) && !viewRestyleStructure(v))
        v->styleDirty = true;
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
