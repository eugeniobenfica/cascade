/* TrueType / OpenType parsing (glyf and CFF outlines), kerning, system font discovery, CSS font
 * matching and fallback. */
#include "text/fontInternal.h"
#include "text/bidiInternal.h"
#include "cascade/canvas.h"
#include "cascade/image.h"
#include "core/thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif


uint32_t cscdFontFindTable(const uint8_t* data, size_t size, uint32_t offset, const char* name, uint32_t* length) {
    if ((size_t)offset + 12 > size) return 0;
    uint16_t n = u16(data + offset + 4);
    uint32_t t = tag(name);
    for (uint16_t i = 0; i < n; ++i) {
        const size_t rec = (size_t)offset + 12 + 16 * (size_t)i;
        if (rec + 16 > size) return 0;
        if (u32(data + rec) == t) {
            const uint32_t off = u32(data + rec + 8), len = u32(data + rec + 12);
            if (off >= size || len > size - off) return 0; /* (a table past the end: as if not there) */
            if (length) *length = len;
            return off;
        }
    }
    return 0;
}

static void utf16be(const uint8_t* p, size_t len, CscdStr* out) {
    cscdStrClear(out);
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint32_t c = u16(p + i);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < len) {
            uint32_t lo = u16(p + i + 2);
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        cscdUtf8Append(out, c);
    }
}

/* Reads family / subfamily names from a 'name' table. */
static void readNames(const uint8_t* t, size_t len, CscdStr* family, CscdStr* sub, CscdStr* typoFamily, CscdStr* typoSub) {
    if (len < 6) return;
    uint16_t count = u16(t + 2), strOff = u16(t + 4);
    int bestScore[4] = {-1, -1, -1, -1};
    CscdStr* slots[4] = {family, sub, typoFamily, typoSub};
    CscdStr s = {0};
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* r = t + 6 + 12 * i;
        if ((size_t)(r - t) + 12 > len) break;
        uint16_t platform = u16(r), encoding = u16(r + 2), lang = u16(r + 4), id = u16(r + 6), l = u16(r + 8), off = u16(r + 10);
        int slot = id == 1 ? 0 : id == 2 ? 1 : id == 16 ? 2 : id == 17 ? 3 : -1;
        if (slot < 0 || (size_t)strOff + off + l > len) continue;
        int score = -1;
        cscdStrClear(&s);
        if (platform == 3 && (encoding == 1 || encoding == 0 || encoding == 10)) {
            score = lang == 0x409 ? 3 : 2;
            utf16be(t + strOff + off, l, &s);
        } else if (platform == 0) {
            score = 1;
            utf16be(t + strOff + off, l, &s);
        } else if (platform == 1 && encoding == 0) {
            score = 0;
            cscdStrSet(&s, cscdSvMake((const char*)t + strOff + off, l));
        }
        if (score > bestScore[slot]) {
            bestScore[slot] = score;
            cscdStrSet(slots[slot], cscdStrSv(&s));
        }
    }
    cscdStrFree(&s);
}

/* ---- FontFace ---------------------------------------------------------------------------------- */

static void unwrapWoff(const uint8_t* d, size_t n, CscdBytes* out, bool* ok);
static void parseGpos(CscdFontFace* face);
static void parseGsub(CscdFontFace* face);
static void parseCff(CscdFontFace* face, uint32_t off, uint32_t len);

static void bytesDtor(void* p) { cscdVecFree((CscdBytes*)p); }

CscdFontFace* cscdFontFaceLoad(void* owner, const uint8_t* data, size_t size, int faceIndex) {
    if (size >= 4 && (u32(data) == tag("wOFF") || u32(data) == tag("wOF2"))) { /* WOFF, WOFF2: unwrapped, then as any other */
        CscdBytes* sfnt = (CscdBytes*)cscdRcNew(sizeof(CscdBytes), bytesDtor);
        bool ok = false;
        if (u32(data) == tag("wOFF")) unwrapWoff(data, size, sfnt, &ok);
        else ok = cscdWoff2Unwrap(data, size, sfnt);
        CscdFontFace* f = ok ? cscdFontFaceLoad(sfnt, sfnt->items, sfnt->len, faceIndex) : NULL;
        cscdRelease(sfnt);
        return f;
    }
    CscdFontFace* face = CSCD_NEW(CscdFontFace);
    CscdFontFaceImpl* m = CSCD_NEW(CscdFontFaceImpl);
    face->impl = m;
    face->weight = 400;
    face->unitsPerEm = 1000;
    face->ascender = 800;
    face->descender = -200;
    face->xHeight = 500;
    face->capHeight = 700;
    face->underlinePos = -100;
    face->underlineThickness = 50;
    face->strikePos = 250;
    cscdMutexInit(&m->cacheMutex);
    m->data = cscdRetain(owner);
    m->base = data;
    m->size = size;
    const uint8_t* base = data;
    if (size < 12) goto fail;
    uint32_t off = 0;
    if (u32(base) == tag("ttcf")) {
        uint32_t n = u32(base + 8);
        if (faceIndex < 0 || (uint32_t)faceIndex >= n || 12 + 4 * (size_t)faceIndex + 4 > size) goto fail;
        off = u32(base + 12 + 4 * faceIndex);
    }
    uint32_t headLen = 0, hheaLen = 0, maxpLen = 0;
    uint32_t head = cscdFontFindTable(base, size, off, "head", &headLen);
    uint32_t hhea = cscdFontFindTable(base, size, off, "hhea", &hheaLen);
    uint32_t maxp = cscdFontFindTable(base, size, off, "maxp", &maxpLen);
    if (!head || !hhea || !maxp || headLen < 54 || hheaLen < 36 || maxpLen < 6) goto fail;
    face->unitsPerEm = u16(base + head + 18);
    if (face->unitsPerEm <= 0) face->unitsPerEm = 1000;
    m->locaFormat = s16(base + head + 50);
    face->ascender = s16(base + hhea + 4);
    face->descender = s16(base + hhea + 6);
    face->lineGap = s16(base + hhea + 8);
    m->numHMetrics = u16(base + hhea + 34);
    face->numGlyphs = u16(base + maxp + 4);
    uint32_t cmapLen = 0, hmtxLen = 0, kernLen = 0;
    m->cmap = cscdFontFindTable(base, size, off, "cmap", &cmapLen);
    m->loca = cscdFontFindTable(base, size, off, "loca", &m->locaLen);
    m->glyf = cscdFontFindTable(base, size, off, "glyf", NULL);
    m->hmtx = cscdFontFindTable(base, size, off, "hmtx", &hmtxLen);
    m->kern = cscdFontFindTable(base, size, off, "kern", &kernLen);
    if (m->numHMetrics > (int)(hmtxLen / 4)) m->numHMetrics = (int)(hmtxLen / 4); /* (what the table holds) */
    m->gpos = cscdFontFindTable(base, size, off, "GPOS", NULL);
    m->gsub = cscdFontFindTable(base, size, off, "GSUB", NULL);
    m->gdef = cscdFontFindTable(base, size, off, "GDEF", NULL);
    if (m->gdef && !within(m, m->gdef, 12)) m->gdef = 0;
    if (m->gsub && !within(m, m->gsub, 10)) m->gsub = 0;
    if (m->gpos && !within(m, m->gpos, 10)) m->gpos = 0;
    uint32_t cffLen = 0;
    const uint32_t cff = cscdFontFindTable(base, size, off, "CFF ", &cffLen);
    m->cff = cff != 0;
    if (m->cff) parseCff(face, cff, cffLen);
    uint32_t os2Len = 0;
    uint32_t os2 = cscdFontFindTable(base, size, off, "OS/2", &os2Len);
    if (os2 && os2Len >= 78) {
        face->weight = u16(base + os2 + 4);
        uint16_t fsSel = u16(base + os2 + 62);
        face->italic = (fsSel & 1) || (fsSel & 0x200);
        float winA = u16(base + os2 + 74), winD = u16(base + os2 + 76);
        float typoA = s16(base + os2 + 68), typoD = s16(base + os2 + 70), typoG = s16(base + os2 + 72);
        if (fsSel & 0x80) { face->ascender = typoA; face->descender = typoD; face->lineGap = typoG; }
#ifdef _WIN32
        else if (winA + winD > 0) { face->ascender = winA; face->descender = -winD; face->lineGap = 0; } /* what browsers use on Windows */
#else
        (void)winA; (void)winD;
#endif
        face->strikePos = s16(base + os2 + 28); /* yStrikeoutPosition */
        if (os2Len >= 90 && u16(base + os2) >= 2) {
            face->xHeight = s16(base + os2 + 86);
            face->capHeight = s16(base + os2 + 88);
        } else {
            face->xHeight = face->unitsPerEm * 0.5f;
            face->capHeight = face->unitsPerEm * 0.7f;
        }
    }
    uint32_t postLen = 0;
    uint32_t post = cscdFontFindTable(base, size, off, "post", &postLen);
    if (post && postLen >= 12) {
        face->underlinePos = s16(base + post + 8);
        face->underlineThickness = s16(base + post + 10);
        if (face->underlineThickness <= 0) face->underlineThickness = face->unitsPerEm / 20;
    }
    uint32_t nameLen = 0;
    uint32_t name = cscdFontFindTable(base, size, off, "name", &nameLen);
    if (name) {
        CscdStr tf = {0}, ts = {0};
        readNames(base + name, cscdMinz(nameLen, size - name), &face->family, &face->subfamily, &tf, &ts);
        if (tf.len) cscdStrSet(&face->family, cscdStrSv(&tf));
        if (ts.len) cscdStrSet(&face->subfamily, cscdStrSv(&ts));
        cscdStrFree(&tf);
        cscdStrFree(&ts);
    }
    /* cmap subtable */
    if (m->cmap && cmapLen >= 4) {
        uint16_t n = u16(base + m->cmap + 2);
        int best = -1;
        for (uint16_t i = 0; i < n && 4 + 8 * (size_t)i + 8 <= cmapLen; ++i) {
            const uint8_t* r = base + m->cmap + 4 + 8 * i;
            uint16_t p = u16(r), e = u16(r + 2);
            const uint32_t so = m->cmap + u32(r + 4);
            if (so < m->cmap || !within(m, so, 16)) continue;
            int fmt = u16(base + so);
            /* the arrays a lookup reads must be in the font */
            if (fmt == 4 && !within(m, so + 16, 4u * u16(base + so + 6))) continue;
            if ((fmt == 12 || fmt == 13) && (u32(base + so + 12) > (m->size - so - 16) / 12)) continue;
            if (fmt == 0 && !within(m, so + 6, 256)) continue;
            if (fmt == 6 && !within(m, so + 10, 2u * u16(base + so + 8))) continue;
            if (p == 0 && e == 5 && fmt == 14) {
                m->cmap14 = so;
                continue;
            }
            int score = -1;
            if (p == 3 && e == 10 && fmt == 12) score = 9;
            else if (p == 0 && (e == 4 || e == 6) && fmt == 12) score = 8;
            else if ((p == 3 && e == 10) || (p == 0 && e == 6)) score = fmt == 13 ? 7 : -1; /* (many characters, one glyph) */
            else if (p == 3 && e == 1 && fmt == 4) score = 6;
            else if (p == 0 && fmt == 4) score = 5;
            else if ((p == 0 || (p == 3 && e == 1)) && fmt == 6) score = 4;
            else if (p == 3 && e == 0 && fmt == 4) score = 3; /* symbol fonts */
            else if (p == 1 && e == 0 && (fmt == 0 || fmt == 6)) score = 2; /* Mac OS Roman (or Turkish, by its language) */
            if (score > best) {
                best = score;
                m->cmapSub = so;
                m->cmapFormat = fmt;
                m->cmapMac = p == 1;
                m->cmapTurkish = p == 1 && u16(base + so + 4) == 18; /* (Turkish: Macintosh language 17, plus one) */
            }
        }
    }
    if (m->kern) {
        /* version 0 table, format 0 subtables */
        uint16_t version = u16(base + m->kern);
        if (version == 0) {
            uint16_t nt = u16(base + m->kern + 2);
            uint32_t p = m->kern + 4;
            for (uint16_t t = 0; t < nt && (size_t)p + 14 <= size; ++t) {
                uint16_t len = u16(base + p + 2), cov = u16(base + p + 4);
                if ((cov >> 8) == 0 && (cov & 1)) {
                    /* the pairs are searched where they are (they are sorted); only a subtable
                     * that is not is copied into a map */
                    uint16_t np = u16(base + p + 6);
                    while (np && p + 14 + 6 * (size_t)np > size) --np;
                    bool sorted = true;
                    for (uint16_t k = 1; k < np && sorted; ++k) {
                        const uint8_t *a = base + p + 14 + 6 * (k - 1), *b = a + 6;
                        sorted = (((uint32_t)u16(a) << 16) | u16(a + 2)) < (((uint32_t)u16(b) << 16) | u16(b + 2));
                    }
                    if (sorted) cscdVecPush(&m->kernTables, ((KernTable){p + 14, np}));
                    else
                        for (uint16_t k = 0; k < np; ++k) {
                            const uint8_t* r = base + p + 14 + 6 * k;
                            cscdU64MapPut(&m->kernPairs, ((uint32_t)u16(r) << 16) | u16(r + 2), NULL)->i = s16(r + 4);
                        }
                }
                p += len;
            }
        }
    }
    if (m->gpos) parseGpos(face);
    if (m->gsub) parseGsub(face);
    cscdFontColorInit(face, off);
    cscdFontVarInit(face, off);
    if (m->cmapSub != 0) return face;
fail:
    cscdFontFaceFree(face);
    return NULL;
}

void cscdFontFaceFree(CscdFontFace* f) {
    if (!f || f->varies) return; /* (an instance goes with the face it varies) */
    CscdFontFaceImpl* m = f->impl;
    cscdOtFree(m);
    cscdFontColorFree(m);
    cscdFontVarFree(m);
    cscdVecFree(&m->cffLocalSubrs);
    cscdRelease(m->data);
    cscdVecFree(&m->kernTables);
    cscdU64MapFree(&m->kernPairs);
    cscdVecFree(&m->tnumSubs);
    cscdVecFree(&m->pairTables);
    cscdU64MapFree(&m->cmapCache);
    cscdMutexDestroy(&m->cacheMutex);
    cscdFree(m);
    cscdStrFree(&f->family);
    cscdStrFree(&f->subfamily);
    cscdFree(f);
}

static void parseGpos(CscdFontFace* face) {
    CscdFontFaceImpl* m = face->impl;
    const uint32_t g = m->gpos;
    if (!within(m, g, 10)) return;
    const uint16_t featureList = u16(m->base + g + 6), lookupList = u16(m->base + g + 8);
    if (!featureList || !lookupList) return;
    const uint32_t fl = g + featureList, ll = g + lookupList;
    if (!within(m, fl, 2) || !within(m, ll, 2)) return;
    const uint16_t nf = u16(m->base + fl), nl = u16(m->base + ll);
    if (!within(m, fl + 2, 6u * nf) || !within(m, ll + 2, 2u * nl)) return;
    CscdU16Vec lookups = {0};
    for (uint16_t i = 0; i < nf; ++i) {
        const uint8_t* rec = m->base + fl + 2 + 6 * i;
        if (u32(rec) != tag("kern")) continue;
        const uint32_t feat = fl + u16(rec + 4);
        if (!within(m, feat, 4)) continue;
        const uint16_t n = u16(m->base + feat + 2);
        if (!within(m, feat + 4, 2u * n)) continue;
        for (uint16_t k = 0; k < n; ++k) {
            uint16_t li = u16(m->base + feat + 4 + 2 * k);
            bool found = false;
            for (size_t q = 0; q < lookups.len; ++q) found = found || lookups.items[q] == li;
            if (!found) cscdVecPush(&lookups, li);
        }
    }
    /* sorted */
    for (size_t a = 1; a < lookups.len; ++a)
        for (size_t b = a; b > 0 && lookups.items[b - 1] > lookups.items[b]; --b) {
            uint16_t t = lookups.items[b];
            lookups.items[b] = lookups.items[b - 1];
            lookups.items[b - 1] = t;
        }
    for (size_t q = 0; q < lookups.len; ++q) {
        uint16_t li = lookups.items[q];
        if (li >= nl) continue;
        const uint32_t lookupOff = ll + u16(m->base + ll + 2 + 2 * li);
        if (!within(m, lookupOff, 6)) continue;
        const uint8_t* lk = m->base + lookupOff;
        uint16_t type = u16(lk), nsub = u16(lk + 4);
        if (!within(m, lookupOff + 6, 2u * nsub)) continue;
        for (uint16_t s = 0; s < nsub; ++s) {
            uint32_t sub = lookupOff + u16(lk + 6 + 2 * s);
            if (type == 9) { /* extension */
                if (!within(m, sub, 8) || u16(m->base + sub + 2) != 2) continue;
                sub = sub + u32(m->base + sub + 4);
            } else if (type != 2) continue;
            if (within(m, sub, 10)) cscdVecPush(&m->pairTables, sub); /* (the header of either format) */
        }
    }
    cscdVecFree(&lookups);
}

