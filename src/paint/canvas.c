/* The software painter: analytic rounded rectangles, gradients, image sampling, blurs, glyph
 * caching, layers, and the display lists that record drawing to replay it later (on several
 * threads, by bands of rows). */
#include "cascade/canvas.h"
#include "cascade/font.h"
#include "paint/canvasInternal.h"
#include "core/thread.h"

#include <stdlib.h>

/* ---- pixel math --------------------------------------------------------------------------------- */

static inline uint32_t mulPixel(uint32_t c, uint32_t k) { /* k 0..255 */
    uint32_t rb = (c & 0x00FF00FF) * k + 0x00800080;
    rb = ((rb + ((rb >> 8) & 0x00FF00FF)) >> 8) & 0x00FF00FF;
    uint32_t ag = ((c >> 8) & 0x00FF00FF) * k + 0x00800080;
    ag = (ag + ((ag >> 8) & 0x00FF00FF)) & 0xFF00FF00;
    return rb | ag;
}

static inline uint32_t over(uint32_t d, uint32_t s) {
    uint32_t sa = s >> 24;
    if (sa == 255) return s;
    if (sa == 0 && s == 0) return d;
    return s + mulPixel(d, 255 - sa);
}

/* mulPixel on two pixels at once (the same values: no lane carries into the next) */
static inline uint64_t mulPixel2(uint64_t c, uint64_t k) {
    const uint64_t m = 0x00FF00FF00FF00FFull, half = 0x0080008000800080ull;
    uint64_t rb = (c & m) * k + half;
    rb = ((rb + ((rb >> 8) & m)) >> 8) & m;
    uint64_t ag = ((c >> 8) & m) * k + half;
    ag = (ag + ((ag >> 8) & m)) & ~m;
    return rb | ag;
}

/* n pixels of d set to col, or col drawn over them with its alpha (a premultiplied color) */
static void fillPixels(uint32_t* d, int n, uint32_t col) {
    const uint64_t two = (uint64_t)col << 32 | col;
    int k = 0;
    if ((col >> 24) == 255) {
        for (; k + 2 <= n; k += 2) memcpy(d + k, &two, 8);
    } else {
        const uint64_t ia = 255 - (col >> 24);
        for (; k + 2 <= n; k += 2) {
            uint64_t p;
            memcpy(&p, d + k, 8);
            p = two + mulPixel2(p, ia);
            memcpy(d + k, &p, 8);
        }
    }
    for (; k < n; ++k) d[k] = over(d[k], col);
}

/* how many bytes from p (at most n) are 255 */
static int fullRun(const uint8_t* p, int n) {
    int i = 0;
    for (uint64_t w; i + 8 <= n; i += 8) {
        memcpy(&w, p + i, 8);
        if (w != ~0ull) break;
    }
    while (i < n && p[i] == 255) ++i;
    return i;
}

static inline uint8_t mul8(uint32_t a, uint32_t b) { uint32_t t = a * b + 128; return (uint8_t)((t + (t >> 8)) >> 8); }

static uint32_t premul(CscdColor c, float alpha) {
    if (alpha < 1) c = cscdColorWithAlpha(c, alpha);
    return cscdColorPremul(c);
}

static uint32_t lerpPx(uint32_t a, uint32_t b, uint32_t w) {
    uint32_t rb = ((a & 0x00FF00FF) * (256 - w) + (b & 0x00FF00FF) * w) >> 8;
    uint32_t ag = (((a >> 8) & 0x00FF00FF) * (256 - w) + ((b >> 8) & 0x00FF00FF) * w) >> 8;
    return (rb & 0x00FF00FF) | ((ag & 0x00FF00FF) << 8);
}

/* v rounded half away from zero (what lroundf gives), clamped to 0..255; without a library call */
static inline uint32_t clampByte(float v) {
    if (!(v > 0)) return 0;
    if (v >= 255) return 255;
    const int i = (int)v;
    return (uint32_t)(i + (v - (float)i >= 0.5f));
}

/* ---- shading ------------------------------------------------------------------------------------ */

typedef struct ShaderCtx {
    const CscdShader* s;
    CscdMatrix inv;
    uint32_t solid;
    uint32_t lut[256];
    float alpha;
} ShaderCtx;

static void shaderInit(ShaderCtx* sc, const CscdShader* sh, CscdMatrix m, float globalAlpha) {
    sc->s = sh;
    sc->solid = 0;
    sc->alpha = globalAlpha * sh->opacity;
    sc->inv = cscdMatIdentity();
    cscdMatInvert(m, &sc->inv);
    if (sh->kind == CSCD_SHADER_SOLID) { sc->solid = premul(sh->color, sc->alpha); return; }
    if (sh->kind == CSCD_SHADER_IMAGE) return;
    /* gradient lookup table */
    const CscdGradientStop* st = sh->stops.items;
    const size_t n = sh->stops.len;
    for (int i = 0; i < 256; ++i) {
        float t = i / 255.f;
        CscdColor c = {0, 0, 0, 0};
        if (!n) c = (CscdColor){0, 0, 0, 0};
        else if (t <= st[0].offset) c = st[0].color;
        else if (t >= st[n - 1].offset) c = st[n - 1].color;
        else {
            for (size_t k = 1; k < n; ++k) {
                if (t <= st[k].offset) {
                    float span = st[k].offset - st[k - 1].offset;
                    float u = span <= 0 ? 1 : (t - st[k - 1].offset) / span;
                    c = cscdColorLerp(st[k - 1].color, st[k].color, u);
                    break;
                }
            }
        }
        sc->lut[i] = premul(c, sc->alpha);
    }
}

static uint32_t gradientAt(const ShaderCtx* sc, float t) {
    if (sc->s->repeating) t -= floorf(t);
    /* clamped so that NaN lands on 0 too (cscdClampf passes it on): a length too big for a float
     * is infinite, and a gradient placed at infinity divides it by itself */
    t = t > 0 ? (t < 1 ? t : 1) : 0;
    int i = (int)(t * 255.f + 0.5f);
    return sc->lut[i];
}

static int wrapIndex(int a, int n, bool rep) {
    if (rep) {
        a %= n;
        if (a < 0) a += n;
        return a;
    }
    return cscdClampi(a, 0, n - 1);
}

static uint32_t sampleImage(const ShaderCtx* sc, float lx, float ly) {
    const CscdShader* s = sc->s;
    const CscdImage* img = s->image;
    const CscdRect r = s->imageRect;
    if (r.w <= 0 || r.h <= 0 || cscdImageEmpty(img)) return 0;
    float u = (lx - r.x) * img->width / r.w - 0.5f;
    float v = (ly - r.y) * img->height / r.h - 0.5f;
    if (!s->repeatX && (lx < r.x || lx >= cscdRectRight(r))) return 0;
    if (!s->repeatY && (ly < r.y || ly >= cscdRectBottom(r))) return 0;
    uint32_t px;
    if (!s->smooth) {
        int x = wrapIndex(cscdIntOf(floorf(u + 0.5f)), img->width, s->repeatX), y = wrapIndex(cscdIntOf(floorf(v + 0.5f)), img->height, s->repeatY);
        px = cscdImageRow(img, y)[x];
    } else {
        int x0 = cscdIntOf(floorf(u)), y0 = cscdIntOf(floorf(v));
        float fx = u - x0, fy = v - y0;
        int xa = wrapIndex(x0, img->width, s->repeatX), xb = wrapIndex(x0 + 1, img->width, s->repeatX);
        int ya = wrapIndex(y0, img->height, s->repeatY), yb = wrapIndex(y0 + 1, img->height, s->repeatY);
        uint32_t p00 = cscdImageRow(img, ya)[xa], p10 = cscdImageRow(img, ya)[xb], p01 = cscdImageRow(img, yb)[xa], p11 = cscdImageRow(img, yb)[xb];
        uint32_t wx = (uint32_t)(fx * 256), wy = (uint32_t)(fy * 256);
        px = lerpPx(lerpPx(p00, p10, wx), lerpPx(p01, p11, wx), wy);
    }
    if (sc->alpha < 1) px = mulPixel(px, (uint32_t)(sc->alpha * 255 + 0.5f));
    return px;
}

static uint32_t shadeAt(const ShaderCtx* sc, float dx, float dy) {
    const CscdShader* s = sc->s;
    if (s->kind == CSCD_SHADER_SOLID) return sc->solid;
    CscdPoint p = cscdMatMap(sc->inv, cscdPoint(dx, dy));
    switch (s->kind) {
    case CSCD_SHADER_SOLID: return sc->solid;
    case CSCD_SHADER_LINEAR: {
        float vx = s->p1.x - s->p0.x, vy = s->p1.y - s->p0.y;
        float len2 = vx * vx + vy * vy;
        float t = len2 > 0 ? ((p.x - s->p0.x) * vx + (p.y - s->p0.y) * vy) / len2 : 0;
        return gradientAt(sc, t);
    }
    case CSCD_SHADER_RADIAL: {
        float ux = s->rx > 0 ? (p.x - s->center.x) / s->rx : 0, uy = s->ry > 0 ? (p.y - s->center.y) / s->ry : 0;
        return gradientAt(sc, sqrtf(ux * ux + uy * uy));
    }
    case CSCD_SHADER_CONIC: {
        float a = cscdAtan2f(p.x - s->center.x, -(p.y - s->center.y)) - s->startAngle;
        float t = a / (2 * CSCD_PI);
        t -= floorf(t);
        return gradientAt(sc, t);
    }
    case CSCD_SHADER_IMAGE: return sampleImage(sc, p.x, p.y);
    }
    return 0;
}

/* The colors of a row of pixels of a gradient (their centers at x0 + i + 0.5, y + 0.5), stepping
 * along the row instead of mapping each pixel. True: all of them are out[0]. */
static bool shadeRow(const ShaderCtx* sc, int x0, int y, int len, uint32_t* out) {
    const CscdShader* s = sc->s;
    const CscdPoint p = cscdMatMap(sc->inv, cscdPoint(x0 + 0.5f, y + 0.5f));
    const float sx = sc->inv.a, sy = sc->inv.b; /* one pixel to the right, in local coordinates */
    if (s->kind == CSCD_SHADER_LINEAR) {
        const float vx = s->p1.x - s->p0.x, vy = s->p1.y - s->p0.y;
        const float len2 = vx * vx + vy * vy;
        if (len2 <= 0) { out[0] = gradientAt(sc, 0); return true; }
        const float t = ((p.x - s->p0.x) * vx + (p.y - s->p0.y) * vy) / len2, dt = (sx * vx + sy * vy) / len2;
        if (dt == 0) { out[0] = gradientAt(sc, t); return true; } /* (a vertical gradient: the row is one color) */
        for (int i = 0; i < len; ++i) out[i] = gradientAt(sc, t + dt * i);
        return false;
    }
    /* radial */
    const float kx = s->rx > 0 ? 1 / s->rx : 0, ky = s->ry > 0 ? 1 / s->ry : 0;
    const float ux = (p.x - s->center.x) * kx, uy = (p.y - s->center.y) * ky, dux = sx * kx, duy = sy * ky;
    for (int i = 0; i < len; ++i) {
        const float a = ux + dux * i, b = uy + duy * i;
        out[i] = gradientAt(sc, sqrtf(a * a + b * b));
    }
    return false;
}

/* Coverage of the rectangle of r at pixel (px, py): a box filter on its straight edges. */
static inline float rectCoverage(const CscdRRect* r, float px, float py) {
    float covx = cscdClampf(cscdMinf(px + 1, cscdRectRight(r->rect)) - cscdMaxf(px, r->rect.x), 0.f, 1.f);
    float covy = cscdClampf(cscdMinf(py + 1, cscdRectBottom(r->rect)) - cscdMaxf(py, r->rect.y), 0.f, 1.f);
    return covx * covy;
}

/* Whether no pixel of row py lies in a corner of r: rrectCoverage is rectCoverage there. */
static inline bool rowClearOfCorners(const CscdRRect* r, float py) {
    const float cy = py + 0.5f, t = r->rect.y, B = cscdRectBottom(r->rect);
    return !(cy < t + r->ry[0]) && !(cy < t + r->ry[1]) && !(cy > B - r->ry[2]) && !(cy > B - r->ry[3]);
}

/* Coverage of an axis-aligned rounded rectangle at pixel (px, py) (pixel centers at +0.5). */
static inline float rrectCoverage(const CscdRRect* r, float px, float py) {
    float cx = px + 0.5f, cy = py + 0.5f;
    float l = r->rect.x, t = r->rect.y, R = cscdRectRight(r->rect), B = cscdRectBottom(r->rect);
    /* box filter coverage for straight edges */
    float cov = rectCoverage(r, px, py);
    if (cov <= 0) return 0;
    int corner = -1;
    float ox = 0, oy = 0;
    if (cx < l + r->rx[0] && cy < t + r->ry[0]) { corner = 0; ox = l + r->rx[0]; oy = t + r->ry[0]; }
    else if (cx > R - r->rx[1] && cy < t + r->ry[1]) { corner = 1; ox = R - r->rx[1]; oy = t + r->ry[1]; }
    else if (cx > R - r->rx[2] && cy > B - r->ry[2]) { corner = 2; ox = R - r->rx[2]; oy = B - r->ry[2]; }
    else if (cx < l + r->rx[3] && cy > B - r->ry[3]) { corner = 3; ox = l + r->rx[3]; oy = B - r->ry[3]; }
    if (corner < 0) return cov;
    float rx = r->rx[corner], ry = r->ry[corner];
    float dx = cx - ox, dy = cy - oy;
    if (rx == ry && rx < 4096) {
        /* a round corner: well inside the arc or well outside it (by more than the half pixel the
         * edge is smoothed over, with room for rounding), the answer is known without roots */
        const float d2 = dx * dx + dy * dy, in = rx - 0.55f, out = rx + 0.55f;
        if (in > 0 && d2 <= in * in) return cov;
        if (d2 >= out * out) return 0;
    }
    float nx = dx / rx, ny = dy / ry;
    float f = sqrtf(nx * nx + ny * ny);
    if (f == 0) return cov;
    float gx = nx / (rx * f), gy = ny / (ry * f);
    float g = sqrtf(gx * gx + gy * gy);
    float dist = (f - 1) / cscdMaxf(g, 1e-6f);
    return cscdMinf(cov, cscdClampf(0.5f - dist, 0.f, 1.f));
}

static CscdRRect mapRRect(const CscdRRect* r, CscdMatrix m) {
    CscdRRect o;
    CscdPoint p = cscdMatMap(m, cscdPoint(r->rect.x, r->rect.y));
    o.rect = cscdRect(p.x, p.y, r->rect.w * m.a, r->rect.h * m.d);
    for (int i = 0; i < 4; ++i) { o.rx[i] = r->rx[i] * m.a; o.ry[i] = r->ry[i] * m.d; }
    return o;
}

/* ---- glyph cache ------------------------------------------------------------------------------- */

typedef struct GlyphBitmap {
    int left, top, w, h;
    CscdBytes a;      /* coverage, or (color) */
    CscdU32Vec rgba;  /* a color glyph's premultiplied pixels */
} GlyphBitmap;

typedef struct GlyphKey {
    const CscdFontFace* face;
    uint16_t glyph;
    uint16_t size16; /* size * 16 */
    uint8_t sub;     /* subpixel x position (quarters) */
    uint8_t flags;
    uint32_t fg;     /* a color glyph's layers in the text's color: that color (premultiplied) */
} GlyphKey;

enum { GLYPH_KEY_BYTES = sizeof(void*) + 10 };

static CscdSv glyphKeyBytes(const GlyphKey* k, char* buf) {
    memcpy(buf, &k->face, sizeof(void*));
    memcpy(buf + sizeof(void*), &k->glyph, 2);
    memcpy(buf + sizeof(void*) + 2, &k->size16, 2);
    buf[sizeof(void*) + 4] = (char)k->sub;
    buf[sizeof(void*) + 5] = (char)k->flags;
    memcpy(buf + sizeof(void*) + 6, &k->fg, 4);
    return cscdSvMake(buf, GLYPH_KEY_BYTES);
}

/* A glyph being rasterized: the threads that want it too wait for it, the others go on. */
typedef struct GlyphEntry {
    CscdOnce once;
    GlyphKey key;
    GlyphBitmap bitmap;
    uint64_t born; /* the order they came in: the oldest go first */
} GlyphEntry;

static void glyphEntryDtor(void* p) {
    cscdVecFree(&((GlyphEntry*)p)->bitmap.a);
    cscdVecFree(&((GlyphEntry*)p)->bitmap.rgba);
}

static uint8_t gammaTable[256];
static CscdOnce gammaOnce = CSCD_ONCE_INIT;
static void initGamma(void) {
    for (int i = 0; i < 256; ++i) gammaTable[i] = (uint8_t)lroundf(cscdPowf(i / 255.f, 1 / 1.2f) * 255);
}

static void addClosedContour(void* ctx, const CscdPoint* pts, size_t n, bool closed) {
    (void)closed;
    CscdAccumulator* acc = (CscdAccumulator*)ctx;
    for (size_t i = 0; i < n; ++i) cscdAccAddLine(acc, pts[i], pts[(i + 1) % n]);
}

