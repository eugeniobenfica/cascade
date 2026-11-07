/* HTML tokenizer and tree builder. Follows the HTML5 parsing model closely enough for real pages:
 * implied <html>/<head>/<body>, optional end tags (p, li, dt, dd, option, tr, td...), void
 * elements, raw text elements, character references and implied <tbody>. */
#include "cascade/dom.h"
#include "core/thread.h"

typedef struct Entity { const char* name; uint32_t cp; } Entity;

static const Entity kEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"hellip", 0x2026}, {"mdash", 0x2014},
    {"ndash", 0x2013}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
    {"sbquo", 0x201A}, {"bdquo", 0x201E}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"lsaquo", 0x2039},
    {"rsaquo", 0x203A}, {"bull", 0x2022}, {"middot", 0xB7}, {"times", 0xD7}, {"divide", 0xF7},
    {"plusmn", 0xB1}, {"deg", 0xB0}, {"micro", 0xB5}, {"para", 0xB6}, {"sect", 0xA7},
    {"cent", 0xA2}, {"pound", 0xA3}, {"euro", 0x20AC}, {"yen", 0xA5}, {"curren", 0xA4},
    {"iexcl", 0xA1}, {"iquest", 0xBF}, {"shy", 0xAD}, {"ensp", 0x2002}, {"emsp", 0x2003},
    {"thinsp", 0x2009}, {"zwnj", 0x200C}, {"zwj", 0x200D}, {"larr", 0x2190}, {"uarr", 0x2191},
    {"rarr", 0x2192}, {"darr", 0x2193}, {"harr", 0x2194}, {"lArr", 0x21D0}, {"rArr", 0x21D2},
    {"hArr", 0x21D4}, {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260}, {"asymp", 0x2248},
    {"infin", 0x221E}, {"minus", 0x2212}, {"sum", 0x2211}, {"prod", 0x220F}, {"radic", 0x221A},
    {"part", 0x2202}, {"int", 0x222B}, {"forall", 0x2200}, {"exist", 0x2203}, {"isin", 0x2208},
    {"empty", 0x2205}, {"nabla", 0x2207}, {"and", 0x2227}, {"or", 0x2228}, {"cap", 0x2229},
    {"cup", 0x222A}, {"sub", 0x2282}, {"sup", 0x2283}, {"alpha", 0x3B1}, {"beta", 0x3B2},
    {"gamma", 0x3B3}, {"delta", 0x3B4}, {"epsilon", 0x3B5}, {"theta", 0x3B8}, {"lambda", 0x3BB},
    {"mu", 0x3BC}, {"pi", 0x3C0}, {"sigma", 0x3C3}, {"tau", 0x3C4}, {"phi", 0x3C6},
    {"omega", 0x3C9}, {"Delta", 0x394}, {"Sigma", 0x3A3}, {"Omega", 0x3A9}, {"Pi", 0x3A0},
    {"hearts", 0x2665}, {"spades", 0x2660}, {"clubs", 0x2663}, {"diams", 0x2666},
    {"check", 0x2713}, {"cross", 0x2717}, {"star", 0x2606}, {"starf", 0x2605},
    {"agrave", 0xE0}, {"aacute", 0xE1}, {"acirc", 0xE2}, {"atilde", 0xE3}, {"auml", 0xE4},
    {"ccedil", 0xE7}, {"egrave", 0xE8}, {"eacute", 0xE9}, {"ecirc", 0xEA}, {"euml", 0xEB},
    {"igrave", 0xEC}, {"iacute", 0xED}, {"icirc", 0xEE}, {"iuml", 0xEF}, {"ntilde", 0xF1},
    {"ograve", 0xF2}, {"oacute", 0xF3}, {"ocirc", 0xF4}, {"otilde", 0xF5}, {"ouml", 0xF6},
    {"ugrave", 0xF9}, {"uacute", 0xFA}, {"ucirc", 0xFB}, {"uuml", 0xFC}, {"szlig", 0xDF},
    {"Agrave", 0xC0}, {"Aacute", 0xC1}, {"Acirc", 0xC2}, {"Atilde", 0xC3}, {"Auml", 0xC4},
    {"Ccedil", 0xC7}, {"Egrave", 0xC8}, {"Eacute", 0xC9}, {"Ecirc", 0xCA}, {"Iacute", 0xCD},
    {"Ntilde", 0xD1}, {"Oacute", 0xD3}, {"Ocirc", 0xD4}, {"Otilde", 0xD5}, {"Ouml", 0xD6},
    {"Uacute", 0xDA}, {"Uuml", 0xDC},
};

