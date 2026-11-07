/* Variable fonts: the axes (fvar), their normalization (avar), glyph outlines moved by the tuple
 * variations of gvar (with the deltas of points not named inferred, IUP), and advances moved by
 * HVAR's item variation store (or gvar's phantom points). An instance is a face of its own, made
 * once for each position and kept by the face it varies. */
#include "text/fontInternal.h"

#include <math.h>
#include <string.h>

void cscdFontVarInit(CscdFontFace* face, uint32_t off) {
    CscdFontFaceImpl* m = face->impl;
    const uint8_t* b = m->base;
    const uint32_t fvar = cscdFontFindTable(b, m->size, off, "fvar", NULL);
    if (!fvar || !within(m, fvar, 16)) return;
    const uint16_t axesOff = u16(b + fvar + 4), count = u16(b + fvar + 8), size = u16(b + fvar + 10);
    if (!count || size < 20 || !within(m, fvar + axesOff, (uint32_t)count * size)) return;
    m->axes = (CscdFontAxis*)cscdCalloc(count, sizeof(CscdFontAxis));
    m->axisCount = count;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* a = b + fvar + axesOff + size * i;
        m->axes[i].tag = u32(a);
        m->axes[i].min = (int32_t)u32(a + 4) / 65536.f;
        m->axes[i].def = (int32_t)u32(a + 8) / 65536.f;
        m->axes[i].max = (int32_t)u32(a + 12) / 65536.f;
    }
    m->avar = cscdFontFindTable(b, m->size, off, "avar", NULL);
    m->gvar = cscdFontFindTable(b, m->size, off, "gvar", NULL);
    m->hvar = cscdFontFindTable(b, m->size, off, "HVAR", NULL);
    if (m->gvar && !within(m, m->gvar, 20)) m->gvar = 0;
    if (m->hvar && !within(m, m->hvar, 20)) m->hvar = 0;
    if (m->avar && !within(m, m->avar, 8)) m->avar = 0;
}

static void instanceFree(CscdFontFace* f) {
    cscdStrFree(&f->family);
    cscdStrFree(&f->subfamily);
    cscdFree((void*)f->coords);
    cscdFree(f);
}

void cscdFontVarFree(CscdFontFaceImpl* m) {
    for (size_t i = 0; i < m->instances.len; ++i) instanceFree((CscdFontFace*)m->instances.items[i]);
    cscdVecFree(&m->instances);
    cscdFree(m->axes);
}

int cscdFontFaceAxisCount(const CscdFontFace* f) { return f->impl->axisCount; }

bool cscdFontFaceAxis(const CscdFontFace* f, int i, uint32_t* t, float* min, float* def, float* max) {
    const CscdFontFaceImpl* m = f->impl;
    if (i < 0 || i >= m->axisCount) return false;
    *t = m->axes[i].tag;
    *min = m->axes[i].min;
    *def = m->axes[i].def;
    *max = m->axes[i].max;
    return true;
}

static float f2dot14(const uint8_t* p) { return s16(p) / 16384.f; }

/* avar's segment map of axis i (when the font has one) */
static float applyAvar(const CscdFontFaceImpl* m, int axis, float v) {
    if (!m->avar) return v;
    uint32_t p = m->avar + 8;
    const uint16_t count = u16(m->base + m->avar + 6);
    for (int i = 0; i < count && i <= axis; ++i) {
        if (!within(m, p, 2)) return v;
        const uint16_t n = u16(m->base + p);
        if (!within(m, p + 2, 4u * n)) return v;
        if (i == axis) {
            if (n < 2) return v;
            const uint8_t* maps = m->base + p + 2;
            for (uint16_t k = 0; k + 1 < n; ++k) {
                const float a = f2dot14(maps + 4 * k), b = f2dot14(maps + 4 * k + 4);
                if (v < a || v > b) continue;
                const float ta = f2dot14(maps + 4 * k + 2), tb = f2dot14(maps + 4 * k + 6);
                return b == a ? ta : ta + (tb - ta) * (v - a) / (b - a);
            }
            return v;
        }
        p += 2 + 4u * n;
    }
    return v;
}

