/* The Universal Shaping Engine (Microsoft's model for the Brahmi-derived scripts that have no shaper of their own:
 * Tibetan, Javanese, Balinese, Tai Tham, Sinhala and some sixty others), as HarfBuzz implements it. Each character has a
 * category (useData.inc, from the Unicode Indic properties); the text is cut into clusters by the model's grammar, a
 * broken one (a mark with no base) given a dotted circle; then, one cluster at a time, the font's locl, ccmp, nukt and
 * akhn, its rphf (a repha marked for reordering), pref (a glyph it makes moves like a pre-base vowel), its rkrf, abvf,
 * blwf, half, pstf, vatu and cjct; the repha moved to the end of its base's consonants and the pre-base glyphs to its
 * start; then abvs, blws, haln, pres and psts, and the rest. */
#include "text/fontInternal.h"

#include <string.h>

typedef enum UseCategory {
    U_O, U_B, U_N, U_GB, U_CGJ, U_FAbv, U_FBlw, U_FPst, U_FMAbv, U_FMBlw, U_FMPst, U_MAbv, U_MBlw, U_MPst, U_MPre, U_CMAbv,
    U_CMBlw, U_SUB, U_CS, U_H, U_HVM, U_HN, U_IS, U_Sk, U_ZWNJ, U_R, U_SMAbv, U_SMBlw, U_VAbv, U_VBlw, U_VPst, U_VPre,
    U_VMAbv, U_VMBlw, U_VMPst, U_VMPre, U_WJ
} UseCategory;

#include "text/useData.inc"

static uint32_t useEntry(uint32_t cp) {
    size_t lo = 0, hi = CSCD_COUNT(kUse);
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if ((kUse[mid] >> 8) <= cp) lo = mid;
        else hi = mid;
    }
    return kUse[lo];
}
static UseCategory useCategoryOf(uint32_t cp) { return (UseCategory)(useEntry(cp) & 0x3F); }
/* (a Unicode mark: general category Mn, Mc or Me) */
static bool isMark(uint32_t cp) { return (useEntry(cp) & 0x80) != 0; }