static CscdStrMap entityMap;
static CscdOnce entityOnce = CSCD_ONCE_INIT;
static void initEntities(void) {
    for (size_t i = 0; i < CSCD_COUNT(kEntities); ++i) cscdStrMapPut(&entityMap, cscdSv(kEntities[i].name), NULL)->u = kEntities[i].cp;
}

static bool inList(CscdSv s, const char* const* list, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (cscdSvEqC(s, list[i])) return true;
    return false;
}

static const char* kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                              "param", "source", "track", "wbr", "keygen"};
static const char* kClosesP[] = {"address", "article", "aside", "blockquote", "details", "dialog", "div", "dl", "fieldset",
                                 "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header",
                                 "hgroup", "hr", "main", "menu", "nav", "ol", "p", "pre", "section", "table", "ul", "summary"};
static const char* kHeadOnly[] = {"meta", "link", "title", "base", "style", "script", "noscript", "template"};
/* Elements that stop a scope search for an end tag (simplified "has an element in scope"). */
static const char* kScopeBarrier[] = {"html", "table", "td", "th", "caption", "marquee", "object", "template", "svg"};
static const char* kPBarrier[] = {"html", "table", "td", "th", "button", "object", "template", "svg"};

#define IN(s, list) inList((s), (list), CSCD_COUNT(list))

void cscdDecodeEntities(CscdSv s, CscdStr* out) {
    if (cscdSvFindChar(s, '&', 0) == CSCD_NPOS) {
        cscdStrAppend(out, s);
        return;
    }
    cscdCallOnce(&entityOnce, initEntities);
    cscdStrReserve(out, out->len + s.len);
    for (size_t i = 0; i < s.len;) {
        if (s.ptr[i] != '&') { cscdStrAppendChar(out, s.ptr[i++]); continue; }
        size_t j = i + 1;
        if (j < s.len && s.ptr[j] == '#') {
            ++j;
            bool hex = j < s.len && (s.ptr[j] == 'x' || s.ptr[j] == 'X');
            if (hex) ++j;
            size_t k = j;
            uint32_t cp = 0;
            while (k < s.len && (hex ? cscdIsXDigit(s.ptr[k]) : cscdIsDigit(s.ptr[k]))) {
                char c = s.ptr[k];
                int v = c <= '9' ? c - '0' : (cscdToLower(c) - 'a' + 10);
                cp = cp * (hex ? 16 : 10) + (uint32_t)v;
                if (cp > 0x10FFFF) cp = 0xFFFD;
                ++k;
            }
            if (k == j) { cscdStrAppendChar(out, s.ptr[i++]); continue; }
            if (k < s.len && s.ptr[k] == ';') ++k;
            if (cp == 0) cp = 0xFFFD;
            cscdUtf8Append(out, cp);
            i = k;
            continue;
        }
        size_t k = j;
        while (k < s.len && k - j < 32 && cscdIsAlnum(s.ptr[k])) ++k;
        /* longest match first, so "&notin;" is not read as "&not" + "in;" */
        bool done = false;
        for (size_t e = k; e > j; --e) {
            CscdMapVal* v = cscdStrMapGet(&entityMap, cscdSvSub(s, j, e - j));
            if (v) {
                cscdUtf8Append(out, (uint32_t)v->u);
                i = e;
                if (i < s.len && s.ptr[i] == ';') ++i;
                done = true;
                break;
            }
        }
        if (!done) cscdStrAppendChar(out, s.ptr[i++]);
    }
}

void cscdEscapeHTML(CscdSv s, bool attribute, CscdStr* out) {
    for (size_t i = 0; i < s.len; ++i) {
        char c = s.ptr[i];
        switch (c) {
        case '&': cscdStrAppendC(out, "&amp;"); break;
        case '<': cscdStrAppendC(out, "&lt;"); break;
        case '>': cscdStrAppendC(out, "&gt;"); break;
        case '"':
            if (attribute) { cscdStrAppendC(out, "&quot;"); break; }
            cscdStrAppendChar(out, c);
            break;
        default: cscdStrAppendChar(out, c);
        }
    }
}

