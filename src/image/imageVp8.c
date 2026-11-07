/* Lossy WebP: a VP8 key frame (RFC 6386) — the boolean decoder, the frame header, intra
 * prediction of 16x16, 8x8 and 4x4 blocks, the coefficient tokens, the inverse DCT and WHT, and the
 * normal and simple loop filters. The planes come out as the reference decoder makes them. */
#include "image/imageInternal.h"

#include <stdlib.h>
#include <string.h>

#include "vp8Tables.inc"

/* ---- the boolean decoder (section 7) --------------------------------------------------------- */

typedef struct BoolDec {
    const uint8_t *p, *end;
    uint32_t value, range;
    int bitCount;
    size_t past; /* bytes read as zeros past the end */
} BoolDec;

/* How far a partition may be read past its end. The decoder looks two bytes ahead, and libwebp
 * gives up one byte past: more than this is a stream that ran out, not a well-formed one. */
#define VP8_PAST_END 8

static void bdInit(BoolDec* d, const uint8_t* p, size_t n) {
    d->p = p;
    d->end = p + n;
    d->value = 0;
    d->past = 0;
    for (int i = 0; i < 2; ++i) d->value = (d->value << 8) | (d->p < d->end ? *d->p++ : (++d->past, 0));
    d->range = 255;
    d->bitCount = 0;
}

static int bdRead(BoolDec* d, int prob) {
    const uint32_t split = 1 + (((d->range - 1) * (uint32_t)prob) >> 8), bigSplit = split << 8;
    int bit;
    if (d->value >= bigSplit) {
        bit = 1;
        d->range -= split;
        d->value -= bigSplit;
    } else {
        bit = 0;
        d->range = split;
    }
    while (d->range < 128) {
        d->value <<= 1;
        d->range <<= 1;
        if (++d->bitCount == 8) {
            d->bitCount = 0;
            d->value |= d->p < d->end ? *d->p++ : (++d->past, 0);
        }
    }
    return bit;
}

static int bdLiteral(BoolDec* d, int n) {
    int v = 0;
    while (n--) v = (v << 1) | bdRead(d, 128);
    return v;
}

/* a flag, then (when set) a magnitude of n bits and its sign; 0 when the flag is clear */
static int bdOptionalSigned(BoolDec* d, int n) {
    if (!bdRead(d, 128)) return 0;
    const int v = bdLiteral(d, n);
    return bdRead(d, 128) ? -v : v;
}

/* Trees: positive entries index the next pair, the others are leaves (negated values). */
static int bdTree(BoolDec* d, const int8_t* tree, const uint8_t* probs) {
    int i = 0;
    while ((i = tree[i + bdRead(d, probs[i >> 1])]) > 0) {}
    return -i;
}

/* ---- modes (sections 8, 11) ------------------------------------------------------------------ */

enum { DC_PRED, V_PRED, H_PRED, TM_PRED, B_PRED };
enum { B_DC_PRED, B_TM_PRED, B_VE_PRED, B_HE_PRED, B_LD_PRED, B_RD_PRED, B_VR_PRED, B_VL_PRED, B_HD_PRED, B_HU_PRED };

static const int8_t kKfYModeTree[8] = {-B_PRED, 2, 4, 6, -DC_PRED, -V_PRED, -H_PRED, -TM_PRED};
static const uint8_t kKfYModeProbs[4] = {145, 156, 163, 128};
static const int8_t kUvModeTree[6] = {-DC_PRED, 2, -V_PRED, 4, -H_PRED, -TM_PRED};
static const uint8_t kKfUvModeProbs[3] = {142, 114, 183};
static const int8_t kBModeTree[18] = {-B_DC_PRED, 2, -B_TM_PRED, 4, -B_VE_PRED, 6, 8, 12, -B_HE_PRED, 10,
                                      -B_RD_PRED, -B_VR_PRED, -B_LD_PRED, 14, -B_VL_PRED, 16, -B_HD_PRED, -B_HU_PRED};
static const int8_t kSegmentTree[6] = {2, 4, -0, -1, -2, -3};
static const uint8_t kZigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const uint8_t kBands[17] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
static const uint8_t kPcat3[] = {173, 148, 140, 0}, kPcat4[] = {176, 155, 140, 135, 0},
                     kPcat5[] = {180, 157, 141, 134, 130, 0}, kPcat6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};
static const uint8_t* const kPcat[4] = {kPcat3, kPcat4, kPcat5, kPcat6};
static const int kCatBase[4] = {11, 19, 35, 67};

/* ---- the decoder ----------------------------------------------------------------------------- */

typedef struct MbInfo {
    uint8_t segment, bpred, coeffs; /* (coeffs: any token besides an immediate end of block) */
} MbInfo;

