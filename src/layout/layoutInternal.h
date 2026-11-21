/* Internal interface of the layout engine, shared by the block/inline and the flex/grid/table
 * sources. Not installed. */
#ifndef CASCADE_LAYOUT_INTERNAL_H
#define CASCADE_LAYOUT_INTERNAL_H

#include "cascade/image.h"
#include "cascade/layout.h"
#include <math.h>

/* Layout works in doubles (places past 2^17 px stay on the grid of layout units: see laySnap), as the float helpers do */
CSCD_INLINE double layMin(double a, double b) { return b < a ? b : a; }
CSCD_INLINE double layMax(double a, double b) { return a < b ? b : a; }
CSCD_INLINE double layClamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
CSCD_INLINE int layInt(double v) { return v >= 268435456. ? 268435456 : v <= -268435456. ? -268435456 : v == v ? (int)v : 0; }
/* the style's float helpers on layout's doubles (a basis, an ascent: sizes, which floats hold) */
CSCD_INLINE double layLen(CscdLength l, double basis, double def) { return cscdLenResolve(l, (float)basis, (float)def); }
CSCD_INLINE double layLineHeight(const CscdComputedStyle* s, double ascent, double descent) { return cscdStyleLineHeight(s, (float)ascent, (float)descent); }
typedef CscdBoxRect LayRect;
typedef CSCD_VEC(double) LayDoubleVec;
typedef struct LayPoint { double x, y; } LayPoint;

typedef struct MarginSet { double pos, neg; } MarginSet;
CSCD_INLINE void msAdd(MarginSet* m, double v) { if (v > 0) m->pos = layMax(m->pos, v); else m->neg = layMin(m->neg, v); }
CSCD_INLINE void msAddSet(MarginSet* m, MarginSet o) { m->pos = layMax(m->pos, o.pos); m->neg = layMin(m->neg, o.neg); }
CSCD_INLINE double msValue(MarginSet m) { return m.pos + m.neg; }

/* A float: its margin box, and the outline of its shape-outside when it has one (the lines wrap
 * around that instead; the box keeps the points) */
typedef struct FloatBox { LayRect r; bool left; const CscdPointVec* shape; double shapeMargin; } FloatBox;
typedef struct FloatCtx { CSCD_VEC(FloatBox) floats; } FloatCtx;
/* Free horizontal band [l, r] for a line at y with height h inside [left, right]. */
void fcBand(const FloatCtx* fc, double y, double h, double left, double right, double* outL, double* outR);
double fcClearY(const FloatCtx* fc, CscdClear c);
double fcBottom(const FloatCtx* fc);
/* Places a float of the given margin-box size at or below y; returns its margin-box top-left. */
LayPoint fcPlace(FloatCtx* fc, double w, double h, double y, double left, double right, bool isLeft);

typedef struct BlockInput {
    double x, y;               /* margin-box left edge of the containing block content area, border-box top */
    double cbW;                /* containing block width (percentages, auto width) */
    double cbH;                /* containing block height, -1 when indefinite */
    double forcedW;            /* border-box width imposed by flex / grid / table (-1: none) */
    double forcedH;            /* border-box height imposed by flex / grid / table (stretch) (-1: none) */
    bool shrinkToFit;         /* floats, inline-blocks, absolute boxes */
    double availW;             /* available width for shrink-to-fit (-1: cbW) */
    FloatCtx* floats;
    bool marginTopCollapsed;  /* the top margin (and the first child's) was already applied by the parent */
    bool positionedByCaller;  /* x is the border-box left (no margin resolution against cbW) */
} BlockInput;

CSCD_INLINE BlockInput blockInput(void) {
    BlockInput in;
    memset(&in, 0, sizeof in);
    in.cbH = -1;
    in.forcedW = -1;
    in.forcedH = -1;
    in.availW = -1;
    return in;
}

/* subgrid: the track sizes a parent grid gives a subgrid it lays out (an axis left empty: the
 * subgrid sizes it), and the row sizes a subgrid found for itself, for its parent */
typedef struct SubgridTracks { CscdFloatVec cols, rows; double colGap, rowGap; } SubgridTracks;

/* CSS counters, as the boxes are made in document order: each name's nested instances, with the
 * element whose children's scope made each one (it ends with that element) */
typedef struct CounterInstance { const CscdElement* scope; int value; } CounterInstance;
typedef CSCD_VEC(CounterInstance) CounterStack;

typedef struct Layouter {
    const CscdStyleEngine* styles;
    CscdViewport vp;
    CscdImageLoaderFn images;
    void* imagesUser;
    /* absolute positioning: boxes waiting for their containing block */
    CSCD_VEC(CscdPtrVec) absStack;
    CscdPtrVec fixedBoxes;
    CscdU64Map subgridIn;      /* box -> SubgridTracks* */
    CscdU64Map subgridRowsOut; /* box -> CscdFloatVec* */
    CscdStrMap counters;       /* name -> CounterStack* */
    int quoteDepth;            /* how many quotes are open (open-quote, close-quote) */
    /* the list item numbered last, and its number: the next one counts on from it (the boxes are
     * built in document order) instead of from the start of its list */
    const CscdElement* lastListItem;
    int lastListOrdinal;
    uint32_t pass; /* this layout pass (0: none, nothing is remembered; CscdLayoutBox.memoPass) */
    /* boxes of the last layout taken for elements unchanged since (cscdLayoutDocumentFrom): epoch, the document's layout
     * epoch now (an element stamped before it is unchanged) */
    bool reuse;
    uint64_t epoch;
    bool boxesChanged; /* an element got a box it had not, or lost one (the damage is the whole page) */
    /* the outermost list in absStack a positioned box was handed to since the layout under way began
     * (-1: a fixed one; INT_MAX: none) */
    int deferredTo;
} Layouter;

