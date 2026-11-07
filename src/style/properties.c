/* Shorthand expansion and longhand application for every supported property. */
#include "style/styleInternal.h"
#include "core/thread.h"

#include <stdio.h>
#include <stdlib.h>

typedef CscdCssValueVec Values;

static CscdStr one(const CscdCssValue* x) {
    CscdStr s = {0};
    cscdCssSerialize(x, 1, &s);
    return s;
}

static CscdStr ser(const Values* v, size_t a, size_t b) {
    CscdStr s = {0};
    if (b > a) cscdCssSerialize(v->items + a, b - a, &s);
    return s;
}

static bool isWideKeyword(CscdSv v) {
    return cscdSvIEqC(v, "inherit") || cscdSvIEqC(v, "initial") || cscdSvIEqC(v, "unset") || cscdSvIEqC(v, "revert") || cscdSvIEqC(v, "revert-layer");
}

static const char* kSides[4] = {"top", "right", "bottom", "left"};
static const char* kCorners[4] = {"top-left", "top-right", "bottom-right", "bottom-left"};

/* takes name and value */
static void addPair(CscdStrPairVec* out, CscdStr name, CscdStr value) {
    CscdStrPair p = {name, value};
    cscdVecPush(out, p);
}

static void addPairC(CscdStrPairVec* out, const char* name, CscdSv value) { addPair(out, cscdStrDupC(name), cscdStrDup(value)); }

/* 1-4 values -> top right bottom left */
static void boxSides(const CscdCssValue* v, size_t count, const char* prefix, const char* suffix, CscdStrPairVec* out) {
    CscdStr s[4] = {{0}};
    size_t n = cscdMinz(count, 4);
    for (size_t i = 0; i < n; ++i) s[i] = one(&v[i]);
    if (n == 0) return;
    if (n == 1) { s[1] = cscdStrCopy(&s[0]); s[2] = cscdStrCopy(&s[0]); s[3] = cscdStrCopy(&s[0]); }
    else if (n == 2) { s[2] = cscdStrCopy(&s[0]); s[3] = cscdStrCopy(&s[1]); }
    else if (n == 3) s[3] = cscdStrCopy(&s[1]);
    for (int i = 0; i < 4; ++i) {
        CscdStr name = cscdStrDupC(prefix);
        cscdStrAppend(&name, cscdSv(kSides[i]));
        cscdStrAppend(&name, cscdSv(suffix));
        addPair(out, name, s[i]);
    }
}

static bool isBorderStyle(const CscdCssValue* v) {
    static const char* names[] = {"none", "hidden", "solid", "dashed", "dotted", "double", "groove", "ridge", "inset", "outset"};
    for (size_t i = 0; i < CSCD_COUNT(names); ++i) if (cscdCssIsIdent(v, names[i])) return true;
    return false;
}

static bool isColorValue(const CscdCssValue* v) {
    Values one1 = cscdCssView(v, 1);
    size_t i = 0;
    CscdColor c;
    return cscdParseColor(&one1, &i, &c, (CscdColor){0, 0, 0, 0});
}

static bool isLengthValue(const CscdCssValue* v) {
    CscdLength l;
    CscdValueCtx c = cscdValueCtx();
    if (cscdCssIsIdent(v, "thin") || cscdCssIsIdent(v, "medium") || cscdCssIsIdent(v, "thick")) return true;
    return cscdParseLength(v, &c, &l, false);
}

static void borderParts(const Values* v, CscdStr* w, CscdStr* st, CscdStr* col) {
    for (size_t i = 0; i < v->len; ++i) {
        const CscdCssValue* x = &v->items[i];
        CscdStr s = one(x);
        if (isBorderStyle(x)) { cscdStrFree(st); *st = s; }
        else if (isLengthValue(x)) { cscdStrFree(w); *w = s; }
        else { cscdStrFree(col); *col = s; }
    }
}

/* ---- canonical names ------------------------------------------------------------------------------ */

static const char* kCanonical[][2] = {
    {"word-wrap", "overflow-wrap"}, {"grid-row-gap", "row-gap"}, {"grid-column-gap", "column-gap"},
    {"grid-gap", "gap"}, {"margin-inline-start", "margin-left"}, {"margin-inline-end", "margin-right"},
    {"margin-block-start", "margin-top"}, {"margin-block-end", "margin-bottom"},
    {"padding-inline-start", "padding-left"}, {"padding-inline-end", "padding-right"},
    {"padding-block-start", "padding-top"}, {"padding-block-end", "padding-bottom"},
    {"inset-inline-start", "left"}, {"inset-inline-end", "right"}, {"inset-block-start", "top"}, {"inset-block-end", "bottom"},
    {"border-inline-start", "border-left"}, {"border-inline-end", "border-right"},
    {"border-block-start", "border-top"}, {"border-block-end", "border-bottom"},
    {"border-start-start-radius", "border-top-left-radius"}, {"border-start-end-radius", "border-top-right-radius"},
    {"border-end-start-radius", "border-bottom-left-radius"}, {"border-end-end-radius", "border-bottom-right-radius"},
    {"inline-size", "width"}, {"block-size", "height"}, {"min-inline-size", "min-width"}, {"min-block-size", "min-height"},
    {"max-inline-size", "max-width"}, {"max-block-size", "max-height"},
    {"-webkit-box-shadow", "box-shadow"}, {"-webkit-transform", "transform"}, {"-webkit-line-clamp", "line-clamp"},
    {"-webkit-transition", "transition"}, {"-webkit-animation", "animation"}, {"-webkit-user-select", "user-select"},
    {"-webkit-box-sizing", "box-sizing"}, {"-moz-box-sizing", "box-sizing"}, {"-webkit-border-radius", "border-radius"},
    {"-webkit-flex", "flex"}, {"-webkit-flex-direction", "flex-direction"}, {"-webkit-justify-content", "justify-content"},
    {"-webkit-align-items", "align-items"}, {"-webkit-transform-origin", "transform-origin"},
    {"-webkit-text-decoration", "text-decoration"}, {"-webkit-filter", "filter"}, {"-webkit-appearance", "appearance"},
    {"-moz-appearance", "appearance"}, {"-webkit-font-smoothing", "font-smoothing"},
};

/* the tables' names by hash, made once: every declaration of every matched rule looks its name up */
static CscdOnce namesOnce;
static CscdStrMap canonicalIndex, shorthandIndex; /* name -> its row */
static void buildNameIndex(void);

CscdSv cscdCanonicalProperty(CscdSv p) {
    cscdCallOnce(&namesOnce, buildNameIndex);
    const CscdMapVal* v = cscdStrMapGet(&canonicalIndex, p);
    return v ? cscdSv(kCanonical[v->u][1]) : p;
}

/* ---- shorthands ----------------------------------------------------------------------------------- */

typedef struct Shorthand { const char* name; const char* longhands[13]; } Shorthand;

static const Shorthand kShorthands[] = {
    {"margin", {"margin-top", "margin-right", "margin-bottom", "margin-left"}},
    {"padding", {"padding-top", "padding-right", "padding-bottom", "padding-left"}},
    {"inset", {"top", "right", "bottom", "left"}},
    {"border-width", {"border-top-width", "border-right-width", "border-bottom-width", "border-left-width"}},
    {"border-style", {"border-top-style", "border-right-style", "border-bottom-style", "border-left-style"}},
    {"border-color", {"border-top-color", "border-right-color", "border-bottom-color", "border-left-color"}},
    {"border-radius", {"border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius", "border-bottom-left-radius"}},
    {"border", {"border-top-width", "border-right-width", "border-bottom-width", "border-left-width", "border-top-style", "border-right-style",
                "border-bottom-style", "border-left-style", "border-top-color", "border-right-color", "border-bottom-color", "border-left-color"}},
    {"border-top", {"border-top-width", "border-top-style", "border-top-color"}},
    {"border-right", {"border-right-width", "border-right-style", "border-right-color"}},
    {"border-bottom", {"border-bottom-width", "border-bottom-style", "border-bottom-color"}},
    {"border-left", {"border-left-width", "border-left-style", "border-left-color"}},
    {"background", {"background-color", "background-image", "background-position", "background-size", "background-repeat", "background-clip", "background-origin"}},
    {"font", {"font-style", "font-variant", "font-weight", "font-size", "line-height", "font-family", "font-stretch"}},
    {"flex", {"flex-grow", "flex-shrink", "flex-basis"}},
    {"flex-flow", {"flex-direction", "flex-wrap"}},
    {"gap", {"row-gap", "column-gap"}},
    {"overflow", {"overflow-x", "overflow-y"}},
    {"outline", {"outline-width", "outline-style", "outline-color"}},
    {"text-decoration", {"text-decoration-line", "text-decoration-style", "text-decoration-color", "text-decoration-thickness"}},
    {"list-style", {"list-style-type", "list-style-position", "list-style-image"}},
    {"place-items", {"align-items", "justify-items"}},
    {"place-content", {"align-content", "justify-content"}},
    {"place-self", {"align-self", "justify-self"}},
    {"grid-row", {"grid-row-start", "grid-row-end"}},
    {"grid-column", {"grid-column-start", "grid-column-end"}},
    {"grid-area", {"grid-row-start", "grid-column-start", "grid-row-end", "grid-column-end", "grid-area-name"}},
    {"grid-template", {"grid-template-rows", "grid-template-columns", "grid-template-areas"}},
    {"margin-inline", {"margin-left", "margin-right"}}, {"margin-block", {"margin-top", "margin-bottom"}},
    {"padding-inline", {"padding-left", "padding-right"}}, {"padding-block", {"padding-top", "padding-bottom"}},
    {"inset-inline", {"left", "right"}}, {"inset-block", {"top", "bottom"}},
    {"border-inline", {"border-left-width", "border-left-style", "border-left-color", "border-right-width", "border-right-style", "border-right-color"}},
    {"border-block", {"border-top-width", "border-top-style", "border-top-color", "border-bottom-width", "border-bottom-style", "border-bottom-color"}},
    {"text-wrap", {"text-wrap-mode"}},
    {"white-space-collapse", {"white-space"}},
};

static void buildNameIndex(void) {
    for (size_t i = 0; i < CSCD_COUNT(kCanonical); ++i) cscdStrMapPut(&canonicalIndex, cscdSv(kCanonical[i][0]), NULL)->u = i;
    for (size_t i = 0; i < CSCD_COUNT(kShorthands); ++i) cscdStrMapPut(&shorthandIndex, cscdSv(kShorthands[i].name), NULL)->u = i;
}

static void expandBody(const Shorthand* sh, CscdSv p, const Values* v, CscdStrPairVec* out);

bool cscdIsShorthand(CscdSv prop) {
    cscdCallOnce(&namesOnce, buildNameIndex);
    return cscdStrMapGet(&shorthandIndex, prop) != NULL;
}

bool cscdExpandShorthand(CscdSv prop, CscdSv value, CscdStrPairVec* out) {
    cscdCallOnce(&namesOnce, buildNameIndex);
    const CscdMapVal* found = cscdStrMapGet(&shorthandIndex, prop);
    if (!found) return false;
    const Shorthand* sh = &kShorthands[found->u];
    if (isWideKeyword(value)) {
        for (int i = 0; i < 13 && sh->longhands[i]; ++i) addPairC(out, sh->longhands[i], value);
        return true;
    }
    Values v = {0};
    cscdCssParseValues(value, false, &v);
    expandBody(sh, prop, &v, out);
    cscdCssValueVecFree(&v);
    return true;
}

static void sideList(CscdSv p, const char*** sides, size_t* n) {
    static const char* all[] = {"top", "right", "bottom", "left"};
    static const char* inl[] = {"left", "right"};
    static const char* blk[] = {"top", "bottom"};
    static const char* one1[1];
    if (cscdSvEqC(p, "border")) { *sides = all; *n = 4; }
    else if (cscdSvEqC(p, "border-inline")) { *sides = inl; *n = 2; }
    else if (cscdSvEqC(p, "border-block")) { *sides = blk; *n = 2; }
    else {
        CscdSv side = cscdSvFrom(p, 7);
        for (int i = 0; i < 4; ++i) if (cscdSvEqC(side, all[i])) one1[0] = all[i];
        *sides = one1;
        *n = 1;
    }
}