typedef struct Vp8 {
    int width, height, mbw, mbh;
    uint8_t probs[4][8][3][11];
    bool segmentation, updateMap, segAbsolute;
    int segQuant[4], segFilter[4];
    uint8_t segProbs[3];
    int filterType, filterLevel, sharpness;
    bool lfAdjust;
    int refDelta[4], modeDelta[4];
    bool skipEnabled;
    int skipProb;
    int dq[4][3][2]; /* per segment: Y, Y2, UV; DC and AC */
    BoolDec first, parts[8];
    int nparts;
    MbInfo* info;
    /* the non-zero flags of the blocks above (per macroblock column) and to the left:
     * 4 Y, 2 U, 2 V, and Y2 */
    uint8_t* aboveNz;
    uint8_t leftNz[9];
    uint8_t* aboveModes; /* the subblock modes of the bottom row of the macroblocks above, 4 per column */
    uint8_t leftModes[4]; /* and of the right column of the macroblock to the left */
    CscdYuv* out;
} Vp8;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void parseHeader(Vp8* d) {
    BoolDec* b = &d->first;
    bdLiteral(b, 1); /* color space */
    bdLiteral(b, 1); /* clamping type (pixels are always clamped here) */
    d->segmentation = bdRead(b, 128);
    if (d->segmentation) {
        d->updateMap = bdRead(b, 128);
        const bool updateData = bdRead(b, 128);
        if (updateData) {
            d->segAbsolute = bdRead(b, 128);
            for (int i = 0; i < 4; ++i) d->segQuant[i] = bdOptionalSigned(b, 7);
            for (int i = 0; i < 4; ++i) d->segFilter[i] = bdOptionalSigned(b, 6);
        }
        if (d->updateMap)
            for (int i = 0; i < 3; ++i) d->segProbs[i] = bdRead(b, 128) ? (uint8_t)bdLiteral(b, 8) : 255;
    }
    d->filterType = bdLiteral(b, 1);
    d->filterLevel = bdLiteral(b, 6);
    d->sharpness = bdLiteral(b, 3);
    d->lfAdjust = bdRead(b, 128);
    if (d->lfAdjust && bdRead(b, 128)) {
        for (int i = 0; i < 4; ++i) if (bdRead(b, 128)) { int v = bdLiteral(b, 6); d->refDelta[i] = bdRead(b, 128) ? -v : v; }
        for (int i = 0; i < 4; ++i) if (bdRead(b, 128)) { int v = bdLiteral(b, 6); d->modeDelta[i] = bdRead(b, 128) ? -v : v; }
    }
}

static void setupQuant(Vp8* d, BoolDec* b) {
    const int base = bdLiteral(b, 7);
    const int ydc = bdOptionalSigned(b, 4), y2dc = bdOptionalSigned(b, 4), y2ac = bdOptionalSigned(b, 4);
    const int uvdc = bdOptionalSigned(b, 4), uvac = bdOptionalSigned(b, 4);
    for (int s = 0; s < 4; ++s) {
        int q = base;
        if (d->segmentation) q = d->segAbsolute ? d->segQuant[s] : q + d->segQuant[s];
        q = clampi(q, 0, 127);
        d->dq[s][0][0] = kDcQLookup[clampi(q + ydc, 0, 127)];
        d->dq[s][0][1] = kAcQLookup[q];
        d->dq[s][1][0] = kDcQLookup[clampi(q + y2dc, 0, 127)] * 2;
        int a = kAcQLookup[clampi(q + y2ac, 0, 127)] * 155 / 100;
        d->dq[s][1][1] = a < 8 ? 8 : a;
        a = kDcQLookup[clampi(q + uvdc, 0, 127)];
        d->dq[s][2][0] = a > 132 ? 132 : a;
        d->dq[s][2][1] = kAcQLookup[clampi(q + uvac, 0, 127)];
    }
}

/* One block's tokens (section 13) into coef (raster order, dequantized). Returns the position the
 * block ended at: first when it is empty. */
static int readCoeffs(BoolDec* b, const uint8_t (*probs)[3][11], int ctx, int first, int dc, int ac, int16_t* coef) {
    int i = first;
    const uint8_t* p = probs[kBands[i]][ctx];
    if (!bdRead(b, p[0])) return i; /* end of block */
    for (;;) {
        while (!bdRead(b, p[1])) { /* a zero */
            if (++i == 16) return 16;
            p = probs[kBands[i]][0];
        }
        int v;
        if (!bdRead(b, p[2])) {
            v = 1;
            ctx = 1;
        } else {
            if (!bdRead(b, p[3])) {
                if (!bdRead(b, p[4])) v = 2;
                else v = 3 + bdRead(b, p[5]);
            } else if (!bdRead(b, p[6])) {
                if (!bdRead(b, p[7])) v = 5 + bdRead(b, 159);
                else { v = 7 + 2 * bdRead(b, 165); v += bdRead(b, 145); }
            } else {
                const int b1 = bdRead(b, p[8]);
                const int cat = 2 * b1 + bdRead(b, p[9 + b1]);
                int extra = 0;
                for (const uint8_t* q = kPcat[cat]; *q; ++q) extra = extra * 2 + bdRead(b, *q);
                v = kCatBase[cat] + extra;
            }
            ctx = 2;
        }
        if (bdRead(b, 128)) v = -v;
        coef[kZigzag[i]] = (int16_t)(v * (i > 0 ? ac : dc));
        if (++i == 16) return 16;
        p = probs[kBands[i]][ctx];
        if (!bdRead(b, p[0])) return i;
    }
}

