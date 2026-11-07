/* Computed styles as values: the initial style, copies, comparisons, and the parts they hold
 * (background layers, transforms, grid tracks, custom properties). */
#include "cascade/style.h"
#include "core/thread.h"

/* ---- custom properties ------------------------------------------------------------------------- */

static void varsDtor(void* p) {
    CscdVars* v = (CscdVars*)p;
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&v->map, s));) {
        cscdStrFree((CscdStr*)s->val.p);
        cscdFree(s->val.p);
    }
    cscdStrMapFree(&v->map);
}

CscdVars* cscdVarsNew(void) { return (CscdVars*)cscdRcNew(sizeof(CscdVars), varsDtor); }

CscdVars* cscdVarsClone(const CscdVars* v) {
    CscdVars* c = cscdVarsNew();
    if (v)
        for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&v->map, s));) cscdVarsSet(c, cscdStrMapKey(s), cscdStrSv((const CscdStr*)s->val.p));
    return c;
}

const CscdStr* cscdVarsGet(const CscdVars* v, CscdSv name) {
    if (!v) return NULL;
    CscdMapVal* m = cscdStrMapGet(&v->map, name);
    return m ? (const CscdStr*)m->p : NULL;
}

void cscdVarsSet(CscdVars* v, CscdSv name, CscdSv value) {
    bool inserted = false;
    CscdMapVal* m = cscdStrMapPut(&v->map, name, &inserted);
    if (inserted) {
        m->p = CSCD_NEW(CscdStr);
        *(CscdStr*)m->p = cscdStrDup(value);
    } else cscdStrSet((CscdStr*)m->p, value);
}

void cscdVarsRemove(CscdVars* v, CscdSv name) {
    CscdMapVal old;
    if (cscdStrMapRemove(&v->map, name, &old)) {
        cscdStrFree((CscdStr*)old.p);
        cscdFree(old.p);
    }
}

/* ---- background layers -------------------------------------------------------------------------- */

CscdBackgroundLayer cscdBackgroundLayer(void) {
    CscdBackgroundLayer l;
    memset(&l, 0, sizeof l);
    l.kind = CSCD_LAYER_NONE;
    l.angle = 180;
    l.radiusX = l.radiusY = cscdLenFixed(0);
    l.centerX = l.centerY = cscdLenPercent(50);
    l.sizeX = l.sizeY = cscdLenAuto();
    l.posX = l.posY = cscdLenPercent(0);
    l.repeatX = l.repeatY = true;
    l.clip = CSCD_BG_BORDER_BOX;
    l.origin = CSCD_BG_PADDING_BOX;
    return l;
}

void cscdLayerFini(CscdBackgroundLayer* l) {
    cscdRelease(l->image);
    l->image = NULL;
    cscdStrFree(&l->url);
    cscdVecFree(&l->stops);
}

CscdBackgroundLayer cscdLayerCopy(const CscdBackgroundLayer* l) {
    CscdBackgroundLayer c = *l;
    cscdRetain(c.image);
    c.url = cscdStrCopy(&l->url);
    memset(&c.stops, 0, sizeof c.stops);
    cscdVecAppend(&c.stops, l->stops.items, l->stops.len);
    return c;
}

void cscdLayerVecFree(CscdLayerVec* v) {
    for (size_t i = 0; i < v->len; ++i) cscdLayerFini(&v->items[i]);
    cscdVecFree(v);
}

void cscdLayerVecCopy(CscdLayerVec* dst, const CscdLayerVec* src) {
    if (dst == src) return;
    for (size_t i = 0; i < dst->len; ++i) cscdLayerFini(&dst->items[i]);
    dst->len = 0;
    for (size_t i = 0; i < src->len; ++i) cscdVecPush(dst, cscdLayerCopy(&src->items[i]));
}

/* ---- transforms ---------------------------------------------------------------------------------- */