/* A color glyph's layers (COLR), each outline's coverage painted in its color, into b->rgba. */
static bool renderLayers(const GlyphKey* k, GlyphBitmap* b) {
    CscdColorLayer layers[64];
    const size_t n = cscdMinz(cscdFontFaceColorLayers(k->face, k->glyph, layers, CSCD_COUNT(layers)), CSCD_COUNT(layers));
    if (!n) return false;
    const float size = k->size16 / 16.f, sc = size / k->face->unitsPerEm;
    const CscdMatrix m = cscdMatrix(sc, 0, (k->flags & 2) ? -0.2f * sc : 0, -sc, k->sub / 4.f, 0);
    CscdPath* paths = (CscdPath*)cscdCalloc(n, sizeof(CscdPath));
    bool* have = (bool*)cscdCalloc(n, 1);
    CscdRect bb = cscdRect(0, 0, 0, 0);
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        if (!cscdFontFaceOutline(k->face, layers[i].glyph, &paths[i])) continue;
        have[i] = true;
        const CscdRect r = cscdMatMapRect(m, cscdPathBounds(&paths[i]));
        if (!any) bb = r;
        else {
            const float rx = cscdMaxf(cscdRectRight(bb), cscdRectRight(r)), by = cscdMaxf(cscdRectBottom(bb), cscdRectBottom(r));
            bb.x = cscdMinf(bb.x, r.x);
            bb.y = cscdMinf(bb.y, r.y);
            bb.w = rx - bb.x;
            bb.h = by - bb.y;
        }
        any = true;
    }
    if (any) {
        const int x0 = cscdIntOf(floorf(bb.x)) - 1, y0 = cscdIntOf(floorf(bb.y)) - 1;
        const int x1 = cscdIntOf(ceilf(cscdRectRight(bb))) + 1, y1 = cscdIntOf(ceilf(cscdRectBottom(bb))) + 1;
        b->left = x0; b->top = y0; b->w = x1 - x0; b->h = y1 - y0;
        if (b->w > 0 && b->h > 0 && b->w <= 2048 && b->h <= 2048) {
            cscdVecResize(&b->rgba, (size_t)b->w * b->h);
            memset(b->rgba.items, 0, b->rgba.len * 4);
            CscdBytes cov = {0};
            cscdVecResize(&cov, (size_t)b->w);
            for (size_t i = 0; i < n; ++i) {
                if (!have[i]) continue;
                const uint32_t a = layers[i].argb;
                const uint32_t px = layers[i].foreground ? k->fg
                    : cscdColorPremul((CscdColor){(uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(a >> 24)});
                CscdAccumulator acc = {0};
                cscdAccReset(&acc, x0, y0, b->w, b->h);
                cscdPathFlatten(&paths[i], m, 0.2f, addClosedContour, &acc);
                for (int y = 0; y < b->h; ++y) {
                    cscdAccRow(&acc, y, CSCD_FILL_NONZERO, cov.items);
                    uint32_t* row = &b->rgba.items[(size_t)y * b->w];
                    for (int x = 0; x < b->w; ++x)
                        if (cov.items[x]) row[x] = over(row[x], cov.items[x] == 255 ? px : mulPixel(px, cov.items[x]));
                }
                cscdAccFree(&acc);
            }
            cscdVecFree(&cov);
        } else b->w = b->h = 0;
    }
    for (size_t i = 0; i < n; ++i) cscdPathFree(&paths[i]);
    cscdFree(paths);
    cscdFree(have);
    return true;
}

/* A bitmap glyph (CBDT, sbix): its strike's picture scaled to the size, each pixel the average of
 * the picture's pixels it covers. */
static bool renderBitmapGlyph(const GlyphKey* k, GlyphBitmap* b) {
    CscdRect where;
    const float size = k->size16 / 16.f;
    const CscdImage* img = cscdFontFaceGlyphImage(k->face, k->glyph, size, &where);
    if (!img || cscdImageEmpty(img)) return false;
    where.x += k->sub / 4.f;
    const int x0 = cscdIntOf(floorf(where.x)), y0 = cscdIntOf(floorf(where.y));
    const int x1 = cscdIntOf(ceilf(cscdRectRight(where))), y1 = cscdIntOf(ceilf(cscdRectBottom(where)));
    b->left = x0; b->top = y0; b->w = x1 - x0; b->h = y1 - y0;
    if (b->w <= 0 || b->h <= 0 || b->w > 2048 || b->h > 2048 || where.w <= 0 || where.h <= 0) { b->w = b->h = 0; return true; }
    cscdVecResize(&b->rgba, (size_t)b->w * b->h);
    const float sx = img->width / where.w, sy = img->height / where.h;
    for (int y = 0; y < b->h; ++y) {
        const float ty0 = (y0 + y - where.y) * sy, ty1 = ty0 + sy;
        for (int x = 0; x < b->w; ++x) {
            const float tx0 = (x0 + x - where.x) * sx, tx1 = tx0 + sx;
            float acc[4] = {0, 0, 0, 0};
            for (int py = cscdMaxi(0, cscdIntOf(floorf(ty0))); py < cscdMini(img->height, cscdIntOf(ceilf(ty1))); ++py) {
                const float wy = cscdMinf(ty1, py + 1.f) - cscdMaxf(ty0, (float)py);
                if (wy <= 0) continue;
                const uint32_t* row = cscdImageRow(img, py);
                for (int px = cscdMaxi(0, cscdIntOf(floorf(tx0))); px < cscdMini(img->width, cscdIntOf(ceilf(tx1))); ++px) {
                    const float w = wy * (cscdMinf(tx1, px + 1.f) - cscdMaxf(tx0, (float)px));
                    if (w <= 0) continue;
                    const uint32_t c = row[px];
                    for (int ch = 0; ch < 4; ++ch) acc[ch] += w * ((c >> (8 * ch)) & 255);
                }
            }
            const float total = sx * sy;
            uint32_t out = 0;
            for (int ch = 0; ch < 4; ++ch) out |= (uint32_t)cscdClampi((int)(acc[ch] / total + 0.5f), 0, 255) << (8 * ch);
            b->rgba.items[(size_t)y * b->w + x] = out;
        }
    }
    return true;
}

static void renderGlyph(void* arg) {
    GlyphEntry* e = (GlyphEntry*)arg;
    const GlyphKey* k = &e->key;
    GlyphBitmap* b = &e->bitmap;
    cscdCallOnce(&gammaOnce, initGamma);
    if (cscdFontFaceHasColor(k->face) && (renderLayers(k, b) || renderBitmapGlyph(k, b))) return;
    CscdPath p = {0};
    if (!cscdFontFaceOutline(k->face, k->glyph, &p)) {
        cscdPathFree(&p);
        return;
    }
    float size = k->size16 / 16.f;
    float sc = size / k->face->unitsPerEm;
    float sub = k->sub / 4.f;
    bool italic = k->flags & 2, bold = k->flags & 1;
    CscdMatrix m = cscdMatrix(sc, 0, italic ? -0.2f * sc : 0, -sc, sub, 0);
    CscdRect bb = cscdMatMapRect(m, cscdPathBounds(&p));
    float emb = bold ? cscdMaxf(0.5f, size / 24.f) : 0;
    int x0 = cscdIntOf(floorf(bb.x)) - 1, y0 = cscdIntOf(floorf(bb.y)) - 1;
    int x1 = cscdIntOf(ceilf(cscdRectRight(bb) + emb)) + 1, y1 = cscdIntOf(ceilf(cscdRectBottom(bb))) + 1;
    b->left = x0; b->top = y0; b->w = x1 - x0; b->h = y1 - y0;
    if (b->w <= 0 || b->h <= 0 || b->w > 2048 || b->h > 2048) {
        b->w = b->h = 0;
        cscdPathFree(&p);
        return;
    }
    CscdAccumulator acc = {0};
    cscdAccReset(&acc, x0, y0, b->w, b->h);
    cscdPathFlatten(&p, m, 0.2f, addClosedContour, &acc);
    cscdPathFree(&p);
    cscdVecResize(&b->a, (size_t)b->w * b->h);
    for (int y = 0; y < b->h; ++y) cscdAccRow(&acc, y, CSCD_FILL_NONZERO, &b->a.items[(size_t)y * b->w]);
    cscdAccFree(&acc);
    if (bold) {
        /* embolden by spreading coverage to the right */
        CscdBytes src = {0};
        cscdVecAppend(&src, b->a.items, b->a.len);
        int whole = (int)emb;
        float frac = emb - whole;
        for (int y = 0; y < b->h; ++y) {
            uint8_t* r = &b->a.items[(size_t)y * b->w];
            const uint8_t* s = &src.items[(size_t)y * b->w];
            for (int x = 0; x < b->w; ++x) {
                int mx = s[x];
                for (int k2 = 1; k2 <= whole && x - k2 >= 0; ++k2) mx = cscdMaxi(mx, s[x - k2]);
                if (x - whole - 1 >= 0) mx = cscdMaxi(mx, (int)(s[x - whole - 1] * frac));
                r[x] = (uint8_t)mx;
            }
        }
        cscdVecFree(&src);
    }
    for (size_t i = 0; i < b->a.len; ++i) b->a.items[i] = gammaTable[b->a.items[i]];
}

/* Shared by the threads that paint bands of a frame: a glyph is rasterized once, whichever band
 * needs it first. Each thread also keeps the bitmaps it used (the pointer glyphGet returns stays
 * good until that thread's next call), so that drawing text takes no lock and touches nothing
 * shared. */
static CscdRwLock glyphLock = CSCD_RWLOCK_INIT;
static CscdStrMap glyphMap; /* key bytes -> GlyphEntry* (a reference) */
static CSCD_THREAD_LOCAL CscdStrMap glyphMine;
static volatile int glyphLimit = 8192;
static uint64_t glyphClock, glyphEvictions;

static void releaseAll(CscdStrMap* m) {
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(m, s));) cscdRelease(s->val.p);
    cscdStrMapClear(m);
}

/* The entries of a cache by rank, lowest first: what to drop. The caller frees them. */
typedef struct RankedEntry { uint64_t rank; CscdStr key; size_t bytes; } RankedEntry;

static int rankOrder(const void* a, const void* b) {
    const uint64_t x = ((const RankedEntry*)a)->rank, y = ((const RankedEntry*)b)->rank;
    return x < y ? -1 : x > y;
}

static RankedEntry* rankedEntries(const CscdStrMap* m, void (*rankOf)(const void* entry, RankedEntry* out)) {
    RankedEntry* all = (RankedEntry*)cscdCalloc(m->len + 1, sizeof(RankedEntry));
    size_t n = 0;
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(m, s));) {
        rankOf(s->val.p, &all[n]);
        all[n++].key = cscdStrDup(cscdStrMapKey(s));
    }
    qsort(all, n, sizeof(RankedEntry), rankOrder);
    return all;
}

static void rankedFree(RankedEntry* all, size_t n) {
    for (size_t i = 0; i < n; ++i) cscdStrFree(&all[i].key);
    cscdFree(all);
}

static void dropEntry(CscdStrMap* m, const RankedEntry* e) {
    CscdMapVal old;
    if (cscdStrMapRemove(m, cscdStrSv(&e->key), &old)) cscdRelease(old.p);
}

static void glyphRank(const void* entry, RankedEntry* out) { out->rank = ((const GlyphEntry*)entry)->born; }

/* (with the lock) the oldest half goes: the glyphs a page is showing come back from the maps of the
 * threads that painted them, or are rasterized again on their next use */
static void glyphsMakeRoom(int limit) {
    if ((int)glyphMap.len < limit) return;
    const size_t n = glyphMap.len;
    RankedEntry* all = rankedEntries(&glyphMap, glyphRank);
    for (size_t i = 0; i < n - (size_t)limit / 2; ++i) {
        dropEntry(&glyphMap, &all[i]);
        ++glyphEvictions;
    }
    rankedFree(all, n);
}

static GlyphEntry* glyphShared(const GlyphKey* k, CscdSv key) {
    GlyphEntry* e = NULL;
    cscdRwLockRead(&glyphLock);
    CscdMapVal* v = cscdStrMapGet(&glyphMap, key);
    if (v) e = (GlyphEntry*)cscdRetain(v->p);
    cscdRwUnlockRead(&glyphLock);
    if (!e) {
        cscdRwLockWrite(&glyphLock);
        glyphsMakeRoom(cscdAtomicLoad(&glyphLimit));
        bool inserted = false;
        v = cscdStrMapPut(&glyphMap, key, &inserted);
        if (inserted) {
            GlyphEntry* n = (GlyphEntry*)cscdRcNew(sizeof(GlyphEntry), glyphEntryDtor);
            n->key = *k;
            n->born = ++glyphClock;
            v->p = n;
        }
        e = (GlyphEntry*)cscdRetain(v->p);
        cscdRwUnlockWrite(&glyphLock);
    }
    cscdCallOnceArg(&e->once, renderGlyph, e);
    return e;
}

static const GlyphBitmap* glyphGet(const GlyphKey* k) {
    char buf[GLYPH_KEY_BYTES];
    CscdSv key = glyphKeyBytes(k, buf);
    CscdMapVal* v = cscdStrMapGet(&glyphMine, key);
    if (v) return &((GlyphEntry*)v->p)->bitmap;
    if ((int)glyphMine.len >= cscdAtomicLoad(&glyphLimit)) releaseAll(&glyphMine); /* (references: the shared map has them) */
    GlyphEntry* e = glyphShared(k, key); /* (its reference goes to this thread's map) */
    cscdStrMapPut(&glyphMine, key, NULL)->p = e;
    return &e->bitmap;
}

/* ---- the canvas ---------------------------------------------------------------------------------- */

static void maskDtor(void* p) { cscdVecFree(&((CscdMask*)p)->a); }

static CscdMask* newMask(int x, int y, int w, int h) {
    CscdMask* m = (CscdMask*)cscdRcNew(sizeof(CscdMask), maskDtor);
    m->x = x;
    m->y = y;
    m->width = w;
    m->height = h;
    cscdVecResize(&m->a, (size_t)w * h);
    return m;
}

typedef struct State {
    CscdImage* target;
    CscdMatrix m;
    CscdRect clip;               /* device space */
    CscdMask* mask;              /* optional additional clip (shared) */
    float alpha;
    int layer;                 /* index into layers when this save started a layer */
} State;

typedef struct Layer {
    CscdImage* image;            /* (shared) */
    CscdImage* shared;           /* a band's: the image all bands share at this depth */
    CscdImage* parentTarget;
    float opacity;
    CscdBlend blend;
    CscdFilter filter;
    CscdIRect dirty;
} Layer;

typedef struct GlyphRun { CscdGlyph* glyphs; size_t n; CscdFont* fonts; size_t nfonts; } GlyphRun;

typedef enum OpKind {
    OP_SAVE, OP_RESTORE, OP_TRANSLATE, OP_SCALE, OP_ROTATE, OP_CONCAT, OP_SET_MATRIX, OP_SET_ALPHA, OP_CLIP_RECT,
    OP_CLIP_RRECT, OP_CLIP_PATH, OP_PUSH_LAYER, OP_POP_LAYER, OP_SET_LAYER_FILTER, OP_FILTER_BACKDROP, OP_MASK_LAYER,
    OP_CLEAR, OP_BLEND_SPAN, OP_FILL_PATH, OP_STROKE_PATH, OP_FILL_RRECT, OP_FILL_RING, OP_IMAGE_PROJECTIVE, OP_IMAGE,
    OP_BOX_SHADOW, OP_BLURRED_PATH, OP_GLYPHS
} OpKind;

/* A recorded call, with copies of what it was given. */
typedef struct Op {
    OpKind kind;
    float f[9];          /* numbers: x, y, angle, alpha, opacity, a homography, shadow offsets... */
    CscdMatrix m;
    CscdRect r, r2;
    CscdRRect rr, rr2;
    CscdColor color;
    CscdFilter filter;
    CscdBlend blend;
    CscdFillRule rule;
    bool flag;           /* smooth / inset */
    int ix, iy;
    CscdImage* image;      /* a reference */
    CscdPath* path;        /* owned */
    CscdShader* shader;    /* owned */
    CscdStrokeStyle* stroke; /* owned */
    CscdBytes cov;
    GlyphRun* run;       /* owned */
    /* OP_IMAGE replayed: its picture reduced once for all the bands (reductionStep) */
    CscdOnce reducedOnce;
    CscdImage* reduced;
} Op;

struct CscdDisplayList {
    CSCD_VEC(Op) ops;
    bool bandable;
    int layerDepth; /* layers open at once, at most */
};

struct CscdCanvas {
    CscdImage* base;
    bool dry;
    CscdIRect region;           /* what clear() fills */
    CscdDisplayList* list;      /* recording into */
    bool inCall;              /* a recorded call is running */
    CscdPtrVec* bandLayers;     /* replaying a band: the layers by depth */
    CSCD_VEC(State) stack;
    CSCD_VEC(Layer) layers;
};

static State* st(const CscdCanvas* c) { return &c->stack.items[c->stack.len - 1]; }
static bool skip(const CscdCanvas* c) { return c->dry || c->list; } /* keeps the state, draws nothing */
static bool axisAligned(const CscdCanvas* c) { CscdMatrix m = st(c)->m; return m.b == 0 && m.c == 0 && m.a > 0 && m.d > 0; }
static CscdImage* layerImg(const Layer* L) { return L->shared ? L->shared : L->image; }

/* A call made on a recording canvas goes into its list, with copies of what it was given; the
 * calls it makes itself do not (replaying it makes them again). record returns the op to fill
 * (NULL when the call is not recorded); endRecord must follow the call's work. */