/* Layout units: every place and size layout keeps is on the grid of 1/64 px (as browsers' LayoutUnit), where floats
 * hold sums exactly (below 2^17 px): a box laid out at one place and moved to another is where laying it out there puts
 * it, to the last bit, so that boxes kept from the last layout, or measured and then placed, are moved instead of laid
 * out again. */
/* (and within +-2^25 px, as browsers' LayoutUnit saturates: a length past what floats hold, 1e39px, is infinite, and
 * places made from it, infinity less infinity, NaN; NaN is 0) */
#define LAY_MAX 33554432.
static inline double laySnap(double v) {
    if (!(v > -LAY_MAX)) return v != v ? 0. : -LAY_MAX; /* (NaN, or below) */
    if (v > LAY_MAX) return LAY_MAX;
    return round(v * 64.) * (1. / 64.);
}
/* (a size on the grid, as a float: exact below 2^17 px, and the same float for the same size however it was made) */
static inline float laySnapF(double v) { return (float)laySnap(v); }

/* box tree */
CscdLayoutBox* layBuild(Layouter* L, CscdDocument* doc);
void layFixupChildren(Layouter* L, CscdLayoutBox* b);

/* layout */
void layRoot(Layouter* L, CscdLayoutBox* root);
MarginSet layBlock(Layouter* L, CscdLayoutBox* b, const BlockInput* in); /* returns the bottom margin set that may collapse further */
double layContent(Layouter* L, CscdLayoutBox* b, FloatCtx* fc, double cbH); /* lays out children inside the content box, returns content height */
double layInline(Layouter* L, CscdLayoutBox* b, FloatCtx* fc);
double layFlex(Layouter* L, CscdLayoutBox* b, double cbH);
double layGrid(Layouter* L, CscdLayoutBox* b, double cbH);
double layTable(Layouter* L, CscdLayoutBox* b, double cbH);
void layReplaced(Layouter* L, CscdLayoutBox* b, double cbW, double cbH, double* contentW, double* contentH);
void layFloat(Layouter* L, CscdLayoutBox* b, FloatCtx* fc, double y, double left, double right, double cbW);
void layAbsolute(Layouter* L, CscdLayoutBox* b, LayRect cb);
void layAtomicInline(Layouter* L, CscdLayoutBox* b, double cbW, double cbH);

/* sizes */
void layResolveEdges(CscdLayoutBox* b, double cbW);
double layMinContent(Layouter* L, CscdLayoutBox* b); /* border-box width */
double layMaxContent(Layouter* L, CscdLayoutBox* b);
double layIntrinsicInline(Layouter* L, CscdLayoutBox* b, bool min);
double layIntrinsicFlex(Layouter* L, CscdLayoutBox* b, bool min);
double layIntrinsicGrid(Layouter* L, CscdLayoutBox* b, bool min);
double layIntrinsicTable(Layouter* L, CscdLayoutBox* b, bool min);
double layMarginsFixed(CscdLayoutBox* b);
CSCD_INLINE double layOuterMinContent(Layouter* L, CscdLayoutBox* b) { return layMinContent(L, b) + layMarginsFixed(b); }
CSCD_INLINE double layOuterMaxContent(Layouter* L, CscdLayoutBox* b) { return layMaxContent(L, b) + layMarginsFixed(b); }
double laySpecifiedWidth(Layouter* L, CscdLayoutBox* b, double cbW, bool* definite); /* border-box width from 'width' */
double laySpecifiedHeight(CscdLayoutBox* b, double cbH, bool* definite);
double layClampWidth(Layouter* L, CscdLayoutBox* b, double borderBoxW, double cbW);
double layClampHeight(CscdLayoutBox* b, double borderBoxH, double cbH);
double layMeasure(const CscdFont* f, CscdSv s, double letterSpacing, double wordSpacing);
void layComputeOverflow(CscdLayoutBox* b);
void layApplyRelative(CscdLayoutBox* b, double cbW, double cbH);
void layReplacedIntrinsic(Layouter* L, CscdLayoutBox* b, double* w, double* h, double* ratio);
bool layIsContainingBlock(const CscdLayoutBox* b);

void layApplyCounters(Layouter* L, const CscdComputedStyle* s, const CscdElement* scope);
void layEndCounterScope(Layouter* L, const CscdElement* scope);
CscdStr layResolveContent(Layouter* L, CscdSv content, CscdElement* e);

/* positioned boxes found while laying out: fixed ones wait for the viewport, absolute ones for
 * their containing block */
void layDeferAbsolute(Layouter* L, CscdLayoutBox* c);

CSCD_INLINE bool layIsBlockLevel(const CscdLayoutBox* b) {
    return !b->atomicInline && b->kind != CSCD_BOX_INLINE && b->kind != CSCD_BOX_TEXT && b->kind != CSCD_BOX_LINE_BREAK;
}
CSCD_INLINE bool layIsOutOfFlow(const CscdLayoutBox* b) {
    return b->style && b->kind != CSCD_BOX_TEXT &&
           (b->style->position == CSCD_POS_ABSOLUTE || b->style->position == CSCD_POS_FIXED || b->style->floating != CSCD_FLOAT_NONE);
}

#endif