int cscdOtCoverage(const uint8_t* cov, size_t avail, uint16_t glyph) {
    if (avail < 4) return -1;
    uint16_t fmt = u16(cov);
    if (fmt == 1) {
        int n = (int)cscdMinz(u16(cov + 2), (avail - 4) / 2), lo = 0, hi = n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            uint16_t g = u16(cov + 4 + 2 * mid);
            if (g == glyph) return mid;
            if (g < glyph) lo = mid + 1; else hi = mid - 1;
        }
    } else if (fmt == 2) {
        int n = (int)cscdMinz(u16(cov + 2), (avail - 4) / 6), lo = 0, hi = n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            const uint8_t* r = cov + 4 + 6 * mid;
            uint16_t a = u16(r), b = u16(r + 2);
            if (glyph < a) hi = mid - 1;
            else if (glyph > b) lo = mid + 1;
            else return u16(r + 4) + (glyph - a);
        }
    }
    return -1;
}

/* ---- GSUB: whether the font has tabular figures (the substitutions are fontOt.c's) ---------------- */

/* Whether [off, off + len) lies in the font. */

/* The subtables of the lookups of one feature, of the given type (1 single, 4 ligature), extensions
 * followed. */
static void gsubFeature(CscdFontFaceImpl* m, const char* feature, uint16_t type, CscdU32Vec* out) {
    const uint32_t g = m->gsub;
    if (!within(m, g, 10)) return;
    const uint32_t fl = g + u16(m->base + g + 6), ll = g + u16(m->base + g + 8);
    if (!within(m, fl, 2) || !within(m, ll, 2)) return;
    const uint16_t nf = u16(m->base + fl), nl = u16(m->base + ll);
    if (!within(m, fl + 2, 6u * nf) || !within(m, ll + 2, 2u * nl)) return;
    CscdU16Vec lookups = {0};
    for (uint16_t i = 0; i < nf; ++i) {
        const uint8_t* rec = m->base + fl + 2 + 6 * i;
        if (u32(rec) != tag(feature)) continue;
        const uint32_t feat = fl + u16(rec + 4);
        if (!within(m, feat, 4)) continue;
        const uint16_t n = u16(m->base + feat + 2);
        if (!within(m, feat + 4, 2u * n)) continue;
        for (uint16_t k = 0; k < n; ++k) {
            const uint16_t li = u16(m->base + feat + 4 + 2 * k);
            bool seen = false;
            for (size_t q = 0; q < lookups.len; ++q) seen = seen || lookups.items[q] == li;
            if (!seen && li < nl) cscdVecPush(&lookups, li);
        }
    }
    for (size_t a = 1; a < lookups.len; ++a) /* (in lookup order) */
        for (size_t b = a; b > 0 && lookups.items[b - 1] > lookups.items[b]; --b) {
            const uint16_t t = lookups.items[b];
            lookups.items[b] = lookups.items[b - 1];
            lookups.items[b - 1] = t;
        }
    for (size_t q = 0; q < lookups.len; ++q) {
        const uint32_t lk = ll + u16(m->base + ll + 2 + 2 * lookups.items[q]);
        if (!within(m, lk, 6)) continue;
        const uint16_t lt = u16(m->base + lk), ns = u16(m->base + lk + 4);
        if (!within(m, lk + 6, 2u * ns)) continue;
        for (uint16_t s = 0; s < ns; ++s) {
            uint32_t st = lk + u16(m->base + lk + 6 + 2 * s);
            uint16_t t = lt;
            if (t == 7) { /* extension: the real type and where it is */
                if (!within(m, st, 8) || u16(m->base + st) != 1) continue;
                t = u16(m->base + st + 2);
                st += u32(m->base + st + 4);
            }
            if (t == type && within(m, st, 6)) cscdVecPush(out, st);
        }
    }
    cscdVecFree(&lookups);
}

static void parseGsub(CscdFontFace* face) {
    CscdFontFaceImpl* m = face->impl;
    gsubFeature(m, "tnum", 1, &m->tnumSubs);
}

/* A single substitution of glyph (itself when none applies). */
static uint16_t gsubSingle(const CscdFontFaceImpl* m, uint32_t st, uint16_t glyph) {
    const uint8_t* p = m->base + st;
    const int idx = otCoverageAt(m, st + u16(p + 2), glyph);
    if (idx < 0) return glyph;
    if (u16(p) == 1) return (uint16_t)(glyph + s16(p + 4));
    if (u16(p) == 2 && idx < u16(p + 4) && within(m, st + 6 + 2u * (uint32_t)idx, 2)) return u16(p + 6 + 2 * idx);
    return glyph;
}

bool cscdFontFaceHasTabularFigures(const CscdFontFace* face) { return face && face->impl->tnumSubs.len > 0; }

uint16_t cscdFontFaceTabularGlyph(const CscdFontFace* face, uint16_t glyph) {
    const CscdFontFaceImpl* m = face->impl;
    for (size_t i = 0; i < m->tnumSubs.len; ++i) glyph = gsubSingle(m, m->tnumSubs.items[i], glyph);
    return glyph;
}

int cscdOtClass(const uint8_t* cd, size_t avail, uint16_t glyph) {
    if (avail < 4) return 0;
    uint16_t fmt = u16(cd);
    if (fmt == 1) {
        if (avail < 6) return 0;
        const uint16_t start = u16(cd + 2);
        const size_t n = cscdMinz(u16(cd + 4), (avail - 6) / 2);
        if (glyph >= start && (size_t)(glyph - start) < n) return u16(cd + 6 + 2 * (glyph - start));
    } else if (fmt == 2) {
        int n = (int)cscdMinz(u16(cd + 2), (avail - 4) / 6), lo = 0, hi = n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            const uint8_t* r = cd + 4 + 6 * mid;
            uint16_t a = u16(r), b = u16(r + 2);
            if (glyph < a) hi = mid - 1;
            else if (glyph > b) lo = mid + 1;
            else return u16(r + 4);
        }
    }
    return 0;
}

static int popcount16(uint16_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

static float valueOf(const uint8_t* rec, int xadv, int xpl) {
    float v = 0;
    if (xadv >= 0) v += s16(rec + xadv);
    else if (xpl >= 0) v += s16(rec + xpl);
    return v;
}

float cscdFontFaceKerning(const CscdFontFace* face, uint16_t left, uint16_t right) {
    const CscdFontFaceImpl* m = face->impl;
    for (size_t pt = 0; pt < m->pairTables.len; ++pt) {
        const uint32_t to = m->pairTables.items[pt]; /* (its first 10 bytes are in the font) */
        const uint8_t* t = m->base + to;
        uint16_t fmt = u16(t);
        uint16_t vf1 = u16(t + 4), vf2 = u16(t + 6);
        int ci = otCoverageAt(m, to + u16(t + 2), left);
        if (ci < 0) continue;
        int size1 = popcount16(vf1) * 2, size2 = popcount16(vf2) * 2;
        int xadv = (vf1 & 4) ? popcount16(vf1 & 3) * 2 : -1;
        int xpl = (vf1 & 1) ? 0 : -1;
        if (fmt == 1) {
            uint16_t nsets = u16(t + 8);
            if (ci >= nsets || !within(m, to + 10 + 2u * (uint32_t)ci, 2)) continue;
            const uint32_t setOff = to + u16(t + 10 + 2 * ci);
            if (!within(m, setOff, 2)) continue;
            const uint8_t* set = m->base + setOff;
            int recSize = 2 + size1 + size2;
            int n = (int)cscdMinz(u16(set), (m->size - setOff - 2) / (size_t)recSize); /* (the records in the font) */
            int lo = 0, hi = n - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                const uint8_t* r = set + 2 + recSize * mid;
                uint16_t g = u16(r);
                if (g == right) { if (xadv < 0 && xpl < 0) return 0; return valueOf(r + 2, xadv, xpl); }
                if (g < right) lo = mid + 1; else hi = mid - 1;
            }
        } else if (fmt == 2) {
            if (!within(m, to, 16)) continue;
            uint16_t n1 = u16(t + 12), n2 = u16(t + 14);
            int c1 = otClassAt(m, to + u16(t + 8), left), c2 = otClassAt(m, to + u16(t + 10), right);
            if (c1 >= n1 || c2 >= n2) continue;
            if (xadv < 0 && xpl < 0) continue;
            const uint64_t rec = (uint64_t)(size1 + size2), at = (uint64_t)to + 16 + ((uint64_t)c1 * n2 + (uint64_t)c2) * rec;
            if (at + rec > m->size) continue;
            float v = valueOf(m->base + at, xadv, xpl);
            if (v != 0) return v;
        }
    }
    const uint32_t key = ((uint32_t)left << 16) | right;
    if (m->kernPairs.len) {
        CscdMapVal* v = cscdU64MapGet(&m->kernPairs, key);
        if (v) return (float)v->i;
    }
    for (size_t t = m->kernTables.len; t-- > 0;) { /* (a later subtable wins) */
        const uint8_t* pairs = m->base + m->kernTables.items[t].pairs;
        int lo = 0, hi = (int)m->kernTables.items[t].count - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            const uint8_t* r = pairs + 6 * mid;
            const uint32_t k = ((uint32_t)u16(r) << 16) | u16(r + 2);
            if (k == key) return s16(r + 4);
            if (k < key) lo = mid + 1;
            else hi = mid - 1;
        }
    }
    return 0;
}

/* Mac OS Roman's characters 0x80..0xFF (a Macintosh platform cmap, encoding 0, is in its codes), and where Mac OS
 * Turkish (the same encoding, its subtable's language Turkish) has others */
static const uint16_t kMacRoman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
    0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
    0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};
static const uint16_t kMacTurkish[][2] = {{0xDA, 0x011E}, {0xDB, 0x011F}, {0xDC, 0x0130}, {0xDD, 0x0131},
                                          {0xDE, 0x015E}, {0xDF, 0x015F}, {0xF5, 0xF8A0}};

/* A character's code in a Macintosh cmap (Roman, or Turkish): -1 when it has none. */
static int macCode(uint32_t cp, bool turkish) {
    if (cp < 0x80) return (int)cp;
    if (turkish)
        for (size_t i = 0; i < CSCD_COUNT(kMacTurkish); ++i)
            if (kMacTurkish[i][1] == cp) return kMacTurkish[i][0];
    for (int i = 0; i < 128; ++i) {
        if (kMacRoman[i] != cp) continue;
        bool replaced = false; /* (a code Turkish gives another character) */
        for (size_t k = 0; turkish && k < CSCD_COUNT(kMacTurkish); ++k) replaced = replaced || kMacTurkish[k][0] == 0x80 + i;
        if (!replaced) return 0x80 + i;
    }
    return -1;
}

/* The glyph of a character followed by a variation selector, by the font's format 14 cmap: the glyph the sequence
 * has, the character's own when the sequence takes its default glyph, 0 when the font has not the sequence. */
uint16_t cscdFontFaceVariantGlyph(const CscdFontFace* face, uint32_t cp, uint32_t selector) {
    const CscdFontFaceImpl* m = face->impl;
    const uint32_t t = m->cmap14;
    if (!t || !within(m, t, 10)) return 0;
    const uint32_t n = u32(m->base + t + 6);
    if (n > (m->size - t - 10) / 11) return 0;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t* r = m->base + t + 10 + 11 * mid;
        const uint32_t vs = (uint32_t)r[0] << 16 | (uint32_t)r[1] << 8 | r[2];
        if (vs < selector) { lo = mid + 1; continue; }
        if (vs > selector) { hi = mid; continue; }
        const uint32_t def = u32(r + 3), nondef = u32(r + 7);
        if (nondef && within(m, t + nondef, 4)) {
            const uint32_t k = u32(m->base + t + nondef);
            uint32_t a = 0, b = k <= (m->size - t - nondef - 4) / 5 ? k : 0;
            while (a < b) {
                const uint32_t c = a + (b - a) / 2;
                const uint8_t* e = m->base + t + nondef + 4 + 5 * c;
                const uint32_t u = (uint32_t)e[0] << 16 | (uint32_t)e[1] << 8 | e[2];
                if (u == cp) {
                    const uint16_t g = u16(e + 3);
                    return g < face->numGlyphs ? g : 0;
                }
                if (u < cp) a = c + 1;
                else b = c;
            }
        }
        if (def && within(m, t + def, 4)) {
            const uint32_t k = u32(m->base + t + def);
            if (k > (m->size - t - def - 4) / 4) return 0;
            for (uint32_t i = 0; i < k; ++i) { /* (ranges: a start and how many more) */
                const uint8_t* e = m->base + t + def + 4 + 4 * i;
                const uint32_t start = (uint32_t)e[0] << 16 | (uint32_t)e[1] << 8 | e[2];
                if (cp >= start && cp <= start + e[3]) return cscdFontFaceGlyphIndex(face, cp);
                if (start > cp) break;
            }
        }
        return 0;
    }
    return 0;
}

uint16_t cscdFontFaceGlyphIndex(const CscdFontFace* face, uint32_t cp) {
    CscdFontFaceImpl* m = face->impl;
    cscdMutexLock(&m->cacheMutex);
    CscdMapVal* cached = cscdU64MapGet(&m->cmapCache, cp);
    if (cached) {
        uint16_t g = (uint16_t)cached->u;
        cscdMutexUnlock(&m->cacheMutex);
        return g;
    }
    cscdMutexUnlock(&m->cacheMutex);
    uint16_t g = 0;
    const uint8_t* t = m->base + m->cmapSub;
    if (m->cmapFormat == 4) {
        uint32_t c = cp;
        if (cp > 0xFFFF) c = 0xFFFFFFFF;
        uint16_t segX2 = u16(t + 6);
        int segs = segX2 / 2;
        const uint8_t* ends = t + 14;
        const uint8_t* starts = ends + segX2 + 2;
        const uint8_t* deltas = starts + segX2;
        const uint8_t* ranges = deltas + segX2;
        int lo = 0, hi = segs - 1;
        while (c <= 0xFFFF && lo <= hi) {
            int mid = (lo + hi) / 2;
            uint16_t end = u16(ends + 2 * mid);
            if (c > end) { lo = mid + 1; continue; }
            uint16_t start = u16(starts + 2 * mid);
            if (c < start) { hi = mid - 1; continue; }
            uint16_t ro = u16(ranges + 2 * mid);
            int16_t delta = s16(deltas + 2 * mid);
            if (ro == 0) g = (uint16_t)(c + delta);
            else {
                const uint8_t* p = ranges + 2 * mid + ro + 2 * (c - start);
                if (p + 2 <= m->base + m->size) {
                    g = u16(p);
                    if (g) g = (uint16_t)(g + delta);
                }
            }
            break;
        }
        /* symbol fonts map U+F000 + code */
        if (!g && cp < 0x100 && segs > 0 && u16(starts) >= 0xF000) {
            cscdMutexLock(&m->cacheMutex);
            cscdU64MapPut(&m->cmapCache, cp, NULL)->u = 0;
            cscdMutexUnlock(&m->cacheMutex);
            return 0;
        }
    } else if (m->cmapFormat == 0 || m->cmapFormat == 6) {
        const int code = m->cmapMac ? macCode(cp, m->cmapTurkish) : (cp <= 0xFFFF ? (int)cp : -1);
        if (m->cmapFormat == 0 && code >= 0 && code < 256) g = t[6 + code];
        else if (m->cmapFormat == 6 && code >= 0) {
            const int first = u16(t + 6), count = u16(t + 8);
            if (code >= first && code - first < count) g = u16(t + 10 + 2 * (code - first));
        }
    } else if (m->cmapFormat == 12 || m->cmapFormat == 13) {
        uint32_t n = u32(t + 12);
        int64_t lo = 0, hi = (int64_t)n - 1;
        while (lo <= hi) {
            int64_t mid = (lo + hi) / 2;
            const uint8_t* r = t + 16 + 12 * mid;
            uint32_t a = u32(r), b = u32(r + 4);
            if (cp < a) hi = mid - 1;
            else if (cp > b) lo = mid + 1;
            else { g = (uint16_t)(u32(r + 8) + (m->cmapFormat == 12 ? cp - a : 0)); break; }
        }
    }
    if (g >= face->numGlyphs) g = 0;
    cscdMutexLock(&m->cacheMutex);
    cscdU64MapPut(&m->cmapCache, cp, NULL)->u = g;
    cscdMutexUnlock(&m->cacheMutex);
    return g;
}

float cscdFontFaceAdvance(const CscdFontFace* face, uint16_t glyph) {
    const CscdFontFaceImpl* m = face->impl;
    if (!m->hmtx || m->numHMetrics == 0) return face->unitsPerEm / 2;
    int i = glyph < m->numHMetrics ? glyph : m->numHMetrics - 1; /* (numHMetrics fits the table) */
    float adv = u16(m->base + m->hmtx + 4 * i);
    if (face->coords) adv += cscdFontVarAdvanceDelta(face, glyph);
    return adv;
}

static uint32_t glyphOffset(const CscdFontFace* face, uint16_t glyph, uint32_t* len) {
    const CscdFontFaceImpl* m = face->impl;
    const uint32_t entry = m->locaFormat == 0 ? 2 : 4;
    if (!m->loca || !m->glyf || glyph >= face->numGlyphs || ((size_t)glyph + 2) * entry > m->locaLen) { *len = 0; return 0; }
    uint32_t a, b;
    if (m->locaFormat == 0) { a = u16(m->base + m->loca + 2 * glyph) * 2u; b = u16(m->base + m->loca + 2 * glyph + 2) * 2u; }
    else { a = u32(m->base + m->loca + 4 * glyph); b = u32(m->base + m->loca + 4 * glyph + 4); }
    if (b <= a || (size_t)m->glyf + b > m->size) { *len = 0; return 0; }
    *len = b - a;
    return m->glyf + a;
}

