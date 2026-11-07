/* The cascade: rule indexing, matching, specificity ordering, custom properties with var(),
 * inheritance, presentational hints and the user-agent style sheet. */
#include "style/styleInternal.h"
#include "core/thread.h"

#include <stdio.h>
#include <stdlib.h>

const char* cscdUserAgentStyleSheet(void) {
    return
    "\n"
    "html, address, blockquote, body, dd, div, dl, dt, fieldset, form, frame, frameset, h1, h2, h3, h4, h5, h6,\n"
    "noframes, ol, p, ul, center, dir, hr, menu, pre, article, aside, footer, header, hgroup, main, nav, section,\n"
    "figure, figcaption, details, summary, dialog, legend, search, optgroup { display: block; }\n"
    "head, script, style, link, meta, title, base, template, noscript, datalist, param, [hidden], area, map { display: none; }\n"
    "html { color: canvastext; line-height: normal; font-family: system-ui; font-size: 16px; }\n"
    "body { margin: 8px; }\n"
    "p, blockquote, figure, dl, pre { margin-top: 1em; margin-bottom: 1em; }\n"
    "blockquote, figure { margin-left: 40px; margin-right: 40px; }\n"
    "address, i, cite, em, var, dfn { font-style: italic; }\n"
    "h1 { font-size: 2em; margin-top: .67em; margin-bottom: .67em; font-weight: bold; }\n"
    "h2 { font-size: 1.5em; margin-top: .83em; margin-bottom: .83em; font-weight: bold; }\n"
    "h3 { font-size: 1.17em; margin-top: 1em; margin-bottom: 1em; font-weight: bold; }\n"
    "h4 { margin-top: 1.33em; margin-bottom: 1.33em; font-weight: bold; }\n"
    "h5 { font-size: .83em; margin-top: 1.67em; margin-bottom: 1.67em; font-weight: bold; }\n"
    "h6 { font-size: .67em; margin-top: 2.33em; margin-bottom: 2.33em; font-weight: bold; }\n"
    "b, strong, th, legend { font-weight: bold; }\n"
    "pre, code, kbd, samp, tt, xmp, plaintext, listing { font-family: monospace; }\n"
    "pre, xmp, plaintext, listing { white-space: pre; }\n"
    "code, kbd, samp, tt { font-size: 13px; }\n"
    "pre { font-size: 13px; }\n"
    "textarea { white-space: pre-wrap; }\n"
    "mark { background-color: mark; color: marktext; }\n"
    "big { font-size: larger; }\n"
    "small, sub, sup { font-size: smaller; }\n"
    "sub { vertical-align: sub; }\n"
    "sup { vertical-align: super; }\n"
    "s, strike, del { text-decoration: line-through; }\n"
    "u, ins { text-decoration: underline; }\n"
    "a:any-link { color: linktext; text-decoration: underline; cursor: pointer; }\n"
    "abbr[title] { text-decoration: underline dotted; }\n"
    "q::before { content: open-quote; } q::after { content: close-quote; }\n"
    "center { text-align: center; }\n"
    "hr { color: gray; border-style: inset; border-width: 1px; margin: .5em auto; overflow: hidden; }\n"
    "ul, menu, dir { list-style-type: disc; margin-top: 1em; margin-bottom: 1em; padding-left: 40px; }\n"
    "ol { list-style-type: decimal; margin-top: 1em; margin-bottom: 1em; padding-left: 40px; }\n"
    "ul ul, ol ul, ul ol, ol ol { margin-top: 0; margin-bottom: 0; }\n"
    "ul ul, ol ul { list-style-type: circle; }\n"
    "ul ul ul, ol ul ul { list-style-type: square; }\n"
    "li { display: list-item; }\n"
    "dd { margin-left: 40px; }\n"
    "table { display: table; border-spacing: 2px; border-collapse: separate; box-sizing: border-box; text-indent: 0; }\n"
    "caption { display: table-caption; text-align: center; }\n"
    "thead { display: table-header-group; vertical-align: middle; }\n"
    "tbody { display: table-row-group; vertical-align: middle; }\n"
    "tfoot { display: table-footer-group; vertical-align: middle; }\n"
    "tr { display: table-row; vertical-align: inherit; }\n"
    "td, th { display: table-cell; vertical-align: inherit; padding: 1px; }\n"
    "th { text-align: center; }\n"
    "col { display: table-column; }\n"
    "colgroup { display: table-column-group; }\n"
    "img, video, canvas, svg, iframe, embed, object { display: inline-block; }\n"
    "[dir=rtl] { direction: rtl; }\n"
    "[dir=ltr] { direction: ltr; }\n"
    "svg:not(:root) { overflow: hidden; }\n"
    "fieldset { margin-left: 2px; margin-right: 2px; padding: .35em .75em .625em; border: 2px groove threedface; border-color: #c0c0c0; }\n"
    "legend { padding-left: 2px; padding-right: 2px; }\n"
    "input, select, button, textarea { display: inline-block; font-family: system-ui; font-size: 13.333px; letter-spacing: normal;\n"
    "  word-spacing: normal; line-height: normal; text-transform: none; text-indent: 0; text-shadow: none; text-align: start; color: fieldtext; }\n"
    "input, textarea { background-color: field; border: 2px inset #767676; border-width: 1px; border-color: #767676; border-radius: 2px; padding: 1px 2px; cursor: text; }\n"
    "input { width: 153px; height: 21px; box-sizing: border-box; white-space: pre; overflow: hidden; }\n"
    "textarea { width: 180px; height: 36px; overflow: auto; font-family: monospace; }\n"
    "input::placeholder, textarea::placeholder { color: #757575; }\n"
    "input[type=checkbox], input[type=radio] { width: 13px; height: 13px; margin: 3px 3px 3px 4px; padding: 0; border: none; cursor: default; background: none; }\n"
    "input[type=range] { width: 129px; height: 16px; margin: 2px; padding: 0; border: none; background: none; cursor: default; }\n"
    "input[type=color] { width: 50px; height: 27px; padding: 1px 2px; border: 1px solid #767676; background-color: buttonface; cursor: default; }\n"
    "input[type=hidden] { display: none; }\n"
    "input[type=button], input[type=submit], input[type=reset], button { color: buttontext; background-color: buttonface; border: 1px solid #767676;\n"
    "  border-radius: 3px; padding: 1px 6px; text-align: center; cursor: default; white-space: nowrap; width: auto; height: auto; }\n"
    "button { padding: 1px 6px; }\n"
    "select { background-color: field; border: 1px solid #767676; border-radius: 3px; padding: 1px 2px; cursor: default; white-space: nowrap; }\n"
    "option { display: none; }\n"
    "select option { display: none; }\n"
    "progress { display: inline-block; width: 160px; height: 16px; vertical-align: -0.2em; }\n"
    "meter { display: inline-block; width: 80px; height: 16px; vertical-align: -0.2em; }\n"
    "details > summary { display: list-item; list-style-type: disclosure-closed; cursor: pointer; }\n"
    "details:not([open]) > :not(summary) { display: none; }\n"
    "dialog:not([open]) { display: none; }\n"
    "dialog { position: absolute; left: 0; right: 0; margin: auto; border: solid; padding: 1em; background-color: canvas; color: canvastext; width: fit-content; height: fit-content; }\n"
    "ruby { display: inline; }\n"
    "rt { font-size: 50%; }\n"
    "iframe { border: 2px inset; width: 300px; height: 150px; }\n"
    "canvas { width: 300px; height: 150px; }\n"
    "video { width: 300px; height: 150px; }\n"
    ":focus-visible { outline: auto 1px -webkit-focus-ring-color; outline-color: #101010; }\n"
    "/* popup menus, tooltips and splitters (cscdViewShowMenu, title, the splitter attribute) */\n"
    ".cscd-menu { position: fixed; z-index: 2147483000; display: flex; flex-direction: column; box-sizing: border-box; min-width: 180px;\n"
    "  padding: 4px; background: #2c2c31; color: #e6e6ea; border: 1px solid #44444c; border-radius: 6px; box-shadow: 0 8px 24px rgba(0, 0, 0, 0.35);\n"
    "  font: 13px system-ui; line-height: normal; white-space: nowrap; user-select: none; cursor: default; text-align: left; }\n"
    ".cscd-menu-item { display: flex; align-items: center; height: 26px; padding: 0 10px 0 4px; border-radius: 4px; }\n"
    ".cscd-menu-item.highlighted { background: #3b6fd6; color: #ffffff; }\n"
    ".cscd-menu-item.disabled { color: #808088; }\n"
    ".cscd-menu-item.disabled.highlighted { background: rgba(255, 255, 255, 0.06); color: #808088; }\n"
    ".cscd-menu-check, .cscd-menu-icon { flex: none; width: 16px; height: 16px; margin-right: 6px; }\n"
    ".cscd-menu-label { flex: 1; }\n"
    ".cscd-menu.no-icons .cscd-menu-icon, .cscd-menu.no-checks .cscd-menu-check { display: none; }\n"
    ".cscd-menu-shortcut { margin-left: 28px; opacity: 0.6; }\n"
    ".cscd-menu-shortcut:empty { display: none; }\n"
    ".cscd-menu-arrow { flex: none; width: 12px; height: 12px; margin-left: 10px; }\n"
    ".cscd-menu-separator { flex: none; height: 1px; margin: 4px 6px; background: rgba(255, 255, 255, 0.12); }\n"
    ".cscd-tooltip { position: fixed; z-index: 2147483001; pointer-events: none; max-width: 380px; padding: 4px 8px; box-sizing: border-box;\n"
    "  background: #1e1e22; color: #e6e6ea; border: 1px solid #3c3c44; border-radius: 4px; box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3);\n"
    "  font: 12px system-ui; line-height: 1.35; white-space: pre-line; }\n"
    "textarea[code] { white-space: pre; tab-size: 4; }\n"
    "[popover] { display: none; position: fixed; z-index: 2147482500; margin: 0; padding: 0.25em; color: canvastext; background: canvas; border: 1px solid; }\n"
    "[popover].cscd-popover-open { display: block; }\n"
    "dialog { display: none; position: absolute; left: 0; right: 0; margin: auto; width: fit-content; padding: 1em; color: canvastext;\n"
    "  background: canvas; border: 1px solid; }\n"
    "dialog[open] { display: block; }\n"
    "dialog.cscd-modal { position: fixed; right: auto; margin: 0; max-width: calc(100% - 32px); max-height: calc(100% - 32px); overflow: auto;\n"
    "  z-index: 2147482000; box-shadow: 0 0 0 100vmax rgba(0, 0, 0, 0.45); }\n"
    "\n";
}

