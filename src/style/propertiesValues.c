/* The structured values the longhands take: timing functions, shadows, grid tracks, images and
 * gradients, and background (or mask) layers. */
#include "style/styleInternal.h"

void cscdApplyStateInit(CscdApplyState* st, CscdComputedStyle* s, const CscdComputedStyle* parent, const CscdStyleEnvironment* env, CscdSv baseDir) {
    memset(st, 0, sizeof *st);
    st->s = s;
    st->parent = parent;
    st->env = env;
    st->baseDir = baseDir;
    st->ctx = cscdValueCtx();
}

void cscdApplyStateFini(CscdApplyState* st) {
    cscdStrFree(&st->bgImage); cscdStrFree(&st->bgPosition); cscdStrFree(&st->bgSize); cscdStrFree(&st->bgRepeat);
    cscdStrFree(&st->bgClip); cscdStrFree(&st->bgOrigin);
    cscdStrFree(&st->maskImage); cscdStrFree(&st->maskPosition); cscdStrFree(&st->maskSize); cscdStrFree(&st->maskRepeat);
}

bool cscdParseTiming(const CscdCssValue* x, float* x1, float* y1, float* x2, float* y2, int* steps) {
    typedef struct P { const char* n; float a, b, c, d; } P;
    static const P presets[] = {{"ease", .25f, .1f, .25f, 1}, {"linear", 0, 0, 1, 1}, {"ease-in", .42f, 0, 1, 1},
                                {"ease-out", 0, 0, .58f, 1}, {"ease-in-out", .42f, 0, .58f, 1}};
    for (size_t i = 0; i < CSCD_COUNT(presets); ++i)
        if (cscdCssIsIdent(x, presets[i].n)) {
            *x1 = presets[i].a; *y1 = presets[i].b; *x2 = presets[i].c; *y2 = presets[i].d; *steps = 0;
            return true;
        }
    if (cscdCssIsIdent(x, "step-start")) { *steps = -1; return true; }
    if (cscdCssIsIdent(x, "step-end")) { *steps = 1; return true; }
    if (cscdCssIsFunction(x, "cubic-bezier")) {
        float n[4];
        size_t k = 0;
        for (size_t i = 0; i < x->args.len; ++i)
            if (x->args.items[i].tok.type == CSCD_TOK_NUMBER) {
                if (k < 4) n[k] = (float)x->args.items[i].tok.num;
                ++k;
            }
        if (k != 4) return false;
        *x1 = n[0]; *y1 = n[1]; *x2 = n[2]; *y2 = n[3]; *steps = 0;
        return true;
    }
    if (cscdCssIsFunction(x, "steps")) {
        *steps = !x->args.len ? 1 : cscdMaxi(1, cscdIntOfDouble(x->args.items[0].tok.num));
        return true;
    }
    if (cscdCssIsFunction(x, "linear")) { *x1 = 0; *y1 = 0; *x2 = 1; *y2 = 1; *steps = 0; return true; }
    return false;
}

bool cscdParseShadows(CscdSv value, CscdApplyState* st, CscdShadowVec* out, bool text) {
    cscdVecClear(out);
    CscdCssValueVec v = {0};
    cscdCssParseValues(value, false, &v);
    bool ok = true;
    if (v.len == 1 && cscdCssIsIdent(&v.items[0], "none")) goto done;
    CscdCssSlices parts = {0};
    cscdCssSplitCommas(&v, &parts);
    for (size_t pi = 0; pi < parts.len && ok; ++pi) {
        const CscdCssValueVec* part = &parts.items[pi];
        CscdShadow s;
        memset(&s, 0, sizeof s);
        s.color = st->s->color;
        float lens[8];
        size_t nl = 0;
        for (size_t i = 0; i < part->len; ++i) {
            if (cscdCssIsIdent(&part->items[i], "inset") && !text) { s.inset = true; continue; }
            CscdLength l;
            if (cscdParseLength(&part->items[i], &st->ctx, &l, false)) {
                if (nl < 8) lens[nl] = cscdLenResolve(l, 0, 0);
                ++nl;
                continue;
            }
            size_t k = i;
            CscdColor c;
            if (cscdParseColor(part, &k, &c, st->s->color)) { s.color = c; i = k - 1; continue; }
            ok = false;
            break;
        }
        if (!ok) break;
        if (nl < 2) { ok = false; break; }
        s.x = lens[0];
        s.y = lens[1];
        if (nl > 2) s.blur = cscdMaxf(0.f, lens[2]);
        if (nl > 3 && !text) s.spread = lens[3];
        cscdVecPush(out, s);
    }
    cscdVecFree(&parts);
done:
    cscdCssValueVecFree(&v);
    return ok;
}