/* ---- CFF outlines (OpenType fonts with PostScript outlines) ------------------------------------ */

/* A CFF INDEX: count, offset size, the offsets, then the data. */
typedef struct CffIndex {
    const uint8_t* base;
    size_t size;
    uint32_t pos, count, offSize;
} CffIndex;

static uint32_t cffOffsetAt(const CffIndex* v, uint32_t i) {
    uint32_t r = 0;
    const size_t at = v->pos + 3 + (size_t)i * v->offSize;
    if (at + v->offSize > v->size) return 0;
    for (uint32_t k = 0; k < v->offSize; ++k) r = (r << 8) | v->base[at + k];
    return r;
}

static uint32_t cffEnd(const CffIndex* v) { return v->count ? v->pos + 3 + (v->count + 1) * v->offSize + cffOffsetAt(v, v->count) - 1 : v->pos + 2; }

static bool cffItem(const CffIndex* v, uint32_t i, uint32_t* start, uint32_t* len) {
    if (i >= v->count) return false;
    const uint32_t dataStart = v->pos + 3 + (v->count + 1) * v->offSize - 1;
    const uint32_t a = cffOffsetAt(v, i), b = cffOffsetAt(v, i + 1);
    if (b < a || (size_t)dataStart + b > v->size) return false;
    *start = dataStart + a;
    *len = b - a;
    return true;
}

static CffIndex cffIndex(const uint8_t* base, size_t size, uint32_t pos) {
    CffIndex v = {base, size, pos, 0, 0};
    if ((size_t)pos + 2 > size) return v;
    v.count = ((uint32_t)base[pos] << 8) | base[pos + 1];
    if (v.count) {
        if ((size_t)pos + 3 > size) { v.count = 0; return v; }
        v.offSize = base[pos + 2];
        if (v.offSize < 1 || v.offSize > 4) v.count = 0;
    }
    return v;
}

typedef CSCD_VEC(double) DoubleVec;
typedef void (*DictFn)(void* ctx, int op, const DoubleVec* args);

/* A DICT: operators with their operands (escaped operators as 1200 + b). */
static void cffDict(const uint8_t* p, uint32_t len, DictFn fn, void* ctx) {
    DoubleVec args = {0};
    CscdStr s = {0};
    for (uint32_t i = 0; i < len;) {
        const uint8_t b = p[i];
        if (b <= 21) {
            int op = b;
            ++i;
            if (b == 12 && i < len) op = 1200 + p[i++];
            fn(ctx, op, &args);
            cscdVecClear(&args);
        } else if (b == 28 && i + 2 < len) { cscdVecPush(&args, (double)(int16_t)((p[i + 1] << 8) | p[i + 2])); i += 3; }
        else if (b == 29 && i + 4 < len) { cscdVecPush(&args, (double)(int32_t)(((uint32_t)p[i + 1] << 24) | ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 8) | p[i + 4])); i += 5; }
        else if (b == 30) { /* a real: nibbles */
            cscdStrClear(&s);
            ++i;
            bool done = false;
            while (i < len && !done) {
                const uint8_t v = p[i++];
                for (int k = 0; k < 2 && !done; ++k) {
                    const int nib = k == 0 ? v >> 4 : v & 15;
                    if (nib <= 9) cscdStrAppendChar(&s, (char)('0' + nib));
                    else if (nib == 10) cscdStrAppendChar(&s, '.');
                    else if (nib == 11) cscdStrAppendChar(&s, 'E');
                    else if (nib == 12) cscdStrAppendC(&s, "E-");
                    else if (nib == 14) cscdStrAppendChar(&s, '-');
                    else if (nib == 15) done = true;
                }
            }
            cscdVecPush(&args, atof(cscdStrC(&s)));
        } else if (b >= 32 && b <= 246) { cscdVecPush(&args, (double)(b - 139)); ++i; }
        else if (b >= 247 && b <= 250 && i + 1 < len) { cscdVecPush(&args, (double)((b - 247) * 256 + p[i + 1] + 108)); i += 2; }
        else if (b >= 251 && b <= 254 && i + 1 < len) { cscdVecPush(&args, (double)(-(b - 251) * 256 - p[i + 1] - 108)); i += 2; }
        else ++i;
    }
    cscdVecFree(&args);
    cscdStrFree(&s);
}

static int subrBias(uint32_t count) { return count < 1240 ? 107 : count < 33900 ? 1131 : 32768; }

typedef struct TopDict { uint32_t charStrings, privSize, privOff, fdArray, fdSelect, subrs; } TopDict;

/* An offset or a size from a dict, where a font can write any number: 0, for none, if it is not one. */
static uint32_t dictOffset(double d) { return d >= 0 && d <= 4294967295.0 ? (uint32_t)d : 0; }

static void topDictFn(void* ctx, int op, const DoubleVec* a) {
    TopDict* t = (TopDict*)ctx;
    if (op == 17 && a->len) t->charStrings = dictOffset(a->items[0]);
    else if (op == 18 && a->len >= 2) { t->privSize = dictOffset(a->items[0]); t->privOff = dictOffset(a->items[1]); }
    else if (op == 1236 && a->len) t->fdArray = dictOffset(a->items[0]);
    else if (op == 1237 && a->len) t->fdSelect = dictOffset(a->items[0]);
}

static void privDictFn(void* ctx, int op, const DoubleVec* a) {
    if (op == 19 && a->len) ((TopDict*)ctx)->subrs = dictOffset(a->items[0]);
}

static uint32_t localSubrs(const uint8_t* b, uint32_t off, size_t end, uint32_t pSize, uint32_t pOff) {
    TopDict d = {0};
    if (pOff && (size_t)off + pOff + pSize <= end) cffDict(b + off + pOff, pSize, privDictFn, &d);
    return d.subrs ? off + pOff + d.subrs : 0;
}

static void parseCff(CscdFontFace* face, uint32_t off, uint32_t len) {
    CscdFontFaceImpl* m = face->impl;
    if (!off || (size_t)off + 4 > m->size) return;
    const uint8_t* b = m->base;
    const size_t end = cscdMinz(m->size, (size_t)off + len);
    /* header, Name INDEX, Top DICT INDEX, String INDEX, Global Subrs INDEX */
    const uint32_t hdr = b[off + 2];
    const CffIndex names = cffIndex(b, end, off + hdr);
    const CffIndex tops = cffIndex(b, end, cffEnd(&names));
    const CffIndex strings = cffIndex(b, end, cffEnd(&tops));
    const CffIndex gsubrs = cffIndex(b, end, cffEnd(&strings));
    m->cffGlobalSubrs = gsubrs.pos;
    uint32_t ts, tl;
    if (!cffItem(&tops, 0, &ts, &tl)) return;
    TopDict top = {0};
    cffDict(b + ts, tl, topDictFn, &top);
    if (!top.charStrings) return;
    m->cffCharStrings = off + top.charStrings;
    cscdVecClear(&m->cffLocalSubrs);
    if (top.fdArray) { /* a CID font: each glyph's font DICT gives its local subroutines */
        const CffIndex fds = cffIndex(b, end, off + top.fdArray);
        for (uint32_t k = 0; k < fds.count; ++k) {
            uint32_t s0, l0;
            TopDict fd = {0};
            if (cffItem(&fds, k, &s0, &l0)) cffDict(b + s0, l0, topDictFn, &fd);
            cscdVecPush(&m->cffLocalSubrs, localSubrs(b, off, end, fd.privSize, fd.privOff));
        }
        m->cffFdSelect = top.fdSelect ? off + top.fdSelect : 0;
    } else cscdVecPush(&m->cffLocalSubrs, localSubrs(b, off, end, top.privSize, top.privOff));
    m->cffOk = true;
}

/* The Type 2 charstring interpreter's state. */
typedef struct CharString {
    const uint8_t* b;
    CffIndex lsubrs, gsubrs;
    bool hasLocal;
    CscdPath* out;
    float st[48];
    int sp, stems;
    bool widthDone, open, drew, ended;
    float x, y;
} CharString;

static void csTakeWidth(CharString* c, bool odd) { /* the first stack-clearing operator may carry the advance first */
    if (!c->widthDone && odd && c->sp > 0) { for (int k = 1; k < c->sp; ++k) c->st[k - 1] = c->st[k]; --c->sp; }
    c->widthDone = true;
}

static void csMoveTo(CharString* c, float dx, float dy) {
    if (c->open) cscdPathClose(c->out);
    c->x += dx;
    c->y += dy;
    cscdPathMoveTo(c->out, c->x, c->y);
    c->open = true;
}

static void csLineTo(CharString* c, float dx, float dy) {
    c->x += dx;
    c->y += dy;
    cscdPathLineTo(c->out, c->x, c->y);
    c->drew = true;
}

static void csCurveTo(CharString* c, float a, float bb, float cc, float d, float e, float f) {
    const float x1 = c->x + a, y1 = c->y + bb, x2 = x1 + cc, y2 = y1 + d;
    c->x = x2 + e;
    c->y = y2 + f;
    cscdPathCubicTo(c->out, x1, y1, x2, y2, c->x, c->y);
    c->drew = true;
}

static bool csRun(CharString* c, uint32_t start, uint32_t len, int depth) {
    if (depth > 10) return false;
    const uint8_t* p = c->b + start;
    float* st = c->st;
    for (uint32_t i = 0; i < len && !c->ended;) {
        const uint8_t v = p[i];
        if (v >= 32 || v == 28) { /* a number */
            float n;
            if (v == 28) { if (i + 2 >= len) return false; n = (int16_t)((p[i + 1] << 8) | p[i + 2]); i += 3; }
            else if (v <= 246) { n = v - 139.f; ++i; }
            else if (v <= 250) { if (i + 1 >= len) return false; n = (v - 247) * 256.f + p[i + 1] + 108; i += 2; }
            else if (v <= 254) { if (i + 1 >= len) return false; n = -(v - 251) * 256.f - p[i + 1] - 108; i += 2; }
            else { if (i + 4 >= len) return false; n = (int32_t)(((uint32_t)p[i + 1] << 24) | ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 8) | p[i + 4]) / 65536.f; i += 5; }
            if (c->sp < 48) st[c->sp++] = n;
            continue;
        }
        ++i;
        const int sp = c->sp;
        switch (v) {
        case 1: case 3: case 18: case 23: /* stems */
            csTakeWidth(c, c->sp % 2 == 1);
            c->stems += c->sp / 2;
            c->sp = 0;
            break;
        case 19: case 20: /* hintmask, cntrmask (stems may come first, as vstem) */
            csTakeWidth(c, c->sp % 2 == 1);
            c->stems += c->sp / 2;
            c->sp = 0;
            i += (c->stems + 7) / 8;
            break;
        case 21: csTakeWidth(c, sp > 2); if (c->sp >= 2) csMoveTo(c, st[c->sp - 2], st[c->sp - 1]); c->sp = 0; break;
        case 22: csTakeWidth(c, sp > 1); if (c->sp >= 1) csMoveTo(c, st[c->sp - 1], 0); c->sp = 0; break;
        case 4: csTakeWidth(c, sp > 1); if (c->sp >= 1) csMoveTo(c, 0, st[c->sp - 1]); c->sp = 0; break;
        case 5: for (int k = 0; k + 1 < sp; k += 2) csLineTo(c, st[k], st[k + 1]); c->sp = 0; break;
        case 6: case 7: { /* hlineto, vlineto: alternating */
            bool h = v == 6;
            for (int k = 0; k < sp; ++k, h = !h) {
                if (h) csLineTo(c, st[k], 0);
                else csLineTo(c, 0, st[k]);
            }
            c->sp = 0;
            break;
        }
        case 8: for (int k = 0; k + 5 < sp; k += 6) csCurveTo(c, st[k], st[k + 1], st[k + 2], st[k + 3], st[k + 4], st[k + 5]); c->sp = 0; break;
        case 24: { /* rcurveline */
            int k = 0;
            for (; k + 7 < sp; k += 6) csCurveTo(c, st[k], st[k + 1], st[k + 2], st[k + 3], st[k + 4], st[k + 5]);
            if (k + 1 < sp) csLineTo(c, st[k], st[k + 1]);
            c->sp = 0;
            break;
        }
        case 25: { /* rlinecurve */
            int k = 0;
            for (; k + 7 < sp; k += 2) csLineTo(c, st[k], st[k + 1]);
            if (k + 5 < sp) csCurveTo(c, st[k], st[k + 1], st[k + 2], st[k + 3], st[k + 4], st[k + 5]);
            c->sp = 0;
            break;
        }
        case 26: { /* vvcurveto */
            int k = 0;
            float dx1 = 0;
            if (sp % 2) dx1 = st[k++];
            for (; k + 3 < sp; k += 4) { csCurveTo(c, dx1, st[k], st[k + 1], st[k + 2], 0, st[k + 3]); dx1 = 0; }
            c->sp = 0;
            break;
        }
        case 27: { /* hhcurveto */
            int k = 0;
            float dy1 = 0;
            if (sp % 2) dy1 = st[k++];
            for (; k + 3 < sp; k += 4) { csCurveTo(c, st[k], dy1, st[k + 1], st[k + 2], st[k + 3], 0); dy1 = 0; }
            c->sp = 0;
            break;
        }
        case 30: case 31: { /* vhcurveto, hvcurveto: alternating, the last may end off the axis */
            bool h = v == 31;
            for (int k = 0; k + 3 < sp; h = !h) {
                const bool last = sp - k == 5;
                if (h) csCurveTo(c, st[k], 0, st[k + 1], st[k + 2], last ? st[k + 4] : 0, st[k + 3]);
                else csCurveTo(c, 0, st[k], st[k + 1], st[k + 2], st[k + 3], last ? st[k + 4] : 0);
                k += last ? 5 : 4;
            }
            c->sp = 0;
            break;
        }
        case 10: case 29: { /* callsubr, callgsubr */
            if (c->sp < 1) return false;
            const CffIndex* subrs = v == 10 ? &c->lsubrs : &c->gsubrs;
            if (v == 10 && !c->hasLocal) return false;
            const int idx = (int)st[--c->sp] + subrBias(subrs->count);
            uint32_t ss, sl;
            if (idx < 0 || !cffItem(subrs, (uint32_t)idx, &ss, &sl)) return false;
            if (!csRun(c, ss, sl, depth + 1)) return false;
            break;
        }
        case 11: return true; /* return */
        case 14: /* endchar */
            csTakeWidth(c, sp == 1 || sp == 5);
            if (c->open) cscdPathClose(c->out);
            c->open = false;
            c->ended = true;
            return true;
        case 12: {
            if (i >= len) return false;
            const uint8_t e = p[i++];
            if (e == 35 && sp >= 12) { /* flex */
                csCurveTo(c, st[0], st[1], st[2], st[3], st[4], st[5]);
                csCurveTo(c, st[6], st[7], st[8], st[9], st[10], st[11]);
            } else if (e == 34 && sp >= 7) { /* hflex */
                csCurveTo(c, st[0], 0, st[1], st[2], st[3], 0);
                csCurveTo(c, st[4], 0, st[5], -st[2], st[6], 0);
            } else if (e == 36 && sp >= 9) { /* hflex1 */
                csCurveTo(c, st[0], st[1], st[2], st[3], st[4], 0);
                csCurveTo(c, st[5], 0, st[6], st[7], st[8], -(st[1] + st[3] + st[7]));
            } else if (e == 37 && sp >= 11) { /* flex1 */
                const float dx = st[0] + st[2] + st[4] + st[6] + st[8], dy = st[1] + st[3] + st[5] + st[7] + st[9];
                csCurveTo(c, st[0], st[1], st[2], st[3], st[4], st[5]);
                if (fabsf(dx) > fabsf(dy)) csCurveTo(c, st[6], st[7], st[8], st[9], st[10], -dy);
                else csCurveTo(c, st[6], st[7], st[8], st[9], -dx, st[10]);
            }
            c->sp = 0;
            break;
        }
        default: c->sp = 0; break;
        }
    }
    return true;
}

