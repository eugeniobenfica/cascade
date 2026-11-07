/* CSS value parsing: lengths, calc(), angles, numbers and colors. */
#include "style/styleInternal.h"
#include "cascade/canvas.h"
#include "core/thread.h"

/* ---- calc() ------------------------------------------------------------------------------------- */

typedef enum CalcOp { CALC_VALUE, CALC_ADD, CALC_SUB, CALC_MUL, CALC_DIV, CALC_MIN, CALC_MAX, CALC_CLAMP } CalcOp;

struct CscdCalcNode {
    uint8_t op; /* CalcOp */
    bool isNumber;
    float px, pct, num;
    CscdPtrVec kids; /* CscdCalcNode* */
};

typedef CscdCalcNode CalcNode;

static CalcNode* calcNew(CalcOp op) {
    CalcNode* n = CSCD_NEW(CalcNode);
    n->op = (uint8_t)op;
    return n;
}

static void calcFree(CalcNode* n) {
    if (!n) return;
    for (size_t i = 0; i < n->kids.len; ++i) calcFree((CalcNode*)n->kids.items[i]);
    cscdVecFree(&n->kids);
    cscdFree(n);
}

#define KID(n, i) ((const CalcNode*)(n)->kids.items[i])

/* returns px; numbers return their value */
float cscdCalcEval(const CalcNode* n, float basis) {
    switch ((CalcOp)n->op) {
    case CALC_VALUE: return n->isNumber ? n->num : n->px + n->pct * basis / 100.f;
    case CALC_ADD: return cscdCalcEval(KID(n, 0), basis) + cscdCalcEval(KID(n, 1), basis);
    case CALC_SUB: return cscdCalcEval(KID(n, 0), basis) - cscdCalcEval(KID(n, 1), basis);
    case CALC_MUL: return cscdCalcEval(KID(n, 0), basis) * cscdCalcEval(KID(n, 1), basis);
    case CALC_DIV: { float d = cscdCalcEval(KID(n, 1), basis); return d == 0 ? 0 : cscdCalcEval(KID(n, 0), basis) / d; }
    case CALC_MIN: { float v = cscdCalcEval(KID(n, 0), basis); for (size_t i = 0; i < n->kids.len; ++i) v = cscdMinf(v, cscdCalcEval(KID(n, i), basis)); return v; }
    case CALC_MAX: { float v = cscdCalcEval(KID(n, 0), basis); for (size_t i = 0; i < n->kids.len; ++i) v = cscdMaxf(v, cscdCalcEval(KID(n, i), basis)); return v; }
    case CALC_CLAMP: return cscdMaxf(cscdCalcEval(KID(n, 0), basis), cscdMinf(cscdCalcEval(KID(n, 1), basis), cscdCalcEval(KID(n, 2), basis)));
    }
    return 0;
}

static bool calcNumeric(const CalcNode* n) {
    if (n->op == CALC_VALUE) return n->isNumber;
    if (n->op == CALC_MUL) return calcNumeric(KID(n, 0)) && calcNumeric(KID(n, 1));
    if (n->op == CALC_DIV) return calcNumeric(KID(n, 0));
    for (size_t i = 0; i < n->kids.len; ++i) if (!calcNumeric(KID(n, i))) return false;
    return true;
}

/* folds to px + pct when linear */
static bool calcLinear(const CalcNode* n, float* opx, float* opct) {
    switch ((CalcOp)n->op) {
    case CALC_VALUE:
        if (n->isNumber) { *opx = n->num; *opct = 0; } else { *opx = n->px; *opct = n->pct; }
        return true;
    case CALC_ADD: case CALC_SUB: {
        float a, b, c, d;
        if (!calcLinear(KID(n, 0), &a, &b) || !calcLinear(KID(n, 1), &c, &d)) return false;
        float s = n->op == CALC_ADD ? 1.f : -1.f;
        *opx = a + s * c;
        *opct = b + s * d;
        return true;
    }
    case CALC_MUL: {
        float a, b, c, d;
        if (!calcLinear(KID(n, 0), &a, &b) || !calcLinear(KID(n, 1), &c, &d)) return false;
        if (calcNumeric(KID(n, 0))) { *opx = a * c; *opct = a * d; return true; }
        if (calcNumeric(KID(n, 1))) { *opx = a * c; *opct = b * c; return true; }
        return false;
    }
    case CALC_DIV: {
        float a, b, c, d;
        if (!calcLinear(KID(n, 0), &a, &b) || !calcLinear(KID(n, 1), &c, &d) || !calcNumeric(KID(n, 1)) || c == 0) return false;
        *opx = a / c;
        *opct = b / c;
        return true;
    }
    default: {
        bool anyPct = false;
        for (size_t i = 0; i < n->kids.len; ++i) {
            float a, b;
            if (!calcLinear(KID(n, i), &a, &b)) return false;
            anyPct = anyPct || b != 0;
        }
        if (anyPct) return false;
        *opx = cscdCalcEval(n, 0);
        *opct = 0;
        return true;
    }
    }
}

/* Trees kept for the program's life, one per distinct expression (lengths hold them). */
static CscdMutex internMutex = CSCD_MUTEX_INIT;
static CscdStrMap interned;

