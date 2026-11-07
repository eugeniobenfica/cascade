/* Transitions and keyframe animations: timing functions, interpolating computed values, starting
 * and advancing them each frame. */
#include "style/styleInternal.h"
#include "view/viewInternal.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- transitions and animations ------------------------------------------------------------------ */

static float bezierX(float x1, float x2, float t) { float u = 1 - t; return 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t; }

static float bezierY(float x1, float y1, float x2, float y2, float x) {
    /* solve x(t) = x with Newton, then evaluate y(t) */
    float t = x;
    for (int i = 0; i < 8; ++i) {
        float d = (bezierX(x1, x2, t + 1e-4f) - bezierX(x1, x2, t - 1e-4f)) / 2e-4f;
        if (fabsf(d) < 1e-6f) break;
        t -= (bezierX(x1, x2, t) - x) / d;
        t = cscdClampf(t, 0.f, 1.f);
    }
    return bezierX(y1, y2, t);
}

static float ease(float t, float x1, float y1, float x2, float y2, int steps) {
    t = cscdClampf(t, 0.f, 1.f);
    if (steps > 0) return floorf(t * steps) / steps;
    if (steps < 0) return t > 0 ? 1.f : 0.f;
    return bezierY(x1, y1, x2, y2, t);
}

static float lerpF(float a, float b, float t) { return a + (b - a) * t; }

static CscdLength lerpL(CscdLength a, CscdLength b, float t) {
    if (a.kind != CSCD_LEN_FIXED || b.kind != CSCD_LEN_FIXED) return t < 0.5f ? a : b;
    CscdLength r = cscdLenFixed(lerpF(a.px, b.px, t));
    r.pct = lerpF(a.pct, b.pct, t);
    return r;
}

static CscdTransformOp identityOp(const CscdTransformOp* op) {
    CscdTransformOp i = *op;
    i.x = cscdLenFixed(0);
    i.y = cscdLenFixed(0);
    if (op->kind == CSCD_TF_SCALE || op->kind == CSCD_TF_SCALE_Z) { i.a = 1; i.b = 1; }
    else { i.a = 0; i.b = 0; }
    i.m = cscdMatIdentity();
    i.z = 0;
    for (int k = 0; k < 16; ++k) i.m4[k] = k % 5 == 0 ? 1.f : 0.f;
    return i;
}

static bool transformsEq(const CscdTransformVec* a, const CscdTransformVec* b) {
    if (a->len != b->len) return false;
    for (size_t i = 0; i < a->len; ++i) if (!cscdTransformOpEq(&a->items[i], &b->items[i])) return false;
    return true;
}

typedef struct Interp {
    const char* p;
    size_t pLen;
    bool all, any;
} Interp;

static bool isProp(Interp* I, const char* name) {
    if (I->all || cscdSvEqC(cscdSvMake(I->p, I->pLen), name)) { I->any = true; return true; }
    return false;
}

static bool startsProp(Interp* I, const char* prefix) {
    if (I->all || cscdSvStartsC(cscdSvMake(I->p, I->pLen), prefix)) { I->any = true; return true; }
    return false;
}

/* o takes the value of the property p between a and b at t; returns whether p is one that moves
 * (layout: whether it changes the layout). */
