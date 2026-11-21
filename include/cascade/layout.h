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

/* Places in layout are doubles, sizes floats: a place is on the grid of 1/64 px (layout units) up to +-2^25 px, where a
 * double holds every sum of them exactly (a float only below 2^17 px: a page of 5,000 rows is past it), so that a box
 * laid out at one place and moved to another is exactly where laying it out there puts it. */
typedef struct CscdBoxRect { double x, y, w, h; } CscdBoxRect;
CSCD_INLINE CscdRect cscdBoxRectF(CscdBoxRect r) { return cscdRect((float)r.x, (float)r.y, (float)r.w, (float)r.h); }
CSCD_INLINE CscdBoxRect cscdBoxRectOf(CscdRect r) { CscdBoxRect b = {r.x, r.y, r.w, r.h}; return b; }
CSCD_INLINE CscdBoxRect cscdBoxRect(double x, double y, double w, double h) { CscdBoxRect b = {x, y, w, h}; return b; }
CSCD_INLINE double cscdBoxRectRight(CscdBoxRect r) { return r.x + r.w; }
CSCD_INLINE double cscdBoxRectBottom(CscdBoxRect r) { return r.y + r.h; }
CSCD_INLINE bool cscdBoxRectEmpty(CscdBoxRect r) { return r.w <= 0 || r.h <= 0; }
CSCD_INLINE CscdBoxRect cscdBoxRectTranslated(CscdBoxRect r, double dx, double dy) { r.x += dx; r.y += dy; return r; }
CSCD_INLINE CscdBoxRect cscdBoxRectIntersect(CscdBoxRect a, CscdBoxRect b) {
    const double x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    const double x1 = cscdBoxRectRight(a) < cscdBoxRectRight(b) ? cscdBoxRectRight(a) : cscdBoxRectRight(b);
    const double y1 = cscdBoxRectBottom(a) < cscdBoxRectBottom(b) ? cscdBoxRectBottom(a) : cscdBoxRectBottom(b);
    return cscdBoxRect(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}
/* (as cscdRectUnite: an empty one adds nothing) */
CSCD_INLINE CscdBoxRect cscdBoxRectUnite(CscdBoxRect a, CscdBoxRect b) {
    if (cscdBoxRectEmpty(a)) return b;
    if (cscdBoxRectEmpty(b)) return a;
    const double x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    const double x1 = cscdBoxRectRight(a) > cscdBoxRectRight(b) ? cscdBoxRectRight(a) : cscdBoxRectRight(b);
    const double y1 = cscdBoxRectBottom(a) > cscdBoxRectBottom(b) ? cscdBoxRectBottom(a) : cscdBoxRectBottom(b);
    return cscdBoxRect(x0, y0, x1 - x0, y1 - y0);
}

typedef struct CscdTextFrag {
    double x, baseline;       /* absolute, CSS px */
    float width;
    double top;               /* the line box's extent for this fragment */
    float height;
    CscdStr text;
    float justify;            /* extra space added at each space (text-align: justify) */
    size_t start, end;        /* byte range in the source text (after whitespace processing) */
    bool rtl;                 /* a right-to-left run: the text is in logical order, shown reversed */
} CscdTextFrag;
typedef CSCD_VEC(CscdTextFrag) CscdTextFragVec;
void cscdTextFragVecClear(CscdTextFragVec* v);

typedef struct CscdInlineFrag {
    CscdBoxRect rect; /* border box on one line */
    bool first;  /* starts on this line (left border/padding drawn) */
    bool last;   /* ends on this line */
} CscdInlineFrag;

typedef struct CscdLineInfo {
    double top;
    float height;
    double baseline, left;
    float width;
} CscdLineInfo;

typedef enum CscdBoxKind {
    CSCD_BOX_BLOCK, CSCD_BOX_INLINE, CSCD_BOX_TEXT, CSCD_BOX_REPLACED, CSCD_BOX_FLEX, CSCD_BOX_GRID, CSCD_BOX_TABLE, CSCD_BOX_TABLE_ROW_GROUP,
    CSCD_BOX_TABLE_ROW, CSCD_BOX_TABLE_CELL, CSCD_BOX_LINE_BREAK
} CscdBoxKind;

typedef enum CscdReplacedKind {
    CSCD_REPL_NONE, CSCD_REPL_IMAGE, CSCD_REPL_CANVAS, CSCD_REPL_SVG, CSCD_REPL_TEXT_INPUT, CSCD_REPL_TEXT_AREA, CSCD_REPL_CHECKBOX, CSCD_REPL_RADIO,
    CSCD_REPL_RANGE, CSCD_REPL_SELECT, CSCD_REPL_PROGRESS, CSCD_REPL_METER, CSCD_REPL_COLOR_INPUT, CSCD_REPL_VIDEO, CSCD_REPL_FRAME
} CscdReplacedKind;

#ifdef CASCADE_INTERNAL /* (its fields, and what reads them: the library's own, not the API) */
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

    /* geometry: border box, absolute CSS px (scroll offsets are applied when painting); places doubles, sizes floats */
    double x, y;
    float w, h;
    float margin[4], border[4], padding[4];
    double baseline;       /* absolute y of the first baseline */
    bool hasBaseline;
    double lastBaseline;
    CscdBoxRect overflow;  /* scrollable overflow, absolute */
    double staticX, staticY;
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
    /* (across layouts) ordered: it or something in it uses or moves counters, quotes or list numbers, which run in
     * document order: never taken into the next layout as it is. taken: it was, into the tree being built (the tree
     * it leaves frees what is left without it) */
    bool ordered, taken;
    uint64_t builtIn; /* (the layout epoch of the layout that made it, or took it) */
    /* (across layouts) keptIn: the layout that took it as it was, where it was (it and all in it unchanged); prevInk
     * and prevLook: what its element's own boxes drew, and how, before this layout made it again or laid it out again
     * (hasPrev: there was one), for the damage */
    uint64_t keptIn;
    /* (across layouts) newIn: the layout that made it for an element that had none; followedIn: the layout that made
     * a box again for its element in place of this one (an old box neither taken nor followed: its element went) */
    uint64_t newIn, followedIn;
    /* (painting) layersAt: the view's stamp when layersNone was found: nothing in it (not crossing a positioned box or
     * a stacking context) is positioned or a stacking context, so painting need not look for layers in it */
    uint64_t layersAt;
    bool layersNone;
    /* (painting) the same for what does not move with the page as it scrolls: a fixed or sticky box, a backdrop */
    uint64_t stillAt;
    bool stillNone;
    double keptX, keptY; /* (where it was when the layout took it: a flex or grid parent may shift it afterwards) */
    CscdRect prevInk;
    uint64_t prevLook;
    bool hasPrev;
    double memoIn[8];
    double memoInX, memoInY, memoX, memoY;
    /* (across layouts) whether every place in it is on the grid of layout units and nothing in it is placed otherwise
     * (sticky, a shape): 0 not known since it was last laid out, 1 yes, 2 no. Moved by a shift on the grid, it stays so. */
    uint8_t gridState;
    float memoH, memoOut[2];
};

