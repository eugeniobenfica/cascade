/* The Unicode bidirectional algorithm, UAX #9 (revision for Unicode 16): explicit embeddings,
 * overrides and isolates, isolating run sequences, the weak, bracket pair, neutral and implicit
 * rules, and the line rules L1 and L2. */
#include "text/bidiInternal.h"
#include "core/thread.h"

typedef struct BidiRange {
    uint32_t first, last;
    CscdBidiClass cls;
} BidiRange;

static const BidiRange kBidiRanges[] = {
#include "bidiData.inc"
};

typedef struct CpPair { uint32_t a, b; } CpPair;

/* Bidi_Mirroring_Glyph for the characters text uses (brackets, quotes, comparison and set signs). */
static const CpPair kMirrors[] = {
    {0x28, 0x29}, {0x3C, 0x3E}, {0x5B, 0x5D}, {0x7B, 0x7D}, {0xAB, 0xBB}, {0x0F3A, 0x0F3B}, {0x0F3C, 0x0F3D},
    {0x169B, 0x169C}, {0x2039, 0x203A}, {0x2045, 0x2046}, {0x207D, 0x207E}, {0x208D, 0x208E}, {0x2208, 0x220B},
    {0x2209, 0x220C}, {0x220A, 0x220D}, {0x2215, 0x29F5}, {0x223C, 0x223D}, {0x2243, 0x22CD}, {0x2252, 0x2253},
    {0x2254, 0x2255}, {0x2264, 0x2265}, {0x2266, 0x2267}, {0x2268, 0x2269}, {0x226A, 0x226B}, {0x226E, 0x226F},
    {0x2270, 0x2271}, {0x2272, 0x2273}, {0x2274, 0x2275}, {0x2276, 0x2277}, {0x2278, 0x2279}, {0x227A, 0x227B},
    {0x227C, 0x227D}, {0x227E, 0x227F}, {0x2280, 0x2281}, {0x2282, 0x2283}, {0x2284, 0x2285}, {0x2286, 0x2287},
    {0x2288, 0x2289}, {0x228A, 0x228B}, {0x228F, 0x2290}, {0x2291, 0x2292}, {0x2298, 0x29B8}, {0x22A2, 0x22A3},
    {0x22A6, 0x2ADE}, {0x22B0, 0x22B1}, {0x22B2, 0x22B3}, {0x22B4, 0x22B5}, {0x22B6, 0x22B7}, {0x22C9, 0x22CA},
    {0x22CB, 0x22CC}, {0x22D0, 0x22D1}, {0x22D6, 0x22D7}, {0x22D8, 0x22D9}, {0x22DA, 0x22DB}, {0x22DC, 0x22DD},
    {0x22DE, 0x22DF}, {0x22E0, 0x22E1}, {0x22E2, 0x22E3}, {0x22E4, 0x22E5}, {0x22E6, 0x22E7}, {0x22E8, 0x22E9},
    {0x22EA, 0x22EB}, {0x22EC, 0x22ED}, {0x22F0, 0x22F1}, {0x2308, 0x2309}, {0x230A, 0x230B}, {0x2329, 0x232A},
    {0x2768, 0x2769}, {0x276A, 0x276B}, {0x276C, 0x276D}, {0x276E, 0x276F}, {0x2770, 0x2771}, {0x2772, 0x2773},
    {0x2774, 0x2775}, {0x27C5, 0x27C6}, {0x27E6, 0x27E7}, {0x27E8, 0x27E9}, {0x27EA, 0x27EB}, {0x27EC, 0x27ED},
    {0x27EE, 0x27EF}, {0x2983, 0x2984}, {0x2985, 0x2986}, {0x2987, 0x2988}, {0x2989, 0x298A}, {0x298B, 0x298C},
    {0x298D, 0x2990}, {0x298F, 0x298E}, {0x2991, 0x2992}, {0x2993, 0x2994}, {0x2995, 0x2996}, {0x2997, 0x2998},
    {0x29D8, 0x29D9}, {0x29DA, 0x29DB}, {0x29FC, 0x29FD}, {0x2E02, 0x2E03}, {0x2E04, 0x2E05}, {0x2E09, 0x2E0A},
    {0x2E0C, 0x2E0D}, {0x2E1C, 0x2E1D}, {0x2E20, 0x2E21}, {0x2E22, 0x2E23}, {0x2E24, 0x2E25}, {0x2E26, 0x2E27},
    {0x2E28, 0x2E29}, {0x3008, 0x3009}, {0x300A, 0x300B}, {0x300C, 0x300D}, {0x300E, 0x300F}, {0x3010, 0x3011},
    {0x3014, 0x3015}, {0x3016, 0x3017}, {0x3018, 0x3019}, {0x301A, 0x301B}, {0xFE59, 0xFE5A}, {0xFE5B, 0xFE5C},
    {0xFE5D, 0xFE5E}, {0xFE64, 0xFE65}, {0xFF08, 0xFF09}, {0xFF1C, 0xFF1E}, {0xFF3B, 0xFF3D}, {0xFF5B, 0xFF5D},
    {0xFF5F, 0xFF60}, {0xFF62, 0xFF63},
};

