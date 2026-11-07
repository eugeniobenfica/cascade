/* OpenType layout: the GSUB and GPOS lookups of the features asked for, in the font's order, over a
 * buffer of glyphs. GSUB: single, multiple, alternate, ligature, contextual, chained contextual,
 * extension and reverse chained substitutions. GPOS: single and pair adjustments, mark-to-base,
 * mark-to-ligature and mark-to-mark attachment, contextual and chained contextual positioning.
 * Lookup flags (ignored glyph classes, mark attachment types, mark filtering sets) come from GDEF. */
#include "text/fontInternal.h"

#include <stdlib.h>
#include <string.h>

/* ---- GDEF ------------------------------------------------------------------------------------ */

int cscdOtGlyphClass(const CscdFontFace* face, uint16_t glyph) {
    const CscdFontFaceImpl* m = face->impl;
    if (!m->gdef || !within(m, m->gdef, 12)) return 0;
    const uint16_t off = u16(m->base + m->gdef + 4);
    return off ? otClassAt(m, m->gdef + off, glyph) : 0;
}

static int markAttachClass(const CscdFontFaceImpl* m, uint16_t glyph) {
    if (!m->gdef || !within(m, m->gdef, 12)) return 0;
    const uint16_t off = u16(m->base + m->gdef + 10);
    return off ? otClassAt(m, m->gdef + off, glyph) : 0;
}

static bool inMarkSet(const CscdFontFaceImpl* m, uint16_t set, uint16_t glyph) {
    if (!m->gdef || !within(m, m->gdef, 14) || u16(m->base + m->gdef + 2) < 2) return false; /* (GDEF 1.2) */
    const uint32_t sets = m->gdef + u16(m->base + m->gdef + 12);
    if (!within(m, sets, 4) || set >= u16(m->base + sets + 2) || !within(m, sets + 4 + 4u * set, 4)) return false;
    const uint32_t cov = sets + u32(m->base + sets + 4 + 4 * set);
    return otCoverageAt(m, cov, glyph) >= 0;
}

bool cscdOtHasPositioning(const CscdFontFace* face) { return face->impl->gpos != 0; }

/* ---- plans: the lookups of a script's features ----------------------------------------------- */

typedef struct PlanLookup { uint16_t index; int value; uint32_t mask; } PlanLookup;
typedef CSCD_VEC(PlanLookup) PlanLookupVec;

static uint32_t scriptTable(const CscdFontFaceImpl* m, uint32_t table, uint32_t script) {
    const uint32_t sl = table + u16(m->base + table + 4);
    if (!within(m, sl, 2)) return 0;
    const uint16_t n = u16(m->base + sl);
    if (!within(m, sl + 2, 6u * n)) return 0;
    /* (fonts made for the first Indic model have the old tags) */
    static const char* kOld[][2] = {{"dev2", "deva"}, {"bng2", "beng"}, {"gur2", "guru"}, {"gjr2", "gujr"}, {"ory2", "orya"},
                                    {"tml2", "taml"}, {"tel2", "telu"}, {"knd2", "knda"}, {"mlm2", "mlym"}, {"mym2", "mymr"}};
    uint32_t old = script;
    for (size_t i = 0; i < CSCD_COUNT(kOld); ++i) if (script == tag(kOld[i][0])) old = tag(kOld[i][1]);
    const uint32_t tries[5] = {script, old, tag("DFLT"), tag("dflt"), tag("latn")};
    for (int t = 0; t < 5; ++t)
        for (uint16_t i = 0; i < n; ++i) {
            const uint8_t* rec = m->base + sl + 2 + 6 * i;
            if (u32(rec) == tries[t]) return sl + u16(rec + 4);
        }
    return 0;
}

static void addLookup(PlanLookupVec* v, uint16_t index, int value, uint32_t mask) {
    for (size_t i = 0; i < v->len; ++i)
        if (v->items[i].index == index) {
            v->items[i].mask |= mask;
            if (value > v->items[i].value) v->items[i].value = value;
            return;
        }
    cscdVecPush(v, ((PlanLookup){index, value, mask}));
}

static int comparePlanLookups(const void* a, const void* b, void* ctx) {
    (void)ctx;
    return (int)((const PlanLookup*)a)->index - (int)((const PlanLookup*)b)->index;
}

