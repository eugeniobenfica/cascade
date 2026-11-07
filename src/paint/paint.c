/* Painting: stacking contexts and paint order (CSS 2.2 appendix E), backgrounds and gradients,
 * borders, shadows, outlines, text and its decorations, replaced content, inline SVG, form controls
 * and scrollbars. */
#include "text/bidiInternal.h"
#include "core/thread.h"
#include "view/viewInternal.h"

#include "cascade/css.h"

#include <stdio.h>
#include <stdlib.h>

#define PAINT_PI 3.14159265358979f

/* Painting into an offscreen image that a perspective transform puts on the device: how the image's
 * pixels map to the device's (for the hit areas), and the device clip. */
typedef struct Projection { float h[9]; CscdRect clip; } Projection;

typedef struct Painter {
    CscdView* view;
    CscdCanvas* canvas;
    CscdMatrix base;
    const CscdShader* textShader;
    bool rootPropagated;
    const Projection* projection;
} Painter;

static Painter painter(CscdView* v, CscdCanvas* c) {
    Painter p;
    memset(&p, 0, sizeof p);
    p.view = v;
    p.canvas = c;
    p.base = cscdCanvasMatrix(c);
    return p;
}


/* A right-to-left run in the order it shows: its characters reversed (a base keeps the marks that
 * follow it) and mirrored (")" in place of "("). */
static void visualText(CscdSv s, CscdStr* out) {
    CscdSizeVec clusters = {0}; /* pairs: start, end */
    for (size_t i = 0; i < s.len;) {
        const size_t a = i;
        const uint32_t cp = cscdUtf8Next(s, &i);
        const bool mark = cscdBidiClass(cp) == CSCD_BIDI_NSM || cp == 0x200D;
        if (mark && clusters.len) clusters.items[clusters.len - 1] = i;
        else {
            cscdVecPush(&clusters, a);
            cscdVecPush(&clusters, i);
        }
    }
    cscdStrClear(out);
    cscdStrReserve(out, s.len);
    for (size_t k = clusters.len / 2; k-- > 0;) {
        size_t i = clusters.items[2 * k];
        const uint32_t cp = cscdUtf8Next(s, &i);
        cscdUtf8Append(out, cscdBidiMirror(cp));
        cscdStrAppend(out, cscdSvSub(s, i, clusters.items[2 * k + 1] - i));
    }
    cscdVecFree(&clusters);
}

static CscdColor darker(CscdColor c, float f) { return cscdRgba((int)(c.r * f), (int)(c.g * f), (int)(c.b * f), c.a); }

static CscdRRect borderRRect(const CscdLayoutBox* b, CscdRect r) {
    CscdRRect rr = cscdRRect(r);
    const CscdComputedStyle* s = b->style;
    for (int i = 0; i < 4; ++i) {
        rr.rx[i] = cscdLenResolve(s->radius[i].x, r.w, 0);
        rr.ry[i] = cscdLenResolve(s->radius[i].y, r.h, 0);
    }
    cscdRRectNormalize(&rr);
    return rr;
}

static CscdRRect rrInset(CscdRRect r, float t, float rt, float bt, float l) { return cscdRRectInset(&r, t, rt, bt, l); }

static void resolveStops(const CscdColorStopVec* in, float length, float* firstPos, float* lastPos, CscdGradientStopVec* out) {
    out->len = 0;
    CscdFloatVec p = {0};
    CscdSizeVec idx = {0};
    for (size_t i = 0; i < in->len; ++i) {
        if (in->items[i].hint) continue;
        cscdVecPush(&idx, i);
        cscdVecPush(&p, cscdLenIsAuto(in->items[i].pos) ? -1e30f : cscdLenResolve(in->items[i].pos, length, 0) / cscdMaxf(length, 1e-6f));
    }
    if (!idx.len) {
        cscdVecFree(&p);
        cscdVecFree(&idx);
        return;
    }
    if (p.items[0] < -1e29f) p.items[0] = 0;
    if (p.items[p.len - 1] < -1e29f) p.items[p.len - 1] = 1;
    /* monotonic */
    float maxSoFar = p.items[0];
    for (size_t i = 1; i < p.len; ++i) {
        if (p.items[i] < -1e29f) continue;
        if (p.items[i] < maxSoFar) p.items[i] = maxSoFar;
        maxSoFar = p.items[i];
    }
    /* evenly spread unspecified positions */
    for (size_t i = 1; i < p.len; ++i) {
        if (p.items[i] > -1e29f) continue;
        size_t j = i;
        while (j < p.len && p.items[j] < -1e29f) ++j;
        float a = p.items[i - 1], b = p.items[j];
        for (size_t k = i; k < j; ++k) p.items[k] = a + (b - a) * (float)(k - i + 1) / (float)(j - i + 1);
        i = j;
    }
    *firstPos = p.items[0];
    *lastPos = p.items[p.len - 1];
    for (size_t i = 0; i < idx.len; ++i) {
        CscdGradientStop g = {p.items[i], in->items[idx.items[i]].color};
        cscdVecPush(out, g);
    }
    cscdVecFree(&p);
    cscdVecFree(&idx);
}

static void record(Painter* P, CscdElement* e, CscdRect r, bool text, CscdLayoutBox* b, int frag) {
    if (!e) return;
    if (b && b->style && b->style->pointerEvents == CSCD_PE_NONE) return;
    CscdView* v = P->view;
    if (!v->recordHits) return; /* part of the page: the hits recorded with the whole page stay */
    CscdHitItem hit;
    memset(&hit, 0, sizeof hit);
    hit.element = e;
    hit.rect = r;
    hit.m = cscdCanvasMatrix(P->canvas);
    hit.text = text;
    hit.box = b;
    hit.frag = frag;
    if (P->projection) {
        hit.clip = P->projection->clip;
        hit.projective = true;
        memcpy(hit.h, P->projection->h, sizeof hit.h);
    } else {
        hit.clip = cscdCanvasDeviceClipBounds(P->canvas);
        hit.h[0] = hit.h[4] = hit.h[8] = 1;
    }
    cscdVecPush(&v->hits, hit);
}

/* ---- backgrounds --------------------------------------------------------------------------------- */

static CscdShader gradientShader(const CscdBackgroundLayer* L, CscdRect tile) {
    CscdShader sh = cscdShaderSolid(cscdRgba(0, 0, 0, 0));
    float first = 0, last = 1;
    CscdGradientStopVec stops = {0};
    if (L->kind == CSCD_LAYER_LINEAR) {
        float ang = L->angle * PAINT_PI / 180.f;
        if (L->toCorner) {
            /* perpendicular to the diagonal joining the two other corners */
            float dx = L->cornerX * tile.h, dy = L->cornerY * tile.w;
            ang = cscdAtan2f(dx, -dy);
        }
        float sx = cscdSinf(ang), sy = -cscdCosf(ang);
        float len = fabsf(tile.w * sx) + fabsf(tile.h * sy);
        CscdPoint c = {tile.x + tile.w / 2, tile.y + tile.h / 2};
        CscdPoint p0 = {c.x - sx * len / 2, c.y - sy * len / 2}, p1 = {c.x + sx * len / 2, c.y + sy * len / 2};
        resolveStops(&L->stops, len, &first, &last, &stops);
        if (L->repeating && last > first) {
            CscdPoint a = {p0.x + (p1.x - p0.x) * first, p0.y + (p1.y - p0.y) * first};
            CscdPoint b = {p0.x + (p1.x - p0.x) * last, p0.y + (p1.y - p0.y) * last};
            for (size_t i = 0; i < stops.len; ++i) stops.items[i].offset = (stops.items[i].offset - first) / (last - first);
            sh = cscdShaderLinear(a, b, stops.items, stops.len);
            sh.repeating = true;
        } else sh = cscdShaderLinear(p0, p1, stops.items, stops.len);
        cscdVecFree(&stops);
        return sh;
    }
    if (L->kind == CSCD_LAYER_RADIAL) {
        CscdPoint c = {tile.x + cscdLenResolve(L->centerX, tile.w, 0), tile.y + cscdLenResolve(L->centerY, tile.h, 0)};
        float l = c.x - tile.x, r = cscdRectRight(tile) - c.x, t = c.y - tile.y, b = cscdRectBottom(tile) - c.y;
        float rx, ry;
        switch (L->radialSize) {
        case 1: rx = cscdMinf(l, r); ry = cscdMinf(t, b); if (L->circle) rx = ry = cscdMinf(rx, ry); break;
        case 3: rx = cscdMaxf(l, r); ry = cscdMaxf(t, b); if (L->circle) rx = ry = cscdMaxf(rx, ry); break;
        case 2: {
            float cx = cscdMinf(l, r), cy = cscdMinf(t, b);
            if (L->circle) rx = ry = cscdHypotf(cx, cy);
            else { rx = cx * sqrtf(2.f); ry = cy * sqrtf(2.f); }
            break;
        }
        case 4: rx = cscdLenResolve(L->radiusX, tile.w, 0); ry = cscdLenResolve(L->radiusY, tile.h, 0); if (L->circle) ry = rx; break;
        default: {
            float fx = cscdMaxf(l, r), fy = cscdMaxf(t, b);
            if (L->circle) rx = ry = cscdHypotf(fx, fy);
            else { rx = fx * sqrtf(2.f); ry = fy * sqrtf(2.f); }
            break;
        }
        }
        rx = cscdMaxf(rx, 0.01f);
        ry = cscdMaxf(ry, 0.01f);
        resolveStops(&L->stops, rx, &first, &last, &stops);
        sh = cscdShaderRadial(c, rx, ry, stops.items, stops.len);
        cscdVecFree(&stops);
        if (L->repeating && last > first) {
            for (size_t i = 0; i < sh.stops.len; ++i) sh.stops.items[i].offset = (sh.stops.items[i].offset - first) / (last - first);
            sh.rx = rx * last;
            sh.ry = ry * last; /* approximate: repeat from the center */
            sh.repeating = true;
        }
        return sh;
    }
    /* conic */
    sh.kind = CSCD_SHADER_CONIC;
    sh.center = cscdPoint(tile.x + cscdLenResolve(L->centerX, tile.w, 0), tile.y + cscdLenResolve(L->centerY, tile.h, 0));
    sh.startAngle = L->angle * PAINT_PI / 180.f;
    resolveStops(&L->stops, 100, &first, &last, &sh.stops);
    return sh;
}

static CscdRRect clipFor(const CscdLayoutBox* b, CscdRRect outer, uint8_t c) {
    if (c == CSCD_BG_PADDING_BOX) return rrInset(outer, b->border[0], b->border[1], b->border[2], b->border[3]);
    if (c == CSCD_BG_CONTENT_BOX)
        return rrInset(outer, b->border[0] + b->padding[0], b->border[1] + b->padding[1], b->border[2] + b->padding[2], b->border[3] + b->padding[3]);
    return outer;
}

/* Whether a table cell holds anything that shows (for empty-cells): text other than white space,
 * a replaced element, a box with a size. */
static bool emptyCellHasContent(const CscdLayoutBox* b) {
    for (size_t i = 0; i < b->children.len; ++i) {
        const CscdLayoutBox* c = cscdBoxChild(b, i);
        if (c->kind == CSCD_BOX_TEXT) {
            for (size_t k = 0; k < c->text.len; ++k)
                if (!strchr(" \t\n\r\f", c->text.ptr[k])) return true;
            continue;
        }
        if (c->kind == CSCD_BOX_REPLACED || (c->style && c->style->position != CSCD_POS_ABSOLUTE && c->style->position != CSCD_POS_FIXED && (c->w > 0 && c->h > 0)) ||
            emptyCellHasContent(c))
            return true;
    }
    return false;
}

static void paintBackground(Painter* P, CscdLayoutBox* b, CscdRect borderBox, CscdRRect outer, bool skipText) {
    CscdCanvas* canvas = P->canvas;
    const CscdComputedStyle* s = b->style;
    CscdRect paddingBox = cscdRectInset(borderBox, b->border[3], b->border[0], b->border[1], b->border[2]);
    CscdRect contentBox = cscdRectInset(paddingBox, b->padding[3], b->padding[0], b->padding[1], b->padding[2]);
    uint8_t colorClip = !s->backgrounds.len ? CSCD_BG_BORDER_BOX : s->backgrounds.items[s->backgrounds.len - 1].clip;
    if (colorClip == CSCD_BG_TEXT && skipText) return;
    if (s->backgroundColor.a && colorClip != CSCD_BG_TEXT) {
        CscdRRect cl = clipFor(b, outer, colorClip);
        cscdCanvasFillRRect(canvas, &cl, s->backgroundColor);
    }
    for (size_t i = s->backgrounds.len; i-- > 0;) {
        const CscdBackgroundLayer* L = &s->backgrounds.items[i];
        if (L->clip == CSCD_BG_TEXT) continue;
        CscdRect area = L->origin == CSCD_BG_BORDER_BOX ? borderBox : L->origin == CSCD_BG_CONTENT_BOX ? contentBox : paddingBox;
        CscdRRect clip = clipFor(b, outer, L->clip);
        /* tile size */
        float iw = 0, ih = 0;
        bool img = L->kind == CSCD_LAYER_URL;
        if (img) {
            if (!L->image || cscdImageEmpty(L->image)) continue;
            iw = (float)L->image->width;
            ih = (float)L->image->height;
        }
        float tw = 0, th = 0;
        if (L->sizeKeyword == 1 || L->sizeKeyword == 2) {
            if (img) {
                float sc = L->sizeKeyword == 1 ? cscdMaxf(area.w / iw, area.h / ih) : cscdMinf(area.w / iw, area.h / ih);
                tw = iw * sc;
                th = ih * sc;
            } else { tw = area.w; th = area.h; }
        } else {
            bool ax = cscdLenIsAuto(L->sizeX), ay = cscdLenIsAuto(L->sizeY);
            if (!ax) tw = cscdLenResolve(L->sizeX, area.w, 0);
            if (!ay) th = cscdLenResolve(L->sizeY, area.h, 0);
            if (ax && ay) { tw = img ? iw : area.w; th = img ? ih : area.h; }
            else if (ax) tw = img && ih > 0 ? th * iw / ih : area.w;
            else if (ay) th = img && iw > 0 ? tw * ih / iw : area.h;
        }
        if (tw <= 0 || th <= 0) continue;
        float px = area.x + cscdLenResolve(L->posX, area.w - tw, 0);
        float py = area.y + cscdLenResolve(L->posY, area.h - th, 0);
        CscdRect tile = {px, py, tw, th};
        cscdCanvasSave(canvas);
        cscdCanvasClipRRect(canvas, &clip);
        if (img) {
            CscdShader sh = cscdShaderImage(L->image, tile, L->repeatX, L->repeatY);
            CscdRect fill = clip.rect;
            if (!L->repeatX) { fill.x = cscdMaxf(fill.x, tile.x); fill.w = cscdMinf(cscdRectRight(clip.rect), cscdRectRight(tile)) - fill.x; }
            if (!L->repeatY) { fill.y = cscdMaxf(fill.y, tile.y); fill.h = cscdMinf(cscdRectBottom(clip.rect), cscdRectBottom(tile)) - fill.y; }
            if (!cscdRectEmpty(fill)) cscdCanvasFillRectShader(canvas, fill, &sh);
            cscdShaderFini(&sh);
        } else {
            /* gradients: one tile unless it repeats over a sized tile */
            const bool autoSize = cscdLenIsAuto(L->sizeX) && cscdLenIsAuto(L->sizeY) && L->sizeKeyword == 0;
            bool tiled = (L->repeatX || L->repeatY) && (tw < clip.rect.w - 0.5f || th < clip.rect.h - 0.5f) && !autoSize;
            if (!tiled) {
                CscdShader sh = gradientShader(L, tile);
                cscdCanvasFillRectShader(canvas, autoSize ? clip.rect : tile, &sh);
                cscdShaderFini(&sh);
            } else {
                float x0 = L->repeatX ? tile.x - ceilf((tile.x - clip.rect.x) / tw) * tw : tile.x;
                float y0 = L->repeatY ? tile.y - ceilf((tile.y - clip.rect.y) / th) * th : tile.y;
                int guard = 0;
                for (float yy = y0; yy < cscdRectBottom(clip.rect) && guard < 4000; yy += th) {
                    for (float xx = x0; xx < cscdRectRight(clip.rect) && guard < 4000; xx += tw, ++guard) {
                        CscdRect t = {xx, yy, tw, th};
                        CscdShader sh = gradientShader(L, t);
                        cscdCanvasFillRectShader(canvas, t, &sh);
                        cscdShaderFini(&sh);
                        if (!L->repeatX) break;
                    }
                    if (!L->repeatY) break;
                }
            }
        }
        cscdCanvasRestore(canvas);
    }
}

