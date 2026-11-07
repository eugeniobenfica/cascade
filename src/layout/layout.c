/* Box tree construction, block layout (margins, floats, positioning), inline layout (whitespace,
 * line breaking, vertical alignment) and intrinsic sizes. */
#include "layout/layoutInternal.h"
#include "paint/canvasInternal.h"
#include "text/bidiInternal.h"
#include "core/thread.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- helpers ------------------------------------------------------------------------------------ */

/* What the font-variant-*, font-feature-settings, font-variation-settings, font-kerning,
 * font-optical-sizing and font-stretch properties ask of the font. */
static const CscdFontSettings* fontSettingsOf(const CscdFontDesc* d) {
    CscdFontSettings s;
    memset(&s, 0, sizeof s);
#define FEATURE(t, v) do { if (s.featureCount < CSCD_COUNT(s.features)) s.features[s.featureCount++] = (CscdFontTagValue){cscdFontTag(t), (v)}; } while (0)
    static const char* kCaps[][2] = {{NULL, NULL}, {"smcp", NULL}, {"smcp", "c2sc"}, {"pcap", NULL}, {"pcap", "c2pc"}, {"unic", NULL}, {"titl", NULL}};
    if (d->caps && d->caps < CSCD_COUNT(kCaps)) {
        FEATURE(kCaps[d->caps][0], 1);
        if (kCaps[d->caps][1]) FEATURE(kCaps[d->caps][1], 1);
        s.synthesizeSmallCaps = (d->caps == CSCD_CAPS_SMALL || d->caps == CSCD_CAPS_ALL_SMALL) && !(d->noSynthesis & 4);
    }
    static const struct { uint16_t bit; const char* tag; } kNumeric[] = {
        {CSCD_NUMERIC_LINING, "lnum"}, {CSCD_NUMERIC_OLDSTYLE, "onum"}, {CSCD_NUMERIC_PROPORTIONAL, "pnum"},
        {CSCD_NUMERIC_DIAGONAL, "frac"}, {CSCD_NUMERIC_STACKED, "afrc"}, {CSCD_NUMERIC_ORDINAL, "ordn"}, {CSCD_NUMERIC_SLASHED_ZERO, "zero"}};
    for (size_t i = 0; i < CSCD_COUNT(kNumeric); ++i)
        if (d->numeric & kNumeric[i].bit) FEATURE(kNumeric[i].tag, 1);
    if (d->ligatures & CSCD_LIGATURES_DISCRETIONARY) FEATURE("dlig", 1);
    if (d->ligatures & CSCD_LIGATURES_HISTORICAL) FEATURE("hlig", 1);
    if (d->ligatures & CSCD_LIGATURES_NO_CONTEXTUAL) FEATURE("calt", 0);
    if (d->position == 1) FEATURE("subs", 1);
    if (d->position == 2) FEATURE("sups", 1);
    static const struct { uint16_t bit; const char* tag; } kEastAsian[] = {
        {CSCD_EAST_ASIAN_JIS78, "jp78"}, {CSCD_EAST_ASIAN_JIS83, "jp83"}, {CSCD_EAST_ASIAN_JIS90, "jp90"}, {CSCD_EAST_ASIAN_JIS04, "jp04"},
        {CSCD_EAST_ASIAN_SIMPLIFIED, "smpl"}, {CSCD_EAST_ASIAN_TRADITIONAL, "trad"}, {CSCD_EAST_ASIAN_FULL_WIDTH, "fwid"},
        {CSCD_EAST_ASIAN_PROPORTIONAL_WIDTH, "pwid"}, {CSCD_EAST_ASIAN_RUBY, "ruby"}};
    for (size_t i = 0; i < CSCD_COUNT(kEastAsian); ++i)
        if (d->eastAsian & kEastAsian[i].bit) FEATURE(kEastAsian[i].tag, 1);
#undef FEATURE
    const CscdFontSettings* fs = d->featureSettings;
    for (size_t i = 0; fs && i < fs->featureCount && s.featureCount < CSCD_COUNT(s.features); ++i) s.features[s.featureCount++] = fs->features[i];
    const CscdFontSettings* vs = d->variationSettings;
    for (size_t i = 0; vs && i < vs->variationCount; ++i) s.variations[s.variationCount++] = vs->variations[i];
    s.noKerning = d->kerning == 2;
    s.noOpticalSizing = d->noOpticalSizing;
    s.stretch = d->stretch;
    return cscdFontSettingsIntern(&s);
}

CscdFont cscdFontForStyle(const CscdComputedStyle* cs) {
    /* computed styles are shared and kept, so the face found for one is remembered in it */
    CscdComputedStyle* s = (CscdComputedStyle*)cs; /* (a cache) */
    CscdResolvedFont* r = &s->resolvedFont;
    const uint32_t gen = cscdFontGeneration();
    if (r->generation == gen && r->families == (const void*)s->font.families.items && r->count == s->font.families.len &&
        r->weight == s->font.weight && r->italic == s->font.italic && r->size == s->font.size) {
        CscdFont f = {(CscdFontFace*)r->face, s->font.size, r->fakeBold && !(s->font.noSynthesis & 1), r->fakeItalic && !(s->font.noSynthesis & 2),
                      s->font.tabularNums, s->font.noLigatures, r->settings};
        return f;
    }
    CscdFont f = cscdFontMatch(&s->font.families, s->font.size, s->font.weight, s->font.italic);
    r->settings = fontSettingsOf(&s->font);
    f = cscdFontWithSettings(f, s->font.weight, r->settings);
    r->families = s->font.families.items;
    r->count = s->font.families.len;
    r->weight = s->font.weight;
    r->italic = s->font.italic;
    r->size = s->font.size;
    r->generation = gen;
    r->face = f.face;
    r->fakeBold = f.fakeBold;
    r->fakeItalic = f.fakeItalic;
    f.tabularNums = s->font.tabularNums;
    if (s->font.noSynthesis & 1) f.fakeBold = false;
    if (s->font.noSynthesis & 2) f.fakeItalic = false;
    f.noLigatures = s->font.noLigatures;
    return f;
}

void cscdTextFragVecClear(CscdTextFragVec* v) {
    for (size_t i = 0; i < v->len; ++i) cscdStrFree(&v->items[i].text);
    v->len = 0;
}

CscdLayoutBox* cscdLayoutBoxNew(void) {
    CscdLayoutBox* b = CSCD_NEW(CscdLayoutBox);
    b->kind = CSCD_BOX_BLOCK;
    b->font = cscdFontDefault();
    b->minContent = b->maxContent = -1;
    return b;
}

void cscdLayoutBoxFree(CscdLayoutBox* b) {
    if (!b) return;
    for (size_t i = 0; i < b->children.len; ++i) cscdLayoutBoxFree(cscdBoxChild(b, i));
    cscdVecFree(&b->children);
    cscdStrFree(&b->text);
    cscdStrFree(&b->pseudo);
    cscdVecFree(&b->shape);
    cscdRelease(b->style);
    cscdRelease(b->image);
    cscdTextFragVecClear(&b->textFrags);
    cscdVecFree(&b->textFrags);
    cscdVecFree(&b->inlineFrags);
    cscdVecFree(&b->lines);
    cscdFree(b);
}

void cscdBoxShift(CscdLayoutBox* b, float dx, float dy) {
    if (dx == 0 && dy == 0) return;
    b->x += dx; b->y += dy;
    b->baseline += dy; b->lastBaseline += dy;
    b->staticX += dx; b->staticY += dy;
    b->overflow = cscdRectTranslated(b->overflow, dx, dy);
    for (size_t i = 0; i < b->textFrags.len; ++i) { CscdTextFrag* f = &b->textFrags.items[i]; f->x += dx; f->baseline += dy; f->top += dy; }
    for (size_t i = 0; i < b->inlineFrags.len; ++i) b->inlineFrags.items[i].rect = cscdRectTranslated(b->inlineFrags.items[i].rect, dx, dy);
    for (size_t i = 0; i < b->lines.len; ++i) { CscdLineInfo* l = &b->lines.items[i]; l->top += dy; l->baseline += dy; l->left += dx; }
    for (size_t i = 0; i < b->children.len; ++i) cscdBoxShift(cscdBoxChild(b, i), dx, dy);
}

/* The rightmost (right: true) or leftmost x of a closed polygon between y0 and y1; false when it
 * does not reach into that band. */
static bool shapeExtent(const CscdPointVec* poly, float y0, float y1, bool right, float* out) {
    bool any = false;
    float best = right ? -1e30f : 1e30f;
    for (size_t i = 0; i < poly->len; ++i) {
        CscdPoint a = poly->items[i], b = poly->items[(i + 1) % poly->len];
        if (a.y > b.y) { const CscdPoint t = a; a = b; b = t; }
        if (b.y < y0 || a.y > y1) continue;
        /* the part of the edge inside the band: its ends */
        const float ta = a.y < y0 && b.y > a.y ? (y0 - a.y) / (b.y - a.y) : 0, tb = b.y > y1 && b.y > a.y ? (y1 - a.y) / (b.y - a.y) : 1;
        const float xa = a.x + (b.x - a.x) * ta, xb = a.x + (b.x - a.x) * tb;
        best = right ? cscdMaxf(best, cscdMaxf(xa, xb)) : cscdMinf(best, cscdMinf(xa, xb));
        any = true;
    }
    *out = best;
    return any;
}

void fcBand(const FloatCtx* fc, float y, float h, float left, float right, float* outL, float* outR) {
    *outL = left;
    *outR = right;
    for (size_t i = 0; i < fc->floats.len; ++i) {
        const FloatBox* f = &fc->floats.items[i];
        if (cscdRectBottom(f->r) <= y || f->r.y >= y + cscdMaxf(h, 0.01f)) continue;
        if (f->shape) { /* how far the shape reaches into the band, sideways */
            float x;
            if (!shapeExtent(f->shape, y - f->shapeMargin, y + cscdMaxf(h, 0.01f) + f->shapeMargin, f->left, &x)) continue;
            if (f->left) *outL = cscdMaxf(*outL, cscdMinf(x + f->shapeMargin, cscdRectRight(f->r)));
            else *outR = cscdMinf(*outR, cscdMaxf(x - f->shapeMargin, f->r.x));
            continue;
        }
        if (f->left) *outL = cscdMaxf(*outL, cscdRectRight(f->r));
        else *outR = cscdMinf(*outR, f->r.x);
    }
}

float fcClearY(const FloatCtx* fc, CscdClear c) {
    float y = -1e30f;
    for (size_t i = 0; i < fc->floats.len; ++i) {
        const FloatBox* f = &fc->floats.items[i];
        if (c == CSCD_CLEAR_BOTH || (c == CSCD_CLEAR_LEFT && f->left) || (c == CSCD_CLEAR_RIGHT && !f->left)) y = cscdMaxf(y, cscdRectBottom(f->r));
    }
    return y;
}

float fcBottom(const FloatCtx* fc) {
    float y = -1e30f;
    for (size_t i = 0; i < fc->floats.len; ++i) y = cscdMaxf(y, cscdRectBottom(fc->floats.items[i].r));
    return y;
}

CscdPoint fcPlace(FloatCtx* fc, float w, float h, float y, float left, float right, bool isLeft) {
    /* a float is never placed above an earlier one */
    for (size_t i = 0; i < fc->floats.len; ++i) y = cscdMaxf(y, fc->floats.items[i].r.y);
    for (int guard = 0; guard < 1000; ++guard) {
        float l, r;
        fcBand(fc, y, h, left, right, &l, &r);
        bool any = false;
        for (size_t i = 0; i < fc->floats.len; ++i) {
            const FloatBox* f = &fc->floats.items[i];
            if (!(cscdRectBottom(f->r) <= y || f->r.y >= y + cscdMaxf(h, 0.01f))) any = true;
        }
        if (r - l >= w - 0.01f || !any) {
            CscdPoint p = {isLeft ? l : r - w, y};
            FloatBox fb = {cscdRect(p.x, p.y, w, h), isLeft, NULL, 0};
            cscdVecPush(&fc->floats, fb);
            return p;
        }
        /* move below the next float bottom */
        float next = 1e30f;
        for (size_t i = 0; i < fc->floats.len; ++i)
            if (cscdRectBottom(fc->floats.items[i].r) > y) next = cscdMinf(next, cscdRectBottom(fc->floats.items[i].r));
        if (next >= 1e29f) break;
        y = next;
    }
    CscdPoint p = {isLeft ? left : right - w, y};
    FloatBox fb = {cscdRect(p.x, p.y, w, h), isLeft, NULL, 0};
    cscdVecPush(&fc->floats, fb);
    return p;
}

static CscdComputedStyle* anonymousStyle(const CscdComputedStyle* p, CscdDisplay d) {
    CscdComputedStyle* s = cscdStyleClone(cscdInitialStyle());
    s->color = p->color;
    cscdStrVecCopy(&s->font.families, &p->font.families);
    cscdFontDescInherit(&s->font, &p->font);
    s->lineHeight = p->lineHeight; s->lineHeightFactor = p->lineHeightFactor;
    s->letterSpacing = p->letterSpacing; s->wordSpacing = p->wordSpacing; s->textAlign = p->textAlign; s->textIndent = p->textIndent;
    s->whiteSpace = p->whiteSpace; s->textTransform = p->textTransform; s->wordBreak = p->wordBreak; s->overflowWrap = p->overflowWrap;
    s->hyphens = p->hyphens;
    s->quotes = p->quotes;
    s->writingMode = p->writingMode;
    s->colorScheme = p->colorScheme;
    s->emptyCellsHide = p->emptyCellsHide;
    s->visibility = p->visibility; s->cursor = p->cursor; s->listStyle = p->listStyle; s->listInside = p->listInside;
    s->borderCollapse = p->borderCollapse; s->borderSpacingX = p->borderSpacingX; s->borderSpacingY = p->borderSpacingY;
    s->caretColor = p->caretColor; s->accentColor = p->accentColor; s->pointerEvents = p->pointerEvents; s->userSelect = p->userSelect;
    cscdVecCopy(&s->textShadow, &p->textShadow);
    cscdRelease(s->vars);
    s->vars = (CscdVars*)cscdRetain(p->vars);
    s->underline = p->underline; s->overline = p->overline; s->lineThrough = p->lineThrough;
    s->decorationColor = p->decorationColor; s->decorationStyle = p->decorationStyle; s->fill = p->fill; s->stroke = p->stroke;
    s->textOverflow = p->textOverflow; s->lineClamp = p->lineClamp;
    s->display = (uint8_t)d;
    s->originalDisplay = (uint8_t)d;
    return s;
}

static bool isCollapsibleWhitespace(const CscdLayoutBox* b) {
    if (b->kind != CSCD_BOX_TEXT) return false;
    uint8_t ws = b->style->whiteSpace;
    if (ws == CSCD_WS_PRE || ws == CSCD_WS_PRE_WRAP || ws == CSCD_WS_BREAK_SPACES) return false;
    for (size_t i = 0; i < b->text.len; ++i) if (!cscdIsSpace(b->text.ptr[i])) return false;
    return true;
}

static void roman(int n, bool upper, CscdStr* out) {
    static const struct { int v; const char* s; } t[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"}, {50, "l"},
                                                         {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
    size_t from = out->len;
    for (size_t i = 0; i < CSCD_COUNT(t); ++i) while (n >= t[i].v) { cscdStrAppendC(out, t[i].s); n -= t[i].v; }
    if (upper) for (size_t i = from; i < out->len; ++i) out->ptr[i] = cscdToUpper(out->ptr[i]);
}

static CscdStr markerText(const CscdComputedStyle* s, int ordinal) {
    CscdStr r = {0};
    uint8_t style = s->listStyle;
    /* outside what a style can write (roman numerals stop at 3999, letters start at 1): decimal */
    if (((style == CSCD_LIST_LOWER_ROMAN || style == CSCD_LIST_UPPER_ROMAN) && (ordinal < 1 || ordinal > 3999)) ||
        ((style == CSCD_LIST_LOWER_ALPHA || style == CSCD_LIST_UPPER_ALPHA) && ordinal < 1))
        style = CSCD_LIST_DECIMAL;
    switch (style) {
    case CSCD_LIST_NONE: break;
    case CSCD_LIST_DISC: cscdStrSetC(&r, "\xE2\x80\xA2 "); break;
    case CSCD_LIST_CIRCLE: cscdStrSetC(&r, "\xE2\x97\xA6 "); break;
    case CSCD_LIST_SQUARE: cscdStrSetC(&r, "\xE2\x96\xAA "); break;
    case CSCD_LIST_DECIMAL: cscdStrAppendf(&r, "%d. ", ordinal); break;
    case CSCD_LIST_LOWER_ALPHA: case CSCD_LIST_UPPER_ALPHA: {
        char buf[16];
        int k = 16, n = ordinal;
        while (n > 0 && k > 0) { --n; buf[--k] = (char)((style == CSCD_LIST_LOWER_ALPHA ? 'a' : 'A') + n % 26); n /= 26; }
        cscdStrAppend(&r, cscdSvMake(buf + k, (size_t)(16 - k)));
        cscdStrAppendC(&r, ". ");
        break;
    }
    case CSCD_LIST_LOWER_ROMAN: roman(ordinal, false, &r); cscdStrAppendC(&r, ". "); break;
    case CSCD_LIST_UPPER_ROMAN: roman(ordinal, true, &r); cscdStrAppendC(&r, ". "); break;
    }
    return r;
}

/* (bounded, so that list numbers counted on from it cannot overflow) */
static int attrInt(const CscdElement* e, const char* name) {
    char* c = cscdSvDupC(cscdElementAttrC(e, name));
    const long long n = strtoll(c, NULL, 10);
    cscdFree(c);
    return (int)(n < -1000000000LL ? -1000000000LL : n > 1000000000LL ? 1000000000LL : n);
}

static int listOrdinalFrom(Layouter* L, CscdElement* li) {
    if (cscdElementHasAttributeC(li, "value")) return attrInt(li, "value");
    int n = 0;
    for (CscdElement* s = cscdNodePreviousElementSibling(&li->node); s; s = cscdNodePreviousElementSibling(&s->node)) {
        if (s == L->lastListItem) return (int)cscdMinf(1e9f, (float)L->lastListOrdinal + n + 1);
        if (s->style && s->style->display == CSCD_DISPLAY_LIST_ITEM) {
            if (cscdElementHasAttributeC(s, "value")) return attrInt(s, "value") + n + 1;
            ++n;
        }
    }
    CscdElement* list = cscdNodeParentElement(&li->node);
    int start = 1;
    if (list && cscdElementIs(list, "ol") && cscdElementHasAttributeC(list, "start")) start = attrInt(list, "start");
    return start + n;
}

static int listOrdinal(Layouter* L, CscdElement* li) {
    const int n = listOrdinalFrom(L, li);
    L->lastListItem = li;
    L->lastListOrdinal = n;
    return n;
}

static uint32_t upCase(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xDF) return c;
    if (c >= 0x100 && c <= 0x17F && (c % 2 == 1)) return c - 1;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    return c;
}

static uint32_t lowCase(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x100 && c <= 0x17F && (c % 2 == 0)) return c + 1;
    if (c >= 0x391 && c <= 0x3A9) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    return c;
}

/* s with text-transform applied, into out (replaced) */
static void transformText(CscdSv s, CscdTextTransform t, bool* wordStart, CscdStr* out) {
    cscdStrClear(out);
    if (t == CSCD_TT_NONE) {
        cscdStrAppend(out, s);
        return;
    }
    cscdStrReserve(out, s.len);
    for (size_t i = 0; i < s.len;) {
        uint32_t cp = cscdUtf8Next(s, &i);
        bool letter = (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= 0xC0 && cp <= 0x24F);
        if (t == CSCD_TT_UPPERCASE) cp = upCase(cp);
        else if (t == CSCD_TT_LOWERCASE) cp = lowCase(cp);
        else if (t == CSCD_TT_CAPITALIZE) { if (*wordStart && letter) cp = upCase(cp); }
        *wordStart = !(letter || (cp >= '0' && cp <= '9') || cp == '\'');
        cscdUtf8Append(out, cp);
    }
}