bool cscdParseTrackSize(const CscdCssValue* x, const CscdValueCtx* c, CscdTrackSize* out) {
    *out = cscdTrackAuto();
    out->kind = CSCD_TRACK_AUTO;
    out->len = cscdLenFixed(0);
    if (x->tok.type == CSCD_TOK_DIMENSION && cscdStrEqC(&x->tok.text, "fr")) { out->kind = CSCD_TRACK_FR; out->fr = (float)x->tok.num; return true; }
    if (cscdCssIsIdent(x, "auto")) { out->kind = CSCD_TRACK_AUTO; return true; }
    if (cscdCssIsIdent(x, "min-content")) { out->kind = CSCD_TRACK_MIN_CONTENT; return true; }
    if (cscdCssIsIdent(x, "max-content")) { out->kind = CSCD_TRACK_MAX_CONTENT; return true; }
    if (cscdCssIsFunction(x, "minmax")) {
        CscdCssSlices parts = {0};
        cscdCssSplitCommas(&x->args, &parts);
        bool ok = parts.len == 2 && parts.items[0].len && parts.items[1].len;
        CscdTrackSize a, b;
        if (ok) ok = cscdParseTrackSize(&parts.items[0].items[0], c, &a) && cscdParseTrackSize(&parts.items[1].items[0], c, &b);
        cscdVecFree(&parts);
        if (!ok) return false;
        out->kind = CSCD_TRACK_MINMAX;
        out->min = cscdTrackIntern(&a);
        out->max = cscdTrackIntern(&b);
        return true;
    }
    if (cscdCssIsFunction(x, "fit-content")) {
        out->kind = CSCD_TRACK_FIT_CONTENT;
        if (x->args.len) cscdParseLength(&x->args.items[0], c, &out->len, false);
        return true;
    }
    CscdLength l;
    if (cscdParseLength(x, c, &l, false)) { out->kind = CSCD_TRACK_FIXED; out->len = l; return true; }
    return false;
}

static void takeNames(CscdTrackList* out, CscdStrVec* pending) {
    cscdVecPush(&out->lineNames, *pending);
    memset(pending, 0, sizeof *pending);
}

bool cscdParseTrackList(const CscdCssValueVec* v, const CscdValueCtx* c, CscdTrackList* out) {
    cscdTrackListFini(out);
    cscdTrackListInit(out);
    if (v->len == 1 && cscdCssIsIdent(&v->items[0], "none")) return true;
    out->none = false;
    if (v->len && cscdCssIsIdent(&v->items[0], "subgrid")) { /* (line names after it are accepted and not used) */
        out->subgrid = true;
        return true;
    }
    CscdStrVec pending = {0};
    bool ok = true;
    for (size_t xi = 0; xi < v->len && ok; ++xi) {
        const CscdCssValue* x = &v->items[xi];
        if (x->tok.type == CSCD_TOK_LBRACKET) {
            for (size_t k = 0; k < x->args.len; ++k)
                if (x->args.items[k].tok.type == CSCD_TOK_IDENT) cscdVecPush(&pending, cscdStrCopy(&x->args.items[k].tok.text));
            continue;
        }
        if (cscdCssIsFunction(x, "repeat")) {
            CscdCssSlices parts = {0};
            cscdCssSplitCommas(&x->args, &parts);
            if (parts.len < 2 || !parts.items[0].len) { cscdVecFree(&parts); ok = false; break; }
            /* the body: the part after the count (a comma in it is not a track size) */
            if (parts.len > 2) { cscdVecFree(&parts); ok = false; break; }
            CscdTrackVec unit = {0};
            for (size_t k = 1; k < parts.len && ok; ++k)
                for (size_t q = 0; q < parts.items[k].len && ok; ++q) {
                    const CscdCssValue* b = &parts.items[k].items[q];
                    if (b->tok.type == CSCD_TOK_LBRACKET) continue;
                    CscdTrackSize t;
                    if (!cscdParseTrackSize(b, c, &t)) ok = false;
                    else cscdVecPush(&unit, t);
                }
            if (!ok) { cscdVecFree(&unit); cscdVecFree(&parts); break; }
            const CscdCssValue* count = &parts.items[0].items[0];
            if (cscdCssIsIdent(count, "auto-fill") || cscdCssIsIdent(count, "auto-fit")) {
                out->autoRepeatAt = (int)out->tracks.len;
                out->autoFit = cscdCssIsIdent(count, "auto-fit");
                cscdVecClear(&out->autoTracks);
                cscdVecAppend(&out->autoTracks, unit.items, unit.len);
            } else {
                int n = cscdIntOfDouble(count->tok.num);
                for (int r = 0; r < n && r < 1000; ++r)
                    for (size_t k = 0; k < unit.len; ++k) {
                        takeNames(out, &pending);
                        cscdVecPush(&out->tracks, unit.items[k]);
                    }
            }
            cscdVecFree(&unit);
            cscdVecFree(&parts);
            continue;
        }
        CscdTrackSize t;
        if (!cscdParseTrackSize(x, c, &t)) { ok = false; break; }
        takeNames(out, &pending);
        cscdVecPush(&out->tracks, t);
    }
    if (!ok) {
        cscdStrVecFree(&pending);
        return false;
    }
    takeNames(out, &pending);
    return true;
}

