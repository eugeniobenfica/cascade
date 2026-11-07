/* Flexbox (CSS Flexible Box Layout Level 1), Grid (CSS Grid Layout Level 1, with subgrid, without
 * dense packing) and table layout. */
#include "layout/layoutInternal.h"

#include <stdlib.h>

static float gapOf(CscdLength g, float basis) { return cscdLenIsNone(g) ? 0 : cscdMaxf(0.f, cscdLenResolve(g, cscdMaxf(0.f, basis), 0)); }

static bool isAbsolute(const CscdLayoutBox* c) {
    return c->style && (c->style->position == CSCD_POS_ABSOLUTE || c->style->position == CSCD_POS_FIXED);
}

static uint8_t selfAlign(const CscdComputedStyle* item, const CscdComputedStyle* container) {
    uint8_t a = item->alignSelf;
    if (a == CSCD_AL_AUTO) a = container->alignItems;
    if (a == CSCD_AL_NORMAL) a = CSCD_AL_STRETCH;
    return a;
}

static int byOrder(const void* a, const void* b, void* ctx) {
    (void)ctx;
    const CscdLayoutBox* x = *(CscdLayoutBox* const*)a;
    const CscdLayoutBox* y = *(CscdLayoutBox* const*)b;
    return x->style->order < y->style->order ? -1 : x->style->order > y->style->order ? 1 : 0;
}

/* the in-flow children of a flex or grid container in order-modified document order; the others
 * get their static position and wait for their containing block */
static void inFlowItems(Layouter* L, CscdLayoutBox* b, CscdPtrVec* out) {
    for (size_t i = 0; i < b->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(b, i);
        if (c->isMarker) continue;
        if (isAbsolute(c)) {
            c->staticX = cscdBoxContentX(b);
            c->staticY = cscdBoxContentY(b);
            c->hasStaticPos = true;
            layDeferAbsolute(L, c);
            continue;
        }
        cscdVecPush(out, c);
    }
    cscdSort(out->items, out->len, sizeof(void*), byOrder, NULL);
}

/* ---- flexbox ----------------------------------------------------------------------------------- */

typedef struct FlexItem {
    CscdLayoutBox* b;
    float base, hypo, target;
    float minMain, maxMain;
    float marginMain;  /* sum of main-axis margins (auto counted as 0) */
    float marginCross;
    bool frozen;
    float cross;       /* outer cross size */
    float baselineOffset;
    uint8_t align;
    bool autoMainStart, autoMainEnd, autoCrossStart, autoCrossEnd;
} FlexItem;

typedef struct FlexLine {
    size_t from, n; /* items of the line */
    float cross, pos, maxBaseline;
} FlexLine;

float layIntrinsicFlex(Layouter* L, CscdLayoutBox* b, bool min) {
    const CscdComputedStyle* s = b->style;
    bool row = !cscdStyleIsColumn(s);
    float gap = gapOf(row ? s->columnGap : s->rowGap, 0);
    float total = 0, best = 0;
    int n = 0;
    for (size_t i = 0; i < b->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(b, i);
        if (isAbsolute(c) || c->isMarker) continue;
        float w = min ? layOuterMinContent(L, c) : layOuterMaxContent(L, c);
        if (row) {
            if (min && s->flexWrap != CSCD_FLEX_NOWRAP) best = cscdMaxf(best, w);
            else { total += w; ++n; }
        } else best = cscdMaxf(best, w);
    }
    if (row && !(min && s->flexWrap != CSCD_FLEX_NOWRAP)) return total + gap * cscdMaxi(0, n - 1);
    return best;
}

typedef struct FlexCtx {
    Layouter* L;
    bool row;
    float cw, mainSize, crossSize;
} FlexCtx;

/* lays an item out at a main size, to find its cross size */
static void measureCross(FlexCtx* F, FlexItem* it, float mainBorderBox, bool forceMain) {
    CscdLayoutBox* c = it->b;
    BlockInput in = blockInput();
    in.cbW = F->cw;
    in.cbH = F->row ? F->crossSize : F->mainSize;
    in.positionedByCaller = true;
    if (F->row) {
        in.forcedW = mainBorderBox;
    } else {
        if (forceMain) in.forcedH = mainBorderBox;
        float avail = F->cw - it->marginCross;
        bool wd;
        laySpecifiedWidth(F->L, c, F->cw, &wd);
        if (!wd && it->align == CSCD_AL_STRETCH && !it->autoCrossStart && !it->autoCrossEnd) in.forcedW = layClampWidth(F->L, c, avail, F->cw);
        else if (!wd) { in.shrinkToFit = true; in.availW = avail + it->marginCross; }
    }
    layBlock(F->L, c, &in);
}