static void collect(const CscdFontFaceImpl* m, uint32_t table, uint32_t script, const CscdOtFeature* feats, size_t n, PlanLookupVec* out) {
    if (!table || !within(m, table, 10)) return;
    const uint32_t st = scriptTable(m, table, script);
    if (!st || !within(m, st, 4)) return;
    uint32_t ls = u16(m->base + st) ? st + u16(m->base + st) : 0;
    if (!ls && u16(m->base + st + 2) && within(m, st + 4, 6)) ls = st + u16(m->base + st + 8); /* (no default: the first language) */
    if (!ls || !within(m, ls, 6)) return;
    const uint32_t fl = table + u16(m->base + table + 6), ll = table + u16(m->base + table + 8);
    if (!within(m, fl, 2) || !within(m, ll, 2)) return;
    const uint16_t nf = u16(m->base + fl), nl = u16(m->base + ll);
    const uint16_t required = u16(m->base + ls + 2), count = u16(m->base + ls + 4);
    if (!within(m, ls + 6, 2u * count)) return;
    for (int k = -1; k < (int)count; ++k) {
        const uint16_t fi = k < 0 ? required : u16(m->base + ls + 6 + 2 * k);
        if (fi >= nf || !within(m, fl + 2 + 6u * fi, 6)) continue;
        const uint8_t* rec = m->base + fl + 2 + 6 * fi;
        int value = k < 0 ? 1 : 0;
        uint32_t mask = k < 0 ? ~0u : 0;
        if (k >= 0)
            for (size_t f = 0; f < n; ++f)
                if (feats[f].tag == u32(rec)) {
                    if (feats[f].value) { value = feats[f].value; mask |= feats[f].mask; }
                    else mask = 0, value = 0; /* (a later "off" turns it off) */
                }
        if (!value || !mask) continue;
        const uint32_t ft = fl + u16(rec + 4);
        if (!within(m, ft, 4)) continue;
        const uint16_t nlk = u16(m->base + ft + 2);
        if (!within(m, ft + 4, 2u * nlk)) continue;
        for (uint16_t q = 0; q < nlk; ++q) {
            const uint16_t li = u16(m->base + ft + 4 + 2 * q);
            if (li < nl) addLookup(out, li, value, mask);
        }
    }
    cscdSort(out->items, out->len, sizeof(PlanLookup), comparePlanLookups, NULL);
}

/* The plan for a table, script and features, made once per face. */
static const PlanLookupVec* planFor(const CscdFontFace* face, uint32_t table, uint32_t script, const CscdOtFeature* feats, size_t n) {
    CscdFontFaceImpl* m = face->impl;
    CscdBytes key = {0};
    cscdVecAppend(&key, (const uint8_t*)&table, 4);
    cscdVecAppend(&key, (const uint8_t*)&script, 4);
    for (size_t i = 0; i < n; ++i) cscdVecAppend(&key, (const uint8_t*)&feats[i], sizeof(CscdOtFeature));
    const CscdSv k = cscdSvMake((const char*)key.items, key.len);
    cscdMutexLock(&m->cacheMutex);
    CscdMapVal* have = cscdStrMapGet(&m->plans, k);
    PlanLookupVec* plan = have ? (PlanLookupVec*)have->p : NULL;
    if (!plan) {
        plan = CSCD_NEW(PlanLookupVec);
        collect(m, table, script, feats, n, plan);
        cscdStrMapPut(&m->plans, k, NULL)->p = plan;
    }
    cscdMutexUnlock(&m->cacheMutex);
    cscdVecFree(&key);
    return plan;
}

bool cscdOtHasFeature(const CscdFontFace* face, uint32_t script, uint32_t feature) {
    const CscdOtFeature f = {feature, 1, ~0u};
    return face->impl->gsub && planFor(face, face->impl->gsub, script, &f, 1)->len > 0;
}

void cscdOtFree(CscdFontFaceImpl* m) {
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&m->plans, s));) {
        cscdVecFree((PlanLookupVec*)s->val.p);
        cscdFree(s->val.p);
    }
    cscdStrMapFree(&m->plans);
}

/* ---- applying lookups ------------------------------------------------------------------------ */

typedef struct Ctx {
    const CscdFontFace* face;
    const CscdFontFaceImpl* m;
    const uint8_t* b; /* the font's bytes */
    uint32_t lookups; /* the lookup list */
    bool gpos;
    CscdOtBuffer* buf;
    uint16_t flag, markSet;
    int value; /* (the feature's value: which alternate) */
    int depth;
    size_t maxLen;    /* how long substitutions may make the text */
    size_t* work;     /* subtables that may still be tried in this run (shared by the copies) */
} Ctx;

static void refreshClass(Ctx* c, CscdOtGlyph* g) { g->gclass = (uint8_t)cscdOtGlyphClass(c->face, g->glyph); }

/* What the lookup's flags pass over (glyph classes, mark types). */
static bool flagged(const Ctx* c, const CscdOtGlyph* g) {
    const int cls = g->gclass;
    if (((c->flag & 2) && cls == 1) || ((c->flag & 4) && cls == 2) || ((c->flag & 8) && cls == 3)) return true;
    if (cls == 3) {
        if (c->flag & 0x10) return !inMarkSet(c->m, c->markSet, g->glyph);
        if (c->flag & 0xFF00) return markAttachClass(c->m, g->glyph) != (c->flag >> 8);
    }
    return false;
}

/* ... and, besides, default ignorable characters (variation selectors and the like). */
static bool ignored(const Ctx* c, const CscdOtGlyph* g) { return g->ignorable || flagged(c, g); }