static void calcKey(const CalcNode* n, CscdStr* out) {
    cscdStrAppendChar(out, (char)n->op);
    cscdStrAppendChar(out, n->isNumber ? 1 : 0);
    cscdStrAppend(out, cscdSvMake((const char*)&n->px, sizeof(float)));
    cscdStrAppend(out, cscdSvMake((const char*)&n->pct, sizeof(float)));
    cscdStrAppend(out, cscdSvMake((const char*)&n->num, sizeof(float)));
    uint32_t k = (uint32_t)n->kids.len;
    cscdStrAppend(out, cscdSvMake((const char*)&k, sizeof k));
    for (size_t i = 0; i < n->kids.len; ++i) calcKey(KID(n, i), out);
}

/* Takes n; returns the kept tree equal to it. */
static const CalcNode* calcIntern(CalcNode* n) {
    CscdStr key = {0};
    calcKey(n, &key);
    cscdMutexLock(&internMutex);
    bool inserted = false;
    CscdMapVal* v = cscdStrMapPut(&interned, cscdStrSv(&key), &inserted);
    if (inserted) v->p = n;
    else calcFree(n);
    const CalcNode* r = (const CalcNode*)v->p;
    cscdMutexUnlock(&internMutex);
    cscdStrFree(&key);
    return r;
}

float cscdLenResolve(CscdLength l, float basis, float def) {
    switch (l.kind) {
    case CSCD_LEN_FIXED: return l.px + l.pct * basis / 100.f;
    case CSCD_LEN_EXPR: return l.expr ? cscdCalcEval(l.expr, basis) : 0;
    default: return def;
    }
}

float cscdStyleLineHeight(const CscdComputedStyle* s, float ascent, float descent) {
    if (s->lineHeightFactor > 0) return s->lineHeightFactor * s->font.size;
    if (s->lineHeight >= 0) return s->lineHeight;
    return ascent + descent; /* normal */
}

static float shapeLen(const CscdCssValue* v, float basis) {
    if (v->tok.type == CSCD_TOK_PERCENTAGE) return (float)v->tok.num * basis / 100.f;
    return (float)v->tok.num;
}

/* (a position keyword or length along one axis, for "at") */
static float shapePos(const CscdCssValue* v, float basis) {
    if (cscdCssIsIdent(v, "left") || cscdCssIsIdent(v, "top")) return 0;
    if (cscdCssIsIdent(v, "right") || cscdCssIsIdent(v, "bottom")) return basis;
    if (cscdCssIsIdent(v, "center")) return basis / 2;
    return shapeLen(v, basis);
}

bool cscdBasicShapePath(CscdSv value, CscdRect r, CscdPath* p) {
    CscdCssValueVec vals = {0};
    cscdCssParseValues(value, false, &vals);
    bool ok = false;
    if (vals.len && vals.items[0].tok.type == CSCD_TOK_FUNCTION) {
        const CscdCssValue* f = &vals.items[0];
        const CscdCssValueVec* args = &f->args;
        size_t at = args->len; /* (where "at" is) */
        for (size_t i = 0; i < args->len; ++i) if (cscdCssIsIdent(&args->items[i], "at")) { at = i; break; }
        float cx = r.w / 2, cy = r.h / 2;
        if (at + 1 < args->len) cx = shapePos(&args->items[at + 1], r.w);
        if (at + 2 < args->len) cy = shapePos(&args->items[at + 2], r.h);
        if (cscdCssIsFunction(f, "circle")) {
            float rad = cscdMinf(r.w, r.h) / 2;
            if (at > 0) {
                if (cscdCssIsIdent(&args->items[0], "farthest-side")) rad = cscdMaxf(cscdMaxf(cx, r.w - cx), cscdMaxf(cy, r.h - cy));
                else if (cscdCssIsIdent(&args->items[0], "closest-side")) rad = cscdMinf(cscdMinf(cx, r.w - cx), cscdMinf(cy, r.h - cy));
                else rad = shapeLen(&args->items[0], cscdHypotf(r.w, r.h) / sqrtf(2.f));
            } else rad = cscdMinf(cscdMinf(cx, r.w - cx), cscdMinf(cy, r.h - cy));
            cscdPathAddCircle(p, r.x + cx, r.y + cy, rad);
            ok = true;
        } else if (cscdCssIsFunction(f, "ellipse")) {
            const float rx = at > 0 && !cscdCssIsIdent(&args->items[0], "closest-side") ? shapeLen(&args->items[0], r.w) : cscdMinf(cx, r.w - cx);
            const float ry = at > 1 && !cscdCssIsIdent(&args->items[1], "closest-side") ? shapeLen(&args->items[1], r.h) : cscdMinf(cy, r.h - cy);
            cscdPathAddEllipse(p, r.x + cx, r.y + cy, rx, ry);
            ok = true;
        } else if (cscdCssIsFunction(f, "inset")) {
            CscdFloatVec n = {0};
            for (size_t i = 0; i < args->len; ++i) {
                if (cscdCssIsIdent(&args->items[i], "round")) break;
                const float len = shapeLen(&args->items[i], (n.len & 1) ? r.w : r.h);
                cscdVecPush(&n, len);
            }
            float t = n.len > 0 ? n.items[0] : 0, rr = n.len > 1 ? n.items[1] : t, bb = n.len > 2 ? n.items[2] : t, l = n.len > 3 ? n.items[3] : rr;
            cscdPathAddRect(p, cscdRectInset(r, l, t, rr, bb));
            cscdVecFree(&n);
            ok = true;
        } else if (cscdCssIsFunction(f, "polygon")) {
            bool firstPt = true;
            CscdCssSlices parts = {0};
            cscdCssSplitCommas(args, &parts);
            for (size_t i = 0; i < parts.len; ++i) {
                const CscdCssValueVec* part = &parts.items[i];
                if (part->len < 2) continue;
                const float x = r.x + shapeLen(&part->items[0], r.w), y = r.y + shapeLen(&part->items[1], r.h);
                if (firstPt) { cscdPathMoveTo(p, x, y); firstPt = false; }
                else cscdPathLineTo(p, x, y);
            }
            cscdVecFree(&parts);
            if (!firstPt) { cscdPathClose(p); ok = true; }
        }
    }
    cscdCssValueVecFree(&vals);
    return ok;
}