/* gradient color stops */
static bool parseStops(const CscdCssSlices* parts, size_t from, CscdApplyState* st, CscdBackgroundLayer* L) {
    for (size_t k = from; k < parts->len; ++k) {
        const CscdCssValueVec* p = &parts->items[k];
        size_t i = 0;
        CscdColorStop cs;
        memset(&cs, 0, sizeof cs);
        cs.pos = cscdLenFixed(0);
        if (p->len == 1) {
            CscdLength hint;
            if (cscdParseLength(&p->items[0], &st->ctx, &hint, false)) { cs.hint = true; cs.pos = hint; cscdVecPush(&L->stops, cs); continue; }
        }
        if (!cscdParseColor(p, &i, &cs.color, st->s->color)) {
            /* position before color */
            CscdLength pos;
            if (i < p->len && cscdParseLength(&p->items[i], &st->ctx, &pos, false)) {
                ++i;
                cs.pos = pos;
                if (!cscdParseColor(p, &i, &cs.color, st->s->color)) return false;
                cscdVecPush(&L->stops, cs);
                continue;
            }
            return false;
        }
        cs.pos = cscdLenAuto();
        if (i < p->len) {
            CscdLength pos;
            if (cscdParseLength(&p->items[i], &st->ctx, &pos, false)) { cs.pos = pos; ++i; }
        }
        cscdVecPush(&L->stops, cs);
        if (i < p->len) { /* second position: duplicate the stop */
            CscdLength pos2;
            if (cscdParseLength(&p->items[i], &st->ctx, &pos2, false)) {
                CscdColorStop c2 = cs;
                c2.pos = pos2;
                cscdVecPush(&L->stops, c2);
            }
        }
    }
    return L->stops.len >= 1;
}