static const char* kInherited[] = {
    "color", "font-family", "font-size", "font-weight", "font-style", "font-variant", "font-variant-numeric", "font-variant-ligatures", "line-height", "letter-spacing",
    "word-spacing", "text-align", "text-indent", "white-space", "text-transform", "word-break", "overflow-wrap",
    "visibility", "cursor", "list-style-type", "list-style-position", "list-style-image", "border-collapse",
    "border-spacing", "caret-color", "accent-color", "direction", "fill", "stroke", "stroke-width", "pointer-events",
    "tab-size", "text-shadow", "user-select", "text-decoration-line", "text-decoration-color", "text-decoration-style",
    "line-clamp", "hyphens", "quotes", "writing-mode", "color-scheme", "font-synthesis", "empty-cells", "text-underline-position", "text-emphasis-style", "text-emphasis-color",
    "text-emphasis-position", "font-variant-caps", "font-variant-position", "font-variant-east-asian", "font-kerning", "font-optical-sizing",
    "font-stretch", "font-feature-settings", "font-variation-settings", "scrollbar-color", "text-align-last", "-webkit-text-fill-color", "-webkit-text-stroke",
    "-webkit-text-stroke-width", "-webkit-text-stroke-color", "image-rendering", "caption-side"};

bool cscdStyleIsInherited(CscdSv p) {
    for (size_t i = 0; i < CSCD_COUNT(kInherited); ++i)
        if (cscdSvEqC(p, kInherited[i])) return true;
    return false;
}

#define INVALID "\x01invalid" /* the guaranteed-invalid value */

static bool wordBoundary(CscdSv v, size_t i) { return i == 0 || (!cscdIsAlnum(v.ptr[i - 1]) && v.ptr[i - 1] != '-'); }

/* How long a value with var()s may grow: each one can name others twice, so a few dozen custom
 * properties could otherwise ask for more memory than there is. Longer: invalid at computed-value
 * time, as browsers make it. */
#define kMaxSubstitutedBytes (512u * 1024u)

/* Replaces var(--name, fallback) using the element's custom properties (appended to out). */
static bool substituteInto(CscdSv value, const CscdVars* vars, int depth, CscdStr* out) {
    if (out->len + value.len > kMaxSubstitutedBytes && cscdSvFindC(value, "var(", 0) == CSCD_NPOS) return false;
    if (depth > 16 || cscdSvFindC(value, "var(", 0) == CSCD_NPOS) {
        if (cscdSvFindC(value, "env(", 0) == CSCD_NPOS) {
            cscdStrAppend(out, value);
            return true;
        }
    }
    size_t i = 0;
    while (i < value.len) {
        CscdSv rest = cscdSvFrom(value, i);
        bool isVar = cscdSvStartsC(rest, "var(") && wordBoundary(value, i);
        bool isEnv = cscdSvStartsC(rest, "env(") && wordBoundary(value, i);
        if (!isVar && !isEnv) { cscdStrAppendChar(out, value.ptr[i++]); continue; }
        if (out->len > kMaxSubstitutedBytes) return false;
        size_t j = i + 4;
        int d = 1;
        size_t comma = CSCD_NPOS;
        while (j < value.len && d > 0) {
            if (value.ptr[j] == '(') ++d;
            else if (value.ptr[j] == ')') { if (--d == 0) break; }
            else if (value.ptr[j] == ',' && d == 1 && comma == CSCD_NPOS) comma = j;
            ++j;
        }
        CscdSv name = cscdSvTrim(cscdSvSub(value, i + 4, (comma == CSCD_NPOS ? j : comma) - i - 4));
        CscdSv fallback = comma == CSCD_NPOS ? CSCD_SV("") : cscdSvTrim(cscdSvSub(value, comma + 1, j - comma - 1));
        bool found = false;
        if (isVar && vars) {
            const CscdStr* v = cscdVarsGet(vars, name);
            if (v && cscdSvTrim(cscdStrSv(v)).len && !cscdStrEqC(v, "initial")) {
                if (!substituteInto(cscdStrSv(v), vars, depth + 1, out)) return false;
                found = true;
            }
        }
        if (!found) {
            if (comma == CSCD_NPOS) return false;
            if (!substituteInto(fallback, vars, depth + 1, out)) return false;
        }
        i = j + 1;
    }
    return true;
}

static CscdStr substituteVars(CscdSv value, const CscdVars* vars) {
    CscdStr out = {0};
    if (!substituteInto(value, vars, 0, &out)) cscdStrSetC(&out, INVALID);
    return out;
}

static void blockify(CscdComputedStyle* s) {
    switch (s->display) {
    case CSCD_DISPLAY_INLINE: case CSCD_DISPLAY_INLINE_BLOCK: case CSCD_DISPLAY_TABLE_ROW: case CSCD_DISPLAY_TABLE_CELL:
    case CSCD_DISPLAY_TABLE_ROW_GROUP: case CSCD_DISPLAY_TABLE_HEADER_GROUP: case CSCD_DISPLAY_TABLE_FOOTER_GROUP:
    case CSCD_DISPLAY_TABLE_CAPTION: case CSCD_DISPLAY_TABLE_COLUMN: case CSCD_DISPLAY_TABLE_COLUMN_GROUP:
        s->display = CSCD_DISPLAY_BLOCK; break;
    case CSCD_DISPLAY_INLINE_FLEX: s->display = CSCD_DISPLAY_FLEX; break;
    case CSCD_DISPLAY_INLINE_GRID: s->display = CSCD_DISPLAY_GRID; break;
    case CSCD_DISPLAY_INLINE_TABLE: s->display = CSCD_DISPLAY_TABLE; break;
    default: break;
    }
}

static void addDecl(CscdDeclVec* out, const char* p, CscdStr v) {
    CscdDeclaration d = {cscdStrDupC(p), v, false};
    cscdVecPush(out, d);
}

static void addDim(const CscdElement* e, CscdDeclVec* out, const char* attr, const char* prop) {
    const CscdStr* v = cscdElementGetAttribute(e, cscdSv(attr));
    if (!v || !v->len) return;
    CscdSv s = cscdSvTrim(cscdStrSv(v));
    if (!s.len) return;
    bool num = true;
    for (size_t i = 0; i < s.len; ++i) num = num && (cscdIsDigit(s.ptr[i]) || s.ptr[i] == '.');
    CscdStr val = cscdStrDup(s);
    if (num) cscdStrAppendC(&val, "px");
    addDecl(out, prop, val);
}

static int attrInt(const CscdStr* s) {
    char* c = cscdSvDupC(cscdStrSv(s));
    int n = atoi(c);
    cscdFree(c);
    return n;
}