float cscdAbsoluteLength(float v, CscdSv u, const CscdValueCtx* c, bool* ok) {
    *ok = true;
#define U(s) cscdSvEqC(u, s)
    const float z = c->zoom > 0 ? c->zoom : 1;
    if (U("px")) return v * z;
    if (U("em")) return v * c->em;
    if (U("rem")) return v * c->rem * z;
    if (U("ex")) return v * c->em * 0.5f;
    if (U("ch")) return v * c->em * 0.55f;
    if (U("cap")) return v * c->em * 0.7f;
    if (U("lh") || U("rlh")) return v * c->em * 1.2f;
    if (u.len >= 3 && u.ptr[0] == 'c' && u.ptr[1] == 'q') {
        if (c->container) *c->container = true;
        const float w = c->cqw >= 0 ? c->cqw : c->vw, h = c->cqh >= 0 ? c->cqh : c->vh;
        if (U("cqw") || U("cqi")) return v * w / 100.f;
        if (U("cqh") || U("cqb")) return v * h / 100.f;
        if (U("cqmin")) return v * cscdMinf(w, h) / 100.f;
        if (U("cqmax")) return v * cscdMaxf(w, h) / 100.f;
    }
    if (u.len >= 2 && u.ptr[0] == 'v' && c->viewport) *c->viewport = true;
    if (U("vw") || U("svw") || U("lvw") || U("dvw") || U("vi")) return v * c->vw / 100.f;
    if (U("vh") || U("svh") || U("lvh") || U("dvh") || U("vb")) return v * c->vh / 100.f;
    if (U("vmin")) return v * cscdMinf(c->vw, c->vh) / 100.f;
    if (U("vmax")) return v * cscdMaxf(c->vw, c->vh) / 100.f;
    if (U("pt")) return v * z * 96.f / 72.f;
    if (U("pc")) return v * z * 16.f;
    if (U("in")) return v * z * 96.f;
    if (U("cm")) return v * z * 96.f / 2.54f;
    if (U("mm")) return v * z * 96.f / 25.4f;
    if (U("q")) return v * z * 96.f / 101.6f;
#undef U
    *ok = false;
    return 0;
}

static CalcNode* calcValue(const CscdCssValue* v, const CscdValueCtx* c);

/* sum := product (('+'|'-') product)* ; product := unit (('*'|'/') unit)* */
static CalcNode* calcSum(const CscdCssValueVec* t, size_t* i, const CscdValueCtx* c);

static CalcNode* calcUnit(const CscdCssValueVec* t, size_t* i, const CscdValueCtx* c) {
    if (*i >= t->len) return NULL;
    return calcValue(&t->items[(*i)++], c);
}

static CalcNode* calcPair(CalcOp op, CalcNode* a, CalcNode* b) {
    CalcNode* n = calcNew(op);
    cscdVecPush(&n->kids, a);
    cscdVecPush(&n->kids, b);
    return n;
}

static CalcNode* calcProduct(const CscdCssValueVec* t, size_t* i, const CscdValueCtx* c) {
    CalcNode* a = calcUnit(t, i, c);
    while (a && *i < t->len && (cscdCssIsDelim(&t->items[*i], '*') || cscdCssIsDelim(&t->items[*i], '/'))) {
        CalcOp op = cscdCssIsDelim(&t->items[*i], '*') ? CALC_MUL : CALC_DIV;
        ++*i;
        CalcNode* b = calcUnit(t, i, c);
        if (!b) { calcFree(a); return NULL; }
        a = calcPair(op, a, b);
    }
    return a;
}

static CalcNode* calcSum(const CscdCssValueVec* t, size_t* i, const CscdValueCtx* c) {
    CalcNode* a = calcProduct(t, i, c);
    while (a && *i < t->len) {
        const CscdCssValue* x = &t->items[*i];
        CalcOp op;
        if (cscdCssIsDelim(x, '+')) op = CALC_ADD;
        else if (cscdCssIsDelim(x, '-')) op = CALC_SUB;
        else if ((x->tok.type == CSCD_TOK_NUMBER || x->tok.type == CSCD_TOK_DIMENSION || x->tok.type == CSCD_TOK_PERCENTAGE) && x->tok.num < 0) {
            /* "a -2px" tokenized as a negative number: treat as subtraction */
            CalcNode* b = calcProduct(t, i, c);
            if (!b) { calcFree(a); return NULL; }
            a = calcPair(CALC_ADD, a, b);
            continue;
        } else break;
        ++*i;
        CalcNode* b = calcProduct(t, i, c);
        if (!b) { calcFree(a); return NULL; }
        a = calcPair(op, a, b);
    }
    return a;
}

