/* Image codecs: DEFLATE (inflate), PNG (all color types, Adam7), JPEG (baseline and progressive),
 * GIF and WebP (lossless, lossy with alpha, animated), BMP, and a PNG writer. */
#include "image/imageInternal.h"
#include "core/thread.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- images ---------------------------------------------------------------------------------- */

static void imageDtor(void* p) { cscdFree(((CscdImage*)p)->pixels); }

CscdImage* cscdImageNew(int width, int height) {
    CscdImage* img = (CscdImage*)cscdRcNew(sizeof(CscdImage), imageDtor);
    cscdImageInit(img, width, height);
    return img;
}

CscdImage* cscdImageClone(const CscdImage* src) {
    CscdImage* img = cscdImageNew(src->width, src->height);
    if (cscdImagePixelCount(src)) memcpy(img->pixels, src->pixels, cscdImagePixelCount(src) * 4);
    return img;
}

void cscdImageInit(CscdImage* img, int width, int height) {
    img->width = width > 0 ? width : 0;
    img->height = height > 0 ? height : 0;
    img->pixels = (uint32_t*)cscdCalloc(cscdImagePixelCount(img) ? cscdImagePixelCount(img) : 1, 4);
}

void cscdImageFini(CscdImage* img) {
    cscdFree(img->pixels);
    img->pixels = NULL;
    img->width = img->height = 0;
}

void cscdImageResize(CscdImage* img, int width, int height) {
    cscdFree(img->pixels);
    cscdImageInit(img, width, height);
}

/* ---- inflate --------------------------------------------------------------------------------- */

/* LSB-first bits. The buffer is kept filled ahead (zero bytes past the end, counted in pad); reading
 * any of those pad bits is an overflow. */
typedef struct BitReader {
    const uint8_t* p;
    size_t n, i;
    uint32_t buf;
    int cnt, pad;
    bool overflow;
} BitReader;

static inline void refill(BitReader* br) {
    while (br->cnt <= 24) {
        if (br->i < br->n) br->buf |= (uint32_t)br->p[br->i++] << br->cnt;
        else ++br->pad;
        br->cnt += 8;
    }
}

static inline void consume(BitReader* br, int k) {
    br->buf >>= k;
    br->cnt -= k;
    if (br->pad * 8 > br->cnt) br->overflow = true;
}

/* k bits, k <= 25 */
static uint32_t bits(BitReader* br, int k) {
    refill(br);
    const uint32_t v = br->buf & ((1u << k) - 1);
    consume(br, k);
    return v;
}

static void alignByte(BitReader* br) { consume(br, br->cnt % 8); }

/* Bytes of the input read so far (a byte partly read counts). */
static size_t bytesRead(const BitReader* br) {
    const int buffered = br->cnt - br->pad * 8;
    return br->i - (buffered > 0 ? (size_t)buffered / 8 : 0);
}

enum { FAST_BITS = 9 };

typedef struct Huffman {
    uint16_t counts[16];
    uint16_t symbols[320];
    uint16_t fast[1 << FAST_BITS]; /* the next FAST_BITS bits -> symbol << 4 | length; 0: decoded the long way */
} Huffman;

static void huffBuild(Huffman* h, const uint8_t* lengths, int n) {
    memset(h->counts, 0, sizeof h->counts);
    for (int i = 0; i < n; ++i) h->counts[lengths[i]]++;
    h->counts[0] = 0;
    uint16_t offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; ++i) offs[i + 1] = offs[i] + h->counts[i];
    for (int i = 0; i < n; ++i) if (lengths[i]) h->symbols[offs[lengths[i]]++] = (uint16_t)i;
    /* the short codes, bit-reversed (they are read from the low bit) */
    memset(h->fast, 0, sizeof h->fast);
    int code = 0, index = 0;
    for (int len = 1; len <= FAST_BITS; ++len) {
        for (int k = 0; k < h->counts[len]; ++k, ++code, ++index) {
            if (code >= (1 << len)) return; /* (over-subscribed: the long way decides) */
            int rev = 0;
            for (int b = 0; b < len; ++b) rev |= ((code >> b) & 1) << (len - 1 - b);
            for (int j = rev; j < (1 << FAST_BITS); j += 1 << len) h->fast[j] = (uint16_t)(h->symbols[index] << 4 | len);
        }
        code <<= 1;
    }
}

static int huffDecode(const Huffman* h, BitReader* br) {
    refill(br);
    const uint16_t e = h->fast[br->buf & ((1u << FAST_BITS) - 1)];
    if (e) {
        consume(br, e & 15);
        return br->overflow ? -1 : e >> 4;
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= (int)((br->buf >> (len - 1)) & 1);
        int count = h->counts[len];
        if (code - count < first) {
            consume(br, len);
            return br->overflow ? -1 : h->symbols[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    consume(br, 15);
    return -1;
}

static const uint16_t kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static bool inflateBlock(BitReader* br, CscdBytes* out, const Huffman* lit, const Huffman* dist, size_t maxSize) {
    for (;;) {
        int sym = huffDecode(lit, br);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out->len >= maxSize) return false;
            cscdVecPush(out, (uint8_t)sym);
            continue;
        }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        int len = kLenBase[sym] + (int)bits(br, kLenExtra[sym]);
        int ds = huffDecode(dist, br);
        if (ds < 0 || ds >= 30) return false;
        size_t d = kDistBase[ds] + bits(br, kDistExtra[ds]);
        if (d > out->len || (size_t)len > maxSize - out->len) return false;
        size_t start = out->len - d;
        cscdVecReserve(out, out->len + (size_t)len);
        uint8_t* o = out->items;
        size_t at = out->len;
        if (d >= (size_t)len) memcpy(o + at, o + start, (size_t)len); /* (no overlap) */
        else for (int k = 0; k < len; ++k) o[at + k] = o[start + k];
        out->len += (size_t)len;
        if (br->overflow) return false;
    }
}

static Huffman fixedLit, fixedDist;
static CscdOnce fixedOnce = CSCD_ONCE_INIT;
static void initFixed(void) {
    uint8_t l[288];
    for (int i = 0; i < 144; ++i) l[i] = 8;
    for (int i = 144; i < 256; ++i) l[i] = 9;
    for (int i = 256; i < 280; ++i) l[i] = 7;
    for (int i = 280; i < 288; ++i) l[i] = 8;
    huffBuild(&fixedLit, l, 288);
    uint8_t d[30];
    for (int i = 0; i < 30; ++i) d[i] = 5;
    huffBuild(&fixedDist, d, 30);
}

static bool inflateRaw(const uint8_t* data, size_t size, CscdBytes* out, size_t* consumed, size_t maxSize) {
    if (out->len > maxSize) return false;
    BitReader br = {data, size, 0, 0, 0, 0, false};
    for (;;) {
        uint32_t final = bits(&br, 1);
        uint32_t type = bits(&br, 2);
        if (type == 0) {
            alignByte(&br);
            /* whatever is left in the bit buffer is whole bytes */
            uint8_t hdr[4];
            for (int k = 0; k < 4; ++k) hdr[k] = (uint8_t)bits(&br, 8);
            uint16_t len = (uint16_t)(hdr[0] | (hdr[1] << 8));
            if (br.overflow || len > maxSize - out->len) return false;
            cscdVecReserve(out, out->len + len);
            uint32_t k = 0;
            for (; k < len && br.cnt - br.pad * 8 >= 8; ++k) out->items[out->len++] = (uint8_t)bits(&br, 8); /* (the bytes buffered) */
            const size_t rest = len - k;
            if (rest > br.n - br.i) return false;
            if (rest) memcpy(out->items + out->len, br.p + br.i, rest); /* (an empty block, with nothing out yet: no items) */
            out->len += rest;
            br.i += rest;
        } else if (type == 1) {
            cscdCallOnce(&fixedOnce, initFixed);
            if (!inflateBlock(&br, out, &fixedLit, &fixedDist, maxSize)) return false;
        } else if (type == 2) {
            int hlit = (int)bits(&br, 5) + 257, hdist = (int)bits(&br, 5) + 1, hclen = (int)bits(&br, 4) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {0};
            for (int i = 0; i < hclen; ++i) cl[order[i]] = (uint8_t)bits(&br, 3);
            Huffman clh;
            huffBuild(&clh, cl, 19);
            uint8_t lens[320] = {0};
            int n = 0;
            while (n < hlit + hdist) {
                int sym = huffDecode(&clh, &br);
                if (sym < 0) return false;
                if (sym < 16) lens[n++] = (uint8_t)sym;
                else if (sym == 16) {
                    if (n == 0) return false;
                    int r = 3 + (int)bits(&br, 2);
                    while (r-- && n < 320) { lens[n] = lens[n - 1]; ++n; }
                } else if (sym == 17) { int r = 3 + (int)bits(&br, 3); while (r-- && n < 320) lens[n++] = 0; }
                else { int r = 11 + (int)bits(&br, 7); while (r-- && n < 320) lens[n++] = 0; }
            }
            Huffman lit, dist;
            huffBuild(&lit, lens, hlit);
            huffBuild(&dist, lens + hlit, hdist);
            if (!inflateBlock(&br, out, &lit, &dist, maxSize)) return false;
        } else return false;
        if (br.overflow) return false;
        if (final) break;
    }
    if (consumed) *consumed = bytesRead(&br);
    return true;
}

bool cscdInflateRaw(const uint8_t* data, size_t size, CscdBytes* out, size_t* consumed) { return inflateRaw(data, size, out, consumed, SIZE_MAX); }

bool cscdInflateZlibLimit(const uint8_t* data, size_t size, CscdBytes* out, size_t maxSize) {
    if (size < 2) return false;
    if ((data[0] & 0x0F) != 8) return false;
    return inflateRaw(data + 2, size - 2, out, NULL, maxSize);
}

bool cscdInflateZlib(const uint8_t* data, size_t size, CscdBytes* out) { return cscdInflateZlibLimit(data, size, out, SIZE_MAX); }

/* ---- PNG ------------------------------------------------------------------------------------- */

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static uint32_t packPremul(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (a < 255) { r = (r * a + 127) / 255; g = (g * a + 127) / 255; b = (b * a + 127) / 255; }
    return b | (g << 8) | (r << 16) | (a << 24);
}

typedef struct PngState {
    const CscdBytes* raw;
    int channels, depth, bpp, ctype;
    const CscdBytes* plte;
    const CscdBytes* trns;
} PngState;

static bool pngUnfilter(const PngState* st, size_t* pos, uint32_t pw, uint32_t ph, CscdBytes* outRows, size_t* strideOut) {
    const size_t stride = ((size_t)pw * st->channels * st->depth + 7) / 8;
    *strideOut = stride;
    cscdVecClear(outRows);
    cscdVecResize(outRows, stride * ph);
    uint8_t* zero = (uint8_t*)cscdCalloc(stride ? stride : 1, 1); /* (the row above the first) */
    const size_t bpp = (size_t)st->bpp;
    for (uint32_t y = 0; y < ph; ++y) {
        if (*pos + 1 + stride > st->raw->len) { cscdFree(zero); return false; }
        uint8_t f = st->raw->items[(*pos)++];
        uint8_t* cur = &outRows->items[y * stride];
        const uint8_t* prev = y ? cur - stride : zero; /* (the rows are one after the other) */
        memcpy(cur, &st->raw->items[*pos], stride);
        *pos += stride;
        /* one loop per filter; the first bpp bytes have nothing to their left */
        switch (f) {
        case 1:
            for (size_t x = bpp; x < stride; ++x) cur[x] = (uint8_t)(cur[x] + cur[x - bpp]);
            break;
        case 2:
            for (size_t x = 0; x < stride; ++x) cur[x] = (uint8_t)(cur[x] + prev[x]);
            break;
        case 3:
            for (size_t x = 0; x < stride; ++x) cur[x] = (uint8_t)(cur[x] + (((x >= bpp ? cur[x - bpp] : 0) + prev[x]) >> 1));
            break;
        case 4:
            for (size_t x = 0; x < stride; ++x) {
                const int a = x >= bpp ? cur[x - bpp] : 0, b = prev[x], c = x >= bpp ? prev[x - bpp] : 0;
                const int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                cur[x] = (uint8_t)(cur[x] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c));
            }
            break;
        default: break;
        }
    }
    cscdFree(zero);
    return true;
}