/* ---- tokenizer ------------------------------------------------------------------------------------ */

typedef enum TokKind { TOK_START, TOK_END_TAG, TOK_CHARS, TOK_COMMENT, TOK_DOCTYPE, TOK_EOF } TokKind;

typedef struct Token {
    TokKind kind;
    CscdStr name;
    CscdStr data;
    CscdStrPairVec attrs;
    bool selfClosing;
} Token;

static void tokenFree(Token* t) {
    cscdStrFree(&t->name);
    cscdStrFree(&t->data);
    cscdStrPairVecFree(&t->attrs);
}

typedef struct Tokenizer {
    CscdSv s;
    size_t i;
    CscdStr rawEnd;
    bool rawDecode;
} Tokenizer;

static Token tzNext(Tokenizer* tz);

static Token tzRawText(Tokenizer* tz) {
    Token t;
    memset(&t, 0, sizeof t);
    size_t e = tz->i;
    for (;;) {
        e = cscdSvFindC(tz->s, "</", e);
        if (e == CSCD_NPOS) { e = tz->s.len; break; }
        if (cscdSvIEq(cscdSvSub(tz->s, e, tz->rawEnd.len), cscdStrSv(&tz->rawEnd))) break;
        e += 2;
    }
    t.kind = TOK_CHARS;
    CscdSv body = cscdSvSub(tz->s, tz->i, e - tz->i);
    if (tz->rawDecode) cscdDecodeEntities(body, &t.data);
    else cscdStrSet(&t.data, body);
    tz->i = e;
    cscdStrClear(&tz->rawEnd);
    if (!t.data.len) {
        tokenFree(&t);
        return tzNext(tz);
    }
    return t;
}

static Token tzMarkup(Tokenizer* tz) {
    Token t;
    memset(&t, 0, sizeof t);
    CscdSv s = tz->s;
    if (cscdSvStartsC(cscdSvFrom(s, tz->i), "<!--")) {
        size_t e = cscdSvFindC(s, "-->", tz->i + 4);
        t.kind = TOK_COMMENT;
        if (e == CSCD_NPOS) { cscdStrSet(&t.data, cscdSvFrom(s, tz->i + 4)); tz->i = s.len; }
        else { cscdStrSet(&t.data, cscdSvSub(s, tz->i + 4, e - tz->i - 4)); tz->i = e + 3; }
        return t;
    }
    size_t e = cscdSvFindChar(s, '>', tz->i);
    if (e == CSCD_NPOS) e = s.len;
    CscdSv body = cscdSvSub(s, tz->i + 2, e - tz->i - 2);
    t.kind = cscdSvIEqC(cscdSvSub(body, 0, 7), "doctype") ? TOK_DOCTYPE : TOK_COMMENT;
    if (cscdSvIEqC(cscdSvSub(body, 0, 7), "[cdata[")) {
        size_t ce = cscdSvFindC(s, "]]>", tz->i);
        if (ce == CSCD_NPOS) ce = s.len;
        t.kind = TOK_CHARS;
        cscdStrSet(&t.data, cscdSvSub(s, tz->i + 9, ce >= tz->i + 9 ? ce - tz->i - 9 : 0));
        tz->i = cscdMinz(s.len, ce + 3);
        return t;
    }
    cscdStrSet(&t.data, body);
    tz->i = cscdMinz(s.len, e + 1);
    return t;
}

#define MANY_ATTRIBUTES 32