static bool isCJK(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FFFF) || (cp >= 0x3000 && cp <= 0x303F);
}

static bool collapses(uint8_t ws) { return ws == CSCD_WS_NORMAL || ws == CSCD_WS_NOWRAP || ws == CSCD_WS_PRE_LINE; }
static bool wraps(uint8_t ws) { return ws != CSCD_WS_NOWRAP && ws != CSCD_WS_PRE; }

/* ---- box tree -------------------------------------------------------------------------------------- */

static CscdImage* loadImage(Layouter* L, CscdSv url) { return L->images ? L->images(L->imagesUser, url) : NULL; }

static CscdImage* imageOf(Layouter* L, CscdElement* e) {
    CscdSv src = cscdElementAttrC(e, "src");
    if (!src.len || !L->images) return NULL;
    CscdStr path = cscdJoinPath(e->node.owner ? cscdStrSv(&e->node.owner->baseDir) : CSCD_SV(""), src);
    CscdImage* img = loadImage(L, cscdStrSv(&path));
    cscdStrFree(&path);
    return img;
}

static void setReplaced(CscdLayoutBox* b, CscdReplacedKind k, bool inl) {
    b->kind = CSCD_BOX_REPLACED;
    b->replaced = (uint8_t)k;
    b->atomicInline = inl;
}

static void appendChild(CscdLayoutBox* parent, CscdLayoutBox* c) {
    c->parent = parent;
    cscdVecPush(&parent->children, c);
}

/* takes a reference to style */
static CscdLayoutBox* makeBox(Layouter* L, CscdElement* e, CscdComputedStyle* style) {
    CscdLayoutBox* b = cscdLayoutBoxNew();
    b->element = e;
    b->style = style;
    const CscdComputedStyle* s = style;
    switch (s->display) {
    case CSCD_DISPLAY_INLINE: b->kind = CSCD_BOX_INLINE; break;
    case CSCD_DISPLAY_INLINE_BLOCK: b->kind = CSCD_BOX_BLOCK; b->atomicInline = true; break;
    case CSCD_DISPLAY_FLEX: b->kind = CSCD_BOX_FLEX; break;
    case CSCD_DISPLAY_INLINE_FLEX: b->kind = CSCD_BOX_FLEX; b->atomicInline = true; break;
    case CSCD_DISPLAY_GRID: b->kind = CSCD_BOX_GRID; break;
    case CSCD_DISPLAY_INLINE_GRID: b->kind = CSCD_BOX_GRID; b->atomicInline = true; break;
    case CSCD_DISPLAY_TABLE: b->kind = CSCD_BOX_TABLE; break;
    case CSCD_DISPLAY_INLINE_TABLE: b->kind = CSCD_BOX_TABLE; b->atomicInline = true; break;
    case CSCD_DISPLAY_TABLE_ROW_GROUP: case CSCD_DISPLAY_TABLE_HEADER_GROUP: case CSCD_DISPLAY_TABLE_FOOTER_GROUP: b->kind = CSCD_BOX_TABLE_ROW_GROUP; break;
    case CSCD_DISPLAY_TABLE_ROW: b->kind = CSCD_BOX_TABLE_ROW; break;
    case CSCD_DISPLAY_TABLE_CELL: b->kind = CSCD_BOX_TABLE_CELL; break;
    default: b->kind = CSCD_BOX_BLOCK; break;
    }
    if (!e) return b;
    bool inl = s->display == CSCD_DISPLAY_INLINE || s->display == CSCD_DISPLAY_INLINE_BLOCK;
#define T(x) cscdElementIs(e, x)
    if (T("br")) { b->kind = CSCD_BOX_LINE_BREAK; return b; }
    if (T("img")) {
        setReplaced(b, CSCD_REPL_IMAGE, inl);
        b->image = imageOf(L, e);
    } else if (T("canvas")) setReplaced(b, CSCD_REPL_CANVAS, inl);
    else if (T("svg")) setReplaced(b, CSCD_REPL_SVG, inl);
    else if (T("video")) setReplaced(b, CSCD_REPL_VIDEO, inl);
    else if (T("iframe") || T("embed") || T("object")) setReplaced(b, CSCD_REPL_FRAME, inl);
    else if (T("textarea")) setReplaced(b, CSCD_REPL_TEXT_AREA, inl);
    else if (T("select")) setReplaced(b, CSCD_REPL_SELECT, inl);
    else if (T("progress")) setReplaced(b, CSCD_REPL_PROGRESS, inl);
    else if (T("meter")) setReplaced(b, CSCD_REPL_METER, inl);
    else if (T("input")) {
        CscdStr type = cscdLower(cscdElementHasAttributeC(e, "type") ? cscdElementAttrC(e, "type") : CSCD_SV("text"));
        CscdSv ty = cscdStrSv(&type);
        if (cscdSvEqC(ty, "checkbox")) setReplaced(b, CSCD_REPL_CHECKBOX, inl);
        else if (cscdSvEqC(ty, "radio")) setReplaced(b, CSCD_REPL_RADIO, inl);
        else if (cscdSvEqC(ty, "range")) setReplaced(b, CSCD_REPL_RANGE, inl);
        else if (cscdSvEqC(ty, "color")) setReplaced(b, CSCD_REPL_COLOR_INPUT, inl);
        else if (cscdSvEqC(ty, "button") || cscdSvEqC(ty, "submit") || cscdSvEqC(ty, "reset")) {
            b->kind = CSCD_BOX_BLOCK;
            b->atomicInline = inl;
            CscdLayoutBox* tb = cscdLayoutBoxNew();
            tb->kind = CSCD_BOX_TEXT;
            tb->style = (CscdComputedStyle*)cscdRetain(style);
            tb->element = e;
            if (cscdElementHasAttributeC(e, "value")) tb->text = cscdStrDup(cscdElementAttrC(e, "value"));
            else tb->text = cscdStrDupC(cscdSvEqC(ty, "submit") ? "Submit" : cscdSvEqC(ty, "reset") ? "Reset" : "");
            appendChild(b, tb);
            b->inlineContent = true;
        } else if (cscdSvEqC(ty, "image")) {
            setReplaced(b, CSCD_REPL_IMAGE, inl);
            b->image = imageOf(L, e);
        } else setReplaced(b, CSCD_REPL_TEXT_INPUT, inl);
        cscdStrFree(&type);
    }
#undef T
    if (b->kind == CSCD_BOX_REPLACED && s->display == CSCD_DISPLAY_BLOCK) b->atomicInline = false;
    if (b->kind == CSCD_BOX_REPLACED && (s->display == CSCD_DISPLAY_FLEX || s->display == CSCD_DISPLAY_GRID || s->display == CSCD_DISPLAY_LIST_ITEM)) b->atomicInline = false;
    return b;
}

static void addGenerated(Layouter* L, CscdElement* e, CscdLayoutBox* parent, const char* pseudo) {
    CscdComputedStyle* ps = cscdStyleEnginePseudoStyle(L->styles, e, cscdSv(pseudo));
    if (!ps || ps->display == CSCD_DISPLAY_NONE) return;
    if (!ps->hasContent) return;
    CscdLayoutBox* b = makeBox(L, NULL, (CscdComputedStyle*)cscdRetain(ps));
    b->element = e;
    b->pseudo = cscdStrDupC(pseudo);
    if (ps->contentImage.len) {
        b->kind = CSCD_BOX_REPLACED;
        b->replaced = CSCD_REPL_IMAGE;
        b->atomicInline = ps->display == CSCD_DISPLAY_INLINE || ps->display == CSCD_DISPLAY_INLINE_BLOCK;
        b->image = loadImage(L, cscdStrSv(&ps->contentImage));
    }
    /* attr() and counter() references (the pseudo element's own counters count first) */
    layApplyCounters(L, ps, e);
    CscdStr text = layResolveContent(L, cscdStrSv(&ps->content), e);
    if (text.len && b->kind != CSCD_BOX_REPLACED) {
        CscdLayoutBox* t = cscdLayoutBoxNew();
        t->kind = CSCD_BOX_TEXT;
        t->style = (CscdComputedStyle*)cscdRetain(ps);
        t->element = e;
        t->pseudo = cscdStrDupC(pseudo);
        t->text = text;
        appendChild(b, t);
    } else cscdStrFree(&text);
    b->parent = parent;
    layFixupChildren(L, b);
    cscdVecPush(&parent->children, b);
}

static CounterStack* counterStack(Layouter* L, CscdSv name) {
    bool inserted = false;
    CscdMapVal* v = cscdStrMapPut(&L->counters, name, &inserted);
    if (inserted) v->p = CSCD_NEW(CounterStack);
    return (CounterStack*)v->p;
}

void layApplyCounters(Layouter* L, const CscdComputedStyle* s, const CscdElement* scope) {
    for (size_t i = 0; i < s->counterReset.len; ++i) {
        CounterInstance ci = {scope, s->counterReset.items[i].value};
        cscdVecPush(counterStack(L, cscdStrSv(&s->counterReset.items[i].name)), ci);
    }
    for (size_t i = 0; i < s->counterSet.len; ++i) {
        CounterStack* st = counterStack(L, cscdStrSv(&s->counterSet.items[i].name));
        if (!st->len) { CounterInstance ci = {scope, s->counterSet.items[i].value}; cscdVecPush(st, ci); }
        else cscdVecLast(st).value = s->counterSet.items[i].value;
    }
    for (size_t i = 0; i < s->counterIncrement.len; ++i) {
        CounterStack* st = counterStack(L, cscdStrSv(&s->counterIncrement.items[i].name));
        if (!st->len) { CounterInstance ci = {scope, 0}; cscdVecPush(st, ci); } /* (an increment without a reset makes one) */
        /* (stopping at the ends of an int, as browsers do) */
        const int64_t sum = (int64_t)cscdVecLast(st).value + s->counterIncrement.items[i].value;
        cscdVecLast(st).value = sum > INT_MAX ? INT_MAX : sum < INT_MIN ? INT_MIN : (int)sum;
    }
}

void layEndCounterScope(Layouter* L, const CscdElement* scope) {
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&L->counters, s));) {
        CounterStack* st = (CounterStack*)s->val.p;
        while (st->len && cscdVecLast(st).scope == scope) st->len--;
    }
}

static void counterText(int n, CscdSv style, CscdStr* out) {
    if (cscdSvEqC(style, "none")) return;
    if (cscdSvEqC(style, "disc")) { cscdStrAppendC(out, "\xE2\x80\xA2"); return; }
    if (cscdSvEqC(style, "circle")) { cscdStrAppendC(out, "\xE2\x97\xA6"); return; }
    if (cscdSvEqC(style, "square")) { cscdStrAppendC(out, "\xE2\x96\xAA"); return; }
    if (cscdSvEqC(style, "decimal-leading-zero")) { cscdStrAppendf(out, "%s%d", n >= 0 && n < 10 ? "0" : "", n); return; }
    if (n > 0 && (cscdSvEqC(style, "lower-alpha") || cscdSvEqC(style, "lower-latin") || cscdSvEqC(style, "upper-alpha") || cscdSvEqC(style, "upper-latin"))) {
        char buf[16];
        int k = 16;
        for (int v = n; v > 0 && k > 0; v /= 26) { --v; buf[--k] = (char)((style.ptr[0] == 'l' ? 'a' : 'A') + v % 26); }
        cscdStrAppend(out, cscdSvMake(buf + k, (size_t)(16 - k)));
        return;
    }
    if (n > 0 && n <= 3999 && (cscdSvEqC(style, "lower-roman") || cscdSvEqC(style, "upper-roman"))) { roman(n, style.ptr[0] == 'u', out); return; }
    cscdStrAppendf(out, "%d", n);
}

/* The text of a content value: attr() and counter() references resolved. */
CscdStr layResolveContent(Layouter* L, CscdSv c, CscdElement* e) {
    CscdStr text = {0};
    for (size_t i = 0; i < c.len;) {
        if (c.ptr[i] != '\x01') { cscdStrAppendChar(&text, c.ptr[i++]); continue; }
        const size_t end = cscdSvFindChar(c, '\x02', i);
        if (end == CSCD_NPOS) break;
        const CscdSv ref = cscdSvSub(c, i + 1, end - i - 1);
        i = end + 1;
        if (cscdSvStartsC(ref, "attr:")) { cscdStrAppend(&text, cscdElementAttr(e, cscdSvFrom(ref, 5))); continue; }
        if (cscdSvEqC(ref, "open-quote") || cscdSvEqC(ref, "close-quote") || cscdSvEqC(ref, "no-open-quote") || cscdSvEqC(ref, "no-close-quote")) {
            const bool open = cscdSvEqC(ref, "open-quote") || cscdSvEqC(ref, "no-open-quote");
            const bool shown = cscdSvEqC(ref, "open-quote") || cscdSvEqC(ref, "close-quote");
            if (!open) {
                if (!L->quoteDepth) continue; /* (a close-quote with none open shows nothing) */
                --L->quoteDepth;
            }
            const char* q = e && e->style && e->style->quotes ? e->style->quotes : "\xE2\x80\x9C\0\xE2\x80\x9D\0\xE2\x80\x98\0\xE2\x80\x99\0";
            /* the pair for the depth (the last pair deeper down) */
            const char* pair = NULL;
            for (int level = 0; *q; ++level) {
                const char* close = q + strlen(q) + 1;
                pair = q;
                if (level == L->quoteDepth) break;
                q = close + strlen(close) + 1;
            }
            if (shown && pair) cscdStrAppendC(&text, open ? pair : pair + strlen(pair) + 1);
            if (open) ++L->quoteDepth;
            continue;
        }
        const bool all = cscdSvStartsC(ref, "counters:");
        if (!all && !cscdSvStartsC(ref, "counter:")) continue;
        const CscdSv rest = cscdSvFrom(ref, all ? 9 : 8);
        const size_t a = cscdSvFindChar(rest, ':', 0);
        const CscdSv name = cscdSvSub(rest, 0, a);
        CscdSv style = a == CSCD_NPOS ? CSCD_SV("decimal") : cscdSvFrom(rest, a + 1), sep = CSCD_SV("");
        if (all) {
            const size_t b = cscdSvFindChar(style, ':', 0);
            if (b != CSCD_NPOS) { sep = cscdSvFrom(style, b + 1); style = cscdSvSub(style, 0, b); }
        }
        CscdMapVal* v = cscdStrMapGet(&L->counters, name);
        CounterStack* st = v ? (CounterStack*)v->p : NULL;
        if (!st || !st->len) { counterText(0, style, &text); continue; }
        if (!all) { counterText(cscdVecLast(st).value, style, &text); continue; }
        for (size_t k = 0; k < st->len; ++k) {
            if (k) cscdStrAppend(&text, sep);
            counterText(st->items[k].value, style, &text);
        }
    }
    return text;
}

static void buildChildren(Layouter* L, CscdElement* e, CscdLayoutBox* parent) {
    if (e->style && e->style->contentVisibility == 2) return; /* (content-visibility: hidden: none of its contents) */
    /* list marker */
    if (e->style && e->style->display == CSCD_DISPLAY_LIST_ITEM && e->style->listStyle != CSCD_LIST_NONE && !cscdElementIs(e, "summary")) {
        CscdComputedStyle* ms = cscdStyleEnginePseudoStyle(L->styles, e, CSCD_SV("marker"));
        CscdStr mt = markerText(ms ? ms : e->style, listOrdinal(L, e));
        if (ms && ms->hasContent) {
            cscdStrFree(&mt);
            mt = layResolveContent(L, cscdStrSv(&ms->content), e);
        }
        if (mt.len) {
            CscdLayoutBox* m = cscdLayoutBoxNew();
            m->kind = CSCD_BOX_TEXT;
            m->isMarker = !e->style->listInside;
            m->style = (CscdComputedStyle*)cscdRetain(ms ? ms : e->style);
            m->element = e;
            m->pseudo = cscdStrDupC("marker");
            m->text = mt;
            appendChild(parent, m);
        } else cscdStrFree(&mt);
    }
    if (cscdElementIs(e, "summary") && e->style && e->style->display == CSCD_DISPLAY_LIST_ITEM) {
        CscdElement* det = cscdNodeParentElement(&e->node);
        bool open = det && cscdElementHasAttributeC(det, "open");
        CscdLayoutBox* m = cscdLayoutBoxNew();
        m->kind = CSCD_BOX_TEXT;
        m->style = (CscdComputedStyle*)cscdRetain(e->style);
        m->element = e;
        m->pseudo = cscdStrDupC("marker");
        m->text = cscdStrDupC(open ? "\xE2\x96\xBE " : "\xE2\x96\xB8 ");
        appendChild(parent, m);
    }
    addGenerated(L, e, parent, "before");
    for (size_t i = 0; i < e->node.children.len; ++i) {
        CscdNode* cn = cscdNodeChild(&e->node, i);
        CscdElement* ce = cscdNodeAsElement(cn);
        if (ce) {
            if (!ce->style) continue;
            if (ce->style->display == CSCD_DISPLAY_NONE) { ce->layout = NULL; continue; }
            layApplyCounters(L, ce->style, e);
            if (ce->style->display == CSCD_DISPLAY_CONTENTS) {
                ce->layout = NULL;
                buildChildren(L, ce, parent);
                continue;
            }
            CscdLayoutBox* b = makeBox(L, ce, (CscdComputedStyle*)cscdRetain(ce->style));
            ce->layout = b;
            b->parent = parent;
            if (b->kind != CSCD_BOX_REPLACED && b->kind != CSCD_BOX_LINE_BREAK && !cscdElementIs(ce, "input")) buildChildren(L, ce, b);
            layFixupChildren(L, b);
            cscdVecPush(&parent->children, b);
        } else if (cscdNodeIsText(cn)) {
            CscdText* tn = (CscdText*)cn;
            if (!tn->data.len) continue;
            CscdLayoutBox* b = cscdLayoutBoxNew();
            b->kind = CSCD_BOX_TEXT;
            b->textNode = tn;
            b->text = cscdStrCopy(&tn->data);
            b->style = (CscdComputedStyle*)cscdRetain(e->style);
            b->element = e;
            appendChild(parent, b);
        }
    }
    addGenerated(L, e, parent, "after");
    layEndCounterScope(L, e); /* (the counters its children reset) */
}