static uint32_t pngSample(const PngState* st, const uint8_t* row, uint32_t x, int ch) {
    if (st->depth == 8) return row[x * st->channels + ch];
    if (st->depth == 16) return row[(x * st->channels + ch) * 2];
    /* sub-byte depths (grayscale or palette, one channel) */
    int bitsPos = (int)(x * st->depth);
    int v = (row[bitsPos / 8] >> (8 - st->depth - bitsPos % 8)) & ((1 << st->depth) - 1);
    return (uint32_t)v;
}

static uint32_t pngPixel(const PngState* st, const uint8_t* row, uint32_t x) {
    uint32_t r, g, b, a = 255;
    const CscdBytes* trns = st->trns;
    const CscdBytes* plte = st->plte;
    switch (st->ctype) {
    case 0: {
        uint32_t v = pngSample(st, row, x, 0);
        uint32_t vv = st->depth < 8 ? v * 255 / ((1u << st->depth) - 1) : v;
        if (trns->len >= 2) {
            uint32_t t = ((uint32_t)trns->items[0] << 8) | trns->items[1];
            uint32_t raw16 = st->depth == 16 ? ((uint32_t)row[x * 2] << 8 | row[x * 2 + 1]) : v;
            if (raw16 == t) a = 0;
        }
        r = g = b = vv;
        break;
    }
    case 2:
        r = pngSample(st, row, x, 0); g = pngSample(st, row, x, 1); b = pngSample(st, row, x, 2);
        if (trns->len >= 6 && st->depth == 8 && r == trns->items[1] && g == trns->items[3] && b == trns->items[5]) a = 0;
        break;
    case 3: {
        uint32_t idx = pngSample(st, row, x, 0);
        if (idx * 3 + 2 < plte->len) { r = plte->items[idx * 3]; g = plte->items[idx * 3 + 1]; b = plte->items[idx * 3 + 2]; }
        else r = g = b = 0;
        if (idx < trns->len) a = trns->items[idx];
        break;
    }
    case 4: r = g = b = pngSample(st, row, x, 0); a = pngSample(st, row, x, 1); break;
    default: r = pngSample(st, row, x, 0); g = pngSample(st, row, x, 1); b = pngSample(st, row, x, 2); a = pngSample(st, row, x, 3); break;
    }
    return packPremul(r, g, b, a);
}

static CscdImage* decodePNG(const uint8_t* d, size_t n) {
    if (n < 8 || memcmp(d, "\x89PNG\r\n\x1a\n", 8) != 0) return NULL;
    size_t i = 8;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, interlace = 0;
    CscdBytes idat = {0}, plte = {0}, trns = {0}, raw = {0}, rows = {0};
    CscdImage* img = NULL;
    bool header = false;
    while (i + 8 <= n) {
        uint32_t len = be32(d + i);
        const uint8_t* type = d + i + 4;
        const uint8_t* body = d + i + 8;
        if (len > n - i - 8 || n - i - 8 - len < 4) break; /* (the body and its CRC) */
        if (!memcmp(type, "IHDR", 4)) {
            if (header || len < 13) goto done;
            header = true;
            w = be32(body); h = be32(body + 4); depth = body[8]; ctype = body[9]; interlace = body[12];
        } else if (!memcmp(type, "PLTE", 4)) { cscdVecClear(&plte); cscdVecAppend(&plte, body, len); }
        else if (!memcmp(type, "tRNS", 4)) { cscdVecClear(&trns); cscdVecAppend(&trns, body, len); }
        else if (!memcmp(type, "IDAT", 4)) cscdVecAppend(&idat, body, len);
        else if (!memcmp(type, "IEND", 4)) break;
        i += 12 + (size_t)len;
    }
    if (!w || !h || w > 16384 || h > 16384) goto done;
    /* the bit depths each color type allows */
    {
        const int allowed = ctype == 0 ? 0x1F : ctype == 3 ? 0x0F : (ctype == 2 || ctype == 4 || ctype == 6) ? 0x18 : 0;
        const int bit = depth == 1 ? 1 : depth == 2 ? 2 : depth == 4 ? 4 : depth == 8 ? 8 : depth == 16 ? 16 : 0;
        if (!(allowed & bit) || interlace > 1) goto done;
    }
    PngState st;
    st.channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4;
    /* the filtered rows (each with its filter byte), all passes: what the data must inflate to */
    size_t expected = 0;
    if (!interlace) expected = (((size_t)w * st.channels * depth + 7) / 8 + 1) * h;
    else {
        static const int xs[7] = {0, 4, 0, 2, 0, 1, 0}, ys[7] = {0, 0, 4, 0, 2, 0, 1};
        static const int dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
        for (int p = 0; p < 7; ++p) {
            if (w <= (uint32_t)xs[p] || h <= (uint32_t)ys[p]) continue;
            const size_t pw = (w - xs[p] + dx[p] - 1) / dx[p], ph = (h - ys[p] + dy[p] - 1) / dy[p];
            expected += ((pw * st.channels * depth + 7) / 8 + 1) * ph;
        }
    }
    cscdVecReserve(&raw, cscdMinz(expected, idat.len * 1032 + 1024)); /* (DEFLATE makes at most ~1032 bytes of one) */
    if (!cscdInflateZlibLimit(idat.items, idat.len, &raw, expected + 65536)) goto done; /* (some slack for sloppy encoders) */
    st.raw = &raw;
    st.depth = depth;
    st.bpp = cscdMaxi(1, st.channels * depth / 8);
    st.ctype = ctype;
    st.plte = &plte;
    st.trns = &trns;
    img = cscdImageNew((int)w, (int)h);
    size_t pos = 0, stride = 0;
    if (!interlace) {
        if (!pngUnfilter(&st, &pos, w, h, &rows, &stride)) { cscdRelease(img); img = NULL; goto done; }
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t* out = cscdImageRow(img, (int)y);
            for (uint32_t x = 0; x < w; ++x) out[x] = pngPixel(&st, &rows.items[y * stride], x);
        }
    } else {
        static const int xs[7] = {0, 4, 0, 2, 0, 1, 0}, ys[7] = {0, 0, 4, 0, 2, 0, 1};
        static const int dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
        for (int p = 0; p < 7; ++p) {
            uint32_t pw = (w - xs[p] + dx[p] - 1) / dx[p], ph = (h - ys[p] + dy[p] - 1) / dy[p];
            if (w <= (uint32_t)xs[p] || h <= (uint32_t)ys[p] || !pw || !ph) continue;
            if (!pngUnfilter(&st, &pos, pw, ph, &rows, &stride)) { cscdRelease(img); img = NULL; goto done; }
            for (uint32_t y = 0; y < ph; ++y)
                for (uint32_t x = 0; x < pw; ++x)
                    cscdImageRow(img, (int)(ys[p] + y * dy[p]))[xs[p] + x * dx[p]] = pngPixel(&st, &rows.items[y * stride], x);
        }
    }
done:
    cscdVecFree(&idat);
    cscdVecFree(&plte);
    cscdVecFree(&trns);
    cscdVecFree(&raw);
    cscdVecFree(&rows);
    return img;
}

/* ---- JPEG (baseline and progressive, huffman) ---------------------------------------------- */

/* the largest picture decoded (as many pixels as PNG's 16384 x 16384) */
#define JPEG_MAX_PIXELS ((size_t)1 << 28)
#define JPEG_FAST_BITS 9

typedef struct HT {
    uint8_t bits[17];
    uint8_t vals[256];
    int count;
    int maxcode[18];
    int valptr[17];
    int mincode[17];
    uint16_t fast[1 << JPEG_FAST_BITS]; /* the next JPEG_FAST_BITS bits -> length << 8 | value; 0: the long way */
} HT;

typedef struct Comp {
    int id, h, v, tq, td, ta;
    int pred;
    CscdBytes data;
    int bw, bh;
    CSCD_VEC(int16_t) coef; /* progressive: the coefficients of every block, natural order, not dequantized */
} Comp;

typedef struct Jpeg {
    const uint8_t* d;
    size_t n, i;
    uint16_t qt[4][64];
    HT ht[2][4];
    Comp comps[3];
    int ncomp, width, height, hmax, vmax, restart;
    uint32_t bitbuf; /* the next bits, from the top */
    int bitcnt;
    bool progressive;
    int eobrun;
} Jpeg;

static const uint8_t kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                                    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* The value of the first code that the top bits of v (MSB first) make, its length in *len; -1 when
 * none of max bits or fewer does. */
static int htMatch(const HT* t, uint32_t v, int max, int* len) {
    int code = 0;
    for (int l = 1; l <= max; ++l) {
        code = (code << 1) | (int)((v >> (31 - (l - 1))) & 1);
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l] && code >= t->mincode[l]) { *len = l; return t->vals[t->valptr[l] + code - t->mincode[l]]; }
    }
    return -1;
}

static void buildHT(HT* t) {
    int code = 0, k = 0;
    for (int len = 1; len <= 16; ++len) {
        t->valptr[len] = k;
        t->mincode[len] = code;
        code += t->bits[len];
        k += t->bits[len];
        t->maxcode[len] = t->bits[len] ? code - 1 : -1;
        code <<= 1;
    }
    t->count = k;
    /* each pattern of the first bits: what the long way decodes from it (so the same for any table) */
    for (uint32_t p = 0; p < (1u << JPEG_FAST_BITS); ++p) {
        int len = 0;
        const int v = htMatch(t, p << (32 - JPEG_FAST_BITS), JPEG_FAST_BITS, &len);
        t->fast[p] = v < 0 ? 0 : (uint16_t)(len << 8 | v);
    }
}

/* At least 25 bits in the buffer. Stuffed zeros are dropped; a marker ends the data: it reads as
 * zeros and stays to be found, as does the end. */
static void jfill(Jpeg* j) {
    while (j->bitcnt <= 24) {
        uint32_t b = 0;
        if (j->i < j->n) {
            b = j->d[j->i];
            if (b != 0xFF) ++j->i;
            else if ((j->i + 1 < j->n ? j->d[j->i + 1] : 0) == 0) j->i += 2;
            else b = 0;
        }
        j->bitbuf |= b << (24 - j->bitcnt);
        j->bitcnt += 8;
    }
}