static Op* record(CscdCanvas* c, OpKind kind) {
    if (!c->list || c->inCall) return NULL;
    c->inCall = true;
    Op* op = cscdVecPushZero(&c->list->ops);
    op->kind = kind;
    return op;
}
static void endRecord(CscdCanvas* c, Op* op) {
    if (op) c->inCall = false;
}

static CscdPath* copyPath(const CscdPath* p) {
    CscdPath* q = CSCD_NEW(CscdPath);
    cscdPathCopy(q, p);
    return q;
}

static CscdShader* copyShader(const CscdShader* s) {
    CscdShader* q = CSCD_NEW(CscdShader);
    *q = cscdShaderCopy(s);
    return q;
}

CscdCanvas* cscdCanvasNewRegion(CscdImage* target, CscdIRect region) {
    CscdCanvas* c = CSCD_NEW(CscdCanvas);
    c->base = target;
    const int x0 = cscdMaxi(0, region.x), y0 = cscdMaxi(0, region.y);
    const int x1 = cscdMini(target->width, region.x + region.w), y1 = cscdMini(target->height, region.y + region.h);
    c->region = cscdIRect(x0, y0, cscdMaxi(0, x1 - x0), cscdMaxi(0, y1 - y0));
    State s;
    memset(&s, 0, sizeof s);
    s.target = target;
    s.m = cscdMatIdentity();
    s.clip = cscdRect((float)c->region.x, (float)c->region.y, (float)c->region.w, (float)c->region.h);
    s.alpha = 1;
    s.layer = -1;
    cscdVecPush(&c->stack, s);
    return c;
}

CscdCanvas* cscdCanvasNew(CscdImage* target) { return cscdCanvasNewRegion(target, cscdIRect(0, 0, target->width, target->height)); }

CscdCanvas* cscdCanvasNewRecording(CscdImage* target, CscdDisplayList* list) {
    CscdCanvas* c = cscdCanvasNew(target);
    c->list = list;
    return c;
}

void cscdCanvasFree(CscdCanvas* c) {
    if (!c) return;
    while (c->layers.len) cscdCanvasPopLayer(c);
    for (size_t i = 0; i < c->stack.len; ++i) cscdRelease(c->stack.items[i].mask);
    cscdVecFree(&c->stack);
    cscdVecFree(&c->layers);
    cscdFree(c);
}

CscdImage* cscdCanvasTarget(CscdCanvas* c) { return st(c)->target; }
bool cscdCanvasRecording(const CscdCanvas* c) { return c->list != NULL; }
void cscdCanvasSetDryRun(CscdCanvas* c, bool on) { c->dry = on; }
bool cscdCanvasDryRun(const CscdCanvas* c) { return c->dry; }
int cscdCanvasWidth(const CscdCanvas* c) { return c->base->width; }
int cscdCanvasHeight(const CscdCanvas* c) { return c->base->height; }
int cscdCanvasSaveCount(const CscdCanvas* c) { return (int)c->stack.len; }
CscdMatrix cscdCanvasMatrix(const CscdCanvas* c) { return st(c)->m; }
float cscdCanvasGlobalAlpha(const CscdCanvas* c) { return st(c)->alpha; }
CscdRect cscdCanvasDeviceClipBounds(const CscdCanvas* c) { return st(c)->clip; }
bool cscdCanvasClipIsEmpty(const CscdCanvas* c) { return cscdRectEmpty(st(c)->clip); }

static void pushState(CscdCanvas* c, State s) {
    cscdRetain(s.mask);
    cscdVecPush(&c->stack, s);
}

static void popState(CscdCanvas* c) {
    cscdRelease(cscdVecLast(&c->stack).mask);
    c->stack.len--;
}

static void setMask(CscdCanvas* c, CscdMask* m) { /* takes the reference */
    cscdRelease(st(c)->mask);
    st(c)->mask = m;
}

void cscdCanvasSave(CscdCanvas* c) {
    Op* op = record(c, OP_SAVE);
    State s = *st(c);
    s.layer = -1;
    pushState(c, s);
    endRecord(c, op);
}

void cscdCanvasRestore(CscdCanvas* c) {
    Op* op = record(c, OP_RESTORE);
    if (c->stack.len > 1) {
        int layer = cscdVecLast(&c->stack).layer;
        if (layer >= 0) cscdCanvasPopLayer(c);
        else popState(c);
    }
    endRecord(c, op);
}

void cscdCanvasTranslate(CscdCanvas* c, float x, float y) {
    Op* op = record(c, OP_TRANSLATE);
    if (op) { op->f[0] = x; op->f[1] = y; }
    st(c)->m = cscdMatMul(st(c)->m, cscdMatTranslate(x, y));
    endRecord(c, op);
}

void cscdCanvasScale(CscdCanvas* c, float sx, float sy) {
    Op* op = record(c, OP_SCALE);
    if (op) { op->f[0] = sx; op->f[1] = sy; }
    st(c)->m = cscdMatMul(st(c)->m, cscdMatScale(sx, sy));
    endRecord(c, op);
}

void cscdCanvasRotate(CscdCanvas* c, float rad) {
    Op* op = record(c, OP_ROTATE);
    if (op) op->f[0] = rad;
    st(c)->m = cscdMatMul(st(c)->m, cscdMatRotate(rad));
    endRecord(c, op);
}

void cscdCanvasConcat(CscdCanvas* c, CscdMatrix m) {
    Op* op = record(c, OP_CONCAT);
    if (op) op->m = m;
    st(c)->m = cscdMatMul(st(c)->m, m);
    endRecord(c, op);
}

void cscdCanvasSetMatrix(CscdCanvas* c, CscdMatrix m) {
    Op* op = record(c, OP_SET_MATRIX);
    if (op) op->m = m;
    st(c)->m = m;
    endRecord(c, op);
}

void cscdCanvasSetGlobalAlpha(CscdCanvas* c, float a) {
    Op* op = record(c, OP_SET_ALPHA);
    if (op) op->f[0] = a;
    st(c)->alpha = a;
    endRecord(c, op);
}

static bool aligned(float v) { return fabsf(v - roundf(v)) < 0.01f; }

static void clipRRectImpl(CscdCanvas* c, const CscdRRect* r);
static void clipPathImpl(CscdCanvas* c, const CscdPath* p, CscdFillRule rule);

static void clipRectImpl(CscdCanvas* c, CscdRect r) {
    if (axisAligned(c)) {
        CscdRect d = cscdMatMapRect(st(c)->m, r);
        /* snap to whole pixels when the edges are (almost) aligned, otherwise keep exact AA via a mask */
        if (aligned(d.x) && aligned(d.y) && aligned(cscdRectRight(d)) && aligned(cscdRectBottom(d))) {
            CscdRect s = cscdRect(roundf(d.x), roundf(d.y), roundf(cscdRectRight(d)) - roundf(d.x), roundf(cscdRectBottom(d)) - roundf(d.y));
            st(c)->clip = cscdRectIntersect(st(c)->clip, s);
            return;
        }
        CscdRRect rr = cscdRRect(r);
        clipRRectImpl(c, &rr);
        return;
    }
    CscdPath p = {0};
    cscdPathAddRect(&p, r);
    clipPathImpl(c, &p, CSCD_FILL_NONZERO);
    cscdPathFree(&p);
}

void cscdCanvasClipRect(CscdCanvas* c, CscdRect r) {
    Op* op = record(c, OP_CLIP_RECT);
    if (op) op->r = r;
    clipRectImpl(c, r);
    endRecord(c, op);
}

static void clipRRectImpl(CscdCanvas* c, const CscdRRect* r) {
    if (!axisAligned(c)) {
        CscdPath p = {0};
        cscdPathAddRoundRect(&p, r->rect, r->rx, r->ry);
        clipPathImpl(c, &p, CSCD_FILL_NONZERO);
        cscdPathFree(&p);
        return;
    }
    CscdRRect d = mapRRect(r, st(c)->m);
    cscdRRectNormalize(&d);
    CscdRect bounds = cscdRect(floorf(d.rect.x), floorf(d.rect.y), 0, 0);
    bounds.w = ceilf(cscdRectRight(d.rect)) - bounds.x;
    bounds.h = ceilf(cscdRectBottom(d.rect)) - bounds.y;
    if (cscdRRectIsRect(&d)) {
        /* a straight clip snaps to whole pixels, as browsers clip overflow: no mask to make or apply */
        const float l = roundf(d.rect.x), t = roundf(d.rect.y);
        st(c)->clip = cscdRectIntersect(st(c)->clip, cscdRect(l, t, roundf(cscdRectRight(d.rect)) - l, roundf(cscdRectBottom(d.rect)) - t));
        return;
    }
    CscdRect nc = cscdRectIntersect(st(c)->clip, bounds);
    st(c)->clip = nc;
    if (cscdRectEmpty(nc) || skip(c)) return;
    const CscdMask* prev = st(c)->mask;
    int x0 = (int)nc.x, y0 = (int)nc.y, x1 = (int)cscdRectRight(nc), y1 = (int)cscdRectBottom(nc);
    CscdMask* mask = newMask(x0, y0, x1 - x0, y1 - y0);
    /* pixels wholly inside the edges and clear of the corners are covered: only the rim is worked out */
    const float l = d.rect.x, t = d.rect.y, R = cscdRectRight(d.rect), B = cscdRectBottom(d.rect);
    const int inX0 = cscdIntOf(ceilf(l + cscdMaxf(d.rx[0], d.rx[3]))), inX1 = cscdIntOf(floorf(R - cscdMaxf(d.rx[1], d.rx[2])));
    const int inY0 = cscdIntOf(ceilf(t)), inY1 = cscdIntOf(floorf(B));
    for (int y = y0; y < y1; ++y) {
        uint8_t* out = mask->a.items + (size_t)(y - y0) * mask->width;
        const bool rowInside = y >= inY0 && y + 1 <= inY1;
        const int m0 = rowInside ? cscdClampi(inX0, x0, x1) : x1, m1 = rowInside ? cscdClampi(inX1, m0, x1) : x1;
        for (int x = m0; x < m1; ++x) out[x - x0] = prev ? cscdMaskAt(prev, x, y) : 255;
        for (int x = x0; x < x1; ++x) {
            if (x == m0 && m1 > m0) { x = m1 - 1; continue; }
            float cv = rrectCoverage(&d, (float)x, (float)y);
            uint8_t v = (uint8_t)(cv * 255 + 0.5f);
            if (prev) v = mul8(v, cscdMaskAt(prev, x, y));
            out[x - x0] = v;
        }
    }
    setMask(c, mask);
}

void cscdCanvasClipRRect(CscdCanvas* c, const CscdRRect* r) {
    Op* op = record(c, OP_CLIP_RRECT);
    if (op) op->rr = *r;
    clipRRectImpl(c, r);
    endRecord(c, op);
}

static void coverageToMask(const CscdPolyVec* contours, CscdFillRule rule, CscdMask* out, CscdIRect area) {
    CscdAccumulator acc = {0};
    cscdAccReset(&acc, area.x, area.y, area.w, area.h);
    cscdAccAddPolys(&acc, contours);
    uint8_t* row = (uint8_t*)cscdMalloc((size_t)(area.w ? area.w : 1));
    for (int y = 0; y < area.h; ++y) {
        cscdAccRow(&acc, y, rule, row);
        memcpy(&out->a.items[(size_t)(area.y + y - out->y) * out->width + (area.x - out->x)], row, (size_t)area.w);
    }
    cscdFree(row);
    cscdAccFree(&acc);
}

static void clipPathImpl(CscdCanvas* c, const CscdPath* p, CscdFillRule rule) {
    CscdPolyVec contours = {0};
    cscdFlattenInto(p, st(c)->m, 0.25f, &contours);
    /* the points' bounds (min and max: a union of rectangles would drop the first point, an empty one) */
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool first = true;
    for (size_t i = 0; i < contours.len; ++i)
        for (size_t k = 0; k < contours.items[i].len; ++k) {
            const CscdPoint q = contours.items[i].items[k];
            if (first) { x0 = x1 = q.x; y0 = y1 = q.y; first = false; }
            else { x0 = cscdMinf(x0, q.x); y0 = cscdMinf(y0, q.y); x1 = cscdMaxf(x1, q.x); y1 = cscdMaxf(y1, q.y); }
        }
    CscdRect nb = cscdRect(floorf(x0), floorf(y0), ceilf(x1) - floorf(x0) + 1, ceilf(y1) - floorf(y0) + 1);
    CscdRect nc = cscdRectIntersect(st(c)->clip, nb);
    st(c)->clip = nc;
    if (cscdRectEmpty(nc) || skip(c)) {
        cscdPolyVecFree(&contours);
        return;
    }
    CscdIRect area = cscdIRect((int)nc.x, (int)nc.y, (int)nc.w, (int)nc.h);
    CscdMask* mask = newMask(area.x, area.y, area.w, area.h);
    coverageToMask(&contours, rule, mask, area);
    const CscdMask* prev = st(c)->mask;
    if (prev)
        for (int y = area.y; y < area.y + area.h; ++y)
            for (int x = area.x; x < area.x + area.w; ++x) {
                uint8_t* v = &mask->a.items[(size_t)(y - area.y) * mask->width + (x - area.x)];
                *v = mul8(*v, cscdMaskAt(prev, x, y));
            }
    setMask(c, mask);
    cscdPolyVecFree(&contours);
}

void cscdCanvasClipPath(CscdCanvas* c, const CscdPath* p, CscdFillRule rule) {
    Op* op = record(c, OP_CLIP_PATH);
    if (op) { op->path = copyPath(p); op->rule = rule; }
    clipPathImpl(c, p, rule);
    endRecord(c, op);
}

/* ---- layers ---------------------------------------------------------------------------------------
 * Layer images of the size of the target, transparent, kept for the next layers (and frames): a
 * layer is the size of the whole target, and making and clearing one for every translucent box
 * cost more than painting it. A layer given back is cleared where it was drawn on. */

static CscdMutex layerPoolMutex = CSCD_MUTEX_INIT;
static CscdPtrVec layerPool; /* CscdImage* */

static CscdImage* takeLayerImage(int w, int h) {
    cscdMutexLock(&layerPoolMutex);
    for (size_t i = layerPool.len; i-- > 0;) {
        CscdImage* img = (CscdImage*)layerPool.items[i];
        if (img->width == w && img->height == h) {
            cscdVecRemove(&layerPool, i);
            cscdMutexUnlock(&layerPoolMutex);
            return img;
        }
    }
    cscdMutexUnlock(&layerPoolMutex);
    return cscdImageNew(w, h);
}

static void giveLayerImage(CscdImage* img, CscdIRect used) {
    if (!img) return;
    if (cscdImageEmpty(img)) {
        cscdRelease(img);
        return;
    }
    for (int y = cscdMaxi(0, used.y); y < cscdMini(img->height, used.y + used.h); ++y) {
        uint32_t* r = cscdImageRow(img, y);
        const int a = cscdMaxi(0, used.x), b = cscdMini(img->width, used.x + used.w);
        if (b > a) memset(r + a, 0, (size_t)(b - a) * 4);
    }
    cscdMutexLock(&layerPoolMutex);
    if (layerPool.len >= 8) {
        cscdRelease(layerPool.items[0]);
        cscdVecRemove(&layerPool, 0);
    }
    cscdVecPush(&layerPool, img);
    cscdMutexUnlock(&layerPoolMutex);
}

void cscdCanvasPushLayer(CscdCanvas* c, float opacity, CscdBlend blend) {
    Op* op = record(c, OP_PUSH_LAYER);
    if (op) { op->f[0] = opacity; op->blend = blend; }
    Layer L;
    memset(&L, 0, sizeof L);
    /* the bands of a page painted on several threads share each depth's layer: their rows differ */
    const size_t depth = c->layers.len;
    if (c->bandLayers && depth < c->bandLayers->len) L.shared = (CscdImage*)c->bandLayers->items[depth];
    else L.image = skip(c) ? cscdImageNew(0, 0) : takeLayerImage(c->base->width, c->base->height);
    if (c->list) c->list->layerDepth = cscdMaxi(c->list->layerDepth, (int)depth + 1);
    L.parentTarget = st(c)->target;
    L.opacity = opacity;
    L.blend = blend;
    L.filter = cscdFilterNone();
    L.dirty = cscdIRect(0, 0, 0, 0);
    cscdVecPush(&c->layers, L);
    State s = *st(c);
    s.target = layerImg(&cscdVecLast(&c->layers));
    s.layer = (int)c->layers.len - 1;
    s.alpha = 1;
    pushState(c, s);
    endRecord(c, op);
}

/* The color functions of a filter on one premultiplied pixel. */
static uint32_t colorFilter(uint32_t p, const CscdFilter* f) {
    const float a = (p >> 24) / 255.f;
    if (a <= 0) return p;
    float b = (p & 255) / 255.f / a, g = ((p >> 8) & 255) / 255.f / a, r = ((p >> 16) & 255) / 255.f / a;
    if (f->grayscale > 0) {
        const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        r += (l - r) * f->grayscale; g += (l - g) * f->grayscale; b += (l - b) * f->grayscale;
    }
    if (f->saturate != 1) {
        const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        r = l + (r - l) * f->saturate; g = l + (g - l) * f->saturate; b = l + (b - l) * f->saturate;
    }
    if (f->invert > 0) { r += (1 - 2 * r) * f->invert; g += (1 - 2 * g) * f->invert; b += (1 - 2 * b) * f->invert; }
    r *= f->brightness; g *= f->brightness; b *= f->brightness;
    return cscdColorPremul(cscdColorFromFloat(cscdClampf(r, 0.f, 1.f), cscdClampf(g, 0.f, 1.f), cscdClampf(b, 0.f, 1.f), a));
}