static void expandBody(const Shorthand* sh, CscdSv p, const Values* v, CscdStrPairVec* out) {
#define P(s) cscdSvEqC(p, s)
#define X(i) (&v->items[i])
    if (P("margin") || P("padding") || P("inset") || P("border-width") || P("border-style") || P("border-color")) {
        if (P("inset")) boxSides(v->items, v->len, "", "", out);
        else if (P("margin")) boxSides(v->items, v->len, "margin-", "", out);
        else if (P("padding")) boxSides(v->items, v->len, "padding-", "", out);
        else {
            CscdStr suffix = cscdStrDup(cscdSvFrom(p, 6)); /* "-width" ... */
            boxSides(v->items, v->len, "border-", cscdStrC(&suffix), out);
            cscdStrFree(&suffix);
        }
        return;
    }
    if (P("margin-inline") || P("margin-block") || P("padding-inline") || P("padding-block") || P("inset-inline") || P("inset-block")) {
        if (!v->len) return;
        CscdStr a = one(X(0)), b = v->len > 1 ? one(X(1)) : cscdStrCopy(&a);
        addPair(out, cscdStrDupC(sh->longhands[0]), a);
        addPair(out, cscdStrDupC(sh->longhands[1]), b);
        return;
    }
    if (P("border-radius")) {
        Values h = {0}, vv = {0};
        bool slash = false;
        for (size_t i = 0; i < v->len; ++i) {
            if (cscdCssIsDelim(X(i), '/')) { slash = true; continue; }
            cscdVecPush(slash ? &vv : &h, *X(i)); /* (shallow: views of v's values) */
        }
        CscdStrPairVec hs = {0}, vs = {0};
        boxSides(h.items, h.len, "", "", &hs);
        if (vv.len) boxSides(vv.items, vv.len, "", "", &vs);
        for (size_t i = 0; i < 4 && i < hs.len; ++i) {
            CscdStr val = cscdStrCopy(&hs.items[i].value);
            if (vs.len) { cscdStrAppendChar(&val, ' '); cscdStrAppend(&val, cscdStrSv(&vs.items[i].value)); }
            addPair(out, cscdStrf("border-%s-radius", kCorners[i]), val);
        }
        cscdStrPairVecFree(&hs);
        cscdStrPairVecFree(&vs);
        cscdVecFree(&h);
        cscdVecFree(&vv);
        return;
    }
    if (P("border") || P("border-top") || P("border-right") || P("border-bottom") || P("border-left") || P("border-inline") || P("border-block")) {
        CscdStr w = cscdStrDupC("medium"), st = cscdStrDupC("none"), col = cscdStrDupC("currentcolor");
        borderParts(v, &w, &st, &col);
        const char** sides;
        size_t n;
        sideList(p, &sides, &n);
        for (size_t i = 0; i < n; ++i) {
            addPair(out, cscdStrf("border-%s-width", sides[i]), cscdStrCopy(&w));
            addPair(out, cscdStrf("border-%s-style", sides[i]), cscdStrCopy(&st));
            addPair(out, cscdStrf("border-%s-color", sides[i]), cscdStrCopy(&col));
        }
        cscdStrFree(&w);
        cscdStrFree(&st);
        cscdStrFree(&col);
        return;
    }
    if (P("outline")) {
        CscdStr w = cscdStrDupC("medium"), st = cscdStrDupC("none"), col = cscdStrDupC("currentcolor");
        for (size_t i = 0; i < v->len; ++i) {
            const CscdCssValue* x = X(i);
            if (isBorderStyle(x) || cscdCssIsIdent(x, "auto")) { cscdStrFree(&st); st = cscdCssIsIdent(x, "auto") ? cscdStrDupC("solid") : one(x); }
            else if (isLengthValue(x)) { cscdStrFree(&w); w = one(x); }
            else { cscdStrFree(&col); col = one(x); }
        }
        addPair(out, cscdStrDupC("outline-width"), w);
        addPair(out, cscdStrDupC("outline-style"), st);
        addPair(out, cscdStrDupC("outline-color"), col);
        return;
    }
    if (P("flex")) {
        if (v->len == 1 && cscdCssIsIdent(X(0), "none")) {
            addPairC(out, "flex-grow", CSCD_SV("0")); addPairC(out, "flex-shrink", CSCD_SV("0")); addPairC(out, "flex-basis", CSCD_SV("auto"));
            return;
        }
        if (v->len == 1 && cscdCssIsIdent(X(0), "auto")) {
            addPairC(out, "flex-grow", CSCD_SV("1")); addPairC(out, "flex-shrink", CSCD_SV("1")); addPairC(out, "flex-basis", CSCD_SV("auto"));
            return;
        }
        if (v->len == 1 && cscdCssIsIdent(X(0), "initial")) {
            addPairC(out, "flex-grow", CSCD_SV("0")); addPairC(out, "flex-shrink", CSCD_SV("1")); addPairC(out, "flex-basis", CSCD_SV("auto"));
            return;
        }
        CscdStr g = cscdStrDupC("1"), s = cscdStrDupC("1"), b = cscdStrDupC("0%");
        int nums = 0;
        for (size_t i = 0; i < v->len; ++i) {
            const CscdCssValue* x = X(i);
            if (x->tok.type == CSCD_TOK_NUMBER && !(nums == 0 && x->tok.num == 0 && v->len == 3 && i == 2)) {
                if (nums == 0) { cscdStrFree(&g); g = one(x); }
                else { cscdStrFree(&s); s = one(x); }
                ++nums;
            } else { cscdStrFree(&b); b = one(x); }
        }
        addPair(out, cscdStrDupC("flex-grow"), g);
        addPair(out, cscdStrDupC("flex-shrink"), s);
        addPair(out, cscdStrDupC("flex-basis"), b);
        return;
    }
    if (P("flex-flow")) {
        for (size_t i = 0; i < v->len; ++i) {
            const CscdCssValue* x = X(i);
            if (cscdCssIsIdent(x, "wrap") || cscdCssIsIdent(x, "nowrap") || cscdCssIsIdent(x, "wrap-reverse")) addPair(out, cscdStrDupC("flex-wrap"), one(x));
            else addPair(out, cscdStrDupC("flex-direction"), one(x));
        }
        return;
    }
    if (P("gap") || P("overflow") || P("place-items") || P("place-content") || P("place-self")) {
        if (!v->len) return;
        /* (place-*: the first value may be two tokens ("safe center"); kept simple: one token each) */
        const char* a = P("gap") ? "row-gap" : P("overflow") ? "overflow-x" : sh->longhands[0];
        const char* b = P("gap") ? "column-gap" : P("overflow") ? "overflow-y" : sh->longhands[1];
        addPair(out, cscdStrDupC(a), one(X(0)));
        addPair(out, cscdStrDupC(b), one(v->len > 1 ? X(1) : X(0)));
        return;
    }
    if (P("text-decoration")) {
        CscdStr line = {0}, style = cscdStrDupC("solid"), color = cscdStrDupC("currentcolor"), thick = cscdStrDupC("auto");
        for (size_t i = 0; i < v->len; ++i) {
            const CscdCssValue* x = X(i);
            if (cscdCssIsIdent(x, "underline") || cscdCssIsIdent(x, "overline") || cscdCssIsIdent(x, "line-through") || cscdCssIsIdent(x, "none") || cscdCssIsIdent(x, "blink")) {
                if (line.len) cscdStrAppendChar(&line, ' ');
                CscdStr o = one(x);
                cscdStrAppend(&line, cscdStrSv(&o));
                cscdStrFree(&o);
            } else if (cscdCssIsIdent(x, "solid") || cscdCssIsIdent(x, "double") || cscdCssIsIdent(x, "dotted") || cscdCssIsIdent(x, "dashed") || cscdCssIsIdent(x, "wavy")) {
                cscdStrFree(&style);
                style = one(x);
            } else if (isLengthValue(x) || cscdCssIsIdent(x, "from-font") || cscdCssIsIdent(x, "auto")) {
                cscdStrFree(&thick);
                thick = one(x);
            } else {
                cscdStrFree(&color);
                color = one(x);
            }
        }
        if (!line.len) cscdStrSetC(&line, "none");
        addPair(out, cscdStrDupC("text-decoration-line"), line);
        addPair(out, cscdStrDupC("text-decoration-style"), style);
        addPair(out, cscdStrDupC("text-decoration-color"), color);
        addPair(out, cscdStrDupC("text-decoration-thickness"), thick);
        return;
    }
    if (P("list-style")) {
        CscdStr type = cscdStrDupC("disc"), pos = cscdStrDupC("outside"), img = cscdStrDupC("none");
        int nones = 0;
        for (size_t i = 0; i < v->len; ++i) {
            const CscdCssValue* x = X(i);
            if (cscdCssIsIdent(x, "inside") || cscdCssIsIdent(x, "outside")) { cscdStrFree(&pos); pos = one(x); }
            else if (x->tok.type == CSCD_TOK_URL || cscdCssIsFunction(x, "url")) { cscdStrFree(&img); img = one(x); }
            else if (cscdCssIsIdent(x, "none")) ++nones;
            else { cscdStrFree(&type); type = one(x); }
        }
        if (nones) cscdStrSetC(&type, "none");
        addPair(out, cscdStrDupC("list-style-type"), type);
        addPair(out, cscdStrDupC("list-style-position"), pos);
        addPair(out, cscdStrDupC("list-style-image"), img);
        return;
    }
    if (P("grid-row") || P("grid-column")) {
        size_t slash = v->len;
        for (size_t k = 0; k < v->len; ++k) if (cscdCssIsDelim(X(k), '/')) { slash = k; break; }
        CscdStr a = ser(v, 0, slash);
        CscdStr b;
        if (slash < v->len) b = ser(v, slash + 1, v->len);
        else if (v->len == 1 && X(0)->tok.type == CSCD_TOK_IDENT && !cscdCssIsIdent(X(0), "auto")) b = cscdStrCopy(&a);
        else b = cscdStrDupC("auto");
        addPair(out, cscdStrDupC(sh->longhands[0]), a);
        addPair(out, cscdStrDupC(sh->longhands[1]), b);
        return;
    }
    if (P("grid-area")) {
        CscdStrVec parts = {0};
        size_t start = 0;
        for (size_t k = 0; k <= v->len; ++k)
            if (k == v->len || cscdCssIsDelim(X(k), '/')) { cscdVecPush(&parts, ser(v, start, k)); start = k + 1; }
        if (parts.len == 1 && v->len == 1 && X(0)->tok.type == CSCD_TOK_IDENT && !cscdCssIsIdent(X(0), "auto")) {
            addPair(out, cscdStrDupC("grid-area-name"), cscdStrCopy(&parts.items[0]));
            cscdStrVecFree(&parts);
            return;
        }
        while (parts.len < 4) cscdVecPush(&parts, cscdStrDupC("auto"));
        addPair(out, cscdStrDupC("grid-row-start"), cscdStrCopy(&parts.items[0]));
        addPair(out, cscdStrDupC("grid-column-start"), cscdStrCopy(&parts.items[1]));
        addPair(out, cscdStrDupC("grid-row-end"), cscdStrCopy(&parts.items[2]));
        addPair(out, cscdStrDupC("grid-column-end"), cscdStrCopy(&parts.items[3]));
        cscdStrVecFree(&parts);
        return;
    }
    if (P("grid-template")) {
        size_t slash = v->len;
        for (size_t k = 0; k < v->len; ++k) if (cscdCssIsDelim(X(k), '/')) { slash = k; break; }
        /* areas form: strings with row sizes */
        bool strings = false;
        for (size_t k = 0; k < v->len; ++k) strings = strings || X(k)->tok.type == CSCD_TOK_STRING;
        if (strings) {
            CscdStr areas = {0}, rows = {0};
            for (size_t k = 0; k < slash; ++k) {
                if (X(k)->tok.type == CSCD_TOK_STRING) {
                    cscdStrAppendChar(&areas, '"');
                    cscdStrAppend(&areas, cscdStrSv(&X(k)->tok.text));
                    cscdStrAppendC(&areas, "\" ");
                } else {
                    CscdStr o = one(X(k));
                    cscdStrAppend(&rows, cscdStrSv(&o));
                    cscdStrAppendChar(&rows, ' ');
                    cscdStrFree(&o);
                }
            }
            addPair(out, cscdStrDupC("grid-template-areas"), areas);
            if (rows.len) addPair(out, cscdStrDupC("grid-template-rows"), rows);
            else cscdStrFree(&rows);
        } else addPair(out, cscdStrDupC("grid-template-rows"), ser(v, 0, slash));
        if (slash < v->len) addPair(out, cscdStrDupC("grid-template-columns"), ser(v, slash + 1, v->len));
        return;
    }
    if (P("text-wrap")) {
        for (size_t k = 0; k < v->len; ++k) if (cscdCssIsIdent(X(k), "nowrap")) addPairC(out, "white-space", CSCD_SV("nowrap"));
        return;
    }
    if (P("white-space-collapse")) {
        if (v->len && cscdCssIsIdent(X(0), "preserve")) addPairC(out, "white-space", CSCD_SV("pre-wrap"));
        return;
    }
    if (P("font")) {
        /* system fonts */
        if (v->len == 1 && X(0)->tok.type == CSCD_TOK_IDENT) {
            CscdSv id = cscdStrSv(&X(0)->tok.text);
            if (cscdSvIEqC(id, "caption") || cscdSvIEqC(id, "icon") || cscdSvIEqC(id, "menu") || cscdSvIEqC(id, "message-box") ||
                cscdSvIEqC(id, "small-caption") || cscdSvIEqC(id, "status-bar")) {
                bool small = cscdSvIEqC(id, "small-caption") || cscdSvIEqC(id, "status-bar");
                addPairC(out, "font-style", CSCD_SV("normal"));
                addPairC(out, "font-variant", CSCD_SV("normal"));
                addPairC(out, "font-weight", CSCD_SV("400"));
                addPairC(out, "font-size", small ? CSCD_SV("12px") : CSCD_SV("13.333px"));
                addPairC(out, "line-height", CSCD_SV("normal"));
                addPairC(out, "font-family", CSCD_SV("system-ui"));
                return;
            }
        }
        CscdStr style = cscdStrDupC("normal"), variant = cscdStrDupC("normal"), weight = cscdStrDupC("normal"), size = {0}, lh = cscdStrDupC("normal");
        CscdStr stretch = cscdStrDupC("normal");
        size_t k = 0;
        for (; k < v->len; ++k) {
            const CscdCssValue* x = X(k);
            if (cscdCssIsIdent(x, "italic") || cscdCssIsIdent(x, "oblique")) { cscdStrFree(&style); style = one(x); }
            else if (cscdCssIsIdent(x, "small-caps")) { cscdStrFree(&variant); variant = one(x); }
            else if (cscdCssIsIdent(x, "bold") || cscdCssIsIdent(x, "bolder") || cscdCssIsIdent(x, "lighter") ||
                     (x->tok.type == CSCD_TOK_NUMBER && x->tok.num >= 1 && x->tok.num <= 1000)) { cscdStrFree(&weight); weight = one(x); }
            else if (cscdCssIsIdent(x, "ultra-condensed") || cscdCssIsIdent(x, "extra-condensed") || cscdCssIsIdent(x, "condensed") ||
                     cscdCssIsIdent(x, "semi-condensed") || cscdCssIsIdent(x, "semi-expanded") || cscdCssIsIdent(x, "expanded") ||
                     cscdCssIsIdent(x, "extra-expanded") || cscdCssIsIdent(x, "ultra-expanded")) { cscdStrFree(&stretch); stretch = one(x); }
            else if (cscdCssIsIdent(x, "normal")) {}
            else break;
        }
        if (k >= v->len) {
            cscdStrFree(&style); cscdStrFree(&variant); cscdStrFree(&weight); cscdStrFree(&lh); cscdStrFree(&stretch);
            return;
        }
        size = one(X(k++));
        if (k < v->len && cscdCssIsDelim(X(k), '/')) { ++k; if (k < v->len) { cscdStrFree(&lh); lh = one(X(k++)); } }
        CscdStr family = {0};
        for (; k < v->len; ++k) {
            if (cscdCssIsComma(X(k))) cscdStrAppendChar(&family, ',');
            else {
                if (family.len && family.ptr[family.len - 1] != ',') cscdStrAppendChar(&family, ' ');
                CscdStr o = one(X(k));
                cscdStrAppend(&family, cscdStrSv(&o));
                cscdStrFree(&o);
            }
        }
        addPair(out, cscdStrDupC("font-style"), style);
        addPair(out, cscdStrDupC("font-variant"), variant);
        addPair(out, cscdStrDupC("font-weight"), weight);
        addPair(out, cscdStrDupC("font-size"), size);
        addPair(out, cscdStrDupC("line-height"), lh);
        addPair(out, cscdStrDupC("font-family"), family);
        addPair(out, cscdStrDupC("font-stretch"), stretch);
        return;
    }
    if (P("background")) {
        CscdCssSlices layers = {0};
        cscdCssSplitCommas(v, &layers);
        CscdStr imgs = {0}, poss = {0}, sizes = {0}, reps = {0}, clips = {0}, origins = {0}, color = cscdStrDupC("transparent");
        for (size_t li = 0; li < layers.len; ++li) {
            const Values* L = &layers.items[li];
            CscdStr img = cscdStrDupC("none"), pos = cscdStrDupC("0% 0%"), size = cscdStrDupC("auto"), rep = cscdStrDupC("repeat");
            CscdStr clip = cscdStrDupC("border-box"), origin = cscdStrDupC("padding-box"), posParts = {0};
            int boxes = 0;
            for (size_t k = 0; k < L->len; ++k) {
                const CscdCssValue* x = &L->items[k];
                if (x->tok.type == CSCD_TOK_URL || cscdCssIsFunction(x, "url") || cscdCssIsFunction(x, "linear-gradient") || cscdCssIsFunction(x, "radial-gradient") ||
                    cscdCssIsFunction(x, "repeating-linear-gradient") || cscdCssIsFunction(x, "repeating-radial-gradient") || cscdCssIsFunction(x, "conic-gradient") ||
                    cscdCssIsFunction(x, "-webkit-linear-gradient") || cscdCssIsFunction(x, "image-set") || cscdCssIsIdent(x, "none")) {
                    cscdStrFree(&img);
                    img = one(x);
                } else if (cscdCssIsIdent(x, "repeat") || cscdCssIsIdent(x, "no-repeat") || cscdCssIsIdent(x, "repeat-x") || cscdCssIsIdent(x, "repeat-y") ||
                           cscdCssIsIdent(x, "space") || cscdCssIsIdent(x, "round")) {
                    CscdStr o = one(x);
                    if (cscdStrEqC(&rep, "repeat") || k == 0) { cscdStrFree(&rep); rep = o; }
                    else { cscdStrAppendChar(&rep, ' '); cscdStrAppend(&rep, cscdStrSv(&o)); cscdStrFree(&o); }
                } else if (cscdCssIsIdent(x, "border-box") || cscdCssIsIdent(x, "padding-box") || cscdCssIsIdent(x, "content-box") || cscdCssIsIdent(x, "text")) {
                    if (boxes == 0) { cscdStrFree(&origin); origin = one(x); cscdStrFree(&clip); clip = one(x); }
                    else { cscdStrFree(&clip); clip = one(x); }
                    ++boxes;
                } else if (cscdCssIsIdent(x, "fixed") || cscdCssIsIdent(x, "scroll") || cscdCssIsIdent(x, "local")) {
                } else if (cscdCssIsDelim(x, '/')) {
                    cscdStrClear(&size);
                    while (k + 1 < L->len && (isLengthValue(&L->items[k + 1]) || cscdCssIsIdent(&L->items[k + 1], "auto") ||
                                              cscdCssIsIdent(&L->items[k + 1], "cover") || cscdCssIsIdent(&L->items[k + 1], "contain"))) {
                        if (size.len) cscdStrAppendChar(&size, ' ');
                        CscdStr o = one(&L->items[++k]);
                        cscdStrAppend(&size, cscdStrSv(&o));
                        cscdStrFree(&o);
                    }
                    if (!size.len) cscdStrSetC(&size, "auto");
                } else if (isLengthValue(x) || cscdCssIsIdent(x, "center") || cscdCssIsIdent(x, "left") || cscdCssIsIdent(x, "right") ||
                           cscdCssIsIdent(x, "top") || cscdCssIsIdent(x, "bottom")) {
                    if (posParts.len) cscdStrAppendChar(&posParts, ' ');
                    CscdStr o = one(x);
                    cscdStrAppend(&posParts, cscdStrSv(&o));
                    cscdStrFree(&o);
                } else if (li == layers.len - 1 && isColorValue(x)) {
                    cscdStrFree(&color);
                    color = one(x);
                }
            }
            if (posParts.len) cscdStrSet(&pos, cscdStrSv(&posParts));
#define ADD(acc, s) do { if (li) cscdStrAppendC(&(acc), ", "); cscdStrAppend(&(acc), cscdStrSv(&(s))); } while (0)
            ADD(imgs, img); ADD(poss, pos); ADD(sizes, size); ADD(reps, rep); ADD(clips, clip); ADD(origins, origin);
#undef ADD
            cscdStrFree(&img); cscdStrFree(&pos); cscdStrFree(&size); cscdStrFree(&rep); cscdStrFree(&clip); cscdStrFree(&origin); cscdStrFree(&posParts);
        }
        cscdVecFree(&layers);
        addPair(out, cscdStrDupC("background-color"), color);
        addPair(out, cscdStrDupC("background-image"), imgs);
        addPair(out, cscdStrDupC("background-position"), poss);
        addPair(out, cscdStrDupC("background-size"), sizes);
        addPair(out, cscdStrDupC("background-repeat"), reps);
        addPair(out, cscdStrDupC("background-clip"), clips);
        addPair(out, cscdStrDupC("background-origin"), origins);
        return;
    }
#undef P
#undef X
}

/* ---- longhands ------------------------------------------------------------------------------------- */

typedef bool (*Handler)(CscdApplyState* st, const Values* v, CscdSv raw);

#define S (st->s)
#define V(i) (&v->items[i])
#define ID(i, s) cscdCssIsIdent(V(i), s)
#define HANDLER(name) static bool name(CscdApplyState* st, const Values* v, CscdSv raw)
#define UNUSED_ARGS (void)st, (void)v, (void)raw

static bool lengthOf(CscdApplyState* st, const Values* v, CscdLength* out, bool keywords, bool allowNegative) {
    if (v->len != 1) return false;
    CscdLength l;
    if (!cscdParseLength(V(0), &st->ctx, &l, keywords)) return false;
    if (!allowNegative && l.kind == CSCD_LEN_FIXED && (l.px < 0 || l.pct < 0)) return false;
    *out = l;
    return true;
}

static bool colorOf(CscdApplyState* st, const Values* v, CscdColor* out) {
    size_t i = 0;
    CscdColor c;
    if (!cscdParseColor(v, &i, &c, S->color) || i != v->len) return false;
    *out = c;
    return true;
}

static CscdAlign parseAlign(const Values* v, bool* ok) {
    *ok = true;
    /* skip overflow positions */
    for (size_t i = 0; i < v->len; ++i) {
        const CscdCssValue* x = V(i);
        if (cscdCssIsIdent(x, "safe") || cscdCssIsIdent(x, "unsafe") || cscdCssIsIdent(x, "first") || cscdCssIsIdent(x, "legacy")) continue;
        if (x->tok.type != CSCD_TOK_IDENT) break;
        CscdSv id = cscdStrSv(&x->tok.text);
        static const struct { const char* n; CscdAlign a; } m[] = {
            {"auto", CSCD_AL_AUTO}, {"normal", CSCD_AL_NORMAL}, {"stretch", CSCD_AL_STRETCH}, {"start", CSCD_AL_START}, {"end", CSCD_AL_END},
            {"flex-start", CSCD_AL_FLEX_START}, {"flex-end", CSCD_AL_FLEX_END}, {"center", CSCD_AL_CENTER}, {"baseline", CSCD_AL_BASELINE},
            {"last", CSCD_AL_LAST_BASELINE}, {"space-between", CSCD_AL_SPACE_BETWEEN}, {"space-around", CSCD_AL_SPACE_AROUND},
            {"space-evenly", CSCD_AL_SPACE_EVENLY}, {"left", CSCD_AL_LEFT}, {"right", CSCD_AL_RIGHT}, {"self-start", CSCD_AL_SELF_START},
            {"self-end", CSCD_AL_SELF_END}};
        for (size_t k = 0; k < CSCD_COUNT(m); ++k)
            if (cscdSvIEqC(id, m[k].n)) return m[k].a;
        break;
    }
    *ok = false;
    return CSCD_AL_NORMAL;
}

static bool parseGridLine(const Values* v, CscdGridLine* out) {
    CscdGridLine g = {0, 0, {0}};
    for (size_t i = 0; i < v->len; ++i) {
        const CscdCssValue* x = V(i);
        if (cscdCssIsIdent(x, "auto")) continue;
        if (cscdCssIsIdent(x, "span")) { g.span = cscdMaxi(g.span, 1); continue; }
        if (x->tok.type == CSCD_TOK_NUMBER) {
            if (g.span) g.span = cscdIntOfDouble(x->tok.num);
            else g.line = cscdIntOfDouble(x->tok.num);
        } else if (x->tok.type == CSCD_TOK_IDENT) cscdStrSet(&g.name, cscdStrSv(&x->tok.text));
    }
    cscdStrFree(&out->name);
    *out = g;
    return true;
}

static CscdCursor parseCursor(const Values* v) {
    static const struct { const char* n; CscdCursor c; } m[] = {
        {"auto", CSCD_CURSOR_AUTO}, {"default", CSCD_CURSOR_DEFAULT}, {"pointer", CSCD_CURSOR_POINTER}, {"text", CSCD_CURSOR_TEXT},
        {"vertical-text", CSCD_CURSOR_TEXT}, {"move", CSCD_CURSOR_MOVE}, {"all-scroll", CSCD_CURSOR_MOVE}, {"not-allowed", CSCD_CURSOR_NOT_ALLOWED},
        {"no-drop", CSCD_CURSOR_NOT_ALLOWED}, {"grab", CSCD_CURSOR_GRAB}, {"grabbing", CSCD_CURSOR_GRABBING}, {"crosshair", CSCD_CURSOR_CROSSHAIR},
        {"wait", CSCD_CURSOR_WAIT}, {"progress", CSCD_CURSOR_PROGRESS}, {"help", CSCD_CURSOR_HELP}, {"ew-resize", CSCD_CURSOR_EW_RESIZE},
        {"e-resize", CSCD_CURSOR_EW_RESIZE}, {"w-resize", CSCD_CURSOR_EW_RESIZE}, {"ns-resize", CSCD_CURSOR_NS_RESIZE}, {"n-resize", CSCD_CURSOR_NS_RESIZE},
        {"s-resize", CSCD_CURSOR_NS_RESIZE}, {"nesw-resize", CSCD_CURSOR_NESW_RESIZE}, {"ne-resize", CSCD_CURSOR_NESW_RESIZE},
        {"sw-resize", CSCD_CURSOR_NESW_RESIZE}, {"nwse-resize", CSCD_CURSOR_NWSE_RESIZE}, {"nw-resize", CSCD_CURSOR_NWSE_RESIZE},
        {"se-resize", CSCD_CURSOR_NWSE_RESIZE}, {"col-resize", CSCD_CURSOR_COL_RESIZE}, {"row-resize", CSCD_CURSOR_ROW_RESIZE},
        {"none", CSCD_CURSOR_NONE}, {"copy", CSCD_CURSOR_COPY}, {"alias", CSCD_CURSOR_COPY}, {"context-menu", CSCD_CURSOR_DEFAULT},
        {"cell", CSCD_CURSOR_CROSSHAIR}, {"zoom-in", CSCD_CURSOR_POINTER}, {"zoom-out", CSCD_CURSOR_POINTER}};
    for (size_t i = 0; i < v->len; ++i) {
        const CscdCssValue* x = V(i);
        if (x->tok.type != CSCD_TOK_IDENT) continue; /* url() fallbacks */
        for (size_t k = 0; k < CSCD_COUNT(m); ++k)
            if (cscdSvIEqC(cscdStrSv(&x->tok.text), m[k].n)) return m[k].c;
    }
    return CSCD_CURSOR_AUTO;
}

typedef struct TfArgs { const CscdCssValue* a[16]; size_t n; CscdApplyState* st; } TfArgs;
static bool tfLen(const TfArgs* t, size_t i, CscdLength* l) { return i < t->n && cscdParseLength(t->a[i], &t->st->ctx, l, false); }
static float tfNum(const TfArgs* t, size_t i, float def) {
    float r = def;
    if (i < t->n) {
        if (t->a[i]->tok.type == CSCD_TOK_PERCENTAGE) r = (float)t->a[i]->tok.num / 100.f;
        else cscdParseNumber(t->a[i], &r, &t->st->ctx);
    }
    return r;
}
static float tfAng(const TfArgs* t, size_t i) { float r = 0; if (i < t->n) cscdParseAngle(t->a[i], &r); return r; }
static float tfPx(const TfArgs* t, size_t i) { return i < t->n ? cscdLengthPx(t->a[i], &t->st->ctx, 0) : 0.f; }