static void jskip(Jpeg* j, int k) {
    j->bitbuf <<= k;
    j->bitcnt -= k;
}

/* The bits buffered dropped (at a restart, and between scans). */
static void jreset(Jpeg* j) {
    j->bitbuf = 0;
    j->bitcnt = 0;
}

static int jbit(Jpeg* j) {
    if (j->bitcnt < 1) jfill(j);
    const int v = (int)(j->bitbuf >> 31);
    jskip(j, 1);
    return v;
}

static int receive(Jpeg* j, int s) {
    if (s <= 0) return 0;
    if (j->bitcnt < s) jfill(j);
    const int v = (int)(j->bitbuf >> (32 - s));
    jskip(j, s);
    return v;
}
static int extend(int v, int s) { return s == 0 ? 0 : (v < (1 << (s - 1)) ? v - (1 << s) + 1 : v); }

static int decodeHuff(Jpeg* j, const HT* t) {
    if (j->bitcnt < 16) jfill(j);
    const uint16_t e = t->fast[j->bitbuf >> (32 - JPEG_FAST_BITS)];
    if (e) {
        jskip(j, e >> 8);
        return e & 255;
    }
    int len = 0;
    const int v = htMatch(t, j->bitbuf, 16, &len);
    jskip(j, v < 0 ? 16 : len);
    return v < 0 ? 0 : v;
}

static float idctC[8][8];
static CscdOnce idctOnce = CSCD_ONCE_INIT;
static void initIdct(void) {
    for (int u = 0; u < 8; ++u)
        for (int x = 0; x < 8; ++x) idctC[u][x] = (u == 0 ? sqrtf(0.125f) : 0.5f) * cscdCosf((2 * x + 1) * u * 3.14159265f / 16);
}

/* v rounded half away from zero (what lroundf gives), clamped to 0..255; without a library call */
static inline int byteOf(float v) {
    const float c = v > 0 ? (v < 255 ? v : 255) : 0; /* (selects, not branches: the loops vectorize) */
    const int i = (int)c;
    return i + (c - (float)i >= 0.5f);
}

/* The rows of eight run side by side: each sum is still taken term by term in the same order, so the
 * values are those of one sum at a time. */
static void idct(const int* in, uint8_t* out, int stride) {
    cscdCallOnce(&idctOnce, initIdct);
    float tmp[64];
    /* most rows of coefficients are all zero, and so are their sums: left out (the same values) */
    int rows[8], nrows = 0;
    for (int y = 0; y < 8; ++y) {
        const int* r = in + y * 8;
        if (!(r[0] | r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7])) continue;
        rows[nrows++] = y;
        /* and so are the zero coefficients of a row (s + 0 * c is s, the sum of the same terms) */
        float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int u = 0; u < 8; ++u) {
            if (!r[u]) continue;
            const float cu = (float)r[u];
            for (int x = 0; x < 8; ++x) s[x] += idctC[u][x] * cu;
        }
        memcpy(tmp + y * 8, s, sizeof s);
    }
    if (nrows == 0) { /* (a flat block) */
        for (int y = 0; y < 8; ++y) memset(out + y * stride, 128, 8);
        return;
    }
    for (int y = 0; y < 8; ++y) {
        float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int k = 0; k < nrows; ++k) {
            const float c = idctC[rows[k]][y];
            const float* t = tmp + rows[k] * 8;
            for (int x = 0; x < 8; ++x) s[x] += c * t[x];
        }
        uint8_t* o = out + (size_t)y * stride;
        for (int x = 0; x < 8; ++x) o[x] = (uint8_t)byteOf(s[x] + 128);
    }
}

static void decodeBlock(Jpeg* j, Comp* c, int* blk) {
    memset(blk, 0, 64 * sizeof(int));
    const HT* dc = &j->ht[0][c->td];
    const HT* ac = &j->ht[1][c->ta];
    int t = decodeHuff(j, dc) & 15; /* (a size past 11 is broken data; kept to what the arithmetic takes) */
    int diff = t ? extend(receive(j, t), t) : 0;
    c->pred += diff;
    blk[0] = c->pred * j->qt[c->tq][0];
    for (int k = 1; k < 64;) {
        int rs = decodeHuff(j, ac);
        int r = rs >> 4, s = rs & 15;
        if (s == 0) { if (r == 15) { k += 16; continue; } break; }
        k += r;
        if (k > 63) break;
        blk[kZigzag[k]] = extend(receive(j, s), s) * j->qt[c->tq][k];
        ++k;
    }
}

/* The components (upsampled) as RGB. */
/* The chroma terms of YCbCr to RGB for each sample: 1.402 (Cr - 128) and so on, the same floats. */
static struct { float crR[256], cbG[256], crG[256], cbB[256]; } kYcc;
static CscdOnce yccOnce = CSCD_ONCE_INIT;
static void initYcc(void) {
    for (int v = 0; v < 256; ++v) {
        const float c = (float)v - 128;
        kYcc.crR[v] = 1.402f * c;
        kYcc.cbG[v] = 0.344136f * c;
        kYcc.crG[v] = 0.714136f * c;
        kYcc.cbB[v] = 1.772f * c;
    }
}

static CscdImage* jpegToImage(Jpeg* j) {
    cscdCallOnce(&yccOnce, initYcc);
    CscdImage* img = cscdImageNew(j->width, j->height);
    /* each component's sample for a column (upsampled), worked out once */
    int* cols[3] = {NULL, NULL, NULL};
    for (int ci = 0; ci < j->ncomp && ci < 3; ++ci) {
        cols[ci] = (int*)cscdMalloc((size_t)cscdMaxi(1, j->width) * sizeof(int));
        for (int x = 0; x < j->width; ++x) cols[ci][x] = x * j->comps[ci].h / j->hmax;
    }
    for (int y = 0; y < j->height; ++y) {
        uint32_t* out = cscdImageRow(img, y);
        const uint8_t* rows[3] = {NULL, NULL, NULL};
        for (int ci = 0; ci < j->ncomp && ci < 3; ++ci) rows[ci] = j->comps[ci].data.items + (size_t)(y * j->comps[ci].v / j->vmax) * j->comps[ci].bw;
        if (j->ncomp == 1) {
            for (int x = 0; x < j->width; ++x) {
                const uint32_t g = rows[0][cols[0][x]];
                out[x] = g | (g << 8) | (g << 16) | 0xFF000000u;
            }
            continue;
        }
        for (int x = 0; x < j->width; ++x) {
            const float Y = (float)rows[0][cols[0][x]];
            const int cb = rows[1][cols[1][x]], cr = rows[2][cols[2][x]];
            const int r = byteOf(Y + kYcc.crR[cr]);
            const int g = byteOf(Y - kYcc.cbG[cb] - kYcc.crG[cr]);
            const int b = byteOf(Y + kYcc.cbB[cb]);
            out[x] = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | 0xFF000000u;
        }
    }
    for (int ci = 0; ci < 3; ++ci) cscdFree(cols[ci]);
    return img;
}

static CscdImage* jpegScan(Jpeg* j) {
    if (!j->width || !j->height) return NULL;
    int mcuw = 8 * j->hmax, mcuh = 8 * j->vmax;
    int mx = (j->width + mcuw - 1) / mcuw, my = (j->height + mcuh - 1) / mcuh;
    for (int c = 0; c < j->ncomp; ++c) {
        Comp* cp = &j->comps[c];
        cp->bw = mx * cp->h * 8;
        cp->bh = my * cp->v * 8;
        cscdVecClear(&cp->data);
        cscdVecResize(&cp->data, (size_t)cp->bw * cp->bh);
        cp->pred = 0;
    }
    int blk[64];
    int mcus = 0;
    for (int y = 0; y < my; ++y)
        for (int x = 0; x < mx; ++x) {
            if (j->restart && mcus && mcus % j->restart == 0) {
                jreset(j);
                /* skip to the restart marker */
                while (j->i + 1 < j->n && !(j->d[j->i] == 0xFF && j->d[j->i + 1] >= 0xD0 && j->d[j->i + 1] <= 0xD7)) ++j->i;
                j->i += 2;
                for (int c = 0; c < j->ncomp; ++c) j->comps[c].pred = 0;
            }
            for (int c = 0; c < j->ncomp; ++c) {
                Comp* cp = &j->comps[c];
                for (int v = 0; v < cp->v; ++v)
                    for (int h = 0; h < cp->h; ++h) {
                        decodeBlock(j, cp, blk);
                        int bx = (x * cp->h + h) * 8, by = (y * cp->v + v) * 8;
                        idct(blk, &cp->data.items[(size_t)by * cp->bw + bx], cp->bw);
                    }
            }
            ++mcus;
        }
    return jpegToImage(j);
}

/* ---- progressive: the coefficients come in several scans ---- */

static void setupBlocks(Jpeg* j) {
    int mcuw = 8 * j->hmax, mcuh = 8 * j->vmax;
    int mx = (j->width + mcuw - 1) / mcuw, my = (j->height + mcuh - 1) / mcuh;
    for (int c = 0; c < j->ncomp; ++c) {
        Comp* cp = &j->comps[c];
        cp->bw = mx * cp->h * 8;
        cp->bh = my * cp->v * 8;
        cscdVecClear(&cp->coef);
        cscdVecResize(&cp->coef, (size_t)cp->bw * cp->bh);
    }
}

static int16_t* blockAt(Comp* c, int bx, int by) { return &c->coef.items[((size_t)by * (c->bw / 8) + bx) * 64]; }

static void decodeDC(Jpeg* j, Comp* c, int16_t* blk, int ah, int al) {
    if (ah == 0) {
        const int t = decodeHuff(j, &j->ht[0][c->td]) & 15;
        c->pred += t ? extend(receive(j, t), t) : 0;
        blk[0] = (int16_t)(c->pred * (1 << al));
    } else if (jbit(j)) blk[0] = (int16_t)(blk[0] | (1 << al));
}

static void decodeACFirst(Jpeg* j, Comp* c, int16_t* blk, int ss, int se, int al) {
    if (j->eobrun > 0) { --j->eobrun; return; }
    for (int k = ss; k <= se; ++k) {
        const int rs = decodeHuff(j, &j->ht[1][c->ta]);
        const int r = rs >> 4, sz = rs & 15;
        if (sz == 0) {
            if (r < 15) { j->eobrun = (1 << r) - 1; if (r) j->eobrun += receive(j, r); break; }
            k += 15; /* (sixteen zeros) */
            continue;
        }
        k += r;
        if (k > 63) break;
        blk[kZigzag[k]] = (int16_t)(extend(receive(j, sz), sz) * (1 << al));
    }
}

static void refine(Jpeg* j, int16_t* coef, int p1, int m1) {
    if (jbit(j) && (*coef & p1) == 0) *coef = (int16_t)(*coef >= 0 ? *coef + p1 : *coef + m1);
}

