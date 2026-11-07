/* cascade — computed style: the cascade's result for one element, with every length in CSS pixels
 * or as a percentage still to be resolved by layout. */
#ifndef CASCADE_STYLE_H
#define CASCADE_STYLE_H

#include "cascade/css.h"
#include "cascade/font.h"
#include "cascade/image.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CscdDocument CscdDocument;
typedef struct CscdElement CscdElement;
typedef struct CscdCalcNode CscdCalcNode;
typedef struct CscdComputedStyle CscdComputedStyle;

/* A CSS length after computation: px + pct% (calc() of the two), or a keyword. */
typedef enum CscdLengthKind { CSCD_LEN_FIXED, CSCD_LEN_AUTO, CSCD_LEN_NONE, CSCD_LEN_MIN_CONTENT, CSCD_LEN_MAX_CONTENT, CSCD_LEN_FIT_CONTENT, CSCD_LEN_EXPR } CscdLengthKind;

typedef struct CscdLength {
    uint8_t kind; /* CscdLengthKind */
    float px;
    float pct;
    const CscdCalcNode* expr; /* min(), max(), clamp() mixing percentages (kept for the program's life) */
} CscdLength;

CSCD_INLINE CscdLength cscdLenFixed(float v) { CscdLength l = {CSCD_LEN_FIXED, v, 0, NULL}; return l; }
CSCD_INLINE CscdLength cscdLenPercent(float p) { CscdLength l = {CSCD_LEN_FIXED, 0, p, NULL}; return l; }
CSCD_INLINE CscdLength cscdLenAuto(void) { CscdLength l = {CSCD_LEN_AUTO, 0, 0, NULL}; return l; }
CSCD_INLINE CscdLength cscdLenNone(void) { CscdLength l = {CSCD_LEN_NONE, 0, 0, NULL}; return l; }
CSCD_INLINE CscdLength cscdLenKind(CscdLengthKind k) { CscdLength l = {(uint8_t)k, 0, 0, NULL}; return l; }
CSCD_INLINE bool cscdLenIsAuto(CscdLength l) { return l.kind == CSCD_LEN_AUTO; }
CSCD_INLINE bool cscdLenIsNone(CscdLength l) { return l.kind == CSCD_LEN_NONE; }
CSCD_INLINE bool cscdLenIsFixed(CscdLength l) { return l.kind == CSCD_LEN_FIXED && l.pct == 0; }
CSCD_INLINE bool cscdLenHasPercent(CscdLength l) { return (l.kind == CSCD_LEN_FIXED && l.pct != 0) || l.kind == CSCD_LEN_EXPR; }
CSCD_INLINE bool cscdLenIsIntrinsic(CscdLength l) { return l.kind == CSCD_LEN_MIN_CONTENT || l.kind == CSCD_LEN_MAX_CONTENT || l.kind == CSCD_LEN_FIT_CONTENT; }
/* Resolves against a percentage basis; auto/none resolve to def. */
float cscdLenResolve(CscdLength l, float basis, float def);
CSCD_INLINE bool cscdLenEq(CscdLength a, CscdLength b) { return a.kind == b.kind && a.px == b.px && a.pct == b.pct && a.expr == b.expr; }