void layFixupChildren(Layouter* L, CscdLayoutBox* b) {
    (void)L;
    CscdPtrVec* ch = &b->children;
    if (b->kind == CSCD_BOX_TEXT || b->kind == CSCD_BOX_REPLACED || b->kind == CSCD_BOX_LINE_BREAK) return;
    if (b->kind == CSCD_BOX_INLINE) {
        /* an inline containing blocks becomes a block (simplified block-in-inline) */
        bool block = false;
        for (size_t i = 0; i < ch->len; ++i) {
            CscdLayoutBox* c = cscdBoxChild(b, i);
            block = block || (layIsBlockLevel(c) && !layIsOutOfFlow(c) && !c->isMarker);
        }
        if (!block) return;
        b->kind = CSCD_BOX_BLOCK;
    }
    if (b->kind == CSCD_BOX_TABLE || b->kind == CSCD_BOX_TABLE_ROW_GROUP || b->kind == CSCD_BOX_TABLE_ROW) {
        size_t w = 0;
        for (size_t i = 0; i < ch->len; ++i) {
            CscdLayoutBox* c = cscdBoxChild(b, i);
            if (c->kind == CSCD_BOX_TEXT) cscdLayoutBoxFree(c);
            else ch->items[w++] = c;
        }
        ch->len = w;
        if (b->kind == CSCD_BOX_TABLE_ROW) {
            /* anything that is not a cell gets an anonymous cell */
            for (size_t i = 0; i < ch->len; ++i) {
                CscdLayoutBox* c = cscdBoxChild(b, i);
                if (c->kind == CSCD_BOX_TABLE_CELL) continue;
                CscdLayoutBox* cell = cscdLayoutBoxNew();
                cell->kind = CSCD_BOX_TABLE_CELL;
                cell->anonymous = true;
                cell->style = anonymousStyle(b->style, CSCD_DISPLAY_TABLE_CELL);
                cell->element = b->element;
                cell->parent = b;
                appendChild(cell, c);
                layFixupChildren(L, cell);
                ch->items[i] = cell;
            }
        }
        return;
    }
    bool flexOrGrid = b->kind == CSCD_BOX_FLEX || b->kind == CSCD_BOX_GRID;
    bool anyBlock = false;
    for (size_t i = 0; i < ch->len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(b, i);
        if (!c->isMarker && !layIsOutOfFlow(c) && layIsBlockLevel(c)) anyBlock = true;
    }
    if (!flexOrGrid && !anyBlock) {
        bool any = false;
        for (size_t i = 0; i < ch->len; ++i) if (!cscdBoxChild(b, i)->isMarker) any = true;
        b->inlineContent = any;
        return;
    }
    /* wrap runs of inline-level content in anonymous blocks */
    CscdPtrVec out = {0}, run = {0};
#define FLUSH() do { \
        if (run.len) { \
            bool allWs = true; \
            for (size_t r = 0; r < run.len; ++r) { \
                CscdLayoutBox* rb = (CscdLayoutBox*)run.items[r]; \
                if (!(isCollapsibleWhitespace(rb) || (layIsOutOfFlow(rb) && !flexOrGrid))) allWs = false; \
            } \
            if (allWs) { \
                /* keep the out-of-flow boxes, drop whitespace */ \
                for (size_t r = 0; r < run.len; ++r) { \
                    CscdLayoutBox* rb = (CscdLayoutBox*)run.items[r]; \
                    if (layIsOutOfFlow(rb)) cscdVecPush(&out, rb); \
                    else cscdLayoutBoxFree(rb); \
                } \
            } else { \
                CscdLayoutBox* anon = cscdLayoutBoxNew(); \
                anon->kind = CSCD_BOX_BLOCK; \
                anon->anonymous = true; \
                anon->style = anonymousStyle(b->style, CSCD_DISPLAY_BLOCK); \
                anon->element = b->element; \
                anon->parent = b; \
                for (size_t r = 0; r < run.len; ++r) appendChild(anon, (CscdLayoutBox*)run.items[r]); \
                anon->inlineContent = true; \
                cscdVecPush(&out, anon); \
            } \
            run.len = 0; \
        } \
    } while (0)
    for (size_t i = 0; i < ch->len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(b, i);
        if (c->isMarker) { cscdVecPush(&out, c); continue; }
        bool inlineLevel = !layIsBlockLevel(c);
        if (flexOrGrid) {
            if (c->kind == CSCD_BOX_TEXT || c->kind == CSCD_BOX_INLINE || c->kind == CSCD_BOX_LINE_BREAK) { cscdVecPush(&run, c); continue; }
            FLUSH();
            cscdVecPush(&out, c);
            continue;
        }
        if (inlineLevel || (layIsOutOfFlow(c) && run.len)) { cscdVecPush(&run, c); continue; }
        FLUSH();
        cscdVecPush(&out, c);
    }
    FLUSH();
#undef FLUSH
    cscdVecFree(ch);
    *ch = out;
    cscdVecFree(&run);
    for (size_t i = 0; i < ch->len; ++i) cscdBoxChild(b, i)->parent = b;
}

static void clearLayoutPointers(CscdNode* n) {
    CscdElement* e = cscdNodeAsElement(n);
    if (e) e->layout = NULL;
    for (size_t i = 0; i < n->children.len; ++i) clearLayoutPointers(cscdNodeChild(n, i));
}

void cscdLayoutForget(CscdDocument* doc) { clearLayoutPointers(&doc->node); }

static void clearCounters(Layouter* L) {
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&L->counters, s));) {
        cscdVecFree((CounterStack*)s->val.p);
        cscdFree(s->val.p);
    }
    cscdStrMapClear(&L->counters);
}

CscdLayoutBox* layBuild(Layouter* L, CscdDocument* doc) {
    clearLayoutPointers(&doc->node);
    CscdElement* html = cscdDocumentElement(doc);
    if (!html || !html->style || html->style->display == CSCD_DISPLAY_NONE) return NULL;
    CscdLayoutBox* root = makeBox(L, html, (CscdComputedStyle*)cscdRetain(html->style));
    root->atomicInline = false;
    if (root->kind == CSCD_BOX_INLINE) root->kind = CSCD_BOX_BLOCK;
    html->layout = root;
    clearCounters(L);
    L->lastListItem = NULL;
    layApplyCounters(L, html->style, NULL);
    buildChildren(L, html, root);
    layFixupChildren(L, root);
    return root;
}

/* ---- sizes ------------------------------------------------------------------------------------------ */

void layResolveEdges(CscdLayoutBox* b, float cbW) {
    const CscdComputedStyle* s = b->style;
    for (int i = 0; i < 4; ++i) {
        b->padding[i] = cscdMaxf(0.f, cscdLenResolve(s->padding[i], cbW, 0));
        b->border[i] = s->border[i].width;
        b->margin[i] = cscdLenIsAuto(s->margin[i]) ? 0 : cscdLenResolve(s->margin[i], cbW, 0);
    }
    if (b->kind == CSCD_BOX_TEXT) for (int i = 0; i < 4; ++i) b->padding[i] = b->border[i] = b->margin[i] = 0;
}

float layMarginsFixed(CscdLayoutBox* b) {
    const CscdComputedStyle* s = b->style;
    float m = 0;
    if (!cscdLenIsAuto(s->margin[1]) && !cscdLenHasPercent(s->margin[1])) m += s->margin[1].px;
    if (!cscdLenIsAuto(s->margin[3]) && !cscdLenHasPercent(s->margin[3])) m += s->margin[3].px;
    return m;
}

float laySpecifiedWidth(Layouter* L, CscdLayoutBox* b, float cbW, bool* definite) {
    const CscdComputedStyle* s = b->style;
    *definite = false;
    const CscdLength w = s->width;
    if (cscdLenIsAuto(w) || cscdLenIsNone(w)) return 0;
    if (cscdLenIsIntrinsic(w)) {
        float v = w.kind == CSCD_LEN_MIN_CONTENT ? layMinContent(L, b) : w.kind == CSCD_LEN_MAX_CONTENT ? layMaxContent(L, b)
                : cscdMinf(layMaxContent(L, b), cscdMaxf(layMinContent(L, b), cbW - layMarginsFixed(b)));
        *definite = true;
        return v;
    }
    if (cscdLenHasPercent(w) && cbW < 0) return 0;
    float v = cscdLenResolve(w, cscdMaxf(0.f, cbW), 0);
    *definite = true;
    if (s->boxSizing == CSCD_BOX_CONTENT) v += b->padding[1] + b->padding[3] + b->border[1] + b->border[3];
    else v = cscdMaxf(v, b->padding[1] + b->padding[3] + b->border[1] + b->border[3]);
    return v;
}

float laySpecifiedHeight(CscdLayoutBox* b, float cbH, bool* definite) {
    const CscdComputedStyle* s = b->style;
    *definite = false;
    const CscdLength h = s->height;
    if (cscdLenIsAuto(h) || cscdLenIsNone(h) || cscdLenIsIntrinsic(h)) return 0;
    if (cscdLenHasPercent(h) && cbH < 0) return 0;
    float v = cscdLenResolve(h, cscdMaxf(0.f, cbH), 0);
    *definite = true;
    if (s->boxSizing == CSCD_BOX_CONTENT) v += b->padding[0] + b->padding[2] + b->border[0] + b->border[2];
    else v = cscdMaxf(v, b->padding[0] + b->padding[2] + b->border[0] + b->border[2]);
    return v;
}

float layClampWidth(Layouter* L, CscdLayoutBox* b, float w, float cbW) {
    const CscdComputedStyle* s = b->style;
    float edges = b->padding[1] + b->padding[3] + b->border[1] + b->border[3];
    float adj = s->boxSizing == CSCD_BOX_CONTENT ? edges : 0;
    if (!cscdLenIsNone(s->maxWidth) && !(cscdLenHasPercent(s->maxWidth) && cbW < 0)) {
        float mx = cscdLenIsIntrinsic(s->maxWidth) ? (s->maxWidth.kind == CSCD_LEN_MIN_CONTENT ? layMinContent(L, b) : layMaxContent(L, b))
                                                 : cscdLenResolve(s->maxWidth, cscdMaxf(0.f, cbW), 0) + adj;
        w = cscdMinf(w, mx);
    }
    if (!cscdLenIsAuto(s->minWidth) && !(cscdLenHasPercent(s->minWidth) && cbW < 0)) {
        float mn = cscdLenIsIntrinsic(s->minWidth) ? (s->minWidth.kind == CSCD_LEN_MIN_CONTENT ? layMinContent(L, b) : layMaxContent(L, b))
                                                 : cscdLenResolve(s->minWidth, cscdMaxf(0.f, cbW), 0) + adj;
        w = cscdMaxf(w, mn);
    }
    return cscdMaxf(w, edges);
}

float layClampHeight(CscdLayoutBox* b, float h, float cbH) {
    const CscdComputedStyle* s = b->style;
    float edges = b->padding[0] + b->padding[2] + b->border[0] + b->border[2];
    float adj = s->boxSizing == CSCD_BOX_CONTENT ? edges : 0;
    if (!cscdLenIsNone(s->maxHeight) && !cscdLenIsIntrinsic(s->maxHeight) && !(cscdLenHasPercent(s->maxHeight) && cbH < 0))
        h = cscdMinf(h, cscdLenResolve(s->maxHeight, cscdMaxf(0.f, cbH), 0) + adj);
    if (!cscdLenIsAuto(s->minHeight) && !cscdLenIsIntrinsic(s->minHeight) && !(cscdLenHasPercent(s->minHeight) && cbH < 0))
        h = cscdMaxf(h, cscdLenResolve(s->minHeight, cscdMaxf(0.f, cbH), 0) + adj);
    return cscdMaxf(h, edges);
}

/* The widths of the texts measured, kept from one layout to the next, by the font, the spacing and
 * the text (a face lives as long as the program, so its address names it). */
static CSCD_THREAD_LOCAL CscdStrMap widthCache;
static CSCD_THREAD_LOCAL CscdStr widthKey;

float layMeasure(const CscdFont* f, CscdSv s, float ls, float ws) {
    if (!s.len) return 0;
    const uint8_t flags = (uint8_t)((f->fakeBold ? 1 : 0) | (f->fakeItalic ? 2 : 0));
    cscdStrClear(&widthKey);
    cscdStrAppend(&widthKey, cscdSvMake((const char*)&f->face, sizeof(void*)));
    cscdStrAppend(&widthKey, cscdSvMake((const char*)&f->size, sizeof(float)));
    cscdStrAppend(&widthKey, cscdSvMake((const char*)&ls, sizeof(float)));
    cscdStrAppend(&widthKey, cscdSvMake((const char*)&ws, sizeof(float)));
    cscdStrAppendChar(&widthKey, (char)flags);
    cscdStrAppend(&widthKey, s);
    CscdMapVal* v = cscdStrMapGet(&widthCache, cscdStrSv(&widthKey));
    if (v) return v->f[0];
    float w = cscdFontMeasure(s, f, ls, ws);
    if (widthCache.len > 200000) cscdStrMapClear(&widthCache); /* (a bound: start again) */
    cscdStrMapPut(&widthCache, cscdStrSv(&widthKey), NULL)->f[0] = w;
    return w;
}

static float attrFloat(const CscdElement* e, const char* name) {
    char* c = cscdSvDupC(cscdElementAttrC(e, name));
    float v = (float)atof(c);
    cscdFree(c);
    return v;
}

void layReplacedIntrinsic(Layouter* L, CscdLayoutBox* b, float* w, float* h, float* ratio) {
    *w = 300; *h = 150; *ratio = 2;
    CscdElement* e = b->element;
    switch (b->replaced) {
    case CSCD_REPL_IMAGE:
        if (b->image && !cscdImageEmpty(b->image)) {
            const float z = b->style && b->style->effectiveZoom > 0 ? b->style->effectiveZoom : 1;
            *w = (float)b->image->width * z;
            *h = (float)b->image->height * z;
            *ratio = *w / *h;
        } else { *w = 0; *h = 0; *ratio = 0; }
        break;
    case CSCD_REPL_CANVAS:
        if (e) {
            if (cscdElementHasAttributeC(e, "width")) *w = attrFloat(e, "width");
            if (cscdElementHasAttributeC(e, "height")) *h = attrFloat(e, "height");
        }
        *ratio = *h > 0 ? *w / *h : 0;
        break;
    case CSCD_REPL_SVG: {
        float vbw = 0, vbh = 0;
        if (e && cscdElementHasAttributeC(e, "viewbox")) {
            CscdStr vb = cscdStrDup(cscdElementAttrC(e, "viewbox"));
            for (size_t i = 0; i < vb.len; ++i) if (vb.ptr[i] == ',') vb.ptr[i] = ' ';
            CscdSvVec parts = {0};
            cscdSplitSpaces(cscdStrSv(&vb), &parts);
            float n[4];
            size_t k = 0;
            for (size_t i = 0; i < parts.len && k < 4; ++i) {
                char* c = cscdSvDupC(parts.items[i]);
                n[k++] = (float)atof(c);
                cscdFree(c);
            }
            if (k >= 4) { vbw = n[2]; vbh = n[3]; }
            cscdVecFree(&parts);
            cscdStrFree(&vb);
        }
        if (vbw > 0 && vbh > 0) { *ratio = vbw / vbh; *w = 300; *h = 300 / *ratio; }
        bool hw = e && cscdElementHasAttributeC(e, "width"), hh = e && cscdElementHasAttributeC(e, "height");
        if (hw && cscdSvFindChar(cscdElementAttrC(e, "width"), '%', 0) == CSCD_NPOS) *w = attrFloat(e, "width");
        if (hh && cscdSvFindChar(cscdElementAttrC(e, "height"), '%', 0) == CSCD_NPOS) *h = attrFloat(e, "height");
        if (hw && !hh && *ratio > 0) *h = *w / *ratio;
        if (hh && !hw && *ratio > 0) *w = *h * *ratio;
        if (*ratio <= 0 && *h > 0) *ratio = *w / *h;
        break;
    }
    case CSCD_REPL_SELECT: {
        CscdFont f = cscdFontForStyle(b->style);
        float maxW = 0;
        if (e) {
            CscdPtrVec opts = {0};
            cscdElementQuerySelectorAll(e, CSCD_SV("option"), &opts);
            for (size_t i = 0; i < opts.len; ++i) {
                CscdStr t = {0};
                cscdNodeTextContent(opts.items[i], &t);
                maxW = cscdMaxf(maxW, layMeasure(&f, cscdSvTrim(cscdStrSv(&t)), 0, 0));
                cscdStrFree(&t);
            }
            cscdVecFree(&opts);
        }
        *w = maxW + 24;
        *h = ceilf(cscdFontAscent(&f) + cscdFontDescent(&f)) + 2;
        *ratio = 0;
        break;
    }
    case CSCD_REPL_TEXT_INPUT: {
        CscdFont f = cscdFontForStyle(b->style);
        *w = 150;
        *h = ceilf(cscdFontAscent(&f) + cscdFontDescent(&f)) + 2;
        *ratio = 0;
        break;
    }
    case CSCD_REPL_TEXT_AREA: *w = 180; *h = 36; *ratio = 0; break;
    case CSCD_REPL_CHECKBOX: case CSCD_REPL_RADIO: *w = *h = 13; *ratio = 1; break;
    case CSCD_REPL_RANGE: *w = 129; *h = 16; *ratio = 0; break;
    case CSCD_REPL_PROGRESS: *w = 160; *h = 16; *ratio = 0; break;
    case CSCD_REPL_METER: *w = 80; *h = 16; *ratio = 0; break;
    case CSCD_REPL_COLOR_INPUT: *w = 44; *h = 23; *ratio = 0; break;
    default: break;
    }
    (void)L;
}

void layReplaced(Layouter* L, CscdLayoutBox* b, float cbW, float cbH, float* cw, float* ch) {
    float iw, ih, ratio;
    layReplacedIntrinsic(L, b, &iw, &ih, &ratio);
    if (b->style->aspectRatio > 0) ratio = b->style->aspectRatio;
    float edgesW = b->padding[1] + b->padding[3] + b->border[1] + b->border[3];
    float edgesH = b->padding[0] + b->padding[2] + b->border[0] + b->border[2];
    bool wd, hd;
    float sw = laySpecifiedWidth(L, b, cbW, &wd), sh = laySpecifiedHeight(b, cbH, &hd);
    if (wd) sw -= edgesW;
    if (hd) sh -= edgesH;
    if (wd && hd) { *cw = sw; *ch = sh; }
    else if (wd) { *cw = sw; *ch = ratio > 0 ? sw / ratio : ih; }
    else if (hd) { *ch = sh; *cw = ratio > 0 ? sh * ratio : iw; }
    else { *cw = iw; *ch = ih; }
    /* min / max (content sizes) */
    float bw = layClampWidth(L, b, *cw + edgesW, cbW) - edgesW;
    if (bw != *cw) { if (!hd && ratio > 0) *ch = bw / ratio; *cw = bw; }
    float bh = layClampHeight(b, *ch + edgesH, cbH) - edgesH;
    if (bh != *ch) { if (!wd && ratio > 0) *cw = bh * ratio; *ch = bh; }
    *cw = cscdMaxf(0.f, *cw);
    *ch = cscdMaxf(0.f, *ch);
}

static bool isAbsPos(const CscdLayoutBox* c) { return c->style && (c->style->position == CSCD_POS_ABSOLUTE || c->style->position == CSCD_POS_FIXED); }

static float intrinsicWidth(Layouter* L, CscdLayoutBox* b, bool min);

/* A vertical box's width as content: how deep its lines go, laid out at the viewport's height. */
static float verticalIntrinsicWidth(Layouter* L, CscdLayoutBox* b) {
    BlockInput in = blockInput();
    in.cbW = 1e6f;
    in.availW = 1e6f;
    in.shrinkToFit = true;
    layBlock(L, b, &in);
    return b->w;
}