CscdTransformOp cscdTransformOp(CscdTransformKind kind) {
    CscdTransformOp t;
    memset(&t, 0, sizeof t);
    t.kind = (uint8_t)kind;
    t.x = t.y = cscdLenFixed(0);
    t.m = cscdMatIdentity();
    t.z3 = 1;
    for (int i = 0; i < 16; ++i) t.m4[i] = (i % 5 == 0) ? 1.f : 0.f;
    return t;
}

bool cscdTransformOpEq(const CscdTransformOp* a, const CscdTransformOp* o) {
    if (a->kind != o->kind || !cscdLenEq(a->x, o->x) || !cscdLenEq(a->y, o->y) || a->a != o->a || a->b != o->b || a->x3 != o->x3 ||
        a->y3 != o->y3 || a->z3 != o->z3 || a->z != o->z)
        return false;
    if (a->m.a != o->m.a || a->m.b != o->m.b || a->m.c != o->m.c || a->m.d != o->m.d || a->m.e != o->m.e || a->m.f != o->m.f) return false;
    for (int i = 0; i < 16; ++i) if (a->m4[i] != o->m4[i]) return false;
    return true;
}

/* ---- grid tracks --------------------------------------------------------------------------------- */

bool cscdTrackEq(const CscdTrackSize* a, const CscdTrackSize* b) {
    return a->kind == b->kind && cscdLenEq(a->len, b->len) && a->fr == b->fr && a->min == b->min && a->max == b->max;
}

bool cscdTrackVecEq(const CscdTrackVec* a, const CscdTrackVec* b) {
    if (a->len != b->len) return false;
    for (size_t i = 0; i < a->len; ++i) if (!cscdTrackEq(&a->items[i], &b->items[i])) return false;
    return true;
}

static CscdMutex trackMutex = CSCD_MUTEX_INIT;
static CscdStrMap trackInterned;

const CscdTrackSize* cscdTrackIntern(const CscdTrackSize* t) {
    /* the key: the fields' bytes (its parts are kept already, so their addresses say what they are) */
    CscdStr key = {0};
#define ADD(f) cscdStrAppend(&key, cscdSvMake((const char*)&(f), sizeof(f)))
    ADD(t->kind); ADD(t->len.kind); ADD(t->len.px); ADD(t->len.pct); ADD(t->len.expr); ADD(t->fr); ADD(t->min); ADD(t->max);
#undef ADD
    cscdMutexLock(&trackMutex);
    bool inserted = false;
    CscdMapVal* v = cscdStrMapPut(&trackInterned, cscdStrSv(&key), &inserted);
    cscdStrFree(&key);
    if (inserted) {
        CscdTrackSize* k = CSCD_NEW(CscdTrackSize);
        *k = *t;
        v->p = k;
    }
    const CscdTrackSize* r = (const CscdTrackSize*)v->p;
    cscdMutexUnlock(&trackMutex);
    return r;
}

void cscdTrackListInit(CscdTrackList* t) {
    memset(t, 0, sizeof *t);
    t->autoRepeatAt = -1;
    t->none = true;
}

static void lineNamesFree(CscdLineNames* n) {
    for (size_t i = 0; i < n->len; ++i) cscdStrVecFree(&n->items[i]);
    cscdVecFree(n);
}

void cscdTrackListFini(CscdTrackList* t) {
    cscdVecFree(&t->tracks);
    lineNamesFree(&t->lineNames);
    cscdVecFree(&t->autoTracks);
}

void cscdTrackListCopy(CscdTrackList* dst, const CscdTrackList* src) {
    if (dst == src) return;
    cscdTrackListFini(dst);
    *dst = *src;
    memset(&dst->tracks, 0, sizeof dst->tracks);
    memset(&dst->lineNames, 0, sizeof dst->lineNames);
    memset(&dst->autoTracks, 0, sizeof dst->autoTracks);
    cscdVecAppend(&dst->tracks, src->tracks.items, src->tracks.len);
    cscdVecAppend(&dst->autoTracks, src->autoTracks.items, src->autoTracks.len);
    for (size_t i = 0; i < src->lineNames.len; ++i) {
        CscdStrVec* n = cscdVecPushZero(&dst->lineNames);
        cscdStrVecCopy(n, &src->lineNames.items[i]);
    }
}