static void markDirty(CscdCanvas* c, int x0, int y0, int x1, int y1) {
    if (st(c)->target == c->base) return;
    for (size_t i = 0; i < c->layers.len; ++i) {
        Layer* L = &c->layers.items[i];
        if (layerImg(L) != st(c)->target) continue;
        CscdIRect* d = &L->dirty;
        if (cscdIRectEmpty(*d)) { *d = cscdIRect(x0, y0, x1 - x0, y1 - y0); return; }
        int nx0 = cscdMini(d->x, x0), ny0 = cscdMini(d->y, y0), nx1 = cscdMaxi(d->x + d->w, x1), ny1 = cscdMaxi(d->y + d->h, y1);
        *d = cscdIRect(nx0, ny0, nx1 - nx0, ny1 - ny0);
        return;
    }
}

/* Blurs the four channels of w x h pixels of rows (stride apart) in place. */
static void blurPixels(uint32_t* px, size_t stride, int w, int h, float sigma) {
    CscdFloatVec ch[4] = {{0}};
    for (int k = 0; k < 4; ++k) {
        cscdVecResize(&ch[k], (size_t)w * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) ch[k].items[(size_t)y * w + x] = (float)((px[(size_t)y * stride + x] >> (8 * k)) & 255);
        cscdGaussianBlur(&ch[k], w, h, sigma);
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t p = 0;
            for (int k = 0; k < 4; ++k) p |= clampByte(ch[k].items[(size_t)y * w + x]) << (8 * k);
            px[(size_t)y * stride + x] = p;
        }
    for (int k = 0; k < 4; ++k) cscdVecFree(&ch[k]);
}