static bool parseTransform(CscdApplyState* st, const Values* v, CscdTransformVec* out) {
    cscdVecClear(out);
    if (v->len == 1 && ID(0, "none")) return true;
    for (size_t fi = 0; fi < v->len; ++fi) {
        const CscdCssValue* f = V(fi);
        if (f->tok.type != CSCD_TOK_FUNCTION) return false;
        TfArgs args;
        args.n = 0;
        args.st = st;
        for (size_t k = 0; k < f->args.len; ++k)
            if (!cscdCssIsComma(&f->args.items[k]) && args.n < 16) args.a[args.n++] = &f->args.items[k];
        CscdSv n = cscdStrSv(&f->tok.text);
        CscdTransformOp op = cscdTransformOp(CSCD_TF_TRANSLATE);
        if (cscdSvEqC(n, "translate") || cscdSvEqC(n, "translatex") || cscdSvEqC(n, "translatey") || cscdSvEqC(n, "translate3d")) {
            op.kind = CSCD_TF_TRANSLATE;
            if (cscdSvEqC(n, "translatey")) tfLen(&args, 0, &op.y);
            else { tfLen(&args, 0, &op.x); if (!cscdSvEqC(n, "translatex")) tfLen(&args, 1, &op.y); }
            if (cscdSvEqC(n, "translate3d") && args.n > 2) {
                cscdVecPush(out, op);
                op = cscdTransformOp(CSCD_TF_TRANSLATE_Z);
                op.z = tfPx(&args, 2);
            }
        } else if (cscdSvEqC(n, "translatez")) {
            op.kind = CSCD_TF_TRANSLATE_Z;
            op.z = tfPx(&args, 0);
        } else if (cscdSvEqC(n, "scale") || cscdSvEqC(n, "scalex") || cscdSvEqC(n, "scaley") || cscdSvEqC(n, "scale3d")) {
            op.kind = CSCD_TF_SCALE;
            if (cscdSvEqC(n, "scalex")) { op.a = tfNum(&args, 0, 1); op.b = 1; }
            else if (cscdSvEqC(n, "scaley")) { op.a = 1; op.b = tfNum(&args, 0, 1); }
            else { op.a = tfNum(&args, 0, 1); op.b = args.n > 1 ? tfNum(&args, 1, 1) : op.a; }
            if (cscdSvEqC(n, "scale3d") && args.n > 2) {
                cscdVecPush(out, op);
                op = cscdTransformOp(CSCD_TF_SCALE_Z);
                op.a = tfNum(&args, 2, 1);
            }
        } else if (cscdSvEqC(n, "scalez")) {
            op.kind = CSCD_TF_SCALE_Z;
            op.a = tfNum(&args, 0, 1);
        } else if (cscdSvEqC(n, "rotatex") || cscdSvEqC(n, "rotatey") || cscdSvEqC(n, "rotate3d")) {
            op.kind = CSCD_TF_ROTATE3D;
            if (cscdSvEqC(n, "rotatex")) { op.x3 = 1; op.y3 = 0; op.z3 = 0; op.a = tfAng(&args, 0); }
            else if (cscdSvEqC(n, "rotatey")) { op.x3 = 0; op.y3 = 1; op.z3 = 0; op.a = tfAng(&args, 0); }
            else { op.x3 = tfNum(&args, 0, 0); op.y3 = tfNum(&args, 1, 0); op.z3 = tfNum(&args, 2, 1); op.a = tfAng(&args, 3); }
        } else if (cscdSvEqC(n, "perspective")) {
            op.kind = CSCD_TF_PERSPECTIVE;
            op.z = !args.n || cscdCssIsIdent(args.a[0], "none") ? 0 : tfPx(&args, 0);
        } else if (cscdSvEqC(n, "rotate") || cscdSvEqC(n, "rotatez")) {
            op.kind = CSCD_TF_ROTATE;
            op.a = tfAng(&args, 0);
        } else if (cscdSvEqC(n, "skew") || cscdSvEqC(n, "skewx") || cscdSvEqC(n, "skewy")) {
            op.kind = CSCD_TF_SKEW;
            if (cscdSvEqC(n, "skewy")) op.b = tfAng(&args, 0);
            else { op.a = tfAng(&args, 0); if (cscdSvEqC(n, "skew")) op.b = tfAng(&args, 1); }
        } else if (cscdSvEqC(n, "matrix")) {
            op.kind = CSCD_TF_MATRIX;
            op.m = cscdMatrix(tfNum(&args, 0, 1), tfNum(&args, 1, 0), tfNum(&args, 2, 0), tfNum(&args, 3, 1), tfNum(&args, 4, 0), tfNum(&args, 5, 0));
        } else if (cscdSvEqC(n, "matrix3d") && args.n == 16) {
            op.kind = CSCD_TF_MATRIX3D;
            for (int i = 0; i < 16; ++i) op.m4[i] = tfNum(&args, (size_t)i, (i % 5 == 0) ? 1.f : 0.f);
        } else return false;
        cscdVecPush(out, op);
    }
    return true;
}

static CscdListStyle parseListStyle(CscdSv id) {
    if (cscdSvIEqC(id, "none")) return CSCD_LIST_NONE;
    if (cscdSvIEqC(id, "circle")) return CSCD_LIST_CIRCLE;
    if (cscdSvIEqC(id, "square")) return CSCD_LIST_SQUARE;
    if (cscdSvIEqC(id, "decimal") || cscdSvIEqC(id, "decimal-leading-zero")) return CSCD_LIST_DECIMAL;
    if (cscdSvIEqC(id, "lower-alpha") || cscdSvIEqC(id, "lower-latin")) return CSCD_LIST_LOWER_ALPHA;
    if (cscdSvIEqC(id, "upper-alpha") || cscdSvIEqC(id, "upper-latin")) return CSCD_LIST_UPPER_ALPHA;
    if (cscdSvIEqC(id, "lower-roman")) return CSCD_LIST_LOWER_ROMAN;
    if (cscdSvIEqC(id, "upper-roman")) return CSCD_LIST_UPPER_ROMAN;
    return CSCD_LIST_DISC;
}

static bool parseBorderWidth(CscdApplyState* st, const Values* v, float* out) {
    if (v->len != 1) return false;
    if (ID(0, "thin")) { *out = 1; return true; }
    if (ID(0, "medium")) { *out = 3; return true; }
    if (ID(0, "thick")) { *out = 5; return true; }
    CscdLength l;
    if (!cscdParseLength(V(0), &st->ctx, &l, false) || cscdLenHasPercent(l)) return false;
    *out = cscdMaxf(0.f, l.px);
    /* browsers snap border widths to device pixels, keeping at least one */
    if (*out > 0 && *out < 1) *out = 1;
    return true;
}

static CscdBorderStyle borderStyleOf(const CscdCssValue* x, bool* ok) {
    *ok = true;
    static const struct { const char* n; CscdBorderStyle s; } m[] = {
        {"none", CSCD_BORDER_NONE}, {"hidden", CSCD_BORDER_HIDDEN}, {"solid", CSCD_BORDER_SOLID}, {"dashed", CSCD_BORDER_DASHED},
        {"dotted", CSCD_BORDER_DOTTED}, {"double", CSCD_BORDER_DOUBLE}, {"groove", CSCD_BORDER_GROOVE}, {"ridge", CSCD_BORDER_RIDGE},
        {"inset", CSCD_BORDER_INSET}, {"outset", CSCD_BORDER_OUTSET}};
    for (size_t i = 0; i < CSCD_COUNT(m); ++i) if (cscdCssIsIdent(x, m[i].n)) return m[i].s;
    *ok = false;
    return CSCD_BORDER_NONE;
}

/* counter-reset, counter-set, counter-increment: name [integer] ... | none */
static bool parseCounters(const Values* v, int def, CscdCounterVec* out) {
    for (size_t i = 0; i < out->len; ++i) cscdStrFree(&out->items[i].name);
    out->len = 0;
    for (size_t i = 0; i < v->len; ++i) {
        if (V(i)->tok.type != CSCD_TOK_IDENT || ID(i, "none")) continue;
        CscdCounter c = {cscdStrCopy(&V(i)->tok.text), def};
        if (i + 1 < v->len && V(i + 1)->tok.type == CSCD_TOK_NUMBER) c.value = cscdIntOfDouble(V(++i)->tok.num);
        cscdVecPush(out, c);
    }
    return true;
}

static void contentString(CscdApplyState* st, const Values* v, bool* ok, CscdStr* out, CscdStr* image) {
    *ok = true;
    cscdStrClear(out);
    if (v->len == 1 && (ID(0, "none") || ID(0, "normal"))) { *ok = false; return; }
    for (size_t k = 0; k < v->len; ++k) {
        const CscdCssValue* x = V(k);
        if (cscdCssIsDelim(x, '/')) break; /* alt text */
        if (x->tok.type == CSCD_TOK_STRING) cscdStrAppend(out, cscdStrSv(&x->tok.text));
        else if (cscdCssIsFunction(x, "attr") && x->args.len) {
            CscdStr l = cscdLower(cscdStrSv(&x->args.items[0].tok.text));
            cscdStrAppendC(out, "\x01" "attr:");
            cscdStrAppend(out, cscdStrSv(&l));
            cscdStrAppendChar(out, '\x02');
            cscdStrFree(&l);
        } else if ((cscdCssIsFunction(x, "counter") || cscdCssIsFunction(x, "counters")) && x->args.len) {
            /* resolved while the boxes are made: "\x01counter:name:style\x02", "\x01counters:name:style:separator\x02" */
            const CscdCssValue* a[8];
            size_t na = 0;
            for (size_t q = 0; q < x->args.len && na < 8; ++q) if (!cscdCssIsComma(&x->args.items[q])) a[na++] = &x->args.items[q];
            if (!na) continue; /* (only commas: no counter is named) */
            const bool all = cscdCssIsFunction(x, "counters");
            CscdSv sep = all && na > 1 && a[1]->tok.type == CSCD_TOK_STRING ? cscdStrSv(&a[1]->tok.text) : CSCD_SV("");
            const size_t styleAt = all ? 2 : 1;
            CscdStr style = na > styleAt ? cscdLower(cscdStrSv(&a[styleAt]->tok.text)) : cscdStrDupC("decimal");
            cscdStrAppendC(out, all ? "\x01" "counters:" : "\x01" "counter:");
            cscdStrAppend(out, cscdStrSv(&a[0]->tok.text));
            cscdStrAppendChar(out, ':');
            cscdStrAppend(out, cscdStrSv(&style));
            if (all) { cscdStrAppendChar(out, ':'); cscdStrAppend(out, sep); }
            cscdStrAppendChar(out, '\x02');
            cscdStrFree(&style);
        } else if (cscdCssIsIdent(x, "open-quote") || cscdCssIsIdent(x, "close-quote") || cscdCssIsIdent(x, "no-open-quote") ||
                   cscdCssIsIdent(x, "no-close-quote")) {
            /* resolved while the boxes are made, by the quotes in force and how deep they are */
            cscdStrAppendChar(out, '\x01');
            cscdStrAppend(out, cscdStrSv(&x->tok.text));
            cscdStrAppendChar(out, '\x02');
        }
        else if (x->tok.type == CSCD_TOK_URL) { cscdStrFree(image); *image = cscdJoinPath(st->baseDir, cscdStrSv(&x->tok.text)); }
        else if (cscdCssIsFunction(x, "url") && x->args.len) { cscdStrFree(image); *image = cscdJoinPath(st->baseDir, cscdStrSv(&x->args.items[0].tok.text)); }
    }
}

/* each handler: one longhand */

HANDLER(hDisplay) {
    UNUSED_ARGS;
    static const struct { const char* n; CscdDisplay d; } d[] = {
        {"none", CSCD_DISPLAY_NONE}, {"inline", CSCD_DISPLAY_INLINE}, {"block", CSCD_DISPLAY_BLOCK}, {"inline-block", CSCD_DISPLAY_INLINE_BLOCK},
        {"flex", CSCD_DISPLAY_FLEX}, {"inline-flex", CSCD_DISPLAY_INLINE_FLEX}, {"grid", CSCD_DISPLAY_GRID}, {"inline-grid", CSCD_DISPLAY_INLINE_GRID},
        {"list-item", CSCD_DISPLAY_LIST_ITEM}, {"table", CSCD_DISPLAY_TABLE}, {"inline-table", CSCD_DISPLAY_INLINE_TABLE},
        {"table-row", CSCD_DISPLAY_TABLE_ROW}, {"table-cell", CSCD_DISPLAY_TABLE_CELL}, {"table-row-group", CSCD_DISPLAY_TABLE_ROW_GROUP},
        {"table-header-group", CSCD_DISPLAY_TABLE_HEADER_GROUP}, {"table-footer-group", CSCD_DISPLAY_TABLE_FOOTER_GROUP},
        {"table-column", CSCD_DISPLAY_TABLE_COLUMN}, {"table-column-group", CSCD_DISPLAY_TABLE_COLUMN_GROUP},
        {"table-caption", CSCD_DISPLAY_TABLE_CAPTION}, {"contents", CSCD_DISPLAY_CONTENTS}, {"flow-root", CSCD_DISPLAY_FLOW_ROOT},
        {"-webkit-box", CSCD_DISPLAY_BLOCK}, {"-webkit-flex", CSCD_DISPLAY_FLEX}, {"-webkit-inline-box", CSCD_DISPLAY_INLINE_BLOCK},
        {"ruby", CSCD_DISPLAY_INLINE}, {"ruby-text", CSCD_DISPLAY_INLINE}};
    if (!v->len) return false;
    if (v->len == 2) {
        bool inl = ID(0, "inline") || ID(1, "inline");
        const CscdCssValue* inner = ID(0, "inline") || ID(0, "block") ? V(1) : V(0);
        if (cscdCssIsIdent(inner, "flex")) S->display = inl ? CSCD_DISPLAY_INLINE_FLEX : CSCD_DISPLAY_FLEX;
        else if (cscdCssIsIdent(inner, "grid")) S->display = inl ? CSCD_DISPLAY_INLINE_GRID : CSCD_DISPLAY_GRID;
        else if (cscdCssIsIdent(inner, "flow-root")) S->display = inl ? CSCD_DISPLAY_INLINE_BLOCK : CSCD_DISPLAY_FLOW_ROOT;
        else if (cscdCssIsIdent(inner, "flow")) S->display = inl ? CSCD_DISPLAY_INLINE : CSCD_DISPLAY_BLOCK;
        else if (cscdCssIsIdent(inner, "table")) S->display = inl ? CSCD_DISPLAY_INLINE_TABLE : CSCD_DISPLAY_TABLE;
        else if (cscdCssIsIdent(inner, "list-item")) S->display = CSCD_DISPLAY_LIST_ITEM;
        else return false;
        return true;
    }
    for (size_t i = 0; i < CSCD_COUNT(d); ++i)
        if (cscdSvIEqC(cscdStrSv(&V(0)->tok.text), d[i].n)) { S->display = d[i].d; return true; }
    return false;
}

HANDLER(hPosition) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    if (ID(0, "static")) S->position = CSCD_POS_STATIC;
    else if (ID(0, "relative")) S->position = CSCD_POS_RELATIVE;
    else if (ID(0, "absolute")) S->position = CSCD_POS_ABSOLUTE;
    else if (ID(0, "fixed")) S->position = CSCD_POS_FIXED;
    else if (ID(0, "sticky") || ID(0, "-webkit-sticky")) S->position = CSCD_POS_STICKY;
    else return false;
    return true;
}

HANDLER(hFloat) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    if (ID(0, "left") || ID(0, "inline-start")) S->floating = CSCD_FLOAT_LEFT;
    else if (ID(0, "right") || ID(0, "inline-end")) S->floating = CSCD_FLOAT_RIGHT;
    else if (ID(0, "none")) S->floating = CSCD_FLOAT_NONE;
    else return false;
    return true;
}

HANDLER(hClear) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    if (ID(0, "left")) S->clear = CSCD_CLEAR_LEFT;
    else if (ID(0, "right")) S->clear = CSCD_CLEAR_RIGHT;
    else if (ID(0, "both")) S->clear = CSCD_CLEAR_BOTH;
    else S->clear = CSCD_CLEAR_NONE;
    return true;
}

HANDLER(hBoxSizing) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    S->boxSizing = ID(0, "border-box") ? CSCD_BOX_BORDER : CSCD_BOX_CONTENT;
    return true;
}

#define LEN_HANDLER(name, field, keywords, negative) \
    HANDLER(name) { UNUSED_ARGS; return lengthOf(st, v, &S->field, keywords, negative); }
LEN_HANDLER(hWidth, width, true, false)
LEN_HANDLER(hHeight, height, true, false)
LEN_HANDLER(hMinWidth, minWidth, true, false)
LEN_HANDLER(hMinHeight, minHeight, true, false)
LEN_HANDLER(hMaxWidth, maxWidth, true, false)
LEN_HANDLER(hMaxHeight, maxHeight, true, false)
LEN_HANDLER(hTop, top, true, true)
LEN_HANDLER(hRight, right, true, true)
LEN_HANDLER(hBottom, bottom, true, true)
LEN_HANDLER(hLeft, left, true, true)
LEN_HANDLER(hMarginTop, margin[0], true, true)
LEN_HANDLER(hMarginRight, margin[1], true, true)
LEN_HANDLER(hMarginBottom, margin[2], true, true)
LEN_HANDLER(hMarginLeft, margin[3], true, true)
LEN_HANDLER(hPaddingTop, padding[0], false, false)
LEN_HANDLER(hPaddingRight, padding[1], false, false)
LEN_HANDLER(hPaddingBottom, padding[2], false, false)
LEN_HANDLER(hPaddingLeft, padding[3], false, false)

#define BORDER_HANDLERS(i, W, St, C, R) \
    HANDLER(W) { UNUSED_ARGS; return parseBorderWidth(st, v, &S->border[i].width); } \
    HANDLER(St) { UNUSED_ARGS; bool ok = v->len == 1; if (ok) S->border[i].style = (uint8_t)borderStyleOf(V(0), &ok); return ok; } \
    HANDLER(C) { UNUSED_ARGS; st->colorSet |= 1u << (i); return colorOf(st, v, &S->border[i].color); } \
    HANDLER(R) { \
        UNUSED_ARGS; \
        if (!v->len) return false; \
        CscdLength a, b; \
        if (!cscdParseLength(V(0), &st->ctx, &a, false)) return false; \
        b = a; \
        if (v->len > 1 && !cscdParseLength(V(1), &st->ctx, &b, false)) return false; \
        S->radius[i].x = a; \
        S->radius[i].y = b; \
        return true; \
    }
BORDER_HANDLERS(0, hBorderTopWidth, hBorderTopStyle, hBorderTopColor, hRadiusTL)
BORDER_HANDLERS(1, hBorderRightWidth, hBorderRightStyle, hBorderRightColor, hRadiusTR)
BORDER_HANDLERS(2, hBorderBottomWidth, hBorderBottomStyle, hBorderBottomColor, hRadiusBR)
BORDER_HANDLERS(3, hBorderLeftWidth, hBorderLeftStyle, hBorderLeftColor, hRadiusBL)

HANDLER(hAspectRatio) {
    UNUSED_ARGS;
    float a = 0, b = 1;
    for (size_t i = 0; i < v->len; ++i) {
        if (ID(i, "auto")) continue;
        if (V(i)->tok.type == CSCD_TOK_NUMBER) {
            a = (float)V(i)->tok.num;
            if (i + 2 < v->len && cscdCssIsDelim(V(i + 1), '/')) b = (float)V(i + 2)->tok.num;
            break;
        }
    }
    S->aspectRatio = b > 0 ? a / b : 0;
    return true;
}

HANDLER(hZIndex) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    if (ID(0, "auto")) { S->zAuto = true; S->zIndex = 0; return true; }
    if (V(0)->tok.type != CSCD_TOK_NUMBER) return false;
    S->zAuto = false;
    S->zIndex = cscdIntOfDouble(V(0)->tok.num);
    return true;
}

static bool overflowOf(const Values* v, uint8_t* o) {
    if (v->len != 1) return false;
    if (ID(0, "visible")) *o = CSCD_OVERFLOW_VISIBLE;
    else if (ID(0, "hidden")) *o = CSCD_OVERFLOW_HIDDEN;
    else if (ID(0, "scroll")) *o = CSCD_OVERFLOW_SCROLL;
    else if (ID(0, "auto") || ID(0, "overlay")) *o = CSCD_OVERFLOW_AUTO;
    else if (ID(0, "clip")) *o = CSCD_OVERFLOW_CLIP;
    else return false;
    return true;
}
HANDLER(hOverflowX) { UNUSED_ARGS; return overflowOf(v, &S->overflowX); }
HANDLER(hOverflowY) { UNUSED_ARGS; return overflowOf(v, &S->overflowY); }

HANDLER(hVisibility) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    S->visibility = ID(0, "hidden") ? CSCD_VIS_HIDDEN : ID(0, "collapse") ? CSCD_VIS_COLLAPSE : CSCD_VIS_VISIBLE;
    return true;
}

HANDLER(hOpacity) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    float o;
    if (V(0)->tok.type == CSCD_TOK_PERCENTAGE) o = (float)V(0)->tok.num / 100.f;
    else if (!cscdParseNumber(V(0), &o, &st->ctx)) return false;
    S->opacity = cscdClampf(o, 0.f, 1.f);
    return true;
}

HANDLER(hColor) {
    UNUSED_ARGS;
    size_t i = 0;
    CscdColor c;
    CscdColor cur = st->parent ? st->parent->color : cscdRgb(0, 0, 0);
    if (!cscdParseColor(v, &i, &c, cur) || i != v->len) return false;
    S->color = c;
    return true;
}

HANDLER(hBackgroundColor) { UNUSED_ARGS; return colorOf(st, v, &S->backgroundColor); }
HANDLER(hBackgroundImage) { UNUSED_ARGS; cscdStrSet(&st->bgImage, raw); st->bgTouched = true; return true; }
HANDLER(hBackgroundPosition) { UNUSED_ARGS; cscdStrSet(&st->bgPosition, raw); st->bgTouched = true; return true; }
HANDLER(hBackgroundPositionX) { UNUSED_ARGS; cscdStrSet(&st->bgPosition, raw); cscdStrAppendC(&st->bgPosition, " 0%"); st->bgTouched = true; return true; }
HANDLER(hBackgroundSize) { UNUSED_ARGS; cscdStrSet(&st->bgSize, raw); st->bgTouched = true; return true; }
HANDLER(hBackgroundRepeat) { UNUSED_ARGS; cscdStrSet(&st->bgRepeat, raw); st->bgTouched = true; return true; }
HANDLER(hBackgroundClip) { UNUSED_ARGS; cscdStrSet(&st->bgClip, raw); st->bgTouched = true; return true; }
HANDLER(hBackgroundOrigin) { UNUSED_ARGS; cscdStrSet(&st->bgOrigin, raw); st->bgTouched = true; return true; }
HANDLER(hAccept) { UNUSED_ARGS; return true; }
HANDLER(hBoxShadow) { UNUSED_ARGS; return cscdParseShadows(raw, st, &S->boxShadow, false); }
HANDLER(hTextShadow) { UNUSED_ARGS; return cscdParseShadows(raw, st, &S->textShadow, true); }
HANDLER(hOutlineWidth) { UNUSED_ARGS; return parseBorderWidth(st, v, &S->outlineWidth); }