static Token tzTag(Tokenizer* tz, bool end) {
    Token t;
    memset(&t, 0, sizeof t);
    t.kind = end ? TOK_END_TAG : TOK_START;
    const char* s = tz->s.ptr;
    const size_t n = tz->s.len;
    size_t i = tz->i + (end ? 2 : 1);
    size_t a0 = i;
    while (i < n && !cscdIsSpace(s[i]) && s[i] != '/' && s[i] != '>') ++i;
    t.name = cscdLower(cscdSvSub(tz->s, a0, i - a0));
    CscdStrMap seen = {0}; /* (the names so far, once they are too many to compare one by one) */
    for (;;) {
        while (i < n && (cscdIsSpace(s[i]) || (s[i] == '/' && !(i + 1 < n && s[i + 1] == '>')))) ++i;
        if (i >= n) break;
        if (s[i] == '>') { ++i; break; }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '>') { t.selfClosing = true; i += 2; break; }
        size_t a = i;
        while (i < n && !cscdIsSpace(s[i]) && s[i] != '=' && s[i] != '>' && !(s[i] == '/' && i + 1 < n && s[i + 1] == '>')) ++i;
        CscdStr name = cscdLower(cscdSvSub(tz->s, a, i - a));
        while (i < n && cscdIsSpace(s[i])) ++i;
        CscdStr value = {0};
        if (i < n && s[i] == '=') {
            ++i;
            while (i < n && cscdIsSpace(s[i])) ++i;
            if (i < n && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                size_t v = i;
                while (i < n && s[i] != q) ++i;
                cscdDecodeEntities(cscdSvSub(tz->s, v, i - v), &value);
                if (i < n) ++i;
            } else {
                size_t v = i;
                while (i < n && !cscdIsSpace(s[i]) && s[i] != '>') ++i;
                cscdDecodeEntities(cscdSvSub(tz->s, v, i - v), &value);
            }
        }
        if (!name.len) {
            cscdStrFree(&name);
            cscdStrFree(&value);
            ++i;
            continue;
        }
        bool dup = false;
        if (t.attrs.len < MANY_ATTRIBUTES) {
            for (size_t k = 0; k < t.attrs.len; ++k) dup = dup || cscdStrEq(&t.attrs.items[k].name, &name);
        } else {
            if (!seen.len) for (size_t k = 0; k < t.attrs.len; ++k) cscdStrMapPut(&seen, cscdStrSv(&t.attrs.items[k].name), NULL);
            bool inserted = false;
            cscdStrMapPut(&seen, cscdStrSv(&name), &inserted);
            dup = !inserted;
        }
        if (!dup) {
            CscdStrPair p = {name, value};
            cscdVecPush(&t.attrs, p);
        } else {
            cscdStrFree(&name);
            cscdStrFree(&value);
        }
    }
    cscdStrMapFree(&seen);
    tz->i = i;
    return t;
}

static Token tzNext(Tokenizer* tz) {
    Token t;
    memset(&t, 0, sizeof t);
    t.kind = TOK_EOF;
    if (tz->rawEnd.len) return tzRawText(tz);
    const CscdSv s = tz->s;
    if (tz->i >= s.len) return t;
    if (s.ptr[tz->i] == '<' && tz->i + 1 < s.len) {
        char c = s.ptr[tz->i + 1];
        if (c == '!') return tzMarkup(tz);
        if (c == '/' && tz->i + 2 < s.len && cscdIsAlpha(s.ptr[tz->i + 2])) return tzTag(tz, true);
        if (cscdIsAlpha(c)) return tzTag(tz, false);
        if (c == '?') { /* bogus comment <?xml ... ?> */
            size_t e = cscdSvFindChar(s, '>', tz->i);
            t.kind = TOK_COMMENT;
            tz->i = e == CSCD_NPOS ? s.len : e + 1;
            return t;
        }
    }
    size_t start = tz->i;
    ++tz->i;
    while (tz->i < s.len && s.ptr[tz->i] != '<') ++tz->i;
    t.kind = TOK_CHARS;
    cscdDecodeEntities(cscdSvSub(s, start, tz->i - start), &t.data);
    return t;
}

/* After a <script>, <style>, <textarea> or <title> start tag the content is text up to the
 * matching end tag. */
static void tzSetRawText(Tokenizer* tz, CscdSv tagName, bool decode) {
    cscdStrSetC(&tz->rawEnd, "</");
    cscdStrAppend(&tz->rawEnd, tagName);
    tz->rawDecode = decode;
}

/* ---- tree builder --------------------------------------------------------------------------------- */

typedef struct TreeBuilder {
    CscdDocument* doc;
    CscdElement* html;
    CscdElement* head;
    CscdElement* body;
    CscdPtrVec stack; /* CscdElement* */
    bool inBody;
    bool fragment;
} TreeBuilder;