static void decodeACRefine(Jpeg* j, Comp* c, int16_t* blk, int ss, int se, int al) {
    const int p1 = 1 << al, m1 = -1 * (1 << al);
    int k = ss;
    if (j->eobrun <= 0) {
        for (; k <= se; ++k) {
            const int rs = decodeHuff(j, &j->ht[1][c->ta]);
            int r = rs >> 4, sz = rs & 15, value = 0;
            if (sz) value = jbit(j) ? p1 : m1;
            else if (r != 15) {
                j->eobrun = 1 << r;
                if (r) j->eobrun += receive(j, r);
                break;
            }
            /* skip r zero coefficients (refining the nonzero ones passed), then place the new one */
            while (k <= se) {
                int16_t* coef = &blk[kZigzag[k]];
                if (*coef != 0) refine(j, coef, p1, m1);
                else {
                    if (r == 0) break;
                    --r;
                }
                ++k;
            }
            if (value && k <= se) blk[kZigzag[k]] = (int16_t)value;
        }
    }
    if (j->eobrun > 0) {
        for (; k <= se; ++k) {
            int16_t* coef = &blk[kZigzag[k]];
            if (*coef != 0) refine(j, coef, p1, m1);
        }
        --j->eobrun;
    }
}

static void restartIfDue(Jpeg* j, int units, const int* in, int nin) {
    if (!j->restart || !units || units % j->restart) return;
    jreset(j);
    while (j->i + 1 < j->n && !(j->d[j->i] == 0xFF && j->d[j->i + 1] >= 0xD0 && j->d[j->i + 1] <= 0xD7)) ++j->i;
    j->i += 2;
    j->eobrun = 0;
    for (int k = 0; k < nin; ++k) j->comps[in[k]].pred = 0;
}

static bool progressiveScan(Jpeg* j, const int* in, int nin, int ss, int se, int ah, int al) {
    if (!j->width || !j->height || nin == 0 || ss > se || se > 63) return false;
    if (j->comps[0].coef.len == 0) setupBlocks(j);
    jreset(j);
    j->eobrun = 0;
    for (int k = 0; k < nin; ++k) j->comps[in[k]].pred = 0;
    int units = 0;
    if (nin == 1) {
        /* one component: its blocks in order, as many as its own size needs */
        Comp* c = &j->comps[in[0]];
        const int cw = (j->width * c->h + j->hmax - 1) / j->hmax, ch = (j->height * c->v + j->vmax - 1) / j->vmax;
        const int bxN = (cw + 7) / 8, byN = (ch + 7) / 8;
        for (int by = 0; by < byN; ++by)
            for (int bx = 0; bx < bxN; ++bx) {
                restartIfDue(j, units, in, nin);
                int16_t* blk = blockAt(c, bx, by);
                if (ss == 0) decodeDC(j, c, blk, ah, al);
                else if (ah == 0) decodeACFirst(j, c, blk, ss, se, al);
                else decodeACRefine(j, c, blk, ss, se, al);
                ++units;
            }
        return true;
    }
    if (ss != 0) return false; /* (interleaved scans carry DC only) */
    const int mx = j->comps[0].bw / (8 * j->comps[0].h), my = j->comps[0].bh / (8 * j->comps[0].v);
    for (int y = 0; y < my; ++y)
        for (int x = 0; x < mx; ++x) {
            restartIfDue(j, units, in, nin);
            for (int k = 0; k < nin; ++k) {
                Comp* c = &j->comps[in[k]];
                for (int v = 0; v < c->v; ++v)
                    for (int h = 0; h < c->h; ++h) decodeDC(j, c, blockAt(c, x * c->h + h, y * c->v + v), ah, al);
            }
            ++units;
        }
    return true;
}

static CscdImage* finishProgressive(Jpeg* j) {
    if (!j->width || !j->height || j->comps[0].coef.len == 0) return NULL;
    int blk[64];
    for (int ci = 0; ci < j->ncomp; ++ci) {
        Comp* c = &j->comps[ci];
        cscdVecClear(&c->data);
        cscdVecResize(&c->data, (size_t)c->bw * c->bh);
        for (int by = 0; by < c->bh / 8; ++by)
            for (int bx = 0; bx < c->bw / 8; ++bx) {
                const int16_t* q = blockAt(c, bx, by);
                for (int k = 0; k < 64; ++k) blk[kZigzag[k]] = q[kZigzag[k]] * j->qt[c->tq][k];
                idct(blk, &c->data.items[(size_t)by * 8 * c->bw + bx * 8], c->bw);
            }
    }
    return jpegToImage(j);
}

static CscdImage* jpegRun(Jpeg* j) {
    const uint8_t* d = j->d;
    const size_t n = j->n;
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return NULL;
    j->i = 2;
    while (j->i + 4 <= n) {
        if (d[j->i] != 0xFF) { ++j->i; continue; }
        uint8_t m = d[j->i + 1];
        j->i += 2;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0xFF) continue;
        if (m == 0xD9) break;
        uint16_t len = (uint16_t)((d[j->i] << 8) | d[j->i + 1]);
        const uint8_t* s = d + j->i + 2;
        size_t segEnd = j->i + len;
        if (len < 2 || segEnd > n) return NULL;
        const size_t slen = (size_t)len - 2; /* (the bytes of s) */
        if (m == 0xDB) {
            size_t k = 0;
            while (k < slen) {
                int pq = s[k] >> 4, tq = s[k] & 3;
                if (k + 1 + (pq ? 128 : 64) > slen) break;
                ++k;
                for (int q = 0; q < 64; ++q) { j->qt[tq][q] = pq ? (uint16_t)((s[k] << 8) | s[k + 1]) : s[k]; k += pq ? 2 : 1; }
            }
        } else if (m == 0xC4) {
            size_t k = 0;
            while (k + 17 <= slen) {
                int tc = s[k] >> 4, th = s[k] & 3;
                HT* t = &j->ht[tc & 1][th];
                int total = 0;
                for (int q = 1; q <= 16; ++q) total += s[k + q];
                if (total > 256 || k + 17 + (size_t)total > slen) return NULL;
                t->bits[0] = 0;
                for (int q = 1; q <= 16; ++q) t->bits[q] = s[k + q];
                k += 17;
                for (int q = 0; q < total; ++q) t->vals[q] = s[k + q];
                k += total;
                buildHT(t);
            }
        } else if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            if (j->width || slen < 6) return NULL; /* (one frame) */
            j->progressive = m == 0xC2;
            j->height = (s[1] << 8) | s[2];
            j->width = (s[3] << 8) | s[4];
            j->ncomp = s[5];
            if (j->ncomp != 1 && j->ncomp != 3) return NULL;
            if (slen < 6 + 3 * (size_t)j->ncomp) return NULL;
            if ((size_t)j->width * j->height > JPEG_MAX_PIXELS) return NULL;
            for (int c = 0; c < j->ncomp; ++c) {
                j->comps[c].id = s[6 + 3 * c];
                j->comps[c].h = s[7 + 3 * c] >> 4;
                j->comps[c].v = s[7 + 3 * c] & 15;
                j->comps[c].tq = s[8 + 3 * c] & 3;
                if (j->comps[c].h < 1 || j->comps[c].h > 4 || j->comps[c].v < 1 || j->comps[c].v > 4) return NULL;
                j->hmax = cscdMaxi(j->hmax, j->comps[c].h);
                j->vmax = cscdMaxi(j->vmax, j->comps[c].v);
            }
        } else if (m == 0xDD) {
            if (slen >= 2) j->restart = (s[0] << 8) | s[1];
        } else if (m == 0xDA) {
            if (slen < 1) return NULL;
            int ns = s[0];
            if (slen < 4 + 2 * (size_t)ns) return NULL;
            int in[4];
            int nin = 0;
            for (int k = 0; k < ns; ++k) {
                int id = s[1 + 2 * k];
                for (int c = 0; c < j->ncomp; ++c)
                    if (j->comps[c].id == id) {
                        j->comps[c].td = (s[2 + 2 * k] >> 4) & 3;
                        j->comps[c].ta = s[2 + 2 * k] & 3;
                        if (nin < 4) in[nin++] = c;
                    }
            }
            j->i = segEnd;
            if (!j->progressive) return jpegScan(j);
            const int ss = s[1 + 2 * ns], se = s[2 + 2 * ns], ah = s[3 + 2 * ns] >> 4, al = s[3 + 2 * ns] & 15;
            if (!progressiveScan(j, in, nin, ss, se, ah, al)) return NULL;
            /* on to the next marker */
            while (j->i + 1 < n && !(d[j->i] == 0xFF && d[j->i + 1] != 0 && !(d[j->i + 1] >= 0xD0 && d[j->i + 1] <= 0xD7))) ++j->i;
            continue;
        }
        j->i = segEnd;
    }
    return j->progressive ? finishProgressive(j) : NULL; /* (a progressive file may end without EOI) */
}

static CscdImage* decodeJPEG(const uint8_t* d, size_t n) {
    Jpeg* j = CSCD_NEW(Jpeg);
    j->d = d;
    j->n = n;
    j->hmax = j->vmax = 1;
    CscdImage* img = jpegRun(j);
    for (int c = 0; c < 3; ++c) {
        cscdVecFree(&j->comps[c].data);
        cscdVecFree(&j->comps[c].coef);
    }
    cscdFree(j);
    return img;
}

/* ---- BMP ------------------------------------------------------------------------------------- */

static uint32_t le32(const uint8_t* p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static CscdImage* decodeBMP(const uint8_t* d, size_t n) {
    if (n < 54 || d[0] != 'B' || d[1] != 'M') return NULL;
    uint32_t off = le32(d + 10);
    int32_t w = (int32_t)le32(d + 18), h = (int32_t)le32(d + 22);
    int bpp = le16(d + 28);
    uint32_t comp = le32(d + 30);
    bool flip = h > 0;
    h = abs(h);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (bpp != 24 && bpp != 32 && bpp != 8) || (comp != 0 && comp != 3)) return NULL;
    size_t stride = ((size_t)w * bpp / 8 + 3) & ~(size_t)3;
    if (off > n || stride * h > n - off) return NULL;
    /* the palette, after the info header (as many of its 256 colors as the file holds) */
    const size_t palAt = 14 + (size_t)le32(d + 14);
    const size_t colors = bpp == 8 && palAt <= n ? cscdMinz(256, (n - palAt) / 4) : 0;
    CscdImage* img = cscdImageNew(w, h);
    const uint8_t* pal = d + palAt;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = d + off + stride * (flip ? h - 1 - y : y);
        uint32_t* out = cscdImageRow(img, y);
        for (int x = 0; x < w; ++x) {
            uint32_t b, g, r, a = 255;
            if (bpp == 8) {
                if (row[x] < colors) { const uint8_t* p = pal + 4 * row[x]; b = p[0]; g = p[1]; r = p[2]; }
                else b = g = r = 0;
            }
            else { b = row[x * bpp / 8]; g = row[x * bpp / 8 + 1]; r = row[x * bpp / 8 + 2]; if (bpp == 32) a = row[x * 4 + 3]; }
            out[x] = packPremul(r, g, b, a);
        }
    }
    return img;
}

static uint32_t crcTable[256];
static CscdOnce crcOnce = CSCD_ONCE_INIT;
static void initCrc(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t k = i;
        for (int j = 0; j < 8; ++j) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
        crcTable[i] = k;
    }
}