static CalcNode* calcValue(const CscdCssValue* v, const CscdValueCtx* c) {
    const CscdCssToken* t = &v->tok;
    if (t->type == CSCD_TOK_NUMBER) { CalcNode* n = calcNew(CALC_VALUE); n->isNumber = true; n->num = (float)t->num; return n; }
    if (t->type == CSCD_TOK_PERCENTAGE) { CalcNode* n = calcNew(CALC_VALUE); n->pct = (float)t->num; return n; }
    if (t->type == CSCD_TOK_DIMENSION) {
        bool ok;
        float px = cscdAbsoluteLength((float)t->num, cscdStrSv(&t->text), c, &ok);
        CalcNode* n = calcNew(CALC_VALUE);
        if (!ok) {
            /* angles and times inside calc() are returned as plain numbers (deg / s) */
            float a;
            if (cscdParseAngleUnit((float)t->num, cscdStrSv(&t->text), &a)) { n->isNumber = true; n->num = a * 180.f / 3.14159265f; return n; }
            calcFree(n);
            return NULL;
        }
        n->px = px;
        return n;
    }
    if (t->type == CSCD_TOK_IDENT) {
        CscdSv id = cscdStrSv(&t->text);
        CalcNode* n = calcNew(CALC_VALUE);
        n->isNumber = true;
        if (cscdSvIEqC(id, "pi")) { n->num = 3.14159265f; return n; }
        if (cscdSvIEqC(id, "e")) { n->num = 2.7182818f; return n; }
        if (cscdSvIEqC(id, "infinity")) { n->num = 1e30f; return n; }
        calcFree(n);
        return NULL;
    }
    if (t->type == CSCD_TOK_LPAREN || cscdCssIsFunction(v, "calc")) {
        size_t i = 0;
        return calcSum(&v->args, &i, c);
    }
    if (cscdCssIsFunction(v, "min") || cscdCssIsFunction(v, "max") || cscdCssIsFunction(v, "clamp")) {
        CalcNode* n = calcNew(cscdCssIsFunction(v, "min") ? CALC_MIN : cscdCssIsFunction(v, "max") ? CALC_MAX : CALC_CLAMP);
        CscdCssSlices parts = {0};
        cscdCssSplitCommas(&v->args, &parts);
        for (size_t p = 0; p < parts.len; ++p) {
            size_t i = 0;
            CalcNode* k = calcSum(&parts.items[p], &i, c);
            if (!k) { calcFree(n); cscdVecFree(&parts); return NULL; }
            cscdVecPush(&n->kids, k);
        }
        cscdVecFree(&parts);
        if (!n->kids.len || (n->op == CALC_CLAMP && n->kids.len != 3)) { calcFree(n); return NULL; }
        return n;
    }
    return NULL;
}

bool cscdParseAngleUnit(float v, CscdSv u, float* rad) {
    if (cscdSvEqC(u, "deg")) { *rad = v * 3.14159265f / 180.f; return true; }
    if (cscdSvEqC(u, "rad")) { *rad = v; return true; }
    if (cscdSvEqC(u, "grad")) { *rad = v * 3.14159265f / 200.f; return true; }
    if (cscdSvEqC(u, "turn")) { *rad = v * 2 * 3.14159265f; return true; }
    return false;
}

bool cscdParseAngle(const CscdCssValue* v, float* rad) {
    if (v->tok.type == CSCD_TOK_DIMENSION) return cscdParseAngleUnit((float)v->tok.num, cscdStrSv(&v->tok.text), rad);
    if (v->tok.type == CSCD_TOK_NUMBER && v->tok.num == 0) { *rad = 0; return true; }
    if (cscdCssIsFunction(v, "calc")) {
        CscdValueCtx c = cscdValueCtx();
        size_t i = 0;
        CalcNode* n = calcSum(&v->args, &i, &c);
        if (!n) return false;
        *rad = cscdCalcEval(n, 0) * 3.14159265f / 180.f;
        calcFree(n);
        return true;
    }
    return false;
}

bool cscdParseNumber(const CscdCssValue* v, float* out, const CscdValueCtx* c) {
    if (v->tok.type == CSCD_TOK_NUMBER) { *out = (float)v->tok.num; return true; }
    if (cscdCssIsFunction(v, "calc") || cscdCssIsFunction(v, "min") || cscdCssIsFunction(v, "max") || cscdCssIsFunction(v, "clamp")) {
        CalcNode* n = calcValue(v, c);
        if (!n || !calcNumeric(n)) { calcFree(n); return false; }
        *out = cscdCalcEval(n, 0);
        calcFree(n);
        return true;
    }
    return false;
}