float layFlex(Layouter* L, CscdLayoutBox* b, float cbH) {
    const CscdComputedStyle* s = b->style;
    bool row = !cscdStyleIsColumn(s);
    bool reverse = s->flexDirection == CSCD_FLEX_ROW_REVERSE || s->flexDirection == CSCD_FLEX_COLUMN_REVERSE;
    bool wrap = s->flexWrap != CSCD_FLEX_NOWRAP;
    float cx = cscdBoxContentX(b), cy = cscdBoxContentY(b), cw = cscdBoxContentW(b);
    float colGap = gapOf(s->columnGap, cw), rowGap = gapOf(s->rowGap, cbH >= 0 ? cbH : 0);
    float mainGap = row ? colGap : rowGap, crossGap = row ? rowGap : colGap;
    float edgesH = b->padding[0] + b->padding[2] + b->border[0] + b->border[2];
    float mainSize = row ? cw : cbH;  /* -1 when indefinite */
    float crossSize = row ? cbH : cw; /* -1 when indefinite */

    CscdPtrVec boxes = {0};
    inFlowItems(L, b, &boxes);
    CSCD_VEC(FlexItem) items = {0};
    cscdVecResize(&items, boxes.len);
    for (size_t i = 0; i < boxes.len; ++i) {
        items.items[i].b = (CscdLayoutBox*)boxes.items[i];
        items.items[i].maxMain = 1e30f;
        items.items[i].align = CSCD_AL_STRETCH;
    }
    cscdVecFree(&boxes);
    FlexCtx F = {L, row, cw, mainSize, crossSize};

    for (size_t i = 0; i < items.len; ++i) {
        FlexItem* it = &items.items[i];
        CscdLayoutBox* c = it->b;
        const CscdComputedStyle* cs = c->style;
        layResolveEdges(c, cw);
        it->align = selfAlign(cs, s);
        if (row) {
            it->autoMainStart = cscdLenIsAuto(cs->margin[3]); it->autoMainEnd = cscdLenIsAuto(cs->margin[1]);
            it->autoCrossStart = cscdLenIsAuto(cs->margin[0]); it->autoCrossEnd = cscdLenIsAuto(cs->margin[2]);
            it->marginMain = c->margin[1] + c->margin[3];
            it->marginCross = c->margin[0] + c->margin[2];
        } else {
            it->autoMainStart = cscdLenIsAuto(cs->margin[0]); it->autoMainEnd = cscdLenIsAuto(cs->margin[2]);
            it->autoCrossStart = cscdLenIsAuto(cs->margin[3]); it->autoCrossEnd = cscdLenIsAuto(cs->margin[1]);
            it->marginMain = c->margin[0] + c->margin[2];
            it->marginCross = c->margin[1] + c->margin[3];
        }
        float edgesMain = row ? c->padding[1] + c->padding[3] + c->border[1] + c->border[3] : c->padding[0] + c->padding[2] + c->border[0] + c->border[2];
        /* flex base size */
        CscdLength fb = cs->flexBasis;
        if (cscdLenIsAuto(fb)) fb = row ? cs->width : cs->height;
        bool definite = false;
        float base = 0;
        if (!cscdLenIsAuto(fb) && !cscdLenIsNone(fb) && !cscdLenIsIntrinsic(fb) && !(cscdLenHasPercent(fb) && mainSize < 0)) {
            base = cscdLenResolve(fb, cscdMaxf(0.f, mainSize), 0);
            if (cs->boxSizing == CSCD_BOX_CONTENT) base += edgesMain;
            else base = cscdMaxf(base, edgesMain);
            definite = true;
        }
        float contentMain = -1;
#define CONTENT_SIZE() (contentMain >= 0 ? contentMain : row ? (contentMain = layMaxContent(L, c)) \
                                                             : (measureCross(&F, it, 0, false), contentMain = c->h))
        if (!definite) {
            if (row && fb.kind == CSCD_LEN_MIN_CONTENT) base = layMinContent(L, c);
            else base = CONTENT_SIZE();
            if (!row && c->kind == CSCD_BOX_REPLACED) base = c->h;
        }
        it->base = base;
        /* min / max main sizes */
        const CscdLength mn = row ? cs->minWidth : cs->minHeight;
        const CscdLength mx = row ? cs->maxWidth : cs->maxHeight;
        float adj = cs->boxSizing == CSCD_BOX_CONTENT ? edgesMain : 0;
        if (cscdLenIsAuto(mn)) {
            bool scroll = row ? cs->overflowX != CSCD_OVERFLOW_VISIBLE : cs->overflowY != CSCD_OVERFLOW_VISIBLE;
            if (scroll) it->minMain = edgesMain;
            else {
                float content = row ? layMinContent(L, c) : CONTENT_SIZE();
                const CscdLength spec = row ? cs->width : cs->height;
                if (!cscdLenIsAuto(spec) && !cscdLenHasPercent(spec) && !cscdLenIsIntrinsic(spec)) content = cscdMinf(content, spec.px + adj);
                it->minMain = content;
            }
        } else if (!(cscdLenHasPercent(mn) && mainSize < 0)) it->minMain = cscdLenResolve(mn, cscdMaxf(0.f, mainSize), 0) + adj;
#undef CONTENT_SIZE
        if (!cscdLenIsNone(mx) && !(cscdLenHasPercent(mx) && mainSize < 0) && !cscdLenIsIntrinsic(mx)) it->maxMain = cscdLenResolve(mx, cscdMaxf(0.f, mainSize), 0) + adj;
        it->minMain = cscdMaxf(it->minMain, edgesMain);
        if (it->maxMain < it->minMain) it->maxMain = it->minMain;
        it->hypo = cscdClampf(base, it->minMain, it->maxMain);
    }

    /* container main size when indefinite (column with auto height) */
    if (mainSize < 0) {
        float sum = 0;
        for (size_t i = 0; i < items.len; ++i) sum += items.items[i].hypo + items.items[i].marginMain;
        sum += mainGap * cscdMaxi(0, (int)items.len - 1);
        /* min-height / max-height of the container apply */
        mainSize = layClampHeight(b, sum + edgesH, -1) - edgesH;
        F.mainSize = mainSize;
        wrap = false;
    }

    /* lines */
    CSCD_VEC(FlexLine) lines = {0};
    cscdVecPushZero(&lines);
    float used = 0;
    for (size_t i = 0; i < items.len; ++i) {
        FlexItem* it = &items.items[i];
        float outer = it->hypo + it->marginMain;
        if (wrap && cscdVecLast(&lines).n && used + mainGap + outer > mainSize + 0.01f) {
            cscdVecPushZero(&lines);
            cscdVecLast(&lines).from = i;
            used = 0;
        }
        if (cscdVecLast(&lines).n) used += mainGap;
        used += outer;
        cscdVecLast(&lines).n++;
    }

    /* resolve flexible lengths */
    for (size_t li = 0; li < lines.len; ++li) {
        FlexItem* li0 = items.items + lines.items[li].from;
        const size_t ln = lines.items[li].n;
        float sumHypo = 0;
        for (size_t k = 0; k < ln; ++k) sumHypo += li0[k].hypo + li0[k].marginMain;
        sumHypo += mainGap * cscdMaxi(0, (int)ln - 1);
        bool grow = sumHypo < mainSize;
        for (size_t k = 0; k < ln; ++k) {
            FlexItem* it = &li0[k];
            it->frozen = false;
            it->target = it->hypo;
            const CscdComputedStyle* cs = it->b->style;
            if ((grow && (cs->flexGrow == 0 || it->base > it->hypo)) || (!grow && (cs->flexShrink == 0 || it->base < it->hypo))) it->frozen = true;
        }
        /* initial free space, computed with frozen items at their targets and the others at their base size */
        float initialFree = mainSize - mainGap * cscdMaxi(0, (int)ln - 1);
        for (size_t k = 0; k < ln; ++k) initialFree -= (li0[k].frozen ? li0[k].target : li0[k].base) + li0[k].marginMain;
        for (int guard = 0; guard < 64; ++guard) {
            bool all = true;
            for (size_t k = 0; k < ln; ++k) all = all && li0[k].frozen;
            if (all) break;
            float freeS = mainSize - mainGap * cscdMaxi(0, (int)ln - 1);
            float sumFactor = 0, sumScaled = 0;
            for (size_t k = 0; k < ln; ++k) {
                FlexItem* it = &li0[k];
                freeS -= (it->frozen ? it->target : it->base) + it->marginMain;
                if (it->frozen) continue;
                sumFactor += grow ? it->b->style->flexGrow : it->b->style->flexShrink;
                sumScaled += it->b->style->flexShrink * it->base;
            }
            if (sumFactor < 1) {
                float scaled = initialFree * sumFactor;
                if (fabsf(scaled) < fabsf(freeS)) freeS = scaled;
            }
            float violation = 0;
            for (size_t k = 0; k < ln; ++k) {
                FlexItem* it = &li0[k];
                if (it->frozen) continue;
                const CscdComputedStyle* cs = it->b->style;
                float t = it->base;
                if (grow && sumFactor > 0) t = it->base + freeS * cs->flexGrow / sumFactor;
                else if (!grow && sumScaled > 0) t = it->base + freeS * (cs->flexShrink * it->base) / sumScaled;
                float cl = cscdClampf(t, it->minMain, it->maxMain);
                violation += cl - t;
                it->target = cl;
            }
            for (size_t k = 0; k < ln; ++k) {
                FlexItem* it = &li0[k];
                if (it->frozen) continue;
                if (fabsf(violation) < 1e-4f) it->frozen = true;
                else if (violation > 0 && it->target <= it->minMain + 1e-4f) it->frozen = true;
                else if (violation < 0 && it->target >= it->maxMain - 1e-4f) it->frozen = true;
            }
        }
    }

    /* hypothetical cross sizes */
    for (size_t i = 0; i < items.len; ++i) {
        FlexItem* it = &items.items[i];
        CscdLayoutBox* c = it->b;
        measureCross(&F, it, it->target, true);
        it->cross = (row ? c->h : c->w) + it->marginCross;
        it->baselineOffset = c->hasBaseline ? c->baseline - c->y + (row ? c->margin[0] : 0) : (row ? c->h + c->margin[0] : 0);
    }

    /* line cross sizes */
    float totalCross = 0;
    for (size_t li = 0; li < lines.len; ++li) {
        FlexLine* line = &lines.items[li];
        float mx = 0, maxB = 0, maxBelow = 0;
        for (size_t k = 0; k < line->n; ++k) {
            FlexItem* it = &items.items[line->from + k];
            if (row && (it->align == CSCD_AL_BASELINE || it->align == CSCD_AL_LAST_BASELINE)) {
                maxB = cscdMaxf(maxB, it->baselineOffset);
                maxBelow = cscdMaxf(maxBelow, it->cross - it->baselineOffset);
            } else mx = cscdMaxf(mx, it->cross);
        }
        line->maxBaseline = maxB;
        line->cross = cscdMaxf(mx, maxB + maxBelow);
        totalCross += line->cross;
    }
    totalCross += crossGap * cscdMaxi(0, (int)lines.len - 1);
    if (lines.len == 1 && crossSize >= 0) lines.items[0].cross = crossSize;
    float containerCross = crossSize >= 0 ? crossSize : (row ? layClampHeight(b, totalCross + edgesH, -1) - edgesH : cw);
    if (lines.len == 1 && crossSize < 0 && row) lines.items[0].cross = cscdMaxf(lines.items[0].cross, containerCross);

    /* align-content */
    float freeCross = containerCross - totalCross;
    uint8_t ac = s->alignContent;
    float crossStart = 0, crossBetween = crossGap;
    if (lines.len > 1 || (wrap && lines.len == 1 && crossSize >= 0)) {
        if (ac == CSCD_AL_NORMAL || ac == CSCD_AL_STRETCH) {
            if (freeCross > 0 && wrap) for (size_t li = 0; li < lines.len; ++li) lines.items[li].cross += freeCross / lines.len;
        } else if (ac == CSCD_AL_FLEX_END || ac == CSCD_AL_END) crossStart = freeCross;
        else if (ac == CSCD_AL_CENTER) crossStart = freeCross / 2;
        else if (ac == CSCD_AL_SPACE_BETWEEN && lines.len > 1) crossBetween += cscdMaxf(0.f, freeCross) / (lines.len - 1);
        else if (ac == CSCD_AL_SPACE_AROUND) { float g = cscdMaxf(0.f, freeCross) / lines.len; crossStart = g / 2; crossBetween += g; }
        else if (ac == CSCD_AL_SPACE_EVENLY) { float g = cscdMaxf(0.f, freeCross) / (lines.len + 1); crossStart = g; crossBetween += g; }
    }
    float pos = crossStart;
    const bool wrapReverse = s->flexWrap == CSCD_FLEX_WRAP_REVERSE;
    for (size_t k = 0; k < lines.len; ++k) {
        FlexLine* l = &lines.items[wrapReverse ? lines.len - 1 - k : k];
        l->pos = pos;
        pos += l->cross + crossBetween;
    }

    /* final layout and positions */
    float mainOrigin = row ? cx : cy;
    float crossOrigin = row ? cy : cx;
    bool firstBaseline = false;
    for (size_t li = 0; li < lines.len; ++li) {
        FlexLine* line = &lines.items[li];
        FlexItem* li0 = items.items + line->from;
        const size_t ln = line->n;
        /* stretch */
        for (size_t k = 0; k < ln; ++k) {
            FlexItem* it = &li0[k];
            CscdLayoutBox* c = it->b;
            const CscdComputedStyle* cs = c->style;
            const CscdLength crossLen = row ? cs->height : cs->width;
            if (it->align == CSCD_AL_STRETCH && cscdLenIsAuto(crossLen) && !it->autoCrossStart && !it->autoCrossEnd) {
                float target = line->cross - it->marginCross;
                if (row) {
                    BlockInput in = blockInput();
                    in.positionedByCaller = true;
                    in.cbW = cw;
                    in.cbH = containerCross;
                    in.forcedW = it->target;
                    in.forcedH = layClampHeight(c, target, containerCross);
                    layBlock(L, c, &in);
                } else if (fabsf(c->w - target) > 0.01f) {
                    BlockInput in = blockInput();
                    in.positionedByCaller = true;
                    in.cbW = cw;
                    in.cbH = mainSize;
                    in.forcedW = layClampWidth(L, c, target, cw);
                    in.forcedH = it->target;
                    layBlock(L, c, &in);
                }
                it->cross = (row ? c->h : c->w) + it->marginCross;
            }
        }
        /* main axis */
        float freeM = mainSize - mainGap * cscdMaxi(0, (int)ln - 1);
        int autoMargins = 0;
        for (size_t k = 0; k < ln; ++k) {
            freeM -= li0[k].target + li0[k].marginMain;
            autoMargins += li0[k].autoMainStart + li0[k].autoMainEnd;
        }
        float start = 0, between = mainGap, autoShare = 0;
        if (autoMargins > 0 && freeM > 0) autoShare = freeM / autoMargins;
        else {
            uint8_t jc = s->justifyContent;
            if (jc == CSCD_AL_NORMAL || jc == CSCD_AL_STRETCH) jc = CSCD_AL_FLEX_START;
            if (jc == CSCD_AL_START || jc == CSCD_AL_LEFT) jc = reverse ? CSCD_AL_FLEX_END : CSCD_AL_FLEX_START;
            if (jc == CSCD_AL_END || jc == CSCD_AL_RIGHT) jc = reverse ? CSCD_AL_FLEX_START : CSCD_AL_FLEX_END;
            if (jc == CSCD_AL_FLEX_END) start = freeM;
            else if (jc == CSCD_AL_CENTER) start = freeM / 2;
            else if (jc == CSCD_AL_SPACE_BETWEEN) { if (ln > 1 && freeM > 0) between += freeM / (ln - 1); }
            else if (jc == CSCD_AL_SPACE_AROUND) { if (freeM > 0) { float g = freeM / ln; start = g / 2; between += g; } else start = freeM / 2; }
            else if (jc == CSCD_AL_SPACE_EVENLY) { if (freeM > 0) { float g = freeM / (ln + 1); start = g; between += g; } else start = freeM / 2; }
        }
        float m = start;
        for (size_t k = 0; k < ln; ++k) {
            FlexItem* it = &li0[k];
            CscdLayoutBox* c = it->b;
            float msMargin = (row ? c->margin[3] : c->margin[0]) + (it->autoMainStart ? autoShare : 0);
            float meMargin = (row ? c->margin[1] : c->margin[2]) + (it->autoMainEnd ? autoShare : 0);
            float mainPos;
            if (!reverse) mainPos = mainOrigin + m + msMargin;
            else mainPos = mainOrigin + mainSize - m - meMargin - it->target;
            m += msMargin + it->target + meMargin + between;
            /* cross axis */
            float lineStart = crossOrigin + line->pos;
            float csMargin = row ? c->margin[0] : c->margin[3];
            float ceMargin = row ? c->margin[2] : c->margin[1];
            float itemCross = row ? c->h : c->w;
            float crossFree = line->cross - itemCross - csMargin - ceMargin;
            float crossPos;
            uint8_t a = it->align;
            if (it->autoCrossStart || it->autoCrossEnd) {
                if (it->autoCrossStart && it->autoCrossEnd) crossPos = lineStart + csMargin + cscdMaxf(0.f, crossFree) / 2;
                else if (it->autoCrossStart) crossPos = lineStart + csMargin + cscdMaxf(0.f, crossFree);
                else crossPos = lineStart + csMargin;
            } else if (a == CSCD_AL_FLEX_END || a == CSCD_AL_END || a == CSCD_AL_SELF_END) crossPos = lineStart + csMargin + crossFree;
            else if (a == CSCD_AL_CENTER) crossPos = lineStart + csMargin + crossFree / 2;
            else if ((a == CSCD_AL_BASELINE || a == CSCD_AL_LAST_BASELINE) && row) crossPos = lineStart + (line->maxBaseline - it->baselineOffset) + csMargin;
            else crossPos = lineStart + csMargin;
            if (wrapReverse && !(a == CSCD_AL_BASELINE)) {
                /* cross-start and cross-end are swapped */
                if (a == CSCD_AL_FLEX_START || a == CSCD_AL_START || a == CSCD_AL_STRETCH) crossPos = lineStart + csMargin + crossFree;
                else if (a == CSCD_AL_FLEX_END || a == CSCD_AL_END) crossPos = lineStart + csMargin;
            }
            float nx = row ? mainPos : crossPos, ny = row ? crossPos : mainPos;
            cscdBoxShift(c, nx - c->x, ny - c->y);
            layApplyRelative(c, cw, row ? containerCross : mainSize);
            if (!firstBaseline && c->hasBaseline) {
                b->baseline = c->baseline;
                b->lastBaseline = c->lastBaseline;
                b->hasBaseline = true;
                firstBaseline = true;
            }
        }
    }
    if (!firstBaseline && items.len) {
        CscdLayoutBox* c = items.items[0].b;
        b->baseline = b->lastBaseline = c->y + c->h;
        b->hasBaseline = true;
    }
    cscdVecFree(&items);
    cscdVecFree(&lines);
    return row ? containerCross : mainSize;
}

