/* WOFF2 (W3C's WOFF File Format 2.0): the tables of an OpenType font, or a collection's, in one Brotli stream; the
 * glyf and loca tables transformed (points as triplets, streams of contours, flags, instructions, composites and
 * boxes), maybe hmtx too (the side bearings the glyphs' boxes give left out). Unwrapped here into the font it was: the
 * tables back in their own form, put in an sfnt (or a TrueType collection) the rest of the library reads. */
#include "text/fontInternal.h"

#include <string.h>

/* ---- reading ----------------------------------------------------------------------------------------------------- */

typedef struct Rd {
    const uint8_t* p;
    size_t n, at;
    bool bad;
} Rd;

static Rd rdOf(const uint8_t* p, size_t n) { return (Rd){p, n, 0, false}; }
static bool has(Rd* r, size_t k) {
    if (r->bad || k > r->n - r->at) r->bad = true;
    return !r->bad;
}
static uint8_t r8(Rd* r) { return has(r, 1) ? r->p[r->at++] : 0; }
static uint16_t r16(Rd* r) {
    if (!has(r, 2)) return 0;
    const uint16_t v = (uint16_t)(r->p[r->at] << 8 | r->p[r->at + 1]);
    r->at += 2;
    return v;
}
static uint32_t r32(Rd* r) {
    const uint32_t hi = r16(r);
    return hi << 16 | r16(r);
}
static const uint8_t* rBytes(Rd* r, size_t k) {
    if (!has(r, k)) return NULL;
    const uint8_t* p = r->p + r->at;
    r->at += k;
    return p;
}

/* UIntBase128: 7 bits a byte, the high bit set on all but the last; at most 5 bytes, no leading zeros */
static uint32_t rBase128(Rd* r) {
    uint32_t v = 0;
    for (int i = 0; i < 5; ++i) {
        const uint8_t b = r8(r);
        if ((i == 0 && b == 0x80) || (v & 0xFE000000u)) r->bad = true;
        v = v << 7 | (b & 0x7F);
        if (!(b & 0x80)) return v;
    }
    r->bad = true;
    return 0;
}

/* 255UInt16: a byte, or 253 then a word, 255 then 253 more than a byte, 254 then 506 more */
static uint16_t r255(Rd* r) {
    const uint8_t c = r8(r);
    if (c == 253) return r16(r);
    if (c == 255) return (uint16_t)(253 + r8(r));
    if (c == 254) return (uint16_t)(506 + r8(r));
    return c;
}

/* ---- writing ----------------------------------------------------------------------------------------------------- */

static void w8(CscdBytes* o, uint32_t v) { cscdVecPush(o, (uint8_t)v); }
static void w16(CscdBytes* o, uint32_t v) {
    w8(o, v >> 8);
    w8(o, v);
}
static void w32(CscdBytes* o, uint32_t v) {
    w16(o, v >> 16);
    w16(o, v);
}
static void pad4(CscdBytes* o) {
    while (o->len & 3) w8(o, 0);
}
static void set32(CscdBytes* o, size_t at, uint32_t v) {
    o->items[at] = (uint8_t)(v >> 24);
    o->items[at + 1] = (uint8_t)(v >> 16);
    o->items[at + 2] = (uint8_t)(v >> 8);
    o->items[at + 3] = (uint8_t)v;
}

/* ---- the table directory ------------------------------------------------------------------------------------------ */

static const char kKnownTags[63][5] = {
    "cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ", "fpgm", "glyf", "loca", "prep", "CFF ", "VORG", "EBDT",
    "EBLC", "gasp", "hdmx", "kern", "LTSH", "PCLT", "VDMX", "vhea", "vmtx", "BASE", "GDEF", "GPOS", "GSUB", "EBSC", "JSTF", "MATH",
    "CBDT", "CBLC", "COLR", "CPAL", "SVG ", "sbix", "acnt", "avar", "bdat", "bloc", "bsln", "cvar", "fdsc", "feat", "fmtx", "fvar",
    "gvar", "hsty", "just", "lcar", "mort", "morx", "opbd", "prop", "trak", "Zapf", "Silf", "Glat", "Gloc", "Feat", "Sill",
};

#define TAG(s) ((uint32_t)(s)[0] << 24 | (uint32_t)(s)[1] << 16 | (uint32_t)(s)[2] << 8 | (uint32_t)(s)[3])