static uint32_t crc32(const uint8_t* p, size_t n, uint32_t c) {
    cscdCallOnce(&crcOnce, initCrc);
    for (size_t i = 0; i < n; ++i) c = crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

/* ---- animations ------------------------------------------------------------------------------ */

static void animationDtor(void* p) {
    CscdAnimatedImage* a = (CscdAnimatedImage*)p;
    for (int i = 0; i < a->frameCount; ++i) cscdRelease(a->frames[i]);
    cscdFree(a->frames);
    cscdFree(a->durations);
}

static CscdAnimatedImage* animationNew(int width, int height) {
    CscdAnimatedImage* a = (CscdAnimatedImage*)cscdRcNew(sizeof(CscdAnimatedImage), animationDtor);
    memset(a, 0, sizeof *a);
    a->width = width;
    a->height = height;
    a->loops = 1;
    return a;
}

/* All the frames of an animation stay in memory, whole: past this many pixels in all, the rest of
 * the frames are left out (the animation plays what it has). */
#define ANIMATION_PIXELS ((size_t)1 << 25)

/* Appends a copy of the canvas as the next frame; false when the animation is full. */
static bool animationPush(CscdAnimatedImage* a, const CscdImage* canvas, int ms) {
    if (a->frameCount && (size_t)(a->frameCount + 1) * cscdImagePixelCount(canvas) > ANIMATION_PIXELS) return false;
    a->frames = (CscdImage**)cscdRealloc(a->frames, sizeof(CscdImage*) * (size_t)(a->frameCount + 1));
    a->durations = (int*)cscdRealloc(a->durations, sizeof(int) * (size_t)(a->frameCount + 1));
    a->frames[a->frameCount] = cscdImageClone(canvas);
    a->durations[a->frameCount] = ms <= 10 ? 100 : ms; /* (as browsers: 0 and 10 ms play at 100) */
    ++a->frameCount;
    return true;
}

/* A shared image of the first frame; the animation is released. */
static CscdImage* firstFrame(CscdAnimatedImage* a) {
    CscdImage* img = a && a->frameCount ? (CscdImage*)cscdRetain(a->frames[0]) : NULL;
    cscdRelease(a);
    return img;
}

/* ---- GIF -------------------------------------------------------------------------------------
 * Every frame composed onto a transparent canvas of the logical screen's size, as browsers show
 * them: global and local palettes, transparency, interlacing, frame offsets, the disposal methods
 * (keep, clear to transparent, restore the previous picture), delays, and the loop count of the
 * NETSCAPE2.0 extension. */

/* One frame's LZW data (its sub-blocks joined) as palette indices, at most count of them. */
static void gifLzw(const CscdBytes* data, int minSize, size_t count, CscdBytes* indices) {
    cscdVecReserve(indices, cscdMinz(count, data->len * 8 + 4096)); /* (a broken header can claim any size) */
    const int clear = 1 << minSize, end = clear + 1;
    uint16_t* prefix = (uint16_t*)cscdCalloc(4096, sizeof(uint16_t));
    uint8_t* suffix = (uint8_t*)cscdCalloc(4096, 1);
    uint8_t* first = (uint8_t*)cscdCalloc(4096, 1);
    CscdBytes stack = {0};
    for (int k = 0; k < clear; ++k) { prefix[k] = 0xFFFF; suffix[k] = (uint8_t)k; first[k] = (uint8_t)k; }
    int codeSize = minSize + 1, next = clear + 2, prev = -1;
    uint32_t buf = 0;
    int cnt = 0;
    size_t p = 0;
    while (indices->len < count) {
        while (cnt < codeSize && p < data->len) { buf |= (uint32_t)data->items[p++] << cnt; cnt += 8; }
        if (cnt < codeSize) break;
        int code = (int)(buf & ((1u << codeSize) - 1));
        buf >>= codeSize;
        cnt -= codeSize;
        if (code == clear) { codeSize = minSize + 1; next = clear + 2; prev = -1; continue; }
        if (code == end) break;
        int emit = code;
        if (prev >= 0 && code >= next) { /* (the code being defined now: prev + its first byte) */
            if (code > next) break;
            cscdVecPush(&stack, first[prev]);
            emit = prev;
        } else if (code >= next) break;
        for (int c = emit; c != 0xFFFF && stack.len < 4096; c = prefix[c]) cscdVecPush(&stack, suffix[c]);
        while (stack.len) cscdVecPush(indices, cscdVecPop(&stack));
        if (prev >= 0 && next < 4096) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = first[code < next ? code : prev];
            first[next] = first[prev];
            ++next;
            if (next == (1 << codeSize) && codeSize < 12) ++codeSize;
        }
        prev = code;
    }
    cscdVecFree(&stack);
    cscdFree(prefix);
    cscdFree(suffix);
    cscdFree(first);
}

static void clearRect(CscdImage* img, int x0, int y0, int w, int h) {
    for (int y = cscdMaxi(0, y0); y < cscdMini(img->height, y0 + h); ++y)
        for (int x = cscdMaxi(0, x0); x < cscdMini(img->width, x0 + w); ++x) cscdImageRow(img, y)[x] = 0;
}

static CscdAnimatedImage* decodeGIF(const uint8_t* d, size_t n, int maxFrames) {
    if (n < 13 || memcmp(d, "GIF8", 4) != 0) return NULL;
    int sw = d[6] | (d[7] << 8), sh = d[8] | (d[9] << 8); /* (0: the first frame's size, as browsers do) */
    if ((uint64_t)sw * sh > 100000000) return NULL;
    size_t i = 13;
    uint32_t global[256] = {0};
    if (d[10] & 0x80) {
        const int count = 2 << (d[10] & 7);
        if (i + 3 * (size_t)count > n) return NULL;
        for (int k = 0; k < count; ++k) global[k] = 0xFF000000u | ((uint32_t)d[i + 3 * k] << 16) | ((uint32_t)d[i + 3 * k + 1] << 8) | d[i + 3 * k + 2];
        i += 3 * (size_t)count;
    }
    CscdAnimatedImage* anim = NULL;
    CscdImage* canvas = NULL;
    CscdImage* saved = NULL; /* the picture before a frame that restores it */
    int transparent = -1, disposal = 0, delay = 0, loops = 1;
    int prevDisposal = 0, px = 0, py = 0, pw = 0, ph = 0;
    while (i < n) {
        const uint8_t kind = d[i++];
        if (kind == 0x3B) break; /* trailer */
        if (kind == 0x21) { /* extension */
            if (i >= n) break;
            const uint8_t label = d[i++];
            if (label == 0xF9 && i + 5 < n && d[i] >= 4) { /* graphics control, for the next frame */
                transparent = (d[i + 1] & 1) ? d[i + 4] : -1;
                disposal = (d[i + 1] >> 2) & 7;
                delay = (d[i + 2] | (d[i + 3] << 8)) * 10;
            } else if (label == 0xFF && i + 16 < n && d[i] == 11 &&
                       (memcmp(d + i + 1, "NETSCAPE2.0", 11) == 0 || memcmp(d + i + 1, "ANIMEXTS1.0", 11) == 0) &&
                       d[i + 12] >= 3 && d[i + 13] == 1) {
                const int count = d[i + 14] | (d[i + 15] << 8);
                loops = count == 0 ? 0 : count + 1; /* (the count is of repeats, after the first time) */
            }
            while (i < n && d[i]) i += 1 + (size_t)d[i];
            ++i;
            continue;
        }
        if (kind != 0x2C || i + 9 > n) break;
        const int fx = d[i] | (d[i + 1] << 8), fy = d[i + 2] | (d[i + 3] << 8);
        const int fw = d[i + 4] | (d[i + 5] << 8), fh = d[i + 6] | (d[i + 7] << 8);
        const uint8_t flags = d[i + 8];
        i += 9;
        if (!canvas) {
            if (sw <= 0 || sh <= 0) { sw = fx + fw; sh = fy + fh; }
            if (sw <= 0 || sh <= 0 || (uint64_t)sw * sh > 100000000) return NULL;
            canvas = cscdImageNew(sw, sh);
            anim = animationNew(sw, sh);
        }
        uint32_t local[256];
        const uint32_t* palette = global;
        if (flags & 0x80) {
            const int count = 2 << (flags & 7);
            if (i + 3 * (size_t)count > n) break;
            for (int k = 0; k < count; ++k) local[k] = 0xFF000000u | ((uint32_t)d[i + 3 * k] << 16) | ((uint32_t)d[i + 3 * k + 1] << 8) | d[i + 3 * k + 2];
            for (int k = count; k < 256; ++k) local[k] = 0;
            palette = local;
            i += 3 * (size_t)count;
        }
        if (i >= n) break;
        const int minSize = d[i++];
        if (minSize < 1 || minSize > 11 || (uint64_t)fw * fh > 100000000) break;
        CscdBytes data = {0};
        while (i < n && d[i]) {
            const size_t len = d[i];
            if (i + 1 + len > n) break;
            cscdVecAppend(&data, d + i + 1, len);
            i += 1 + len;
        }
        ++i;
        CscdBytes indices = {0};
        gifLzw(&data, minSize, (size_t)fw * fh, &indices);
        cscdVecFree(&data);
        /* what the frame before leaves */
        if (prevDisposal == 2) clearRect(canvas, px, py, pw, ph);
        else if (prevDisposal == 3 && saved) memcpy(canvas->pixels, saved->pixels, cscdImagePixelCount(canvas) * 4);
        if (disposal == 3) {
            if (!saved) saved = cscdImageNew(sw, sh);
            memcpy(saved->pixels, canvas->pixels, cscdImagePixelCount(canvas) * 4);
        }
        /* onto the canvas, in the interlaced row order when there is one */
        CscdIntVec rows = {0};
        if (flags & 0x40) {
            for (int y = 0; y < fh; y += 8) cscdVecPush(&rows, y);
            for (int y = 4; y < fh; y += 8) cscdVecPush(&rows, y);
            for (int y = 2; y < fh; y += 4) cscdVecPush(&rows, y);
            for (int y = 1; y < fh; y += 2) cscdVecPush(&rows, y);
        } else for (int y = 0; y < fh; ++y) cscdVecPush(&rows, y);
        size_t k = 0;
        for (int r = 0; r < (int)rows.len && k < indices.len; ++r) {
            const int y = fy + rows.items[r];
            for (int x = 0; x < fw && k < indices.len; ++x, ++k) {
                const int idx = indices.items[k];
                if (idx == transparent || y >= sh || fx + x >= sw) continue;
                const uint32_t c = palette[idx];
                cscdImageRow(canvas, y)[fx + x] = c ? packPremul((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) : 0;
            }
        }
        cscdVecFree(&rows);
        cscdVecFree(&indices);
        if (!animationPush(anim, canvas, delay) || anim->frameCount >= maxFrames) break;
        prevDisposal = disposal;
        px = fx; py = fy; pw = fw; ph = fh;
        transparent = -1;
        disposal = delay = 0;
    }
    cscdRelease(canvas);
    cscdRelease(saved);
    if (anim) anim->loops = loops;
    if (anim && !anim->frameCount) { cscdRelease(anim); return NULL; }
    return anim;
}

/* ---- WebP (lossless) ------------------------------------------------------------------------
 * VP8L: prefix-coded ARGB with LZ77 back-references and a color cache, under the predictor, color,
 * subtract-green and color-indexing transforms. */

typedef struct VP8LCode { /* a canonical prefix code, read a bit at a time (as DEFLATE's) */
    uint16_t counts[16];
    CscdU16Vec symbols;
    int single; /* only one symbol: it takes no bits */
} VP8LCode;

static bool codeBuild(VP8LCode* c, const uint8_t* lengths, size_t nlen) {
    memset(c->counts, 0, sizeof c->counts);
    c->single = -1;
    int used = 0, last = 0;
    for (size_t s = 0; s < nlen; ++s)
        if (lengths[s]) { if (lengths[s] > 15) return false; c->counts[lengths[s]]++; ++used; last = (int)s; }
    if (used == 0) return false;
    if (used == 1) { c->single = last; return true; }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = offs[l] + c->counts[l];
    cscdVecClear(&c->symbols);
    cscdVecResize(&c->symbols, (size_t)used);
    for (size_t s = 0; s < nlen; ++s) if (lengths[s]) c->symbols.items[offs[lengths[s]]++] = (uint16_t)s;
    return true;
}

static int codeRead(const VP8LCode* c, BitReader* br) {
    if (c->single >= 0) return c->single;
    refill(br); /* (the 15 bits a code can take are all in the buffer) */
    int code = 0, firstCode = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= (int)((br->buf >> (len - 1)) & 1);
        const int count = c->counts[len];
        if (code - count < firstCode) {
            consume(br, len);
            return br->overflow ? -1 : c->symbols.items[index + (code - firstCode)];
        }
        index += count;
        firstCode = (firstCode + count) << 1;
        code <<= 1;
    }
    consume(br, 15);
    return -1;
}