typedef enum CscdDisplay {
    CSCD_DISPLAY_NONE, CSCD_DISPLAY_INLINE, CSCD_DISPLAY_BLOCK, CSCD_DISPLAY_INLINE_BLOCK, CSCD_DISPLAY_FLEX, CSCD_DISPLAY_INLINE_FLEX,
    CSCD_DISPLAY_GRID, CSCD_DISPLAY_INLINE_GRID, CSCD_DISPLAY_LIST_ITEM, CSCD_DISPLAY_TABLE, CSCD_DISPLAY_INLINE_TABLE, CSCD_DISPLAY_TABLE_ROW,
    CSCD_DISPLAY_TABLE_CELL, CSCD_DISPLAY_TABLE_ROW_GROUP, CSCD_DISPLAY_TABLE_HEADER_GROUP, CSCD_DISPLAY_TABLE_FOOTER_GROUP,
    CSCD_DISPLAY_TABLE_COLUMN, CSCD_DISPLAY_TABLE_COLUMN_GROUP, CSCD_DISPLAY_TABLE_CAPTION, CSCD_DISPLAY_CONTENTS, CSCD_DISPLAY_FLOW_ROOT
} CscdDisplay;
typedef enum CscdPosition { CSCD_POS_STATIC, CSCD_POS_RELATIVE, CSCD_POS_ABSOLUTE, CSCD_POS_FIXED, CSCD_POS_STICKY } CscdPosition;
typedef enum CscdFloat { CSCD_FLOAT_NONE, CSCD_FLOAT_LEFT, CSCD_FLOAT_RIGHT } CscdFloat;
typedef enum CscdClear { CSCD_CLEAR_NONE, CSCD_CLEAR_LEFT, CSCD_CLEAR_RIGHT, CSCD_CLEAR_BOTH } CscdClear;
typedef enum CscdBoxSizing { CSCD_BOX_CONTENT, CSCD_BOX_BORDER } CscdBoxSizing;
typedef enum CscdOverflow { CSCD_OVERFLOW_VISIBLE, CSCD_OVERFLOW_HIDDEN, CSCD_OVERFLOW_SCROLL, CSCD_OVERFLOW_AUTO, CSCD_OVERFLOW_CLIP } CscdOverflow;
typedef enum CscdVisibility { CSCD_VIS_VISIBLE, CSCD_VIS_HIDDEN, CSCD_VIS_COLLAPSE } CscdVisibility;
typedef enum CscdBorderStyle {
    CSCD_BORDER_NONE, CSCD_BORDER_HIDDEN, CSCD_BORDER_SOLID, CSCD_BORDER_DASHED, CSCD_BORDER_DOTTED, CSCD_BORDER_DOUBLE, CSCD_BORDER_GROOVE,
    CSCD_BORDER_RIDGE, CSCD_BORDER_INSET, CSCD_BORDER_OUTSET
} CscdBorderStyle;
typedef enum CscdTextAlign { CSCD_ALIGN_START, CSCD_ALIGN_LEFT, CSCD_ALIGN_RIGHT, CSCD_ALIGN_CENTER, CSCD_ALIGN_JUSTIFY, CSCD_ALIGN_END } CscdTextAlign;
typedef enum CscdWhiteSpace { CSCD_WS_NORMAL, CSCD_WS_NOWRAP, CSCD_WS_PRE, CSCD_WS_PRE_WRAP, CSCD_WS_PRE_LINE, CSCD_WS_BREAK_SPACES } CscdWhiteSpace;
typedef enum CscdTextTransform { CSCD_TT_NONE, CSCD_TT_UPPERCASE, CSCD_TT_LOWERCASE, CSCD_TT_CAPITALIZE } CscdTextTransform;
typedef enum CscdTextOverflow { CSCD_TO_CLIP, CSCD_TO_ELLIPSIS } CscdTextOverflow;
typedef enum CscdWordBreak { CSCD_WB_NORMAL, CSCD_WB_BREAK_ALL, CSCD_WB_KEEP_ALL, CSCD_WB_BREAK_WORD } CscdWordBreak;
typedef enum CscdVerticalAlign {
    CSCD_VA_BASELINE, CSCD_VA_TOP, CSCD_VA_MIDDLE, CSCD_VA_BOTTOM, CSCD_VA_TEXT_TOP, CSCD_VA_TEXT_BOTTOM, CSCD_VA_SUB, CSCD_VA_SUPER, CSCD_VA_LENGTH
} CscdVerticalAlign;
typedef enum CscdFlexDirection { CSCD_FLEX_ROW, CSCD_FLEX_ROW_REVERSE, CSCD_FLEX_COLUMN, CSCD_FLEX_COLUMN_REVERSE } CscdFlexDirection;
typedef enum CscdFlexWrap { CSCD_FLEX_NOWRAP, CSCD_FLEX_WRAP, CSCD_FLEX_WRAP_REVERSE } CscdFlexWrap;
/* Shared by justify-content, align-items, align-self, align-content, justify-items, justify-self. */
typedef enum CscdAlign {
    CSCD_AL_AUTO, CSCD_AL_NORMAL, CSCD_AL_STRETCH, CSCD_AL_START, CSCD_AL_END, CSCD_AL_FLEX_START, CSCD_AL_FLEX_END, CSCD_AL_CENTER,
    CSCD_AL_BASELINE, CSCD_AL_LAST_BASELINE, CSCD_AL_SPACE_BETWEEN, CSCD_AL_SPACE_AROUND, CSCD_AL_SPACE_EVENLY, CSCD_AL_LEFT, CSCD_AL_RIGHT,
    CSCD_AL_SELF_START, CSCD_AL_SELF_END
} CscdAlign;
typedef enum CscdCursor {
    CSCD_CURSOR_AUTO, CSCD_CURSOR_DEFAULT, CSCD_CURSOR_POINTER, CSCD_CURSOR_TEXT, CSCD_CURSOR_MOVE, CSCD_CURSOR_NOT_ALLOWED, CSCD_CURSOR_GRAB,
    CSCD_CURSOR_GRABBING, CSCD_CURSOR_CROSSHAIR, CSCD_CURSOR_WAIT, CSCD_CURSOR_PROGRESS, CSCD_CURSOR_HELP, CSCD_CURSOR_EW_RESIZE,
    CSCD_CURSOR_NS_RESIZE, CSCD_CURSOR_NESW_RESIZE, CSCD_CURSOR_NWSE_RESIZE, CSCD_CURSOR_COL_RESIZE, CSCD_CURSOR_ROW_RESIZE, CSCD_CURSOR_NONE,
    CSCD_CURSOR_COPY
} CscdCursor;
typedef enum CscdListStyle {
    CSCD_LIST_NONE, CSCD_LIST_DISC, CSCD_LIST_CIRCLE, CSCD_LIST_SQUARE, CSCD_LIST_DECIMAL, CSCD_LIST_LOWER_ALPHA, CSCD_LIST_UPPER_ALPHA,
    CSCD_LIST_LOWER_ROMAN, CSCD_LIST_UPPER_ROMAN
} CscdListStyle;
typedef enum CscdObjectFit { CSCD_FIT_FILL, CSCD_FIT_CONTAIN, CSCD_FIT_COVER, CSCD_FIT_NONE, CSCD_FIT_SCALE_DOWN } CscdObjectFit;
typedef enum CscdGridFlow { CSCD_GRID_ROW, CSCD_GRID_COLUMN, CSCD_GRID_ROW_DENSE, CSCD_GRID_COLUMN_DENSE } CscdGridFlow;
typedef enum CscdBackgroundClip { CSCD_BG_BORDER_BOX, CSCD_BG_PADDING_BOX, CSCD_BG_CONTENT_BOX, CSCD_BG_TEXT } CscdBackgroundClip;
typedef enum CscdPointerEvents { CSCD_PE_AUTO, CSCD_PE_NONE } CscdPointerEvents;
typedef enum CscdUserSelect { CSCD_US_AUTO, CSCD_US_NONE, CSCD_US_TEXT, CSCD_US_ALL } CscdUserSelect;
/* touch-action, as bits: 0 is auto (every gesture); otherwise SET with the gestures the page leaves to the
 * view (none: SET alone) */