/* ---- transforms (section 14) ----------------------------------------------------------------- */

static void inverseWht(const int16_t* in, int16_t* dcOut /* 16 */) {
    int t[16];
    for (int i = 0; i < 4; ++i) {
        const int a1 = in[i] + in[12 + i], b1 = in[4 + i] + in[8 + i];
        const int c1 = in[4 + i] - in[8 + i], d1 = in[i] - in[12 + i];
        t[i] = a1 + b1;
        t[4 + i] = c1 + d1;
        t[8 + i] = a1 - b1;
        t[12 + i] = d1 - c1;
    }
    for (int i = 0; i < 4; ++i) {
        const int* r = t + 4 * i;
        const int a1 = r[0] + r[3], b1 = r[1] + r[2], c1 = r[1] - r[2], d1 = r[0] - r[3];
        dcOut[4 * i + 0] = (int16_t)((a1 + b1 + 3) >> 3);
        dcOut[4 * i + 1] = (int16_t)((c1 + d1 + 3) >> 3);
        dcOut[4 * i + 2] = (int16_t)((a1 - b1 + 3) >> 3);
        dcOut[4 * i + 3] = (int16_t)((d1 - c1 + 3) >> 3);
    }
}

/* v * k / 65536, wide enough for what a damaged stream puts in a block's second pass. */
static int mulShift16(int v, int k) { return (int)(((int64_t)v * k) >> 16); }

/* The inverse DCT of a 4x4 block, added to the prediction at dst. */
static void inverseDctAdd(const int16_t* in, uint8_t* dst, int stride) {
    static const int c1k = 20091, s1k = 35468; /* cos(pi/8)*sqrt(2) - 1, sin(pi/8)*sqrt(2), in 1/65536 */
    int t[16];
    for (int i = 0; i < 4; ++i) {
        const int i0 = in[i], i4 = in[4 + i], i8 = in[8 + i], i12 = in[12 + i];
        const int a1 = i0 + i8, b1 = i0 - i8;
        const int c1 = ((i4 * s1k) >> 16) - (i12 + ((i12 * c1k) >> 16));
        const int d1 = (i4 + ((i4 * c1k) >> 16)) + ((i12 * s1k) >> 16);
        t[i] = a1 + d1;
        t[12 + i] = a1 - d1;
        t[4 + i] = b1 + c1;
        t[8 + i] = b1 - c1;
    }
    for (int i = 0; i < 4; ++i) {
        const int* r = t + 4 * i;
        const int a1 = r[0] + r[2], b1 = r[0] - r[2];
        const int c1 = mulShift16(r[1], s1k) - (r[3] + mulShift16(r[3], c1k));
        const int d1 = (r[1] + mulShift16(r[1], c1k)) + mulShift16(r[3], s1k);
        uint8_t* o = dst + i * stride;
        o[0] = clamp255(o[0] + ((a1 + d1 + 4) >> 3));
        o[3] = clamp255(o[3] + ((a1 - d1 + 4) >> 3));
        o[1] = clamp255(o[1] + ((b1 + c1 + 4) >> 3));
        o[2] = clamp255(o[2] + ((b1 - c1 + 4) >> 3));
    }
}

static bool blockEmpty(const int16_t* c) {
    for (int i = 0; i < 16; ++i) if (c[i]) return false;
    return true;
}

/* ---- prediction (section 12) ----------------------------------------------------------------- */

/* A macroblock's working area: row 0 is the row above (with the corner at column 0 and four more
 * pixels above and to the right), column 0 the column to the left. */
#define WB 21

static void predictBlock(uint8_t* wb, int size, int mode, bool haveAbove, bool haveLeft) {
    const uint8_t* A = wb + 1;     /* above */
    const uint8_t P = wb[0];
    uint8_t L[16];
    for (int r = 0; r < size; ++r) L[r] = wb[(r + 1) * WB];
    switch (mode) {
    case DC_PRED: {
        const int shift = size == 16 ? 3 : 2;
        int sum = 0, v;
        if (haveAbove && haveLeft) {
            for (int i = 0; i < size; ++i) sum += A[i] + L[i];
            v = (sum + size) >> (shift + 2);
        } else if (haveAbove || haveLeft) {
            for (int i = 0; i < size; ++i) sum += haveAbove ? A[i] : L[i];
            v = (sum + (size >> 1)) >> (shift + 1);
        } else v = 128;
        for (int r = 0; r < size; ++r) memset(wb + (r + 1) * WB + 1, v, (size_t)size);
        break;
    }
    case V_PRED:
        for (int r = 0; r < size; ++r) memcpy(wb + (r + 1) * WB + 1, A, (size_t)size);
        break;
    case H_PRED:
        for (int r = 0; r < size; ++r) memset(wb + (r + 1) * WB + 1, L[r], (size_t)size);
        break;
    default: /* TM_PRED */
        for (int r = 0; r < size; ++r)
            for (int c = 0; c < size; ++c) wb[(r + 1) * WB + 1 + c] = clamp255(L[r] + A[c] - P);
        break;
    }
}

