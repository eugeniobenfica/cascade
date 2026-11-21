/* A Brotli decoder (RFC 7932), for WOFF2: meta-blocks of commands (literals inserted, then a copy from the text before
 * or a word of the static dictionary, transformed), each part read with canonical prefix codes chosen by block type and
 * context. Whole streams only (the output in memory); anything the format calls invalid makes it fail. */
#include "text/fontInternal.h"

#include "cascade/image.h"
#include "core/thread.h"

#include <string.h>

typedef struct Transform {
    const char* prefix;
    uint8_t prefixLen, type;
    const char* suffix;
    uint8_t suffixLen;
} Transform;

#include "text/brotliData.inc"

/* ---- the static dictionary --------------------------------------------------------------------------------------- */

static const uint8_t kNdBits[25] = {0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10, 9, 9, 8, 7, 7, 8, 7, 7, 6, 6, 5, 5};
static uint32_t dictOffset[25];
static CscdBytes dictionary;
static CscdOnce dictionaryOnce;

static void loadDictionary(void) {
    if (!cscdInflateZlib(kDictionaryZ, sizeof kDictionaryZ, &dictionary) || dictionary.len != 122784) cscdVecFree(&dictionary);
    uint32_t at = 0;
    for (int l = 0; l < 24; ++l) {
        dictOffset[l] = at;
        if (l >= 4) at += (uint32_t)l << kNdBits[l];
    }
    dictOffset[24] = at;
}

/* ---- bits ---------------------------------------------------------------------------------------------------------- */

typedef struct Bits {
    const uint8_t* p;
    size_t n, pos;
    uint64_t acc;
    int count;
    bool bad;
} Bits;

static uint32_t readBits(Bits* b, int k) {
    if (!k) return 0;
    while (b->count < k) {
        if (b->pos < b->n) b->acc |= (uint64_t)b->p[b->pos] << b->count;
        else b->bad = true; /* (past the end: zeros, and the stream is broken) */
        ++b->pos;
        b->count += 8;
    }
    const uint32_t v = (uint32_t)(b->acc & ((1u << k) - 1));
    b->acc >>= k;
    b->count -= k;
    return v;
}

/* to the next byte: the bits skipped must be zeros */
static void alignBits(Bits* b) {
    const int skip = b->count & 7;
    if (readBits(b, skip)) b->bad = true;
}

/* ---- prefix codes ---------------------------------------------------------------------------------------------- */

#define MAX_LEN 15

typedef struct Huff {
    uint16_t count[MAX_LEN + 1]; /* (codes of each length) */
    uint16_t* symbols;           /* (by length, then by value) */
    int single;                  /* the one symbol of a code with one, read with no bits; -1: none */
} Huff;

static void huffFree(Huff* h) {
    cscdFree(h->symbols);
    h->symbols = NULL;
}

/* A canonical code from its symbols' lengths (0: not in it). */
static bool huffBuild(Huff* h, const uint8_t* lengths, int n) {
    memset(h->count, 0, sizeof h->count);
    h->single = -1;
    int used = 0, last = 0;
    for (int i = 0; i < n; ++i)
        if (lengths[i]) {
            ++h->count[lengths[i]];
            ++used;
            last = i;
        }
    cscdFree(h->symbols);
    h->symbols = (uint16_t*)cscdMalloc(sizeof(uint16_t) * (size_t)(used ? used : 1));
    if (used == 1) {
        h->single = last;
        return true;
    }
    uint16_t offs[MAX_LEN + 2];
    offs[1] = 0;
    for (int l = 1; l <= MAX_LEN; ++l) offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
    for (int i = 0; i < n; ++i)
        if (lengths[i]) h->symbols[offs[lengths[i]]++] = (uint16_t)i;
    return used > 1;
}

static int huffDecode(Bits* b, const Huff* h) {
    if (h->single >= 0) return h->single;
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= MAX_LEN; ++l) {
        code |= (int)readBits(b, 1);
        const int count = h->count[l];
        if (code - first < count) return h->symbols[index + code - first];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    b->bad = true;
    return 0;
}