static float intrinsicWidth(Layouter* L, CscdLayoutBox* b, bool min) {
    float v = 0;
    const CscdComputedStyle* s = b->style;
    layResolveEdges(b, 0);
    float edges = b->padding[1] + b->padding[3] + b->border[1] + b->border[3];
    if (cscdLenIsFixed(s->width) && !cscdLenIsAuto(s->width) && b->kind != CSCD_BOX_INLINE && b->kind != CSCD_BOX_TEXT) {
        v = s->boxSizing == CSCD_BOX_CONTENT ? s->width.px + edges : cscdMaxf(s->width.px, edges);
    } else if (b->kind == CSCD_BOX_REPLACED) {
        float cw, ch;
        layReplaced(L, b, -1, -1, &cw, &ch);
        v = cw + edges;
        if (min && (cscdLenHasPercent(s->width) || cscdLenHasPercent(s->maxWidth))) v = edges; /* percentage-sized replaced boxes can shrink */
    } else if (s->containerType || (s->contain & (CSCD_CONTAIN_SIZE | CSCD_CONTAIN_INLINE_SIZE))) v = edges; /* (size containment: as if it were empty) */
    else if (s->verticalRoot && b->kind != CSCD_BOX_INLINE && b->kind != CSCD_BOX_TEXT) v = verticalIntrinsicWidth(L, b);
    else if (b->kind == CSCD_BOX_FLEX) v = layIntrinsicFlex(L, b, min) + edges;
    else if (b->kind == CSCD_BOX_GRID) v = layIntrinsicGrid(L, b, min) + edges;
    else if (b->kind == CSCD_BOX_TABLE) v = layIntrinsicTable(L, b, min) + edges;
    else if (b->inlineContent || b->kind == CSCD_BOX_INLINE) v = layIntrinsicInline(L, b, min) + edges;
    else {
        float m = 0, floatsSum = 0;
        for (size_t i = 0; i < b->children.len; ++i) {
            CscdLayoutBox* c = cscdBoxChild(b, i);
            if (c->isMarker || isAbsPos(c)) continue;
            if (min) { m = cscdMaxf(m, layOuterMinContent(L, c)); continue; }
            if (c->style && c->style->floating != CSCD_FLOAT_NONE) { floatsSum += layOuterMaxContent(L, c); continue; }
            m = cscdMaxf(m, layOuterMaxContent(L, c));
        }
        v = (min ? m : cscdMaxf(m, floatsSum)) + edges;
    }
    if (b->kind != CSCD_BOX_INLINE && b->kind != CSCD_BOX_TEXT) {
        if (!cscdLenIsNone(s->maxWidth) && cscdLenIsFixed(s->maxWidth)) v = cscdMinf(v, s->maxWidth.px + (s->boxSizing == CSCD_BOX_CONTENT ? edges : 0));
        if (!cscdLenIsAuto(s->minWidth) && cscdLenIsFixed(s->minWidth)) v = cscdMaxf(v, s->minWidth.px + (s->boxSizing == CSCD_BOX_CONTENT ? edges : 0));
    }
    return v;
}

float layMinContent(Layouter* L, CscdLayoutBox* b) {
    if (b->minContent >= 0) return b->minContent;
    float v = intrinsicWidth(L, b, true);
    b->minContent = v;
    return v;
}

float layMaxContent(Layouter* L, CscdLayoutBox* b) {
    if (b->maxContent >= 0) return b->maxContent;
    float v = intrinsicWidth(L, b, false);
    v = cscdMaxf(v, layMinContent(L, b));
    b->maxContent = v;
    return v;
}

/* ---- inline layout ---------------------------------------------------------------------------------- */

typedef enum ItemType { IT_TEXT, IT_OPEN, IT_CLOSE, IT_ATOMIC, IT_BREAK, IT_FLOAT, IT_ABS } ItemType;

typedef struct InlineItem {
    uint8_t t;           /* ItemType */
    CscdLayoutBox* box;
    size_t start, end;   /* byte range in the processed text of box */
    float width;         /* advance including trailing spaces */
    float wordWidth;     /* advance without trailing collapsible spaces */
    bool breakAfter;     /* break opportunity after this item */
    bool forced;         /* forced break (newline in pre text / <br>) */
    bool hyphen;         /* it ends at a soft hyphen: broken there, the line ends with a hyphen */
} InlineItem;

/* The inline ancestors of a box (outermost first), a range of InlineState.chainPool. */
typedef struct Chain { uint32_t from, n; } Chain;

/* A value for each box of one inline layout, found through the box itself (no hashing): the box
 * keeps its slot in inlineOwner / inlineSlot. */
typedef struct InlineState {
    uint32_t id;
    uint32_t count;
    CSCD_VEC(InlineItem) items;
    CscdStrVec processed;
    CSCD_VEC(CscdFont) fonts;
    CSCD_VEC(Chain) chainAt;
    CSCD_VEC(CscdBytes) levels; /* bidi: each byte's embedding level (when any is odd) */
    CscdPtrVec chainPool;
    CscdFloatVec floats1, floats2; /* (scratch of layInline, kept with the state for its capacity) */
    Chain lastChain;
} InlineState;

static volatile uint32_t nextInlineId = 1;

static void stReset(InlineState* st) {
    st->id = cscdAtomicFetchAddU32(&nextInlineId, 1);
    st->count = 0;
    st->items.len = 0;
    for (size_t i = 0; i < st->processed.len; ++i) cscdStrFree(&st->processed.items[i]);
    st->processed.len = 0;
    st->fonts.len = 0;
    for (size_t i = 0; i < st->levels.len; ++i) cscdVecFree(&st->levels.items[i]);
    st->levels.len = 0;
    st->chainAt.len = 0;
    st->chainPool.len = 0;
    st->lastChain.from = st->lastChain.n = 0;
}

static uint32_t stSlot(InlineState* st, CscdLayoutBox* b) {
    if (b->inlineOwner != st->id) {
        b->inlineOwner = st->id;
        b->inlineSlot = st->count++;
    }
    return b->inlineSlot;
}

#define PER_BOX(name, vec, T) \
    static T* name(InlineState* st, CscdLayoutBox* b) { \
        const uint32_t i = stSlot(st, b); \
        if (i >= st->vec.len) cscdVecResize(&st->vec, (size_t)i + 1); \
        return &st->vec.items[i]; \
    }
PER_BOX(stProcessed, processed, CscdStr)
PER_BOX(stFont, fonts, CscdFont)
PER_BOX(stChainAt, chainAt, Chain)
PER_BOX(stLevels, levels, CscdBytes)

static void stSetChain(InlineState* st, CscdLayoutBox* b, const CscdPtrVec* stack) {
    /* siblings share their ancestors: the last chain is reused when it is the same */
    Chain* at = stChainAt(st, b);
    bool same = st->lastChain.n && st->lastChain.n == stack->len;
    for (size_t i = 0; same && i < stack->len; ++i) same = st->chainPool.items[st->lastChain.from + i] == stack->items[i];
    if (!same) {
        st->lastChain.from = (uint32_t)st->chainPool.len;
        st->lastChain.n = (uint32_t)stack->len;
        cscdVecAppend(&st->chainPool, stack->items, stack->len);
    }
    if (!stack->len) { at->from = at->n = 0; }
    else *at = st->lastChain;
}

static Chain stChain(InlineState* st, CscdLayoutBox* b) { return *stChainAt(st, b); }
#define CHAIN_BOX(st, c, k) ((CscdLayoutBox*)(st)->chainPool.items[(c).from + (k)])

/* Inline states are reused (one per nesting depth, on each thread): once warm, a block's inline
 * layout allocates almost nothing. */
static CSCD_THREAD_LOCAL CscdPtrVec freeStates;

static InlineState* leaseState(void) {
    InlineState* st;
    if (freeStates.len) st = (InlineState*)cscdVecPop(&freeStates);
    else st = CSCD_NEW(InlineState);
    stReset(st);
    return st;
}

static void freeState(InlineState* st) {
    stReset(st);
    cscdVecFree(&st->items);
    cscdVecFree(&st->processed);
    cscdVecFree(&st->fonts);
    cscdVecFree(&st->chainAt);
    cscdVecFree(&st->levels);
    cscdVecFree(&st->chainPool);
    cscdVecFree(&st->floats1);
    cscdVecFree(&st->floats2);
    cscdFree(st);
}

static void returnState(InlineState* st) {
    if (freeStates.len < 64) cscdVecPush(&freeStates, st);
    else freeState(st);
}

typedef struct Collect {
    Layouter* L;
    InlineState* st;
    float cbW;
    bool prevSpace;  /* collapsible space at the start of a line is removed */
    bool wordStart;
    CscdPtrVec stack;
    CscdStr raw;       /* (scratch) */
} Collect;

static void pushItem(InlineState* st, InlineItem it) { cscdVecPush(&st->items, it); }

static void emitSeg(Collect* C, CscdLayoutBox* c, const CscdFont* f, size_t a, size_t b, bool brk, bool forced) {
    if (b <= a && !forced) return;
    const CscdComputedStyle* s = c->style;
    const CscdStr* text = stProcessed(C->st, c);
    InlineItem it;
    memset(&it, 0, sizeof it);
    it.t = IT_TEXT;
    it.box = c;
    it.start = a;
    it.end = b;
    CscdSv seg = cscdSvMake(text->ptr + a, b - a);
    it.width = layMeasure(f, seg, s->letterSpacing, s->wordSpacing);
    CscdSv word = seg;
    if (collapses(s->whiteSpace) || s->whiteSpace == CSCD_WS_PRE_WRAP)
        while (word.len && word.ptr[word.len - 1] == ' ') word.len--;
    it.wordWidth = word.len == seg.len ? it.width : layMeasure(f, word, s->letterSpacing, s->wordSpacing);
    it.breakAfter = brk;
    pushItem(C->st, it);
    if (forced) {
        InlineItem br;
        memset(&br, 0, sizeof br);
        br.t = IT_BREAK;
        br.box = c;
        br.forced = true;
        br.breakAfter = true;
        pushItem(C->st, br);
    }
}

static void collectText(Collect* C, CscdLayoutBox* c) {
    const CscdComputedStyle* s = c->style;
    uint8_t ws = s->whiteSpace;
    transformText(cscdStrSv(&c->text), (CscdTextTransform)s->textTransform, &C->wordStart, &C->raw);
    CscdStr* out = stProcessed(C->st, c);
    cscdStrClear(out);
    cscdStrReserve(out, C->raw.len);
    const char* raw = C->raw.ptr ? C->raw.ptr : "";
    if (collapses(ws)) {
        for (size_t i = 0; i < C->raw.len; ++i) {
            char ch = raw[i];
            if (ch == '\n' && ws == CSCD_WS_PRE_LINE) {
                while (out->len && out->ptr[out->len - 1] == ' ') cscdStrTruncate(out, out->len - 1);
                cscdStrAppendChar(out, '\n');
                C->prevSpace = true;
                continue;
            }
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') {
                if (!C->prevSpace) { cscdStrAppendChar(out, ' '); C->prevSpace = true; }
                continue;
            }
            cscdStrAppendChar(out, ch);
            C->prevSpace = false;
        }
    } else {
        for (size_t i = 0; i < C->raw.len; ++i) {
            char ch = raw[i];
            if (ch == '\r') continue;
            if (ch == '\t') { int n = cscdMaxi(1, (int)s->tabSize); cscdStrAppendN(out, ' ', (size_t)n); continue; }
            cscdStrAppendChar(out, ch);
        }
        C->prevSpace = out->len && out->ptr[out->len - 1] == '\n';
    }
    CscdFont f = cscdFontForStyle(s);
    c->font = f;
    *stFont(C->st, c) = f;
    stSetChain(C->st, c, &C->stack);
    /* segments */
    bool canWrap = wraps(ws);
    bool breakAll = s->wordBreak == CSCD_WB_BREAK_ALL;
    size_t segStart = 0;
    const size_t n = stProcessed(C->st, c)->len;
    for (size_t i = 0; i < n;) {
        const CscdStr* text = stProcessed(C->st, c);
        CscdSv tv = cscdStrSv(text);
        size_t at = i;
        uint32_t ch = cscdUtf8Next(tv, &i);
        if (ch == '\n') {
            emitSeg(C, c, &f, segStart, at, true, true);
            segStart = i;
            continue;
        }
        if (!canWrap) continue;
        if (ch == ' ') {
            /* include all following spaces in this segment */
            while (i < n && tv.ptr[i] == ' ') ++i;
            emitSeg(C, c, &f, segStart, i, true, false);
            segStart = i;
        } else if (breakAll || isCJK(ch)) {
            if (i < n && tv.ptr[i] == ' ') continue;
            emitSeg(C, c, &f, segStart, i, true, false);
            segStart = i;
        } else if (ch == '-' && at > segStart && i < n && tv.ptr[i] != ' ') {
            emitSeg(C, c, &f, segStart, i, true, false);
            segStart = i;
        } else if (ch == 0xAD && s->hyphens != 1 && at > segStart) {
            emitSeg(C, c, &f, segStart, i, true, false);
            if (C->st->items.len) cscdVecLast(&C->st->items).hyphen = true;
            segStart = i;
        }
    }
    if (segStart < n) emitSeg(C, c, &f, segStart, n, false, false);
}

static void collectWalk(Collect* C, CscdLayoutBox* parent) {
    InlineState* st = C->st;
    for (size_t ci = 0; ci < parent->children.len; ++ci) {
        CscdLayoutBox* c = cscdBoxChild(parent, ci);
        if (c->isMarker) continue;
        if (c->kind == CSCD_BOX_TEXT) {
            collectText(C, c);
        } else if (c->kind == CSCD_BOX_INLINE) {
            layResolveEdges(c, C->cbW);
            c->font = cscdFontForStyle(c->style);
            *stFont(st, c) = c->font;
            InlineItem o;
            memset(&o, 0, sizeof o);
            o.t = IT_OPEN;
            o.box = c;
            o.width = o.wordWidth = c->margin[3] + c->border[3] + c->padding[3];
            pushItem(st, o);
            cscdVecPush(&C->stack, c);
            stSetChain(st, c, &C->stack);
            collectWalk(C, c);
            C->stack.len--;
            InlineItem cl;
            memset(&cl, 0, sizeof cl);
            cl.t = IT_CLOSE;
            cl.box = c;
            cl.width = cl.wordWidth = c->margin[1] + c->border[1] + c->padding[1];
            /* a break is allowed after an inline if the text before allowed it */
            cl.breakAfter = false;
            pushItem(st, cl);
        } else if (c->kind == CSCD_BOX_LINE_BREAK) {
            InlineItem br;
            memset(&br, 0, sizeof br);
            br.t = IT_BREAK;
            br.box = c;
            br.forced = true;
            br.breakAfter = true;
            c->font = cscdFontForStyle(c->style);
            stSetChain(st, c, &C->stack);
            pushItem(st, br);
            C->prevSpace = true;
        } else if (layIsOutOfFlow(c)) {
            InlineItem it;
            memset(&it, 0, sizeof it);
            it.t = c->style->floating != CSCD_FLOAT_NONE ? IT_FLOAT : IT_ABS;
            it.box = c;
            if (it.t == IT_ABS) c->hasStaticPos = false; /* (found again by this layout) */
            pushItem(st, it);
        } else {
            InlineItem it;
            memset(&it, 0, sizeof it);
            it.t = IT_ATOMIC;
            it.box = c;
            it.breakAfter = wraps(parent->style->whiteSpace);
            stSetChain(st, c, &C->stack);
            pushItem(st, it);
            C->prevSpace = false;
            C->wordStart = true;
        }
    }
}

/* Splits the inline content of a block container into items. */
static void collectInline(Layouter* L, CscdLayoutBox* root, InlineState* st, float cbW) {
    Collect C;
    memset(&C, 0, sizeof C);
    C.L = L;
    C.st = st;
    C.cbW = cbW;
    C.prevSpace = true;
    C.wordStart = true;
    collectWalk(&C, root);
    cscdVecFree(&C.stack);
    cscdStrFree(&C.raw);
    /* break opportunities before atomic inlines */
    for (size_t i = 1; i < st->items.len; ++i) {
        if (st->items.items[i].t == IT_ATOMIC && wraps(root->style->whiteSpace)) {
            for (size_t k = i; k-- > 0;) {
                InlineItem* p = &st->items.items[k];
                if (p->t == IT_OPEN || p->t == IT_CLOSE) continue;
                if (p->t == IT_TEXT && wraps(p->box->style->whiteSpace)) p->breakAfter = true;
                break;
            }
        }
    }
}

float layIntrinsicInline(Layouter* L, CscdLayoutBox* b, bool min) {
    InlineState* st = leaseState();
    cscdVecReserve(&st->items, b->children.len * 2 + 2);
    collectInline(L, b, st, 0);
    float best = 0, line = 0, word = 0;
    for (size_t i = 0; i < st->items.len; ++i) {
        const InlineItem* it = &st->items.items[i];
        switch (it->t) {
        case IT_TEXT:
            if (min) {
                word += it->wordWidth;
                best = cscdMaxf(best, word);
                if (it->breakAfter) word = 0;
                else word += it->width - it->wordWidth;
            } else {
                line += it->width;
                best = cscdMaxf(best, line - (it->width - it->wordWidth));
            }
            break;
        case IT_OPEN: case IT_CLOSE:
            if (min) { word += it->width; best = cscdMaxf(best, word); }
            else { line += it->width; best = cscdMaxf(best, line); }
            break;
        case IT_ATOMIC: {
            float w = min ? layOuterMinContent(L, it->box) : layOuterMaxContent(L, it->box);
            if (min) { best = cscdMaxf(best, word + w); word = it->breakAfter ? 0 : word + w; }
            else { line += w; best = cscdMaxf(best, line); }
            break;
        }
        case IT_FLOAT: {
            float w = min ? layOuterMinContent(L, it->box) : layOuterMaxContent(L, it->box);
            best = cscdMaxf(best, w);
            if (!min) line += w;
            break;
        }
        case IT_BREAK: line = 0; word = 0; break;
        default: break;
        }
    }
    float indent = cscdLenResolve(b->style->textIndent, 0, 0);
    returnState(st);
    return best + cscdMaxf(0.f, indent);
}

typedef struct Placed {
    const InlineItem* it;
    float x;  /* relative to the line's left edge */
    float w;
    size_t start, end;
    uint8_t level; /* bidi embedding level (odd: right to left) */
    bool hyphen;   /* the line was broken at its soft hyphen: a hyphen shows after it */
} Placed;
typedef CSCD_VEC(Placed) PlacedVec;

static Placed placed(const InlineItem* it, float x, float w, size_t start, size_t end) {
    Placed p = {it, x, w, start, end, 0};
    return p;
}

typedef struct VMetrics { float ascent, descent; } VMetrics; /* half-leading included (CSS inline box line-height area) */

/* The A/D of an inline box including half leading, as CSS 10.8.1 defines it. */
static VMetrics lineMetrics(const CscdComputedStyle* s, const CscdFont* f) {
    float A = cscdFontAscent(f), D = cscdFontDescent(f);
    float lh = cscdStyleLineHeight(s, A + cscdFontLineGap(f), D);
    if (s->lineHeight < 0 && s->lineHeightFactor <= 0) lh = A + D + cscdFontLineGap(f);
    float half = (lh - (A + D)) / 2;
    VMetrics m = {A + half, D + half};
    return m;
}

/* Baseline shift of an element relative to its parent's baseline (y down positive). */
static float baselineShift(const CscdComputedStyle* s, const CscdComputedStyle* parent, const CscdFont* parentFont) {
    switch (s->verticalAlign) {
    case CSCD_VA_SUB: return parent->font.size * 0.2f;
    case CSCD_VA_SUPER: return -parent->font.size * 0.34f;
    case CSCD_VA_LENGTH: return -cscdLenResolve(s->verticalAlignLength, cscdStyleLineHeight(s, cscdFontAscent(parentFont), cscdFontDescent(parentFont)), 0);
    default: return 0;
    }
}

static void clearFrags(CscdLayoutBox* p) {
    for (size_t i = 0; i < p->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(p, i);
        cscdTextFragVecClear(&c->textFrags);
        if (c->kind == CSCD_BOX_INLINE) {
            c->inlineFrags.len = 0;
            clearFrags(c);
        }
    }
}