static size_t nextOf(const Ctx* c, size_t i) {
    for (++i; i < c->buf->len && ignored(c, &c->buf->items[i]); ++i) {}
    return i;
}

static ptrdiff_t prevOf(const Ctx* c, ptrdiff_t i) {
    for (--i; i >= 0 && ignored(c, &c->buf->items[i]); --i) {}
    return i;
}

static bool covered(const Ctx* c, uint32_t cov, uint16_t glyph) { return otCoverageAt(c->m, cov, glyph) >= 0; }
static int coverIndex(const Ctx* c, uint32_t cov, uint16_t glyph) { return otCoverageAt(c->m, cov, glyph); }
static int classIn(const Ctx* c, uint32_t cd, uint16_t glyph) { return cd ? otClassAt(c->m, cd, glyph) : 0; }

static bool applyAt(Ctx* c, uint16_t lookupIndex, size_t pos, size_t* next);

/* ---- contexts: matching sequences, then the lookups they call ---- */

typedef enum { MATCH_GLYPH, MATCH_CLASS, MATCH_COVERAGE } MatchKind;
typedef struct Matcher { MatchKind kind; uint32_t base; /* (class definition, or where coverage offsets count from) */ } Matcher;

static bool matches(const Ctx* c, const Matcher* mt, uint16_t glyph, uint16_t value) {
    switch (mt->kind) {
    case MATCH_GLYPH: return glyph == value;
    case MATCH_CLASS: return classIn(c, mt->base, glyph) == value;
    default: return covered(c, mt->base + value, glyph);
    }
}

#define MAX_CONTEXT 64

/* The next glyph from i (step 1 or -1) that is value: glyphs the flags pass over are passed over,
 * and so is an ignorable character unless it is the one asked for (as HarfBuzz does); -1 when
 * the next one is something else. */
static ptrdiff_t stepMatch(const Ctx* c, ptrdiff_t i, int step, const Matcher* mt, uint16_t value) {
    for (ptrdiff_t j = i + step; j >= 0 && (size_t)j < c->buf->len; j += step) {
        const CscdOtGlyph* g = &c->buf->items[j];
        if (flagged(c, g)) continue;
        if (matches(c, mt, g->glyph, value)) return j;
        if (g->ignorable) continue;
        return -1;
    }
    return -1;
}

/* The input sequence from pos (its first glyph already matched): count glyphs, the rest from values. */
static bool matchInput(const Ctx* c, size_t pos, int count, const uint8_t* values, const Matcher* mt, size_t* positions) {
    if (count < 1 || count > MAX_CONTEXT) return false;
    positions[0] = pos;
    ptrdiff_t at = (ptrdiff_t)pos;
    for (int k = 1; k < count; ++k) {
        at = stepMatch(c, at, 1, mt, u16(values + 2 * (k - 1)));
        if (at < 0) return false;
        positions[k] = (size_t)at;
    }
    return true;
}

static bool matchBacktrack(const Ctx* c, size_t pos, int count, const uint8_t* values, const Matcher* mt) {
    ptrdiff_t at = (ptrdiff_t)pos;
    for (int k = 0; k < count && at >= 0; ++k) at = stepMatch(c, at, -1, mt, u16(values + 2 * k));
    return at >= 0;
}

static bool matchLookahead(const Ctx* c, size_t last, int count, const uint8_t* values, const Matcher* mt) {
    ptrdiff_t at = (ptrdiff_t)last;
    for (int k = 0; k < count && at >= 0; ++k) at = stepMatch(c, at, 1, mt, u16(values + 2 * k));
    return at >= 0;
}

/* A font can ask for anything: 65535 lookups of 65535 subtables, contexts that call contexts (64
 * lookups each, 8 deep), or one glyph made into 65535, again and again. Real fonts stay far below
 * these limits (a thousand lookups of sixteen subtables, at every glyph, takes a quarter of this). */
#define MIN_MAX_GLYPHS 16384
#define MAX_GROWTH 64
#define WORK_PER_GLYPH 65536
#define MIN_WORK (1u << 22)

/* The lookups a matched context calls, each at its glyph of the input. */
static void applyRecords(Ctx* c, size_t* positions, int count, const uint8_t* records, int nrec, size_t* next) {
    size_t end = positions[count - 1] + 1;
    for (int r = 0; r < nrec; ++r) {
        const int seq = u16(records + 4 * r);
        const uint16_t li = u16(records + 4 * r + 2);
        if (seq >= count || c->depth > 8) continue;
        const size_t before = c->buf->len;
        size_t ignore;
        Ctx saved = *c;
        ++c->depth;
        applyAt(c, li, positions[seq], &ignore);
        *c = saved;
        const ptrdiff_t delta = (ptrdiff_t)c->buf->len - (ptrdiff_t)before;
        if (delta) {
            for (int k = seq + 1; k < count; ++k) positions[k] = (size_t)((ptrdiff_t)positions[k] + delta);
            end = (size_t)((ptrdiff_t)end + delta);
        }
    }
    *next = end > positions[0] ? end : positions[0] + 1;
}

