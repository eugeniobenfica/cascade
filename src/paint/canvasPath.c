/* Paths and their flattening, SVG path data, rounded rectangles, shaders, the stroker, blurs and
 * the exact-area anti-aliased rasterizer. */
#include "paint/canvasInternal.h"

#include <stdlib.h>

/* ---- polygons -------------------------------------------------------------------------------- */

void cscdPolyVecClear(CscdPolyVec* v) {
    for (size_t i = 0; i < v->len; ++i) cscdVecFree(&v->items[i]);
    v->len = 0;
}

void cscdPolyVecFree(CscdPolyVec* v) {
    cscdPolyVecClear(v);
    cscdVecFree(v);
}

void cscdPolyVecAdd(CscdPolyVec* v, const CscdPoint* pts, size_t n) {
    CscdPointVec* p = cscdVecPushZero(v);
    cscdVecAppend(p, pts, n);
}

/* ---- Path ------------------------------------------------------------------------------------ */

static void addVerb(CscdPath* p, CscdPathVerb v) { cscdVecPush(&p->verbs, (uint8_t)v); }
static void addPoint(CscdPath* p, float x, float y) { cscdVecPush(&p->points, cscdPoint(x, y)); }

void cscdPathFree(CscdPath* p) {
    cscdVecFree(&p->verbs);
    cscdVecFree(&p->points);
}

void cscdPathClear(CscdPath* p) {
    cscdVecClear(&p->verbs);
    cscdVecClear(&p->points);
}

void cscdPathCopy(CscdPath* dst, const CscdPath* src) {
    cscdVecCopy(&dst->verbs, &src->verbs);
    cscdVecCopy(&dst->points, &src->points);
    dst->start_ = src->start_;
}

void cscdPathMoveTo(CscdPath* p, float x, float y) {
    addVerb(p, CSCD_VERB_MOVE);
    addPoint(p, x, y);
    p->start_ = cscdPoint(x, y);
}

void cscdPathLineTo(CscdPath* p, float x, float y) {
    if (!p->verbs.len) cscdPathMoveTo(p, x, y);
    addVerb(p, CSCD_VERB_LINE);
    addPoint(p, x, y);
}

void cscdPathQuadTo(CscdPath* p, float cx, float cy, float x, float y) {
    if (!p->verbs.len) cscdPathMoveTo(p, cx, cy);
    addVerb(p, CSCD_VERB_QUAD);
    addPoint(p, cx, cy);
    addPoint(p, x, y);
}

void cscdPathCubicTo(CscdPath* p, float c1x, float c1y, float c2x, float c2y, float x, float y) {
    if (!p->verbs.len) cscdPathMoveTo(p, c1x, c1y);
    addVerb(p, CSCD_VERB_CUBIC);
    addPoint(p, c1x, c1y);
    addPoint(p, c2x, c2y);
    addPoint(p, x, y);
}

void cscdPathClose(CscdPath* p) {
    if (p->verbs.len && cscdVecLast(&p->verbs) != CSCD_VERB_CLOSE) {
        addVerb(p, CSCD_VERB_CLOSE);
        addPoint(p, p->start_.x, p->start_.y);
    }
}

void cscdPathAddRect(CscdPath* p, CscdRect r) {
    cscdPathMoveTo(p, r.x, r.y);
    cscdPathLineTo(p, cscdRectRight(r), r.y);
    cscdPathLineTo(p, cscdRectRight(r), cscdRectBottom(r));
    cscdPathLineTo(p, r.x, cscdRectBottom(r));
    cscdPathClose(p);
}

void cscdPathAddRoundRectR(CscdPath* p, CscdRect r, float radius) {
    float rx[4] = {radius, radius, radius, radius};
    cscdPathAddRoundRect(p, r, rx, rx);
}

void cscdPathAddRoundRect(CscdPath* p, CscdRect r, const float rxIn[4], const float ryIn[4]) {
    CscdRRect rr = cscdRRect(r);
    for (int i = 0; i < 4; ++i) { rr.rx[i] = rxIn[i]; rr.ry[i] = ryIn[i]; }
    cscdRRectNormalize(&rr);
    const float k = 0.5522847498f;
    const float* rx = rr.rx;
    const float* ry = rr.ry;
    float l = r.x, t = r.y, R = cscdRectRight(r), B = cscdRectBottom(r);
    cscdPathMoveTo(p, l + rx[0], t);
    cscdPathLineTo(p, R - rx[1], t);
    if (rx[1] > 0 && ry[1] > 0) cscdPathCubicTo(p, R - rx[1] + rx[1] * k, t, R, t + ry[1] - ry[1] * k, R, t + ry[1]);
    cscdPathLineTo(p, R, B - ry[2]);
    if (rx[2] > 0 && ry[2] > 0) cscdPathCubicTo(p, R, B - ry[2] + ry[2] * k, R - rx[2] + rx[2] * k, B, R - rx[2], B);
    cscdPathLineTo(p, l + rx[3], B);
    if (rx[3] > 0 && ry[3] > 0) cscdPathCubicTo(p, l + rx[3] - rx[3] * k, B, l, B - ry[3] + ry[3] * k, l, B - ry[3]);
    cscdPathLineTo(p, l, t + ry[0]);
    if (rx[0] > 0 && ry[0] > 0) cscdPathCubicTo(p, l, t + ry[0] - ry[0] * k, l + rx[0] - rx[0] * k, t, l + rx[0], t);
    cscdPathClose(p);
}