bool cscdParseImageValue(const CscdCssValue* x, CscdApplyState* st, CscdBackgroundLayer* L) {
    if (cscdCssIsIdent(x, "none")) { L->kind = CSCD_LAYER_NONE; return true; }
    if (x->tok.type == CSCD_TOK_URL || (cscdCssIsFunction(x, "url") && x->args.len)) {
        L->kind = CSCD_LAYER_URL;
        cscdStrFree(&L->url);
        L->url = cscdJoinPath(st->baseDir, cscdStrSv(x->tok.type == CSCD_TOK_URL ? &x->tok.text : &x->args.items[0].tok.text));
        if (st->env->loadImage) {
            cscdRelease(L->image);
            L->image = st->env->loadImage(st->env->loadImageUser, cscdStrSv(&L->url));
        }
        return true;
    }
    if (cscdCssIsFunction(x, "image-set") && x->args.len) return cscdParseImageValue(&x->args.items[0], st, L);
    if (x->tok.type != CSCD_TOK_FUNCTION) return false;
    CscdSv fn = cscdStrSv(&x->tok.text);
    if (cscdSvStartsC(fn, "-webkit-")) fn = cscdSvFrom(fn, 8);
    if (cscdSvStartsC(fn, "repeating-")) { L->repeating = true; fn = cscdSvFrom(fn, 10); }
    CscdCssSlices parts = {0};
    cscdCssSplitCommas(&x->args, &parts);
    size_t first = 0;
    bool result = false;
    if (cscdSvEqC(fn, "linear-gradient")) {
        L->kind = CSCD_LAYER_LINEAR;
        L->angle = 180;
        if (parts.len && parts.items[0].len) {
            const CscdCssValueVec* p = &parts.items[0];
            float a;
            if (cscdParseAngle(&p->items[0], &a) && p->items[0].tok.type != CSCD_TOK_NUMBER) { L->angle = a * 180.f / 3.14159265f; first = 1; }
            else if (cscdCssIsIdent(&p->items[0], "to")) {
                int cx = 0, cy = 0;
                for (size_t k = 1; k < p->len; ++k) {
                    if (cscdCssIsIdent(&p->items[k], "top")) cy = -1;
                    if (cscdCssIsIdent(&p->items[k], "bottom")) cy = 1;
                    if (cscdCssIsIdent(&p->items[k], "left")) cx = -1;
                    if (cscdCssIsIdent(&p->items[k], "right")) cx = 1;
                }
                if (cx && cy) { L->toCorner = true; L->cornerX = cx; L->cornerY = cy; }
                else L->angle = cy < 0 ? 0 : cy > 0 ? 180 : cx > 0 ? 90 : 270;
                first = 1;
            } else if (cscdSvStartsC(cscdStrSv(&x->tok.text), "-webkit-") && p->items[0].tok.type == CSCD_TOK_IDENT) {
                /* legacy: side where it starts */
                const CscdCssValue* s0 = &p->items[0];
                if (cscdCssIsIdent(s0, "top")) L->angle = 180;
                else if (cscdCssIsIdent(s0, "left")) L->angle = 90;
                else if (cscdCssIsIdent(s0, "bottom")) L->angle = 0;
                else if (cscdCssIsIdent(s0, "right")) L->angle = 270;
                first = 1;
            }
        }
        result = parseStops(&parts, first, st, L);
    } else if (cscdSvEqC(fn, "radial-gradient")) {
        L->kind = CSCD_LAYER_RADIAL;
        if (parts.len && parts.items[0].len) {
            const CscdCssValueVec* p = &parts.items[0];
            CscdColor tmp;
            size_t ci = 0;
            if (!cscdParseColor(p, &ci, &tmp, st->s->color)) {
                first = 1;
                bool at = false;
                int posN = 0;
                CscdLength sizes[4];
                size_t ns = 0;
                for (size_t k = 0; k < p->len; ++k) {
                    const CscdCssValue* t = &p->items[k];
                    if (cscdCssIsIdent(t, "at")) { at = true; continue; }
                    if (!at) {
                        if (cscdCssIsIdent(t, "circle")) L->circle = true;
                        else if (cscdCssIsIdent(t, "ellipse")) L->circle = false;
                        else if (cscdCssIsIdent(t, "closest-side")) L->radialSize = 1;
                        else if (cscdCssIsIdent(t, "closest-corner")) L->radialSize = 2;
                        else if (cscdCssIsIdent(t, "farthest-side")) L->radialSize = 3;
                        else if (cscdCssIsIdent(t, "farthest-corner")) L->radialSize = 0;
                        else {
                            CscdLength l;
                            if (cscdParseLength(t, &st->ctx, &l, false) && ns < 4) sizes[ns++] = l;
                        }
                    } else {
                        CscdLength l;
                        if (cscdCssIsIdent(t, "left")) L->centerX = cscdLenPercent(0);
                        else if (cscdCssIsIdent(t, "right")) L->centerX = cscdLenPercent(100);
                        else if (cscdCssIsIdent(t, "top")) L->centerY = cscdLenPercent(0);
                        else if (cscdCssIsIdent(t, "bottom")) L->centerY = cscdLenPercent(100);
                        else if (cscdCssIsIdent(t, "center")) { if (posN == 0) L->centerX = cscdLenPercent(50); else L->centerY = cscdLenPercent(50); }
                        else if (cscdParseLength(t, &st->ctx, &l, false)) { if (posN == 0) L->centerX = l; else L->centerY = l; }
                        ++posN;
                    }
                }
                if (ns) {
                    L->radialSize = 4;
                    L->radiusX = sizes[0];
                    L->radiusY = ns > 1 ? sizes[1] : sizes[0];
                    if (ns == 1) L->circle = true;
                }
            }
        }
        result = parseStops(&parts, first, st, L);
    } else if (cscdSvEqC(fn, "conic-gradient")) {
        L->kind = CSCD_LAYER_CONIC;
        L->angle = 0;
        if (parts.len && parts.items[0].len && cscdCssIsIdent(&parts.items[0].items[0], "from")) {
            float a;
            if (parts.items[0].len > 1 && cscdParseAngle(&parts.items[0].items[1], &a)) L->angle = a * 180.f / 3.14159265f;
            bool at = false;
            int posN = 0;
            for (size_t k = 0; k < parts.items[0].len; ++k) {
                const CscdCssValue* t = &parts.items[0].items[k];
                if (cscdCssIsIdent(t, "at")) { at = true; continue; }
                CscdLength l;
                if (at && cscdParseLength(t, &st->ctx, &l, false)) { if (posN++ == 0) L->centerX = l; else L->centerY = l; }
            }
            first = 1;
        } else if (parts.len && parts.items[0].len && cscdCssIsIdent(&parts.items[0].items[0], "at")) first = 1;
        /* conic stops use angles; store them as percentages of a turn (on copies of the stops) */
        CscdCssValueVec copies[64];
        size_t nc = cscdMinz(parts.len, 64);
        CscdCssSlices conv = {0};
        for (size_t k = 0; k < nc; ++k) {
            memset(&copies[k], 0, sizeof copies[k]);
            for (size_t q = 0; q < parts.items[k].len; ++q) {
                CscdCssValue t = cscdCssValueCopy(&parts.items[k].items[q]);
                float a;
                if (k >= first && t.tok.type == CSCD_TOK_DIMENSION && cscdParseAngleUnit((float)t.tok.num, cscdStrSv(&t.tok.text), &a)) {
                    t.tok.type = CSCD_TOK_PERCENTAGE;
                    t.tok.num = a / (2 * 3.14159265f) * 100.f;
                }
                cscdVecPush(&copies[k], t);
            }
            cscdVecPush(&conv, copies[k]);
        }
        result = parseStops(&conv, first, st, L);
        for (size_t k = 0; k < nc; ++k) cscdCssValueVecFree(&copies[k]);
        cscdVecFree(&conv);
    }
    cscdVecFree(&parts);
    return result;
}