/* A rule of a (non-chained) context: glyph count, record count, the input values, the records. */
static bool contextRule(Ctx* c, uint32_t rule, size_t pos, const Matcher* mt, size_t* next) {
    if (!within(c->m, rule, 4)) return false;
    const int count = u16(c->b + rule), nrec = u16(c->b + rule + 2);
    if (!within(c->m, rule + 4, 2u * (count ? count - 1 : 0) + 4u * nrec)) return false;
    size_t positions[MAX_CONTEXT];
    if (!matchInput(c, pos, count, c->b + rule + 4, mt, positions)) return false;
    applyRecords(c, positions, count, c->b + rule + 4 + 2 * (count - 1), nrec, next);
    return true;
}

/* A chained rule: backtrack, input, lookahead, records. */
static bool chainRule(Ctx* c, uint32_t rule, size_t pos, const Matcher* back, const Matcher* in, const Matcher* ahead, size_t* next) {
    uint32_t p = rule;
    if (!within(c->m, p, 2)) return false;
    const int nb = u16(c->b + p);
    const uint32_t backVals = p + 2;
    p += 2 + 2u * nb;
    if (!within(c->m, p, 2)) return false;
    const int ni = u16(c->b + p);
    const uint32_t inVals = p + 2;
    p += 2 + 2u * (ni ? ni - 1 : 0);
    if (!within(c->m, p, 2)) return false;
    const int na = u16(c->b + p);
    const uint32_t aheadVals = p + 2;
    p += 2 + 2u * na;
    if (!within(c->m, p, 2)) return false;
    const int nrec = u16(c->b + p);
    if (!within(c->m, p + 2, 4u * nrec)) return false;
    size_t positions[MAX_CONTEXT];
    if (!matchInput(c, pos, ni, c->b + inVals, in, positions)) return false;
    if (!matchBacktrack(c, pos, nb, c->b + backVals, back)) return false;
    if (!matchLookahead(c, positions[ni - 1], na, c->b + aheadVals, ahead)) return false;
    applyRecords(c, positions, ni, c->b + p + 2, nrec, next);
    return true;
}

static bool applyContext(Ctx* c, uint32_t st, size_t pos, size_t* next) {
    const uint16_t fmt = u16(c->b + st);
    const uint16_t glyph = c->buf->items[pos].glyph;
    if (fmt == 1 || fmt == 2) {
        const int ci = coverIndex(c, st + u16(c->b + st + 2), glyph);
        if (ci < 0) return false;
        Matcher mt = {MATCH_GLYPH, 0};
        int setIndex = ci;
        uint32_t sets = st + 4;
        if (fmt == 2) {
            mt.kind = MATCH_CLASS;
            mt.base = st + u16(c->b + st + 4);
            setIndex = classIn(c, mt.base, glyph);
            sets = st + 6;
        }
        if (!within(c->m, sets, 2) || setIndex >= u16(c->b + sets) || !within(c->m, sets + 2 + 2u * setIndex, 2)) return false;
        const uint16_t so = u16(c->b + sets + 2 + 2 * setIndex);
        if (!so) return false;
        const uint32_t set = st + so;
        if (!within(c->m, set, 2)) return false;
        const uint16_t nr = u16(c->b + set);
        for (uint16_t r = 0; r < nr && within(c->m, set + 2 + 2u * r, 2); ++r)
            if (contextRule(c, set + u16(c->b + set + 2 + 2 * r), pos, &mt, next)) return true;
        return false;
    }
    if (fmt == 3) {
        if (!within(c->m, st, 6)) return false;
        const int count = u16(c->b + st + 2), nrec = u16(c->b + st + 4);
        if (count < 1 || !within(c->m, st + 6, 2u * count + 4u * nrec)) return false;
        if (!covered(c, st + u16(c->b + st + 6), glyph)) return false;
        const Matcher mt = {MATCH_COVERAGE, st};
        size_t positions[MAX_CONTEXT];
        if (!matchInput(c, pos, count, c->b + st + 8, &mt, positions)) return false;
        applyRecords(c, positions, count, c->b + st + 6 + 2 * count, nrec, next);
        return true;
    }
    return false;
}