/* ---- grid ---------------------------------------------------------------------------------------- */

typedef struct GridItem {
    CscdLayoutBox* b;
    int r0, r1, c0, c1; /* 0-based track indices, end exclusive */
    float extra;        /* (column sizing of a subgrid's items: its edges at the subgrid's first / last column) */
} GridItem;
typedef CSCD_VEC(GridItem) GridItemVec;

typedef struct Track {
    CscdTrackSize spec;
    float base, limit;
    bool flexible;
    float fr;
    float pos;
} Track;
typedef CSCD_VEC(Track) TrackVec;

static GridItem gridItem(CscdLayoutBox* b) {
    GridItem g = {b, -1, -1, -1, -1, 0};
    return g;
}

/* The explicit tracks, repeat(auto-fill/auto-fit) expanded, into out. */
static void expandTracks(const CscdTrackList* tl, float avail, float gap, CscdTrackVec* out) {
    out->len = 0;
    cscdVecAppend(out, tl->tracks.items, tl->tracks.len);
    if (tl->autoRepeatAt >= 0 && tl->autoTracks.len) {
        float fixedSum = 0, unit = 0;
        for (size_t i = 0; i < tl->tracks.len; ++i)
            if (tl->tracks.items[i].kind == CSCD_TRACK_FIXED) fixedSum += cscdLenResolve(tl->tracks.items[i].len, cscdMaxf(0.f, avail), 0) + gap;
        for (size_t i = 0; i < tl->autoTracks.len; ++i) {
            const CscdTrackSize* t = &tl->autoTracks.items[i];
            float v = 0;
            if (t->kind == CSCD_TRACK_FIXED) v = cscdLenResolve(t->len, cscdMaxf(0.f, avail), 0);
            else if (t->kind == CSCD_TRACK_MINMAX) {
                if (t->min->kind == CSCD_TRACK_FIXED) v = cscdLenResolve(t->min->len, cscdMaxf(0.f, avail), 0);
                if (t->max->kind == CSCD_TRACK_FIXED) v = cscdMaxf(v, cscdLenResolve(t->max->len, cscdMaxf(0.f, avail), 0));
            }
            unit += cscdMaxf(v, 1.f) + gap;
        }
        int count = 1;
        if (avail > 0 && unit > 0) count = cscdMaxi(1, cscdIntOf(floorf((avail - fixedSum + gap) / unit + 1e-4f)));
        size_t at = (size_t)cscdMini((int)out->len, tl->autoRepeatAt);
        for (int i = 0; i < count && i < 1000; ++i) {
            for (size_t k = 0; k < tl->autoTracks.len; ++k) cscdVecInsert(out, at, tl->autoTracks.items[k]), ++at;
        }
    }
}

typedef struct Area { int a[4]; } Area; /* r0 c0 r1 c1 */

typedef struct Placer {
    const CscdComputedStyle* s;
    CscdStrMap areas; /* name -> index in areaList */
    CSCD_VEC(Area) areaList;
    CSCD_VEC(CscdBytes) occ;
    int cols, rows;
} Placer;

static const Area* findArea(const Placer* P, CscdSv name) {
    const CscdMapVal* v = cscdStrMapGet(&P->areas, name);
    return v ? &P->areaList.items[v->u] : NULL;
}

static int lineOf(const Placer* P, const CscdGridLine* g, int explicitCount, bool isStart, bool isRow) {
    if (g->name.len) {
        const CscdSv name = cscdStrSv(&g->name);
        const Area* it = findArea(P, name);
        if (cscdSvEndsC(name, "-start") || cscdSvEndsC(name, "-end")) {
            bool st = cscdSvEndsC(name, "-start");
            const Area* a = findArea(P, cscdSvSub(name, 0, name.len - (st ? 6 : 4)));
            if (a) return isRow ? (st ? a->a[0] : a->a[2]) : (st ? a->a[1] : a->a[3]);
        }
        if (it) return isRow ? (isStart ? it->a[0] : it->a[2]) : (isStart ? it->a[1] : it->a[3]);
        /* named lines in the track list */
        const CscdTrackList* tl = isRow ? &P->s->gridRows : &P->s->gridColumns;
        for (size_t i = 0; i < tl->lineNames.len; ++i)
            for (size_t k = 0; k < tl->lineNames.items[i].len; ++k)
                if (cscdStrEq(&tl->lineNames.items[i].items[k], &g->name)) return (int)i;
        return -1;
    }
    if (g->line > 0) return g->line - 1;
    if (g->line < 0) return cscdMaxi(0, explicitCount + 1 + g->line);
    return -1;
}

static void ensure(Placer* P, int r, int c) {
    if (c > P->cols) P->cols = c;
    while ((int)P->occ.len < r) cscdVecPushZero(&P->occ);
    for (size_t i = 0; i < P->occ.len; ++i)
        if ((int)P->occ.items[i].len < P->cols) cscdVecResize(&P->occ.items[i], (size_t)P->cols);
    P->rows = cscdMaxi(P->rows, r);
}

static bool isFree(Placer* P, int r0, int c0, int rs, int cs) {
    ensure(P, r0 + rs, c0 + cs);
    for (int r = r0; r < r0 + rs; ++r)
        for (int c = c0; c < c0 + cs; ++c)
            if (P->occ.items[r].items[c]) return false;
    return true;
}

static void mark(Placer* P, const GridItem* it) {
    ensure(P, it->r1, it->c1);
    for (int r = it->r0; r < it->r1; ++r)
        for (int c = it->c0; c < it->c1; ++c) P->occ.items[r].items[c] = 1;
}

/* The grid lines an item can name (as browsers bound them): placement keeps a cell map of the grid,
 * so "grid-column: 1 / 99999999" must not make one of that size. */
#define kMaxGridLine 1000

static bool resolveAxis(const Placer* P, const CscdGridLine* a, const CscdGridLine* e, int explicitCount, bool isRow, int* s0, int* s1) {
    int st = lineOf(P, a, explicitCount, true, isRow), en = lineOf(P, e, explicitCount, false, isRow);
    if (st > kMaxGridLine) st = kMaxGridLine;
    if (en > kMaxGridLine) en = kMaxGridLine;
    int span = 1;
    if (a->span > 0) span = cscdMini(a->span, kMaxGridLine);
    if (e->span > 0) span = cscdMini(e->span, kMaxGridLine);
    if (st >= 0 && en >= 0) {
        if (en < st) { int t = st; st = en; en = t; }
        if (en == st) en = st + 1;
        *s0 = st; *s1 = en;
        return true;
    }
    if (st >= 0) { *s0 = st; *s1 = st + span; return true; }
    if (en >= 0) { *s1 = en; *s0 = cscdMaxi(0, en - span); return true; }
    *s0 = -1; *s1 = span;
    return false;
}