static CscdElement* current(TreeBuilder* tb) { return tb->stack.len ? (CscdElement*)cscdVecLast(&tb->stack) : NULL; }

/* Whether a <template> is open: what it holds nests in it, wherever it is (even in the head). */
static bool inTemplate(const TreeBuilder* tb) {
    for (size_t i = tb->stack.len; i-- > 0;)
        if (cscdElementIs((const CscdElement*)tb->stack.items[i], "template")) return true;
    return false;
}

static void setStack1(TreeBuilder* tb, CscdElement* a) { tb->stack.len = 0; cscdVecPush(&tb->stack, a); }
static void setStack2(TreeBuilder* tb, CscdElement* a, CscdElement* b) { setStack1(tb, a); cscdVecPush(&tb->stack, b); }

static void ensureHtml(TreeBuilder* tb) {
    if (tb->html) return;
    tb->html = cscdElementNew(CSCD_SV("html"));
    cscdNodeAppendChild(&tb->doc->node, &tb->html->node);
    setStack1(tb, tb->html);
}

static void ensureHead(TreeBuilder* tb) {
    ensureHtml(tb);
    if (tb->head) return;
    tb->head = cscdElementNew(CSCD_SV("head"));
    cscdNodeInsertBefore(&tb->html->node, &tb->head->node, tb->html->node.children.len ? cscdNodeChild(&tb->html->node, 0) : NULL);
}

static void ensureBody(TreeBuilder* tb) {
    ensureHead(tb);
    if (tb->body) return;
    tb->body = cscdElementNew(CSCD_SV("body"));
    cscdNodeAppendChild(&tb->html->node, &tb->body->node);
    setStack2(tb, tb->html, tb->body);
    tb->inBody = true;
}

static void insertNode(TreeBuilder* tb, CscdNode* n) {
    if (!tb->fragment && !tb->inBody && !inTemplate(tb)) {
        if (n->type == CSCD_NODE_COMMENT) {
            ensureHtml(tb);
            cscdNodeAppendChild(&current(tb)->node, n);
            return;
        }
        ensureBody(tb);
    }
    cscdNodeAppendChild(&current(tb)->node, n);
}

static bool allSpace(CscdSv s) {
    for (size_t i = 0; i < s.len; ++i)
        if (!cscdIsSpace(s.ptr[i])) return false;
    return true;
}

static void chars(TreeBuilder* tb, CscdSv data) {
    /* text of <title>, <style>, <script> in the head stays there */
    if (!tb->fragment && !tb->inBody && current(tb) && current(tb) != tb->html && current(tb) != tb->head) {
        cscdNodeAppendChild(&current(tb)->node, &cscdTextNew(data)->node);
        return;
    }
    if (!tb->fragment && !tb->inBody) {
        if (allSpace(data)) return;
        ensureBody(tb);
    }
    CscdElement* cur = current(tb);
    /* text directly in a table goes before it (foster parenting, simplified: drop whitespace) */
    if (cscdElementIs(cur, "table") || cscdElementIs(cur, "tbody") || cscdElementIs(cur, "tr") || cscdElementIs(cur, "thead") || cscdElementIs(cur, "tfoot")) {
        if (allSpace(data)) return;
    }
    if (cur->node.children.len && cscdNodeIsText((CscdNode*)cscdVecLast(&cur->node.children))) {
        cscdStrAppend(&((CscdText*)cscdVecLast(&cur->node.children))->data, data);
        return;
    }
    cscdNodeAppendChild(&cur->node, &cscdTextNew(data)->node);
}

static bool inStack(TreeBuilder* tb, const char* tag, const char* const* barrier, size_t nb) {
    for (size_t k = tb->stack.len; k-- > 0;) {
        CscdElement* e = (CscdElement*)tb->stack.items[k];
        if (cscdStrEqC(&e->tag, tag)) return true;
        if (inList(cscdStrSv(&e->tag), barrier, nb)) return false;
    }
    return false;
}

static size_t minDepth(const TreeBuilder* tb) { return tb->fragment ? 1 : (tb->body ? 2 : 1); }

static void popUntil(TreeBuilder* tb, CscdSv tag) {
    while (tb->stack.len > minDepth(tb)) {
        CscdElement* e = (CscdElement*)cscdVecPop(&tb->stack);
        if (cscdSvEq(cscdStrSv(&e->tag), tag)) return;
    }
}