typedef struct Table {
    uint32_t tag, origLength, length; /* (length: of its data in the stream, transformed or not) */
    uint8_t version;
    bool transformed;
    size_t at;       /* (where its data is in the stream) */
    CscdBytes built; /* the table rebuilt from a transform */
    CscdIntVec xMins; /* (glyf: each glyph's xMin, for an hmtx transformed) */
    int glyf;        /* (loca, hmtx: their font's glyf table, -1: none) */
} Table;

/* ---- glyf and loca ----------------------------------------------------------------------------------------------- */

#define MAX_POINTS 65535

static bool rebuildGlyf(Table* glyf, Table* loca, const uint8_t* d) {
    Rd h = rdOf(d, glyf->length);
    r16(&h); /* (reserved) */
    const uint16_t options = r16(&h), numGlyphs = r16(&h), indexFormat = r16(&h);
    uint32_t sizes[7];
    size_t total = 36;
    for (int i = 0; i < 7; ++i) {
        sizes[i] = r32(&h);
        total += sizes[i];
    }
    if (h.bad || total > glyf->length || indexFormat > 1) return false;
    Rd s[7];
    size_t at = 36;
    for (int i = 0; i < 7; ++i) {
        s[i] = rdOf(d + at, sizes[i]);
        at += sizes[i];
    }
    Rd *contours = &s[0], *points = &s[1], *flagsS = &s[2], *glyphs = &s[3], *composites = &s[4], *boxes = &s[5], *instr = &s[6];
    const uint8_t* overlap = NULL;
    if (options & 1) {
        if (at + (numGlyphs + 7u) / 8 > glyf->length) return false;
        overlap = d + at;
    }
    const size_t bitmapLen = ((numGlyphs + 31u) >> 5) << 2;
    const uint8_t* bboxBitmap = rBytes(boxes, bitmapLen);
    if (!bboxBitmap) return false;
    CscdBytes* o = &glyf->built;
    CscdBytes locaOut = {0};
    cscdVecClear(&glyf->xMins);
    uint16_t* ends = (uint16_t*)cscdMalloc(sizeof(uint16_t) * 1);
    int32_t* xs = NULL;
    int32_t* ys = NULL;
    uint8_t* on = NULL;
    size_t cap = 0, endsCap = 1;
    bool ok = true;
    for (uint32_t g = 0; ok && g < numGlyphs; ++g) {
        if (indexFormat) w32(&locaOut, (uint32_t)o->len);
        else w16(&locaOut, (uint32_t)(o->len / 2));
        const int16_t nc = (int16_t)r16(contours);
        const bool haveBox = bboxBitmap[g >> 3] & (0x80 >> (g & 7));
        if (nc == 0) {
            if (haveBox) ok = false;
            cscdVecPush(&glyf->xMins, 0);
            continue;
        }
        if (nc > 0) { /* simple */
            if ((size_t)nc > endsCap) {
                endsCap = (size_t)nc;
                ends = (uint16_t*)cscdRealloc(ends, sizeof(uint16_t) * endsCap);
            }
            uint32_t n = 0;
            for (int c = 0; c < nc; ++c) {
                n += r255(points);
                if (n == 0 || n > MAX_POINTS) { ok = false; break; }
                ends[c] = (uint16_t)(n - 1);
            }
            if (!ok || points->bad) { ok = false; break; }
            if (n > cap) {
                cap = n;
                xs = (int32_t*)cscdRealloc(xs, sizeof(int32_t) * cap);
                ys = (int32_t*)cscdRealloc(ys, sizeof(int32_t) * cap);
                on = (uint8_t*)cscdRealloc(on, cap);
            }
            const uint8_t* fl = rBytes(flagsS, n);
            if (!fl) { ok = false; break; }
            int32_t x = 0, y = 0;
            for (uint32_t i = 0; i < n && ok; ++i) {
                uint32_t f = fl[i];
                on[i] = (uint8_t)!(f >> 7);
                f &= 0x7F;
                const int bytes = f < 84 ? 1 : f < 120 ? 2 : f < 124 ? 3 : 4;
                const uint8_t* t = rBytes(glyphs, (size_t)bytes);
                if (!t) { ok = false; break; }
                int dx, dy;
#define SIGNED(flag, v) (((flag) & 1) ? (v) : -(v))
                if (f < 10) {
                    dx = 0;
                    dy = SIGNED(f, (int)((f & 14) << 7) + t[0]);
                } else if (f < 20) {
                    dx = SIGNED(f, (int)(((f - 10) & 14) << 7) + t[0]);
                    dy = 0;
                } else if (f < 84) {
                    const uint32_t b0 = f - 20, b1 = t[0];
                    dx = SIGNED(f, (int)(1 + (b0 & 0x30) + (b1 >> 4)));
                    dy = SIGNED(f >> 1, (int)(1 + ((b0 & 0x0C) << 2) + (b1 & 0x0F)));
                } else if (f < 120) {
                    const uint32_t b0 = f - 84;
                    dx = SIGNED(f, (int)(1 + ((b0 / 12) << 8) + t[0]));
                    dy = SIGNED(f >> 1, (int)(1 + (((b0 % 12) >> 2) << 8) + t[1]));
                } else if (f < 124) {
                    dx = SIGNED(f, (int)((t[0] << 4) + (t[1] >> 4)));
                    dy = SIGNED(f >> 1, (int)(((t[1] & 0x0F) << 8) + t[2]));
                } else {
                    dx = SIGNED(f, (int)((t[0] << 8) + t[1]));
                    dy = SIGNED(f >> 1, (int)((t[2] << 8) + t[3]));
                }
#undef SIGNED
                x += dx;
                y += dy;
                if (x < -32768 || x > 32767 || y < -32768 || y > 32767) ok = false;
                xs[i] = x;
                ys[i] = y;
            }
            const uint16_t insLen = r255(glyphs);
            const uint8_t* ins = rBytes(instr, insLen);
            if (!ok || !ins) { ok = false; break; }
            int32_t box[4];
            if (haveBox) {
                for (int k = 0; k < 4; ++k) box[k] = (int16_t)r16(boxes);
            } else {
                box[0] = box[2] = xs[0];
                box[1] = box[3] = ys[0];
                for (uint32_t i = 1; i < n; ++i) {
                    box[0] = xs[i] < box[0] ? xs[i] : box[0];
                    box[1] = ys[i] < box[1] ? ys[i] : box[1];
                    box[2] = xs[i] > box[2] ? xs[i] : box[2];
                    box[3] = ys[i] > box[3] ? ys[i] : box[3];
                }
            }
            cscdVecPush(&glyf->xMins, box[0]);
            w16(o, (uint16_t)nc);
            for (int k = 0; k < 4; ++k) w16(o, (uint16_t)box[k]);
            for (int c = 0; c < nc; ++c) w16(o, ends[c]);
            w16(o, insLen);
            cscdVecAppend(o, ins, insLen);
            /* the flags (on the curve; each coordinate the same, a byte with its sign, or a word), then x, then y */
            const bool overlapping = overlap && (overlap[g >> 3] & (0x80 >> (g & 7)));
            int32_t px = 0, py = 0;
            for (uint32_t i = 0; i < n; ++i) {
                const int32_t dx = xs[i] - px, dy = ys[i] - py;
                uint8_t f = on[i] ? 1 : 0;
                if (dx == 0) f |= 0x10;
                else if (dx >= -255 && dx <= 255) f |= (uint8_t)(0x02 | (dx > 0 ? 0x10 : 0));
                if (dy == 0) f |= 0x20;
                else if (dy >= -255 && dy <= 255) f |= (uint8_t)(0x04 | (dy > 0 ? 0x20 : 0));
                if (i == 0 && overlapping) f |= 0x40;
                w8(o, f);
                px = xs[i];
                py = ys[i];
            }
            px = 0;
            for (uint32_t i = 0; i < n; ++i) {
                const int32_t dx = xs[i] - px;
                if (dx != 0 && dx >= -255 && dx <= 255) w8(o, (uint32_t)(dx < 0 ? -dx : dx));
                else if (dx != 0) w16(o, (uint16_t)dx);
                px = xs[i];
            }
            py = 0;
            for (uint32_t i = 0; i < n; ++i) {
                const int32_t dy = ys[i] - py;
                if (dy != 0 && dy >= -255 && dy <= 255) w8(o, (uint32_t)(dy < 0 ? -dy : dy));
                else if (dy != 0) w16(o, (uint16_t)dy);
                py = ys[i];
            }
        } else if (nc == -1) { /* composite: its components as they are, its box given */
            if (!haveBox) { ok = false; break; }
            const size_t from = composites->at;
            bool more = true, instructions = false;
            while (more && !composites->bad) {
                const uint16_t f = r16(composites);
                r16(composites); /* (the glyph) */
                size_t skip = (f & 1) ? 4 : 2;
                if (f & 0x08) skip += 2;
                else if (f & 0x40) skip += 4;
                else if (f & 0x80) skip += 8;
                rBytes(composites, skip);
                more = f & 0x20;
                instructions = instructions || (f & 0x100);
            }
            if (composites->bad) { ok = false; break; }
            int32_t box[4];
            for (int k = 0; k < 4; ++k) box[k] = (int16_t)r16(boxes);
            cscdVecPush(&glyf->xMins, box[0]);
            w16(o, 0xFFFF);
            for (int k = 0; k < 4; ++k) w16(o, (uint16_t)box[k]);
            cscdVecAppend(o, composites->p + from, composites->at - from);
            if (instructions) {
                const uint16_t insLen = r255(glyphs);
                const uint8_t* ins = rBytes(instr, insLen);
                if (!ins) { ok = false; break; }
                w16(o, insLen);
                cscdVecAppend(o, ins, insLen);
            }
        } else ok = false;
        pad4(o);
        if (o->len > ((size_t)256 << 20) || (!indexFormat && o->len > 0x1FFFE)) ok = false;
    }
    if (indexFormat) w32(&locaOut, (uint32_t)o->len);
    else w16(&locaOut, (uint32_t)(o->len / 2));
    for (int i = 0; i < 7; ++i) ok = ok && !s[i].bad;
    if (ok && loca) {
        ok = loca->origLength == locaOut.len;
        cscdVecFree(&loca->built);
        loca->built = locaOut;
        locaOut = (CscdBytes){0};
    }
    cscdVecFree(&locaOut);
    cscdFree(ends);
    cscdFree(xs);
    cscdFree(ys);
    cscdFree(on);
    return ok;
}