/* ---- borders ------------------------------------------------------------------------------------- */

static CscdColor styleColor(const CscdBorderSide* sd, int side, bool outerHalf) {
    CscdColor c = sd[side].color;
    bool topLeft = side == 0 || side == 3;
    switch (sd[side].style) {
    case CSCD_BORDER_INSET: return topLeft ? darker(c, 0.6f) : c;
    case CSCD_BORDER_OUTSET: return topLeft ? c : darker(c, 0.6f);
    case CSCD_BORDER_GROOVE: return (topLeft == outerHalf) ? darker(c, 0.6f) : c;
    case CSCD_BORDER_RIDGE: return (topLeft == outerHalf) ? c : darker(c, 0.6f);
    default: return c;
    }
}

static void fillRingColor(CscdCanvas* c, const CscdRRect* outer, const CscdRRect* inner, CscdColor color) {
    CscdShader sh = cscdShaderSolid(color);
    cscdCanvasFillRing(c, outer, inner, &sh);
}

static void strokeColor(CscdCanvas* c, const CscdPath* p, CscdColor color, const CscdStrokeStyle* st) {
    CscdShader sh = cscdShaderSolid(color);
    cscdCanvasStrokePathShader(c, p, &sh, st);
}

static void addLen(void* ctx, const CscdPoint* poly, size_t n, bool closed) {
    (void)closed;
    float* len = (float*)ctx;
    for (size_t i = 0; i < n; ++i) {
        const CscdPoint a = poly[i], c = poly[(i + 1) % n];
        *len += cscdHypotf(c.x - a.x, c.y - a.y);
    }
}

static void setDash(CscdStrokeStyle* ss, float a, float b) {
    ss->dash.len = 0;
    cscdVecPush(&ss->dash, a);
    cscdVecPush(&ss->dash, b);
}

static void paintBorder(Painter* P, CscdLayoutBox* b, CscdRRect outer, const CscdBorderSide sides[4], bool includeLeft, bool includeRight) {
    CscdCanvas* canvas = P->canvas;
    float bw[4];
    CscdBorderSide sd[4];
    for (int i = 0; i < 4; ++i) { sd[i] = sides[i]; bw[i] = cscdBorderVisible(&sd[i]) ? b->border[i] : 0; }
    if (!includeLeft) bw[3] = 0;
    if (!includeRight) bw[1] = 0;
    if (bw[0] == 0 && bw[1] == 0 && bw[2] == 0 && bw[3] == 0) return;
    CscdRRect inner = rrInset(outer, bw[0], bw[1], bw[2], bw[3]);
    bool uniform = true;
    for (int i = 1; i < 4; ++i)
        if (!cscdColorEq(sd[i].color, sd[0].color) || sd[i].style != sd[0].style || (bw[i] > 0) != (bw[0] > 0)) uniform = false;
    if (uniform && sd[0].style == CSCD_BORDER_SOLID) {
        fillRingColor(canvas, &outer, &inner, sd[0].color);
        return;
    }
    bool sameWidth = bw[0] == bw[1] && bw[1] == bw[2] && bw[2] == bw[3];
    if (uniform && sameWidth && (sd[0].style == CSCD_BORDER_DASHED || sd[0].style == CSCD_BORDER_DOTTED) && !cscdRRectIsRect(&outer)) {
        /* one dashed path along the middle of the border, following the corners */
        float w = bw[0];
        CscdRRect mid = rrInset(outer, w / 2, w / 2, w / 2, w / 2);
        CscdPath p = {0};
        cscdPathAddRoundRect(&p, mid.rect, mid.rx, mid.ry);
        float len = 0;
        cscdPathFlatten(&p, cscdMatIdentity(), 0.5f, addLen, &len);
        CscdStrokeStyle ss = cscdStrokeStyle(w);
        if (sd[0].style == CSCD_BORDER_DOTTED) {
            int n = cscdMaxi(1, cscdIntOf(floorf(len / (2 * w))));
            float unit = len / n;
            ss.cap = CSCD_CAP_ROUND;
            setDash(&ss, 0.001f, unit - 0.001f);
        } else {
            int n = cscdMaxi(1, cscdIntOf(roundf(len / (w * 6))));
            float unit = len / n;
            setDash(&ss, unit / 2, unit / 2);
        }
        strokeColor(canvas, &p, sd[0].color, &ss);
        cscdStrokeStyleFini(&ss);
        cscdPathFree(&p);
        return;
    }
    const CscdRect o = outer.rect;
    const CscdRect in = inner.rect;
    for (int side = 0; side < 4; ++side) {
        if (bw[side] <= 0) continue;
        uint8_t st = sd[side].style;
        /* trapezoid owned by this side, split along the corner diagonals */
        CscdPath clip = {0};
        switch (side) {
        case 0:
            cscdPathMoveTo(&clip, o.x, o.y); cscdPathLineTo(&clip, cscdRectRight(o), o.y);
            cscdPathLineTo(&clip, cscdRectRight(in), in.y); cscdPathLineTo(&clip, in.x, in.y);
            break;
        case 1:
            cscdPathMoveTo(&clip, cscdRectRight(o), o.y); cscdPathLineTo(&clip, cscdRectRight(o), cscdRectBottom(o));
            cscdPathLineTo(&clip, cscdRectRight(in), cscdRectBottom(in)); cscdPathLineTo(&clip, cscdRectRight(in), in.y);
            break;
        case 2:
            cscdPathMoveTo(&clip, cscdRectRight(o), cscdRectBottom(o)); cscdPathLineTo(&clip, o.x, cscdRectBottom(o));
            cscdPathLineTo(&clip, in.x, cscdRectBottom(in)); cscdPathLineTo(&clip, cscdRectRight(in), cscdRectBottom(in));
            break;
        default:
            cscdPathMoveTo(&clip, o.x, cscdRectBottom(o)); cscdPathLineTo(&clip, o.x, o.y);
            cscdPathLineTo(&clip, in.x, in.y); cscdPathLineTo(&clip, in.x, cscdRectBottom(in));
            break;
        }
        cscdPathClose(&clip);
        cscdCanvasSave(canvas);
        cscdCanvasClipPath(canvas, &clip, CSCD_FILL_NONZERO);
        cscdPathFree(&clip);
        if (st == CSCD_BORDER_SOLID || st == CSCD_BORDER_INSET || st == CSCD_BORDER_OUTSET) fillRingColor(canvas, &outer, &inner, styleColor(sd, side, true));
        else if (st == CSCD_BORDER_DOUBLE) {
            float t[4] = {bw[0] / 3, bw[1] / 3, bw[2] / 3, bw[3] / 3};
            CscdRRect m1 = rrInset(outer, t[0], t[1], t[2], t[3]);
            CscdRRect m2 = rrInset(outer, t[0] * 2, t[1] * 2, t[2] * 2, t[3] * 2);
            fillRingColor(canvas, &outer, &m1, sd[side].color);
            fillRingColor(canvas, &m2, &inner, sd[side].color);
        } else if (st == CSCD_BORDER_GROOVE || st == CSCD_BORDER_RIDGE) {
            CscdRRect mid = rrInset(outer, bw[0] / 2, bw[1] / 2, bw[2] / 2, bw[3] / 2);
            fillRingColor(canvas, &outer, &mid, styleColor(sd, side, true));
            fillRingColor(canvas, &mid, &inner, styleColor(sd, side, false));
        } else if (st == CSCD_BORDER_DASHED || st == CSCD_BORDER_DOTTED) {
            float w = bw[side];
            /* along the middle of the side */
            CscdPoint a, c;
            switch (side) {
            case 0: a = cscdPoint(o.x, o.y + w / 2); c = cscdPoint(cscdRectRight(o), o.y + w / 2); break;
            case 1: a = cscdPoint(cscdRectRight(o) - w / 2, o.y); c = cscdPoint(cscdRectRight(o) - w / 2, cscdRectBottom(o)); break;
            case 2: a = cscdPoint(cscdRectRight(o), cscdRectBottom(o) - w / 2); c = cscdPoint(o.x, cscdRectBottom(o) - w / 2); break;
            default: a = cscdPoint(o.x + w / 2, cscdRectBottom(o)); c = cscdPoint(o.x + w / 2, o.y); break;
            }
            float len = cscdHypotf(c.x - a.x, c.y - a.y);
            CscdPath p = {0};
            cscdPathMoveTo(&p, a.x, a.y);
            cscdPathLineTo(&p, c.x, c.y);
            CscdStrokeStyle ss = cscdStrokeStyle(w);
            if (st == CSCD_BORDER_DOTTED) {
                ss.cap = CSCD_CAP_ROUND;
                int n = cscdMaxi(1, cscdIntOf(floorf(len / (2 * w))));
                float gap = len / n;
                setDash(&ss, 0.001f, gap - 0.001f);
                ss.dashOffset = -gap / 2;
            } else {
                float dash = cscdMaxf(w * 3, 3.f);
                int n = cscdMaxi(1, cscdIntOf(roundf(len / (dash * 2))));
                float unit = len / n;
                setDash(&ss, unit / 2, unit / 2);
                ss.dashOffset = -unit / 4;
            }
            cscdCanvasSave(canvas);
            cscdCanvasClipRRect(canvas, &outer);
            strokeColor(canvas, &p, sd[side].color, &ss);
            cscdCanvasRestore(canvas);
            cscdStrokeStyleFini(&ss);
            cscdPathFree(&p);
        }
        cscdCanvasRestore(canvas);
    }
}

static void paintOutline(Painter* P, CscdLayoutBox* b, CscdRect borderBox) {
    const CscdComputedStyle* s = b->style;
    if (s->outlineWidth <= 0 || s->outlineStyle == CSCD_BORDER_NONE || s->outlineColor.a == 0) return;
    float o = s->outlineOffset, w = s->outlineWidth;
    CscdRRect inner = borderRRect(b, borderBox);
    inner.rect = cscdRectInset(borderBox, -o, -o, -o, -o);
    for (int i = 0; i < 4; ++i)
        if (inner.rx[i] > 0) { inner.rx[i] = cscdMaxf(0.f, inner.rx[i] + o); inner.ry[i] = cscdMaxf(0.f, inner.ry[i] + o); }
    CscdRRect outer = inner;
    outer.rect = cscdRectInset(inner.rect, -w, -w, -w, -w);
    for (int i = 0; i < 4; ++i)
        if (outer.rx[i] > 0) { outer.rx[i] += w; outer.ry[i] += w; }
    if (s->outlineStyle == CSCD_BORDER_DASHED || s->outlineStyle == CSCD_BORDER_DOTTED) {
        CscdPath p = {0};
        CscdRRect mid = inner;
        mid.rect = cscdRectInset(inner.rect, -w / 2, -w / 2, -w / 2, -w / 2);
        cscdPathAddRoundRect(&p, mid.rect, mid.rx, mid.ry);
        CscdStrokeStyle ss = cscdStrokeStyle(w);
        if (s->outlineStyle == CSCD_BORDER_DOTTED) setDash(&ss, w, w);
        else setDash(&ss, w * 3, w * 2);
        strokeColor(P->canvas, &p, s->outlineColor, &ss);
        cscdStrokeStyleFini(&ss);
        cscdPathFree(&p);
        return;
    }
    fillRingColor(P->canvas, &outer, &inner, s->outlineColor);
}

static void paintDecorations(Painter* P, CscdLayoutBox* b) {
    CscdCanvas* canvas = P->canvas;
    const CscdComputedStyle* s = b->style;
    if (s->visibility != CSCD_VIS_VISIBLE) return;
    if (b->kind == CSCD_BOX_INLINE) {
        for (size_t i = 0; i < b->inlineFrags.len; ++i) {
            const CscdInlineFrag f = b->inlineFrags.items[i];
            CscdRRect rr = borderRRect(b, f.rect);
            if (!f.first) { rr.rx[0] = rr.ry[0] = rr.rx[3] = rr.ry[3] = 0; }
            if (!f.last) { rr.rx[1] = rr.ry[1] = rr.rx[2] = rr.ry[2] = 0; }
            for (size_t k = s->boxShadow.len; k-- > 0;) {
                const CscdShadow* sh = &s->boxShadow.items[k];
                if (!sh->inset) cscdCanvasDrawBoxShadow(canvas, &rr, sh->x, sh->y, sh->blur, sh->spread, sh->color, false);
            }
            /* backgrounds use the full box, not trimmed borders */
            float saveL = b->border[3], saveR = b->border[1], pl = b->padding[3], pr = b->padding[1];
            if (!f.first) { b->border[3] = 0; b->padding[3] = 0; }
            if (!f.last) { b->border[1] = 0; b->padding[1] = 0; }
            paintBackground(P, b, f.rect, rr, true);
            paintBorder(P, b, rr, s->border, f.first, f.last);
            b->border[3] = saveL; b->border[1] = saveR; b->padding[3] = pl; b->padding[1] = pr;
            record(P, b->element, f.rect, false, b, -1);
        }
        return;
    }
    if (b->kind == CSCD_BOX_TEXT) return;
    /* empty-cells: hide: an empty cell of a table with separate borders shows no background or border */
    if (s->emptyCellsHide && s->display == CSCD_DISPLAY_TABLE_CELL && !emptyCellHasContent(b)) {
        if (b->element && (!b->anonymous || b->pseudo.len)) record(P, b->element, cscdBoxBorderBox(b), false, b, -1);
        return;
    }
    CscdRect bb = cscdBoxBorderBox(b);
    CscdRRect outer = borderRRect(b, bb);
    if (cscdBackdropAny(&s->backdrop)) {
        CscdFilter f;
        f.blur = s->backdrop.blur * P->view->dpr;
        f.brightness = s->backdrop.brightness; f.grayscale = s->backdrop.grayscale; f.saturate = s->backdrop.saturate; f.invert = s->backdrop.invert;
        cscdCanvasFilterBackdrop(canvas, &outer, &f);
    }
    for (size_t k = s->boxShadow.len; k-- > 0;) {
        const CscdShadow* sh = &s->boxShadow.items[k];
        if (!sh->inset) cscdCanvasDrawBoxShadow(canvas, &outer, sh->x, sh->y, sh->blur, sh->spread, sh->color, false);
    }
    bool isCanvasRoot = (b->parent == NULL) || (b->element && cscdElementIs(b->element, "body") && P->rootPropagated);
    if (!isCanvasRoot) paintBackground(P, b, bb, outer, true);
    for (size_t k = s->boxShadow.len; k-- > 0;) {
        const CscdShadow* sh = &s->boxShadow.items[k];
        if (sh->inset) {
            CscdRRect in = rrInset(outer, b->border[0], b->border[1], b->border[2], b->border[3]);
            cscdCanvasDrawBoxShadow(canvas, &in, sh->x, sh->y, sh->blur, sh->spread, sh->color, true);
        }
    }
    paintBorder(P, b, outer, s->border, true, true);
    if (b->element && (!b->anonymous || b->pseudo.len)) record(P, b->element, bb, false, b, -1);
}