static bool interpolate(CscdComputedStyle* o, const CscdComputedStyle* a, const CscdComputedStyle* b, float t, CscdSv p, bool* layout) {
    *layout = false;
    Interp I = {p.ptr ? p.ptr : "", p.len, cscdSvEqC(p, "all"), false};
    if (isProp(&I, "opacity")) o->opacity = lerpF(a->opacity, b->opacity, t);
    if (isProp(&I, "color")) o->color = cscdColorLerp(a->color, b->color, t);
    if (isProp(&I, "background-color") || isProp(&I, "background")) o->backgroundColor = cscdColorLerp(a->backgroundColor, b->backgroundColor, t);
    if (startsProp(&I, "border") && !cscdSvStartsC(p, "border-radius") && cscdSvFind(p, CSCD_SV("width"), 0) == CSCD_NPOS)
        for (int i = 0; i < 4; ++i) o->border[i].color = cscdColorLerp(a->border[i].color, b->border[i].color, t);
    if (isProp(&I, "border-radius") || startsProp(&I, "border-top-left-radius"))
        for (int i = 0; i < 4; ++i) {
            o->radius[i].x = lerpL(a->radius[i].x, b->radius[i].x, t);
            o->radius[i].y = lerpL(a->radius[i].y, b->radius[i].y, t);
        }
    if (isProp(&I, "outline-color") || isProp(&I, "outline")) o->outlineColor = cscdColorLerp(a->outlineColor, b->outlineColor, t);
    if (isProp(&I, "fill")) o->fill = cscdColorLerp(a->fill, b->fill, t);
    if (isProp(&I, "stroke")) o->stroke = cscdColorLerp(a->stroke, b->stroke, t);
    if (isProp(&I, "box-shadow") && a->boxShadow.len == b->boxShadow.len) {
        if (o->boxShadow.len < a->boxShadow.len) cscdVecResize(&o->boxShadow, a->boxShadow.len);
        for (size_t i = 0; i < a->boxShadow.len; ++i) {
            CscdShadow* s = &o->boxShadow.items[i];
            const CscdShadow* x = &a->boxShadow.items[i];
            const CscdShadow* y = &b->boxShadow.items[i];
            s->x = lerpF(x->x, y->x, t); s->y = lerpF(x->y, y->y, t);
            s->blur = lerpF(x->blur, y->blur, t); s->spread = lerpF(x->spread, y->spread, t);
            s->color = cscdColorLerp(x->color, y->color, t);
        }
    } else if (isProp(&I, "box-shadow") && (!a->boxShadow.len || !b->boxShadow.len)) {
        const CscdShadowVec* src = !a->boxShadow.len ? &b->boxShadow : &a->boxShadow;
        if (src != &o->boxShadow) cscdVecCopy(&o->boxShadow, src);
        float k = !a->boxShadow.len ? t : 1 - t;
        for (size_t i = 0; i < o->boxShadow.len; ++i) o->boxShadow.items[i].color = cscdColorWithAlpha(o->boxShadow.items[i].color, k);
    }
    if (isProp(&I, "transform") || isProp(&I, "translate") || isProp(&I, "scale") || isProp(&I, "rotate")) {
        CscdTransformVec ta = {0}, tb = {0};
        cscdVecCopy(&ta, &a->transform);
        cscdVecCopy(&tb, &b->transform);
        if (!ta.len) for (size_t i = 0; i < tb.len; ++i) cscdVecPush(&ta, identityOp(&tb.items[i]));
        if (!tb.len) for (size_t i = 0; i < ta.len; ++i) cscdVecPush(&tb, identityOp(&ta.items[i]));
        bool same = ta.len == tb.len;
        for (size_t i = 0; same && i < ta.len; ++i) same = ta.items[i].kind == tb.items[i].kind;
        if (same) {
            cscdVecCopy(&o->transform, &ta);
            for (size_t i = 0; i < ta.len; ++i) {
                CscdTransformOp* r = &o->transform.items[i];
                const CscdTransformOp* x = &ta.items[i];
                const CscdTransformOp* y = &tb.items[i];
                r->x = lerpL(x->x, y->x, t); r->y = lerpL(x->y, y->y, t);
                r->a = lerpF(x->a, y->a, t); r->b = lerpF(x->b, y->b, t);
                r->m = cscdMatrix(lerpF(x->m.a, y->m.a, t), lerpF(x->m.b, y->m.b, t), lerpF(x->m.c, y->m.c, t),
                                lerpF(x->m.d, y->m.d, t), lerpF(x->m.e, y->m.e, t), lerpF(x->m.f, y->m.f, t));
                r->z = lerpF(x->z, y->z, t);
                r->x3 = lerpF(x->x3, y->x3, t); r->y3 = lerpF(x->y3, y->y3, t); r->z3 = lerpF(x->z3, y->z3, t);
                for (int k = 0; k < 16; ++k) r->m4[k] = lerpF(x->m4[k], y->m4[k], t);
            }
        } else {
            const CscdTransformVec* src = t < 0.5f ? &a->transform : &b->transform;
            if (src != &o->transform) cscdVecCopy(&o->transform, src);
        }
        cscdVecFree(&ta);
        cscdVecFree(&tb);
    }
    if (isProp(&I, "filter")) {
        o->blur = lerpF(a->blur, b->blur, t);
        o->brightness = lerpF(a->brightness, b->brightness, t);
        o->grayscale = lerpF(a->grayscale, b->grayscale, t);
    }
    /* layout properties */
#define LP(name, field) if (isProp(&I, name)) { o->field = lerpL(a->field, b->field, t); *layout = true; }
    LP("width", width)
    LP("height", height)
    LP("max-height", maxHeight)
    LP("max-width", maxWidth)
    LP("min-height", minHeight)
    LP("min-width", minWidth)
    LP("top", top)
    LP("left", left)
    LP("right", right)
    LP("bottom", bottom)
#undef LP
    if (isProp(&I, "inset")) {
        o->top = lerpL(a->top, b->top, t); o->left = lerpL(a->left, b->left, t);
        o->right = lerpL(a->right, b->right, t); o->bottom = lerpL(a->bottom, b->bottom, t);
        *layout = true;
    }
    static const char* m[4] = {"margin-top", "margin-right", "margin-bottom", "margin-left"};
    static const char* pd[4] = {"padding-top", "padding-right", "padding-bottom", "padding-left"};
    for (int i = 0; i < 4; ++i) {
        if (isProp(&I, m[i]) || isProp(&I, "margin")) { o->margin[i] = lerpL(a->margin[i], b->margin[i], t); *layout = true; }
        if (isProp(&I, pd[i]) || isProp(&I, "padding")) { o->padding[i] = lerpL(a->padding[i], b->padding[i], t); *layout = true; }
    }
    if (isProp(&I, "flex-grow")) { o->flexGrow = lerpF(a->flexGrow, b->flexGrow, t); *layout = true; }
    if (isProp(&I, "font-size")) { o->font.size = lerpF(a->font.size, b->font.size, t); *layout = true; }
    if (isProp(&I, "letter-spacing")) { o->letterSpacing = lerpF(a->letterSpacing, b->letterSpacing, t); *layout = true; }
    if (isProp(&I, "gap") || isProp(&I, "column-gap")) { o->columnGap = lerpL(a->columnGap, b->columnGap, t); *layout = true; }
    if (isProp(&I, "gap") || isProp(&I, "row-gap")) { o->rowGap = lerpL(a->rowGap, b->rowGap, t); *layout = true; }
    if (I.all) {
        /* layout changes from 'all' only count when something actually differs */
        *layout = !(cscdLenEq(a->width, b->width) && cscdLenEq(a->height, b->height) && cscdLenEq(a->top, b->top) && cscdLenEq(a->left, b->left) &&
                    cscdLenEq(a->margin[0], b->margin[0]) && cscdLenEq(a->margin[1], b->margin[1]) && cscdLenEq(a->margin[2], b->margin[2]) &&
                    cscdLenEq(a->margin[3], b->margin[3]) && cscdLenEq(a->padding[0], b->padding[0]) && cscdLenEq(a->padding[1], b->padding[1]) &&
                    cscdLenEq(a->padding[2], b->padding[2]) && cscdLenEq(a->padding[3], b->padding[3]) && a->font.size == b->font.size &&
                    cscdLenEq(a->maxHeight, b->maxHeight) && cscdLenEq(a->maxWidth, b->maxWidth) && cscdLenEq(a->right, b->right) &&
                    cscdLenEq(a->bottom, b->bottom));
    }
    return I.any;
}