static bool applyChain(Ctx* c, uint32_t st, size_t pos, size_t* next) {
    const uint16_t fmt = u16(c->b + st);
    const uint16_t glyph = c->buf->items[pos].glyph;
    if (fmt == 1 || fmt == 2) {
        const int ci = coverIndex(c, st + u16(c->b + st + 2), glyph);
        if (ci < 0) return false;
        Matcher back = {MATCH_GLYPH, 0}, in = back, ahead = back;
        int setIndex = ci;
        uint32_t sets = st + 4;
        if (fmt == 2) {
            back = (Matcher){MATCH_CLASS, st + u16(c->b + st + 4)};
            in = (Matcher){MATCH_CLASS, st + u16(c->b + st + 6)};
            ahead = (Matcher){MATCH_CLASS, st + u16(c->b + st + 8)};
            if (!u16(c->b + st + 4)) back.base = 0;
            if (!u16(c->b + st + 8)) ahead.base = 0;
            setIndex = classIn(c, in.base, glyph);
            sets = st + 10;
        }
        if (!within(c->m, sets, 2) || setIndex >= u16(c->b + sets) || !within(c->m, sets + 2 + 2u * setIndex, 2)) return false;
        const uint16_t so = u16(c->b + sets + 2 + 2 * setIndex);
        if (!so) return false;
        const uint32_t set = st + so;
        if (!within(c->m, set, 2)) return false;
        const uint16_t nr = u16(c->b + set);
        for (uint16_t r = 0; r < nr && within(c->m, set + 2 + 2u * r, 2); ++r)
            if (chainRule(c, set + u16(c->b + set + 2 + 2 * r), pos, &back, &in, &ahead, next)) return true;
        return false;
    }
    if (fmt == 3) {
        /* coverages throughout: the rule is the subtable itself, after its format */
        const Matcher mt = {MATCH_COVERAGE, st};
        uint32_t p = st + 2;
        if (!within(c->m, p, 2)) return false;
        const int nb = u16(c->b + p);
        const uint32_t backVals = p + 2;
        p += 2 + 2u * nb;
        if (!within(c->m, p, 2)) return false;
        const int ni = u16(c->b + p);
        if (ni < 1 || !within(c->m, p + 2, 2u * ni)) return false;
        if (!covered(c, st + u16(c->b + p + 2), glyph)) return false;
        const uint32_t inVals = p + 4;
        p += 2 + 2u * ni;
        if (!within(c->m, p, 2)) return false;
        const int na = u16(c->b + p);
        const uint32_t aheadVals = p + 2;
        p += 2 + 2u * na;
        if (!within(c->m, p, 2)) return false;
        const int nrec = u16(c->b + p);
        if (!within(c->m, p + 2, 4u * nrec)) return false;
        size_t positions[MAX_CONTEXT];
        if (!matchInput(c, pos, ni, c->b + inVals, &mt, positions)) return false;
        if (!matchBacktrack(c, pos, nb, c->b + backVals, &mt)) return false;
        if (!matchLookahead(c, positions[ni - 1], na, c->b + aheadVals, &mt)) return false;
        applyRecords(c, positions, ni, c->b + p + 2, nrec, next);
        return true;
    }
    return false;
}

/* ---- GSUB ---- */

static void setGlyph(Ctx* c, size_t i, uint16_t glyph) {
    c->buf->items[i].glyph = glyph;
    refreshClass(c, &c->buf->items[i]);
}

static bool gsubSingle(Ctx* c, uint32_t st, size_t pos) {
    CscdOtGlyph* g = &c->buf->items[pos];
    const int ci = coverIndex(c, st + u16(c->b + st + 2), g->glyph);
    if (ci < 0) return false;
    const uint16_t fmt = u16(c->b + st);
    if (fmt == 1) setGlyph(c, pos, (uint16_t)(g->glyph + s16(c->b + st + 4)));
    else if (fmt == 2 && ci < u16(c->b + st + 4) && within(c->m, st + 6 + 2u * ci, 2)) setGlyph(c, pos, u16(c->b + st + 6 + 2 * ci));
    else return false;
    return true;
}

static bool gsubMultiple(Ctx* c, uint32_t st, size_t pos, size_t* next) {
    const int ci = coverIndex(c, st + u16(c->b + st + 2), c->buf->items[pos].glyph);
    if (ci < 0 || ci >= u16(c->b + st + 4) || !within(c->m, st + 6 + 2u * ci, 2)) return false;
    const uint32_t seq = st + u16(c->b + st + 6 + 2 * ci);
    if (!within(c->m, seq, 2)) return false;
    const uint16_t n = u16(c->b + seq);
    if (!within(c->m, seq + 2, 2u * n) || c->buf->len + n > c->maxLen) return false;
    if (n == 0) { /* (deleted) */
        cscdVecRemove(c->buf, pos);
        *next = pos;
        return true;
    }
    const CscdOtGlyph proto = c->buf->items[pos];
    setGlyph(c, pos, u16(c->b + seq + 2));
    for (uint16_t k = 1; k < n; ++k) {
        CscdOtGlyph g = proto;
        g.glyph = u16(c->b + seq + 2 + 2 * k);
        refreshClass(c, &g);
        cscdVecInsert(c->buf, pos + k, g);
    }
    *next = pos + n;
    return true;
}

static bool gsubAlternate(Ctx* c, uint32_t st, size_t pos) {
    const int ci = coverIndex(c, st + u16(c->b + st + 2), c->buf->items[pos].glyph);
    if (ci < 0 || ci >= u16(c->b + st + 4) || !within(c->m, st + 6 + 2u * ci, 2)) return false;
    const uint32_t set = st + u16(c->b + st + 6 + 2 * ci);
    if (!within(c->m, set, 2)) return false;
    const uint16_t n = u16(c->b + set);
    const int which = cscdClampi(c->value, 1, n) - 1;
    if (!n || !within(c->m, set + 2 + 2u * which, 2)) return false;
    setGlyph(c, pos, u16(c->b + set + 2 + 2 * which));
    return true;
}