static bool cffOutline(const CscdFontFace* face, uint16_t glyph, CscdPath* out) {
    const CscdFontFaceImpl* m = face->impl;
    if (!m->cffOk) return false;
    const uint8_t* b = m->base;
    const size_t size = m->size;
    const CffIndex chars = cffIndex(b, size, m->cffCharStrings);
    /* the glyph's local subroutines (in a CID font, those of its font DICT) */
    uint32_t fd = 0;
    if (m->cffFdSelect && (size_t)m->cffFdSelect < size) {
        const uint8_t fmt = b[m->cffFdSelect];
        if (fmt == 0 && (size_t)m->cffFdSelect + 1 + glyph < size) fd = b[m->cffFdSelect + 1 + glyph];
        else if (fmt == 3 && (size_t)m->cffFdSelect + 3 <= size) {
            const uint32_t nr = ((uint32_t)b[m->cffFdSelect + 1] << 8) | b[m->cffFdSelect + 2];
            for (uint32_t r = 0; r < nr; ++r) {
                const size_t at = m->cffFdSelect + 3 + 3 * (size_t)r;
                if (at + 5 > size) break;
                const uint32_t first = ((uint32_t)b[at] << 8) | b[at + 1], next = ((uint32_t)b[at + 3] << 8) | b[at + 4];
                if (glyph >= first && glyph < next) { fd = b[at + 2]; break; }
            }
        }
    }
    CharString* c = CSCD_NEW(CharString);
    c->b = b;
    c->gsubrs = cffIndex(b, size, m->cffGlobalSubrs);
    c->lsubrs = cffIndex(b, size, fd < m->cffLocalSubrs.len ? m->cffLocalSubrs.items[fd] : 0);
    c->hasLocal = fd < m->cffLocalSubrs.len && m->cffLocalSubrs.items[fd];
    c->out = out;
    uint32_t cs, cl;
    bool ok = cffItem(&chars, glyph, &cs, &cl) && csRun(c, cs, cl, 0);
    if (ok && c->open) cscdPathClose(out);
    ok = ok && c->drew;
    cscdFree(c);
    return ok;
}

/* ---- WOFF: the tables of an OpenType font, each zlib-compressed ------------------------------- */

static uint32_t rd32(const uint8_t* d, size_t at) { return ((uint32_t)d[at] << 24) | ((uint32_t)d[at + 1] << 16) | ((uint32_t)d[at + 2] << 8) | d[at + 3]; }
static void put32At(CscdBytes* o, size_t at, uint32_t v) { o->items[at] = (uint8_t)(v >> 24); o->items[at + 1] = (uint8_t)(v >> 16); o->items[at + 2] = (uint8_t)(v >> 8); o->items[at + 3] = (uint8_t)v; }
static void put16At(CscdBytes* o, size_t at, uint32_t v) { o->items[at] = (uint8_t)(v >> 8); o->items[at + 1] = (uint8_t)v; }

static void unwrapWoff(const uint8_t* d, size_t n, CscdBytes* o, bool* ok) {
    *ok = false;
    if (n < 44 || rd32(d, 0) != tag("wOFF")) return;
    const uint32_t numTables = ((uint32_t)d[12] << 8) | d[13];
    if (n < 44 + 20 * (size_t)numTables || numTables > 512) return;
    /* the sfnt header and table records, then the tables (4-byte aligned) */
    cscdVecClear(o);
    cscdVecResize(o, 12 + 16 * (size_t)numTables);
    put32At(o, 0, rd32(d, 4)); /* (flavor: TrueType or 'OTTO') */
    put16At(o, 4, numTables);
    uint32_t entry = 1, log = 0;
    while (entry * 2 <= numTables) { entry *= 2; ++log; }
    put16At(o, 6, entry * 16); put16At(o, 8, log); put16At(o, 10, numTables * 16 - entry * 16);
    CscdBytes raw = {0};
    for (uint32_t t = 0; t < numTables; ++t) {
        const size_t rec = 44 + 20 * (size_t)t;
        const uint32_t tg = rd32(d, rec), off = rd32(d, rec + 4), compLen = rd32(d, rec + 8), origLen = rd32(d, rec + 12), sum = rd32(d, rec + 16);
        if ((size_t)off + compLen > n || origLen > (64u << 20)) { cscdVecFree(&raw); return; }
        const size_t at = o->len;
        if (compLen < origLen) {
            cscdVecClear(&raw);
            if (!cscdInflateZlibLimit(d + off, compLen, &raw, origLen) || raw.len != origLen) { cscdVecFree(&raw); return; }
            cscdVecAppend(o, raw.items, raw.len);
        } else cscdVecAppend(o, d + off, origLen);
        while (o->len % 4) cscdVecPush(o, 0);
        put32At(o, 12 + 16 * t, tg); put32At(o, 16 + 16 * t, sum); put32At(o, 20 + 16 * t, (uint32_t)at); put32At(o, 24 + 16 * t, origLen);
    }
    cscdVecFree(&raw);
    *ok = true;
}

static bool glyphOutline(const CscdFontFace* face, uint16_t glyph, CscdPath* out, float ma, float mb, float mc, float md, float me, float mf, int depth);

bool cscdFontFaceOutline(const CscdFontFace* f, uint16_t glyph, CscdPath* out) {
    if (f->impl->cff) return cffOutline(f, glyph, out);
    if (f->coords && f->impl->gvar) return cscdFontVarOutline(f, glyph, out);
    return glyphOutline(f, glyph, out, 1, 0, 0, 1, 0, 0, 0);
}

typedef struct Xform { float a, b, c, d, e, f; } Xform;
static CscdPoint tx(const Xform* m, float x, float y) { return cscdPoint(m->a * x + m->c * y + m->e, m->b * x + m->d * y + m->f); }
static CscdPoint midPt(CscdPoint a, CscdPoint b) { return cscdPoint((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f); }

/* The points of a simple glyph: coordinates (font units), flags (bit 0: on the curve) and the
 * index of each contour's last point. False for a composite or broken glyph. */
bool cscdGlyfPoints(const uint8_t* g, uint32_t len, CscdGlyfPoints* out) {
    memset(out, 0, sizeof *out);
    const uint8_t* end = g + len;
    if (len < 10) return false;
    const int16_t nContours = s16(g);
    if (nContours < 0) return false;
    const uint8_t* p = g + 10;
    if (p + 2 * (size_t)nContours + 2 > end) return false;
    out->nContours = nContours;
    out->ends = (uint16_t*)cscdMalloc(sizeof(uint16_t) * (nContours ? nContours : 1));
    for (int i = 0; i < nContours; ++i) out->ends[i] = u16(p + 2 * i);
    p += 2 * nContours;
    const uint16_t insLen = u16(p);
    p += 2 + insLen;
    const int nPts = nContours ? out->ends[nContours - 1] + 1 : 0;
    out->n = nPts;
    out->flags = (uint8_t*)cscdCalloc((size_t)(nPts ? nPts : 1), 1);
    out->pts = (CscdPoint*)cscdCalloc((size_t)(nPts ? nPts : 1), sizeof(CscdPoint));
    for (int i = 0; i < nContours; ++i)
        if (out->ends[i] >= nPts || (i && out->ends[i] < out->ends[i - 1])) goto fail;
    for (int i = 0; i < nPts && p < end;) {
        const uint8_t f = *p++;
        out->flags[i++] = f;
        if (f & 8) {
            uint8_t r = p < end ? *p++ : 0;
            while (r-- && i < nPts) out->flags[i++] = f;
        }
    }
    int v = 0;
    for (int i = 0; i < nPts; ++i) {
        const uint8_t f = out->flags[i];
        if (f & 2) { if (p >= end) goto fail; v += (f & 16) ? *p : -(int)*p; ++p; }
        else if (!(f & 16)) { if (p + 2 > end) goto fail; v += s16(p); p += 2; }
        out->pts[i].x = (float)v;
    }
    v = 0;
    for (int i = 0; i < nPts; ++i) {
        const uint8_t f = out->flags[i];
        if (f & 4) { if (p >= end) goto fail; v += (f & 32) ? *p : -(int)*p; ++p; }
        else if (!(f & 32)) { if (p + 2 > end) goto fail; v += s16(p); p += 2; }
        out->pts[i].y = (float)v;
    }
    return true;
fail:
    cscdGlyfPointsFree(out);
    return false;
}

void cscdGlyfPointsFree(CscdGlyfPoints* p) {
    cscdFree(p->pts);
    cscdFree(p->flags);
    cscdFree(p->ends);
    memset(p, 0, sizeof *p);
}

/* The contours of a simple glyph's points (quadratic curves between on-curve points, implied
 * on-curve points between two off-curve ones), through m (a, b, c, d, e, f). */
void cscdGlyfEmit(const CscdGlyfPoints* gp, const float* m, CscdPath* out) {
    const Xform X = {m[0], m[1], m[2], m[3], m[4], m[5]};
    const CscdPoint* pts = gp->pts;
    const uint8_t* flags = gp->flags;
    int start = 0;
    for (int c = 0; c < gp->nContours; ++c) {
        int last = gp->ends[c];
        int n = last - start + 1;
        if (n <= 1) { start = last + 1; continue; }
#define ON(k) ((flags[start + ((k) % n)] & 1) != 0)
#define PT(k) (pts[start + ((k) % n)])
        /* find a starting on-curve point */
        int first = -1;
        for (int k = 0; k < n; ++k) if (ON(k)) { first = k; break; }
        CscdPoint startPt;
        if (first < 0) { /* all off-curve: start at the midpoint of the first two */
            startPt = midPt(PT(0), PT(1));
            CscdPoint s = tx(&X, startPt.x, startPt.y);
            cscdPathMoveTo(out, s.x, s.y);
            for (int k = 1; k <= n; ++k) {
                CscdPoint c0 = PT(k), nx = PT(k + 1);
                CscdPoint mid = midPt(c0, nx);
                CscdPoint a = tx(&X, c0.x, c0.y), b = tx(&X, mid.x, mid.y);
                cscdPathQuadTo(out, a.x, a.y, b.x, b.y);
            }
            cscdPathClose(out);
            start = last + 1;
            continue;
        }
        startPt = PT(first);
        CscdPoint s = tx(&X, startPt.x, startPt.y);
        cscdPathMoveTo(out, s.x, s.y);
        bool havePending = false;
        CscdPoint pending = {0, 0};
        for (int k = 1; k <= n; ++k) {
            int idx = first + k;
            CscdPoint q = PT(idx);
            if (ON(idx)) {
                CscdPoint d = tx(&X, q.x, q.y);
                if (havePending) { CscdPoint cc = tx(&X, pending.x, pending.y); cscdPathQuadTo(out, cc.x, cc.y, d.x, d.y); havePending = false; }
                else cscdPathLineTo(out, d.x, d.y);
            } else {
                if (havePending) {
                    CscdPoint mid = midPt(pending, q);
                    CscdPoint cc = tx(&X, pending.x, pending.y), d = tx(&X, mid.x, mid.y);
                    cscdPathQuadTo(out, cc.x, cc.y, d.x, d.y);
                }
                pending = q;
                havePending = true;
            }
        }
        if (havePending) {
            CscdPoint cc = tx(&X, pending.x, pending.y), d = tx(&X, startPt.x, startPt.y);
            cscdPathQuadTo(out, cc.x, cc.y, d.x, d.y);
        }
        cscdPathClose(out);
        start = last + 1;
#undef ON
#undef PT
    }
}

const uint8_t* cscdGlyfData(const CscdFontFace* face, uint16_t glyph, uint32_t* len) {
    const uint32_t off = glyphOffset(face, glyph, len);
    return *len ? face->impl->base + off : NULL;
}

static bool glyphOutline(const CscdFontFace* face, uint16_t glyph, CscdPath* out, float ma, float mb, float mc, float md, float me, float mf, int depth) {
    if (depth > 8 || face->impl->cff) return false;
    uint32_t len;
    uint32_t off = glyphOffset(face, glyph, &len);
    if (len < 10) return false;
    const uint8_t* g = face->impl->base + off;
    const uint8_t* end = g + len;
    int16_t nContours = s16(g);
    if (nContours >= 0) {
        CscdGlyfPoints gp;
        if (!cscdGlyfPoints(g, len, &gp)) return false;
        const float m[6] = {ma, mb, mc, md, me, mf};
        cscdGlyfEmit(&gp, m, out);
        cscdGlyfPointsFree(&gp);
        return true;
    }
    /* composite glyph */
    const uint8_t* p = g + 10;
    bool any = false;
    for (;;) {
        if (p + 4 > end) break;
        uint16_t flags = u16(p), comp = u16(p + 2);
        p += 4;
        const size_t rest = ((flags & 1) ? 4 : 2) + ((flags & 8) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0);
        if ((size_t)(end - p) < rest) break;
        float dx, dy;
        if (flags & 1) { dx = s16(p); dy = s16(p + 2); p += 4; }
        else { dx = (int8_t)p[0]; dy = (int8_t)p[1]; p += 2; }
        float a = 1, b = 0, c = 0, d = 1;
        if (flags & 8) { a = d = s16(p) / 16384.f; p += 2; }
        else if (flags & 0x40) { a = s16(p) / 16384.f; d = s16(p + 2) / 16384.f; p += 4; }
        else if (flags & 0x80) { a = s16(p) / 16384.f; b = s16(p + 2) / 16384.f; c = s16(p + 4) / 16384.f; d = s16(p + 6) / 16384.f; p += 8; }
        if (!(flags & 2)) dx = dy = 0; /* point matching is not supported */
        /* combined = M * [a c dx; b d dy] */
        float na = ma * a + mc * b, nb = mb * a + md * b;
        float nc = ma * c + mc * d, nd = mb * c + md * d;
        float ne = ma * dx + mc * dy + me, nf = mb * dx + md * dy + mf;
        any |= glyphOutline(face, comp, out, na, nb, nc, nd, ne, nf, depth + 1);
        if (!(flags & 0x20)) break;
    }
    return any;
}

/* ---- Font ---------------------------------------------------------------------------------------- */

float cscdFontAdvance(const CscdFont* f, uint32_t cp) {
    if (!f->face) return f->size * 0.5f;
    uint16_t g = cscdFontFaceGlyphIndex(f->face, cp);
    if (g) return cscdFontFaceAdvance(f->face, g) * cscdFontScale(f);
    CscdFontFace* fb = cscdFontFallback(cp, 400, false, f->face);
    if (fb) return cscdFontFaceAdvance(fb, cscdFontFaceGlyphIndex(fb, cp)) * f->size / fb->unitsPerEm;
    return cscdFontFaceAdvance(f->face, 0) * cscdFontScale(f);
}

/* ---- the font manager ---------------------------------------------------------------------------- */

typedef struct FontFile { CscdStr path; uint64_t size, stamp; } FontFile;
typedef CSCD_VEC(FontFile) FontFileVec;

typedef struct Entry {
    CscdStr path;
    int index;
    CscdStr family; /* lowercase */
    int weight, weightMax;
    bool italic;
    CscdFontFace* face;
    CscdBytes* data; /* in-memory fonts (shared) */
} Entry;
typedef CSCD_VEC(Entry) EntryVec;

typedef struct FileData { void* owner; const uint8_t* bytes; size_t size; } FileData;

static struct {
    EntryVec entries;
    CscdStrMap loaded;   /* path#index -> CscdFontFace* (NULL: it does not load) */
    CscdStrMap files;    /* path -> FileData* */
    CscdStrMap generic;  /* family -> CscdStr* */
    CscdStrVec fallbackFamilies;
    CscdU64Map fallbackCache; /* -> CscdFontFace* */
    CscdStrMap matchCache;    /* -> CscdFont* */
    CscdStrVec extraDirs;
    bool scanned;
    volatile uint32_t generation;
    CscdRecMutex mutex;
} M;

static CscdOnce managerOnce = CSCD_ONCE_INIT;

static void addGeneric(const char* a, const char* b) {
    CscdStr* s = CSCD_NEW(CscdStr);
    *s = cscdStrDupC(b);
    cscdStrMapPut(&M.generic, cscdSv(a), NULL)->p = s;
}

static void initManager(void) {
    cscdMutexInit(&M.mutex.m);
    M.generation = 1;
#ifdef _WIN32
    static const char* gen[][2] = {{"serif", "times new roman"}, {"sans-serif", "arial"}, {"monospace", "consolas"},
        {"system-ui", "segoe ui"}, {"-apple-system", "segoe ui"}, {"blinkmacsystemfont", "segoe ui"},
        {"ui-sans-serif", "segoe ui"}, {"ui-serif", "times new roman"}, {"ui-monospace", "consolas"},
        {"ui-rounded", "segoe ui"}, {"cursive", "comic sans ms"}, {"fantasy", "impact"}, {"emoji", "segoe ui emoji"},
        {"math", "cambria math"}, {"helvetica", "arial"}, {"helvetica neue", "arial"}, {"sf pro", "segoe ui"},
        {"sf pro text", "segoe ui"}, {"sf pro display", "segoe ui"}, {"roboto", "segoe ui"}, {"inter", "segoe ui"},
        {"menlo", "consolas"}, {"monaco", "consolas"}, {"sf mono", "consolas"}, {"courier", "courier new"},
        {"times", "times new roman"}, {"noto sans", "segoe ui"}, {"ubuntu", "segoe ui"}, {"cantarell", "segoe ui"},
        {"fira sans", "segoe ui"}, {"oxygen", "segoe ui"}, {"liberation mono", "consolas"}, {"dejavu sans mono", "consolas"},
        {"source code pro", "consolas"}, {"fira code", "consolas"}, {"jetbrains mono", "consolas"}};
    static const char* fallback[] = {"segoe ui", "arial", "segoe ui symbol", "segoe ui emoji", "microsoft yahei", "yu gothic", "malgun gothic",
        "nirmala ui", "ebrima", "gadugi", "leelawadee ui", "segoe ui historic", "cambria math", "mongolian baiti",
        "myanmar text", "javanese text", "simsun", "mingliu", "segoe fluent icons", "segoe mdl2 assets"};
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    /* (iOS: no Times, Apple Chancery or Helvetica's older cuts) */
    static const char* gen[][2] = {{"serif", "times new roman"}, {"sans-serif", "helvetica neue"}, {"monospace", "menlo"},
        {"system-ui", "helvetica neue"}, {"-apple-system", "helvetica neue"}, {"blinkmacsystemfont", "helvetica neue"},
        {"ui-sans-serif", "helvetica neue"}, {"ui-serif", "times new roman"}, {"ui-monospace", "menlo"}, {"ui-rounded", "helvetica neue"},
        {"cursive", "snell roundhand"}, {"fantasy", "papyrus"}, {"emoji", "apple color emoji"}, {"times", "times new roman"},
        {"helvetica", "helvetica neue"}, {"arial", "helvetica neue"}, {"segoe ui", "helvetica neue"}, {"roboto", "helvetica neue"},
        {"consolas", "menlo"}, {"courier", "courier new"}};
    static const char* fallback[] = {"helvetica neue", "apple color emoji", "apple symbols", "pingfang sc", "hiragino sans",
        "apple sd gothic neo", "geeza pro", "kohinoor devanagari", "thonburi"};
#elif defined(__ANDROID__)
    static const char* gen[][2] = {{"serif", "noto serif"}, {"sans-serif", "roboto"}, {"monospace", "droid sans mono"},
        {"system-ui", "roboto"}, {"-apple-system", "roboto"}, {"blinkmacsystemfont", "roboto"}, {"ui-sans-serif", "roboto"},
        {"ui-serif", "noto serif"}, {"ui-monospace", "droid sans mono"}, {"ui-rounded", "roboto"}, {"cursive", "dancing script"},
        {"fantasy", "roboto"}, {"emoji", "noto color emoji"}, {"arial", "roboto"}, {"helvetica", "roboto"},
        {"helvetica neue", "roboto"}, {"segoe ui", "roboto"}, {"sf pro", "roboto"}, {"times", "noto serif"},
        {"times new roman", "noto serif"}, {"georgia", "noto serif"}, {"courier", "cutive mono"}, {"courier new", "cutive mono"},
        {"consolas", "droid sans mono"}, {"menlo", "droid sans mono"}};
    static const char* fallback[] = {"roboto", "noto sans", "noto color emoji", "noto sans cjk jp", "noto sans symbols",
        "noto sans arabic", "noto sans hebrew", "noto sans devanagari", "noto sans thai"};
#elif defined(__APPLE__)
    static const char* gen[][2] = {{"serif", "times"}, {"sans-serif", "helvetica"}, {"monospace", "menlo"}, {"system-ui", "helvetica neue"},
        {"-apple-system", "helvetica neue"}, {"cursive", "apple chancery"}, {"fantasy", "papyrus"}};
    static const char* fallback[] = {"helvetica neue", "helvetica", "apple symbols", "pingfang sc", "hiragino sans", "apple sd gothic neo"};
#else
    static const char* gen[][2] = {{"serif", "dejavu serif"}, {"sans-serif", "dejavu sans"}, {"monospace", "dejavu sans mono"},
        {"system-ui", "dejavu sans"}, {"-apple-system", "dejavu sans"}, {"cursive", "dejavu sans"}, {"fantasy", "dejavu sans"},
        {"arial", "liberation sans"}, {"helvetica", "liberation sans"}, {"times new roman", "liberation serif"},
        {"courier new", "liberation mono"}, {"segoe ui", "dejavu sans"}};
    static const char* fallback[] = {"dejavu sans", "liberation sans", "noto sans", "noto sans cjk sc", "noto color emoji", "freesans"};
#endif
    for (size_t i = 0; i < CSCD_COUNT(gen); ++i) addGeneric(gen[i][0], gen[i][1]);
    for (size_t i = 0; i < CSCD_COUNT(fallback); ++i) cscdVecPush(&M.fallbackFamilies, cscdStrDupC(fallback[i]));
}

static void lockM(void) {
    cscdCallOnce(&managerOnce, initManager);
    cscdRecMutexLock(&M.mutex);
}
static void unlockM(void) { cscdRecMutexUnlock(&M.mutex); }

static bool isFontName(CscdSv name) {
    CscdStr n = cscdLower(name);
    CscdSv s = cscdStrSv(&n);
    bool r = cscdSvEndsC(s, ".ttf") || cscdSvEndsC(s, ".ttc") || cscdSvEndsC(s, ".otf");
    cscdStrFree(&n);
    return r;
}

/* The font files in a folder (and, outside Windows, its subfolders), with their size and time. */
static void listFonts(CscdSv dir, FontFileVec* out, int depth) {
#ifdef _WIN32
    (void)depth;
    WIN32_FIND_DATAW fd;
    CscdStr pattern = cscdStrDup(dir);
    cscdStrAppendC(&pattern, "\\*");
    wchar_t* wp = cscdWidePath(cscdStrSv(&pattern));
    HANDLE h = FindFirstFileW(wp, &fd);
    cscdFree(wp);
    cscdStrFree(&pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    CscdStr name = {0};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        cscdStrClear(&name);
        cscdAppendWide(&name, fd.cFileName);
        if (isFontName(cscdStrSv(&name))) {
            FontFile f;
            f.path = cscdStrDup(dir);
            cscdStrAppendChar(&f.path, '\\');
            cscdStrAppend(&f.path, cscdStrSv(&name));
            f.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            f.stamp = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            cscdVecPush(out, f);
        }
    } while (FindNextFileW(h, &fd));
    cscdStrFree(&name);
    FindClose(h);
#else
    if (depth > 6) return;
    char* d = cscdSvDupC(dir);
    DIR* dp = opendir(d);
    cscdFree(d);
    if (!dp) return;
    struct dirent* de;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        CscdStr full = cscdStrDup(dir);
        cscdStrAppendChar(&full, '/');
        cscdStrAppendC(&full, de->d_name);
        struct stat st;
        if (stat(cscdStrC(&full), &st) != 0) { cscdStrFree(&full); continue; }
        if (S_ISDIR(st.st_mode)) {
            listFonts(cscdStrSv(&full), out, depth + 1);
            cscdStrFree(&full);
        } else if (isFontName(cscdSv(de->d_name))) {
            FontFile f = {full, (uint64_t)st.st_size, (uint64_t)st.st_mtime};
            cscdVecPush(out, f);
        } else cscdStrFree(&full);
    }
    closedir(dp);
#endif
}