/* ---- hmtx -------------------------------------------------------------------------------------------------------- */

static bool rebuildHmtx(Table* hmtx, const Table* glyf, const uint8_t* d, uint16_t numHMetrics, uint16_t numGlyphs) {
    Rd r = rdOf(d, hmtx->length);
    const uint8_t flags = r8(&r);
    if ((flags & 0xFC) || !(flags & 3) || !glyf || glyf->xMins.len != numGlyphs || numHMetrics > numGlyphs || !numHMetrics) return false;
    const uint8_t* adv = rBytes(&r, 2u * numHMetrics);
    const uint8_t* lsb = (flags & 1) ? NULL : rBytes(&r, 2u * numHMetrics);
    const uint8_t* more = (flags & 2) ? NULL : rBytes(&r, 2u * (numGlyphs - numHMetrics));
    if (r.bad) return false;
    CscdBytes* o = &hmtx->built;
    for (uint32_t g = 0; g < numHMetrics; ++g) {
        w16(o, (uint32_t)(adv[2 * g] << 8 | adv[2 * g + 1]));
        if (lsb) w16(o, (uint32_t)(lsb[2 * g] << 8 | lsb[2 * g + 1]));
        else w16(o, (uint16_t)glyf->xMins.items[g]);
    }
    for (uint32_t g = numHMetrics; g < numGlyphs; ++g) {
        const uint32_t k = g - numHMetrics;
        if (more) w16(o, (uint32_t)(more[2 * k] << 8 | more[2 * k + 1]));
        else w16(o, (uint16_t)glyf->xMins.items[g]);
    }
    return o->len == hmtx->origLength;
}