typedef enum CscdTouchAction { CSCD_TA_PAN_X = 1, CSCD_TA_PAN_Y = 2, CSCD_TA_PINCH = 4, CSCD_TA_SET = 8 } CscdTouchAction;
typedef enum CscdDecorationStyle { CSCD_DECO_SOLID, CSCD_DECO_DOUBLE, CSCD_DECO_DOTTED, CSCD_DECO_DASHED, CSCD_DECO_WAVY } CscdDecorationStyle;
typedef enum CscdBorderCollapse { CSCD_COLLAPSE_SEPARATE, CSCD_COLLAPSE_COLLAPSE } CscdBorderCollapse;
typedef enum CscdBlendMode { CSCD_BM_NORMAL, CSCD_BM_MULTIPLY, CSCD_BM_SCREEN, CSCD_BM_OVERLAY, CSCD_BM_DARKEN, CSCD_BM_LIGHTEN } CscdBlendMode;

typedef struct CscdBorderSide {
    float width; /* computed: 0 when style is none/hidden */
    uint8_t style; /* CscdBorderStyle */
    CscdColor color;
} CscdBorderSide;
CSCD_INLINE bool cscdBorderVisible(const CscdBorderSide* b) {
    return b->width > 0 && b->style != CSCD_BORDER_NONE && b->style != CSCD_BORDER_HIDDEN && b->color.a > 0;
}

typedef struct CscdRadius { CscdLength x, y; } CscdRadius;

typedef struct CscdShadow {
    float x, y, blur, spread;
    CscdColor color;
    bool inset;
} CscdShadow;
typedef CSCD_VEC(CscdShadow) CscdShadowVec;

typedef struct CscdColorStop {
    CscdColor color;
    CscdLength pos; /* auto when unspecified */
    bool hint;
} CscdColorStop;
typedef CSCD_VEC(CscdColorStop) CscdColorStopVec;

/* One background layer: an image (url) or a gradient. */
typedef enum CscdLayerKind { CSCD_LAYER_NONE, CSCD_LAYER_URL, CSCD_LAYER_LINEAR, CSCD_LAYER_RADIAL, CSCD_LAYER_CONIC } CscdLayerKind;
typedef struct CscdBackgroundLayer {
    uint8_t kind;     /* CscdLayerKind */
    CscdImage* image;   /* shared */
    CscdStr url;
    float angle;      /* linear: degrees, CSS convention (0 = to top) */
    bool toCorner;
    int cornerX, cornerY; /* linear "to top right" etc. */
    bool repeating;
    /* radial */
    bool circle;
    int radialSize;   /* 0 farthest-corner, 1 closest-side, 2 closest-corner, 3 farthest-side, 4 explicit */
    CscdLength radiusX, radiusY;
    CscdLength centerX, centerY;
    CscdColorStopVec stops;
    /* sizing */
    CscdLength sizeX, sizeY;
    int sizeKeyword;  /* 0 explicit/auto, 1 cover, 2 contain */
    CscdLength posX, posY;
    bool repeatX, repeatY;
    uint8_t clip;     /* CscdBackgroundClip */
    uint8_t origin;
} CscdBackgroundLayer;
typedef CSCD_VEC(CscdBackgroundLayer) CscdLayerVec;
CscdBackgroundLayer cscdBackgroundLayer(void); /* the defaults */
void cscdLayerFini(CscdBackgroundLayer* l);
CscdBackgroundLayer cscdLayerCopy(const CscdBackgroundLayer* l);
void cscdLayerVecFree(CscdLayerVec* v);
void cscdLayerVecCopy(CscdLayerVec* dst, const CscdLayerVec* src); /* deep; dst replaced */

typedef enum CscdTransformKind {
    CSCD_TF_TRANSLATE, CSCD_TF_SCALE, CSCD_TF_ROTATE, CSCD_TF_SKEW, CSCD_TF_MATRIX, CSCD_TF_ROTATE3D, CSCD_TF_TRANSLATE_Z, CSCD_TF_SCALE_Z,
    CSCD_TF_PERSPECTIVE, CSCD_TF_MATRIX3D
} CscdTransformKind;
/* 2D: Translate, Scale, Rotate, Skew, Matrix. 3D: Rotate3D (around x3 y3 z3 by a), TranslateZ (z px),
 * ScaleZ (a), Perspective (z px; 0: none), Matrix3D (m4). */