/* HTML attributes that map to CSS (width="", bgcolor="", align=""...), at author level with zero specificity. */
static void presentationalHints(const CscdElement* e, CscdDeclVec* out) {
    CscdSv t = cscdStrSv(&e->tag);
#define T(s) cscdSvEqC(t, s)
#define ATTR(n) cscdElementGetAttribute(e, CSCD_SV(n))
    if (T("img") || T("canvas") || T("video") || T("iframe") || T("td") || T("th") || T("table") || T("col") || T("embed") ||
        T("object") || T("svg") || T("hr") || (T("input") && cscdSvIEqC(cscdElementAttrC(e, "type"), "image"))) {
        addDim(e, out, "width", "width");
        if (!T("td") && !T("th") && !T("col") && !T("hr")) addDim(e, out, "height", "height");
    }
    const CscdStr* a;
    if ((a = ATTR("bgcolor"))) addDecl(out, "background-color", cscdStrCopy(a));
    if (T("font")) {
        if ((a = ATTR("color"))) addDecl(out, "color", cscdStrCopy(a));
        if ((a = ATTR("face"))) addDecl(out, "font-family", cscdStrCopy(a));
        if ((a = ATTR("size"))) {
            static const char* sizes[] = {"10px", "13px", "16px", "18px", "24px", "32px", "48px"};
            int n = attrInt(a);
            if (a->len && (a->ptr[0] == '+' || a->ptr[0] == '-')) n += 3;
            addDecl(out, "font-size", cscdStrDupC(sizes[cscdClampi(n, 1, 7) - 1]));
        }
    }
    if ((a = ATTR("align"))) {
        CscdStr v = cscdLower(cscdStrSv(a));
        if (T("img") || T("table")) {
            if (cscdStrEqC(&v, "left") || cscdStrEqC(&v, "right")) addDecl(out, "float", cscdStrCopy(&v));
            if (T("table") && cscdStrEqC(&v, "center")) { addDecl(out, "margin-left", cscdStrDupC("auto")); addDecl(out, "margin-right", cscdStrDupC("auto")); }
        } else if (cscdStrEqC(&v, "center") || cscdStrEqC(&v, "left") || cscdStrEqC(&v, "right") || cscdStrEqC(&v, "justify"))
            addDecl(out, "text-align", cscdStrCopy(&v));
        cscdStrFree(&v);
    }
    if ((a = ATTR("valign"))) addDecl(out, "vertical-align", cscdStrCopy(a));
    if (T("table")) {
        if ((a = ATTR("border"))) {
            CscdStr w = a->len ? cscdStrCopy(a) : cscdStrDupC("1");
            if (!cscdStrEqC(&w, "0")) {
                cscdStrAppendC(&w, "px");
                addDecl(out, "border-width", w);
                addDecl(out, "border-style", cscdStrDupC("outset"));
                addDecl(out, "border-color", cscdStrDupC("gray"));
            } else cscdStrFree(&w);
        }
        if ((a = ATTR("cellspacing"))) { CscdStr v = cscdStrCopy(a); cscdStrAppendC(&v, "px"); addDecl(out, "border-spacing", v); }
    }
    if (T("td") || T("th")) {
        /* cellpadding / border of the enclosing table */
        const CscdElement* tbl = cscdNodeParentElement(&e->node);
        while (tbl && !cscdElementIs(tbl, "table")) tbl = cscdNodeParentElement(&tbl->node);
        if (tbl) {
            const CscdStr* cp = cscdElementGetAttribute(tbl, CSCD_SV("cellpadding"));
            if (cp) { CscdStr v = cscdStrCopy(cp); cscdStrAppendC(&v, "px"); addDecl(out, "padding", v); }
            const CscdStr* b = cscdElementGetAttribute(tbl, CSCD_SV("border"));
            if (b && !cscdStrEqC(b, "0")) {
                addDecl(out, "border-width", cscdStrDupC("1px"));
                addDecl(out, "border-style", cscdStrDupC("inset"));
                addDecl(out, "border-color", cscdStrDupC("gray"));
            }
        }
        if (cscdElementHasAttributeC(e, "nowrap")) addDecl(out, "white-space", cscdStrDupC("nowrap"));
    }
    if (T("body") && (a = ATTR("text"))) addDecl(out, "color", cscdStrCopy(a));
    if ((T("ol") || T("ul") || T("li")) && (a = ATTR("type"))) {
        const char* m = cscdStrEqC(a, "1") ? "decimal" : cscdStrEqC(a, "a") ? "lower-alpha" : cscdStrEqC(a, "A") ? "upper-alpha"
                       : cscdStrEqC(a, "i") ? "lower-roman" : cscdStrEqC(a, "I") ? "upper-roman" : NULL;
        addDecl(out, "list-style-type", m ? cscdStrDupC(m) : cscdStrCopy(a));
    }
    if (cscdElementHasAttributeC(e, "hidden")) addDecl(out, "display", cscdStrDupC("none"));
    if (T("textarea")) {
        if ((a = ATTR("cols"))) addDecl(out, "width", cscdStrf("%dpx", attrInt(a) * 8 + 4));
        if ((a = ATTR("rows"))) addDecl(out, "height", cscdStrf("%dpx", attrInt(a) * 16 + 4));
    }
    if (T("input") && (a = ATTR("size")) && attrInt(a) > 0) addDecl(out, "width", cscdStrf("%dpx", attrInt(a) * 7 + 12));
#undef T
#undef ATTR
}

typedef struct Match {
    const CscdDeclaration* decl;
    int rank;        /* 0 UA normal, 1 author normal, 2 author important, 3 UA important */
    uint32_t spec;
    uint32_t sheet, order, index;
} Match;
typedef CSCD_VEC(Match) MatchVec;

static int compareMatches(const void* pa, const void* pb, void* ctx) {
    (void)ctx;
    const Match* a = (const Match*)pa;
    const Match* b = (const Match*)pb;
    if (a->rank != b->rank) return a->rank < b->rank ? -1 : 1;
    if (a->spec != b->spec) return a->spec < b->spec ? -1 : 1;
    if (a->sheet != b->sheet) return a->sheet < b->sheet ? -1 : 1;
    if (a->order != b->order) return a->order < b->order ? -1 : 1;
    if (a->index != b->index) return a->index < b->index ? -1 : 1;
    return 0;
}

/* The pseudo elements the cascade computes, by index (0: the element itself). */
static const char* kPseudos[] = {"", "before", "after", "placeholder", "marker", "selection"};
static int pseudoIndex(CscdSv p) {
    if (!p.len) return 0;
    for (int i = 1; i < 6; ++i) if (cscdSvEqC(p, kPseudos[i])) return i;
    return -1;
}

typedef struct Indexed { const CscdStyleRule* rule; const CscdSelector* sel; uint32_t sheetIndex; CscdOrigin origin; } Indexed;
typedef CSCD_VEC(Indexed) IndexedVec;
typedef CSCD_VEC(const Indexed*) IndexedRefs;

/* What a computed style depends on: the rules that matched, the element's own declarations, the
 * parent's style and the environment. Two elements with the same key get the same style. */
typedef struct CacheEntry {
    CscdComputedStyle* style; /* a reference */
    uint64_t id;
    bool viewport;          /* it resolved viewport units: valid only at this viewport size */
    float w, h;
    uint32_t used;          /* the last pass that used it */
} CacheEntry;

typedef struct Cache {
    CscdStrMap map; /* key bytes -> CacheEntry* */
    CscdU64Map ids; /* the styles held, by id */
    uint64_t next;
    uint32_t pass;
} Cache;

typedef struct PseudoSet { CscdComputedStyle* s[6]; } PseudoSet;

struct CscdStyleEngine {
    Cache cache;
    CscdStr key; /* (scratch) */
    CscdStrMap expansions; /* property, a zero byte, value -> CscdStrPairVec* (the longhands of a shorthand) */
    CscdStr expansionKey;  /* (scratch) */
    CscdStrMap values;     /* value text -> its parsed values (see CscdApplyState) */
    IndexedRefs matched[6];
    CscdU64Map containerSizes; /* element -> f[2] */
    bool hasContainers;
    bool lastUsedContainerUnits;
    bool usedViewport; /* a style of this pass resolved viewport units */
    uint64_t computed, reused; /* (counts, for cscdViewFrameStats) */
    CscdPtrVec sheets; /* CscdStyleSheet* (references) */
    CscdStrMap byId, byClass, byTag; /* -> IndexedVec* */
    IndexedVec universal;
    CscdU64Map pseudo; /* element -> PseudoSet* */
    bool indexValid;
    bool statesReachSiblings;
    CscdStrMap selectedAttrs; /* attribute names the selectors test (lowercase) */
    bool valuesUseAttr;       /* a declaration reads an attribute (attr()) */
    CscdMediaContext indexMedia;
};

static void clearExpansions(CscdStrMap* m) {
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(m, s));) {
        cscdStrPairVecFree((CscdStrPairVec*)s->val.p);
        cscdFree(s->val.p);
    }
    cscdStrMapClear(m);
}

static void cacheClear(Cache* c) {
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&c->map, s));) {
        CacheEntry* e = (CacheEntry*)s->val.p;
        cscdRelease(e->style);
        cscdFree(e);
    }
    cscdStrMapClear(&c->map);
    cscdU64MapClear(&c->ids);
}

/* A new pass over the document. The styles stay (a class toggled off and on again finds its
 * styles); once there are many, those the last pass did not use go. */
static void cacheNewPass(Cache* c) {
    ++c->pass;
    if (c->map.len <= 4096) return;
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&c->map, s));) {
        CacheEntry* e = (CacheEntry*)s->val.p;
        if (e->used + 1 >= c->pass) continue;
        if (e->style) cscdU64MapRemove(&c->ids, CSCD_PKEY(e->style), NULL);
        cscdRelease(e->style);
        cscdFree(e);
        cscdStrMapRemove(&c->map, cscdStrMapKey(s), NULL);
    }
}

static CacheEntry* cacheFind(Cache* c, CscdSv key) {
    CscdMapVal* v = cscdStrMapGet(&c->map, key);
    if (!v) return NULL;
    CacheEntry* e = (CacheEntry*)v->p;
    e->used = c->pass;
    return e;
}

static void freeIndexMap(CscdStrMap* m) {
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(m, s));) {
        cscdVecFree((IndexedVec*)s->val.p);
        cscdFree(s->val.p);
    }
    cscdStrMapClear(m);
}