static int bitsFor(int alphabet) {
    int k = 0;
    while ((1 << k) < alphabet) ++k;
    return k;
}

/* the code of the code lengths' code lengths: 0 "00", 1 "0111", 2 "011", 3 "10", 4 "01", 5 "1111" (read right to left) */
static int readCodeLengthLength(Bits* b) {
    if (!readBits(b, 1)) return readBits(b, 1) ? 3 : 0;
    if (!readBits(b, 1)) return 4;
    if (!readBits(b, 1)) return 2;
    return readBits(b, 1) ? 5 : 1;
}

/* One prefix code over an alphabet of the size given (Section 3.4, 3.5). */
static bool readPrefixCode(Bits* b, int alphabet, Huff* h) {
    uint8_t* lengths = (uint8_t*)cscdCalloc((size_t)alphabet, 1);
    bool ok = false;
    const uint32_t hskip = readBits(b, 2);
    if (hskip == 1) { /* simple: up to four symbols */
        const int nsym = (int)readBits(b, 2) + 1, width = bitsFor(alphabet);
        int syms[4];
        for (int i = 0; i < nsym; ++i) {
            syms[i] = (int)readBits(b, width);
            if (syms[i] >= alphabet) goto done;
            for (int k = 0; k < i; ++k)
                if (syms[k] == syms[i]) goto done;
        }
        static const uint8_t kLens[4][4] = {{0}, {1, 1}, {1, 2, 2}, {2, 2, 2, 2}};
        const bool tree = nsym == 4 && readBits(b, 1);
        for (int i = 0; i < nsym; ++i) lengths[syms[i]] = tree ? (uint8_t)(i < 2 ? i + 1 : 3) : kLens[nsym - 1][i];
        if (nsym == 1) lengths[syms[0]] = 1; /* (one symbol: read with no bits) */
        ok = huffBuild(h, lengths, alphabet);
        if (nsym == 1) h->single = syms[0];
        goto done;
    }
    /* complex: the code lengths, themselves prefix coded */
    static const uint8_t kOrder[18] = {1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t clLens[18] = {0};
    int space = 32, nonzero = 0;
    for (int i = (int)hskip; i < 18 && space > 0; ++i) {
        const int v = readCodeLengthLength(b);
        clLens[kOrder[i]] = (uint8_t)v;
        if (v) {
            space -= 32 >> v;
            ++nonzero;
        }
    }
    if (b->bad || (nonzero != 1 && space != 0)) goto done;
    Huff cl = {{0}, NULL, -1};
    if (!huffBuild(&cl, clLens, 18) && cl.single < 0) {
        huffFree(&cl);
        goto done;
    }
    int prev = 8, repeat = 0, repeatLen = 0, sym = 0;
    space = 32768;
    while (sym < alphabet && space > 0 && !b->bad) {
        const int c = huffDecode(b, &cl);
        if (c < 16) {
            repeat = 0;
            lengths[sym++] = (uint8_t)c;
            if (c) {
                prev = c;
                space -= 32768 >> c;
            }
            continue;
        }
        const int extra = c == 16 ? 2 : 3, len = c == 16 ? prev : 0;
        if (len != repeatLen) {
            repeat = 0;
            repeatLen = len;
        }
        const int old = repeat;
        if (repeat > 0) repeat = (repeat - 2) << extra;
        repeat += (int)readBits(b, extra) + 3;
        const int delta = repeat - old;
        if (sym + delta > alphabet) {
            huffFree(&cl);
            goto done;
        }
        for (int k = 0; k < delta; ++k) lengths[sym++] = (uint8_t)repeatLen;
        if (repeatLen) space -= delta * (32768 >> repeatLen);
    }
    huffFree(&cl);
    if (space != 0 || b->bad) goto done;
    ok = huffBuild(h, lengths, alphabet);
done:
    cscdFree(lengths);
    return ok && !b->bad;
}

/* ---- the header's numbers ---------------------------------------------------------------------------------------- */

/* 1..256: NBLTYPES, NTREES */
static int readCount256(Bits* b) {
    if (!readBits(b, 1)) return 1;
    const int n = (int)readBits(b, 3);
    if (!n) return 2;
    return (1 << n) + 1 + (int)readBits(b, n);
}

static const uint16_t kBlockBase[26] = {1, 5, 9, 13, 17, 25, 33, 41, 49, 65, 81, 97, 113, 145, 177, 209, 241, 305, 369, 497, 753, 1265,
                                        2289, 4337, 8433, 16625};
static const uint8_t kBlockExtra[26] = {2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13, 24};
static const uint32_t kInsertBase[24] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 14, 18, 26, 34, 50, 66, 98, 130, 194, 322, 578, 1090, 2114, 6210, 22594};
static const uint8_t kInsertExtra[24] = {0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 12, 14, 24};
static const uint32_t kCopyBase[24] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 18, 22, 30, 38, 54, 70, 102, 134, 198, 326, 582, 1094, 2118};
static const uint8_t kCopyExtra[24] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 24};