CscdFontFace* cscdFontFaceVary(CscdFontFace* f, const CscdFontTagValue* values, size_t n) {
    CscdFontFace* base = f->varies ? f->varies : f;
    CscdFontFaceImpl* m = base->impl;
    if (!m->axisCount) return base;
    float coords[64];
    const int count = m->axisCount < 64 ? m->axisCount : 64;
    bool any = false;
    float weight = -1;
    for (int i = 0; i < count; ++i) {
        const CscdFontAxis* a = &m->axes[i];
        float v = a->def;
        for (size_t k = 0; k < n; ++k)
            if (values[k].tag == a->tag) v = values[k].value;
        v = cscdClampf(v, a->min, a->max);
        if (a->tag == tag("wght")) weight = v;
        float c = v < a->def ? (a->def > a->min ? (v - a->def) / (a->def - a->min) : 0)
                             : (a->max > a->def ? (v - a->def) / (a->max - a->def) : 0);
        c = applyAvar(m, i, cscdClampf(c, -1, 1));
        c = roundf(c * 16384) / 16384;
        coords[i] = c;
        any = any || c != 0;
    }
    if (!any) return base;
    cscdMutexLock(&m->cacheMutex);
    for (size_t i = 0; i < m->instances.len; ++i) {
        CscdFontFace* inst = (CscdFontFace*)m->instances.items[i];
        if (!memcmp(inst->coords, coords, sizeof(float) * count)) {
            cscdMutexUnlock(&m->cacheMutex);
            return inst;
        }
    }
    CscdFontFace* inst = CSCD_NEW(CscdFontFace);
    *inst = *base;
    inst->family = cscdStrDup(cscdStrSv(&base->family));
    inst->subfamily = cscdStrDup(cscdStrSv(&base->subfamily));
    float* c = (float*)cscdMalloc(sizeof(float) * count);
    memcpy(c, coords, sizeof(float) * count);
    inst->coords = c;
    inst->varies = base;
    if (weight > 0) inst->weight = (int)lroundf(weight);
    cscdVecPush(&m->instances, inst);
    cscdMutexUnlock(&m->cacheMutex);
    return inst;
}

/* ---- tuples ---------------------------------------------------------------------------------- */

/* How much of a tuple's deltas apply at coords: peak, and the region's start and end (NULL: the
 * intermediate region implied by the peak). */
static float tupleScalar(const float* coords, int axes, const uint8_t* peak, const uint8_t* start, const uint8_t* end) {
    float scalar = 1;
    for (int i = 0; i < axes; ++i) {
        const float p = f2dot14(peak + 2 * i), v = coords[i];
        if (p == 0) continue;
        if (v == p) continue;
        if (start) {
            const float s = f2dot14(start + 2 * i), e = f2dot14(end + 2 * i);
            if (s > p || p > e || (s < 0 && e > 0)) continue;
            if (v < s || v > e) return 0;
            scalar *= v < p ? (v - s) / (p - s) : (e - v) / (e - p);
        } else {
            if (v == 0 || v < fminf(0, p) || v > fmaxf(0, p)) return 0;
            scalar *= v / p;
        }
    }
    return scalar;
}

/* Packed point numbers: how many (0 with *all set: every point), into out (at most max). */
static const uint8_t* readPoints(const uint8_t* p, const uint8_t* end, uint16_t* out, int max, int* count, bool* all) {
    *all = false;
    if (p >= end) { *count = 0; return p; }
    int n = *p++;
    if (n & 0x80) {
        if (p >= end) { *count = 0; return p; }
        n = ((n & 0x7F) << 8) | *p++;
    }
    if (n == 0) { *all = true; *count = 0; return p; }
    int got = 0, v = 0;
    while (got < n && p < end) {
        const int ctl = *p++;
        const int run = (ctl & 0x7F) + 1;
        for (int k = 0; k < run && got < n; ++k) {
            if (ctl & 0x80) { if (p + 2 > end) break; v += u16(p); p += 2; }
            else { if (p >= end) break; v += *p++; }
            if (got < max) out[got] = (uint16_t)v;
            ++got;
        }
    }
    *count = got < max ? got : max;
    return p;
}