static bool readCode(BitReader* br, int alphabet, VP8LCode* out) {
    uint8_t* lengths = (uint8_t*)cscdCalloc((size_t)alphabet, 1);
    bool ok = false;
    if (bits(br, 1)) { /* simple: one or two symbols */
        const int count = (int)bits(br, 1) + 1;
        const int s0 = (int)bits(br, bits(br, 1) ? 8 : 1);
        if (s0 >= alphabet) goto done;
        lengths[s0] = 1;
        if (count == 2) {
            const int s1 = (int)bits(br, 8);
            if (s1 >= alphabet) goto done;
            lengths[s1] = 1;
        }
        ok = codeBuild(out, lengths, (size_t)alphabet);
        goto done;
    }
    {
        static const uint8_t order[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        uint8_t clen[19] = {0};
        const int numLengths = 4 + (int)bits(br, 4);
        for (int k = 0; k < numLengths && k < 19; ++k) clen[order[k]] = (uint8_t)bits(br, 3);
        VP8LCode lengthCode = {0};
        if (!codeBuild(&lengthCode, clen, 19)) { cscdVecFree(&lengthCode.symbols); goto done; }
        int maxSymbol = alphabet;
        if (bits(br, 1)) {
            const int nbits = 2 + 2 * (int)bits(br, 3);
            maxSymbol = 2 + (int)bits(br, nbits);
            if (maxSymbol > alphabet) { cscdVecFree(&lengthCode.symbols); goto done; }
        }
        int prev = 8;
        bool bad = false;
        for (int s = 0; s < alphabet;) {
            if (maxSymbol-- == 0) break;
            const int c = codeRead(&lengthCode, br);
            if (c < 0 || br->overflow) { bad = true; break; }
            if (c < 16) {
                lengths[s++] = (uint8_t)c;
                if (c) prev = c;
                continue;
            }
            const int repeat = c == 16 ? 3 + (int)bits(br, 2) : c == 17 ? 3 + (int)bits(br, 3) : 11 + (int)bits(br, 7);
            const uint8_t value = c == 16 ? (uint8_t)prev : 0;
            if (s + repeat > alphabet) { bad = true; break; }
            for (int r = 0; r < repeat; ++r) lengths[s++] = value;
        }
        cscdVecFree(&lengthCode.symbols);
        if (!bad) ok = codeBuild(out, lengths, (size_t)alphabet);
    }
done:
    cscdFree(lengths);
    return ok;
}

/* A prefix-coded value: LZ77 lengths and distances. */
static int copyValue(BitReader* br, int symbol) {
    if (symbol < 4) return symbol + 1;
    const int extra = (symbol - 2) >> 1;
    const int offset = (2 + (symbol & 1)) << extra;
    return offset + (int)bits(br, extra) + 1;
}

static int planeDistance(int xsize, int code) {
    static const int8_t plane[120][2] = {
        {0, 1}, {1, 0}, {1, 1}, {-1, 1}, {0, 2}, {2, 0}, {1, 2}, {-1, 2}, {2, 1}, {-2, 1}, {2, 2}, {-2, 2}, {0, 3}, {3, 0}, {1, 3}, {-1, 3},
        {3, 1}, {-3, 1}, {2, 3}, {-2, 3}, {3, 2}, {-3, 2}, {0, 4}, {4, 0}, {1, 4}, {-1, 4}, {4, 1}, {-4, 1}, {3, 3}, {-3, 3}, {2, 4}, {-2, 4},
        {4, 2}, {-4, 2}, {0, 5}, {3, 4}, {-3, 4}, {4, 3}, {-4, 3}, {5, 0}, {1, 5}, {-1, 5}, {5, 1}, {-5, 1}, {2, 5}, {-2, 5}, {5, 2}, {-5, 2},
        {4, 4}, {-4, 4}, {3, 5}, {-3, 5}, {5, 3}, {-5, 3}, {0, 6}, {6, 0}, {1, 6}, {-1, 6}, {6, 1}, {-6, 1}, {2, 6}, {-2, 6}, {6, 2}, {-6, 2},
        {4, 5}, {-4, 5}, {5, 4}, {-5, 4}, {3, 6}, {-3, 6}, {6, 3}, {-6, 3}, {0, 7}, {7, 0}, {1, 7}, {-1, 7}, {5, 5}, {-5, 5}, {7, 1}, {-7, 1},
        {4, 6}, {-4, 6}, {6, 4}, {-6, 4}, {2, 7}, {-2, 7}, {7, 2}, {-7, 2}, {3, 7}, {-3, 7}, {7, 3}, {-7, 3}, {5, 6}, {-5, 6}, {6, 5}, {-6, 5},
        {8, 0}, {4, 7}, {-4, 7}, {7, 4}, {-7, 4}, {8, 1}, {8, 2}, {6, 6}, {-6, 6}, {8, 3}, {5, 7}, {-5, 7}, {7, 5}, {-7, 5}, {8, 4}, {6, 7},
        {-6, 7}, {7, 6}, {-7, 6}, {8, 5}, {7, 7}, {-7, 7}, {8, 6}, {8, 7}};
    if (code > 120) return code - 120;
    const int dist = plane[code - 1][0] + plane[code - 1][1] * xsize;
    return dist >= 1 ? dist : 1;
}

/* An entropy-coded image of w x h ARGB pixels. Only the main image has meta prefix codes. */
static bool readImage(BitReader* br, int w, int h, bool main, CscdU32Vec* out) {
    int cacheBits = 0;
    if (bits(br, 1)) {
        cacheBits = (int)bits(br, 4);
        if (cacheBits < 1 || cacheBits > 11) return false;
    }
    int prefixBits = 0, metaW = 1;
    CscdU32Vec meta = {0};
    int groups = 1;
    if (main && bits(br, 1)) {
        prefixBits = (int)bits(br, 3) + 2;
        metaW = (w + (1 << prefixBits) - 1) >> prefixBits;
        const int metaH = (h + (1 << prefixBits) - 1) >> prefixBits;
        if (!readImage(br, metaW, metaH, false, &meta)) { cscdVecFree(&meta); return false; }
        for (size_t k = 0; k < meta.len; ++k) {
            meta.items[k] = (meta.items[k] >> 8) & 0xFFFF;
            groups = cscdMaxi(groups, (int)meta.items[k] + 1);
        }
    }
    const int cacheSize = cacheBits ? 1 << cacheBits : 0;
    VP8LCode* codes = (VP8LCode*)cscdCalloc((size_t)groups * 5, sizeof(VP8LCode));
    uint32_t* cache = (uint32_t*)cscdCalloc((size_t)(cacheSize ? cacheSize : 1), 4);
    bool ok = false;
    for (int g = 0; g < groups; ++g) {
        const int alphabets[5] = {256 + 24 + cacheSize, 256, 256, 256, 40};
        for (int k = 0; k < 5; ++k) if (!readCode(br, alphabets[k], &codes[(size_t)g * 5 + k])) goto done;
    }
#define REMEMBER(argb) do { if (cacheSize) cache[(0x1E35A7BDu * (argb)) >> (32 - cacheBits)] = (argb); } while (0)
    cscdVecClear(out);
    cscdVecResize(out, (size_t)w * h);
    const size_t total = out->len;
    uint32_t* o = out->items;
    for (size_t pos = 0; pos < total;) {
        const int x = (int)(pos % w), y = (int)(pos / w);
        const int g = meta.len == 0 ? 0 : (int)meta.items[(size_t)(y >> prefixBits) * metaW + (x >> prefixBits)];
        const VP8LCode* c = &codes[(size_t)g * 5];
        const int green = codeRead(&c[0], br);
        if (green < 0 || br->overflow) goto done;
        if (green < 256) {
            const int red = codeRead(&c[1], br), blue = codeRead(&c[2], br), alpha = codeRead(&c[3], br);
            if (red < 0 || blue < 0 || alpha < 0) goto done;
            const uint32_t argb = ((uint32_t)alpha << 24) | ((uint32_t)red << 16) | ((uint32_t)green << 8) | (uint32_t)blue;
            o[pos++] = argb;
            REMEMBER(argb);
        } else if (green < 256 + 24) {
            const int length = copyValue(br, green - 256);
            const int ds = codeRead(&c[4], br);
            if (ds < 0) goto done;
            const int dist = planeDistance(w, copyValue(br, ds));
            if ((size_t)dist > pos || pos + length > total) goto done;
            for (int k = 0; k < length; ++k, ++pos) { o[pos] = o[pos - dist]; REMEMBER(o[pos]); }
        } else {
            const int index = green - 256 - 24;
            if (index >= cacheSize) goto done;
            o[pos++] = cache[index];
            REMEMBER(cache[index]);
        }
    }
#undef REMEMBER
    ok = true;
done:
    for (int k = 0; k < groups * 5; ++k) cscdVecFree(&codes[k].symbols);
    cscdFree(codes);
    cscdFree(cache);
    cscdVecFree(&meta);
    return ok;
}

static uint32_t addPx(uint32_t a, uint32_t b) { /* per channel, modulo 256 */
    return (((a & 0xFF00FF00u) + (b & 0xFF00FF00u)) & 0xFF00FF00u) | (((a & 0x00FF00FFu) + (b & 0x00FF00FFu)) & 0x00FF00FFu);
}
static int ch(uint32_t p, int s) { return (int)((p >> s) & 255); }
static uint32_t avgPx(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) r |= (uint32_t)((ch(a, s) + ch(b, s)) / 2) << s;
    return r;
}
static uint32_t clamp255(int v) { return (uint32_t)cscdClampi(v, 0, 255); }
static int colorDelta(uint32_t t8, uint32_t c8) { return ((int)(int8_t)t8 * (int)(int8_t)c8) >> 5; }

typedef struct Transform { int type, bits, w; CscdU32Vec data; } Transform;