/* a block category: its types, the codes of type and count, the current type, the previous one, what is left */
typedef struct Blocks {
    int types, type, prev;
    uint32_t left;
    Huff typeCode, countCode;
} Blocks;

static uint32_t readBlockCount(Bits* b, const Huff* h) {
    const int c = huffDecode(b, h);
    return kBlockBase[c] + readBits(b, kBlockExtra[c]);
}

static bool readBlocks(Bits* b, Blocks* k) {
    k->types = readCount256(b);
    k->type = 0;
    k->prev = 1;
    k->left = 1u << 24;
    if (k->types < 2) return true;
    if (!readPrefixCode(b, k->types + 2, &k->typeCode) || !readPrefixCode(b, 26, &k->countCode)) return false;
    k->left = readBlockCount(b, &k->countCode);
    return !b->bad;
}

static bool switchBlock(Bits* b, Blocks* k) {
    const int c = huffDecode(b, &k->typeCode);
    const int t = c == 0 ? k->prev : c == 1 ? (k->type + 1) % k->types : c - 2;
    if (t >= k->types) return false;
    k->prev = k->type;
    k->type = t;
    k->left = readBlockCount(b, &k->countCode);
    return !b->bad;
}

static void blocksFree(Blocks* k) {
    huffFree(&k->typeCode);
    huffFree(&k->countCode);
}

static bool readContextMap(Bits* b, int size, int trees, uint8_t* map) {
    if (trees < 2) {
        memset(map, 0, (size_t)size);
        return true;
    }
    const int rleMax = readBits(b, 1) ? (int)readBits(b, 4) + 1 : 0;
    Huff h = {{0}, NULL, -1};
    bool ok = readPrefixCode(b, trees + rleMax, &h);
    for (int i = 0; ok && i < size && !b->bad;) {
        const int c = huffDecode(b, &h);
        if (c == 0) map[i++] = 0;
        else if (c <= rleMax) {
            const int reps = (1 << c) + (int)readBits(b, c);
            if (i + reps > size) ok = false;
            else {
                memset(map + i, 0, (size_t)reps);
                i += reps;
            }
        } else map[i++] = (uint8_t)(c - rleMax);
    }
    huffFree(&h);
    if (ok && readBits(b, 1)) { /* (the values move-to-front coded) */
        uint8_t mtf[256];
        for (int i = 0; i < 256; ++i) mtf[i] = (uint8_t)i;
        for (int i = 0; i < size; ++i) {
            int index = map[i];
            const uint8_t value = mtf[index];
            map[i] = value;
            for (; index; --index) mtf[index] = mtf[index - 1];
            mtf[0] = value;
        }
    }
    for (int i = 0; ok && i < size; ++i) ok = map[i] < trees;
    return ok && !b->bad;
}

/* ---- the dictionary's words, transformed ------------------------------------------------------------------------- */