static uint8_t avg3(int x, int y, int z) { return (uint8_t)((x + y + y + z + 2) >> 2); }
static uint8_t avg2(int x, int y) { return (uint8_t)((x + y + 1) >> 1); }

/* A 4x4 subblock (section 12.3): A[-1] is the corner, A[0..7] the row above and to the right,
 * L[0..3] the column to the left. */
static void predictSubblock(uint8_t B[4][4], const uint8_t* A, const uint8_t* L, int mode) {
    uint8_t E[9];
    E[0] = L[3]; E[1] = L[2]; E[2] = L[1]; E[3] = L[0];
    E[4] = A[-1];
    E[5] = A[0]; E[6] = A[1]; E[7] = A[2]; E[8] = A[3];
#define AVG3P(p) avg3((p)[-1], (p)[0], (p)[1])
#define AVG2P(p) avg2((p)[0], (p)[1])
    switch (mode) {
    case B_DC_PRED: {
        int v = 4;
        for (int i = 0; i < 4; ++i) v += A[i] + L[i];
        v >>= 3;
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) B[r][c] = (uint8_t)v;
        break;
    }
    case B_TM_PRED:
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) B[r][c] = clamp255(L[r] + A[c] - A[-1]);
        break;
    case B_VE_PRED:
        for (int c = 0; c < 4; ++c) B[0][c] = B[1][c] = B[2][c] = B[3][c] = AVG3P(A + c);
        break;
    case B_HE_PRED: {
        const uint8_t Lp[5] = {A[-1], L[0], L[1], L[2], L[3]}; /* (L[-1] is the corner) */
        for (int r = 0; r < 3; ++r) { const uint8_t v = AVG3P(Lp + 1 + r); B[r][0] = B[r][1] = B[r][2] = B[r][3] = v; }
        B[3][0] = B[3][1] = B[3][2] = B[3][3] = avg3(L[2], L[3], L[3]);
        break;
    }
    case B_LD_PRED:
        B[0][0] = AVG3P(A + 1);
        B[0][1] = B[1][0] = AVG3P(A + 2);
        B[0][2] = B[1][1] = B[2][0] = AVG3P(A + 3);
        B[0][3] = B[1][2] = B[2][1] = B[3][0] = AVG3P(A + 4);
        B[1][3] = B[2][2] = B[3][1] = AVG3P(A + 5);
        B[2][3] = B[3][2] = AVG3P(A + 6);
        B[3][3] = avg3(A[6], A[7], A[7]);
        break;
    case B_RD_PRED:
        B[3][0] = AVG3P(E + 1);
        B[3][1] = B[2][0] = AVG3P(E + 2);
        B[3][2] = B[2][1] = B[1][0] = AVG3P(E + 3);
        B[3][3] = B[2][2] = B[1][1] = B[0][0] = AVG3P(E + 4);
        B[2][3] = B[1][2] = B[0][1] = AVG3P(E + 5);
        B[1][3] = B[0][2] = AVG3P(E + 6);
        B[0][3] = AVG3P(E + 7);
        break;
    case B_VR_PRED:
        B[3][0] = AVG3P(E + 2);
        B[2][0] = AVG3P(E + 3);
        B[3][1] = B[1][0] = AVG3P(E + 4);
        B[2][1] = B[0][0] = AVG2P(E + 4);
        B[3][2] = B[1][1] = AVG3P(E + 5);
        B[2][2] = B[0][1] = AVG2P(E + 5);
        B[3][3] = B[1][2] = AVG3P(E + 6);
        B[2][3] = B[0][2] = AVG2P(E + 6);
        B[1][3] = AVG3P(E + 7);
        B[0][3] = AVG2P(E + 7);
        break;
    case B_VL_PRED:
        B[0][0] = AVG2P(A);
        B[1][0] = AVG3P(A + 1);
        B[2][0] = B[0][1] = AVG2P(A + 1);
        B[1][1] = B[3][0] = AVG3P(A + 2);
        B[2][1] = B[0][2] = AVG2P(A + 2);
        B[3][1] = B[1][2] = AVG3P(A + 3);
        B[2][2] = B[0][3] = AVG2P(A + 3);
        B[3][2] = B[1][3] = AVG3P(A + 4);
        B[2][3] = AVG3P(A + 5);
        B[3][3] = AVG3P(A + 6);
        break;
    case B_HD_PRED:
        B[3][0] = AVG2P(E);
        B[3][1] = AVG3P(E + 1);
        B[2][0] = B[3][2] = AVG2P(E + 1);
        B[2][1] = B[3][3] = AVG3P(E + 2);
        B[2][2] = B[1][0] = AVG2P(E + 2);
        B[2][3] = B[1][1] = AVG3P(E + 3);
        B[1][2] = B[0][0] = AVG2P(E + 3);
        B[1][3] = B[0][1] = AVG3P(E + 4);
        B[0][2] = AVG3P(E + 5);
        B[0][3] = AVG3P(E + 6);
        break;
    default: /* B_HU_PRED */
        B[0][0] = AVG2P(L);
        B[0][1] = AVG3P(L + 1);
        B[0][2] = B[1][0] = AVG2P(L + 1);
        B[0][3] = B[1][1] = AVG3P(L + 2);
        B[1][2] = B[2][0] = AVG2P(L + 2);
        B[1][3] = B[2][1] = avg3(L[2], L[3], L[3]);
        B[2][2] = B[2][3] = B[3][0] = B[3][1] = B[3][2] = B[3][3] = L[3];
        break;
    }
