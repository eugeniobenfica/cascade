/* Color glyphs: COLR (version 0 layers) with CPAL's first palette, and bitmap glyphs, CBDT/CBLC
 * (Google's emoji) and sbix (Apple's), their PNGs decoded once per strike. */
#include "text/fontInternal.h"
#include "cascade/image.h"

#include <stdio.h>
#include <string.h>

void cscdFontColorInit(CscdFontFace* face, uint32_t off) {
    CscdFontFaceImpl* m = face->impl;
    const uint8_t* b = m->base;
    m->colr = cscdFontFindTable(b, m->size, off, "COLR", NULL);
    m->cbdt = cscdFontFindTable(b, m->size, off, "CBDT", NULL);
    m->cblc = cscdFontFindTable(b, m->size, off, "CBLC", NULL);
    m->sbix = cscdFontFindTable(b, m->size, off, "sbix", NULL);
    if (m->colr && !within(m, m->colr, 14)) m->colr = 0;
    if (!m->cbdt || !m->cblc || !within(m, m->cblc, 8)) m->cbdt = m->cblc = 0;
    if (m->sbix && !within(m, m->sbix, 8)) m->sbix = 0;
    const uint32_t cpal = cscdFontFindTable(b, m->size, off, "CPAL", NULL);
    if (cpal && within(m, cpal, 14)) {
        const uint16_t entries = u16(b + cpal + 2), records = u16(b + cpal + 6);
        const uint32_t array = cpal + u32(b + cpal + 8);
        const uint16_t first = u16(b + cpal + 12); /* (palette 0's first color record) */
        for (uint16_t i = 0; i < entries && first + i < records && within(m, array + 4u * (first + i), 4); ++i) {
            const uint8_t* c = b + array + 4 * (first + i); /* blue, green, red, alpha */
            cscdVecPush(&m->palette, ((uint32_t)c[3] << 24) | ((uint32_t)c[2] << 16) | ((uint32_t)c[1] << 8) | c[0]);
        }
    }
}

void cscdFontColorFree(CscdFontFaceImpl* m) {
    cscdVecFree(&m->palette);
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&m->bitmaps, s));) cscdRelease(s->val.p);
    cscdStrMapFree(&m->bitmaps);
}

bool cscdFontFaceHasColor(const CscdFontFace* f) {
    const CscdFontFaceImpl* m = f->impl;
    return m->colr || m->cbdt || m->sbix;
}

size_t cscdFontFaceColorLayers(const CscdFontFace* f, uint16_t glyph, CscdColorLayer* out, size_t max) {
    const CscdFontFaceImpl* m = f->impl;
    if (!m->colr) return 0;
    const uint8_t* c = m->base + m->colr;
    const uint16_t nbase = u16(c + 2), nlayers = u16(c + 12);
    const uint32_t bases = m->colr + u32(c + 4), layers = m->colr + u32(c + 8);
    if (!within(m, bases, 6u * nbase) || !within(m, layers, 4u * nlayers)) return 0;
    int lo = 0, hi = nbase - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint8_t* r = m->base + bases + 6 * mid;
        const uint16_t g = u16(r);
        if (g < glyph) { lo = mid + 1; continue; }
        if (g > glyph) { hi = mid - 1; continue; }
        const uint16_t first = u16(r + 2), count = u16(r + 4);
        if (first + count > nlayers) return 0;
        for (uint16_t i = 0; i < count && i < max; ++i) {
            const uint8_t* l = m->base + layers + 4 * (first + i);
            const uint16_t pi = u16(l + 2);
            out[i].glyph = u16(l);
            out[i].foreground = pi == 0xFFFF || pi >= m->palette.len;
            out[i].argb = out[i].foreground ? 0 : m->palette.items[pi];
        }
        return count;
    }
    return 0;
}

/* ---- bitmaps --------------------------------------------------------------------------------- */