/* ---- text ------------------------------------------------------------------------------------------ */

/* The characters that stand upright in vertical text (CJK ideographs, kana, hangul, full-width forms). */
static bool uprightInVertical(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3FFFF) || (cp >= 0x1F000 && cp <= 0x1FAFF);
}

/* (punctuation takes no emphasis mark) */
static bool isPunctuation(uint32_t cp) {
    return (cp < 128 && cp > 32 && !((cp | 32) >= 'a' && (cp | 32) <= 'z') && !(cp >= '0' && cp <= '9')) || (cp >= 0x2010 && cp <= 0x205E) ||
           (cp >= 0x3001 && cp <= 0x303F) || (cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40);
}

static CSCD_THREAD_LOCAL CscdShapedGlyphVec shapedTls;

static void outlinesOf(const CscdGlyph* glyphs, size_t n, float dx, float dy, CscdPath* all) {
    CscdPath p = {0};
    for (size_t i = 0; i < n; ++i) {
        const CscdGlyph* g = &glyphs[i];
        cscdPathClear(&p);
        if (!cscdFontFaceOutline(g->font->face, g->id, &p)) continue;
        float sc = g->font->size / g->font->face->unitsPerEm;
        cscdPathAddPath(all, &p, cscdMatrix(sc, 0, g->font->fakeItalic ? -0.2f * sc : 0, -sc, g->x + dx, g->y + dy));
    }
    cscdPathFree(&p);
}

typedef struct DecoLine { CscdCanvas* canvas; const CscdTextFrag* f; const CscdComputedStyle* s; float thick; CscdColor color; } DecoLine;

static void decorationLine(const DecoLine* D, float y) {
    CscdCanvas* canvas = D->canvas;
    const CscdTextFrag* f = D->f;
    const float w = f->width, thick = D->thick;
    const uint8_t style = D->s->decorationStyle;
    if (style == CSCD_DECO_WAVY) {
        CscdPath p = {0};
        float amp = thick * 1.5f, step = thick * 3;
        cscdPathMoveTo(&p, f->x, y);
        for (float x = 0; x < w; x += step * 2) {
            cscdPathQuadTo(&p, f->x + x + step / 2, y - amp, f->x + x + step, y);
            cscdPathQuadTo(&p, f->x + x + step * 1.5f, y + amp, f->x + x + step * 2, y);
        }
        cscdCanvasSave(canvas);
        cscdCanvasClipRect(canvas, cscdRect(f->x, y - amp * 2, w, amp * 4));
        cscdCanvasStrokePath(canvas, &p, D->color, thick);
        cscdCanvasRestore(canvas);
        cscdPathFree(&p);
        return;
    }
    if (style == CSCD_DECO_DOTTED || style == CSCD_DECO_DASHED) {
        CscdPath p = {0};
        cscdPathMoveTo(&p, f->x, y + thick / 2);
        cscdPathLineTo(&p, f->x + w, y + thick / 2);
        CscdStrokeStyle ss = cscdStrokeStyle(thick);
        if (style == CSCD_DECO_DOTTED) setDash(&ss, thick, thick);
        else setDash(&ss, thick * 3, thick * 2);
        strokeColor(canvas, &p, D->color, &ss);
        cscdStrokeStyleFini(&ss);
        cscdPathFree(&p);
        return;
    }
    cscdCanvasFillRect(canvas, cscdRect(f->x, y, w, thick), D->color);
    if (style == CSCD_DECO_DOUBLE) cscdCanvasFillRect(canvas, cscdRect(f->x, y + thick * 2, w, thick), D->color);
}

static void paintTextRun(Painter* P, CscdLayoutBox* tb, const CscdTextFrag* f, const CscdFont* font, const CscdShader* fill) {
    CscdCanvas* canvas = P->canvas;
    const CscdComputedStyle* s = tb->style;
    CscdShapedGlyphVec* shaped = &shapedTls;
    CscdStr visual = {0}, joined = {0};
    CscdSv text = cscdStrSv(&f->text);
    if (f->rtl) cscdFontShapeRtl(text, font, s->letterSpacing, s->wordSpacing, shaped); /* (in reading order, out as it shows) */
    else cscdFontShape(text, font, s->letterSpacing, s->wordSpacing, shaped);
    CscdFont* fonts = cscdMalloc((shaped->len + 1) * sizeof *fonts);
    CscdGlyph* glyphs = cscdMalloc((shaped->len + 1) * sizeof *glyphs);
    size_t ng = 0;
    float extra = 0;
    for (size_t i = 0; i < shaped->len; ++i) {
        const CscdShapedGlyph* g = &shaped->items[i];
        if (g->glyph) {
            fonts[i] = *font;
            fonts[i].face = g->face;
            if (g->scale) fonts[i].size *= g->scale;
            CscdGlyph gl = {g->glyph, f->x + g->x + extra, f->baseline + g->y, &fonts[i]};
            glyphs[ng++] = gl;
        }
        if (f->justify > 0 && g->cluster < text.len && text.ptr[g->cluster] == ' ') extra += f->justify;
    }
    /* shadows (painted from last to first, below the text) */
    for (size_t k = s->textShadow.len; k-- > 0;) {
        const CscdShadow* sh = &s->textShadow.items[k];
        if (sh->color.a == 0) continue;
        if (sh->blur <= 0.3f) {
            CscdGlyph* moved = cscdMalloc((ng + 1) * sizeof *moved);
            memcpy(moved, glyphs, ng * sizeof *moved);
            for (size_t i = 0; i < ng; ++i) { moved[i].x += sh->x; moved[i].y += sh->y; }
            CscdShader solid = cscdShaderSolid(sh->color);
            cscdCanvasDrawGlyphs(canvas, moved, ng, &solid);
            cscdFree(moved);
        } else {
            CscdPath all = {0};
            outlinesOf(glyphs, ng, sh->x, sh->y, &all);
            cscdCanvasDrawBlurredPath(canvas, &all, sh->blur, sh->color);
            cscdPathFree(&all);
        }
    }
    /* vertical writing (text-orientation: mixed): the ideographs, kana and hangul stand upright,
     * each turned back a quarter in its own cell; the rest lie on their side with the line */
    size_t upright = 0;
    if ((s->writingMode == CSCD_WRITING_VERTICAL_RL || s->writingMode == CSCD_WRITING_VERTICAL_LR) && ng) {
        size_t kept = 0;
        for (size_t i = 0, gi = 0; i < shaped->len; ++i) {
            const CscdShapedGlyph* g = &shaped->items[i];
            if (!g->glyph) continue;
            size_t at = g->cluster;
            const uint32_t cp = at < text.len ? cscdUtf8Next(text, &at) : 0;
            const CscdGlyph gl = glyphs[gi++];
            if (!uprightInVertical(cp)) { glyphs[kept++] = gl; continue; }
            const float asc = cscdFontAscent(gl.font), desc = cscdFontDescent(gl.font);
            const float cx = gl.x + g->advance / 2, cy = gl.y - (asc - desc) / 2;
            cscdCanvasSave(canvas);
            cscdCanvasConcat(canvas, cscdMatrix(0, -1, 1, 0, cx - cy, cy + cx)); /* (a quarter turn back about the cell's center) */
            CscdGlyph turned = gl;
            turned.x = cx - g->advance / 2;
            turned.y = cy + (asc - desc) / 2;
            cscdCanvasDrawGlyphs(canvas, &turned, 1, fill);
            cscdCanvasRestore(canvas);
            ++upright;
        }
        ng = kept;
    }
    (void)upright;
    cscdCanvasDrawGlyphs(canvas, glyphs, ng, fill);
    if (s->textStrokeWidth > 0) { /* -webkit-text-stroke: the outlines stroked, centered on the edge */
        CscdPath all = {0};
        outlinesOf(glyphs, ng, 0, 0, &all);
        CscdStrokeStyle st = cscdStrokeStyle(s->textStrokeWidth);
        st.join = CSCD_JOIN_ROUND;
        strokeColor(canvas, &all, s->textStrokeSet ? s->textStroke : s->color, &st);
        cscdStrokeStyleFini(&st);
        cscdPathFree(&all);
    }
    /* decorations */
    if (s->underline || s->overline || s->lineThrough) {
        const CscdFontFace* face = font->face;
        float sc = cscdFontScale(font);
        float thick = s->decorationThickness > 0 ? s->decorationThickness : cscdMaxf(1.f, face ? face->underlineThickness * sc : font->size / 14);
        thick = cscdMaxf(1.f, roundf(thick));
        DecoLine D = {canvas, f, s, thick, s->decorationColor.a ? s->decorationColor : s->color};
        if (s->underline) {
            float off = face ? -face->underlinePos * sc : font->size / 10;
            if (s->underlineUnder) off = cscdFontDescent(font) + thick; /* (below the descenders) */
            decorationLine(&D, roundf(f->baseline + cscdMaxf(1.f, off)));
        }
        if (s->overline) decorationLine(&D, f->baseline - cscdFontAscent(font));
        if (s->lineThrough) decorationLine(&D, f->baseline - (face ? face->strikePos * sc : cscdFontXHeight(font) / 2) - thick / 2);
    }
    /* text-emphasis: a mark centered over (or under) each character that shows, half the size */
    if (s->emphasisShape) {
        static const char* kMarks[][2] = {{"\xE2\x80\xA2", "\xE2\x97\xA6"}, {"\xE2\x97\x8F", "\xE2\x97\x8B"}, {"\xE2\x97\x89", "\xE2\x97\x8E"},
                                          {"\xE2\x96\xB2", "\xE2\x96\xB3"}, {"\xEF\xB9\x85", "\xEF\xB9\x86"}};
        const char* mark = s->emphasisShape == 6 ? s->emphasisString : kMarks[s->emphasisShape - 1][s->emphasisOpen];
        CscdFont mf = *font;
        mf.size = font->size / 2;
        mf.settings = NULL;
        const float mw = cscdFontMeasure(cscdSv(mark), &mf, 0, 0);
        const float y = s->emphasisUnder ? f->baseline + cscdFontDescent(font) + cscdFontAscent(&mf) : f->baseline - cscdFontAscent(font) - cscdFontDescent(&mf);
        const CscdColor mc = s->emphasisColorSet ? s->emphasisColor : s->color;
        for (size_t i = 0; i < shaped->len; ++i) {
            const CscdShapedGlyph* g = &shaped->items[i];
            if (!g->glyph || g->advance <= 0 || g->cluster >= text.len) continue;
            size_t at = g->cluster;
            const uint32_t cp = cscdUtf8Next(text, &at);
            if (cp == ' ' || cp == 0xA0 || cp == 0x3000 || isPunctuation(cp)) continue; /* (not on spaces and punctuation) */
            cscdCanvasDrawText(canvas, cscdSv(mark), f->x + g->x + (g->advance - mw) / 2, y, &mf, mc);
        }
    }
    cscdFree(glyphs);
    cscdFree(fonts);
    cscdStrFree(&visual);
    cscdStrFree(&joined);
}

static void paintText(Painter* P, CscdLayoutBox* tb) {
    CscdCanvas* canvas = P->canvas;
    const CscdComputedStyle* s = tb->style;
    if (s->visibility != CSCD_VIS_VISIBLE) return;
    CscdFont font = tb->font.face ? tb->font : cscdFontForStyle(s);
    CscdShader fill = P->textShader ? *P->textShader : cscdShaderSolid(s->textFillSet ? s->textFill : s->color);
    /* a run that lands outside the clip (a long page's offscreen lines) is not shaped at all: its
     * line box, with room for overhanging glyphs, decorations and shadows, is tested first */
    float pad = font.size;
    for (size_t i = 0; i < s->textShadow.len; ++i) {
        const CscdShadow* sh = &s->textShadow.items[i];
        pad = cscdMaxf(pad, font.size + fabsf(sh->x) + fabsf(sh->y) + 2 * sh->blur);
    }
    const CscdRect clip = cscdCanvasDeviceClipBounds(canvas);
    const CscdMatrix m = cscdCanvasMatrix(canvas);
    const bool dry = cscdCanvasDryRun(canvas);
    for (size_t i = 0; i < tb->textFrags.len; ++i) {
        const CscdTextFrag* f = &tb->textFrags.items[i];
        if (!f->text.len) continue;
        if (!dry) {
            const CscdRect r = {f->x - pad, f->top - pad, f->width + 2 * pad, f->height + 2 * pad};
            const CscdPoint p[4] = {cscdMatMap(m, cscdPoint(r.x, r.y)), cscdMatMap(m, cscdPoint(cscdRectRight(r), r.y)), cscdMatMap(m, cscdPoint(r.x, cscdRectBottom(r))),
                                  cscdMatMap(m, cscdPoint(cscdRectRight(r), cscdRectBottom(r)))};
            float x0 = p[0].x, y0 = p[0].y, x1 = x0, y1 = y0;
            for (int k = 0; k < 4; ++k) { x0 = cscdMinf(x0, p[k].x); y0 = cscdMinf(y0, p[k].y); x1 = cscdMaxf(x1, p[k].x); y1 = cscdMaxf(y1, p[k].y); }
            if (x1 > clip.x && x0 < cscdRectRight(clip) && y1 > clip.y && y0 < cscdRectBottom(clip)) paintTextRun(P, tb, f, &font, &fill);
        }
        record(P, tb->element, cscdRect(f->x, f->top, f->width, f->height), true, tb, (int)i);
    }
}

/* ---- replaced content ------------------------------------------------------------------------------ */

static void paintImage(Painter* P, CscdLayoutBox* b, CscdImage* image) {
    CscdCanvas* canvas = P->canvas;
    CscdRect cb = cscdBoxContentBox(b);
    const CscdComputedStyle* s = b->style;
    float iw = (float)image->width, ih = (float)image->height;
    CscdRect dst = cb;
    switch (s->objectFit) {
    case CSCD_FIT_CONTAIN: case CSCD_FIT_SCALE_DOWN: {
        float sc = cscdMinf(cb.w / iw, cb.h / ih);
        if (s->objectFit == CSCD_FIT_SCALE_DOWN) sc = cscdMinf(sc, 1.f);
        dst.w = iw * sc;
        dst.h = ih * sc;
        break;
    }
    case CSCD_FIT_COVER: { float sc = cscdMaxf(cb.w / iw, cb.h / ih); dst.w = iw * sc; dst.h = ih * sc; break; }
    case CSCD_FIT_NONE: dst.w = iw; dst.h = ih; break;
    default: break;
    }
    dst.x = cb.x + cscdLenResolve(s->objectPosX, cb.w - dst.w, 0);
    dst.y = cb.y + cscdLenResolve(s->objectPosY, cb.h - dst.h, 0);
    cscdCanvasSave(canvas);
    CscdRRect clip = rrInset(borderRRect(b, cscdBoxBorderBox(b)), b->border[0] + b->padding[0], b->border[1] + b->padding[1], b->border[2] + b->padding[2],
                           b->border[3] + b->padding[3]);
    cscdCanvasClipRRect(canvas, &clip);
    cscdCanvasDrawSharedImage(canvas, image, cscdRect(0, 0, iw, ih), dst, !s->pixelated); /* (kept, not copied, by a recording) */
    cscdCanvasRestore(canvas);
}