static void clearPseudo(CscdStyleEngine* se) {
    for (CscdU64MapSlot* s = NULL; (s = cscdU64MapNext(&se->pseudo, s));) {
        PseudoSet* p = (PseudoSet*)s->val.p;
        for (int i = 0; i < 6; ++i) cscdRelease(p->s[i]);
        cscdFree(p);
    }
    cscdU64MapClear(&se->pseudo);
}

static CscdStyleSheet* uaSheet;
static CscdOnce uaOnce = CSCD_ONCE_INIT;
static void makeUaSheet(void) {
    /* the user-agent sheet is parsed once and shared (it is never changed) */
    uaSheet = cscdStyleSheetNew(CSCD_ORIGIN_USER_AGENT);
    cscdStyleSheetParse(uaSheet, cscdSv(cscdUserAgentStyleSheet()), CSCD_SV(""), 0);
}

void cscdStyleEngineClear(CscdStyleEngine* se) {
    for (size_t i = 0; i < se->sheets.len; ++i) cscdRelease(se->sheets.items[i]);
    se->sheets.len = 0;
    cscdCallOnce(&uaOnce, makeUaSheet);
    cscdVecPush(&se->sheets, cscdRetain(uaSheet));
    se->indexValid = false;
    cscdU64MapClear(&se->containerSizes);
    cacheClear(&se->cache); /* the keys point into the sheets */
}

CscdStyleEngine* cscdStyleEngineNew(void) {
    CscdStyleEngine* se = CSCD_NEW(CscdStyleEngine);
    se->cache.next = 1;
    se->indexMedia = cscdMediaContextDefault();
    cscdStyleEngineClear(se);
    return se;
}

void cscdStyleEngineFree(CscdStyleEngine* se) {
    if (!se) return;
    cacheClear(&se->cache);
    clearExpansions(&se->expansions);
    cscdStrMapFree(&se->expansions);
    cscdValueCacheClear(&se->values);
    cscdStrMapFree(&se->values);
    cscdStrFree(&se->expansionKey);
    cscdStrMapFree(&se->cache.map);
    cscdU64MapFree(&se->cache.ids);
    cscdStrFree(&se->key);
    for (int i = 0; i < 6; ++i) cscdVecFree(&se->matched[i]);
    cscdU64MapFree(&se->containerSizes);
    for (size_t i = 0; i < se->sheets.len; ++i) cscdRelease(se->sheets.items[i]);
    cscdVecFree(&se->sheets);
    freeIndexMap(&se->byId);
    freeIndexMap(&se->byClass);
    freeIndexMap(&se->byTag);
    cscdStrMapFree(&se->selectedAttrs);
    cscdStrMapFree(&se->byId);
    cscdStrMapFree(&se->byClass);
    cscdStrMapFree(&se->byTag);
    cscdVecFree(&se->universal);
    clearPseudo(se);
    cscdU64MapFree(&se->pseudo);
    cscdFree(se);
}

void cscdStyleEngineAddSheet(CscdStyleEngine* se, CscdStyleSheet* sheet) {
    cscdVecPush(&se->sheets, cscdRetain(sheet));
    se->indexValid = false;
    cacheClear(&se->cache);
}

size_t cscdStyleEngineSheetCount(const CscdStyleEngine* se) { return se->sheets.len; }
CscdStyleSheet* cscdStyleEngineSheet(const CscdStyleEngine* se, size_t i) { return (CscdStyleSheet*)se->sheets.items[i]; }

const CscdKeyframesRule* cscdStyleEngineKeyframes(const CscdStyleEngine* se, CscdSv name) {
    for (size_t i = se->sheets.len; i-- > 0;) {
        const CscdStyleSheet* s = (const CscdStyleSheet*)se->sheets.items[i];
        for (size_t k = 0; k < s->keyframes.len; ++k)
            if (cscdSvEq(cscdStrSv(&s->keyframes.items[k].name), name)) return &s->keyframes.items[k];
    }
    return NULL;
}

void cscdStyleEngineFontFaces(const CscdStyleEngine* se, CscdPtrVec* out) {
    for (size_t i = 0; i < se->sheets.len; ++i) {
        const CscdStyleSheet* s = (const CscdStyleSheet*)se->sheets.items[i];
        for (size_t k = 0; k < s->fontFaces.len; ++k) cscdVecPush(out, (void*)&s->fontFaces.items[k]);
    }
}

static bool isState(const CscdSimpleSelector* p) {
    return p->kind == CSCD_SEL_PSEUDO_CLASS && (cscdStrEqC(&p->name, "hover") || cscdStrEqC(&p->name, "active"));
}

static bool usesStateSel(const CscdSelector* s);

static bool usesStateCompound(const CscdCompound* c) {
    for (size_t i = 0; i < c->parts.len; ++i) {
        const CscdSimpleSelector* p = &c->parts.items[i];
        if (isState(p)) return true;
        for (size_t a = 0; a < p->args.len; ++a) if (usesStateSel(&p->args.items[a])) return true;
    }
    return false;
}

static bool usesStateSel(const CscdSelector* s) {
    for (size_t i = 0; i < s->compounds.len; ++i) if (usesStateCompound(&s->compounds.items[i])) return true;
    return false;
}

/* A state on a compound with + or ~ after it (its siblings depend on it), or inside :has() (its
 * ancestors do). */
static bool stateReachesSiblings(const CscdSelector* s) {
    bool state = false;
    for (size_t i = 0; i < s->compounds.len; ++i) {
        const CscdCompound* c = &s->compounds.items[i];
        if ((c->combinator == CSCD_COMB_ADJACENT || c->combinator == CSCD_COMB_SIBLING) && state) return true;
        state = state || usesStateCompound(c);
        for (size_t k = 0; k < c->parts.len; ++k) {
            const CscdSimpleSelector* p = &c->parts.items[k];
            for (size_t a = 0; a < p->args.len; ++a) {
                if (p->kind == CSCD_SEL_PSEUDO_CLASS && cscdStrEqC(&p->name, "has") && usesStateSel(&p->args.items[a])) return true;
                if (stateReachesSiblings(&p->args.items[a])) return true;
            }
        }
    }
    return false;
}

void cscdStyleEngineInvalidateCache(CscdStyleEngine* se) { cacheClear(&se->cache); }

bool cscdStyleEngineIsShared(const CscdStyleEngine* se, const CscdComputedStyle* s) { return cscdU64MapGet(&se->cache.ids, CSCD_PKEY(s)) != NULL; }

bool cscdStyleEngineStatesReachSiblings(const CscdStyleEngine* se) { return se->statesReachSiblings; }

/* the attribute names a selector tests, in it and in its :is(), :not(), :has()... */
static void noteAttributes(CscdStyleEngine* se, const CscdSelector* s) {
    for (size_t i = 0; i < s->compounds.len; ++i) {
        const CscdCompound* c = &s->compounds.items[i];
        for (size_t k = 0; k < c->parts.len; ++k) {
            const CscdSimpleSelector* p = &c->parts.items[k];
            if (p->kind == CSCD_SEL_ATTRIBUTE) {
                CscdStr n = cscdLower(cscdStrSv(&p->name));
                cscdStrMapPut(&se->selectedAttrs, cscdStrSv(&n), NULL);
                cscdStrFree(&n);
            }
            for (size_t a = 0; a < p->args.len; ++a) noteAttributes(se, &p->args.items[a]);
        }
    }
}

bool cscdStyleEngineSelectsAttribute(const CscdStyleEngine* se, CscdSv name) {
    if (!se->indexValid || se->valuesUseAttr) return true; /* (not known yet, or any attribute may show) */
    CscdStr n = cscdLower(name);
    const bool found = cscdStrMapGet(&se->selectedAttrs, cscdStrSv(&n)) != NULL;
    cscdStrFree(&n);
    return found;
}
bool cscdStyleEngineHasContainers(const CscdStyleEngine* se) { return se->hasContainers; }

static void addIndexed(CscdStrMap* m, CscdSv key, Indexed ix) {
    bool inserted = false;
    CscdMapVal* v = cscdStrMapPut(m, key, &inserted);
    if (inserted) v->p = CSCD_NEW(IndexedVec);
    cscdVecPush((IndexedVec*)v->p, ix);
}