static void placeItems(CscdLayoutBox* b, GridItemVec* items, int* outCols, int* outRows, const CscdTrackVec* colTracks, const CscdTrackVec* rowTracks) {
    const CscdComputedStyle* s = b->style;
    Placer P;
    memset(&P, 0, sizeof P);
    P.s = s;
    int explicitCols = (int)colTracks->len, explicitRows = (int)rowTracks->len;
    /* named areas */
    CscdSvVec names = {0};
    for (int r = 0; r < (int)s->gridAreas.len; ++r) {
        names.len = 0;
        cscdSplitSpaces(cscdStrSv(&s->gridAreas.items[r]), &names);
        for (int c = 0; c < (int)names.len; ++c) {
            CscdSv n = names.items[c];
            if (!n.len || n.ptr[0] == '.') continue;
            bool inserted = false;
            CscdMapVal* v = cscdStrMapPut(&P.areas, n, &inserted);
            if (inserted) {
                v->u = P.areaList.len;
                Area a = {{r, c, r + 1, c + 1}};
                cscdVecPush(&P.areaList, a);
            } else {
                int* a = P.areaList.items[v->u].a;
                a[0] = cscdMini(a[0], r); a[1] = cscdMini(a[1], c); a[2] = cscdMaxi(a[2], r + 1); a[3] = cscdMaxi(a[3], c + 1);
            }
        }
        explicitCols = cscdMaxi(explicitCols, (int)names.len);
    }
    cscdVecFree(&names);
    explicitRows = cscdMaxi(explicitRows, (int)s->gridAreas.len);
    P.cols = cscdMaxi(1, explicitCols);
    P.rows = cscdMaxi(0, explicitRows);
    /* pass 1: items with both axes fixed, pass 2: the rest in order */
    const size_t n = items->len;
    int* spanR = cscdCalloc(n + 1, sizeof(int));
    int* spanC = cscdCalloc(n + 1, sizeof(int));
    bool* fixedR = cscdCalloc(n + 1, sizeof(bool));
    bool* fixedC = cscdCalloc(n + 1, sizeof(bool));
    for (size_t i = 0; i < n; ++i) {
        GridItem* it = &items->items[i];
        const CscdComputedStyle* cs = it->b->style;
        CscdGridLine rs = cs->gridRowStart, re = cs->gridRowEnd, csl = cs->gridColumnStart, ce = cs->gridColumnEnd;
        if (cs->gridArea.len) rs.name = re.name = csl.name = ce.name = cs->gridArea; /* (borrowed) */
        fixedR[i] = resolveAxis(&P, &rs, &re, explicitRows, true, &it->r0, &it->r1);
        fixedC[i] = resolveAxis(&P, &csl, &ce, explicitCols, false, &it->c0, &it->c1);
        spanR[i] = fixedR[i] ? it->r1 - it->r0 : it->r1;
        spanC[i] = fixedC[i] ? it->c1 - it->c0 : it->c1;
        if (fixedC[i]) P.cols = cscdMaxi(P.cols, it->c1);
    }
    for (size_t i = 0; i < n; ++i) if (fixedR[i] && fixedC[i]) mark(&P, &items->items[i]);
    bool columnFlow = s->gridAutoFlow == CSCD_GRID_COLUMN || s->gridAutoFlow == CSCD_GRID_COLUMN_DENSE;
    int curR = 0, curC = 0;
    for (size_t i = 0; i < n; ++i) {
        if (fixedR[i] && fixedC[i]) continue;
        GridItem* it = &items->items[i];
        int rs = spanR[i], cs = cscdMini(spanC[i], cscdMaxi(P.cols, spanC[i]));
        if (fixedC[i] && !columnFlow) {
            /* a definite column: the cursor moves to it (a row down when that is behind it), then
             * down to the first row where it fits (CSS Grid 8.5, step 4) */
            if (it->c0 < curC) ++curR;
            curC = it->c0;
            while (!isFree(&P, curR, it->c0, rs, it->c1 - it->c0)) ++curR;
            it->r0 = curR; it->r1 = curR + rs;
        } else if (fixedC[i]) {
            /* find a row where the fixed columns are free */
            int r = 0;
            while (!isFree(&P, r, it->c0, rs, it->c1 - it->c0)) ++r;
            it->r0 = r; it->r1 = r + rs;
        } else if (fixedR[i]) {
            int c = 0;
            while (!isFree(&P, it->r0, c, it->r1 - it->r0, cs)) ++c;
            it->c0 = c; it->c1 = c + cs;
        } else if (!columnFlow) {
            if (cs > P.cols) P.cols = cs;
            for (;;) {
                if (curC + cs > P.cols) { curC = 0; ++curR; }
                if (isFree(&P, curR, curC, rs, cs)) break;
                ++curC;
            }
            it->r0 = curR; it->r1 = curR + rs; it->c0 = curC; it->c1 = curC + cs;
            curC += cs;
        } else {
            int rowsLimit = cscdMaxi(1, explicitRows);
            for (;;) {
                if (curR + rs > rowsLimit) { curR = 0; ++curC; }
                if (isFree(&P, curR, curC, rs, cs)) break;
                ++curR;
            }
            it->r0 = curR; it->r1 = curR + rs; it->c0 = curC; it->c1 = curC + cs;
            curR += rs;
        }
        mark(&P, it);
    }
    for (size_t i = 0; i < n; ++i) { P.cols = cscdMaxi(P.cols, items->items[i].c1); P.rows = cscdMaxi(P.rows, items->items[i].r1); }
    *outCols = P.cols;
    *outRows = P.rows;
    cscdFree(spanR); cscdFree(spanC); cscdFree(fixedR); cscdFree(fixedC);
    for (size_t i = 0; i < P.occ.len; ++i) cscdVecFree(&P.occ.items[i]);
    cscdVecFree(&P.occ);
    cscdVecFree(&P.areaList);
    cscdStrMapFree(&P.areas);
}

static CscdTrackSize trackFor(const CscdTrackVec* explicitTracks, const CscdTrackVec* autoTracks, int i) {
    if (i < (int)explicitTracks->len) return explicitTracks->items[i];
    if (autoTracks->len) return autoTracks->items[((size_t)i - explicitTracks->len) % autoTracks->len];
    return cscdTrackAuto();
}

/* the outer size of a grid item in one axis (min: min-content) */
typedef float (*ContributionFn)(Layouter* L, const GridItem* it, bool min, bool allMin);

static bool fixedOf(const CscdTrackSize* x, float avail, float* out) {
    if (x->kind == CSCD_TRACK_FIXED && !(cscdLenHasPercent(x->len) && avail < 0)) { *out = cscdLenResolve(x->len, cscdMaxf(0.f, avail), 0); return true; }
    return false;
}

static bool isIntrinsicMin(const CscdTrackSize* sp) {
    if (sp->kind == CSCD_TRACK_AUTO || sp->kind == CSCD_TRACK_MIN_CONTENT || sp->kind == CSCD_TRACK_MAX_CONTENT || sp->kind == CSCD_TRACK_FIT_CONTENT) return true;
    if (sp->kind == CSCD_TRACK_MINMAX) return sp->min->kind != CSCD_TRACK_FIXED;
    return false;
}

static bool isIntrinsicMax(const CscdTrackSize* sp) {
    if (sp->kind == CSCD_TRACK_AUTO || sp->kind == CSCD_TRACK_MIN_CONTENT || sp->kind == CSCD_TRACK_MAX_CONTENT || sp->kind == CSCD_TRACK_FIT_CONTENT) return true;
    if (sp->kind == CSCD_TRACK_MINMAX) return sp->max->kind == CSCD_TRACK_AUTO || sp->max->kind == CSCD_TRACK_MAX_CONTENT || sp->max->kind == CSCD_TRACK_MIN_CONTENT;
    return false;
}

/* Sizes tracks for one axis. contribution gives the outer size of an item. */
static void sizeTracks(Layouter* L, TrackVec* tracksV, const GridItemVec* items, bool columns, float avail, float gap,
                       ContributionFn contribution, bool allMin, bool stretchAuto) {
    Track* tracks = tracksV->items;
    int n = (int)tracksV->len;
    for (int k = 0; k < n; ++k) {
        Track* t = &tracks[k];
        const CscdTrackSize* sp = &t->spec;
        t->flexible = false;
        float v;
        switch (sp->kind) {
        case CSCD_TRACK_FIXED: if (fixedOf(sp, avail, &v)) { t->base = t->limit = v; } else { t->base = 0; t->limit = 1e30f; } break;
        case CSCD_TRACK_FR: t->flexible = true; t->fr = sp->fr; t->base = 0; t->limit = 1e30f; break;
        case CSCD_TRACK_MINMAX:
            t->base = 0; t->limit = 1e30f;
            if (fixedOf(sp->min, avail, &v)) t->base = v;
            if (sp->max->kind == CSCD_TRACK_FR) { t->flexible = true; t->fr = sp->max->fr; }
            else if (fixedOf(sp->max, avail, &v)) t->limit = cscdMaxf(v, t->base);
            break;
        default: t->base = 0; t->limit = 0; break;
        }
    }
    /* intrinsic contributions: items spanning one track first, then the others */
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < items->len; ++i) {
            const GridItem* it = &items->items[i];
            int a = columns ? it->c0 : it->r0, e = columns ? it->c1 : it->r1;
            int span = e - a;
            if ((pass == 0) != (span == 1)) continue;
            bool anyFlex = false;
            for (int k = a; k < e && k < n; ++k) anyFlex = anyFlex || tracks[k].flexible;
            if (anyFlex && span > 1) continue;
            float minC = -1, maxC = -1;
#define GET_MIN() (minC < 0 ? (minC = contribution(L, it, true, allMin)) : minC)
#define GET_MAX() (maxC < 0 ? (maxC = contribution(L, it, false, allMin)) : maxC)
            if (span == 1) {
                Track* t = &tracks[a];
                const CscdTrackSize* sp = &t->spec;
                if (isIntrinsicMin(sp)) {
                    float c = sp->kind == CSCD_TRACK_MAX_CONTENT ? GET_MAX() : GET_MIN();
                    if (sp->kind == CSCD_TRACK_AUTO && !columns) c = GET_MAX();
                    t->base = cscdMaxf(t->base, c);
                }
                if (isIntrinsicMax(sp)) {
                    float c = (sp->kind == CSCD_TRACK_MIN_CONTENT || (sp->kind == CSCD_TRACK_MINMAX && sp->max->kind == CSCD_TRACK_MIN_CONTENT)) ? GET_MIN() : GET_MAX();
                    if (sp->kind == CSCD_TRACK_FIT_CONTENT) c = cscdMinf(c, cscdMaxf(GET_MIN(), cscdLenResolve(sp->len, cscdMaxf(0.f, avail), 0)));
                    t->limit = cscdMaxf(t->limit, c);
                }
                if (t->flexible && sp->kind == CSCD_TRACK_FR && avail < 0) t->base = cscdMaxf(t->base, GET_MAX());
            } else {
                float cur = gap * (span - 1), curLimit = gap * (span - 1);
                int intrinsicCount = 0;
                for (int k = a; k < e; ++k) {
                    cur += tracks[k].base;
                    curLimit += cscdMinf(tracks[k].limit, 1e29f);
                    intrinsicCount += isIntrinsicMin(&tracks[k].spec);
                }
                float need = GET_MIN() - cur;
                if (need > 0 && intrinsicCount)
                    for (int k = a; k < e; ++k) if (isIntrinsicMin(&tracks[k].spec)) tracks[k].base += need / intrinsicCount;
                float needMax = GET_MAX() - curLimit;
                if (needMax > 0 && intrinsicCount)
                    for (int k = a; k < e; ++k) if (isIntrinsicMax(&tracks[k].spec)) tracks[k].limit += needMax / intrinsicCount;
            }
#undef GET_MIN
#undef GET_MAX
        }
    }
    for (int k = 0; k < n; ++k) if (tracks[k].limit < tracks[k].base) tracks[k].limit = tracks[k].base;
    float gaps = gap * cscdMaxi(0, n - 1);
    /* maximize: grow tracks toward their limits */
    if (avail >= 0) {
        float used = gaps;
        for (int k = 0; k < n; ++k) used += tracks[k].base;
        float freeS = avail - used;
        for (int guard = 0; guard < 16 && freeS > 0.01f; ++guard) {
            int growable = 0;
            for (int k = 0; k < n; ++k) {
                const Track* t = &tracks[k];
                if (!t->flexible && t->limit > t->base + 0.01f && t->limit < 1e29f) ++growable;
            }
            if (!growable) break;
            float share = freeS / growable;
            for (int k = 0; k < n; ++k) {
                Track* t = &tracks[k];
                if (t->flexible || !(t->limit > t->base + 0.01f) || t->limit >= 1e29f) continue;
                float g = cscdMinf(share, t->limit - t->base);
                t->base += g;
                freeS -= g;
            }
        }
    } else {
        for (int k = 0; k < n; ++k) if (!tracks[k].flexible && tracks[k].limit < 1e29f) tracks[k].base = tracks[k].limit;
    }
    /* flexible tracks */
    float sumFr = 0;
    for (int k = 0; k < n; ++k) if (tracks[k].flexible) sumFr += tracks[k].fr;
    if (sumFr > 0) {
        if (avail >= 0) {
            float leftover = avail - gaps;
            for (int k = 0; k < n; ++k) if (!tracks[k].flexible) leftover -= tracks[k].base;
            /* tracks whose base exceeds their share are treated as inflexible */
            bool* inflex = cscdCalloc((size_t)n + 1, sizeof(bool));
            float frSize = 0;
            for (int guard = 0; guard < 8; ++guard) {
                float lo = leftover, fr = 0;
                for (int k = 0; k < n; ++k) {
                    if (!tracks[k].flexible) continue;
                    if (inflex[k]) lo -= tracks[k].base;
                    else fr += tracks[k].fr;
                }
                frSize = fr > 0 ? cscdMaxf(0.f, lo) / cscdMaxf(fr, 1.f) : 0;
                if (fr > 0 && fr < 1) frSize = cscdMaxf(0.f, lo) / 1.f;
                bool changed = false;
                for (int k = 0; k < n; ++k) {
                    if (!tracks[k].flexible || inflex[k]) continue;
                    if (tracks[k].fr * frSize < tracks[k].base) { inflex[k] = true; changed = true; }
                }
                if (!changed) break;
            }
            for (int k = 0; k < n; ++k)
                if (tracks[k].flexible && !inflex[k]) tracks[k].base = cscdMaxf(tracks[k].base, tracks[k].fr * frSize);
            cscdFree(inflex);
        } else {
            /* indefinite: the flex fraction is the largest base / fr */
            float frSize = 0;
            for (int k = 0; k < n; ++k) if (tracks[k].flexible && tracks[k].fr > 0) frSize = cscdMaxf(frSize, tracks[k].base / tracks[k].fr);
            for (int k = 0; k < n; ++k) if (tracks[k].flexible) tracks[k].base = cscdMaxf(tracks[k].base, tracks[k].fr * frSize);
        }
    }
    /* stretch auto tracks into the remaining space */
    if (stretchAuto && avail >= 0) {
        float used = gaps;
        int autos = 0;
        for (int k = 0; k < n; ++k) { used += tracks[k].base; if (tracks[k].spec.kind == CSCD_TRACK_AUTO) ++autos; }
        if (used < avail && autos && sumFr == 0)
            for (int k = 0; k < n; ++k) if (tracks[k].spec.kind == CSCD_TRACK_AUTO) tracks[k].base += (avail - used) / autos;
    }
}