static float attrF(const CscdElement* e, const char* name, const char* def) {
    const CscdStr* a = cscdElementGetAttribute(e, cscdSv(name));
    char* c = a ? cscdSvDupC(cscdStrSv(a)) : NULL;
    float v = (float)atof(c ? c : def);
    cscdFree(c);
    return v;
}

static void strokeRound(CscdCanvas* canvas, const CscdPath* p, CscdColor color, float width) {
    CscdStrokeStyle ss = cscdStrokeStyle(width);
    ss.cap = CSCD_CAP_ROUND;
    ss.join = CSCD_JOIN_ROUND;
    strokeColor(canvas, p, color, &ss);
    cscdStrokeStyleFini(&ss);
}

static CscdRRect roundRect(CscdRect r, float radius) {
    CscdRRect rr = cscdRRect(r);
    for (int i = 0; i < 4; ++i) rr.rx[i] = rr.ry[i] = radius;
    return rr;
}

static void paintTextControl(Painter* P, CscdLayoutBox* b);

static void paintControl(Painter* P, CscdLayoutBox* b) {
    CscdCanvas* canvas = P->canvas;
    CscdElement* e = b->element;
    const CscdComputedStyle* s = b->style;
    CscdRect cb = cscdBoxContentBox(b);
    CscdColor accent = s->accentColor;
    bool disabled = e && cscdElementHasAttributeC(e, "disabled");
    /* appearance: none: the box, its background and border are the CSS's; nothing native on top */
    if (s->appearanceNone && (b->replaced == CSCD_REPL_CHECKBOX || b->replaced == CSCD_REPL_RADIO || b->replaced == CSCD_REPL_RANGE)) return;
    switch (b->replaced) {
    case CSCD_REPL_CHECKBOX: {
        bool on = e && cscdElementChecked(e);
        CscdRRect rr = roundRect(cb, 2);
        if (on) {
            cscdCanvasFillRRect(canvas, &rr, disabled ? cscdRgb(160, 160, 160) : accent);
            CscdPath p = {0};
            float w = cb.w, h = cb.h;
            cscdPathMoveTo(&p, cb.x + w * 0.22f, cb.y + h * 0.52f);
            cscdPathLineTo(&p, cb.x + w * 0.42f, cb.y + h * 0.72f);
            cscdPathLineTo(&p, cb.x + w * 0.78f, cb.y + h * 0.3f);
            strokeRound(canvas, &p, cscdRgb(255, 255, 255), cscdMaxf(1.5f, w / 7));
            cscdPathFree(&p);
        } else {
            cscdCanvasFillRRect(canvas, &rr, cscdRgb(255, 255, 255));
            CscdRRect in = rrInset(rr, 1, 1, 1, 1);
            fillRingColor(canvas, &rr, &in, disabled ? cscdRgb(190, 190, 190) : cscdRgb(118, 118, 118));
        }
        break;
    }
    case CSCD_REPL_RADIO: {
        bool on = e && cscdElementChecked(e);
        CscdPoint c = {cb.x + cb.w / 2, cb.y + cb.h / 2};
        float r = cscdMinf(cb.w, cb.h) / 2;
        if (on) {
            cscdCanvasFillCircle(canvas, c, r, disabled ? cscdRgb(160, 160, 160) : accent);
            cscdCanvasFillCircle(canvas, c, r - 1.5f, cscdRgb(255, 255, 255));
            cscdCanvasFillCircle(canvas, c, r * 0.5f, disabled ? cscdRgb(160, 160, 160) : accent);
        } else {
            cscdCanvasFillCircle(canvas, c, r, disabled ? cscdRgb(190, 190, 190) : cscdRgb(118, 118, 118));
            cscdCanvasFillCircle(canvas, c, r - 1, cscdRgb(255, 255, 255));
        }
        break;
    }
    case CSCD_REPL_RANGE: {
        float mn = e ? attrF(e, "min", "0") : 0, mx = e ? attrF(e, "max", "100") : 100;
        float val = 50;
        if (e) {
            CscdStr cur = cscdElementValue(e);
            char mid[64];
            snprintf(mid, sizeof mid, "%f", (mn + mx) / 2);
            val = (float)atof(cur.len ? cscdStrC(&cur) : mid);
            cscdStrFree(&cur);
        }
        float t = mx > mn ? cscdClampf((val - mn) / (mx - mn), 0.f, 1.f) : 0;
        float th = 4, r = cscdMinf(8.f, cb.h / 2);
        float y = cb.y + cb.h / 2;
        float x0 = cb.x + r, x1 = cscdRectRight(cb) - r;
        float tx = x0 + (x1 - x0) * t;
        CscdRRect track = roundRect(cscdRect(cb.x, y - th / 2, cb.w, th), th / 2);
        cscdCanvasFillRRect(canvas, &track, cscdRgb(215, 215, 215));
        CscdRRect fillT = track;
        fillT.rect.w = tx - cb.x;
        cscdCanvasFillRRect(canvas, &fillT, accent);
        cscdCanvasFillCircle(canvas, cscdPoint(tx, y), r, accent);
        break;
    }
    case CSCD_REPL_PROGRESS: case CSCD_REPL_METER: {
        float mx = e ? attrF(e, "max", "1") : 1;
        float val = e ? attrF(e, "value", "0") : 0;
        float t = mx > 0 ? cscdClampf(val / mx, 0.f, 1.f) : 0;
        CscdRRect track = roundRect(cb, cscdMinf(cb.h / 2, 4.f));
        cscdCanvasFillRRect(canvas, &track, cscdRgb(230, 230, 230));
        if (t > 0) {
            cscdCanvasSave(canvas);
            cscdCanvasClipRRect(canvas, &track);
            cscdCanvasFillRect(canvas, cscdRect(cb.x, cb.y, cb.w * t, cb.h), b->replaced == CSCD_REPL_METER ? cscdRgb(16, 185, 80) : accent);
            cscdCanvasRestore(canvas);
        }
        break;
    }
    case CSCD_REPL_COLOR_INPUT: {
        CscdColor c = cscdRgb(0, 0, 0);
        if (e) {
            CscdStr cur = cscdElementValue(e);
            cscdParseColorString(cur.len ? cscdStrSv(&cur) : CSCD_SV("#000000"), &c);
            cscdStrFree(&cur);
        }
        cscdCanvasFillRect(canvas, cscdRectInset(cb, 2, 2, 2, 2), c);
        break;
    }
    case CSCD_REPL_SELECT: {
        CscdStr text = {0};
        if (e) {
            CscdStr cur = cscdElementValue(e);
            CscdPtrVec opts = {0};
            cscdElementQuerySelectorAll(e, CSCD_SV("option"), &opts);
            for (size_t i = 0; i < opts.len; ++i) {
                CscdStr ov = cscdElementValue((CscdElement*)opts.items[i]);
                const bool match = cscdStrEq(&ov, &cur);
                cscdStrFree(&ov);
                if (!match) continue;
                CscdStr all = {0};
                cscdNodeTextContent((CscdNode*)opts.items[i], &all);
                text = cscdStrDup(cscdSvTrim(cscdStrSv(&all)));
                cscdStrFree(&all);
                break;
            }
            cscdVecFree(&opts);
            cscdStrFree(&cur);
        }
        CscdFont f = cscdFontForStyle(s);
        float base = cb.y + (cb.h - (cscdFontAscent(&f) + cscdFontDescent(&f))) / 2 + cscdFontAscent(&f);
        cscdCanvasSave(canvas);
        cscdCanvasClipRect(canvas, cb);
        cscdCanvasDrawText(canvas, cscdStrSv(&text), cb.x + 2, base, &f, s->color);
        cscdCanvasRestore(canvas);
        cscdStrFree(&text);
        /* chevron */
        float cx = cscdRectRight(cb) - 8, cy = cb.y + cb.h / 2;
        CscdPath p = {0};
        cscdPathMoveTo(&p, cx - 4, cy - 2);
        cscdPathLineTo(&p, cx, cy + 2);
        cscdPathLineTo(&p, cx + 4, cy - 2);
        strokeRound(canvas, &p, s->color, 1.5f);
        cscdPathFree(&p);
        break;
    }
    case CSCD_REPL_TEXT_INPUT: case CSCD_REPL_TEXT_AREA: paintTextControl(P, b); break;
    case CSCD_REPL_VIDEO: case CSCD_REPL_FRAME: cscdCanvasFillRect(canvas, cb, cscdRgba(0, 0, 0, b->replaced == CSCD_REPL_VIDEO ? 255 : 20)); break;
    default: break;
    }
}

typedef struct TextRuns {
    CscdCanvas* canvas;
    const CscdFont* f;
    float ls;
    const CscdTextSpanVec* spans;
} TextRuns;

/* draws [from, to) of text at x, the colors of the spans over the default color */
static void drawRun(const TextRuns* T, CscdSv text, size_t from, size_t to, float x, float base, CscdColor def) {
    const CscdTextSpanVec* spans = T->spans;
    size_t pos = from;
    size_t si = 0;
    while (si < spans->len && spans->items[si].end <= from) ++si;
    while (pos < to) {
        CscdColor c = def;
        bool bold = false;
        size_t next = to;
        if (si < spans->len && spans->items[si].start <= pos) {
            c = spans->items[si].color;
            bold = spans->items[si].bold;
            next = cscdMinz(to, spans->items[si].end);
            ++si;
        } else if (si < spans->len) next = cscdMinz(to, spans->items[si].start);
        if (next <= pos) next = pos + 1;
        float dx = cscdFontMeasure(cscdSvSub(text, from, pos - from), T->f, T->ls, 0);
        CscdFont rf = *T->f;
        if (bold) rf.fakeBold = true;
        cscdCanvasDrawText(T->canvas, cscdSvSub(text, pos, next - pos), x + dx, base, &rf, c);
        pos = next;
    }
}

/* A right-to-left run of a field: shaped in reading order, each glyph in its span's color. */
static void drawRunRtl(const TextRuns* T, CscdSv text, size_t from, size_t to, float x, float base, CscdColor def) {
    CscdShapedGlyphVec shaped = {0};
    cscdFontShapeRtl(cscdSvSub(text, from, to - from), T->f, T->ls, 0, &shaped);
    for (size_t i = 0; i < shaped.len; ++i) {
        const CscdShapedGlyph* g = &shaped.items[i];
        if (!g->glyph) continue;
        CscdColor c = def;
        CscdFont rf = *T->f;
        rf.face = g->face;
        if (g->scale) rf.size *= g->scale;
        for (size_t k = 0; k < T->spans->len; ++k) {
            const CscdTextSpan* sp = &T->spans->items[k];
            if (from + g->cluster >= sp->start && from + g->cluster < sp->end) { c = sp->color; rf.fakeBold = sp->bold; break; }
        }
        const CscdGlyph gl = {g->glyph, x + g->x, base + g->y, &rf};
        const CscdShader sh = cscdShaderSolid(c);
        cscdCanvasDrawGlyphs(T->canvas, &gl, 1, &sh);
    }
    cscdVecFree(&shaped);
}

/* A line of a field, run by run. */
static void drawRuns(const TextRuns* T, const EditRunVec* runs, CscdSv text, float x, float base, CscdColor def) {
    for (size_t i = 0; i < runs->len; ++i) {
        const EditRun* r = &runs->items[i];
        if (r->rtl) drawRunRtl(T, text, r->start, r->end, x + r->x, base, def);
        else drawRun(T, text, r->start, r->end, x + r->x, base, def);
    }
}

/* positions in the shown text: a password shows three bytes per character */
static size_t shownPos(bool password, CscdSv value, size_t pos) {
    if (!password) return pos;
    size_t n = 0;
    for (size_t i = 0; i < pos && i < value.len;) {
        cscdUtf8Next(value, &i);
        n += 3;
    }
    return n;
}