static void fontFilesFree(FontFileVec* v) {
    for (size_t i = 0; i < v->len; ++i) cscdStrFree(&v->items[i].path);
    cscdVecFree(v);
}

/* Where the index of the system fonts is kept between runs (empty: nowhere). */
static CscdStr fontIndexPath(bool create) {
    const char* off = getenv("CASCADE_NO_FONT_CACHE");
    CscdStr none = {0};
    if (off && *off) return none;
#ifdef _WIN32
    CscdStr dir = cscdEnv("LOCALAPPDATA");
    if (!dir.len) return dir;
    cscdStrAppendC(&dir, "\\cascade");
    if (create) {
        wchar_t* w = cscdWidePath(cscdStrSv(&dir));
        CreateDirectoryW(w, NULL);
        cscdFree(w);
    }
    cscdStrAppendC(&dir, "\\fonts.idx");
    return dir;
#else
    CscdStr dir = {0};
#ifdef __APPLE__
    const char* home = getenv("HOME");
    if (home) { cscdStrSetC(&dir, home); cscdStrAppendC(&dir, "/Library/Caches"); }
#else
    const char* x = getenv("XDG_CACHE_HOME");
    const char* home = getenv("HOME");
    if (x && *x) cscdStrSetC(&dir, x);
    else if (home) {
        cscdStrSetC(&dir, home);
        cscdStrAppendC(&dir, "/.cache");
        if (create) mkdir(cscdStrC(&dir), 0755);
    }
#endif
    if (!dir.len) return none;
    cscdStrAppendC(&dir, "/cascade");
    if (create) mkdir(cscdStrC(&dir), 0755);
    cscdStrAppendC(&dir, "/fonts.idx");
    return dir;
#endif
}

static const char* kIndexHeader = "cascade-font-index 2"; /* (2: CFF fonts are listed too) */

static void entryFree(Entry* e) {
    cscdStrFree(&e->path);
    cscdStrFree(&e->family);
    cscdRelease(e->data);
}

static Entry entryCopy(const Entry* e) {
    Entry c = *e;
    c.path = cscdStrCopy(&e->path);
    c.family = cscdStrCopy(&e->family);
    c.data = (CscdBytes*)cscdRetain(e->data);
    return c;
}

static bool readAt(FILE* f, uint32_t off, size_t n, CscdBytes* buf) {
    cscdVecClear(buf);
    cscdVecResize(buf, n);
    if (fseek(f, (long)off, SEEK_SET) != 0) return false;
    return fread(buf->items, 1, n, f) == n;
}

static void scanFile(CscdSv path, EntryVec* out) {
    FILE* f = cscdFileOpen(path, "rb");
    if (!f) return;
    CscdBytes hdr = {0}, offs = {0}, dir = {0}, nameT = {0}, os2 = {0};
    CscdU32Vec faceOffsets = {0};
    CscdStr fam = {0}, sub = {0}, tf = {0}, ts = {0};
    if (!readAt(f, 0, 12, &hdr)) goto done;
    if (u32(hdr.items) == tag("ttcf")) {
        uint32_t n = u32(hdr.items + 8);
        if (n > 256 || !readAt(f, 12, 4 * n, &offs)) goto done;
        for (uint32_t i = 0; i < n; ++i) cscdVecPush(&faceOffsets, u32(offs.items + 4 * i));
    } else cscdVecPush(&faceOffsets, 0);
    for (size_t fi = 0; fi < faceOffsets.len; ++fi) {
        if (!readAt(f, faceOffsets.items[fi], 12, &dir)) continue;
        uint16_t nt = u16(dir.items + 4);
        if (nt > 128 || !readAt(f, faceOffsets.items[fi], 12 + 16 * (size_t)nt, &dir)) continue;
        uint32_t nameOff = 0, nameLen = 0, os2Off = 0, os2Len = 0;
        bool glyf = false; /* (TrueType or CFF outlines: what can be drawn) */
        for (uint16_t i = 0; i < nt; ++i) {
            const uint8_t* r = dir.items + 12 + 16 * i;
            uint32_t t = u32(r);
            if (t == tag("name")) { nameOff = u32(r + 8); nameLen = u32(r + 12); }
            if (t == tag("OS/2")) { os2Off = u32(r + 8); os2Len = u32(r + 12); }
            if (t == tag("glyf") || t == tag("CFF ")) glyf = true;
        }
        if (!glyf || !nameOff || nameLen > (1u << 20)) continue; /* (CFF2, bitmap-only fonts: not drawn) */
        if (!readAt(f, nameOff, nameLen, &nameT)) continue;
        cscdStrClear(&fam); cscdStrClear(&sub); cscdStrClear(&tf); cscdStrClear(&ts);
        readNames(nameT.items, nameT.len, &fam, &sub, &tf, &ts);
        Entry e;
        memset(&e, 0, sizeof e);
        e.path = cscdStrDup(path);
        e.index = (int)fi;
        e.family = cscdLower(tf.len ? cscdStrSv(&tf) : cscdStrSv(&fam));
        e.weight = 400;
        e.italic = false;
        if (os2Off && os2Len >= 64 && readAt(f, os2Off, 64, &os2)) {
            e.weight = u16(os2.items + 4);
            uint16_t fsSel = u16(os2.items + 62);
            e.italic = (fsSel & 1) || (fsSel & 0x200);
        }
        CscdStr ls = cscdLower(ts.len ? cscdStrSv(&ts) : cscdStrSv(&sub));
        if (cscdSvContains(cscdStrSv(&ls), "italic") || cscdSvContains(cscdStrSv(&ls), "oblique")) e.italic = true;
        if (e.weight < 100 || e.weight > 1000) e.weight = cscdSvContains(cscdStrSv(&ls), "bold") ? 700 : 400;
        cscdStrFree(&ls);
        e.weightMax = e.weight;
        cscdVecPush(out, e);
        /* also register the legacy family name ("Segoe UI Semibold") so it can be asked for directly */
        CscdStr lf = cscdLower(cscdStrSv(&fam));
        if (tf.len && !cscdStrEq(&lf, &e.family)) {
            Entry e2 = entryCopy(&e);
            cscdStrFree(&e2.family);
            e2.family = lf;
            cscdVecPush(out, e2);
        } else cscdStrFree(&lf);
    }
done:
    fclose(f);
    cscdVecFree(&hdr); cscdVecFree(&offs); cscdVecFree(&dir); cscdVecFree(&nameT); cscdVecFree(&os2);
    cscdVecFree(&faceOffsets);
    cscdStrFree(&fam); cscdStrFree(&sub); cscdStrFree(&tf); cscdStrFree(&ts);
}

typedef struct Known { uint64_t size, stamp; EntryVec entries; } Known;

/* Reads a decimal number at the start of *line (after spaces), and the space after it. */
static bool number(CscdSv* line, long long* out) {
    while (line->len && line->ptr[0] == ' ') *line = cscdSvFrom(*line, 1);
    size_t n = 0;
    bool neg = line->len && line->ptr[0] == '-';
    size_t i = neg ? 1 : 0;
    unsigned long long v = 0;
    while (i < line->len && cscdIsDigit(line->ptr[i])) { v = v * 10 + (unsigned)(line->ptr[i] - '0'); ++i; ++n; }
    if (!n) return false;
    *out = neg ? -(long long)v : (long long)v;
    *line = cscdSvFrom(*line, i);
    if (line->len && line->ptr[0] == ' ') *line = cscdSvFrom(*line, 1);
    return true;
}

typedef struct ScanWork {
    const FontFileVec* files;
    EntryVec* found;
    const CscdSizeVec* todo;
} ScanWork;

/* (a cscdParallelFor job: the k-th file to read) */
static void scanOne(void* arg, int k) {
    ScanWork* w = (ScanWork*)arg;
    const size_t i = w->todo->items[k];
    scanFile(cscdStrSv(&w->files->items[i].path), &w->found[i]);
}

/* Adds the faces of the files to the entries. Opening hundreds of font files takes a while, so
 * what they hold is kept in an index file between runs (cached), and a file is read again only
 * when its size or time changed; the files that have to be read are read on several threads. */
