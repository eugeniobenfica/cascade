/* The Unicode Line Breaking Algorithm (UAX #14, Unicode 16.0), its default rules: the classes of the code points
 * (lineBreakData.inc, made by tools/genLineBreak.py), then each boundary decided by the first rule that speaks of it.
 * tests.c runs LineBreakTest.txt against it when it is given the file. */
#include "text/lineBreakInternal.h"

#include <string.h>

typedef struct LbRange { uint32_t first, last; uint8_t cls, flags; } LbRange;

static const LbRange kRanges[] = {
#include "lineBreakData.inc"
};

static uint8_t asciiCls[128], asciiFlags[128];
static volatile int asciiReady;

static CscdLineBreakClass lookup(uint32_t cp, uint8_t* flags) {
    size_t lo = 0, hi = CSCD_COUNT(kRanges);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (cp < kRanges[mid].first) hi = mid;
        else if (cp > kRanges[mid].last) lo = mid + 1;
        else {
            *flags = kRanges[mid].flags;
            return (CscdLineBreakClass)kRanges[mid].cls;
        }
    }
    *flags = 0;
    return LB_AL;
}

CscdLineBreakClass cscdLineBreakClass(uint32_t cp, uint8_t* flags) {
    if (cp < 128) {
        if (!asciiReady) { /* (the same values from every thread: a race only writes them twice) */
            for (uint32_t c = 0; c < 128; ++c) asciiCls[c] = (uint8_t)lookup(c, &asciiFlags[c]);
            asciiReady = 1;
        }
        *flags = asciiFlags[cp];
        return (CscdLineBreakClass)asciiCls[cp];
    }
    return lookup(cp, flags);
}

/* ---- the rules ------------------------------------------------------------------------------------------------- */

typedef struct Lb {
    const uint32_t* cp;
    size_t n;
    uint8_t* raw;   /* each code point's class */
    uint8_t* eff;   /* its class after LB9 and LB10: a mark takes its base's, one with none is AL */
    uint8_t* flags; /* its base's flags */
    size_t* base;   /* the code point a mark belongs to (itself, for any other) */
} Lb;

#define NONE ((size_t)-1)
#define IN2(x, a, b) ((x) == (a) || (x) == (b))

/* the base before position k's base (NONE: the start) */
static size_t prevBase(const Lb* L, size_t k) {
    const size_t b = L->base[k];
    return b == 0 ? NONE : L->base[b - 1];
}

/* the first base after position k (NONE: the end) */
static size_t nextBase(const Lb* L, size_t k) {
    for (size_t j = k + 1; j < L->n; ++j)
        if (L->base[j] == j) return j;
    return NONE;
}

/* the base before the spaces that end at position k's base (NONE: only spaces back to the start) */
static size_t beforeSpaces(const Lb* L, size_t k) {
    size_t j = L->base[k];
    for (;;) {
        if (L->eff[j] != LB_SP) return j;
        if (j == 0) return NONE;
        j = L->base[j - 1];
    }
}

static int clsAt(const Lb* L, size_t k) { return k == NONE ? -1 : L->eff[k]; }
static bool eastAsian(const Lb* L, size_t k) { return k != NONE && (L->flags[k] & LBF_EAST_ASIAN); }
static bool dottedCircle(const Lb* L, size_t k) { return k != NONE && L->cp[L->base[k]] == 0x25CC; }
static bool aksara(const Lb* L, size_t k) { const int c = clsAt(L, k); return c == LB_AK || c == LB_AS || dottedCircle(L, k); }

/* "NU (SY | IS)*" ends at position k */
static bool numberEndsAt(const Lb* L, size_t k) {
    while (k != NONE && IN2(L->eff[k], LB_SY, LB_IS)) k = prevBase(L, k);
    return k != NONE && L->eff[k] == LB_NU;
}