static void closeP(TreeBuilder* tb) {
    if (inStack(tb, "p", kPBarrier, CSCD_COUNT(kPBarrier))) popUntil(tb, CSCD_SV("p"));
}

static void closeSibling(TreeBuilder* tb, const char* const* same, size_t ns, const char* const* stops, size_t nst) {
    for (size_t k = tb->stack.len; k-- > 0;) {
        CscdElement* e = (CscdElement*)tb->stack.items[k];
        CscdSv t = cscdStrSv(&e->tag);
        if (inList(t, same, ns)) {
            CscdStr tag = cscdStrDup(t); /* (popUntil pops e) */
            popUntil(tb, cscdStrSv(&tag));
            cscdStrFree(&tag);
            return;
        }
        if (inList(t, stops, nst)) return;
        if (cscdSvEqC(t, "div") || cscdSvEqC(t, "section") || cscdSvEqC(t, "article") || cscdSvEqC(t, "body")) return;
    }
}

#define CLOSE_SIBLING(tb, same, stops) closeSibling((tb), (same), CSCD_COUNT(same), (stops), CSCD_COUNT(stops))

static void closeImplied(TreeBuilder* tb, CscdSv tag) {
    if (IN(tag, kClosesP)) closeP(tb);
    if (cscdSvEqC(tag, "li")) {
        static const char* same[] = {"li"};
        static const char* stops[] = {"ul", "ol", "menu"};
        CLOSE_SIBLING(tb, same, stops);
    } else if (cscdSvEqC(tag, "dt") || cscdSvEqC(tag, "dd")) {
        static const char* same[] = {"dt", "dd"};
        static const char* stops[] = {"dl"};
        CLOSE_SIBLING(tb, same, stops);
    } else if (cscdSvEqC(tag, "option")) {
        if (cscdElementIs(current(tb), "option")) tb->stack.len--;
    } else if (cscdSvEqC(tag, "optgroup")) {
        if (cscdElementIs(current(tb), "option")) tb->stack.len--;
        if (cscdElementIs(current(tb), "optgroup")) tb->stack.len--;
    } else if (cscdSvEqC(tag, "tr")) {
        static const char* same[] = {"tr"};
        static const char* stops[] = {"table", "tbody", "thead", "tfoot"};
        CLOSE_SIBLING(tb, same, stops);
    } else if (cscdSvEqC(tag, "td") || cscdSvEqC(tag, "th")) {
        static const char* same[] = {"td", "th"};
        static const char* stops[] = {"tr", "table"};
        CLOSE_SIBLING(tb, same, stops);
    } else if (cscdSvEqC(tag, "thead") || cscdSvEqC(tag, "tbody") || cscdSvEqC(tag, "tfoot")) {
        static const char* same[] = {"thead", "tbody", "tfoot", "tr", "td", "th"};
        static const char* stops[] = {"table"};
        CLOSE_SIBLING(tb, same, stops);
    } else if (cscdSvEqC(tag, "rt") || cscdSvEqC(tag, "rp")) {
        static const char* same[] = {"rt", "rp"};
        static const char* stops[] = {"ruby"};
        CLOSE_SIBLING(tb, same, stops);
    } else if (tag.len == 2 && tag.ptr[0] == 'h' && tag.ptr[1] >= '1' && tag.ptr[1] <= '6') {
        CscdSv c = cscdStrSv(&current(tb)->tag);
        if (c.len == 2 && c.ptr[0] == 'h' && c.ptr[1] >= '1' && c.ptr[1] <= '6') tb->stack.len--;
    } else if (cscdSvEqC(tag, "a")) {
        if (inStack(tb, "a", kScopeBarrier, CSCD_COUNT(kScopeBarrier))) popUntil(tb, CSCD_SV("a"));
    } else if (cscdSvEqC(tag, "button")) {
        if (inStack(tb, "button", kScopeBarrier, CSCD_COUNT(kScopeBarrier))) popUntil(tb, CSCD_SV("button"));
    }
}

/* How deep the parser nests elements (as browsers do): deeper ones go into the element at this depth,
 * side by side (they are not left open), so that a hostile page can make neither the tree deeper than
 * styling, layout and painting recurse safely nor the open elements more than a scope search walks. */