/* Packed deltas: n of them into out. */
static const uint8_t* readDeltas(const uint8_t* p, const uint8_t* end, float* out, int n) {
    int got = 0;
    while (got < n && p < end) {
        const int ctl = *p++;
        const int run = (ctl & 0x3F) + 1;
        for (int k = 0; k < run && got < n; ++k) {
            if ((ctl & 0xC0) == 0x80) out[got++] = 0;
            else if ((ctl & 0xC0) == 0xC0) { if (p + 4 > end) return end; out[got++] = (float)(int32_t)u32(p); p += 4; }
            else if (ctl & 0x40) { if (p + 2 > end) return end; out[got++] = s16(p); p += 2; }
            else { if (p >= end) return end; out[got++] = (int8_t)*p++; }
        }
    }
    while (got < n) out[got++] = 0;
    return p;
}

/* Deltas not given for points of a contour, from the nearest given ones on each side (IUP), along
 * one coordinate. */
static void inferAlong(const CscdGlyfPoints* gp, const bool* touched, float* d, bool useX) {
    int start = 0;
    for (int c = 0; c < gp->nContours; ++c) {
        const int last = gp->ends[c], n = last - start + 1;
        int firstTouched = -1;
        for (int k = 0; k < n; ++k) if (touched[start + k]) { firstTouched = k; break; }
        if (firstTouched < 0) { start = last + 1; continue; }
        for (int k = 0; k < n; ++k) {
            const int i = start + k;
            if (touched[i]) continue;
            int a = k, b = k;
            do a = (a + n - 1) % n; while (!touched[start + a]);
            do b = (b + 1) % n; while (!touched[start + b]);
            const int ia = start + a, ib = start + b;
            const float ca = useX ? gp->pts[ia].x : gp->pts[ia].y, cb = useX ? gp->pts[ib].x : gp->pts[ib].y;
            const float v = useX ? gp->pts[i].x : gp->pts[i].y;
            float lo = ca, hi = cb, dlo = d[ia], dhi = d[ib];
            if (lo > hi) { lo = cb; hi = ca; dlo = d[ib]; dhi = d[ia]; }
            if (lo == hi) d[i] = dlo == dhi ? dlo : v < lo ? dlo : v > hi ? dhi : 0;
            else if (v <= lo) d[i] = dlo;
            else if (v >= hi) d[i] = dhi;
            else d[i] = dlo + (v - lo) * (dhi - dlo) / (hi - lo);
        }
        start = last + 1;
    }
}

/* The deltas of glyph's points (n of them, the four phantom points last) at the face's position.
 * gp: the simple glyph's points, for inferring the deltas of points a tuple leaves out (NULL for a
 * composite: those stay put). */