static uint8_t decide(const Lb* L, size_t i) {
    const int a = L->raw[i - 1], b = L->raw[i];
    /* (most of a text: a letter or a digit after another, which nothing before LB23, LB25 and LB28 speaks of; but the
     * dotted circle, which LB28a does) */
    if ((a == LB_AL || a == LB_HL || a == LB_NU) && (b == LB_AL || b == LB_HL || b == LB_NU) && L->base[i - 1] == i - 1 &&
        L->cp[i - 1] != 0x25CC && L->cp[i] != 0x25CC)
        return CSCD_LB_NONE;
    /* LB4, LB5: hard line breaks */
    if (a == LB_BK) return CSCD_LB_MANDATORY;
    if (a == LB_CR && b == LB_LF) return CSCD_LB_NONE;
    if (a == LB_CR || a == LB_LF || a == LB_NL) return CSCD_LB_MANDATORY;
    /* LB6, LB7 */
    if (b == LB_BK || b == LB_CR || b == LB_LF || b == LB_NL || b == LB_SP || b == LB_ZW) return CSCD_LB_NONE;
    /* LB8: ZW SP* ÷ */
    const size_t pa = beforeSpaces(L, i - 1);
    if (pa != NONE && L->raw[pa] == LB_ZW) return CSCD_LB_ALLOWED;
    /* LB8a, LB9 */
    if (a == LB_ZWJ) return CSCD_LB_NONE;
    if (L->base[i] != i) return CSCD_LB_NONE;
    const int A = L->eff[i - 1], B = L->eff[i];
    const size_t ia = L->base[i - 1], before = prevBase(L, i - 1), after = nextBase(L, i);
    const int P = clsAt(L, before), N = clsAt(L, after); /* (-1: the start, the end) */
    /* LB11 .. LB13 */
    if (A == LB_WJ || B == LB_WJ) return CSCD_LB_NONE;
    if (A == LB_GL) return CSCD_LB_NONE;
    if (B == LB_GL && A != LB_SP && A != LB_BA && A != LB_HY && A != LB_HH) return CSCD_LB_NONE;
    if (B == LB_CL || B == LB_CP || B == LB_EX || B == LB_SY) return CSCD_LB_NONE;
    /* LB14: OP SP* × */
    const int S = clsAt(L, pa); /* (what is before the spaces, or A) */
    if (S == LB_OP) return CSCD_LB_NONE;
    /* LB15a: (sot | BK | CR | LF | NL | OP | QU | GL | SP | ZW) [QU & Pi] SP* × */
    if (S == LB_QU && (L->flags[pa] & LBF_PI)) {
        const int q = clsAt(L, prevBase(L, pa));
        if (q < 0 || q == LB_BK || q == LB_CR || q == LB_LF || q == LB_NL || q == LB_OP || q == LB_QU || q == LB_GL || q == LB_SP ||
            q == LB_ZW)
            return CSCD_LB_NONE;
    }
    /* LB15b: × [QU & Pf] (SP | GL | WJ | CL | QU | CP | EX | IS | SY | BK | CR | LF | NL | ZW | eot) */
    if (B == LB_QU && (L->flags[i] & LBF_PF) &&
        (N < 0 || N == LB_SP || N == LB_GL || N == LB_WJ || N == LB_CL || N == LB_QU || N == LB_CP || N == LB_EX || N == LB_IS || N == LB_SY ||
         N == LB_BK || N == LB_CR || N == LB_LF || N == LB_NL || N == LB_ZW))
        return CSCD_LB_NONE;
    /* LB15c: SP ÷ IS NU; LB15d: × IS */
    if (A == LB_SP && B == LB_IS && N == LB_NU) return CSCD_LB_ALLOWED;
    if (B == LB_IS) return CSCD_LB_NONE;
    /* LB16: (CL | CP) SP* × NS; LB17: B2 SP* × B2 */
    if (B == LB_NS && (S == LB_CL || S == LB_CP)) return CSCD_LB_NONE;
    if (B == LB_B2 && S == LB_B2) return CSCD_LB_NONE;
    /* LB18 */
    if (A == LB_SP) return CSCD_LB_ALLOWED;
    /* LB19: × [QU - Pi]; [QU - Pf] × */
    if (B == LB_QU && !(L->flags[i] & LBF_PI)) return CSCD_LB_NONE;
    if (A == LB_QU && !(L->flags[ia] & LBF_PF)) return CSCD_LB_NONE;
    /* LB19a: quotation marks among what is not East Asian */
    if (B == LB_QU && (!eastAsian(L, ia) || after == NONE || !eastAsian(L, after))) return CSCD_LB_NONE;
    if (A == LB_QU && (!eastAsian(L, i) || before == NONE || !eastAsian(L, before))) return CSCD_LB_NONE;
    /* LB20 */
    if (A == LB_CB || B == LB_CB) return CSCD_LB_ALLOWED;
    /* LB20a: (sot | BK | CR | LF | NL | SP | ZW | CB | GL) (HY | U+2010) × AL (Unicode 16: the hyphen is still BA) */
    if ((A == LB_HY || A == LB_HH || L->cp[ia] == 0x2010) && B == LB_AL &&
        (P < 0 || P == LB_BK || P == LB_CR || P == LB_LF || P == LB_NL || P == LB_SP || P == LB_ZW || P == LB_CB || P == LB_GL))
        return CSCD_LB_NONE;
    /* LB21, LB21a, LB21b */
    if (B == LB_BA || B == LB_HH || B == LB_HY || B == LB_NS || A == LB_BB) return CSCD_LB_NONE;
    /* (LB21a: HL (HY | [BA - East Asian]) × [^HL]) */
    if ((A == LB_HY || A == LB_HH || (A == LB_BA && !eastAsian(L, ia))) && P == LB_HL && B != LB_HL) return CSCD_LB_NONE;
    if (A == LB_SY && B == LB_HL) return CSCD_LB_NONE;
    /* LB22 */
    if (B == LB_IN) return CSCD_LB_NONE;
    /* LB23, LB23a, LB24 */
    if ((IN2(A, LB_AL, LB_HL) && B == LB_NU) || (A == LB_NU && IN2(B, LB_AL, LB_HL))) return CSCD_LB_NONE;
    if (A == LB_PR && (B == LB_ID || B == LB_EB || B == LB_EM)) return CSCD_LB_NONE;
    if ((A == LB_ID || A == LB_EB || A == LB_EM) && B == LB_PO) return CSCD_LB_NONE;
    if (IN2(A, LB_PR, LB_PO) && IN2(B, LB_AL, LB_HL)) return CSCD_LB_NONE;
    if (IN2(A, LB_AL, LB_HL) && IN2(B, LB_PR, LB_PO)) return CSCD_LB_NONE;
    /* LB25: numbers */
    if (IN2(B, LB_PO, LB_PR)) {
        if (IN2(A, LB_CL, LB_CP) && numberEndsAt(L, before)) return CSCD_LB_NONE;
        if (numberEndsAt(L, ia)) return CSCD_LB_NONE;
    }
    if (IN2(A, LB_PO, LB_PR)) {
        if (B == LB_NU) return CSCD_LB_NONE;
        if (B == LB_OP && (N == LB_NU || (N == LB_IS && clsAt(L, nextBase(L, after)) == LB_NU))) return CSCD_LB_NONE;
    }
    if (B == LB_NU && (A == LB_HY || A == LB_IS || numberEndsAt(L, ia))) return CSCD_LB_NONE;
    /* LB26, LB27: Korean syllables */
    if (A == LB_JL && (B == LB_JL || B == LB_JV || B == LB_H2 || B == LB_H3)) return CSCD_LB_NONE;
    if (IN2(A, LB_JV, LB_H2) && IN2(B, LB_JV, LB_JT)) return CSCD_LB_NONE;
    if (IN2(A, LB_JT, LB_H3) && B == LB_JT) return CSCD_LB_NONE;
    const bool korA = A == LB_JL || A == LB_JV || A == LB_JT || A == LB_H2 || A == LB_H3;
    const bool korB = B == LB_JL || B == LB_JV || B == LB_JT || B == LB_H2 || B == LB_H3;
    if ((korA && B == LB_PO) || (A == LB_PR && korB)) return CSCD_LB_NONE;
    /* LB28 */
    if (IN2(A, LB_AL, LB_HL) && IN2(B, LB_AL, LB_HL) && !dottedCircle(L, ia) && !dottedCircle(L, i)) return CSCD_LB_NONE;
    /* LB28a: Brahmic orthographic syllables (aksara) */
    if (A == LB_AP && aksara(L, i)) return CSCD_LB_NONE;
    if (aksara(L, ia) && IN2(B, LB_VF, LB_VI)) return CSCD_LB_NONE;
    if (A == LB_VI && aksara(L, before) && (B == LB_AK || dottedCircle(L, i))) return CSCD_LB_NONE;
    if (aksara(L, ia) && aksara(L, i) && N == LB_VF) return CSCD_LB_NONE;
    /* LB28 for the dotted circle, an AL that LB28a looks at first */
    if (IN2(A, LB_AL, LB_HL) && IN2(B, LB_AL, LB_HL)) return CSCD_LB_NONE;
    /* LB29, LB30 */
    if (A == LB_IS && IN2(B, LB_AL, LB_HL)) return CSCD_LB_NONE;
    if ((A == LB_AL || A == LB_HL || A == LB_NU) && B == LB_OP && !eastAsian(L, i)) return CSCD_LB_NONE;
    if (A == LB_CP && !eastAsian(L, ia) && (B == LB_AL || B == LB_HL || B == LB_NU)) return CSCD_LB_NONE;
    /* LB30a: regional indicators in pairs */
    if (A == LB_RI && B == LB_RI) {
        size_t count = 0;
        for (size_t k = ia; k != NONE && L->eff[k] == LB_RI; k = prevBase(L, k)) ++count;
        if (count % 2) return CSCD_LB_NONE;
    }
    /* LB30b */
    if (B == LB_EM && (A == LB_EB || (L->flags[ia] & LBF_PICTOGRAPHIC))) return CSCD_LB_NONE;
    /* LB31 */
    return CSCD_LB_ALLOWED;
}