void cscdPathAddEllipse(CscdPath* p, float cx, float cy, float rx, float ry) {
    const float k = 0.5522847498f;
    cscdPathMoveTo(p, cx + rx, cy);
    cscdPathCubicTo(p, cx + rx, cy + ry * k, cx + rx * k, cy + ry, cx, cy + ry);
    cscdPathCubicTo(p, cx - rx * k, cy + ry, cx - rx, cy + ry * k, cx - rx, cy);
    cscdPathCubicTo(p, cx - rx, cy - ry * k, cx - rx * k, cy - ry, cx, cy - ry);
    cscdPathCubicTo(p, cx + rx * k, cy - ry, cx + rx, cy - ry * k, cx + rx, cy);
    cscdPathClose(p);
}

void cscdPathArc(CscdPath* p, float cx, float cy, float r, float a0, float a1, bool ccw) {
    const float kPi = CSCD_PI;
    float sweep = a1 - a0;
    if (!ccw) { if (sweep < 0) sweep = fmodf(sweep, 2 * kPi) + 2 * kPi; if (a1 - a0 >= 2 * kPi) sweep = 2 * kPi; }
    else { if (sweep > 0) sweep = fmodf(sweep, 2 * kPi) - 2 * kPi; if (a0 - a1 >= 2 * kPi) sweep = -2 * kPi; }
    CscdPoint s = {cx + r * cscdCosf(a0), cy + r * cscdSinf(a0)};
    if (!p->verbs.len || cscdVecLast(&p->verbs) == CSCD_VERB_CLOSE) cscdPathMoveTo(p, s.x, s.y);
    else cscdPathLineTo(p, s.x, s.y);
    int n = cscdMaxi(1, cscdIntOf(ceilf(fabsf(sweep) / (kPi / 2) - 1e-4f)));
    float step = sweep / n;
    for (int i = 0; i < n; ++i) {
        float t0 = a0 + step * i, t1 = t0 + step;
        float k = 4.f / 3.f * cscdTanf(step / 4);
        CscdPoint p0 = {cx + r * cscdCosf(t0), cy + r * cscdSinf(t0)};
        CscdPoint p3 = {cx + r * cscdCosf(t1), cy + r * cscdSinf(t1)};
        CscdPoint p1 = {p0.x - k * r * cscdSinf(t0), p0.y + k * r * cscdCosf(t0)};
        CscdPoint p2 = {p3.x + k * r * cscdSinf(t1), p3.y - k * r * cscdCosf(t1)};
        cscdPathCubicTo(p, p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
    }
}

static float vecAngle(float ux, float uy, float vx, float vy) { return cscdAtan2f(ux * vy - uy * vx, ux * vx + uy * vy); }

void cscdPathArcTo(CscdPath* p, float rx, float ry, float rotDeg, bool large, bool sweep, float x, float y) {
    const float kPi = CSCD_PI;
    CscdPoint p0 = cscdPathCurrent(p);
    if (rx == 0 || ry == 0) {
        cscdPathLineTo(p, x, y);
        return;
    }
    rx = fabsf(rx);
    ry = fabsf(ry);
    float phi = rotDeg * kPi / 180.f, cs = cscdCosf(phi), sn = cscdSinf(phi);
    float dx2 = (p0.x - x) / 2, dy2 = (p0.y - y) / 2;
    float x1 = cs * dx2 + sn * dy2, y1 = -sn * dx2 + cs * dy2;
    float lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lam > 1) { float s = sqrtf(lam); rx *= s; ry *= s; }
    float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    float co = den == 0 ? 0 : sqrtf(cscdMaxf(0.f, num / den));
    if (large == sweep) co = -co;
    float cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
    float cx = cs * cxp - sn * cyp + (p0.x + x) / 2, cy = sn * cxp + cs * cyp + (p0.y + y) / 2;
    float t1 = vecAngle(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
    float dt = vecAngle((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
    if (!sweep && dt > 0) dt -= 2 * kPi;
    else if (sweep && dt < 0) dt += 2 * kPi;
    int n = cscdMaxi(1, cscdIntOf(ceilf(fabsf(dt) / (kPi / 2) - 1e-4f)));
    float step = dt / n;
    float k = 4.f / 3.f * cscdTanf(step / 4);
    for (int i = 0; i < n; ++i) {
        float a = t1 + step * i, b = a + step;
        float ca = cscdCosf(a), sa = cscdSinf(a), cb = cscdCosf(b), sb = cscdSinf(b);
#define MAP(ex, ey) cscdPoint(cx + cs * (ex) * rx - sn * (ey) * ry, cy + sn * (ex) * rx + cs * (ey) * ry)
        CscdPoint c1 = MAP(ca - k * sa, sa + k * ca), c2 = MAP(cb + k * sb, sb - k * cb), e = MAP(cb, sb);
#undef MAP
        cscdPathCubicTo(p, c1.x, c1.y, c2.x, c2.y, e.x, e.y);
    }
    cscdVecLast(&p->points) = cscdPoint(x, y);
}

void cscdPathAddPath(CscdPath* p, const CscdPath* src, CscdMatrix m) {
    size_t pi = 0;
    const CscdPoint* pts = src->points.items;
    for (size_t vi = 0; vi < src->verbs.len; ++vi) {
        switch ((CscdPathVerb)src->verbs.items[vi]) {
        case CSCD_VERB_MOVE: { CscdPoint a = cscdMatMap(m, pts[pi++]); cscdPathMoveTo(p, a.x, a.y); break; }
        case CSCD_VERB_LINE: { CscdPoint a = cscdMatMap(m, pts[pi++]); cscdPathLineTo(p, a.x, a.y); break; }
        case CSCD_VERB_QUAD: { CscdPoint a = cscdMatMap(m, pts[pi]), b = cscdMatMap(m, pts[pi + 1]); pi += 2; cscdPathQuadTo(p, a.x, a.y, b.x, b.y); break; }
        case CSCD_VERB_CUBIC: {
            CscdPoint a = cscdMatMap(m, pts[pi]), b = cscdMatMap(m, pts[pi + 1]), c = cscdMatMap(m, pts[pi + 2]);
            pi += 3;
            cscdPathCubicTo(p, a.x, a.y, b.x, b.y, c.x, c.y);
            break;
        }
        case CSCD_VERB_CLOSE: pi++; cscdPathClose(p); break;
        }
    }
}

CscdRect cscdPathBounds(const CscdPath* p) {
    if (!p->points.len) return cscdRect(0, 0, 0, 0);
    float l = p->points.items[0].x, t = p->points.items[0].y, r = l, b = t;
    for (size_t i = 0; i < p->points.len; ++i) {
        CscdPoint q = p->points.items[i];
        l = cscdMinf(l, q.x); t = cscdMinf(t, q.y); r = cscdMaxf(r, q.x); b = cscdMaxf(b, q.y);
    }
    return cscdRect(l, t, r - l, b - t);
}

void cscdPathTransformed(const CscdPath* p, CscdMatrix m, CscdPath* out) {
    cscdPathClear(out);
    cscdPathAddPath(out, p, m);
}

static void flush(CscdPointVec* poly, bool closed, CscdContourFn fn, void* ctx) {
    if (poly->len > 1) fn(ctx, poly->items, poly->len, closed);
    cscdVecClear(poly);
}

void cscdPathFlatten(const CscdPath* p, CscdMatrix m, float tol, CscdContourFn fn, void* ctx) {
    CscdPointVec poly = {0};
    size_t pi = 0;
    const CscdPoint* pts = p->points.items;
    for (size_t vi = 0; vi < p->verbs.len; ++vi) {
        switch ((CscdPathVerb)p->verbs.items[vi]) {
        case CSCD_VERB_MOVE:
            flush(&poly, false, fn, ctx);
            cscdVecPush(&poly, cscdMatMap(m, pts[pi++]));
            break;
        case CSCD_VERB_LINE: cscdVecPush(&poly, cscdMatMap(m, pts[pi++])); break;
        case CSCD_VERB_QUAD: {
            CscdPoint p0 = poly.len ? cscdVecLast(&poly) : cscdMatMap(m, pts[pi]);
            CscdPoint p1 = cscdMatMap(m, pts[pi]), p2 = cscdMatMap(m, pts[pi + 1]);
            pi += 2;
            float ddx = p0.x - 2 * p1.x + p2.x, ddy = p0.y - 2 * p1.y + p2.y;
            float dd = sqrtf(ddx * ddx + ddy * ddy);
            int n = cscdClampi(cscdIntOf(ceilf(sqrtf(dd / (8 * tol)))), 1, 256);
            for (int i = 1; i <= n; ++i) {
                float t = (float)i / n, u = 1 - t;
                cscdVecPush(&poly, cscdPoint(u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x, u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y));
            }
            break;
        }
        case CSCD_VERB_CUBIC: {
            CscdPoint p0 = poly.len ? cscdVecLast(&poly) : cscdMatMap(m, pts[pi]);
            CscdPoint p1 = cscdMatMap(m, pts[pi]), p2 = cscdMatMap(m, pts[pi + 1]), p3 = cscdMatMap(m, pts[pi + 2]);
            pi += 3;
            float d1x = p0.x - 2 * p1.x + p2.x, d1y = p0.y - 2 * p1.y + p2.y;
            float d2x = p1.x - 2 * p2.x + p3.x, d2y = p1.y - 2 * p2.y + p3.y;
            float dd = sqrtf(cscdMaxf(d1x * d1x + d1y * d1y, d2x * d2x + d2y * d2y));
            int n = cscdClampi(cscdIntOf(ceilf(sqrtf(dd * 0.75f / tol))), 1, 512);
            for (int i = 1; i <= n; ++i) {
                float t = (float)i / n, u = 1 - t;
                float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
                cscdVecPush(&poly, cscdPoint(a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y));
            }
            break;
        }
        case CSCD_VERB_CLOSE:
            pi++;
            if (poly.len) {
                CscdPoint first = poly.items[0];
                flush(&poly, true, fn, ctx);
                cscdVecPush(&poly, first); /* a following lineTo continues from the start point */
            }
            break;
        }
    }
    flush(&poly, false, fn, ctx);
    cscdVecFree(&poly);
}

static void collectContour(void* ctx, const CscdPoint* pts, size_t n, bool closed) {
    (void)closed;
    cscdPolyVecAdd((CscdPolyVec*)ctx, pts, n);
}

void cscdFlattenInto(const CscdPath* p, CscdMatrix m, float tolerance, CscdPolyVec* out) { cscdPathFlatten(p, m, tolerance, collectContour, out); }

/* ---- SVG path data ---------------------------------------------------------------------------- */

typedef struct SvgReader { CscdSv d; size_t i; } SvgReader;

static void svgSkip(SvgReader* r) { while (r->i < r->d.len && (cscdIsSpace(r->d.ptr[r->i]) || r->d.ptr[r->i] == ',')) ++r->i; }

static bool svgNumber(SvgReader* r, float* out) {
    svgSkip(r);
    const char* d = r->d.ptr;
    const size_t n = r->d.len;
    if (r->i >= n) return false;
    size_t s = r->i;
    if (d[r->i] == '+' || d[r->i] == '-') ++r->i;
    bool dot = false, digits = false;
    while (r->i < n && (cscdIsDigit(d[r->i]) || (d[r->i] == '.' && !dot))) { if (d[r->i] == '.') dot = true; else digits = true; ++r->i; }
    if (r->i < n && (d[r->i] == 'e' || d[r->i] == 'E') && r->i + 1 < n && (cscdIsDigit(d[r->i + 1]) || d[r->i + 1] == '-' || d[r->i + 1] == '+')) {
        ++r->i;
        if (d[r->i] == '-' || d[r->i] == '+') ++r->i;
        while (r->i < n && cscdIsDigit(d[r->i])) ++r->i;
    }
    if (!digits) { r->i = s; return false; }
    char buf[64];
    size_t len = r->i - s;
    if (len >= sizeof buf) len = sizeof buf - 1;
    memcpy(buf, d + s, len);
    buf[len] = 0;
    *out = strtof(buf, NULL);
    return true;
}

static bool svgFlag(SvgReader* r, bool* out) {
    svgSkip(r);
    if (r->i >= r->d.len || (r->d.ptr[r->i] != '0' && r->d.ptr[r->i] != '1')) return false;
    *out = r->d.ptr[r->i++] == '1';
    return true;
}

void cscdParseSvgPath(CscdSv d, CscdPath* p) {
    SvgReader rd = {d, 0};
    char cmd = 0;
    CscdPoint cur = {0, 0}, start = {0, 0}, lastCtrl = {0, 0};
    char lastCmd = 0;
    while (rd.i < d.len) {
        svgSkip(&rd);
        if (rd.i >= d.len) break;
        if (cscdIsAlpha(d.ptr[rd.i])) cmd = d.ptr[rd.i++];
        else if (!cmd) break;
        bool rel = cmd >= 'a' && cmd <= 'z';
        char C = cscdToUpper(cmd);
        float a[7];
#define RD(n) do { for (int k_ = 0; k_ < (n); ++k_) if (!svgNumber(&rd, &a[k_])) return; } while (0)
        switch (C) {
        case 'M':
            RD(2);
            if (rel) { a[0] += cur.x; a[1] += cur.y; }
            cscdPathMoveTo(p, a[0], a[1]);
            cur = start = cscdPoint(a[0], a[1]);
            cmd = rel ? 'l' : 'L';
            break;
        case 'L':
            RD(2);
            if (rel) { a[0] += cur.x; a[1] += cur.y; }
            cscdPathLineTo(p, a[0], a[1]);
            cur = cscdPoint(a[0], a[1]);
            break;
        case 'H':
            RD(1);
            cur.x = rel ? cur.x + a[0] : a[0];
            cscdPathLineTo(p, cur.x, cur.y);
            break;
        case 'V':
            RD(1);
            cur.y = rel ? cur.y + a[0] : a[0];
            cscdPathLineTo(p, cur.x, cur.y);
            break;
        case 'C':
            RD(6);
            if (rel) for (int k = 0; k < 6; k += 2) { a[k] += cur.x; a[k + 1] += cur.y; }
            cscdPathCubicTo(p, a[0], a[1], a[2], a[3], a[4], a[5]);
            lastCtrl = cscdPoint(a[2], a[3]);
            cur = cscdPoint(a[4], a[5]);
            break;
        case 'S': {
            RD(4);
            if (rel) for (int k = 0; k < 4; k += 2) { a[k] += cur.x; a[k + 1] += cur.y; }
            CscdPoint c1 = (lastCmd == 'C' || lastCmd == 'S') ? cscdPoint(2 * cur.x - lastCtrl.x, 2 * cur.y - lastCtrl.y) : cur;
            cscdPathCubicTo(p, c1.x, c1.y, a[0], a[1], a[2], a[3]);
            lastCtrl = cscdPoint(a[0], a[1]);
            cur = cscdPoint(a[2], a[3]);
            break;
        }
        case 'Q':
            RD(4);
            if (rel) for (int k = 0; k < 4; k += 2) { a[k] += cur.x; a[k + 1] += cur.y; }
            cscdPathQuadTo(p, a[0], a[1], a[2], a[3]);
            lastCtrl = cscdPoint(a[0], a[1]);
            cur = cscdPoint(a[2], a[3]);
            break;
        case 'T': {
            RD(2);
            if (rel) { a[0] += cur.x; a[1] += cur.y; }
            CscdPoint c = (lastCmd == 'Q' || lastCmd == 'T') ? cscdPoint(2 * cur.x - lastCtrl.x, 2 * cur.y - lastCtrl.y) : cur;
            cscdPathQuadTo(p, c.x, c.y, a[0], a[1]);
            lastCtrl = c;
            cur = cscdPoint(a[0], a[1]);
            break;
        }
        case 'A': {
            bool large, sweep;
            if (!svgNumber(&rd, &a[0]) || !svgNumber(&rd, &a[1]) || !svgNumber(&rd, &a[2]) || !svgFlag(&rd, &large) || !svgFlag(&rd, &sweep) ||
                !svgNumber(&rd, &a[3]) || !svgNumber(&rd, &a[4]))
                return;
            if (rel) { a[3] += cur.x; a[4] += cur.y; }
            cscdPathArcTo(p, a[0], a[1], a[2], large, sweep, a[3], a[4]);
            cur = cscdPoint(a[3], a[4]);
            break;
        }
        case 'Z':
            cscdPathClose(p);
            cur = start;
            cmd = 0; /* (it takes no numbers: one after it ends the path data, as an error does) */
            break;
        default: return;
        }
#undef RD
        lastCmd = C;
    }
}

/* ---- RRect ------------------------------------------------------------------------------------- */

static void fitRadii(float* f, float side, float a, float b) { if (a + b > side && a + b > 0) *f = cscdMinf(*f, side / (a + b)); }

void cscdRRectNormalize(CscdRRect* r) {
    float w = r->rect.w, h = r->rect.h;
    for (int i = 0; i < 4; ++i) {
        r->rx[i] = cscdMaxf(0.f, r->rx[i]);
        r->ry[i] = cscdMaxf(0.f, r->ry[i]);
        if (r->rx[i] == 0 || r->ry[i] == 0) r->rx[i] = r->ry[i] = 0;
    }
    float f = 1;
    fitRadii(&f, w, r->rx[0], r->rx[1]); fitRadii(&f, w, r->rx[3], r->rx[2]);
    fitRadii(&f, h, r->ry[0], r->ry[3]); fitRadii(&f, h, r->ry[1], r->ry[2]);
    if (f < 1) for (int i = 0; i < 4; ++i) { r->rx[i] *= f; r->ry[i] *= f; }
}

CscdRRect cscdRRectInset(const CscdRRect* r, float t, float rr, float b, float l) {
    CscdRRect o = cscdRRect(cscdRectInset(r->rect, l, t, rr, b));
    o.rx[0] = cscdMaxf(0.f, r->rx[0] - l); o.ry[0] = cscdMaxf(0.f, r->ry[0] - t);
    o.rx[1] = cscdMaxf(0.f, r->rx[1] - rr); o.ry[1] = cscdMaxf(0.f, r->ry[1] - t);
    o.rx[2] = cscdMaxf(0.f, r->rx[2] - rr); o.ry[2] = cscdMaxf(0.f, r->ry[2] - b);
    o.rx[3] = cscdMaxf(0.f, r->rx[3] - l); o.ry[3] = cscdMaxf(0.f, r->ry[3] - b);
    if (o.rect.w < 0) o.rect.w = 0;
    if (o.rect.h < 0) o.rect.h = 0;
    return o;
}

/* ---- Shader ------------------------------------------------------------------------------------ */

CscdShader cscdShaderSolid(CscdColor c) {
    CscdShader s;
    memset(&s, 0, sizeof s);
    s.kind = CSCD_SHADER_SOLID;
    s.color = c;
    s.smooth = true;
    s.opacity = 1;
    return s;
}

CscdShader cscdShaderLinear(CscdPoint a, CscdPoint b, const CscdGradientStop* stops, size_t n) {
    CscdShader s = cscdShaderSolid((CscdColor){0, 0, 0, 0});
    s.kind = CSCD_SHADER_LINEAR;
    s.p0 = a;
    s.p1 = b;
    cscdVecAppend(&s.stops, stops, n);
    return s;
}

CscdShader cscdShaderRadial(CscdPoint c, float rx, float ry, const CscdGradientStop* stops, size_t n) {
    CscdShader s = cscdShaderSolid((CscdColor){0, 0, 0, 0});
    s.kind = CSCD_SHADER_RADIAL;
    s.center = c;
    s.rx = rx;
    s.ry = ry;
    cscdVecAppend(&s.stops, stops, n);
    return s;
}

CscdShader cscdShaderImage(CscdImage* img, CscdRect dst, bool repeatX, bool repeatY) {
    CscdShader s = cscdShaderSolid((CscdColor){0, 0, 0, 0});
    s.kind = CSCD_SHADER_IMAGE;
    s.image = (CscdImage*)cscdRetain(img);
    s.imageRect = dst;
    s.repeatX = repeatX;
    s.repeatY = repeatY;
    return s;
}

CscdShader cscdShaderCopy(const CscdShader* src) {
    CscdShader s = *src;
    memset(&s.stops, 0, sizeof s.stops);
    cscdVecAppend(&s.stops, src->stops.items, src->stops.len);
    cscdRetain(s.image);
    return s;
}

void cscdShaderFini(CscdShader* s) {
    cscdVecFree(&s->stops);
    cscdRelease(s->image);
    s->image = NULL;
}

CscdStrokeStyle cscdStrokeStyle(float width) {
    CscdStrokeStyle st;
    memset(&st, 0, sizeof st);
    st.width = width;
    st.cap = CSCD_CAP_BUTT;
    st.join = CSCD_JOIN_MITER;
    st.miterLimit = 4;
    return st;
}

void cscdStrokeStyleFini(CscdStrokeStyle* s) { cscdVecFree(&s->dash); }

/* ---- blurs ------------------------------------------------------------------------------------- */

static void boxBlurSizes(float sigma, int out[3]) {
    float wIdeal = sqrtf(12 * sigma * sigma / 3 + 1);
    int wl = cscdIntOf(floorf(wIdeal));
    if (wl % 2 == 0) --wl;
    int wu = wl + 2;
    float mIdeal = (12 * sigma * sigma - 3 * wl * wl - 4 * 3 * wl - 3 * 3) / (-4.f * wl - 4);
    int m = (int)lroundf(mIdeal);
    for (int i = 0; i < 3; ++i) out[i] = i < m ? wl : wu;
}

static void boxBlurH(const float* src, float* dst, int w, int h, int r) {
    if (r <= 0) { memcpy(dst, src, sizeof(float) * w * h); return; }
    float inv = 1.f / (2 * r + 1);
    for (int y = 0; y < h; ++y) {
        const float* s = src + (size_t)y * w;
        float* d = dst + (size_t)y * w;
        float acc = 0;
        for (int x = 0, end = r < w - 1 ? r : w - 1; x <= end; ++x) acc += s[x]; /* (the window's part inside the row) */
        for (int x = 0; x < w; ++x) {
            d[x] = acc * inv;
            int add = x + r + 1, sub = x - r;
            if (add < w) acc += s[add];
            if (sub >= 0) acc -= s[sub];
        }
    }
}

static void transpose(const float* src, float* dst, int w, int h) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) dst[(size_t)x * h + y] = src[(size_t)y * w + x];
}

static void swapVec(CscdFloatVec* a, CscdFloatVec* b) {
    CscdFloatVec t = *a;
    *a = *b;
    *b = t;
}

void cscdGaussianBlur(CscdFloatVec* buf, int w, int h, float sigma) {
    if (sigma <= 0.01f || w <= 0 || h <= 0) return;
    int sizes[3];
    boxBlurSizes(sigma, sizes);
    CscdFloatVec tmp = {0};
    cscdVecResize(&tmp, buf->len);
    for (int i = 0; i < 3; ++i) { boxBlurH(buf->items, tmp.items, w, h, (sizes[i] - 1) / 2); swapVec(buf, &tmp); }
    transpose(buf->items, tmp.items, w, h);
    swapVec(buf, &tmp);
    for (int i = 0; i < 3; ++i) { boxBlurH(buf->items, tmp.items, h, w, (sizes[i] - 1) / 2); swapVec(buf, &tmp); }
    transpose(buf->items, tmp.items, h, w);
    swapVec(buf, &tmp);
    cscdVecFree(&tmp);
}

/* ---- stroker ----------------------------------------------------------------------------------- */

static void orient(CscdPoint* poly, size_t n) {
    float area = 0;
    for (size_t i = 0; i < n; ++i) {
        const CscdPoint a = poly[i];
        const CscdPoint b = poly[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    if (area < 0)
        for (size_t i = 0, j = n; i + 1 < j; ++i, --j) {
            CscdPoint t = poly[i];
            poly[i] = poly[j - 1];
            poly[j - 1] = t;
        }
}

static void addOriented(CscdPolyVec* out, CscdPoint* pts, size_t n) {
    orient(pts, n);
    cscdPolyVecAdd(out, pts, n);
}

static void circlePoly(CscdPoint c, float r, CscdPolyVec* out) {
    int n = cscdClampi(cscdIntOf(ceilf(r * 1.5f)), 8, 64);
    CscdPoint p[64];
    for (int i = 0; i < n; ++i) {
        float a = 2 * CSCD_PI * i / n;
        p[i] = cscdPoint(c.x + r * cscdCosf(a), c.y + r * cscdSinf(a));
    }
    addOriented(out, p, (size_t)n);
}

/* More dashes than this on one line are finer than any pixel: it is drawn solid instead (a line
 * across the whole of float's range with a dash of 1 would never end). */
#define MAX_DASHES (1 << 18)

void cscdApplyDash(const CscdPoint* poly, size_t np, bool closed, const float* dash, size_t ndash, float offset, CscdPolyVec* out) {
    float total = 0;
    bool valid = true;
    for (size_t i = 0; i < ndash; ++i) {
        total += dash[i];
        valid = valid && dash[i] >= 0;
    }
    double length = 0;
    for (size_t i = 0; i + 1 < np; ++i) length += cscdHypotf(poly[i + 1].x - poly[i].x, poly[i + 1].y - poly[i].y);
    if (closed && np) length += cscdHypotf(poly[0].x - poly[np - 1].x, poly[0].y - poly[np - 1].y);
    /* (written so that a NaN or an infinity anywhere draws it solid too) */
    if (!valid || !(total > 0) || !(length / total * (double)ndash < MAX_DASHES) || !isfinite(offset)) { cscdPolyVecAdd(out, poly, np); return; }
    CscdPointVec pts = {0};
    cscdVecAppend(&pts, poly, np);
    if (closed) cscdVecPush(&pts, poly[0]);
    size_t di = 0;
    float rem = dash[0];
    offset = fmodf(offset, total);
    if (offset < 0) offset += total;
    while (offset > 0) {
        if (offset >= rem) { offset -= rem; di = (di + 1) % ndash; rem = dash[di]; }
        else { rem -= offset; offset = 0; }
    }
    bool on = di % 2 == 0;
    CscdPointVec cur = {0};
    if (on) cscdVecPush(&cur, pts.items[0]);
    for (size_t i = 1; i < pts.len; ++i) {
        CscdPoint a = pts.items[i - 1], b = pts.items[i];
        float len = cscdHypotf(b.x - a.x, b.y - a.y);
        float pos = 0;
        while (len - pos > rem) {
            pos += rem;
            float t = pos / len;
            CscdPoint m = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
            if (on) { cscdVecPush(&cur, m); cscdPolyVecAdd(out, cur.items, cur.len); cscdVecClear(&cur); }
            else { cscdVecClear(&cur); cscdVecPush(&cur, m); }
            on = !on;
            di = (di + 1) % ndash;
            rem = dash[di];
        }
        rem -= len - pos;
        if (on) cscdVecPush(&cur, b);
    }
    if (on && cur.len > 1) cscdPolyVecAdd(out, cur.items, cur.len);
    cscdVecFree(&cur);
    cscdVecFree(&pts);
}

void cscdStrokePolyline(const CscdPoint* in, size_t nin, bool closed, const CscdStrokeStyle* st, float hw, CscdPolyVec* out) {
    /* drop duplicate points */
    CscdPointVec pv = {0};
    for (size_t i = 0; i < nin; ++i) {
        CscdPoint q = in[i];
        if (!pv.len || fabsf(q.x - cscdVecLast(&pv).x) > 1e-4f || fabsf(q.y - cscdVecLast(&pv).y) > 1e-4f) cscdVecPush(&pv, q);
    }
    if (closed && pv.len > 2 && fabsf(pv.items[0].x - cscdVecLast(&pv).x) < 1e-4f && fabsf(pv.items[0].y - cscdVecLast(&pv).y) < 1e-4f) pv.len--;
    const CscdPoint* p = pv.items;
    size_t n = pv.len;
    if (n == 1) {
        if (st->cap == CSCD_CAP_ROUND) circlePoly(p[0], hw, out);
        else if (st->cap == CSCD_CAP_SQUARE) {
            CscdPoint q[4] = {{p[0].x - hw, p[0].y - hw}, {p[0].x + hw, p[0].y - hw}, {p[0].x + hw, p[0].y + hw}, {p[0].x - hw, p[0].y + hw}};
            cscdPolyVecAdd(out, q, 4);
        }
        cscdVecFree(&pv);
        return;
    }
    if (n == 0) {
        cscdVecFree(&pv);
        return;
    }
    size_t segs = closed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i) {
        CscdPoint a = p[i], b = p[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = cscdHypotf(dx, dy);
        if (len <= 0) continue;
        float ux = dx / len, uy = dy / len;
        float nx = -uy * hw, ny = ux * hw;
        if (!closed && st->cap == CSCD_CAP_SQUARE) {
            if (i == 0) { a.x -= ux * hw; a.y -= uy * hw; }
            if (i == segs - 1) { b.x += ux * hw; b.y += uy * hw; }
        }
        CscdPoint q[4] = {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}};
        addOriented(out, q, 4);
    }
    /* joins */
    size_t first = closed ? 0 : 1, last = closed ? n : n - 1;
    for (size_t i = first; i < last; ++i) {
        CscdPoint prev = p[(i + n - 1) % n], c = p[i], next = p[(i + 1) % n];
        float d0x = c.x - prev.x, d0y = c.y - prev.y, d1x = next.x - c.x, d1y = next.y - c.y;
        float l0 = cscdHypotf(d0x, d0y), l1 = cscdHypotf(d1x, d1y);
        if (l0 <= 0 || l1 <= 0) continue;
        d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
        float cross = d0x * d1y - d0y * d1x;
        if (fabsf(cross) < 1e-5f && d0x * d1x + d0y * d1y > 0) continue; /* straight */
        if (st->join == CSCD_JOIN_ROUND) { circlePoly(c, hw, out); continue; }
        /* outer side normals */
        float s = cross > 0 ? -1.f : 1.f;
        CscdPoint n0 = {-d0y * hw * s, d0x * hw * s}, n1 = {-d1y * hw * s, d1x * hw * s};
        CscdPoint a = {c.x + n0.x, c.y + n0.y}, b = {c.x + n1.x, c.y + n1.y};
        CscdPoint tri[4] = {c, a, b, {0, 0}};
        size_t ntri = 3;
        if (st->join == CSCD_JOIN_MITER) {
            float cosT = d0x * d1x + d0y * d1y; /* angle between directions */
            float miterLen = 1.f / sqrtf(cscdMaxf(1e-6f, (1 + cosT) / 2)); /* 1/sin(theta/2) with theta the interior angle */
            if (miterLen <= st->miterLimit) {
                float mx = n0.x + n1.x, my = n0.y + n1.y;
                float ml = cscdHypotf(mx, my);
                if (ml > 0) {
                    CscdPoint tip = {c.x + mx / ml * hw * miterLen, c.y + my / ml * hw * miterLen};
                    tri[2] = tip;
                    tri[3] = b;
                    ntri = 4;
                }
            }
        }
        addOriented(out, tri, ntri);
    }
    if (!closed && st->cap == CSCD_CAP_ROUND) { circlePoly(p[0], hw, out); circlePoly(p[n - 1], hw, out); }
    cscdVecFree(&pv);
}

/* ---- the rasterizer ------------------------------------------------------------------------------ */

void cscdAccReset(CscdAccumulator* acc, int x, int y, int w, int h) {
    acc->x0 = x;
    acc->y0 = y;
    acc->w = w;
    acc->h = h;
    cscdVecClear(&acc->a);
    cscdVecResize(&acc->a, (size_t)(w + 3) * h);
}

void cscdAccFree(CscdAccumulator* acc) { cscdVecFree(&acc->a); }

static void accLine(CscdAccumulator* acc, CscdPoint p0, CscdPoint p1) {
    if (p0.y == p1.y) return;
    float dir = 1;
    if (p0.y > p1.y) { CscdPoint t = p0; p0 = p1; p1 = t; dir = -1; }
    const int y0 = acc->y0, h = acc->h, w = acc->w;
    if (p1.y <= (float)y0 || p0.y >= (float)(y0 + h)) return;
    float dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    int yStart = cscdMaxi(y0, cscdIntOf(floorf(p0.y)));
    int yEnd = cscdMini(y0 + h, cscdIntOf(ceilf(p1.y)));
    const int stride = w + 3;
    for (int Y = yStart; Y < yEnd; ++Y) {
        float* row = &acc->a.items[(size_t)(Y - y0) * stride];
        /* where the edge enters and leaves this row, from its ends (not stepped from the rows
         * above, which would depend on where the area starts) */
        const float top = cscdMaxf((float)Y, p0.y), bottom = cscdMinf((float)(Y + 1), p1.y);
        float x = top == p0.y ? p0.x : p0.x + (top - p0.y) * dxdy;
        float xnext = bottom == p1.y ? p1.x : p0.x + (bottom - p0.y) * dxdy;
        float dy = bottom - top;
        float d = dy * dir;
        float xa = cscdMinf(x, xnext), xb = cscdMaxf(x, xnext);
        float xaf = floorf(xa);
        int xai = (int)xaf;
        float xbc = ceilf(xb);
        int xbi = (int)xbc;
        if (xai < 0) xai = 0;
        if (xbi <= xai + 1) {
            float xmf = 0.5f * (x + xnext) - xaf;
            row[xai] += d - d * xmf;
            row[xai + 1] += d * xmf;
        } else {
            float s = 1.f / (xb - xa);
            float x0f = xa - xaf;
            float a0 = 0.5f * s * (1 - x0f) * (1 - x0f);
            float x1f = xb - xbc + 1;
            float am = 0.5f * s * x1f * x1f;
            row[xai] += d * a0;
            if (xbi == xai + 2) row[xai + 1] += d * (1 - a0 - am);
            else {
                float a1 = s * (1.5f - x0f);
                row[xai + 1] += d * (a1 - a0);
                for (int xi = xai + 2; xi < xbi - 1; ++xi) row[xi] += d * s;
                float a2 = a1 + (xbi - xai - 3) * s;
                row[xbi - 1] += d * (1 - a2 - am);
            }
            if (xbi <= w + 2) row[xbi] += d * am;
        }
    }
}

typedef struct Seg { CscdPoint a, b; } Seg;

/* splits at a vertical edge so clamping x is exact */
static size_t clipX(CscdPoint a0, CscdPoint a1, float edge, Seg* out) {
    if ((a0.x < edge) != (a1.x < edge) && a0.x != a1.x) {
        float t = (edge - a0.x) / (a1.x - a0.x);
        CscdPoint m = {edge, a0.y + (a1.y - a0.y) * t};
        out[0] = (Seg){a0, m};
        out[1] = (Seg){m, a1};
        return 2;
    }
    out[0] = (Seg){a0, a1};
    return 1;
}

void cscdAccAddLine(CscdAccumulator* acc, CscdPoint p0, CscdPoint p1) {
    /* (x relative to the area's left side, y kept on the device) */
    p0.x -= acc->x0;
    p1.x -= acc->x0;
    Seg s1[2], s2[4];
    size_t n1 = clipX(p0, p1, 0, s1), n2 = 0;
    for (size_t i = 0; i < n1; ++i) n2 += clipX(s1[i].a, s1[i].b, (float)acc->w, s2 + n2);
    for (size_t i = 0; i < n2; ++i) {
        CscdPoint a0 = s2[i].a, a1 = s2[i].b;
        a0.x = cscdClampf(a0.x, 0.f, (float)acc->w);
        a1.x = cscdClampf(a1.x, 0.f, (float)acc->w);
        accLine(acc, a0, a1);
    }
}

void cscdAccAddPolys(CscdAccumulator* acc, const CscdPolyVec* polys) {
    for (size_t c = 0; c < polys->len; ++c) {
        const CscdPointVec* pv = &polys->items[c];
        for (size_t i = 0; i < pv->len; ++i) cscdAccAddLine(acc, pv->items[i], pv->items[(i + 1) % pv->len]);
    }
}

void cscdAccRow(const CscdAccumulator* acc, int y, CscdFillRule rule, uint8_t* out) {
    const float* r = &acc->a.items[(size_t)y * (acc->w + 3)];
    float sum = 0;
    for (int x = 0; x < acc->w; ++x) {
        sum += r[x];
        float c = fabsf(sum);
        if (rule == CSCD_FILL_EVENODD) { c = fmodf(c, 2.f); if (c > 1) c = 2 - c; }
        else if (c > 1) c = 1;
        out[x] = (uint8_t)(c * 255.f + 0.5f);
    }
}