/* the comma-separated parts of a longhand's raw value (vals holds what parts view) */
static void listOf(const CscdStr* raw, CscdCssValueVec* vals, CscdCssSlices* parts) {
    if (!raw->len) return;
    cscdCssParseValues(cscdStrSv(raw), false, vals);
    cscdCssSplitCommas(vals, parts);
}

static uint8_t boxOf(const CscdCssValueVec* p, CscdBackgroundClip def) {
    if (!p->len) return (uint8_t)def;
    if (cscdCssIsIdent(&p->items[0], "padding-box")) return CSCD_BG_PADDING_BOX;
    if (cscdCssIsIdent(&p->items[0], "content-box")) return CSCD_BG_CONTENT_BOX;
    if (cscdCssIsIdent(&p->items[0], "text")) return CSCD_BG_TEXT;
    return CSCD_BG_BORDER_BOX;
}

void cscdParseBackgrounds(CscdApplyState* st) {
    if (!st->bgTouched) return;
    CscdComputedStyle* s = st->s;
    cscdLayerVecFree(&s->backgrounds);
    if (!st->bgImage.len || cscdSvIEqC(cscdStrSv(&st->bgImage), "none")) return;
    CscdCssValueVec imageVals = {0}, posVals = {0}, sizeVals = {0}, repVals = {0}, clipVals = {0}, originVals = {0};
    CscdCssSlices images = {0}, positions = {0}, sizes = {0}, repeats = {0}, clips = {0}, origins = {0};
    cscdCssParseValues(cscdStrSv(&st->bgImage), false, &imageVals);
    cscdCssSplitCommas(&imageVals, &images);
    listOf(&st->bgPosition, &posVals, &positions);
    listOf(&st->bgSize, &sizeVals, &sizes);
    listOf(&st->bgRepeat, &repVals, &repeats);
    listOf(&st->bgClip, &clipVals, &clips);
    listOf(&st->bgOrigin, &originVals, &origins);
    for (size_t i = 0; i < images.len; ++i) {
        CscdBackgroundLayer L = cscdBackgroundLayer();
        if (!images.items[i].len || !cscdParseImageValue(&images.items[i].items[0], st, &L) || L.kind == CSCD_LAYER_NONE) {
            cscdLayerFini(&L);
            continue;
        }
        if (positions.len) {
            const CscdCssValueVec* p = &positions.items[i % positions.len];
            /* keyword/length pairs, with optional offsets ("right 10px bottom 20px") */
            CscdLength x = cscdLenPercent(0), y = cscdLenPercent(0);
            bool xSet = false, ySet = false;
            for (size_t k = 0; k < p->len; ++k) {
                CscdLength l;
                const CscdCssValue* t = &p->items[k];
                CscdLength* target = NULL;
                float pctBase = 0;
                bool fromEnd = false;
                if (cscdCssIsIdent(t, "left")) { target = &x; pctBase = 0; xSet = true; }
                else if (cscdCssIsIdent(t, "right")) { target = &x; pctBase = 100; fromEnd = true; xSet = true; }
                else if (cscdCssIsIdent(t, "top")) { target = &y; pctBase = 0; ySet = true; }
                else if (cscdCssIsIdent(t, "bottom")) { target = &y; pctBase = 100; fromEnd = true; ySet = true; }
                else if (cscdCssIsIdent(t, "center")) { if (!xSet) { x = cscdLenPercent(50); xSet = true; } else { y = cscdLenPercent(50); ySet = true; } }
                else if (cscdParseLength(t, &st->ctx, &l, false)) { if (!xSet) { x = l; xSet = true; } else { y = l; ySet = true; } }
                if (target) {
                    *target = cscdLenPercent(pctBase);
                    if (k + 1 < p->len && cscdParseLength(&p->items[k + 1], &st->ctx, &l, false)) {
                        ++k;
                        if (fromEnd) { target->pct = 100 - l.pct; target->px = -l.px; }
                        else { target->pct = l.pct; target->px = l.px; }
                    }
                }
            }
            if (xSet && !ySet && p->len == 1) y = cscdLenPercent(50);
            if (ySet && !xSet) x = cscdLenPercent(50);
            L.posX = x;
            L.posY = y;
        }
        if (sizes.len) {
            const CscdCssValueVec* p = &sizes.items[i % sizes.len];
            if (p->len && cscdCssIsIdent(&p->items[0], "cover")) L.sizeKeyword = 1;
            else if (p->len && cscdCssIsIdent(&p->items[0], "contain")) L.sizeKeyword = 2;
            else {
                if (p->len > 0) cscdParseLength(&p->items[0], &st->ctx, &L.sizeX, true);
                if (p->len > 1) cscdParseLength(&p->items[1], &st->ctx, &L.sizeY, true);
            }
        }
        if (repeats.len) {
            const CscdCssValueVec* p = &repeats.items[i % repeats.len];
            if (p->len) {
                if (cscdCssIsIdent(&p->items[0], "no-repeat")) { L.repeatX = L.repeatY = false; }
                else if (cscdCssIsIdent(&p->items[0], "repeat-x")) { L.repeatX = true; L.repeatY = false; }
                else if (cscdCssIsIdent(&p->items[0], "repeat-y")) { L.repeatX = false; L.repeatY = true; }
                if (p->len > 1) { L.repeatX = !cscdCssIsIdent(&p->items[0], "no-repeat"); L.repeatY = !cscdCssIsIdent(&p->items[1], "no-repeat"); }
            }
        }
        if (clips.len) L.clip = boxOf(&clips.items[i % clips.len], CSCD_BG_BORDER_BOX);
        if (origins.len) L.origin = boxOf(&origins.items[i % origins.len], CSCD_BG_PADDING_BOX);
        cscdVecPush(&s->backgrounds, L);
    }
    cscdVecFree(&images); cscdVecFree(&positions); cscdVecFree(&sizes); cscdVecFree(&repeats); cscdVecFree(&clips); cscdVecFree(&origins);
    cscdCssValueVecFree(&imageVals); cscdCssValueVecFree(&posVals); cscdCssValueVecFree(&sizeVals);
    cscdCssValueVecFree(&repVals); cscdCssValueVecFree(&clipVals); cscdCssValueVecFree(&originVals);
}