void cscdCanvasFilterBackdrop(CscdCanvas* c, const CscdRRect* shapeIn, const CscdFilter* f) {
    Op* op = record(c, OP_FILTER_BACKDROP);
    if (op) { op->rr = *shapeIn; op->filter = *f; }
    if (c->list && f->blur > 0) c->list->bandable = false; /* (reads what is drawn around it) */
    if (skip(c)) { endRecord(c, op); return; }
    CscdImage* t = cscdCanvasTarget(c);
    const CscdMatrix m = st(c)->m;
    CscdRRect d;
    if (axisAligned(c)) d = mapRRect(shapeIn, m);
    else d = cscdRRect(cscdMatMapRect(m, shapeIn->rect)); /* (rotated: its bounds, square) */
    cscdRRectNormalize(&d);
    const CscdRect area = cscdRectIntersect(d.rect, st(c)->clip);
    if (cscdRectEmpty(area)) { endRecord(c, op); return; }
    const int x0 = cscdMaxi(0, cscdIntOf(floorf(area.x))), y0 = cscdMaxi(0, cscdIntOf(floorf(area.y)));
    const int x1 = cscdMini(t->width, cscdIntOf(ceilf(cscdRectRight(area)))), y1 = cscdMini(t->height, cscdIntOf(ceilf(cscdRectBottom(area))));
    if (x1 <= x0 || y1 <= y0) { endRecord(c, op); return; }
    /* the blur reads beyond the box (what is next to it shows through its edge, as in browsers) */
    const int pad = f->blur > 0 ? cscdIntOf(ceilf(f->blur * 3)) : 0;
    const int bx0 = cscdMaxi(0, x0 - pad), by0 = cscdMaxi(0, y0 - pad), bx1 = cscdMini(t->width, x1 + pad), by1 = cscdMini(t->height, y1 + pad);
    const int w = bx1 - bx0, h = by1 - by0;
    uint32_t* filtered = (uint32_t*)cscdMalloc((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) memcpy(filtered + (size_t)y * w, cscdImageRow(t, by0 + y) + bx0, (size_t)w * 4);
    if (f->blur > 0) blurPixels(filtered, (size_t)w, w, h, f->blur);
    const bool colors = f->brightness != 1 || f->grayscale > 0 || f->saturate != 1 || f->invert > 0;
    const CscdMask* mask = st(c)->mask;
    for (int y = y0; y < y1; ++y) {
        uint32_t* row = cscdImageRow(t, y);
        for (int x = x0; x < x1; ++x) {
            uint32_t k = clampByte(rrectCoverage(&d, (float)x, (float)y) * 255);
            if (mask) k = mul8(k, cscdMaskAt(mask, x, y));
            if (!k) continue;
            uint32_t p = filtered[(size_t)(y - by0) * w + (x - bx0)];
            if (colors) p = colorFilter(p, f);
            row[x] = k == 255 ? p : mulPixel(p, k) + mulPixel(row[x], 255 - k);
        }
    }
    cscdFree(filtered);
    markDirty(c, x0, y0, x1, y1);
    endRecord(c, op);
}

void cscdCanvasMaskLayer(CscdCanvas* c, const CscdImage* mask) {
    Op* op = record(c, OP_MASK_LAYER);
    if (op) op->image = cscdImageClone(mask);
    if (!skip(c) && c->layers.len) {
        Layer* L = &cscdVecLast(&c->layers);
        CscdImage* img = layerImg(L);
        const CscdIRect d = L->dirty;
        const int yEnd = cscdMini(cscdMini(img->height, mask->height), d.y + d.h);
        const int xEnd = cscdMini(cscdMini(img->width, mask->width), d.x + d.w);
        for (int y = cscdMaxi(0, d.y); y < yEnd; ++y) {
            uint32_t* row = cscdImageRow(img, y);
            const uint32_t* mr = cscdImageRow(mask, y);
            for (int x = cscdMaxi(0, d.x); x < xEnd; ++x) {
                const uint32_t a = mr[x] >> 24;
                if (a == 255) continue;
                row[x] = a ? mulPixel(row[x], a) : 0;
            }
        }
    }
    endRecord(c, op);
}

void cscdCanvasSetLayerFilter(CscdCanvas* c, const CscdFilter* f) {
    Op* op = record(c, OP_SET_LAYER_FILTER);
    if (op) op->filter = *f;
    if (c->list && f->blur > 0) c->list->bandable = false; /* (the blur reads the layer around each pixel) */
    if (c->layers.len) cscdVecLast(&c->layers).filter = *f;
    endRecord(c, op);
}

void cscdCanvasPopLayer(CscdCanvas* c) {
    Op* op = record(c, OP_POP_LAYER);
    if (!c->layers.len) { endRecord(c, op); return; }
    /* unwind saves made inside the layer */
    while (c->stack.len > 1 && cscdVecLast(&c->stack).layer < 0) popState(c);
    if (c->stack.len > 1) popState(c);
    Layer L = cscdVecPop(&c->layers);
    CscdImage* src = layerImg(&L);
    CscdImage* dst = L.parentTarget;
    CscdIRect d = L.dirty;
    if (cscdIRectEmpty(d)) { /* (a shared one: nothing to clear) */
        giveLayerImage(L.image, d);
        endRecord(c, op);
        return;
    }
    const CscdFilter* f = &L.filter;
    if (f->blur > 0) {
        int pad = cscdIntOf(ceilf(f->blur * 3));
        int x0 = cscdMaxi(0, d.x - pad), y0 = cscdMaxi(0, d.y - pad);
        int x1 = cscdMini(src->width, d.x + d.w + pad), y1 = cscdMini(src->height, d.y + d.h + pad);
        int w = x1 - x0, h = y1 - y0;
        blurPixels(cscdImageRow(src, y0) + x0, (size_t)src->width, w, h, f->blur);
        d = cscdIRect(x0, y0, w, h);
    }
    bool colors = f->brightness != 1 || f->grayscale > 0 || f->saturate != 1 || f->invert > 0;
    uint32_t k = (uint32_t)cscdClampi((int)lroundf(L.opacity * 255), 0, 255);
    for (int y = d.y; y < d.y + d.h; ++y) {
        uint32_t* s = cscdImageRow(src, y);
        uint32_t* t = cscdImageRow(dst, y);
        for (int x = d.x; x < d.x + d.w; ++x) {
            uint32_t p = s[x];
            if (!p) continue;
            if (colors) {
                if ((p >> 24) == 0) continue;
                p = colorFilter(p, f);
            }
            if (k < 255) p = mulPixel(p, k);
            if (L.blend == CSCD_BLEND_SRC_OVER) { t[x] = over(t[x], p); continue; }
            /* separable blend modes on premultiplied colors */
            float sa = (p >> 24) / 255.f, da = (t[x] >> 24) / 255.f;
            float out[3];
            for (int ci = 0; ci < 3; ++ci) {
                float sc = ((p >> (8 * ci)) & 255) / 255.f, dc = ((t[x] >> (8 * ci)) & 255) / 255.f;
                float su = sa > 0 ? sc / sa : 0, du = da > 0 ? dc / da : 0;
                float bl;
                switch (L.blend) {
                case CSCD_BLEND_MULTIPLY: bl = su * du; break;
                case CSCD_BLEND_SCREEN: bl = su + du - su * du; break;
                case CSCD_BLEND_OVERLAY: bl = du <= 0.5f ? 2 * su * du : 1 - 2 * (1 - su) * (1 - du); break;
                case CSCD_BLEND_DARKEN: bl = cscdMinf(su, du); break;
                case CSCD_BLEND_LIGHTEN: bl = cscdMaxf(su, du); break;
                default: bl = su; break;
                }
                out[ci] = sc * (1 - da) + dc * (1 - sa) + sa * da * bl;
            }
            float oa = sa + da - sa * da;
            t[x] = clampByte(out[0] * 255) | (clampByte(out[1] * 255) << 8) | (clampByte(out[2] * 255) << 16) | (clampByte(oa * 255) << 24);
        }
    }
    /* the parent layer (if any) now has content in this area */
    if (c->layers.len && L.parentTarget == layerImg(&cscdVecLast(&c->layers))) markDirty(c, d.x, d.y, d.x + d.w, d.y + d.h);
    if (L.shared) {
        /* left transparent for the next layer at this depth (only this band's rows were drawn) */
        for (int y = d.y; y < d.y + d.h; ++y) memset(cscdImageRow(src, y) + d.x, 0, (size_t)d.w * 4);
        endRecord(c, op);
        return;
    }
    giveLayerImage(L.image, d);
    endRecord(c, op);
}

void cscdCanvasClear(CscdCanvas* c, CscdColor color) {
    Op* op = record(c, OP_CLEAR);
    if (op) op->color = color;
    if (!skip(c)) {
        uint32_t p = cscdColorPremul(color);
        CscdImage* t = cscdCanvasTarget(c);
        const CscdIRect r = c->region;
        for (int y = r.y; y < r.y + r.h; ++y) {
            uint32_t* row = cscdImageRow(t, y);
            for (int x = r.x; x < r.x + r.w; ++x) row[x] = p;
        }
        markDirty(c, r.x, r.y, r.x + r.w, r.y + r.h);
    }
    endRecord(c, op);
}

void cscdCanvasBlendSpan(CscdCanvas* c, int x, int y, int len, const uint8_t* cov, const CscdShader* s, CscdMatrix inv) {
    Op* op = record(c, OP_BLEND_SPAN);
    if (op) {
        op->ix = x;
        op->iy = y;
        op->shader = copyShader(s);
        op->m = inv;
        cscdVecAppend(&op->cov, cov, (size_t)cscdMaxi(0, len));
    }
    CscdImage* t = cscdCanvasTarget(c);
    /* shaders are evaluated through a context made by the caller; this entry point is for solid fills */
    if (!skip(c) && !(y < (int)st(c)->clip.y || y >= (int)cscdRectBottom(st(c)->clip)) && y >= 0 && y < t->height) {
        uint32_t col = premul(s->color, st(c)->alpha);
        uint32_t* row = cscdImageRow(t, y);
        const CscdMask* mask = st(c)->mask;
        for (int i = 0; i < len; ++i) {
            int px = x + i;
            if (px < 0 || px >= t->width) continue;
            uint32_t cv = cov[i];
            if (mask) cv = mul8(cv, cscdMaskAt(mask, px, y));
            if (!cv) continue;
            row[px] = over(row[px], cv == 255 ? col : mulPixel(col, cv));
        }
        markDirty(c, x, y, x + len, y + 1);
    }
    endRecord(c, op);
}

/* ---- filling ------------------------------------------------------------------------------------- */

/* Writes one row of coverage through the clip mask and a shader. */
typedef struct RowBlender {
    CscdImage* t;
    const CscdMask* mask;
    const ShaderCtx* sc;
} RowBlender;

/* panels and backgrounds: runs of full coverage are stored (opaque) or blended with one color */
static void solidRun(uint32_t* row, int x0, int len, const uint8_t* cov, uint32_t col) {
    if (!col) return;
    for (int i = 0; i < len;) {
        const uint32_t cv = cov[i];
        if (cv == 255) {
            const int n = fullRun(cov + i, len - i);
            fillPixels(row + x0 + i, n, col);
            i += n;
            continue;
        }
        if (cv) row[x0 + i] = over(row[x0 + i], mulPixel(col, cv));
        ++i;
    }
}

static CSCD_THREAD_LOCAL CscdU32Vec rowColors;

static void blendRow(const RowBlender* rb, int x0, int y, int len, const uint8_t* cov) {
    uint32_t* row = cscdImageRow(rb->t, y);
    const ShaderCtx* sc = rb->sc;
    const CscdMask* mask = rb->mask;
    bool solid = sc->s->kind == CSCD_SHADER_SOLID;
    if (solid && !mask) { solidRun(row, x0, len, cov, sc->solid); return; }
    if (sc->s->kind == CSCD_SHADER_LINEAR || sc->s->kind == CSCD_SHADER_RADIAL) {
        if ((int)rowColors.len < len) cscdVecResize(&rowColors, (size_t)len);
        uint32_t* colors = rowColors.items;
        if (shadeRow(sc, x0, y, len, colors)) {
            if (!mask) { solidRun(row, x0, len, cov, colors[0]); return; }
            for (int i = 1; i < len; ++i) colors[i] = colors[0];
        }
        for (int i = 0; i < len; ++i) {
            uint32_t cv = cov[i];
            if (mask) cv = mul8(cv, cscdMaskAt(mask, x0 + i, y));
            const uint32_t col = colors[i];
            if (!cv || !col) continue;
            row[x0 + i] = over(row[x0 + i], cv == 255 ? col : mulPixel(col, cv));
        }
        return;
    }
    for (int i = 0; i < len; ++i) {
        int x = x0 + i;
        uint32_t cv = cov[i];
        if (mask) cv = mul8(cv, cscdMaskAt(mask, x, y));
        if (!cv) continue;
        uint32_t col = solid ? sc->solid : shadeAt(sc, x + 0.5f, y + 0.5f);
        if (!col) continue;
        row[x] = over(row[x], cv == 255 ? col : mulPixel(col, cv));
    }
}

static void rasterize(CscdCanvas* c, const CscdPolyVec* contours, CscdFillRule rule, const CscdShader* s) {
    if (skip(c)) return;
    if (!contours->len) return;
    float l = 1e30f, t = 1e30f, r = -1e30f, b = -1e30f;
    for (size_t i = 0; i < contours->len; ++i)
        for (size_t k = 0; k < contours->items[i].len; ++k) {
            CscdPoint p = contours->items[i].items[k];
            l = cscdMinf(l, p.x); t = cscdMinf(t, p.y); r = cscdMaxf(r, p.x); b = cscdMaxf(b, p.y);
        }
    CscdRect clip = st(c)->clip;
    int x0 = cscdMaxi(cscdIntOf(floorf(l)), (int)clip.x), y0 = cscdMaxi(cscdIntOf(floorf(t)), (int)clip.y);
    int x1 = cscdMini(cscdIntOf(ceilf(r)) + 1, (int)cscdRectRight(clip)), y1 = cscdMini(cscdIntOf(ceilf(b)) + 1, (int)cscdRectBottom(clip));
    if (x1 <= x0 || y1 <= y0) return;
    CscdAccumulator acc = {0};
    cscdAccReset(&acc, x0, y0, x1 - x0, y1 - y0);
    cscdAccAddPolys(&acc, contours);
    ShaderCtx sc;
    shaderInit(&sc, s, st(c)->m, st(c)->alpha);
    RowBlender rb = {cscdCanvasTarget(c), st(c)->mask, &sc};
    uint8_t* row = (uint8_t*)cscdMalloc((size_t)(x1 - x0));
    for (int y = 0; y < acc.h; ++y) {
        cscdAccRow(&acc, y, rule, row);
        blendRow(&rb, x0, y0 + y, x1 - x0, row);
    }
    cscdFree(row);
    cscdAccFree(&acc);
    markDirty(c, x0, y0, x1, y1);
}

static void fillPathImpl(CscdCanvas* c, const CscdPath* p, const CscdShader* s, CscdFillRule rule) {
    if (skip(c)) return;
    CscdPolyVec contours = {0};
    cscdFlattenInto(p, st(c)->m, 0.2f, &contours);
    rasterize(c, &contours, rule, s);
    cscdPolyVecFree(&contours);
}

void cscdCanvasFillPathShader(CscdCanvas* c, const CscdPath* p, const CscdShader* s, CscdFillRule rule) {
    Op* op = record(c, OP_FILL_PATH);
    if (op) { op->path = copyPath(p); op->shader = copyShader(s); op->rule = rule; }
    fillPathImpl(c, p, s, rule);
    endRecord(c, op);
}

void cscdCanvasFillPath(CscdCanvas* c, const CscdPath* p, CscdColor color, CscdFillRule rule) {
    CscdShader s = cscdShaderSolid(color);
    cscdCanvasFillPathShader(c, p, &s, rule);
}

typedef struct StrokeCtx {
    const CscdStrokeStyle* st;
    const float* dash;
    size_t ndash;
    float dashOffset;
    float hw;
    CscdPolyVec* polys;
} StrokeCtx;

static void strokeContour(void* ctx, const CscdPoint* poly, size_t n, bool closed) {
    StrokeCtx* sc = (StrokeCtx*)ctx;
    if (sc->ndash) {
        CscdPolyVec pieces = {0};
        cscdApplyDash(poly, n, closed, sc->dash, sc->ndash, sc->dashOffset, &pieces);
        for (size_t i = 0; i < pieces.len; ++i) cscdStrokePolyline(pieces.items[i].items, pieces.items[i].len, false, sc->st, sc->hw, sc->polys);
        cscdPolyVecFree(&pieces);
    } else cscdStrokePolyline(poly, n, closed, sc->st, sc->hw, sc->polys);
}

static void strokePathImpl(CscdCanvas* c, const CscdPath* p, const CscdShader* s, const CscdStrokeStyle* stl) {
    if (skip(c)) return;
    const CscdMatrix m = st(c)->m;
    float scale = sqrtf(fabsf(m.a * m.d - m.b * m.c));
    float hw = stl->width * scale / 2;
    if (hw <= 0) return;
    CscdFloatVec dash = {0};
    for (size_t i = 0; i < stl->dash.len; ++i) cscdVecPush(&dash, stl->dash.items[i] * scale);
    if (dash.len % 2) cscdVecAppend(&dash, dash.items, dash.len);
    CscdPolyVec polys = {0};
    StrokeCtx ctx = {stl, dash.items, dash.len, stl->dashOffset * scale, hw, &polys};
    cscdPathFlatten(p, m, 0.2f, strokeContour, &ctx);
    /* hairlines thinner than a pixel fade instead of disappearing */
    CscdShader sh = *s;
    if (hw < 0.5f) sh.opacity *= hw * 2;
    rasterize(c, &polys, CSCD_FILL_NONZERO, &sh);
    cscdPolyVecFree(&polys);
    cscdVecFree(&dash);
}

void cscdCanvasStrokePathShader(CscdCanvas* c, const CscdPath* p, const CscdShader* s, const CscdStrokeStyle* stl) {
    Op* op = record(c, OP_STROKE_PATH);
    if (op) {
        op->path = copyPath(p);
        op->shader = copyShader(s);
        op->stroke = CSCD_NEW(CscdStrokeStyle);
        *op->stroke = *stl;
        memset(&op->stroke->dash, 0, sizeof op->stroke->dash);
        cscdVecAppend(&op->stroke->dash, stl->dash.items, stl->dash.len);
    }
    strokePathImpl(c, p, s, stl);
    endRecord(c, op);
}

void cscdCanvasStrokePath(CscdCanvas* c, const CscdPath* p, CscdColor color, float width) {
    CscdStrokeStyle stl = cscdStrokeStyle(width);
    CscdShader s = cscdShaderSolid(color);
    cscdCanvasStrokePathShader(c, p, &s, &stl);
}

static void fillRRectImpl(CscdCanvas* c, const CscdRRect* rin, const CscdShader* s) {
    if (skip(c)) return;
    if (cscdRectEmpty(rin->rect)) return;
    if (!axisAligned(c)) {
        CscdPath p = {0};
        cscdPathAddRoundRect(&p, rin->rect, rin->rx, rin->ry);
        cscdCanvasFillPathShader(c, &p, s, CSCD_FILL_NONZERO);
        cscdPathFree(&p);
        return;
    }
    CscdRRect d = mapRRect(rin, st(c)->m);
    cscdRRectNormalize(&d);
    CscdRect clip = st(c)->clip;
    int x0 = cscdMaxi(cscdIntOf(floorf(d.rect.x)), (int)clip.x), y0 = cscdMaxi(cscdIntOf(floorf(d.rect.y)), (int)clip.y);
    int x1 = cscdMini(cscdIntOf(ceilf(cscdRectRight(d.rect))), (int)cscdRectRight(clip)), y1 = cscdMini(cscdIntOf(ceilf(cscdRectBottom(d.rect))), (int)cscdRectBottom(clip));
    if (x1 <= x0 || y1 <= y0) return;
    ShaderCtx sc;
    shaderInit(&sc, s, st(c)->m, st(c)->alpha);
    RowBlender rb = {cscdCanvasTarget(c), st(c)->mask, &sc};
    uint8_t* row = (uint8_t*)cscdMalloc((size_t)(x1 - x0));
    float maxTop = cscdMaxf(d.ry[0], d.ry[1]), maxBottom = cscdMaxf(d.ry[2], d.ry[3]);
    float maxLeft = cscdMaxf(d.rx[0], d.rx[3]), maxRight = cscdMaxf(d.rx[1], d.rx[2]);
    const bool isRect = cscdRRectIsRect(&d);
    const bool solid = s->kind == CSCD_SHADER_SOLID && !rb.mask; /* (what blendRow gives solidRun) */
    for (int y = y0; y < y1; ++y) {
        bool inCornerRows = (y + 1 > d.rect.y && y < d.rect.y + maxTop + 1) || (y + 1 > cscdRectBottom(d.rect) - maxBottom - 1);
        float covy = cscdClampf(cscdMinf(y + 1.f, cscdRectBottom(d.rect)) - cscdMaxf((float)y, d.rect.y), 0.f, 1.f);
        if (!inCornerRows || isRect) {
            const uint8_t inside = (uint8_t)(covy * 255.f + 0.5f);
            const int in0 = cscdMaxi(x0, cscdIntOf(ceilf(d.rect.x))), in1 = cscdMini(x1, cscdIntOf(floorf(cscdRectRight(d.rect))));
            if (solid && inside == 255 && in0 < in1) {
                /* a row of a solid fill: its inside filled at once, the edge pixels by their coverage */
                if (!sc.solid) continue;
                for (int x = x0; x < in0; ++x) row[x - x0] = (uint8_t)(cscdClampf(cscdMinf(x + 1.f, cscdRectRight(d.rect)) - cscdMaxf((float)x, d.rect.x), 0.f, 1.f) * covy * 255.f + 0.5f);
                for (int x = in1; x < x1; ++x) row[x - x0] = (uint8_t)(cscdClampf(cscdMinf(x + 1.f, cscdRectRight(d.rect)) - cscdMaxf((float)x, d.rect.x), 0.f, 1.f) * covy * 255.f + 0.5f);
                uint32_t* px = cscdImageRow(rb.t, y);
                solidRun(px, x0, in0 - x0, row, sc.solid);
                fillPixels(px + in0, in1 - in0, sc.solid);
                solidRun(px, in1, x1 - in1, row + (in1 - x0), sc.solid);
                continue;
            }
            for (int x = x0; x < x1; ++x) {
                if (x >= in0 && x < in1) {
                    memset(&row[x - x0], inside, (size_t)(in1 - x));
                    x = in1 - 1;
                    continue;
                }
                float covx = cscdClampf(cscdMinf(x + 1.f, cscdRectRight(d.rect)) - cscdMaxf((float)x, d.rect.x), 0.f, 1.f);
                row[x - x0] = (uint8_t)(covx * covy * 255.f + 0.5f);
            }
        } else {
            for (int x = x0; x < x1; ++x) {
                if (x > d.rect.x + maxLeft + 1 && x + 1 < cscdRectRight(d.rect) - maxRight - 1) {
                    row[x - x0] = (uint8_t)(covy * 255.f + 0.5f);
                    continue;
                }
                row[x - x0] = (uint8_t)(rrectCoverage(&d, (float)x, (float)y) * 255.f + 0.5f);
            }
        }
        blendRow(&rb, x0, y, x1 - x0, row);
    }
    cscdFree(row);
    markDirty(c, x0, y0, x1, y1);
}

void cscdCanvasFillRRectShader(CscdCanvas* c, const CscdRRect* r, const CscdShader* s) {
    Op* op = record(c, OP_FILL_RRECT);
    if (op) { op->rr = *r; op->shader = copyShader(s); }
    fillRRectImpl(c, r, s);
    endRecord(c, op);
}

void cscdCanvasFillRRect(CscdCanvas* c, const CscdRRect* r, CscdColor color) {
    CscdShader s = cscdShaderSolid(color);
    cscdCanvasFillRRectShader(c, r, &s);
}

void cscdCanvasFillRectShader(CscdCanvas* c, CscdRect r, const CscdShader* s) {
    CscdRRect rr = cscdRRect(r);
    cscdCanvasFillRRectShader(c, &rr, s);
}

void cscdCanvasFillRect(CscdCanvas* c, CscdRect r, CscdColor color) {
    CscdShader s = cscdShaderSolid(color);
    cscdCanvasFillRectShader(c, r, &s);
}

/* the columns where neither box has a corner nor a side */
static void straightColumns(const CscdRRect* r, int* a, int* b) {
    *a = cscdMaxi(cscdIntOf(ceilf(r->rect.x)), cscdIntOf(ceilf(r->rect.x + cscdMaxf(r->rx[0], r->rx[3]) - 0.5f)));
    *b = cscdMini(cscdIntOf(floorf(cscdRectRight(r->rect))) - 1, cscdIntOf(floorf(cscdRectRight(r->rect) - cscdMaxf(r->rx[1], r->rx[2]) - 0.5f))) + 1;
}

static float rowCoverage(const CscdRRect* r, int y) { return cscdClampf(cscdMinf(y + 1.f, cscdRectBottom(r->rect)) - cscdMaxf((float)y, r->rect.y), 0.f, 1.f); }

typedef struct Ring {
    const CscdRRect* o;
    const CscdRRect* in;
    bool hasInner;
    int sx0, sx1, x0;
    uint8_t* row;
} Ring;

static void ringCover(const Ring* g, int from, int to, int y) {
    const int m0 = cscdClampi(g->sx0, from, to), m1 = cscdClampi(g->sx1, m0, to);
    /* (rows away from the corners: the sides only) */
    const bool outerClear = rowClearOfCorners(g->o, (float)y), innerClear = g->hasInner && rowClearOfCorners(g->in, (float)y);
    for (int x = from; x < to; ++x) {
        if (x == m0 && m0 < m1) {
            float cv = rowCoverage(g->o, y);
            if (g->hasInner && cv > 0) cv = cscdMaxf(0.f, cv - rowCoverage(g->in, y));
            memset(&g->row[m0 - g->x0], (uint8_t)(cv * 255.f + 0.5f), (size_t)(m1 - m0));
            x = m1 - 1;
            continue;
        }
        float cv = outerClear ? rectCoverage(g->o, (float)x, (float)y) : rrectCoverage(g->o, (float)x, (float)y);
        if (g->hasInner && cv > 0) cv = cscdMaxf(0.f, cv - (innerClear ? rectCoverage(g->in, (float)x, (float)y) : rrectCoverage(g->in, (float)x, (float)y)));
        g->row[x - g->x0] = (uint8_t)(cv * 255.f + 0.5f);
    }
}

static void reversedContour(void* ctx, const CscdPoint* pts, size_t n, bool closed) {
    (void)closed;
    CscdPolyVec* out = (CscdPolyVec*)ctx;
    CscdPointVec* p = cscdVecPushZero(out);
    for (size_t i = n; i-- > 0;) cscdVecPush(p, pts[i]);
}

static void fillRingImpl(CscdCanvas* c, const CscdRRect* outerIn, const CscdRRect* innerIn, const CscdShader* s) {
    if (skip(c)) return;
    if (!axisAligned(c)) {
        CscdPath p = {0};
        cscdPathAddRoundRect(&p, outerIn->rect, outerIn->rx, outerIn->ry);
        if (!cscdRectEmpty(innerIn->rect)) {
            /* inner contour reversed so non-zero leaves a hole */
            CscdPath inner = {0};
            cscdPathAddRoundRect(&inner, innerIn->rect, innerIn->rx, innerIn->ry);
            CscdPolyVec contours = {0};
            cscdFlattenInto(&p, st(c)->m, 0.2f, &contours);
            cscdPathFlatten(&inner, st(c)->m, 0.2f, reversedContour, &contours);
            rasterize(c, &contours, CSCD_FILL_NONZERO, s);
            cscdPolyVecFree(&contours);
            cscdPathFree(&inner);
            cscdPathFree(&p);
            return;
        }
        cscdCanvasFillPathShader(c, &p, s, CSCD_FILL_NONZERO);
        cscdPathFree(&p);
        return;
    }
    CscdRRect o = mapRRect(outerIn, st(c)->m), in = mapRRect(innerIn, st(c)->m);
    cscdRRectNormalize(&o);
    cscdRRectNormalize(&in);
    CscdRect clip = st(c)->clip;
    int x0 = cscdMaxi(cscdIntOf(floorf(o.rect.x)), (int)clip.x), y0 = cscdMaxi(cscdIntOf(floorf(o.rect.y)), (int)clip.y);
    int x1 = cscdMini(cscdIntOf(ceilf(cscdRectRight(o.rect))), (int)cscdRectRight(clip)), y1 = cscdMini(cscdIntOf(ceilf(cscdRectBottom(o.rect))), (int)cscdRectBottom(clip));
    if (x1 <= x0 || y1 <= y0) return;
    ShaderCtx sc;
    shaderInit(&sc, s, st(c)->m, st(c)->alpha);
    RowBlender rb = {cscdCanvasTarget(c), st(c)->mask, &sc};
    uint8_t* row = (uint8_t*)cscdMalloc((size_t)(x1 - x0));
    bool hasInner = !cscdRectEmpty(in.rect);
    /* the columns of the hole, away from its corners: nothing is drawn there (a border is drawn as
     * its four edges, not as the area of its box) */
    const float holeL = in.rect.x + cscdMaxf(in.rx[0], in.rx[3]) + 1, holeR = cscdRectRight(in.rect) - cscdMaxf(in.rx[1], in.rx[2]) - 1;
    const int hx0 = cscdClampi(cscdIntOf(floorf(holeL)) + 1, x0, x1), hx1 = cscdClampi(cscdIntOf(ceilf(holeR - 1)), x0, x1);
    /* the columns where neither box has a corner nor a side: a row's coverage there is the same for
     * every pixel (what rrectCoverage gives for them), worked out once */
    int sx0, sx1;
    straightColumns(&o, &sx0, &sx1);
    if (hasInner) {
        int a, b;
        straightColumns(&in, &a, &b);
        sx0 = cscdMaxi(sx0, a);
        sx1 = cscdMini(sx1, b);
    }
    Ring g = {&o, &in, hasInner, sx0, sx1, x0, row};
    for (int y = y0; y < y1; ++y) {
        if (hasInner && hx0 < hx1 && y > in.rect.y + 1 && y + 1 < cscdRectBottom(in.rect) - 1) {
            ringCover(&g, x0, hx0, y);
            ringCover(&g, hx1, x1, y);
            blendRow(&rb, x0, y, hx0 - x0, row);
            blendRow(&rb, hx1, y, x1 - hx1, row + (hx1 - x0));
            continue;
        }
        ringCover(&g, x0, x1, y);
        blendRow(&rb, x0, y, x1 - x0, row);
    }
    cscdFree(row);
    markDirty(c, x0, y0, x1, y1);
}

void cscdCanvasFillRing(CscdCanvas* c, const CscdRRect* outer, const CscdRRect* inner, const CscdShader* s) {
    Op* op = record(c, OP_FILL_RING);
    if (op) { op->rr = *outer; op->rr2 = *inner; op->shader = copyShader(s); }
    fillRingImpl(c, outer, inner, s);
    endRecord(c, op);
}

void cscdCanvasStrokeRect(CscdCanvas* c, CscdRect r, CscdColor color, float w) {
    CscdRRect o = cscdRRect(cscdRectInset(r, -w / 2, -w / 2, -w / 2, -w / 2));
    CscdRRect i = cscdRRect(cscdRectInset(r, w / 2, w / 2, w / 2, w / 2));
    CscdShader s = cscdShaderSolid(color);
    cscdCanvasFillRing(c, &o, &i, &s);
}

void cscdCanvasDrawLine(CscdCanvas* c, CscdPoint a, CscdPoint b, CscdColor color, float width, CscdLineCap cap) {
    CscdPath p = {0};
    cscdPathMoveTo(&p, a.x, a.y);
    cscdPathLineTo(&p, b.x, b.y);
    CscdStrokeStyle stl = cscdStrokeStyle(width);
    stl.cap = cap;
    CscdShader s = cscdShaderSolid(color);
    cscdCanvasStrokePathShader(c, &p, &s, &stl);
    cscdPathFree(&p);
}

void cscdCanvasFillCircle(CscdCanvas* c, CscdPoint center, float r, CscdColor color) {
    CscdRRect rr = cscdRRect(cscdRect(center.x - r, center.y - r, 2 * r, 2 * r));
    for (int i = 0; i < 4; ++i) rr.rx[i] = rr.ry[i] = r;
    cscdCanvasFillRRect(c, &rr, color);
}

/* ---- images ------------------------------------------------------------------------------------- */

static uint32_t projPx(const CscdImage* img, int x, int y) { return x < 0 || y < 0 || x >= img->width || y >= img->height ? 0u : cscdImageRow(img, y)[x]; }

void cscdCanvasDrawImageProjective(CscdCanvas* c, const CscdImage* img, const float h[9]) {
    Op* op = record(c, OP_IMAGE_PROJECTIVE);
    if (op) { op->image = cscdImageClone(img); memcpy(op->f, h, 9 * sizeof(float)); }
    if (skip(c) || cscdImageEmpty(img)) { endRecord(c, op); return; }
    /* the inverse: canvas pixels back to image pixels */
    const float a = h[4] * h[8] - h[5] * h[7], b = h[5] * h[6] - h[3] * h[8], cf = h[3] * h[7] - h[4] * h[6];
    const float det = h[0] * a + h[1] * b + h[2] * cf;
    if (fabsf(det) < 1e-12f) { endRecord(c, op); return; }
    const float i[9] = {a / det, (h[2] * h[7] - h[1] * h[8]) / det, (h[1] * h[5] - h[2] * h[4]) / det,
                        b / det, (h[0] * h[8] - h[2] * h[6]) / det, (h[2] * h[3] - h[0] * h[5]) / det,
                        cf / det, (h[1] * h[6] - h[0] * h[7]) / det, (h[0] * h[4] - h[1] * h[3]) / det};
    /* where it lands: its corners (all of the clip when one is behind the viewer) */
    CscdRect clip = st(c)->clip;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool behind = false;
    for (int k = 0; k < 4; ++k) {
        const float u = (k & 1) ? (float)img->width : 0.f, v = (k & 2) ? (float)img->height : 0.f;
        const float w = h[6] * u + h[7] * v + h[8];
        if (w <= 1e-6f) { behind = true; break; }
        const float x = (h[0] * u + h[1] * v + h[2]) / w, y = (h[3] * u + h[4] * v + h[5]) / w;
        x0 = cscdMinf(x0, x); y0 = cscdMinf(y0, y); x1 = cscdMaxf(x1, x); y1 = cscdMaxf(y1, y);
    }
    CscdRect area = behind ? clip : cscdRectIntersect(cscdRect(x0 - 1, y0 - 1, x1 - x0 + 2, y1 - y0 + 2), clip);
    if (cscdRectEmpty(area)) { endRecord(c, op); return; }
    CscdImage* t = cscdCanvasTarget(c);
    const int ax0 = cscdMaxi(0, cscdIntOf(floorf(area.x))), ay0 = cscdMaxi(0, cscdIntOf(floorf(area.y)));
    const int ax1 = cscdMini(t->width, cscdIntOf(ceilf(cscdRectRight(area)))), ay1 = cscdMini(t->height, cscdIntOf(ceilf(cscdRectBottom(area))));
    const CscdMask* mask = st(c)->mask;
    const uint32_t alpha = (uint32_t)cscdClampi((int)lroundf(st(c)->alpha * 255), 0, 255);
    /* bilinear, with transparent pixels around the image: its edges come out anti-aliased */
    for (int y = ay0; y < ay1; ++y) {
        uint32_t* row = cscdImageRow(t, y);
        const float cy = y + 0.5f, cx0 = ax0 + 0.5f;
        /* the numerators and the divisor step by a constant along the row */
        float nu = i[0] * cx0 + i[1] * cy + i[2], nv = i[3] * cx0 + i[4] * cy + i[5], nw = i[6] * cx0 + i[7] * cy + i[8];
        for (int x = ax0; x < ax1; ++x, nu += i[0], nv += i[3], nw += i[6]) {
            if (nw <= 0) continue;
            const float rw = 1 / nw;
            const float u = nu * rw - 0.5f, v = nv * rw - 0.5f;
            if (u <= -1 || v <= -1 || u >= img->width || v >= img->height) continue;
            const int ux = (int)(u + 1) - 1, vy = (int)(v + 1) - 1; /* (floor: u, v > -1) */
            const uint32_t fx = (uint32_t)((u - ux) * 256), fy = (uint32_t)((v - vy) * 256);
            uint32_t p00, p10, p01, p11;
            if (ux >= 0 && vy >= 0 && ux + 1 < img->width && vy + 1 < img->height) { /* (inside: no bounds to check) */
                const uint32_t* r0 = cscdImageRow(img, vy) + ux;
                const uint32_t* r1 = cscdImageRow(img, vy + 1) + ux;
                p00 = r0[0]; p10 = r0[1]; p01 = r1[0]; p11 = r1[1];
            } else {
                p00 = projPx(img, ux, vy); p10 = projPx(img, ux + 1, vy); p01 = projPx(img, ux, vy + 1); p11 = projPx(img, ux + 1, vy + 1);
            }
            if (!(p00 | p10 | p01 | p11)) continue;
            const bool plain = p00 == p10 && p00 == p01 && p00 == p11; /* (most of a flat color, or of a picture's sky) */
            uint32_t p = plain ? p00 : lerpPx(lerpPx(p00, p10, fx), lerpPx(p01, p11, fx), fy);
            if (!p) continue;
            uint32_t k = alpha;
            if (mask) k = mul8(k, cscdMaskAt(mask, x, y));
            if (k < 255) p = mulPixel(p, k);
            row[x] = p >> 24 == 255 ? p : over(row[x], p);
        }
    }
    markDirty(c, ax0, ay0, ax1, ay1);
    endRecord(c, op);
}

typedef struct Sample { int a, b; uint32_t w; uint8_t cov; bool in; } Sample;

/* where a device pixel's middle falls in the picture, along one axis (as sampleImage) */
static Sample* imageSamples(int from, int to, float scale, float offset, float pos, float size, int n, float d0, float d1, bool smooth) {
    Sample* out = (Sample*)cscdCalloc((size_t)(to - from), sizeof(Sample));
    for (int i = from; i < to; ++i) {
        Sample* sm = &out[i - from];
        const float l = (i + 0.5f - offset) / scale; /* local coordinate */
        sm->in = l >= pos && l < pos + size;
        const float u = (l - pos) * n / size - 0.5f;
        if (!smooth) {
            sm->a = sm->b = cscdClampi(cscdIntOf(floorf(u + 0.5f)), 0, n - 1);
            sm->w = 0;
        } else {
            const int k = cscdIntOf(floorf(u));
            sm->a = cscdClampi(k, 0, n - 1);
            sm->b = cscdClampi(k + 1, 0, n - 1);
            sm->w = (uint32_t)((u - k) * 256);
        }
        sm->cov = (uint8_t)(cscdClampf(cscdMinf(i + 1.f, d1) - cscdMaxf((float)i, d0), 0.f, 1.f) * 255.f + 0.5f);
    }
    return out;
}

static void drawScaledImage(CscdCanvas* c, const CscdImage* img, CscdRect placed, CscdRect dst, bool smooth) {
    const CscdMatrix m = st(c)->m;
    const CscdRect dev = cscdRect(m.a * dst.x + m.e, m.d * dst.y + m.f, m.a * dst.w, m.d * dst.h);
    const CscdRect clip = st(c)->clip;
    const int x0 = cscdMaxi(cscdIntOf(floorf(dev.x)), (int)clip.x), y0 = cscdMaxi(cscdIntOf(floorf(dev.y)), (int)clip.y);
    const int x1 = cscdMini(cscdIntOf(ceilf(cscdRectRight(dev))), (int)cscdRectRight(clip)), y1 = cscdMini(cscdIntOf(ceilf(cscdRectBottom(dev))), (int)cscdRectBottom(clip));
    if (x1 <= x0 || y1 <= y0 || placed.w <= 0 || placed.h <= 0 || cscdImageEmpty(img)) return;
    Sample* cols = imageSamples(x0, x1, m.a, m.e, placed.x, placed.w, img->width, dev.x, cscdRectRight(dev), smooth);
    Sample* rows = imageSamples(y0, y1, m.d, m.f, placed.y, placed.h, img->height, dev.y, cscdRectBottom(dev), smooth);
    const float alpha = st(c)->alpha;
    const uint32_t k = (uint32_t)(alpha * 255 + 0.5f);
    const CscdMask* mask = st(c)->mask;
    CscdImage* t = cscdCanvasTarget(c);
    for (int y = y0; y < y1; ++y) {
        const Sample* r = &rows[y - y0];
        if (!r->in || !r->cov) continue;
        const uint32_t* ra = cscdImageRow(img, r->a);
        const uint32_t* rb = cscdImageRow(img, r->b);
        uint32_t* d = cscdImageRow(t, y);
        for (int x = x0; x < x1; ++x) {
            const Sample* cs = &cols[x - x0];
            if (!cs->in) continue;
            uint32_t px = smooth ? lerpPx(lerpPx(ra[cs->a], ra[cs->b], cs->w), lerpPx(rb[cs->a], rb[cs->b], cs->w), r->w) : ra[cs->a];
            if (alpha < 1) px = mulPixel(px, k);
            uint32_t cov = cs->cov == 255 && r->cov == 255
                               ? 255
                               : (uint32_t)(cscdClampf(cscdMinf(x + 1.f, cscdRectRight(dev)) - cscdMaxf((float)x, dev.x), 0.f, 1.f) *
                                                cscdClampf(cscdMinf(y + 1.f, cscdRectBottom(dev)) - cscdMaxf((float)y, dev.y), 0.f, 1.f) * 255.f + 0.5f);
            if (mask) cov = mul8(cov, cscdMaskAt(mask, x, y));
            if (!cov || !px) continue;
            d[x] = over(d[x], cov == 255 ? px : mulPixel(px, cov));
        }
    }
    cscdFree(cols);
    cscdFree(rows);
    markDirty(c, x0, y0, x1, y1);
}

/* A picture drawn much smaller is first reduced with a box filter (step pixels of it to one), so that
 * it does not alias. The step, 0 when it is not reduced. */
static int reductionStep(const CscdCanvas* c, const CscdImage* img, CscdRect src, CscdRect dst, bool smooth) {
    if (!smooth || img->width <= 2 || img->height <= 2) return 0;
    const CscdMatrix m = st(c)->m;
    const float devScale = sqrtf(fabsf(m.a * m.d - m.b * m.c));
    const float factor = cscdMinf(dst.w / src.w, dst.h / src.h) * devScale;
    return factor < 0.5f ? cscdMaxi(1, cscdIntOf(floorf(1 / factor))) : 0;
}

static CscdImage* reduceImage(const CscdImage* img, int step) {
    const int nw = cscdMaxi(1, img->width / step), nh = cscdMaxi(1, img->height / step);
    CscdImage* small = cscdImageNew(nw, nh);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            uint32_t acc[4] = {0, 0, 0, 0};
            int n = 0;
            for (int yy = y * step; yy < cscdMini(img->height, (y + 1) * step); ++yy)
                for (int xx = x * step; xx < cscdMini(img->width, (x + 1) * step); ++xx) {
                    uint32_t p = cscdImageRow(img, yy)[xx];
                    for (int ch = 0; ch < 4; ++ch) acc[ch] += (p >> (8 * ch)) & 255;
                    ++n;
                }
            uint32_t p = 0;
            for (int ch = 0; ch < 4; ++ch) p |= (acc[ch] / (uint32_t)cscdMaxi(1, n)) << (8 * ch);
            cscdImageRow(small, y)[x] = p;
        }
    return small;
}