static void glyphDeltas(const CscdFontFace* face, uint16_t glyph, int n, const CscdGlyfPoints* gp, float* dx, float* dy) {
    memset(dx, 0, sizeof(float) * n);
    memset(dy, 0, sizeof(float) * n);
    const CscdFontFaceImpl* m = face->impl;
    const uint8_t* g = m->base + m->gvar;
    const int axes = u16(g + 4);
    if (axes != m->axisCount) return;
    const uint16_t sharedCount = u16(g + 6), glyphCount = u16(g + 12), flags = u16(g + 14);
    const uint32_t shared = m->gvar + u32(g + 8), data = m->gvar + u32(g + 16);
    if (glyph >= glyphCount) return;
    uint32_t a, b;
    if (flags & 1) {
        if (!within(m, m->gvar + 20 + 4u * glyph, 8)) return;
        a = u32(g + 20 + 4 * glyph);
        b = u32(g + 24 + 4 * glyph);
    } else {
        if (!within(m, m->gvar + 20 + 2u * glyph, 4)) return;
        a = 2u * u16(g + 20 + 2 * glyph);
        b = 2u * u16(g + 22 + 2 * glyph);
    }
    if (b <= a || !within(m, data + a, b - a) || !withinItems(m, shared, (uint64_t)axes * sharedCount, 2)) return;
    const uint8_t* v = m->base + data + a;
    const uint8_t* vend = m->base + data + b;
    if (v + 4 > vend) return;
    const uint16_t countFlags = u16(v);
    const int tuples = countFlags & 0x0FFF;
    const uint8_t* serialized = v + u16(v + 2);
    const uint8_t* hdr = v + 4;
    uint16_t* sharedPts = (uint16_t*)cscdMalloc(sizeof(uint16_t) * (size_t)n);
    uint16_t* pts = (uint16_t*)cscdMalloc(sizeof(uint16_t) * (size_t)n);
    float* tdx = (float*)cscdMalloc(sizeof(float) * (size_t)n * 2);
    float* tdy = tdx + n;
    bool* touched = (bool*)cscdCalloc((size_t)n, 1);
    int sharedCountPts = 0;
    bool sharedAll = true;
    if (countFlags & 0x8000) serialized = readPoints(serialized, vend, sharedPts, n, &sharedCountPts, &sharedAll);
    for (int t = 0; t < tuples; ++t) {
        if (hdr + 4 > vend) break;
        const uint16_t size = u16(hdr), index = u16(hdr + 2);
        hdr += 4;
        const uint8_t* peak;
        if (index & 0x8000) { peak = hdr; hdr += 2 * axes; }
        else {
            if ((index & 0x0FFF) >= sharedCount) { serialized += size; continue; }
            peak = m->base + shared + 2 * axes * (index & 0x0FFF);
        }
        const uint8_t *start = NULL, *end = NULL;
        if (index & 0x4000) { start = hdr; end = hdr + 2 * axes; hdr += 4 * axes; }
        if (hdr > vend) break;
        const uint8_t* tupleData = serialized;
        const uint8_t* tupleEnd = serialized + size > vend ? vend : serialized + size;
        serialized = tupleEnd;
        const float scalar = tupleScalar(face->coords, axes, peak, start, end);
        if (scalar == 0) continue;
        const uint8_t* p = tupleData;
        int count = sharedCountPts;
        bool all = sharedAll;
        const uint16_t* which = sharedPts;
        if (index & 0x2000) { p = readPoints(p, tupleEnd, pts, n, &count, &all); which = pts; }
        const int nd = all ? n : count;
        p = readDeltas(p, tupleEnd, tdx, nd);
        readDeltas(p, tupleEnd, tdy, nd);
        if (all) {
            for (int i = 0; i < n; ++i) { dx[i] += tdx[i] * scalar; dy[i] += tdy[i] * scalar; }
            continue;
        }
        /* the points named; the others of a simple glyph inferred */
        float* fx = (float*)cscdCalloc((size_t)n * 2, sizeof(float));
        float* fy = fx + n;
        memset(touched, 0, (size_t)n);
        for (int k = 0; k < count; ++k)
            if (which[k] < n) { fx[which[k]] = tdx[k]; fy[which[k]] = tdy[k]; touched[which[k]] = true; }
        if (gp) {
            inferAlong(gp, touched, fx, true);
            inferAlong(gp, touched, fy, false);
        }
        for (int i = 0; i < n; ++i) { dx[i] += fx[i] * scalar; dy[i] += fy[i] * scalar; }
        cscdFree(fx);
    }
    cscdFree(sharedPts);
    cscdFree(pts);
    cscdFree(tdx);
    cscdFree(touched);
}

/* ---- outlines -------------------------------------------------------------------------------- */