/* Bidi_Paired_Bracket (opening, closing): the brackets rule N0 pairs. */
static const CpPair kBrackets[] = {
    {0x28, 0x29}, {0x5B, 0x5D}, {0x7B, 0x7D}, {0x0F3A, 0x0F3B}, {0x0F3C, 0x0F3D}, {0x169B, 0x169C}, {0x2045, 0x2046},
    {0x207D, 0x207E}, {0x208D, 0x208E}, {0x2308, 0x2309}, {0x230A, 0x230B}, {0x2329, 0x232A}, {0x2768, 0x2769},
    {0x276A, 0x276B}, {0x276C, 0x276D}, {0x276E, 0x276F}, {0x2770, 0x2771}, {0x2772, 0x2773}, {0x2774, 0x2775},
    {0x27C5, 0x27C6}, {0x27E6, 0x27E7}, {0x27E8, 0x27E9}, {0x27EA, 0x27EB}, {0x27EC, 0x27ED}, {0x27EE, 0x27EF},
    {0x2983, 0x2984}, {0x2985, 0x2986}, {0x2987, 0x2988}, {0x2989, 0x298A}, {0x298B, 0x298C}, {0x298D, 0x2990},
    {0x298F, 0x298E}, {0x2991, 0x2992}, {0x2993, 0x2994}, {0x2995, 0x2996}, {0x2997, 0x2998}, {0x29D8, 0x29D9},
    {0x29DA, 0x29DB}, {0x29FC, 0x29FD}, {0x2E22, 0x2E23}, {0x2E24, 0x2E25}, {0x2E26, 0x2E27}, {0x2E28, 0x2E29},
    {0x3008, 0x3009}, {0x300A, 0x300B}, {0x300C, 0x300D}, {0x300E, 0x300F}, {0x3010, 0x3011}, {0x3014, 0x3015},
    {0x3016, 0x3017}, {0x3018, 0x3019}, {0x301A, 0x301B}, {0xFE59, 0xFE5A}, {0xFE5B, 0xFE5C}, {0xFE5D, 0xFE5E},
    {0xFF08, 0xFF09}, {0xFF3B, 0xFF3D}, {0xFF5B, 0xFF5D}, {0xFF5F, 0xFF60}, {0xFF62, 0xFF63},
};

/* U+2329 and U+232A are canonically U+3008 and U+3009: they pair with them (BD16). */
static uint32_t canonicalBracket(uint32_t cp) { return cp == 0x2329 ? 0x3008 : cp == 0x232A ? 0x3009 : cp; }

enum { kMaxDepth = 125 };
#define NONE ((size_t)-1)

typedef CscdBidiClass C;

static bool isRemoved(C t) { return t == CSCD_BIDI_RLE || t == CSCD_BIDI_LRE || t == CSCD_BIDI_RLO || t == CSCD_BIDI_LRO || t == CSCD_BIDI_PDF || t == CSCD_BIDI_BN; }
static bool isIsolateInit(C t) { return t == CSCD_BIDI_LRI || t == CSCD_BIDI_RLI || t == CSCD_BIDI_FSI; }
static bool isNI(C t) { return t == CSCD_BIDI_B || t == CSCD_BIDI_S || t == CSCD_BIDI_WS || t == CSCD_BIDI_ON || isIsolateInit(t) || t == CSCD_BIDI_PDI; }
static bool isStrong(C t) { return t == CSCD_BIDI_L || t == CSCD_BIDI_R || t == CSCD_BIDI_AL; }