HANDLER(hOutlineStyle) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    if (ID(0, "auto")) { S->outlineStyle = CSCD_BORDER_SOLID; return true; }
    bool ok;
    S->outlineStyle = (uint8_t)borderStyleOf(V(0), &ok);
    return ok;
}

HANDLER(hOutlineColor) {
    UNUSED_ARGS;
    st->colorSet |= 16;
    if (v->len == 1 && ID(0, "invert")) { S->outlineColor = S->color; return true; }
    return colorOf(st, v, &S->outlineColor);
}

HANDLER(hOutlineOffset) { UNUSED_ARGS; if (v->len != 1) return false; S->outlineOffset = cscdLengthPx(V(0), &st->ctx, 0); return true; }
HANDLER(hTransform) { UNUSED_ARGS; return parseTransform(st, v, &S->transform); }

HANDLER(hTranslate) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "none")) return true;
    CscdTransformOp op = cscdTransformOp(CSCD_TF_TRANSLATE);
    if (!v->len || !cscdParseLength(V(0), &st->ctx, &op.x, false)) return false;
    if (v->len > 1) cscdParseLength(V(1), &st->ctx, &op.y, false);
    cscdVecInsert(&S->transform, 0, op);
    if (v->len > 2) {
        CscdTransformOp tz = cscdTransformOp(CSCD_TF_TRANSLATE_Z);
        tz.z = cscdLengthPx(V(2), &st->ctx, 0);
        cscdVecInsert(&S->transform, 1, tz);
    }
    return true;
}

HANDLER(hRotate) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "none")) return true;
    CscdTransformOp op = cscdTransformOp(CSCD_TF_ROTATE);
    if (!v->len || !cscdParseAngle(V(v->len - 1), &op.a)) return false;
    /* an axis: x, y, z or three numbers ("rotate: y 45deg", "rotate: 1 1 0 30deg") */
    if (v->len == 2 && (ID(0, "x") || ID(0, "y"))) {
        op.kind = CSCD_TF_ROTATE3D;
        op.x3 = ID(0, "x") ? 1.f : 0.f;
        op.y3 = ID(0, "y") ? 1.f : 0.f;
        op.z3 = 0;
    } else if (v->len == 4) {
        op.kind = CSCD_TF_ROTATE3D;
        cscdParseNumber(V(0), &op.x3, &st->ctx);
        cscdParseNumber(V(1), &op.y3, &st->ctx);
        cscdParseNumber(V(2), &op.z3, &st->ctx);
    }
    cscdVecPush(&S->transform, op);
    return true;
}

static float scaleNum(CscdApplyState* st, const CscdCssValue* x) {
    float r = 1;
    if (x->tok.type == CSCD_TOK_PERCENTAGE) r = (float)x->tok.num / 100.f;
    else cscdParseNumber(x, &r, &st->ctx);
    return r;
}

HANDLER(hScale) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "none")) return true;
    if (!v->len) return false;
    CscdTransformOp op = cscdTransformOp(CSCD_TF_SCALE);
    op.a = scaleNum(st, V(0));
    op.b = v->len > 1 ? scaleNum(st, V(1)) : op.a;
    cscdVecPush(&S->transform, op);
    if (v->len > 2) {
        CscdTransformOp sz = cscdTransformOp(CSCD_TF_SCALE_Z);
        sz.a = scaleNum(st, V(2));
        cscdVecPush(&S->transform, sz);
    }
    return true;
}

HANDLER(hTransformOrigin) {
    UNUSED_ARGS;
    CscdLength x = cscdLenPercent(50), y = cscdLenPercent(50);
    int n = 0;
    for (size_t i = 0; i < v->len; ++i) {
        const CscdCssValue* p = V(i);
        if (cscdCssIsIdent(p, "left")) x = cscdLenPercent(0);
        else if (cscdCssIsIdent(p, "right")) x = cscdLenPercent(100);
        else if (cscdCssIsIdent(p, "top")) y = cscdLenPercent(0);
        else if (cscdCssIsIdent(p, "bottom")) y = cscdLenPercent(100);
        else if (cscdCssIsIdent(p, "center")) { if (n == 0) x = cscdLenPercent(50); else y = cscdLenPercent(50); }
        else {
            CscdLength l;
            if (cscdParseLength(p, &st->ctx, &l, false)) {
                if (n == 0) x = l;
                else if (n == 1) y = l;
                else S->originZ = cscdLengthPx(p, &st->ctx, 0);
            }
        }
        ++n;
    }
    S->originX = x;
    S->originY = y;
    return true;
}

HANDLER(hPerspective) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    S->perspective = ID(0, "none") ? 0 : cscdMaxf(0.f, cscdLengthPx(V(0), &st->ctx, 0));
    return true;
}

HANDLER(hPerspectiveOrigin) {
    UNUSED_ARGS;
    CscdLength x = cscdLenPercent(50), y = cscdLenPercent(50);
    int n = 0;
    for (size_t i = 0; i < v->len; ++i) {
        const CscdCssValue* p = V(i);
        if (cscdCssIsIdent(p, "left")) x = cscdLenPercent(0);
        else if (cscdCssIsIdent(p, "right")) x = cscdLenPercent(100);
        else if (cscdCssIsIdent(p, "top")) y = cscdLenPercent(0);
        else if (cscdCssIsIdent(p, "bottom")) y = cscdLenPercent(100);
        else if (cscdCssIsIdent(p, "center")) { if (n == 0) x = cscdLenPercent(50); else y = cscdLenPercent(50); }
        else {
            CscdLength l;
            if (cscdParseLength(p, &st->ctx, &l, false)) { if (n == 0) x = l; else y = l; }
        }
        ++n;
    }
    S->perspectiveX = x;
    S->perspectiveY = y;
    return true;
}

HANDLER(hContainerType) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    S->containerType = ID(0, "inline-size") ? 1 : ID(0, "size") ? 2 : 0;
    return true;
}

HANDLER(hContainerName) {
    UNUSED_ARGS;
    cscdStrVecFree(&S->containerNames);
    for (size_t i = 0; i < v->len; ++i)
        if (V(i)->tok.type == CSCD_TOK_IDENT && !ID(i, "none")) cscdVecPush(&S->containerNames, cscdStrCopy(&V(i)->tok.text));
    return true;
}

HANDLER(hContainer) { /* name [/ type] */
    UNUSED_ARGS;
    cscdStrVecFree(&S->containerNames);
    S->containerType = 0;
    bool afterSlash = false;
    for (size_t i = 0; i < v->len; ++i) {
        const CscdCssValue* x = V(i);
        if (x->tok.type == CSCD_TOK_DELIM && x->tok.delim == '/') { afterSlash = true; continue; }
        if (x->tok.type != CSCD_TOK_IDENT) continue;
        if (afterSlash) S->containerType = cscdCssIsIdent(x, "inline-size") ? 1 : cscdCssIsIdent(x, "size") ? 2 : 0;
        else if (!cscdCssIsIdent(x, "none")) cscdVecPush(&S->containerNames, cscdStrCopy(&x->tok.text));
    }
    return true;
}

HANDLER(hCounterReset) { UNUSED_ARGS; return parseCounters(v, 0, &S->counterReset); }
HANDLER(hCounterSet) { UNUSED_ARGS; return parseCounters(v, 0, &S->counterSet); }
HANDLER(hCounterIncrement) { UNUSED_ARGS; return parseCounters(v, 1, &S->counterIncrement); }
HANDLER(hBackfaceVisibility) { UNUSED_ARGS; if (v->len != 1) return false; S->backfaceHidden = ID(0, "hidden"); return true; }
HANDLER(hTransformStyle) { UNUSED_ARGS; if (v->len != 1) return false; S->preserve3d = ID(0, "preserve-3d"); return true; }
HANDLER(hCursor) { UNUSED_ARGS; S->cursor = (uint8_t)parseCursor(v); return true; }
HANDLER(hPointerEvents) { UNUSED_ARGS; S->pointerEvents = v->len == 1 && ID(0, "none") ? CSCD_PE_NONE : CSCD_PE_AUTO; return true; }

HANDLER(hUserSelect) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    S->userSelect = ID(0, "none") ? CSCD_US_NONE : ID(0, "text") ? CSCD_US_TEXT : ID(0, "all") ? CSCD_US_ALL : CSCD_US_AUTO;
    return true;
}

/* touch-action: auto | none | manipulation | [pan-x | pan-left | pan-right] || [pan-y | pan-up | pan-down] || pinch-zoom */
HANDLER(hTouchAction) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "auto")) { S->touchAction = 0; return true; }
    if (v->len == 1 && ID(0, "none")) { S->touchAction = CSCD_TA_SET; return true; }
    if (v->len == 1 && ID(0, "manipulation")) { S->touchAction = CSCD_TA_SET | CSCD_TA_PAN_X | CSCD_TA_PAN_Y | CSCD_TA_PINCH; return true; }
    unsigned bits = CSCD_TA_SET;
    for (size_t i = 0; i < v->len; ++i) {
        unsigned b = ID(i, "pan-x") || ID(i, "pan-left") || ID(i, "pan-right") ? CSCD_TA_PAN_X
                     : ID(i, "pan-y") || ID(i, "pan-up") || ID(i, "pan-down") ? CSCD_TA_PAN_Y
                     : ID(i, "pinch-zoom") ? CSCD_TA_PINCH : 0;
        if (!b) return false;
        bits |= b;
    }
    S->touchAction = (uint8_t)bits;
    return true;
}

HANDLER(hAccentColor) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "auto")) { S->accentColor = cscdRgb(0, 117, 255); return true; }
    return colorOf(st, v, &S->accentColor);
}

HANDLER(hCaretColor) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "auto")) { S->caretAuto = true; return true; }
    S->caretAuto = false;
    return colorOf(st, v, &S->caretColor);
}

HANDLER(hMixBlendMode) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    CscdSv id = cscdStrSv(&V(0)->tok.text);
    S->blend = cscdSvIEqC(id, "multiply") ? CSCD_BM_MULTIPLY : cscdSvIEqC(id, "screen") ? CSCD_BM_SCREEN : cscdSvIEqC(id, "overlay") ? CSCD_BM_OVERLAY
             : cscdSvIEqC(id, "darken") ? CSCD_BM_DARKEN : cscdSvIEqC(id, "lighten") ? CSCD_BM_LIGHTEN : CSCD_BM_NORMAL;
    return true;
}

HANDLER(hTextAlignLast) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    S->alignLastSet = !ID(0, "auto");
    CscdTextAlign a = CSCD_ALIGN_START;
    if (ID(0, "left")) a = CSCD_ALIGN_LEFT;
    else if (ID(0, "right")) a = CSCD_ALIGN_RIGHT;
    else if (ID(0, "center")) a = CSCD_ALIGN_CENTER;
    else if (ID(0, "justify")) a = CSCD_ALIGN_JUSTIFY;
    else if (ID(0, "end")) a = CSCD_ALIGN_END;
    S->textAlignLast = (uint8_t)a;
    return true;
}

HANDLER(hTextFillColor) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "currentcolor")) { S->textFillSet = false; return true; }
    S->textFillSet = colorOf(st, v, &S->textFill);
    return true;
}

HANDLER(hTextStrokeWidth) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    S->textStrokeWidth = ID(0, "thin") ? 1.f : ID(0, "medium") ? 3.f : ID(0, "thick") ? 5.f : cscdMaxf(0.f, cscdLengthPx(V(0), &st->ctx, 0));
    return true;
}

HANDLER(hTextStrokeColor) { UNUSED_ARGS; S->textStrokeSet = colorOf(st, v, &S->textStroke); return true; }

HANDLER(hTextStroke) { /* width and color, any order */
    UNUSED_ARGS;
    S->textStrokeWidth = 0;
    S->textStrokeSet = false;
    for (size_t i = 0; i < v->len; ++i) {
        if (V(i)->tok.type == CSCD_TOK_DIMENSION || V(i)->tok.type == CSCD_TOK_NUMBER) S->textStrokeWidth = cscdMaxf(0.f, cscdLengthPx(V(i), &st->ctx, 0));
        else if (ID(i, "thin")) S->textStrokeWidth = 1;
        else if (ID(i, "medium")) S->textStrokeWidth = 3;
        else if (ID(i, "thick")) S->textStrokeWidth = 5;
        else {
            Values one1 = cscdCssView(V(i), 1);
            S->textStrokeSet = colorOf(st, &one1, &S->textStroke) || S->textStrokeSet;
        }
    }
    return true;
}

HANDLER(hImageRendering) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    S->pixelated = ID(0, "pixelated") || ID(0, "crisp-edges") || ID(0, "-webkit-optimize-contrast");
    return true;
}

HANDLER(hIsolation) { UNUSED_ARGS; S->isolate = v->len == 1 && ID(0, "isolate"); return true; }
HANDLER(hTableLayout) { UNUSED_ARGS; S->tableFixed = v->len == 1 && ID(0, "fixed"); return true; }
HANDLER(hCaptionSide) { UNUSED_ARGS; S->captionBottom = v->len == 1 && ID(0, "bottom"); return true; }

/* the filter functions of filter / backdrop-filter */
static void filterFunctions(CscdApplyState* st, const Values* v, float* blur, float* brightness, float* grayscale, float* saturate, float* invert, float* opacity) {
    for (size_t i = 0; i < v->len; ++i) {
        const CscdCssValue* f = V(i);
        if (f->tok.type != CSCD_TOK_FUNCTION || !f->args.len) continue;
        float amount = 1;
        const CscdCssValue* a = &f->args.items[0];
        if (a->tok.type == CSCD_TOK_PERCENTAGE) amount = (float)a->tok.num / 100.f;
        else if (a->tok.type == CSCD_TOK_NUMBER) amount = (float)a->tok.num;
        if (cscdCssIsFunction(f, "blur")) *blur = cscdLengthPx(a, &st->ctx, 0);
        else if (cscdCssIsFunction(f, "brightness")) *brightness = amount;
        else if (cscdCssIsFunction(f, "grayscale")) *grayscale = cscdClampf(amount, 0.f, 1.f);
        else if (cscdCssIsFunction(f, "saturate")) *saturate = amount;
        else if (cscdCssIsFunction(f, "invert")) *invert = cscdClampf(amount, 0.f, 1.f);
        else if (opacity && cscdCssIsFunction(f, "opacity")) *opacity *= cscdClampf(amount, 0.f, 1.f);
    }
}

HANDLER(hBackdropFilter) {
    UNUSED_ARGS;
    CscdBackdrop* b = &S->backdrop;
    memset(b, 0, sizeof *b);
    b->brightness = 1;
    b->saturate = 1;
    filterFunctions(st, v, &b->blur, &b->brightness, &b->grayscale, &b->saturate, &b->invert, NULL);
    return true;
}

HANDLER(hMaskImage) { UNUSED_ARGS; cscdStrSet(&st->maskImage, raw); st->maskTouched = true; return true; }
HANDLER(hMaskPosition) { UNUSED_ARGS; cscdStrSet(&st->maskPosition, raw); st->maskTouched = true; return true; }
HANDLER(hMaskSize) { UNUSED_ARGS; cscdStrSet(&st->maskSize, raw); st->maskTouched = true; return true; }
HANDLER(hMaskRepeat) { UNUSED_ARGS; cscdStrSet(&st->maskRepeat, raw); st->maskTouched = true; return true; }

HANDLER(hMask) { /* (the image, and a repeat keyword when there is one) */
    UNUSED_ARGS;
    cscdStrSet(&st->maskImage, raw);
    for (size_t i = 0; i < v->len; ++i)
        if (ID(i, "no-repeat") || ID(i, "repeat") || ID(i, "repeat-x") || ID(i, "repeat-y")) cscdStrSet(&st->maskRepeat, cscdStrSv(&V(i)->tok.text));
    st->maskTouched = true;
    return true;
}

HANDLER(hFilter) {
    UNUSED_ARGS;
    S->blur = 0; S->brightness = 1; S->grayscale = 0; S->saturate = 1; S->invert = 0;
    filterFunctions(st, v, &S->blur, &S->brightness, &S->grayscale, &S->saturate, &S->invert, &S->opacity);
    return true;
}

HANDLER(hObjectFit) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    CscdSv id = cscdStrSv(&V(0)->tok.text);
    S->objectFit = cscdSvIEqC(id, "contain") ? CSCD_FIT_CONTAIN : cscdSvIEqC(id, "cover") ? CSCD_FIT_COVER : cscdSvIEqC(id, "none") ? CSCD_FIT_NONE
                 : cscdSvIEqC(id, "scale-down") ? CSCD_FIT_SCALE_DOWN : CSCD_FIT_FILL;
    return true;
}

HANDLER(hObjectPosition) {
    UNUSED_ARGS;
    int n = 0;
    for (size_t i = 0; i < v->len; ++i) {
        CscdLength l;
        const CscdCssValue* p = V(i);
        if (cscdCssIsIdent(p, "left")) S->objectPosX = cscdLenPercent(0);
        else if (cscdCssIsIdent(p, "right")) S->objectPosX = cscdLenPercent(100);
        else if (cscdCssIsIdent(p, "top")) S->objectPosY = cscdLenPercent(0);
        else if (cscdCssIsIdent(p, "bottom")) S->objectPosY = cscdLenPercent(100);
        else if (cscdParseLength(p, &st->ctx, &l, false)) { if (n == 0) S->objectPosX = l; else S->objectPosY = l; }
        ++n;
    }
    return true;
}

HANDLER(hFill) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "none")) { S->fillNone = true; return true; }
    S->fillNone = false;
    S->fillCurrent = v->len == 1 && ID(0, "currentcolor");
    return colorOf(st, v, &S->fill);
}

HANDLER(hStroke) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "none")) { S->strokeNone = true; return true; }
    S->strokeNone = false;
    S->strokeCurrent = v->len == 1 && ID(0, "currentcolor");
    return colorOf(st, v, &S->stroke);
}

HANDLER(hStrokeWidth) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    if (V(0)->tok.type == CSCD_TOK_NUMBER) S->strokeWidth = (float)V(0)->tok.num;
    else S->strokeWidth = cscdLengthPx(V(0), &st->ctx, 1);
    return true;
}

HANDLER(hScrollbarColor) {
    UNUSED_ARGS;
    if (v->len == 1 && (ID(0, "auto") || ID(0, "initial"))) { S->scrollbarThumbSet = false; return true; }
    size_t i = 0;
    CscdColor thumb;
    if (!cscdParseColor(v, &i, &thumb, S->color)) return false;
    S->scrollbarThumb = thumb;
    S->scrollbarThumbSet = true;
    return true;
}

HANDLER(hClipPath) { UNUSED_ARGS; cscdStrSet(&S->clipPath, cscdSvEqC(raw, "none") ? CSCD_SV("") : raw); return true; }

HANDLER(hFontFamily) {
    UNUSED_ARGS;
    CscdStrVec fams = {0};
    CscdCssSlices parts = {0};
    cscdCssSplitCommas(v, &parts);
    for (size_t p = 0; p < parts.len; ++p) {
        CscdStr name = {0};
        for (size_t k = 0; k < parts.items[p].len; ++k) {
            if (name.len) cscdStrAppendChar(&name, ' ');
            cscdStrAppend(&name, cscdStrSv(&parts.items[p].items[k].tok.text));
        }
        if (name.len) cscdVecPush(&fams, name);
        else cscdStrFree(&name);
    }
    cscdVecFree(&parts);
    if (!fams.len) return false;
    cscdStrVecFree(&S->font.families);
    S->font.families = fams;
    memset(&S->resolvedFont, 0, sizeof S->resolvedFont); /* (the face is found again) */
    return true;
}

HANDLER(hFontSize) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    const float textScale = st->env && st->env->media.textScale > 0 ? st->env->media.textScale : 1;
    float parentSize = st->parent ? st->parent->font.size : 16 * textScale;
    static const struct { const char* n; float s; } kw[] = {
        {"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16}, {"large", 18}, {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 48}};
    /* (zoom: absolute sizes by the zoom of all, sizes relative to the parent's by this element's own) */
    const float own = S->zoom > 0 ? S->zoom : 1, all = st->ctx.zoom > 0 ? st->ctx.zoom : 1;
    if (V(0)->tok.type == CSCD_TOK_IDENT) {
        CscdSv id = cscdStrSv(&V(0)->tok.text);
        for (size_t i = 0; i < CSCD_COUNT(kw); ++i)
            if (cscdSvIEqC(id, kw[i].n)) { S->font.size = kw[i].s * all * textScale; return true; }
        if (cscdSvIEqC(id, "smaller")) { S->font.size = parentSize / 1.2f * own; return true; }
        if (cscdSvIEqC(id, "larger")) { S->font.size = parentSize * 1.2f * own; return true; }
        return false;
    }
    CscdValueCtx c = st->ctx;
    c.em = parentSize * own;
    CscdLength l;
    if (!cscdParseLength(V(0), &c, &l, false)) return false;
    S->font.size = cscdMaxf(0.f, cscdLenResolve(l, parentSize * own, 0));
    return true;
}

HANDLER(hFontWeight) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    int pw = st->parent ? st->parent->font.weight : 400;
    if (ID(0, "normal")) S->font.weight = 400;
    else if (ID(0, "bold")) S->font.weight = 700;
    else if (ID(0, "bolder")) S->font.weight = pw < 350 ? 400 : pw < 550 ? 700 : 900;
    else if (ID(0, "lighter")) S->font.weight = pw < 550 ? 100 : pw < 750 ? 400 : 700;
    else if (V(0)->tok.type == CSCD_TOK_NUMBER) S->font.weight = cscdClampi(cscdIntOfDouble(V(0)->tok.num), 1, 1000);
    else return false;
    return true;
}

