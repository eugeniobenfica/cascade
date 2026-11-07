/* Indic shaping (Devanagari, Bengali, Gurmukhi, Gujarati, Oriya, Tamil, Telugu, Kannada,
 * Malayalam), after OpenType's "new" Indic model: syllables found, their base consonant chosen,
 * pre-base matras moved before the consonants and the reph after the base, then the basic
 * features applied one by one to the glyphs each is for (rphf to the reph, half to the consonants
 * before the base, blwf and pstf to those after it), then the presentation features. */
#include "text/fontInternal.h"

#include <string.h>

typedef enum { CAT_OTHER, CAT_C, CAT_V, CAT_M, CAT_H, CAT_N, CAT_SM, CAT_ZWJ, CAT_ZWNJ, CAT_RA } Category;
typedef enum { POS_NONE, POS_PRE, POS_ABOVE, POS_BELOW, POS_POST } MatraPos;

/* The block of an Indic script, and what is special in it. */
typedef struct Script {
    uint32_t base;           /* the first code point of its block */
    uint32_t tag, oldTag;
    bool baseFirst;          /* the base is the first consonant (Telugu, Kannada: the ones after it take below forms) */
    uint8_t preMatras[4];    /* offsets in the block of the matras written before the consonant */
    uint8_t belowRa;         /* a final Ra after halant takes a below form (rakaar, ra-phala) */
} Script;

static const Script kScripts[] = {
    {0x900, 0, 0, false, {0x3F, 0x4E, 0, 0}, 1},
    {0x980, 0, 0, false, {0x3F, 0x47, 0x48, 0}, 1},
    {0xA00, 0, 0, false, {0x3F, 0, 0, 0}, 1},
    {0xA80, 0, 0, false, {0x3F, 0, 0, 0}, 1},
    {0xB00, 0, 0, false, {0x47, 0, 0, 0}, 1},
    {0xB80, 0, 0, false, {0x46, 0x47, 0x48, 0}, 0},
    {0xC00, 0, 0, true, {0, 0, 0, 0}, 0},
    {0xC80, 0, 0, true, {0, 0, 0, 0}, 0},
    {0xD00, 0, 0, false, {0x46, 0x47, 0x48, 0}, 0},
};

static const Script* scriptFor(uint32_t cp) {
    if (cp < 0x900 || cp >= 0xD80) return NULL;
    return &kScripts[(cp - 0x900) >> 7];
}

static bool isIndicScript(uint32_t script) {
    static const char* tags[] = {"dev2", "bng2", "gur2", "gjr2", "ory2", "tml2", "tel2", "knd2", "mlm2"};
    for (size_t i = 0; i < CSCD_COUNT(tags); ++i) if (script == tag(tags[i])) return true;
    return false;
}

static Category categoryOf(uint32_t cp) {
    if (cp == 0x200D) return CAT_ZWJ;
    if (cp == 0x200C) return CAT_ZWNJ;
    if (cp == 0x25CC) return CAT_C; /* (the dotted circle stands for a missing base) */
    const Script* s = scriptFor(cp);
    if (!s) return CAT_OTHER;
    const uint32_t o = cp - s->base;
    if (o >= 0x01 && o <= 0x03) return CAT_SM;
    if ((o >= 0x04 && o <= 0x14) || o == 0x60 || o == 0x61 || (s->base == 0x900 && o >= 0x72 && o <= 0x77)) return CAT_V;
    if (o == 0x30) return CAT_RA;
    if ((o >= 0x15 && o <= 0x39) || (o >= 0x58 && o <= 0x5F) || (s->base == 0x900 && o >= 0x78 && o <= 0x7F)) return CAT_C;
    if (o == 0x3C) return CAT_N;
    if (o == 0x4D) return CAT_H;
    if ((o >= 0x3A && o <= 0x4C) || o == 0x4E || o == 0x4F || o == 0x55 || o == 0x56 || o == 0x57 || o == 0x62 || o == 0x63) return o == 0x3D ? CAT_OTHER : CAT_M;
    if (o >= 0x51 && o <= 0x54) return CAT_SM;
    return CAT_OTHER;
}

static MatraPos matraPos(uint32_t cp) {
    const Script* s = scriptFor(cp);
    if (!s) return POS_NONE;
    const uint32_t o = cp - s->base;
    for (int i = 0; i < 4 && s->preMatras[i]; ++i) if (s->preMatras[i] == o) return POS_PRE;
    if (o == 0x3E || o == 0x40 || (o >= 0x49 && o <= 0x4C) || o == 0x4F || o == 0x57) return POS_POST;
    if ((o >= 0x41 && o <= 0x44) || o == 0x62 || o == 0x63 || o == 0x56) return POS_BELOW;
    return POS_ABOVE;
}