/* P2 / P3 on [from, to): 1 when the first strong character (outside isolates) is R or AL. */
static int firstStrongLevel(const uint8_t* types, size_t from, size_t to, int otherwise) {
    int depth = 0;
    for (size_t i = from; i < to; ++i) {
        const C t = (C)types[i];
        if (isIsolateInit(t)) ++depth;
        else if (t == CSCD_BIDI_PDI) { if (depth > 0) --depth; }
        else if (depth == 0 && isStrong(t)) return t == CSCD_BIDI_L ? 0 : 1;
    }
    return otherwise;
}

typedef struct Paragraph {
    const uint32_t* cps;
    size_t n;
    int paraLevel;
    uint8_t* initial; /* CscdBidiClass each */
    uint8_t* types;
    uint8_t* levels;
    size_t* matchingPDI;
    size_t* matchingInit;
} Paragraph;

/* BD9: each isolate initiator's matching PDI, and the other way round */
static void matchIsolates(Paragraph* p) {
    CscdSizeVec open = {0};
    for (size_t i = 0; i < p->n; ++i) {
        p->matchingPDI[i] = p->matchingInit[i] = NONE;
    }
    for (size_t i = 0; i < p->n; ++i) {
        const C t = (C)p->types[i];
        if (isIsolateInit(t)) cscdVecPush(&open, i);
        else if (t == CSCD_BIDI_PDI && open.len) {
            p->matchingPDI[cscdVecLast(&open)] = i;
            p->matchingInit[i] = cscdVecLast(&open);
            open.len--;
        } else if (t == CSCD_BIDI_B) cscdVecClear(&open);
    }
    cscdVecFree(&open);
}

typedef struct Entry { uint8_t level; int8_t override; bool isolate; } Entry; /* override: -1 none, 0 L, 1 R */

static int nextOdd(int l) { return (l + 1) | 1; }
static int nextEven(int l) { return (l + 2) & ~1; }

/* X1 to X8 */
static void explicitLevels(Paragraph* p) {
    Entry stack[kMaxDepth + 2];
    size_t sp = 0;
    stack[sp++] = (Entry){(uint8_t)p->paraLevel, -1, false};
    int overflowIsolates = 0, overflowEmbeddings = 0, validIsolates = 0;
    for (size_t i = 0; i < p->n; ++i) {
        const C t = (C)p->types[i];
        const Entry top = stack[sp - 1];
        switch (t) {
        case CSCD_BIDI_RLE: case CSCD_BIDI_LRE: case CSCD_BIDI_RLO: case CSCD_BIDI_LRO: {
            const bool rtl = t == CSCD_BIDI_RLE || t == CSCD_BIDI_RLO;
            const int level = rtl ? nextOdd(top.level) : nextEven(top.level);
            p->levels[i] = top.level;
            if (level <= kMaxDepth && !overflowIsolates && !overflowEmbeddings) {
                const int8_t ov = t == CSCD_BIDI_RLO ? 1 : t == CSCD_BIDI_LRO ? 0 : -1;
                stack[sp++] = (Entry){(uint8_t)level, ov, false};
            } else if (!overflowIsolates) ++overflowEmbeddings;
            break;
        }
        case CSCD_BIDI_RLI: case CSCD_BIDI_LRI: case CSCD_BIDI_FSI: {
            bool rtl = t == CSCD_BIDI_RLI;
            if (t == CSCD_BIDI_FSI) rtl = firstStrongLevel(p->types, i + 1, p->matchingPDI[i] == NONE ? p->n : p->matchingPDI[i], 0) == 1;
            p->levels[i] = top.level;
            if (top.override >= 0) p->types[i] = top.override ? CSCD_BIDI_R : CSCD_BIDI_L;
            const int level = rtl ? nextOdd(top.level) : nextEven(top.level);
            if (level <= kMaxDepth && !overflowIsolates && !overflowEmbeddings) {
                ++validIsolates;
                stack[sp++] = (Entry){(uint8_t)level, -1, true};
            } else ++overflowIsolates;
            break;
        }
        case CSCD_BIDI_PDI:
            if (overflowIsolates > 0) --overflowIsolates;
            else if (validIsolates > 0) {
                overflowEmbeddings = 0;
                while (!stack[sp - 1].isolate) --sp;
                --sp;
                --validIsolates;
            }
            p->levels[i] = stack[sp - 1].level;
            if (stack[sp - 1].override >= 0) p->types[i] = stack[sp - 1].override ? CSCD_BIDI_R : CSCD_BIDI_L;
            break;
        case CSCD_BIDI_PDF:
            p->levels[i] = top.level;
            if (overflowIsolates > 0) {
            } else if (overflowEmbeddings > 0) --overflowEmbeddings;
            else if (!top.isolate && sp >= 2) --sp;
            break;
        case CSCD_BIDI_B:
            p->levels[i] = (uint8_t)p->paraLevel;
            break;
        case CSCD_BIDI_BN:
            p->levels[i] = top.level;
            break;
        default:
            p->levels[i] = top.level;
            if (top.override >= 0) p->types[i] = top.override ? CSCD_BIDI_R : CSCD_BIDI_L;
            break;
        }
    }
}