HANDLER(hFontStyle) {
    UNUSED_ARGS;
    if (!v->len) return false;
    S->font.italic = ID(0, "italic") || ID(0, "oblique");
    return true;
}

/* The font-variant-* keywords: each sets its part of the font (the OpenType features the shaper
 * turns on); false for one none of them knows. */
static bool variantCaps(CscdComputedStyle* s, const CscdCssValue* x) {
    static const char* kCaps[] = {"normal", "small-caps", "all-small-caps", "petite-caps", "all-petite-caps", "unicase", "titling-caps"};
    for (size_t i = 0; i < CSCD_COUNT(kCaps); ++i)
        if (cscdCssIsIdent(x, kCaps[i])) { s->font.caps = (uint8_t)i; s->font.smallCaps = i == CSCD_CAPS_SMALL || i == CSCD_CAPS_ALL_SMALL; return true; }
    return false;
}

static bool variantNumeric(CscdComputedStyle* s, const CscdCssValue* x) {
    static const struct { const char* name; uint16_t bit, clears; } kNumeric[] = {
        {"lining-nums", CSCD_NUMERIC_LINING, CSCD_NUMERIC_OLDSTYLE}, {"oldstyle-nums", CSCD_NUMERIC_OLDSTYLE, CSCD_NUMERIC_LINING},
        {"proportional-nums", CSCD_NUMERIC_PROPORTIONAL, CSCD_NUMERIC_TABULAR}, {"tabular-nums", CSCD_NUMERIC_TABULAR, CSCD_NUMERIC_PROPORTIONAL},
        {"diagonal-fractions", CSCD_NUMERIC_DIAGONAL, CSCD_NUMERIC_STACKED}, {"stacked-fractions", CSCD_NUMERIC_STACKED, CSCD_NUMERIC_DIAGONAL},
        {"ordinal", CSCD_NUMERIC_ORDINAL, 0}, {"slashed-zero", CSCD_NUMERIC_SLASHED_ZERO, 0}};
    for (size_t i = 0; i < CSCD_COUNT(kNumeric); ++i)
        if (cscdCssIsIdent(x, kNumeric[i].name)) {
            s->font.numeric = (uint16_t)((s->font.numeric & ~kNumeric[i].clears) | kNumeric[i].bit);
            s->font.tabularNums = (s->font.numeric & CSCD_NUMERIC_TABULAR) != 0;
            return true;
        }
    return false;
}

static bool variantLigatures(CscdComputedStyle* s, const CscdCssValue* x) {
    static const struct { const char* name; uint8_t set, clear; } kLigatures[] = {
        {"common-ligatures", 0, CSCD_LIGATURES_NO_COMMON}, {"no-common-ligatures", CSCD_LIGATURES_NO_COMMON, 0},
        {"discretionary-ligatures", CSCD_LIGATURES_DISCRETIONARY, 0}, {"no-discretionary-ligatures", 0, CSCD_LIGATURES_DISCRETIONARY},
        {"historical-ligatures", CSCD_LIGATURES_HISTORICAL, 0}, {"no-historical-ligatures", 0, CSCD_LIGATURES_HISTORICAL},
        {"contextual", 0, CSCD_LIGATURES_NO_CONTEXTUAL}, {"no-contextual", CSCD_LIGATURES_NO_CONTEXTUAL, 0}};
    for (size_t i = 0; i < CSCD_COUNT(kLigatures); ++i)
        if (cscdCssIsIdent(x, kLigatures[i].name)) {
            s->font.ligatures = (uint8_t)((s->font.ligatures & ~kLigatures[i].clear) | kLigatures[i].set);
            s->font.noLigatures = (s->font.ligatures & CSCD_LIGATURES_NO_COMMON) != 0;
            return true;
        }
    return false;
}

static bool variantPosition(CscdComputedStyle* s, const CscdCssValue* x) {
    if (cscdCssIsIdent(x, "sub")) s->font.position = 1;
    else if (cscdCssIsIdent(x, "super")) s->font.position = 2;
    else return false;
    return true;
}

static bool variantEastAsian(CscdComputedStyle* s, const CscdCssValue* x) {
    static const struct { const char* name; uint16_t bit; } kEastAsian[] = {
        {"jis78", CSCD_EAST_ASIAN_JIS78}, {"jis83", CSCD_EAST_ASIAN_JIS83}, {"jis90", CSCD_EAST_ASIAN_JIS90}, {"jis04", CSCD_EAST_ASIAN_JIS04},
        {"simplified", CSCD_EAST_ASIAN_SIMPLIFIED}, {"traditional", CSCD_EAST_ASIAN_TRADITIONAL}, {"full-width", CSCD_EAST_ASIAN_FULL_WIDTH},
        {"proportional-width", CSCD_EAST_ASIAN_PROPORTIONAL_WIDTH}, {"ruby", CSCD_EAST_ASIAN_RUBY}};
    for (size_t i = 0; i < CSCD_COUNT(kEastAsian); ++i)
        if (cscdCssIsIdent(x, kEastAsian[i].name)) { s->font.eastAsian |= kEastAsian[i].bit; return true; }
    return false;
}

static void variantReset(CscdComputedStyle* s, bool none) {
    s->font.caps = 0;
    s->font.smallCaps = false;
    s->font.numeric = 0;
    s->font.tabularNums = false;
    s->font.ligatures = none ? CSCD_LIGATURES_NO_COMMON | CSCD_LIGATURES_NO_CONTEXTUAL : 0;
    s->font.noLigatures = none;
    s->font.position = 0;
    s->font.eastAsian = 0;
}

/* font-variant: every font-variant-* at once */
HANDLER(hFontVariant) {
    UNUSED_ARGS;
    if (!v->len) return false;
    CscdComputedStyle saved = *S; /* (only the font's numbers change: restored when a keyword is wrong) */
    variantReset(S, v->len == 1 && ID(0, "none"));
    if (v->len == 1 && (ID(0, "none") || ID(0, "normal"))) return true;
    for (size_t i = 0; i < v->len; ++i)
        if (!variantCaps(S, V(i)) && !variantNumeric(S, V(i)) && !variantLigatures(S, V(i)) && !variantPosition(S, V(i)) && !variantEastAsian(S, V(i))) {
            S->font.caps = saved.font.caps; S->font.smallCaps = saved.font.smallCaps; S->font.numeric = saved.font.numeric;
            S->font.tabularNums = saved.font.tabularNums; S->font.ligatures = saved.font.ligatures; S->font.noLigatures = saved.font.noLigatures;
            S->font.position = saved.font.position; S->font.eastAsian = saved.font.eastAsian;
            return false;
        }
    return true;
}

HANDLER(hFontVariantCaps) { UNUSED_ARGS; return v->len == 1 && variantCaps(S, V(0)); }

HANDLER(hFontVariantLigatures) {
    UNUSED_ARGS;
    if (!v->len) return false;
    const bool none = v->len == 1 && ID(0, "none");
    S->font.ligatures = none ? CSCD_LIGATURES_NO_COMMON | CSCD_LIGATURES_NO_CONTEXTUAL : 0;
    S->font.noLigatures = none;
    if (none || (v->len == 1 && ID(0, "normal"))) return true;
    for (size_t i = 0; i < v->len; ++i)
        if (!variantLigatures(S, V(i))) return false;
    return true;
}

HANDLER(hFontVariantNumeric) {
    UNUSED_ARGS;
    if (!v->len) return false;
    S->font.numeric = 0;
    S->font.tabularNums = false;
    if (v->len == 1 && ID(0, "normal")) return true;
    for (size_t i = 0; i < v->len; ++i)
        if (!variantNumeric(S, V(i))) return false;
    return true;
}

HANDLER(hFontVariantPosition) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    if (ID(0, "normal")) { S->font.position = 0; return true; }
    return variantPosition(S, V(0));
}

HANDLER(hFontVariantEastAsian) {
    UNUSED_ARGS;
    if (!v->len) return false;
    S->font.eastAsian = 0;
    if (v->len == 1 && ID(0, "normal")) return true;
    for (size_t i = 0; i < v->len; ++i)
        if (!variantEastAsian(S, V(i))) return false;
    return true;
}

HANDLER(hFontKerning) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    if (ID(0, "auto")) S->font.kerning = 0;
    else if (ID(0, "normal")) S->font.kerning = 1;
    else if (ID(0, "none")) S->font.kerning = 2;
    else return false;
    return true;
}

HANDLER(hFontOpticalSizing) {
    UNUSED_ARGS;
    if (v->len != 1 || !(ID(0, "auto") || ID(0, "none"))) return false;
    S->font.noOpticalSizing = ID(0, "none");
    return true;
}

HANDLER(hFontStretch) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    static const struct { const char* name; float percent; } kStretch[] = {
        {"normal", 0}, {"ultra-condensed", 50}, {"extra-condensed", 62.5f}, {"condensed", 75}, {"semi-condensed", 87.5f},
        {"semi-expanded", 112.5f}, {"expanded", 125}, {"extra-expanded", 150}, {"ultra-expanded", 200}};
    for (size_t i = 0; i < CSCD_COUNT(kStretch); ++i)
        if (ID(0, kStretch[i].name)) { S->font.stretch = kStretch[i].percent; return true; }
    if (V(0)->tok.type == CSCD_TOK_PERCENTAGE && V(0)->tok.num >= 0) { S->font.stretch = V(0)->tok.num == 100 ? 0 : (float)V(0)->tok.num; return true; }
    return false;
}

/* font-feature-settings ("liga" 0, "ss01", "cv01" 2) and font-variation-settings ("wght" 650): tags
 * of four characters, each with a number (on, 1, for a feature without one) */
static bool tagList(const Values* v, bool features, CscdFontSettings* out) {
    memset(out, 0, sizeof *out);
    if (v->len == 1 && cscdCssIsIdent(&v->items[0], "normal")) return true;
    CscdCssSlices parts = {0};
    cscdCssSplitCommas(v, &parts);
    bool ok = parts.len > 0;
    for (size_t p = 0; p < parts.len && ok; ++p) {
        const CscdCssValue* items = parts.items[p].items;
        const size_t n = parts.items[p].len;
        if (n < 1 || n > 2 || items[0].tok.type != CSCD_TOK_STRING || items[0].tok.text.len != 4) { ok = false; break; }
        float value = 1;
        if (n == 2) {
            if (items[1].tok.type == CSCD_TOK_NUMBER) value = (float)items[1].tok.num;
            else if (features && cscdCssIsIdent(&items[1], "on")) value = 1;
            else if (features && cscdCssIsIdent(&items[1], "off")) value = 0;
            else { ok = false; break; }
        } else if (!features) { ok = false; break; }
        const CscdFontTagValue tv = {cscdFontTag(items[0].tok.text.ptr), value};
        if (features && out->featureCount < CSCD_COUNT(out->features)) out->features[out->featureCount++] = tv;
        if (!features && out->variationCount < CSCD_COUNT(out->variations)) out->variations[out->variationCount++] = tv;
    }
    cscdVecFree(&parts);
    return ok;
}

HANDLER(hFontFeatureSettings) {
    UNUSED_ARGS;
    CscdFontSettings s;
    if (!tagList(v, true, &s)) return false;
    S->font.featureSettings = cscdFontSettingsIntern(&s);
    return true;
}

HANDLER(hFontVariationSettings) {
    UNUSED_ARGS;
    CscdFontSettings s;
    if (!tagList(v, false, &s)) return false;
    S->font.variationSettings = cscdFontSettingsIntern(&s);
    return true;
}

HANDLER(hLineHeight) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    if (ID(0, "normal")) { S->lineHeight = -1; S->lineHeightFactor = 0; return true; }
    float n;
    if (V(0)->tok.type == CSCD_TOK_NUMBER || (V(0)->tok.type == CSCD_TOK_FUNCTION && cscdParseNumber(V(0), &n, &st->ctx))) {
        if (V(0)->tok.type == CSCD_TOK_NUMBER) n = (float)V(0)->tok.num;
        S->lineHeightFactor = cscdMaxf(0.f, n);
        S->lineHeight = -1;
        return true;
    }
    CscdLength l;
    if (!cscdParseLength(V(0), &st->ctx, &l, false)) return false;
    S->lineHeight = cscdMaxf(0.f, cscdLenResolve(l, S->font.size, 0));
    S->lineHeightFactor = 0;
    return true;
}

HANDLER(hLetterSpacing) { UNUSED_ARGS; if (v->len != 1) return false; S->letterSpacing = ID(0, "normal") ? 0 : cscdLengthPx(V(0), &st->ctx, 0); return true; }
HANDLER(hWordSpacing) { UNUSED_ARGS; if (v->len != 1) return false; S->wordSpacing = ID(0, "normal") ? 0 : cscdLengthPx(V(0), &st->ctx, 0); return true; }

HANDLER(hTextAlign) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    CscdSv id = cscdStrSv(&V(0)->tok.text);
    if (cscdSvIEqC(id, "left") || cscdSvIEqC(id, "-webkit-left")) S->textAlign = CSCD_ALIGN_LEFT;
    else if (cscdSvIEqC(id, "right") || cscdSvIEqC(id, "-webkit-right")) S->textAlign = CSCD_ALIGN_RIGHT;
    else if (cscdSvIEqC(id, "center") || cscdSvIEqC(id, "-webkit-center")) S->textAlign = CSCD_ALIGN_CENTER;
    else if (cscdSvIEqC(id, "justify")) S->textAlign = CSCD_ALIGN_JUSTIFY;
    else if (cscdSvIEqC(id, "end")) S->textAlign = CSCD_ALIGN_END;
    else S->textAlign = CSCD_ALIGN_START;
    return true;
}

HANDLER(hTextIndent) { UNUSED_ARGS; return v->len && cscdParseLength(V(0), &st->ctx, &S->textIndent, false); }

HANDLER(hWhiteSpace) {
    UNUSED_ARGS;
    if (!v->len) return false;
    CscdSv id = cscdStrSv(&V(0)->tok.text);
    S->whiteSpace = cscdSvIEqC(id, "nowrap") ? CSCD_WS_NOWRAP : cscdSvIEqC(id, "pre") ? CSCD_WS_PRE : cscdSvIEqC(id, "pre-wrap") ? CSCD_WS_PRE_WRAP
                  : cscdSvIEqC(id, "pre-line") ? CSCD_WS_PRE_LINE : cscdSvIEqC(id, "break-spaces") ? CSCD_WS_BREAK_SPACES : CSCD_WS_NORMAL;
    return true;
}

HANDLER(hTextTransform) {
    UNUSED_ARGS;
    if (!v->len) return false;
    CscdSv id = cscdStrSv(&V(0)->tok.text);
    S->textTransform = cscdSvIEqC(id, "uppercase") ? CSCD_TT_UPPERCASE : cscdSvIEqC(id, "lowercase") ? CSCD_TT_LOWERCASE
                     : cscdSvIEqC(id, "capitalize") ? CSCD_TT_CAPITALIZE : CSCD_TT_NONE;
    return true;
}

HANDLER(hTextOverflow) { UNUSED_ARGS; S->textOverflow = v->len && ID(0, "ellipsis") ? CSCD_TO_ELLIPSIS : CSCD_TO_CLIP; return true; }

HANDLER(hWordBreak) {
    UNUSED_ARGS;
    if (!v->len) return false;
    CscdSv id = cscdStrSv(&V(0)->tok.text);
    S->wordBreak = cscdSvIEqC(id, "break-all") ? CSCD_WB_BREAK_ALL : cscdSvIEqC(id, "keep-all") ? CSCD_WB_KEEP_ALL
                 : cscdSvIEqC(id, "break-word") ? CSCD_WB_BREAK_WORD : CSCD_WB_NORMAL;
    return true;
}

HANDLER(hOverflowWrap) { UNUSED_ARGS; S->overflowWrap = v->len && (ID(0, "break-word") || ID(0, "anywhere")); return true; }

HANDLER(hVerticalAlign) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    static const struct { const char* n; CscdVerticalAlign a; } m[] = {
        {"baseline", CSCD_VA_BASELINE}, {"top", CSCD_VA_TOP}, {"middle", CSCD_VA_MIDDLE}, {"bottom", CSCD_VA_BOTTOM},
        {"text-top", CSCD_VA_TEXT_TOP}, {"text-bottom", CSCD_VA_TEXT_BOTTOM}, {"sub", CSCD_VA_SUB}, {"super", CSCD_VA_SUPER}};
    if (V(0)->tok.type == CSCD_TOK_IDENT) {
        for (size_t i = 0; i < CSCD_COUNT(m); ++i)
            if (cscdSvIEqC(cscdStrSv(&V(0)->tok.text), m[i].n)) { S->verticalAlign = (uint8_t)m[i].a; return true; }
        return false;
    }
    CscdLength l;
    if (!cscdParseLength(V(0), &st->ctx, &l, false)) return false;
    S->verticalAlign = CSCD_VA_LENGTH;
    S->verticalAlignLength = l;
    return true;
}

HANDLER(hDecorationLine) {
    UNUSED_ARGS;
    S->underline = S->overline = S->lineThrough = false;
    for (size_t i = 0; i < v->len; ++i) {
        if (ID(i, "underline")) S->underline = true;
        else if (ID(i, "overline")) S->overline = true;
        else if (ID(i, "line-through")) S->lineThrough = true;
    }
    return true;
}

HANDLER(hDecorationColor) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "currentcolor")) { S->decorationCurrent = true; return true; }
    S->decorationCurrent = false;
    return colorOf(st, v, &S->decorationColor);
}

HANDLER(hDecorationStyle) {
    UNUSED_ARGS;
    if (!v->len) return false;
    CscdSv id = cscdStrSv(&V(0)->tok.text);
    S->decorationStyle = cscdSvIEqC(id, "double") ? CSCD_DECO_DOUBLE : cscdSvIEqC(id, "dotted") ? CSCD_DECO_DOTTED
                       : cscdSvIEqC(id, "dashed") ? CSCD_DECO_DASHED : cscdSvIEqC(id, "wavy") ? CSCD_DECO_WAVY : CSCD_DECO_SOLID;
    return true;
}

HANDLER(hDecorationThickness) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    S->decorationThickness = ID(0, "auto") || ID(0, "from-font") ? 0 : cscdLengthPx(V(0), &st->ctx, 0);
    return true;
}

HANDLER(hTabSize) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    S->tabSize = V(0)->tok.type == CSCD_TOK_NUMBER ? (float)V(0)->tok.num : cscdLengthPx(V(0), &st->ctx, 0) / 8.f;
    return true;
}

HANDLER(hListStyleType) { UNUSED_ARGS; if (!v->len) return false; S->listStyle = (uint8_t)parseListStyle(cscdStrSv(&V(0)->tok.text)); return true; }
HANDLER(hListStylePosition) { UNUSED_ARGS; S->listInside = v->len && ID(0, "inside"); return true; }

HANDLER(hListStyleImage) {
    UNUSED_ARGS;
    cscdStrClear(&S->listImage);
    if (v->len && V(0)->tok.type == CSCD_TOK_URL) {
        CscdStr p = cscdJoinPath(st->baseDir, cscdStrSv(&V(0)->tok.text));
        cscdStrFree(&S->listImage);
        S->listImage = p;
    }
    return true;
}

HANDLER(hLineClamp) { UNUSED_ARGS; S->lineClamp = v->len && V(0)->tok.type == CSCD_TOK_NUMBER ? cscdIntOfDouble(V(0)->tok.num) : 0; return true; }
HANDLER(hDirection) { UNUSED_ARGS; S->rtl = v->len && ID(0, "rtl"); return true; }

/* quotes: the strings of each level, interned (a style only points at them) */
static CscdStrMap quoteLists;
static CscdMutex quoteMutex = CSCD_MUTEX_INIT;

HANDLER(hQuotes) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "auto")) { S->quotes = NULL; return true; }
    CscdStr list = {0};
    if (!(v->len == 1 && ID(0, "none"))) {
        if (!v->len || v->len % 2) return false;
        for (size_t i = 0; i < v->len; ++i) {
            if (V(i)->tok.type != CSCD_TOK_STRING) { cscdStrFree(&list); return false; }
            cscdStrAppend(&list, cscdStrSv(&V(i)->tok.text));
            cscdStrAppendChar(&list, '\0');
        }
    }
    cscdStrAppendChar(&list, '\0');
    cscdMutexLock(&quoteMutex);
    bool inserted = false;
    CscdMapVal* slot = cscdStrMapPut(&quoteLists, cscdStrSv(&list), &inserted);
    if (inserted) {
        slot->p = cscdMalloc(list.len);
        memcpy(slot->p, list.ptr, list.len);
    }
    S->quotes = (const char*)slot->p;
    cscdMutexUnlock(&quoteMutex);
    cscdStrFree(&list);
    return true;
}