static bool gsubLigature(Ctx* c, uint32_t st, size_t pos) {
    const int ci = coverIndex(c, st + u16(c->b + st + 2), c->buf->items[pos].glyph);
    if (ci < 0 || ci >= u16(c->b + st + 4) || !within(c->m, st + 6 + 2u * ci, 2)) return false;
    const uint32_t set = st + u16(c->b + st + 6 + 2 * ci);
    if (!within(c->m, set, 2)) return false;
    const uint16_t nl = u16(c->b + set);
    for (uint16_t l = 0; l < nl && within(c->m, set + 2 + 2u * l, 2); ++l) {
        const uint32_t lig = set + u16(c->b + set + 2 + 2 * l);
        if (!within(c->m, lig, 4)) continue;
        const int comps = u16(c->b + lig + 2);
        if (comps < 1 || comps > MAX_CONTEXT || !within(c->m, lig + 4, 2u * (comps - 1))) continue;
        size_t positions[MAX_CONTEXT];
        const Matcher mt = {MATCH_GLYPH, 0};
        if (!matchInput(c, pos, comps, c->b + lig + 4, &mt, positions)) continue;
        setGlyph(c, pos, u16(c->b + lig));
        c->buf->items[pos].ligComponents = (uint8_t)comps;
        for (int k = comps - 1; k >= 1; --k) cscdVecRemove(c->buf, positions[k]); /* (glyphs skipped over stay) */
        return true;
    }
    return false;
}

static bool gsubReverse(Ctx* c, uint32_t st, size_t pos) {
    if (u16(c->b + st) != 1) return false;
    const int ci = coverIndex(c, st + u16(c->b + st + 2), c->buf->items[pos].glyph);
    if (ci < 0) return false;
    const Matcher mt = {MATCH_COVERAGE, st};
    uint32_t p = st + 4;
    if (!within(c->m, p, 2)) return false;
    const int nb = u16(c->b + p);
    if (!matchBacktrack(c, pos, nb, c->b + p + 2, &mt)) return false;
    p += 2 + 2u * nb;
    if (!within(c->m, p, 2)) return false;
    const int na = u16(c->b + p);
    if (!matchLookahead(c, pos, na, c->b + p + 2, &mt)) return false;
    p += 2 + 2u * na;
    if (!within(c->m, p, 2) || ci >= u16(c->b + p) || !within(c->m, p + 2 + 2u * ci, 2)) return false;
    setGlyph(c, pos, u16(c->b + p + 2 + 2 * ci));
    return true;
}

/* ---- GPOS ---- */

static int popcount(uint16_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

static void applyValue(const Ctx* c, const uint8_t* rec, uint16_t fmt, CscdOtGlyph* g) {
    (void)c;
    int at = 0;
    if (fmt & 1) { g->xOffset += s16(rec + at); at += 2; }
    if (fmt & 2) { g->yOffset += s16(rec + at); at += 2; }
    if (fmt & 4) { g->xAdvance += s16(rec + at); at += 2; }
}

static bool gposSingle(Ctx* c, uint32_t st, size_t pos) {
    CscdOtGlyph* g = &c->buf->items[pos];
    const int ci = coverIndex(c, st + u16(c->b + st + 2), g->glyph);
    if (ci < 0) return false;
    const uint16_t fmt = u16(c->b + st), vf = u16(c->b + st + 4);
    const int size = 2 * popcount(vf);
    if (fmt == 1 && within(c->m, st + 6, (uint32_t)size)) applyValue(c, c->b + st + 6, vf, g);
    else if (fmt == 2 && ci < u16(c->b + st + 6) && within(c->m, st + 8 + (uint32_t)(size * ci), (uint32_t)size)) applyValue(c, c->b + st + 8 + size * ci, vf, g);
    else return false;
    return true;
}

static bool gposPair(Ctx* c, uint32_t st, size_t pos, size_t* next) {
    CscdOtGlyph* first = &c->buf->items[pos];
    const int ci = coverIndex(c, st + u16(c->b + st + 2), first->glyph);
    if (ci < 0) return false;
    const size_t j = nextOf(c, pos);
    if (j >= c->buf->len) return false;
    CscdOtGlyph* second = &c->buf->items[j];
    const uint16_t fmt = u16(c->b + st), vf1 = u16(c->b + st + 4), vf2 = u16(c->b + st + 6);
    const int s1 = 2 * popcount(vf1), s2 = 2 * popcount(vf2);
    const uint8_t* rec = NULL;
    if (fmt == 1) {
        if (ci >= u16(c->b + st + 8) || !within(c->m, st + 10 + 2u * ci, 2)) return false;
        const uint32_t set = st + u16(c->b + st + 10 + 2 * ci);
        if (!within(c->m, set, 2)) return false;
        const int n = u16(c->b + set), size = 2 + s1 + s2;
        if (!within(c->m, set + 2, (uint32_t)(size * n))) return false;
        int lo = 0, hi = n - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            const uint8_t* r = c->b + set + 2 + size * mid;
            const uint16_t g = u16(r);
            if (g == second->glyph) { rec = r + 2; break; }
            if (g < second->glyph) lo = mid + 1;
            else hi = mid - 1;
        }
    } else if (fmt == 2) {
        const uint32_t cd1 = st + u16(c->b + st + 8), cd2 = st + u16(c->b + st + 10);
        const uint16_t n1 = u16(c->b + st + 12), n2 = u16(c->b + st + 14);
        const int c1 = classIn(c, cd1, first->glyph), c2 = classIn(c, cd2, second->glyph);
        if (c1 >= n1 || c2 >= n2) return false;
        const uint32_t at = st + 16 + (uint32_t)(c1 * n2 + c2) * (uint32_t)(s1 + s2);
        if (!within(c->m, at, (uint32_t)(s1 + s2))) return false;
        rec = c->b + at;
    }
    if (!rec) return false;
    applyValue(c, rec, vf1, first);
    applyValue(c, rec + s1, vf2, second);
    *next = vf2 ? j + 1 : j;
    return true;
}