bool cscdTrackListEq(const CscdTrackList* a, const CscdTrackList* b) {
    if (!cscdTrackVecEq(&a->tracks, &b->tracks) || a->autoRepeatAt != b->autoRepeatAt || a->autoFit != b->autoFit ||
        !cscdTrackVecEq(&a->autoTracks, &b->autoTracks) || a->none != b->none || a->subgrid != b->subgrid || a->lineNames.len != b->lineNames.len)
        return false;
    for (size_t i = 0; i < a->lineNames.len; ++i)
        if (!cscdStrVecEq(&a->lineNames.items[i], &b->lineNames.items[i])) return false;
    return true;
}

bool cscdGridLineEq(const CscdGridLine* a, const CscdGridLine* b) { return a->line == b->line && a->span == b->span && cscdStrEq(&a->name, &b->name); }

bool cscdFontDescEq(const CscdFontDesc* a, const CscdFontDesc* b) {
    return cscdStrVecEq(&a->families, &b->families) && a->size == b->size && a->weight == b->weight && a->italic == b->italic && a->smallCaps == b->smallCaps &&
           a->tabularNums == b->tabularNums && a->noLigatures == b->noLigatures && a->caps == b->caps && a->numeric == b->numeric &&
           a->ligatures == b->ligatures && a->position == b->position && a->eastAsian == b->eastAsian && a->kerning == b->kerning &&
           a->noOpticalSizing == b->noOpticalSizing && a->stretch == b->stretch && a->noSynthesis == b->noSynthesis && a->featureSettings == b->featureSettings &&
           a->variationSettings == b->variationSettings;
}

void cscdFontDescInherit(CscdFontDesc* d, const CscdFontDesc* s) {
    d->size = s->size; d->weight = s->weight; d->italic = s->italic; d->smallCaps = s->smallCaps;
    d->tabularNums = s->tabularNums; d->noLigatures = s->noLigatures;
    d->caps = s->caps; d->numeric = s->numeric; d->ligatures = s->ligatures; d->position = s->position; d->eastAsian = s->eastAsian;
    d->kerning = s->kerning; d->noOpticalSizing = s->noOpticalSizing; d->stretch = s->stretch; d->noSynthesis = s->noSynthesis;
    d->featureSettings = s->featureSettings; d->variationSettings = s->variationSettings;
}

/* ---- computed styles ----------------------------------------------------------------------------- */

static void counterVecFree(CscdCounterVec* v) {
    for (size_t i = 0; i < v->len; ++i) cscdStrFree(&v->items[i].name);
    cscdVecFree(v);
}

static void counterVecCopy(CscdCounterVec* dst, const CscdCounterVec* src) {
    for (size_t i = 0; i < src->len; ++i) {
        CscdCounter c = {cscdStrCopy(&src->items[i].name), src->items[i].value};
        cscdVecPush(dst, c);
    }
}

static void transitionsFree(CscdTransitionVec* v) {
    for (size_t i = 0; i < v->len; ++i) cscdStrFree(&v->items[i].property);
    cscdVecFree(v);
}

static void animationsFree(CscdAnimationVec* v) {
    for (size_t i = 0; i < v->len; ++i) cscdStrFree(&v->items[i].name);
    cscdVecFree(v);
}