/* reduced: the picture already reduced by its reductionStep (the bands of a frame share one), or NULL */
static void drawImageImpl(CscdCanvas* c, const CscdImage* img, CscdRect src, CscdRect dst, bool smooth, CscdImage* reduced) {
    if (skip(c)) return;
    if (cscdImageEmpty(img) || src.w <= 0 || src.h <= 0 || cscdRectEmpty(dst)) return;
    /* fast path: unscaled, pixel-aligned copy */
    const CscdMatrix m = st(c)->m;
    if (cscdMatIsTranslate(m) && src.w == dst.w && src.h == dst.h && fabsf(m.e + dst.x - roundf(m.e + dst.x)) < 0.01f &&
        fabsf(m.f + dst.y - roundf(m.f + dst.y)) < 0.01f && src.x == floorf(src.x) && src.y == floorf(src.y)) {
        int dx = cscdIntOf(roundf(m.e + dst.x)), dy = cscdIntOf(roundf(m.f + dst.y));
        CscdRect clip = st(c)->clip;
        int x0 = cscdMaxi(dx, (int)clip.x), y0 = cscdMaxi(dy, (int)clip.y);
        int x1 = cscdMini(dx + (int)dst.w, (int)cscdRectRight(clip)), y1 = cscdMini(dy + (int)dst.h, (int)cscdRectBottom(clip));
        uint32_t k = (uint32_t)lroundf(st(c)->alpha * 255);
        const CscdMask* mask = st(c)->mask;
        CscdImage* t = cscdCanvasTarget(c);
        for (int y = y0; y < y1; ++y) {
            const uint32_t* s = cscdImageRow(img, y - dy + (int)src.y) + (int)src.x - dx;
            uint32_t* d = cscdImageRow(t, y);
            for (int x = x0; x < x1; ++x) {
                uint32_t p = s[x];
                uint32_t cv = k;
                if (mask) cv = mul8(cv, cscdMaskAt(mask, x, y));
                if (cv < 255) p = mulPixel(p, cv);
                d[x] = over(d[x], p);
            }
        }
        if (x1 > x0 && y1 > y0) markDirty(c, x0, y0, x1, y1);
        return;
    }
    float sx = dst.w / src.w, sy = dst.h / src.h;
    CscdRect placed = cscdRect(dst.x - src.x * sx, dst.y - src.y * sy, img->width * sx, img->height * sy);
    const int step = reductionStep(c, img, src, dst, smooth);
    CscdImage* small = !step ? NULL : reduced ? (CscdImage*)cscdRetain(reduced) : reduceImage(img, step);
    const CscdImage* source = small ? small : img;
    if (m.b == 0 && m.c == 0 && m.a > 0 && m.d > 0) {
        /* only scaled (a picture enlarged or reduced): the columns' and rows' samples worked out once,
         * then the pixels blended by rows. The same samples as the image pattern's. */
        drawScaledImage(c, source, placed, dst, smooth);
        cscdRelease(small);
        return;
    }
    /* (the shader holds a shared image: a borrowed one is copied) */
    CscdImage* shared = small ? small : cscdImageClone(img);
    CscdShader sh = cscdShaderImage(shared, placed, false, false);
    cscdRelease(shared);
    sh.smooth = smooth;
    cscdCanvasFillRectShader(c, dst, &sh);
    cscdShaderFini(&sh);
}

void cscdCanvasDrawImageRect(CscdCanvas* c, const CscdImage* img, CscdRect src, CscdRect dst, bool smooth) {
    Op* op = record(c, OP_IMAGE);
    if (op) { op->image = cscdImageClone(img); op->r = src; op->r2 = dst; op->flag = smooth; }
    drawImageImpl(c, img, src, dst, smooth, NULL);
    endRecord(c, op);
}

void cscdCanvasDrawImage(CscdCanvas* c, const CscdImage* img, CscdRect dst, bool smooth) {
    cscdCanvasDrawImageRect(c, img, cscdRect(0, 0, (float)img->width, (float)img->height), dst, smooth);
}

void cscdCanvasDrawSharedImage(CscdCanvas* c, CscdImage* img, CscdRect src, CscdRect dst, bool smooth) {
    if (!img) return;
    Op* op = record(c, OP_IMAGE);
    if (op) { op->image = (CscdImage*)cscdRetain(img); op->r = src; op->r2 = dst; op->flag = smooth; }
    drawImageImpl(c, img, src, dst, smooth, NULL);
    endRecord(c, op);
}

/* ---- shadows --------------------------------------------------------------------------------------
 * Blurred shadows, by their shape: one for all the threads that paint bands (a shadow that crosses
 * several is blurred once, whatever its size), kept for the next frames within the limits
 * (cscdPaintCacheSetLimits). A page scrolling smoothly makes a new shape at every fraction of a
 * pixel: full, the cache drops what went longest unused, and a large shadow, which costs far more to
 * blur again, counts as used later than it was (shadowRank). */

typedef struct ShadowEntry {
    CscdOnce once; /* (the bands that want one being blurred wait for it) */
    CscdFloatVec values;
    CscdBytes alpha; /* the values as bytes (clampByte(v * 255)) */
    uint64_t rank;   /* when it was last used, plus what it costs */
    size_t bytes;
} ShadowEntry;

static void shadowEntryDtor(void* p) {
    cscdVecFree(&((ShadowEntry*)p)->values);
    cscdVecFree(&((ShadowEntry*)p)->alpha);
}

static CscdMutex shadowMutex = CSCD_MUTEX_INIT;
static CscdStrMap shadowMap; /* key bytes -> ShadowEntry* (a reference) */
static size_t shadowBytes;   /* what the map's shadows hold */
static size_t shadowLimitBytes = (size_t)128 << 20, shadowLimitCount = 1024;
static uint64_t shadowClock, shadowHits, shadowMisses, shadowEvictions;
/* A shadow with no blur has nothing costly to keep, unless it is large (a page dimmed by a spread). */
enum { kPlainShadowKept = 1 << 16 /* pixels */ };

static void shadowRankOf(const void* entry, RankedEntry* out) {
    out->rank = ((const ShadowEntry*)entry)->rank;
    out->bytes = ((const ShadowEntry*)entry)->bytes;
}

/* (with the lock) room for bytes more, down to three quarters of the limits so that it is not done
 * again at the next shadow */