static int ferment(uint8_t* w, int len, int pos) {
    if (w[pos] < 192) {
        if (w[pos] >= 97 && w[pos] <= 122) w[pos] = (uint8_t)(w[pos] ^ 32);
        return 1;
    }
    if (w[pos] < 224) {
        if (pos + 1 < len) w[pos + 1] = (uint8_t)(w[pos + 1] ^ 32);
        return 2;
    }
    if (pos + 2 < len) w[pos + 2] = (uint8_t)(w[pos + 2] ^ 5);
    return 3;
}

/* the word of the dictionary for its length and id, transformed into out (38 bytes): its length, -1 when invalid */
static int dictionaryWord(int len, uint32_t id, uint8_t* out) {
    if (len < 4 || len > 24 || !dictionary.len) return -1;
    const uint32_t index = id & ((1u << kNdBits[len]) - 1), t = id >> kNdBits[len];
    if (t > 120) return -1;
    const uint8_t* word = dictionary.items + dictOffset[len] + index * (uint32_t)len;
    const Transform* tr = &kTransforms[t];
    int n = 0;
    memcpy(out, tr->prefix, tr->prefixLen);
    n += tr->prefixLen;
    int from = 0, count = len;
    if (tr->type >= 3 && tr->type <= 11) { /* (omit the first k) */
        from = tr->type - 2;
        count = len - from;
    } else if (tr->type >= 12) { /* (omit the last k) */
        count = len - (tr->type - 11);
    }
    if (count < 0) count = 0;
    memcpy(out + n, word + from, (size_t)count);
    if (tr->type == 1 && count) ferment(out + n, count, 0);
    else if (tr->type == 2)
        for (int i = 0; i < count;) i += ferment(out + n, count, i);
    n += count;
    memcpy(out + n, tr->suffix, tr->suffixLen);
    return n + tr->suffixLen;
}

/* ---- the stream ---------------------------------------------------------------------------------------------------- */

