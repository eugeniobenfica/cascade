/* cascade — fonts: a TrueType / OpenType (glyf and CFF outlines, WOFF) parser with cmap and
 * hmtx, OpenType layout (GSUB substitutions, GPOS kerning and mark positioning), color glyphs (COLR,
 * CBDT, sbix), variable fonts (fvar, avar, gvar, HVAR), system font discovery and fallback. */
#ifndef CASCADE_FONT_H
#define CASCADE_FONT_H

#include "cascade/core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CscdPath CscdPath;
typedef struct CscdImage CscdImage;
typedef struct CscdFontFaceImpl CscdFontFaceImpl;

/* One face of a font file (a .ttc holds several). The font manager owns the faces it loads. */
typedef struct CscdFontFace {
    CscdStr family, subfamily;
    int weight;
    bool italic;
    float unitsPerEm;
    float ascender, descender, lineGap; /* font units (descender negative) */
    float xHeight, capHeight;
    float underlinePos, underlineThickness, strikePos;
    int numGlyphs;
    CscdFontFaceImpl* impl; /* (the tables) */
    /* an instance of a variable font (cscdFontFaceVary): the normalized position on each of the
     * font's axes, and the face it varies; NULL for a face as it is in the file */
    const float* coords;
    struct CscdFontFace* varies;
} CscdFontFace;

/* A face of the font in data (kept alive by owner, a shared block that cscdRelease frees: a mapped
 * file, a buffer); NULL when it is not a font this reads. The caller owns it (cscdFontFaceFree). */
CscdFontFace* cscdFontFaceLoad(void* owner, const uint8_t* data, size_t size, int faceIndex);
void cscdFontFaceFree(CscdFontFace* f);
uint16_t cscdFontFaceGlyphIndex(const CscdFontFace* f, uint32_t codepoint);
float cscdFontFaceAdvance(const CscdFontFace* f, uint16_t glyph);              /* font units */
float cscdFontFaceKerning(const CscdFontFace* f, uint16_t left, uint16_t right); /* font units */
/* Glyph outline in font units, y up, appended to out. Returns false for empty glyphs. */
bool cscdFontFaceOutline(const CscdFontFace* f, uint16_t glyph, CscdPath* out);
CSCD_INLINE bool cscdFontFaceHasGlyph(const CscdFontFace* f, uint32_t cp) { return cscdFontFaceGlyphIndex(f, cp) != 0; }

/* Color glyphs. A layered glyph (COLR) is outlines painted in turn, each in a palette color or the
 * text's color; a bitmap glyph (CBDT, sbix) is a picture. */
bool cscdFontFaceHasColor(const CscdFontFace* f);
typedef struct CscdColorLayer { uint16_t glyph; bool foreground; uint32_t argb; /* (straight alpha) */ } CscdColorLayer;
/* The layers of glyph, bottom first (at most max written): how many it has, 0 when it is not layered. */
size_t cscdFontFaceColorLayers(const CscdFontFace* f, uint16_t glyph, CscdColorLayer* out, size_t max);
/* The picture of a bitmap glyph for text of size px (the strike nearest above it, shared: the face
 * keeps it), and where it goes in px at that size from the pen, y down; NULL when glyph has none. */
const CscdImage* cscdFontFaceGlyphImage(const CscdFontFace* f, uint16_t glyph, float size, CscdRect* where);

/* Variable fonts: the face's axes (tag, smallest, default and largest value). */
int cscdFontFaceAxisCount(const CscdFontFace* f);
bool cscdFontFaceAxis(const CscdFontFace* f, int i, uint32_t* tag, float* min, float* def, float* max);
/* An instance at the given axis values (by tag; axes not given stay at their defaults). The face
 * keeps its instances: the same values give the same face. f itself when it has no axes. */
typedef struct CscdFontTagValue { uint32_t tag; float value; } CscdFontTagValue;
CscdFontFace* cscdFontFaceVary(CscdFontFace* f, const CscdFontTagValue* values, size_t n);
CSCD_INLINE uint32_t cscdFontTag(const char* t) { return ((uint32_t)(uint8_t)t[0] << 24) | ((uint32_t)(uint8_t)t[1] << 16) | ((uint32_t)(uint8_t)t[2] << 8) | (uint8_t)t[3]; }

/* What CSS asks of the font beyond the face and the size: OpenType features (font-feature-settings
 * and the font-variant-* properties, in that order of precedence), axis values
 * (font-variation-settings), kerning, optical sizing and the width (font-stretch). Interned:
 * cscdFontSettingsIntern returns one shared, unchanging copy for each different value (NULL for
 * the defaults), so that two are equal when their pointers are. */
typedef struct CscdFontSettings {
    uint8_t featureCount, variationCount;
    CscdFontTagValue features[24];
    CscdFontTagValue variations[8];
    bool noKerning;       /* font-kerning: none */
    bool noOpticalSizing; /* font-optical-sizing: none */
    float stretch;        /* percent; 0 or 100: normal */
    bool synthesizeSmallCaps; /* small capitals asked for: made from capitals when the font has none */
} CscdFontSettings;
const CscdFontSettings* cscdFontSettingsIntern(const CscdFontSettings* s);