CscdLayoutBox* cscdLayoutBoxNew(void);
/* Frees the box and those under it. */
/* Its elements' boxes are gone (they were freed with cscdLayoutBoxFree). */
void cscdLayoutForget(CscdDocument* doc);
void cscdLayoutBoxFree(CscdLayoutBox* b);
CSCD_INLINE CscdLayoutBox* cscdBoxChild(const CscdLayoutBox* b, size_t i) { return (CscdLayoutBox*)b->children.items[i]; }
CSCD_INLINE double cscdBoxContentX(const CscdLayoutBox* b) { return b->x + b->border[3] + b->padding[3]; }
CSCD_INLINE double cscdBoxContentY(const CscdLayoutBox* b) { return b->y + b->border[0] + b->padding[0]; }
CSCD_INLINE float cscdBoxContentW(const CscdLayoutBox* b) { return cscdMaxf(0.f, b->w - b->border[1] - b->border[3] - b->padding[1] - b->padding[3]); }
CSCD_INLINE float cscdBoxContentH(const CscdLayoutBox* b) { return cscdMaxf(0.f, b->h - b->border[0] - b->border[2] - b->padding[0] - b->padding[2]); }
CSCD_INLINE CscdBoxRect cscdBoxBorderBoxD(const CscdLayoutBox* b) { return cscdBoxRect(b->x, b->y, b->w, b->h); }
CSCD_INLINE CscdBoxRect cscdBoxPaddingBoxD(const CscdLayoutBox* b) {
    return cscdBoxRect(b->x + b->border[3], b->y + b->border[0], (double)b->w - b->border[1] - b->border[3], (double)b->h - b->border[0] - b->border[2]);
}
CSCD_INLINE CscdBoxRect cscdBoxContentBoxD(const CscdLayoutBox* b) { return cscdBoxRect(cscdBoxContentX(b), cscdBoxContentY(b), cscdBoxContentW(b), cscdBoxContentH(b)); }
CSCD_INLINE CscdBoxRect cscdBoxMarginBoxD(const CscdLayoutBox* b) {
    return cscdBoxRect(b->x - b->margin[3], b->y - b->margin[0], (double)b->w + b->margin[1] + b->margin[3], (double)b->h + b->margin[0] + b->margin[2]);
}
/* (as floats, for painting: within 1/64 px below 2^17 px, coarser past it) */
CSCD_INLINE CscdRect cscdBoxBorderBox(const CscdLayoutBox* b) { return cscdRect((float)b->x, (float)b->y, b->w, b->h); }
CSCD_INLINE CscdRect cscdBoxPaddingBox(const CscdLayoutBox* b) {
    return cscdRect((float)(b->x + b->border[3]), (float)(b->y + b->border[0]), b->w - b->border[1] - b->border[3], b->h - b->border[0] - b->border[2]);
}
CSCD_INLINE CscdRect cscdBoxContentBox(const CscdLayoutBox* b) {
    return cscdRect((float)cscdBoxContentX(b), (float)cscdBoxContentY(b), cscdBoxContentW(b), cscdBoxContentH(b));
}
CSCD_INLINE CscdRect cscdBoxMarginBox(const CscdLayoutBox* b) {
    return cscdRect((float)(b->x - b->margin[3]), (float)(b->y - b->margin[0]), b->w + b->margin[1] + b->margin[3], b->h + b->margin[0] + b->margin[2]);
}
CSCD_INLINE bool cscdBoxIsScrollContainer(const CscdLayoutBox* b) {
    const CscdComputedStyle* s = b->style;
    return s && (s->overflowX == CSCD_OVERFLOW_AUTO || s->overflowX == CSCD_OVERFLOW_SCROLL || s->overflowY == CSCD_OVERFLOW_AUTO ||
                 s->overflowY == CSCD_OVERFLOW_SCROLL || s->overflowX == CSCD_OVERFLOW_HIDDEN || s->overflowY == CSCD_OVERFLOW_HIDDEN);
}
CSCD_INLINE bool cscdBoxIsElements(const CscdLayoutBox* b) {
    return b->element && !b->anonymous && !b->pseudo.len && b->kind != CSCD_BOX_TEXT;
}
#endif
/* Moves the box and everything inside it. */
void cscdBoxShift(CscdLayoutBox* b, double dx, double dy);