/* An image stream (its transforms, then the entropy-coded pixels) of width x height ARGB pixels: a
 * VP8L image after its header, and the alpha of a lossy WebP (in the green channel). */
static bool vp8lStream(BitReader* br, int width, int height, CscdU32Vec* out) {
    Transform transforms[4];
    int ntrans = 0;
    CscdU32Vec px = {0};
    bool ok = false;
    int xsize = width;
    while (bits(br, 1)) {
        Transform t = {0};
        t.type = (int)bits(br, 2);
        t.w = xsize;
        bool dup = false;
        for (int k = 0; k < ntrans; ++k) dup = dup || transforms[k].type == t.type;
        if (dup || ntrans == 4) goto done; /* (each at most once) */
        if (t.type == 0 || t.type == 1) { /* predictor, color: a sub-image of per-block parameters */
            t.bits = (int)bits(br, 3) + 2;
            if (!readImage(br, (xsize + (1 << t.bits) - 1) >> t.bits, (height + (1 << t.bits) - 1) >> t.bits, false, &t.data)) { cscdVecFree(&t.data); goto done; }
        } else if (t.type == 3) { /* color indexing: a palette, pixels packed by its size */
            const int size = (int)bits(br, 8) + 1;
            if (!readImage(br, size, 1, false, &t.data)) { cscdVecFree(&t.data); goto done; }
            for (int k = 1; k < size; ++k) t.data.items[k] = addPx(t.data.items[k - 1], t.data.items[k]); /* (delta-coded) */
            t.bits = size <= 2 ? 3 : size <= 4 ? 2 : size <= 16 ? 1 : 0;
            xsize = (xsize + (1 << t.bits) - 1) >> t.bits;
        }
        transforms[ntrans++] = t;
        if (br->overflow) goto done;
    }
    if (!readImage(br, xsize, height, true, &px)) goto done;
    for (int ti = ntrans - 1; ti >= 0; --ti) {
        Transform* t = &transforms[ti];
        const int w = t->w;
        if (t->type == 2) { /* subtract green */
            for (size_t k = 0; k < px.len; ++k) {
                uint32_t q = px.items[k];
                const uint32_t g = (q >> 8) & 255;
                px.items[k] = (q & 0xFF00FF00u) | ((((q >> 16) + g) & 255) << 16) | ((q + g) & 255);
            }
        } else if (t->type == 3) { /* color indexing */
            const int per = 1 << t->bits, bitsEach = 8 >> t->bits, mask = (1 << bitsEach) - 1;
            const int packedW = (w + per - 1) / per;
            CscdU32Vec outPx = {0};
            cscdVecResize(&outPx, (size_t)w * height);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < w; ++x) {
                    const uint32_t packed = (px.items[(size_t)y * packedW + x / per] >> 8) & 255;
                    const uint32_t idx = (packed >> ((x % per) * bitsEach)) & mask;
                    outPx.items[(size_t)y * w + x] = idx < t->data.len ? t->data.items[idx] : 0;
                }
            cscdVecFree(&px);
            px = outPx;
        } else if (t->type == 1) { /* color transform */
            const int bw = (w + (1 << t->bits) - 1) >> t->bits;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < w; ++x) {
                    const uint32_t m = t->data.items[(size_t)(y >> t->bits) * bw + (x >> t->bits)];
                    const uint32_t g2r = m & 255, g2b = (m >> 8) & 255, r2b = (m >> 16) & 255;
                    uint32_t* q = &px.items[(size_t)y * w + x];
                    const uint32_t g = (*q >> 8) & 255;
                    int r = (int)((*q >> 16) & 255), b = (int)(*q & 255);
                    r = (r + colorDelta(g2r, g)) & 255;
                    b = (b + colorDelta(g2b, g)) & 255;
                    b = (b + colorDelta(r2b, (uint32_t)r)) & 255;
                    *q = (*q & 0xFF00FF00u) | ((uint32_t)r << 16) | (uint32_t)b;
                }
        } else { /* predictor */
            const int bw = (w + (1 << t->bits) - 1) >> t->bits;
            uint32_t* P = px.items;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < w; ++x) {
                    const size_t pos = (size_t)y * w + x;
                    uint32_t pred;
                    if (y == 0) pred = x == 0 ? 0xFF000000u : P[pos - 1];
                    else if (x == 0) pred = P[pos - w];
                    else {
                        const uint32_t L = P[pos - 1], T = P[pos - w], TL = P[pos - w - 1], TR = P[pos - w + 1];
                        const int mode = (int)((t->data.items[(size_t)(y >> t->bits) * bw + (x >> t->bits)] >> 8) & 15);
                        switch (mode) {
                        case 0: pred = 0xFF000000u; break;
                        case 1: pred = L; break;
                        case 2: pred = T; break;
                        case 3: pred = TR; break;
                        case 4: pred = TL; break;
                        case 5: pred = avgPx(avgPx(L, TR), T); break;
                        case 6: pred = avgPx(L, TL); break;
                        case 7: pred = avgPx(L, T); break;
                        case 8: pred = avgPx(TL, T); break;
                        case 9: pred = avgPx(T, TR); break;
                        case 10: pred = avgPx(avgPx(L, TL), avgPx(T, TR)); break;
                        case 11: { /* select: the one of L and T nearer L + T - TL */
                            int pL = 0, pT = 0;
                            for (int s = 0; s < 32; s += 8) {
                                const int e = ch(L, s) + ch(T, s) - ch(TL, s);
                                pL += abs(e - ch(L, s));
                                pT += abs(e - ch(T, s));
                            }
                            pred = pL < pT ? L : T;
                            break;
                        }
                        case 12: {
                            pred = 0;
                            for (int s = 0; s < 32; s += 8) pred |= clamp255(ch(L, s) + ch(T, s) - ch(TL, s)) << s;
                            break;
                        }
                        case 13: {
                            const uint32_t a = avgPx(L, T);
                            pred = 0;
                            for (int s = 0; s < 32; s += 8) pred |= clamp255(ch(a, s) + (ch(a, s) - ch(TL, s)) / 2) << s;
                            break;
                        }
                        default: pred = 0xFF000000u; break;
                        }
                    }
                    P[pos] = addPx(P[pos], pred);
                }
        }
    }
    ok = !br->overflow && px.len == (size_t)width * height;
    if (ok) {
        *out = px;
        px = (CscdU32Vec){0};
    }
done:
    for (int k = 0; k < ntrans; ++k) cscdVecFree(&transforms[k].data);
    cscdVecFree(&px);
    return ok;
}

static CscdImage* vp8lRun(const uint8_t* p, size_t n) {
    BitReader brs = {p, n, 0, 0, 0, 0, false};
    BitReader* br = &brs;
    if (bits(br, 8) != 0x2F) return NULL;
    const int width = (int)bits(br, 14) + 1, height = (int)bits(br, 14) + 1;
    bits(br, 1); /* (alpha is used: the pixels say) */
    if (bits(br, 3) != 0) return NULL;
    if ((uint64_t)width * height > 100000000) return NULL;
    CscdU32Vec px = {0};
    if (!vp8lStream(br, width, height, &px)) return NULL;
    CscdImage* img = cscdImageNew(width, height);
    for (size_t k = 0; k < cscdImagePixelCount(img); ++k) {
        const uint32_t q = px.items[k];
        img->pixels[k] = packPremul((q >> 16) & 255, (q >> 8) & 255, q & 255, q >> 24);
    }
    cscdVecFree(&px);
    return img;
}

/* ---- WebP: lossy frames, alpha, the extended format and animation ----------------------------
 * A "VP8 " chunk is a VP8 key frame (imageVp8.c); its YUV turns into RGB as libwebp turns it (the
 * "fancy" upsampling of the chroma, the same fixed-point arithmetic), so a lossy WebP shows the
 * pixels a browser shows. ALPH holds its alpha (raw or VP8L-coded, with its prediction filter).
 * VP8X, ANIM and ANMF hold an animation: frames placed on the canvas, blended or not, disposed
 * to transparent or not. */

static uint32_t le24(const uint8_t* p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }

static int yuvMultHi(int v, int coeff) { return (v * coeff) >> 8; }
static int yuvClip(int v) { return (v & ~16383) == 0 ? v >> 6 : v < 0 ? 0 : 255; }

/* An ALPH chunk's alpha plane (w x h). */
static bool decodeAlpha(const uint8_t* p, size_t n, int w, int h, uint8_t* alpha) {
    if (n < 1) return false;
    const int compression = p[0] & 3, filter = (p[0] >> 2) & 3;
    const size_t count = (size_t)w * h;
    if (compression == 0) {
        if (n - 1 < count) return false;
        memcpy(alpha, p + 1, count);
    } else if (compression == 1) {
        BitReader br = {p + 1, n - 1, 0, 0, 0, 0, false};
        CscdU32Vec px = {0};
        if (!vp8lStream(&br, w, h, &px)) return false;
        for (size_t k = 0; k < count; ++k) alpha[k] = (uint8_t)(px.items[k] >> 8);
        cscdVecFree(&px);
    } else return false;
    if (filter == 0) return true;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* a = alpha + (size_t)y * w + x;
            int pred;
            if (x == 0 && y == 0) pred = 0;
            else if (y == 0) pred = a[-1];
            else if (x == 0) pred = a[-w];
            else if (filter == 1) pred = a[-1];
            else if (filter == 2) pred = a[-w];
            else pred = cscdClampi(a[-1] + a[-w] - a[-w - 1], 0, 255);
            *a = (uint8_t)(*a + pred);
        }
    return true;
}

static CscdImage* vp8Image(const uint8_t* p, size_t n, const uint8_t* alph, size_t alphN) {
    CscdYuv yuv;
    if (!cscdDecodeVP8(p, n, &yuv)) return NULL;
    const int w = yuv.width, h = yuv.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    uint8_t* alpha = NULL;
    if (alph) {
        alpha = (uint8_t*)cscdMalloc((size_t)w * h);
        if (!decodeAlpha(alph, alphN, w, h, alpha)) memset(alpha, 255, (size_t)w * h);
    }
    CscdImage* img = cscdImageNew(w, h);
    for (int y = 0; y < h; ++y) {
        /* the chroma row nearest this row and the one beyond it, weighted 3 to 1 (and so across) */
        const int nr = y >> 1, fr = cscdClampi((y & 1) ? nr + 1 : nr - 1, 0, ch - 1);
        const uint8_t *un = yuv.u + (size_t)nr * yuv.uvStride, *uf = yuv.u + (size_t)fr * yuv.uvStride;
        const uint8_t *vn = yuv.v + (size_t)nr * yuv.uvStride, *vf = yuv.v + (size_t)fr * yuv.uvStride;
        const uint8_t* yr = yuv.y + (size_t)y * yuv.yStride;
        uint32_t* out = cscdImageRow(img, y);
        for (int x = 0; x < w; ++x) {
            const int nc = x >> 1, fc = cscdClampi((x & 1) ? nc + 1 : nc - 1, 0, cw - 1);
            const int u = (9 * un[nc] + 3 * un[fc] + 3 * uf[nc] + uf[fc] + 8) >> 4;
            const int v = (9 * vn[nc] + 3 * vn[fc] + 3 * vf[nc] + vf[fc] + 8) >> 4;
            const int yy = yuvMultHi(yr[x], 19077);
            const int r = yuvClip(yy + yuvMultHi(v, 26149) - 14234);
            const int g = yuvClip(yy - yuvMultHi(u, 6419) - yuvMultHi(v, 13320) + 8708);
            const int b = yuvClip(yy + yuvMultHi(u, 33050) - 17685);
            out[x] = packPremul((uint32_t)r, (uint32_t)g, (uint32_t)b, alpha ? alpha[(size_t)y * w + x] : 255);
        }
    }
    cscdFree(alpha);
    cscdYuvFree(&yuv);
    return img;
}