/* A strike's PNG for glyph, decoded once (the face keeps it). */
static const CscdImage* strikeImage(const CscdFontFace* f, uint16_t glyph, int strike, const uint8_t* png, size_t n) {
    CscdFontFaceImpl* m = f->impl;
    char key[32];
    snprintf(key, sizeof key, "%u/%d", glyph, strike);
    cscdMutexLock(&m->cacheMutex);
    CscdMapVal* have = cscdStrMapGet(&m->bitmaps, cscdSv(key));
    if (have) {
        const CscdImage* img = (const CscdImage*)have->p;
        cscdMutexUnlock(&m->cacheMutex);
        return img;
    }
    cscdMutexUnlock(&m->cacheMutex);
    CscdImage* img = cscdDecodeImage(png, n);
    cscdMutexLock(&m->cacheMutex);
    bool inserted = false;
    CscdMapVal* slot = cscdStrMapPut(&m->bitmaps, cscdSv(key), &inserted);
    if (inserted) slot->p = img;
    else { cscdRelease(img); img = (CscdImage*)slot->p; } /* (another thread was first) */
    cscdMutexUnlock(&m->cacheMutex);
    return img;
}

/* CBLC's strikes: the one nearest above size (else the largest), then the glyph's data in CBDT. */
static const CscdImage* cbdtImage(const CscdFontFace* f, uint16_t glyph, float size, CscdRect* where) {
    const CscdFontFaceImpl* m = f->impl;
    const uint8_t* b = m->base;
    const uint32_t n = u32(b + m->cblc + 4);
    if (!withinItems(m, m->cblc + 8, n, 48)) return NULL;
    int best = -1, bestPpem = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* s = b + m->cblc + 8 + 48 * i;
        const uint16_t start = u16(s + 40), end = u16(s + 42);
        const int ppem = s[45];
        if (glyph < start || glyph > end || !ppem) continue;
        const bool better = best < 0 || (bestPpem < size ? ppem > bestPpem : ppem >= size && ppem < bestPpem);
        if (better) { best = (int)i; bestPpem = ppem; }
    }
    if (best < 0) return NULL;
    const uint8_t* s = b + m->cblc + 8 + 48 * best;
    const uint32_t arr = m->cblc + u32(s), count = u32(s + 8);
    if (!withinItems(m, arr, count, 8)) return NULL;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = b + arr + 8 * i;
        const uint16_t first = u16(e), last = u16(e + 2);
        if (glyph < first || glyph > last) continue;
        const uint32_t sub = arr + u32(e + 4);
        if (!within(m, sub, 8)) return NULL;
        const uint16_t indexFormat = u16(b + sub), imageFormat = u16(b + sub + 2);
        const uint32_t dataOff = u32(b + sub + 4);
        uint32_t at = 0, len = 0;
        const uint8_t* bigMetrics = NULL;
        const uint32_t k = glyph - first;
        if (indexFormat == 1 && within(m, sub + 8 + 4 * k, 8)) {
            at = u32(b + sub + 8 + 4 * k);
            len = u32(b + sub + 12 + 4 * k) - at;
        } else if (indexFormat == 3 && within(m, sub + 8 + 2 * k, 4)) {
            at = u16(b + sub + 8 + 2 * k);
            len = u16(b + sub + 10 + 2 * k) - at;
        } else if (indexFormat == 2 && within(m, sub + 8, 12)) {
            len = u32(b + sub + 8);
            at = len * k;
            bigMetrics = b + sub + 12;
        } else if (indexFormat == 4 && within(m, sub + 8, 4)) {
            const uint32_t ng = u32(b + sub + 8);
            if (!withinItems(m, sub + 12, (uint64_t)ng + 1, 4)) return NULL;
            for (uint32_t q = 0; q < ng; ++q)
                if (u16(b + sub + 12 + 4 * q) == glyph) {
                    at = u16(b + sub + 14 + 4 * q);
                    len = u16(b + sub + 18 + 4 * q) - at;
                    break;
                }
        } else if (indexFormat == 5 && within(m, sub + 8, 16)) {
            const uint32_t ng = u32(b + sub + 20);
            if (!withinItems(m, sub + 24, ng, 2)) return NULL;
            for (uint32_t q = 0; q < ng; ++q)
                if (u16(b + sub + 24 + 2 * q) == glyph) { len = u32(b + sub + 8); at = len * q; bigMetrics = b + sub + 12; break; }
        }
        if (!len) return NULL;
        const uint32_t g = m->cbdt + dataOff + at;
        if (!within(m, g, len)) return NULL;
        int width, height, bearingX, bearingY;
        uint32_t png, pngLen;
        if (imageFormat == 17 && len >= 9) {
            height = b[g]; width = b[g + 1]; bearingX = (int8_t)b[g + 2]; bearingY = (int8_t)b[g + 3];
            pngLen = u32(b + g + 5); png = g + 9;
        } else if (imageFormat == 18 && len >= 12) {
            height = b[g]; width = b[g + 1]; bearingX = (int8_t)b[g + 2]; bearingY = (int8_t)b[g + 3];
            pngLen = u32(b + g + 8); png = g + 12;
        } else if (imageFormat == 19 && bigMetrics && len >= 4) {
            height = bigMetrics[0]; width = bigMetrics[1]; bearingX = (int8_t)bigMetrics[2]; bearingY = (int8_t)bigMetrics[3];
            pngLen = u32(b + g); png = g + 4;
        } else return NULL;
        if (!within(m, png, pngLen)) return NULL;
        const CscdImage* img = strikeImage(f, glyph, best, b + png, pngLen);
        if (!img) return NULL;
        const float sc = size / bestPpem;
        *where = cscdRect(bearingX * sc, -bearingY * sc, (width ? width : img->width) * sc, (height ? height : img->height) * sc);
        return img;
    }
    return NULL;
}