static void paintTextControl(Painter* P, CscdLayoutBox* b) {
    CscdCanvas* canvas = P->canvas;
    CscdView* v = P->view;
    CscdElement* e = b->element;
    const CscdComputedStyle* s = b->style;
    CscdRect cb = cscdBoxContentBox(b);
    CscdFont f = cscdFontForStyle(s);
    float ls = s->letterSpacing;
    CscdStr value = e ? cscdElementValue(e) : cscdStrDupC("");
    bool password = e && cscdSvIEqC(cscdElementAttrC(e, "type"), "password");
    bool focused = e && v->doc->focused == e;
    CscdColor color = s->color;
    CscdColor selColor = cscdRgba(51, 144, 255, 110);
    /* ::selection { background-color } (and its color is the text's) */
    if (focused && e) {
        const CscdComputedStyle* sel = cscdStyleEnginePseudoStyle(v->styles, e, CSCD_SV("selection"));
        if (sel && sel->backgroundColor.a) selColor = sel->backgroundColor;
    }
    bool placeholder = !value.len && e && cscdElementHasAttributeC(e, "placeholder") && !(focused && v->composition.len);
    const CscdComputedStyle* ps = placeholder ? cscdStyleEnginePseudoStyle(v->styles, e, CSCD_SV("placeholder")) : NULL;
    /* an input method's composition is shown at the caret, underlined */
    size_t caret = e ? cscdMinz(e->caret, value.len) : 0;
    size_t compA = CSCD_NPOS, compZ = 0;
    bool compInserted = false;
    CscdStr shownValue = cscdStrCopy(&value);
    if (focused && v->composition.len) {
        cscdStrInsert(&shownValue, caret, cscdStrSv(&v->composition));
        compA = caret;
        compZ = caret + v->composition.len;
        caret = compZ;
        compInserted = true;
    } else if (focused && v->markEnd > v->markStart && v->markEnd <= value.len) {
        /* (a phone's input method composes in the text itself: cscdViewSetTextState) */
        compA = v->markStart;
        compZ = v->markEnd;
    }
    size_t selA = e ? cscdMinz(cscdMinz(e->selAnchor, e->caret), value.len) : 0;
    size_t selZ = e ? cscdMinz(cscdMaxz(e->selAnchor, e->caret), value.len) : 0;
    if (compA != CSCD_NPOS) selA = selZ = 0;
    /* highlighted spans (shifted past a composition) */
    CscdTextSpanVec spans = {0};
    if (e && cscdElementHasHighlighter(e) && !password && !placeholder) {
        cscdVecCopy(&spans, cscdElementHighlights(e, cscdStrSv(&value)));
        if (compInserted)
            for (size_t i = 0; i < spans.len; ++i) {
                CscdTextSpan* sp = &spans.items[i];
                if (sp->start >= compA) sp->start += compZ - compA;
                if (sp->end > compA) sp->end += compZ - compA;
            }
    }
    TextRuns T = {canvas, &f, ls, &spans};
    cscdCanvasSave(canvas);
    cscdCanvasClipRect(canvas, cscdBoxPaddingBox(b));
    float textH = cscdFontAscent(&f) + cscdFontDescent(&f);
    const CscdColor caretColor = s->caretColor.a ? s->caretColor : color;
    if (b->replaced == CSCD_REPL_TEXT_INPUT) {
        float base = cb.y + (cb.h - textH) / 2 + cscdFontAscent(&f);
        CscdStr shown = cscdStrCopy(&shownValue);
        if (password) {
            cscdStrClear(&shown);
            for (size_t i = 0; i < shownValue.len;) {
                cscdUtf8Next(cscdStrSv(&shownValue), &i);
                cscdStrAppendC(&shown, "\xE2\x80\xA2");
            }
        }
        const CscdSv sv = cscdStrSv(&shown), svv = cscdStrSv(&shownValue);
        /* the text by its runs of one direction (Arabic or Hebrew among Latin, a right-to-left field) */
        EditRunVec runs = {0};
        CscdFloatVec spanX = {0};
        const EditLine all = {0, sv.len};
        editRuns(sv, all, &f, ls, s->rtl, &runs);
        float caretX = editCaretX(&runs, sv, &f, ls, shownPos(password, svv, caret));
        float textW = editRunsWidth(&runs);
        float scroll = 0;
        if (focused && caretX > cb.w - 2) scroll = caretX - cb.w + 2;
        float x0 = cb.x - scroll + editAlignShift(s, textW, cb.w);
        if (placeholder) {
            CscdColor pc = ps ? ps->color : cscdRgb(117, 117, 117);
            CscdFont pf = ps ? cscdFontForStyle(ps) : f;
            const CscdSv ph = cscdElementAttrC(e, "placeholder");
            EditRunVec pr = {0};
            const EditLine pl = {0, ph.len};
            editRuns(ph, pl, &pf, 0, s->rtl, &pr);
            const CscdTextSpanVec none = {0};
            const TextRuns PT = {canvas, &pf, 0, &none};
            drawRuns(&PT, &pr, ph, cb.x + editAlignShift(s, editRunsWidth(&pr), cb.w), base, pc);
            cscdVecFree(&pr);
        } else {
            if (focused && selA != selZ) {
                editRunsSpan(&runs, sv, &f, ls, shownPos(password, svv, selA), shownPos(password, svv, selZ), &spanX);
                for (size_t k = 0; k + 1 < spanX.len; k += 2)
                    cscdCanvasFillRect(canvas, cscdRect(x0 + spanX.items[k], base - cscdFontAscent(&f), spanX.items[k + 1] - spanX.items[k], textH), selColor);
            }
            drawRuns(&T, &runs, sv, x0, base, color);
            if (compA != CSCD_NPOS) {
                editRunsSpan(&runs, sv, &f, ls, shownPos(password, svv, compA), shownPos(password, svv, compZ), &spanX);
                for (size_t k = 0; k + 1 < spanX.len; k += 2)
                    cscdCanvasFillRect(canvas, cscdRect(x0 + spanX.items[k], base + 2, spanX.items[k + 1] - spanX.items[k], 1), color);
            }
        }
        cscdVecFree(&runs);
        cscdVecFree(&spanX);
        if (focused && cscdViewCaretVisible(v)) cscdCanvasFillRect(canvas, cscdRect(roundf(x0 + caretX), base - cscdFontAscent(&f), 1, textH), caretColor);
        cscdStrFree(&shown);
    } else {
        /* textarea: lines wrapped at spaces (unless wrap="off"), scrolled by scrollTop / scrollLeft */
        bool wrap = editWraps(e);
        CscdStr text = placeholder ? cscdStrDup(cscdElementAttrC(e, "placeholder")) : cscdStrCopy(&shownValue);
        CscdColor tc = placeholder ? (ps ? ps->color : cscdRgb(117, 117, 117)) : color;
        EditLineVec lines = {0};
        editLines(cscdStrSv(&text), &f, cb.w, wrap, ls, &lines);
        float lineH = editLineHeight(s, &f);
        float sx = wrap ? 0 : e->scrollLeft, sy = e->scrollTop;
        float x0 = cb.x - sx, y0 = cb.y - sy;
        /* line numbers in the left padding (<textarea line-numbers>) */
        bool numbers = cscdElementHasAttributeC(e, "line-numbers");
        size_t logical = 1;
        size_t first = (size_t)cscdMaxf(0.f, floorf(sy / lineH)), last = cscdMinz(lines.len, (size_t)ceilf((sy + cb.h) / lineH) + 1);
        if (numbers)
            for (size_t i = 0; i < first && i < lines.len; ++i)
                if (i + 1 < lines.len && lines.items[i + 1].start > lines.items[i].end) ++logical; /* a line break follows */
        const CscdSv sv = cscdStrSv(&text);
        EditRunVec runs = {0};
        CscdFloatVec spanX = {0};
        for (size_t i = first; i < last; ++i) {
            const EditLine ln = lines.items[i];
            editRuns(sv, ln, &f, ls, s->rtl, &runs);
            const float lx = x0 + (wrap ? editAlignShift(s, editRunsWidth(&runs), cb.w) : 0);
            float top = y0 + i * lineH;
            float base = top + (lineH - textH) / 2 + cscdFontAscent(&f);
            bool startsLogical = i == 0 || lines.items[i - 1].end < ln.start;
            if (numbers && startsLogical && !placeholder) {
                cscdCanvasSave(canvas);
                cscdCanvasClipRect(canvas, cscdBoxBorderBox(b));
                char n[32];
                snprintf(n, sizeof n, "%zu", logical);
                float nw = cscdFontMeasure(cscdSv(n), &f, 0, 0);
                bool current = focused && editLineOf(&lines, caret) == i;
                cscdCanvasDrawText(canvas, cscdSv(n), cb.x - 12 - nw, base, &f, current ? color : cscdColorWithAlpha(color, 0.38f));
                cscdCanvasRestore(canvas);
            }
            if (numbers && i + 1 < lines.len && lines.items[i + 1].start > ln.end) ++logical;
            if (!placeholder && focused && selA != selZ && selZ >= ln.start && selA <= ln.end + 1) {
                size_t a = cscdMaxz(selA, ln.start), z = cscdMinz(selZ, ln.end);
                editRunsSpan(&runs, sv, &f, ls, a, z, &spanX);
                if (selZ > ln.end && ln.end < sv.len && sv.ptr[ln.end] == '\n') { /* the line break is selected too */
                    const float end = editRunsWidth(&runs);
                    cscdVecPush(&spanX, s->rtl ? -f.size * 0.3f : end);
                    cscdVecPush(&spanX, s->rtl ? 0 : end + f.size * 0.3f);
                }
                for (size_t k = 0; k + 1 < spanX.len; k += 2)
                    if (spanX.items[k + 1] > spanX.items[k]) cscdCanvasFillRect(canvas, cscdRect(lx + spanX.items[k], top, spanX.items[k + 1] - spanX.items[k], lineH), selColor);
            }
            drawRuns(&T, &runs, sv, lx, base, tc);
            if (compA != CSCD_NPOS && compZ > ln.start && compA <= ln.end) {
                editRunsSpan(&runs, sv, &f, ls, cscdMaxz(compA, ln.start), cscdMinz(compZ, ln.end), &spanX);
                for (size_t k = 0; k + 1 < spanX.len; k += 2)
                    cscdCanvasFillRect(canvas, cscdRect(lx + spanX.items[k], base + 2, spanX.items[k + 1] - spanX.items[k], 1), color);
            }
        }
        if (focused && cscdViewCaretVisible(v) && lines.len) {
            size_t i = placeholder ? 0 : editLineOf(&lines, caret);
            editRuns(sv, lines.items[i], &f, ls, s->rtl, &runs);
            const float lx = x0 + (wrap ? editAlignShift(s, placeholder ? 0 : editRunsWidth(&runs), cb.w) : 0);
            float cx = placeholder ? 0 : editCaretX(&runs, sv, &f, ls, caret);
            float top = y0 + i * lineH;
            cscdCanvasFillRect(canvas, cscdRect(roundf(lx + cx), top + (lineH - textH) / 2, 1, textH), caretColor);
        }
        cscdVecFree(&runs);
        cscdVecFree(&spanX);
        /* a scroll bar when the text is taller than the field */
        float total = lines.len * lineH;
        if (total > cb.h + 0.5f && !placeholder) {
            CscdRect pb = cscdBoxPaddingBox(b);
            float trackH = pb.h - 4;
            float th = cscdMaxf(20.f, trackH * pb.h / (total + pb.h - cb.h));
            float maxY = total - cb.h;
            float ty = pb.y + 2 + (trackH - th) * cscdClampf(sy / maxY, 0.f, 1.f);
            CscdRRect r = roundRect(cscdRect(cscdRectRight(pb) - 7, ty, 5, th), 2.5f);
            cscdCanvasFillRRect(canvas, &r, s->scrollbarThumbSet ? s->scrollbarThumb : cscdColorWithAlpha(color, 0.3f));
        }
        cscdVecFree(&lines);
        cscdStrFree(&text);
    }
    cscdCanvasRestore(canvas);
    cscdVecFree(&spans);
    cscdStrFree(&shownValue);
    cscdStrFree(&value);
}

/* ---- inline SVG ------------------------------------------------------------------------------------ */

static void numbers(CscdSv s, CscdFloatVec* out) {
    out->len = 0;
    size_t i = 0;
    while (i < s.len) {
        while (i < s.len && (cscdIsSpace(s.ptr[i]) || s.ptr[i] == ',')) ++i;
        if (i >= s.len) break;
        char rest[65];
        size_t n = cscdMinz(64, s.len - i);
        memcpy(rest, s.ptr + i, n);
        rest[n] = 0;
        char* end = NULL;
        float v = strtof(rest, &end);
        if (end == rest) { ++i; continue; }
        cscdVecPush(out, v);
        i += (size_t)(end - rest);
    }
}

static CscdMatrix svgTransform(CscdSv t) {
    CscdMatrix m = cscdMatIdentity();
    CscdFloatVec n = {0};
    size_t i = 0;
    while (i < t.len) {
        size_t p = cscdSvFindChar(t, '(', i);
        if (p == CSCD_NPOS) break;
        CscdStr name = cscdLower(cscdSvTrim(cscdSvSub(t, i, p - i)));
        size_t q = cscdSvFindChar(t, ')', p);
        if (q == CSCD_NPOS) {
            cscdStrFree(&name);
            break;
        }
        numbers(cscdSvSub(t, p + 1, q - p - 1), &n);
        CscdMatrix op = cscdMatIdentity();
        const float* a = n.items;
        if (cscdStrEqC(&name, "translate") && n.len) op = cscdMatTranslate(a[0], n.len > 1 ? a[1] : 0);
        else if (cscdStrEqC(&name, "scale") && n.len) op = cscdMatScale(a[0], n.len > 1 ? a[1] : a[0]);
        else if (cscdStrEqC(&name, "rotate") && n.len) {
            float ang = a[0] * PAINT_PI / 180.f;
            if (n.len >= 3) op = cscdMatMul(cscdMatMul(cscdMatTranslate(a[1], a[2]), cscdMatRotate(ang)), cscdMatTranslate(-a[1], -a[2]));
            else op = cscdMatRotate(ang);
        } else if (cscdStrEqC(&name, "matrix") && n.len >= 6) op = cscdMatrix(a[0], a[1], a[2], a[3], a[4], a[5]);
        else if (cscdStrEqC(&name, "skewx") && n.len) op = cscdMatSkew(a[0] * PAINT_PI / 180.f, 0);
        else if (cscdStrEqC(&name, "skewy") && n.len) op = cscdMatSkew(0, a[0] * PAINT_PI / 180.f);
        m = cscdMatMul(m, op);
        cscdStrFree(&name);
        i = q + 1;
        while (i < t.len && (cscdIsSpace(t.ptr[i]) || t.ptr[i] == ',')) ++i;
    }
    cscdVecFree(&n);
    return m;
}

/* the attribute, or else the style property, on e or an ancestor up to stop ("" when none) */
static CscdStr inheritedAttr(CscdElement* e, const char* name, const CscdElement* stop) {
    for (CscdElement* a = e; a; a = parentEl(a)) {
        const CscdStr* v = cscdElementGetAttribute(a, cscdSv(name));
        if (v) return cscdStrCopy(v);
        CscdStr st = cscdElementGetStyle(a, cscdSv(name));
        if (st.len) return st;
        cscdStrFree(&st);
        if (a == stop) break;
    }
    CscdStr none = {0};
    return none;
}

/* the attribute, or def when e has none */
static CscdSv attrOr(const CscdElement* e, const char* name, CscdSv def) {
    const CscdStr* v = cscdElementGetAttribute(e, cscdSv(name));
    return v ? cscdStrSv(v) : def;
}

static float svgNum(const CscdElement* e, const char* name, float def) {
    CscdSv v = cscdElementAttrC(e, name);
    if (!v.len) return def;
    char* c = cscdSvDupC(v);
    float f = (float)atof(c);
    cscdFree(c);
    return f;
}

static float svToFloat(CscdSv v) {
    char* c = cscdSvDupC(v);
    float f = (float)atof(c);
    cscdFree(c);
    return f;
}

/* paint servers and properties (attributes first, then CSS) */
static bool colorFrom(Painter* P, const CscdComputedStyle* s, CscdSv val, CscdColor* out, bool* none) {
    *none = false;
    if (!val.len) return false;
    if (cscdSvEqC(val, "none")) { *none = true; return true; }
    if (cscdSvIEqC(val, "currentcolor")) { *out = s->color; return true; }
    if (cscdSvStartsC(val, "url(")) {
        /* gradients by reference: use the first stop color */
        size_t h = cscdSvFindChar(val, '#', 0);
        size_t end = cscdSvFindChar(val, ')', 0);
        if (h != CSCD_NPOS && end != CSCD_NPOS) {
            CscdElement* g = cscdDocumentGetElementById(P->view->doc, cscdSvSub(val, h + 1, end - h - 1));
            if (g) {
                CscdPtrVec stops = {0};
                cscdElementQuerySelectorAll(g, CSCD_SV("stop"), &stops);
                bool found = false;
                for (size_t i = 0; i < stops.len && !found; ++i) {
                    CscdElement* st = (CscdElement*)stops.items[i];
                    CscdStr sc = cscdElementGetAttribute(st, CSCD_SV("stop-color")) ? cscdStrDup(cscdElementAttrC(st, "stop-color"))
                                                                               : cscdElementGetStyle(st, CSCD_SV("stop-color"));
                    if (sc.len && cscdParseColorString(cscdStrSv(&sc), out)) found = true;
                    cscdStrFree(&sc);
                }
                cscdVecFree(&stops);
                if (found) return true;
            }
        }
        *out = cscdRgb(0, 0, 0);
        return true;
    }
    return cscdParseColorString(val, out);
}