/* What a VP8 or VP8L chunk says its picture measures; false when it is too short to say. */
static bool webpChunkSize(const uint8_t* body, size_t size, bool lossless, int* w, int* h) {
    if (lossless) {
        if (size < 5) return false;
        const uint32_t bits = le32(body + 1);
        *w = (int)(bits & 0x3FFF) + 1;
        *h = (int)((bits >> 14) & 0x3FFF) + 1;
        return true;
    }
    if (size < 10) return false;
    *w = (body[6] | body[7] << 8) & 0x3FFF;
    *h = (body[8] | body[9] << 8) & 0x3FFF;
    return true;
}

/* A picture from a list of chunks: VP8L, or VP8 with an ALPH before it. maxW, maxH: what it may
 * measure, so that a few bytes that claim a huge frame are not decoded to be cut (0: anything). */
static CscdImage* webpPicture(const uint8_t* d, size_t n, int maxW, int maxH) {
    const uint8_t* alph = NULL;
    size_t alphN = 0;
    for (size_t i = 0; i + 8 <= n;) {
        const uint32_t size = le32(d + i + 4);
        if (size > n - i - 8) return NULL;
        const uint8_t* body = d + i + 8;
        const bool lossless = memcmp(d + i, "VP8L", 4) == 0;
        if (lossless || memcmp(d + i, "VP8 ", 4) == 0) {
            int w, h;
            if (maxW && webpChunkSize(body, size, lossless, &w, &h) && (w > maxW || h > maxH)) return NULL;
            return lossless ? vp8lRun(body, size) : vp8Image(body, size, alph, alphN);
        }
        if (memcmp(d + i, "ALPH", 4) == 0) { alph = body; alphN = size; }
        i += 8 + (size_t)size + (size & 1);
    }
    return NULL;
}

static CscdAnimatedImage* decodeWebP(const uint8_t* d, size_t n, int maxFrames) {
    if (n < 20 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WEBP", 4) != 0) return NULL;
    const size_t riff = (size_t)le32(d + 4) + 8;
    if (riff < n) n = riff;
    if (n < 12) return NULL; /* (a RIFF that says it holds less than its own header) */
    d += 12;
    n -= 12;
    int cw = 0, chh = 0;
    bool animated = false;
    if (n >= 18 && memcmp(d, "VP8X", 4) == 0) {
        animated = (d[8] & 0x02) != 0;
        cw = (int)le24(d + 12) + 1;
        chh = (int)le24(d + 15) + 1;
    }
    if (!animated) {
        CscdImage* img = webpPicture(d, n, 0, 0);
        if (!img) return NULL;
        CscdAnimatedImage* a = animationNew(img->width, img->height);
        animationPush(a, img, 0);
        a->durations[0] = 0;
        cscdRelease(img);
        return a;
    }
    if ((uint64_t)cw * chh > 100000000) return NULL;
    CscdAnimatedImage* anim = animationNew(cw, chh);
    CscdImage* canvas = cscdImageNew(cw, chh);
    bool dispose = false;
    int px = 0, py = 0, pw = 0, ph = 0;
    for (size_t i = 0; i + 8 <= n;) {
        const uint32_t size = le32(d + i + 4);
        if (size > n - i - 8) break;
        const uint8_t* body = d + i + 8;
        if (memcmp(d + i, "ANIM", 4) == 0 && size >= 6) anim->loops = body[4] | (body[5] << 8);
        if (memcmp(d + i, "ANMF", 4) == 0 && size >= 16) {
            const int fx = (int)le24(body) * 2, fy = (int)le24(body + 3) * 2;
            const int dur = (int)le24(body + 12);
            const bool blend = !(body[15] & 2), disposeThis = body[15] & 1;
            if (fx >= cw || fy >= chh) break; /* (frames lie inside the canvas) */
            CscdImage* frame = webpPicture(body + 16, size - 16, cw - fx, chh - fy);
            if (!frame) break;
            if (dispose) clearRect(canvas, px, py, pw, ph);
            for (int y = 0; y < frame->height; ++y) {
                if (fy + y >= chh) break;
                const uint32_t* s = cscdImageRow(frame, y);
                uint32_t* o = cscdImageRow(canvas, fy + y) + fx;
                for (int x = 0; x < frame->width && fx + x < cw; ++x) {
                    const uint32_t src = s[x], a = src >> 24;
                    if (!blend || a == 255) { o[x] = src; continue; }
                    if (!a) continue;
                    const uint32_t dst = o[x];
                    uint32_t out = 0;
                    for (int c = 0; c < 32; c += 8)
                        out |= (((src >> c) & 255) + (((dst >> c) & 255) * (255 - a) + 127) / 255) << c;
                    o[x] = out;
                }
            }
            dispose = disposeThis;
            px = fx; py = fy; pw = frame->width; ph = frame->height;
            cscdRelease(frame);
            if (!animationPush(anim, canvas, dur) || anim->frameCount >= maxFrames) break;
        }
        i += 8 + (size_t)size + (size & 1);
    }
    cscdRelease(canvas);
    if (!anim->frameCount) { cscdRelease(anim); return NULL; }
    return anim;
}

static CscdAnimatedImage* decodeAnimation(const uint8_t* data, size_t size, int maxFrames) {
    if (size >= 6 && memcmp(data, "GIF8", 4) == 0) return decodeGIF(data, size, maxFrames);
    if (size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) return decodeWebP(data, size, maxFrames);
    CscdImage* img = cscdDecodeImage(data, size);
    if (!img) return NULL;
    CscdAnimatedImage* a = animationNew(img->width, img->height);
    animationPush(a, img, 0);
    a->durations[0] = 0;
    cscdRelease(img);
    return a;
}

CscdAnimatedImage* cscdDecodeAnimatedImage(const uint8_t* data, size_t size) { return decodeAnimation(data, size, 1 << 20); }

CscdAnimatedImage* cscdLoadAnimatedImage(CscdSv path) {
    CscdStr s = {0};
    if (!cscdReadFile(path, &s)) return NULL;
    CscdAnimatedImage* a = cscdDecodeAnimatedImage((const uint8_t*)s.ptr, s.len);
    cscdStrFree(&s);
    return a;
}

CscdImage* cscdDecodeImage(const uint8_t* data, size_t size) {
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P') return decodePNG(data, size);
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8) return decodeJPEG(data, size);
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return decodeBMP(data, size);
    if (size >= 6 && memcmp(data, "GIF8", 4) == 0) return firstFrame(decodeGIF(data, size, 1));
    if (size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) return firstFrame(decodeWebP(data, size, 1));
    return NULL;
}

CscdImage* cscdLoadImage(CscdSv path) {
    CscdStr s = {0};
    if (!cscdReadFile(path, &s)) return NULL;
    CscdImage* img = cscdDecodeImage((const uint8_t*)s.ptr, s.len);
    cscdStrFree(&s);
    return img;
}

static void put32(CscdBytes* v, uint32_t x) {
    cscdVecPush(v, (uint8_t)(x >> 24));
    cscdVecPush(v, (uint8_t)((x >> 16) & 255));
    cscdVecPush(v, (uint8_t)((x >> 8) & 255));
    cscdVecPush(v, (uint8_t)(x & 255));
}

static void chunk(CscdBytes* out, const char* type, const uint8_t* body, size_t len) {
    put32(out, (uint32_t)len);
    size_t start = out->len;
    cscdVecAppend(out, (const uint8_t*)type, 4);
    cscdVecAppend(out, body, len);
    put32(out, crc32(&out->items[start], out->len - start, 0xFFFFFFFFu) ^ 0xFFFFFFFFu);
}

void cscdEncodePNG(const CscdImage* img, CscdBytes* out) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    cscdVecAppend(out, sig, 8);
    CscdBytes ihdr = {0};
    put32(&ihdr, (uint32_t)img->width);
    put32(&ihdr, (uint32_t)img->height);
    static const uint8_t rest[5] = {8, 6, 0, 0, 0};
    cscdVecAppend(&ihdr, rest, 5);
    chunk(out, "IHDR", ihdr.items, ihdr.len);
    cscdVecFree(&ihdr);
    /* raw scanlines (filter 0), RGBA unpremultiplied */
    CscdBytes raw = {0};
    cscdVecReserve(&raw, (size_t)img->height * (img->width * 4 + 1));
    for (int y = 0; y < img->height; ++y) {
        cscdVecPush(&raw, 0);
        const uint32_t* r = cscdImageRow(img, y);
        for (int x = 0; x < img->width; ++x) {
            uint32_t p = r[x];
            uint32_t a = p >> 24, b = p & 255, g = (p >> 8) & 255, rr = (p >> 16) & 255;
            if (a > 0 && a < 255) {
                rr = rr * 255 / a; if (rr > 255) rr = 255;
                g = g * 255 / a; if (g > 255) g = 255;
                b = b * 255 / a; if (b > 255) b = 255;
            }
            cscdVecPush(&raw, (uint8_t)rr);
            cscdVecPush(&raw, (uint8_t)g);
            cscdVecPush(&raw, (uint8_t)b);
            cscdVecPush(&raw, (uint8_t)a);
        }
    }
    /* zlib with stored blocks */
    CscdBytes z = {0};
    cscdVecPush(&z, 0x78);
    cscdVecPush(&z, 0x01);
    size_t pos = 0;
    do {
        size_t len = cscdMinz(65535, raw.len - pos);
        bool final = pos + len >= raw.len;
        cscdVecPush(&z, final ? 1 : 0);
        cscdVecPush(&z, (uint8_t)(len & 255));
        cscdVecPush(&z, (uint8_t)(len >> 8));
        cscdVecPush(&z, (uint8_t)(~len & 255));
        cscdVecPush(&z, (uint8_t)((~len >> 8) & 255));
        cscdVecAppend(&z, raw.items + pos, len);
        pos += len;
    } while (pos < raw.len);
    uint32_t a = 1, b = 0;
    for (size_t k = 0; k < raw.len; ++k) { a = (a + raw.items[k]) % 65521; b = (b + a) % 65521; }
    put32(&z, (b << 16) | a);
    chunk(out, "IDAT", z.items, z.len);
    chunk(out, "IEND", NULL, 0);
    cscdVecFree(&z);
    cscdVecFree(&raw);
}

bool cscdSavePNG(CscdSv path, const CscdImage* img) {
    CscdBytes data = {0};
    cscdEncodePNG(img, &data);
    FILE* f = cscdFileOpen(path, "wb");
    bool ok = false;
    if (f) {
        ok = fwrite(data.items, 1, data.len, f) == data.len;
        fclose(f);
    }
    cscdVecFree(&data);
    return ok;
}
