/* What some scripts ask of the characters before the font sees them (as the OpenType shaping models for them, and
 * HarfBuzz, do): Thai and Lao SARA AM split into NIKHAHIT and SARA AA, NIKHAHIT moved before the tone marks over the
 * consonant; Hangul jamo composed into the syllable the font has; Khmer's and Sinhala's multi-part vowels split, their
 * first part a pre-base vowel. */
#include "text/fontInternal.h"

#include <string.h>

typedef struct Cp { uint32_t cp, cluster; } Cp;
typedef CSCD_VEC(Cp) CpVec;

/* (Thai and Lao: a mark written over the consonant, which NIKHAHIT goes before) */
static bool aboveMark(uint32_t c) {
    return c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E37) || (c >= 0x0E47 && c <= 0x0E4E) || c == 0x0EB1 || (c >= 0x0EB4 && c <= 0x0EB7) ||
           c == 0x0EBB || (c >= 0x0EC8 && c <= 0x0ECE);
}

/* Hangul: the syllable of a leading consonant, a vowel and a trailing consonant (0: none) */
static uint32_t hangulSyllable(uint32_t l, uint32_t v, uint32_t t) {
    if (l < 0x1100 || l > 0x1112 || v < 0x1161 || v > 0x1175) return 0;
    if (t && (t < 0x11A8 || t > 0x11C2)) return 0;
    return 0xAC00 + ((l - 0x1100) * 21 + (v - 0x1161)) * 28 + (t ? t - 0x11A7 : 0);
}

void cscdScriptsPrepare(const CscdFontFace* face, CscdU32Vec* cps, CscdU32Vec* clusters) {
    bool any = false;
    for (size_t i = 0; i < cps->len && !any; ++i) {
        const uint32_t c = cps->items[i];
        any = c == 0x0E33 || c == 0x0EB3 || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x17BE && c <= 0x17C5) || c == 0x0DDA || (c >= 0x0DDC && c <= 0x0DDE);
    }
    if (!any) return;
    CpVec out = {0};
    for (size_t i = 0; i < cps->len; ++i) {
        const uint32_t c = cps->items[i], cl = clusters->items[i];
        if (c == 0x0E33 || c == 0x0EB3) {
            /* NIKHAHIT before the marks over the consonant (those just before it), then SARA AA */
            size_t at = out.len;
            while (at > 0 && aboveMark(out.items[at - 1].cp)) --at;
            const Cp nikhahit = {c == 0x0E33 ? 0x0E4Du : 0x0ECDu, cl};
            cscdVecPush(&out, nikhahit);
            for (size_t k = out.len - 1; k > at; --k) out.items[k] = out.items[k - 1];
            out.items[at] = nikhahit;
            cscdVecPush(&out, ((Cp){c == 0x0E33 ? 0x0E32u : 0x0EB2u, cl}));
            continue;
        }
        if (c >= 0x1100 && c <= 0x1112 && i + 1 < cps->len) {
            /* L V (T): the syllable, when the font has it */
            const uint32_t v = cps->items[i + 1], t = i + 2 < cps->len ? cps->items[i + 2] : 0;
            const uint32_t withT = hangulSyllable(c, v, t), noT = hangulSyllable(c, v, 0);
            if (withT && cscdFontFaceGlyphIndex(face, withT)) {
                cscdVecPush(&out, ((Cp){withT, cl}));
                i += 2;
                continue;
            }
            if (noT && cscdFontFaceGlyphIndex(face, noT)) {
                cscdVecPush(&out, ((Cp){noT, cl}));
                i += 1;
                continue;
            }
        }
        if (c == 0x17BE || c == 0x17BF || c == 0x17C0 || c == 0x17C4 || c == 0x17C5) {
            /* (Khmer: the pre-base part, E, then the vowel itself, which the font draws as its second part) */
            cscdVecPush(&out, ((Cp){0x17C1, cl}));
            cscdVecPush(&out, ((Cp){c, cl}));
            continue;
        }
        if (c == 0x0DDA || (c >= 0x0DDC && c <= 0x0DDE)) {
            /* (Sinhala: the two- and three-part vowels as their canonical parts, KOMBUVA first, a pre-base vowel) */
            cscdVecPush(&out, ((Cp){0x0DD9, cl}));
            if (c == 0x0DDC || c == 0x0DDD) cscdVecPush(&out, ((Cp){0x0DCF, cl}));
            if (c == 0x0DDE) cscdVecPush(&out, ((Cp){0x0DDF, cl}));
            if (c == 0x0DDA || c == 0x0DDD) cscdVecPush(&out, ((Cp){0x0DCA, cl}));
            continue;
        }
        cscdVecPush(&out, ((Cp){c, cl}));
    }
    cps->len = clusters->len = 0;
    for (size_t i = 0; i < out.len; ++i) {
        cscdVecPush(cps, out.items[i].cp);
        cscdVecPush(clusters, out.items[i].cluster);
    }
    cscdVecFree(&out);
}