static bool differs(const CscdComputedStyle* a, const CscdComputedStyle* b, CscdSv p) {
    if (!cscdSvEqC(p, "all")) return true;
    if (a->opacity != b->opacity || !cscdColorEq(a->color, b->color) || !cscdColorEq(a->backgroundColor, b->backgroundColor) ||
        !transformsEq(&a->transform, &b->transform) || !cscdColorEq(a->border[0].color, b->border[0].color) || !cscdLenEq(a->width, b->width) ||
        !cscdLenEq(a->height, b->height) || a->boxShadow.len != b->boxShadow.len || !cscdLenEq(a->top, b->top) || !cscdLenEq(a->left, b->left) ||
        !cscdColorEq(a->outlineColor, b->outlineColor) || !cscdLenEq(a->maxHeight, b->maxHeight) || a->font.size != b->font.size)
        return true;
    for (size_t i = 0; i < a->boxShadow.len && i < b->boxShadow.len; ++i) {
        const CscdShadow* x = &a->boxShadow.items[i];
        const CscdShadow* y = &b->boxShadow.items[i];
        if (!cscdColorEq(x->color, y->color) || x->blur != y->blur || x->y != y->y) return true;
    }
    return false;
}

static void removeTransition(CscdView* v, size_t i) {
    RunningTransition* r = &v->transitions.items[i];
    cscdStrFree(&r->prop);
    cscdRelease(r->from);
    cscdRelease(r->to);
    cscdVecRemove(&v->transitions, i);
}