/* sos and eos: from the higher of this level and the one on the other side */
static int levelBefore(const Paragraph* p, size_t i) {
    for (size_t k = i; k-- > 0;) if (!isRemoved((C)p->initial[k])) return p->levels[k];
    return p->paraLevel;
}

static int levelAfter(const Paragraph* p, size_t i) {
    if (isIsolateInit((C)p->types[i]) && p->matchingPDI[i] == NONE) return p->paraLevel; /* (an unmatched initiator: to the paragraph's end) */
    if (isIsolateInit((C)p->initial[i])) return p->paraLevel;
    for (size_t k = i + 1; k < p->n; ++k) if (!isRemoved((C)p->initial[k])) return p->levels[k];
    return p->paraLevel;
}

static C strongOf(C c) { return c == CSCD_BIDI_EN || c == CSCD_BIDI_AN ? CSCD_BIDI_R : c; }

typedef struct Pair { size_t open, close; } Pair;
typedef CSCD_VEC(Pair) PairVec;

static int comparePairs(const void* a, const void* b, void* ctx) {
    (void)ctx;
    size_t x = ((const Pair*)a)->open, y = ((const Pair*)b)->open;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* X10 and the rules after it, on one isolating run sequence (its indices, in order) */
static void resolveSequence(Paragraph* p, const size_t* seq, size_t m) {
    if (!m) return;
    const int level = p->levels[seq[0]];
    int lb = levelBefore(p, seq[0]), la = levelAfter(p, seq[m - 1]);
    const C sos = ((level > lb ? level : lb) & 1) ? CSCD_BIDI_R : CSCD_BIDI_L;
    const C eos = ((level > la ? level : la) & 1) ? CSCD_BIDI_R : CSCD_BIDI_L;
    C* t = (C*)cscdMalloc(m * sizeof(C));
    for (size_t k = 0; k < m; ++k) t[k] = (C)p->types[seq[k]];

    /* W1: NSM takes the type before it (ON after an isolate initiator or PDI) */
    for (size_t k = 0; k < m; ++k) {
        if (t[k] != CSCD_BIDI_NSM) continue;
        if (k == 0) t[k] = sos;
        else t[k] = isIsolateInit(t[k - 1]) || t[k - 1] == CSCD_BIDI_PDI ? CSCD_BIDI_ON : t[k - 1];
    }
    /* W2: EN after AL is AN; W3: AL is R */
    C lastStrong = sos;
    for (size_t k = 0; k < m; ++k) {
        if (t[k] == CSCD_BIDI_L || t[k] == CSCD_BIDI_R || t[k] == CSCD_BIDI_AL) lastStrong = t[k];
        else if (t[k] == CSCD_BIDI_EN && lastStrong == CSCD_BIDI_AL) t[k] = CSCD_BIDI_AN;
    }
    for (size_t k = 0; k < m; ++k) if (t[k] == CSCD_BIDI_AL) t[k] = CSCD_BIDI_R;
    /* W4: one ES between ENs, one CS between two ENs or two ANs */
    for (size_t k = 1; k + 1 < m; ++k) {
        if (t[k] == CSCD_BIDI_ES && t[k - 1] == CSCD_BIDI_EN && t[k + 1] == CSCD_BIDI_EN) t[k] = CSCD_BIDI_EN;
        else if (t[k] == CSCD_BIDI_CS && t[k - 1] == CSCD_BIDI_EN && t[k + 1] == CSCD_BIDI_EN) t[k] = CSCD_BIDI_EN;
        else if (t[k] == CSCD_BIDI_CS && t[k - 1] == CSCD_BIDI_AN && t[k + 1] == CSCD_BIDI_AN) t[k] = CSCD_BIDI_AN;
    }
    /* W5: ETs next to an EN are EN */
    for (size_t k = 0; k < m; ++k) {
        if (t[k] != CSCD_BIDI_ET) continue;
        size_t e = k;
        while (e < m && t[e] == CSCD_BIDI_ET) ++e;
        const bool en = (k > 0 && t[k - 1] == CSCD_BIDI_EN) || (e < m && t[e] == CSCD_BIDI_EN);
        if (en) for (size_t q = k; q < e; ++q) t[q] = CSCD_BIDI_EN;
        k = e - 1;
    }
    /* W6: the separators and terminators left are ON */
    for (size_t k = 0; k < m; ++k) if (t[k] == CSCD_BIDI_ES || t[k] == CSCD_BIDI_ET || t[k] == CSCD_BIDI_CS) t[k] = CSCD_BIDI_ON;
    /* W7: EN after L is L */
    lastStrong = sos;
    for (size_t k = 0; k < m; ++k) {
        if (t[k] == CSCD_BIDI_L || t[k] == CSCD_BIDI_R) lastStrong = t[k];
        else if (t[k] == CSCD_BIDI_EN && lastStrong == CSCD_BIDI_L) t[k] = CSCD_BIDI_L;
    }

    const C e = (level & 1) ? CSCD_BIDI_R : CSCD_BIDI_L; /* the embedding direction */

    /* N0: paired brackets take the direction of what they enclose (or of what precedes them) */
    PairVec pairs = {0};
    {
        typedef struct Open { uint32_t close; size_t at; } Open;
        Open stack[63];
        size_t sp = 0;
        for (size_t k = 0; k < m; ++k) {
            if (t[k] != CSCD_BIDI_ON) continue;
            const uint32_t cp = canonicalBracket(p->cps[seq[k]]);
            bool isOpen = false, isClose = false;
            uint32_t partner = 0;
            for (size_t b = 0; b < CSCD_COUNT(kBrackets); ++b) {
                if (canonicalBracket(kBrackets[b].a) == cp) { isOpen = true; partner = canonicalBracket(kBrackets[b].b); break; }
                if (canonicalBracket(kBrackets[b].b) == cp) { isClose = true; break; }
            }
            if (isOpen) {
                if (sp == 63) break; /* (BD16: too deep, no more pairs) */
                stack[sp++] = (Open){partner, k};
            } else if (isClose) {
                for (size_t s = sp; s-- > 0;)
                    if (stack[s].close == cp) {
                        cscdVecPush(&pairs, ((Pair){stack[s].at, k}));
                        sp = s;
                        break;
                    }
            }
        }
        cscdSort(pairs.items, pairs.len, sizeof(Pair), comparePairs, NULL);
    }
    for (size_t pi = 0; pi < pairs.len; ++pi) {
        const Pair pr = pairs.items[pi];
        bool sameInside = false, oppositeInside = false;
        for (size_t k = pr.open + 1; k < pr.close; ++k) {
            const C s = strongOf(t[k]);
            if (s == e) sameInside = true;
            else if (s == CSCD_BIDI_L || s == CSCD_BIDI_R) oppositeInside = true;
        }
        C to = CSCD_BIDI_ON;
        if (sameInside) to = e;
        else if (oppositeInside) {
            C before = sos;
            for (size_t k = pr.open; k-- > 0;) {
                const C s = strongOf(t[k]);
                if (s == CSCD_BIDI_L || s == CSCD_BIDI_R) { before = s; break; }
            }
            to = before != e ? before : e;
        }
        if (to == CSCD_BIDI_ON) continue;
        t[pr.open] = t[pr.close] = to;
        /* the NSMs that followed a bracket follow it */
        for (size_t k = pr.open + 1; k < m && p->initial[seq[k]] == CSCD_BIDI_NSM; ++k) t[k] = to;
        for (size_t k = pr.close + 1; k < m && p->initial[seq[k]] == CSCD_BIDI_NSM; ++k) t[k] = to;
    }
    cscdVecFree(&pairs);

    /* N1 / N2: neutrals between two strong runs of one direction take it; others the embedding's */
    for (size_t k = 0; k < m; ++k) {
        if (!isNI(t[k])) continue;
        size_t end = k;
        while (end < m && isNI(t[end])) ++end;
        const C before = k == 0 ? sos : strongOf(t[k - 1]);
        const C after = end == m ? eos : strongOf(t[end]);
        const C to = before == after && (before == CSCD_BIDI_L || before == CSCD_BIDI_R) ? before : e;
        for (size_t q = k; q < end; ++q) t[q] = to;
        k = end - 1;
    }

    /* I1 / I2 */
    for (size_t k = 0; k < m; ++k) {
        uint8_t* lv = &p->levels[seq[k]];
        if (!(*lv & 1)) {
            if (t[k] == CSCD_BIDI_R) *lv += 1;
            else if (t[k] == CSCD_BIDI_AN || t[k] == CSCD_BIDI_EN) *lv += 2;
        } else if (t[k] == CSCD_BIDI_L || t[k] == CSCD_BIDI_EN || t[k] == CSCD_BIDI_AN) *lv += 1;
    }
    cscdFree(t);
}

typedef struct Run { size_t start, len; } Run; /* a span of kept[] */
typedef CSCD_VEC(Run) RunVec;

static void runParagraph(Paragraph* p) {
    const size_t n = p->n;
    matchIsolates(p);
    explicitLevels(p);
    /* X9: the explicit formatting characters and BN are left out; level runs of the rest */
    CscdSizeVec kept = {0};
    cscdVecReserve(&kept, n);
    for (size_t i = 0; i < n; ++i) if (!isRemoved((C)p->initial[i])) cscdVecPush(&kept, i);
    RunVec runs = {0};
    for (size_t k = 0; k < kept.len; ++k) {
        if (k == 0 || p->levels[kept.items[k]] != p->levels[kept.items[k - 1]]) cscdVecPush(&runs, ((Run){k, 0}));
        runs.items[runs.len - 1].len++;
    }
    /* X10: isolating run sequences, chained through matching isolate initiators and PDIs */
    int* runOf = (int*)cscdMalloc((n ? n : 1) * sizeof(int));
    for (size_t i = 0; i < n; ++i) runOf[i] = -1;
    for (size_t r = 0; r < runs.len; ++r)
        for (size_t k = 0; k < runs.items[r].len; ++k) runOf[kept.items[runs.items[r].start + k]] = (int)r;
    CscdSizeVec seq = {0};
    for (size_t r = 0; r < runs.len; ++r) {
        const size_t first = kept.items[runs.items[r].start];
        if (p->initial[first] == CSCD_BIDI_PDI && p->matchingInit[first] != NONE) continue; /* (continues a sequence) */
        cscdVecClear(&seq);
        size_t cur = r;
        for (;;) {
            cscdVecAppend(&seq, kept.items + runs.items[cur].start, runs.items[cur].len);
            const size_t last = kept.items[runs.items[cur].start + runs.items[cur].len - 1];
            if (!isIsolateInit((C)p->initial[last]) || p->matchingPDI[last] == NONE) break;
            const int next = runOf[p->matchingPDI[last]];
            if (next < 0) break;
            cur = (size_t)next;
        }
        resolveSequence(p, seq.items, seq.len);
    }
    /* the removed characters take the level before them */
    for (size_t i = 0; i < n; ++i)
        if (isRemoved((C)p->initial[i])) p->levels[i] = i ? p->levels[i - 1] : (uint8_t)p->paraLevel;
    cscdVecFree(&seq);
    cscdFree(runOf);
    cscdVecFree(&runs);
    cscdVecFree(&kept);
}

static CscdBidiClass searchClass(uint32_t cp) {
    /* the last range whose first is <= cp */
    size_t lo = 0, hi = CSCD_COUNT(kBidiRanges);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < kBidiRanges[mid].first) hi = mid;
        else lo = mid + 1;
    }
    if (lo == 0) return CSCD_BIDI_L;
    const BidiRange* r = &kBidiRanges[lo - 1];
    return cp <= r->last ? r->cls : CSCD_BIDI_L;
}