static bool varGlyph(const CscdFontFace* face, uint16_t glyph, CscdPath* out, const float* mx, int depth) {
    if (depth > 8) return false;
    uint32_t len;
    const uint8_t* g = cscdGlyfData(face, glyph, &len);
    if (!g || len < 10) return false;
    if (s16(g) >= 0) {
        CscdGlyfPoints gp;
        if (!cscdGlyfPoints(g, len, &gp)) return false;
        const int n = gp.n + 4;
        float* d = (float*)cscdMalloc(sizeof(float) * (size_t)n * 2);
        glyphDeltas(face, glyph, n, &gp, d, d + n);
        CscdGlyfPoints moved = gp;
        moved.pts = (CscdPoint*)cscdMalloc(sizeof(CscdPoint) * (size_t)(gp.n ? gp.n : 1));
        for (int i = 0; i < gp.n; ++i) moved.pts[i] = cscdPoint(gp.pts[i].x + d[i], gp.pts[i].y + d[n + i]);
        cscdGlyfEmit(&moved, mx, out);
        cscdFree(moved.pts);
        cscdFree(d);
        cscdGlyfPointsFree(&gp);
        return true;
    }
    /* a composite: its components' offsets are its points */
    const uint8_t* end = g + len;
    int count = 0;
    for (const uint8_t* p = g + 10; p + 4 <= end;) {
        const uint16_t flags = u16(p);
        p += 4 + ((flags & 1) ? 4 : 2) + ((flags & 8) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0);
        ++count;
        if (!(flags & 0x20)) break;
    }
    const int n = count + 4;
    float* d = (float*)cscdMalloc(sizeof(float) * (size_t)n * 2);
    glyphDeltas(face, glyph, n, NULL, d, d + n);
    bool any = false;
    int k = 0;
    for (const uint8_t* p = g + 10; p + 4 <= end; ++k) {
        const uint16_t flags = u16(p), comp = u16(p + 2);
        p += 4;
        float dx, dy;
        if (flags & 1) { dx = s16(p); dy = s16(p + 2); p += 4; }
        else { dx = (int8_t)p[0]; dy = (int8_t)p[1]; p += 2; }
        float a = 1, b = 0, c = 0, e = 1;
        if (flags & 8) { a = e = s16(p) / 16384.f; p += 2; }
        else if (flags & 0x40) { a = s16(p) / 16384.f; e = s16(p + 2) / 16384.f; p += 4; }
        else if (flags & 0x80) { a = s16(p) / 16384.f; b = s16(p + 2) / 16384.f; c = s16(p + 4) / 16384.f; e = s16(p + 6) / 16384.f; p += 8; }
        if (!(flags & 2)) dx = dy = 0; /* (point matching is not supported) */
        else { dx += d[k]; dy += d[n + k]; }
        const float m2[6] = {mx[0] * a + mx[2] * b, mx[1] * a + mx[3] * b, mx[0] * c + mx[2] * e, mx[1] * c + mx[3] * e,
                             mx[0] * dx + mx[2] * dy + mx[4], mx[1] * dx + mx[3] * dy + mx[5]};
        any |= varGlyph(face, comp, out, m2, depth + 1);
        if (!(flags & 0x20)) break;
    }
    cscdFree(d);
    return any;
}

bool cscdFontVarOutline(const CscdFontFace* face, uint16_t glyph, CscdPath* out) {
    const float identity[6] = {1, 0, 0, 1, 0, 0};
    return varGlyph(face, glyph, out, identity, 0);
}

/* ---- advances -------------------------------------------------------------------------------- */