/* the glyphs' feature masks */
enum { M_GLOBAL = 1u << 31, M_RPHF = 1, M_HALF = 2, M_BLWF = 4, M_PSTF = 8, M_MOD = 64 /* (a modifier, anusvara...: the reph goes before it) */ };

typedef struct Info { Category cat; MatraPos pos; uint32_t cp; } Info;

/* One syllable from i: a consonant syllable (consonants joined by halants, then matras and
 * modifiers), a vowel syllable, or anything else alone. Returns its end. */
static size_t syllableEnd(const Info* in, size_t i, size_t n) {
    const Category c = in[i].cat;
    size_t k = i + 1;
    if (c == CAT_C || c == CAT_RA || c == CAT_V) {
        if (k < n && in[k].cat == CAT_N) ++k;
        if (c != CAT_V)
            while (k < n) { /* (halant, maybe a joiner, then the next consonant) */
                size_t j = k;
                if (in[j].cat != CAT_H) break;
                ++j;
                if (j < n && (in[j].cat == CAT_ZWJ || in[j].cat == CAT_ZWNJ)) ++j;
                if (j < n && (in[j].cat == CAT_C || in[j].cat == CAT_RA)) {
                    k = j + 1;
                    if (k < n && in[k].cat == CAT_N) ++k;
                } else break;
            }
        if (k < n && in[k].cat == CAT_H) {
            ++k;
            if (k < n && (in[k].cat == CAT_ZWJ || in[k].cat == CAT_ZWNJ)) ++k;
        }
        while (k < n && (in[k].cat == CAT_M || in[k].cat == CAT_N || in[k].cat == CAT_H)) ++k;
        while (k < n && in[k].cat == CAT_SM) ++k;
    }
    return k;
}

static void moveGlyph(CscdOtBuffer* buf, Info* in, size_t from, size_t to) {
    if (from == to) return;
    const CscdOtGlyph g = buf->items[from];
    const Info f = in[from];
    if (from < to) {
        memmove(&buf->items[from], &buf->items[from + 1], (to - from) * sizeof *buf->items);
        memmove(&in[from], &in[from + 1], (to - from) * sizeof *in);
    } else {
        memmove(&buf->items[to + 1], &buf->items[to], (from - to) * sizeof *buf->items);
        memmove(&in[to + 1], &in[to], (from - to) * sizeof *in);
    }
    buf->items[to] = g;
    in[to] = f;
}

/* Whether the font has a form of the feature for the glyphs a and b (a consonant and a halant, in
 * either order): HarfBuzz's test of which consonants take below-base and post-base forms. */
static bool wouldSubstitute(const CscdFontFace* face, uint32_t script, const char* feature, uint16_t a, uint16_t b) {
    CscdOtGlyph gs[2] = {{a, 0, ~0u, 0, 0, false, 0, 0, 0, 0}, {b, 1, ~0u, 0, 0, false, 0, 0, 0, 0}};
    CscdOtBuffer tmp = {0};
    cscdVecAppend(&tmp, gs, 2);
    const CscdOtFeature f = {tag(feature), 1, ~0u};
    cscdOtSubstitute(face, script, &tmp, &f, 1);
    const bool changed = tmp.len != 2 || tmp.items[0].glyph != a || tmp.items[1].glyph != b;
    cscdVecFree(&tmp);
    return changed;
}

static void apply(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const char* feature, uint32_t mask) {
    const CscdOtFeature f = {tag(feature), 1, mask};
    cscdOtSubstitute(face, script, buf, &f, 1);
}