/* ---- Khmer: syllables reordered, then the basic features to their glyphs ---------------------------------------
 * (as HarfBuzz's Khmer shaper: Coeng Ro and the pre-base vowels to the syllable's start; pref on Coeng Ro, blwf, abvf
 * and pstf on what follows the base, cfar on what follows Coeng Ro; then pres, abvs, blws, psts; then the rest) */

static uint32_t stag(const char* s) { return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | (uint32_t)s[3]; }

/* features that look at one syllable at a time (a syllable: the glyphs of one cluster, merged before): each syllable
 * substituted on its own, so that no lookup reaches into the next one (one that skips base glyphs would) */
static void bySyllable(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const CscdOtFeature* features, size_t n) {
    CscdOtBuffer one = {0}, out = {0};
    for (size_t s = 0; s < buf->len;) {
        size_t e = s + 1;
        while (e < buf->len && buf->items[e].cluster == buf->items[s].cluster) ++e;
        one.len = 0;
        cscdVecAppend(&one, buf->items + s, e - s);
        cscdOtSubstitute(face, script, &one, features, n);
        cscdVecAppend(&out, one.items, one.len);
        s = e;
    }
    CscdOtBuffer t = *buf;
    *buf = out;
    cscdVecFree(&t);
    cscdVecFree(&one);
}

#define K_PREF (1u << 5)
#define K_BAP (1u << 6) /* (blwf, abvf, pstf) */
#define K_CFAR (1u << 7)

static bool khmerBase(uint32_t c) { return (c >= 0x1780 && c <= 0x17B3) || c == 0x25CC; }