static float contentContribution(Layouter* L, const GridItem* it, bool mn, bool allMin) {
    return ((mn || allMin) ? layOuterMinContent(L, it->b) : layOuterMaxContent(L, it->b)) + it->extra;
}

static float heightContribution(Layouter* L, const GridItem* it, bool mn, bool allMin) {
    (void)L; (void)mn; (void)allMin;
    return it->b->h + it->b->margin[0] + it->b->margin[2];
}

static void gridItemsOf(CscdLayoutBox* b, GridItemVec* out) {
    for (size_t i = 0; i < b->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(b, i);
        if (!isAbsolute(c) && !c->isMarker) cscdVecPush(out, gridItem(c));
    }
}

static void tracksFor(TrackVec* out, int n, const CscdTrackVec* explicitTracks, const CscdTrackVec* autoTracks) {
    cscdVecResize(out, (size_t)n);
    for (int i = 0; i < n; ++i) out->items[i].spec = trackFor(explicitTracks, autoTracks, i);
}

float layIntrinsicGrid(Layouter* L, CscdLayoutBox* b, bool min) {
    const CscdComputedStyle* s = b->style;
    GridItemVec items = {0};
    gridItemsOf(b, &items);
    float gap = gapOf(s->columnGap, 0);
    CscdTrackVec colTracks = {0}, rowTracks = {0};
    expandTracks(&s->gridColumns, -1, gap, &colTracks);
    expandTracks(&s->gridRows, -1, 0, &rowTracks);
    int cols, rows;
    placeItems(b, &items, &cols, &rows, &colTracks, &rowTracks);
    TrackVec tracks = {0};
    tracksFor(&tracks, cols, &colTracks, &s->gridAutoColumns);
    sizeTracks(L, &tracks, &items, true, -1, gap, contentContribution, min, false);
    float total = gap * cscdMaxi(0, cols - 1);
    for (size_t k = 0; k < tracks.len; ++k) total += tracks.items[k].base;
    cscdVecFree(&tracks);
    cscdVecFree(&colTracks);
    cscdVecFree(&rowTracks);
    cscdVecFree(&items);
    return total;
}

static void fixedTracks(const CscdFloatVec* sizes, CscdTrackVec* out) {
    out->len = 0;
    for (size_t i = 0; i < sizes->len; ++i) {
        CscdTrackSize t;
        memset(&t, 0, sizeof t);
        t.kind = CSCD_TRACK_FIXED;
        t.len = cscdLenFixed(cscdMaxf(0.f, sizes->items[i]));
        cscdVecPush(out, t);
    }
}

static bool subCols(const CscdLayoutBox* c) { return c->kind == CSCD_BOX_GRID && c->style->gridColumns.subgrid; }
static bool subRows(const CscdLayoutBox* c) { return c->kind == CSCD_BOX_GRID && c->style->gridRows.subgrid; }

/* what a subgrid's margin, border and padding take from its first and last tracks */
static float edgeStart(const CscdLayoutBox* c, bool rowsAxis) {
    return rowsAxis ? c->margin[0] + c->border[0] + c->padding[0] : c->margin[3] + c->border[3] + c->padding[3];
}
static float edgeEnd(const CscdLayoutBox* c, bool rowsAxis) {
    return rowsAxis ? c->margin[2] + c->border[2] + c->padding[2] : c->margin[1] + c->border[1] + c->padding[1];
}

static void slice(const TrackVec* tr, int a, int e, const CscdLayoutBox* c, bool rowsAxis, CscdFloatVec* out) {
    out->len = 0;
    for (int k = a; k < e && k < (int)tr->len; ++k) cscdVecPush(out, tr->items[k].base);
    if (out->len) {
        out->items[0] -= edgeStart(c, rowsAxis);
        out->items[out->len - 1] -= edgeEnd(c, rowsAxis);
    }
}

static SubgridTracks* subgridTracks(Layouter* L, CscdLayoutBox* c) {
    bool inserted = false;
    CscdMapVal* v = cscdU64MapPut(&L->subgridIn, CSCD_PKEY(c), &inserted);
    if (inserted) v->p = CSCD_NEW(SubgridTracks);
    return (SubgridTracks*)v->p;
}

/* track positions with content distribution; returns the extent */
static float distribute(TrackVec* tr, float avail, float gap, uint8_t a) {
    float used = gap * cscdMaxi(0, (int)tr->len - 1);
    for (size_t k = 0; k < tr->len; ++k) used += tr->items[k].base;
    float freeS = avail >= 0 ? avail - used : 0;
    float start = 0, between = gap;
    if (freeS > 0 && tr->len) {
        if (a == CSCD_AL_END || a == CSCD_AL_FLEX_END || a == CSCD_AL_RIGHT) start = freeS;
        else if (a == CSCD_AL_CENTER) start = freeS / 2;
        else if (a == CSCD_AL_SPACE_BETWEEN && tr->len > 1) between += freeS / (tr->len - 1);
        else if (a == CSCD_AL_SPACE_AROUND) { float g = freeS / tr->len; start = g / 2; between += g; }
        else if (a == CSCD_AL_SPACE_EVENLY) { float g = freeS / (tr->len + 1); start = g; between += g; }
    }
    float p = start;
    for (size_t k = 0; k < tr->len; ++k) { tr->items[k].pos = p; p += tr->items[k].base + between; }
    return p - between;
}

static uint8_t justifySelfOf(const CscdLayoutBox* c, const CscdComputedStyle* s) {
    uint8_t js = c->style->justifySelf;
    if (js == CSCD_AL_AUTO) js = s->justifyItems;
    if (js == CSCD_AL_NORMAL) js = c->kind == CSCD_BOX_REPLACED ? CSCD_AL_START : CSCD_AL_STRETCH;
    return js;
}

