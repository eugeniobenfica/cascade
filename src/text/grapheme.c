/* Grapheme cluster boundaries by Unicode's rules (UAX #29, Unicode 16.0): the properties from graphemeData.inc (made by
 * tools/genGrapheme.py), then the rules GB3 .. GB999 as a state that walks forward over the code points. */
#include "text/graphemeInternal.h"

typedef struct GbRange { uint32_t first, last; uint8_t cls, flags; } GbRange;

static const GbRange kRanges[] = {
#include "graphemeData.inc"
};

CscdGraphemeClass cscdGraphemeClass(uint32_t cp, uint8_t* flags) {
    if (cp >= 0x20 && cp < 0x7F) { /* (printable ASCII: Other, as most text is) */
        *flags = 0;
        return GB_OTHER;
    }
    size_t lo = 0, hi = CSCD_COUNT(kRanges);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (cp < kRanges[mid].first) hi = mid;
        else if (cp > kRanges[mid].last) lo = mid + 1;
        else {
            *flags = kRanges[mid].flags;
            return (CscdGraphemeClass)kRanges[mid].cls;
        }
    }
    *flags = 0;
    return GB_OTHER;
}

/* what the rules remember of the code points since the last boundary that matters */
typedef struct GbState {
    int cls;         /* the code point before */
    uint8_t flags;
    unsigned ri;     /* regional indicators in a row, ending with it (GB12, GB13) */
    int emoji;       /* 1: Extended_Pictographic Extend*, 2: then ZWJ (GB11) */
    int conjunct;    /* 1: an InCB consonant [Extend | Linker]*, 2: with a linker among them (GB9c) */
} GbState;

static void gbStart(GbState* S, int cls, uint8_t flags) {
    S->cls = cls;
    S->flags = flags;
    S->ri = cls == GB_RI;
    S->emoji = (flags & GBF_PICTOGRAPHIC) ? 1 : 0;
    S->conjunct = (flags & GBF_CONSONANT) ? 1 : 0;
}

/* whether a cluster ends between S's code point and the next (cls, flags); then S moves on to it */
static bool gbStep(GbState* S, int cls, uint8_t flags) {
    const int p = S->cls;
    bool brk;
    if (p == GB_CR && cls == GB_LF) brk = false;                                                    /* GB3 */
    else if (p == GB_CR || p == GB_LF || p == GB_CONTROL) brk = true;                               /* GB4 */
    else if (cls == GB_CR || cls == GB_LF || cls == GB_CONTROL) brk = true;                         /* GB5 */
    else if (p == GB_L && (cls == GB_L || cls == GB_V || cls == GB_LV || cls == GB_LVT)) brk = false; /* GB6 */
    else if ((p == GB_LV || p == GB_V) && (cls == GB_V || cls == GB_T)) brk = false;               /* GB7 */
    else if ((p == GB_LVT || p == GB_T) && cls == GB_T) brk = false;                                /* GB8 */
    else if (cls == GB_EXTEND || cls == GB_ZWJ || cls == GB_SPACING_MARK) brk = false;              /* GB9, GB9a */
    else if (p == GB_PREPEND) brk = false;                                                          /* GB9b */
    else if ((flags & GBF_CONSONANT) && S->conjunct == 2) brk = false;                              /* GB9c */
    else if ((flags & GBF_PICTOGRAPHIC) && p == GB_ZWJ && S->emoji == 2) brk = false;               /* GB11 */
    else if (p == GB_RI && cls == GB_RI && S->ri % 2 == 1) brk = false;                             /* GB12, GB13 */
    else brk = true;                                                                                /* GB999 */
    /* what follows looks back at */
    S->ri = cls == GB_RI ? S->ri + 1 : 0;
    if (flags & GBF_PICTOGRAPHIC) S->emoji = 1;
    else if (cls == GB_EXTEND && S->emoji == 1) S->emoji = 1;
    else if (cls == GB_ZWJ && S->emoji == 1) S->emoji = 2;
    else S->emoji = 0;
    if (flags & GBF_CONSONANT) S->conjunct = 1;
    else if ((flags & GBF_LINKER) && S->conjunct) S->conjunct = 2;
    else if (!(flags & GBF_INCB_EXTEND)) S->conjunct = 0;
    S->cls = cls;
    S->flags = flags;
    return brk;
}

void cscdGraphemeBreaks(const uint32_t* cps, size_t n, bool* out) {
    out[0] = n > 0;
    if (!n) return;
    GbState S;
    uint8_t f = 0;
    const int first = cscdGraphemeClass(cps[0], &f); /* (before f is read: arguments go in any order) */
    gbStart(&S, first, f);
    for (size_t i = 1; i < n; ++i) {
        const int c = cscdGraphemeClass(cps[i], &f);
        out[i] = gbStep(&S, c, f);
    }
    out[n] = true;
}

size_t cscdGraphemeNext(CscdSv s, size_t i) {
    if (i >= s.len) return s.len;
    uint8_t f = 0;
    GbState S;
    const int first = cscdGraphemeClass(cscdUtf8Next(s, &i), &f);
    gbStart(&S, first, f);
    while (i < s.len) {
        size_t j = i;
        const uint32_t cp = cscdUtf8Next(s, &j);
        const int c = cscdGraphemeClass(cp, &f);
        if (gbStep(&S, c, f)) return i;
        i = j;
    }
    return s.len;
}

/* the start of the code point that ends before i */
static size_t cpBefore(CscdSv s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && ((unsigned char)s.ptr[i] & 0xC0) == 0x80) --i;
    return i;
}

size_t cscdGraphemePrev(CscdSv s, size_t i) {
    if (i == 0) return 0;
    if (i > s.len) i = s.len;
    /* a boundary before i that is certain, to walk forward from: after a control (GB4), or between two code points of no
     * class that nothing joins (GB999), within a reach; else the start */
    size_t from = 0;
    uint8_t f = 0, fb = 0;
    for (size_t p = cpBefore(s, i), steps = 0; p > 0 && steps < 512; p = cpBefore(s, p), ++steps) {
        size_t q = p;
        const int here = cscdGraphemeClass(cscdUtf8Next(s, &q), &f);
        size_t b = cpBefore(s, p), qb = b;
        const int before = cscdGraphemeClass(cscdUtf8Next(s, &qb), &fb);
        if ((before == GB_CR && here != GB_LF) || before == GB_LF || before == GB_CONTROL || (here == GB_OTHER && !f && before == GB_OTHER && !fb)) {
            from = p;
            break;
        }
    }
    size_t last = from;
    for (size_t p = from; p < i;) {
        const size_t n = cscdGraphemeNext(s, p);
        if (n >= i) break;
        last = p = n;
    }
    return last;
}