static void indexFiles(const FontFileVec* files, bool cached) {
    CscdStrMap known = {0}; /* path -> Known* */
    CscdStr indexPath = cached ? fontIndexPath(false) : (CscdStr){0};
    if (indexPath.len) {
        CscdStr text = {0};
        if (cscdReadFile(cscdStrSv(&indexPath), &text) && cscdSvStartsC(cscdStrSv(&text), kIndexHeader)) {
            Known* cur = NULL;
            /* "F size stamp path" and, after it, "E index weight weightMax italic family" per face */
            CscdSv t = cscdStrSv(&text);
            size_t i = cscdSvFindChar(t, '\n', 0);
            while (i != CSCD_NPOS && i + 1 < t.len) {
                const size_t end = cscdSvFindChar(t, '\n', i + 1);
                CscdSv line = cscdSvSub(t, i + 1, (end == CSCD_NPOS ? t.len : end) - i - 1);
                i = end;
                if (line.len < 2) continue;
                const char kind = line.ptr[0];
                line = cscdSvFrom(line, 1);
                if (kind == 'F') {
                    long long a = 0, b = 0;
                    if (!number(&line, &a) || !number(&line, &b)) { cur = NULL; continue; }
                    bool inserted = false;
                    CscdMapVal* v = cscdStrMapPut(&known, line, &inserted);
                    if (inserted) v->p = CSCD_NEW(Known);
                    cur = (Known*)v->p;
                    for (size_t q = 0; q < cur->entries.len; ++q) entryFree(&cur->entries.items[q]);
                    cur->entries.len = 0;
                    cur->size = (uint64_t)a;
                    cur->stamp = (uint64_t)b;
                } else if (kind == 'E' && cur) {
                    Entry e;
                    memset(&e, 0, sizeof e);
                    long long idx, w, wm, it;
                    if (!number(&line, &idx) || !number(&line, &w) || !number(&line, &wm) || !number(&line, &it)) continue;
                    e.index = (int)idx;
                    e.weight = (int)w;
                    e.weightMax = (int)wm;
                    e.family = cscdStrDup(line);
                    e.italic = it != 0;
                    cscdVecPush(&cur->entries, e);
                }
            }
        }
        cscdStrFree(&text);
    }
    /* what the index does not know (or knows from before the file changed) is read */
    EntryVec* found = (EntryVec*)cscdCalloc(files->len ? files->len : 1, sizeof(EntryVec));
    CscdSizeVec todo = {0};
    for (size_t i = 0; i < files->len; ++i) {
        CscdMapVal* v = cscdStrMapGet(&known, cscdStrSv(&files->items[i].path));
        Known* k = v ? (Known*)v->p : NULL;
        if (k && k->size == files->items[i].size && k->stamp == files->items[i].stamp) {
            found[i] = k->entries;
            memset(&k->entries, 0, sizeof k->entries);
            for (size_t q = 0; q < found[i].len; ++q) cscdStrSet(&found[i].items[q].path, cscdStrSv(&files->items[i].path));
        } else cscdVecPush(&todo, i);
    }
    if (todo.len) {
        ScanWork work = {files, found, &todo};
        /* a thread for every 16 files or so, at most 8 */
        const int threads = (int)cscdMinz(cscdMinz(cscdHardwareThreads(), 8), (todo.len + 15) / 16);
        cscdParallelFor((int)todo.len, threads, scanOne, &work);
    }
    /* the index is written again when something changed (fonts installed or removed) */
    if (indexPath.len && (todo.len || known.len != files->len)) {
        CscdStr out = cscdStrDupC(kIndexHeader);
        cscdStrAppendChar(&out, '\n');
        for (size_t i = 0; i < files->len; ++i) {
            if (cscdSvFindChar(cscdStrSv(&files->items[i].path), '\n', 0) != CSCD_NPOS) continue;
            cscdStrAppendf(&out, "F %llu %llu ", (unsigned long long)files->items[i].size, (unsigned long long)files->items[i].stamp);
            cscdStrAppend(&out, cscdStrSv(&files->items[i].path));
            cscdStrAppendChar(&out, '\n');
            for (size_t q = 0; q < found[i].len; ++q) {
                const Entry* e = &found[i].items[q];
                if (cscdSvFindChar(cscdStrSv(&e->family), '\n', 0) != CSCD_NPOS) continue;
                cscdStrAppendf(&out, "E %d %d %d %d ", e->index, e->weight, e->weightMax, e->italic ? 1 : 0);
                cscdStrAppend(&out, cscdStrSv(&e->family));
                cscdStrAppendChar(&out, '\n');
            }
        }
        /* written beside it, then moved over it: another program reading it sees one or the other */
        CscdStr made = fontIndexPath(true); /* (its folder made) */
        cscdStrFree(&made);
#ifdef _WIN32
        CscdStr tmp = cscdStrf("%s.%lu", cscdStrC(&indexPath), (unsigned long)GetCurrentProcessId());
#else
        CscdStr tmp = cscdStrf("%s.%ld", cscdStrC(&indexPath), (long)getpid());
#endif
        FILE* f = cscdFileOpen(cscdStrSv(&tmp), "wb");
        if (f) {
            const bool ok = fwrite(out.ptr, 1, out.len, f) == out.len;
            const bool closed = fclose(f) == 0;
#ifdef _WIN32
            wchar_t *wt = cscdWidePath(cscdStrSv(&tmp)), *wi = cscdWidePath(cscdStrSv(&indexPath));
            if (!(closed && ok && MoveFileExW(wt, wi, MOVEFILE_REPLACE_EXISTING))) DeleteFileW(wt);
            cscdFree(wt);
            cscdFree(wi);
#else
            if (!(closed && ok && rename(cscdStrC(&tmp), cscdStrC(&indexPath)) == 0)) remove(cscdStrC(&tmp));
#endif
        }
        cscdStrFree(&tmp);
        cscdStrFree(&out);
    }
    for (size_t i = 0; i < files->len; ++i) {
        for (size_t q = 0; q < found[i].len; ++q) cscdVecPush(&M.entries, found[i].items[q]);
        cscdVecFree(&found[i]);
    }
    cscdFree(found);
    cscdVecFree(&todo);
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&known, s));) {
        Known* k = (Known*)s->val.p;
        for (size_t q = 0; q < k->entries.len; ++q) entryFree(&k->entries.items[q]);
        cscdVecFree(&k->entries);
        cscdFree(k);
    }
    cscdStrMapFree(&known);
    cscdStrFree(&indexPath);
}

static void scanSystem(void) {
    if (M.scanned) return;
    M.scanned = true;
    CscdStrVec dirs = {0};
    cscdStrVecCopy(&dirs, &M.extraDirs);
#ifdef _WIN32
    wchar_t win[MAX_PATH];
    const UINT wn = GetWindowsDirectoryW(win, MAX_PATH);
    if (wn && wn < MAX_PATH) { CscdStr d = {0}; cscdAppendWide(&d, win); cscdStrAppendC(&d, "\\Fonts"); cscdVecPush(&dirs, d); }
    CscdStr la = cscdEnv("LOCALAPPDATA");
    if (la.len) { cscdStrAppendC(&la, "\\Microsoft\\Windows\\Fonts"); cscdVecPush(&dirs, la); }
    else cscdStrFree(&la);
#elif defined(__ANDROID__)
    cscdVecPush(&dirs, cscdStrDupC("/system/fonts"));
    cscdVecPush(&dirs, cscdStrDupC("/product/fonts"));
#else
    cscdVecPush(&dirs, cscdStrDupC("/usr/share/fonts"));
    cscdVecPush(&dirs, cscdStrDupC("/usr/local/share/fonts"));
    cscdVecPush(&dirs, cscdStrDupC("/Library/Fonts"));
    cscdVecPush(&dirs, cscdStrDupC("/System/Library/Fonts"));
    const char* home = getenv("HOME");
    if (home) {
        CscdStr a = cscdStrDupC(home); cscdStrAppendC(&a, "/.fonts"); cscdVecPush(&dirs, a);
        CscdStr b = cscdStrDupC(home); cscdStrAppendC(&b, "/.local/share/fonts"); cscdVecPush(&dirs, b);
    }
#endif
    FontFileVec files = {0};
    for (size_t i = 0; i < dirs.len; ++i) listFonts(cscdStrSv(&dirs.items[i]), &files, 0);
    indexFiles(&files, true);
    fontFilesFree(&files);
    cscdStrVecFree(&dirs);
}

static void clearMatchCache(void) {
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&M.matchCache, s));) cscdFree(s->val.p);
    cscdStrMapClear(&M.matchCache);
}

void cscdFontAddDirectory(CscdSv dir) {
    lockM();
    cscdVecPush(&M.extraDirs, cscdStrDup(dir));
    if (M.scanned) {
        FontFileVec files = {0};
        listFonts(dir, &files, 0);
        indexFiles(&files, false);
        fontFilesFree(&files);
        clearMatchCache();
        cscdAtomicFetchAddU32(&M.generation, 1);
        cscdU64MapClear(&M.fallbackCache);
    }
    unlockM();
}

static void mappingDtor(void* p) {
    FileData* d = (FileData*)p;
#ifdef _WIN32
    UnmapViewOfFile(d->bytes);
#else
    munmap((void*)d->bytes, d->size);
#endif
}

/* A font file mapped into memory: only the parts read (cmap, the glyphs drawn) are loaded, and a
 * large file (a CJK fallback font of 20 MB) costs nothing up front. Read whole when mapping fails.
 * The FileData is the shared owner of the bytes. */
static FileData* mapFont(CscdSv path) {
#ifdef _WIN32
    wchar_t* p = cscdWidePath(path);
    HANDLE file = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size;
        size.QuadPart = 0;
        HANDLE map = GetFileSizeEx(file, &size) && size.QuadPart > 0 ? CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
        const void* view = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : NULL;
        if (map) CloseHandle(map); /* (the view keeps the file) */
        CloseHandle(file);
        if (view) {
            cscdFree(p);
            FileData* d = (FileData*)cscdRcNew(sizeof(FileData), mappingDtor);
            d->bytes = (const uint8_t*)view;
            d->size = (size_t)size.QuadPart;
            d->owner = d;
            return d;
        }
    }
#else
    char* p = cscdSvDupC(path);
    int fd = open(p, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        void* view = fstat(fd, &st) == 0 && st.st_size > 0 ? mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
        close(fd); /* (the mapping keeps the file) */
        if (view != MAP_FAILED) {
            cscdFree(p);
            FileData* d = (FileData*)cscdRcNew(sizeof(FileData), mappingDtor);
            d->bytes = (const uint8_t*)view;
            d->size = (size_t)st.st_size;
            d->owner = d;
            return d;
        }
    }
#endif
    cscdFree(p);
    CscdStr s = {0};
    if (!cscdReadFile(path, &s)) return NULL;
    CscdBytes* bytes = (CscdBytes*)cscdRcNew(sizeof(CscdBytes), bytesDtor);
    bytes->items = (uint8_t*)s.ptr; /* (the bytes read, taken over: not copied) */
    bytes->len = s.len;
    bytes->cap = s.cap;
    FileData* d = (FileData*)cscdRcNew(sizeof(FileData), NULL);
    d->bytes = bytes->items;
    d->size = bytes->len;
    d->owner = bytes; /* (d itself keeps nothing; the face retains bytes) */
    return d;
}

CscdFontFace* cscdFontLoad(CscdSv path, int index) {
    lockM();
    CscdStr key = cscdStrf("%.*s#%d", CSCD_SVF(path), index);
    CscdMapVal* have = cscdStrMapGet(&M.loaded, cscdStrSv(&key));
    if (have) {
        cscdStrFree(&key);
        unlockM();
        return (CscdFontFace*)have->p;
    }
    bool inserted = false;
    CscdMapVal* fv = cscdStrMapPut(&M.files, path, &inserted);
    if (!fv->p) fv->p = mapFont(path);
    FileData* data = (FileData*)fv->p;
    CscdFontFace* face = data ? cscdFontFaceLoad(data->owner, data->bytes, data->size, index) : NULL;
    cscdStrMapPut(&M.loaded, cscdStrSv(&key), NULL)->p = face;
    cscdStrFree(&key);
    unlockM();
    return face;
}

static CscdFontFace* faceFor(Entry* e) {
    if (e->face) return e->face;
    if (e->data) {
        CscdFontFace* face = cscdFontFaceLoad(e->data, e->data->items, e->data->len, e->index);
        if (!face) return NULL;
        e->face = face;
        CscdStr key = cscdStrf("mem:%s%d%s%llu", cscdStrC(&e->family), e->weight, e->italic ? "i" : "", (unsigned long long)(uintptr_t)e->data);
        cscdStrMapPut(&M.loaded, cscdStrSv(&key), NULL)->p = face;
        cscdStrFree(&key);
        return e->face;
    }
    e->face = cscdFontLoad(cscdStrSv(&e->path), e->index);
    return e->face;
}

bool cscdFontAdd(CscdSv family, CscdSv path, int weight, bool italic, int weightMax) {
    lockM();
    CscdFontFace* f = cscdFontLoad(path, 0);
    if (!f) {
        unlockM();
        return false;
    }
    Entry e;
    memset(&e, 0, sizeof e);
    e.path = cscdStrDup(path);
    e.family = cscdLower(family);
    e.weight = weight;
    e.weightMax = weightMax ? weightMax : weight;
    e.italic = italic;
    e.face = f;
    cscdVecInsert(&M.entries, 0, e);
    clearMatchCache();
    cscdAtomicFetchAddU32(&M.generation, 1);
    unlockM();
    return true;
}

bool cscdFontAddData(CscdSv family, const uint8_t* data, size_t size, int weight, bool italic) {
    lockM();
    Entry e;
    memset(&e, 0, sizeof e);
    e.family = cscdLower(family);
    e.weight = e.weightMax = weight;
    e.italic = italic;
    e.data = (CscdBytes*)cscdRcNew(sizeof(CscdBytes), bytesDtor);
    cscdVecAppend(e.data, data, size);
    if (!faceFor(&e)) {
        entryFree(&e);
        unlockM();
        return false;
    }
    cscdVecInsert(&M.entries, 0, e);
    clearMatchCache();
    cscdAtomicFetchAddU32(&M.generation, 1);
    unlockM();
    return true;
}

uint32_t cscdFontGeneration(void) {
    cscdCallOnce(&managerOnce, initManager);
    return cscdAtomicLoadU32(&M.generation);
}

static Entry* pick(CscdSv fam, int weight, bool italic) {
    /* style */
    bool anyStyled = false, anyFam = false;
    for (size_t i = 0; i < M.entries.len; ++i) {
        const Entry* e = &M.entries.items[i];
        if (!cscdSvEq(cscdStrSv(&e->family), fam)) continue;
        anyFam = true;
        if (e->italic == italic) anyStyled = true;
    }
    if (!anyFam) return NULL;
    /* weight (CSS Fonts 4 matching) */
    Entry* best = NULL;
    int bestScore = 1 << 30;
    for (size_t i = 0; i < M.entries.len; ++i) {
        Entry* e = &M.entries.items[i];
        if (!cscdSvEq(cscdStrSv(&e->family), fam)) continue;
        if (anyStyled && e->italic != italic) continue;
        int w = e->weight;
        if (weight >= e->weight && weight <= e->weightMax) { best = e; break; }
        int score;
        if (weight >= 400 && weight <= 500) {
            if (w >= weight && w <= 500) score = w - weight;
            else if (w < weight) score = 1000 + (weight - w);
            else score = 2000 + (w - weight);
        } else if (weight < 400) {
            score = w <= weight ? weight - w : 1000 + (w - weight);
        } else {
            score = w >= weight ? w - weight : 1000 + (weight - w);
        }
        if (score < bestScore) { bestScore = score; best = e; }
    }
    return best;
}

static CscdFont matchLocked(const CscdStrVec* families, float size, int weight, bool italic) {
    lockM();
    CscdStr key = {0};
    for (size_t i = 0; i < families->len; ++i) {
        cscdStrAppend(&key, cscdStrSv(&families->items[i]));
        cscdStrAppendChar(&key, ',');
    }
    cscdStrAppendf(&key, "%d%s", weight, italic ? "i" : "n");
    CscdMapVal* have = cscdStrMapGet(&M.matchCache, cscdStrSv(&key));
    if (have) {
        CscdFont f = *(CscdFont*)have->p;
        f.size = size;
        cscdStrFree(&key);
        unlockM();
        return f;
    }
    scanSystem();
    CscdFont f = cscdFontDefault();
    f.size = size;
    for (size_t i = 0; i < families->len; ++i) {
        CscdStr fam = cscdLower(cscdSvTrim(cscdStrSv(&families->items[i])));
        Entry* e = pick(cscdStrSv(&fam), weight, italic);
        if (!e) {
            CscdMapVal* g = cscdStrMapGet(&M.generic, cscdStrSv(&fam));
            if (g) e = pick(cscdStrSv((CscdStr*)g->p), weight, italic);
            if (!e && cscdStrEqC(&fam, "monospace")) e = pick(CSCD_SV("courier new"), weight, italic);
        }
        cscdStrFree(&fam);
        if (e && faceFor(e)) {
            f.face = e->face;
            f.fakeBold = weight >= 600 && e->weight < 550;
            f.fakeItalic = italic && !e->italic;
            break;
        }
    }
    if (!f.face) {
        for (size_t i = 0; i < M.fallbackFamilies.len; ++i) {
            Entry* e = pick(cscdStrSv(&M.fallbackFamilies.items[i]), weight, italic);
            if (e && faceFor(e)) { f.face = e->face; f.fakeBold = weight >= 600 && e->weight < 550; f.fakeItalic = italic && !e->italic; break; }
        }
    }
    if (!f.face) {
        for (size_t i = 0; i < M.entries.len; ++i)
            if (faceFor(&M.entries.items[i])) { f.face = M.entries.items[i].face; break; }
    }
    CscdFont* stored = CSCD_NEW(CscdFont);
    *stored = f;
    bool inserted = false;
    CscdMapVal* slot = cscdStrMapPut(&M.matchCache, cscdStrSv(&key), &inserted);
    if (!inserted) cscdFree(slot->p);
    slot->p = stored;
    cscdStrFree(&key);
    unlockM();
    return f;
}

/* The last answers on this thread, found without the lock (layout and paint ask for the same few
 * fonts over and over). (Each thread's copies of the family lists stay until the program ends.) */
typedef struct Recent { CscdStrVec families; int weight; bool italic; uint32_t gen; CscdFont font; } Recent;
static CSCD_THREAD_LOCAL Recent recent[8];
static CSCD_THREAD_LOCAL unsigned nextRecent;

CscdFont cscdFontMatch(const CscdStrVec* families, float size, int weight, bool italic) {
    const uint32_t gen = cscdFontGeneration();
    for (int i = 0; i < 8; ++i) {
        const Recent* r = &recent[i];
        if (r->gen == gen && r->weight == weight && r->italic == italic && cscdStrVecEq(&r->families, families)) {
            CscdFont f = r->font;
            f.size = size;
            return f;
        }
    }
    CscdFont found = matchLocked(families, size, weight, italic);
    Recent* r = &recent[nextRecent++ % 8];
    cscdStrVecCopy(&r->families, families);
    r->weight = weight;
    r->italic = italic;
    r->gen = gen;
    r->font = found;
    return found;
}