typedef struct CscdTransformOp {
    uint8_t kind;      /* CscdTransformKind */
    CscdLength x, y;     /* translate (may be %) */
    float a, b;        /* scale x/y, rotate angle (rad), skew x/y (rad) */
    CscdMatrix m;
    float x3, y3, z3;  /* rotation axis */
    float z;           /* translateZ / perspective distance, px */
    float m4[16];      /* matrix3d(), column by column as CSS lists it */
} CscdTransformOp;
typedef CSCD_VEC(CscdTransformOp) CscdTransformVec;
CscdTransformOp cscdTransformOp(CscdTransformKind kind);
bool cscdTransformOpEq(const CscdTransformOp* a, const CscdTransformOp* b);
CSCD_INLINE bool cscdTransformIs3D(const CscdTransformOp* t) { return t->kind >= CSCD_TF_ROTATE3D; }

/* grid track size */
typedef enum CscdTrackKind { CSCD_TRACK_FIXED, CSCD_TRACK_FR, CSCD_TRACK_AUTO, CSCD_TRACK_MIN_CONTENT, CSCD_TRACK_MAX_CONTENT, CSCD_TRACK_MINMAX, CSCD_TRACK_FIT_CONTENT } CscdTrackKind;
typedef struct CscdTrackSize {
    uint8_t kind;  /* CscdTrackKind */
    CscdLength len;  /* Fixed */
    float fr;      /* Fr */
    const struct CscdTrackSize* min; /* MinMax (kept for the program's life) */
    const struct CscdTrackSize* max;
} CscdTrackSize;
typedef CSCD_VEC(CscdTrackSize) CscdTrackVec;
CSCD_INLINE CscdTrackSize cscdTrackAuto(void) { CscdTrackSize t; memset(&t, 0, sizeof t); t.kind = CSCD_TRACK_AUTO; return t; }
bool cscdTrackEq(const CscdTrackSize* a, const CscdTrackSize* b);
bool cscdTrackVecEq(const CscdTrackVec* a, const CscdTrackVec* b);
/* The same size, kept for the program's life (minmax() parts). */
const CscdTrackSize* cscdTrackIntern(const CscdTrackSize* t);

typedef CSCD_VEC(CscdStrVec) CscdLineNames;
typedef struct CscdTrackList {
    CscdTrackVec tracks;
    CscdLineNames lineNames; /* names before each line */
    /* repeat(auto-fill|auto-fit, ...) expands at layout time */
    int autoRepeatAt;      /* insertion index, -1 if none */
    bool autoFit;
    CscdTrackVec autoTracks;
    bool none;
    bool subgrid;          /* the tracks are the parent grid's, where this grid spans them */
} CscdTrackList;
void cscdTrackListInit(CscdTrackList* t);
void cscdTrackListFini(CscdTrackList* t);
void cscdTrackListCopy(CscdTrackList* dst, const CscdTrackList* src); /* deep; dst replaced */
bool cscdTrackListEq(const CscdTrackList* a, const CscdTrackList* b);

typedef struct CscdGridLine {
    int line; /* 0 = auto */
    int span; /* > 0 for "span N" */
    CscdStr name;
} CscdGridLine;
bool cscdGridLineEq(const CscdGridLine* a, const CscdGridLine* b);

typedef struct CscdTransition {
    CscdStr property; /* "all" or a property name */
    float duration, delay; /* seconds */
    float x1, y1, x2, y2;  /* cubic-bezier, ease by default */
    int steps;
} CscdTransition;
typedef CSCD_VEC(CscdTransition) CscdTransitionVec;

typedef struct CscdAnimation {
    CscdStr name;
    float duration, delay;
    float x1, y1, x2, y2;
    int steps;
    float iterations; /* infinity = -1 */
    bool alternate, reverse;
    int fillMode;     /* 0 none, 1 forwards, 2 backwards, 3 both */
    bool paused;
} CscdAnimation;
typedef CSCD_VEC(CscdAnimation) CscdAnimationVec;

typedef struct CscdFontDesc {
    CscdStrVec families; /* "serif" by default */
    float size;
    int weight;
    bool italic;
    bool smallCaps;
    bool tabularNums; /* font-variant-numeric: tabular-nums */
    bool noLigatures; /* font-variant-ligatures: none */
    uint8_t caps;     /* font-variant-caps: CSCD_CAPS_* */
    uint16_t numeric; /* font-variant-numeric: CSCD_NUMERIC_* bits */
    uint8_t ligatures; /* font-variant-ligatures: CSCD_LIGATURES_* bits */
    uint8_t position; /* font-variant-position: 0 normal, 1 sub, 2 super */
    uint16_t eastAsian; /* font-variant-east-asian: CSCD_EAST_ASIAN_* bits */
    uint8_t kerning;  /* font-kerning: 0 auto, 1 normal, 2 none */
    bool noOpticalSizing; /* font-optical-sizing: none */
    float stretch;    /* font-stretch, percent (0: normal) */
    uint8_t noSynthesis; /* font-synthesis: bits 1 weight, 2 style, 4 small-caps not made up when the font lacks them */
    const CscdFontSettings* featureSettings;   /* font-feature-settings (interned; NULL: normal) */
    const CscdFontSettings* variationSettings; /* font-variation-settings (interned; NULL: normal) */
} CscdFontDesc;
enum { CSCD_CAPS_NORMAL, CSCD_CAPS_SMALL, CSCD_CAPS_ALL_SMALL, CSCD_CAPS_PETITE, CSCD_CAPS_ALL_PETITE, CSCD_CAPS_UNICASE, CSCD_CAPS_TITLING };
enum { CSCD_NUMERIC_LINING = 1, CSCD_NUMERIC_OLDSTYLE = 2, CSCD_NUMERIC_PROPORTIONAL = 4, CSCD_NUMERIC_TABULAR = 8,
       CSCD_NUMERIC_DIAGONAL = 16, CSCD_NUMERIC_STACKED = 32, CSCD_NUMERIC_ORDINAL = 64, CSCD_NUMERIC_SLASHED_ZERO = 128 };