static bool anchorAt(const Ctx* c, uint32_t a, int* x, int* y) {
    if (!within(c->m, a, 6)) return false;
    *x = s16(c->b + a + 2);
    *y = s16(c->b + a + 4);
    return true;
}

/* The mark at pos onto the glyph at base, by the anchors of its class: markArray (class, anchor),
 * and the anchor the base has for that class. */
static bool attachMark(Ctx* c, size_t pos, size_t base, uint32_t markArray, int markIndex, uint32_t baseAnchor) {
    if (!within(c->m, markArray + 2 + 4u * markIndex, 4)) return false;
    const uint32_t markAnchor = markArray + u16(c->b + markArray + 2 + 4 * markIndex + 2);
    int mx, my, bx, by;
    if (!anchorAt(c, markAnchor, &mx, &my) || !anchorAt(c, baseAnchor, &bx, &by)) return false;
    CscdOtGlyph* g = &c->buf->items[pos];
    g->xOffset = bx - mx;
    g->yOffset = by - my;
    g->attach = (int32_t)(pos - base);
    return true;
}

static bool gposMark(Ctx* c, uint32_t st, size_t pos, int type) {
    if (u16(c->b + st) != 1 || !within(c->m, st, 12)) return false;
    CscdOtGlyph* g = &c->buf->items[pos];
    const int mi = coverIndex(c, st + u16(c->b + st + 2), g->glyph);
    if (mi < 0) return false;
    const uint16_t classCount = u16(c->b + st + 6);
    const uint32_t markArray = st + u16(c->b + st + 8), baseArray = st + u16(c->b + st + 10);
    if (!within(c->m, markArray + 2 + 4u * mi, 4)) return false;
    const uint16_t cls = u16(c->b + markArray + 2 + 4 * mi);
    if (cls >= classCount) return false;
    /* the glyph it goes on: a base (or ligature) before it for 4 and 5, the mark just before for 6 */
    ptrdiff_t b = (ptrdiff_t)pos - 1;
    if (type == 6) {
        while (b >= 0 && ignored(c, &c->buf->items[b])) --b;
        if (b < 0 || c->buf->items[b].gclass != 3) return false;
    } else
        while (b >= 0 && (c->buf->items[b].gclass == 3 || (type == 4 && c->buf->items[b].gclass == 4))) --b;
    if (b < 0) return false;
    const int bi = coverIndex(c, st + u16(c->b + st + 4), c->buf->items[b].glyph);
    if (bi < 0 || !within(c->m, baseArray, 2) || bi >= u16(c->b + baseArray)) return false;
    uint32_t anchor;
    if (type == 5) { /* a ligature: the anchor of its last component */
        const uint32_t attach = baseArray + u16(c->b + baseArray + 2 + 2 * bi);
        if (!within(c->m, attach, 2)) return false;
        const uint16_t comps = u16(c->b + attach);
        const uint64_t item = (uint64_t)(comps - 1) * classCount + cls; /* (its last component's) */
        if (!comps || !withinItems(c->m, attach + 2, item + 1, 2)) return false;
        const uint16_t ao = u16(c->b + attach + 2 + 2 * (uint32_t)item);
        if (!ao) return false;
        anchor = attach + ao;
    } else {
        const uint64_t item = (uint64_t)bi * classCount + cls;
        if (!withinItems(c->m, baseArray + 2, item + 1, 2)) return false;
        const uint32_t at = baseArray + 2 + 2 * (uint32_t)item;
        if (!u16(c->b + at)) return false;
        anchor = baseArray + u16(c->b + at);
    }
    return attachMark(c, pos, (size_t)b, markArray, mi, anchor);
}