bool cscdParseLength(const CscdCssValue* v, const CscdValueCtx* c, CscdLength* out, bool allowKeywords) {
    const CscdCssToken* t = &v->tok;
    switch (t->type) {
    case CSCD_TOK_DIMENSION: {
        bool ok;
        float px = cscdAbsoluteLength((float)t->num, cscdStrSv(&t->text), c, &ok);
        if (!ok) return false;
        *out = cscdLenFixed(px);
        return true;
    }
    case CSCD_TOK_PERCENTAGE: *out = cscdLenPercent((float)t->num); return true;
    case CSCD_TOK_NUMBER:
        if (t->num == 0 || c->quirkUnitless) { *out = cscdLenFixed((float)t->num); return true; }
        return false;
    case CSCD_TOK_IDENT: {
        if (!allowKeywords) return false;
        CscdSv id = cscdStrSv(&t->text);
        if (cscdSvIEqC(id, "auto")) { *out = cscdLenAuto(); return true; }
        if (cscdSvIEqC(id, "none")) { *out = cscdLenNone(); return true; }
        if (cscdSvIEqC(id, "min-content")) { *out = cscdLenKind(CSCD_LEN_MIN_CONTENT); return true; }
        if (cscdSvIEqC(id, "max-content")) { *out = cscdLenKind(CSCD_LEN_MAX_CONTENT); return true; }
        if (cscdSvIEqC(id, "fit-content")) { *out = cscdLenKind(CSCD_LEN_FIT_CONTENT); return true; }
        if (cscdSvIEqC(id, "-webkit-fill-available") || cscdSvIEqC(id, "stretch") || cscdSvIEqC(id, "-moz-available")) {
            *out = cscdLenPercent(100);
            return true;
        }
        return false;
    }
    case CSCD_TOK_FUNCTION: {
        if (cscdCssIsFunction(v, "fit-content") && allowKeywords) { *out = cscdLenKind(CSCD_LEN_FIT_CONTENT); return true; }
        CalcNode* n = calcValue(v, c);
        if (!n) return false;
        float px, pct;
        if (calcLinear(n, &px, &pct)) {
            *out = cscdLenFixed(px);
            out->pct = pct;
            calcFree(n);
            return true;
        }
        *out = cscdLenKind(CSCD_LEN_EXPR);
        out->expr = calcIntern(n);
        return true;
    }
    default: return false;
    }
}

float cscdLengthPx(const CscdCssValue* v, const CscdValueCtx* c, float def) {
    CscdLength l;
    if (!cscdParseLength(v, c, &l, false)) return def;
    return cscdLenResolve(l, 0, 0);
}

/* ---- colors ------------------------------------------------------------------------------------- */

typedef struct NamedColor { const char* name; uint32_t rgb; } NamedColor;