static uint8_t asciiClasses[128];
static CscdOnce asciiOnce = CSCD_ONCE_INIT;
static void initAscii(void) {
    for (uint32_t c = 0; c < 128; ++c) asciiClasses[c] = (uint8_t)searchClass(c);
}

CscdBidiClass cscdBidiClass(uint32_t cp) {
    if (cp < 0x80) { /* ASCII without a search */
        cscdCallOnce(&asciiOnce, initAscii);
        return (CscdBidiClass)asciiClasses[cp];
    }
    return searchClass(cp);
}

uint32_t cscdBidiMirror(uint32_t cp) {
    for (size_t i = 0; i < CSCD_COUNT(kMirrors); ++i) {
        if (kMirrors[i].a == cp) return kMirrors[i].b;
        if (kMirrors[i].b == cp) return kMirrors[i].a;
    }
    return cp;
}

bool cscdBidiNeeded(const uint32_t* cps, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (cps[i] < 0x0590) continue; /* (Latin, Greek, Cyrillic...: nothing right to left) */
        const CscdBidiClass c = cscdBidiClass(cps[i]);
        if (c == CSCD_BIDI_R || c == CSCD_BIDI_AL || c == CSCD_BIDI_AN || (c >= CSCD_BIDI_LRE && c <= CSCD_BIDI_PDI)) return true;
    }
    return false;
}