static void paintSvgElement(Painter* P, CscdElement* e, CscdElement* svgRoot, float opacityIn) {
    CscdCanvas* canvas = P->canvas;
    if (!e->style || e->style->display == CSCD_DISPLAY_NONE) return;
    const CscdComputedStyle* s = e->style;
    static const char* skipped[] = {"defs", "title", "desc", "metadata", "symbol", "clippath", "mask", "lineargradient", "radialgradient", "style",
                                    "pattern", "filter", "marker"};
    for (size_t i = 0; i < CSCD_COUNT(skipped); ++i)
        if (cscdElementIs(e, skipped[i])) return;
    cscdCanvasSave(canvas);
    if (cscdElementHasAttributeC(e, "transform")) cscdCanvasConcat(canvas, svgTransform(cscdElementAttrC(e, "transform")));
    float opacity = opacityIn * (cscdElementHasAttributeC(e, "opacity") ? svToFloat(cscdElementAttrC(e, "opacity")) : 1) * s->opacity;
    if (cscdElementIs(e, "svg") && e != svgRoot) cscdCanvasTranslate(canvas, svToFloat(attrOr(e, "x", CSCD_SV("0"))), svToFloat(attrOr(e, "y", CSCD_SV("0"))));
    if (cscdElementIs(e, "g") || cscdElementIs(e, "svg") || cscdElementIs(e, "a") || cscdElementIs(e, "switch")) {
        for (size_t i = 0; i < e->node.children.len; ++i) {
            CscdElement* ce = cscdNodeAsElement(cscdNodeChild(&e->node, i));
            if (ce) paintSvgElement(P, ce, svgRoot, opacity);
        }
        cscdCanvasRestore(canvas);
        return;
    }
    if (cscdElementIs(e, "use")) {
        CscdSv href = attrOr(e, "href", cscdElementAttrC(e, "xlink:href"));
        if (cscdSvStartsC(href, "#")) {
            CscdElement* ref = cscdDocumentGetElementById(P->view->doc, cscdSvFrom(href, 1));
            if (ref && ref != e) {
                cscdCanvasTranslate(canvas, svToFloat(attrOr(e, "x", CSCD_SV("0"))), svToFloat(attrOr(e, "y", CSCD_SV("0"))));
                if (cscdElementIs(ref, "symbol")) {
                    for (size_t i = 0; i < ref->node.children.len; ++i) {
                        CscdElement* ce = cscdNodeAsElement(cscdNodeChild(&ref->node, i));
                        if (ce) paintSvgElement(P, ce, svgRoot, opacity);
                    }
                } else paintSvgElement(P, ref, svgRoot, opacity);
            }
        }
        cscdCanvasRestore(canvas);
        return;
    }
    CscdPath p = {0};
    if (cscdElementIs(e, "path")) cscdParseSvgPath(cscdElementAttrC(e, "d"), &p);
    else if (cscdElementIs(e, "rect")) {
        float x = svgNum(e, "x", 0), y = svgNum(e, "y", 0), w = svgNum(e, "width", 0), h = svgNum(e, "height", 0);
        float rx = svgNum(e, "rx", -1), ry = svgNum(e, "ry", -1);
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
        if (rx < 0) rx = ry = 0;
        float rxs[4] = {rx, rx, rx, rx}, rys[4] = {ry, ry, ry, ry};
        if (rx > 0) cscdPathAddRoundRect(&p, cscdRect(x, y, w, h), rxs, rys);
        else cscdPathAddRect(&p, cscdRect(x, y, w, h));
    } else if (cscdElementIs(e, "circle")) cscdPathAddCircle(&p, svgNum(e, "cx", 0), svgNum(e, "cy", 0), svgNum(e, "r", 0));
    else if (cscdElementIs(e, "ellipse")) cscdPathAddEllipse(&p, svgNum(e, "cx", 0), svgNum(e, "cy", 0), svgNum(e, "rx", 0), svgNum(e, "ry", 0));
    else if (cscdElementIs(e, "line")) {
        cscdPathMoveTo(&p, svgNum(e, "x1", 0), svgNum(e, "y1", 0));
        cscdPathLineTo(&p, svgNum(e, "x2", 0), svgNum(e, "y2", 0));
    } else if (cscdElementIs(e, "polyline") || cscdElementIs(e, "polygon")) {
        CscdFloatVec n = {0};
        numbers(cscdElementAttrC(e, "points"), &n);
        for (size_t i = 0; i + 1 < n.len; i += 2) {
            if (i == 0) cscdPathMoveTo(&p, n.items[i], n.items[i + 1]);
            else cscdPathLineTo(&p, n.items[i], n.items[i + 1]);
        }
        cscdVecFree(&n);
        if (cscdElementIs(e, "polygon")) cscdPathClose(&p);
    } else if (cscdElementIs(e, "text")) {
        CscdFont f = cscdFontForStyle(s);
        CscdColor c = s->fillNone ? cscdRgba(0, 0, 0, 0) : s->fill;
        CscdStr fillA = inheritedAttr(e, "fill", svgRoot);
        if (fillA.len && !cscdStrEqC(&fillA, "none") && !cscdSvIEqC(cscdStrSv(&fillA), "currentcolor")) cscdParseColorString(cscdStrSv(&fillA), &c);
        cscdStrFree(&fillA);
        float x = svgNum(e, "x", 0), y = svgNum(e, "y", 0);
        CscdStr anchor = inheritedAttr(e, "text-anchor", svgRoot);
        CscdStr all = {0};
        cscdNodeTextContent(&e->node, &all);
        CscdSv txt = cscdSvTrim(cscdStrSv(&all));
        float w = cscdFontMeasure(txt, &f, 0, 0);
        if (cscdStrEqC(&anchor, "middle")) x -= w / 2;
        else if (cscdStrEqC(&anchor, "end")) x -= w;
        cscdCanvasDrawText(canvas, txt, x, y, &f, cscdColorWithAlpha(c, opacity));
        cscdStrFree(&all);
        cscdStrFree(&anchor);
        cscdCanvasRestore(canvas);
        cscdPathFree(&p);
        return;
    }
    CscdColor fill = s->fill;
    bool fillNone = s->fillNone;
    if (s->fillCurrent) fill = s->color;
    CscdStr fa = cscdElementGetStyle(e, CSCD_SV("fill"));
    if (!fa.len) { cscdStrFree(&fa); fa = inheritedAttr(e, "fill", svgRoot); }
    if (fa.len) colorFrom(P, s, cscdStrSv(&fa), &fill, &fillNone);
    cscdStrFree(&fa);
    CscdColor stroke = s->stroke;
    bool strokeNone = s->strokeNone;
    if (s->strokeCurrent) stroke = s->color;
    CscdStr sa = cscdElementGetStyle(e, CSCD_SV("stroke"));
    if (!sa.len) { cscdStrFree(&sa); sa = inheritedAttr(e, "stroke", svgRoot); }
    if (sa.len) colorFrom(P, s, cscdStrSv(&sa), &stroke, &strokeNone);
    cscdStrFree(&sa);
    float sw = s->strokeWidth;
    CscdStr swa = inheritedAttr(e, "stroke-width", svgRoot);
    if (swa.len) sw = svToFloat(cscdStrSv(&swa));
    cscdStrFree(&swa);
    float fo = 1, so = 1;
    CscdStr foa = inheritedAttr(e, "fill-opacity", svgRoot), soa = inheritedAttr(e, "stroke-opacity", svgRoot);
    if (foa.len) fo = svToFloat(cscdStrSv(&foa));
    if (soa.len) so = svToFloat(cscdStrSv(&soa));
    cscdStrFree(&foa);
    cscdStrFree(&soa);
    CscdStr fr = inheritedAttr(e, "fill-rule", svgRoot);
    CscdFillRule rule = cscdStrEqC(&fr, "evenodd") ? CSCD_FILL_EVENODD : CSCD_FILL_NONZERO;
    cscdStrFree(&fr);
    if (cscdElementIs(e, "line")) fillNone = true;
    if (!fillNone && fill.a) cscdCanvasFillPath(canvas, &p, cscdColorWithAlpha(fill, opacity * fo), rule);
    if (!strokeNone && stroke.a && sw > 0) {
        CscdStrokeStyle st = cscdStrokeStyle(sw);
        CscdStr cap = inheritedAttr(e, "stroke-linecap", svgRoot), join = inheritedAttr(e, "stroke-linejoin", svgRoot);
        st.cap = cscdStrEqC(&cap, "round") ? CSCD_CAP_ROUND : cscdStrEqC(&cap, "square") ? CSCD_CAP_SQUARE : CSCD_CAP_BUTT;
        st.join = cscdStrEqC(&join, "round") ? CSCD_JOIN_ROUND : cscdStrEqC(&join, "bevel") ? CSCD_JOIN_BEVEL : CSCD_JOIN_MITER;
        cscdStrFree(&cap);
        cscdStrFree(&join);
        CscdStr dash = inheritedAttr(e, "stroke-dasharray", svgRoot);
        if (dash.len && !cscdStrEqC(&dash, "none")) numbers(cscdStrSv(&dash), &st.dash);
        cscdStrFree(&dash);
        CscdStr ml = inheritedAttr(e, "stroke-miterlimit", svgRoot);
        if (ml.len) st.miterLimit = svToFloat(cscdStrSv(&ml));
        cscdStrFree(&ml);
        strokeColor(canvas, &p, cscdColorWithAlpha(stroke, opacity * so), &st);
        cscdStrokeStyleFini(&st);
    }
    cscdPathFree(&p);
    cscdCanvasRestore(canvas);
}

static void paintSvg(Painter* P, CscdLayoutBox* b) {
    CscdCanvas* canvas = P->canvas;
    CscdElement* svg = b->element;
    if (!svg) return;
    CscdRect cb = cscdBoxContentBox(b);
    cscdCanvasSave(canvas);
    cscdCanvasClipRect(canvas, cb);
    cscdCanvasTranslate(canvas, cb.x, cb.y);
    if (cscdElementHasAttributeC(svg, "viewbox")) {
        CscdFloatVec n = {0};
        numbers(cscdElementAttrC(svg, "viewbox"), &n);
        if (n.len >= 4 && n.items[2] > 0 && n.items[3] > 0) {
            float sx = cb.w / n.items[2], sy = cb.h / n.items[3];
            CscdSv par = attrOr(svg, "preserveaspectratio", CSCD_SV("xMidYMid meet"));
            if (cscdSvFind(par, CSCD_SV("none"), 0) == CSCD_NPOS) {
                bool slice = cscdSvFind(par, CSCD_SV("slice"), 0) != CSCD_NPOS;
                float sc = slice ? cscdMaxf(sx, sy) : cscdMinf(sx, sy);
                float dx = 0, dy = 0;
                if (cscdSvFind(par, CSCD_SV("xMid"), 0) != CSCD_NPOS) dx = (cb.w - n.items[2] * sc) / 2;
                else if (cscdSvFind(par, CSCD_SV("xMax"), 0) != CSCD_NPOS) dx = cb.w - n.items[2] * sc;
                if (cscdSvFind(par, CSCD_SV("YMid"), 0) != CSCD_NPOS) dy = (cb.h - n.items[3] * sc) / 2;
                else if (cscdSvFind(par, CSCD_SV("YMax"), 0) != CSCD_NPOS) dy = cb.h - n.items[3] * sc;
                cscdCanvasTranslate(canvas, dx, dy);
                sx = sy = sc;
            }
            cscdCanvasScale(canvas, sx, sy);
            cscdCanvasTranslate(canvas, -n.items[0], -n.items[1]);
        }
        cscdVecFree(&n);
    }
    for (size_t i = 0; i < svg->node.children.len; ++i) {
        CscdElement* ce = cscdNodeAsElement(cscdNodeChild(&svg->node, i));
        if (ce) paintSvgElement(P, ce, svg, 1);
    }
    cscdCanvasRestore(canvas);
}

static void paintReplaced(Painter* P, CscdLayoutBox* b) {
    CscdCanvas* canvas = P->canvas;
    if (b->style->visibility != CSCD_VIS_VISIBLE) return;
    switch (b->replaced) {
    case CSCD_REPL_IMAGE:
        if (b->image && !cscdImageEmpty(b->image)) paintImage(P, b, b->image);
        else if (b->element && cscdElementHasAttributeC(b->element, "alt") && b->w > 0 && b->h > 0) {
            CscdFont f = cscdFontForStyle(b->style);
            cscdCanvasSave(canvas);
            cscdCanvasClipRect(canvas, cscdBoxContentBox(b));
            cscdCanvasDrawText(canvas, cscdElementAttrC(b->element, "alt"), cscdBoxContentX(b), cscdBoxContentY(b) + cscdFontAscent(&f), &f, b->style->color);
            cscdCanvasRestore(canvas);
        }
        break;
    case CSCD_REPL_SVG: paintSvg(P, b); break;
    case CSCD_REPL_CANVAS: break; /* painted through the element's painter */
    default: paintControl(P, b); break;
    }
}

/* ---- scrollbars ------------------------------------------------------------------------------------ */

static void paintScrollbars(Painter* P, CscdLayoutBox* b) {
    if (!b->element) return;
    const CscdComputedStyle* s = b->style;
    CscdElement* e = b->element;
    CscdRect pb = cscdBoxPaddingBox(b);
    float maxY = cscdMaxf(0.f, b->overflow.h - pb.h), maxX = cscdMaxf(0.f, b->overflow.w - pb.w);
    bool showY = (s->overflowY == CSCD_OVERFLOW_SCROLL) || (s->overflowY == CSCD_OVERFLOW_AUTO && maxY > 0.5f);
    bool showX = (s->overflowX == CSCD_OVERFLOW_SCROLL) || (s->overflowX == CSCD_OVERFLOW_AUTO && maxX > 0.5f);
    CscdColor thumb = s->scrollbarThumbSet ? s->scrollbarThumb : cscdRgba(0, 0, 0, 90);
    if (s->scrollbarWidth == 2) return; /* (scrollbar-width: none: it scrolls, without bars) */
    const float bw = s->scrollbarWidth == 1 ? 4 : 6, inset = bw + 2;
    if (showY && maxY > 0) {
        float trackH = pb.h - 4;
        float th = cscdMaxf(20.f, trackH * pb.h / (pb.h + maxY));
        float ty = pb.y + 2 + (trackH - th) * (e->scrollTop / maxY);
        CscdRRect r = roundRect(cscdRect(cscdRectRight(pb) - inset, ty, bw, th), bw / 2);
        cscdCanvasFillRRect(P->canvas, &r, thumb);
    }
    if (showX && maxX > 0) {
        float trackW = pb.w - 4;
        float tw = cscdMaxf(20.f, trackW * pb.w / (pb.w + maxX));
        float tx = pb.x + 2 + (trackW - tw) * (e->scrollLeft / maxX);
        CscdRRect r = roundRect(cscdRect(tx, cscdRectBottom(pb) - inset, tw, bw), bw / 2);
        cscdCanvasFillRRect(P->canvas, &r, thumb);
    }
}

/* ---- paint order ----------------------------------------------------------------------------------- */

static bool isStackingContext(const CscdLayoutBox* b) {
    if (!b->parent) return true;
    if (b->kind == CSCD_BOX_TEXT) return false;
    return cscdStyleCreatesStackingContext(b->style);
}

static bool isPositioned(const CscdLayoutBox* b) { return b->kind != CSCD_BOX_TEXT && b->style && b->style->position != CSCD_POS_STATIC; }

/* 4x4 matrices acting on column vectors (x, y, z, 1), by rows. */
typedef struct Mat4 { float m[16]; } Mat4;
#define AT(M, r, c) ((M).m[(r) * 4 + (c)])

static Mat4 m4Identity(void) {
    Mat4 t;
    memset(&t, 0, sizeof t);
    t.m[0] = t.m[5] = t.m[10] = t.m[15] = 1;
    return t;
}

static Mat4 m4Mul(Mat4 a, Mat4 o) {
    Mat4 out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float v = 0;
            for (int k = 0; k < 4; ++k) v += AT(a, r, k) * AT(o, k, c);
            AT(out, r, c) = v;
        }
    return out;
}

static Mat4 m4Translate(float x, float y, float z) { Mat4 t = m4Identity(); AT(t, 0, 3) = x; AT(t, 1, 3) = y; AT(t, 2, 3) = z; return t; }
static Mat4 m4Scale(float x, float y, float z) { Mat4 t = m4Identity(); AT(t, 0, 0) = x; AT(t, 1, 1) = y; AT(t, 2, 2) = z; return t; }
static Mat4 m4Of(CscdMatrix a) {
    Mat4 t = m4Identity();
    AT(t, 0, 0) = a.a; AT(t, 0, 1) = a.c; AT(t, 0, 3) = a.e;
    AT(t, 1, 0) = a.b; AT(t, 1, 1) = a.d; AT(t, 1, 3) = a.f;
    return t;
}
static Mat4 m4Perspective(float d) { Mat4 t = m4Identity(); if (d > 0) AT(t, 3, 2) = -1 / d; return t; }