CscdFontFace* cscdFontFallback(uint32_t cp, int weight, bool italic, const CscdFontFace* exclude) {
    lockM();
    uint64_t key = ((uint64_t)cp << 8) | (weight >= 600 ? 1 : 0) | (italic ? 2 : 0);
    CscdMapVal* have = cscdU64MapGet(&M.fallbackCache, key);
    if (have) {
        CscdFontFace* f = (CscdFontFace*)have->p;
        unlockM();
        return f;
    }
    scanSystem();
    CscdFontFace* found = NULL;
    for (size_t fi = 0; fi < M.fallbackFamilies.len && !found; ++fi) {
        CscdSv fam = cscdStrSv(&M.fallbackFamilies.items[fi]);
        for (size_t i = 0; i < M.entries.len; ++i) {
            Entry* e = &M.entries.items[i];
            if (!cscdSvEq(cscdStrSv(&e->family), fam)) continue;
            if ((e->weight >= 600) != (weight >= 600) || e->italic != italic) continue;
            CscdFontFace* f = faceFor(e);
            if (f && f != exclude && cscdFontFaceGlyphIndex(f, cp)) { found = f; break; }
        }
        if (!found)
            for (size_t i = 0; i < M.entries.len; ++i) {
                Entry* e = &M.entries.items[i];
                if (!cscdSvEq(cscdStrSv(&e->family), fam)) continue;
                CscdFontFace* f = faceFor(e);
                if (f && f != exclude && cscdFontFaceGlyphIndex(f, cp)) { found = f; break; }
            }
    }
    cscdU64MapPut(&M.fallbackCache, key, NULL)->p = found;
    unlockM();
    return found;
}

/* ---- emoji fonts, settings ------------------------------------------------------------------------ */

/* A color font with a glyph for cp: the "emoji" family first, then the fallback families whose
 * name says emoji. */
CscdFontFace* cscdFontFallbackEmoji(uint32_t cp) {
    lockM();
    const uint64_t key = ((uint64_t)cp << 8) | 0x80;
    CscdMapVal* have = cscdU64MapGet(&M.fallbackCache, key);
    if (have) {
        CscdFontFace* f = (CscdFontFace*)have->p;
        unlockM();
        return f;
    }
    scanSystem();
    CscdFontFace* found = NULL;
    CscdMapVal* gen = cscdStrMapGet(&M.generic, CSCD_SV("emoji"));
    const CscdStr* emojiFamily = gen ? (const CscdStr*)gen->p : NULL;
    for (int pass = 0; pass < 2 && !found; ++pass)
        for (size_t i = 0; i < M.entries.len && !found; ++i) {
            Entry* e = &M.entries.items[i];
            const CscdSv fam = cscdStrSv(&e->family);
            if (pass == 0 ? !(emojiFamily && cscdSvEq(fam, cscdStrSv(emojiFamily))) : cscdSvFind(fam, CSCD_SV("emoji"), 0) == CSCD_NPOS) continue;
            CscdFontFace* f = faceFor(e);
            if (f && cscdFontFaceHasColor(f) && cscdFontFaceGlyphIndex(f, cp)) found = f;
        }
    cscdU64MapPut(&M.fallbackCache, key, NULL)->p = found;
    unlockM();
    return found;
}

static CscdStrMap settingsInterned;
static CscdMutex settingsMutex = CSCD_MUTEX_INIT;

const CscdFontSettings* cscdFontSettingsIntern(const CscdFontSettings* s) {
    if (!s) return NULL;
    CscdFontSettings c;
    memset(&c, 0, sizeof c); /* (the key is the bytes: padding and unused entries zeroed) */
    c.featureCount = cscdMini(s->featureCount, (int)CSCD_COUNT(c.features));
    c.variationCount = cscdMini(s->variationCount, (int)CSCD_COUNT(c.variations));
    memcpy(c.features, s->features, sizeof(CscdFontTagValue) * c.featureCount);
    memcpy(c.variations, s->variations, sizeof(CscdFontTagValue) * c.variationCount);
    c.noKerning = s->noKerning;
    c.noOpticalSizing = s->noOpticalSizing;
    c.stretch = s->stretch == 100 ? 0 : s->stretch;
    c.synthesizeSmallCaps = s->synthesizeSmallCaps;
    if (!c.featureCount && !c.variationCount && !c.noKerning && !c.noOpticalSizing && !c.stretch && !c.synthesizeSmallCaps) return NULL;
    const CscdSv key = cscdSvMake((const char*)&c, sizeof c);
    cscdMutexLock(&settingsMutex);
    bool inserted = false;
    CscdMapVal* slot = cscdStrMapPut(&settingsInterned, key, &inserted);
    if (inserted) {
        CscdFontSettings* kept = CSCD_NEW(CscdFontSettings);
        *kept = c;
        slot->p = kept;
    }
    const CscdFontSettings* r = (const CscdFontSettings*)slot->p;
    cscdMutexUnlock(&settingsMutex);
    return r;
}

/* The instance of a variable face for a font's weight, width, size (optical sizing) and the axis
 * values the settings name. */
static CscdFontFace* variedFace(CscdFontFace* face, int weight, float size, const CscdFontSettings* s) {
    if (!face || !cscdFontFaceAxisCount(face)) return face;
    CscdFontTagValue v[12];
    size_t n = 0;
    if (weight > 0) v[n++] = (CscdFontTagValue){cscdFontTag("wght"), (float)weight};
    if (s && s->stretch) v[n++] = (CscdFontTagValue){cscdFontTag("wdth"), s->stretch};
    if (!s || !s->noOpticalSizing) v[n++] = (CscdFontTagValue){cscdFontTag("opsz"), size};
    for (size_t i = 0; s && i < s->variationCount && n < CSCD_COUNT(v); ++i) v[n++] = s->variations[i];
    return cscdFontFaceVary(face, v, n);
}

CscdFont cscdFontWithSettings(CscdFont f, int weight, const CscdFontSettings* s) {
    f.settings = s;
    if (!f.face) return f;
    CscdFontFace* base = f.face->varies ? f.face->varies : f.face;
    uint32_t t;
    float lo, def, hi;
    for (int i = 0; i < cscdFontFaceAxisCount(base); ++i)
        if (cscdFontFaceAxis(base, i, &t, &lo, &def, &hi) && t == cscdFontTag("wght") && weight >= lo && weight <= hi) f.fakeBold = false;
    f.face = variedFace(base, weight, f.size, s);
    return f;
}

/* ---- shaping ---------------------------------------------------------------------------------------- */

#include "unicodeData.inc"

static bool inRanges(const uint32_t (*r)[2], size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (cp < r[mid][0]) hi = mid;
        else if (cp > r[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}

static bool isMark(uint32_t cp) { return cp >= 0x300 && inRanges(kMarks, CSCD_COUNT(kMarks), cp); }
static bool isEmojiPresentation(uint32_t cp) { return cp >= 0x231A && inRanges(kEmojiPresentation, CSCD_COUNT(kEmojiPresentation), cp); }
static bool isPictographic(uint32_t cp) { return cp >= 0xA9 && inRanges(kPictographic, CSCD_COUNT(kPictographic), cp); }
static bool isRegional(uint32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }
static bool isEmojiModifier(uint32_t cp) { return cp >= 0x1F3FB && cp <= 0x1F3FF; }
static bool isIgnorable(uint32_t cp) {
    return cp == 0xAD || cp == 0x34F || cp == 0x61C || (cp >= 0x115F && cp <= 0x1160) || (cp >= 0x17B4 && cp <= 0x17B5) ||
           (cp >= 0x180B && cp <= 0x180F) || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x206F) || cp == 0x3164 || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || cp == 0xFFA0 ||
           (cp >= 0x1BCA0 && cp <= 0x1BCA3) || (cp >= 0x1D173 && cp <= 0x1D17A) || (cp >= 0xE0000 && cp <= 0xE0FFF);
}

/* The OpenType script of a character; 0 for the characters every script shares. */
static uint32_t scriptOf(uint32_t cp) {
    static const struct { uint32_t lo, hi; char t[5]; } kScripts[] = {
        {0x41, 0x5A, "latn"}, {0x61, 0x7A, "latn"}, {0xC0, 0x24F, "latn"}, {0x370, 0x3FF, "grek"}, {0x400, 0x52F, "cyrl"},
        {0x530, 0x58F, "armn"}, {0x590, 0x5FF, "hebr"}, {0x600, 0x6FF, "arab"}, {0x700, 0x74F, "syrc"}, {0x750, 0x77F, "arab"},
        {0x780, 0x7BF, "thaa"}, {0x8A0, 0x8FF, "arab"}, {0x900, 0x97F, "dev2"}, {0x980, 0x9FF, "bng2"}, {0xA00, 0xA7F, "gur2"},
        {0xA80, 0xAFF, "gjr2"}, {0xB00, 0xB7F, "ory2"}, {0xB80, 0xBFF, "tml2"}, {0xC00, 0xC7F, "tel2"}, {0xC80, 0xCFF, "knd2"},
        {0xD00, 0xD7F, "mlm2"}, {0xD80, 0xDFF, "sinh"}, {0xE00, 0xE7F, "thai"}, {0xE80, 0xEFF, "lao "}, {0xF00, 0xFFF, "tibt"},
        {0x1000, 0x109F, "mym2"}, {0x10A0, 0x10FF, "geor"}, {0x1100, 0x11FF, "hang"}, {0x1200, 0x139F, "ethi"},
        {0x1700, 0x171F, "tglg"}, {0x1720, 0x173F, "hano"}, {0x1740, 0x175F, "buhd"}, {0x1760, 0x177F, "tagb"},
        {0x1780, 0x17FF, "khmr"}, {0x1800, 0x18AF, "mong"}, {0x1900, 0x194F, "limb"}, {0x1950, 0x197F, "tale"},
        {0x1980, 0x19DF, "talu"}, {0x1A00, 0x1A1F, "bugi"}, {0x1A20, 0x1AAF, "lana"}, {0x1B00, 0x1B7F, "bali"},
        {0x1B80, 0x1BBF, "sund"}, {0x1BC0, 0x1BFF, "batk"}, {0x1C00, 0x1C4F, "lepc"}, {0x1CC0, 0x1CCF, "sund"},
        {0x1E00, 0x1EFF, "latn"}, {0x1F00, 0x1FFF, "grek"},
        {0x3040, 0x30FF, "kana"}, {0x3130, 0x318F, "hang"}, {0x3400, 0x4DBF, "hani"}, {0x4E00, 0x9FFF, "hani"},
        {0xA800, 0xA82F, "sylo"}, {0xA880, 0xA8DF, "saur"}, {0xA8E0, 0xA8FF, "dev2"}, {0xA900, 0xA92F, "kali"},
        {0xA930, 0xA95F, "rjng"}, {0xA960, 0xA97F, "hang"}, {0xA980, 0xA9DF, "java"}, {0xA9E0, 0xA9FF, "mym2"},
        {0xAA00, 0xAA5F, "cham"}, {0xAA60, 0xAA7F, "mym2"}, {0xAA80, 0xAADF, "tavt"}, {0xAAE0, 0xAAFF, "mtei"},
        {0xABC0, 0xABFF, "mtei"}, {0xAC00, 0xD7AF, "hang"}, {0xD7B0, 0xD7FF, "hang"},
        {0xFB1D, 0xFB4F, "hebr"}, {0xFB50, 0xFDFF, "arab"}, {0xFE70, 0xFEFF, "arab"},
        {0x10A00, 0x10A5F, "khar"}, {0x11000, 0x1107F, "brah"}, {0x11080, 0x110CF, "kthi"}, {0x11100, 0x1114F, "cakm"},
        {0x11150, 0x1117F, "mahj"}, {0x11180, 0x111DF, "shrd"}, {0x111E0, 0x111FF, "sinh"}, {0x11200, 0x1124F, "khoj"},
        {0x11280, 0x112AF, "mult"}, {0x112B0, 0x112FF, "sind"}, {0x11300, 0x1137F, "gran"}, {0x11380, 0x113FF, "tutg"},
        {0x11400, 0x1147F, "newa"}, {0x11480, 0x114DF, "tirh"}, {0x11580, 0x115FF, "sidd"}, {0x11600, 0x1165F, "modi"},
        {0x11680, 0x116CF, "takr"}, {0x11700, 0x1174F, "ahom"}, {0x11800, 0x1184F, "dogr"}, {0x11900, 0x1195F, "diak"},
        {0x119A0, 0x119FF, "nand"}, {0x11A00, 0x11A4F, "zanb"}, {0x11A50, 0x11AAF, "soyo"}, {0x11C00, 0x11C6F, "bhks"},
        {0x11C70, 0x11CBF, "marc"}, {0x11D00, 0x11D5F, "gonm"}, {0x11D60, 0x11DAF, "gong"}, {0x11EE0, 0x11EFF, "maka"},
        {0x11F00, 0x11F5F, "kawi"}, {0x20000, 0x2FA1F, "hani"},
    };
    if (cp < 0x41) return 0;
    size_t lo = 0, hi = CSCD_COUNT(kScripts);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (cp < kScripts[mid].lo) hi = mid;
        else if (cp > kScripts[mid].hi) lo = mid + 1;
        else return cscdFontTag(kScripts[mid].t);
    }
    return 0;
}

typedef struct ShapeItem {
    uint32_t cp, cluster;
    uint16_t glyph;
    CscdFontFace* face;
    bool invisible;
    uint32_t script;
    bool small; /* a synthesized small capital */
} ShapeItem;

/* Small capitals made from capitals (browsers draw them at 70%), for fonts without smcp. */
#define SMALL_CAPS_SCALE 0.7f
typedef CSCD_VEC(ShapeItem) ShapeItemVec;
typedef CSCD_VEC(CscdOtFeature) OtFeatureVec;

static CSCD_THREAD_LOCAL ShapeItemVec shapeItems;
static CSCD_THREAD_LOCAL CscdU32Vec shapeInCps, shapeInClusters;
static CSCD_THREAD_LOCAL CscdOtBuffer shapeBuf;
static CSCD_THREAD_LOCAL CscdU32Vec shapeOut;
static CSCD_THREAD_LOCAL OtFeatureVec shapeFeatures;
static CSCD_THREAD_LOCAL CscdU32Vec shapeCps;

/* How many characters from i make one emoji (0: none starts there): a pictograph and what extends
 * it (variation selectors, skin tones, the keycap, tags, more pictographs after ZWJ), a pair of
 * regional indicators, a keycap. */
static size_t emojiLength(const uint32_t* cps, size_t i, size_t n) {
    const uint32_t c = cps[i];
    const bool vs16 = i + 1 < n && cps[i + 1] == 0xFE0F, vs15 = i + 1 < n && cps[i + 1] == 0xFE0E;
    if (isRegional(c)) return i + 1 < n && isRegional(cps[i + 1]) ? 2 : 1;
    if ((c == '#' || c == '*' || (c >= '0' && c <= '9'))) {
        if (vs16 && i + 2 < n && cps[i + 2] == 0x20E3) return 3;
        if (i + 1 < n && cps[i + 1] == 0x20E3) return 2;
        return 0;
    }
    if (vs15 || !(isEmojiPresentation(c) || (isPictographic(c) && (vs16 || (i + 1 < n && isEmojiModifier(cps[i + 1])))))) return 0;
    size_t k = i + 1;
    for (;;) {
        while (k < n && (cps[k] == 0xFE0F || isEmojiModifier(cps[k]) || cps[k] == 0x20E3 || (cps[k] >= 0xE0020 && cps[k] <= 0xE007F))) ++k;
        if (k + 1 < n && cps[k] == 0x200D && isPictographic(cps[k + 1])) { k += 2; continue; }
        break;
    }
    return k - i;
}

static void addFeature(OtFeatureVec* v, const char* t, int value) { cscdVecPush(v, ((CscdOtFeature){cscdFontTag(t), value, ~0u})); }

/* The features for a run: the defaults browsers apply, what CSS asks for, letter spacing turning off
 * the optional ligatures. */
static void runFeatures(const CscdFont* font, float letterSpacing, bool tnum, OtFeatureVec* out) {
    out->len = 0;
    static const char* kDefaults[] = {"ccmp", "locl", "rlig", "rclt", "mark", "mkmk", "abvm", "blwm", "dist", "curs"};
    for (size_t i = 0; i < CSCD_COUNT(kDefaults); ++i) addFeature(out, kDefaults[i], 1);
    const bool ligatures = !font->noLigatures && letterSpacing == 0;
    addFeature(out, "liga", ligatures);
    addFeature(out, "clig", ligatures);
    addFeature(out, "calt", ligatures);
    addFeature(out, "kern", !(font->settings && font->settings->noKerning));
    if (tnum) addFeature(out, "tnum", 1);
    const CscdFontSettings* s = font->settings;
    for (size_t i = 0; s && i < s->featureCount; ++i) {
        const int value = (int)s->features[i].value;
        const bool optionalLigature = s->features[i].tag == cscdFontTag("dlig") || s->features[i].tag == cscdFontTag("hlig");
        cscdVecPush(out, ((CscdOtFeature){s->features[i].tag, optionalLigature && letterSpacing != 0 ? 0 : value, ~0u}));
    }
}

/* One run of glyphs of one face and script: substituted, positioned, appended to out. */
/* (beside each glyph shaped: the pen where it was put out, and for a mark the glyph it is on) */
static CSCD_THREAD_LOCAL CscdFloatVec shapePens;
static CSCD_THREAD_LOCAL CscdIntVec shapeBases;

static void emit(CscdShapedGlyphVec* out, CscdShapedGlyph g, float pen, int base) {
    cscdVecPush(out, g);
    cscdVecPush(&shapePens, pen);
    cscdVecPush(&shapeBases, base);
}