#define kMaxParserDepth 512

/* Makes the element of a start tag (taking its attributes) and appends it to the current one. */
/* A second <html> or <body>: its attributes the element does not have yet join it. */
static void mergeAttributes(CscdElement* e, const CscdStrPairVec* attrs) {
    if (!attrs->len) return;
    CscdStrMap have = {0};
    for (size_t i = 0; i < e->attributes.len; ++i) cscdStrMapPut(&have, cscdStrSv(&e->attributes.items[i].name), NULL);
    for (size_t i = 0; i < attrs->len; ++i) {
        bool inserted = false;
        cscdStrMapPut(&have, cscdStrSv(&attrs->items[i].name), &inserted);
        if (!inserted) continue;
        CscdStrPair p = {cscdStrCopy(&attrs->items[i].name), cscdStrCopy(&attrs->items[i].value)};
        cscdVecPush(&e->attributes, p);
    }
    cscdStrMapFree(&have);
    e->classesValid_ = false;
    cscdNodeMarkDirty(&e->node);
}

static CscdElement* pushTag(TreeBuilder* tb, CscdSv name, CscdStrPairVec* attrs) {
    CscdElement* e = cscdElementNew(name);
    if (attrs) {
        e->attributes = *attrs;
        memset(attrs, 0, sizeof *attrs);
    }
    if (cscdElementIs(e, "input") && (cscdSvIEqC(cscdElementAttrC(e, "type"), "checkbox") || cscdSvIEqC(cscdElementAttrC(e, "type"), "radio")) &&
        cscdElementHasAttributeC(e, "checked"))
        e->state |= CSCD_STATE_CHECKED;
    if (cscdElementIs(e, "option") && cscdElementHasAttributeC(e, "selected")) e->state |= CSCD_STATE_CHECKED;
    cscdNodeAppendChild(&current(tb)->node, &e->node);
    if (tb->stack.len < kMaxParserDepth) cscdVecPush(&tb->stack, e);
    return e;
}

static CscdElement* startTag(TreeBuilder* tb, Token* t) {
    CscdSv name = cscdStrSv(&t->name);
    if (!tb->fragment && !inTemplate(tb)) {
        if (cscdSvEqC(name, "html")) {
            ensureHtml(tb);
            mergeAttributes(tb->html, &t->attrs);
            return NULL;
        }
        if (cscdSvEqC(name, "head")) {
            ensureHead(tb);
            if (!tb->inBody) setStack2(tb, tb->html, tb->head);
            return NULL;
        }
        if (cscdSvEqC(name, "body")) {
            ensureBody(tb);
            mergeAttributes(tb->body, &t->attrs);
            return NULL;
        }
        if (!tb->inBody && IN(name, kHeadOnly)) {
            ensureHead(tb);
            CscdElement* e = cscdElementNew(name);
            e->attributes = t->attrs;
            memset(&t->attrs, 0, sizeof t->attrs);
            cscdNodeAppendChild(&tb->head->node, &e->node);
            if (!IN(name, kVoid) && !t->selfClosing) {
                setStack2(tb, tb->html, tb->head);
                cscdVecPush(&tb->stack, e);
            }
            return e;
        }
        if (!tb->inBody) ensureBody(tb);
    }
    closeImplied(tb, name);

    /* implied tbody / tr inside tables */
    CscdElement* cur = current(tb);
    if (cscdElementIs(cur, "table") && cscdSvEqC(name, "tr")) {
        pushTag(tb, CSCD_SV("tbody"), NULL);
    } else if ((cscdElementIs(cur, "table") || cscdElementIs(cur, "tbody") || cscdElementIs(cur, "thead") || cscdElementIs(cur, "tfoot")) &&
               (cscdSvEqC(name, "td") || cscdSvEqC(name, "th"))) {
        if (cscdElementIs(cur, "table")) pushTag(tb, CSCD_SV("tbody"), NULL);
        pushTag(tb, CSCD_SV("tr"), NULL);
    }
    CscdElement* e = pushTag(tb, name, &t->attrs);
    if ((IN(name, kVoid) || t->selfClosing) && current(tb) == e) tb->stack.len--;
    return e;
}