bool cscdIndicShape(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const uint32_t* cps, const CscdOtFeature* features, size_t n) {
    if (!isIndicScript(script) || !buf->len) return false;
    const size_t len = buf->len;
    Info* in = (Info*)cscdCalloc(len, sizeof(Info));
    for (size_t i = 0; i < len; ++i) {
        in[i].cp = cps[buf->items[i].cluster];
        in[i].cat = categoryOf(in[i].cp);
        in[i].pos = in[i].cat == CAT_M ? matraPos(in[i].cp) : POS_NONE;
        buf->items[i].mask = M_GLOBAL | (in[i].cat == CAT_SM ? M_MOD : 0);
    }
    const bool hasReph = cscdOtHasFeature(face, script, tag("rphf"));
    /* 1. each syllable: its base, the reph, the pre-base matras; the masks */
    for (size_t s = 0; s < len;) {
        const size_t e = syllableEnd(in, s, len);
        const Script* sc = scriptFor(in[s].cp);
        if (e - s < 2 || !sc || (in[s].cat != CAT_C && in[s].cat != CAT_RA)) {
            for (size_t k = s; k < e; ++k) buf->items[k].cluster = buf->items[s].cluster;
            s = e;
            continue;
        }
        size_t start = s;
        bool reph = false;
        /* Ra + halant before another consonant: a reph (not before a joiner) */
        if (hasReph && in[s].cat == CAT_RA && s + 2 < e && in[s + 1].cat == CAT_H && (in[s + 2].cat == CAT_C || in[s + 2].cat == CAT_RA)) {
            reph = true;
            start = s + 2;
            buf->items[s].mask |= M_RPHF;
            buf->items[s + 1].mask |= M_RPHF;
        }
        /* the consonants and where the matras start */
        size_t cons[32];
        int nc = 0;
        size_t tail = e;
        for (size_t k = start; k < e; ++k) {
            if ((in[k].cat == CAT_C || in[k].cat == CAT_RA) && nc < 32) cons[nc++] = k;
            if (in[k].cat == CAT_M || in[k].cat == CAT_SM) { tail = k; break; }
        }
        if (!nc) { s = e; continue; }
        /* the base: the last consonant without a below-base or post-base form (after a halant) */
        int bi = nc - 1;
        while (bi > 0 && in[cons[bi] - 1].cat == CAT_H) {
            const uint16_t h = buf->items[cons[bi] - 1].glyph, c = buf->items[cons[bi]].glyph;
            if (!wouldSubstitute(face, script, "blwf", h, c) && !wouldSubstitute(face, script, "blwf", c, h) &&
                !wouldSubstitute(face, script, "pstf", h, c) && !wouldSubstitute(face, script, "pstf", c, h))
                break;
            --bi;
        }
        const size_t base = cons[bi];
        for (int c = 0; c < nc; ++c) {
            const size_t k = cons[c];
            const size_t partEnd = c + 1 < nc ? cons[c + 1] : tail;
            const uint32_t m = k < base ? M_HALF : k > base ? (M_BLWF | M_PSTF) : 0;
            if (!m) continue;
            if (k > base) buf->items[k - 1].mask |= m; /* (the halant before it) */
            for (size_t q = k; q < partEnd; ++q) buf->items[q].mask |= m;
        }
        /* pre-base matras before the consonants (after the reph) */
        for (size_t k = tail; k < e; ++k)
            if (in[k].pos == POS_PRE) {
                moveGlyph(buf, in, k, start);
            }
        for (size_t k = s; k < e; ++k) buf->items[k].cluster = buf->items[s].cluster;
        (void)reph;
        (void)sc;
        s = e;
    }
    /* 2. the basic features, one by one, each to its glyphs */
    apply(face, script, buf, "locl", ~0u);
    apply(face, script, buf, "nukt", ~0u);
    apply(face, script, buf, "akhn", ~0u);
    apply(face, script, buf, "rphf", M_RPHF);
    apply(face, script, buf, "rkrf", ~0u);
    apply(face, script, buf, "pref", M_PSTF);
    apply(face, script, buf, "blwf", M_BLWF);
    apply(face, script, buf, "abvf", ~0u);
    apply(face, script, buf, "half", M_HALF);
    apply(face, script, buf, "pstf", M_PSTF);
    apply(face, script, buf, "vatu", ~0u);
    apply(face, script, buf, "cjct", ~0u);
    /* 3. the reph to its place: after the base and what hangs on it, before the matras written
     * after it (or the syllable's end) */
    for (size_t s = 0; s < buf->len;) {
        size_t e = s + 1;
        while (e < buf->len && buf->items[e].cluster == buf->items[s].cluster) ++e;
        const CscdOtGlyph* first = &buf->items[s];
        if ((first->mask & M_RPHF) && e - s > 1 && !(buf->items[s + 1].mask & M_RPHF)) {
            /* (a reph glyph: Ra and halant became one; the mask on the next glyph too means they did not) */
            size_t to = e - 1;
            for (size_t k = s + 1; k < e; ++k)
                if (buf->items[k].mask & M_MOD) { to = k - 1; break; }
            CscdOtGlyph r = buf->items[s];
            memmove(&buf->items[s], &buf->items[s + 1], (to - s) * sizeof *buf->items);
            buf->items[to] = r;
        }
        s = e;
    }
    /* 4. the presentation forms and the rest */
    CscdOtFeature pres[] = {{tag("init"), 1, ~0u}, {tag("pres"), 1, ~0u}, {tag("abvs"), 1, ~0u}, {tag("blws"), 1, ~0u},
                            {tag("psts"), 1, ~0u}, {tag("haln"), 1, ~0u}};
    cscdOtSubstitute(face, script, buf, pres, CSCD_COUNT(pres));
    cscdOtSubstitute(face, script, buf, features, n);
    cscdFree(in);
    return true;
}
