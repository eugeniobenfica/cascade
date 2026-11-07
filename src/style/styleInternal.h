/* Internal helpers shared by the style sources (styleValues.c, properties.c, cascade.c). */
#ifndef CASCADE_STYLE_INTERNAL_H
#define CASCADE_STYLE_INTERNAL_H

#include "cascade/dom.h"
#include "cascade/style.h"

typedef struct CscdValueCtx {
    float em;            /* font size of the element (parent's for font-size itself) */
    float rem;
    float vw, vh;
    bool quirkUnitless;  /* unitless numbers accepted as px (presentational attributes) */
    bool* viewport;      /* set when a viewport unit (vw, vh...) was resolved */
    float cqw, cqh;      /* the query container's content box (-1: none, the viewport then) */
    bool* container;     /* set when a container unit (cqw, cqi...) was resolved */
    float zoom;          /* the element's zoom, all ancestors' together (absolute lengths are multiplied by it) */
} CscdValueCtx;

CSCD_INLINE CscdValueCtx cscdValueCtx(void) {
    CscdValueCtx c = {16, 16, 1024, 768, false, NULL, -1, -1, NULL, 1};
    return c;
}

float cscdAbsoluteLength(float v, CscdSv unit, const CscdValueCtx* c, bool* ok);
bool cscdParseLength(const CscdCssValue* v, const CscdValueCtx* c, CscdLength* out, bool allowKeywords);
bool cscdParseNumber(const CscdCssValue* v, float* out, const CscdValueCtx* c);
bool cscdParseAngle(const CscdCssValue* v, float* rad);
bool cscdParseAngleUnit(float v, CscdSv unit, float* rad);
float cscdLengthPx(const CscdCssValue* v, const CscdValueCtx* c, float def);

/* Expands a shorthand into longhands (appended). Returns false when property is not a shorthand. */
bool cscdExpandShorthand(CscdSv property, CscdSv value, CscdStrPairVec* out);
bool cscdIsShorthand(CscdSv property); /* (what cscdExpandShorthand expands) */
bool cscdIsKnownProperty(CscdSv property); /* a property the engine computes (or a custom one) */
CscdSv cscdClosestProperty(CscdSv property); /* the known one a misspelling probably meant ("" when none is close) */
/* Maps vendor-prefixed and logical properties onto the ones the engine computes. */
CscdSv cscdCanonicalProperty(CscdSv property);

/* Applies one longhand. Returns false when the property or value is not understood. */
typedef struct CscdApplyState {
    CscdComputedStyle* s;
    const CscdComputedStyle* parent;
    const CscdStyleEnvironment* env;
    CscdSv baseDir;
    CscdValueCtx ctx;
    uint32_t colorSet; /* bits: border sides 0..3, outline 4, decoration 5, caret 6 */
    /* background longhands are combined into layers once all are known */
    CscdStr bgImage, bgPosition, bgSize, bgRepeat, bgClip, bgOrigin;
    bool bgTouched;
    /* mask layers, combined the same way */
    CscdStr maskImage, maskPosition, maskSize, maskRepeat;
    bool maskTouched;
    CscdStrMap* valueCache; /* (optional) value text -> its parsed values, kept by the caller */
} CscdApplyState;

void cscdApplyStateInit(CscdApplyState* st, CscdComputedStyle* s, const CscdComputedStyle* parent, const CscdStyleEnvironment* env, CscdSv baseDir);
void cscdApplyStateFini(CscdApplyState* st);
bool cscdApplyLonghand(CscdApplyState* st, CscdSv property, CscdSv value);
void cscdValueCacheClear(CscdStrMap* cache); /* (what a valueCache holds freed; the map stays) */
void cscdFinalizeStyle(CscdApplyState* st);
bool cscdCopyProperty(CscdComputedStyle* dst, const CscdComputedStyle* src, CscdSv property);

/* Parses a background-image layer list, used by background-image and by finalizeStyle. */
void cscdParseBackgrounds(CscdApplyState* st);
/* Parses <image> values (url / gradients) into a layer. */
bool cscdParseImageValue(const CscdCssValue* v, CscdApplyState* st, CscdBackgroundLayer* out);
bool cscdParseTrackList(const CscdCssValueVec* v, const CscdValueCtx* c, CscdTrackList* out);
bool cscdParseTrackSize(const CscdCssValue* v, const CscdValueCtx* c, CscdTrackSize* out);
bool cscdParseShadows(CscdSv value, CscdApplyState* st, CscdShadowVec* out, bool text);
bool cscdParseTiming(const CscdCssValue* v, float* x1, float* y1, float* x2, float* y2, int* steps);

/* The value of a calc() tree at a percentage basis. */
float cscdCalcEval(const CscdCalcNode* n, float basis);

#endif