/* the state of one block's inline layout */
typedef struct Inl {
    Layouter* L;
    InlineState* st;
    CscdLayoutBox* b;
    FloatCtx* fc;
    const CscdComputedStyle* bs;
    uint8_t paraLevel;
    bool bidi;
    CscdBytes atomLevel; /* by item index (atomic inlines: an object replacement character) */
    CscdFont rootFont;
    VMetrics strut;
    float strutH;
    float textIndent;
    uint8_t align;
    bool rootWraps;
    int clamp;
    float left, top, cw;
    float y;
    bool firstLine;
    PlacedVec line;
    size_t noOpportunityIn; /* the items of the line known to offer no break (lineHasOpportunity) */
    float lineLeft, lineAvail;
    CscdPtrVec pendingFloats;
    bool hasFloats;
    float lineX;
    int lineCount;
    bool stopped;
} Inl;

static void startLine(Inl* I) {
    I->noOpportunityIn = 0;
    float l = I->left, r = I->left + I->cw;
    if (I->fc && I->fc->floats.len) fcBand(I->fc, I->y, I->strutH, I->left, I->left + I->cw, &l, &r);
    I->lineLeft = l;
    I->lineAvail = cscdMaxf(0.f, r - l);
    if (I->firstLine) { I->lineLeft += I->textIndent; I->lineAvail -= I->textIndent; }
}

/* places floats that could not go on the current line */
static void placeFloat(Inl* I, CscdLayoutBox* f, float atY) {
    layFloat(I->L, f, I->fc, atY, I->left, I->left + I->cw, I->cw);
    I->hasFloats = true;
}

typedef struct CpAt { size_t piece, byte; } CpAt;
typedef CSCD_VEC(CpAt) CpAtVec;

/* Bidi on one line (in logical order, its trailing spaces trimmed): rule L1, each text piece split
 * where its level changes, then the pieces in the order they show (rule L2). */
static void reorderLine(Inl* I) {
    InlineState* st = I->st;
    CscdU32Vec cps = {0};
    CscdBytes lv = {0};
    CpAtVec at = {0};
    for (size_t k = 0; k < I->line.len; ++k) {
        const Placed* p = &I->line.items[k];
        if (p->it->t != IT_TEXT) continue;
        CscdSv txt = cscdStrSv(stProcessed(st, p->it->box));
        const CscdBytes* bl = stLevels(st, p->it->box);
        for (size_t q = p->start; q < p->end;) {
            CpAt a = {k, q};
            cscdVecPush(&at, a);
            cscdVecPush(&lv, q < bl->len ? bl->items[q] : I->paraLevel);
            cscdVecPush(&cps, cscdUtf8Next(txt, &q));
        }
    }
    cscdBidiLineLevels(cps.items, cps.len, I->paraLevel, lv.items);
    const uint8_t kUnset = 255;
    PlacedVec pieces = {0};
    cscdVecReserve(&pieces, I->line.len + 4);
    size_t c = 0;
    for (size_t k = 0; k < I->line.len; ++k) {
        Placed p = I->line.items[k];
        if (p.it->t == IT_TEXT && p.end > p.start) {
            CscdSv txt = cscdStrSv(stProcessed(st, p.it->box));
            const CscdComputedStyle* ts = p.it->box->style;
            size_t from = p.start;
            uint8_t cur = lv.items[c];
            bool split = false;
            for (; c < cps.len && at.items[c].piece == k; ++c) {
                if (lv.items[c] == cur) continue;
                split = true;
                Placed q = p;
                q.start = from;
                q.end = at.items[c].byte;
                q.level = cur;
                q.w = layMeasure(stFont(st, p.it->box), cscdSvSub(txt, q.start, q.end - q.start), ts->letterSpacing, ts->wordSpacing);
                cscdVecPush(&pieces, q);
                from = at.items[c].byte;
                cur = lv.items[c];
            }
            Placed q = p;
            q.start = from;
            q.end = p.end;
            q.level = cur;
            if (split) q.w = layMeasure(stFont(st, p.it->box), cscdSvSub(txt, q.start, q.end - q.start), ts->letterSpacing, ts->wordSpacing);
            cscdVecPush(&pieces, q);
            continue;
        }
        p.level = p.it->t == IT_ATOMIC ? I->atomLevel.items[(size_t)(p.it - st->items.items)] : kUnset;
        if (p.it->t == IT_BREAK) p.level = I->paraLevel;
        cscdVecPush(&pieces, p);
    }
    /* an inline's opening edge goes with what follows it, its closing edge with what precedes */
    for (size_t k = pieces.len; k-- > 0;)
        if (pieces.items[k].level == kUnset && pieces.items[k].it->t == IT_OPEN)
            pieces.items[k].level = k + 1 < pieces.len && pieces.items[k + 1].level != kUnset ? pieces.items[k + 1].level : I->paraLevel;
    for (size_t k = 0; k < pieces.len; ++k)
        if (pieces.items[k].level == kUnset) pieces.items[k].level = k > 0 ? pieces.items[k - 1].level : I->paraLevel;
    CscdBytes levels = {0};
    cscdVecResize(&levels, pieces.len);
    for (size_t k = 0; k < pieces.len; ++k) levels.items[k] = pieces.items[k].level;
    CscdSizeVec order = {0};
    cscdBidiVisualOrder(levels.items, levels.len, &order);
    I->line.len = 0;
    for (size_t k = 0; k < order.len; ++k) cscdVecPush(&I->line, pieces.items[order.items[k]]);
    cscdVecFree(&order);
    cscdVecFree(&levels);
    cscdVecFree(&pieces);
    cscdVecFree(&cps);
    cscdVecFree(&lv);
    cscdVecFree(&at);
}

static float shiftFor(Inl* I, CscdLayoutBox* box) {
    InlineState* st = I->st;
    CscdFloatVec* shiftOf = &st->floats1;
    const uint32_t slot = stSlot(st, box);
    while (slot >= shiftOf->len) cscdVecPush(shiftOf, NAN);
    if (!isnan(shiftOf->items[slot])) return shiftOf->items[slot];
    float sh = 0;
    const Chain chain = stChain(st, box);
    const CscdComputedStyle* parentStyle = I->bs;
    CscdFont parentFont = I->rootFont;
    for (uint32_t k = 0; k < chain.n; ++k) {
        CscdLayoutBox* anc = CHAIN_BOX(st, chain, k);
        sh += baselineShift(anc->style, parentStyle, &parentFont);
        parentStyle = anc->style;
        parentFont = *stFont(st, anc);
    }
    st->floats1.items[stSlot(st, box)] = sh;
    return sh;
}

typedef struct Aligned { size_t idx; float h; bool top; } Aligned;

static void extendRange(CscdU64Map* ranges, CscdLayoutBox* box, float x0, float x1) {
    bool inserted = false;
    CscdMapVal* v = cscdU64MapPut(ranges, CSCD_PKEY(box), &inserted);
    if (inserted) { v->f[0] = x0; v->f[1] = x1; }
    else { v->f[0] = cscdMinf(v->f[0], x0); v->f[1] = cscdMaxf(v->f[1], x1); }
}

static void finishLine(Inl* I, bool lastLine) {
    InlineState* st = I->st;
    const CscdComputedStyle* bs = I->bs;
    PlacedVec* line = &I->line;
    if (I->stopped) { line->len = 0; return; }
    /* trailing collapsible spaces hang */
    for (size_t k = line->len; k-- > 0;) {
        const InlineItem* it = line->items[k].it;
        if (it->t == IT_OPEN || it->t == IT_CLOSE) continue;
        if (it->t == IT_TEXT && collapses(it->box->style->whiteSpace)) {
            CscdSv txt = cscdStrSv(stProcessed(st, it->box));
            size_t e = line->items[k].end;
            while (e > line->items[k].start && txt.ptr[e - 1] == ' ') --e;
            if (e != line->items[k].end) {
                line->items[k].end = e;
                line->items[k].w = layMeasure(stFont(st, it->box), cscdSvSub(txt, line->items[k].start, e - line->items[k].start),
                                              it->box->style->letterSpacing, it->box->style->wordSpacing);
            }
        }
        break;
    }
    bool empty = true;
    for (size_t k = 0; k < line->len; ++k) {
        const Placed* p = &line->items[k];
        if (p->it->t == IT_ATOMIC || p->it->t == IT_BREAK) { empty = false; break; }
        if (p->it->t == IT_TEXT && p->end > p->start) { empty = false; break; }
        if ((p->it->t == IT_OPEN || p->it->t == IT_CLOSE) && p->w > 0) { empty = false; break; }
    }
    if (!line->len || (empty && !(line->len == 1 && line->items[0].it->t == IT_BREAK))) {
        bool onlyBreak = false;
        for (size_t k = 0; k < line->len; ++k) onlyBreak = onlyBreak || line->items[k].it->t == IT_BREAK;
        if (!onlyBreak) { line->len = 0; return; }
    }
    if (I->bidi) reorderLine(I);
    /* recompute x positions after trimming */
    float x = 0;
    for (size_t k = 0; k < line->len; ++k) { line->items[k].x = x; x += line->items[k].w; }
    float lineW = x;

    /* vertical metrics, relative to the baseline (y down positive) */
    float minTop = -I->strut.ascent, maxBottom = I->strut.descent;
    cscdVecClear(&st->floats1); /* baseline shift of each inline box (NaN: not known yet) */
    for (uint32_t k = 0; k < st->count; ++k) cscdVecPush(&st->floats1, NAN);
    CSCD_VEC(Aligned) edgeAligned = {0};
    CscdFloatVec* atomTop = &st->floats2; /* margin-box top relative to baseline */
    cscdVecClear(atomTop);
    cscdVecResize(atomTop, line->len);
    for (size_t k = 0; k < line->len; ++k) {
        const InlineItem* it = line->items[k].it;
        if (it->t == IT_TEXT || it->t == IT_BREAK) {
            CscdLayoutBox* tb = it->box;
            if (it->t == IT_BREAK && tb->kind == CSCD_BOX_LINE_BREAK) {
                VMetrics m = lineMetrics(tb->style, &tb->font);
                float sh = shiftFor(I, tb);
                minTop = cscdMinf(minTop, sh - m.ascent);
                maxBottom = cscdMaxf(maxBottom, sh + m.descent);
                continue;
            }
            VMetrics m = lineMetrics(tb->style, stFont(st, tb));
            float sh = shiftFor(I, tb);
            /* text directly inside an inline uses the inline's metrics; the text box shares its style */
            minTop = cscdMinf(minTop, sh - m.ascent);
            maxBottom = cscdMaxf(maxBottom, sh + m.descent);
        } else if (it->t == IT_OPEN) {
            CscdLayoutBox* ib = it->box;
            VMetrics m = lineMetrics(ib->style, stFont(st, ib));
            float sh = shiftFor(I, ib);
            minTop = cscdMinf(minTop, sh - m.ascent);
            maxBottom = cscdMaxf(maxBottom, sh + m.descent);
        } else if (it->t == IT_ATOMIC) {
            CscdLayoutBox* ab = it->box;
            float H = ab->h + ab->margin[0] + ab->margin[2];
            float B = ab->hasBaseline && !(cscdStyleClipsOverflow(ab->style) && ab->kind == CSCD_BOX_BLOCK) ? ab->lastBaseline - (ab->y - ab->margin[0]) : H;
            if (ab->kind == CSCD_BOX_FLEX || ab->kind == CSCD_BOX_GRID || ab->kind == CSCD_BOX_TABLE) B = ab->hasBaseline ? ab->baseline - (ab->y - ab->margin[0]) : H;
            float sh = shiftFor(I, ab);
            const CscdComputedStyle* as = ab->style;
            Chain ch = stChain(st, ab);
            const CscdComputedStyle* ps = !ch.n ? bs : CHAIN_BOX(st, ch, ch.n - 1)->style;
            CscdFont pf = !ch.n ? I->rootFont : *stFont(st, CHAIN_BOX(st, ch, ch.n - 1));
            float t;
            switch (as->verticalAlign) {
            case CSCD_VA_MIDDLE: t = sh - cscdFontXHeight(&pf) / 2 - H / 2; break;
            case CSCD_VA_TEXT_TOP: t = sh - cscdFontAscent(&pf); break;
            case CSCD_VA_TEXT_BOTTOM: t = sh + cscdFontDescent(&pf) - H; break;
            case CSCD_VA_SUB: t = sh + ps->font.size * 0.2f - B; break;
            case CSCD_VA_SUPER: t = sh - ps->font.size * 0.34f - B; break;
            case CSCD_VA_LENGTH: t = sh - cscdLenResolve(as->verticalAlignLength, cscdStyleLineHeight(as, cscdFontAscent(&pf), cscdFontDescent(&pf)), 0) - B; break;
            case CSCD_VA_TOP: case CSCD_VA_BOTTOM: {
                Aligned a = {k, H, as->verticalAlign == CSCD_VA_TOP};
                cscdVecPush(&edgeAligned, a);
                continue;
            }
            default: t = sh - B; break;
            }
            atomTop->items[k] = t;
            minTop = cscdMinf(minTop, t);
            maxBottom = cscdMaxf(maxBottom, t + H);
        }
    }
    float lineH = maxBottom - minTop;
    for (size_t k = 0; k < edgeAligned.len; ++k) lineH = cscdMaxf(lineH, edgeAligned.items[k].h);
    float baselineY = I->y - minTop;
    /* horizontal alignment */
    float extra = I->lineAvail - lineW;
    float offset = 0;
    float justify = 0;
    uint8_t a = I->align;
    const bool lastByRule = lastLine && bs->alignLastSet; /* text-align-last */
    if (lastByRule) a = bs->textAlignLast;
    if (a == CSCD_ALIGN_START) a = bs->rtl ? CSCD_ALIGN_RIGHT : CSCD_ALIGN_LEFT;
    if (a == CSCD_ALIGN_END) a = bs->rtl ? CSCD_ALIGN_LEFT : CSCD_ALIGN_RIGHT;
    if (a == CSCD_ALIGN_RIGHT) offset = extra;
    else if (a == CSCD_ALIGN_CENTER) offset = extra / 2;
    else if (a == CSCD_ALIGN_JUSTIFY && (!lastLine || lastByRule) && extra > 0) {
        int spaces = 0;
        for (size_t k = 0; k < line->len; ++k) {
            const Placed* p = &line->items[k];
            if (p->it->t != IT_TEXT) continue;
            CscdSv txt = cscdStrSv(stProcessed(st, p->it->box));
            for (size_t q = p->start; q < p->end; ++q) spaces += txt.ptr[q] == ' ';
        }
        if (spaces) justify = extra / spaces;
    }
    if (offset < 0 && I->rootWraps) offset = 0;
    /* place */
    float jx = 0; /* accumulated justification */
    CscdU64Map ranges = {0}; /* inline box -> x extent */
    CscdU64Map opened = {0}, closed = {0};
    CscdLayoutBox* lastFragBox = NULL;
    size_t lastFragIdx = 0;
    bool haveLastFrag = false;
    for (size_t k = 0; k < line->len; ++k) {
        Placed* p = &line->items[k];
        const InlineItem* it = p->it;
        float px = I->lineLeft + offset + p->x + jx;
        if (it->t == IT_TEXT) {
            CscdLayoutBox* tb = it->box;
            CscdSv txt = cscdStrSv(stProcessed(st, tb));
            int sp = 0;
            if (justify > 0) for (size_t q = p->start; q < p->end; ++q) sp += txt.ptr[q] == ' ';
            float w = p->w + sp * justify;
            if (p->end > p->start) {
                const bool rtl = p->level & 1;
                CscdTextFrag* lastFrag = haveLastFrag ? &lastFragBox->textFrags.items[lastFragIdx] : NULL;
                if (lastFragBox == tb && lastFrag && lastFrag->end == p->start && lastFrag->rtl == rtl) {
                    cscdStrAppend(&lastFrag->text, cscdSvSub(txt, p->start, p->end - p->start));
                    if (p->hyphen) cscdStrAppendChar(&lastFrag->text, '-');
                    lastFrag->end = p->end;
                    lastFrag->width = px + w - lastFrag->x;
                } else {
                    CscdTextFrag f;
                    memset(&f, 0, sizeof f);
                    f.x = px;
                    f.baseline = baselineY + shiftFor(I, tb);
                    f.width = w;
                    f.top = I->y;
                    f.height = lineH;
                    f.text = cscdStrDup(cscdSvSub(txt, p->start, p->end - p->start));
                    if (p->hyphen) cscdStrAppendChar(&f.text, '-');
                    f.justify = justify;
                    f.start = p->start;
                    f.end = p->end;
                    f.rtl = rtl;
                    cscdVecPush(&tb->textFrags, f);
                    lastFragBox = tb;
                    lastFragIdx = tb->textFrags.len - 1;
                    haveLastFrag = true;
                }
            }
            Chain ch = stChain(st, tb);
            for (uint32_t q = 0; q < ch.n; ++q) extendRange(&ranges, CHAIN_BOX(st, ch, q), px, px + w);
            jx += sp * justify;
        } else if (it->t == IT_OPEN) {
            CscdLayoutBox* ib = it->box;
            cscdU64MapPut(&opened, CSCD_PKEY(ib), NULL);
            float x0 = px + ib->margin[3];
            bool inserted = false;
            CscdMapVal* r = cscdU64MapPut(&ranges, CSCD_PKEY(ib), &inserted);
            if (inserted) { r->f[0] = x0; r->f[1] = x0 + ib->border[3] + ib->padding[3]; }
            else r->f[0] = cscdMinf(r->f[0], x0);
            /* parents contain this inline's margin box too */
            Chain ch = stChain(st, ib);
            for (uint32_t q = 0; q < ch.n; ++q) {
                CscdLayoutBox* anc = CHAIN_BOX(st, ch, q);
                if (anc == ib) continue;
                CscdMapVal* ra = cscdU64MapPut(&ranges, CSCD_PKEY(anc), &inserted);
                if (inserted) { ra->f[0] = px; ra->f[1] = px + p->w; }
                else ra->f[1] = cscdMaxf(ra->f[1], px + p->w);
            }
            haveLastFrag = false;
        } else if (it->t == IT_CLOSE) {
            CscdLayoutBox* ib = it->box;
            cscdU64MapPut(&closed, CSCD_PKEY(ib), NULL);
            float x1 = px + p->w - ib->margin[1];
            bool inserted = false;
            CscdMapVal* r = cscdU64MapPut(&ranges, CSCD_PKEY(ib), &inserted);
            if (inserted) { r->f[0] = px; r->f[1] = x1; }
            else r->f[1] = cscdMaxf(r->f[1], x1);
            Chain ch = stChain(st, ib);
            for (uint32_t q = 0; q < ch.n; ++q) {
                CscdLayoutBox* anc = CHAIN_BOX(st, ch, q);
                if (anc == ib) continue;
                CscdMapVal* ra = cscdU64MapGet(&ranges, CSCD_PKEY(anc));
                if (ra) ra->f[1] = cscdMaxf(ra->f[1], px + p->w);
            }
            haveLastFrag = false;
        } else if (it->t == IT_ATOMIC) {
            CscdLayoutBox* ab = it->box;
            float mt = 0;
            bool edge = false;
            for (size_t q = 0; q < edgeAligned.len; ++q)
                if (edgeAligned.items[q].idx == k) { edge = true; mt = edgeAligned.items[q].top ? I->y : I->y + lineH - edgeAligned.items[q].h; }
            if (!edge) mt = baselineY + atomTop->items[k];
            float tx = px + ab->margin[3], ty = mt + ab->margin[0];
            cscdBoxShift(ab, tx - ab->x, ty - ab->y);
            Chain ch = stChain(st, ab);
            for (uint32_t q = 0; q < ch.n; ++q) extendRange(&ranges, CHAIN_BOX(st, ch, q), px, px + p->w);
            haveLastFrag = false;
        } else if (it->t == IT_ABS) {
            CscdLayoutBox* ab = it->box;
            ab->staticX = px;
            ab->staticY = I->y;
            ab->hasStaticPos = true;
        }
    }
    /* inline box fragments */
    for (CscdU64MapSlot* s = NULL; (s = cscdU64MapNext(&ranges, s));) {
        CscdLayoutBox* ib = (CscdLayoutBox*)(uintptr_t)s->key;
        if (ib->kind != CSCD_BOX_INLINE) continue;
        CscdFont f = *stFont(st, ib);
        float bl = baselineY + shiftFor(I, ib);
        CscdInlineFrag fr;
        float t = bl - cscdFontAscent(&f) - ib->padding[0] - ib->border[0];
        float bt = bl + cscdFontDescent(&f) + ib->padding[2] + ib->border[2];
        float x0 = s->val.f[0], x1 = s->val.f[1];
        fr.first = cscdU64MapGet(&opened, s->key) != NULL;
        fr.last = cscdU64MapGet(&closed, s->key) != NULL;
        fr.rect = cscdRect(x0, t, cscdMaxf(0.f, x1 - x0), bt - t);
        cscdVecPush(&ib->inlineFrags, fr);
    }
    cscdU64MapFree(&ranges);
    cscdU64MapFree(&opened);
    cscdU64MapFree(&closed);
    cscdVecFree(&edgeAligned);
    CscdLineInfo li = {I->y, lineH, baselineY, I->lineLeft + offset, lineW + jx};
    cscdVecPush(&I->b->lines, li);
    I->y += lineH;
    ++I->lineCount;
    I->firstLine = false;
    line->len = 0;
    if (I->clamp > 0 && I->lineCount >= I->clamp) I->stopped = true;
    for (size_t k = 0; k < I->pendingFloats.len; ++k) placeFloat(I, (CscdLayoutBox*)I->pendingFloats.items[k], I->y);
    I->pendingFloats.len = 0;
}