#undef AVG3P
#undef AVG2P
}

/* Fills a working area from the plane: the row above (127 on the top row), the column to the left
 * (129 on the left edge), the corner, and for luma the four pixels above and to the right. */
static void loadEdges(uint8_t* wb, const uint8_t* plane, int stride, int size, int mx, int my, int mbw) {
    const int x0 = mx * size, y0 = my * size;
    if (my == 0) memset(wb, 127, size + 5);
    else {
        const uint8_t* above = plane + (size_t)(y0 - 1) * stride + x0;
        wb[0] = mx == 0 ? 129 : above[-1];
        memcpy(wb + 1, above, (size_t)size);
        if (size == 16) {
            if (mx == mbw - 1) memset(wb + 17, above[15], 4);
            else memcpy(wb + 17, above + 16, 4);
        }
    }
    for (int r = 0; r < size; ++r) wb[(r + 1) * WB] = mx == 0 ? 129 : plane[(size_t)(y0 + r) * stride + x0 - 1];
}

static void storeBlock(const uint8_t* wb, uint8_t* plane, int stride, int size, int mx, int my) {
    for (int r = 0; r < size; ++r) memcpy(plane + (size_t)(my * size + r) * stride + mx * size, wb + (r + 1) * WB + 1, (size_t)size);
}

/* ---- one macroblock -------------------------------------------------------------------------- */