/* Frees what s holds (not s). */
static void styleFini(CscdComputedStyle* s) {
    cscdLayerVecFree(&s->backgrounds);
    cscdVecFree(&s->boxShadow);
    cscdVecFree(&s->textShadow);
    cscdVecFree(&s->transform);
    cscdLayerVecFree(&s->masks);
    cscdStrFree(&s->clipPath);
    cscdStrFree(&s->shapeOutside);
    cscdStrVecFree(&s->font.families);
    cscdStrFree(&s->listImage);
    cscdTrackListFini(&s->gridColumns);
    cscdTrackListFini(&s->gridRows);
    cscdVecFree(&s->gridAutoColumns);
    cscdVecFree(&s->gridAutoRows);
    cscdStrFree(&s->gridColumnStart.name);
    cscdStrFree(&s->gridColumnEnd.name);
    cscdStrFree(&s->gridRowStart.name);
    cscdStrFree(&s->gridRowEnd.name);
    cscdStrVecFree(&s->gridAreas);
    cscdStrFree(&s->gridArea);
    cscdStrVecFree(&s->containerNames);
    counterVecFree(&s->counterReset);
    counterVecFree(&s->counterSet);
    counterVecFree(&s->counterIncrement);
    cscdStrFree(&s->content);
    cscdStrFree(&s->contentImage);
    transitionsFree(&s->transitions);
    animationsFree(&s->animations);
    cscdRelease(s->vars);
    s->vars = NULL;
}

static void styleDtor(void* p) { styleFini((CscdComputedStyle*)p); }

static void initValues(CscdComputedStyle* s) {
    memset(s, 0, sizeof *s);
    s->display = s->originalDisplay = CSCD_DISPLAY_INLINE;
    s->width = s->height = cscdLenAuto();
    s->minWidth = s->minHeight = cscdLenAuto();
    s->maxWidth = s->maxHeight = cscdLenNone();
    s->top = s->right = s->bottom = s->left = cscdLenAuto();
    for (int i = 0; i < 4; ++i) {
        s->margin[i] = s->padding[i] = cscdLenFixed(0);
        s->radius[i].x = s->radius[i].y = cscdLenFixed(0);
    }
    s->zAuto = true;
    s->opacity = 1;
    s->color = cscdRgb(0, 0, 0);
    s->originX = s->originY = cscdLenPercent(50);
    s->perspectiveX = s->perspectiveY = cscdLenPercent(50);
    s->accentColor = cscdRgb(0, 117, 255);
    s->caretAuto = true;
    s->brightness = 1;
    s->saturate = 1;
    s->backdrop.brightness = 1;
    s->backdrop.saturate = 1;
    s->objectPosX = s->objectPosY = cscdLenPercent(50);
    s->fill = cscdRgb(0, 0, 0);
    s->strokeNone = true;
    s->strokeWidth = 1;
    cscdVecPush(&s->font.families, cscdStrDupC("serif"));
    s->font.size = 16;
    s->font.weight = 400;
    s->lineHeight = -1;
    s->textIndent = cscdLenFixed(0);
    s->verticalAlignLength = cscdLenFixed(0);
    s->decorationCurrent = true;
    s->tabSize = 8;
    s->listStyle = CSCD_LIST_DISC;
    s->alignSelf = CSCD_AL_AUTO;
    s->justifySelf = CSCD_AL_AUTO;
    s->justifyContent = s->alignItems = s->alignContent = s->justifyItems = CSCD_AL_NORMAL;
    s->flexShrink = 1;
    s->flexBasis = cscdLenAuto();
    s->rowGap = s->columnGap = cscdLenNone();
    cscdTrackListInit(&s->gridColumns);
    cscdTrackListInit(&s->gridRows);
}

CscdComputedStyle* cscdStyleNew(void) {
    CscdComputedStyle* s = (CscdComputedStyle*)cscdRcNew(sizeof(CscdComputedStyle), styleDtor);
    initValues(s);
    return s;
}

static void copyStrVec(CscdStrVec* dst, const CscdStrVec* src) {
    memset(dst, 0, sizeof *dst);
    cscdStrVecCopy(dst, src);
}