static void shadowsMakeRoom(size_t bytes) {
    if (shadowBytes + bytes <= shadowLimitBytes && shadowMap.len < shadowLimitCount) return;
    const size_t goalBytes = shadowLimitBytes / 4 * 3 > bytes ? shadowLimitBytes / 4 * 3 - bytes : 0, goalCount = shadowLimitCount / 4 * 3;
    const size_t n = shadowMap.len;
    RankedEntry* all = rankedEntries(&shadowMap, shadowRankOf);
    for (size_t i = 0; i < n && (shadowBytes > goalBytes || shadowMap.len > goalCount); ++i) {
        shadowBytes -= all[i].bytes;
        dropEntry(&shadowMap, &all[i]);
        ++shadowEvictions;
    }
    rankedFree(all, n);
}

typedef struct ShadowJob {
    CscdFloatVec* values;
    CscdBytes* alpha;
    const CscdRRect* shape;
    int bx0, by0, w, h;
    float sigma;
    bool inset;
} ShadowJob;

static void blurShadow(void* arg) {
    ShadowJob* j = (ShadowJob*)arg;
    cscdVecResize(j->values, (size_t)j->w * j->h);
    for (int y = 0; y < j->h; ++y)
        for (int x = 0; x < j->w; ++x) {
            const float cv = rrectCoverage(j->shape, (float)(j->bx0 + x), (float)(j->by0 + y));
            j->values->items[(size_t)y * j->w + x] = j->inset ? 1 - cv : cv;
        }
    cscdGaussianBlur(j->values, j->w, j->h, j->sigma);
    cscdVecResize(j->alpha, j->values->len);
    for (size_t i = 0; i < j->values->len; ++i) j->alpha->items[i] = (uint8_t)clampByte(j->values->items[i] * 255);
}

static void drawBoxShadowImpl(CscdCanvas* c, const CscdRRect* box, float dx, float dy, float blur, float spread, CscdColor color, bool inset) {
    if (skip(c)) return;
    if (color.a == 0) return;
    const CscdMatrix m = st(c)->m;
    if (!axisAligned(c)) {
        /* rotated: unblurred approximation */
        if (inset) return;
        CscdRRect s = *box;
        s.rect = cscdRectTranslated(cscdRectInset(box->rect, -spread, -spread, -spread, -spread), dx, dy);
        for (int i = 0; i < 4; ++i) if (s.rx[i] > 0) { s.rx[i] += spread; s.ry[i] += spread; }
        cscdCanvasFillRRect(c, &s, cscdColorWithAlpha(color, blur > 0 ? 0.6f : 1.f));
        return;
    }
    float scale = m.a;
    float sigma = blur * scale / 2;
    CscdRRect dbox = mapRRect(box, m);
    cscdRRectNormalize(&dbox);
    CscdRRect shape = dbox;
    float sp = spread * scale;
    if (!inset) {
        shape.rect = cscdRectTranslated(cscdRectInset(dbox.rect, -sp, -sp, -sp, -sp), dx * scale, dy * scale);
        for (int i = 0; i < 4; ++i) if (shape.rx[i] > 0) { shape.rx[i] = cscdMaxf(0.f, shape.rx[i] + sp); shape.ry[i] = cscdMaxf(0.f, shape.ry[i] + sp); }
    } else {
        shape.rect = cscdRectTranslated(cscdRectInset(dbox.rect, sp, sp, sp, sp), dx * scale, dy * scale);
        for (int i = 0; i < 4; ++i) if (shape.rx[i] > 0) { shape.rx[i] = cscdMaxf(0.f, shape.rx[i] - sp); shape.ry[i] = cscdMaxf(0.f, shape.ry[i] - sp); }
        if (shape.rect.w < 0) shape.rect.w = 0;
        if (shape.rect.h < 0) shape.rect.h = 0;
    }
    cscdRRectNormalize(&shape);
    int pad = cscdIntOf(ceilf(sigma * 3)) + 1;
    CscdRect area = inset ? dbox.rect : cscdRectInset(shape.rect, (float)-pad, (float)-pad, (float)-pad, (float)-pad);
    CscdRect clip = st(c)->clip;
    int x0 = cscdMaxi(cscdIntOf(floorf(area.x)), (int)clip.x), y0 = cscdMaxi(cscdIntOf(floorf(area.y)), (int)clip.y);
    int x1 = cscdMini(cscdIntOf(ceilf(cscdRectRight(area))), (int)cscdRectRight(clip)), y1 = cscdMini(cscdIntOf(ceilf(cscdRectBottom(area))), (int)cscdRectBottom(clip));
    if (x1 <= x0 || y1 <= y0) return;
    /* The blur needs samples beyond the clip: the shadow is blurred for all of it (unclipped: the
     * same values in any clip, and in every band of a page painted on several threads). */
    const int bx0 = cscdIntOf(floorf(area.x)) - pad, by0 = cscdIntOf(floorf(area.y)) - pad;
    const int w = cscdIntOf(ceilf(cscdRectRight(area))) + pad - bx0, h = cscdIntOf(ceilf(cscdRectBottom(area))) + pad - by0;
    if ((double)w * h > 40000000) return;
    /* Shared: the bands of a page painted on several threads each need the one that crosses them
     * (blurred again by each, a large one cost more on four threads than on one), and a list of cards
     * repaints the same few shadows over and over. Those being drawn stay until they are done, even
     * if the cache drops them. */
    ShadowEntry* entry;
    {
        float key[16];
        int ik[2] = {w, h};
        key[0] = shape.rect.x - bx0; key[1] = shape.rect.y - by0; key[2] = shape.rect.w; key[3] = shape.rect.h;
        for (int i = 0; i < 4; ++i) { key[4 + i] = shape.rx[i]; key[8 + i] = shape.ry[i]; }
        key[12] = sigma;
        key[13] = inset ? 1.f : 0.f;
        char bytes[sizeof ik + 14 * sizeof(float)];
        memcpy(bytes, ik, sizeof ik);
        memcpy(bytes + sizeof ik, key, 14 * sizeof(float));
        CscdSv k = cscdSvMake(bytes, sizeof bytes);
        cscdMutexLock(&shadowMutex);
        const size_t pixels = (size_t)w * h, memory = pixels * (sizeof(float) + 1);
        const bool kept = (sigma > 0 || pixels >= kPlainShadowKept) && shadowLimitCount && memory <= shadowLimitBytes;
        CscdMapVal* v = kept ? cscdStrMapGet(&shadowMap, k) : NULL;
        if (v) ++shadowHits;
        else ++shadowMisses;
        if (!v && kept) {
            shadowsMakeRoom(memory);
            v = cscdStrMapPut(&shadowMap, k, NULL);
            v->p = cscdRcNew(sizeof(ShadowEntry), shadowEntryDtor);
            ((ShadowEntry*)v->p)->bytes = memory;
            shadowBytes += memory;
        }
        entry = v ? (ShadowEntry*)cscdRetain(v->p) : (ShadowEntry*)cscdRcNew(sizeof(ShadowEntry), shadowEntryDtor);
        entry->rank = ++shadowClock + (pixels >> 12); /* (a large one lasts as if used later) */
        cscdMutexUnlock(&shadowMutex);
        ShadowJob job = {&entry->values, &entry->alpha, &shape, bx0, by0, w, h, sigma, inset};
        cscdCallOnceArg(&entry->once, blurShadow, &job);
    }
    const CscdFloatVec* buf = &entry->values;
    const CscdBytes* alpha = &entry->alpha;
    /* where the box covers whole pixels: an outer shadow is not drawn there, an inset one fully */
    const float il = dbox.rect.x + cscdMaxf(dbox.rx[0], dbox.rx[3]) + 1, ir = cscdRectRight(dbox.rect) - cscdMaxf(dbox.rx[1], dbox.rx[2]) - 1;
    const float it = dbox.rect.y + cscdMaxf(dbox.ry[0], dbox.ry[1]) + 1, ib = cscdRectBottom(dbox.rect) - cscdMaxf(dbox.ry[2], dbox.ry[3]) - 1;
    uint32_t col = premul(color, st(c)->alpha);
    uint32_t tint[256]; /* the color at each alpha */
    for (int k = 0; k < 256; ++k) tint[k] = mulPixel(col, (uint32_t)k);
    CscdImage* t = cscdCanvasTarget(c);
    const CscdMask* mask = st(c)->mask;
    /* the pixels the box touches at all (elsewhere its coverage is 0), and its whole pixels: x from
     * wx0 to wx1 on the rows from it to ib */
    const int bl = cscdIntOf(floorf(dbox.rect.x)), br = cscdIntOf(ceilf(cscdRectRight(dbox.rect)));
    const int bt = cscdIntOf(floorf(dbox.rect.y)), bb = cscdIntOf(ceilf(cscdRectBottom(dbox.rect)));
    const int wx0 = cscdMaxi(x0, cscdIntOf(ceilf(il))), wx1 = cscdMini(x1, cscdIntOf(floorf(ir)));
    for (int y = y0; y < y1; ++y) {
        uint32_t* row = cscdImageRow(t, y);
        const float* src = buf->items + (size_t)(y - by0) * w - bx0;
        const uint8_t* srcA = alpha->items + (size_t)(y - by0) * w - bx0;
        const bool boxRow = y >= bt && y < bb;
        const bool wholeRow = y >= it && y + 1 <= ib && wx0 < wx1;
        for (int x = inset ? cscdMaxi(x0, bl) : x0, end = inset ? cscdMini(x1, br) : x1; x < end; ++x) {
            if (wholeRow && x == wx0) {
                if (!inset) { x = wx1 - 1; continue; } /* (an outer shadow is not drawn under the box) */
                for (; x < wx1; ++x) {
                    uint32_t k = srcA[x];
                    if (mask) k = mul8(k, cscdMaskAt(mask, x, y));
                    if (k) row[x] = over(row[x], tint[k]);
                }
                --x;
                continue;
            }
            uint32_t k = srcA[x];
            if (!k) continue; /* (the box's coverage only makes it less) */
            if (boxRow && x >= bl && x < br) {
                const float boxCov = rrectCoverage(&dbox, (float)x, (float)y);
                /* outer shadows are not drawn under the box, inset ones only inside */
                k = clampByte(src[x] * (inset ? boxCov : 1 - boxCov) * 255);
            } else if (inset) continue;
            if (mask) k = mul8(k, cscdMaskAt(mask, x, y));
            if (!k) continue;
            row[x] = over(row[x], tint[k]);
        }
    }
    cscdRelease(entry);
    markDirty(c, x0, y0, x1, y1);
}

void cscdCanvasDrawBoxShadow(CscdCanvas* c, const CscdRRect* box, float dx, float dy, float blur, float spread, CscdColor color, bool inset) {
    Op* op = record(c, OP_BOX_SHADOW);
    if (op) {
        op->rr = *box;
        op->f[0] = dx; op->f[1] = dy; op->f[2] = blur; op->f[3] = spread;
        op->color = color;
        op->flag = inset;
    }
    drawBoxShadowImpl(c, box, dx, dy, blur, spread, color, inset);
    endRecord(c, op);
}

static void drawBlurredPathImpl(CscdCanvas* c, const CscdPath* p, float blur, CscdColor color) {
    if (skip(c)) return;
    CscdPolyVec contours = {0};
    cscdFlattenInto(p, st(c)->m, 0.25f, &contours);
    if (!contours.len) { cscdPolyVecFree(&contours); return; }
    float scale = sqrtf(fabsf(st(c)->m.a * st(c)->m.d));
    float sigma = blur * scale / 2;
    if (sigma < 0.3f) {
        CscdShader s = cscdShaderSolid(color);
        rasterize(c, &contours, CSCD_FILL_NONZERO, &s);
        cscdPolyVecFree(&contours);
        return;
    }
    float l = 1e30f, t = 1e30f, r = -1e30f, b = -1e30f;
    for (size_t i = 0; i < contours.len; ++i)
        for (size_t k = 0; k < contours.items[i].len; ++k) {
            CscdPoint q = contours.items[i].items[k];
            l = cscdMinf(l, q.x); t = cscdMinf(t, q.y); r = cscdMaxf(r, q.x); b = cscdMaxf(b, q.y);
        }
    int pad = cscdIntOf(ceilf(sigma * 3)) + 1;
    int bx0 = cscdIntOf(floorf(l)) - pad, by0 = cscdIntOf(floorf(t)) - pad, bx1 = cscdIntOf(ceilf(r)) + pad, by1 = cscdIntOf(ceilf(b)) + pad;
    int w = bx1 - bx0, h = by1 - by0;
    if (w <= 0 || h <= 0 || (double)w * h > 20000000 || cscdRectEmpty(cscdRectIntersect(cscdRect((float)bx0, (float)by0, (float)w, (float)h), st(c)->clip))) {
        cscdPolyVecFree(&contours); /* (another band's) */
        return;
    }
    CscdAccumulator acc = {0};
    cscdAccReset(&acc, bx0, by0, w, h);
    cscdAccAddPolys(&acc, &contours);
    cscdPolyVecFree(&contours);
    CscdFloatVec buf = {0};
    cscdVecResize(&buf, (size_t)w * h);
    uint8_t* row = (uint8_t*)cscdMalloc((size_t)w);
    for (int y = 0; y < h; ++y) {
        cscdAccRow(&acc, y, CSCD_FILL_NONZERO, row);
        for (int x = 0; x < w; ++x) buf.items[(size_t)y * w + x] = row[x] / 255.f;
    }
    cscdFree(row);
    cscdAccFree(&acc);
    cscdGaussianBlur(&buf, w, h, sigma);
    CscdRect clip = st(c)->clip;
    int x0 = cscdMaxi(bx0, (int)clip.x), y0 = cscdMaxi(by0, (int)clip.y), x1 = cscdMini(bx1, (int)cscdRectRight(clip)), y1 = cscdMini(by1, (int)cscdRectBottom(clip));
    uint32_t col = premul(color, st(c)->alpha);
    CscdImage* tg = cscdCanvasTarget(c);
    const CscdMask* mask = st(c)->mask;
    for (int y = y0; y < y1; ++y) {
        uint32_t* rr = cscdImageRow(tg, y);
        for (int x = x0; x < x1; ++x) {
            uint32_t k = clampByte(buf.items[(size_t)(y - by0) * w + (x - bx0)] * 255);
            if (mask) k = mul8(k, cscdMaskAt(mask, x, y));
            if (k) rr[x] = over(rr[x], mulPixel(col, k));
        }
    }
    cscdVecFree(&buf);
    if (x1 > x0 && y1 > y0) markDirty(c, x0, y0, x1, y1);
}

void cscdCanvasDrawBlurredPath(CscdCanvas* c, const CscdPath* p, float blur, CscdColor color) {
    Op* op = record(c, OP_BLURRED_PATH);
    if (op) { op->path = copyPath(p); op->f[0] = blur; op->color = color; }
    drawBlurredPathImpl(c, p, blur, color);
    endRecord(c, op);
}

/* ---- text ---------------------------------------------------------------------------------------- */

float cscdCanvasMeasureText(const CscdCanvas* c, CscdSv utf8, const CscdFont* font) {
    (void)c;
    return cscdFontMeasure(utf8, font, 0, 0);
}

void cscdCanvasDrawText(CscdCanvas* c, CscdSv utf8, float x, float y, const CscdFont* font, CscdColor color) {
    CscdShader s = cscdShaderSolid(color);
    cscdCanvasDrawTextShader(c, utf8, x, y, font, &s);
}

static CSCD_THREAD_LOCAL CscdShapedGlyphVec textShaped;

void cscdCanvasDrawTextShader(CscdCanvas* c, CscdSv utf8, float x, float y, const CscdFont* font, const CscdShader* s) {
    if (c->dry) return;
    cscdFontShape(utf8, font, 0, 0, &textShaped);
    size_t n = 0;
    for (size_t i = 0; i < textShaped.len; ++i) n += textShaped.items[i].glyph != 0;
    CscdGlyph* gl = (CscdGlyph*)cscdMalloc((n ? n : 1) * sizeof(CscdGlyph));
    CscdFont* fonts = (CscdFont*)cscdMalloc((n ? n : 1) * sizeof(CscdFont));
    size_t k = 0;
    for (size_t i = 0; i < textShaped.len; ++i) {
        const CscdShapedGlyph* g = &textShaped.items[i];
        if (!g->glyph) continue;
        fonts[k] = *font;
        fonts[k].face = g->face;
        if (g->scale) fonts[k].size *= g->scale;
        gl[k].id = g->glyph;
        gl[k].x = x + g->x;
        gl[k].y = y + g->y;
        gl[k].font = &fonts[k];
        ++k;
    }
    cscdCanvasDrawGlyphs(c, gl, n, s);
    cscdFree(gl);
    cscdFree(fonts);
}

static void freeRun(GlyphRun* r) {
    if (!r) return;
    cscdFree(r->glyphs);
    cscdFree(r->fonts);
    cscdFree(r);
}