/* A face at a size, with synthetic bold/italic when the family lacks them. */
typedef struct CscdFont {
    CscdFontFace* face;
    float size; /* px */
    bool fakeBold, fakeItalic;
    bool tabularNums; /* digits 0-9 all as wide as the widest (the font's tabular figures when it has them) */
    bool noLigatures; /* (font-variant-ligatures: none) */
    const CscdFontSettings* settings; /* (interned; NULL: the defaults) */
} CscdFont;

CSCD_INLINE CscdFont cscdFontDefault(void) { CscdFont f = {NULL, 16, false, false, false, false, NULL}; return f; }
CSCD_INLINE float cscdFontScale(const CscdFont* f) { return f->face ? f->size / f->face->unitsPerEm : 0; }
CSCD_INLINE float cscdFontAscent(const CscdFont* f) { return f->face ? f->face->ascender * cscdFontScale(f) : f->size * 0.8f; }
CSCD_INLINE float cscdFontDescent(const CscdFont* f) { return f->face ? -f->face->descender * cscdFontScale(f) : f->size * 0.2f; } /* positive */
CSCD_INLINE float cscdFontLineGap(const CscdFont* f) { return f->face ? f->face->lineGap * cscdFontScale(f) : 0; }
CSCD_INLINE float cscdFontXHeight(const CscdFont* f) { return f->face ? f->face->xHeight * cscdFontScale(f) : f->size * 0.5f; }
CSCD_INLINE bool cscdFontEq(const CscdFont* a, const CscdFont* b) {
    return a->face == b->face && a->size == b->size && a->fakeBold == b->fakeBold && a->fakeItalic == b->fakeItalic &&
           a->tabularNums == b->tabularNums && a->noLigatures == b->noLigatures && a->settings == b->settings;
}
/* Advance of a code point, with fallback faces for missing glyphs. */
float cscdFontAdvance(const CscdFont* f, uint32_t cp);

typedef struct CscdShapedGlyph {
    uint16_t glyph;
    CscdFontFace* face; /* primary or a fallback */
    float x;          /* where the glyph is drawn, relative to the run start, px */
    float y;          /* and how far below the baseline (marks placed by GPOS), px */
    float advance;    /* px */
    uint32_t cluster; /* byte offset in the source text */
    float scale;      /* drawn at this part of the font's size (synthesized small capitals); 0: 1 */
} CscdShapedGlyph;
typedef CSCD_VEC(CscdShapedGlyph) CscdShapedGlyphVec;

/* ---- the font manager (one per program, safe to use from any thread) ---- */

/* Picks the best face for a CSS family list, weight and style. */
CscdFont cscdFontMatch(const CscdStrVec* families, float size, int weight, bool italic);
/* Face with a glyph for cp, for fallback. */
CscdFontFace* cscdFontFallback(uint32_t cp, int weight, bool italic, const CscdFontFace* exclude);
/* A color (emoji) face with a glyph for cp. */
CscdFontFace* cscdFontFallbackEmoji(uint32_t cp);
/* The font with CSS's settings: the instance of a variable face for the weight, width, size and
 * axis values asked for (synthetic bold dropped when its weight axis reaches the weight). */
CscdFont cscdFontWithSettings(CscdFont f, int weight, const CscdFontSettings* s);
/* Registers a font file (from @font-face), under the given family. weightMax 0: weight. */
bool cscdFontAdd(CscdSv family, CscdSv path, int weight, bool italic, int weightMax);
/* The same with the file's bytes (copied). */
bool cscdFontAddData(CscdSv family, const uint8_t* data, size_t size, int weight, bool italic);
CscdFontFace* cscdFontLoad(CscdSv path, int index);
/* Maps text to glyphs with kerning, letter- and word-spacing, falling back per character (out is
 * replaced). */
void cscdFontShape(CscdSv text, const CscdFont* font, float letterSpacing, float wordSpacing, CscdShapedGlyphVec* out);
/* Arabic letters in the forms their neighbours ask for (Unicode presentation forms): out gets the
 * text, map the offset in text of each byte of out. False (nothing written) when text has none. */
bool cscdArabicJoin(CscdSv text, CscdStr* out, CscdU32Vec* map);
bool cscdFontFaceHasTabularFigures(const CscdFontFace* face);
uint16_t cscdFontFaceTabularGlyph(const CscdFontFace* face, uint16_t glyph);
float cscdFontMeasure(CscdSv text, const CscdFont* font, float letterSpacing, float wordSpacing);
/* Right-to-left text, in its logical order: shaped as such (mirrored characters mirrored), the
 * glyphs out left to right as they show. */
void cscdFontShapeRtl(CscdSv text, const CscdFont* font, float letterSpacing, float wordSpacing, CscdShapedGlyphVec* out);
void cscdFontAddDirectory(CscdSv dir);
/* Changes whenever fonts are added (a font matched before may not be the best one any more). */
uint32_t cscdFontGeneration(void);

#ifdef __cplusplus
}
#endif

#endif