enum { CSCD_LIGATURES_NO_COMMON = 1, CSCD_LIGATURES_DISCRETIONARY = 2, CSCD_LIGATURES_HISTORICAL = 4, CSCD_LIGATURES_NO_CONTEXTUAL = 8 };
enum { CSCD_EAST_ASIAN_JIS78 = 1, CSCD_EAST_ASIAN_JIS83 = 2, CSCD_EAST_ASIAN_JIS90 = 4, CSCD_EAST_ASIAN_JIS04 = 8, CSCD_EAST_ASIAN_SIMPLIFIED = 16,
       CSCD_EAST_ASIAN_TRADITIONAL = 32, CSCD_EAST_ASIAN_FULL_WIDTH = 64, CSCD_EAST_ASIAN_PROPORTIONAL_WIDTH = 128, CSCD_EAST_ASIAN_RUBY = 256 };
enum { CSCD_WRITING_HORIZONTAL, CSCD_WRITING_VERTICAL_RL, CSCD_WRITING_VERTICAL_LR, CSCD_WRITING_SIDEWAYS_RL, CSCD_WRITING_SIDEWAYS_LR };
enum { CSCD_UNICODE_BIDI_NORMAL, CSCD_UNICODE_BIDI_EMBED, CSCD_UNICODE_BIDI_ISOLATE, CSCD_UNICODE_BIDI_OVERRIDE,
       CSCD_UNICODE_BIDI_ISOLATE_OVERRIDE, CSCD_UNICODE_BIDI_PLAINTEXT };
/* The inherited font properties other than the families (copied from s to d). */
void cscdFontDescInherit(CscdFontDesc* d, const CscdFontDesc* s);
bool cscdFontDescEq(const CscdFontDesc* a, const CscdFontDesc* b);

typedef struct CscdCounter { CscdStr name; int value; } CscdCounter;
typedef CSCD_VEC(CscdCounter) CscdCounterVec;

/* Custom properties: name -> value (CscdStr*). Shared (cscdRetain / cscdRelease); copied before it is
 * changed when others hold it. */
typedef struct CscdVars { CscdStrMap map; } CscdVars;
CscdVars* cscdVarsNew(void);
CscdVars* cscdVarsClone(const CscdVars* v);
const CscdStr* cscdVarsGet(const CscdVars* v, CscdSv name);
void cscdVarsSet(CscdVars* v, CscdSv name, CscdSv value);
void cscdVarsRemove(CscdVars* v, CscdSv name);

typedef struct CscdBackdrop { float blur, brightness, grayscale, saturate, invert; } CscdBackdrop;
CSCD_INLINE bool cscdBackdropAny(const CscdBackdrop* b) { return b->blur > 0 || b->brightness != 1 || b->grayscale > 0 || b->saturate != 1 || b->invert > 0; }

/* (the face cscdFontForStyle found for font, and what it was found for) */
typedef struct CscdResolvedFont {
    const void* families;
    size_t count;
    int weight;
    bool italic;
    float size;
    const CscdFontSettings* settings;
    uint32_t generation;
    const void* face;
    bool fakeBold, fakeItalic;
} CscdResolvedFont;

/* Shared (cscdRetain / cscdRelease): the cascade gives elements with the same rules and parent the
 * same style. cscdStyleNew makes the initial style; cscdStyleClone a copy to change. */
struct CscdComputedStyle {
    /* box */
    uint8_t display;          /* CscdDisplay */
    uint8_t originalDisplay;  /* before blockification */
    uint8_t position;         /* CscdPosition */
    uint8_t floating;         /* CscdFloat */
    uint8_t clear;            /* CscdClear */
    uint8_t boxSizing;        /* CscdBoxSizing */
    CscdLength width, height;
    CscdLength minWidth, minHeight;
    CscdLength maxWidth, maxHeight;
    CscdLength top, right, bottom, left;
    CscdLength margin[4];       /* top right bottom left */
    CscdLength padding[4];
    CscdBorderSide border[4];
    CscdRadius radius[4];       /* top-left, top-right, bottom-right, bottom-left */
    float aspectRatio;
    int zIndex;
    bool zAuto;
    uint8_t overflowX, overflowY; /* CscdOverflow */
    uint8_t visibility;       /* CscdVisibility */
    float opacity;