/* dst gets the values of src; dst holds nothing (its heap parts are overwritten). */
static void copyInto(CscdComputedStyle* dst, const CscdComputedStyle* src) {
    memcpy(dst, src, sizeof *dst);
    memset(&dst->backgrounds, 0, sizeof dst->backgrounds);
    cscdLayerVecCopy(&dst->backgrounds, &src->backgrounds);
    memset(&dst->boxShadow, 0, sizeof dst->boxShadow);
    cscdVecAppend(&dst->boxShadow, src->boxShadow.items, src->boxShadow.len);
    memset(&dst->textShadow, 0, sizeof dst->textShadow);
    cscdVecAppend(&dst->textShadow, src->textShadow.items, src->textShadow.len);
    memset(&dst->transform, 0, sizeof dst->transform);
    cscdVecAppend(&dst->transform, src->transform.items, src->transform.len);
    memset(&dst->masks, 0, sizeof dst->masks);
    cscdLayerVecCopy(&dst->masks, &src->masks);
    dst->clipPath = cscdStrCopy(&src->clipPath);
    dst->shapeOutside = cscdStrCopy(&src->shapeOutside);
    copyStrVec(&dst->font.families, &src->font.families);
    dst->listImage = cscdStrCopy(&src->listImage);
    cscdTrackListInit(&dst->gridColumns);
    cscdTrackListCopy(&dst->gridColumns, &src->gridColumns);
    cscdTrackListInit(&dst->gridRows);
    cscdTrackListCopy(&dst->gridRows, &src->gridRows);
    memset(&dst->gridAutoColumns, 0, sizeof dst->gridAutoColumns);
    cscdVecAppend(&dst->gridAutoColumns, src->gridAutoColumns.items, src->gridAutoColumns.len);
    memset(&dst->gridAutoRows, 0, sizeof dst->gridAutoRows);
    cscdVecAppend(&dst->gridAutoRows, src->gridAutoRows.items, src->gridAutoRows.len);
    dst->gridColumnStart.name = cscdStrCopy(&src->gridColumnStart.name);
    dst->gridColumnEnd.name = cscdStrCopy(&src->gridColumnEnd.name);
    dst->gridRowStart.name = cscdStrCopy(&src->gridRowStart.name);
    dst->gridRowEnd.name = cscdStrCopy(&src->gridRowEnd.name);
    copyStrVec(&dst->gridAreas, &src->gridAreas);
    dst->gridArea = cscdStrCopy(&src->gridArea);
    copyStrVec(&dst->containerNames, &src->containerNames);
    memset(&dst->counterReset, 0, sizeof dst->counterReset);
    counterVecCopy(&dst->counterReset, &src->counterReset);
    memset(&dst->counterSet, 0, sizeof dst->counterSet);
    counterVecCopy(&dst->counterSet, &src->counterSet);
    memset(&dst->counterIncrement, 0, sizeof dst->counterIncrement);
    counterVecCopy(&dst->counterIncrement, &src->counterIncrement);
    dst->content = cscdStrCopy(&src->content);
    dst->contentImage = cscdStrCopy(&src->contentImage);
    memset(&dst->transitions, 0, sizeof dst->transitions);
    for (size_t i = 0; i < src->transitions.len; ++i) {
        CscdTransition t = src->transitions.items[i];
        t.property = cscdStrCopy(&src->transitions.items[i].property);
        cscdVecPush(&dst->transitions, t);
    }
    memset(&dst->animations, 0, sizeof dst->animations);
    for (size_t i = 0; i < src->animations.len; ++i) {
        CscdAnimation a = src->animations.items[i];
        a.name = cscdStrCopy(&src->animations.items[i].name);
        cscdVecPush(&dst->animations, a);
    }
    dst->vars = (CscdVars*)cscdRetain(src->vars);
}

CscdComputedStyle* cscdStyleClone(const CscdComputedStyle* src) {
    CscdComputedStyle* s = (CscdComputedStyle*)cscdRcNew(sizeof(CscdComputedStyle), styleDtor);
    copyInto(s, src);
    return s;
}

void cscdStyleAssign(CscdComputedStyle* dst, const CscdComputedStyle* src) {
    if (dst == src) return;
    styleFini(dst);
    copyInto(dst, src);
}

static CscdComputedStyle* initialStyle;
static CscdOnce initialOnce = CSCD_ONCE_INIT;
static void makeInitial(void) { initialStyle = cscdStyleNew(); }

const CscdComputedStyle* cscdInitialStyle(void) {
    cscdCallOnce(&initialOnce, makeInitial);
    return initialStyle;
}