bool cscdBrotliDecode(const uint8_t* data, size_t size, CscdBytes* out, size_t maxSize) {
    Bits b = {data, size, 0, 0, 0, false};
    const size_t start = out->len;
    /* the window: WBITS */
    int wbits = 16;
    if (readBits(&b, 1)) {
        const int n = (int)readBits(&b, 3);
        if (n) wbits = 17 + n;
        else {
            const int m = (int)readBits(&b, 3);
            if (m == 1) return false;
            wbits = m ? 8 + m : 17;
        }
    }
    const size_t window = ((size_t)1 << wbits) - 16;
    int dist[4] = {4, 11, 15, 16}; /* (the last distance first) */
    bool ok = true, last = false;
    Blocks blocks[3] = {{0}};
    for (int i = 0; i < 3; ++i) blocks[i].typeCode.single = blocks[i].countCode.single = -1;
    uint8_t* cmodes = NULL;
    uint8_t *cmapL = NULL, *cmapD = NULL;
    Huff *lits = NULL, *cmds = NULL, *dists = NULL;
    int nLits = 0, nCmds = 0, nDists = 0;
    while (ok && !last) {
        last = readBits(&b, 1);
        if (last && readBits(&b, 1)) break; /* (ISLASTEMPTY) */
        const int nibbles = (int)readBits(&b, 2);
        if (nibbles == 3) { /* metadata, skipped */
            if (readBits(&b, 1)) { ok = false; break; }
            const int skipBytes = (int)readBits(&b, 2);
            uint32_t skip = 0;
            for (int i = 0; i < skipBytes; ++i) {
                const uint32_t v = readBits(&b, 8);
                if (i == skipBytes - 1 && skipBytes > 1 && !v) ok = false;
                skip |= v << (8 * i);
            }
            if (skipBytes) ++skip;
            alignBits(&b);
            for (uint32_t i = 0; i < skip && !b.bad; ++i) readBits(&b, 8);
            ok = ok && !b.bad;
            continue;
        }
        const int mn = nibbles + 4;
        uint32_t mlen = 0;
        for (int i = 0; i < mn; ++i) {
            const uint32_t v = readBits(&b, 4);
            if (i == mn - 1 && mn > 4 && !v) ok = false;
            mlen |= v << (4 * i);
        }
        ++mlen;
        if (!ok || out->len - start + mlen > maxSize) { ok = false; break; }
        if (!last && readBits(&b, 1)) { /* uncompressed */
            alignBits(&b);
            for (uint32_t i = 0; i < mlen && !b.bad; ++i) cscdVecPush(out, (uint8_t)readBits(&b, 8));
            ok = !b.bad;
            continue;
        }
        /* the header: block types, the distance parameters, the context modes and maps, the prefix codes */
        for (int i = 0; i < 3; ++i) blocksFree(&blocks[i]);
        for (int i = 0; ok && i < 3; ++i) ok = readBlocks(&b, &blocks[i]);
        if (!ok) break;
        const int npostfix = (int)readBits(&b, 2);
        const int ndirect = (int)readBits(&b, 4) << npostfix;
        cscdFree(cmodes);
        cmodes = (uint8_t*)cscdMalloc((size_t)blocks[0].types);
        for (int i = 0; i < blocks[0].types; ++i) cmodes[i] = (uint8_t)readBits(&b, 2);
        const int treesL = readCount256(&b);
        cscdFree(cmapL);
        cmapL = (uint8_t*)cscdMalloc((size_t)blocks[0].types * 64);
        ok = readContextMap(&b, blocks[0].types * 64, treesL, cmapL);
        const int treesD = ok ? readCount256(&b) : 0;
        cscdFree(cmapD);
        cmapD = (uint8_t*)cscdMalloc((size_t)blocks[2].types * 4);
        ok = ok && readContextMap(&b, blocks[2].types * 4, treesD, cmapD);
        for (int i = 0; i < nLits; ++i) huffFree(&lits[i]);
        for (int i = 0; i < nCmds; ++i) huffFree(&cmds[i]);
        for (int i = 0; i < nDists; ++i) huffFree(&dists[i]);
        cscdFree(lits);
        cscdFree(cmds);
        cscdFree(dists);
        nLits = treesL, nCmds = blocks[1].types, nDists = treesD;
        lits = (Huff*)cscdCalloc((size_t)nLits, sizeof(Huff));
        cmds = (Huff*)cscdCalloc((size_t)nCmds, sizeof(Huff));
        dists = (Huff*)cscdCalloc((size_t)(nDists ? nDists : 1), sizeof(Huff));
        const int distAlphabet = 16 + ndirect + (48 << npostfix);
        for (int i = 0; ok && i < nLits; ++i) ok = readPrefixCode(&b, 256, &lits[i]);
        for (int i = 0; ok && i < nCmds; ++i) ok = readPrefixCode(&b, 704, &cmds[i]);
        for (int i = 0; ok && i < nDists; ++i) ok = readPrefixCode(&b, distAlphabet, &dists[i]);
        if (!ok) break;
        /* the commands */
        const size_t end = out->len + mlen;
        while (ok && out->len < end) {
            if (b.bad) { ok = false; break; }
            Blocks* bi = &blocks[1];
            if (!bi->left && !switchBlock(&b, bi)) { ok = false; break; }
            --bi->left;
            const int code = huffDecode(&b, &cmds[bi->type]);
            static const uint8_t kInsertCell[11] = {0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16};
            static const uint8_t kCopyCell[11] = {0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16};
            const int cell = code >> 6;
            const int insertCode = kInsertCell[cell] + ((code >> 3) & 7), copyCode = kCopyCell[cell] + (code & 7);
            const uint32_t insert = kInsertBase[insertCode] + readBits(&b, kInsertExtra[insertCode]);
            const uint32_t copy = kCopyBase[copyCode] + readBits(&b, kCopyExtra[copyCode]);
            if (insert > end - out->len) { ok = false; break; }
            Blocks* bl = &blocks[0];
            for (uint32_t i = 0; i < insert; ++i) {
                if (!bl->left && !switchBlock(&b, bl)) { ok = false; break; }
                --bl->left;
                const size_t at = out->len - start;
                const uint8_t p1 = at > 0 ? out->items[out->len - 1] : 0, p2 = at > 1 ? out->items[out->len - 2] : 0;
                int ctx;
                switch (cmodes[bl->type]) {
                case 0: ctx = p1 & 0x3F; break;
                case 1: ctx = p1 >> 2; break;
                case 2: ctx = kLut0[p1] | kLut1[p2]; break;
                default: ctx = (kLut2[p1] << 3) | kLut2[p2]; break;
                }
                cscdVecPush(out, (uint8_t)huffDecode(&b, &lits[cmapL[64 * bl->type + ctx]]));
            }
            if (!ok || out->len >= end) break; /* (the last command's copy, when the literals fill the meta-block: none) */
            /* the distance */
            int distance;
            bool push = true;
            if (code < 128) {
                distance = dist[0];
                push = false;
            } else {
                Blocks* bd = &blocks[2];
                if (!bd->left && !switchBlock(&b, bd)) { ok = false; break; }
                --bd->left;
                const int dctx = copy > 4 ? 3 : (int)copy - 2;
                const int dcode = huffDecode(&b, &dists[cmapD[4 * bd->type + dctx]]);
                if (dcode < 16) {
                    static const int8_t kIndex[16] = {0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1};
                    static const int8_t kDelta[16] = {0, 0, 0, 0, -1, 1, -2, 2, -3, 3, -1, 1, -2, 2, -3, 3};
                    distance = dist[kIndex[dcode]] + kDelta[dcode];
                    if (distance <= 0) { ok = false; break; }
                    push = dcode != 0;
                } else if (dcode < 16 + ndirect) distance = dcode - 15;
                else {
                    const int d = dcode - ndirect - 16, nbits = 1 + (d >> (npostfix + 1));
                    const int hcode = d >> npostfix, lcode = d & ((1 << npostfix) - 1);
                    const int64_t offset = ((int64_t)(2 + (hcode & 1)) << nbits) - 4;
                    const int64_t v = ((offset + readBits(&b, nbits)) << npostfix) + lcode + ndirect + 1;
                    if (v > 0x7FFFFFFF) { ok = false; break; }
                    distance = (int)v;
                }
            }
            const size_t produced = out->len - start;
            const size_t maxDistance = produced < window ? produced : window;
            if ((size_t)distance > maxDistance) { /* a word of the dictionary */
                cscdCallOnce(&dictionaryOnce, loadDictionary);
                uint8_t word[40];
                const int n = dictionaryWord((int)copy, (uint32_t)((size_t)distance - maxDistance - 1), word);
                if (n < 0 || (size_t)n > end - out->len) { ok = false; break; }
                cscdVecAppend(out, word, (size_t)n);
                continue; /* (not remembered as a distance) */
            }
            if (copy > end - out->len) { ok = false; break; }
            if (push) {
                dist[3] = dist[2];
                dist[2] = dist[1];
                dist[1] = dist[0];
                dist[0] = distance;
            }
            for (uint32_t i = 0; i < copy; ++i) {
                const uint8_t c = out->items[out->len - (size_t)distance];
                cscdVecPush(out, c);
            }
        }
    }
    for (int i = 0; i < 3; ++i) blocksFree(&blocks[i]);
    for (int i = 0; i < nLits; ++i) huffFree(&lits[i]);
    for (int i = 0; i < nCmds; ++i) huffFree(&cmds[i]);
    for (int i = 0; i < nDists; ++i) huffFree(&dists[i]);
    cscdFree(lits);
    cscdFree(cmds);
    cscdFree(dists);
    cscdFree(cmodes);
    cscdFree(cmapL);
    cscdFree(cmapD);
    if (b.bad) ok = false;
    if (!ok) out->len = start;
    return ok;
}