static void buildIndex(CscdStyleEngine* se, const CscdMediaContext* media) {
    freeIndexMap(&se->byId);
    freeIndexMap(&se->byClass);
    freeIndexMap(&se->byTag);
    cscdVecClear(&se->universal);
    se->statesReachSiblings = false;
    cscdStrMapClear(&se->selectedAttrs);
    se->valuesUseAttr = false;
    for (uint32_t si = 0; si < se->sheets.len; ++si) {
        const CscdStyleSheet* sheet = (const CscdStyleSheet*)se->sheets.items[si];
        for (size_t ri = 0; ri < sheet->rules.len; ++ri) {
            const CscdStyleRule* rule = &sheet->rules.items[ri];
            if (rule->media.len && !cscdEvalMediaQuery(cscdStrSv(&rule->media), media)) continue;
            for (size_t d = 0; d < rule->declarations.len && !se->valuesUseAttr; ++d)
                se->valuesUseAttr = cscdSvContains(cscdStrSv(&rule->declarations.items[d].value), "attr(");
            for (size_t k = 0; k < rule->selectors.len; ++k) {
                const CscdSelector* sel = &rule->selectors.items[k];
                se->statesReachSiblings = se->statesReachSiblings || stateReachesSiblings(sel);
                noteAttributes(se, sel);
                Indexed ix = {rule, sel, si, sheet->origin};
                const CscdCompound* last = &cscdVecLast(&sel->compounds);
                const CscdSimpleSelector* key = NULL;
                for (size_t p = 0; p < last->parts.len && !key; ++p) if (last->parts.items[p].kind == CSCD_SEL_ID) key = &last->parts.items[p];
                for (size_t p = 0; p < last->parts.len && !key; ++p) if (last->parts.items[p].kind == CSCD_SEL_CLASS) key = &last->parts.items[p];
                for (size_t p = 0; p < last->parts.len && !key; ++p) if (last->parts.items[p].kind == CSCD_SEL_TYPE) key = &last->parts.items[p];
                if (!key) cscdVecPush(&se->universal, ix);
                else if (key->kind == CSCD_SEL_ID) addIndexed(&se->byId, cscdStrSv(&key->name), ix);
                else if (key->kind == CSCD_SEL_CLASS) addIndexed(&se->byClass, cscdStrSv(&key->name), ix);
                else addIndexed(&se->byTag, cscdStrSv(&key->name), ix);
            }
        }
    }
    se->indexValid = true;
    se->indexMedia = *media;
}

static bool hasName(const CscdStrVec* names, CscdSv name) {
    for (size_t i = 0; i < names->len; ++i) if (cscdSvEq(cscdStrSv(&names->items[i]), name)) return true;
    return false;
}

static const CscdElement* containerFor(const CscdStyleEngine* se, const CscdElement* e, bool self, CscdSv name, float size[2]) {
    for (const CscdElement* a = self ? e : cscdNodeParentElement(&e->node); a; a = cscdNodeParentElement(&a->node)) {
        if (!a->style || !a->style->containerType) continue;
        if (name.len && !hasName(&a->style->containerNames, name)) continue;
        CscdMapVal* v = cscdU64MapGet(&se->containerSizes, CSCD_PKEY(a));
        if (!v) return NULL; /* (not laid out yet) */
        size[0] = v->f[0];
        size[1] = v->f[1];
        return a;
    }
    return NULL;
}

static bool containerHolds(const CscdStyleEngine* se, const CscdElement* e, const CscdContainerCondition* c, bool self) {
    float size[2];
    const CscdElement* box = containerFor(se, e, self, cscdStrSv(&c->name), size);
    if (!box) return false;
    /* an inline-size container has no height to ask about */
    if (box->style->containerType == 1) {
        CscdStr lq = cscdLower(cscdStrSv(&c->query));
        bool height = cscdSvContains(cscdStrSv(&lq), "height");
        cscdStrFree(&lq);
        if (height) return false;
    }
    CscdMediaContext m = se->indexMedia;
    m.width = size[0];
    m.height = box->style->containerType == 2 ? size[1] : 0;
    return cscdEvalMediaQuery(cscdStrSv(&c->query), &m);
}

bool cscdStyleEngineSetContainerSizes(CscdStyleEngine* se, CscdU64Map* sizes) {
    bool same = sizes->len == se->containerSizes.len;
    for (CscdU64MapSlot* s = NULL; same && (s = cscdU64MapNext(sizes, s));) {
        CscdMapVal* o = cscdU64MapGet(&se->containerSizes, s->key);
        same = o && o->f[0] == s->val.f[0] && o->f[1] == s->val.f[1];
    }
    if (same) {
        cscdU64MapFree(sizes);
        return false;
    }
    cscdU64MapFree(&se->containerSizes);
    se->containerSizes = *sizes;
    memset(sizes, 0, sizeof *sizes);
    return true;
}

static void considerList(const CscdStyleEngine* se, const CscdElement* e, const IndexedVec* list, IndexedRefs* out) {
    for (size_t i = 0; i < list->len; ++i) {
        const Indexed* ix = &list->items[i];
        const int p = pseudoIndex(cscdStrSv(&ix->sel->pseudoElement));
        if (p < 0 || !cscdMatchSelector(ix->sel, e)) continue;
        bool holds = true;
        for (size_t c = 0; c < ix->rule->containers.len && holds; ++c) holds = containerHolds(se, e, &ix->rule->containers.items[c], p > 0);
        if (holds) cscdVecPush(&out[p], ix);
    }
}

static const IndexedVec* lookup(const CscdStrMap* m, CscdSv key) {
    CscdMapVal* v = cscdStrMapGet(m, key);
    return v ? (const IndexedVec*)v->p : NULL;
}

/* out[6]: the element, then its pseudo elements */
static void matchRules(const CscdStyleEngine* se, const CscdElement* e, IndexedRefs* out) {
    const CscdStr* id = cscdElementGetAttribute(e, CSCD_SV("id"));
    const IndexedVec* l;
    if (id && (l = lookup(&se->byId, cscdStrSv(id)))) considerList(se, e, l, out);
    const CscdStrVec* classes = cscdElementClassList(e);
    for (size_t i = 0; i < classes->len; ++i)
        if ((l = lookup(&se->byClass, cscdStrSv(&classes->items[i])))) considerList(se, e, l, out);
    if ((l = lookup(&se->byTag, cscdStrSv(&e->tag)))) considerList(se, e, l, out);
    considerList(se, e, &se->universal, out);
}

static CscdComputedStyle* computeMatched(CscdStyleEngine* se, CscdElement* e, const CscdComputedStyle* parent, const CscdStyleEnvironment* env, CscdSv pseudo,
                                       const IndexedRefs* sels, const CscdDeclVec* hints, bool* viewport);

static int compareMatched(const void* a, const void* b, void* ctx) {
    (void)ctx;
    const CscdMatchedRule *x = (const CscdMatchedRule*)a, *y = (const CscdMatchedRule*)b;
    if (x->origin != y->origin) return x->origin < y->origin ? -1 : 1;
    if (x->selector->specificity != y->selector->specificity) return x->selector->specificity < y->selector->specificity ? -1 : 1;
    if (x->sheet != y->sheet) return x->sheet < y->sheet ? -1 : 1;
    return x->rule->order < y->rule->order ? -1 : x->rule->order > y->rule->order;
}

void cscdStyleEngineMatchedRules(const CscdStyleEngine* se, const CscdElement* e, CscdMatchedRuleVec* out) {
    if (!se->indexValid || !e) return;
    IndexedRefs sels[6] = {{0}};
    matchRules(se, e, sels);
    const size_t from = out->len;
    for (size_t i = 0; i < sels[0].len; ++i) {
        const Indexed* ix = sels[0].items[i];
        CscdMatchedRule m = {ix->rule, ix->sel, ix->origin, ix->sheetIndex};
        cscdVecPush(out, m);
    }
    for (int p = 0; p < 6; ++p) cscdVecFree(&sels[p]);
    cscdSort(out->items + from, out->len - from, sizeof(CscdMatchedRule), compareMatched, NULL);
}

CscdComputedStyle* cscdStyleEngineComputeFor(CscdStyleEngine* se, CscdElement* e, const CscdComputedStyle* parent, const CscdStyleEnvironment* env, CscdSv pseudo) {
    IndexedRefs sels[6] = {{0}};
    const int p = pseudoIndex(pseudo);
    if (p >= 0) matchRules(se, e, sels);
    else {
        const IndexedVec* lists[64];
        size_t nl = 0;
        const CscdStr* id = cscdElementGetAttribute(e, CSCD_SV("id"));
        const IndexedVec* l;
        if (id && (l = lookup(&se->byId, cscdStrSv(id)))) lists[nl++] = l;
        const CscdStrVec* classes = cscdElementClassList(e);
        for (size_t i = 0; i < classes->len && nl < 62; ++i)
            if ((l = lookup(&se->byClass, cscdStrSv(&classes->items[i])))) lists[nl++] = l;
        if ((l = lookup(&se->byTag, cscdStrSv(&e->tag)))) lists[nl++] = l;
        lists[nl++] = &se->universal;
        for (size_t k = 0; k < nl; ++k)
            for (size_t i = 0; i < lists[k]->len; ++i) {
                const Indexed* ix = &lists[k]->items[i];
                if (cscdSvEq(cscdStrSv(&ix->sel->pseudoElement), pseudo) && cscdMatchSelector(ix->sel, e)) cscdVecPush(&sels[0], ix);
            }
    }
    CscdDeclVec hints = {0};
    if (!pseudo.len) presentationalHints(e, &hints);
    CscdComputedStyle* s = computeMatched(se, e, parent, env, pseudo, &sels[p > 0 ? p : 0], &hints, NULL);
    cscdDeclVecFree(&hints);
    for (int i = 0; i < 6; ++i) cscdVecFree(&sels[i]);
    return s;
}

/* step 4: the winning value of each longhand, in application order */
/* The winning value of each longhand, in the order they were first declared: views (into the
 * declarations, the engine's expansions, and values with var()s substituted, kept until the style is
 * done), found by name through a table of views, so that nothing is copied. */
