/* Internal interface of the layout engine, shared by the block/inline and the flex/grid/table
 * sources. Not installed. */
#ifndef CASCADE_LAYOUT_INTERNAL_H
#define CASCADE_LAYOUT_INTERNAL_H

#include "cascade/image.h"
#include "cascade/layout.h"

typedef struct MarginSet { float pos, neg; } MarginSet;
CSCD_INLINE void msAdd(MarginSet* m, float v) { if (v > 0) m->pos = cscdMaxf(m->pos, v); else m->neg = cscdMinf(m->neg, v); }
CSCD_INLINE void msAddSet(MarginSet* m, MarginSet o) { m->pos = cscdMaxf(m->pos, o.pos); m->neg = cscdMinf(m->neg, o.neg); }
CSCD_INLINE float msValue(MarginSet m) { return m.pos + m.neg; }

/* A float: its margin box, and the outline of its shape-outside when it has one (the lines wrap
 * around that instead; the box keeps the points) */
typedef struct FloatBox { CscdRect r; bool left; const CscdPointVec* shape; float shapeMargin; } FloatBox;
typedef struct FloatCtx { CSCD_VEC(FloatBox) floats; } FloatCtx;
/* Free horizontal band [l, r] for a line at y with height h inside [left, right]. */
void fcBand(const FloatCtx* fc, float y, float h, float left, float right, float* outL, float* outR);
float fcClearY(const FloatCtx* fc, CscdClear c);
float fcBottom(const FloatCtx* fc);
/* Places a float of the given margin-box size at or below y; returns its margin-box top-left. */
CscdPoint fcPlace(FloatCtx* fc, float w, float h, float y, float left, float right, bool isLeft);

typedef struct BlockInput {
    float x, y;               /* margin-box left edge of the containing block content area, border-box top */
    float cbW;                /* containing block width (percentages, auto width) */
    float cbH;                /* containing block height, -1 when indefinite */
    float forcedW;            /* border-box width imposed by flex / grid / table (-1: none) */
    float forcedH;            /* border-box height imposed by flex / grid / table (stretch) (-1: none) */
    bool shrinkToFit;         /* floats, inline-blocks, absolute boxes */
    float availW;             /* available width for shrink-to-fit (-1: cbW) */
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
typedef struct SubgridTracks { CscdFloatVec cols, rows; float colGap, rowGap; } SubgridTracks;

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
    /* the outermost list in absStack a positioned box was handed to since the layout under way began
     * (-1: a fixed one; INT_MAX: none) */
    int deferredTo;
} Layouter;

/* box tree */
CscdLayoutBox* layBuild(Layouter* L, CscdDocument* doc);
void layFixupChildren(Layouter* L, CscdLayoutBox* b);

/* layout */
void layRoot(Layouter* L, CscdLayoutBox* root);
MarginSet layBlock(Layouter* L, CscdLayoutBox* b, const BlockInput* in); /* returns the bottom margin set that may collapse further */
float layContent(Layouter* L, CscdLayoutBox* b, FloatCtx* fc, float cbH); /* lays out children inside the content box, returns content height */
float layInline(Layouter* L, CscdLayoutBox* b, FloatCtx* fc);
float layFlex(Layouter* L, CscdLayoutBox* b, float cbH);
float layGrid(Layouter* L, CscdLayoutBox* b, float cbH);
float layTable(Layouter* L, CscdLayoutBox* b, float cbH);
void layReplaced(Layouter* L, CscdLayoutBox* b, float cbW, float cbH, float* contentW, float* contentH);
void layFloat(Layouter* L, CscdLayoutBox* b, FloatCtx* fc, float y, float left, float right, float cbW);
void layAbsolute(Layouter* L, CscdLayoutBox* b, CscdRect cb);
void layAtomicInline(Layouter* L, CscdLayoutBox* b, float cbW, float cbH);

/* sizes */
void layResolveEdges(CscdLayoutBox* b, float cbW);
float layMinContent(Layouter* L, CscdLayoutBox* b); /* border-box width */
float layMaxContent(Layouter* L, CscdLayoutBox* b);
float layIntrinsicInline(Layouter* L, CscdLayoutBox* b, bool min);
float layIntrinsicFlex(Layouter* L, CscdLayoutBox* b, bool min);
float layIntrinsicGrid(Layouter* L, CscdLayoutBox* b, bool min);
float layIntrinsicTable(Layouter* L, CscdLayoutBox* b, bool min);
float layMarginsFixed(CscdLayoutBox* b);
CSCD_INLINE float layOuterMinContent(Layouter* L, CscdLayoutBox* b) { return layMinContent(L, b) + layMarginsFixed(b); }
CSCD_INLINE float layOuterMaxContent(Layouter* L, CscdLayoutBox* b) { return layMaxContent(L, b) + layMarginsFixed(b); }
float laySpecifiedWidth(Layouter* L, CscdLayoutBox* b, float cbW, bool* definite); /* border-box width from 'width' */
float laySpecifiedHeight(CscdLayoutBox* b, float cbH, bool* definite);
float layClampWidth(Layouter* L, CscdLayoutBox* b, float borderBoxW, float cbW);
float layClampHeight(CscdLayoutBox* b, float borderBoxH, float cbH);
float layMeasure(const CscdFont* f, CscdSv s, float letterSpacing, float wordSpacing);
void layComputeOverflow(CscdLayoutBox* b);
void layApplyRelative(CscdLayoutBox* b, float cbW, float cbH);
void layReplacedIntrinsic(Layouter* L, CscdLayoutBox* b, float* w, float* h, float* ratio);
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