/* An item variation store's delta for (outer, inner). */
static float storeDelta(const CscdFontFace* face, uint32_t store, uint16_t outer, uint16_t inner) {
    const CscdFontFaceImpl* m = face->impl;
    const uint8_t* b = m->base;
    if (!within(m, store, 8) || u16(b + store) != 1) return 0;
    const uint32_t regions = store + u32(b + store + 2);
    const uint16_t ndata = u16(b + store + 6);
    if (outer >= ndata || !within(m, store + 8 + 4u * outer, 4) || !within(m, regions, 4)) return 0;
    const uint32_t vd = store + u32(b + store + 8 + 4 * outer);
    if (!within(m, vd, 6)) return 0;
    const uint16_t items = u16(b + vd), wordFlags = u16(b + vd + 2), nreg = u16(b + vd + 4);
    const bool longWords = wordFlags & 0x8000;
    const int words = wordFlags & 0x7FFF;
    const int axes = u16(b + regions), regionCount = u16(b + regions + 2);
    if (inner >= items || axes != m->axisCount || !within(m, vd + 6, 2u * nreg)) return 0;
    const int rowSize = longWords ? 4 * words + 2 * (nreg - words) : 2 * words + (nreg - words);
    const uint32_t row = vd + 6 + 2u * nreg + (uint32_t)rowSize * inner;
    if (!within(m, row, (uint32_t)rowSize) || !withinItems(m, regions + 4, (uint64_t)axes * regionCount, 6)) return 0;
    float delta = 0;
    uint32_t p = row;
    for (int r = 0; r < nreg; ++r) {
        int v;
        if (r < words) { v = longWords ? (int32_t)u32(b + p) : s16(b + p); p += longWords ? 4 : 2; }
        else { v = longWords ? s16(b + p) : (int8_t)b[p]; p += longWords ? 2 : 1; }
        const uint16_t ri = u16(b + vd + 6 + 2 * r);
        if (ri >= regionCount || !v) continue;
        const uint8_t* reg = b + regions + 4 + 6 * axes * ri;
        float scalar = 1;
        for (int a = 0; a < axes && scalar != 0; ++a) {
            const float s = f2dot14(reg + 6 * a), pk = f2dot14(reg + 6 * a + 2), e = f2dot14(reg + 6 * a + 4);
            const float c = face->coords[a];
            if (pk == 0 || s > pk || pk > e || (s < 0 && e > 0) || c == pk) continue;
            if (c < s || c > e) scalar = 0;
            else scalar *= c < pk ? (c - s) / (pk - s) : (e - c) / (e - pk);
        }
        delta += scalar * v;
    }
    return delta;
}

float cscdFontVarAdvanceDelta(const CscdFontFace* face, uint16_t glyph) {
    const CscdFontFaceImpl* m = face->impl;
    if (m->hvar) {
        const uint8_t* h = m->base + m->hvar;
        const uint32_t store = m->hvar + u32(h + 4), map = u32(h + 8);
        uint16_t outer = 0, inner = glyph;
        if (map) {
            const uint32_t mp = m->hvar + map;
            if (!within(m, mp, 4)) return 0;
            const uint8_t format = m->base[mp], entry = m->base[mp + 1];
            const uint32_t count = format == 1 ? (within(m, mp, 6) ? u32(m->base + mp + 2) : 0) : u16(m->base + mp + 2);
            const uint32_t data = mp + (format == 1 ? 6 : 4);
            const int size = ((entry >> 4) & 3) + 1, innerBits = (entry & 15) + 1;
            if (!count) return 0;
            const uint32_t i = glyph < count ? glyph : count - 1;
            if (!within(m, data + (uint32_t)size * i, (uint32_t)size)) return 0;
            uint32_t v = 0;
            for (int k = 0; k < size; ++k) v = (v << 8) | m->base[data + size * i + k];
            outer = (uint16_t)(v >> innerBits);
            inner = (uint16_t)(v & ((1u << innerBits) - 1));
        }
        return storeDelta(face, store, outer, inner);
    }
    if (!m->gvar) return 0;
    /* the phantom points: the advance is the distance between the first two */
    uint32_t len;
    const uint8_t* g = cscdGlyfData(face, glyph, &len);
    int n = 4;
    CscdGlyfPoints gp = {0};
    bool simple = false;
    if (g && len >= 10) {
        if (s16(g) >= 0) { simple = cscdGlyfPoints(g, len, &gp); n = gp.n + 4; }
        else {
            int count = 0;
            for (const uint8_t* p = g + 10; p + 4 <= g + len;) {
                const uint16_t flags = u16(p);
                p += 4 + ((flags & 1) ? 4 : 2) + ((flags & 8) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0);
                ++count;
                if (!(flags & 0x20)) break;
            }
            n = count + 4;
        }
    }
    float* d = (float*)cscdMalloc(sizeof(float) * (size_t)n * 2);
    glyphDeltas(face, glyph, n, simple ? &gp : NULL, d, d + n);
    const float delta = d[n - 3] - d[n - 4];
    cscdFree(d);
    if (simple) cscdGlyfPointsFree(&gp);
    return delta;
}