typedef struct SvPair { CscdSv name, value; } SvPair;
typedef struct WinSlot { CscdSv name; uint32_t hash, at; } WinSlot; /* at: index in list + 1, 0 free */
typedef struct Winners { CSCD_VEC(SvPair) list; WinSlot* slots; size_t cap; } Winners;

static WinSlot* winSlot(const Winners* w, CscdSv name, uint32_t h) {
    for (size_t j = h & (w->cap - 1);; j = (j + 1) & (w->cap - 1)) {
        WinSlot* s = &w->slots[j];
        if (!s->at || (s->hash == h && cscdSvEq(s->name, name))) return s;
    }
}

static const SvPair* findWinner(const Winners* w, CscdSv name) {
    if (!w->cap) return NULL;
    const WinSlot* s = winSlot(w, name, cscdHash(name.ptr, name.len));
    return s->at ? &w->list.items[s->at - 1] : NULL;
}

static void setWinner(Winners* w, CscdSv prop, CscdSv val) {
    if ((w->list.len + 1) * 2 > w->cap) { /* (at most half full) */
        WinSlot* old = w->slots;
        const size_t oldCap = w->cap;
        w->cap = w->cap ? w->cap * 2 : 64;
        w->slots = (WinSlot*)cscdCalloc(w->cap, sizeof(WinSlot));
        for (size_t i = 0; i < oldCap; ++i)
            if (old[i].at) *winSlot(w, old[i].name, old[i].hash) = old[i];
        cscdFree(old);
    }
    const uint32_t h = cscdHash(prop.ptr, prop.len);
    WinSlot* s = winSlot(w, prop, h);
    if (s->at) { w->list.items[s->at - 1].value = val; return; }
    SvPair p = {prop, val};
    cscdVecPush(&w->list, p);
    s->name = prop;
    s->hash = h;
    s->at = (uint32_t)w->list.len;
}

/* The longhands of a shorthand's value, expanded once per text: the same declarations come back
 * for every style their rules match. */
static const CscdStrPairVec* expansion(CscdStyleEngine* se, CscdSv prop, CscdSv value) {
    CscdStr* k = &se->expansionKey;
    cscdStrClear(k);
    cscdStrAppend(k, prop);
    cscdStrAppendChar(k, 0);
    cscdStrAppend(k, value);
    CscdMapVal* v = cscdStrMapGet(&se->expansions, cscdStrSv(k));
    if (v) return (const CscdStrPairVec*)v->p;
    CscdStrPairVec* out = CSCD_NEW(CscdStrPairVec);
    cscdExpandShorthand(prop, value, out);
    cscdStrMapPut(&se->expansions, cscdStrSv(k), NULL)->p = out;
    return out;
}

static void applyOne(CscdApplyState* st, const CscdComputedStyle* P, CscdSv prop, CscdSv val) {
    CscdComputedStyle* s = st->s;
    CscdSv v = cscdSvTrim(val);
    if (cscdSvIEqC(v, "inherit") || (cscdSvIEqC(v, "unset") && cscdStyleIsInherited(prop)) || (cscdSvIEqC(v, "revert") && cscdStyleIsInherited(prop))) {
        cscdCopyProperty(s, P, prop);
        if (cscdSvStartsC(prop, "border-") && cscdSvContains(prop, "-color")) {
            for (int i = 0; i < 4; ++i)
                if (cscdSvContains(prop, i == 0 ? "top" : i == 1 ? "right" : i == 2 ? "bottom" : "left")) st->colorSet |= 1u << i;
        }
        if (cscdSvEqC(prop, "outline-color")) st->colorSet |= 16;
        if (cscdSvStartsC(prop, "background-")) st->bgTouched = true;
        return;
    }
    if (cscdSvIEqC(v, "initial") || cscdSvIEqC(v, "unset") || cscdSvIEqC(v, "revert") || cscdSvIEqC(v, "revert-layer")) {
        cscdCopyProperty(s, cscdInitialStyle(), prop);
        if (cscdSvEqC(prop, "color")) s->color = cscdRgb(0, 0, 0);
        if (cscdSvStartsC(prop, "background-")) {
            st->bgTouched = true;
            if (cscdSvEqC(prop, "background-image")) cscdStrClear(&st->bgImage);
        }
        return;
    }
    cscdApplyLonghand(st, prop, v);
}