void viewStartTransitions(CscdView* v, CscdElement* e, const CscdComputedStyle* before, const CscdComputedStyle* after) {
    if (!viewOn(v, CSCD_FEATURE_ANIMATIONS)) return; /* (the new style at once) */
    double t = viewNow(v);
    for (size_t i = 0; i < after->transitions.len; ++i) {
        const CscdTransition* tr = &after->transitions.items[i];
        const CscdSv p = cscdStrSv(&tr->property);
        if (tr->duration <= 0 && tr->delay <= 0) continue;
        if (!differs(before, after, p)) continue;
        /* specific properties compare directly */
        if (!cscdSvEqC(p, "all")) {
            CscdComputedStyle* probe = cscdStyleClone(after);
            bool layout;
            const bool moves = interpolate(probe, before, after, 0, p, &layout);
            cscdRelease(probe);
            if (!moves) continue;
            bool changed;
            if (cscdSvEqC(p, "opacity")) changed = before->opacity != after->opacity;
            else if (cscdSvEqC(p, "color")) changed = !cscdColorEq(before->color, after->color);
            else if (cscdSvEqC(p, "background-color") || cscdSvEqC(p, "background")) changed = !cscdColorEq(before->backgroundColor, after->backgroundColor);
            else if (cscdSvEqC(p, "transform")) changed = !transformsEq(&before->transform, &after->transform);
            else changed = true;
            if (!changed) continue;
        }
        /* replace a running transition of the same property, starting from where it is now */
        for (size_t k = v->transitions.len; k-- > 0;)
            if (v->transitions.items[k].e == e && cscdStrEq(&v->transitions.items[k].prop, &tr->property)) removeTransition(v, k);
        RunningTransition r;
        r.e = e;
        r.prop = cscdStrCopy(&tr->property);
        r.from = cscdStyleClone(before);
        r.to = cscdStyleClone(after);
        r.start = t + tr->delay;
        r.duration = tr->duration;
        r.t = *tr;
        r.t.property = r.prop;
        cscdVecPush(&v->transitions, r);
    }
    /* transitions still running whose target changed keep running toward the new style */
    for (size_t k = 0; k < v->transitions.len; ++k) {
        RunningTransition* r = &v->transitions.items[k];
        if (r->e != e) continue;
        cscdRelease(r->to);
        r->to = cscdStyleClone(after);
    }
}