static const NamedColor kNamedColors[] = {
    {"aliceblue", 0xF0F8FF},
    {"antiquewhite", 0xFAEBD7},
    {"aqua", 0x00FFFF},
    {"aquamarine", 0x7FFFD4},
    {"azure", 0xF0FFFF},
    {"beige", 0xF5F5DC},
    {"bisque", 0xFFE4C4},
    {"black", 0x000000},
    {"blanchedalmond", 0xFFEBCD},
    {"blue", 0x0000FF},
    {"blueviolet", 0x8A2BE2},
    {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887},
    {"cadetblue", 0x5F9EA0},
    {"chartreuse", 0x7FFF00},
    {"chocolate", 0xD2691E},
    {"coral", 0xFF7F50},
    {"cornflowerblue", 0x6495ED},
    {"cornsilk", 0xFFF8DC},
    {"crimson", 0xDC143C},
    {"cyan", 0x00FFFF},
    {"darkblue", 0x00008B},
    {"darkcyan", 0x008B8B},
    {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9},
    {"darkgreen", 0x006400},
    {"darkgrey", 0xA9A9A9},
    {"darkkhaki", 0xBDB76B},
    {"darkmagenta", 0x8B008B},
    {"darkolivegreen", 0x556B2F},
    {"darkorange", 0xFF8C00},
    {"darkorchid", 0x9932CC},
    {"darkred", 0x8B0000},
    {"darksalmon", 0xE9967A},
    {"darkseagreen", 0x8FBC8F},
    {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F},
    {"darkslategrey", 0x2F4F4F},
    {"darkturquoise", 0x00CED1},
    {"darkviolet", 0x9400D3},
    {"deeppink", 0xFF1493},
    {"deepskyblue", 0x00BFFF},
    {"dimgray", 0x696969},
    {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF},
    {"firebrick", 0xB22222},
    {"floralwhite", 0xFFFAF0},
    {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF},
    {"gainsboro", 0xDCDCDC},
    {"ghostwhite", 0xF8F8FF},
    {"gold", 0xFFD700},
    {"goldenrod", 0xDAA520},
    {"gray", 0x808080},
    {"green", 0x008000},
    {"greenyellow", 0xADFF2F},
    {"grey", 0x808080},
    {"honeydew", 0xF0FFF0},
    {"hotpink", 0xFF69B4},
    {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082},
    {"ivory", 0xFFFFF0},
    {"khaki", 0xF0E68C},
    {"lavender", 0xE6E6FA},
    {"lavenderblush", 0xFFF0F5},
    {"lawngreen", 0x7CFC00},
    {"lemonchiffon", 0xFFFACD},
    {"lightblue", 0xADD8E6},
    {"lightcoral", 0xF08080},
    {"lightcyan", 0xE0FFFF},
    {"lightgoldenrodyellow", 0xFAFAD2},
    {"lightgray", 0xD3D3D3},
    {"lightgreen", 0x90EE90},
    {"lightgrey", 0xD3D3D3},
    {"lightpink", 0xFFB6C1},
    {"lightsalmon", 0xFFA07A},
    {"lightseagreen", 0x20B2AA},
    {"lightskyblue", 0x87CEFA},
    {"lightslategray", 0x778899},
    {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xB0C4DE},
    {"lightyellow", 0xFFFFE0},
    {"lime", 0x00FF00},
    {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6},
    {"magenta", 0xFF00FF},
    {"maroon", 0x800000},
    {"mediumaquamarine", 0x66CDAA},
    {"mediumblue", 0x0000CD},
    {"mediumorchid", 0xBA55D3},
    {"mediumpurple", 0x9370DB},
    {"mediumseagreen", 0x3CB371},
    {"mediumslateblue", 0x7B68EE},
    {"mediumspringgreen", 0x00FA9A},
    {"mediumturquoise", 0x48D1CC},
    {"mediumvioletred", 0xC71585},
    {"midnightblue", 0x191970},
    {"mintcream", 0xF5FFFA},
    {"mistyrose", 0xFFE4E1},
    {"moccasin", 0xFFE4B5},
    {"navajowhite", 0xFFDEAD},
    {"navy", 0x000080},
    {"oldlace", 0xFDF5E6},
    {"olive", 0x808000},
    {"olivedrab", 0x6B8E23},
    {"orange", 0xFFA500},
    {"orangered", 0xFF4500},
    {"orchid", 0xDA70D6},
    {"palegoldenrod", 0xEEE8AA},
    {"palegreen", 0x98FB98},
    {"paleturquoise", 0xAFEEEE},
    {"palevioletred", 0xDB7093},
    {"papayawhip", 0xFFEFD5},
    {"peachpuff", 0xFFDAB9},
    {"peru", 0xCD853F},
    {"pink", 0xFFC0CB},
    {"plum", 0xDDA0DD},
    {"powderblue", 0xB0E0E6},
    {"purple", 0x800080},
    {"rebeccapurple", 0x663399},
    {"red", 0xFF0000},
    {"rosybrown", 0xBC8F8F},
    {"royalblue", 0x4169E1},
    {"saddlebrown", 0x8B4513},
    {"salmon", 0xFA8072},
    {"sandybrown", 0xF4A460},
    {"seagreen", 0x2E8B57},
    {"seashell", 0xFFF5EE},
    {"sienna", 0xA0522D},
    {"silver", 0xC0C0C0},
    {"skyblue", 0x87CEEB},
    {"slateblue", 0x6A5ACD},
    {"slategray", 0x708090},
    {"slategrey", 0x708090},
    {"snow", 0xFFFAFA},
    {"springgreen", 0x00FF7F},
    {"steelblue", 0x4682B4},
    {"tan", 0xD2B48C},
    {"teal", 0x008080},
    {"thistle", 0xD8BFD8},
    {"tomato", 0xFF6347},
    {"turquoise", 0x40E0D0},
    {"violet", 0xEE82EE},
    {"wheat", 0xF5DEB3},
    {"white", 0xFFFFFF},
    {"whitesmoke", 0xF5F5F5},
    {"yellow", 0xFFFF00},
    {"yellowgreen", 0x9ACD32},
    {"canvas", 0xFFFFFF},
    {"canvastext", 0x000000},
    {"buttonface", 0xEFEFEF},
    {"buttontext", 0x000000},
    {"buttonborder", 0x767676},
    {"field", 0xFFFFFF},
    {"fieldtext", 0x000000},
    {"highlight", 0x3390FF},
    {"highlighttext", 0xFFFFFF},
    {"graytext", 0x6D6D6D},
    {"linktext", 0x0000EE},
    {"visitedtext", 0x551A8B},
    {"activetext", 0xFF0000},
    {"accentcolor", 0x0075FF},
    {"accentcolortext", 0xFFFFFF},
    {"mark", 0xFFFF00},
    {"marktext", 0x000000},
    {"selecteditem", 0x0075FF},
    {"selecteditemtext", 0xFFFFFF},
    {"window", 0xFFFFFF},
    {"windowtext", 0x000000},
    {"buttonhighlight", 0xFFFFFF},
    {"buttonshadow", 0xA0A0A0}
};

static CscdStrMap colorMap;
static CscdOnce colorOnce = CSCD_ONCE_INIT;
static void initColors(void) {
    for (size_t i = 0; i < CSCD_COUNT(kNamedColors); ++i) cscdStrMapPut(&colorMap, cscdSv(kNamedColors[i].name), NULL)->u = kNamedColors[i].rgb;
}

static float hue2rgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < 1.f / 2) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
}

static CscdColor hsl(float h, float s, float l, float a) {
    h = fmodf(fmodf(h, 360.f) + 360.f, 360.f) / 360.f;
    s = cscdClampf(s, 0.f, 1.f);
    l = cscdClampf(l, 0.f, 1.f);
    if (s == 0) return cscdColorFromFloat(l, l, l, a);
    float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
    float p = 2 * l - q;
    return cscdColorFromFloat(hue2rgb(p, q, h + 1.f / 3), hue2rgb(p, q, h), hue2rgb(p, q, h - 1.f / 3), a);
}

static float srgbFromLinear(float x) {
    x = cscdClampf(x, 0.f, 1.f);
    return x <= 0.0031308f ? 12.92f * x : 1.055f * cscdPowf(x, 1 / 2.4f) - 0.055f;
}