bool cscdKhmerShape(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const uint32_t* cps, size_t ncps, const CscdOtFeature* features,
                    size_t n) {
    if (script != stag("khmr") || !buf->len) return false;
    (void)cps; /* (each glyph keeps its character: ch) */
    (void)ncps;
#define CP(k) (buf->items[k].ch)
    for (size_t i = 0; i < buf->len; ++i) buf->items[i].mask = ~(K_PREF | K_BAP | K_CFAR);
    for (size_t s = 0; s < buf->len;) {
        /* the syllable: from a base to the next base not joined by a Coeng */
        size_t e = s + 1;
        if (khmerBase(CP(s)))
            while (e < buf->len && !(khmerBase(CP(e)) && CP(e - 1) != 0x17D2)) ++e;
        for (size_t i = s + 1; i < e; ++i) buf->items[i].mask |= K_BAP;
        int coengs = 0;
        for (size_t i = s + 1; i < e; ++i) {
            const uint32_t c = CP(i);
            if (c == 0x17D2 && coengs <= 2 && i + 1 < e) {
                ++coengs;
                if (CP(i + 1) == 0x179A) { /* Coeng Ro: to the start, pref; what follows it, cfar */
                    buf->items[i].mask |= K_PREF;
                    buf->items[i + 1].mask |= K_PREF;
                    const CscdOtGlyph t0 = buf->items[i], t1 = buf->items[i + 1];
                    memmove(&buf->items[s + 2], &buf->items[s], (i - s) * sizeof *buf->items);
                    buf->items[s] = t0;
                    buf->items[s + 1] = t1;
                    for (size_t j = i + 2; j < e; ++j) buf->items[j].mask |= K_CFAR;
                    coengs = 2;
                }
            } else if (c == 0x17C1 || c == 0x17C2 || c == 0x17C3) { /* a pre-base vowel: to the start */
                const CscdOtGlyph t = buf->items[i];
                memmove(&buf->items[s + 1], &buf->items[s], (i - s) * sizeof *buf->items);
                buf->items[s] = t;
            }
        }
        uint32_t first = buf->items[s].cluster;
        for (size_t k = s; k < e; ++k) first = buf->items[k].cluster < first ? buf->items[k].cluster : first;
        for (size_t k = s; k < e; ++k) buf->items[k].cluster = first;
        s = e;
    }
#undef CP
    const CscdOtFeature basic[] = {{stag("locl"), 1, ~0u}, {stag("ccmp"), 1, ~0u}, {stag("pref"), 1, K_PREF}, {stag("blwf"), 1, K_BAP},
                                   {stag("abvf"), 1, K_BAP},  {stag("pstf"), 1, K_BAP}, {stag("cfar"), 1, K_CFAR}};
    bySyllable(face, script, buf, basic, CSCD_COUNT(basic));
    const CscdOtFeature pres[] = {{stag("pres"), 1, ~0u}, {stag("abvs"), 1, ~0u}, {stag("blws"), 1, ~0u}, {stag("psts"), 1, ~0u}};
    cscdOtSubstitute(face, script, buf, pres, CSCD_COUNT(pres));
    cscdOtSubstitute(face, script, buf, features, n);
    return true;
}

/* ---- Myanmar: syllables reordered by position, then the basic features one by one --------------------------------
 * (as HarfBuzz's Myanmar shaper: a kinzi, Nga or Ra with Asat and Virama starting the syllable, after the base; Medial
 * Ra and the pre-base vowel before it; the marks after it in their order; locl and ccmp before, rphf, pref, blwf, pstf
 * each its stage after, then pres, abvs, blws, psts, then the rest) */

enum { MP_PRE_M, MP_PRE_C, MP_BASE_C, MP_AFTER_MAIN, MP_BEFORE_SUB, MP_BELOW_C, MP_AFTER_SUB };

static bool myanmarConsonant(uint32_t c) {
    return (c >= 0x1000 && c <= 0x102A) || c == 0x103F || (c >= 0x104E && c <= 0x104E) || (c >= 0x1050 && c <= 0x1055) || (c >= 0x105A && c <= 0x105D) ||
           c == 0x1061 || c == 0x1065 || c == 0x1066 || (c >= 0x106E && c <= 0x1070) || (c >= 0x1075 && c <= 0x1081) || c == 0x108E ||
           (c >= 0xA9E0 && c <= 0xA9E4) || (c >= 0xAA60 && c <= 0xAA76) || c == 0x25CC;
}
static bool myanmarBelowVowel(uint32_t c) { return c == 0x102F || c == 0x1030 || c == 0x1058 || c == 0x1059; }

typedef struct MyItem { CscdOtGlyph g; int pos; size_t order; } MyItem;

static int compareMyItems(const void* a, const void* b, void* ctx) {
    (void)ctx;
    const MyItem *x = (const MyItem*)a, *y = (const MyItem*)b;
    if (x->pos != y->pos) return x->pos - y->pos;
    return x->order < y->order ? -1 : x->order > y->order;
}