static CscdComputedStyle* computeMatched(CscdStyleEngine* se, CscdElement* e, const CscdComputedStyle* parent, const CscdStyleEnvironment* env, CscdSv pseudo,
                                       const IndexedRefs* sels, const CscdDeclVec* hints, bool* viewport) {
    /* 1. the declarations of the rules that matched, in cascade order */
    MatchVec matches = {0};
    for (size_t k = 0; k < sels->len; ++k) {
        const Indexed* ix = sels->items[k];
        const CscdDeclVec* decls = &ix->rule->declarations;
        for (uint32_t d = 0; d < decls->len; ++d) {
            const bool imp = decls->items[d].important;
            int rank = ix->origin == CSCD_ORIGIN_USER_AGENT ? (imp ? 3 : 0) : (imp ? 2 : 1);
            Match m = {&decls->items[d], rank, ix->sel->specificity, ix->sheetIndex, ix->rule->order, d};
            cscdVecPush(&matches, m);
        }
    }
    /* a selector indexed under one key can only be seen once, but :is() style rules may produce
     * duplicates across classes listed twice; the stable sort keeps the result correct regardless. */
    CscdDeclVec inl = {0};
    if (!pseudo.len) {
        for (uint32_t k = 0; k < hints->len; ++k) {
            Match m = {&hints->items[k], 1, 0, 0, 0, k};
            cscdVecPush(&matches, m);
        }
        const CscdStr* sty = cscdElementGetAttribute(e, CSCD_SV("style"));
        if (sty) {
            cscdParseDeclarations(cscdStrSv(sty), &inl);
            for (uint32_t k = 0; k < inl.len; ++k) {
                Match m = {&inl.items[k], inl.items[k].important ? 2 : 1, 0x1000000, 0xFFFF, 0xFFFFFF, k};
                cscdVecPush(&matches, m);
            }
        }
    }
    cscdSort(matches.items, matches.len, sizeof(Match), compareMatches, NULL);

    /* 2. inheritance baseline */
    CscdComputedStyle* s = cscdStyleClone(cscdInitialStyle());
    const CscdComputedStyle* P = parent ? parent : cscdInitialStyle();
    if (parent) {
        s->color = P->color;
        cscdStrVecCopy(&s->font.families, &P->font.families);
        cscdFontDescInherit(&s->font, &P->font);
        s->lineHeight = P->lineHeight; s->lineHeightFactor = P->lineHeightFactor;
        s->letterSpacing = P->letterSpacing; s->wordSpacing = P->wordSpacing; s->textAlign = P->textAlign; s->textIndent = P->textIndent;
        s->whiteSpace = P->whiteSpace; s->textTransform = P->textTransform; s->wordBreak = P->wordBreak; s->overflowWrap = P->overflowWrap;
        s->hyphens = P->hyphens;
        s->quotes = P->quotes;
        s->writingMode = P->writingMode;
        s->colorScheme = P->colorScheme;
        s->emptyCellsHide = P->emptyCellsHide;
        s->visibility = P->visibility; s->cursor = P->cursor; s->listStyle = P->listStyle; s->listInside = P->listInside;
        cscdStrSet(&s->listImage, cscdStrSv(&P->listImage));
        s->borderCollapse = P->borderCollapse; s->borderSpacingX = P->borderSpacingX; s->borderSpacingY = P->borderSpacingY;
        s->caretColor = P->caretColor; s->caretAuto = P->caretAuto; s->accentColor = P->accentColor; s->rtl = P->rtl;
        s->fill = P->fill; s->fillNone = P->fillNone; s->fillCurrent = P->fillCurrent; s->stroke = P->stroke; s->strokeNone = P->strokeNone;
        s->strokeCurrent = P->strokeCurrent; s->strokeWidth = P->strokeWidth; s->pointerEvents = P->pointerEvents;
        s->tabSize = P->tabSize;
        cscdVecCopy(&s->textShadow, &P->textShadow);
        s->userSelect = P->userSelect;
        cscdRelease(s->vars);
        s->vars = (CscdVars*)cscdRetain(P->vars);
        s->scrollbarThumb = P->scrollbarThumb; s->scrollbarThumbSet = P->scrollbarThumbSet;
        s->textAlignLast = P->textAlignLast; s->alignLastSet = P->alignLastSet;
        s->textFill = P->textFill; s->textFillSet = P->textFillSet;
        s->textStrokeWidth = P->textStrokeWidth; s->textStroke = P->textStroke; s->textStrokeSet = P->textStrokeSet;
        s->pixelated = P->pixelated; s->captionBottom = P->captionBottom;
        /* text decorations propagate to descendants in browsers; approximated through inheritance */
        s->underline = P->underline; s->overline = P->overline; s->lineThrough = P->lineThrough;
        s->decorationColor = P->decorationColor; s->decorationCurrent = false; s->decorationStyle = P->decorationStyle;
        s->decorationThickness = P->decorationThickness;
        s->underlineUnder = P->underlineUnder;
        s->emphasisShape = P->emphasisShape; s->emphasisOpen = P->emphasisOpen; s->emphasisUnder = P->emphasisUnder;
        s->emphasisColorSet = P->emphasisColorSet; s->emphasisColor = P->emphasisColor;
        memcpy(s->emphasisString, P->emphasisString, sizeof s->emphasisString);
        if (!P->underline && !P->overline && !P->lineThrough) s->decorationCurrent = true;
    }

    /* 3. custom properties */
    bool ownVars = false;
    for (size_t k = 0; k < matches.len; ++k) {
        const CscdDeclaration* d = matches.items[k].decl;
        if (!cscdSvStartsC(cscdStrSv(&d->property), "--")) continue;
        if (!ownVars) {
            CscdVars* nv = cscdVarsClone(s->vars);
            cscdRelease(s->vars);
            s->vars = nv;
            ownVars = true;
        }
        CscdSv v = cscdStrSv(&d->value);
        if (cscdSvIEqC(cscdSvTrim(v), "initial")) { cscdVarsRemove(s->vars, cscdStrSv(&d->property)); continue; }
        if (cscdSvIEqC(cscdSvTrim(v), "inherit")) {
            const CscdStr* pv = cscdVarsGet(P->vars, cscdStrSv(&d->property));
            if (pv) {
                CscdStr copy = cscdStrCopy(pv);
                cscdVarsSet(s->vars, cscdStrSv(&d->property), cscdStrSv(&copy));
                cscdStrFree(&copy);
            } else cscdVarsRemove(s->vars, cscdStrSv(&d->property));
            continue;
        }
        cscdVarsSet(s->vars, cscdStrSv(&d->property), v);
    }
    if (ownVars) { /* resolve references between custom properties once */
        CscdVars* copy = cscdVarsClone(s->vars);
        for (CscdStrMapSlot* slot = NULL; (slot = cscdStrMapNext(&s->vars->map, slot));) {
            CscdStr* val = (CscdStr*)slot->val.p;
            if (cscdSvFindC(cscdStrSv(val), "var(", 0) == CSCD_NPOS) continue;
            CscdStr r = substituteVars(cscdStrSv(val), copy);
            if (cscdStrEqC(&r, INVALID)) cscdStrClear(val);
            else cscdStrSet(val, cscdStrSv(&r));
            cscdStrFree(&r);
        }
        cscdRelease(copy);
    }

    /* 4. winners per longhand */
    Winners w;
    memset(&w, 0, sizeof w);
    CscdStrVec substituted = {0}; /* (the values the winners may point into) */
    if (se->expansions.len >= 4096) clearExpansions(&se->expansions); /* (values made by var()s that keep changing; not while winners point into them) */
    for (size_t k = 0; k < matches.len; ++k) {
        const CscdDeclaration* d = matches.items[k].decl;
        if (cscdSvStartsC(cscdStrSv(&d->property), "--")) continue;
        CscdSv prop = cscdCanonicalProperty(cscdStrSv(&d->property));
        /* (most values have no var() or env(): used as they are, not copied) */
        CscdSv value = cscdStrSv(&d->value);
        if (cscdSvFindC(value, "var(", 0) != CSCD_NPOS || cscdSvFindC(value, "env(", 0) != CSCD_NPOS) {
            CscdStr val = substituteVars(value, s->vars);
            if (cscdStrEqC(&val, INVALID)) cscdStrSetC(&val, "unset");
            cscdVecPush(&substituted, val);
            value = cscdStrSv(&cscdVecLast(&substituted));
        }
        if (cscdIsShorthand(prop)) {
            const CscdStrPairVec* longhands = expansion(se, prop, value);
            for (size_t q = 0; q < longhands->len; ++q) setWinner(&w, cscdStrSv(&longhands->items[q].name), cscdStrSv(&longhands->items[q].value));
        } else setWinner(&w, prop, value);
    }

    /* 5. apply: font first (em units), then color (currentColor), then the rest */
    CscdApplyState st;
    cscdApplyStateInit(&st, s, parent, env, e->node.owner ? cscdStrSv(&e->node.owner->baseDir) : CSCD_SV(""));
    st.valueCache = &se->values;
    st.ctx.rem = env->rootFontSize;
    st.ctx.vw = env->media.width;
    st.ctx.vh = env->media.height;
    st.ctx.viewport = viewport;
    {
        float size[2];
        const CscdElement* box = containerFor(se, e, pseudo.len > 0, CSCD_SV(""), size);
        if (box) {
            st.ctx.cqw = size[0];
            st.ctx.cqh = box->style->containerType == 2 ? size[1] : -1;
        }
    }
    bool containerUnits = false;
    st.ctx.container = &containerUnits;
    /* the root's font size, when nothing sets it: medium, at the system's text size */
    const float baseSize = parent ? P->font.size : 16 * (env->media.textScale > 0 ? env->media.textScale : 1);
    if (!parent) s->font.size = baseSize;
    st.ctx.em = baseSize;
    s->zoom = 1;
    s->effectiveZoom = P->effectiveZoom > 0 ? P->effectiveZoom : 1;
    st.ctx.zoom = s->effectiveZoom;
    {
        const SvPair* z = findWinner(&w, CSCD_SV("zoom"));
        if (z) applyOne(&st, P, z->name, z->value);
    }
    static const char* first[] = {"font-size", "font-family", "font-weight", "font-style", "font-variant", "line-height", "color"};
    size_t applied[CSCD_COUNT(first)], nApplied = 0; /* (the winners already applied) */
    for (size_t f = 0; f < CSCD_COUNT(first); ++f) {
        const SvPair* won = findWinner(&w, cscdSv(first[f]));
        if (won) {
            applyOne(&st, P, won->name, won->value);
            applied[nApplied++] = (size_t)(won - w.list.items);
        }
        if (f == 0) {
            if (!won && s->zoom != 1) s->font.size = baseSize * s->zoom; /* (inherited, then zoomed) */
            st.ctx.em = s->font.size;
        }
    }
    st.ctx.em = s->font.size;
    if (cscdElementIs(e, "html")) st.ctx.rem = s->font.size;
    for (size_t k = 0; k < w.list.len; ++k) {
        bool done = false;
        for (size_t f = 0; f < nApplied && !done; ++f) done = applied[f] == k;
        if (!done) applyOne(&st, P, w.list.items[k].name, w.list.items[k].value);
    }
    cscdFinalizeStyle(&st);
    s->verticalRoot = s->writingMode != CSCD_WRITING_HORIZONTAL && P->writingMode == CSCD_WRITING_HORIZONTAL;
    cscdApplyStateFini(&st);
    s->originalDisplay = s->display;
    if (containerUnits) se->lastUsedContainerUnits = true; /* (such a style is not kept: see styleFor) */
    if (s->containerType) se->hasContainers = true;
    cscdVecFree(&w.list);
    cscdFree(w.slots);
    cscdStrVecFree(&substituted);
    cscdDeclVecFree(&inl);
    cscdVecFree(&matches);
    return s;
}

static void keyAdd(CscdStr* k, const void* p, size_t n) { cscdStrAppend(k, cscdSvMake((const char*)p, n)); }