/* when a script's marks lose their own advances (as HarfBuzz's shapers do it) */
enum { ZERO_NONE, ZERO_EARLY, ZERO_LATE };
static int markZeroing(uint32_t script) {
    static const char* const kNone[] = {"dev2", "bng2", "gur2", "gjr2", "ory2", "tml2", "tel2", "knd2", "mlm2", "khmr", "hang"};
    static const char* const kEarly[] = {"mym2", "mymr"};
    for (size_t i = 0; i < CSCD_COUNT(kNone); ++i) if (script == cscdFontTag(kNone[i])) return ZERO_NONE;
    for (size_t i = 0; i < CSCD_COUNT(kEarly); ++i) if (script == cscdFontTag(kEarly[i])) return ZERO_EARLY;
    if (cscdUseScript(script)) return ZERO_EARLY;
    return ZERO_LATE;
}

static void shapeRun(const ShapeItem* items, size_t from, size_t to, const CscdFont* font, float letterSpacing, float wordSpacing,
                     const OtFeatureVec* feats, bool handTabular, bool rtl, float* pen, CscdShapedGlyphVec* out) {
    CscdFontFace* face = items[from].face;
    const float sc = font->size / face->unitsPerEm;
    CscdOtBuffer* buf = &shapeBuf;
    buf->len = 0;
    uint32_t script = 0;
    for (size_t i = from; i < to; ++i) {
        if (!script) script = items[i].script;
        if (items[i].invisible) continue;
        CscdOtGlyph g = {0};
        g.glyph = items[i].glyph;
        g.cluster = (uint32_t)(i - from);
        g.ch = items[i].cp;
        g.mask = ~0u;
        g.ignorable = isIgnorable(items[i].cp) && items[i].cp != 0x200C && items[i].cp != 0x200D;
        g.xAdvance = (int32_t)lroundf(cscdFontFaceAdvance(face, g.glyph));
        cscdVecPush(buf, g);
    }
    if (!script) script = cscdFontTag("latn");
    CscdU32Vec* cps = &shapeCps;
    cps->len = 0;
    for (size_t i = from; i < to; ++i) cscdVecPush(cps, items[i].cp);
    /* (Indic and Arabic in their stages; a font with no Arabic forms of its own: the Unicode presentation forms) */
    if (!cscdIndicShape(face, script, buf, cps->items, feats->items, feats->len) &&
        !cscdArabicShape(face, script, buf, cps->items, cps->len, feats->items, feats->len) &&
        !cscdKhmerShape(face, script, buf, cps->items, cps->len, feats->items, feats->len) &&
        !cscdMyanmarShape(face, script, buf, cps->items, cps->len, feats->items, feats->len) &&
        !cscdUseShape(face, script, buf, feats->items, feats->len)) {
        cscdArabicPresentationForms(face, script, buf, cps->items, cps->len);
        cscdOtSubstitute(face, script, buf, feats->items, feats->len);
    }
    for (size_t i = 0; i < buf->len; ++i) buf->items[i].xAdvance = (int32_t)lroundf(cscdFontFaceAdvance(face, buf->items[i].glyph));
    /* the marks' own advances (GDEF marks) come to nothing, as the shaping model for the script says: before the
     * positioning (Myanmar, the Universal Shaping Engine's scripts: what the font adds back counts), after it (most
     * scripts), or never (Indic, Khmer, Hangul) */
    const int zeroMarks = markZeroing(script);
    if (zeroMarks == ZERO_EARLY)
        for (size_t i = 0; i < buf->len; ++i)
            if (cscdOtGlyphClass(face, buf->items[i].glyph) == 3) buf->items[i].xAdvance = 0;
    if (cscdOtHasPositioning(face)) cscdOtPosition(face, script, buf, feats->items, feats->len, rtl);
    else if (!(font->settings && font->settings->noKerning))
        for (size_t i = 1; i < buf->len; ++i) buf->items[i - 1].xAdvance += (int32_t)lroundf(cscdFontFaceKerning(face, buf->items[i - 1].glyph, buf->items[i].glyph));
    if (zeroMarks == ZERO_LATE)
        for (size_t i = 0; i < buf->len; ++i)
            if (cscdOtGlyphClass(face, buf->items[i].glyph) == 3) buf->items[i].xAdvance = 0;
    /* out: the glyphs in order, the characters the font has nothing for (and that show nothing) at
     * their places */
    CscdU32Vec* at = &shapeOut;
    at->len = 0;
    size_t scan = from;
    float digitW = -1;
    for (size_t k = 0; k < buf->len; ++k) {
        const CscdOtGlyph* g = &buf->items[k];
        for (; scan < from + g->cluster; ++scan)
            if (items[scan].invisible) emit(out, (CscdShapedGlyph){0, face, *pen, 0, 0, items[scan].cluster}, *pen, -1);
        const ShapeItem* it = &items[from + g->cluster];
        cscdVecPush(at, (uint32_t)out->len);
        if (isIgnorable(g->ch) && !g->substituted) { /* (left over: shows nothing; its own character, not its cluster's) */
            emit(out, (CscdShapedGlyph){0, face, *pen, 0, 0, it->cluster}, *pen, -1);
            continue;
        }
        if (g->attach && (size_t)g->attach <= k) { /* a mark, placed from the glyph it goes on */
            const CscdShapedGlyph* base = &out->items[at->items[k - g->attach]];
            /* (its place from the anchors; its advance, which the font may give it, still moves what follows) */
            const float madv = g->xAdvance * sc;
            emit(out, (CscdShapedGlyph){g->glyph, face, base->x + g->xOffset * sc, base->y - g->yOffset * sc, madv, it->cluster}, *pen, (int)at->items[k - g->attach]);
            *pen += madv;
            continue;
        }
        if (handTabular && it->cp >= '0' && it->cp <= '9' && g->glyph) {
            /* a tabular digit made by hand: centered in the widest digit's width, not kerned */
            if (digitW < 0) {
                digitW = 0;
                for (uint32_t d = '0'; d <= '9'; ++d) digitW = cscdMaxf(digitW, cscdFontFaceAdvance(face, cscdFontFaceGlyphIndex(face, d)));
            }
            const float own = cscdFontFaceAdvance(face, g->glyph) * sc, cell = digitW * sc, pad = (cell - own) / 2;
            emit(out, (CscdShapedGlyph){g->glyph, face, *pen + pad, 0, cell - pad + letterSpacing, it->cluster}, *pen, -1);
            *pen += cell + letterSpacing;
            continue;
        }
        const float k = it->small ? SMALL_CAPS_SCALE : 1;
        float adv = g->xAdvance * sc * k;
        if (it->cp == ' ') adv += wordSpacing;
        if (g->gclass != 3 || adv != 0) adv += letterSpacing;
        emit(out, (CscdShapedGlyph){g->glyph, face, *pen + g->xOffset * sc * k, -g->yOffset * sc * k, adv, it->cluster, it->small ? SMALL_CAPS_SCALE : 0}, *pen, -1);
        *pen += adv;
    }
    for (; scan < to; ++scan)
        if (items[scan].invisible) emit(out, (CscdShapedGlyph){0, face, *pen, 0, 0, items[scan].cluster}, *pen, -1);
}

/* Right-to-left text is shaped in its logical order (contextual forms, ligatures and marks find
 * the characters around them), its mirrored characters mirrored, then turned around: the glyphs
 * come out left to right, placed from the right. */
static void shapeText(CscdSv text, const CscdFont* font, float letterSpacing, float wordSpacing, bool rtl, CscdShapedGlyphVec* out) {
    cscdVecClear(out);
    shapePens.len = shapeBases.len = 0;
    if (!font->face) return;
    const uint32_t* map = NULL; /* (clusters: offsets in text) */
    /* 1. characters to glyphs, from the font or a fallback: an emoji from a color font, marks and
     * invisible characters from their base's font */
    ShapeItemVec* items = &shapeItems;
    items->len = 0;
    /* (the characters, as some scripts ask the font to see them: cscdScriptsPrepare) */
    CscdU32Vec* inCps = &shapeInCps;
    CscdU32Vec* inClusters = &shapeInClusters;
    inCps->len = inClusters->len = 0;
    for (size_t i = 0; i < text.len;) {
        const size_t start = i;
        cscdVecPush(inCps, cscdUtf8Next(text, &i));
        cscdVecPush(inClusters, map ? map[start] : (uint32_t)start);
    }
    cscdScriptsPrepare(font->face, inCps, inClusters);
    for (size_t in = 0; in < inCps->len; ++in) {
        const uint32_t cp = inCps->items[in];
        const uint32_t cluster = inClusters->items[in];
        /* (two-part vowels: their parts, the first one written before the consonant) */
        static const uint32_t kSplit[][4] = {{0x9CB, 0x9C7, 0x9BE, 0}, {0x9CC, 0x9C7, 0x9D7, 0}, {0xB48, 0xB47, 0xB56, 0}, {0xB4B, 0xB47, 0xB3E, 0},
                                             {0xB4C, 0xB47, 0xB57, 0}, {0xBCA, 0xBC6, 0xBBE, 0}, {0xBCB, 0xBC7, 0xBBE, 0}, {0xBCC, 0xBC6, 0xBD7, 0},
                                             {0xC48, 0xC46, 0xC56, 0}, {0xCC0, 0xCBF, 0xCD5, 0}, {0xCC7, 0xCC6, 0xCD5, 0}, {0xCC8, 0xCC6, 0xCD6, 0},
                                             {0xCCA, 0xCC6, 0xCC2, 0}, {0xCCB, 0xCC6, 0xCC2, 0xCD5}, {0xD4A, 0xD46, 0xD3E, 0}, {0xD4B, 0xD47, 0xD3E, 0},
                                             {0xD4C, 0xD46, 0xD57, 0}};
        size_t split = CSCD_COUNT(kSplit);
        for (size_t k = 0; k < CSCD_COUNT(kSplit) && cp >= 0x9CB && cp <= 0xD4C; ++k) if (kSplit[k][0] == cp) split = k;
        if (split < CSCD_COUNT(kSplit)) {
            for (int k = 1; k < 4 && kSplit[split][k]; ++k)
                cscdVecPush(items, ((ShapeItem){kSplit[split][k], cluster, 0, font->face, false, scriptOf(cp), false}));
            continue;
        }
        cscdVecPush(items, ((ShapeItem){rtl ? cscdBidiMirror(cp) : cp, cluster, 0, font->face, false, scriptOf(cp), false}));
    }
    const int weight = font->fakeBold ? 700 : font->face->weight;
    CscdU32Vec cps = {0};
    for (size_t i = 0; i < items->len; ++i) cscdVecPush(&cps, items->items[i].cp);
    for (size_t i = 0; i < items->len;) {
        ShapeItem* it = &items->items[i];
        const size_t emoji = emojiLength(cps.items, i, items->len);
        if (emoji && !cscdFontFaceHasColor(font->face)) {
            CscdFontFace* ef = cscdFontFallbackEmoji(it->cp);
            if (ef) {
                for (size_t k = i; k < i + emoji; ++k) {
                    ShapeItem* e = &items->items[k];
                    e->face = ef;
                    e->glyph = cscdFontFaceGlyphIndex(ef, e->cp);
                    e->invisible = !e->glyph && isIgnorable(e->cp);
                    e->script = 0;
                }
                i += emoji;
                continue;
            }
        }
        uint32_t cp = it->cp;
        if (cp == '\t' || cp == 0xA0) it->cp = cp = ' ';
        CscdFontFace* prev = i ? items->items[i - 1].face : font->face;
        if (isIgnorable(cp) || isMark(cp)) { /* with the character before, when its font has it */
            const uint16_t g = cscdFontFaceGlyphIndex(prev, cp);
            if (g || isIgnorable(cp)) {
                it->face = prev;
                it->glyph = g;
                it->invisible = !g;
                ++i;
                continue;
            }
        }
        it->glyph = cscdFontFaceGlyphIndex(font->face, cp);
        if (!it->glyph && cp >= 0x20) {
            CscdFontFace* fb = cscdFontFallback(cp, weight, font->face->italic, font->face);
            if (fb) {
                if (font->settings && font->settings->variationCount) fb = variedFace(fb, 0, font->size, font->settings);
                it->face = fb;
                it->glyph = cscdFontFaceGlyphIndex(fb, cp);
            }
        }
        ++i;
    }
    cscdVecFree(&cps);
    /* variation sequences the font has (its format 14 cmap): the character's glyph for the sequence, the selector
     * showing nothing */
    for (size_t i = 0; i + 1 < items->len; ++i) {
        ShapeItem *it = &items->items[i], *vs = &items->items[i + 1];
        const bool selector = (vs->cp >= 0xFE00 && vs->cp <= 0xFE0F) || (vs->cp >= 0xE0100 && vs->cp <= 0xE01EF);
        if (!selector || !it->face->impl->cmap14 || vs->face != it->face) continue;
        const uint16_t g = cscdFontFaceVariantGlyph(it->face, it->cp, vs->cp);
        if (!g) continue;
        it->glyph = g;
        vs->glyph = 0;
        vs->invisible = true;
    }
    /* (scripts: the shared characters take the script around them) */
    uint32_t last = 0;
    for (size_t i = 0; i < items->len; ++i) {
        if (items->items[i].script) last = items->items[i].script;
        else items->items[i].script = last;
    }
    /* small capitals the font does not have: capitals, smaller */
    if (font->settings && font->settings->synthesizeSmallCaps) {
        bool allSmall = false;
        for (size_t i = 0; i < font->settings->featureCount; ++i) allSmall = allSmall || font->settings->features[i].tag == cscdFontTag("c2sc");
        for (size_t i = 0; i < items->len; ++i) {
            ShapeItem* it = &items->items[i];
            if (it->invisible || cscdOtHasFeature(it->face, it->script ? it->script : cscdFontTag("latn"), cscdFontTag("smcp"))) continue;
            const uint32_t upper = (uint32_t)towupper((wint_t)it->cp), lower = (uint32_t)towlower((wint_t)it->cp);
            if (upper != it->cp) {
                const uint16_t g = cscdFontFaceGlyphIndex(it->face, upper);
                if (g) { it->cp = upper; it->glyph = g; it->small = true; }
            } else if (allSmall && lower != it->cp) it->small = true;
        }
    }
    /* 2. runs of one face and script: OpenType substitutions and positions */
    /* (the font's tabular figures, when they do make the digits one width: some fonts' only cover
     * figures other than the default ones) */
    bool tabularGlyphs = font->tabularNums && cscdFontFaceHasTabularFigures(font->face);
    if (tabularGlyphs) {
        float w0 = -1;
        for (uint32_t d = '0'; d <= '9' && tabularGlyphs; ++d) {
            const float w = cscdFontFaceAdvance(font->face, cscdFontFaceTabularGlyph(font->face, cscdFontFaceGlyphIndex(font->face, d)));
            if (w0 < 0) w0 = w;
            tabularGlyphs = w == w0;
        }
    }
    runFeatures(font, letterSpacing, tabularGlyphs, &shapeFeatures);
    float pen = 0;
    for (size_t i = 0; i < items->len;) {
        size_t j = i + 1;
        while (j < items->len && items->items[j].face == items->items[i].face && items->items[j].script == items->items[i].script) ++j;
        const bool hand = font->tabularNums && !tabularGlyphs && items->items[i].face == font->face;
        shapeRun(items->items, i, j, font, letterSpacing, wordSpacing, &shapeFeatures, hand, rtl, &pen, out);
        i = j;
    }
    if (!rtl || !out->len) return;
    /* each glyph where it goes from the right: a glyph at pen p, advance a, offset o (from p) at
     * width - p - a + o; a mark where its base went, as far from it as before */
    CscdFloatVec moved = {0};
    cscdVecResize(&moved, out->len);
    for (size_t i = 0; i < out->len; ++i) {
        const CscdShapedGlyph* g = &out->items[i];
        const int b = shapeBases.items[i];
        if (b >= 0 && (size_t)b < i) moved.items[i] = moved.items[b] + (g->x - out->items[b].x);
        else moved.items[i] = pen - shapePens.items[i] - g->advance + (g->x - shapePens.items[i]);
    }
    for (size_t i = 0; i < out->len; ++i) out->items[i].x = moved.items[i];
    for (size_t i = 0, j = out->len - 1; i < j; ++i, --j) {
        const CscdShapedGlyph t = out->items[i];
        out->items[i] = out->items[j];
        out->items[j] = t;
    }
    cscdVecFree(&moved);
}

void cscdFontShape(CscdSv text, const CscdFont* font, float letterSpacing, float wordSpacing, CscdShapedGlyphVec* out) {
    shapeText(text, font, letterSpacing, wordSpacing, false, out);
}

void cscdFontShapeRtl(CscdSv text, const CscdFont* font, float letterSpacing, float wordSpacing, CscdShapedGlyphVec* out) {
    shapeText(text, font, letterSpacing, wordSpacing, true, out);
}

static CSCD_THREAD_LOCAL CscdShapedGlyphVec measureTmp;

float cscdFontMeasure(CscdSv text, const CscdFont* font, float letterSpacing, float wordSpacing) {
    cscdFontShape(text, font, letterSpacing, wordSpacing, &measureTmp);
    /* (where the last glyph that advances ends: marks after it are placed on it) */
    for (size_t i = measureTmp.len; i-- > 0;)
        if (measureTmp.items[i].advance != 0) return measureTmp.items[i].x + measureTmp.items[i].advance;
    return measureTmp.len ? measureTmp.items[measureTmp.len - 1].x : 0;
}