void cscdStyleApplyDeclaration(CscdComputedStyle* s, const CscdComputedStyle* parent, CscdSv property, CscdSv value,
                             const CscdStyleEnvironment* env, CscdSv baseDir) {
    CscdApplyState st;
    cscdApplyStateInit(&st, s, parent, env, baseDir);
    st.ctx.em = s->font.size;
    st.ctx.vw = env->media.width;
    st.ctx.vh = env->media.height;
    st.ctx.rem = env->rootFontSize;
    st.colorSet = 0xFF;
    CscdStr val = cscdStrDup(value);
    if (s->vars && cscdSvFind(cscdStrSv(&val), CSCD_SV("var("), 0) != CSCD_NPOS) {
        /* simple substitution for animations */
        for (CscdStrMapSlot* slot = NULL; (slot = cscdStrMapNext(&s->vars->map, slot));) {
            CscdStr ref = cscdStrf("var(%.*s)", (int)slot->keyLen, slot->key);
            const CscdStr* with = (const CscdStr*)slot->val.p;
            for (size_t pos; (pos = cscdSvFind(cscdStrSv(&val), cscdStrSv(&ref), 0)) != CSCD_NPOS;) {
                cscdStrErase(&val, pos, ref.len);
                cscdStrInsert(&val, pos, cscdStrSv(with));
            }
            cscdStrFree(&ref);
        }
    }
    CscdStrPairVec ex = {0};
    CscdSv prop = cscdCanonicalProperty(property);
    if (cscdExpandShorthand(prop, cscdStrSv(&val), &ex))
        for (size_t i = 0; i < ex.len; ++i) cscdApplyLonghand(&st, cscdStrSv(&ex.items[i].name), cscdStrSv(&ex.items[i].value));
    else cscdApplyLonghand(&st, prop, cscdStrSv(&val));
    if (st.bgTouched && st.bgImage.len) cscdParseBackgrounds(&st);
    cscdStrPairVecFree(&ex);
    cscdStrFree(&val);
    cscdApplyStateFini(&st);
}

void viewRemoveAnimation(CscdView* v, size_t i) {
    cscdStrFree(&v->animations.items[i].a.name);
    cscdRelease(v->animations.items[i].base);
    cscdVecRemove(&v->animations, i);
}