static Mat4 m4Rotate(float x, float y, float z, float angle) { /* CSS rotate3d() */
    const float len = sqrtf(x * x + y * y + z * z);
    if (len <= 0) return m4Identity();
    x /= len; y /= len; z /= len;
    const float s = cscdSinf(angle / 2), c = cscdCosf(angle / 2), sc = s * c, sq = s * s;
    Mat4 t = m4Identity();
    AT(t, 0, 0) = 1 - 2 * (y * y + z * z) * sq; AT(t, 0, 1) = 2 * (x * y * sq - z * sc); AT(t, 0, 2) = 2 * (x * z * sq + y * sc);
    AT(t, 1, 0) = 2 * (x * y * sq + z * sc); AT(t, 1, 1) = 1 - 2 * (x * x + z * z) * sq; AT(t, 1, 2) = 2 * (y * z * sq - x * sc);
    AT(t, 2, 0) = 2 * (x * z * sq - y * sc); AT(t, 2, 1) = 2 * (y * z * sq + x * sc); AT(t, 2, 2) = 1 - 2 * (x * x + y * y) * sq;
    return t;
}

/* On the z = 0 plane only x, y and w matter: an affine 2D matrix when w stays 1. */
static bool m4FlatAffine(const Mat4* m) { return AT(*m, 3, 0) == 0 && AT(*m, 3, 1) == 0 && AT(*m, 3, 3) > 0; }

static CscdMatrix m4Affine(const Mat4* m) {
    const float w = AT(*m, 3, 3);
    return cscdMatrix(AT(*m, 0, 0) / w, AT(*m, 1, 0) / w, AT(*m, 0, 1) / w, AT(*m, 1, 1) / w, AT(*m, 0, 3) / w, AT(*m, 1, 3) / w);
}

static float det3(const Mat4* M, int r0, int r1, int r2, int c0, int c1, int c2) {
    const Mat4 m = *M;
    return AT(m, r0, c0) * (AT(m, r1, c1) * AT(m, r2, c2) - AT(m, r1, c2) * AT(m, r2, c1)) -
           AT(m, r0, c1) * (AT(m, r1, c0) * AT(m, r2, c2) - AT(m, r1, c2) * AT(m, r2, c0)) +
           AT(m, r0, c2) * (AT(m, r1, c0) * AT(m, r2, c1) - AT(m, r1, c1) * AT(m, r2, c0));
}

/* The element shows its back: the inverse's (2, 2) entry is negative (CSS Transforms 2). */
static bool m4BackFacing(const Mat4* m) {
    /* the (2, 2) entry of the inverse: its cofactor over the determinant */
    float det = 0;
    for (int c = 0; c < 4; ++c) {
        int cols[3], n = 0;
        for (int k = 0; k < 4; ++k) if (k != c) cols[n++] = k;
        det += ((c & 1) ? -1.f : 1.f) * AT(*m, 0, c) * det3(m, 1, 2, 3, cols[0], cols[1], cols[2]);
    }
    if (det == 0) return false;
    const float cof = det3(m, 0, 1, 3, 0, 1, 3); /* (+, as 2 + 2 is even) */
    return cof / det < 0;
}

/* What a box and everything in it draws (its box, text, shadows, outline), in layout coordinates. */
static CscdRect inkBounds(const CscdLayoutBox* b) {
    CscdRect r = cscdBoxBorderBox(b);
    for (size_t i = 0; i < b->textFrags.len; ++i) {
        const CscdTextFrag* f = &b->textFrags.items[i];
        r = cscdRectUnite(r, cscdRect(f->x, f->top, f->width, f->height));
    }
    for (size_t i = 0; i < b->inlineFrags.len; ++i) r = cscdRectUnite(r, b->inlineFrags.items[i].rect);
    const CscdComputedStyle* s = b->style;
    if (s) {
        float out = s->outlineStyle != CSCD_BORDER_NONE ? cscdMaxf(0.f, s->outlineWidth + s->outlineOffset) : 0;
        for (size_t i = 0; i < s->boxShadow.len; ++i) {
            const CscdShadow* sh = &s->boxShadow.items[i];
            if (!sh->inset) out = cscdMaxf(out, cscdMaxf(fabsf(sh->x), fabsf(sh->y)) + 2 * sh->blur + cscdMaxf(0.f, sh->spread));
        }
        if (out > 0) r = cscdRectInset(r, -out, -out, -out, -out);
        if (cscdStyleClipsOverflow(s)) return r;
    }
    for (size_t i = 0; i < b->children.len; ++i) r = cscdRectUnite(r, inkBounds(cscdBoxChild(b, i)));
    return r;
}

/* The 3D transform of b, in layout coordinates, with the perspective its parent gives it. */
static Mat4 transform3D(CscdLayoutBox* b) {
    const CscdComputedStyle* s = b->style;
    CscdRect r = b->kind == CSCD_BOX_INLINE && b->inlineFrags.len ? b->inlineFrags.items[0].rect : cscdBoxBorderBox(b);
    const float ox = r.x + cscdLenResolve(s->originX, r.w, 0), oy = r.y + cscdLenResolve(s->originY, r.h, 0), oz = s->originZ;
    Mat4 m = m4Translate(ox, oy, oz);
    for (size_t i = 0; i < s->transform.len; ++i) {
        const CscdTransformOp* op = &s->transform.items[i];
        switch (op->kind) {
        case CSCD_TF_TRANSLATE: m = m4Mul(m, m4Translate(cscdLenResolve(op->x, r.w, 0), cscdLenResolve(op->y, r.h, 0), 0)); break;
        case CSCD_TF_SCALE: m = m4Mul(m, m4Scale(op->a, op->b, 1)); break;
        case CSCD_TF_ROTATE: m = m4Mul(m, m4Rotate(0, 0, 1, op->a)); break;
        case CSCD_TF_SKEW: m = m4Mul(m, m4Of(cscdMatSkew(op->a, op->b))); break;
        case CSCD_TF_MATRIX: m = m4Mul(m, m4Of(op->m)); break;
        case CSCD_TF_ROTATE3D: m = m4Mul(m, m4Rotate(op->x3, op->y3, op->z3, op->a)); break;
        case CSCD_TF_TRANSLATE_Z: m = m4Mul(m, m4Translate(0, 0, op->z)); break;
        case CSCD_TF_SCALE_Z: m = m4Mul(m, m4Scale(1, 1, op->a)); break;
        case CSCD_TF_PERSPECTIVE: m = m4Mul(m, m4Perspective(op->z)); break;
        case CSCD_TF_MATRIX3D: {
            Mat4 t;
            for (int col = 0; col < 4; ++col)
                for (int row = 0; row < 4; ++row) AT(t, row, col) = op->m4[col * 4 + row];
            m = m4Mul(m, t);
            break;
        }
        }
    }
    m = m4Mul(m, m4Translate(-ox, -oy, -oz));
    /* the perspective property of the parent (an anonymous box passes it through) */
    const CscdLayoutBox* p = b->parent;
    while (p && p->anonymous) p = p->parent;
    if (p && p->style && p->style->perspective > 0) {
        const CscdRect pr = cscdBoxBorderBox(p);
        const float px = pr.x + cscdLenResolve(p->style->perspectiveX, pr.w, 0), py = pr.y + cscdLenResolve(p->style->perspectiveY, pr.h, 0);
        m = m4Mul(m4Mul(m4Mul(m4Translate(px, py, 0), m4Perspective(p->style->perspective)), m4Translate(-px, -py, 0)), m);
    }
    return m;
}

static void paintLayerContent(Painter* P, CscdLayoutBox* b);

/* A box under a perspective: painted flat into an image of its own, which the canvas draws through
 * the projection. Returns false when nothing of it can show. */
static bool paintProjective(Painter* P, CscdLayoutBox* b, const Mat4* M) {
    CscdCanvas* canvas = P->canvas;
    /* layout coordinates -> this canvas's pixels, through the projection (by rows) */
    const CscdMatrix A = cscdCanvasMatrix(canvas);
    const float Pm[9] = {AT(*M, 0, 0), AT(*M, 0, 1), AT(*M, 0, 3), AT(*M, 1, 0), AT(*M, 1, 1), AT(*M, 1, 3), AT(*M, 3, 0), AT(*M, 3, 1), AT(*M, 3, 3)};
    float AP[9];
    for (int c = 0; c < 3; ++c) {
        AP[0 + c] = A.a * Pm[c] + A.c * Pm[3 + c] + A.e * Pm[6 + c];
        AP[3 + c] = A.b * Pm[c] + A.d * Pm[3 + c] + A.f * Pm[6 + c];
        AP[6 + c] = Pm[6 + c];
    }
    /* the offscreen image: as many pixels as the box gets on the device at its nearest */
    const CscdRect L = cscdRectInset(inkBounds(b), -2, -2, -2, -2);
    if (cscdRectEmpty(L)) return false;
    float scale = sqrtf(fabsf(A.a * A.d - A.b * A.c));
    float dx0 = 1e30f, dy0 = 1e30f, dx1 = -1e30f, dy1 = -1e30f;
    bool behind = false;
    for (int k = 0; k < 4; ++k) {
        const float u = (k & 1) ? cscdRectRight(L) : L.x, v = (k & 2) ? cscdRectBottom(L) : L.y;
        const float w = AP[6] * u + AP[7] * v + AP[8];
        if (w <= 1e-6f) { behind = true; break; }
        const float x = (AP[0] * u + AP[1] * v + AP[2]) / w, y = (AP[3] * u + AP[4] * v + AP[5]) / w;
        dx0 = cscdMinf(dx0, x); dy0 = cscdMinf(dy0, y); dx1 = cscdMaxf(dx1, x); dy1 = cscdMaxf(dy1, y);
    }
    if (!behind) scale = cscdMaxf(scale, cscdMaxf((dx1 - dx0) / L.w, (dy1 - dy0) / L.h));
    scale = cscdClampf(scale, 0.25f, 4.f);
    scale = cscdMinf(scale, cscdMinf(4096.f / L.w, 4096.f / L.h));
    const int W = cscdMaxi(1, cscdIntOf(ceilf(L.w * scale))), H = cscdMaxi(1, cscdIntOf(ceilf(L.h * scale)));
    /* image pixels -> this canvas's pixels: AP * (pixel / scale + L.origin) */
    float D[9];
    for (int r = 0; r < 3; ++r) {
        D[r * 3 + 0] = AP[r * 3 + 0] / scale;
        D[r * 3 + 1] = AP[r * 3 + 1] / scale;
        D[r * 3 + 2] = AP[r * 3 + 0] * L.x + AP[r * 3 + 1] * L.y + AP[r * 3 + 2];
    }
    Projection proj;
    proj.clip = P->projection ? P->projection->clip : cscdCanvasDeviceClipBounds(canvas);
    if (P->projection) { /* (inside another projection: on to the device through it too) */
        const float* O = P->projection->h;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) proj.h[r * 3 + c] = O[r * 3] * D[c] + O[r * 3 + 1] * D[3 + c] + O[r * 3 + 2] * D[6 + c];
    } else memcpy(proj.h, D, sizeof D);
    const bool dry = cscdCanvasDryRun(canvas);
    CscdImage off;
    memset(&off, 0, sizeof off);
    if (dry) { off.width = W; off.height = H; } /* (only finding where things are) */
    else cscdImageInit(&off, W, H);
    CscdCanvas* oc = cscdCanvasNew(&off);
    cscdCanvasSetDryRun(oc, dry);
    cscdCanvasScale(oc, scale, scale);
    cscdCanvasTranslate(oc, -L.x, -L.y);
    Painter sub = painter(P->view, oc);
    sub.projection = &proj;
    sub.textShader = P->textShader;
    paintLayerContent(&sub, b);
    cscdCanvasFree(oc);
    if (!dry) cscdCanvasDrawImageProjective(canvas, &off, D);
    if (!dry) cscdImageFini(&off);
    return true;
}

/* Applies the clip of b's overflow and its scroll offset to its contents. */
static bool pushContentClip(Painter* P, CscdLayoutBox* b) {
    if (!b->style || b->kind == CSCD_BOX_TEXT || b->kind == CSCD_BOX_INLINE) return false;
    const CscdComputedStyle* s = b->style;
    if (!cscdStyleClipsOverflow(s)) return false;
    CscdCanvas* canvas = P->canvas;
    cscdCanvasSave(canvas);
    CscdRRect clip = rrInset(borderRRect(b, cscdBoxBorderBox(b)), b->border[0], b->border[1], b->border[2], b->border[3]);
    if (!(s->contain & CSCD_CONTAIN_PAINT) && (s->overflowX == CSCD_OVERFLOW_VISIBLE || s->overflowY == CSCD_OVERFLOW_VISIBLE)) { /* one axis only */
        CscdRect r = clip.rect;
        if (s->overflowX == CSCD_OVERFLOW_VISIBLE) { r.x -= 1e5f; r.w += 2e5f; }
        if (s->overflowY == CSCD_OVERFLOW_VISIBLE) { r.y -= 1e5f; r.h += 2e5f; }
        cscdCanvasClipRect(canvas, r);
    } else cscdCanvasClipRRect(canvas, &clip);
    if (b->element && b->parent) cscdCanvasTranslate(canvas, -b->element->scrollLeft, -b->element->scrollTop);
    return true;
}

/* writing-mode: a vertical box's contents, laid out horizontally from (x + logicalDX, y + logicalDY),
 * turned a quarter onto its box (clockwise, or the other way for sideways-lr). */
static bool pushWritingMode(Painter* P, CscdLayoutBox* b) {
    if (!b->style || !b->style->verticalRoot || b->kind == CSCD_BOX_INLINE || b->kind == CSCD_BOX_TEXT) return false;
    const float lx = b->x + b->logicalDX, ly = b->y + b->logicalDY;
    CscdMatrix m;
    if (b->style->writingMode == CSCD_WRITING_SIDEWAYS_LR) m = cscdMatrix(0, -1, 1, 0, b->x - ly, b->y + b->h + lx);
    else m = cscdMatrix(0, 1, -1, 0, b->x + b->w + ly, b->y - lx);
    cscdCanvasSave(P->canvas);
    cscdCanvasConcat(P->canvas, m);
    return true;
}

static void paintCustom(Painter* P, CscdLayoutBox* b) {
    if (!b->element || b->pseudo.len || b->anonymous) return;
    if (b->style->visibility != CSCD_VIS_VISIBLE) return; /* hidden as its background is */
    const CscdPainter* ph = cscdElementPainter(b->element);
    if (!ph->fn) return;
    CscdCanvas* canvas = P->canvas;
    CscdRect cb = cscdBoxContentBox(b);
    if (cscdCanvasDryRun(canvas)) return; /* (only finding where things are) */
    /* painting part of the page: a painter wholly outside it is not asked */
    if (!P->view->recordHits && cscdRectEmpty(cscdRectIntersect(cscdMatMapRect(cscdCanvasMatrix(canvas), cb), cscdCanvasDeviceClipBounds(canvas)))) return;
    cscdCanvasSave(canvas);
    cscdCanvasClipRect(canvas, cb);
    cscdCanvasTranslate(canvas, cb.x, cb.y);
    ph->fn(ph->user, canvas, cb.w, cb.h);
    cscdCanvasRestore(canvas);
}

/* Paints b and its subtree as one atomic stacking layer (stacking contexts, positioned boxes, floats
 * and inline-blocks). */