/* ---- lookups ---- */

static uint32_t lookupOffset(const Ctx* c, uint16_t index) {
    if (!within(c->m, c->lookups, 2) || index >= u16(c->b + c->lookups) || !within(c->m, c->lookups + 2 + 2u * index, 2)) return 0;
    const uint32_t lk = c->lookups + u16(c->b + c->lookups + 2 + 2 * index);
    return within(c->m, lk, 6) ? lk : 0;
}

static bool applySubtable(Ctx* c, uint16_t type, uint32_t st, size_t pos, size_t* next) {
    *next = pos + 1;
    if (!within(c->m, st, 4)) return false;
    if (!c->gpos) {
        if (type == 7) { /* extension (of anything but another: it could be itself) */
            if (!within(c->m, st, 8) || u16(c->b + st) != 1 || u16(c->b + st + 2) == 7) return false;
            return applySubtable(c, u16(c->b + st + 2), st + u32(c->b + st + 4), pos, next);
        }
        switch (type) {
        case 1: return gsubSingle(c, st, pos);
        case 2: return gsubMultiple(c, st, pos, next);
        case 3: return gsubAlternate(c, st, pos);
        case 4: return gsubLigature(c, st, pos);
        case 5: return applyContext(c, st, pos, next);
        case 6: return applyChain(c, st, pos, next);
        case 8: return gsubReverse(c, st, pos);
        default: return false;
        }
    }
    if (type == 9) {
        if (!within(c->m, st, 8) || u16(c->b + st) != 1 || u16(c->b + st + 2) == 9) return false;
        return applySubtable(c, u16(c->b + st + 2), st + u32(c->b + st + 4), pos, next);
    }
    switch (type) {
    case 1: return gposSingle(c, st, pos);
    case 2: return gposPair(c, st, pos, next);
    case 4: case 5: case 6: return gposMark(c, st, pos, type);
    case 7: return applyContext(c, st, pos, next);
    case 8: return applyChain(c, st, pos, next);
    default: return false;
    }
}

/* One lookup at one glyph (the lookup's own flags in force). */
static bool applyAt(Ctx* c, uint16_t lookupIndex, size_t pos, size_t* next) {
    const uint32_t lk = lookupOffset(c, lookupIndex);
    if (!lk || pos >= c->buf->len) return false;
    const uint16_t type = u16(c->b + lk), nsub = u16(c->b + lk + 4);
    c->flag = u16(c->b + lk + 2);
    c->markSet = (c->flag & 0x10) && within(c->m, lk + 6 + 2u * nsub, 2) ? u16(c->b + lk + 6 + 2 * nsub) : 0;
    if (ignored(c, &c->buf->items[pos])) return false;
    for (uint16_t s = 0; s < nsub && *c->work && within(c->m, lk + 6 + 2u * s, 2); ++s) {
        --*c->work;
        if (applySubtable(c, type, lk + u16(c->b + lk + 6 + 2 * s), pos, next)) return true;
    }
    return false;
}

static void run(const CscdFontFace* face, bool gpos, uint32_t script, CscdOtBuffer* buf, const CscdOtFeature* feats, size_t n) {
    const CscdFontFaceImpl* m = face->impl;
    const uint32_t table = gpos ? m->gpos : m->gsub;
    if (!table || !buf->len) return;
    const PlanLookupVec* plan = planFor(face, table, script, feats, n);
    size_t work = cscdMaxz(buf->len * WORK_PER_GLYPH, MIN_WORK);
    Ctx c = {face, m, m->base, table + u16(m->base + table + 8), gpos, buf, 0, 0, 1, 0, cscdMaxz(buf->len * MAX_GROWTH, MIN_MAX_GLYPHS), &work};
    for (size_t i = 0; i < buf->len; ++i) if (!buf->items[i].gclass) refreshClass(&c, &buf->items[i]);
    for (size_t l = 0; l < plan->len; ++l) {
        const PlanLookup* pl = &plan->items[l];
        const uint32_t lk = lookupOffset(&c, pl->index);
        if (!lk) continue;
        c.value = pl->value;
        c.depth = 0;
        if (!gpos && u16(m->base + lk) == 8) { /* reverse chaining: from the end */
            for (size_t i = buf->len; i-- > 0;) {
                size_t next;
                if (buf->items[i].mask & pl->mask) applyAt(&c, pl->index, i, &next);
            }
            continue;
        }
        for (size_t i = 0; i < buf->len;) {
            size_t next = i + 1;
            if ((buf->items[i].mask & pl->mask) && applyAt(&c, pl->index, i, &next)) i = next > i ? next : i + 1;
            else ++i;
        }
    }
}

void cscdOtSubstitute(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const CscdOtFeature* features, size_t n) {
    run(face, false, script, buf, features, n);
}

void cscdOtPosition(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const CscdOtFeature* features, size_t n) {
    run(face, true, script, buf, features, n);
}