HANDLER(hTextUnderlinePosition) {
    UNUSED_ARGS;
    bool under = false;
    for (size_t i = 0; i < v->len; ++i) {
        if (ID(i, "under")) under = true;
        else if (!(ID(i, "auto") || ID(i, "from-font") || ID(i, "left") || ID(i, "right"))) return false;
    }
    S->underlineUnder = under;
    return true;
}

/* text-emphasis-style: none, a string, or [filled | open] and a shape */
static bool emphasisStyle(CscdComputedStyle* s, const Values* v, size_t from, size_t to) {
    static const char* kShapes[] = {"dot", "circle", "double-circle", "triangle", "sesame"};
    if (to - from == 1 && cscdCssIsIdent(&v->items[from], "none")) { s->emphasisShape = 0; return true; }
    if (to - from == 1 && v->items[from].tok.type == CSCD_TOK_STRING) {
        const CscdSv t = cscdStrSv(&v->items[from].tok.text);
        size_t end = 0;
        if (t.len) cscdUtf8Next(t, &end);
        if (!end || end >= sizeof s->emphasisString) return false;
        memset(s->emphasisString, 0, sizeof s->emphasisString);
        memcpy(s->emphasisString, t.ptr, end);
        s->emphasisShape = 6;
        return true;
    }
    int shape = -1, fill = -1;
    for (size_t i = from; i < to; ++i) {
        const CscdCssValue* x = &v->items[i];
        if (cscdCssIsIdent(x, "filled") || cscdCssIsIdent(x, "open")) { if (fill >= 0) return false; fill = cscdCssIsIdent(x, "open"); continue; }
        bool known = false;
        for (size_t k = 0; k < CSCD_COUNT(kShapes); ++k)
            if (cscdCssIsIdent(x, kShapes[k])) { if (shape >= 0) return false; shape = (int)k + 1; known = true; }
        if (!known) return false;
    }
    if (shape < 0 && fill < 0) return false;
    s->emphasisShape = (uint8_t)(shape < 0 ? 1 : shape); /* (filled or open alone: a dot, as for horizontal text) */
    s->emphasisOpen = fill == 1;
    return true;
}

HANDLER(hTextEmphasisStyle) { UNUSED_ARGS; return emphasisStyle(S, v, 0, v->len); }

HANDLER(hTextEmphasisColor) {
    UNUSED_ARGS;
    if (!colorOf(st, v, &S->emphasisColor)) return false;
    S->emphasisColorSet = true;
    return true;
}

HANDLER(hTextEmphasisPosition) {
    UNUSED_ARGS;
    bool under = false;
    for (size_t i = 0; i < v->len; ++i) {
        if (ID(i, "under")) under = true;
        else if (!(ID(i, "over") || ID(i, "right") || ID(i, "left") || ID(i, "auto"))) return false;
    }
    S->emphasisUnder = under;
    return true;
}

/* text-emphasis: the style, then (or before it) a color */
HANDLER(hTextEmphasis) {
    UNUSED_ARGS;
    if (!v->len) return false;
    size_t i = 0;
    CscdColor c;
    bool colored = false;
    if (cscdParseColor(v, &i, &c, S->color) && i < v->len) colored = true;
    else i = 0;
    size_t end = v->len;
    if (!colored) {
        size_t j = v->len - 1;
        CscdColor tail;
        if (v->len > 1 && cscdParseColor(v, &j, &tail, S->color) && j == v->len) { c = tail; colored = true; end = v->len - 1; }
    }
    if (!emphasisStyle(S, v, i, end)) return false;
    S->emphasisColorSet = colored;
    if (colored) S->emphasisColor = c;
    return true;
}

HANDLER(hShapeOutside) { UNUSED_ARGS; cscdStrSet(&S->shapeOutside, cscdSvEqC(raw, "none") ? CSCD_SV("") : raw); return v->len > 0; }

HANDLER(hShapeMargin) {
    UNUSED_ARGS;
    CscdLength l;
    if (v->len != 1 || !cscdParseLength(V(0), &st->ctx, &l, false)) return false;
    S->shapeMargin = cscdMaxf(0.f, l.px);
    return true;
}

HANDLER(hFontSynthesis) {
    UNUSED_ARGS;
    uint8_t allowed = 0;
    if (!(v->len == 1 && ID(0, "none")))
        for (size_t i = 0; i < v->len; ++i) {
            if (ID(i, "weight")) allowed |= 1;
            else if (ID(i, "style")) allowed |= 2;
            else if (ID(i, "small-caps")) allowed |= 4;
            else if (ID(i, "position")) {}
            else return false;
        }
    S->font.noSynthesis = (uint8_t)(~allowed & 7);
    return v->len > 0;
}

HANDLER(hOverscroll) {
    UNUSED_ARGS;
    if (v->len < 1 || v->len > 2) return false;
    uint8_t most = 0;
    for (size_t i = 0; i < v->len; ++i) {
        if (ID(i, "auto")) continue;
        else if (ID(i, "contain")) most = cscdMaxi(most, 1);
        else if (ID(i, "none")) most = 2;
        else return false;
    }
    S->overscroll = most;
    return true;
}

HANDLER(hScrollBehavior) {
    UNUSED_ARGS;
    if (v->len != 1 || !(ID(0, "auto") || ID(0, "smooth"))) return false;
    S->smoothScroll = ID(0, "smooth");
    return true;
}

HANDLER(hColorScheme) {
    UNUSED_ARGS;
    uint8_t bits = 0;
    for (size_t i = 0; i < v->len; ++i) {
        if (ID(i, "light")) bits |= 1;
        else if (ID(i, "dark")) bits |= 2;
        else if (!(ID(i, "normal") || ID(i, "only") || V(i)->tok.type == CSCD_TOK_IDENT)) return false;
    }
    S->colorScheme = bits;
    return v->len > 0;
}

HANDLER(hWritingMode) {
    UNUSED_ARGS;
    static const struct { const char* name; uint8_t mode; } kModes[] = {
        {"horizontal-tb", CSCD_WRITING_HORIZONTAL}, {"vertical-rl", CSCD_WRITING_VERTICAL_RL}, {"vertical-lr", CSCD_WRITING_VERTICAL_LR},
        {"sideways-rl", CSCD_WRITING_SIDEWAYS_RL}, {"sideways-lr", CSCD_WRITING_SIDEWAYS_LR},
        {"lr", CSCD_WRITING_HORIZONTAL}, {"lr-tb", CSCD_WRITING_HORIZONTAL}, {"rl", CSCD_WRITING_HORIZONTAL}, {"rl-tb", CSCD_WRITING_HORIZONTAL},
        {"tb", CSCD_WRITING_VERTICAL_RL}, {"tb-rl", CSCD_WRITING_VERTICAL_RL}};
    if (v->len != 1) return false;
    for (size_t i = 0; i < CSCD_COUNT(kModes); ++i)
        if (ID(0, kModes[i].name)) { S->writingMode = kModes[i].mode; return true; }
    return false;
}

HANDLER(hZoom) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    float z;
    if (ID(0, "normal") || ID(0, "reset")) z = 1;
    else if (V(0)->tok.type == CSCD_TOK_NUMBER && V(0)->tok.num >= 0) z = (float)V(0)->tok.num;
    else if (V(0)->tok.type == CSCD_TOK_PERCENTAGE && V(0)->tok.num >= 0) z = (float)V(0)->tok.num / 100.f;
    else return false;
    S->zoom = z > 0 ? z : 1;
    S->effectiveZoom = (st->parent ? st->parent->effectiveZoom : 1) * S->zoom;
    st->ctx.zoom = S->effectiveZoom;
    return true;
}

HANDLER(hContain) {
    UNUSED_ARGS;
    static const struct { const char* name; uint8_t bits; } kContain[] = {
        {"none", 0}, {"strict", CSCD_CONTAIN_SIZE | CSCD_CONTAIN_LAYOUT | CSCD_CONTAIN_PAINT | CSCD_CONTAIN_STYLE},
        {"content", CSCD_CONTAIN_LAYOUT | CSCD_CONTAIN_PAINT | CSCD_CONTAIN_STYLE}, {"size", CSCD_CONTAIN_SIZE},
        {"inline-size", CSCD_CONTAIN_INLINE_SIZE}, {"layout", CSCD_CONTAIN_LAYOUT}, {"paint", CSCD_CONTAIN_PAINT}, {"style", CSCD_CONTAIN_STYLE}};
    uint8_t bits = 0;
    for (size_t i = 0; i < v->len; ++i) {
        bool known = false;
        for (size_t k = 0; k < CSCD_COUNT(kContain); ++k)
            if (ID(i, kContain[k].name)) { bits |= kContain[k].bits; known = true; }
        if (!known) return false;
    }
    S->contain = (uint8_t)(bits | (S->contentVisibility == 2 ? CSCD_CONTAIN_SIZE | CSCD_CONTAIN_LAYOUT | CSCD_CONTAIN_PAINT | CSCD_CONTAIN_STYLE : 0));
    return v->len > 0;
}

HANDLER(hContentVisibility) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    if (ID(0, "visible")) S->contentVisibility = 0;
    else if (ID(0, "auto")) S->contentVisibility = 1;
    else if (ID(0, "hidden")) S->contentVisibility = 2;
    else return false;
    if (S->contentVisibility == 2) S->contain |= CSCD_CONTAIN_SIZE | CSCD_CONTAIN_LAYOUT | CSCD_CONTAIN_PAINT | CSCD_CONTAIN_STYLE;
    return true;
}

HANDLER(hScrollbarWidth) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    if (ID(0, "auto")) S->scrollbarWidth = 0;
    else if (ID(0, "thin")) S->scrollbarWidth = 1;
    else if (ID(0, "none")) S->scrollbarWidth = 2;
    else return false;
    return true;
}

HANDLER(hEmptyCells) {
    UNUSED_ARGS;
    if (v->len != 1 || !(ID(0, "show") || ID(0, "hide"))) return false;
    S->emptyCellsHide = ID(0, "hide");
    return true;
}

HANDLER(hHyphens) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    if (ID(0, "manual")) S->hyphens = 0;
    else if (ID(0, "none")) S->hyphens = 1;
    else if (ID(0, "auto")) S->hyphens = 2;
    else return false;
    return true;
}

HANDLER(hUnicodeBidi) {
    UNUSED_ARGS;
    static const char* kValues[] = {"normal", "embed", "isolate", "bidi-override", "isolate-override", "plaintext"};
    if (v->len != 1) return false;
    for (size_t i = 0; i < CSCD_COUNT(kValues); ++i)
        if (ID(0, kValues[i]) || (i == CSCD_UNICODE_BIDI_ISOLATE && ID(0, "-webkit-isolate")) || (i == CSCD_UNICODE_BIDI_PLAINTEXT && ID(0, "-webkit-plaintext"))) {
            S->unicodeBidi = (uint8_t)i;
            return true;
        }
    return false;
}

HANDLER(hFlexDirection) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    CscdSv id = cscdStrSv(&V(0)->tok.text);
    S->flexDirection = cscdSvIEqC(id, "row-reverse") ? CSCD_FLEX_ROW_REVERSE : cscdSvIEqC(id, "column") ? CSCD_FLEX_COLUMN
                     : cscdSvIEqC(id, "column-reverse") ? CSCD_FLEX_COLUMN_REVERSE : CSCD_FLEX_ROW;
    return true;
}

HANDLER(hFlexWrap) {
    UNUSED_ARGS;
    if (v->len != 1) return false;
    S->flexWrap = ID(0, "wrap") ? CSCD_FLEX_WRAP : ID(0, "wrap-reverse") ? CSCD_FLEX_WRAP_REVERSE : CSCD_FLEX_NOWRAP;
    return true;
}

#define ALIGN_HANDLER(name, field) \
    HANDLER(name) { UNUSED_ARGS; bool ok; CscdAlign a = parseAlign(v, &ok); if (ok) S->field = (uint8_t)a; return ok; }
ALIGN_HANDLER(hJustifyContent, justifyContent)
ALIGN_HANDLER(hAlignItems, alignItems)
ALIGN_HANDLER(hAlignSelf, alignSelf)
ALIGN_HANDLER(hAlignContent, alignContent)
ALIGN_HANDLER(hJustifyItems, justifyItems)
ALIGN_HANDLER(hJustifySelf, justifySelf)

HANDLER(hFlexGrow) { UNUSED_ARGS; return v->len == 1 && cscdParseNumber(V(0), &S->flexGrow, &st->ctx); }
HANDLER(hFlexShrink) { UNUSED_ARGS; return v->len == 1 && cscdParseNumber(V(0), &S->flexShrink, &st->ctx); }

HANDLER(hFlexBasis) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "content")) { S->flexBasis = cscdLenKind(CSCD_LEN_MAX_CONTENT); return true; }
    return lengthOf(st, v, &S->flexBasis, true, false);
}

HANDLER(hOrder) { UNUSED_ARGS; if (v->len != 1 || V(0)->tok.type != CSCD_TOK_NUMBER) return false; S->order = cscdIntOfDouble(V(0)->tok.num); return true; }

HANDLER(hRowGap) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "normal")) { S->rowGap = cscdLenNone(); return true; }
    return lengthOf(st, v, &S->rowGap, false, false);
}

HANDLER(hColumnGap) {
    UNUSED_ARGS;
    if (v->len == 1 && ID(0, "normal")) { S->columnGap = cscdLenNone(); return true; }
    return lengthOf(st, v, &S->columnGap, false, false);
}

HANDLER(hGridTemplateColumns) { UNUSED_ARGS; return cscdParseTrackList(v, &st->ctx, &S->gridColumns); }
HANDLER(hGridTemplateRows) { UNUSED_ARGS; return cscdParseTrackList(v, &st->ctx, &S->gridRows); }

static bool autoTracks(CscdApplyState* st, const Values* v, CscdTrackVec* out) {
    cscdVecClear(out);
    for (size_t i = 0; i < v->len; ++i) {
        CscdTrackSize t;
        if (!cscdParseTrackSize(V(i), &st->ctx, &t)) return false;
        cscdVecPush(out, t);
    }
    return true;
}
HANDLER(hGridAutoColumns) { UNUSED_ARGS; return autoTracks(st, v, &S->gridAutoColumns); }
HANDLER(hGridAutoRows) { UNUSED_ARGS; return autoTracks(st, v, &S->gridAutoRows); }

HANDLER(hGridAutoFlow) {
    UNUSED_ARGS;
    bool col = false, dense = false;
    for (size_t i = 0; i < v->len; ++i) { col = col || ID(i, "column"); dense = dense || ID(i, "dense"); }
    S->gridAutoFlow = col ? (dense ? CSCD_GRID_COLUMN_DENSE : CSCD_GRID_COLUMN) : (dense ? CSCD_GRID_ROW_DENSE : CSCD_GRID_ROW);
    return true;
}

HANDLER(hGridColumnStart) { UNUSED_ARGS; return parseGridLine(v, &S->gridColumnStart); }
HANDLER(hGridColumnEnd) { UNUSED_ARGS; return parseGridLine(v, &S->gridColumnEnd); }
HANDLER(hGridRowStart) { UNUSED_ARGS; return parseGridLine(v, &S->gridRowStart); }
HANDLER(hGridRowEnd) { UNUSED_ARGS; return parseGridLine(v, &S->gridRowEnd); }

HANDLER(hGridTemplateAreas) {
    UNUSED_ARGS;
    cscdStrVecFree(&S->gridAreas);
    for (size_t i = 0; i < v->len; ++i) if (V(i)->tok.type == CSCD_TOK_STRING) cscdVecPush(&S->gridAreas, cscdStrCopy(&V(i)->tok.text));
    return true;
}

HANDLER(hGridAreaName) { UNUSED_ARGS; cscdStrSet(&S->gridArea, v->len ? cscdStrSv(&V(0)->tok.text) : CSCD_SV("")); return true; }
HANDLER(hBorderCollapse) { UNUSED_ARGS; S->borderCollapse = v->len && ID(0, "collapse") ? CSCD_COLLAPSE_COLLAPSE : CSCD_COLLAPSE_SEPARATE; return true; }

HANDLER(hBorderSpacing) {
    UNUSED_ARGS;
    if (!v->len) return false;
    S->borderSpacingX = cscdLengthPx(V(0), &st->ctx, 0);
    S->borderSpacingY = v->len > 1 ? cscdLengthPx(V(1), &st->ctx, 0) : S->borderSpacingX;
    return true;
}

HANDLER(hContent) {
    UNUSED_ARGS;
    bool ok;
    CscdStr img = {0};
    contentString(st, v, &ok, &S->content, &img);
    cscdStrFree(&S->contentImage);
    S->contentImage = img;
    S->hasContent = ok;
    return true;
}

static float timeOf(const CscdCssValue* x) { return (float)x->tok.num * (cscdStrEqC(&x->tok.text, "ms") ? 0.001f : 1.f); }
static bool isTime(const CscdCssValue* x) { return x->tok.type == CSCD_TOK_DIMENSION && (cscdStrEqC(&x->tok.text, "s") || cscdStrEqC(&x->tok.text, "ms")); }

static CscdTransition newTransition(void) {
    CscdTransition t;
    memset(&t, 0, sizeof t);
    t.x1 = 0.25f; t.y1 = 0.1f; t.x2 = 0.25f; t.y2 = 1;
    return t;
}

static CscdAnimation newAnimation(void) {
    CscdAnimation a;
    memset(&a, 0, sizeof a);
    a.x1 = 0.25f; a.y1 = 0.1f; a.x2 = 0.25f; a.y2 = 1;
    a.iterations = 1;
    return a;
}

static void clearTransitions(CscdTransitionVec* v) {
    for (size_t i = 0; i < v->len; ++i) cscdStrFree(&v->items[i].property);
    v->len = 0;
}

static void clearAnimations(CscdAnimationVec* v) {
    for (size_t i = 0; i < v->len; ++i) cscdStrFree(&v->items[i].name);
    v->len = 0;
}

HANDLER(hTransition) {
    UNUSED_ARGS;
    clearTransitions(&S->transitions);
    CscdCssSlices parts = {0};
    cscdCssSplitCommas(v, &parts);
    for (size_t p = 0; p < parts.len; ++p) {
        CscdTransition t = newTransition();
        t.property = cscdStrDupC("all");
        int times = 0;
        for (size_t k = 0; k < parts.items[p].len; ++k) {
            const CscdCssValue* x = &parts.items[p].items[k];
            if (isTime(x)) {
                float sec = timeOf(x);
                if (times++ == 0) t.duration = sec;
                else t.delay = sec;
            } else if (cscdParseTiming(x, &t.x1, &t.y1, &t.x2, &t.y2, &t.steps)) {
            } else if (x->tok.type == CSCD_TOK_IDENT) {
                cscdStrFree(&t.property);
                t.property = cscdLower(cscdStrSv(&x->tok.text));
            }
        }
        if (!cscdStrEqC(&t.property, "none")) cscdVecPush(&S->transitions, t);
        else cscdStrFree(&t.property);
    }
    cscdVecFree(&parts);
    return true;
}

HANDLER(hTransitionProperty) {
    UNUSED_ARGS;
    CscdCssSlices parts = {0};
    cscdCssSplitCommas(v, &parts);
    while (S->transitions.len < parts.len) cscdVecPush(&S->transitions, newTransition());
    for (size_t i = 0; i < parts.len; ++i)
        if (parts.items[i].len) {
            cscdStrFree(&S->transitions.items[i].property);
            S->transitions.items[i].property = cscdLower(cscdStrSv(&parts.items[i].items[0].tok.text));
        }
    cscdVecFree(&parts);
    return true;
}

HANDLER(hTransitionDuration) {
    UNUSED_ARGS;
    CscdCssSlices parts = {0};
    cscdCssSplitCommas(v, &parts);
    if (!S->transitions.len) {
        CscdTransition t = newTransition();
        t.property = cscdStrDupC("all");
        cscdVecPush(&S->transitions, t);
    }
    for (size_t i = 0; i < S->transitions.len; ++i) {
        const Values* x = &parts.items[i % parts.len];
        if (x->len) S->transitions.items[i].duration = timeOf(&x->items[0]);
    }
    cscdVecFree(&parts);
    return true;
}

HANDLER(hTransitionDelay) {
    UNUSED_ARGS;
    CscdCssSlices parts = {0};
    cscdCssSplitCommas(v, &parts);
    for (size_t i = 0; i < S->transitions.len; ++i) {
        const Values* x = &parts.items[i % parts.len];
        if (x->len) S->transitions.items[i].delay = timeOf(&x->items[0]);
    }
    cscdVecFree(&parts);
    return true;
}

HANDLER(hTransitionTiming) {
    UNUSED_ARGS;
    CscdCssSlices parts = {0};
    cscdCssSplitCommas(v, &parts);
    for (size_t i = 0; i < S->transitions.len; ++i) {
        const Values* x = &parts.items[i % parts.len];
        CscdTransition* t = &S->transitions.items[i];
        if (x->len) cscdParseTiming(&x->items[0], &t->x1, &t->y1, &t->x2, &t->y2, &t->steps);
    }
    cscdVecFree(&parts);
    return true;
}