static void decodeMacroblock(Vp8* d, int mx, int my) {
    BoolDec* b = &d->first;
    MbInfo* mi = &d->info[(size_t)my * d->mbw + mx];
    mi->segment = d->updateMap ? (uint8_t)bdTree(b, kSegmentTree, d->segProbs) : 0;
    const bool skip = d->skipEnabled ? bdRead(b, d->skipProb) : false;
    const int ymode = bdTree(b, kKfYModeTree, kKfYModeProbs);
    uint8_t bmodes[16];
    uint8_t* above = d->aboveModes + 4 * mx;
    uint8_t* leftModes = d->leftModes;
    if (mx == 0) memset(leftModes, B_DC_PRED, 4);
    if (ymode == B_PRED) {
        for (int i = 0; i < 16; ++i) {
            const int a = i < 4 ? above[i] : bmodes[i - 4];
            const int l = (i & 3) == 0 ? leftModes[i >> 2] : bmodes[i - 1];
            bmodes[i] = (uint8_t)bdTree(b, kBModeTree, kKfBmodeProbs[a][l]);
        }
    } else {
        const uint8_t implied = ymode == DC_PRED ? B_DC_PRED : ymode == V_PRED ? B_VE_PRED : ymode == H_PRED ? B_HE_PRED : B_TM_PRED;
        memset(bmodes, implied, 16);
    }
    for (int i = 0; i < 4; ++i) { above[i] = bmodes[12 + i]; leftModes[i] = bmodes[4 * i + 3]; }
    const int uvmode = bdTree(b, kUvModeTree, kKfUvModeProbs);
    mi->bpred = ymode == B_PRED;

    /* the residue */
    int16_t coef[25][16];
    memset(coef, 0, sizeof coef);
    uint8_t* an = d->aboveNz + 9 * mx;
    uint8_t* ln = d->leftNz;
    bool any = false;
    if (!skip) {
        BoolDec* t = &d->parts[my % d->nparts];
        const int (*dq)[2] = d->dq[mi->segment];
        int first = 0, ytype = 3;
        if (ymode != B_PRED) {
            const int end = readCoeffs(t, d->probs[1], an[8] + ln[8], 0, dq[1][0], dq[1][1], coef[24]);
            an[8] = ln[8] = end > 0;
            any = any || end > 0;
            first = 1;
            ytype = 0;
        }
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const int end = readCoeffs(t, d->probs[ytype], an[x] + ln[y], first, dq[0][0], dq[0][1], coef[4 * y + x]);
                an[x] = ln[y] = end > first;
                any = any || end > first;
            }
        for (int pl = 0; pl < 2; ++pl)
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x) {
                    const int end = readCoeffs(t, d->probs[2], an[4 + 2 * pl + x] + ln[4 + 2 * pl + y], 0, dq[2][0], dq[2][1], coef[16 + 4 * pl + 2 * y + x]);
                    an[4 + 2 * pl + x] = ln[4 + 2 * pl + y] = end > 0;
                    any = any || end > 0;
                }
        if (ymode != B_PRED) {
            int16_t dc[16];
            inverseWht(coef[24], dc);
            for (int i = 0; i < 16; ++i) coef[i][0] = dc[i];
        }
    } else {
        memset(an, 0, 8);
        memset(ln, 0, 8);
        if (ymode != B_PRED) an[8] = ln[8] = 0;
    }
    mi->coeffs = any;

    /* prediction and reconstruction */
    CscdYuv* o = d->out;
    uint8_t wb[17 * WB];
    loadEdges(wb, o->y, o->yStride, 16, mx, my, d->mbw);
    if (ymode == B_PRED) {
        for (int i = 0; i < 16; ++i) {
            const int bx = i & 3, by = i >> 2;
            uint8_t A[9], L[4], B[4][4];
            const uint8_t* row = wb + (4 * by) * WB + 4 * bx; /* the corner of this subblock */
            A[0] = row[0];
            memcpy(A + 1, row + 1, 4);
            if (bx == 3 && by > 0) memcpy(A + 5, wb + 17, 4);
            else memcpy(A + 5, row + 5, 4);
            for (int r = 0; r < 4; ++r) L[r] = wb[(4 * by + 1 + r) * WB + 4 * bx];
            predictSubblock(B, A + 1, L, bmodes[i]);
            uint8_t* dst = wb + (4 * by + 1) * WB + 4 * bx + 1;
            for (int r = 0; r < 4; ++r) memcpy(dst + r * WB, B[r], 4);
            if (!blockEmpty(coef[i])) inverseDctAdd(coef[i], dst, WB);
        }
    } else {
        predictBlock(wb, 16, ymode, my > 0, mx > 0);
        for (int i = 0; i < 16; ++i)
            if (!blockEmpty(coef[i])) inverseDctAdd(coef[i], wb + (4 * (i >> 2) + 1) * WB + 4 * (i & 3) + 1, WB);
    }
    storeBlock(wb, o->y, o->yStride, 16, mx, my);
    for (int pl = 0; pl < 2; ++pl) {
        uint8_t* plane = pl ? o->v : o->u;
        loadEdges(wb, plane, o->uvStride, 8, mx, my, d->mbw);
        predictBlock(wb, 8, uvmode, my > 0, mx > 0);
        for (int i = 0; i < 4; ++i) {
            const int16_t* c = coef[16 + 4 * pl + i];
            if (!blockEmpty(c)) inverseDctAdd(c, wb + (4 * (i >> 1) + 1) * WB + 4 * (i & 1) + 1, WB);
        }
        storeBlock(wb, plane, o->uvStride, 8, mx, my);
    }
}

/* ---- the loop filter (section 15) ------------------------------------------------------------ */