void cscdCanvasDrawGlyphs(CscdCanvas* c, const CscdGlyph* glyphs, size_t n, const CscdShader* s) {
    if (c->list && !c->inCall && n) {
        /* the glyphs point at their fonts: copies of both, the glyphs pointing at the copies */
        GlyphRun* run = CSCD_NEW(GlyphRun);
        run->glyphs = (CscdGlyph*)cscdMalloc(n * sizeof(CscdGlyph));
        run->fonts = (CscdFont*)cscdMalloc(n * sizeof(CscdFont));
        for (size_t i = 0; i < n; ++i) {
            const CscdFont f = glyphs[i].font ? *glyphs[i].font : cscdFontDefault();
            size_t which = run->nfonts;
            for (size_t q = 0; q < run->nfonts; ++q)
                if (cscdFontEq(&run->fonts[q], &f)) { which = q; break; }
            if (which == run->nfonts) run->fonts[run->nfonts++] = f;
            run->glyphs[i] = glyphs[i];
            run->glyphs[i].font = (const CscdFont*)(uintptr_t)which; /* (an index until all are known) */
        }
        for (size_t i = 0; i < n; ++i) run->glyphs[i].font = &run->fonts[(size_t)(uintptr_t)run->glyphs[i].font];
        run->n = n;
        Op* op = cscdVecPushZero(&c->list->ops);
        op->kind = OP_GLYPHS;
        op->run = run;
        op->shader = copyShader(s);
    }
    if (skip(c)) return;
    if (!n) return;
    const CscdMatrix m = st(c)->m;
    bool simple = m.b == 0 && m.c == 0 && m.a > 0 && fabsf(m.a - m.d) < 1e-4f;
    if (!simple) {
        /* transformed text: fill outlines */
        CscdPath all = {0}, p = {0};
        for (size_t i = 0; i < n; ++i) {
            const CscdGlyph* g = &glyphs[i];
            cscdPathClear(&p);
            if (!g->font->face) continue;
            float sc = g->font->size / g->font->face->unitsPerEm;
            CscdMatrix gm = cscdMatrix(sc, 0, g->font->fakeItalic ? -0.2f * sc : 0, -sc, g->x, g->y);
            if (cscdFontFaceHasColor(g->font->face)) { /* its layers, or its picture, through the transform */
                CscdColorLayer layers[64];
                const size_t nl = cscdMinz(cscdFontFaceColorLayers(g->font->face, g->id, layers, CSCD_COUNT(layers)), CSCD_COUNT(layers));
                const float device = g->font->size * cscdMaxf(fabsf(m.a), fabsf(m.d));
                CscdRect where;
                const CscdImage* img = nl ? NULL : cscdFontFaceGlyphImage(g->font->face, g->id, device, &where);
                if (nl || img) {
                    cscdCanvasFillPathShader(c, &all, s, CSCD_FILL_NONZERO); /* (the glyphs before it, under it) */
                    cscdPathClear(&all);
                    for (size_t l = 0; l < nl; ++l) {
                        CscdPath lp = {0}, placed = {0};
                        if (cscdFontFaceOutline(g->font->face, layers[l].glyph, &lp)) {
                            cscdPathAddPath(&placed, &lp, gm);
                            const uint32_t a = layers[l].argb;
                            CscdShader ls = layers[l].foreground ? *s : cscdShaderSolid((CscdColor){(uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(a >> 24)});
                            cscdCanvasFillPathShader(c, &placed, &ls, CSCD_FILL_NONZERO);
                        }
                        cscdPathFree(&lp);
                        cscdPathFree(&placed);
                    }
                    if (img) {
                        const float k2 = g->font->size / device;
                        cscdCanvasDrawImage(c, img, cscdRect(g->x + where.x * k2, g->y + where.y * k2, where.w * k2, where.h * k2), true);
                    }
                    continue;
                }
            }
            if (!cscdFontFaceOutline(g->font->face, g->id, &p)) continue;
            cscdPathAddPath(&all, &p, gm);
        }
        cscdCanvasFillPathShader(c, &all, s, CSCD_FILL_NONZERO);
        cscdPathFree(&all);
        cscdPathFree(&p);
        return;
    }
    ShaderCtx sc;
    shaderInit(&sc, s, m, st(c)->alpha);
    CscdImage* t = cscdCanvasTarget(c);
    const CscdMask* mask = st(c)->mask;
    CscdRect clip = st(c)->clip;
    int cx0 = (int)clip.x, cy0 = (int)clip.y, cx1 = (int)cscdRectRight(clip), cy1 = (int)cscdRectBottom(clip);
    int dx0 = 1 << 30, dy0 = 1 << 30, dx1 = -(1 << 30), dy1 = -(1 << 30);
    bool solid = s->kind == CSCD_SHADER_SOLID;
    for (size_t i = 0; i < n; ++i) {
        const CscdGlyph* g = &glyphs[i];
        if (!g->font->face) continue;
        CscdPoint p = cscdMatMap(m, cscdPoint(g->x, g->y));
        float size = g->font->size * m.a;
        float fx = floorf(p.x);
        int sub = cscdIntOf(floorf((p.x - fx) * 4 + 0.5f));
        if (sub == 4) { sub = 0; fx += 1; }
        int ix = (int)fx, iy = cscdIntOf(floorf(p.y + 0.5f));
        const bool colorFace = cscdFontFaceHasColor(g->font->face);
        CscdColor fg = s->color;
        fg.a = 255;
        GlyphKey key = {g->font->face, g->id, (uint16_t)cscdClampi((int)lroundf(size * 16), 1, 65535), (uint8_t)sub,
                        (uint8_t)((g->font->fakeBold ? 1 : 0) | (g->font->fakeItalic ? 2 : 0)),
                        colorFace ? (solid ? cscdColorPremul(fg) : 0xFF000000u) : 0};
        const GlyphBitmap* b = glyphGet(&key);
        if (!b->w) continue;
        int gx0 = ix + b->left, gy0 = iy + b->top;
        int x0 = cscdMaxi(gx0, cx0), y0 = cscdMaxi(gy0, cy0), x1 = cscdMini(gx0 + b->w, cx1), y1 = cscdMini(gy0 + b->h, cy1);
        if (x1 <= x0 || y1 <= y0) continue;
        if (b->rgba.len) { /* a color glyph: its own pixels, at the state's opacity (and the text color's alpha) */
            const uint32_t k8 = (uint32_t)cscdClampi((int)lroundf(st(c)->alpha * (solid ? s->color.a : 255)), 0, 255);
            for (int y = y0; y < y1; ++y) {
                uint32_t* row = cscdImageRow(t, y);
                const uint32_t* src = &b->rgba.items[(size_t)(y - gy0) * b->w];
                for (int x = x0; x < x1; ++x) {
                    uint32_t px = src[x - gx0];
                    if (!px) continue;
                    uint32_t k = k8;
                    if (mask) k = mul8(k, cscdMaskAt(mask, x, y));
                    if (k < 255) px = mulPixel(px, k);
                    row[x] = over(row[x], px);
                }
            }
            dx0 = cscdMini(dx0, x0); dy0 = cscdMini(dy0, y0); dx1 = cscdMaxi(dx1, x1); dy1 = cscdMaxi(dy1, y1);
            continue;
        }
        for (int y = y0; y < y1; ++y) {
            uint32_t* row = cscdImageRow(t, y);
            const uint8_t* src = &b->a.items[(size_t)(y - gy0) * b->w];
            for (int x = x0; x < x1; ++x) {
                uint32_t cv = src[x - gx0];
                if (!cv) continue;
                if (mask) cv = mul8(cv, cscdMaskAt(mask, x, y));
                if (!cv) continue;
                uint32_t col = solid ? sc.solid : shadeAt(&sc, x + 0.5f, y + 0.5f);
                row[x] = over(row[x], cv == 255 ? col : mulPixel(col, cv));
            }
        }
        dx0 = cscdMini(dx0, x0); dy0 = cscdMini(dy0, y0); dx1 = cscdMaxi(dx1, x1); dy1 = cscdMaxi(dy1, y1);
    }
    if (dx1 > dx0) markDirty(c, dx0, dy0, dx1, dy1);
}

/* ---- display lists --------------------------------------------------------------------------------- */

CscdDisplayList* cscdDisplayListNew(void) {
    CscdDisplayList* l = CSCD_NEW(CscdDisplayList);
    l->bandable = true;
    return l;
}

static void opFini(Op* op) {
    cscdRelease(op->image);
    cscdRelease(op->reduced);
    if (op->path) { cscdPathFree(op->path); cscdFree(op->path); }
    if (op->shader) { cscdShaderFini(op->shader); cscdFree(op->shader); }
    if (op->stroke) { cscdStrokeStyleFini(op->stroke); cscdFree(op->stroke); }
    cscdVecFree(&op->cov);
    freeRun(op->run);
}

void cscdDisplayListClear(CscdDisplayList* l) {
    for (size_t i = 0; i < l->ops.len; ++i) opFini(&l->ops.items[i]);
    l->ops.len = 0;
    l->bandable = true;
    l->layerDepth = 0;
}

void cscdDisplayListFree(CscdDisplayList* l) {
    if (!l) return;
    cscdDisplayListClear(l);
    cscdVecFree(&l->ops);
    cscdFree(l);
}

bool cscdDisplayListEmpty(const CscdDisplayList* l) { return l->ops.len == 0; }
size_t cscdDisplayListSize(const CscdDisplayList* l) { return l->ops.len; }
bool cscdDisplayListBandable(const CscdDisplayList* l) { return l->bandable; }

typedef struct ReduceJob { Op* op; int step; } ReduceJob;
static void reduceOpImage(void* arg) {
    ReduceJob* j = (ReduceJob*)arg;
    j->op->reduced = reduceImage(j->op->image, j->step);
}

/* An image replayed: when it is drawn much smaller, the first band to need it reduces it and the
 * others use that (the same matrix in every band: the same step). */
static void replayImage(Op* op, CscdCanvas* c) {
    if (skip(c) || !op->image) {
        cscdCanvasDrawSharedImage(c, op->image, op->r, op->r2, op->flag); /* (recorded again, or nothing) */
        return;
    }
    const int step = reductionStep(c, op->image, op->r, op->r2, op->flag);
    if (step) {
        ReduceJob job = {op, step};
        cscdCallOnceArg(&op->reducedOnce, reduceOpImage, &job);
    }
    drawImageImpl(c, op->image, op->r, op->r2, op->flag, step ? op->reduced : NULL);
}

static void replayOp(const Op* op, CscdCanvas* c) {
    switch (op->kind) {
    case OP_SAVE: cscdCanvasSave(c); break;
    case OP_RESTORE: cscdCanvasRestore(c); break;
    case OP_TRANSLATE: cscdCanvasTranslate(c, op->f[0], op->f[1]); break;
    case OP_SCALE: cscdCanvasScale(c, op->f[0], op->f[1]); break;
    case OP_ROTATE: cscdCanvasRotate(c, op->f[0]); break;
    case OP_CONCAT: cscdCanvasConcat(c, op->m); break;
    case OP_SET_MATRIX: cscdCanvasSetMatrix(c, op->m); break;
    case OP_SET_ALPHA: cscdCanvasSetGlobalAlpha(c, op->f[0]); break;
    case OP_CLIP_RECT: cscdCanvasClipRect(c, op->r); break;
    case OP_CLIP_RRECT: cscdCanvasClipRRect(c, &op->rr); break;
    case OP_CLIP_PATH: cscdCanvasClipPath(c, op->path, op->rule); break;
    case OP_PUSH_LAYER: cscdCanvasPushLayer(c, op->f[0], op->blend); break;
    case OP_POP_LAYER: cscdCanvasPopLayer(c); break;
    case OP_SET_LAYER_FILTER: cscdCanvasSetLayerFilter(c, &op->filter); break;
    case OP_FILTER_BACKDROP: cscdCanvasFilterBackdrop(c, &op->rr, &op->filter); break;
    case OP_MASK_LAYER: cscdCanvasMaskLayer(c, op->image); break;
    case OP_CLEAR: cscdCanvasClear(c, op->color); break;
    case OP_BLEND_SPAN: cscdCanvasBlendSpan(c, op->ix, op->iy, (int)op->cov.len, op->cov.items, op->shader, op->m); break;
    case OP_FILL_PATH: cscdCanvasFillPathShader(c, op->path, op->shader, op->rule); break;
    case OP_STROKE_PATH: cscdCanvasStrokePathShader(c, op->path, op->shader, op->stroke); break;
    case OP_FILL_RRECT: cscdCanvasFillRRectShader(c, &op->rr, op->shader); break;
    case OP_FILL_RING: cscdCanvasFillRing(c, &op->rr, &op->rr2, op->shader); break;
    case OP_IMAGE_PROJECTIVE: cscdCanvasDrawImageProjective(c, op->image, op->f); break;
    case OP_IMAGE: replayImage((Op*)op, c); break; /* (its reduced picture is a cache) */
    case OP_BOX_SHADOW: cscdCanvasDrawBoxShadow(c, &op->rr, op->f[0], op->f[1], op->f[2], op->f[3], op->color, op->flag); break;
    case OP_BLURRED_PATH: cscdCanvasDrawBlurredPath(c, op->path, op->f[0], op->color); break;
    case OP_GLYPHS: cscdCanvasDrawGlyphs(c, op->run->glyphs, op->run->n, op->shader); break;
    }
}

void cscdDisplayListReplay(const CscdDisplayList* l, CscdCanvas* c) {
    for (size_t i = 0; i < l->ops.len; ++i) replayOp(&l->ops.items[i], c);
}

static volatile int paintThreadSetting = -1;

static int machineThreads(void) { return cscdClampi((int)cscdHardwareThreads(), 1, 8); }

void cscdSetPaintThreads(int n) { cscdAtomicStore(&paintThreadSetting, cscdMaxi(0, n)); }

CscdPaintCacheLimits cscdPaintCacheLimits(void) {
    CscdPaintCacheLimits l;
    cscdMutexLock(&shadowMutex);
    l.shadowBytes = shadowLimitBytes;
    l.shadowCount = shadowLimitCount;
    cscdMutexUnlock(&shadowMutex);
    l.glyphCount = (size_t)cscdAtomicLoad(&glyphLimit);
    return l;
}

void cscdPaintCacheSetLimits(CscdPaintCacheLimits limits) {
    cscdMutexLock(&shadowMutex);
    shadowLimitBytes = limits.shadowBytes;
    shadowLimitCount = limits.shadowCount;
    if (!shadowLimitBytes || !shadowLimitCount) {
        releaseAll(&shadowMap);
        shadowBytes = 0;
    } else {
        shadowsMakeRoom(0);
    }
    cscdMutexUnlock(&shadowMutex);
    const int glyphs = limits.glyphCount < 256 ? 256 : limits.glyphCount > (1u << 30) ? 1 << 30 : (int)limits.glyphCount;
    cscdAtomicStore(&glyphLimit, glyphs);
    cscdRwLockWrite(&glyphLock);
    glyphsMakeRoom(glyphs);
    cscdRwUnlockWrite(&glyphLock);
}

CscdPaintCacheStats cscdPaintCacheStats(void) {
    CscdPaintCacheStats s;
    cscdMutexLock(&shadowMutex);
    s.shadowCount = shadowMap.len;
    s.shadowBytes = shadowBytes;
    s.shadowHits = shadowHits;
    s.shadowMisses = shadowMisses;
    s.shadowEvictions = shadowEvictions;
    cscdMutexUnlock(&shadowMutex);
    cscdRwLockRead(&glyphLock);
    s.glyphCount = glyphMap.len;
    s.glyphEvictions = glyphEvictions;
    cscdRwUnlockRead(&glyphLock);
    return s;
}

void cscdPaintCacheClear(void) {
    cscdMutexLock(&shadowMutex);
    releaseAll(&shadowMap);
    shadowBytes = 0;
    cscdMutexUnlock(&shadowMutex);
    cscdRwLockWrite(&glyphLock);
    releaseAll(&glyphMap);
    cscdRwUnlockWrite(&glyphLock);
}

int cscdPaintThreads(void) {
    int n = cscdAtomicLoad(&paintThreadSetting);
    if (n < 0) {
        const char* env = getenv("CASCADE_PAINT_THREADS");
        n = env ? cscdMaxi(0, atoi(env)) : 0;
        cscdAtomicStore(&paintThreadSetting, n);
    }
    return n == 0 ? machineThreads() : cscdMini(n, 64);
}

typedef struct BandCtx {
    const CscdDisplayList* list;
    CscdImage* target;
    int bandH, h;
    CscdPtrVec* shared;
} BandCtx;

/* (a cscdParallelFor job: band i) */
static void paintBand(void* arg, int i) {
    BandCtx* b = (BandCtx*)arg;
    CscdCanvas* c = cscdCanvasNewRegion(b->target, cscdIRect(0, i * b->bandH, b->target->width, cscdMini(b->bandH, b->h - i * b->bandH)));
    c->bandLayers = b->shared;
    cscdDisplayListReplay(b->list, c);
    cscdCanvasFree(c);
}

void cscdDisplayListReplayInto(const CscdDisplayList* l, CscdImage* target, int threads) {
    if (threads <= 0) threads = cscdPaintThreads();
    /* bands of at least 32 rows, a few for each thread (they do not all take as long) */
    const int h = target->height;
    const int bandH = cscdMaxi(32, (h + threads * 4 - 1) / cscdMaxi(1, threads * 4));
    const int bands = (h + bandH - 1) / bandH;
    if (threads <= 1 || bands <= 1 || !l->bandable) {
        CscdCanvas* c = cscdCanvasNew(target);
        cscdDisplayListReplay(l, c);
        cscdCanvasFree(c);
        return;
    }
    CscdPtrVec shared = {0};
    for (int i = 0; i < l->layerDepth; ++i) cscdVecPush(&shared, takeLayerImage(target->width, target->height));
    BandCtx ctx = {l, target, bandH, h, &shared};
    cscdParallelFor(bands, threads, paintBand, &ctx);
    for (size_t i = 0; i < shared.len; ++i) giveLayerImage((CscdImage*)shared.items[i], cscdIRect(0, 0, 0, 0)); /* (each band cleared what it drew) */
    cscdVecFree(&shared);
}