HANDLER(hAnimation) {
    UNUSED_ARGS;
    clearAnimations(&S->animations);
    CscdCssSlices parts = {0};
    cscdCssSplitCommas(v, &parts);
    for (size_t p = 0; p < parts.len; ++p) {
        CscdAnimation a = newAnimation();
        int times = 0;
        for (size_t k = 0; k < parts.items[p].len; ++k) {
            const CscdCssValue* x = &parts.items[p].items[k];
            if (isTime(x)) {
                float sec = timeOf(x);
                if (times++ == 0) a.duration = sec;
                else a.delay = sec;
            } else if (x->tok.type == CSCD_TOK_NUMBER) a.iterations = (float)x->tok.num;
            else if (cscdCssIsIdent(x, "infinite")) a.iterations = -1;
            else if (cscdCssIsIdent(x, "alternate")) a.alternate = true;
            else if (cscdCssIsIdent(x, "reverse")) a.reverse = true;
            else if (cscdCssIsIdent(x, "alternate-reverse")) { a.alternate = true; a.reverse = true; }
            else if (cscdCssIsIdent(x, "forwards")) a.fillMode = 1;
            else if (cscdCssIsIdent(x, "backwards")) a.fillMode = 2;
            else if (cscdCssIsIdent(x, "both")) a.fillMode = 3;
            else if (cscdCssIsIdent(x, "paused")) a.paused = true;
            else if (cscdCssIsIdent(x, "running") || cscdCssIsIdent(x, "normal")) {}
            else if (cscdParseTiming(x, &a.x1, &a.y1, &a.x2, &a.y2, &a.steps)) {}
            else if (x->tok.type == CSCD_TOK_IDENT || x->tok.type == CSCD_TOK_STRING) cscdStrSet(&a.name, cscdStrSv(&x->tok.text));
        }
        if (a.name.len && !cscdStrEqC(&a.name, "none")) cscdVecPush(&S->animations, a);
        else cscdStrFree(&a.name);
    }
    cscdVecFree(&parts);
    return true;
}

HANDLER(hAnimationName) {
    UNUSED_ARGS;
    CscdCssSlices parts = {0};
    cscdCssSplitCommas(v, &parts);
    while (S->animations.len > parts.len) cscdStrFree(&S->animations.items[--S->animations.len].name);
    while (S->animations.len < parts.len) cscdVecPush(&S->animations, newAnimation());
    for (size_t i = 0; i < parts.len; ++i)
        if (parts.items[i].len) cscdStrSet(&S->animations.items[i].name, cscdStrSv(&parts.items[i].items[0].tok.text));
    size_t w = 0;
    for (size_t i = 0; i < S->animations.len; ++i) {
        CscdAnimation* a = &S->animations.items[i];
        if (!a->name.len || cscdStrEqC(&a->name, "none")) cscdStrFree(&a->name);
        else S->animations.items[w++] = *a;
    }
    S->animations.len = w;
    cscdVecFree(&parts);
    return true;
}

#define ANIMATION_EACH(name, body) \
    HANDLER(name) { \
        UNUSED_ARGS; \
        CscdCssSlices parts = {0}; \
        cscdCssSplitCommas(v, &parts); \
        for (size_t i = 0; i < S->animations.len; ++i) { \
            const Values* x = &parts.items[i % parts.len]; \
            CscdAnimation* a = &S->animations.items[i]; \
            if (!x->len) continue; \
            body; \
        } \
        cscdVecFree(&parts); \
        return true; \
    }
ANIMATION_EACH(hAnimationDuration, a->duration = timeOf(&x->items[0]))
ANIMATION_EACH(hAnimationDelay, a->delay = timeOf(&x->items[0]))
ANIMATION_EACH(hAnimationIterations, a->iterations = cscdCssIsIdent(&x->items[0], "infinite") ? -1 : (float)x->items[0].tok.num)
ANIMATION_EACH(hAnimationTiming, cscdParseTiming(&x->items[0], &a->x1, &a->y1, &a->x2, &a->y2, &a->steps))
ANIMATION_EACH(hAnimationDirection,
               a->alternate = cscdCssIsIdent(&x->items[0], "alternate") || cscdCssIsIdent(&x->items[0], "alternate-reverse");
               a->reverse = cscdCssIsIdent(&x->items[0], "reverse") || cscdCssIsIdent(&x->items[0], "alternate-reverse"))
ANIMATION_EACH(hAnimationFillMode,
               a->fillMode = cscdCssIsIdent(&x->items[0], "forwards") ? 1 : cscdCssIsIdent(&x->items[0], "backwards") ? 2 : cscdCssIsIdent(&x->items[0], "both") ? 3 : 0)
ANIMATION_EACH(hAnimationPlayState, a->paused = cscdCssIsIdent(&x->items[0], "paused"))

HANDLER(hAppearance) { UNUSED_ARGS; S->appearanceNone = v->len == 1 && ID(0, "none"); return true; }

typedef struct Entry { const char* name; Handler fn; } Entry;

static const Entry kHandlers[] = {
    {"display", hDisplay}, {"position", hPosition}, {"float", hFloat}, {"clear", hClear}, {"box-sizing", hBoxSizing},
    {"width", hWidth}, {"height", hHeight}, {"min-width", hMinWidth}, {"min-height", hMinHeight}, {"max-width", hMaxWidth},
    {"max-height", hMaxHeight}, {"top", hTop}, {"right", hRight}, {"bottom", hBottom}, {"left", hLeft},
    {"margin-top", hMarginTop}, {"margin-right", hMarginRight}, {"margin-bottom", hMarginBottom}, {"margin-left", hMarginLeft},
    {"padding-top", hPaddingTop}, {"padding-right", hPaddingRight}, {"padding-bottom", hPaddingBottom}, {"padding-left", hPaddingLeft},
    {"border-top-width", hBorderTopWidth}, {"border-top-style", hBorderTopStyle}, {"border-top-color", hBorderTopColor},
    {"border-right-width", hBorderRightWidth}, {"border-right-style", hBorderRightStyle}, {"border-right-color", hBorderRightColor},
    {"border-bottom-width", hBorderBottomWidth}, {"border-bottom-style", hBorderBottomStyle}, {"border-bottom-color", hBorderBottomColor},
    {"border-left-width", hBorderLeftWidth}, {"border-left-style", hBorderLeftStyle}, {"border-left-color", hBorderLeftColor},
    {"border-top-left-radius", hRadiusTL}, {"border-top-right-radius", hRadiusTR}, {"border-bottom-right-radius", hRadiusBR},
    {"border-bottom-left-radius", hRadiusBL}, {"aspect-ratio", hAspectRatio}, {"z-index", hZIndex}, {"overflow-x", hOverflowX},
    {"overflow-y", hOverflowY}, {"visibility", hVisibility}, {"opacity", hOpacity}, {"color", hColor},
    {"background-color", hBackgroundColor}, {"background-image", hBackgroundImage}, {"background-position", hBackgroundPosition},
    {"background-position-x", hBackgroundPositionX}, {"background-size", hBackgroundSize}, {"background-repeat", hBackgroundRepeat},
    {"background-clip", hBackgroundClip}, {"-webkit-background-clip", hBackgroundClip}, {"background-origin", hBackgroundOrigin},
    {"background-attachment", hAccept}, {"box-shadow", hBoxShadow}, {"text-shadow", hTextShadow}, {"outline-width", hOutlineWidth},
    {"outline-style", hOutlineStyle}, {"outline-color", hOutlineColor}, {"outline-offset", hOutlineOffset}, {"transform", hTransform},
    {"translate", hTranslate}, {"rotate", hRotate}, {"scale", hScale}, {"transform-origin", hTransformOrigin},
    {"perspective", hPerspective}, {"perspective-origin", hPerspectiveOrigin}, {"container-type", hContainerType},
    {"container-name", hContainerName}, {"container", hContainer}, {"counter-reset", hCounterReset}, {"counter-set", hCounterSet},
    {"counter-increment", hCounterIncrement}, {"backface-visibility", hBackfaceVisibility}, {"transform-style", hTransformStyle},
    {"cursor", hCursor}, {"pointer-events", hPointerEvents}, {"user-select", hUserSelect}, {"touch-action", hTouchAction}, {"accent-color", hAccentColor},
    {"caret-color", hCaretColor}, {"mix-blend-mode", hMixBlendMode}, {"text-align-last", hTextAlignLast},
    {"-webkit-text-fill-color", hTextFillColor}, {"-webkit-text-stroke-width", hTextStrokeWidth},
    {"-webkit-text-stroke-color", hTextStrokeColor}, {"-webkit-text-stroke", hTextStroke}, {"image-rendering", hImageRendering},
    {"isolation", hIsolation}, {"table-layout", hTableLayout}, {"caption-side", hCaptionSide}, {"backdrop-filter", hBackdropFilter},
    {"-webkit-backdrop-filter", hBackdropFilter}, {"mask-image", hMaskImage}, {"mask-position", hMaskPosition}, {"mask-size", hMaskSize},
    {"mask-repeat", hMaskRepeat}, {"mask", hMask}, {"-webkit-mask-image", hMaskImage}, {"-webkit-mask-position", hMaskPosition},
    {"-webkit-mask-size", hMaskSize}, {"-webkit-mask-repeat", hMaskRepeat}, {"-webkit-mask", hMask}, {"filter", hFilter},
    {"object-fit", hObjectFit}, {"object-position", hObjectPosition}, {"fill", hFill}, {"stroke", hStroke},
    {"stroke-width", hStrokeWidth}, {"scrollbar-color", hScrollbarColor}, {"clip-path", hClipPath}, {"font-family", hFontFamily},
    {"font-size", hFontSize}, {"font-weight", hFontWeight}, {"font-style", hFontStyle}, {"font-variant", hFontVariant},
    {"font-variant-numeric", hFontVariantNumeric}, {"font-variant-ligatures", hFontVariantLigatures},
    {"text-emphasis", hTextEmphasis}, {"text-emphasis-style", hTextEmphasisStyle}, {"text-emphasis-color", hTextEmphasisColor},
    {"text-emphasis-position", hTextEmphasisPosition}, {"-webkit-text-emphasis", hTextEmphasis},
    {"-webkit-text-emphasis-style", hTextEmphasisStyle}, {"-webkit-text-emphasis-color", hTextEmphasisColor},
    {"-webkit-text-emphasis-position", hTextEmphasisPosition}, {"unicode-bidi", hUnicodeBidi}, {"hyphens", hHyphens}, {"-webkit-hyphens", hHyphens}, {"font-variant-caps", hFontVariantCaps}, {"font-variant-position", hFontVariantPosition},
    {"font-variant-east-asian", hFontVariantEastAsian}, {"font-kerning", hFontKerning}, {"font-optical-sizing", hFontOpticalSizing},
    {"font-stretch", hFontStretch}, {"font-feature-settings", hFontFeatureSettings}, {"font-variation-settings", hFontVariationSettings}, {"line-height", hLineHeight}, {"letter-spacing", hLetterSpacing},
    {"word-spacing", hWordSpacing}, {"text-align", hTextAlign}, {"text-indent", hTextIndent}, {"white-space", hWhiteSpace},
    {"text-wrap-mode", hAccept}, {"text-transform", hTextTransform}, {"text-overflow", hTextOverflow}, {"word-break", hWordBreak},
    {"overflow-wrap", hOverflowWrap}, {"vertical-align", hVerticalAlign}, {"text-decoration-line", hDecorationLine},
    {"text-decoration-color", hDecorationColor}, {"text-decoration-style", hDecorationStyle},
    {"text-decoration-thickness", hDecorationThickness}, {"text-underline-offset", hAccept}, {"tab-size", hTabSize},
    {"list-style-type", hListStyleType}, {"list-style-position", hListStylePosition}, {"list-style-image", hListStyleImage},
    {"line-clamp", hLineClamp}, {"direction", hDirection}, {"flex-direction", hFlexDirection}, {"flex-wrap", hFlexWrap},
    {"justify-content", hJustifyContent}, {"align-items", hAlignItems}, {"align-self", hAlignSelf}, {"align-content", hAlignContent},
    {"justify-items", hJustifyItems}, {"justify-self", hJustifySelf}, {"flex-grow", hFlexGrow}, {"flex-shrink", hFlexShrink},
    {"flex-basis", hFlexBasis}, {"order", hOrder}, {"row-gap", hRowGap}, {"column-gap", hColumnGap},
    {"grid-template-columns", hGridTemplateColumns}, {"grid-template-rows", hGridTemplateRows}, {"grid-auto-columns", hGridAutoColumns},
    {"grid-auto-rows", hGridAutoRows}, {"grid-auto-flow", hGridAutoFlow}, {"grid-column-start", hGridColumnStart},
    {"grid-column-end", hGridColumnEnd}, {"grid-row-start", hGridRowStart}, {"grid-row-end", hGridRowEnd},
    {"grid-template-areas", hGridTemplateAreas}, {"grid-area-name", hGridAreaName}, {"border-collapse", hBorderCollapse},
    {"border-spacing", hBorderSpacing}, {"content", hContent}, {"transition", hTransition}, {"transition-property", hTransitionProperty},
    {"transition-duration", hTransitionDuration}, {"transition-delay", hTransitionDelay}, {"transition-timing-function", hTransitionTiming},
    {"animation", hAnimation}, {"animation-name", hAnimationName}, {"animation-duration", hAnimationDuration},
    {"animation-delay", hAnimationDelay}, {"animation-iteration-count", hAnimationIterations},
    {"animation-timing-function", hAnimationTiming}, {"animation-direction", hAnimationDirection},
    {"animation-fill-mode", hAnimationFillMode}, {"animation-play-state", hAnimationPlayState}, {"appearance", hAppearance},
    /* accepted and ignored: no visual effect in this engine */
    {"font-smoothing", hAccept}, {"-moz-osx-font-smoothing", hAccept}, {"text-rendering", hAccept}, {"will-change", hAccept},
    {"contain", hContain},
    {"font-display", hAccept}, {"resize", hAccept}, {"scroll-behavior", hScrollBehavior},
    {"overscroll-behavior", hOverscroll}, {"overscroll-behavior-x", hOverscroll}, {"overscroll-behavior-y", hOverscroll},
    {"overscroll-behavior-block", hOverscroll}, {"overscroll-behavior-inline", hOverscroll}, {"-webkit-tap-highlight-color", hAccept}, {"text-size-adjust", hAccept},
    {"-webkit-text-size-adjust", hAccept}, {"speak", hAccept}, {"box-decoration-break", hAccept},
    {"orphans", hAccept}, {"widows", hAccept}, {"page-break-inside", hAccept}, {"break-inside", hAccept},
    {"quotes", hQuotes}, {"content-visibility", hContentVisibility}, {"color-scheme", hColorScheme}, {"forced-color-adjust", hAccept},
    {"print-color-adjust", hAccept}, {"-webkit-print-color-adjust", hAccept}, {"scrollbar-width", hScrollbarWidth},
    {"scrollbar-gutter", hAccept}, {"empty-cells", hEmptyCells}, {"shape-outside", hShapeOutside}, {"shape-margin", hShapeMargin}, {"-webkit-box-orient", hAccept},
    {"box-orient", hAccept},
    {"text-underline-position", hTextUnderlinePosition}, {"overflow-anchor", hAccept}, {"zoom", hZoom},
    {"writing-mode", hWritingMode}, {"-webkit-writing-mode", hWritingMode}, {"text-justify", hAccept}, {"line-break", hAccept}, {"word-wrap-mode", hAccept},
    {"font-synthesis", hFontSynthesis}, {"inset-area", hAccept},
};

static CscdStrMap handlerMap;
static CscdOnce handlerOnce = CSCD_ONCE_INIT;
static void initHandlers(void) {
    for (size_t i = 0; i < CSCD_COUNT(kHandlers); ++i) cscdStrMapPut(&handlerMap, cscdSv(kHandlers[i].name), NULL)->p = (void*)(uintptr_t)kHandlers[i].fn;
}

bool cscdIsKnownProperty(CscdSv property) {
    if (cscdSvStartsC(property, "--")) return true;
    property = cscdCanonicalProperty(property);
    cscdCallOnce(&handlerOnce, initHandlers);
    return cscdStrMapGet(&handlerMap, property) || cscdIsShorthand(property);
}

/* Edit distance with transpositions (optimal string alignment), stopping above limit. */
static int editDistance(CscdSv a, CscdSv b, int limit) {
    if (a.len > 63 || b.len > 63) return limit + 1;
    const int n = (int)a.len, m = (int)b.len;
    if (abs(n - m) > limit) return limit + 1;
    int d[64][64];
    for (int i = 0; i <= n; ++i) d[i][0] = i;
    for (int j = 0; j <= m; ++j) d[0][j] = j;
    for (int i = 1; i <= n; ++i) {
        int best = limit + 1;
        for (int j = 1; j <= m; ++j) {
            const int cost = a.ptr[i - 1] != b.ptr[j - 1];
            int v = cscdMini(cscdMini(d[i - 1][j] + 1, d[i][j - 1] + 1), d[i - 1][j - 1] + cost);
            if (i > 1 && j > 1 && a.ptr[i - 1] == b.ptr[j - 2] && a.ptr[i - 2] == b.ptr[j - 1]) v = cscdMini(v, d[i - 2][j - 2] + 1);
            d[i][j] = v;
            best = cscdMini(best, v);
        }
        if (best > limit) return limit + 1;
    }
    return d[n][m];
}

CscdSv cscdClosestProperty(CscdSv property) {
    const int limit = property.len <= 4 ? 1 : 2;
    int best = limit + 1;
    CscdSv found = CSCD_SV("");
#define TRY(name) do { const CscdSv c_ = cscdSv(name); const int e_ = editDistance(property, c_, limit); if (e_ < best) { best = e_; found = c_; } } while (0)
    for (size_t i = 0; i < CSCD_COUNT(kHandlers); ++i) TRY(kHandlers[i].name);
    for (size_t i = 0; i < CSCD_COUNT(kShorthands); ++i) TRY(kShorthands[i].name);
    for (size_t i = 0; i < CSCD_COUNT(kCanonical); ++i) TRY(kCanonical[i][0]);
#undef TRY
    return found;
}

void cscdValueCacheClear(CscdStrMap* cache) {
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(cache, s));) {
        cscdCssValueVecFree((Values*)s->val.p);
        cscdFree(s->val.p);
    }
    cscdStrMapClear(cache);
}

/* a value's tokens, parsed once per text: the same declarations are applied to many styles */
static const Values* cachedValues(CscdStrMap* cache, CscdSv value) {
    CscdMapVal* m = cscdStrMapGet(cache, value);
    if (m) return (const Values*)m->p;
    if (cache->len >= 4096) cscdValueCacheClear(cache); /* (values made by var()s that keep changing) */
    Values* v = CSCD_NEW(Values);
    cscdCssParseValues(value, false, v);
    cscdStrMapPut(cache, value, NULL)->p = v;
    return v;
}

bool cscdApplyLonghand(CscdApplyState* st, CscdSv property, CscdSv value) {
    cscdCallOnce(&handlerOnce, initHandlers);
    CscdMapVal* h = cscdStrMapGet(&handlerMap, property);
    if (!h) return false;
    Values own = {0};
    const Values* v = &own;
    if (st->valueCache) v = cachedValues(st->valueCache, value);
    else cscdCssParseValues(value, false, &own);
    bool ok = false;
    if (v->len || cscdSvEqC(property, "content") || cscdSvEqC(property, "font-family")) ok = ((Handler)(uintptr_t)h->p)(st, v, value);
    cscdCssValueVecFree(&own);
    return ok;
}

#undef S
#undef V
#undef ID

void cscdFinalizeStyle(CscdApplyState* st) {
    CscdComputedStyle* s = st->s;
    cscdParseBackgrounds(st);
    if (st->maskTouched) {
        /* mask layers take the background syntax (images, gradients, position, size, repeat; from the
         * border box): parsed by the same code */
        CscdLayerVec backgrounds = s->backgrounds;
        memset(&s->backgrounds, 0, sizeof s->backgrounds);
        CscdStr saved[6] = {st->bgImage, st->bgPosition, st->bgSize, st->bgRepeat, st->bgClip, st->bgOrigin};
        const bool touched = st->bgTouched;
        st->bgImage = cscdStrCopy(&st->maskImage);
        st->bgPosition = cscdStrCopy(&st->maskPosition);
        st->bgSize = cscdStrCopy(&st->maskSize);
        st->bgRepeat = cscdStrCopy(&st->maskRepeat);
        st->bgClip = cscdStrDupC("border-box");
        st->bgOrigin = cscdStrDupC("border-box");
        st->bgTouched = true;
        cscdParseBackgrounds(st);
        cscdLayerVecFree(&s->masks);
        s->masks = s->backgrounds;
        s->backgrounds = backgrounds;
        cscdStrFree(&st->bgImage); cscdStrFree(&st->bgPosition); cscdStrFree(&st->bgSize);
        cscdStrFree(&st->bgRepeat); cscdStrFree(&st->bgClip); cscdStrFree(&st->bgOrigin);
        st->bgImage = saved[0]; st->bgPosition = saved[1]; st->bgSize = saved[2];
        st->bgRepeat = saved[3]; st->bgClip = saved[4]; st->bgOrigin = saved[5];
        st->bgTouched = touched;
    }
    for (int i = 0; i < 4; ++i) {
        if (!(st->colorSet & (1u << i))) s->border[i].color = s->color;
        if (s->border[i].style == CSCD_BORDER_NONE || s->border[i].style == CSCD_BORDER_HIDDEN) s->border[i].width = 0;
    }
    if (!(st->colorSet & 16)) s->outlineColor = s->color;
    if (s->outlineStyle == CSCD_BORDER_NONE) s->outlineWidth = 0;
    if (s->decorationCurrent) s->decorationColor = s->color;
    if (s->caretAuto) s->caretColor = s->color;
    if (s->fillCurrent) s->fill = s->color;
    if (s->strokeCurrent) s->stroke = s->color;
    /* overflow: visible paired with a clipping value computes to auto */
    if (s->overflowX != CSCD_OVERFLOW_VISIBLE && s->overflowY == CSCD_OVERFLOW_VISIBLE) s->overflowY = CSCD_OVERFLOW_AUTO;
    if (s->overflowY != CSCD_OVERFLOW_VISIBLE && s->overflowX == CSCD_OVERFLOW_VISIBLE) s->overflowX = CSCD_OVERFLOW_AUTO;
}