void cscdBidiLevels(const uint32_t* cps, size_t n, int paragraphLevel, uint8_t* levels) {
    if (!n) return;
    Paragraph p;
    p.cps = cps;
    p.n = n;
    p.paraLevel = paragraphLevel & 1;
    p.initial = (uint8_t*)cscdMalloc(n);
    p.types = (uint8_t*)cscdMalloc(n);
    p.levels = levels;
    p.matchingPDI = (size_t*)cscdMalloc(n * sizeof(size_t));
    p.matchingInit = (size_t*)cscdMalloc(n * sizeof(size_t));
    for (size_t i = 0; i < n; ++i) {
        p.initial[i] = p.types[i] = (uint8_t)cscdBidiClass(cps[i]);
        levels[i] = (uint8_t)p.paraLevel;
    }
    runParagraph(&p);
    cscdFree(p.initial);
    cscdFree(p.types);
    cscdFree(p.matchingPDI);
    cscdFree(p.matchingInit);
}

static bool resets(CscdBidiClass c) { return c == CSCD_BIDI_WS || isIsolateInit(c) || c == CSCD_BIDI_PDI || isRemoved(c); }

void cscdBidiLineLevels(const uint32_t* cps, size_t n, int paragraphLevel, uint8_t* levels) {
    bool trailing = true; /* from the line's end back */
    for (size_t i = n; i-- > 0;) {
        const CscdBidiClass c = cscdBidiClass(cps[i]);
        if (c == CSCD_BIDI_S || c == CSCD_BIDI_B) {
            levels[i] = (uint8_t)paragraphLevel;
            trailing = true;
        } else if (trailing && resets(c)) levels[i] = (uint8_t)paragraphLevel;
        else trailing = false;
    }
}

void cscdBidiVisualOrder(const uint8_t* levels, size_t n, CscdSizeVec* order) {
    cscdVecResize(order, n);
    for (size_t i = 0; i < n; ++i) order->items[i] = i;
    if (!n) return;
    uint8_t highest = 0, lowestOdd = 255;
    for (size_t i = 0; i < n; ++i) {
        if (levels[i] > highest) highest = levels[i];
        if ((levels[i] & 1) && levels[i] < lowestOdd) lowestOdd = levels[i];
    }
    size_t* o = order->items;
    /* from the highest level down to the lowest odd one, reverse every run at that level or higher */
    for (int lv = highest; lv >= (int)lowestOdd && lv > 0; --lv) {
        for (size_t i = 0; i < n;) {
            if (levels[o[i]] < lv) { ++i; continue; }
            size_t j = i;
            while (j < n && levels[o[j]] >= lv) ++j;
            for (size_t a = i, b = j; a + 1 < b; ++a, --b) {
                size_t tmp = o[a];
                o[a] = o[b - 1];
                o[b - 1] = tmp;
            }
            i = j;
        }
    }
}