/* break the current line at the last opportunity, carrying the rest over */
static void breakLine(Inl* I) {
    PlacedVec* line = &I->line;
    size_t cut = line->len;
    for (size_t k = line->len; k-- > 0;) {
        const InlineItem* it = line->items[k].it;
        if (it->breakAfter && (it->t == IT_TEXT || it->t == IT_ATOMIC)) { cut = k + 1; break; }
    }
    /* broken at a soft hyphen: it shows */
    if (cut > 0 && line->items[cut - 1].it->hyphen) {
        Placed* h = &line->items[cut - 1];
        h->hyphen = true;
        h->w += layMeasure(stFont(I->st, h->it->box), CSCD_SV("-"), 0, 0);
    }
    /* Close items right after the cut stay on this line */
    while (cut < line->len && line->items[cut].it->t == IT_CLOSE) ++cut;
    PlacedVec carry = {0};
    cscdVecAppend(&carry, line->items + cut, line->len - cut);
    line->len = cut;
    finishLine(I, false);
    startLine(I);
    I->lineX = 0;
    for (size_t k = 0; k < carry.len; ++k) {
        Placed p = carry.items[k];
        p.x = I->lineX;
        cscdVecPush(line, p);
        I->lineX += p.w;
    }
    cscdVecFree(&carry);
}

/* (asked at each item of a line that overflows: what was looked at once is not looked at again) */
static bool lineHasOpportunity(Inl* I) {
    for (size_t k = I->noOpportunityIn; k < I->line.len; ++k) {
        const InlineItem* it = I->line.items[k].it;
        if (it->breakAfter && (it->t == IT_TEXT || it->t == IT_ATOMIC)) return true;
    }
    I->noOpportunityIn = I->line.len;
    return false;
}

/* text-overflow: ellipsis on the line at top */
static void ellipsisCut(Inl* I, CscdLayoutBox* p, float lineTop, float limit) {
    static const char* ell = "\xE2\x80\xA6";
    for (size_t i = 0; i < p->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(p, i);
        if (c->kind == CSCD_BOX_INLINE) { ellipsisCut(I, c, lineTop, limit); continue; }
        if (c->kind != CSCD_BOX_TEXT) continue;
        for (size_t k = 0; k < c->textFrags.len; ++k) {
            CscdTextFrag* f = &c->textFrags.items[k];
            if (f->top != lineTop) continue;
            float ew = layMeasure(&c->font, cscdSv(ell), 0, 0);
            if (f->x >= limit - ew) { cscdStrClear(&f->text); f->width = 0; continue; }
            if (f->x + f->width <= limit) continue;
            /* shortened to the longest run of whole characters that fits with the ellipsis, found by
             * halving (a longer run is never narrower) */
            CscdSizeVec starts = {0};
            for (size_t q = 0; q < f->text.len; ++q)
                if (((unsigned char)f->text.ptr[q] & 0xC0) != 0x80) cscdVecPush(&starts, q);
            size_t lo = 0, hi = starts.len; /* starts[lo] fits (or is 0); none from hi on was found to */
            while (hi - lo > 1) {
                const size_t mid = (lo + hi) / 2;
                const float w = layMeasure(&c->font, cscdSvMake(f->text.ptr, starts.items[mid]), c->style->letterSpacing, 0);
                if (f->x + w + ew <= limit) lo = mid;
                else hi = mid;
            }
            CscdStr s = cscdStrDup(cscdSvMake(f->text.ptr, starts.len ? starts.items[lo] : 0));
            cscdVecFree(&starts);
            cscdStrAppendC(&s, ell);
            cscdStrFree(&f->text);
            f->text = s;
            f->width = layMeasure(&c->font, cscdStrSv(&f->text), c->style->letterSpacing, 0);
        }
    }
}

static void findLastFrag(CscdLayoutBox* p, float lastTop, CscdLayoutBox** lastBox, size_t* lastIdx) {
    for (size_t i = 0; i < p->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(p, i);
        if (c->kind == CSCD_BOX_INLINE) { findLastFrag(c, lastTop, lastBox, lastIdx); continue; }
        if (c->kind != CSCD_BOX_TEXT) continue;
        for (size_t k = 0; k < c->textFrags.len; ++k)
            if (c->textFrags.items[k].top == lastTop) { *lastBox = c; *lastIdx = k; }
    }
}

typedef struct CpItem { size_t item, byte; } CpItem;
typedef CSCD_VEC(CpItem) CpItemVec;

/* The bidirectional controls unicode-bidi stands for (UAX #9 and CSS Writing Modes): opening
 * (open) or closing an element of style s; how many written to out. */
static int bidiControls(const CscdComputedStyle* s, bool open, uint32_t* out) {
    const bool r = s->rtl;
    switch (s->unicodeBidi) {
    case CSCD_UNICODE_BIDI_EMBED: out[0] = open ? (r ? 0x202B : 0x202A) : 0x202C; return 1;
    case CSCD_UNICODE_BIDI_ISOLATE: out[0] = open ? (r ? 0x2067 : 0x2066) : 0x2069; return 1;
    case CSCD_UNICODE_BIDI_OVERRIDE: out[0] = open ? (r ? 0x202E : 0x202D) : 0x202C; return 1;
    case CSCD_UNICODE_BIDI_ISOLATE_OVERRIDE:
        if (open) { out[0] = r ? 0x2067 : 0x2066; out[1] = r ? 0x202E : 0x202D; }
        else { out[0] = 0x202C; out[1] = 0x2069; }
        return 2;
    case CSCD_UNICODE_BIDI_PLAINTEXT: out[0] = open ? 0x2068 : 0x2069; return 1;
    default: return 0;
    }
}

/* unicode-bidi: plaintext on a block: each paragraph's direction from its first strong letter
 * (outside isolates); dflt when it has none. */
static int firstStrongLevel(const uint32_t* cps, size_t n, int dflt) {
    int depth = 0;
    for (size_t i = 0; i < n; ++i) {
        const CscdBidiClass c = cscdBidiClass(cps[i]);
        if (c == CSCD_BIDI_LRI || c == CSCD_BIDI_RLI || c == CSCD_BIDI_FSI) ++depth;
        else if (c == CSCD_BIDI_PDI) { if (depth) --depth; }
        else if (!depth && c == CSCD_BIDI_L) return 0;
        else if (!depth && (c == CSCD_BIDI_R || c == CSCD_BIDI_AL)) return 1;
    }
    return dflt;
}

static void bidiParagraph(Inl* I, CscdU32Vec* cps, CpItemVec* at) {
    InlineState* st = I->st;
    if (!cps->len) return;
    CscdBytes lv = {0};
    cscdVecResize(&lv, cps->len);
    int level = I->paraLevel;
    if (I->bs->unicodeBidi == CSCD_UNICODE_BIDI_PLAINTEXT) level = firstStrongLevel(cps->items, cps->len, level);
    memset(lv.items, level, cps->len);
    if (level == 1 || cscdBidiNeeded(cps->items, cps->len)) cscdBidiLevels(cps->items, cps->len, level, lv.items);
    for (size_t c = 0; c < cps->len; ++c) {
        const InlineItem* it = &st->items.items[at->items[c].item];
        if (it->t == IT_OPEN || it->t == IT_CLOSE) continue; /* (a control unicode-bidi put there) */
        if (it->t == IT_ATOMIC) { I->atomLevel.items[at->items[c].item] = lv.items[c]; continue; }
        CscdBytes* bl = stLevels(st, it->box);
        size_t q = at->items[c].byte, e = q;
        cscdUtf8Next(cscdStrSv(stProcessed(st, it->box)), &e);
        for (; q < e; ++q) bl->items[q] = lv.items[c];
    }
    cscdVecClear(cps);
    cscdVecClear(at);
    cscdVecFree(&lv);
}

float layInline(Layouter* L, CscdLayoutBox* b, FloatCtx* fc) {
    InlineState* st = leaseState();
    Inl I;
    memset(&I, 0, sizeof I);
    I.L = L;
    I.st = st;
    I.b = b;
    I.fc = fc;
    I.cw = cscdBoxContentW(b);
    I.left = cscdBoxContentX(b);
    I.top = cscdBoxContentY(b);
    collectInline(L, b, st, I.cw);
    b->lines.len = 0;
    /* clear old fragments */
    clearFrags(b);

    const CscdComputedStyle* bs = b->style;
    I.bs = bs;
    /* Bidi (UAX #9): the embedding level of every character, when the block goes right to left or
     * holds text that does. Text without any (no byte of a code point from U+0580 up) skips it. */
    I.paraLevel = bs->rtl ? 1 : 0;
    I.bidi = I.paraLevel == 1 || bs->unicodeBidi == CSCD_UNICODE_BIDI_OVERRIDE || bs->unicodeBidi == CSCD_UNICODE_BIDI_PLAINTEXT ||
             bs->unicodeBidi == CSCD_UNICODE_BIDI_ISOLATE_OVERRIDE;
    for (size_t i = 0; i < st->items.len && !I.bidi; ++i) {
        const InlineItem* it = &st->items.items[i];
        if (it->t == IT_OPEN && it->box->style->unicodeBidi && (it->box->style->rtl || it->box->style->unicodeBidi >= CSCD_UNICODE_BIDI_OVERRIDE)) I.bidi = true;
        if (it->t != IT_TEXT) continue;
        CscdSv txt = cscdStrSv(stProcessed(st, it->box));
        for (size_t q = it->start; q < it->end && !I.bidi; ++q) I.bidi = (unsigned char)txt.ptr[q] >= 0xD6;
    }
    if (I.bidi) {
        CscdU32Vec cps = {0};
        CpItemVec at = {0}; /* each code point's item and byte */
        cscdVecResize(&I.atomLevel, st->items.len);
        memset(I.atomLevel.items, I.paraLevel, st->items.len);
        /* (the block's own override: around each paragraph) */
        const bool blockOverride = bs->unicodeBidi == CSCD_UNICODE_BIDI_OVERRIDE || bs->unicodeBidi == CSCD_UNICODE_BIDI_ISOLATE_OVERRIDE;
        const uint32_t overrideCp = bs->rtl ? 0x202E : 0x202D;
        bool paragraphOpen = false;
        for (size_t i = 0; i < st->items.len; ++i) {
            const InlineItem* it = &st->items.items[i];
            if (blockOverride && !paragraphOpen && it->t != IT_BREAK) {
                cscdVecPush(&at, ((CpItem){i, 0}));
                cscdVecPush(&cps, overrideCp);
                paragraphOpen = true;
            }
            if ((it->t == IT_OPEN || it->t == IT_CLOSE) && it->box && it->box->style) {
                uint32_t ctl[2];
                const int n = bidiControls(it->box->style, it->t == IT_OPEN, ctl);
                for (int k = 0; k < n; ++k) {
                    cscdVecPush(&at, ((CpItem){i, 0}));
                    cscdVecPush(&cps, ctl[k]);
                }
                continue;
            }
            if (it->t == IT_BREAK) paragraphOpen = false;
            if (it->t == IT_TEXT) {
                CscdSv txt = cscdStrSv(stProcessed(st, it->box));
                CscdBytes* bl = stLevels(st, it->box);
                if (bl->len != txt.len) {
                    cscdVecClear(bl);
                    cscdVecResize(bl, txt.len);
                    memset(bl->items, I.paraLevel, txt.len);
                }
                for (size_t q = it->start; q < it->end;) {
                    CpItem a = {i, q};
                    cscdVecPush(&at, a);
                    cscdVecPush(&cps, cscdUtf8Next(txt, &q));
                }
            } else if (it->t == IT_ATOMIC) {
                CpItem a = {i, 0};
                cscdVecPush(&at, a);
                cscdVecPush(&cps, 0xFFFC);
            } else if (it->t == IT_BREAK) bidiParagraph(&I, &cps, &at); /* (a forced break ends a paragraph) */
        }
        bidiParagraph(&I, &cps, &at);
        cscdVecFree(&cps);
        cscdVecFree(&at);
    }

    I.rootFont = cscdFontForStyle(bs);
    b->font = I.rootFont;
    I.strut = lineMetrics(bs, &I.rootFont);
    I.strutH = I.strut.ascent + I.strut.descent;
    I.textIndent = cscdLenResolve(bs->textIndent, I.cw, 0);
    I.align = bs->textAlign;
    I.rootWraps = wraps(bs->whiteSpace);
    I.clamp = bs->lineClamp;
    I.y = I.top;
    I.firstLine = true;
    cscdVecReserve(&I.line, 16);
    I.lineLeft = I.left;
    I.lineAvail = I.cw;
    I.hasFloats = fc && fc->floats.len;
    startLine(&I);
    I.lineX = 0;

    for (size_t i = 0; i < st->items.len; ++i) {
        InlineItem* it = &st->items.items[i];
        switch (it->t) {
        case IT_OPEN: case IT_CLOSE:
            cscdVecPush(&I.line, placed(it, I.lineX, it->width, 0, 0));
            I.lineX += it->width;
            break;
        case IT_TEXT: {
            CscdLayoutBox* tb = it->box;
            const CscdComputedStyle* ts = tb->style;
            bool canWrapHere = wraps(ts->whiteSpace);
            bool fits = I.lineX + it->wordWidth <= I.lineAvail + 0.01f;
            if (!fits && canWrapHere) {
                if (lineHasOpportunity(&I)) {
                    breakLine(&I);
                    fits = I.lineX + it->wordWidth <= I.lineAvail + 0.01f;
                } else if (!I.line.len || I.lineX == 0) {
                    /* floats may leave room further down */
                    if (I.hasFloats && fc) {
                        float l, r;
                        fcBand(fc, I.y, I.strutH, I.left, I.left + I.cw, &l, &r);
                        if (r - l < I.cw && it->wordWidth > I.lineAvail) {
                            float next = 1e30f;
                            for (size_t q = 0; q < fc->floats.len; ++q)
                                if (cscdRectBottom(fc->floats.items[q].r) > I.y) next = cscdMinf(next, cscdRectBottom(fc->floats.items[q].r));
                            if (next < 1e29f) { I.y = next; startLine(&I); fits = it->wordWidth <= I.lineAvail; }
                        }
                    }
                }
            }
            if (!fits && canWrapHere && (ts->overflowWrap || ts->wordBreak == CSCD_WB_BREAK_WORD || ts->wordBreak == CSCD_WB_BREAK_ALL)) {
                /* break inside the word, character by character */
                CscdSv txt = cscdStrSv(stProcessed(st, tb));
                const CscdFont f = *stFont(st, tb);
                size_t pos = it->start;
                while (pos < it->end) {
                    size_t q = pos, lastFit = pos;
                    while (q < it->end) {
                        size_t nq = q;
                        cscdUtf8Next(txt, &nq);
                        float w = layMeasure(&f, cscdSvSub(txt, pos, nq - pos), ts->letterSpacing, ts->wordSpacing);
                        if (I.lineX + w > I.lineAvail + 0.01f && lastFit > pos) break;
                        lastFit = nq;
                        q = nq;
                        if (I.lineX + w > I.lineAvail + 0.01f) break;
                    }
                    if (lastFit == pos) { size_t nq = pos; cscdUtf8Next(txt, &nq); lastFit = nq; }
                    float w = layMeasure(&f, cscdSvSub(txt, pos, lastFit - pos), ts->letterSpacing, ts->wordSpacing);
                    cscdVecPush(&I.line, placed(it, I.lineX, w, pos, lastFit));
                    I.lineX += w;
                    pos = lastFit;
                    if (pos < it->end) { finishLine(&I, false); startLine(&I); I.lineX = 0; }
                }
                break;
            }
            cscdVecPush(&I.line, placed(it, I.lineX, it->width, it->start, it->end));
            I.lineX += it->width;
            break;
        }
        case IT_ATOMIC: {
            CscdLayoutBox* ab = it->box;
            layAtomicInline(L, ab, I.cw, -1);
            float w = ab->w + ab->margin[1] + ab->margin[3];
            it->width = it->wordWidth = w;
            bool fits = I.lineX + w <= I.lineAvail + 0.01f;
            if (!fits && I.rootWraps && lineHasOpportunity(&I)) breakLine(&I);
            cscdVecPush(&I.line, placed(it, I.lineX, w, 0, 0));
            I.lineX += w;
            break;
        }
        case IT_BREAK:
            cscdVecPush(&I.line, placed(it, I.lineX, 0, 0, 0));
            finishLine(&I, true);
            startLine(&I);
            I.lineX = 0;
            break;
        case IT_FLOAT:
            if (!fc) break;
            if (!I.line.len || I.lineX == 0) {
                placeFloat(&I, it->box, I.y);
                startLine(&I);
            } else {
                /* fits beside the current line? */
                CscdLayoutBox* f = it->box;
                layFloat(L, f, fc, I.y, I.left, I.left + I.cw, I.cw);
                float fw = f->w + f->margin[1] + f->margin[3];
                if (I.lineX + fw <= I.lineAvail) {
                    bool isLeft = f->style->floating == CSCD_FLOAT_LEFT;
                    if (isLeft) I.lineLeft += fw; /* positions are recomputed at finish */
                    I.lineAvail -= fw;
                } else {
                    /* move it below the line */
                    fc->floats.len--;
                    cscdVecPush(&I.pendingFloats, f);
                }
            }
            break;
        case IT_ABS:
            cscdVecPush(&I.line, placed(it, I.lineX, 0, 0, 0));
            break;
        }
    }
    finishLine(&I, true);
    for (size_t k = 0; k < I.pendingFloats.len; ++k) placeFloat(&I, (CscdLayoutBox*)I.pendingFloats.items[k], I.y);

    /* the static position of absolutely positioned boxes found in inline content */
    for (size_t i = 0; i < st->items.len; ++i) {
        const InlineItem* it = &st->items.items[i];
        if (it->t != IT_ABS) continue;
        CscdLayoutBox* ab = it->box;
        /* (not on a line: where the next one would start) */
        if (!ab->hasStaticPos) { ab->staticX = I.left; ab->staticY = I.y; }
        ab->hasStaticPos = true;
        layDeferAbsolute(L, ab);
    }

    /* text-overflow: ellipsis */
    if (bs->textOverflow == CSCD_TO_ELLIPSIS && bs->overflowX != CSCD_OVERFLOW_VISIBLE) {
        for (size_t k = 0; k < b->lines.len; ++k) {
            const CscdLineInfo* li = &b->lines.items[k];
            if (li->width <= I.cw + 0.5f) continue;
            ellipsisCut(&I, b, li->top, I.left + I.cw);
        }
    }
    if (I.clamp > 0 && I.stopped && b->lines.len) {
        /* ellipsis at the end of the last kept line */
        float lastTop = cscdVecLast(&b->lines).top;
        CscdLayoutBox* lastBox = NULL;
        size_t lastIdx = 0;
        findLastFrag(b, lastTop, &lastBox, &lastIdx);
        if (lastBox) {
            static const char* ell = "\xE2\x80\xA6";
            CscdTextFrag* lastF = &lastBox->textFrags.items[lastIdx];
            float ew = layMeasure(&lastBox->font, cscdSv(ell), 0, 0);
            CscdStr s = cscdStrCopy(&lastF->text);
            while (s.len && lastF->x + layMeasure(&lastBox->font, cscdStrSv(&s), 0, 0) + ew > I.left + I.cw) {
                size_t q = s.len - 1;
                while (q > 0 && ((unsigned char)s.ptr[q] & 0xC0) == 0x80) --q;
                cscdStrTruncate(&s, q);
            }
            while (s.len && s.ptr[s.len - 1] == ' ') cscdStrTruncate(&s, s.len - 1);
            cscdStrAppendC(&s, ell);
            cscdStrFree(&lastF->text);
            lastF->text = s;
            lastF->width = layMeasure(&lastBox->font, cscdStrSv(&lastF->text), 0, 0);
        }
    }

    if (b->lines.len) {
        b->baseline = b->lines.items[0].baseline;
        b->lastBaseline = cscdVecLast(&b->lines).baseline;
        b->hasBaseline = true;
    }
    float result = I.y - I.top;
    cscdVecFree(&I.line);
    cscdVecFree(&I.pendingFloats);
    cscdVecFree(&I.atomLevel);
    returnState(st);
    return result;
}