/* ---- copying one property -------------------------------------------------------------------------- */

typedef void (*Copier)(CscdComputedStyle* d, const CscdComputedStyle* s);

#define CP(fn, field) static void fn(CscdComputedStyle* d, const CscdComputedStyle* s) { d->field = s->field; }
CP(cDisplay, display) CP(cPosition, position) CP(cFloat, floating) CP(cClear, clear) CP(cBoxSizing, boxSizing)
CP(cWidth, width) CP(cHeight, height) CP(cMinWidth, minWidth) CP(cMinHeight, minHeight) CP(cMaxWidth, maxWidth) CP(cMaxHeight, maxHeight)
CP(cTop, top) CP(cRight, right) CP(cBottom, bottom) CP(cLeft, left)
CP(cMarginTop, margin[0]) CP(cMarginRight, margin[1]) CP(cMarginBottom, margin[2]) CP(cMarginLeft, margin[3])
CP(cPaddingTop, padding[0]) CP(cPaddingRight, padding[1]) CP(cPaddingBottom, padding[2]) CP(cPaddingLeft, padding[3])
CP(cBTW, border[0].width) CP(cBRW, border[1].width) CP(cBBW, border[2].width) CP(cBLW, border[3].width)
CP(cBTS, border[0].style) CP(cBRS, border[1].style) CP(cBBS, border[2].style) CP(cBLS, border[3].style)
CP(cBTC, border[0].color) CP(cBRC, border[1].color) CP(cBBC, border[2].color) CP(cBLC, border[3].color)
CP(cR0, radius[0]) CP(cR1, radius[1]) CP(cR2, radius[2]) CP(cR3, radius[3])
CP(cAspectRatio, aspectRatio) CP(cOverflowX, overflowX) CP(cOverflowY, overflowY) CP(cVisibility, visibility)
CP(cOpacity, opacity) CP(cColor, color) CP(cBackgroundColor, backgroundColor)
CP(cOutlineWidth, outlineWidth) CP(cOutlineStyle, outlineStyle) CP(cOutlineColor, outlineColor) CP(cOutlineOffset, outlineOffset)
CP(cCursor, cursor) CP(cPointerEvents, pointerEvents) CP(cUserSelect, userSelect) CP(cTouchAction, touchAction) CP(cAccentColor, accentColor) CP(cCaretColor, caretColor)
CP(cObjectFit, objectFit) CP(cFill, fill) CP(cStroke, stroke) CP(cStrokeWidth, strokeWidth)
CP(cFontSize, font.size) CP(cFontWeight, font.weight) CP(cFontStyle, font.italic)
CP(cFontVariantPosition, font.position) CP(cFontVariantEastAsian, font.eastAsian) CP(cFontKerning, font.kerning)
CP(cFontOpticalSizing, font.noOpticalSizing) CP(cFontStretch, font.stretch) CP(cFontFeatureSettings, font.featureSettings)
CP(cFontVariationSettings, font.variationSettings) CP(cContain, contain) CP(cContentVisibility, contentVisibility) CP(cScrollbarWidth, scrollbarWidth) CP(cEmptyCells, emptyCellsHide)
CP(cUnderlineUnder, underlineUnder) CP(cEmphasisPosition, emphasisUnder) CP(cUnicodeBidi, unicodeBidi) CP(cWritingMode, writingMode) CP(cOverscroll, overscroll) CP(cScrollBehavior, smoothScroll)
CP(cColorScheme, colorScheme) CP(cFontSynthesis, font.noSynthesis) CP(cHyphens, hyphens) CP(cQuotes, quotes)
CP(cLetterSpacing, letterSpacing) CP(cWordSpacing, wordSpacing) CP(cTextAlign, textAlign) CP(cTextIndent, textIndent)
CP(cWhiteSpace, whiteSpace) CP(cTextTransform, textTransform) CP(cTextOverflow, textOverflow) CP(cWordBreak, wordBreak)
CP(cOverflowWrap, overflowWrap) CP(cVerticalAlign, verticalAlign) CP(cDecorationColor, decorationColor)
CP(cDecorationStyle, decorationStyle) CP(cTabSize, tabSize) CP(cListStyle, listStyle) CP(cListInside, listInside)
CP(cFlexDirection, flexDirection) CP(cFlexWrap, flexWrap) CP(cJustifyContent, justifyContent) CP(cAlignItems, alignItems)
CP(cAlignSelf, alignSelf) CP(cAlignContent, alignContent) CP(cJustifyItems, justifyItems) CP(cJustifySelf, justifySelf)
CP(cFlexGrow, flexGrow) CP(cFlexShrink, flexShrink) CP(cFlexBasis, flexBasis) CP(cOrder, order) CP(cRowGap, rowGap) CP(cColumnGap, columnGap)
CP(cGridAutoFlow, gridAutoFlow) CP(cBorderCollapse, borderCollapse) CP(cLineClamp, lineClamp) CP(cDirection, rtl)
CP(cPerspective, perspective) CP(cBackfaceVisibility, backfaceHidden) CP(cTransformStyle, preserve3d) CP(cContainerType, containerType)
CP(cBackdropFilter, backdrop) CP(cImageRendering, pixelated) CP(cIsolation, isolate) CP(cTableLayout, tableFixed)
CP(cCaptionSide, captionBottom) CP(cTextStrokeWidth, textStrokeWidth)
#undef CP

static void cBackgroundImage(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdLayerVecCopy(&d->backgrounds, &s->backgrounds); }
static void cMaskImage(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdLayerVecCopy(&d->masks, &s->masks); }
static void cBoxShadow(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdVecCopy(&d->boxShadow, &s->boxShadow); }
static void cTextShadow(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdVecCopy(&d->textShadow, &s->textShadow); }
static void cTransform(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdVecCopy(&d->transform, &s->transform); }
static void cEmphasisStyle(CscdComputedStyle* d, const CscdComputedStyle* s) {
    d->emphasisShape = s->emphasisShape;
    d->emphasisOpen = s->emphasisOpen;
    memcpy(d->emphasisString, s->emphasisString, sizeof d->emphasisString);
}
static void cEmphasisColor(CscdComputedStyle* d, const CscdComputedStyle* s) { d->emphasisColor = s->emphasisColor; d->emphasisColorSet = s->emphasisColorSet; }
static void cFontVariantCaps(CscdComputedStyle* d, const CscdComputedStyle* s) { d->font.caps = s->font.caps; d->font.smallCaps = s->font.smallCaps; }
static void cFontVariantNumeric(CscdComputedStyle* d, const CscdComputedStyle* s) { d->font.numeric = s->font.numeric; d->font.tabularNums = s->font.tabularNums; }
static void cFontVariantLigatures(CscdComputedStyle* d, const CscdComputedStyle* s) { d->font.ligatures = s->font.ligatures; d->font.noLigatures = s->font.noLigatures; }
static void cFontVariant(CscdComputedStyle* d, const CscdComputedStyle* s) {
    cFontVariantCaps(d, s);
    cFontVariantNumeric(d, s);
    cFontVariantLigatures(d, s);
    d->font.position = s->font.position;
    d->font.eastAsian = s->font.eastAsian;
}
static void cFontFamily(CscdComputedStyle* d, const CscdComputedStyle* s) {
    cscdStrVecCopy(&d->font.families, &s->font.families);
    memset(&d->resolvedFont, 0, sizeof d->resolvedFont);
}
static void cListImage(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdStrSet(&d->listImage, cscdStrSv(&s->listImage)); }
static void cGridColumns(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdTrackListCopy(&d->gridColumns, &s->gridColumns); }
static void cGridRows(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdTrackListCopy(&d->gridRows, &s->gridRows); }
static void cGridAutoColumns(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdVecCopy(&d->gridAutoColumns, &s->gridAutoColumns); }
static void cGridAutoRows(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdVecCopy(&d->gridAutoRows, &s->gridAutoRows); }
static void copyLine(CscdGridLine* d, const CscdGridLine* s) { d->line = s->line; d->span = s->span; cscdStrSet(&d->name, cscdStrSv(&s->name)); }
static void cGridColumnStart(CscdComputedStyle* d, const CscdComputedStyle* s) { copyLine(&d->gridColumnStart, &s->gridColumnStart); }
static void cGridColumnEnd(CscdComputedStyle* d, const CscdComputedStyle* s) { copyLine(&d->gridColumnEnd, &s->gridColumnEnd); }
static void cGridRowStart(CscdComputedStyle* d, const CscdComputedStyle* s) { copyLine(&d->gridRowStart, &s->gridRowStart); }
static void cGridRowEnd(CscdComputedStyle* d, const CscdComputedStyle* s) { copyLine(&d->gridRowEnd, &s->gridRowEnd); }
static void cGridAreas(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdStrVecCopy(&d->gridAreas, &s->gridAreas); }
static void cGridArea(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdStrSet(&d->gridArea, cscdStrSv(&s->gridArea)); }
static void cContent(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdStrSet(&d->content, cscdStrSv(&s->content)); }
static void cTransitions(CscdComputedStyle* d, const CscdComputedStyle* s) {
    clearTransitions(&d->transitions);
    for (size_t i = 0; i < s->transitions.len; ++i) {
        CscdTransition t = s->transitions.items[i];
        t.property = cscdStrCopy(&s->transitions.items[i].property);
        cscdVecPush(&d->transitions, t);
    }
}
static void cAnimations(CscdComputedStyle* d, const CscdComputedStyle* s) {
    clearAnimations(&d->animations);
    for (size_t i = 0; i < s->animations.len; ++i) {
        CscdAnimation a = s->animations.items[i];
        a.name = cscdStrCopy(&s->animations.items[i].name);
        cscdVecPush(&d->animations, a);
    }
}
static void copyCounters(CscdCounterVec* d, const CscdCounterVec* s) {
    for (size_t i = 0; i < d->len; ++i) cscdStrFree(&d->items[i].name);
    d->len = 0;
    for (size_t i = 0; i < s->len; ++i) {
        CscdCounter c = {cscdStrCopy(&s->items[i].name), s->items[i].value};
        cscdVecPush(d, c);
    }
}
static void cCounterReset(CscdComputedStyle* d, const CscdComputedStyle* s) { copyCounters(&d->counterReset, &s->counterReset); }
static void cCounterSet(CscdComputedStyle* d, const CscdComputedStyle* s) { copyCounters(&d->counterSet, &s->counterSet); }
static void cCounterIncrement(CscdComputedStyle* d, const CscdComputedStyle* s) { copyCounters(&d->counterIncrement, &s->counterIncrement); }
static void cContainerName(CscdComputedStyle* d, const CscdComputedStyle* s) { cscdStrVecCopy(&d->containerNames, &s->containerNames); }
static void cScrollbarColor(CscdComputedStyle* d, const CscdComputedStyle* s) { d->scrollbarThumb = s->scrollbarThumb; d->scrollbarThumbSet = s->scrollbarThumbSet; }
static void cLineHeight(CscdComputedStyle* d, const CscdComputedStyle* s) { d->lineHeight = s->lineHeight; d->lineHeightFactor = s->lineHeightFactor; }
static void cZIndex(CscdComputedStyle* d, const CscdComputedStyle* s) { d->zIndex = s->zIndex; d->zAuto = s->zAuto; }
static void cDecorationLine(CscdComputedStyle* d, const CscdComputedStyle* s) { d->underline = s->underline; d->overline = s->overline; d->lineThrough = s->lineThrough; }
static void cTransformOrigin(CscdComputedStyle* d, const CscdComputedStyle* s) { d->originX = s->originX; d->originY = s->originY; d->originZ = s->originZ; }
static void cPerspectiveOrigin(CscdComputedStyle* d, const CscdComputedStyle* s) { d->perspectiveX = s->perspectiveX; d->perspectiveY = s->perspectiveY; }
static void cTextAlignLast(CscdComputedStyle* d, const CscdComputedStyle* s) { d->textAlignLast = s->textAlignLast; d->alignLastSet = s->alignLastSet; }
static void cTextFill(CscdComputedStyle* d, const CscdComputedStyle* s) { d->textFill = s->textFill; d->textFillSet = s->textFillSet; }
static void cTextStroke(CscdComputedStyle* d, const CscdComputedStyle* s) { d->textStrokeWidth = s->textStrokeWidth; d->textStroke = s->textStroke; d->textStrokeSet = s->textStrokeSet; }
static void cTextStrokeColor(CscdComputedStyle* d, const CscdComputedStyle* s) { d->textStroke = s->textStroke; d->textStrokeSet = s->textStrokeSet; }
static void cBorderSpacing(CscdComputedStyle* d, const CscdComputedStyle* s) { d->borderSpacingX = s->borderSpacingX; d->borderSpacingY = s->borderSpacingY; }
static void cFilter(CscdComputedStyle* d, const CscdComputedStyle* s) {
    d->blur = s->blur; d->brightness = s->brightness; d->grayscale = s->grayscale; d->saturate = s->saturate; d->invert = s->invert;
}

typedef struct CopyEntry { const char* name; Copier fn; } CopyEntry;

static const CopyEntry kCopiers[] = {
    {"display", cDisplay}, {"position", cPosition}, {"float", cFloat}, {"clear", cClear}, {"box-sizing", cBoxSizing},
    {"width", cWidth}, {"height", cHeight}, {"min-width", cMinWidth}, {"min-height", cMinHeight}, {"max-width", cMaxWidth},
    {"max-height", cMaxHeight}, {"top", cTop}, {"right", cRight}, {"bottom", cBottom}, {"left", cLeft},
    {"margin-top", cMarginTop}, {"margin-right", cMarginRight}, {"margin-bottom", cMarginBottom}, {"margin-left", cMarginLeft},
    {"padding-top", cPaddingTop}, {"padding-right", cPaddingRight}, {"padding-bottom", cPaddingBottom}, {"padding-left", cPaddingLeft},
    {"border-top-width", cBTW}, {"border-right-width", cBRW}, {"border-bottom-width", cBBW}, {"border-left-width", cBLW},
    {"border-top-style", cBTS}, {"border-right-style", cBRS}, {"border-bottom-style", cBBS}, {"border-left-style", cBLS},
    {"border-top-color", cBTC}, {"border-right-color", cBRC}, {"border-bottom-color", cBBC}, {"border-left-color", cBLC},
    {"border-top-left-radius", cR0}, {"border-top-right-radius", cR1}, {"border-bottom-right-radius", cR2}, {"border-bottom-left-radius", cR3},
    {"aspect-ratio", cAspectRatio}, {"overflow-x", cOverflowX}, {"overflow-y", cOverflowY}, {"visibility", cVisibility},
    {"opacity", cOpacity}, {"color", cColor}, {"background-color", cBackgroundColor}, {"background-image", cBackgroundImage},
    {"box-shadow", cBoxShadow}, {"text-shadow", cTextShadow}, {"outline-width", cOutlineWidth}, {"outline-style", cOutlineStyle},
    {"outline-color", cOutlineColor}, {"outline-offset", cOutlineOffset}, {"transform", cTransform}, {"cursor", cCursor},
    {"pointer-events", cPointerEvents}, {"user-select", cUserSelect}, {"touch-action", cTouchAction}, {"accent-color", cAccentColor}, {"caret-color", cCaretColor},
    {"object-fit", cObjectFit}, {"fill", cFill}, {"stroke", cStroke}, {"stroke-width", cStrokeWidth}, {"font-family", cFontFamily},
    {"font-size", cFontSize}, {"font-weight", cFontWeight}, {"font-style", cFontStyle}, {"font-variant", cFontVariant},
    {"font-variant-numeric", cFontVariantNumeric}, {"font-variant-ligatures", cFontVariantLigatures},
    {"font-variant-caps", cFontVariantCaps}, {"font-variant-position", cFontVariantPosition}, {"font-variant-east-asian", cFontVariantEastAsian},
    {"contain", cContain}, {"content-visibility", cContentVisibility}, {"scrollbar-width", cScrollbarWidth},
    {"empty-cells", cEmptyCells}, {"text-underline-position", cUnderlineUnder}, {"text-emphasis-position", cEmphasisPosition},
    {"text-emphasis-style", cEmphasisStyle}, {"text-emphasis-color", cEmphasisColor}, {"unicode-bidi", cUnicodeBidi}, {"writing-mode", cWritingMode},
    {"overscroll-behavior", cOverscroll}, {"scroll-behavior", cScrollBehavior}, {"color-scheme", cColorScheme}, {"font-synthesis", cFontSynthesis}, {"hyphens", cHyphens}, {"quotes", cQuotes}, {"font-kerning", cFontKerning}, {"font-optical-sizing", cFontOpticalSizing}, {"font-stretch", cFontStretch},
    {"font-feature-settings", cFontFeatureSettings}, {"font-variation-settings", cFontVariationSettings},
    {"letter-spacing", cLetterSpacing}, {"word-spacing", cWordSpacing}, {"text-align", cTextAlign}, {"text-indent", cTextIndent},
    {"white-space", cWhiteSpace}, {"text-transform", cTextTransform}, {"text-overflow", cTextOverflow}, {"word-break", cWordBreak},
    {"overflow-wrap", cOverflowWrap}, {"vertical-align", cVerticalAlign}, {"text-decoration-color", cDecorationColor},
    {"text-decoration-style", cDecorationStyle}, {"tab-size", cTabSize}, {"list-style-type", cListStyle},
    {"list-style-position", cListInside}, {"list-style-image", cListImage}, {"flex-direction", cFlexDirection},
    {"flex-wrap", cFlexWrap}, {"justify-content", cJustifyContent}, {"align-items", cAlignItems}, {"align-self", cAlignSelf},
    {"align-content", cAlignContent}, {"justify-items", cJustifyItems}, {"justify-self", cJustifySelf}, {"flex-grow", cFlexGrow},
    {"flex-shrink", cFlexShrink}, {"flex-basis", cFlexBasis}, {"order", cOrder}, {"row-gap", cRowGap}, {"column-gap", cColumnGap},
    {"grid-template-columns", cGridColumns}, {"grid-template-rows", cGridRows}, {"grid-auto-columns", cGridAutoColumns},
    {"grid-auto-rows", cGridAutoRows}, {"grid-auto-flow", cGridAutoFlow}, {"grid-column-start", cGridColumnStart},
    {"grid-column-end", cGridColumnEnd}, {"grid-row-start", cGridRowStart}, {"grid-row-end", cGridRowEnd},
    {"grid-template-areas", cGridAreas}, {"grid-area-name", cGridArea}, {"border-collapse", cBorderCollapse}, {"content", cContent},
    {"transition", cTransitions}, {"animation", cAnimations}, {"line-clamp", cLineClamp}, {"direction", cDirection},
    {"scrollbar-color", cScrollbarColor}, {"line-height", cLineHeight}, {"z-index", cZIndex}, {"text-decoration-line", cDecorationLine},
    {"transform-origin", cTransformOrigin}, {"perspective", cPerspective}, {"perspective-origin", cPerspectiveOrigin},
    {"backface-visibility", cBackfaceVisibility}, {"transform-style", cTransformStyle}, {"container-type", cContainerType},
    {"counter-reset", cCounterReset}, {"backdrop-filter", cBackdropFilter}, {"text-align-last", cTextAlignLast},
    {"-webkit-text-fill-color", cTextFill}, {"-webkit-text-stroke", cTextStroke}, {"-webkit-text-stroke-width", cTextStrokeWidth},
    {"-webkit-text-stroke-color", cTextStrokeColor}, {"image-rendering", cImageRendering}, {"isolation", cIsolation},
    {"table-layout", cTableLayout}, {"caption-side", cCaptionSide}, {"mask-image", cMaskImage}, {"counter-set", cCounterSet},
    {"counter-increment", cCounterIncrement}, {"container-name", cContainerName}, {"border-spacing", cBorderSpacing},
    {"filter", cFilter},
};

static CscdStrMap copierMap;
static CscdOnce copierOnce = CSCD_ONCE_INIT;
static void initCopiers(void) {
    for (size_t i = 0; i < CSCD_COUNT(kCopiers); ++i) cscdStrMapPut(&copierMap, cscdSv(kCopiers[i].name), NULL)->p = (void*)(uintptr_t)kCopiers[i].fn;
}

bool cscdCopyProperty(CscdComputedStyle* d, const CscdComputedStyle* s, CscdSv p) {
    cscdCallOnce(&copierOnce, initCopiers);
    CscdMapVal* c = cscdStrMapGet(&copierMap, p);
    if (!c) return false;
    ((Copier)(uintptr_t)c->p)(d, s);
    return true;
}