static void endTag(TreeBuilder* tb, CscdSv name) {
    if (!tb->fragment) {
        if (cscdSvEqC(name, "head")) {
            if (!tb->inBody && tb->html) setStack1(tb, tb->html); /* (a </head> before anything: nothing is open) */
            return;
        }
        if (cscdSvEqC(name, "body") || cscdSvEqC(name, "html")) return;
        if (!tb->inBody) {
            if (current(tb) && cscdSvEq(cscdStrSv(&current(tb)->tag), name)) tb->stack.len--;
            return;
        }
    }
    if (cscdSvEqC(name, "p") && !inStack(tb, "p", kScopeBarrier, CSCD_COUNT(kScopeBarrier))) {
        if (current(tb) == pushTag(tb, CSCD_SV("p"), NULL)) tb->stack.len--;
        return;
    }
    if (cscdSvEqC(name, "br")) {
        if (current(tb) == pushTag(tb, CSCD_SV("br"), NULL)) tb->stack.len--;
        return;
    }
    /* pop to the matching element if it is open (in scope) */
    for (size_t k = tb->stack.len; k-- > minDepth(tb);) {
        CscdElement* e = (CscdElement*)tb->stack.items[k];
        if (cscdSvEq(cscdStrSv(&e->tag), name)) {
            tb->stack.len = k;
            return;
        }
        if (IN(cscdStrSv(&e->tag), kScopeBarrier) && !cscdSvEqC(name, "table") && !cscdSvEq(cscdStrSv(&e->tag), name)) {
            /* table cells: allow </tr> or </table> to close through td/th */
            if (!(cscdSvEqC(name, "tr") || cscdSvEqC(name, "table") || cscdSvEqC(name, "tbody") || cscdSvEqC(name, "thead") || cscdSvEqC(name, "tfoot"))) return;
        }
    }
}

static void build(TreeBuilder* tb, CscdSv html) {
    Tokenizer tz;
    memset(&tz, 0, sizeof tz);
    tz.s = html;
    for (;;) {
        Token t = tzNext(&tz);
        if (t.kind == TOK_EOF) {
            tokenFree(&t);
            break;
        }
        switch (t.kind) {
        case TOK_CHARS: chars(tb, cscdStrSv(&t.data)); break;
        case TOK_COMMENT: insertNode(tb, &cscdCommentNew(cscdStrSv(&t.data))->node); break;
        case TOK_START: {
            CscdElement* e = startTag(tb, &t);
            if (e && !t.selfClosing) {
                if (cscdElementIs(e, "script") || cscdElementIs(e, "style") || cscdElementIs(e, "xmp") || cscdElementIs(e, "noscript"))
                    tzSetRawText(&tz, cscdStrSv(&e->tag), false);
                else if (cscdElementIs(e, "textarea") || cscdElementIs(e, "title"))
                    tzSetRawText(&tz, cscdStrSv(&e->tag), true);
            }
            break;
        }
        case TOK_END_TAG: endTag(tb, cscdStrSv(&t.name)); break;
        default: break;
        }
        tokenFree(&t);
    }
    if (!tb->fragment) ensureBody(tb);
    cscdStrFree(&tz.rawEnd);
    cscdVecFree(&tb->stack);
}

CscdDocument* cscdParseHTML(CscdSv html) {
    CscdDocument* doc = cscdDocumentNew();
    /* skip a UTF-8 byte order mark */
    if (html.len >= 3 && (unsigned char)html.ptr[0] == 0xEF && (unsigned char)html.ptr[1] == 0xBB && (unsigned char)html.ptr[2] == 0xBF)
        html = cscdSvFrom(html, 3);
    TreeBuilder tb;
    memset(&tb, 0, sizeof tb);
    tb.doc = doc;
    build(&tb, html);
    cscdDomAdoptTemplates(&doc->node);
    return doc;
}

void cscdParseHTMLFragment(CscdSv html, CscdElement* parent) {
    TreeBuilder tb;
    memset(&tb, 0, sizeof tb);
    tb.doc = parent->node.owner;
    cscdVecPush(&tb.stack, parent);
    tb.fragment = true;
    tb.inBody = true;
    build(&tb, html);
    cscdDomAdoptTemplates(&parent->node);
}
