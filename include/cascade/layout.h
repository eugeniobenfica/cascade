/* cascade — the box tree and layout: block and inline formatting contexts (line breaking, floats,
 * margin collapsing), positioning, flexbox, grid and tables, following CSS 2.2, Flexbox Level 1
 * and Grid Level 1. */
#ifndef CASCADE_LAYOUT_H
#define CASCADE_LAYOUT_H

#include "cascade/dom.h"
#include "cascade/font.h"
#include "cascade/style.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CscdTextFrag {
    float x, baseline, width; /* absolute, CSS px */
    float top, height;        /* the line box's extent for this fragment */
    CscdStr text;
    float justify;            /* extra space added at each space (text-align: justify) */
    size_t start, end;        /* byte range in the source text (after whitespace processing) */
    bool rtl;                 /* a right-to-left run: the text is in logical order, shown reversed */
} CscdTextFrag;
typedef CSCD_VEC(CscdTextFrag) CscdTextFragVec;
void cscdTextFragVecClear(CscdTextFragVec* v);

typedef struct CscdInlineFrag {
    CscdRect rect; /* border box on one line */
    bool first;  /* starts on this line (left border/padding drawn) */
    bool last;   /* ends on this line */
} CscdInlineFrag;

typedef struct CscdLineInfo {
    float top, height, baseline, left, width;
} CscdLineInfo;

typedef enum CscdBoxKind {
    CSCD_BOX_BLOCK, CSCD_BOX_INLINE, CSCD_BOX_TEXT, CSCD_BOX_REPLACED, CSCD_BOX_FLEX, CSCD_BOX_GRID, CSCD_BOX_TABLE, CSCD_BOX_TABLE_ROW_GROUP,
    CSCD_BOX_TABLE_ROW, CSCD_BOX_TABLE_CELL, CSCD_BOX_LINE_BREAK
} CscdBoxKind;

typedef enum CscdReplacedKind {
    CSCD_REPL_NONE, CSCD_REPL_IMAGE, CSCD_REPL_CANVAS, CSCD_REPL_SVG, CSCD_REPL_TEXT_INPUT, CSCD_REPL_TEXT_AREA, CSCD_REPL_CHECKBOX, CSCD_REPL_RADIO,
    CSCD_REPL_RANGE, CSCD_REPL_SELECT, CSCD_REPL_PROGRESS, CSCD_REPL_METER, CSCD_REPL_COLOR_INPUT, CSCD_REPL_VIDEO, CSCD_REPL_FRAME
} CscdReplacedKind;

struct CscdLayoutBox {
    uint8_t kind;          /* CscdBoxKind */
    uint8_t replaced;      /* CscdReplacedKind */
    bool anonymous;
    bool atomicInline;     /* inline-block, inline-flex, replaced inline... */
    bool inlineContent;    /* block container whose children are inline-level */
    bool isMarker;
    uint32_t inlineOwner, inlineSlot; /* (scratch: the inline layout working on this box, and its entry there) */
    CscdElement* element;
    CscdText* textNode;
    CscdStr text;            /* text box content (generated content or the DOM text) */
    CscdStr pseudo;          /* "before", "after", "marker" */
    CscdPointVec shape;      /* a float's shape-outside, as a polygon (layout coordinates) */
    /* a box whose writing mode is vertical (style->verticalRoot): its contents are laid out
     * horizontally, from (x + logicalDX, y + logicalDY), and shown turned (a quarter turn, clockwise
     * for vertical-rl, vertical-lr and sideways-rl, the other way for sideways-lr) */
    float logicalDX, logicalDY;
    CscdComputedStyle* style; /* shared */
    CscdLayoutBox* parent;
    CscdPtrVec children;     /* CscdLayoutBox*, owned */
    CscdImage* image;        /* shared */
    CscdFont font;           /* resolved font for text */

    /* geometry: border box, absolute CSS px (scroll offsets are applied when painting) */
    float x, y, w, h;
    float margin[4], border[4], padding[4];
    float baseline;        /* absolute y of the first baseline */
    bool hasBaseline;
    float lastBaseline;
    CscdRect overflow;       /* scrollable overflow, absolute */
    float staticX, staticY;
    bool hasStaticPos;