/* A new reference, or NULL (a pseudo element without content). */
static CscdComputedStyle* styleFor(CscdStyleEngine* se, CscdElement* e, const CscdComputedStyle* parent, const CscdStyleEnvironment* env, int pseudo, bool root,
                                 const IndexedRefs* sels) {
    CscdDeclVec hints = {0};
    if (pseudo == 0) presentationalHints(e, &hints);
    /* the key; a parent the cache does not hold (an animated style): computed, not kept */
    CscdStr* k = &se->key;
    bool keep = true;
    uint64_t parentId = 0;
    if (parent) {
        CscdMapVal* v = cscdU64MapGet(&se->cache.ids, CSCD_PKEY(parent));
        if (!v) keep = false;
        else parentId = v->u;
    }
    if (keep) {
        cscdStrClear(k);
        keyAdd(k, &parentId, sizeof parentId);
        uint32_t flags = (uint32_t)pseudo | (root ? 8u : 0u) | (cscdElementIs(e, "html") ? 16u : 0u) | (env->media.dark ? 32u : 0u) | (env->media.print ? 64u : 0u);
        keyAdd(k, &flags, sizeof flags);
        keyAdd(k, &env->media.dpr, sizeof(float));
        keyAdd(k, &env->rootFontSize, sizeof(float));
        keyAdd(k, &env->media.textScale, sizeof(float));
        uint64_t n = sels->len;
        keyAdd(k, &n, sizeof n);
        for (size_t i = 0; i < sels->len; ++i) keyAdd(k, &sels->items[i]->sel, sizeof(void*)); /* (stable while the sheets are) */
        for (size_t i = 0; i < hints.len; ++i) {
            cscdStrAppend(k, cscdStrSv(&hints.items[i].property));
            cscdStrAppendChar(k, ':');
            cscdStrAppend(k, cscdStrSv(&hints.items[i].value));
            cscdStrAppendChar(k, ';');
        }
        cscdStrAppendChar(k, '\x1f');
        if (pseudo == 0) {
            const CscdStr* sty = cscdElementGetAttribute(e, CSCD_SV("style"));
            if (sty) cscdStrAppend(k, cscdStrSv(sty));
        }
        cscdStrAppendChar(k, '\x1f');
        if (e->node.owner) cscdStrAppend(k, cscdStrSv(&e->node.owner->baseDir));
        CacheEntry* hit = cacheFind(&se->cache, cscdStrSv(k));
        if (hit && (!hit->viewport || (hit->w == env->media.width && hit->h == env->media.height))) {
            if (hit->style && hit->style->containerType) se->hasContainers = true;
            if (hit->viewport) se->usedViewport = true;
            ++se->reused;
            cscdDeclVecFree(&hints);
            return (CscdComputedStyle*)cscdRetain(hit->style);
        }
    }
    bool viewport = false;
    se->lastUsedContainerUnits = false;
    CscdComputedStyle* s = computeMatched(se, e, parent, env, cscdSv(kPseudos[pseudo]), sels, &hints, &viewport);
    cscdDeclVecFree(&hints);
    if (viewport) se->usedViewport = true;
    ++se->computed;
    if (se->lastUsedContainerUnits) keep = false; /* (container units: the container's size is not in the key) */
    if (pseudo == 0) {
        /* blockification */
        if (root || s->floating != CSCD_FLOAT_NONE || s->position == CSCD_POS_ABSOLUTE || s->position == CSCD_POS_FIXED) blockify(s);
        if (parent && (cscdStyleIsFlex(parent) || cscdStyleIsGrid(parent)) && s->display != CSCD_DISPLAY_CONTENTS && s->display != CSCD_DISPLAY_NONE) blockify(s);
        if (s->position == CSCD_POS_ABSOLUTE || s->position == CSCD_POS_FIXED) s->floating = CSCD_FLOAT_NONE;
        if (parent && (cscdStyleIsFlex(parent) || cscdStyleIsGrid(parent))) s->floating = CSCD_FLOAT_NONE; /* floats inside flex containers are ignored by layout */
    } else if ((pseudo == 1 || pseudo == 2) && !s->hasContent) {
        cscdRelease(s);
        s = NULL;
    } else {
        if (s->position == CSCD_POS_ABSOLUTE || s->position == CSCD_POS_FIXED || s->floating != CSCD_FLOAT_NONE) blockify(s);
        if (cscdStyleIsFlex(parent) || cscdStyleIsGrid(parent)) { blockify(s); s->floating = CSCD_FLOAT_NONE; }
    }
    if (keep) {
        if (se->cache.map.len > 65536) cacheClear(&se->cache); /* (styles that keep changing within a pass: a huge document) */
        const uint64_t id = se->cache.next++;
        bool inserted = false;
        CscdMapVal* v = cscdStrMapPut(&se->cache.map, cscdStrSv(k), &inserted);
        if (inserted) v->p = CSCD_NEW(CacheEntry);
        CacheEntry* entry = (CacheEntry*)v->p;
        if (entry->style) cscdU64MapRemove(&se->cache.ids, CSCD_PKEY(entry->style), NULL); /* (the same rules at another viewport size) */
        cscdRelease(entry->style);
        entry->style = (CscdComputedStyle*)cscdRetain(s);
        entry->id = id;
        entry->viewport = viewport;
        entry->w = env->media.width;
        entry->h = env->media.height;
        entry->used = se->cache.pass;
        if (s) cscdU64MapPut(&se->cache.ids, CSCD_PKEY(s), NULL)->u = id;
    }
    return s;
}

static PseudoSet* pseudoSetFor(CscdStyleEngine* se, const CscdElement* e) {
    bool inserted = false;
    CscdMapVal* v = cscdU64MapPut(&se->pseudo, CSCD_PKEY(e), &inserted);
    if (inserted) v->p = CSCD_NEW(PseudoSet);
    return (PseudoSet*)v->p;
}

static void computeTree(CscdStyleEngine* se, CscdElement* e, const CscdComputedStyle* parent, const CscdStyleEnvironment* env) {
    const bool root = e->node.parent && e->node.parent->type == CSCD_NODE_DOCUMENT;
    /* the rules matching e and each of its pseudo elements, in one pass over the index */
    for (int i = 0; i < 6; ++i) cscdVecClear(&se->matched[i]);
    matchRules(se, e, se->matched);
    CscdComputedStyle* s = styleFor(se, e, parent, env, 0, root, &se->matched[0]);
    cscdRelease(e->style);
    e->style = s;

    CscdStyleEnvironment rootEnv;
    const CscdStyleEnvironment* childEnv = env;
    if (root) {
        rootEnv = *env;
        rootEnv.rootFontSize = s->font.size;
        childEnv = &rootEnv;
    }

    if (s->display != CSCD_DISPLAY_NONE) {
        const bool field = cscdElementIs(e, "input") || cscdElementIs(e, "textarea");
        for (int p = 1; p < 6; ++p) {
            bool want = p == 3 || p == 5 ? field : p == 4 ? s->display == CSCD_DISPLAY_LIST_ITEM : true;
            if (!want || !se->matched[p].len) continue; /* only when some rule targets this pseudo element */
            CscdComputedStyle* ps = styleFor(se, e, s, childEnv, p, root, &se->matched[p]);
            if (ps) {
                PseudoSet* set = pseudoSetFor(se, e);
                cscdRelease(set->s[p]);
                set->s[p] = ps;
            }
        }
    }
    for (size_t i = 0; i < e->node.children.len; ++i) {
        CscdElement* ce = cscdNodeAsElement(cscdNodeChild(&e->node, i));
        if (ce) computeTree(se, ce, s, childEnv);
    }
}

static bool mediaSame(const CscdMediaContext* a, const CscdMediaContext* b) {
    return a->width == b->width && a->height == b->height && a->dark == b->dark && a->dpr == b->dpr && a->print == b->print &&
           a->reducedMotion == b->reducedMotion && a->textScale == b->textScale;
}

void cscdStyleEngineCounts(const CscdStyleEngine* se, uint64_t* computed, uint64_t* reused) {
    if (computed) *computed = se->computed;
    if (reused) *reused = se->reused;
}

bool cscdStyleEngineMediaChanges(CscdStyleEngine* se, const CscdMediaContext* media) {
    if (!se->indexValid) return true;
    if (mediaSame(&se->indexMedia, media)) return false;
    const CscdMediaContext* was = &se->indexMedia;
    if (se->usedViewport || was->dark != media->dark || was->dpr != media->dpr || was->print != media->print || was->reducedMotion != media->reducedMotion ||
        was->textScale != media->textScale)
        return true;
    const CscdStr* last = NULL; /* (the rules of one @media block follow each other: evaluated once) */
    for (size_t si = 0; si < se->sheets.len; ++si) {
        const CscdStyleSheet* sheet = (const CscdStyleSheet*)se->sheets.items[si];
        for (size_t ri = 0; ri < sheet->rules.len; ++ri) {
            const CscdStr* m = &sheet->rules.items[ri].media;
            if (!m->len || (last && cscdStrEq(last, m))) continue;
            last = m;
            if (cscdEvalMediaQuery(cscdStrSv(m), was) != cscdEvalMediaQuery(cscdStrSv(m), media)) return true;
        }
    }
    se->indexMedia = *media;
    return false;
}

void cscdStyleEngineComputeAll(CscdStyleEngine* se, CscdDocument* doc, const CscdStyleEnvironment* env) {
    if (!se->indexValid || !mediaSame(&se->indexMedia, &env->media)) buildIndex(se, &env->media);
    cacheNewPass(&se->cache);
    se->hasContainers = false;
    se->usedViewport = false;
    clearPseudo(se);
    CscdElement* root = cscdDocumentElement(doc);
    if (root) computeTree(se, root, NULL, env);
}

static void forgetPseudo(CscdStyleEngine* se, CscdNode* n) {
    if (cscdNodeIsElement(n)) {
        CscdMapVal old;
        if (cscdU64MapRemove(&se->pseudo, CSCD_PKEY(n), &old)) {
            PseudoSet* p = (PseudoSet*)old.p;
            for (int i = 0; i < 6; ++i) cscdRelease(p->s[i]);
            cscdFree(p);
        }
    }
    for (size_t i = 0; i < n->children.len; ++i) forgetPseudo(se, cscdNodeChild(n, i));
}

bool cscdStyleEngineComputeSubtree(CscdStyleEngine* se, CscdElement* e, const CscdStyleEnvironment* env) {
    if (!se->indexValid || !mediaSame(&se->indexMedia, &env->media)) return false;
    CscdElement* parent = cscdNodeParentElement(&e->node);
    if (!parent || !parent->style) return false; /* (the root: the whole document) */
    /* its pseudo elements are worked out again */
    forgetPseudo(se, &e->node);
    CscdStyleEnvironment childEnv = *env;
    const CscdElement* root = e->node.owner ? cscdDocumentElement(e->node.owner) : NULL;
    if (root && root->style) childEnv.rootFontSize = root->style->font.size;
    computeTree(se, e, parent->style, &childEnv);
    return true;
}

CscdComputedStyle* cscdStyleEnginePseudoStyle(const CscdStyleEngine* se, const CscdElement* e, CscdSv pseudo) {
    CscdMapVal* v = cscdU64MapGet(&se->pseudo, CSCD_PKEY(e));
    if (!v) return NULL;
    int p = pseudoIndex(pseudo);
    if (p <= 0) return NULL;
    return ((PseudoSet*)v->p)->s[p];
}