float layGrid(Layouter* L, CscdLayoutBox* b, float cbH) {
    const CscdComputedStyle* s = b->style;
    float cx = cscdBoxContentX(b), cy = cscdBoxContentY(b), cw = cscdBoxContentW(b);
    float colGap = gapOf(s->columnGap, cw), rowGap = gapOf(s->rowGap, cbH >= 0 ? cbH : 0);
    CscdPtrVec boxes = {0};
    inFlowItems(L, b, &boxes);
    GridItemVec items = {0};
    for (size_t i = 0; i < boxes.len; ++i) cscdVecPush(&items, gridItem((CscdLayoutBox*)boxes.items[i]));
    cscdVecFree(&boxes);
    CscdTrackVec colTracks = {0}, rowTracks = {0};
    expandTracks(&s->gridColumns, cw, colGap, &colTracks);
    expandTracks(&s->gridRows, cbH, rowGap, &rowTracks);
    /* a subgrid takes the tracks its parent gives it (and the parent's gap, unless it sets one) */
    const CscdMapVal* gv = cscdU64MapGet(&L->subgridIn, CSCD_PKEY(b));
    const SubgridTracks* given = gv ? (const SubgridTracks*)gv->p : NULL;
    if (s->gridColumns.subgrid && given && given->cols.len) {
        fixedTracks(&given->cols, &colTracks);
        if (cscdLenIsNone(s->columnGap)) colGap = given->colGap;
    }
    if (s->gridRows.subgrid && given && given->rows.len) {
        fixedTracks(&given->rows, &rowTracks);
        if (cscdLenIsNone(s->rowGap)) rowGap = given->rowGap;
    }
    int cols, rows;
    placeItems(b, &items, &cols, &rows, &colTracks, &rowTracks);
    TrackVec ct = {0}, rt = {0};
    tracksFor(&ct, cols, &colTracks, &s->gridAutoColumns);
    tracksFor(&rt, rows, &rowTracks, &s->gridAutoRows);
    bool stretchCols = s->justifyContent == CSCD_AL_NORMAL || s->justifyContent == CSCD_AL_STRETCH;
    /* the columns: a subgrid on columns is not sized as one item; its items, placed in it, size the
     * parent's columns they lie in (its edges added to its first and last columns) */
    GridItemVec colItems = {0}, inner = {0};
    CscdTrackVec spanTracks = {0}, innerRows = {0};
    for (size_t i = 0; i < items.len; ++i) {
        GridItem* it = &items.items[i];
        CscdLayoutBox* c = it->b;
        if (!subCols(c)) { cscdVecPush(&colItems, *it); continue; }
        layResolveEdges(c, cw);
        inner.len = 0;
        gridItemsOf(c, &inner);
        const int span = it->c1 - it->c0;
        spanTracks.len = 0;
        for (int k = 0; k < span; ++k) cscdVecPush(&spanTracks, cscdTrackAuto());
        expandTracks(&c->style->gridRows, -1, 0, &innerRows);
        if (c->style->gridRows.subgrid) {
            innerRows.len = 0;
            for (int k = 0; k < cscdMaxi(1, it->r1 - it->r0); ++k) cscdVecPush(&innerRows, cscdTrackAuto());
        }
        int ic, ir;
        placeItems(c, &inner, &ic, &ir, &spanTracks, &innerRows);
        for (size_t k = 0; k < inner.len; ++k) {
            const GridItem* g = &inner.items[k];
            if (g->c0 >= span) continue; /* (implicit columns past the span: not the parent's) */
            GridItem v = *g;
            v.c0 += it->c0;
            v.c1 = it->c0 + cscdMini(g->c1, span);
            float extra = 0;
            if (g->c0 == 0) extra += c->margin[3] + c->border[3] + c->padding[3];
            if (g->c1 >= span) extra += c->margin[1] + c->border[1] + c->padding[1];
            if (extra > 0) v.extra = extra;
            cscdVecPush(&colItems, v);
        }
    }
    cscdVecFree(&inner);
    cscdVecFree(&spanTracks);
    cscdVecFree(&innerRows);
    sizeTracks(L, &ct, &colItems, true, cw, colGap, contentContribution, false, stretchCols);
    cscdVecFree(&colItems);
    /* auto-fit: collapse empty repeated tracks */
    if (s->gridColumns.autoFit) {
        bool* used = cscdCalloc((size_t)cols + 1, sizeof(bool));
        for (size_t i = 0; i < items.len; ++i)
            for (int k = items.items[i].c0; k < items.items[i].c1 && k < cols; ++k) used[k] = true;
        int a = s->gridColumns.autoRepeatAt;
        for (int k = a; k < cols && k < a + (int)(colTracks.len - s->gridColumns.tracks.len); ++k) if (!used[k]) ct.items[k].base = 0;
        cscdFree(used);
    }
    /* lay out items at their column widths to get heights */
    CscdFloatVec scratch = {0};
    for (size_t i = 0; i < items.len; ++i) {
        GridItem* it = &items.items[i];
        CscdLayoutBox* c = it->b;
        const CscdComputedStyle* cs = c->style;
        layResolveEdges(c, cw);
        if (subCols(c) || subRows(c)) {
            SubgridTracks* g = subgridTracks(L, c);
            if (subCols(c)) slice(&ct, it->c0, it->c1, c, false, &g->cols);
            else g->cols.len = 0;
            g->rows.len = 0; /* (first it sizes its rows itself: they tell the parent how tall its rows must be) */
            g->colGap = colGap;
            g->rowGap = rowGap;
        }
        float area = colGap * (it->c1 - it->c0 - 1);
        for (int k = it->c0; k < it->c1 && k < cols; ++k) area += ct.items[k].base;
        uint8_t js = justifySelfOf(c, s);
        BlockInput in = blockInput();
        in.positionedByCaller = true;
        in.cbW = area;
        in.cbH = -1;
        bool wd;
        laySpecifiedWidth(L, c, area, &wd);
        if (!wd && js == CSCD_AL_STRETCH && !cscdLenIsAuto(cs->margin[1]) && !cscdLenIsAuto(cs->margin[3]))
            in.forcedW = layClampWidth(L, c, area - c->margin[1] - c->margin[3], area);
        else if (!wd) { in.shrinkToFit = true; in.availW = area; }
        layBlock(L, c, &in);
    }
    bool stretchRows = s->alignContent == CSCD_AL_NORMAL || s->alignContent == CSCD_AL_STRETCH;
    /* the rows: a subgrid on rows contributes each of its rows to the parent's row it lies in, not its
     * whole height spread over them (cards whose parts line up across the row) */
    GridItemVec rowItems = {0};
    for (size_t i = 0; i < items.len; ++i) if (!subRows(items.items[i].b)) cscdVecPush(&rowItems, items.items[i]);
    sizeTracks(L, &rt, &rowItems, false, cbH, rowGap, heightContribution, false, stretchRows);
    cscdVecFree(&rowItems);
    for (size_t i = 0; i < items.len; ++i) {
        const GridItem* it = &items.items[i];
        if (!subRows(it->b)) continue;
        const CscdMapVal* ov = cscdU64MapGet(&L->subgridRowsOut, CSCD_PKEY(it->b));
        const CscdFloatVec* out = ov ? (const CscdFloatVec*)ov->p : NULL;
        const int span = it->r1 - it->r0;
        for (int k = 0; k < span && it->r0 + k < rows; ++k) {
            float need = out && k < (int)out->len ? out->items[k] : 0;
            if (k == 0) need += edgeStart(it->b, true);
            if (k == span - 1) need += edgeEnd(it->b, true);
            rt.items[it->r0 + k].base = cscdMaxf(rt.items[it->r0 + k].base, need);
        }
    }
    if (s->gridRows.subgrid && !(given && given->rows.len)) {
        bool inserted = false;
        CscdMapVal* v = cscdU64MapPut(&L->subgridRowsOut, CSCD_PKEY(b), &inserted);
        if (inserted) v->p = CSCD_NEW(CscdFloatVec);
        CscdFloatVec* out = (CscdFloatVec*)v->p;
        out->len = 0;
        for (size_t k = 0; k < rt.len; ++k) cscdVecPush(out, rt.items[k].base);
    }
    distribute(&ct, cw, colGap, s->justifyContent);
    float totalH = distribute(&rt, cbH, rowGap, s->alignContent);
    if (rows == 0) totalH = 0;
    bool baselineSet = false;
    for (size_t i = 0; i < items.len; ++i) {
        GridItem* it = &items.items[i];
        CscdLayoutBox* c = it->b;
        const CscdComputedStyle* cs = c->style;
        const Track* c0 = it->c0 < cols ? &ct.items[it->c0] : NULL;
        const Track* c1 = it->c1 - 1 < cols ? &ct.items[it->c1 - 1] : NULL;
        const Track* r0 = it->r0 < rows ? &rt.items[it->r0] : NULL;
        const Track* r1 = it->r1 - 1 < rows ? &rt.items[it->r1 - 1] : NULL;
        float ax = cx + (c0 ? c0->pos : 0);
        float aw = (c1 ? c1->pos + c1->base : 0) - (c0 ? c0->pos : 0);
        float ay = cy + (r0 ? r0->pos : 0);
        float ah = (r1 ? r1->pos + r1->base : 0) - (r0 ? r0->pos : 0);
        uint8_t as = cs->alignSelf;
        if (as == CSCD_AL_AUTO) as = s->alignItems;
        if (as == CSCD_AL_NORMAL) as = c->kind == CSCD_BOX_REPLACED ? CSCD_AL_START : CSCD_AL_STRETCH;
        uint8_t js = justifySelfOf(c, s);
        bool hd;
        laySpecifiedHeight(c, ah, &hd);
        bool wd;
        laySpecifiedWidth(L, c, aw, &wd);
        bool relayout = false;
        if (subRows(c)) { /* now with the parent's rows */
            slice(&rt, it->r0, it->r1, c, true, &subgridTracks(L, c)->rows);
            relayout = true;
        }
        BlockInput in = blockInput();
        in.positionedByCaller = true;
        in.cbW = aw;
        in.cbH = ah;
        in.forcedW = c->w;
        if (!wd && js == CSCD_AL_STRETCH && fabsf(aw - c->margin[1] - c->margin[3] - c->w) > 0.01f && !cscdLenIsAuto(cs->margin[1]) && !cscdLenIsAuto(cs->margin[3])) {
            in.forcedW = layClampWidth(L, c, aw - c->margin[1] - c->margin[3], aw);
            relayout = true;
        }
        if (!hd && as == CSCD_AL_STRETCH && !cscdLenIsAuto(cs->margin[0]) && !cscdLenIsAuto(cs->margin[2])) {
            in.forcedH = layClampHeight(c, ah - c->margin[0] - c->margin[2], ah);
            relayout = true;
        } else if (hd && ah >= 0) relayout = true; /* percentage heights against the grid area */
        if (relayout) layBlock(L, c, &in);
        float freeX = aw - c->w - c->margin[1] - c->margin[3];
        float freeY = ah - c->h - c->margin[0] - c->margin[2];
        float px = ax + c->margin[3], py = ay + c->margin[0];
        if (cscdLenIsAuto(cs->margin[3]) && cscdLenIsAuto(cs->margin[1])) px += cscdMaxf(0.f, freeX) / 2;
        else if (cscdLenIsAuto(cs->margin[3])) px += cscdMaxf(0.f, freeX);
        else if (js == CSCD_AL_END || js == CSCD_AL_FLEX_END || js == CSCD_AL_RIGHT || js == CSCD_AL_SELF_END) px += freeX;
        else if (js == CSCD_AL_CENTER) px += freeX / 2;
        if (cscdLenIsAuto(cs->margin[0]) && cscdLenIsAuto(cs->margin[2])) py += cscdMaxf(0.f, freeY) / 2;
        else if (cscdLenIsAuto(cs->margin[0])) py += cscdMaxf(0.f, freeY);
        else if (as == CSCD_AL_END || as == CSCD_AL_FLEX_END || as == CSCD_AL_SELF_END) py += freeY;
        else if (as == CSCD_AL_CENTER) py += freeY / 2;
        cscdBoxShift(c, px - c->x, py - c->y);
        layApplyRelative(c, aw, ah);
        if (!baselineSet && c->hasBaseline && it->r0 == 0) {
            b->baseline = c->baseline;
            b->lastBaseline = c->lastBaseline;
            b->hasBaseline = true;
            baselineSet = true;
        }
    }
    cscdVecFree(&scratch);
    cscdVecFree(&ct);
    cscdVecFree(&rt);
    cscdVecFree(&colTracks);
    cscdVecFree(&rowTracks);
    cscdVecFree(&items);
    return cscdMaxf(0.f, totalH);
}

/* ---- tables -------------------------------------------------------------------------------------- */

typedef struct TableCellRef {
    CscdLayoutBox* cell;
    int row, col, colspan, rowspan;
} TableCellRef;