static int combiningClass(uint32_t cp) {
    size_t lo = 0, hi = CSCD_COUNT(kUseCombiningClasses);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const uint32_t c = kUseCombiningClasses[mid] >> 8;
        if (c == cp) return (int)(kUseCombiningClasses[mid] & 0xFF);
        if (c < cp) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static const uint32_t* decompositionOf(uint32_t cp) {
    size_t lo = 0, hi = CSCD_COUNT(kUseDecompositions);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (kUseDecompositions[mid][0] == cp) return kUseDecompositions[mid];
        if (kUseDecompositions[mid][0] < cp) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static uint32_t composition(uint32_t a, uint32_t b) {
    for (size_t i = 0; i < CSCD_COUNT(kUseDecompositions); ++i)
        if (kUseDecompositions[i][1] == a && kUseDecompositions[i][2] == b && kUseDecompositions[i][3]) return kUseDecompositions[i][0];
    return 0;
}

static uint32_t tagOf(const char* s) { return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | (uint32_t)s[3]; }

bool cscdUseScript(uint32_t script) {
    static const char kTags[][5] = {
        "ahom", "bali", "batk", "bhks", "brah", "bugi", "buhd", "cakm", "cham", "diak", "dogr", "gong", "gonm", "gran", "hano",
        "java", "kali", "kawi", "khar", "khoj", "kthi", "lana", "lepc", "limb", "maka", "mahj", "marc", "modi", "mtei", "mult",
        "nand", "newa", "rjng", "saur", "shrd", "sidd", "sind", "sinh", "soyo", "sund", "sylo", "tagb", "takr", "tale", "talu",
        "tavt", "tglg", "tibt", "tirh", "tutg", "zanb",
    };
    for (size_t i = 0; i < CSCD_COUNT(kTags); ++i)
        if (script == tagOf(kTags[i])) return true;
    return false;
}

/* (the glyphs' mask bits of this shaper: the features that look at some glyphs only, what rphf and pref did, the kind of
 * cluster a glyph is in) */
#define U_RPHF (1u << 20)
#define U_REPHA (1u << 21) /* (made by rphf: a repha) */
#define U_PREF (1u << 22)  /* (made by pref: a pre-base glyph) */
#define U_KIND_SHIFT 23
#define U_KIND (3u << U_KIND_SHIFT)
enum { KIND_REORDERED = 1, KIND_BROKEN, KIND_OTHER };

static UseCategory categoryOf(const CscdOtGlyph* g) {
    if (g->mask & U_REPHA) return U_R;
    if (g->mask & U_PREF) return U_VPre;
    return useCategoryOf(g->ch);
}

/* ---- clusters, by the model's grammar ------------------------------------------------------------------------------ */

typedef struct Cats { const UseCategory* c; size_t n; } Cats;

static bool isAny(const Cats* s, size_t i, uint64_t set) { return i < s->n && (set >> s->c[i] & 1); }
#define F(x) ((uint64_t)1 << (x))
#define HALANTS (F(U_H) | F(U_HVM) | F(U_IS) | F(U_Sk))
static size_t many(const Cats* s, size_t i, UseCategory c) {
    while (i < s->n && s->c[i] == c) ++i;
    return i;
}
static size_t optional(const Cats* s, size_t i, UseCategory c) { return i < s->n && s->c[i] == c ? i + 1 : i; }

/* consonant modifiers: CMAbv* CMBlw* ((halant B | SUB) CMAbv? CMBlw*)* */
static size_t consonantModifiers(const Cats* s, size_t i) {
    i = many(s, many(s, i, U_CMAbv), U_CMBlw);
    for (;;) {
        if (isAny(s, i, HALANTS) && isAny(s, i + 1, F(U_B))) i += 2;
        else if (isAny(s, i, F(U_SUB))) ++i;
        else return i;
        i = optional(s, optional(s, i, U_CMAbv), U_CMBlw);
    }
}

/* the middle of a cluster: its consonant modifiers, medials, vowels (or a halant), vowel modifiers, then (Sk B)* */
static size_t middle(const Cats* s, size_t i) {
    i = consonantModifiers(s, i);
    i = optional(s, optional(s, optional(s, optional(s, i, U_MPre), U_MAbv), U_MBlw), U_MPst);
    if (isAny(s, i, F(U_H))) ++i;
    else i = many(s, many(s, many(s, many(s, i, U_VPre), U_VAbv), U_VBlw), U_VPst);
    i = optional(s, i, U_HVM);
    i = many(s, many(s, many(s, many(s, i, U_VMPre), U_VMAbv), U_VMBlw), U_VMPst);
    while (isAny(s, i, F(U_Sk)) && isAny(s, i + 1, F(U_B))) i += 2;
    return i;
}

/* the tail after the middle: the finals and their modifiers */
static size_t finals(const Cats* s, size_t i) {
    i = many(s, many(s, many(s, i, U_FAbv), U_FBlw), U_FPst);
    if (isAny(s, i, F(U_FMPst))) return i + 1;
    return many(s, many(s, i, U_FMAbv), U_FMBlw);
}

/* what follows a cluster's start (its base, or nothing in a broken one): to the end of the cluster */
static size_t tail(const Cats* s, size_t i) {
    const size_t m = consonantModifiers(s, i);
    if (isAny(s, m, F(U_IS))) return m + 1; /* (ended by an invisible stacker) */
    const size_t mid = middle(s, i);
    if (isAny(s, mid, F(U_Sk))) return mid + 1; /* (ended by a sakot) */
    return finals(s, mid);
}

/* The cluster from i: its end, and its kind. */
static size_t clusterAt(const Cats* s, size_t i, int* kind) {
    const UseCategory c = s->c[i];
    *kind = KIND_REORDERED;
    if ((c == U_R || c == U_CS) && isAny(s, i + 1, F(U_B) | F(U_GB))) return tail(s, i + 2);
    if (c == U_B || c == U_GB) {
        if (c == U_GB && isAny(s, i + 1, F(U_SMAbv) | F(U_SMBlw))) {
            *kind = KIND_OTHER;
            return many(s, many(s, i + 1, U_SMAbv), U_SMBlw);
        }
        return tail(s, i + 1);
    }
    if (c == U_N) {
        *kind = KIND_OTHER;
        size_t e = i + 1;
        while (isAny(s, e, F(U_HN)) && isAny(s, e + 1, F(U_N))) e += 2;
        return optional(s, e, U_HN);
    }
    if (c == U_O) {
        /* (a character of no script's syllables: the marks after it are its, as after a base, or its symbol modifiers) */
        if (isAny(s, i + 1, F(U_SMAbv) | F(U_SMBlw))) {
            *kind = KIND_OTHER;
            return many(s, many(s, i + 1, U_SMAbv), U_SMBlw);
        }
        const size_t e = tail(s, i + 1);
        if (e == i + 1) *kind = KIND_OTHER;
        return e;
    }
    if (c == U_WJ || c == U_ZWNJ || c == U_CGJ) {
        *kind = KIND_OTHER;
        return i + 1;
    }
    /* a broken cluster: what would follow a base, with none (a repha may lead it) */
    *kind = KIND_BROKEN;
    const size_t from = c == U_R ? i + 1 : i;
    const size_t e = tail(s, from);
    return e > i ? e : i + 1;
}

/* ---- reordering --------------------------------------------------------------------------------------------------- */

#define POST_BASE                                                                                                                      \
    (F(U_FAbv) | F(U_FBlw) | F(U_FPst) | F(U_FMAbv) | F(U_FMBlw) | F(U_FMPst) | F(U_MAbv) | F(U_MBlw) | F(U_MPst) | F(U_MPre) | \
     F(U_VAbv) | F(U_VBlw) | F(U_VPst) | F(U_VPre) | F(U_VMAbv) | F(U_VMBlw) | F(U_VMPst) | F(U_VMPre))

static bool isHalant(const CscdOtGlyph* g) {
    const UseCategory c = categoryOf(g);
    return (c == U_H || c == U_HVM || c == U_IS) && g->ligComponents <= 1;
}

static void reorderCluster(CscdOtGlyph* g, size_t n) {
    if (!n) return;
    const unsigned kind = (g[0].mask & U_KIND) >> U_KIND_SHIFT;
    if (kind != KIND_REORDERED && kind != KIND_BROKEN) return;
    /* the repha: to before the first glyph after the base's consonants, or to the end */
    if (categoryOf(&g[0]) == U_R && n > 1) {
        for (size_t i = 1; i < n; ++i) {
            const bool post = (POST_BASE >> categoryOf(&g[i]) & 1) || isHalant(&g[i]);
            if (post || i == n - 1) {
                const size_t to = post ? i - 1 : i;
                const CscdOtGlyph t = g[0];
                memmove(&g[0], &g[1], to * sizeof *g);
                g[to] = t;
                break;
            }
        }
    }
    /* pre-base glyphs: to the start, or after the last halant before them */
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        const UseCategory c = categoryOf(&g[i]);
        if (isHalant(&g[i])) j = i + 1;
        else if ((c == U_VPre || c == U_VMPre) && j < i && !(i > 0 && g[i - 1].ch == g[i].ch && g[i - 1].glyph != g[i].glyph)) {
            const CscdOtGlyph t = g[i];
            memmove(&g[j + 1], &g[j], (i - j) * sizeof *g);
            g[j] = t;
        }
    }
}

/* ---- the stages --------------------------------------------------------------------------------------------------- */

/* features applied to each cluster on its own (no lookup reaching into the next); with `mark` set, the first glyph
 * of each a lookup substituted (among those the features apply to) is marked with it */
static void byCluster(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const CscdOtFeature* features, size_t n,
                      uint32_t mark) {
    CscdOtBuffer one = {0}, out = {0};
    for (size_t s = 0; s < buf->len;) {
        size_t e = s + 1;
        while (e < buf->len && buf->items[e].cluster == buf->items[s].cluster) ++e;
        one.len = 0;
        cscdVecAppend(&one, buf->items + s, e - s);
        for (size_t k = 0; k < one.len; ++k) one.items[k].substituted = false;
        cscdOtSubstitute(face, script, &one, features, n);
        if (mark)
            for (size_t k = 0; k < one.len && (one.items[k].mask & features[0].mask); ++k)
                if (one.items[k].substituted) {
                    one.items[k].mask |= mark;
                    break;
                }
        cscdVecAppend(&out, one.items, one.len);
        s = e;
    }
    CscdOtBuffer t = *buf;
    *buf = out;
    cscdVecFree(&t);
    cscdVecFree(&one);
}

/* A character's canonical decomposition, as glyphs of the font (false when it has none, or the font has not the glyphs
 * of its parts). */
static bool decompose(const CscdFontFace* face, const CscdOtGlyph* g, uint32_t cp, CscdOtBuffer* out) {
    const uint32_t* d = decompositionOf(cp);
    if (!d || (d[2] && !cscdFontFaceGlyphIndex(face, d[2]))) return false;
    if (!decompose(face, g, d[1], out)) {
        const uint16_t first = cscdFontFaceGlyphIndex(face, d[1]);
        if (!first) return false;
        CscdOtGlyph a = *g;
        a.glyph = first;
        a.ch = d[1];
        cscdVecPush(out, a);
    }
    if (d[2]) {
        CscdOtGlyph b = *g;
        b.glyph = cscdFontFaceGlyphIndex(face, d[2]);
        b.ch = d[2];
        cscdVecPush(out, b);
    }
    return true;
}

/* The characters normalized as HarfBuzz does it for this shaper: each decomposed (canonically, all the way: split
 * vowels into their parts); the marks of a run of them in the canonical order (by combining class); then, when the
 * text has marks after a character, a mark composed back with the character before it (not a mark itself: no split
 * vowel comes back together) when the font has the composite. */
static void normalize(const CscdFontFace* face, CscdOtBuffer* buf) {
    bool decomposable = false, marks = false;
    for (size_t i = 0; i < buf->len; ++i) {
        decomposable |= decompositionOf(buf->items[i].ch) != NULL;
        marks |= i > 0 && isMark(buf->items[i].ch);
    }
    if (!decomposable) return;
    CscdOtBuffer out = {0};
    for (size_t i = 0; i < buf->len; ++i)
        if (!decompose(face, &buf->items[i], buf->items[i].ch, &out)) cscdVecPush(&out, buf->items[i]);
    for (size_t i = 1; i < out.len; ++i) { /* (an insertion sort: the runs are short, and it keeps equal classes in order) */
        const int cc = combiningClass(out.items[i].ch);
        if (!cc) continue;
        size_t k = i;
        const CscdOtGlyph g = out.items[i];
        while (k > 0 && combiningClass(out.items[k - 1].ch) > cc) {
            out.items[k] = out.items[k - 1];
            --k;
        }
        out.items[k] = g;
    }
    if (marks) {
        size_t w = 0, starter = 0;
        bool haveStarter = false;
        for (size_t i = 0; i < out.len; ++i) {
            const CscdOtGlyph cur = out.items[i];
            if (haveStarter && isMark(cur.ch) && !isMark(out.items[starter].ch) &&
                (starter == w - 1 || combiningClass(out.items[w - 1].ch) < combiningClass(cur.ch))) {
                const uint32_t composed = composition(out.items[starter].ch, cur.ch);
                const uint16_t glyph = composed ? cscdFontFaceGlyphIndex(face, composed) : 0;
                if (glyph) {
                    out.items[starter].ch = composed;
                    out.items[starter].glyph = glyph;
                    continue;
                }
            }
            out.items[w++] = cur;
            if (!combiningClass(cur.ch)) {
                starter = w - 1;
                haveStarter = true;
            }
        }
        out.len = w;
    }
    CscdOtBuffer t = *buf;
    *buf = out;
    cscdVecFree(&t);
}

bool cscdUseShape(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const CscdOtFeature* features, size_t n) {
    if (!cscdUseScript(script) || !buf->len) return false;
    normalize(face, buf);
    /* the clusters: each one's glyphs merged into one cluster, of one kind; a broken one's dotted circle */
    /* (joiners and variation selectors pass inside a cluster: the grammar sees past them) */
    CSCD_VEC(UseCategory) cats = {0}, seen = {0};
    CscdU32Vec at = {0};
    for (size_t i = 0; i < buf->len; ++i) cscdVecPush(&cats, useCategoryOf(buf->items[i].ch));
    for (size_t i = 0; i < buf->len; ++i) {
        bool skip = cats.items[i] == U_CGJ && i;
        if (cats.items[i] == U_ZWNJ && i) { /* (a ZWNJ before a mark: as if it were not there) */
            size_t k = i + 1;
            while (k < buf->len && cats.items[k] == U_CGJ) ++k;
            skip = k < buf->len && isMark(buf->items[k].ch);
        }
        if (!skip) {
            cscdVecPush(&seen, cats.items[i]);
            cscdVecPush(&at, (uint32_t)i);
        }
    }
    const uint16_t dotted = cscdFontFaceGlyphIndex(face, 0x25CC);
    CscdOtBuffer out = {0};
    for (size_t v = 0, s = 0; s < buf->len;) {
        int kind;
        const Cats view = {seen.items, seen.len};
        const size_t end = clusterAt(&view, v, &kind);
        const size_t e = end < at.len ? at.items[end] : buf->len;
        v = end;
        const uint32_t cl = buf->items[s].cluster;
        const size_t first = out.len;
        for (size_t k = s; k < e; ++k) {
            CscdOtGlyph g = buf->items[k];
            g.cluster = cl;
            g.mask = (g.mask & ~(U_RPHF | U_REPHA | U_PREF | U_KIND)) | (uint32_t)kind << U_KIND_SHIFT;
            if (kind == KIND_BROKEN && dotted && k == s + (cats.items[s] == U_R)) {
                CscdOtGlyph d = g;
                d.glyph = dotted;
                d.ch = 0x25CC;
                d.ligComponents = 0;
                cscdVecPush(&out, d);
            }
            cscdVecPush(&out, g);
        }
        if (kind == KIND_BROKEN && dotted && cats.items[s] == U_R && e == s + 1) {
            CscdOtGlyph d = out.items[out.len - 1];
            d.glyph = dotted;
            d.ch = 0x25CC;
            cscdVecPush(&out, d);
        }
        /* rphf looks at the first glyphs: a repha character alone, or up to three */
        const size_t len = out.len - first;
        const size_t limit = categoryOf(&out.items[first]) == U_R ? 1 : (len < 3 ? len : 3);
        for (size_t k = 0; k < limit; ++k) out.items[first + k].mask |= U_RPHF;
        s = e;
    }
    cscdVecFree(&cats);
    cscdVecFree(&seen);
    cscdVecFree(&at);
    CscdOtBuffer t = *buf;
    *buf = out;
    cscdVecFree(&t);
    /* the stages */
    const CscdOtFeature first[] = {{tagOf("locl"), 1, ~0u}, {tagOf("ccmp"), 1, ~0u}, {tagOf("nukt"), 1, ~0u}, {tagOf("akhn"), 1, ~0u}};
    byCluster(face, script, buf, first, CSCD_COUNT(first), 0);
    const CscdOtFeature rphf = {tagOf("rphf"), 1, U_RPHF};
    byCluster(face, script, buf, &rphf, 1, U_REPHA);
    const CscdOtFeature pref = {tagOf("pref"), 1, ~0u};
    byCluster(face, script, buf, &pref, 1, U_PREF);
    const CscdOtFeature basic[] = {{tagOf("rkrf"), 1, ~0u}, {tagOf("abvf"), 1, ~0u}, {tagOf("blwf"), 1, ~0u}, {tagOf("half"), 1, ~0u},
                                   {tagOf("pstf"), 1, ~0u}, {tagOf("vatu"), 1, ~0u}, {tagOf("cjct"), 1, ~0u}};
    byCluster(face, script, buf, basic, CSCD_COUNT(basic), 0);
    for (size_t s = 0; s < buf->len;) {
        size_t e = s + 1;
        while (e < buf->len && buf->items[e].cluster == buf->items[s].cluster) ++e;
        reorderCluster(buf->items + s, e - s);
        s = e;
    }
    /* abvs, blws, haln, pres and psts, with the rest of the features (one stage: their lookups in the font's order; a
     * feature already applied is not applied again: ccmp would undo what blws made of its decomposition) */
    static const char kStaged[][5] = {"locl", "ccmp", "nukt", "akhn", "rphf", "pref", "rkrf", "abvf", "blwf", "half",
                                      "pstf", "vatu", "cjct", "abvs", "blws", "haln", "pres", "psts"};
    CSCD_VEC(CscdOtFeature) rest = {0};
    static const char kTypographic[][5] = {"abvs", "blws", "haln", "pres", "psts"};
    for (size_t i = 0; i < CSCD_COUNT(kTypographic); ++i) cscdVecPush(&rest, ((CscdOtFeature){tagOf(kTypographic[i]), 1, ~0u}));
    for (size_t i = 0; i < n; ++i) {
        bool staged = false;
        for (size_t k = 0; k < CSCD_COUNT(kStaged) && !staged; ++k) staged = features[i].tag == tagOf(kStaged[k]);
        if (!staged) cscdVecPush(&rest, features[i]);
    }
    cscdOtSubstitute(face, script, buf, rest.items, rest.len);
    cscdVecFree(&rest);
    return true;
}