void viewTickAnimations(CscdView* v, double t) {
    if (!v->transitions.len && !v->animations.len) return; /* (nothing moving: nothing to repaint) */
    bool needLayout = false;
    for (size_t i = 0; i < v->transitions.len;) {
        RunningTransition* r = &v->transitions.items[i];
        if (!r->e->style) { removeTransition(v, i); continue; }
        float p = r->duration > 0 ? (float)((t - r->start) / r->duration) : 1;
        if (t < r->start) p = 0;
        float k = ease(p, r->t.x1, r->t.y1, r->t.x2, r->t.y2, r->t.steps);
        bool layout;
        interpolate(r->e->style, r->from, r->to, k, cscdStrSv(&r->prop), &layout);
        needLayout = needLayout || layout;
        if (p >= 1) removeTransition(v, i);
        else ++i;
    }
    CscdStrVec props = {0};
    for (size_t i = 0; i < v->animations.len;) {
        RunningAnimation* r = &v->animations.items[i];
        const CscdKeyframesRule* kf = cscdStyleEngineKeyframes(v->styles, cscdStrSv(&r->a.name));
        if (!kf || !kf->frames.len || !r->e->style) { viewRemoveAnimation(v, i); continue; }
        double el = t - r->start - r->a.delay;
        if (r->a.paused) el = 0;
        float dur = cscdMaxf(1e-3f, r->a.duration);
        double iter = el / dur;
        bool done = r->a.iterations >= 0 && iter >= r->a.iterations;
        if (el < 0 && !(r->a.fillMode & 2)) { ++i; continue; }
        if (done) iter = r->a.iterations;
        double whole = floor(iter);
        float p = (float)(iter - whole);
        if (done && p == 0 && r->a.iterations > 0) { p = 1; whole -= 1; }
        bool reverse = r->a.reverse;
        if (r->a.alternate && ((long long)whole % 2 == 1)) reverse = !reverse;
        if (reverse) p = 1 - p;
        if (el < 0) p = reverse ? 1 : 0;
        CscdComputedStyle* base = r->base;
        /* keyframe styles */
        const CscdKeyframe* a = &kf->frames.items[0];
        const CscdKeyframe* b = &kf->frames.items[kf->frames.len - 1];
        for (size_t k = 0; k + 1 < kf->frames.len; ++k)
            if (p >= kf->frames.items[k].offset && p <= kf->frames.items[k + 1].offset) { a = &kf->frames.items[k]; b = &kf->frames.items[k + 1]; break; }
        CscdStyleEnvironment env;
        memset(&env, 0, sizeof env);
        env.media = viewMediaOf(v);
        env.rootFontSize = 16 * env.media.textScale;
        CscdElement* pe = parentEl(r->e);
        const CscdComputedStyle* parent = pe ? pe->style : NULL;
        CscdComputedStyle* sa = cscdStyleClone(base);
        CscdComputedStyle* sb = cscdStyleClone(base);
        const CscdSv baseDir = cscdStrSv(&v->doc->baseDir);
        /* offsets 0% / 100% that are not declared use the base style */
        for (size_t d = 0; d < a->declarations.len; ++d)
            cscdStyleApplyDeclaration(sa, parent, cscdStrSv(&a->declarations.items[d].property), cscdStrSv(&a->declarations.items[d].value), &env, baseDir);
        for (size_t d = 0; d < b->declarations.len; ++d)
            cscdStyleApplyDeclaration(sb, parent, cscdStrSv(&b->declarations.items[d].property), cscdStrSv(&b->declarations.items[d].value), &env, baseDir);
        float span = b->offset - a->offset;
        float local = span > 0 ? (p - a->offset) / span : 1;
        float k = ease(local, r->a.x1, r->a.y1, r->a.x2, r->a.y2, r->a.steps);
        cscdStrVecFree(&props);
        for (size_t d = 0; d < a->declarations.len; ++d) cscdVecPush(&props, cscdStrCopy(&a->declarations.items[d].property));
        for (size_t d = 0; d < b->declarations.len; ++d) {
            bool have = false;
            for (size_t q = 0; q < props.len && !have; ++q) have = cscdStrEq(&props.items[q], &b->declarations.items[d].property);
            if (!have) cscdVecPush(&props, cscdStrCopy(&b->declarations.items[d].property));
        }
        if (!(done && !(r->a.fillMode & 1))) {
            for (size_t q = 0; q < props.len; ++q) {
                bool layout;
                const CscdSv pr = cscdStrSv(&props.items[q]);
                interpolate(r->e->style, sa, sb, k, cscdCanonicalProperty(pr), &layout);
                if (cscdSvEqC(pr, "inset") || cscdSvEqC(pr, "margin") || cscdSvEqC(pr, "padding")) interpolate(r->e->style, sa, sb, k, pr, &layout);
                needLayout = needLayout || layout;
            }
        }
        cscdRelease(sa);
        cscdRelease(sb);
        if (done) {
            if (!(r->a.fillMode & 1)) {
                /* restore the base values */
                for (size_t q = 0; q < props.len; ++q) {
                    bool l;
                    interpolate(r->e->style, base, base, 1, cscdCanonicalProperty(cscdStrSv(&props.items[q])), &l);
                    needLayout = needLayout || l;
                }
            }
            viewRemoveAnimation(v, i);
            continue;
        }
        ++i;
    }
    cscdStrVecFree(&props);
    if (needLayout) v->layoutDirty = true;
    v->dirty = v->fullPaint = true;
}