typedef struct TableModel {
    CscdPtrVec rows;
    CscdPtrVec rowGroupOf;
    CSCD_VEC(TableCellRef) cells;
    CscdPtrVec captions;
    int cols;
} TableModel;

static int spanAttr(const CscdElement* e, const char* name) {
    char* c = cscdSvDupC(cscdElementAttrC(e, name));
    int v = c[0] ? atoi(c) : 1;
    cscdFree(c);
    return cscdMaxi(1, cscdMini(1000, v));
}

static void buildModel(CscdLayoutBox* table, TableModel* m) {
    memset(m, 0, sizeof *m);
    CscdPtrVec head = {0}, body = {0}, foot = {0};
    CscdPtrVec headG = {0}, bodyG = {0}, footG = {0};
    for (size_t i = 0; i < table->children.len; ++i) {
        CscdLayoutBox* c = cscdBoxChild(table, i);
        if (c->kind == CSCD_BOX_TABLE_ROW_GROUP) {
            uint8_t d = c->style->display;
            CscdPtrVec* rows = d == CSCD_DISPLAY_TABLE_HEADER_GROUP ? &head : d == CSCD_DISPLAY_TABLE_FOOTER_GROUP ? &foot : &body;
            CscdPtrVec* groups = d == CSCD_DISPLAY_TABLE_HEADER_GROUP ? &headG : d == CSCD_DISPLAY_TABLE_FOOTER_GROUP ? &footG : &bodyG;
            for (size_t k = 0; k < c->children.len; ++k) {
                CscdLayoutBox* r = cscdBoxChild(c, k);
                if (r->kind == CSCD_BOX_TABLE_ROW) { cscdVecPush(rows, r); cscdVecPush(groups, c); }
            }
        } else if (c->kind == CSCD_BOX_TABLE_ROW) {
            cscdVecPush(&body, c);
            cscdVecPush(&bodyG, NULL);
        } else if (c->style && c->style->display == CSCD_DISPLAY_TABLE_CAPTION) cscdVecPush(&m->captions, c);
    }
    cscdVecAppend(&m->rows, head.items, head.len); cscdVecAppend(&m->rowGroupOf, headG.items, headG.len);
    cscdVecAppend(&m->rows, body.items, body.len); cscdVecAppend(&m->rowGroupOf, bodyG.items, bodyG.len);
    cscdVecAppend(&m->rows, foot.items, foot.len); cscdVecAppend(&m->rowGroupOf, footG.items, footG.len);
    cscdVecFree(&head); cscdVecFree(&body); cscdVecFree(&foot);
    cscdVecFree(&headG); cscdVecFree(&bodyG); cscdVecFree(&footG);
    const int nr = (int)m->rows.len;
    CscdBytes* occ = cscdCalloc((size_t)nr + 1, sizeof(CscdBytes));
    for (int r = 0; r < nr; ++r) {
        int col = 0;
        CscdLayoutBox* rowBox = (CscdLayoutBox*)m->rows.items[r];
        for (size_t k = 0; k < rowBox->children.len; ++k) {
            CscdLayoutBox* c = cscdBoxChild(rowBox, k);
            if (c->kind != CSCD_BOX_TABLE_CELL) continue;
            while (col < (int)occ[r].len && occ[r].items[col]) ++col;
            int cs = 1, rs = 1;
            if (c->element && !c->anonymous) {
                cs = spanAttr(c->element, "colspan");
                rs = spanAttr(c->element, "rowspan");
            }
            rs = cscdMini(rs, nr - r);
            for (int rr = r; rr < r + rs; ++rr) {
                if ((int)occ[rr].len < col + cs) cscdVecResize(&occ[rr], (size_t)(col + cs));
                for (int cc = col; cc < col + cs; ++cc) occ[rr].items[cc] = 1;
            }
            TableCellRef ref = {c, r, col, cs, rs};
            cscdVecPush(&m->cells, ref);
            col += cs;
            m->cols = cscdMaxi(m->cols, col);
        }
    }
    for (int r = 0; r < nr; ++r) cscdVecFree(&occ[r]);
    cscdFree(occ);
}

static void freeModel(TableModel* m) {
    cscdVecFree(&m->rows);
    cscdVecFree(&m->rowGroupOf);
    cscdVecFree(&m->cells);
    cscdVecFree(&m->captions);
}

/* The percentage a single-column cell's width asks of the table (0: none; calc() mixes are not one). */
static float cellPercent(const CscdLayoutBox* cell) {
    const CscdLength l = cell->style->width;
    return l.kind == CSCD_LEN_FIXED && l.px == 0 && l.pct > 0 ? l.pct / 100.f : 0;
}

/* The columns' percentages (the largest a column's cells ask for), and their sum. Past 100% in all,
 * the columns from the left keep theirs and the later ones get what is left, as browsers do. */
static float columnPercents(const TableModel* m, float* pct) {
    for (size_t i = 0; i < m->cells.len; ++i) {
        const TableCellRef* c = &m->cells.items[i];
        if (c->colspan == 1) pct[c->col] = cscdMaxf(pct[c->col], cellPercent(c->cell));
    }
    float sum = 0;
    for (int k = 0; k < m->cols; ++k) {
        pct[k] = cscdMinf(pct[k], 1 - sum);
        sum += pct[k];
    }
    return sum;
}

float layIntrinsicTable(Layouter* L, CscdLayoutBox* b, bool min) {
    TableModel m;
    buildModel(b, &m);
    const CscdComputedStyle* s = b->style;
    float sx = s->borderCollapse == CSCD_COLLAPSE_COLLAPSE ? 0 : s->borderSpacingX;
    float* w = cscdCalloc((size_t)m.cols + 1, sizeof(float));
    for (size_t i = 0; i < m.cells.len; ++i) {
        const TableCellRef* c = &m.cells.items[i];
        float v = min ? layMinContent(L, c->cell) : layMaxContent(L, c->cell);
        if (c->colspan == 1) w[c->col] = cscdMaxf(w[c->col], v);
    }
    for (size_t i = 0; i < m.cells.len; ++i) {
        const TableCellRef* c = &m.cells.items[i];
        if (c->colspan == 1) continue;
        float v = min ? layMinContent(L, c->cell) : layMaxContent(L, c->cell);
        float cur = sx * (c->colspan - 1);
        for (int k = c->col; k < c->col + c->colspan; ++k) cur += w[k];
        if (v > cur) for (int k = c->col; k < c->col + c->colspan; ++k) w[k] += (v - cur) / c->colspan;
    }
    float total = sx * (m.cols + 1);
    for (int k = 0; k < m.cols; ++k) total += w[k];
    if (!min) {
        /* percentage columns: as wide a table as gives each its content at its share (a 50% column
         * of 100px content wants 200px), and the others theirs in what is left */
        float* pct = cscdCalloc((size_t)m.cols + 1, sizeof(float));
        const float pctSum = columnPercents(&m, pct), spacing = sx * (m.cols + 1);
        if (pctSum > 0) {
            float rest = 0;
            for (int k = 0; k < m.cols; ++k) {
                if (pct[k] > 0) total = cscdMaxf(total, w[k] / pct[k] + spacing);
                else rest += w[k];
            }
            if (pctSum < 1) total = cscdMaxf(total, rest / (1 - pctSum) + spacing);
        }
        cscdFree(pct);
    }
    for (size_t i = 0; i < m.captions.len; ++i) {
        CscdLayoutBox* cap = (CscdLayoutBox*)m.captions.items[i];
        total = cscdMaxf(total, min ? layOuterMinContent(L, cap) : layOuterMaxContent(L, cap));
    }
    const int cols = m.cols;
    cscdFree(w);
    freeModel(&m);
    return cols ? total : 0;
}

static float layCaptions(Layouter* L, const TableModel* m, bool bottom, float cx, float y, float cw) {
    for (size_t i = 0; i < m->captions.len; ++i) {
        CscdLayoutBox* cap = (CscdLayoutBox*)m->captions.items[i];
        if (cap->style->captionBottom != bottom) continue;
        BlockInput in = blockInput();
        in.x = cx;
        in.y = y;
        in.cbW = cw;
        layBlock(L, cap, &in);
        y = cap->y + cap->h + cap->margin[2];
    }
    return y;
}