    /* paint */
    CscdColor color;
    CscdColor backgroundColor;
    CscdLayerVec backgrounds;
    CscdShadowVec boxShadow;
    CscdShadowVec textShadow;
    float outlineWidth;
    uint8_t outlineStyle;     /* CscdBorderStyle */
    CscdColor outlineColor;
    float outlineOffset;
    CscdTransformVec transform;
    CscdLength originX, originY;
    float originZ;            /* transform-origin's third value, px */
    float perspective;        /* px given to the children's 3D transforms; 0: none */
    CscdLength perspectiveX, perspectiveY;
    bool backfaceHidden;      /* backface-visibility: hidden */
    bool preserve3d;          /* transform-style: preserve-3d (children are flattened into it) */
    uint8_t cursor;           /* CscdCursor */
    uint8_t pointerEvents;    /* CscdPointerEvents */
    uint8_t userSelect;       /* CscdUserSelect */
    uint8_t touchAction;      /* CscdTouchAction bits (0: auto) */
    bool appearanceNone;      /* appearance: none (form controls drawn by CSS alone) */
    CscdColor accentColor;
    CscdColor caretColor;
    bool caretAuto;
    uint8_t blend;            /* CscdBlendMode */
    float blur;               /* filter: blur() */
    float brightness, grayscale, saturate, invert; /* filter functions */
    /* backdrop-filter: what is behind the box, filtered, under its background */
    CscdBackdrop backdrop;
    CscdLayerVec masks;         /* mask-image layers (their alpha masks the box and its contents) */
    uint8_t objectFit;        /* CscdObjectFit */
    CscdLength objectPosX, objectPosY;
    CscdColor fill;             /* SVG */
    bool fillNone, fillCurrent;
    CscdColor stroke;
    bool strokeNone, strokeCurrent;
    float strokeWidth;
    CscdStr clipPath;
    CscdStr shapeOutside;    /* shape-outside, as written ("": none) */
    float shapeMargin;       /* px */
    CscdColor scrollbarThumb;
    bool scrollbarThumbSet;   /* scrollbar-color (inherited) */

    /* text */
    CscdFontDesc font;
    float lineHeight;         /* px; -1 = normal */
    float lineHeightFactor;   /* unitless line-height (inherited as factor) */
    float letterSpacing, wordSpacing;
    uint8_t textAlign;        /* CscdTextAlign */
    CscdLength textIndent;
    uint8_t whiteSpace;       /* CscdWhiteSpace */
    uint8_t textTransform;    /* CscdTextTransform */
    uint8_t textOverflow;     /* CscdTextOverflow */
    uint8_t wordBreak;        /* CscdWordBreak */
    const char* quotes;       /* quotes: the pairs, each mark ended by a NUL, the list by another (interned); NULL: auto, "" + NUL: none */
    uint8_t hyphens;          /* 0 manual (soft hyphens break the line, shown as "-"), 1 none, 2 auto (as manual: no dictionaries) */
    bool overflowWrap;        /* break-word / anywhere */
    uint8_t verticalAlign;    /* CscdVerticalAlign */
    CscdLength verticalAlignLength;
    bool underline, overline, lineThrough;
    CscdColor decorationColor;
    bool decorationCurrent;
    uint8_t decorationStyle;  /* CscdDecorationStyle */
    float decorationThickness;
    bool underlineUnder;      /* text-underline-position: under (below the descenders) */
    /* text-emphasis: a mark over (or under) each character: its shape (0 none, 1 dot, 2 circle,
     * 3 double-circle, 4 triangle, 5 sesame, 6 the string), open or filled, its color */
    uint8_t emphasisShape;
    bool emphasisOpen, emphasisUnder, emphasisColorSet;
    CscdColor emphasisColor;
    char emphasisString[8];   /* (a mark of one character, UTF-8) */
    float tabSize;
    uint8_t listStyle;        /* CscdListStyle */
    bool listInside;
    CscdStr listImage;
    int lineClamp;
    bool rtl;
    uint8_t unicodeBidi; /* CSCD_UNICODE_BIDI_* */
    uint8_t writingMode; /* CSCD_WRITING_* */
    uint8_t overscroll;  /* overscroll-behavior: 0 auto, 1 contain, 2 none (a wheel at its end does not scroll what holds it) */
    bool smoothScroll;   /* scroll-behavior: smooth (scrolls the page asks for glide) */
    uint8_t colorScheme; /* color-scheme: bits 1 light, 2 dark (0: normal) */
    bool verticalRoot;   /* its writing mode is vertical where its parent's is horizontal: it turns its contents */
    uint8_t textAlignLast;    /* text-align-last (auto: unset) */
    bool alignLastSet;
    CscdColor textFill;
    bool textFillSet;         /* -webkit-text-fill-color (inherited) */
    float textStrokeWidth;
    CscdColor textStroke;
    bool textStrokeSet;       /* -webkit-text-stroke (inherited) */
    bool pixelated;           /* image-rendering: pixelated / crisp-edges (inherited) */
    bool isolate;             /* isolation: isolate */
    bool tableFixed;          /* table-layout: fixed */
    bool captionBottom;       /* caption-side: bottom (inherited) */

    /* flex */
    uint8_t flexDirection;    /* CscdFlexDirection */
    uint8_t flexWrap;         /* CscdFlexWrap */
    uint8_t justifyContent, alignItems, alignSelf, alignContent, justifyItems, justifySelf; /* CscdAlign */
    float flexGrow, flexShrink;
    CscdLength flexBasis;
    int order;
    CscdLength rowGap, columnGap; /* none = normal */