/* ---- the font ------------------------------------------------------------------------------------------------------ */

static uint32_t checksum(const uint8_t* p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = 0;
        for (size_t k = 0; k < 4; ++k) v = v << 8 | (i + k < n ? p[i + k] : 0);
        sum += v;
    }
    return sum;
}

typedef struct Font {
    uint32_t flavor;
    CscdIntVec tables; /* (indices in the directory, as the collection lists them) */
} Font;

static const uint8_t* tableData(const Table* t, const CscdBytes* stream) { return t->transformed ? t->built.items : stream->items + t->at; }
static size_t tableLength(const Table* t) { return t->transformed ? t->built.len : t->origLength; }

static int compareTags(const void* a, const void* b, void* ctx) {
    const Table* tables = (const Table*)ctx;
    const uint32_t x = tables[*(const int*)a].tag, y = tables[*(const int*)b].tag;
    return x < y ? -1 : x > y;
}

/* One font's offset table and table records at the end of out (the tables' places filled in by the caller). */
static void writeOffsetTable(CscdBytes* out, const Font* f, Table* tables, size_t* recordsAt) {
    const uint32_t n = (uint32_t)f->tables.len;
    uint32_t entry = 1, log = 0;
    while (entry * 2 <= n) {
        entry *= 2;
        ++log;
    }
    w32(out, f->flavor);
    w16(out, n);
    w16(out, entry * 16);
    w16(out, log);
    w16(out, n * 16 - entry * 16);
    *recordsAt = out->len;
    for (uint32_t i = 0; i < n; ++i) {
        w32(out, tables[f->tables.items[i]].tag);
        w32(out, 0);
        w32(out, 0);
        w32(out, 0);
    }
}