float layTable(Layouter* L, CscdLayoutBox* b, float cbH) {
    (void)cbH;
    TableModel m;
    buildModel(b, &m);
    const CscdComputedStyle* s = b->style;
    bool collapse = s->borderCollapse == CSCD_COLLAPSE_COLLAPSE;
    float sx = collapse ? 0 : s->borderSpacingX, sy = collapse ? 0 : s->borderSpacingY;
    if (collapse) {
        /* collapsed borders: neighbouring cells share their border by overlapping it */
        float bw = 0;
        for (size_t i = 0; i < m.cells.len; ++i) {
            const CscdComputedStyle* cs = m.cells.items[i].cell->style;
            bw = cscdMaxf(bw, cscdMaxf(cs->border[1].width, cs->border[2].width));
        }
        sx = sy = -bw;
    }
    float cx = cscdBoxContentX(b), cy = cscdBoxContentY(b), cw = cscdBoxContentW(b);
    /* captions on top (caption-side: bottom ones after the rows) */
    float y = layCaptions(L, &m, false, cx, cy, cw);
    if (!m.cols) {
        freeModel(&m);
        return y - cy;
    }
    const int ncols = m.cols;
    float* mn = cscdCalloc((size_t)ncols, sizeof(float));
    float* mx = cscdCalloc((size_t)ncols, sizeof(float));
    float* fixedW = cscdCalloc((size_t)ncols, sizeof(float));
    float* w = cscdCalloc((size_t)ncols, sizeof(float));
    float* colX = cscdCalloc((size_t)ncols, sizeof(float));
    for (int k = 0; k < ncols; ++k) fixedW[k] = -1;
    for (size_t i = 0; i < m.cells.len; ++i) {
        const TableCellRef* c = &m.cells.items[i];
        layResolveEdges(c->cell, cw);
        float a = layMinContent(L, c->cell), z = layMaxContent(L, c->cell);
        if (c->colspan == 1) {
            mn[c->col] = cscdMaxf(mn[c->col], a);
            mx[c->col] = cscdMaxf(mx[c->col], z);
            const CscdLength cl = c->cell->style->width;
            if (cscdLenIsFixed(cl) && !cscdLenIsAuto(cl)) fixedW[c->col] = cscdMaxf(fixedW[c->col], z);
        }
    }
    for (size_t i = 0; i < m.cells.len; ++i) {
        const TableCellRef* c = &m.cells.items[i];
        if (c->colspan == 1) continue;
        float a = layMinContent(L, c->cell), z = layMaxContent(L, c->cell);
        float curMin = sx * (c->colspan - 1), curMax = curMin;
        for (int k = c->col; k < c->col + c->colspan; ++k) { curMin += mn[k]; curMax += mx[k]; }
        for (int k = c->col; k < c->col + c->colspan; ++k) {
            if (a > curMin) mn[k] += (a - curMin) / c->colspan;
            if (z > curMax) mx[k] += (z - curMax) / c->colspan;
        }
    }
    for (int k = 0; k < ncols; ++k) mx[k] = cscdMaxf(mx[k], mn[k]);
    float lead = collapse ? 0 : sx, leadY = collapse ? 0 : sy;
    float avail = cw - 2 * lead - sx * (ncols - 1);
    float sumMin = 0, sumMax = 0;
    for (int k = 0; k < ncols; ++k) { sumMin += mn[k]; sumMax += mx[k]; }
    if (sumMax <= avail) {
        float extra = avail - sumMax;
        bool autoWidth = cscdLenIsAuto(s->width);
        for (int k = 0; k < ncols; ++k) w[k] = mx[k];
        if (!autoWidth || b->parent == NULL || extra > 0) {
            /* distribute among non-fixed columns proportionally to their max widths */
            float base = 0;
            int nonFixed = 0;
            for (int k = 0; k < ncols; ++k) if (fixedW[k] < 0) { base += mx[k]; ++nonFixed; }
            if (extra > 0) {
                for (int k = 0; k < ncols; ++k) {
                    if (nonFixed == 0) w[k] += extra / ncols;
                    else if (fixedW[k] < 0) w[k] += base > 0 ? extra * mx[k] / base : extra / nonFixed;
                }
            }
        }
    } else if (sumMin >= avail) {
        for (int k = 0; k < ncols; ++k) w[k] = mn[k];
    } else {
        float range = sumMax - sumMin;
        float f = range > 0 ? (avail - sumMin) / range : 0;
        for (int k = 0; k < ncols; ++k) w[k] = mn[k] + (mx[k] - mn[k]) * f;
    }
    float* pct = cscdCalloc((size_t)ncols + 1, sizeof(float));
    const float pctSum = columnPercents(&m, pct);
    if (pctSum > 0 && avail > 0 && !(s->tableFixed && !cscdLenIsAuto(s->width))) {
        /* percentage columns take their share of the width (at least their content's minimum); the
         * other columns share what is left as above */
        float used = 0, oMin = 0, oMax = 0;
        int others = 0;
        for (int k = 0; k < ncols; ++k) {
            if (pct[k] > 0) { w[k] = cscdMaxf(mn[k], pct[k] * avail); used += w[k]; }
            else { oMin += mn[k]; oMax += mx[k]; ++others; }
        }
        const float rest = avail - used;
        if (!others) {
            /* all of them percentages that leave room: they grow in proportion */
            if (rest > 0 && used > 0) for (int k = 0; k < ncols; ++k) w[k] *= avail / used;
        } else if (oMax <= rest) {
            float base = 0;
            int nonFixed = 0;
            for (int k = 0; k < ncols; ++k) if (pct[k] <= 0 && fixedW[k] < 0) { base += mx[k]; ++nonFixed; }
            for (int k = 0; k < ncols; ++k) {
                if (pct[k] > 0) continue;
                w[k] = mx[k];
                if (nonFixed == 0) w[k] += (rest - oMax) / others;
                else if (fixedW[k] < 0) w[k] += base > 0 ? (rest - oMax) * mx[k] / base : (rest - oMax) / nonFixed;
            }
        } else if (oMin >= rest) {
            for (int k = 0; k < ncols; ++k) if (pct[k] <= 0) w[k] = mn[k];
            /* not enough room for the others' minimum: the percentage columns give way, down to theirs */
            const float over = used + oMin - avail;
            float give = 0;
            for (int k = 0; k < ncols; ++k) if (pct[k] > 0) give += w[k] - mn[k];
            if (over > 0 && give > 0) {
                const float f = cscdMinf(1.f, over / give);
                for (int k = 0; k < ncols; ++k) if (pct[k] > 0) w[k] -= (w[k] - mn[k]) * f;
            }
        } else {
            const float f = (rest - oMin) / (oMax - oMin);
            for (int k = 0; k < ncols; ++k) if (pct[k] <= 0) w[k] = mn[k] + (mx[k] - mn[k]) * f;
        }
    }
    cscdFree(pct);
    if (s->tableFixed && !cscdLenIsAuto(s->width)) {
        /* table-layout: fixed: the widths the first row's cells ask for; the rest share what is left */
        float* fw = cscdCalloc((size_t)ncols, sizeof(float));
        for (int k = 0; k < ncols; ++k) fw[k] = -1;
        for (size_t i = 0; i < m.cells.len; ++i) {
            const TableCellRef* c = &m.cells.items[i];
            if (c->row != 0) continue;
            const CscdLength cl = c->cell->style->width;
            if (cscdLenIsAuto(cl)) continue;
            float v = cscdLenResolve(cl, avail, 0);
            if (c->cell->style->boxSizing == CSCD_BOX_CONTENT) v += c->cell->padding[1] + c->cell->padding[3] + c->cell->border[1] + c->cell->border[3];
            for (int k = c->col; k < c->col + c->colspan && k < ncols; ++k) fw[k] = v / c->colspan;
        }
        float used = 0;
        int open = 0;
        for (int k = 0; k < ncols; ++k) { if (fw[k] >= 0) used += fw[k]; else ++open; }
        for (int k = 0; k < ncols; ++k) w[k] = fw[k] >= 0 ? fw[k] : (open ? cscdMaxf(0.f, avail - used) / open : 0);
        if (!open && used < avail && used > 0) for (int k = 0; k < ncols; ++k) w[k] *= avail / used;
        cscdFree(fw);
    }
    float px = cx + lead;
    for (int k = 0; k < ncols; ++k) { colX[k] = px; px += w[k] + sx; }
    if (collapse) px += lead - sx;
    /* rows */
    const int nr = (int)m.rows.len;
    float* rowH = cscdCalloc((size_t)nr + 1, sizeof(float));
    float* rowY = cscdCalloc((size_t)nr + 1, sizeof(float));
    for (int r = 0; r < nr; ++r) {
        bool hd;
        CscdLayoutBox* rb = (CscdLayoutBox*)m.rows.items[r];
        layResolveEdges(rb, cw);
        float h = laySpecifiedHeight(rb, -1, &hd);
        if (hd) rowH[r] = h;
    }
    for (size_t i = 0; i < m.cells.len; ++i) {
        const TableCellRef* c = &m.cells.items[i];
        float cwid = sx * (c->colspan - 1);
        for (int k = c->col; k < c->col + c->colspan; ++k) cwid += w[k];
        BlockInput in = blockInput();
        in.positionedByCaller = true;
        in.cbW = cwid;
        in.forcedW = cwid;
        layBlock(L, c->cell, &in);
        if (c->rowspan == 1) rowH[c->row] = cscdMaxf(rowH[c->row], c->cell->h);
    }
    for (size_t i = 0; i < m.cells.len; ++i) {
        const TableCellRef* c = &m.cells.items[i];
        if (c->rowspan == 1) continue;
        float cur = sy * (c->rowspan - 1);
        for (int r = c->row; r < c->row + c->rowspan; ++r) cur += rowH[r];
        if (c->cell->h > cur) for (int r = c->row; r < c->row + c->rowspan; ++r) rowH[r] += (c->cell->h - cur) / c->rowspan;
    }
    y += leadY;
    for (int r = 0; r < nr; ++r) { rowY[r] = y; y += rowH[r] + sy; }
    if (collapse && nr) y += leadY - sy;
    /* final cell geometry with vertical alignment */
    for (size_t i = 0; i < m.cells.len; ++i) {
        const TableCellRef* c = &m.cells.items[i];
        CscdLayoutBox* cell = c->cell;
        float cwid = sx * (c->colspan - 1);
        for (int k = c->col; k < c->col + c->colspan; ++k) cwid += w[k];
        float ch = sy * (c->rowspan - 1);
        for (int r = c->row; r < c->row + c->rowspan; ++r) ch += rowH[r];
        float contentH = cscdBoxContentH(cell);
        BlockInput in = blockInput();
        in.positionedByCaller = true;
        in.cbW = cwid;
        in.forcedW = cwid;
        in.forcedH = ch;
        layBlock(L, cell, &in);
        float innerH = cscdBoxContentH(cell);
        uint8_t va = cell->style->verticalAlign;
        float d = 0;
        if (va == CSCD_VA_MIDDLE) d = (innerH - contentH) / 2;
        else if (va == CSCD_VA_BOTTOM) d = innerH - contentH;
        if (d > 0.01f) {
            for (size_t k = 0; k < cell->children.len; ++k) cscdBoxShift(cscdBoxChild(cell, k), 0, d);
            cell->memoPass = 0; /* (no longer as it laid itself out) */
            cell->baseline += d;
            cell->lastBaseline += d;
            for (size_t k = 0; k < cell->lines.len; ++k) { cell->lines.items[k].top += d; cell->lines.items[k].baseline += d; }
        }
        cscdBoxShift(cell, colX[c->col] - cell->x, rowY[c->row] - cell->y);
    }
    /* rows and groups cover their cells (for backgrounds and hit testing) */
    float tableInnerW = px - cx;
    for (int r = 0; r < nr; ++r) {
        CscdLayoutBox* row = (CscdLayoutBox*)m.rows.items[r];
        row->x = cx + lead; row->y = rowY[r]; row->w = cscdMaxf(0.f, tableInnerW - 2 * lead); row->h = rowH[r];
        row->overflow = cscdBoxBorderBox(row);
    }
    for (size_t i = 0; i < b->children.len; ++i) {
        CscdLayoutBox* g = cscdBoxChild(b, i);
        if (g->kind != CSCD_BOX_TABLE_ROW_GROUP) continue;
        float top = 1e30f, bot = -1e30f;
        for (size_t k = 0; k < g->children.len; ++k) {
            CscdLayoutBox* r = cscdBoxChild(g, k);
            if (r->kind == CSCD_BOX_TABLE_ROW) { top = cscdMinf(top, r->y); bot = cscdMaxf(bot, r->y + r->h); }
        }
        if (top > bot) continue;
        g->x = cx + lead; g->y = top; g->w = cscdMaxf(0.f, tableInnerW - 2 * lead); g->h = bot - top;
        g->overflow = cscdBoxBorderBox(g);
    }
    if (m.cells.len) {
        CscdLayoutBox* first = m.cells.items[0].cell;
        if (first->hasBaseline) { b->baseline = first->baseline; b->lastBaseline = first->lastBaseline; b->hasBaseline = true; }
    }
    /* the table grows to fit its columns */
    if (px - cx > cw + 0.01f) b->w += (px - cx) - cw;
    y = layCaptions(L, &m, true, cx, y, cw);
    cscdFree(mn); cscdFree(mx); cscdFree(fixedW); cscdFree(w); cscdFree(colX); cscdFree(rowH); cscdFree(rowY);
    freeModel(&m);
    return y - cy;
}