    /* grid */
    CscdTrackList gridColumns, gridRows;
    CscdTrackVec gridAutoColumns, gridAutoRows;
    uint8_t gridAutoFlow;     /* CscdGridFlow */
    CscdGridLine gridColumnStart, gridColumnEnd, gridRowStart, gridRowEnd;
    CscdStrVec gridAreas;       /* grid-template-areas rows */
    CscdStr gridArea;           /* named area for this item */

    /* container queries: 0 normal, 1 inline-size, 2 size (size containment on those axes) */
    uint8_t containerType;
    float zoom;               /* zoom: its own (1: none) */
    float effectiveZoom;      /* and its ancestors' together: lengths, fonts and images are this much larger */
    uint8_t contain;          /* CSCD_CONTAIN_* bits (content-visibility: hidden adds them all) */
    uint8_t contentVisibility; /* 0 visible, 1 auto, 2 hidden (the contents are not made) */
    uint8_t scrollbarWidth;   /* 0 auto, 1 thin, 2 none */
    bool emptyCellsHide;      /* empty-cells: hide */
    CscdStrVec containerNames;

    /* table */
    uint8_t borderCollapse;   /* CscdBorderCollapse */
    float borderSpacingX, borderSpacingY;

    /* CSS counters: name and value (reset, set) or step (increment) */
    CscdCounterVec counterReset, counterSet, counterIncrement;

    /* generated content (::before / ::after) */
    bool hasContent;
    CscdStr content;
    CscdStr contentImage;

    /* animation */
    CscdTransitionVec transitions;
    CscdAnimationVec animations;

    /* custom properties, inherited (shared) */
    CscdVars* vars;

    CscdResolvedFont resolvedFont; /* (a cache) */
};

/* The initial style, shared. */
CscdComputedStyle* cscdStyleNew(void);
/* A shared copy of s. */
CscdComputedStyle* cscdStyleClone(const CscdComputedStyle* s);
/* dst becomes a copy of src (both initialized). */
void cscdStyleAssign(CscdComputedStyle* dst, const CscdComputedStyle* src);
/* The style every element starts from (shared, never changed). */
const CscdComputedStyle* cscdInitialStyle(void);

CSCD_INLINE bool cscdStyleIsPositioned(const CscdComputedStyle* s) { return s->position != CSCD_POS_STATIC; }
CSCD_INLINE bool cscdStyleIsOutOfFlow(const CscdComputedStyle* s) {
    return s->position == CSCD_POS_ABSOLUTE || s->position == CSCD_POS_FIXED || s->floating != CSCD_FLOAT_NONE;
}
CSCD_INLINE bool cscdStyleIsInlineLevel(const CscdComputedStyle* s) {
    return s->display == CSCD_DISPLAY_INLINE || s->display == CSCD_DISPLAY_INLINE_BLOCK || s->display == CSCD_DISPLAY_INLINE_FLEX ||
           s->display == CSCD_DISPLAY_INLINE_GRID || s->display == CSCD_DISPLAY_INLINE_TABLE;
}
CSCD_INLINE bool cscdStyleIsFlex(const CscdComputedStyle* s) { return s->display == CSCD_DISPLAY_FLEX || s->display == CSCD_DISPLAY_INLINE_FLEX; }
CSCD_INLINE bool cscdStyleIsGrid(const CscdComputedStyle* s) { return s->display == CSCD_DISPLAY_GRID || s->display == CSCD_DISPLAY_INLINE_GRID; }
CSCD_INLINE bool cscdStyleIsColumn(const CscdComputedStyle* s) { return s->flexDirection == CSCD_FLEX_COLUMN || s->flexDirection == CSCD_FLEX_COLUMN_REVERSE; }
enum { CSCD_CONTAIN_SIZE = 1, CSCD_CONTAIN_INLINE_SIZE = 2, CSCD_CONTAIN_LAYOUT = 4, CSCD_CONTAIN_PAINT = 8, CSCD_CONTAIN_STYLE = 16 };
CSCD_INLINE bool cscdStyleClipsOverflow(const CscdComputedStyle* s) {
    return s->overflowX != CSCD_OVERFLOW_VISIBLE || s->overflowY != CSCD_OVERFLOW_VISIBLE || (s->contain & CSCD_CONTAIN_PAINT);
}
CSCD_INLINE bool cscdStyleCreatesStackingContext(const CscdComputedStyle* s) {
    return (cscdStyleIsPositioned(s) && !s->zAuto) || s->opacity < 1 || s->transform.len || s->perspective > 0 || s->preserve3d ||
           cscdBackdropAny(&s->backdrop) || s->masks.len || s->isolate || s->position == CSCD_POS_FIXED || s->position == CSCD_POS_STICKY ||
           s->blur > 0 || s->blend != CSCD_BM_NORMAL || s->clipPath.len || s->verticalRoot || (s->contain & (CSCD_CONTAIN_PAINT | CSCD_CONTAIN_LAYOUT));
}
float cscdStyleLineHeight(const CscdComputedStyle* s, float ascent, float descent); /* px */
/* A CSS basic shape (inset(), circle(), ellipse(), polygon()) in the reference box, appended to
 * out; false for anything else. */
bool cscdBasicShapePath(CscdSv value, CscdRect box, CscdPath* out);