static int s8(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
static int u2s(uint8_t v) { return (int)v - 128; }
static uint8_t s2u(int v) { return (uint8_t)(s8(v) + 128); }

static int commonAdjust(bool outer, uint8_t* p, int step) {
    const int p1 = u2s(p[-2 * step]), p0 = u2s(p[-step]), q0 = u2s(p[0]), q1 = u2s(p[step]);
    int a = s8((outer ? s8(p1 - q1) : 0) + 3 * (q0 - p0));
    const int b = s8(a + 3) >> 3;
    a = s8(a + 4) >> 3;
    p[0] = s2u(q0 - a);
    p[-step] = s2u(p0 + b);
    return a;
}

static int iabs(int v) { return v < 0 ? -v : v; }

static bool edgeOk(const uint8_t* p, int step, int E) {
    return iabs(p[-step] - p[0]) * 2 + (iabs(p[-2 * step] - p[step]) >> 1) <= E;
}

static bool filterYes(const uint8_t* p, int step, int I, int E) {
    const int p3 = p[-4 * step], p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
    const int q0 = p[0], q1 = p[step], q2 = p[2 * step], q3 = p[3 * step];
    return edgeOk(p, step, E) && iabs(p3 - p2) <= I && iabs(p2 - p1) <= I && iabs(p1 - p0) <= I &&
           iabs(q3 - q2) <= I && iabs(q2 - q1) <= I && iabs(q1 - q0) <= I;
}

static bool highVariance(const uint8_t* p, int step, int t) {
    return iabs(p[-2 * step] - p[-step]) > t || iabs(p[step] - p[0]) > t;
}

/* n pixels along an edge: p points at the first q0, step crosses the edge, along moves along it */
static void mbFilter(uint8_t* p, int step, int along, int n, int E, int I, int hev) {
    for (int k = 0; k < n; ++k, p += along) {
        if (!filterYes(p, step, I, E)) continue;
        if (!highVariance(p, step, hev)) {
            const int p2 = u2s(p[-3 * step]), p1 = u2s(p[-2 * step]), p0 = u2s(p[-step]);
            const int q0 = u2s(p[0]), q1 = u2s(p[step]), q2 = u2s(p[2 * step]);
            const int w = s8(s8(p1 - q1) + 3 * (q0 - p0));
            int a = s8((27 * w + 63) >> 7);
            p[0] = s2u(q0 - a); p[-step] = s2u(p0 + a);
            a = s8((18 * w + 63) >> 7);
            p[step] = s2u(q1 - a); p[-2 * step] = s2u(p1 + a);
            a = s8((9 * w + 63) >> 7);
            p[2 * step] = s2u(q2 - a); p[-3 * step] = s2u(p2 + a);
        } else commonAdjust(true, p, step);
    }
}

static void subFilter(uint8_t* p, int step, int along, int n, int E, int I, int hev) {
    for (int k = 0; k < n; ++k, p += along) {
        if (!filterYes(p, step, I, E)) continue;
        const int p1 = u2s(p[-2 * step]), q1 = u2s(p[step]);
        const bool hv = highVariance(p, step, hev);
        const int a = (commonAdjust(hv, p, step) + 1) >> 1;
        if (!hv) { p[step] = s2u(q1 - a); p[-2 * step] = s2u(p1 + a); }
    }
}

static void simpleFilter(uint8_t* p, int step, int along, int n, int E) {
    for (int k = 0; k < n; ++k, p += along)
        if (edgeOk(p, step, E)) commonAdjust(true, p, step);
}

static void loopFilter(Vp8* d) {
    CscdYuv* o = d->out;
    for (int my = 0; my < d->mbh; ++my)
        for (int mx = 0; mx < d->mbw; ++mx) {
            const MbInfo* mi = &d->info[(size_t)my * d->mbw + mx];
            int level = d->filterLevel;
            if (d->segmentation) {
                level = d->segAbsolute ? d->segFilter[mi->segment] : level + d->segFilter[mi->segment];
                level = clampi(level, 0, 63);
            }
            if (d->lfAdjust) {
                level += d->refDelta[0]; /* (intra) */
                if (mi->bpred) level += d->modeDelta[0];
                level = clampi(level, 0, 63);
            }
            if (!level) continue;
            int interior = level;
            if (d->sharpness) {
                interior >>= d->sharpness > 4 ? 2 : 1;
                if (interior > 9 - d->sharpness) interior = 9 - d->sharpness;
            }
            if (!interior) interior = 1;
            const int hev = level >= 40 ? 2 : level >= 15 ? 1 : 0;
            const int mbE = (level + 2) * 2 + interior, subE = level * 2 + interior;
            const bool inner = mi->bpred || mi->coeffs;
            uint8_t* y = o->y + (size_t)my * 16 * o->yStride + mx * 16;
            const int ys = o->yStride;
            if (d->filterType == 1) {
                if (mx > 0) simpleFilter(y, 1, ys, 16, mbE);
                if (inner) for (int x = 4; x < 16; x += 4) simpleFilter(y + x, 1, ys, 16, subE);
                if (my > 0) simpleFilter(y, ys, 1, 16, mbE);
                if (inner) for (int r = 4; r < 16; r += 4) simpleFilter(y + r * ys, ys, 1, 16, subE);
                continue;
            }
            const int cs = o->uvStride;
            uint8_t* u = o->u + (size_t)my * 8 * cs + mx * 8;
            uint8_t* v = o->v + (size_t)my * 8 * cs + mx * 8;
            if (mx > 0) {
                mbFilter(y, 1, ys, 16, mbE, interior, hev);
                mbFilter(u, 1, cs, 8, mbE, interior, hev);
                mbFilter(v, 1, cs, 8, mbE, interior, hev);
            }
            if (inner) {
                for (int x = 4; x < 16; x += 4) subFilter(y + x, 1, ys, 16, subE, interior, hev);
                subFilter(u + 4, 1, cs, 8, subE, interior, hev);
                subFilter(v + 4, 1, cs, 8, subE, interior, hev);
            }
            if (my > 0) {
                mbFilter(y, ys, 1, 16, mbE, interior, hev);
                mbFilter(u, cs, 1, 8, mbE, interior, hev);
                mbFilter(v, cs, 1, 8, mbE, interior, hev);
            }
            if (inner) {
                for (int r = 4; r < 16; r += 4) subFilter(y + r * ys, ys, 1, 16, subE, interior, hev);
                subFilter(u + 4 * cs, cs, 1, 8, subE, interior, hev);
                subFilter(v + 4 * cs, cs, 1, 8, subE, interior, hev);
            }
        }
}

/* ---- the frame ------------------------------------------------------------------------------- */

void cscdYuvFree(CscdYuv* yuv) {
    cscdFree(yuv->y);
    memset(yuv, 0, sizeof *yuv);
}

bool cscdDecodeVP8(const uint8_t* data, size_t size, CscdYuv* out) {
    memset(out, 0, sizeof *out);
    if (size < 10) return false;
    const uint32_t tag = data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16);
    if (tag & 1) return false; /* (not a key frame) */
    const size_t firstSize = tag >> 5;
    if (data[3] != 0x9D || data[4] != 0x01 || data[5] != 0x2A) return false;
    const int width = (data[6] | (data[7] << 8)) & 0x3FFF, height = (data[8] | (data[9] << 8)) & 0x3FFF;
    if (width <= 0 || height <= 0 || (uint64_t)width * height > 100000000) return false;
    data += 10;
    size -= 10;
    if (firstSize > size) return false;

    Vp8 d;
    memset(&d, 0, sizeof d);
    d.width = width;
    d.height = height;
    d.mbw = (width + 15) / 16;
    d.mbh = (height + 15) / 16;
    d.segProbs[0] = d.segProbs[1] = d.segProbs[2] = 255;
    memcpy(d.probs, kDefaultCoeffProbs, sizeof d.probs);
    bdInit(&d.first, data, firstSize);
    parseHeader(&d);
    d.nparts = 1 << bdLiteral(&d.first, 2);
    setupQuant(&d, &d.first);
    bdLiteral(&d.first, 1); /* refresh entropy probs (one frame: nothing to keep) */
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j)
            for (int k = 0; k < 3; ++k)
                for (int l = 0; l < 11; ++l)
                    if (bdRead(&d.first, kCoeffUpdateProbs[i][j][k][l])) d.probs[i][j][k][l] = (uint8_t)bdLiteral(&d.first, 8);
    d.skipEnabled = bdRead(&d.first, 128);
    if (d.skipEnabled) d.skipProb = bdLiteral(&d.first, 8);

    /* the token partitions: their sizes (3 bytes each but the last) follow the first partition */
    const uint8_t* parts = data + firstSize;
    size_t left = size - firstSize;
    const size_t sizesLen = 3 * (size_t)(d.nparts - 1);
    if (left < sizesLen) return false;
    const uint8_t* sizes = parts;
    parts += sizesLen;
    left -= sizesLen;
    for (int i = 0; i < d.nparts; ++i) {
        size_t n = left;
        if (i < d.nparts - 1) {
            n = sizes[3 * i] | ((size_t)sizes[3 * i + 1] << 8) | ((size_t)sizes[3 * i + 2] << 16);
            if (n > left) n = left;
        }
        bdInit(&d.parts[i], parts, n);
        parts += n;
        left -= n;
    }

    out->width = width;
    out->height = height;
    out->yStride = d.mbw * 16;
    out->uvStride = d.mbw * 8;
    const size_t ySize = (size_t)out->yStride * d.mbh * 16, uvSize = (size_t)out->uvStride * d.mbh * 8;
    out->y = (uint8_t*)cscdCalloc(ySize + 2 * uvSize, 1);
    out->u = out->y + ySize;
    out->v = out->u + uvSize;
    d.out = out;
    d.info = (MbInfo*)cscdCalloc((size_t)d.mbw * d.mbh, sizeof(MbInfo));
    d.aboveNz = (uint8_t*)cscdCalloc((size_t)d.mbw * 9, 1);
    d.aboveModes = (uint8_t*)cscdCalloc((size_t)d.mbw * 4, 1); /* (B_DC_PRED above the top row) */
    bool ok = true;
    for (int my = 0; my < d.mbh && ok; ++my) {
        memset(d.leftNz, 0, sizeof d.leftNz);
        for (int mx = 0; mx < d.mbw; ++mx) decodeMacroblock(&d, mx, my);
        /* a frame that claims more macroblocks than its bytes hold stops here, not after
         * decoding millions of them from zeros (as libwebp: "premature end-of-file") */
        ok = d.first.past <= VP8_PAST_END;
        for (int i = 0; i < d.nparts; ++i) ok = ok && d.parts[i].past <= VP8_PAST_END;
    }
    if (ok && d.filterLevel) loopFilter(&d);
    cscdFree(d.info);
    cscdFree(d.aboveNz);
    cscdFree(d.aboveModes);
    if (!ok) cscdYuvFree(out);
    return ok;
}