static const CscdImage* sbixImage(const CscdFontFace* f, uint16_t glyph, float size, CscdRect* where) {
    const CscdFontFaceImpl* m = f->impl;
    const uint8_t* b = m->base;
    const uint32_t n = u32(b + m->sbix + 4);
    if (!withinItems(m, m->sbix + 8, n, 4) || glyph >= f->numGlyphs) return NULL;
    int best = -1, bestPpem = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t s = m->sbix + u32(b + m->sbix + 8 + 4 * i);
        if (!within(m, s, 4 + 4u * (f->numGlyphs + 1))) continue;
        const uint32_t a = u32(b + s + 4 + 4 * glyph), e = u32(b + s + 8 + 4 * glyph);
        if (e <= a + 8) continue; /* (no picture at this strike) */
        const int ppem = u16(b + s);
        if (!ppem) continue;
        const bool better = best < 0 || (bestPpem < size ? ppem > bestPpem : ppem >= size && ppem < bestPpem);
        if (better) { best = (int)i; bestPpem = ppem; }
    }
    if (best < 0) return NULL;
    const uint32_t s = m->sbix + u32(b + m->sbix + 8 + 4 * best);
    const uint32_t a = s + u32(b + s + 4 + 4 * glyph), e = s + u32(b + s + 8 + 4 * glyph);
    if (e < a || e - a < 8 || !within(m, a, e - a)) return NULL; /* (the 8 bytes read below, at least) */
    if (u32(b + a + 4) != tag("png ")) return NULL; /* (jpg, tiff, dupe and mask graphics are not drawn) */
    const int ox = s16(b + a), oy = s16(b + a + 2);
    const CscdImage* img = strikeImage(f, glyph, best, b + a + 8, e - a - 8);
    if (!img) return NULL;
    const float sc = size / bestPpem;
    *where = cscdRect(ox * sc, -(oy + img->height) * sc, img->width * sc, img->height * sc);
    return img;
}

const CscdImage* cscdFontFaceGlyphImage(const CscdFontFace* f, uint16_t glyph, float size, CscdRect* where) {
    const CscdFontFaceImpl* m = f->impl;
    if (m->cbdt) return cbdtImage(f, glyph, size, where);
    if (m->sbix) return sbixImage(f, glyph, size, where);
    return NULL;
}