static CscdColor oklab(float L, float a, float b, float alpha) {
    float l_ = L + 0.3963377774f * a + 0.2158037573f * b;
    float m_ = L - 0.1055613458f * a - 0.0638541728f * b;
    float s_ = L - 0.0894841775f * a - 1.2914855480f * b;
    float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    float r = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    float g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    float bb = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
    return cscdColorFromFloat(srgbFromLinear(r), srgbFromLinear(g), srgbFromLinear(bb), alpha);
}

/* arguments of rgb()/hsl()...: comma or space separated, optional "/ alpha" */
static bool colorArgs(const CscdCssValueVec* args, const CscdCssValue** out, const CscdCssValue** alpha) {
    *alpha = NULL;
    bool slash = false;
    size_t n = 0;
    for (size_t i = 0; i < args->len; ++i) {
        const CscdCssValue* a = &args->items[i];
        if (cscdCssIsComma(a)) continue;
        if (cscdCssIsDelim(a, '/')) { slash = true; continue; }
        if (slash) *alpha = a;
        else if (n < 8) out[n++] = a;
        else ++n;
    }
    if (!*alpha && n == 4) { *alpha = out[3]; n = 3; }
    return n == 3;
}

static float alphaValue(const CscdCssValue* a) {
    if (!a) return 1;
    if (a->tok.type == CSCD_TOK_PERCENTAGE) return cscdClampf((float)a->tok.num / 100.f, 0.f, 1.f);
    if (a->tok.type == CSCD_TOK_NUMBER) return cscdClampf((float)a->tok.num, 0.f, 1.f);
    if (cscdCssIsIdent(a, "none")) return 0;
    float v;
    CscdValueCtx c = cscdValueCtx();
    if (cscdParseNumber(a, &v, &c)) return cscdClampf(v, 0.f, 1.f);
    return 1;
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = cscdToLower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool hexColor(CscdSv h, CscdColor* out) {
    for (size_t i = 0; i < h.len; ++i) if (hexValue(h.ptr[i]) < 0) return false;
    const char* p = h.ptr;
    if (h.len == 3 || h.len == 4) {
        out->r = (uint8_t)(hexValue(p[0]) * 17); out->g = (uint8_t)(hexValue(p[1]) * 17); out->b = (uint8_t)(hexValue(p[2]) * 17);
        out->a = h.len == 4 ? (uint8_t)(hexValue(p[3]) * 17) : 255;
        return true;
    }
    if (h.len == 6 || h.len == 8) {
        out->r = (uint8_t)(hexValue(p[0]) * 16 + hexValue(p[1])); out->g = (uint8_t)(hexValue(p[2]) * 16 + hexValue(p[3]));
        out->b = (uint8_t)(hexValue(p[4]) * 16 + hexValue(p[5]));
        out->a = h.len == 8 ? (uint8_t)(hexValue(p[6]) * 16 + hexValue(p[7])) : 255;
        return true;
    }
    return false;
}

static float colorNum(const CscdCssValue* p, float scalePct, const CscdValueCtx* c) {
    if (p->tok.type == CSCD_TOK_PERCENTAGE) return (float)p->tok.num * scalePct / 100.f;
    if (cscdCssIsIdent(p, "none")) return 0;
    float r = 0;
    if (cscdParseNumber(p, &r, c)) return r;
    float ang;
    if (cscdParseAngle(p, &ang)) return ang * 180.f / 3.14159265f;
    return 0;
}

static float pctOr(const CscdCssValue* p, float div, float scale, const CscdValueCtx* c) {
    return p->tok.type == CSCD_TOK_PERCENTAGE ? (float)p->tok.num / 100.f * scale : colorNum(p, 1, c) / div;
}

bool cscdParseColor(const CscdCssValueVec* v, size_t* i, CscdColor* out, CscdColor current) {
    if (*i >= v->len) return false;
    const CscdCssValue* x = &v->items[*i];
    const CscdCssToken* t = &x->tok;
    if (t->type == CSCD_TOK_HASH) {
        if (!hexColor(cscdStrSv(&t->text), out)) return false;
        ++*i;
        return true;
    }
    if (t->type == CSCD_TOK_IDENT) {
        CscdStr id = cscdLower(cscdStrSv(&t->text));
        bool ok = true;
        if (cscdStrEqC(&id, "transparent")) *out = (CscdColor){0, 0, 0, 0};
        else if (cscdStrEqC(&id, "currentcolor")) *out = current;
        else {
            cscdCallOnce(&colorOnce, initColors);
            CscdMapVal* c = cscdStrMapGet(&colorMap, cscdStrSv(&id));
            if (c) *out = cscdRgb((int)((c->u >> 16) & 255), (int)((c->u >> 8) & 255), (int)(c->u & 255));
            else ok = false;
        }
        cscdStrFree(&id);
        if (ok) ++*i;
        return ok;
    }
    if (t->type != CSCD_TOK_FUNCTION) return false;
    const CscdCssValue* a[8];
    const CscdCssValue* alpha;
    CscdValueCtx c = cscdValueCtx();
    if (cscdCssIsFunction(x, "rgb") || cscdCssIsFunction(x, "rgba")) {
        if (!colorArgs(&x->args, a, &alpha)) return false;
        *out = cscdRgba((int)lroundf(colorNum(a[0], 255, &c)), (int)lroundf(colorNum(a[1], 255, &c)), (int)lroundf(colorNum(a[2], 255, &c)),
                      (int)lroundf(alphaValue(alpha) * 255));
        ++*i;
        return true;
    }
    if (cscdCssIsFunction(x, "hsl") || cscdCssIsFunction(x, "hsla")) {
        if (!colorArgs(&x->args, a, &alpha)) return false;
        float s = pctOr(a[1], 100.f, 1, &c);
        float l = pctOr(a[2], 100.f, 1, &c);
        *out = hsl(colorNum(a[0], 1, &c), s, l, alphaValue(alpha));
        ++*i;
        return true;
    }
    if (cscdCssIsFunction(x, "hwb")) {
        if (!colorArgs(&x->args, a, &alpha)) return false;
        float w = colorNum(a[1], 1, &c) / 100.f, b = colorNum(a[2], 1, &c) / 100.f;
        if (a[1]->tok.type == CSCD_TOK_PERCENTAGE) w = (float)a[1]->tok.num / 100.f;
        if (a[2]->tok.type == CSCD_TOK_PERCENTAGE) b = (float)a[2]->tok.num / 100.f;
        if (w + b >= 1) {
            float g = w / (w + b);
            *out = cscdColorFromFloat(g, g, g, alphaValue(alpha));
        } else {
            CscdColor base = hsl(colorNum(a[0], 1, &c), 1, 0.5f, 1);
#define CH(v) (((v) / 255.f) * (1 - w - b) + w)
            *out = cscdColorFromFloat(CH(base.r), CH(base.g), CH(base.b), alphaValue(alpha));
#undef CH
        }
        ++*i;
        return true;
    }
    if (cscdCssIsFunction(x, "oklab") || cscdCssIsFunction(x, "oklch") || cscdCssIsFunction(x, "lab") || cscdCssIsFunction(x, "lch")) {
        if (!colorArgs(&x->args, a, &alpha)) return false;
        bool ok = cscdCssIsFunction(x, "oklab") || cscdCssIsFunction(x, "oklch");
        float L = a[0]->tok.type == CSCD_TOK_PERCENTAGE ? (float)a[0]->tok.num / 100.f : colorNum(a[0], 1, &c);
        if (!ok) L /= 100.f;
        float A, B;
        if (cscdCssIsFunction(x, "oklch") || cscdCssIsFunction(x, "lch")) {
            float C = a[1]->tok.type == CSCD_TOK_PERCENTAGE ? (float)a[1]->tok.num / 100.f * (ok ? 0.4f : 150.f) : colorNum(a[1], 1, &c);
            float H = colorNum(a[2], 1, &c) * 3.14159265f / 180.f;
            A = C * cscdCosf(H);
            B = C * cscdSinf(H);
        } else {
            A = a[1]->tok.type == CSCD_TOK_PERCENTAGE ? (float)a[1]->tok.num / 100.f * (ok ? 0.4f : 125.f) : colorNum(a[1], 1, &c);
            B = a[2]->tok.type == CSCD_TOK_PERCENTAGE ? (float)a[2]->tok.num / 100.f * (ok ? 0.4f : 125.f) : colorNum(a[2], 1, &c);
        }
        if (!ok) { A /= 400.f; B /= 400.f; } /* CIE Lab approximated through Oklab */
        *out = oklab(L, A, B, alphaValue(alpha));
        ++*i;
        return true;
    }
    if (cscdCssIsFunction(x, "color-mix")) {
        /* color-mix(in srgb, c1 [p1], c2 [p2]) */
        CscdCssSlices parts = {0};
        cscdCssSplitCommas(&x->args, &parts);
        bool ok = parts.len == 3;
        CscdColor c1, c2;
        float p1 = -1, p2 = -1;
        size_t k = 0;
        if (ok && !cscdParseColor(&parts.items[1], &k, &c1, current)) ok = false;
        if (ok && k < parts.items[1].len && parts.items[1].items[k].tok.type == CSCD_TOK_PERCENTAGE) p1 = (float)parts.items[1].items[k].tok.num / 100.f;
        k = 0;
        if (ok && !cscdParseColor(&parts.items[2], &k, &c2, current)) ok = false;
        if (ok && k < parts.items[2].len && parts.items[2].items[k].tok.type == CSCD_TOK_PERCENTAGE) p2 = (float)parts.items[2].items[k].tok.num / 100.f;
        cscdVecFree(&parts);
        if (!ok) return false;
        if (p1 < 0 && p2 < 0) p1 = 0.5f;
        if (p1 < 0) p1 = 1 - p2;
        *out = cscdColorLerp(c1, c2, 1 - p1);
        ++*i;
        return true;
    }
    if (cscdCssIsFunction(x, "light-dark")) {
        CscdCssSlices parts = {0};
        cscdCssSplitCommas(&x->args, &parts);
        bool ok = parts.len == 2;
        size_t k = 0;
        if (ok && !cscdParseColor(&parts.items[0], &k, out, current)) ok = false;
        cscdVecFree(&parts);
        if (!ok) return false;
        ++*i;
        return true;
    }
    return false;
}

bool cscdParseColorString(CscdSv s, CscdColor* out) {
    CscdCssValueVec v = {0};
    cscdCssParseValues(s, false, &v);
    size_t i = 0;
    bool ok = cscdParseColor(&v, &i, out, cscdRgb(0, 0, 0)) && i == v.len;
    cscdCssValueVecFree(&v);
    return ok;
}