/* ---- block layout ---------------------------------------------------------------------------------- */

bool layIsContainingBlock(const CscdLayoutBox* b) { return b->style && (b->style->position != CSCD_POS_STATIC || b->style->transform.len); }

/* Queues a positioned box for its containing block, once: a box laid out again (a flex item measured,
 * then laid out for good) finds it again. */
void layDeferAbsolute(Layouter* L, CscdLayoutBox* c) {
    const int to = c->style->position == CSCD_POS_FIXED ? -1 : (int)L->absStack.len - 1;
    if (to < L->deferredTo) L->deferredTo = to;
    CscdPtrVec* list = c->style->position == CSCD_POS_FIXED ? &L->fixedBoxes : L->absStack.len ? &cscdVecLast(&L->absStack) : NULL;
    if (!list) return;
    for (size_t i = list->len; i-- > 0;)
        if (list->items[i] == c) return;
    cscdVecPush(list, c);
}

static bool establishesBFC(const CscdLayoutBox* b, const BlockInput* in) {
    const CscdComputedStyle* s = b->style;
    if (!b->parent) return true;
    if (b->atomicInline || s->floating != CSCD_FLOAT_NONE || s->position == CSCD_POS_ABSOLUTE || s->position == CSCD_POS_FIXED) return true;
    if (s->overflowX != CSCD_OVERFLOW_VISIBLE && s->overflowX != CSCD_OVERFLOW_CLIP) return true;
    if (s->display == CSCD_DISPLAY_FLOW_ROOT || b->kind == CSCD_BOX_TABLE_CELL || b->kind == CSCD_BOX_FLEX || b->kind == CSCD_BOX_GRID || b->kind == CSCD_BOX_TABLE) return true;
    if (in->forcedW >= 0) return true; /* flex / grid items */
    if (b->kind == CSCD_BOX_REPLACED) return true;
    if (s->display == CSCD_DISPLAY_TABLE_CAPTION) return true;
    return false;
}

static bool topCollapsible(const CscdLayoutBox* b) {
    if (b->kind != CSCD_BOX_BLOCK || b->atomicInline || (!b->anonymous && !b->style)) return false;
    const CscdComputedStyle* s = b->style;
    if (s->floating != CSCD_FLOAT_NONE || s->position == CSCD_POS_ABSOLUTE || s->position == CSCD_POS_FIXED) return false;
    if (s->overflowX != CSCD_OVERFLOW_VISIBLE || s->display == CSCD_DISPLAY_FLOW_ROOT) return false;
    if (b->border[0] > 0 || b->padding[0] > 0) return false;
    if (b->inlineContent) return false;
    return true;
}

/* The margins that collapse at the top of b: its own and those of its first in-flow child chain. */
static MarginSet collapsedTop(CscdLayoutBox* b, float cbW) {
    MarginSet m = {0, 0};
    layResolveEdges(b, cbW);
    msAdd(&m, b->margin[0]);
    if (!topCollapsible(b)) return m;
    if (b->style->clear != CSCD_CLEAR_NONE) return m;
    for (size_t i = 0; i < b->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(b, i);
        if (layIsOutOfFlow(c) || c->isMarker) continue;
        if (c->kind == CSCD_BOX_TEXT) return m;
        if (c->style && c->style->clear != CSCD_CLEAR_NONE) return m;
        msAddSet(&m, collapsedTop(c, cbW));
        break;
    }
    return m;
}

void layFloat(Layouter* L, CscdLayoutBox* b, FloatCtx* fc, float y, float left, float right, float cbW) {
    BlockInput in = blockInput();
    in.cbW = cbW;
    in.shrinkToFit = true;
    in.availW = right - left;
    layBlock(L, b, &in);
    float mw = b->w + b->margin[1] + b->margin[3], mh = b->h + b->margin[0] + b->margin[2];
    if (b->style->clear != CSCD_CLEAR_NONE && fc) y = cscdMaxf(y, fcClearY(fc, (CscdClear)b->style->clear));
    CscdPoint p = fc ? fcPlace(fc, mw, mh, y, left, right, b->style->floating == CSCD_FLOAT_LEFT) : cscdPoint(left, y);
    cscdBoxShift(b, p.x + b->margin[3] - b->x, p.y + b->margin[0] - b->y);
    /* shape-outside: the lines wrap around the shape (in its reference box: the margin box unless named) */
    b->shape.len = 0;
    if (fc && b->style->shapeOutside.len) {
        const CscdSv sv = cscdStrSv(&b->style->shapeOutside);
        CscdRect ref = cscdRect(p.x, p.y, mw, mh);
        if (cscdSvFind(sv, CSCD_SV("border-box"), 0) != CSCD_NPOS) ref = cscdBoxBorderBox(b);
        else if (cscdSvFind(sv, CSCD_SV("padding-box"), 0) != CSCD_NPOS) ref = cscdBoxPaddingBox(b);
        else if (cscdSvFind(sv, CSCD_SV("content-box"), 0) != CSCD_NPOS) ref = cscdBoxContentBox(b);
        CscdPath path = {0};
        if (!cscdBasicShapePath(sv, ref, &path)) cscdPathAddRect(&path, ref); /* (a box keyword alone: that box) */
        CscdPolyVec polys = {0};
        cscdFlattenInto(&path, cscdMatrix(1, 0, 0, 1, 0, 0), 0.5f, &polys);
        for (size_t i = 0; i < polys.len; ++i) cscdVecAppend(&b->shape, polys.items[i].items, polys.items[i].len);
        cscdPolyVecFree(&polys);
        cscdPathFree(&path);
        if (b->shape.len >= 3) {
            FloatBox* fb = &cscdVecLast(&fc->floats);
            fb->shape = &b->shape;
            fb->shapeMargin = b->style->shapeMargin;
        }
    }
}

void layAtomicInline(Layouter* L, CscdLayoutBox* b, float cbW, float cbH) {
    BlockInput in = blockInput();
    in.cbW = cbW;
    in.cbH = cbH;
    in.shrinkToFit = true;
    in.availW = cbW;
    layBlock(L, b, &in);
    /* placed by the line: border box at the margin offset */
    cscdBoxShift(b, -b->x + b->margin[3], -b->y + b->margin[0]);
}

void layApplyRelative(CscdLayoutBox* b, float cbW, float cbH) {
    const CscdComputedStyle* s = b->style;
    if (s->position != CSCD_POS_RELATIVE) return;
    float dx = 0, dy = 0;
    if (!cscdLenIsAuto(s->left)) dx = cscdLenResolve(s->left, cbW, 0);
    else if (!cscdLenIsAuto(s->right)) dx = -cscdLenResolve(s->right, cbW, 0);
    if (!cscdLenIsAuto(s->top) && !(cscdLenHasPercent(s->top) && cbH < 0)) dy = cscdLenResolve(s->top, cbH, 0);
    else if (!cscdLenIsAuto(s->bottom) && !(cscdLenHasPercent(s->bottom) && cbH < 0)) dy = -cscdLenResolve(s->bottom, cbH, 0);
    cscdBoxShift(b, dx, dy);
}

void layComputeOverflow(CscdLayoutBox* b) {
    CscdRect o = cscdBoxBorderBox(b);
    CscdRect content = cscdBoxPaddingBox(b);
    bool clip = b->style && cscdStyleClipsOverflow(b->style);
    for (size_t i = 0; i < b->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(b, i);
        if (c->kind == CSCD_BOX_TEXT) {
            for (size_t k = 0; k < c->textFrags.len; ++k) {
                const CscdTextFrag* f = &c->textFrags.items[k];
                content = cscdRectUnite(content, cscdRect(f->x, f->top, cscdMaxf(f->width, 0.01f), f->height));
            }
            continue;
        }
        if (c->kind == CSCD_BOX_INLINE) {
            for (size_t k = 0; k < c->inlineFrags.len; ++k) content = cscdRectUnite(content, c->inlineFrags.items[k].rect);
            content = cscdRectUnite(content, c->overflow);
            continue;
        }
        CscdRect cr = cscdBoxBorderBox(c);
        if (!(c->style && cscdStyleClipsOverflow(c->style))) cr = cscdRectUnite(cr, c->overflow);
        /* the end margins of in-flow children count for scrolling */
        cr.w += cscdMaxf(0.f, c->margin[1]);
        cr.h += cscdMaxf(0.f, c->margin[2]);
        content = cscdRectUnite(content, cr);
    }
    if (clip) {
        /* scroll extents include the end padding */
        b->overflow = cscdRectUnite(content, cscdBoxPaddingBox(b));
        CscdRect inner = cscdBoxPaddingBox(b);
        if (cscdRectRight(b->overflow) > cscdRectRight(inner)) b->overflow.w += b->padding[1];
        if (cscdRectBottom(b->overflow) > cscdRectBottom(inner)) b->overflow.h += b->padding[2];
    } else {
        b->overflow = cscdRectUnite(o, content);
    }
}

static float layBlockChildren(Layouter* L, CscdLayoutBox* b, FloatCtx* fc, float cbH, MarginSet* trailing, bool topCollapsed) {
    float cx = cscdBoxContentX(b), cw = cscdBoxContentW(b);
    float top = cscdBoxContentY(b);
    float cursor = top;
    MarginSet pending = {0, 0};
    bool first = true;
    bool firstBaselineSet = false;
    for (size_t i = 0; i < b->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(b, i);
        if (c->isMarker) continue;
        if (c->kind == CSCD_BOX_TEXT || c->kind == CSCD_BOX_INLINE || c->kind == CSCD_BOX_LINE_BREAK) continue; /* fixed up earlier */
        const CscdComputedStyle* cs = c->style;
        if (cs->position == CSCD_POS_ABSOLUTE || cs->position == CSCD_POS_FIXED) {
            c->staticX = cx;
            c->staticY = cursor + msValue(pending);
            c->hasStaticPos = true;
            layDeferAbsolute(L, c);
            continue;
        }
        if (cs->floating != CSCD_FLOAT_NONE) {
            FloatCtx local;
            memset(&local, 0, sizeof local);
            layFloat(L, c, fc ? fc : &local, cursor + msValue(pending), cx, cx + cw, cw);
            cscdVecFree(&local.floats);
            continue;
        }
        MarginSet ct = collapsedTop(c, cw);
        bool skip = first && topCollapsed;
        MarginSet ms = pending;
        if (!skip) msAddSet(&ms, ct);
        float y = cursor + (skip ? 0 : msValue(ms));
        if (cs->clear != CSCD_CLEAR_NONE && fc) {
            float clr = fcClearY(fc, (CscdClear)cs->clear);
            if (clr > y) { y = clr; ms.pos = ms.neg = 0; }
        }
        BlockInput in = blockInput();
        in.x = cx;
        in.y = y;
        in.cbW = cw;
        in.cbH = cbH;
        in.floats = fc;
        in.marginTopCollapsed = topCollapsible(c);
        /* block formatting contexts avoid floats */
        bool bfcChild = cs->overflowX != CSCD_OVERFLOW_VISIBLE || cs->display == CSCD_DISPLAY_FLOW_ROOT || c->kind == CSCD_BOX_FLEX ||
                        c->kind == CSCD_BOX_GRID || c->kind == CSCD_BOX_TABLE || c->kind == CSCD_BOX_REPLACED;
        if (bfcChild && fc && fc->floats.len) {
            float l, r;
            fcBand(fc, y, 1, cx, cx + cw, &l, &r);
            if (l > cx || r < cx + cw) { in.x = l; in.cbW = r - l; in.availW = r - l; }
        }
        MarginSet bottom = layBlock(L, c, &in);
        bool emptyThrough = c->h == 0 && c->kind == CSCD_BOX_BLOCK && !c->lines.len && c->border[0] == 0 && c->border[2] == 0 &&
                            c->padding[0] == 0 && c->padding[2] == 0 && cscdLenIsAuto(cs->height) && cscdLenIsAuto(cs->minHeight) && !c->atomicInline;
        if (emptyThrough) {
            pending = ms;
            msAddSet(&pending, bottom);
        } else {
            cursor = c->y + c->h;
            pending = bottom;
            first = false;
            if (!firstBaselineSet && c->hasBaseline) { b->baseline = c->baseline; b->hasBaseline = true; firstBaselineSet = true; }
            if (c->hasBaseline) b->lastBaseline = c->lastBaseline;
        }
        if (!emptyThrough) first = false;
    }
    *trailing = pending;
    return cursor - top;
}

float layContent(Layouter* L, CscdLayoutBox* b, FloatCtx* fc, float cbH) {
    switch (b->kind) {
    case CSCD_BOX_FLEX: return layFlex(L, b, cbH);
    case CSCD_BOX_GRID: return layGrid(L, b, cbH);
    case CSCD_BOX_TABLE: return layTable(L, b, cbH);
    default: break;
    }
    if (b->inlineContent) return layInline(L, b, fc);
    MarginSet t;
    return layBlockChildren(L, b, fc, cbH, &t, false);
}

/* ---- remembered block layouts ------------------------------------------------------------------------
 * Flex, grid and table containers lay their items out twice: once to measure them (their height
 * left to their content), then for good (at the height the line or row gave them). Nested, that
 * doubles at every level. So a box that starts a formatting context of its own keeps its last layout
 * for the rest of the pass: laid out again with the same input it is only moved, and so is it when
 * the height now imposed is the one it found for itself, unless something inside it lays out
 * differently once its height is definite. Laying anything inside it out again forgets it. */

/* The box's own style resolves percentages against the containing block's height. */
static bool ownHeightPercent(const CscdLayoutBox* b) {
    const CscdComputedStyle* s = b->style;
    if (!s) return false;
    if (cscdLenHasPercent(s->height) || cscdLenHasPercent(s->minHeight) || cscdLenHasPercent(s->maxHeight)) return true;
    return s->position != CSCD_POS_STATIC && (cscdLenHasPercent(s->top) || cscdLenHasPercent(s->bottom));
}

/* Something in the box lays out differently when the box's height is definite rather than found from
 * its content, even at the same value: percentage heights, column flex containers, grids with rows of
 * their own, aspect ratios. */
static bool heightSensitive(Layouter* L, CscdLayoutBox* b) {
    if (b->sensPass == L->pass) return b->heightSensitive;
    bool sens = false;
    const CscdComputedStyle* s = b->style;
    if (b->kind != CSCD_BOX_TEXT && s)
        sens = ownHeightPercent(b) || s->aspectRatio > 0 || (b->kind == CSCD_BOX_FLEX && cscdStyleIsColumn(s)) ||
               (b->kind == CSCD_BOX_GRID && (s->gridRows.tracks.len || s->gridRows.subgrid || s->gridRows.autoRepeatAt >= 0));
    for (size_t i = 0; i < b->children.len && !sens; ++i) sens = heightSensitive(L, cscdBoxChild(b, i));
    b->sensPass = L->pass;
    b->heightSensitive = sens;
    return sens;
}

/* What a block layout depends on besides its position (and the floats around it, which a box with a
 * formatting context of its own does not see). */
static void memoKey(const CscdLayoutBox* b, const BlockInput* in, float key[8]) {
    key[0] = in->cbW;
    key[1] = ownHeightPercent(b) ? in->cbH : 0;
    key[2] = in->forcedW;
    key[3] = in->forcedH;
    key[4] = in->availW;
    key[5] = in->shrinkToFit;
    key[6] = in->marginTopCollapsed;
    key[7] = in->positionedByCaller;
}

static void forgetLayout(CscdLayoutBox* b) {
    for (; b; b = b->parent) b->memoPass = 0;
}

static bool memoMatches(Layouter* L, CscdLayoutBox* b, const float key[8], const BlockInput* in) {
    if (!L->pass || b->memoPass != L->pass) return false;
    for (int i = 0; i < 8; ++i)
        if (i != 3 && key[i] != b->memoIn[i]) return false;
    if (key[3] == b->memoIn[3]) return true;
    /* laid out at its own height before, now at that height imposed */
    return b->memoIn[3] < 0 && in->forcedH >= 0 && fabsf(in->forcedH - b->memoH) < 0.01f && !heightSensitive(L, b);
}

static MarginSet layBlockFresh(Layouter* L, CscdLayoutBox* b, const BlockInput* in);

/* The style of a vertical box as its contents see it: its height as the line length, its edges
 * turned the way the painter turns it. */
static CscdComputedStyle* logicalStyle(const CscdComputedStyle* s) {
    CscdComputedStyle* l = cscdStyleClone(s);
    const bool clockwise = s->writingMode != CSCD_WRITING_SIDEWAYS_LR;
    l->width = s->height; l->height = s->width;
    l->minWidth = s->minHeight; l->minHeight = s->minWidth;
    l->maxWidth = s->maxHeight; l->maxHeight = s->maxWidth;
    for (int i = 0; i < 4; ++i) {
        const int p = clockwise ? (i + 1) % 4 : (i + 3) % 4; /* (the physical side shown as logical side i) */
        l->margin[i] = s->margin[p];
        l->padding[i] = s->padding[p];
        l->border[i] = s->border[p];
    }
    l->verticalRoot = false;
    return l;
}

static MarginSet layBlockFresh(Layouter* L, CscdLayoutBox* b, const BlockInput* in);

/* writing-mode: a vertical box in a horizontal flow is laid out in its own axes (its lines as long
 * as its height, or the viewport's), then given the size that shows turned: the painter turns it. */