bool cscdMyanmarShape(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const uint32_t* cps, size_t ncps, const CscdOtFeature* features,
                      size_t n) {
    if ((script != stag("mym2") && script != stag("mymr")) || !buf->len) return false;
    (void)cps; /* (each glyph keeps its character: ch) */
    (void)ncps;
#define CP(k) (buf->items[k].ch)
    /* the syllables, each one cluster: from a consonant to the next one not stacked under a Virama (a kinzi's in it) */
    for (size_t s = 0; s < buf->len;) {
        size_t e = s + 1;
        if (myanmarConsonant(CP(s)))
            while (e < buf->len && !(myanmarConsonant(CP(e)) && CP(e - 1) != 0x1039)) ++e;
        for (size_t k = s + 1; k < e; ++k) buf->items[k].cluster = buf->items[s].cluster;
        s = e;
    }
    const CscdOtFeature first[] = {{stag("locl"), 1, ~0u}, {stag("ccmp"), 1, ~0u}};
    bySyllable(face, script, buf, first, CSCD_COUNT(first));
    /* (a glyph's character: what its cluster's characters say, the first at its place in them) */
    CSCD_VEC(MyItem) items = {0};
    for (size_t s = 0; s < buf->len;) {
        size_t e = s + 1;
        while (e < buf->len && buf->items[e].cluster == buf->items[s].cluster) ++e;
        const bool kinzi = e - s >= 4 && (CP(s) == 0x1004 || CP(s) == 0x101B || CP(s) == 0x105A) && CP(s + 1) == 0x103A && CP(s + 2) == 0x1039;
        size_t base = kinzi ? s + 3 : s;
        while (base < e && !myanmarConsonant(CP(base))) ++base;
        items.len = 0;
        int pos = MP_AFTER_MAIN;
        for (size_t i = s; i < e; ++i) {
            const uint32_t c = CP(i);
            int p;
            if (kinzi && i < s + 3) p = MP_AFTER_MAIN;
            else if (i < base) p = MP_PRE_C;
            else if (i == base) p = MP_BASE_C;
            else if (c == 0x103C) p = MP_PRE_C; /* (Medial Ra) */
            else if (c == 0x1031 || c == 0x1084) p = MP_PRE_M; /* (the pre-base vowel) */
            else if (c >= 0xFE00 && c <= 0xFE0F) p = items.len ? items.items[items.len - 1].pos : pos;
            else if (pos == MP_AFTER_MAIN && myanmarBelowVowel(c)) p = pos = MP_BELOW_C;
            else if (pos == MP_BELOW_C && c == 0x1036) p = MP_BEFORE_SUB;
            else if (pos == MP_BELOW_C && myanmarBelowVowel(c)) p = pos;
            else if (pos == MP_BELOW_C) p = pos = MP_AFTER_SUB;
            else p = pos;
            const MyItem it = {buf->items[i], p, i};
            cscdVecPush(&items, it);
        }
        cscdSort(items.items, items.len, sizeof(MyItem), compareMyItems, NULL);
        uint32_t cl = buf->items[s].cluster;
        for (size_t k = s; k < e; ++k) cl = buf->items[k].cluster < cl ? buf->items[k].cluster : cl;
        for (size_t k = 0; k < items.len; ++k) {
            buf->items[s + k] = items.items[k].g;
            buf->items[s + k].cluster = cl;
        }
        s = e;
    }
    cscdVecFree(&items);
#undef CP
    static const char* const kBasic[] = {"rphf", "pref", "blwf", "pstf"};
    for (int f = 0; f < 4; ++f) {
        const CscdOtFeature one = {stag(kBasic[f]), 1, ~0u};
        bySyllable(face, script, buf, &one, 1);
    }
    const CscdOtFeature pres[] = {{stag("pres"), 1, ~0u}, {stag("abvs"), 1, ~0u}, {stag("blws"), 1, ~0u}, {stag("psts"), 1, ~0u}};
    cscdOtSubstitute(face, script, buf, pres, CSCD_COUNT(pres));
    cscdOtSubstitute(face, script, buf, features, n);
    return true;
}