bool cscdWoff2Unwrap(const uint8_t* data, size_t size, CscdBytes* out) {
    Rd r = rdOf(data, size);
    if (r32(&r) != TAG("wOF2")) return false;
    const uint32_t flavor = r32(&r), length = r32(&r);
    const uint16_t numTables = r16(&r);
    r16(&r); /* (reserved) */
    const uint32_t sfntSize = r32(&r), compressedSize = r32(&r);
    r.at = 48;
    if (r.bad || length != size || !numTables || sfntSize > ((uint32_t)256 << 20)) return false;
    Table* tables = (Table*)cscdCalloc(numTables, sizeof(Table));
    Font* fonts = NULL;
    uint32_t numFonts = 0;
    CscdBytes stream = {0};
    bool ok = true;
    size_t streamLen = 0;
    int glyfIndex = -1;
    for (uint32_t i = 0; ok && i < numTables; ++i) {
        Table* t = &tables[i];
        t->glyf = -1;
        const uint8_t flags = r8(&r);
        const int tagIndex = flags & 63;
        t->version = (uint8_t)(flags >> 6);
        t->tag = tagIndex == 63 ? r32(&r) : TAG(kKnownTags[tagIndex]);
        t->origLength = rBase128(&r);
        const bool glyfOrLoca = t->tag == TAG("glyf") || t->tag == TAG("loca");
        t->transformed = glyfOrLoca ? t->version == 0 : t->version != 0;
        if (t->transformed && !glyfOrLoca && !(t->tag == TAG("hmtx") && t->version == 1)) ok = false;
        if (glyfOrLoca && t->version != 0 && t->version != 3) ok = false;
        t->length = t->transformed ? rBase128(&r) : t->origLength;
        if (t->tag == TAG("loca") && t->transformed && t->length) ok = false;
        t->at = streamLen;
        streamLen += t->length;
        if (r.bad || streamLen > ((size_t)256 << 20)) ok = false;
        if (t->tag == TAG("glyf")) glyfIndex = (int)i;
    }
    for (uint32_t i = 0; ok && flavor != TAG("ttcf") && i < numTables; ++i) /* (one font: its glyf, wherever it is listed) */
        if (tables[i].tag == TAG("loca") || tables[i].tag == TAG("hmtx")) tables[i].glyf = glyfIndex;
    /* a collection: its fonts, each a list of the tables */
    if (ok && flavor == TAG("ttcf")) {
        const uint32_t version = r32(&r);
        numFonts = r255(&r);
        if (version != 0x00010000 && version != 0x00020000) ok = false;
        fonts = (Font*)cscdCalloc(numFonts ? numFonts : 1, sizeof(Font));
        for (uint32_t f = 0; ok && f < numFonts; ++f) {
            const uint16_t n = r255(&r);
            fonts[f].flavor = r32(&r);
            int glyf = -1;
            for (uint16_t k = 0; ok && k < n; ++k) {
                const uint16_t index = r255(&r);
                if (index >= numTables) ok = false;
                else {
                    cscdVecPush(&fonts[f].tables, (int)index);
                    if (tables[index].tag == TAG("glyf")) glyf = index;
                }
            }
            for (size_t k = 0; ok && k < fonts[f].tables.len; ++k) {
                Table* t = &tables[fonts[f].tables.items[k]];
                if (t->tag == TAG("loca") || t->tag == TAG("hmtx")) t->glyf = glyf;
            }
        }
        ok = ok && !r.bad && numFonts > 0;
    } else if (ok) {
        numFonts = 1;
        fonts = (Font*)cscdCalloc(1, sizeof(Font));
        fonts[0].flavor = flavor;
        for (uint32_t i = 0; i < numTables; ++i) cscdVecPush(&fonts[0].tables, (int)i);
    }
    /* the stream */
    const uint8_t* compressed = ok ? rBytes(&r, compressedSize) : NULL;
    ok = ok && compressed && cscdBrotliDecode(compressed, compressedSize, &stream, streamLen) && stream.len == streamLen;
    /* the transforms undone: glyf (with its loca), then hmtx (with glyf's boxes) */
    for (uint32_t i = 0; ok && i < numTables; ++i) {
        Table* t = &tables[i];
        if (t->tag != TAG("glyf") || !t->transformed) continue;
        Table* loca = NULL;
        for (uint32_t k = 0; k < numTables; ++k)
            if (tables[k].tag == TAG("loca") && tables[k].glyf == (int)i) loca = &tables[k];
        if (!loca || !loca->transformed) ok = false;
        else ok = rebuildGlyf(t, loca, stream.items + t->at);
    }
    for (uint32_t i = 0; ok && i < numTables; ++i) {
        Table* t = &tables[i];
        if (t->tag == TAG("loca") && t->transformed && (t->glyf < 0 || !tables[t->glyf].transformed)) ok = false;
        if (t->tag != TAG("hmtx") || !t->transformed) continue;
        if (t->glyf < 0 || !tables[t->glyf].transformed) { ok = false; break; }
        /* (numberOfHMetrics from hhea, numGlyphs from maxp: those of its font) */
        uint16_t numHMetrics = 0, numGlyphs = 0;
        for (uint32_t f = 0; f < numFonts; ++f) {
            bool mine = false;
            for (size_t k = 0; k < fonts[f].tables.len; ++k) mine = mine || fonts[f].tables.items[k] == (int)i;
            if (!mine) continue;
            for (size_t k = 0; k < fonts[f].tables.len; ++k) {
                const Table* o = &tables[fonts[f].tables.items[k]];
                if (o->tag == TAG("hhea") && o->origLength >= 36) numHMetrics = (uint16_t)(stream.items[o->at + 34] << 8 | stream.items[o->at + 35]);
                if (o->tag == TAG("maxp") && o->origLength >= 6) numGlyphs = (uint16_t)(stream.items[o->at + 4] << 8 | stream.items[o->at + 5]);
            }
            break;
        }
        ok = rebuildHmtx(t, &tables[t->glyf], stream.items + t->at, numHMetrics, numGlyphs);
    }
    /* the font (or collection): offset tables, then each table once, 4-byte aligned */
    if (ok) {
        cscdVecClear(out);
        for (uint32_t f = 0; f < numFonts; ++f) cscdSort(fonts[f].tables.items, fonts[f].tables.len, sizeof(int), compareTags, tables);
        size_t* records = (size_t*)cscdCalloc(numFonts, sizeof(size_t));
        if (flavor == TAG("ttcf")) {
            w32(out, TAG("ttcf"));
            w32(out, 0x00010000);
            w32(out, numFonts);
            for (uint32_t f = 0; f < numFonts; ++f) w32(out, 0);
        }
        for (uint32_t f = 0; f < numFonts; ++f) {
            if (flavor == TAG("ttcf")) set32(out, 12 + 4 * f, (uint32_t)out->len);
            writeOffsetTable(out, &fonts[f], tables, &records[f]);
        }
        size_t* placed = (size_t*)cscdCalloc(numTables, sizeof(size_t));
        for (uint32_t i = 0; i < numTables; ++i) {
            pad4(out);
            placed[i] = out->len;
            cscdVecAppend(out, tableData(&tables[i], &stream), tableLength(&tables[i]));
        }
        pad4(out);
        for (uint32_t f = 0; f < numFonts; ++f)
            for (size_t k = 0; k < fonts[f].tables.len; ++k) {
                const int i = fonts[f].tables.items[k];
                const size_t rec = records[f] + 16 * k;
                const size_t len = tableLength(&tables[i]);
                set32(out, rec + 4, checksum(out->items + placed[i], len));
                set32(out, rec + 8, (uint32_t)placed[i]);
                set32(out, rec + 12, (uint32_t)len);
            }
        cscdFree(placed);
        cscdFree(records);
    }
    for (uint32_t i = 0; i < numTables; ++i) {
        cscdVecFree(&tables[i].built);
        cscdVecFree(&tables[i].xMins);
    }
    for (uint32_t f = 0; f < numFonts && fonts; ++f) cscdVecFree(&fonts[f].tables);
    cscdFree(fonts);
    cscdFree(tables);
    cscdVecFree(&stream);
    return ok;
}