static MarginSet layVertical(Layouter* L, CscdLayoutBox* b, const BlockInput* in) {
    CscdComputedStyle* phys = b->style;
    CscdComputedStyle* logi = logicalStyle(phys);
    BlockInput li = *in;
    li.cbW = in->cbH >= 0 ? in->cbH : L->vp.height;
    li.cbH = in->cbW;
    li.forcedW = in->forcedH;
    li.forcedH = in->forcedW;
    li.shrinkToFit = cscdLenIsAuto(phys->height);
    li.availW = li.cbW;
    li.floats = NULL;
    li.marginTopCollapsed = false;
    li.positionedByCaller = true;
    b->style = logi;
    layBlockFresh(L, b, &li);
    const float logicalX = b->x, logicalY = b->y, lw = b->w, lh = b->h;
    b->style = phys;
    cscdRelease(logi);
    layResolveEdges(b, in->cbW);
    /* the physical box: as wide as the lines are deep, as tall as they are long */
    const float edgesW = b->padding[1] + b->padding[3] + b->border[1] + b->border[3];
    float w = lh;
    if (in->forcedW >= 0) w = in->forcedW;
    else if (!cscdLenIsAuto(phys->width)) {
        const float cw = cscdLenResolve(phys->width, in->cbW, 0);
        w = phys->boxSizing == CSCD_BOX_CONTENT ? cw + edgesW : cscdMaxf(cw, edgesW);
    }
    b->w = layClampWidth(L, b, w, in->cbW);
    b->h = in->forcedH >= 0 ? in->forcedH : lw;
    b->x = in->positionedByCaller ? in->x : in->x + cscdLenResolve(phys->margin[3], in->cbW, 0);
    b->y = in->y;
    b->logicalDX = logicalX - b->x;
    b->logicalDY = logicalY - b->y;
    b->overflow = cscdRect(b->x, b->y, b->w, b->h);
    const float mb = b->margin[2];
    MarginSet out = {cscdMaxf(mb, 0.f), cscdMinf(mb, 0.f)};
    return out;
}

MarginSet layBlock(Layouter* L, CscdLayoutBox* b, const BlockInput* in) {
    float key[8];
    memoKey(b, in, key);
    if (memoMatches(L, b, key, in)) {
        /* the same layout, at another place: the box is moved there (from wherever it was put since) */
        const float x = b->memoX + in->x - b->memoInX, y = b->memoY + in->y - b->memoInY;
        if (x != b->x || y != b->y) {
            forgetLayout(b->parent);
            cscdBoxShift(b, x - b->x, y - b->y);
        }
        if (in->forcedH >= 0) b->h = in->forcedH;
        b->memoInX = in->x;
        b->memoInY = in->y;
        b->memoX = x;
        b->memoY = y;
        MarginSet out = {b->memoOut[0], b->memoOut[1]};
        return out;
    }
    forgetLayout(b->parent); /* (what holds it changes with it) */
    b->memoPass = 0;
    /* what it lays out must not escape it for the layout to be kept: positioned boxes waiting for a
     * containing block further out (or fixed ones), a subgrid's tracks */
    const int outer = L->deferredTo, ownList = (int)L->absStack.len; /* (the list it pushes, as a containing block) */
    L->deferredTo = INT_MAX;
    const MarginSet out = b->style && b->style->verticalRoot && b->kind != CSCD_BOX_INLINE && b->kind != CSCD_BOX_TEXT ? layVertical(L, b, in) : layBlockFresh(L, b, in);
    const bool escaped = L->deferredTo < ownList;
    if (L->deferredTo > outer) L->deferredTo = outer;
    if (L->pass && b->memoBfc && !escaped && !cscdU64MapGet(&L->subgridIn, (uint64_t)(uintptr_t)b)) {
        b->memoPass = L->pass;
        memcpy(b->memoIn, key, sizeof b->memoIn);
        b->memoInX = in->x;
        b->memoInY = in->y;
        b->memoX = b->x;
        b->memoY = b->y;
        b->memoH = b->h;
        b->memoOut[0] = out.pos;
        b->memoOut[1] = out.neg;
    }
    return out;
}

static MarginSet layBlockFresh(Layouter* L, CscdLayoutBox* b, const BlockInput* in) {
    const CscdComputedStyle* s = b->style;
    layResolveEdges(b, in->cbW);
    b->hasBaseline = false;
    b->lines.len = 0;
    bool mlAuto = cscdLenIsAuto(s->margin[3]), mrAuto = cscdLenIsAuto(s->margin[1]);
    float edgesW = b->padding[1] + b->padding[3] + b->border[1] + b->border[3];
    float edgesH = b->padding[0] + b->padding[2] + b->border[0] + b->border[2];
    bool replaced = b->kind == CSCD_BOX_REPLACED;
    float replW = 0, replH = 0;
    float bw;
    if (replaced) {
        layReplaced(L, b, in->cbW, in->cbH, &replW, &replH);
        bw = in->forcedW >= 0 ? in->forcedW : replW + edgesW;
        if (in->forcedW >= 0) replW = in->forcedW - edgesW;
    } else if (in->forcedW >= 0) {
        bw = in->forcedW;
    } else {
        bool def;
        float sw = laySpecifiedWidth(L, b, in->cbW, &def);
        if (def) bw = sw;
        else if (in->shrinkToFit || b->kind == CSCD_BOX_TABLE) {
            float avail = (in->availW >= 0 ? in->availW : in->cbW) - b->margin[1] - b->margin[3];
            bw = cscdMinf(cscdMaxf(layMinContent(L, b), avail), layMaxContent(L, b));
        } else bw = in->cbW - b->margin[1] - b->margin[3];
        bw = layClampWidth(L, b, bw, in->cbW);
        /* aspect-ratio with a definite height */
        bool hd;
        float sh = laySpecifiedHeight(b, in->cbH, &hd);
        if (!def && hd && s->aspectRatio > 0) bw = layClampWidth(L, b, (sh - edgesH) * s->aspectRatio + edgesW, in->cbW);
    }
    if (!in->shrinkToFit && in->forcedW < 0 && !in->positionedByCaller && !b->atomicInline) {
        float remaining = in->cbW - bw - b->margin[1] - b->margin[3];
        if (mlAuto && mrAuto) { float m = cscdMaxf(0.f, remaining) / 2; b->margin[3] = m; b->margin[1] = m; }
        else if (mlAuto) b->margin[3] = cscdMaxf(0.f, remaining) + 0;
        else if (mrAuto) b->margin[1] = remaining;
    }
    b->w = bw;
    b->x = in->positionedByCaller ? in->x : in->x + b->margin[3];
    b->y = in->y;
    /* over-constrained: the containing block's direction says which margin gives way (CSS 2 10.3.3) */
    const bool cbRtl = b->parent && b->parent->style ? b->parent->style->rtl : s->rtl;
    if (cbRtl && !in->positionedByCaller && in->forcedW < 0 && !mlAuto && !mrAuto && !in->shrinkToFit) b->x = in->x + in->cbW - b->margin[1] - bw;

    bool bfc = establishesBFC(b, in);
    b->memoBfc = bfc;
    FloatCtx own;
    memset(&own, 0, sizeof own);
    FloatCtx* fc = bfc ? &own : in->floats;
    if (!fc) fc = &own;

    bool hdef;
    float sh = laySpecifiedHeight(b, in->cbH, &hdef);
    if (!hdef && s->aspectRatio > 0 && !replaced && in->forcedH < 0) { sh = (bw - edgesW) / s->aspectRatio + edgesH; hdef = true; }
    float childCbH = in->forcedH >= 0 ? in->forcedH - edgesH : hdef ? layClampHeight(b, sh, in->cbH) - edgesH : -1;
    /* percentages of a definite min-height do not resolve, but a percentage height chain from the root does */
    b->h = in->forcedH >= 0 ? in->forcedH : hdef ? layClampHeight(b, sh, in->cbH) : edgesH;

    bool cb = layIsContainingBlock(b);
    if (cb) cscdVecPushZero(&L->absStack);

    MarginSet trailing = {0, 0};
    float contentH = 0;
    if (replaced) {
        contentH = in->forcedH >= 0 ? in->forcedH - edgesH : replH;
        if (b->replaced == CSCD_REPL_TEXT_INPUT || b->replaced == CSCD_REPL_SELECT) {
            CscdFont f = cscdFontForStyle(s);
            b->font = f;
            float ch = in->forcedH >= 0 ? in->forcedH - edgesH : replH;
            b->baseline = b->y + b->border[0] + b->padding[0] + (ch - (cscdFontAscent(&f) + cscdFontDescent(&f))) / 2 + cscdFontAscent(&f);
            b->lastBaseline = b->baseline;
            b->hasBaseline = true;
        } else if (b->replaced == CSCD_REPL_CHECKBOX || b->replaced == CSCD_REPL_RADIO) {
            b->baseline = b->lastBaseline = b->y + replH + edgesH - 2;
            b->hasBaseline = true;
        }
    } else if (b->kind == CSCD_BOX_FLEX || b->kind == CSCD_BOX_GRID || b->kind == CSCD_BOX_TABLE || b->inlineContent) {
        /* flex and grid need the final border-box height known when it is definite */
        contentH = layContent(L, b, fc, childCbH);
    } else {
        contentH = layBlockChildren(L, b, fc, childCbH, &trailing, in->marginTopCollapsed && topCollapsible(b));
    }
    bool bottomCollapsible = !bfc && b->kind == CSCD_BOX_BLOCK && !replaced && b->border[2] == 0 && b->padding[2] == 0 && !hdef &&
                             in->forcedH < 0 && cscdLenIsAuto(s->minHeight) && !b->inlineContent;
    if (!bottomCollapsible) contentH = cscdMaxf(0.f, contentH + msValue(trailing));
    if (bfc && !hdef && in->forcedH < 0) {
        float fb = fcBottom(fc);
        if (fb > -1e29f) contentH = cscdMaxf(contentH, fb - cscdBoxContentY(b));
    }
    cscdVecFree(&own.floats);
    float bh;
    if (in->forcedH >= 0) bh = in->forcedH;
    else if (hdef) bh = layClampHeight(b, sh, in->cbH);
    else bh = layClampHeight(b, (s->containerType == 2 || (s->contain & CSCD_CONTAIN_SIZE) ? 0 : contentH) + edgesH, in->cbH); /* (size containment) */
    b->h = bh;

    /* vertical centering of button content (as browsers do) */
    if ((b->element && cscdElementIs(b->element, "button")) || (b->element && cscdElementIs(b->element, "input") && !replaced && !b->pseudo.len)) {
        float inner = cscdBoxContentH(b);
        if (inner > contentH + 0.5f) {
            float d = (inner - contentH) / 2;
            for (size_t i = 0; i < b->children.len; ++i) cscdBoxShift(cscdBoxChild(b, i), 0, d);
            if (b->hasBaseline) { b->baseline += d; b->lastBaseline += d; }
            for (size_t i = 0; i < b->lines.len; ++i) { b->lines.items[i].top += d; b->lines.items[i].baseline += d; }
        }
    }
    /* outside list markers */
    for (size_t i = 0; i < b->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(b, i);
        if (!c->isMarker) continue;
        CscdFont f = cscdFontForStyle(c->style);
        c->font = f;
        float w = layMeasure(&f, cscdStrSv(&c->text), 0, 0);
        float base = b->hasBaseline ? b->baseline : cscdBoxContentY(b) + cscdFontAscent(&f);
        CscdTextFrag fr;
        memset(&fr, 0, sizeof fr);
        fr.text = cscdStrCopy(&c->text);
        fr.width = w;
        fr.x = cscdBoxContentX(b) - w;
        fr.baseline = base;
        fr.top = base - cscdFontAscent(&f);
        fr.height = cscdFontAscent(&f) + cscdFontDescent(&f);
        cscdTextFragVecClear(&c->textFrags);
        cscdVecPush(&c->textFrags, fr);
        c->x = fr.x; c->y = fr.top; c->w = w; c->h = fr.height;
    }

    if (cb) {
        CscdPtrVec list = cscdVecPop(&L->absStack);
        CscdRect pb = cscdBoxPaddingBox(b);
        for (size_t i = 0; i < list.len; ++i) layAbsolute(L, (CscdLayoutBox*)list.items[i], pb);
        cscdVecFree(&list);
    }
    layComputeOverflow(b);
    if (in->forcedW < 0 || s->position == CSCD_POS_RELATIVE) layApplyRelative(b, in->cbW, in->cbH);

    MarginSet out = {0, 0};
    msAdd(&out, b->margin[2]);
    if (bottomCollapsible) msAddSet(&out, trailing);
    return out;
}

void layAbsolute(Layouter* L, CscdLayoutBox* b, CscdRect cb) {
    const CscdComputedStyle* s = b->style;
    layResolveEdges(b, cb.w);
    bool lA = cscdLenIsAuto(s->left), rA = cscdLenIsAuto(s->right), tA = cscdLenIsAuto(s->top), bA = cscdLenIsAuto(s->bottom);
    float Lf = lA ? 0 : cscdLenResolve(s->left, cb.w, 0), R = rA ? 0 : cscdLenResolve(s->right, cb.w, 0);
    float T = tA ? 0 : cscdLenResolve(s->top, cb.h, 0), B = bA ? 0 : cscdLenResolve(s->bottom, cb.h, 0);
    bool mlAuto = cscdLenIsAuto(s->margin[3]), mrAuto = cscdLenIsAuto(s->margin[1]), mtAuto = cscdLenIsAuto(s->margin[0]), mbAuto = cscdLenIsAuto(s->margin[2]);
    float edgesW = b->padding[1] + b->padding[3] + b->border[1] + b->border[3];
    bool wd;
    float w = laySpecifiedWidth(L, b, cb.w, &wd);
    BlockInput in = blockInput();
    in.cbW = cb.w;
    in.cbH = cb.h;
    in.positionedByCaller = true;
    if (b->kind == CSCD_BOX_REPLACED) {
        float cw, ch;
        layReplaced(L, b, cb.w, cb.h, &cw, &ch);
        w = cw + edgesW;
        wd = true;
    }
    if (!wd) {
        if (!lA && !rA) w = cb.w - Lf - R - b->margin[1] - b->margin[3];
        else {
            float avail = cb.w - (lA ? 0 : Lf) - (rA ? 0 : R) - b->margin[1] - b->margin[3];
            w = cscdMinf(cscdMaxf(layMinContent(L, b), avail), layMaxContent(L, b));
        }
        w = layClampWidth(L, b, w, cb.w);
    }
    in.forcedW = layClampWidth(L, b, w, cb.w);
    bool hd;
    float h = laySpecifiedHeight(b, cb.h, &hd);
    if (!hd && !tA && !bA) { h = cb.h - T - B - b->margin[0] - b->margin[2]; hd = true; }
    if (hd) in.forcedH = layClampHeight(b, h, cb.h);
    layBlock(L, b, &in);
    /* horizontal */
    float x;
    if (!lA && !rA && (mlAuto || mrAuto)) {
        float freeW = cb.w - Lf - R - b->w;
        float ml = mlAuto && mrAuto ? cscdMaxf(0.f, freeW / 2) : mlAuto ? freeW - b->margin[1] : b->margin[3];
        x = cb.x + Lf + ml;
    } else if (!lA) x = cb.x + Lf + b->margin[3];
    else if (!rA) x = cscdRectRight(cb) - R - b->margin[1] - b->w;
    else x = (b->hasStaticPos ? b->staticX : cb.x) + b->margin[3];
    float y;
    if (!tA && !bA && (mtAuto || mbAuto)) {
        float freeH = cb.h - T - B - b->h;
        float mt = mtAuto && mbAuto ? cscdMaxf(0.f, freeH / 2) : mtAuto ? freeH - b->margin[2] : b->margin[0];
        y = cb.y + T + mt;
    } else if (!tA) y = cb.y + T + b->margin[0];
    else if (!bA) y = cscdRectBottom(cb) - B - b->margin[2] - b->h;
    else y = (b->hasStaticPos ? b->staticY : cb.y) + b->margin[0];
    /* (its static position stays where the flow put it: the box is laid out from it again on the next pass) */
    const float sx = b->staticX, sy = b->staticY;
    cscdBoxShift(b, x - b->x, y - b->y);
    b->staticX = sx;
    b->staticY = sy;
}

/* What is placed against the viewport was laid out after the boxes it sits in had their overflow: it
 * goes into the page's here, unless the root or the body clips the page. (Not fixed boxes: scrolling
 * does not reach them.) */
static void addToPageOverflow(CscdLayoutBox* root, const CscdPtrVec* placed) {
    if (root->style && cscdStyleClipsOverflow(root->style)) return;
    for (size_t i = 0; i < root->children.len; ++i) {
        const CscdLayoutBox* c = cscdBoxChild(root, i);
        if (c->style && cscdStyleClipsOverflow(c->style)) return;
    }
    for (size_t i = 0; i < placed->len; ++i) {
        const CscdLayoutBox* b = (const CscdLayoutBox*)placed->items[i];
        CscdRect r = cscdBoxBorderBox(b);
        if (!(b->style && cscdStyleClipsOverflow(b->style))) r = cscdRectUnite(r, b->overflow);
        root->overflow = cscdRectUnite(root->overflow, r);
    }
}

void layRoot(Layouter* L, CscdLayoutBox* root) {
    static CSCD_THREAD_LOCAL uint32_t passes;
    if (!++passes) ++passes; /* (0 stays "none") */
    L->pass = passes;
    L->deferredTo = INT_MAX;
    for (size_t i = 0; i < L->absStack.len; ++i) cscdVecFree(&L->absStack.items[i]);
    L->absStack.len = 0;
    L->fixedBoxes.len = 0;
    cscdVecPushZero(&L->absStack);
    BlockInput in = blockInput();
    in.cbW = L->vp.width;
    in.cbH = L->vp.height;
    /* the root's height percentages resolve against the viewport */
    layBlock(L, root, &in);
    CscdRect icb = cscdRect(0, 0, L->vp.width, L->vp.height);
    CscdPtrVec list = cscdVecPop(&L->absStack);
    for (size_t i = 0; i < list.len; ++i) layAbsolute(L, (CscdLayoutBox*)list.items[i], icb);
    for (size_t i = 0; i < L->fixedBoxes.len; ++i) layAbsolute(L, (CscdLayoutBox*)L->fixedBoxes.items[i], icb);
    layComputeOverflow(root);
    addToPageOverflow(root, &list);
    cscdVecFree(&list);
}

static void freeLayouter(Layouter* L) {
    for (size_t i = 0; i < L->absStack.len; ++i) cscdVecFree(&L->absStack.items[i]);
    cscdVecFree(&L->absStack);
    cscdVecFree(&L->fixedBoxes);
    for (CscdU64MapSlot* s = NULL; (s = cscdU64MapNext(&L->subgridIn, s));) {
        SubgridTracks* t = (SubgridTracks*)s->val.p;
        cscdVecFree(&t->cols);
        cscdVecFree(&t->rows);
        cscdFree(t);
    }
    cscdU64MapFree(&L->subgridIn);
    for (CscdU64MapSlot* s = NULL; (s = cscdU64MapNext(&L->subgridRowsOut, s));) {
        cscdVecFree((CscdFloatVec*)s->val.p);
        cscdFree(s->val.p);
    }
    cscdU64MapFree(&L->subgridRowsOut);
    clearCounters(L);
    cscdStrMapFree(&L->counters);
}

CscdLayoutBox* cscdLayoutDocument(CscdDocument* doc, const CscdStyleEngine* styles, CscdViewport vp, CscdImageLoaderFn images, void* imagesUser) {
    Layouter L;
    memset(&L, 0, sizeof L);
    L.styles = styles;
    L.vp = vp;
    L.images = images;
    L.imagesUser = imagesUser;
    CscdLayoutBox* root = layBuild(&L, doc);
    if (root) layRoot(&L, root);
    freeLayouter(&L);
    return root;
}