static void paintLayer(Painter* P, CscdLayoutBox* b) {
    CscdCanvas* canvas = P->canvas;
    CscdView* v = P->view;
    const CscdComputedStyle* s = b->style;
    if (s->display == CSCD_DISPLAY_NONE) return;
    if (s->opacity <= 0) return;
    cscdCanvasSave(canvas);
    if (s->position == CSCD_POS_FIXED) cscdCanvasTranslate(canvas, v->scrollX, v->scrollY); /* fixed boxes ignore the document scroll */
    if (s->position == CSCD_POS_STICKY && b->element) {
        /* stick to the top of the nearest scroll container */
        float dy = 0;
        float scrollTop = v->scrollY;
        float containerTop = 0;
        for (CscdLayoutBox* p = b->parent; p; p = p->parent)
            if (p->style && cscdStyleClipsOverflow(p->style) && p->element && p->parent) {
                scrollTop = p->element->scrollTop;
                containerTop = cscdBoxPaddingBox(p).y;
                break;
            }
        if (!cscdLenIsAuto(s->top)) {
            float stickTop = containerTop + scrollTop + cscdLenResolve(s->top, 0, 0);
            if (b->y < stickTop) {
                dy = stickTop - b->y;
                /* stay inside the parent */
                if (b->parent) dy = cscdMinf(dy, cscdMaxf(0.f, b->parent->y + b->parent->h - b->parent->border[2] - b->parent->padding[2] - (b->y + b->h)));
            }
        }
        cscdCanvasTranslate(canvas, 0, dy);
    }
    if (s->transform.len) {
        const Mat4 m = transform3D(b);
        if (s->backfaceHidden && m4BackFacing(&m)) {
            cscdCanvasRestore(canvas);
            return;
        }
        if (!m4FlatAffine(&m)) { /* a perspective: through an image of its own */
            paintProjective(P, b, &m);
            cscdCanvasRestore(canvas);
            return;
        }
        cscdCanvasConcat(canvas, m4Affine(&m));
    }
    paintLayerContent(P, b);
    cscdCanvasRestore(canvas);
}

static void paintStacking(Painter* P, CscdLayoutBox* b);

/* clip-path basic shapes: inset(), circle(), ellipse(), polygon() */
static void clipPathShape(Painter* P, CscdLayoutBox* b) {
    CscdPath p = {0};
    if (cscdBasicShapePath(cscdStrSv(&b->style->clipPath), cscdBoxBorderBox(b), &p) && !cscdPathEmpty(&p))
        cscdCanvasClipPath(P->canvas, &p, CSCD_FILL_NONZERO);
    cscdPathFree(&p);
}

static void paintLayerContent(Painter* P, CscdLayoutBox* b) {
    CscdCanvas* canvas = P->canvas;
    const CscdComputedStyle* s = b->style;
    bool layer = s->opacity < 1 || s->blur > 0 || s->grayscale > 0 || s->brightness != 1 || s->saturate != 1 || s->invert > 0 || s->blend != CSCD_BM_NORMAL ||
                 s->masks.len;
    if (layer) {
        CscdBlend bl = CSCD_BLEND_SRC_OVER;
        switch (s->blend) {
        case CSCD_BM_MULTIPLY: bl = CSCD_BLEND_MULTIPLY; break;
        case CSCD_BM_SCREEN: bl = CSCD_BLEND_SCREEN; break;
        case CSCD_BM_OVERLAY: bl = CSCD_BLEND_OVERLAY; break;
        case CSCD_BM_DARKEN: bl = CSCD_BLEND_DARKEN; break;
        case CSCD_BM_LIGHTEN: bl = CSCD_BLEND_LIGHTEN; break;
        default: break;
        }
        cscdCanvasPushLayer(canvas, s->opacity, bl);
        CscdFilter f;
        f.blur = s->blur * P->view->dpr;
        f.brightness = s->brightness; f.grayscale = s->grayscale; f.saturate = s->saturate; f.invert = s->invert;
        cscdCanvasSetLayerFilter(canvas, &f);
    }
    if (s->clipPath.len) clipPathShape(P, b);
    paintStacking(P, b);
    if (s->masks.len && !cscdCanvasDryRun(canvas)) {
        /* the mask layers painted as a background of the box, into an image whose alpha masks it */
        CscdImage maskImg; /* (the canvas's size: on a recording canvas a layer has none) */
        cscdImageInit(&maskImg, cscdCanvasWidth(canvas), cscdCanvasHeight(canvas));
        CscdCanvas* mc = cscdCanvasNew(&maskImg);
        cscdCanvasClipRect(mc, cscdCanvasDeviceClipBounds(canvas));
        cscdCanvasSetMatrix(mc, cscdCanvasMatrix(canvas));
        Painter mp = painter(P->view, mc);
        CscdComputedStyle* style = cscdStyleClone(s);
        cscdLayerVecCopy(&style->backgrounds, &s->masks);
        style->backgroundColor = cscdRgba(0, 0, 0, 0);
        CscdComputedStyle* saved = b->style;
        b->style = style;
        const CscdRect bb = cscdBoxBorderBox(b);
        paintBackground(&mp, b, bb, borderRRect(b, bb), true);
        b->style = saved;
        cscdRelease(style);
        cscdCanvasFree(mc);
        cscdCanvasMaskLayer(canvas, &maskImg);
        cscdImageFini(&maskImg);
    }
    if (layer) cscdCanvasPopLayer(canvas);
}

static void collectLayers(CscdLayoutBox* b, CscdPtrVec* neg, CscdPtrVec* zero, CscdPtrVec* pos) {
    for (size_t i = 0; i < b->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(b, i);
        if (c->kind == CSCD_BOX_TEXT) continue;
        if (isPositioned(c) || isStackingContext(c)) {
            int z = c->style->zAuto ? 0 : c->style->zIndex;
            if (z < 0) cscdVecPush(neg, c);
            else if (z > 0) cscdVecPush(pos, c);
            else cscdVecPush(zero, c);
            continue; /* its descendants belong to it */
        }
        collectLayers(c, neg, zero, pos);
    }
}

/* the shader of background-clip: text over r (an image or a gradient) */
static CscdShader textClipShader(const CscdBackgroundLayer* L, CscdRect imageArea, CscdRect gradientArea) {
    if (L->kind == CSCD_LAYER_URL && L->image) return cscdShaderImage(L->image, imageArea, L->repeatX, L->repeatY);
    return gradientShader(L, gradientArea);
}

/* Normal-flow painting of the subtree of b inside its stacking context.
 * phase 0: block backgrounds, 1: floats, 2: inline content and replaced content. */
static void paintFlow(Painter* P, CscdLayoutBox* b, int phase) {
    for (size_t i = 0; i < b->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(b, i);
        if (c->kind != CSCD_BOX_TEXT && (isPositioned(c) || isStackingContext(c))) continue;
        if (c->kind == CSCD_BOX_TEXT) {
            if (phase == 2) paintText(P, c);
            continue;
        }
        bool isFloat = c->style->floating != CSCD_FLOAT_NONE;
        if (isFloat || c->atomicInline) {
            if ((isFloat && phase == 1) || (!isFloat && phase == 2)) paintLayer(P, c);
            continue;
        }
        const CscdLayerVec* bgs = &c->style->backgrounds;
        if (c->kind == CSCD_BOX_INLINE) {
            if (phase == 2) {
                paintDecorations(P, c);
                /* background-clip: text on inline boxes */
                const CscdShader* saved = P->textShader;
                CscdShader clipShader;
                bool own = false;
                if (bgs->len && bgs->items[0].clip == CSCD_BG_TEXT && c->inlineFrags.len) {
                    CscdRect r = c->inlineFrags.items[0].rect;
                    for (size_t k = 0; k < c->inlineFrags.len; ++k) r = cscdRectUnite(r, c->inlineFrags.items[k].rect);
                    clipShader = gradientShader(&bgs->items[0], r);
                    own = true;
                    P->textShader = &clipShader;
                }
                paintFlow(P, c, 2);
                P->textShader = saved;
                if (own) cscdShaderFini(&clipShader);
                for (size_t k = 0; k < c->inlineFrags.len; ++k) paintOutline(P, c, c->inlineFrags.items[k].rect);
            }
            continue;
        }
        /* block-level box in flow */
        if (phase == 0) paintDecorations(P, c);
        const CscdShader* saved = P->textShader;
        CscdShader clipShader;
        bool own = false;
        if (bgs->len && bgs->items[0].clip == CSCD_BG_TEXT) {
            clipShader = textClipShader(&bgs->items[0], cscdBoxPaddingBox(c), cscdBoxBorderBox(c));
            own = true;
            P->textShader = &clipShader;
        }
        if (phase == 2) {
            if (c->kind == CSCD_BOX_REPLACED) paintReplaced(P, c);
            paintCustom(P, c);
        }
        bool clipped = pushContentClip(P, c);
        paintFlow(P, c, phase);
        if (clipped) cscdCanvasRestore(P->canvas);
        P->textShader = saved;
        if (own) cscdShaderFini(&clipShader);
        if (phase == 2) {
            if (cscdStyleClipsOverflow(c->style)) paintScrollbars(P, c);
            paintOutline(P, c, cscdBoxBorderBox(c));
        }
    }
}

static int byZIndex(const void* a, const void* b, void* ctx) {
    (void)ctx;
    const int x = (*(CscdLayoutBox* const*)a)->style->zIndex, y = (*(CscdLayoutBox* const*)b)->style->zIndex;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* re-applies the clips of the ancestors between b and d (overflow and scroll), then paints d */
static void paintDescendant(Painter* P, CscdLayoutBox* b, CscdLayoutBox* d) {
    CscdPtrVec chain = {0};
    for (CscdLayoutBox* p = d->parent; p && p != b; p = p->parent) cscdVecPush(&chain, p);
    int pushed = 0;
    bool fixed = d->style->position == CSCD_POS_FIXED;
    if (!fixed)
        for (size_t i = chain.len; i-- > 0;)
            /* absolutely positioned boxes escape clips of non-positioned ancestors outside their
             * containing block (approximation: keep all) */
            if (pushContentClip(P, (CscdLayoutBox*)chain.items[i])) ++pushed;
    paintLayer(P, d);
    for (int i = 0; i < pushed; ++i) cscdCanvasRestore(P->canvas);
    cscdVecFree(&chain);
}

static void paintStacking(Painter* P, CscdLayoutBox* b) {
    CscdPtrVec neg = {0}, zero = {0}, pos = {0};
    collectLayers(b, &neg, &zero, &pos);
    cscdSort(neg.items, neg.len, sizeof(void*), byZIndex, NULL);
    cscdSort(pos.items, pos.len, sizeof(void*), byZIndex, NULL);
    /* the box itself */
    paintDecorations(P, b);
    const CscdShader* saved = P->textShader;
    CscdShader clipShader;
    bool own = false;
    if (b->style && b->style->backgrounds.len && b->style->backgrounds.items[0].clip == CSCD_BG_TEXT) {
        clipShader = gradientShader(&b->style->backgrounds.items[0], cscdBoxBorderBox(b));
        own = true;
        P->textShader = &clipShader;
    }
    if (b->kind == CSCD_BOX_REPLACED) paintReplaced(P, b);
    paintCustom(P, b);
    bool clipped = pushContentClip(P, b);
    const bool turned = pushWritingMode(P, b);
    for (size_t i = 0; i < neg.len; ++i) paintDescendant(P, b, (CscdLayoutBox*)neg.items[i]);
    if (b->kind == CSCD_BOX_TEXT) {
    } else if (b->kind == CSCD_BOX_INLINE) paintFlow(P, b, 2);
    else {
        paintFlow(P, b, 0);
        paintFlow(P, b, 1);
        paintFlow(P, b, 2);
    }
    for (size_t i = 0; i < zero.len; ++i) paintDescendant(P, b, (CscdLayoutBox*)zero.items[i]);
    for (size_t i = 0; i < pos.len; ++i) paintDescendant(P, b, (CscdLayoutBox*)pos.items[i]);
    if (turned) cscdCanvasRestore(P->canvas);
    if (clipped) cscdCanvasRestore(P->canvas);
    P->textShader = saved;
    if (own) cscdShaderFini(&clipShader);
    if (b->style && cscdStyleClipsOverflow(b->style) && b->parent) paintScrollbars(P, b);
    if (b->kind != CSCD_BOX_INLINE) paintOutline(P, b, cscdBoxBorderBox(b));
    else for (size_t i = 0; i < b->inlineFrags.len; ++i) paintOutline(P, b, b->inlineFrags.items[i].rect);
    cscdVecFree(&neg);
    cscdVecFree(&zero);
    cscdVecFree(&pos);
}

CscdColor cscdViewCanvasColor(CscdView* v) {
    viewLayoutNow(v);
    CscdLayoutBox* root = v->root;
    if (!root || !root->style) return viewDark(v) ? cscdRgb(18, 18, 18) : cscdRgb(255, 255, 255);
    /* as paintRoot: the root's background, or body's when the root has none, over the canvas */
    const CscdComputedStyle* bg = root->style;
    if (bg->backgroundColor.a == 0 && !bg->backgrounds.len)
        for (size_t i = 0; i < root->children.len; ++i) {
            CscdLayoutBox* c = cscdBoxChild(root, i);
            if (c->element && cscdElementIs(c->element, "body") && c->style) {
                bg = c->style;
                break;
            }
        }
    const uint8_t scheme = root->style->colorScheme;
    const bool dark = (scheme == 2 || (viewDark(v) && scheme != 1)) && bg->backgroundColor.a == 0;
    const CscdColor under = dark ? cscdRgb(18, 18, 18) : cscdRgb(255, 255, 255), c = bg->backgroundColor;
    const int a = c.a;
    return cscdRgb((c.r * a + under.r * (255 - a)) / 255, (c.g * a + under.g * (255 - a)) / 255, (c.b * a + under.b * (255 - a)) / 255);
}

void paintRoot(CscdView* v, CscdCanvas* canvas, CscdLayoutBox* root) {
    Painter Pv = painter(v, canvas);
    Painter* P = &Pv;
    /* the canvas background comes from the root, or from body when the root has none */
    const CscdComputedStyle* bg = root->style;
    CscdLayoutBox* bgBox = root;
    if (bg->backgroundColor.a == 0 && !bg->backgrounds.len) {
        for (size_t i = 0; i < root->children.len; ++i) {
            CscdLayoutBox* c = cscdBoxChild(root, i);
            if (c->element && cscdElementIs(c->element, "body")) {
                bgBox = c;
                P->rootPropagated = true;
                break;
            }
        }
    }
    /* (an opaque page background covers the canvas: nothing under it) */
    const CscdRect view = {v->scrollX, v->scrollY, v->vw, v->vh};
    if (!v->transparent && bgBox->style->backgroundColor.a < 255) cscdCanvasFillRect(canvas, view, cscdRgb(255, 255, 255));
    const uint8_t scheme = root->style->colorScheme; /* (color-scheme: only light keeps the canvas light; only dark makes it dark) */
    const bool darkCanvas = scheme == 2 || (viewDark(v) && scheme != 1);
    if (!v->transparent && darkCanvas && bgBox->style->backgroundColor.a == 0 && !bgBox->style->backgrounds.len) cscdCanvasFillRect(canvas, view, cscdRgb(18, 18, 18));
    {
        CscdRRect rr = cscdRRect(cscdRect(0, 0, cscdMaxf(v->vw, cscdRectRight(root->overflow)), cscdMaxf(v->vh, cscdRectBottom(root->overflow))));
        CscdLayoutBox* b = bgBox;
        CscdComputedStyle* s = b->style;
        if (s->backgroundColor.a) cscdCanvasFillRect(canvas, rr.rect, s->backgroundColor);
        /* background images are positioned against the root element */
        if (s->backgrounds.len) {
            float saveB[4], saveP[4];
            for (int i = 0; i < 4; ++i) { saveB[i] = b->border[i]; saveP[i] = b->padding[i]; }
            CscdColor saveC = s->backgroundColor;
            s->backgroundColor = cscdRgba(0, 0, 0, 0); /* (for a moment) */
            CscdRect area2 = rr.rect;
            for (int i = 0; i < 4; ++i) { b->border[i] = 0; b->padding[i] = 0; }
            paintBackground(P, b, area2, rr, true);
            for (int i = 0; i < 4; ++i) { b->border[i] = saveB[i]; b->padding[i] = saveP[i]; }
            s->backgroundColor = saveC;
        }
    }
    paintStacking(P, root);
}