typedef struct CscdViewport { float width, height; } CscdViewport;

/* Builds the box tree from the styled DOM and lays it out in the viewport (the caller owns it;
 * NULL when nothing is shown). images gives the image of a URL (a reference, or NULL). */
CscdLayoutBox* cscdLayoutDocument(CscdDocument* doc, const CscdStyleEngine* styles, CscdViewport vp, CscdImageLoaderFn images, void* imagesUser);
/* The same, taking from previous (the document's last layout, at the same scale) the boxes of what has not changed
 * since (CscdNode.changedAt): they are not made again, and with the same input not laid out again either, only moved.
 * Free previous with cscdLayoutBoxFreeReplaced once this returns (not before: elements still point into it). The
 * document's layout epoch moves on. */
CscdLayoutBox* cscdLayoutDocumentFrom(CscdDocument* doc, const CscdStyleEngine* styles, CscdViewport vp, CscdImageLoaderFn images, void* imagesUser,
                                      CscdLayoutBox* previous, bool* boxesChanged);
/* What an element's own boxes draw (its box, its text, its ::before and ::after, its marker: not the elements in it),
 * in layout coordinates, with its outline and shadows; and how, as a number (styles, places, scrolling). An element
 * box (one with an element, not anonymous, not a pseudo element's nor a text). */
CscdRect cscdBoxOwnInk(const CscdLayoutBox* b);
uint64_t cscdBoxOwnLook(const CscdLayoutBox* b);
/* Frees what is left of a layout cscdLayoutDocumentFrom took boxes from (not those), and forgets it in the elements
 * whose boxes go. */
void cscdLayoutBoxFreeReplaced(CscdLayoutBox* b);

CscdFont cscdFontForStyle(const CscdComputedStyle* s);

#ifdef __cplusplus
}
#endif

#endif