void cscdLineBreaks(const uint32_t* cps, size_t n, uint8_t* out) {
    if (!n) {
        out[0] = CSCD_LB_NONE;
        return;
    }
    /* (a short text, as most of a page's are: its scratch on the stack) */
    size_t small[256];
    uint8_t* mem = n <= 64 ? (uint8_t*)(void*)small : (uint8_t*)cscdMalloc(n * (3 + sizeof(size_t)));
    Lb L = {cps, n, mem + n * sizeof(size_t), mem + n * sizeof(size_t) + n, mem + n * sizeof(size_t) + 2 * n, (size_t*)(void*)mem};
    for (size_t i = 0; i < n; ++i) {
        uint8_t f;
        L.raw[i] = (uint8_t)cscdLineBreakClass(cps[i], &f);
        /* LB9: X (CM | ZWJ)* is X, but after a hard break, a space or ZW; LB10: a mark left alone is AL */
        const bool mark = L.raw[i] == LB_CM || L.raw[i] == LB_ZWJ;
        if (mark && i > 0) {
            const int x = L.raw[L.base[i - 1]];
            if (x != LB_BK && x != LB_CR && x != LB_LF && x != LB_NL && x != LB_SP && x != LB_ZW) {
                L.base[i] = L.base[i - 1];
                L.eff[i] = L.eff[i - 1];
                L.flags[i] = L.flags[i - 1];
                continue;
            }
        }
        L.base[i] = i;
        L.eff[i] = mark ? LB_AL : L.raw[i];
        L.flags[i] = f;
    }
    out[0] = CSCD_LB_NONE;
    for (size_t i = 1; i < n; ++i) out[i] = decide(&L, i);
    out[n] = CSCD_LB_MANDATORY;
    if (n > 64) cscdFree(mem);
}