    CscdTextFragVec textFrags;           /* Text boxes */
    CSCD_VEC(CscdInlineFrag) inlineFrags;  /* Inline boxes */
    CSCD_VEC(CscdLineInfo) lines;          /* inline formatting contexts */

    /* intrinsic size cache */
    float minContent, maxContent;

    /* (scratch, within one layout pass: the box's last block layout, which a later one with the same
     * input reuses, only moving the box; see layBlock in layout.c) */
    uint32_t memoPass, sensPass;
    bool memoBfc, heightSensitive;
    float memoIn[8];
    float memoInX, memoInY, memoX, memoY, memoH, memoOut[2];
};

CscdLayoutBox* cscdLayoutBoxNew(void);
/* Frees the box and those under it. */
/* Its elements' boxes are gone (they were freed with cscdLayoutBoxFree). */
void cscdLayoutForget(CscdDocument* doc);
void cscdLayoutBoxFree(CscdLayoutBox* b);
CSCD_INLINE CscdLayoutBox* cscdBoxChild(const CscdLayoutBox* b, size_t i) { return (CscdLayoutBox*)b->children.items[i]; }
CSCD_INLINE float cscdBoxContentX(const CscdLayoutBox* b) { return b->x + b->border[3] + b->padding[3]; }
CSCD_INLINE float cscdBoxContentY(const CscdLayoutBox* b) { return b->y + b->border[0] + b->padding[0]; }
CSCD_INLINE float cscdBoxContentW(const CscdLayoutBox* b) { return cscdMaxf(0.f, b->w - b->border[1] - b->border[3] - b->padding[1] - b->padding[3]); }
CSCD_INLINE float cscdBoxContentH(const CscdLayoutBox* b) { return cscdMaxf(0.f, b->h - b->border[0] - b->border[2] - b->padding[0] - b->padding[2]); }
CSCD_INLINE CscdRect cscdBoxBorderBox(const CscdLayoutBox* b) { return cscdRect(b->x, b->y, b->w, b->h); }
CSCD_INLINE CscdRect cscdBoxPaddingBox(const CscdLayoutBox* b) {
    return cscdRect(b->x + b->border[3], b->y + b->border[0], b->w - b->border[1] - b->border[3], b->h - b->border[0] - b->border[2]);
}
CSCD_INLINE CscdRect cscdBoxContentBox(const CscdLayoutBox* b) { return cscdRect(cscdBoxContentX(b), cscdBoxContentY(b), cscdBoxContentW(b), cscdBoxContentH(b)); }
CSCD_INLINE CscdRect cscdBoxMarginBox(const CscdLayoutBox* b) {
    return cscdRect(b->x - b->margin[3], b->y - b->margin[0], b->w + b->margin[1] + b->margin[3], b->h + b->margin[0] + b->margin[2]);
}
CSCD_INLINE bool cscdBoxIsScrollContainer(const CscdLayoutBox* b) {
    const CscdComputedStyle* s = b->style;
    return s && (s->overflowX == CSCD_OVERFLOW_AUTO || s->overflowX == CSCD_OVERFLOW_SCROLL || s->overflowY == CSCD_OVERFLOW_AUTO ||
                 s->overflowY == CSCD_OVERFLOW_SCROLL || s->overflowX == CSCD_OVERFLOW_HIDDEN || s->overflowY == CSCD_OVERFLOW_HIDDEN);
}
/* Moves the box and everything inside it. */
void cscdBoxShift(CscdLayoutBox* b, float dx, float dy);

typedef struct CscdViewport { float width, height; } CscdViewport;

/* Builds the box tree from the styled DOM and lays it out in the viewport (the caller owns it;
 * NULL when nothing is shown). images gives the image of a URL (a reference, or NULL). */
CscdLayoutBox* cscdLayoutDocument(CscdDocument* doc, const CscdStyleEngine* styles, CscdViewport vp, CscdImageLoaderFn images, void* imagesUser);

CscdFont cscdFontForStyle(const CscdComputedStyle* s);

#ifdef __cplusplus
}
#endif

#endif
