/* What the font sources share: a face's tables, the OpenType layout engine (GSUB, GPOS, GDEF),
 * color glyphs (COLR, CPAL, CBDT, sbix) and variations (fvar, avar, gvar, HVAR). Not installed. */
#ifndef CASCADE_FONT_INTERNAL_H
#define CASCADE_FONT_INTERNAL_H

#include "cascade/font.h"
#include "cascade/canvas.h"
#include "core/thread.h"

static inline uint16_t u16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline int16_t s16(const uint8_t* p) { return (int16_t)u16(p); }
static inline uint32_t u32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint32_t tag(const char* t) { return ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | (uint32_t)t[3]; }

typedef struct KernTable { uint32_t pairs; uint16_t count; } KernTable;
typedef struct CscdFontAxis { uint32_t tag; float min, def, max; } CscdFontAxis;

struct CscdFontFaceImpl {
    bool cffOk;
    uint32_t cffCharStrings, cffGlobalSubrs, cffFdSelect;
    CscdU32Vec cffLocalSubrs;
    void* data; /* the owner of the bytes (shared) */
    const uint8_t* base;
    size_t size;
    uint32_t glyf, loca, hmtx, cmap, kern, gpos, gsub, gdef;
    uint32_t locaLen;
    uint32_t cmapSub;
    int cmapFormat;
    int numHMetrics, locaFormat;
    bool cff;
    CSCD_VEC(KernTable) kernTables; /* kern format 0 subtables: where their sorted pairs are, how many */
    CscdU64Map kernPairs;           /* (pairs of a subtable that is not sorted) */
    CscdU32Vec pairTables;          /* GPOS pair adjustment subtables */
    CscdU32Vec tnumSubs;            /* GSUB subtables of tabular figures, in lookup order */
    CscdU64Map cmapCache;
    CscdMutex cacheMutex;
    /* color glyphs */
    uint32_t colr, cbdt, cblc, sbix;
    CscdU32Vec palette; /* CPAL's first palette, straight ARGB */
    CscdStrMap bitmaps; /* "glyph/strike" -> CscdImage* (a reference, NULL: none), decoded once */
    /* variations */
    CscdFontAxis* axes;
    int axisCount;
    uint32_t avar, gvar, hvar, mvar;
    CscdPtrVec instances; /* CscdFontFace*, owned: the instances made of this face */
    /* the OpenType layout plans made (see fontOt.c) */
    CscdStrMap plans;
};

/* Whether [off, off + len) lies in the font. */
static inline bool within(const CscdFontFaceImpl* m, uint32_t off, uint32_t len) { return off <= m->size && len <= m->size - off; }
/* count items of each bytes at off, multiplied in 64 bits: a 32-bit count from the font times an
 * item's size can wrap to a small length that within() would pass. Item i exists: count = i + 1. */
static inline bool withinItems(const CscdFontFaceImpl* m, uint32_t off, uint64_t count, uint32_t each) { return off <= m->size && count * each <= m->size - off; }

uint32_t cscdFontFindTable(const uint8_t* data, size_t size, uint32_t offset, const char* name, uint32_t* length);
/* A Coverage / ClassDef table with avail bytes after it in the font (its arrays are read no further). */
int cscdOtCoverage(const uint8_t* cov, size_t avail, uint16_t glyph);
int cscdOtClass(const uint8_t* cd, size_t avail, uint16_t glyph);
/* The same, at an offset in the font: -1 (not covered) or class 0 when it is not in the font. */
static inline int otCoverageAt(const CscdFontFaceImpl* m, uint32_t off, uint16_t glyph) { return within(m, off, 4) ? cscdOtCoverage(m->base + off, m->size - off, glyph) : -1; }
static inline int otClassAt(const CscdFontFaceImpl* m, uint32_t off, uint16_t glyph) { return within(m, off, 4) ? cscdOtClass(m->base + off, m->size - off, glyph) : 0; }

/* ---- the OpenType layout engine (fontOt.c) ---------------------------------------------------- */

/* A glyph being shaped. Positions are font units; x and y offsets move the glyph from where the
 * advances put it (y up). */
typedef struct CscdOtGlyph {
    uint16_t glyph;
    uint32_t cluster;    /* the first character it stands for (index in the run) */
    uint32_t mask;       /* the features that may apply to it (bits of the plan's features) */
    uint8_t gclass;      /* GDEF class: 1 base, 2 ligature, 3 mark, 4 component (0: unknown) */
    uint8_t ligComponents;
    bool ignorable;      /* a default ignorable character (a variation selector...): matching passes over it */
    int32_t xAdvance, xOffset, yOffset;
    int32_t attach;      /* a mark attached to another glyph: how far back it is (0: none) */
} CscdOtGlyph;
typedef CSCD_VEC(CscdOtGlyph) CscdOtBuffer;

/* A feature asked for: its tag, the value (0 off, 1 on, n: the n-th alternate) and the glyphs it
 * applies to (a mask; ~0u: all). */
typedef struct CscdOtFeature { uint32_t tag; int value; uint32_t mask; } CscdOtFeature;

/* Substitutions (GSUB) and positions (GPOS) of the features, for the script (an OpenType script tag,
 * 'DFLT' and 'latn' tried when the font has none for it). The buffer's glyphs come in with their
 * advances (xAdvance) set. */
void cscdOtSubstitute(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const CscdOtFeature* features, size_t n);
void cscdOtPosition(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const CscdOtFeature* features, size_t n);
/* Indic scripts (fontIndic.c): the syllables reordered and the features applied in their stages;
 * cps: the characters, by cluster. False (nothing done) for other scripts. */
bool cscdIndicShape(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const uint32_t* cps, const CscdOtFeature* features, size_t n);
/* The GDEF class of a glyph (0 when the font says nothing). */
int cscdOtGlyphClass(const CscdFontFace* face, uint16_t glyph);
bool cscdOtHasPositioning(const CscdFontFace* face);
/* Whether the font's GSUB has a feature for the script. */
bool cscdOtHasFeature(const CscdFontFace* face, uint32_t script, uint32_t feature);
void cscdOtFree(CscdFontFaceImpl* m);

/* ---- glyf outlines (font.c) ------------------------------------------------------------------- */

typedef struct CscdGlyfPoints {
    CscdPoint* pts;
    uint8_t* flags;  /* bit 0: on the curve */
    uint16_t* ends;  /* each contour's last point */
    int n, nContours;
} CscdGlyfPoints;
const uint8_t* cscdGlyfData(const CscdFontFace* face, uint16_t glyph, uint32_t* len);
bool cscdGlyfPoints(const uint8_t* g, uint32_t len, CscdGlyfPoints* out);
void cscdGlyfPointsFree(CscdGlyfPoints* p);
void cscdGlyfEmit(const CscdGlyfPoints* p, const float* m /* a, b, c, d, e, f */, CscdPath* out);

/* ---- color glyphs and variations (fontColor.c, fontVar.c) ------------------------------------ */

void cscdFontColorInit(CscdFontFace* face, uint32_t offset);
void cscdFontColorFree(CscdFontFaceImpl* m);
void cscdFontVarInit(CscdFontFace* face, uint32_t offset);
void cscdFontVarFree(CscdFontFaceImpl* m);
/* The outline of a glyf glyph with the face's variation applied (face->coords set). */
bool cscdFontVarOutline(const CscdFontFace* face, uint16_t glyph, CscdPath* out);
/* The advance's change at the face's variation, font units. */
float cscdFontVarAdvanceDelta(const CscdFontFace* face, uint16_t glyph);

#endif