/* Viewport and environment needed to compute styles. */
typedef CscdImage* (*CscdImageLoaderFn)(void* user, CscdSv url); /* a reference the caller releases, or NULL */
typedef struct CscdStyleEnvironment {
    CscdMediaContext media;
    float rootFontSize;
    CscdImageLoaderFn loadImage;
    void* loadImageUser;
} CscdStyleEnvironment;

/* The cascade: matches every style sheet against a document and assigns element->style for each
 * element, plus ::before/::after styles. */
typedef struct CscdStyleEngine CscdStyleEngine;

CscdStyleEngine* cscdStyleEngineNew(void);
void cscdStyleEngineFree(CscdStyleEngine* se);
void cscdStyleEngineClear(CscdStyleEngine* se);
void cscdStyleEngineAddSheet(CscdStyleEngine* se, CscdStyleSheet* sheet); /* retains it */
size_t cscdStyleEngineSheetCount(const CscdStyleEngine* se);
CscdStyleSheet* cscdStyleEngineSheet(const CscdStyleEngine* se, size_t i);
/* Recomputes styles for the whole document. */
void cscdStyleEngineComputeAll(CscdStyleEngine* se, CscdDocument* doc, const CscdStyleEnvironment* env);
/* Recomputes e and its descendants (the rest of the document as it is). False: the whole document
 * must be computed (the index is out of date). */
bool cscdStyleEngineComputeSubtree(CscdStyleEngine* se, CscdElement* e, const CscdStyleEnvironment* env);
/* A change of :hover / :active can reach beyond the element and its descendants (a state followed
 * by + or ~, or :has()): such a change needs the whole document computed. */
bool cscdStyleEngineStatesReachSiblings(const CscdStyleEngine* se);
/* A selector tests this attribute, or a value reads attributes (attr()): its changes need styles
 * computed again. True too while the rules' index is out of date. */
bool cscdStyleEngineSelectsAttribute(const CscdStyleEngine* se, CscdSv name);
/* style of a pseudo element of e ("before", "after", "placeholder", "marker"), or NULL (borrowed) */
CscdComputedStyle* cscdStyleEnginePseudoStyle(const CscdStyleEngine* se, const CscdElement* e, CscdSv pseudo);
/* A new reference. */
CscdComputedStyle* cscdStyleEngineComputeFor(CscdStyleEngine* se, CscdElement* e, const CscdComputedStyle* parent, const CscdStyleEnvironment* env, CscdSv pseudo);
const CscdKeyframesRule* cscdStyleEngineKeyframes(const CscdStyleEngine* se, CscdSv name);
/* Every sheet's @font-face rules, in order (borrowed pointers, appended). */
void cscdStyleEngineFontFaces(const CscdStyleEngine* se, CscdPtrVec* out);

/* Applies a declaration to a style (used for animations and inline overrides). */
void cscdStyleApplyDeclaration(CscdComputedStyle* s, const CscdComputedStyle* parent, CscdSv property, CscdSv value,
                             const CscdStyleEnvironment* env, CscdSv baseDir);
bool cscdStyleIsInherited(CscdSv property);

/* Elements with the same matched rules, own declarations and parent style share one computed
 * style, kept from one pass to the next. isShared: s is such a style (copy it to change it). */
bool cscdStyleEngineIsShared(const CscdStyleEngine* se, const CscdComputedStyle* s);
/* Forgets the computed styles: what they resolved changed (an image or a font was added). */
void cscdStyleEngineInvalidateCache(CscdStyleEngine* se);

/* Container queries: the content-box sizes of the containers, as the last layout made them
 * (element -> two floats in a CscdU64Map, CSCD_PKEY keys). Returns true when they changed (the styles
 * then need computing again); sizes is taken (left empty). */
bool cscdStyleEngineSetContainerSizes(CscdStyleEngine* se, CscdU64Map* sizes);
bool cscdStyleEngineHasContainers(const CscdStyleEngine* se); /* some element is a container */
/* The rules that apply to e itself (not its pseudo elements), in cascade order: the last one wins
 * where they disagree. For tools such as the inspector; empty until styles were computed. */
typedef struct CscdMatchedRule {
    const CscdStyleRule* rule;
    const CscdSelector* selector; /* the one of the rule's selectors that matched */
    CscdOrigin origin;
    uint32_t sheet;                /* its sheet's place in the engine */
} CscdMatchedRule;
typedef CSCD_VEC(CscdMatchedRule) CscdMatchedRuleVec;
void cscdStyleEngineMatchedRules(const CscdStyleEngine* se, const CscdElement* e, CscdMatchedRuleVec* out); /* appended */
/* How many styles the engine has worked out, and how many it found already computed, so far. */
void cscdStyleEngineCounts(const CscdStyleEngine* se, uint64_t* computed, uint64_t* reused);
/* Whether the styles computed last would differ under media: a @media rule that holds for one and
 * not the other, a style that used viewport units, another color scheme or resolution. When they
 * would not, the engine takes media as its own (the styles stay valid). */
bool cscdStyleEngineMediaChanges(CscdStyleEngine* se, const CscdMediaContext* media);

/* Value parsing helpers shared with layout and paint. */
bool cscdParseColor(const CscdCssValueVec* v, size_t* i, CscdColor* out, CscdColor current);
bool cscdParseColorString(CscdSv s, CscdColor* out);
const char* cscdUserAgentStyleSheet(void);

#ifdef __cplusplus
}
#endif

#endif
