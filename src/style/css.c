/* CSS tokenizer, component values, selectors, declarations, style sheets and media queries. */
#include "cascade/css.h"
#include "cascade/dom.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- tokenizer ---------------------------------------------------------------------------------- */

static bool isNameStart(unsigned char c) { return cscdIsAlpha((char)c) || c == '_' || c >= 0x80; }
static bool isNameChar(unsigned char c) { return isNameStart(c) || cscdIsDigit((char)c) || c == '-'; }

typedef struct Lexer { CscdSv s; size_t i; } Lexer;

static char peek(const Lexer* L, size_t k) { return L->i + k < L->s.len ? L->s.ptr[L->i + k] : '\0'; }
static bool validEscape(const Lexer* L, size_t k) { return peek(L, k) == '\\' && peek(L, k + 1) != '\n' && L->i + k + 1 < L->s.len; }

static bool startsIdent(const Lexer* L, size_t k) {
    char c = peek(L, k);
    if (c == '-') return isNameStart((unsigned char)peek(L, k + 1)) || peek(L, k + 1) == '-' || validEscape(L, k + 1);
    return isNameStart((unsigned char)c) || validEscape(L, k);
}

static bool startsNumber(const Lexer* L) {
    char c = peek(L, 0);
    if (cscdIsDigit(c)) return true;
    if (c == '.') return cscdIsDigit(peek(L, 1));
    if (c == '+' || c == '-') return cscdIsDigit(peek(L, 1)) || (peek(L, 1) == '.' && cscdIsDigit(peek(L, 2)));
    return false;
}

static uint32_t lexEscape(Lexer* L) {
    /* i is past the backslash */
    if (cscdIsXDigit(peek(L, 0))) {
        uint32_t cp = 0;
        for (int n = 0; n < 6 && cscdIsXDigit(peek(L, 0)); ++n) {
            char c = L->s.ptr[L->i++];
            cp = cp * 16 + (uint32_t)(c <= '9' ? c - '0' : cscdToLower(c) - 'a' + 10);
        }
        if (cscdIsSpace(peek(L, 0))) ++L->i;
        if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
        return cp;
    }
    if (L->i >= L->s.len) return 0xFFFD;
    return cscdUtf8Next(L->s, &L->i);
}

static void lexName(Lexer* L, CscdStr* out) {
    while (L->i < L->s.len) {
        unsigned char c = (unsigned char)L->s.ptr[L->i];
        if (isNameChar(c)) cscdStrAppendChar(out, L->s.ptr[L->i++]);
        else if (validEscape(L, 0)) { ++L->i; cscdUtf8Append(out, lexEscape(L)); }
        else break;
    }
}

static void lexNumber(Lexer* L, CscdCssToken* t) {
    size_t start = L->i;
    t->integer = true;
    if (peek(L, 0) == '+' || peek(L, 0) == '-') ++L->i;
    while (cscdIsDigit(peek(L, 0))) ++L->i;
    if (peek(L, 0) == '.' && cscdIsDigit(peek(L, 1))) {
        t->integer = false;
        ++L->i;
        while (cscdIsDigit(peek(L, 0))) ++L->i;
    }
    if ((peek(L, 0) == 'e' || peek(L, 0) == 'E') && (cscdIsDigit(peek(L, 1)) || ((peek(L, 1) == '+' || peek(L, 1) == '-') && cscdIsDigit(peek(L, 2))))) {
        t->integer = false;
        L->i += 2;
        while (cscdIsDigit(peek(L, 0))) ++L->i;
    }
    t->num = cscdParseDouble(cscdSvSub(L->s, start, L->i - start), NULL);
}

static void lexComments(Lexer* L) {
    while (peek(L, 0) == '/' && peek(L, 1) == '*') {
        size_t e = cscdSvFindC(L->s, "*/", L->i + 2);
        L->i = e == CSCD_NPOS ? L->s.len : e + 2;
    }
}

static CscdCssToken lexString(Lexer* L, char q) {
    CscdCssToken t;
    memset(&t, 0, sizeof t);
    t.type = CSCD_TOK_STRING;
    ++L->i;
    while (L->i < L->s.len) {
        char c = L->s.ptr[L->i];
        if (c == q) { ++L->i; break; }
        if (c == '\n') break;
        if (c == '\\') {
            if (peek(L, 1) == '\n') { L->i += 2; continue; }
            ++L->i;
            cscdUtf8Append(&t.text, lexEscape(L));
            continue;
        }
        cscdStrAppendChar(&t.text, c);
        ++L->i;
    }
    return t;
}

static CscdCssToken lexUrl(Lexer* L) {
    CscdCssToken t;
    memset(&t, 0, sizeof t);
    t.type = CSCD_TOK_URL;
    while (L->i < L->s.len && L->s.ptr[L->i] != ')') {
        if (L->s.ptr[L->i] == '\\' && validEscape(L, 0)) { ++L->i; cscdUtf8Append(&t.text, lexEscape(L)); continue; }
        cscdStrAppendChar(&t.text, L->s.ptr[L->i++]);
    }
    if (L->i < L->s.len) ++L->i;
    CscdSv tr = cscdSvTrim(cscdStrSv(&t.text));
    CscdStr trimmed = cscdStrDup(tr);
    cscdStrFree(&t.text);
    t.text = trimmed;
    return t;
}

static void lowerInPlace(CscdStr* s) { for (size_t i = 0; i < s->len; ++i) s->ptr[i] = cscdToLower(s->ptr[i]); }

static CscdCssToken lexNext(Lexer* L) {
    for (;;) {
        lexComments(L);
        CscdCssToken t;
        memset(&t, 0, sizeof t);
        t.type = CSCD_TOK_EOF;
        if (L->i >= L->s.len) return t;
        char c = L->s.ptr[L->i];
        if (cscdIsSpace(c)) {
            while (cscdIsSpace(peek(L, 0))) ++L->i;
            t.type = CSCD_TOK_WHITESPACE;
            return t;
        }
        if (c == '"' || c == '\'') return lexString(L, c);
        if (c == '#') {
            if (isNameChar((unsigned char)peek(L, 1)) || validEscape(L, 1)) {
                ++L->i;
                t.hashId = startsIdent(L, 0);
                t.type = CSCD_TOK_HASH;
                lexName(L, &t.text);
                return t;
            }
        }
        if (startsNumber(L)) {
            lexNumber(L, &t);
            if (startsIdent(L, 0)) { t.type = CSCD_TOK_DIMENSION; lexName(L, &t.text); lowerInPlace(&t.text); }
            else if (peek(L, 0) == '%') { ++L->i; t.type = CSCD_TOK_PERCENTAGE; }
            else t.type = CSCD_TOK_NUMBER;
            return t;
        }
        if (startsIdent(L, 0)) {
            CscdStr n = {0};
            lexName(L, &n);
            if (peek(L, 0) == '(') {
                ++L->i;
                if (cscdSvIEqC(cscdStrSv(&n), "url")) {
                    size_t k = L->i;
                    while (k < L->s.len && cscdIsSpace(L->s.ptr[k])) ++k;
                    if (k < L->s.len && L->s.ptr[k] != '"' && L->s.ptr[k] != '\'') {
                        L->i = k;
                        cscdStrFree(&n);
                        return lexUrl(L);
                    }
                }
                t.type = CSCD_TOK_FUNCTION;
                lowerInPlace(&n);
                t.text = n;
                return t;
            }
            t.type = CSCD_TOK_IDENT;
            t.text = n;
            return t;
        }
        if (c == '@' && startsIdent(L, 1)) {
            ++L->i;
            t.type = CSCD_TOK_AT_KEYWORD;
            lexName(L, &t.text);
            lowerInPlace(&t.text);
            return t;
        }
        if (c == '<' && cscdSvStartsC(cscdSvFrom(L->s, L->i), "<!--")) { L->i += 4; continue; }
        if (c == '-' && cscdSvStartsC(cscdSvFrom(L->s, L->i), "-->")) { L->i += 3; continue; }
        if (c == '\\' && validEscape(L, 0)) {
            t.type = CSCD_TOK_IDENT;
            lexName(L, &t.text);
            return t;
        }
        ++L->i;
        switch (c) {
        case ',': t.type = CSCD_TOK_COMMA; break;
        case ':': t.type = CSCD_TOK_COLON; break;
        case ';': t.type = CSCD_TOK_SEMICOLON; break;
        case '{': t.type = CSCD_TOK_LBRACE; break;
        case '}': t.type = CSCD_TOK_RBRACE; break;
        case '(': t.type = CSCD_TOK_LPAREN; break;
        case ')': t.type = CSCD_TOK_RPAREN; break;
        case '[': t.type = CSCD_TOK_LBRACKET; break;
        case ']': t.type = CSCD_TOK_RBRACKET; break;
        default: t.type = CSCD_TOK_DELIM; t.delim = c; break;
        }
        return t;
    }
}

void cscdCssTokenize(CscdSv src, CscdCssTokenVec* out) {
    Lexer L = {src, 0};
    for (;;) {
        CscdCssToken t = lexNext(&L);
        if (t.type == CSCD_TOK_EOF) break;
        cscdVecPush(out, t);
    }
}

void cscdCssTokenVecFree(CscdCssTokenVec* v) {
    for (size_t i = 0; i < v->len; ++i) cscdStrFree(&v->items[i].text);
    cscdVecFree(v);
}

CscdCssToken cscdCssTokenCopy(const CscdCssToken* t) {
    CscdCssToken c = *t;
    c.text = cscdStrCopy(&t->text);
    return c;
}

/* ---- component values --------------------------------------------------------------------------- */

void cscdCssValueFree(CscdCssValue* v) {
    cscdStrFree(&v->tok.text);
    cscdCssValueVecFree(&v->args);
}

void cscdCssValueVecFree(CscdCssValueVec* v) {
    if (!v->cap) { v->items = NULL; v->len = 0; return; } /* (a view) */
    for (size_t i = 0; i < v->len; ++i) cscdCssValueFree(&v->items[i]);
    cscdVecFree(v);
}

CscdCssValue cscdCssValueCopy(const CscdCssValue* v) {
    CscdCssValue c;
    c.tok = cscdCssTokenCopy(&v->tok);
    memset(&c.args, 0, sizeof c.args);
    for (size_t i = 0; i < v->args.len; ++i) cscdVecPush(&c.args, cscdCssValueCopy(&v->args.items[i]));
    return c;
}

/* How deep functions and brackets nest in a value: deeper ones are left out (the value is then
 * invalid where it is used), so that parsing, copying and evaluating it never recurse further. */
#define kMaxValueNesting 32

static void skipBlock(const CscdCssTokenVec* toks, size_t* i) {
    for (int d = 1; *i < toks->len && d > 0; ++*i) {
        const CscdCssTokenType t = toks->items[*i].type;
        if (t == CSCD_TOK_FUNCTION || t == CSCD_TOK_LPAREN || t == CSCD_TOK_LBRACKET || t == CSCD_TOK_LBRACE) ++d;
        else if (t == CSCD_TOK_RPAREN || t == CSCD_TOK_RBRACKET || t == CSCD_TOK_RBRACE) --d;
    }
}

static void parseBlock(const CscdCssTokenVec* toks, size_t* i, CscdCssValueVec* out, CscdCssTokenType closer, bool keepSpace, int depth) {
    while (*i < toks->len) {
        const CscdCssToken* t = &toks->items[*i];
        if (t->type == closer) { ++*i; return; }
        if (t->type == CSCD_TOK_RPAREN || t->type == CSCD_TOK_RBRACKET || t->type == CSCD_TOK_RBRACE) { ++*i; continue; }
        if (t->type == CSCD_TOK_WHITESPACE && !keepSpace) { ++*i; continue; }
        CscdCssValue v;
        v.tok = cscdCssTokenCopy(t);
        memset(&v.args, 0, sizeof v.args);
        ++*i;
        const bool opens = t->type == CSCD_TOK_FUNCTION || t->type == CSCD_TOK_LPAREN || t->type == CSCD_TOK_LBRACKET || t->type == CSCD_TOK_LBRACE;
        if (opens && depth >= kMaxValueNesting) skipBlock(toks, i);
        else if (t->type == CSCD_TOK_FUNCTION || t->type == CSCD_TOK_LPAREN) parseBlock(toks, i, &v.args, CSCD_TOK_RPAREN, keepSpace, depth + 1);
        else if (t->type == CSCD_TOK_LBRACKET) parseBlock(toks, i, &v.args, CSCD_TOK_RBRACKET, keepSpace, depth + 1);
        else if (t->type == CSCD_TOK_LBRACE) parseBlock(toks, i, &v.args, CSCD_TOK_RBRACE, keepSpace, depth + 1);
        cscdVecPush(out, v);
    }
}

static void serializeNum(double n, CscdStr* out) {
    char buf[64];
    snprintf(buf, sizeof buf, "%g", n);
    cscdStrAppendC(out, buf);
}

static void serializeTo(const CscdCssValue* v, CscdStr* out) {
    const CscdCssToken* t = &v->tok;
    switch (t->type) {
    case CSCD_TOK_IDENT: cscdStrAppend(out, cscdStrSv(&t->text)); break;
    case CSCD_TOK_FUNCTION:
        cscdStrAppend(out, cscdStrSv(&t->text));
        cscdStrAppendChar(out, '(');
        for (size_t k = 0; k < v->args.len; ++k) { if (k) cscdStrAppendChar(out, ' '); serializeTo(&v->args.items[k], out); }
        cscdStrAppendChar(out, ')');
        break;
    case CSCD_TOK_AT_KEYWORD: cscdStrAppendChar(out, '@'); cscdStrAppend(out, cscdStrSv(&t->text)); break;
    case CSCD_TOK_HASH: cscdStrAppendChar(out, '#'); cscdStrAppend(out, cscdStrSv(&t->text)); break;
    case CSCD_TOK_STRING: cscdStrAppendChar(out, '"'); cscdStrAppend(out, cscdStrSv(&t->text)); cscdStrAppendChar(out, '"'); break;
    case CSCD_TOK_URL: cscdStrAppendC(out, "url("); cscdStrAppend(out, cscdStrSv(&t->text)); cscdStrAppendChar(out, ')'); break;
    case CSCD_TOK_NUMBER: serializeNum(t->num, out); break;
    case CSCD_TOK_PERCENTAGE: serializeNum(t->num, out); cscdStrAppendChar(out, '%'); break;
    case CSCD_TOK_DIMENSION: serializeNum(t->num, out); cscdStrAppend(out, cscdStrSv(&t->text)); break;
    case CSCD_TOK_DELIM: cscdStrAppendChar(out, t->delim); break;
    case CSCD_TOK_COMMA: cscdStrAppendChar(out, ','); break;
    case CSCD_TOK_COLON: cscdStrAppendChar(out, ':'); break;
    case CSCD_TOK_SEMICOLON: cscdStrAppendChar(out, ';'); break;
    case CSCD_TOK_WHITESPACE: cscdStrAppendChar(out, ' '); break;
    case CSCD_TOK_LPAREN:
        cscdStrAppendChar(out, '(');
        for (size_t k = 0; k < v->args.len; ++k) { if (k) cscdStrAppendChar(out, ' '); serializeTo(&v->args.items[k], out); }
        cscdStrAppendChar(out, ')');
        break;
    case CSCD_TOK_LBRACKET:
        cscdStrAppendChar(out, '[');
        for (size_t k = 0; k < v->args.len; ++k) { if (k) cscdStrAppendChar(out, ' '); serializeTo(&v->args.items[k], out); }
        cscdStrAppendChar(out, ']');
        break;
    default: break;
    }
}

void cscdCssParseValues(CscdSv src, bool keepSpace, CscdCssValueVec* out) {
    CscdCssTokenVec toks = {0};
    cscdCssTokenize(src, &toks);
    size_t i = 0;
    parseBlock(&toks, &i, out, CSCD_TOK_EOF, keepSpace, 0);
    cscdCssTokenVecFree(&toks);
}

void cscdCssSplitCommas(const CscdCssValueVec* v, CscdCssSlices* out) {
    cscdVecClear(out);
    size_t start = 0;
    for (size_t k = 0; k <= v->len; ++k) {
        if (k == v->len || cscdCssIsComma(&v->items[k])) {
            cscdVecPush(out, cscdCssView(v->items + start, k - start));
            start = k + 1;
        }
    }
}

void cscdCssSerialize(const CscdCssValue* v, size_t n, CscdStr* out) {
    for (size_t k = 0; k < n; ++k) {
        if (k && !cscdCssIsComma(&v[k])) cscdStrAppendChar(out, ' ');
        serializeTo(&v[k], out);
    }
}

CscdStr cscdCssUnquote(CscdSv s) {
    s = cscdSvTrim(s);
    if (s.len >= 2 && (s.ptr[0] == '"' || s.ptr[0] == '\'') && s.ptr[s.len - 1] == s.ptr[0]) return cscdStrDup(cscdSvSub(s, 1, s.len - 2));
    return cscdStrDup(s);
}

/* ---- selectors ------------------------------------------------------------------------------------ */

static void simpleFree(CscdSimpleSelector* p) {
    cscdStrFree(&p->name);
    cscdStrFree(&p->value);
    cscdSelectorVecFree(&p->args);
}

static void selectorFree(CscdSelector* s) {
    for (size_t c = 0; c < s->compounds.len; ++c) {
        CscdCompound* comp = &s->compounds.items[c];
        for (size_t p = 0; p < comp->parts.len; ++p) simpleFree(&comp->parts.items[p]);
        cscdVecFree(&comp->parts);
    }
    cscdVecFree(&s->compounds);
    cscdStrFree(&s->pseudoElement);
    cscdStrFree(&s->text);
}

void cscdSelectorVecFree(CscdSelectorVec* v) {
    for (size_t i = 0; i < v->len; ++i) selectorFree(&v->items[i]);
    cscdVecFree(v);
}

static CscdSelector selectorCopy(const CscdSelector* s) {
    CscdSelector c;
    memset(&c, 0, sizeof c);
    c.specificity = s->specificity;
    c.pseudoElement = cscdStrCopy(&s->pseudoElement);
    c.text = cscdStrCopy(&s->text);
    for (size_t i = 0; i < s->compounds.len; ++i) {
        const CscdCompound* src = &s->compounds.items[i];
        CscdCompound* d = cscdVecPushZero(&c.compounds);
        d->combinator = src->combinator;
        for (size_t p = 0; p < src->parts.len; ++p) {
            const CscdSimpleSelector* sp = &src->parts.items[p];
            CscdSimpleSelector* dp = cscdVecPushZero(&d->parts);
            *dp = *sp;
            dp->name = cscdStrCopy(&sp->name);
            dp->value = cscdStrCopy(&sp->value);
            memset(&dp->args, 0, sizeof dp->args);
            cscdSelectorVecCopy(&dp->args, &sp->args);
        }
    }
    return c;
}

void cscdSelectorVecCopy(CscdSelectorVec* dst, const CscdSelectorVec* src) {
    for (size_t i = 0; i < src->len; ++i) cscdVecPush(dst, selectorCopy(&src->items[i]));
}

/* depth: the :is() / :not() / :has() arguments we are in (a selector nested deeper than
 * kMaxSelectorNesting is invalid: each level copies the tokens it holds) */
typedef struct SelParser { const CscdCssToken* t; size_t n, i; int depth; } SelParser;
#define kMaxSelectorNesting 32

static const CscdCssToken* spPeek(const SelParser* p) { return p->i < p->n ? &p->t[p->i] : NULL; }
static void spSkipWs(SelParser* p) { while (p->i < p->n && p->t[p->i].type == CSCD_TOK_WHITESPACE) ++p->i; }

static bool selector(SelParser* p, CscdSelector* s);
static bool selList(SelParser* p, CscdSelectorVec* out, bool forgiving);

static void addSpec(const CscdCompound* comp, uint32_t* a, uint32_t* b, uint32_t* c) {
    for (size_t i = 0; i < comp->parts.len; ++i) {
        const CscdSimpleSelector* p = &comp->parts.items[i];
        switch (p->kind) {
        case CSCD_SEL_ID: ++*a; break;
        case CSCD_SEL_CLASS: case CSCD_SEL_ATTRIBUTE: ++*b; break;
        case CSCD_SEL_TYPE: ++*c; break;
        case CSCD_SEL_PSEUDO_CLASS:
            if (cscdStrEqC(&p->name, "where")) break;
            if (cscdStrEqC(&p->name, "not") || cscdStrEqC(&p->name, "is") || cscdStrEqC(&p->name, "has") || cscdStrEqC(&p->name, "matches")) {
                uint32_t best = 0;
                for (size_t k = 0; k < p->args.len; ++k) if (p->args.items[k].specificity > best) best = p->args.items[k].specificity;
                *a += best >> 16; *b += (best >> 8) & 0xFF; *c += best & 0xFF;
            } else ++*b;
            break;
        default: break;
        }
    }
}

static void computeSpecificity(CscdSelector* s) {
    uint32_t a = 0, b = 0, c = 0;
    for (size_t i = 0; i < s->compounds.len; ++i) addSpec(&s->compounds.items[i], &a, &b, &c);
    if (s->pseudoElement.len) ++c;
    if (a > 255) a = 255;
    if (b > 255) b = 255;
    if (c > 255) c = 255;
    s->specificity = (a << 16) | (b << 8) | c;
}

static void parseNth(const CscdCssToken* toks, size_t n, CscdSimpleSelector* ss) {
    CscdStr txt = {0};
    for (size_t k = 0; k < n; ++k) {
        const CscdCssToken* t = &toks[k];
        char b[32];
        if (t->type == CSCD_TOK_WHITESPACE) continue;
        if (t->type == CSCD_TOK_IDENT) { CscdStr l = cscdLower(cscdStrSv(&t->text)); cscdStrAppend(&txt, cscdStrSv(&l)); cscdStrFree(&l); }
        else if (t->type == CSCD_TOK_NUMBER) {
            snprintf(b, 32, "%d", cscdIntOfDouble(t->num));
            if (t->num >= 0 && txt.len) cscdStrAppendChar(&txt, '+');
            cscdStrAppendC(&txt, b);
        } else if (t->type == CSCD_TOK_DIMENSION) {
            snprintf(b, 32, "%d", cscdIntOfDouble(t->num));
            if (t->num >= 0 && txt.len) cscdStrAppendChar(&txt, '+');
            cscdStrAppendC(&txt, b);
            CscdStr l = cscdLower(cscdStrSv(&t->text));
            cscdStrAppend(&txt, cscdStrSv(&l));
            cscdStrFree(&l);
        } else if (t->type == CSCD_TOK_DELIM) cscdStrAppendChar(&txt, t->delim);
    }
    CscdSv s = cscdStrSv(&txt);
    if (cscdSvEqC(s, "odd")) { ss->nthA = 2; ss->nthB = 1; }
    else if (cscdSvEqC(s, "even")) { ss->nthA = 2; ss->nthB = 0; }
    else {
        size_t np = cscdSvFindChar(s, 'n', 0);
        if (np == CSCD_NPOS) { ss->nthA = 0; ss->nthB = atoi(cscdStrC(&txt)); }
        else {
            CscdSv as = cscdSvSub(s, 0, np), bs = cscdSvFrom(s, np + 1);
            if (as.len == 0 || cscdSvEqC(as, "+")) ss->nthA = 1;
            else if (cscdSvEqC(as, "-")) ss->nthA = -1;
            else { char* a = cscdSvDupC(as); ss->nthA = atoi(a); cscdFree(a); }
            /* "n-of-type" style suffixes are not expected here; bs is like "+1" or "-2" */
            size_t k = 0;
            while (k < bs.len && (bs.ptr[k] == '+' || bs.ptr[k] == '-') && k + 1 < bs.len && (bs.ptr[k + 1] == '+' || bs.ptr[k + 1] == '-')) ++k;
            if (!bs.len) ss->nthB = 0;
            else { char* b = cscdSvDupC(cscdSvFrom(bs, k)); ss->nthB = atoi(b); cscdFree(b); }
        }
    }
    cscdStrFree(&txt);
}

/* collects the raw tokens of a function argument up to its matching ')' (copies) */
static void funcArgs(SelParser* p, CscdCssTokenVec* args) {
    int depth = 1;
    while (p->i < p->n) {
        const CscdCssToken* t = &p->t[p->i++];
        if (t->type == CSCD_TOK_FUNCTION || t->type == CSCD_TOK_LPAREN) ++depth;
        if (t->type == CSCD_TOK_RPAREN && --depth == 0) break;
        cscdVecPush(args, cscdCssTokenCopy(t));
    }
}

static bool compound(SelParser* p, CscdCompound* c, CscdSelector* sel) {
    bool any = false;
    for (;;) {
        const CscdCssToken* t = spPeek(p);
        if (!t) break;
        CscdSimpleSelector ss;
        memset(&ss, 0, sizeof ss);
        if (t->type == CSCD_TOK_IDENT) {
            if (any) return false;
            ss.kind = CSCD_SEL_TYPE;
            ss.name = cscdLower(cscdStrSv(&t->text));
            ++p->i;
        } else if (t->type == CSCD_TOK_DELIM && t->delim == '*') {
            ss.kind = CSCD_SEL_UNIVERSAL;
            ++p->i;
        } else if (t->type == CSCD_TOK_DELIM && t->delim == '&') {
            ss.kind = CSCD_SEL_UNIVERSAL; /* nesting selector, treated as a no-op */
            ++p->i;
        } else if (t->type == CSCD_TOK_HASH) {
            ss.kind = CSCD_SEL_ID;
            ss.name = cscdStrCopy(&t->text);
            ++p->i;
        } else if (t->type == CSCD_TOK_DELIM && t->delim == '.') {
            ++p->i;
            if (!spPeek(p) || spPeek(p)->type != CSCD_TOK_IDENT) return false;
            ss.kind = CSCD_SEL_CLASS;
            ss.name = cscdStrCopy(&spPeek(p)->text);
            ++p->i;
        } else if (t->type == CSCD_TOK_LBRACKET) {
            ++p->i;
            spSkipWs(p);
            if (!spPeek(p) || spPeek(p)->type != CSCD_TOK_IDENT) return false;
            ss.kind = CSCD_SEL_ATTRIBUTE;
            ss.name = cscdLower(cscdStrSv(&spPeek(p)->text));
            ++p->i;
            spSkipWs(p);
            const CscdCssToken* o = spPeek(p);
            if (o && o->type == CSCD_TOK_DELIM) {
                char d = o->delim;
                ++p->i;
                if (d == '=') ss.op = CSCD_ATTR_EQUALS;
                else {
                    if (!spPeek(p) || spPeek(p)->type != CSCD_TOK_DELIM || spPeek(p)->delim != '=') { simpleFree(&ss); return false; }
                    ++p->i;
                    ss.op = d == '~' ? CSCD_ATTR_INCLUDES : d == '|' ? CSCD_ATTR_DASH_MATCH : d == '^' ? CSCD_ATTR_PREFIX
                          : d == '$' ? CSCD_ATTR_SUFFIX : d == '*' ? CSCD_ATTR_SUBSTRING : CSCD_ATTR_EQUALS;
                }
                spSkipWs(p);
                const CscdCssToken* v = spPeek(p);
                if (!v) { simpleFree(&ss); return false; }
                if (v->type == CSCD_TOK_IDENT || v->type == CSCD_TOK_STRING) ss.value = cscdStrCopy(&v->text);
                else if (v->type == CSCD_TOK_NUMBER) { char b[32]; snprintf(b, 32, "%g", v->num); ss.value = cscdStrDupC(b); }
                else { simpleFree(&ss); return false; }
                ++p->i;
                spSkipWs(p);
                v = spPeek(p);
                if (v && v->type == CSCD_TOK_IDENT && (cscdStrEqC(&v->text, "i") || cscdStrEqC(&v->text, "I"))) { ss.caseInsensitive = true; ++p->i; spSkipWs(p); }
                else if (v && v->type == CSCD_TOK_IDENT && cscdStrEqC(&v->text, "s")) { ++p->i; spSkipWs(p); }
            }
            if (!spPeek(p) || spPeek(p)->type != CSCD_TOK_RBRACKET) { simpleFree(&ss); return false; }
            ++p->i;
        } else if (t->type == CSCD_TOK_COLON) {
            ++p->i;
            bool element = false;
            if (spPeek(p) && spPeek(p)->type == CSCD_TOK_COLON) { element = true; ++p->i; }
            const CscdCssToken* n = spPeek(p);
            if (!n) return false;
            if (n->type == CSCD_TOK_IDENT) {
                CscdStr name = cscdLower(cscdStrSv(&n->text));
                ++p->i;
                if (element || cscdStrEqC(&name, "before") || cscdStrEqC(&name, "after") || cscdStrEqC(&name, "first-line") || cscdStrEqC(&name, "first-letter")) {
                    cscdStrFree(&sel->pseudoElement);
                    sel->pseudoElement = name;
                    any = true;
                    continue;
                }
                ss.kind = CSCD_SEL_PSEUDO_CLASS;
                ss.name = name;
            } else if (n->type == CSCD_TOK_FUNCTION) {
                CscdStr name = cscdStrCopy(&n->text);
                ++p->i;
                CscdCssTokenVec args = {0};
                funcArgs(p, &args);
                if (element) {
                    cscdStrFree(&sel->pseudoElement);
                    sel->pseudoElement = name;
                    cscdCssTokenVecFree(&args);
                    any = true;
                    continue;
                }
                ss.kind = CSCD_SEL_PSEUDO_CLASS;
                ss.name = name;
                CscdSv nm = cscdStrSv(&name);
                if (cscdSvEqC(nm, "not") || cscdSvEqC(nm, "is") || cscdSvEqC(nm, "where") || cscdSvEqC(nm, "matches") || cscdSvEqC(nm, "has") || cscdSvEqC(nm, "any")) {
                    size_t from = 0;
                    if (cscdSvEqC(nm, "has")) { /* relative selectors: strip a leading combinator, matched as descendants */
                        size_t k = 0;
                        while (k < args.len && args.items[k].type == CSCD_TOK_WHITESPACE) ++k;
                        if (k < args.len && args.items[k].type == CSCD_TOK_DELIM && args.items[k].delim == '>') { ss.value = cscdStrDupC(">"); from = k + 1; }
                    }
                    if (p->depth >= kMaxSelectorNesting) {
                        simpleFree(&ss);
                        cscdCssTokenVecFree(&args);
                        return false;
                    }
                    SelParser sub = {args.len ? args.items + from : NULL, args.len - from, 0, p->depth + 1};
                    selList(&sub, &ss.args, !cscdSvEqC(nm, "not"));
                } else if (cscdSvStartsC(nm, "nth-")) {
                    /* "An+B of S" is accepted without the "of S" part */
                    size_t k = 0;
                    while (k < args.len && !(args.items[k].type == CSCD_TOK_IDENT && cscdSvIEqC(cscdStrSv(&args.items[k].text), "of"))) ++k;
                    parseNth(args.items, k, &ss);
                } else {
                    for (size_t k = 0; k < args.len; ++k)
                        if (args.items[k].type == CSCD_TOK_IDENT || args.items[k].type == CSCD_TOK_STRING) cscdStrAppend(&ss.value, cscdStrSv(&args.items[k].text));
                }
                cscdCssTokenVecFree(&args);
            } else return false;
        } else break;
        cscdVecPush(&c->parts, ss);
        any = true;
    }
    return any;
}

static bool selector(SelParser* p, CscdSelector* s) {
    CscdCombinator comb = CSCD_COMB_NONE;
    for (;;) {
        CscdCompound c;
        memset(&c, 0, sizeof c);
        c.combinator = s->compounds.len == 0 ? CSCD_COMB_NONE : comb;
        if (!compound(p, &c, s)) {
            for (size_t i = 0; i < c.parts.len; ++i) simpleFree(&c.parts.items[i]);
            cscdVecFree(&c.parts);
            return false;
        }
        cscdVecPush(&s->compounds, c);
        bool ws = false;
        while (spPeek(p) && spPeek(p)->type == CSCD_TOK_WHITESPACE) { ++p->i; ws = true; }
        const CscdCssToken* t = spPeek(p);
        if (!t || t->type == CSCD_TOK_COMMA || t->type == CSCD_TOK_RPAREN) break;
        if (t->type == CSCD_TOK_DELIM && (t->delim == '>' || t->delim == '+' || t->delim == '~')) {
            comb = t->delim == '>' ? CSCD_COMB_CHILD : t->delim == '+' ? CSCD_COMB_ADJACENT : CSCD_COMB_SIBLING;
            ++p->i;
            spSkipWs(p);
        } else if (ws) {
            comb = CSCD_COMB_DESCENDANT;
        } else {
            return false;
        }
    }
    computeSpecificity(s);
    return s->compounds.len > 0;
}

static bool selList(SelParser* p, CscdSelectorVec* out, bool forgiving) {
    for (;;) {
        spSkipWs(p);
        CscdSelector s;
        memset(&s, 0, sizeof s);
        bool ok = selector(p, &s);
        spSkipWs(p);
        if (ok) cscdVecPush(out, s);
        else {
            selectorFree(&s);
            if (!forgiving) return false;
            while (p->i < p->n && p->t[p->i].type != CSCD_TOK_COMMA) ++p->i; /* skip to next comma */
        }
        if (p->i >= p->n) return out->len > 0 || forgiving;
        if (p->t[p->i].type == CSCD_TOK_COMMA) { ++p->i; continue; }
        if (!forgiving) return false;
        while (p->i < p->n && p->t[p->i].type != CSCD_TOK_COMMA) ++p->i;
        if (p->i >= p->n) return true;
        ++p->i;
    }
}

bool cscdParseSelectorList(CscdSv text, CscdSelectorVec* out) {
    CscdCssTokenVec toks = {0};
    cscdCssTokenize(text, &toks);
    SelParser p = {toks.items, toks.len, 0, 0};
    const size_t before = out->len;
    bool ok = selList(&p, out, false);
    cscdCssTokenVecFree(&toks);
    if (!ok) return false;
    /* each selector's own text: the list split at its top-level commas */
    size_t at = 0, n = before;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i <= text.len && n < out->len; ++i) {
        const char ch = i < text.len ? text.ptr[i] : ',';
        if (quote) { if (ch == quote) quote = 0; continue; }
        if (ch == '"' || ch == '\'') quote = ch;
        else if (ch == '(' || ch == '[') ++depth;
        else if (ch == ')' || ch == ']') --depth;
        else if (ch == ',' && depth <= 0) {
            cscdStrSet(&out->items[n++].text, cscdSvTrim(cscdSvSub(text, at, i - at)));
            at = i + 1;
        }
    }
    return true;
}

static bool nthMatch(int a, int b, int index) { /* index is 1-based */
    if (a == 0) return index == b;
    int n = index - b;
    return n % a == 0 && n / a >= 0;
}

static bool isFormControl(const CscdElement* e) {
    return cscdElementIs(e, "input") || cscdElementIs(e, "button") || cscdElementIs(e, "select") || cscdElementIs(e, "textarea") ||
           cscdElementIs(e, "option") || cscdElementIs(e, "fieldset");
}

static bool matchCompound(const CscdCompound* c, const CscdElement* e);

static const CscdElement* parentEl(const CscdElement* e) { return cscdNodeParentElement(&e->node); }
static const CscdElement* prevEl(const CscdElement* e) { return cscdNodePreviousElementSibling(&e->node); }
static const CscdElement* nextEl(const CscdElement* e) { return cscdNodeNextElementSibling(&e->node); }

/* How a match failed says how much of the search is left. With "a b c", once no ancestor of some b
 * is an a, no b higher up can have one either: without this, each compound of a selector would
 * multiply its cost by the depth of the page. */
typedef enum MatchResult {
    MATCH_YES,
    MATCH_NOT_THIS_ONE,     /* another candidate may match */
    MATCH_NOT_AMONG_SIBLINGS, /* no sibling of this element will: go on with its ancestors */
    MATCH_NOT_AT_ALL        /* nothing further from the subject will */
} MatchResult;

static MatchResult matchFrom(const CscdSelector* s, int idx, const CscdElement* e) {
    const CscdCompound* c = &s->compounds.items[idx];
    if (!matchCompound(c, e)) return MATCH_NOT_THIS_ONE;
    if (idx == 0) return MATCH_YES;
    switch (c->combinator) {
    case CSCD_COMB_CHILD: {
        const CscdElement* p = parentEl(e);
        if (!p) return MATCH_NOT_AT_ALL;
        const MatchResult r = matchFrom(s, idx - 1, p);
        return r == MATCH_YES || r == MATCH_NOT_AT_ALL ? r : MATCH_NOT_AMONG_SIBLINGS;
    }
    case CSCD_COMB_DESCENDANT:
        for (const CscdElement* p = parentEl(e); p; p = parentEl(p)) {
            const MatchResult r = matchFrom(s, idx - 1, p);
            if (r == MATCH_YES || r == MATCH_NOT_AT_ALL) return r;
        }
        return MATCH_NOT_AT_ALL;
    case CSCD_COMB_ADJACENT: {
        const CscdElement* p = prevEl(e);
        return p ? matchFrom(s, idx - 1, p) : MATCH_NOT_AMONG_SIBLINGS;
    }
    case CSCD_COMB_SIBLING:
        for (const CscdElement* p = prevEl(e); p; p = prevEl(p)) {
            const MatchResult r = matchFrom(s, idx - 1, p);
            if (r != MATCH_NOT_THIS_ONE) return r;
        }
        return MATCH_NOT_AMONG_SIBLINGS;
    default: return MATCH_YES;
    }
}

static bool hasDescendantMatching(const CscdElement* e, const CscdSelectorVec* list, bool childOnly) {
    for (size_t i = 0; i < e->node.children.len; ++i) {
        const CscdElement* c = cscdNodeAsElementC(cscdNodeChild(&e->node, i));
        if (!c) continue;
        if (cscdMatchSelectorList(list, c)) return true;
        if (!childOnly && hasDescendantMatching(c, list, false)) return true;
    }
    return false;
}

static int siblingsIndex(const CscdElement* e, bool ofType, bool fromEnd) {
    const CscdNode* parent = e->node.parent;
    const int at = cscdNodeIndexInParent(&e->node);
    if (!parent || at < 0) return 1;
    int idx = 1;
    const int n = (int)parent->children.len, step = fromEnd ? 1 : -1;
    for (int i = at + step; i >= 0 && i < n; i += step) {
        const CscdElement* s = cscdNodeAsElementC(cscdNodeChild(parent, (size_t)i));
        if (s && (!ofType || cscdStrEq(&s->tag, &e->tag))) ++idx;
    }
    return idx;
}

static bool valueEmpty(const CscdElement* e) {
    CscdStr v = cscdElementValue(e);
    bool r = v.len == 0;
    cscdStrFree(&v);
    return r;
}

static bool matchPseudo(const CscdSimpleSelector* p, const CscdElement* e) {
    CscdSv n = cscdStrSv(&p->name);
#define IS(s) cscdSvEqC(n, s)
    if (IS("hover")) return e->state & CSCD_STATE_HOVER;
    if (IS("active")) return e->state & CSCD_STATE_ACTIVE;
    if (IS("focus")) return e->state & CSCD_STATE_FOCUS;
    if (IS("focus-visible")) return e->state & CSCD_STATE_FOCUS_VISIBLE;
    if (IS("focus-within")) {
        const CscdDocument* d = e->node.owner;
        for (const CscdElement* f = d ? d->focused : NULL; f; f = parentEl(f)) if (f == e) return true;
        return false;
    }
    if (IS("checked")) return e->state & CSCD_STATE_CHECKED;
    if (IS("disabled")) return isFormControl(e) && cscdElementHasAttributeC(e, "disabled");
    if (IS("enabled")) return isFormControl(e) && !cscdElementHasAttributeC(e, "disabled");
    if (IS("first-child")) return !prevEl(e) && parentEl(e);
    if (IS("last-child")) return !nextEl(e) && parentEl(e);
    if (IS("only-child")) return !prevEl(e) && !nextEl(e) && parentEl(e);
    if (IS("first-of-type")) return siblingsIndex(e, true, false) == 1;
    if (IS("last-of-type")) return siblingsIndex(e, true, true) == 1;
    if (IS("only-of-type")) return siblingsIndex(e, true, false) == 1 && siblingsIndex(e, true, true) == 1;
    if (IS("nth-child")) return nthMatch(p->nthA, p->nthB, siblingsIndex(e, false, false));
    if (IS("nth-last-child")) return nthMatch(p->nthA, p->nthB, siblingsIndex(e, false, true));
    if (IS("nth-of-type")) return nthMatch(p->nthA, p->nthB, siblingsIndex(e, true, false));
    if (IS("nth-last-of-type")) return nthMatch(p->nthA, p->nthB, siblingsIndex(e, true, true));
    if (IS("root") || IS("scope")) return e->node.parent && e->node.parent->type == CSCD_NODE_DOCUMENT;
    if (IS("empty")) {
        for (size_t i = 0; i < e->node.children.len; ++i) {
            const CscdNode* c = cscdNodeChild(&e->node, i);
            if (cscdNodeIsElement(c)) return false;
            if (cscdNodeIsText(c) && ((const CscdText*)c)->data.len) return false;
        }
        return true;
    }
    if (IS("not")) return !cscdMatchSelectorList(&p->args, e);
    if (IS("is") || IS("where") || IS("matches") || IS("any")) return cscdMatchSelectorList(&p->args, e);
    if (IS("has")) return hasDescendantMatching(e, &p->args, cscdStrEqC(&p->value, ">"));
    if (IS("link") || IS("any-link")) return (cscdElementIs(e, "a") || cscdElementIs(e, "area")) && cscdElementHasAttributeC(e, "href");
    if (IS("visited") || IS("target") || IS("indeterminate")) return false;
    if (IS("placeholder-shown")) return (cscdElementIs(e, "input") || cscdElementIs(e, "textarea")) && cscdElementHasAttributeC(e, "placeholder") && valueEmpty(e);
    if (IS("required")) return cscdElementHasAttributeC(e, "required");
    if (IS("optional")) return isFormControl(e) && !cscdElementHasAttributeC(e, "required");
    if (IS("read-only"))
        return !((cscdElementIs(e, "input") || cscdElementIs(e, "textarea")) && !cscdElementHasAttributeC(e, "readonly")) && !cscdElementHasAttributeC(e, "contenteditable");
    if (IS("read-write"))
        return ((cscdElementIs(e, "input") || cscdElementIs(e, "textarea")) && !cscdElementHasAttributeC(e, "readonly")) || cscdElementHasAttributeC(e, "contenteditable");
    if (IS("defined")) return true;
    if (IS("lang")) {
        for (const CscdElement* a = e; a; a = parentEl(a))
            if (cscdElementHasAttributeC(a, "lang")) {
                CscdStr l = cscdLower(cscdElementAttrC(a, "lang")), w = cscdLower(cscdStrSv(&p->value));
                bool r = cscdSvStarts(cscdStrSv(&l), cscdStrSv(&w));
                cscdStrFree(&l);
                cscdStrFree(&w);
                return r;
            }
        return false;
    }
    if (IS("dir")) return cscdSvIEqC(cscdStrSv(&p->value), "ltr");
#undef IS
    return false;
}

static bool includesWord(CscdSv val, CscdSv want) {
    size_t i = 0;
    while (i < val.len) {
        while (i < val.len && cscdIsSpace(val.ptr[i])) ++i;
        size_t j = i;
        while (j < val.len && !cscdIsSpace(val.ptr[j])) ++j;
        if (j > i && cscdSvEq(cscdSvSub(val, i, j - i), want)) return true;
        i = j;
    }
    return false;
}

static bool matchCompound(const CscdCompound* c, const CscdElement* e) {
    for (size_t i = 0; i < c->parts.len; ++i) {
        const CscdSimpleSelector* p = &c->parts.items[i];
        switch (p->kind) {
        case CSCD_SEL_UNIVERSAL: break;
        case CSCD_SEL_TYPE: if (!cscdStrEq(&e->tag, &p->name)) return false; break;
        case CSCD_SEL_ID: { const CscdStr* v = cscdElementGetAttribute(e, CSCD_SV("id")); if (!v || !cscdStrEq(v, &p->name)) return false; break; }
        case CSCD_SEL_CLASS: if (!cscdElementHasClass(e, cscdStrSv(&p->name))) return false; break;
        case CSCD_SEL_ATTRIBUTE: {
            const CscdStr* v = cscdElementGetAttribute(e, cscdStrSv(&p->name));
            if (!v) return false;
            if (p->op == CSCD_ATTR_EXISTS) break;
            CscdStr lv = {0}, lw = {0};
            CscdSv val = cscdStrSv(v), want = cscdStrSv(&p->value);
            if (p->caseInsensitive) {
                lv = cscdLower(val);
                lw = cscdLower(want);
                val = cscdStrSv(&lv);
                want = cscdStrSv(&lw);
            }
            bool ok = true;
            switch (p->op) {
            case CSCD_ATTR_EXISTS: break;
            case CSCD_ATTR_EQUALS: ok = cscdSvEq(val, want); break;
            case CSCD_ATTR_INCLUDES: ok = includesWord(val, want); break;
            case CSCD_ATTR_DASH_MATCH: ok = cscdSvEq(val, want) || (cscdSvStarts(val, want) && val.len > want.len && val.ptr[want.len] == '-'); break;
            case CSCD_ATTR_PREFIX: ok = want.len && cscdSvStarts(val, want); break;
            case CSCD_ATTR_SUFFIX: ok = want.len && cscdSvEnds(val, want); break;
            case CSCD_ATTR_SUBSTRING: ok = want.len && cscdSvFind(val, want, 0) != CSCD_NPOS; break;
            }
            cscdStrFree(&lv);
            cscdStrFree(&lw);
            if (!ok) return false;
            break;
        }
        case CSCD_SEL_PSEUDO_CLASS: if (!matchPseudo(p, e)) return false; break;
        }
    }
    return true;
}

bool cscdMatchSelector(const CscdSelector* s, const CscdElement* e) {
    if (!s->compounds.len) return false;
    return matchFrom(s, (int)s->compounds.len - 1, e) == MATCH_YES;
}

bool cscdMatchSelectorList(const CscdSelectorVec* list, const CscdElement* e) {
    for (size_t i = 0; i < list->len; ++i)
        if (!list->items[i].pseudoElement.len && cscdMatchSelector(&list->items[i], e)) return true;
    return false;
}

/* ---- declarations --------------------------------------------------------------------------------- */

void cscdDeclVecFree(CscdDeclVec* v) {
    for (size_t i = 0; i < v->len; ++i) {
        cscdStrFree(&v->items[i].property);
        cscdStrFree(&v->items[i].value);
    }
    cscdVecFree(v);
}

void cscdDeclVecCopy(CscdDeclVec* dst, const CscdDeclVec* src) {
    for (size_t i = 0; i < src->len; ++i) {
        CscdDeclaration d = {cscdStrCopy(&src->items[i].property), cscdStrCopy(&src->items[i].value), src->items[i].important};
        cscdVecPush(dst, d);
    }
}

/* Removes the comments from s. */
static void stripComments(CscdStr* s) {
    for (size_t c; (c = cscdSvFindC(cscdStrSv(s), "/*", 0)) != CSCD_NPOS;) {
        size_t e = cscdSvFindC(cscdStrSv(s), "*/", c + 2);
        cscdStrErase(s, c, e == CSCD_NPOS ? CSCD_NPOS : e + 2 - c);
    }
}

void cscdParseDeclarations(CscdSv block, CscdDeclVec* out) {
    size_t i = 0;
    const char* b = block.ptr;
    const size_t n = block.len;
    while (i < n) {
        /* property name */
        while (i < n && (cscdIsSpace(b[i]) || b[i] == ';')) ++i;
        if (i >= n) break;
        if (cscdSvStartsC(cscdSvFrom(block, i), "/*")) {
            size_t e = cscdSvFindC(block, "*/", i + 2);
            i = e == CSCD_NPOS ? n : e + 2;
            continue;
        }
        size_t colon = i;
        while (colon < n && b[colon] != ':' && b[colon] != ';' && b[colon] != '{') ++colon;
        if (colon >= n || b[colon] != ':') {
            /* nested rule or garbage: skip to the end of this statement / block */
            int depth = 0;
            while (i < n) {
                if (b[i] == '{') ++depth;
                else if (b[i] == '}') { if (--depth <= 0) { ++i; break; } }
                else if (b[i] == ';' && depth == 0) { ++i; break; }
                ++i;
            }
            continue;
        }
        CscdSv name = cscdSvTrim(cscdSvSub(block, i, colon - i));
        /* value: up to ';' outside of strings and parentheses */
        size_t j = colon + 1;
        int depth = 0;
        char quote = 0;
        while (j < n) {
            char c = b[j];
            if (quote) { if (c == '\\') ++j; else if (c == quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = c;
            else if (c == '(' || c == '[' || c == '{') ++depth;
            else if (c == ')' || c == ']' || c == '}') --depth;
            else if (c == ';' && depth <= 0) break;
            else if (c == '/' && j + 1 < n && b[j + 1] == '*') {
                size_t e = cscdSvFindC(block, "*/", j + 2);
                j = e == CSCD_NPOS ? n : e + 2;
                continue;
            }
            ++j;
        }
        if (j > n) j = n;
        CscdStr value = cscdStrDup(cscdSvTrim(cscdSvSub(block, colon + 1, j - colon - 1)));
        i = j + 1;
        /* strip comments inside the value */
        stripComments(&value);
        CscdDeclaration d;
        memset(&d, 0, sizeof d);
        bool custom = cscdSvStartsC(name, "--");
        d.property = custom ? cscdStrDup(name) : cscdLower(name);
        size_t bang = cscdSvRFindChar(cscdStrSv(&value), '!');
        if (bang != CSCD_NPOS && cscdSvIEqC(cscdSvTrim(cscdSvFrom(cscdStrSv(&value), bang + 1)), "important")) {
            d.important = true;
            CscdStr v2 = cscdStrDup(cscdSvTrim(cscdSvSub(cscdStrSv(&value), 0, bang)));
            cscdStrFree(&value);
            value = v2;
        }
        if (custom) d.value = value;
        else {
            d.value = cscdStrDup(cscdSvTrim(cscdStrSv(&value)));
            cscdStrFree(&value);
        }
        if (!d.property.len) {
            cscdStrFree(&d.property);
            cscdStrFree(&d.value);
            continue;
        }
        cscdVecPush(out, d);
    }
}

/* ---- media queries ---------------------------------------------------------------------------------- */

static float mediaLength(const CscdCssToken* t, const CscdMediaContext* ctx) {
    if (t->type == CSCD_TOK_NUMBER) return (float)t->num;
    if (t->type != CSCD_TOK_DIMENSION) return 0;
    CscdSv u = cscdStrSv(&t->text);
    float v = (float)t->num;
    if (cscdSvEqC(u, "px")) return v;
    if (cscdSvEqC(u, "em") || cscdSvEqC(u, "rem")) return v * 16;
    if (cscdSvEqC(u, "pt")) return v * 96.f / 72.f;
    if (cscdSvEqC(u, "cm")) return v * 96.f / 2.54f;
    if (cscdSvEqC(u, "mm")) return v * 96.f / 25.4f;
    if (cscdSvEqC(u, "in")) return v * 96.f;
    if (cscdSvEqC(u, "vw")) return v * ctx->width / 100.f;
    if (cscdSvEqC(u, "vh")) return v * ctx->height / 100.f;
    if (cscdSvEqC(u, "dppx") || cscdSvEqC(u, "x")) return v;
    if (cscdSvEqC(u, "dpi")) return v / 96.f;
    return v;
}

/* returns false if unknown; *ident is set (static text) for keyword features */
static bool mediaFeature(CscdSv name, const CscdMediaContext* ctx, float* outVal, const char** ident) {
    *ident = NULL;
    if (cscdSvEqC(name, "width")) { *outVal = ctx->width; return true; }
    if (cscdSvEqC(name, "height")) { *outVal = ctx->height; return true; }
    if (cscdSvEqC(name, "aspect-ratio")) { *outVal = ctx->width / cscdMaxf(1.f, ctx->height); return true; }
    if (cscdSvEqC(name, "resolution")) { *outVal = ctx->dpr; return true; }
    if (cscdSvEqC(name, "orientation")) { *ident = ctx->width >= ctx->height ? (ctx->width == ctx->height ? "portrait" : "landscape") : "portrait"; return true; }
    if (cscdSvEqC(name, "prefers-color-scheme")) { *ident = ctx->dark ? "dark" : "light"; return true; }
    if (cscdSvEqC(name, "prefers-reduced-motion")) { *ident = ctx->reducedMotion ? "reduce" : "no-preference"; return true; }
    if (cscdSvEqC(name, "hover")) { *ident = "hover"; return true; }
    if (cscdSvEqC(name, "pointer") || cscdSvEqC(name, "any-pointer")) { *ident = "fine"; return true; }
    if (cscdSvEqC(name, "color")) { *outVal = 8; return true; }
    if (cscdSvEqC(name, "display-mode")) { *ident = "browser"; return true; }
    return false;
}

static bool cmpOp(float a, const char* op, float b) {
    if (!strcmp(op, "<")) return a < b;
    if (!strcmp(op, "<=")) return a <= b;
    if (!strcmp(op, ">")) return a > b;
    if (!strcmp(op, ">=")) return a >= b;
    return fabsf(a - b) < 0.01f;
}

typedef CSCD_VEC(const CscdCssToken*) TokPtrVec;

static bool rangeValue(const TokPtrVec* t, size_t a, size_t b, const CscdMediaContext* ctx, float* out) { /* tokens [a, b) */
    if (b <= a) return false;
    if (t->items[a]->type == CSCD_TOK_IDENT) {
        const char* id;
        CscdStr l = cscdLower(cscdStrSv(&t->items[a]->text));
        bool ok = mediaFeature(cscdStrSv(&l), ctx, out, &id);
        cscdStrFree(&l);
        return ok;
    }
    *out = mediaLength(t->items[a], ctx);
    if (b - a >= 3 && t->items[a + 1]->type == CSCD_TOK_DELIM && t->items[a + 1]->delim == '/') *out /= cscdMaxf(1e-6f, mediaLength(t->items[a + 2], ctx));
    return true;
}

static bool evalFeature(const CscdCssToken* toks, size_t ntoks, const CscdMediaContext* ctx) {
    TokPtrVec t = {0};
    for (size_t k = 0; k < ntoks; ++k) if (toks[k].type != CSCD_TOK_WHITESPACE) cscdVecPush(&t, &toks[k]);
    bool result = false;
    if (!t.len) { result = true; goto done; }
    /* (name: value) / (name) / range forms */
    if (t.items[0]->type == CSCD_TOK_IDENT && (t.len == 1 || t.items[1]->type == CSCD_TOK_COLON)) {
        CscdStr nameS = cscdLower(cscdStrSv(&t.items[0]->text));
        CscdSv name = cscdStrSv(&nameS);
        bool isMin = cscdSvStartsC(name, "min-"), isMax = cscdSvStartsC(name, "max-");
        if (isMin || isMax) name = cscdSvFrom(name, 4);
        float v = 0;
        const char* id = NULL;
        if (cscdSvStartsC(name, "-webkit-") || !mediaFeature(name, ctx, &v, &id)) result = false;
        else if (t.len == 1) result = !id ? v != 0 : strcmp(id, "none") != 0;
        else if (t.len < 3) result = false;
        else if (id) result = t.items[2]->type == CSCD_TOK_IDENT && cscdSvIEqC(cscdStrSv(&t.items[2]->text), id);
        else {
            float want = mediaLength(t.items[2], ctx);
            if (t.len >= 5 && t.items[3]->type == CSCD_TOK_DELIM && t.items[3]->delim == '/') want = want / cscdMaxf(1e-6f, mediaLength(t.items[4], ctx));
            if (isMin) result = v >= want;
            else if (isMax) result = v <= want;
            else result = fabsf(v - want) < 0.01f;
        }
        cscdStrFree(&nameS);
        goto done;
    }
    {
        /* range syntax: (width >= 600px), (400px <= width <= 700px) */
        char ops[8][3];
        size_t opAt[8], nops = 0;
        for (size_t k = 0; k < t.len && nops < 8; ++k) {
            const CscdCssToken* x = t.items[k];
            if (x->type == CSCD_TOK_DELIM && (x->delim == '<' || x->delim == '>' || x->delim == '=')) {
                ops[nops][0] = x->delim;
                ops[nops][1] = 0;
                ops[nops][2] = 0;
                if (k + 1 < t.len && t.items[k + 1]->type == CSCD_TOK_DELIM && t.items[k + 1]->delim == '=') { ops[nops][1] = '='; ++k; }
                opAt[nops++] = k;
            }
        }
        if (!nops) { result = false; goto done; }
        size_t partA[9], partB[9], nparts = 0, start = 0;
        for (size_t k = 0; k < nops; ++k) {
            size_t opStart = opAt[k] - (strlen(ops[k]) - 1);
            partA[nparts] = start;
            partB[nparts++] = opStart;
            start = opAt[k] + 1;
        }
        partA[nparts] = start;
        partB[nparts++] = t.len;
        float vals[9];
        result = true;
        for (size_t k = 0; k < nparts && result; ++k)
            if (!rangeValue(&t, partA[k], partB[k], ctx, &vals[k])) result = false;
        for (size_t k = 0; k < nops && result; ++k)
            if (!cmpOp(vals[k], ops[k], vals[k + 1])) result = false;
    }
done:
    cscdVecFree(&t);
    return result;
}

/* condition := term (('and' | 'or') term)* ; term := 'not' term | '(' condition-or-feature ')' | media-type */
static bool evalCondition(const CscdCssToken* t, size_t n, size_t* i, const CscdMediaContext* ctx);

static bool evalTerm(const CscdCssToken* t, size_t n, size_t* i, const CscdMediaContext* ctx) {
    while (*i < n && t[*i].type == CSCD_TOK_WHITESPACE) ++*i;
    if (*i >= n) return true;
    if (t[*i].type == CSCD_TOK_IDENT) {
        CscdSv w = cscdStrSv(&t[*i].text);
        ++*i;
        if (cscdSvIEqC(w, "not")) return !evalTerm(t, n, i, ctx);
        if (cscdSvIEqC(w, "only")) return evalTerm(t, n, i, ctx);
        if (cscdSvIEqC(w, "all")) return true;
        if (cscdSvIEqC(w, "screen")) return !ctx->print;
        if (cscdSvIEqC(w, "print")) return ctx->print;
        return false;
    }
    if (t[*i].type == CSCD_TOK_LPAREN) {
        ++*i;
        size_t start = *i;
        int depth = 1;
        size_t end = *i;
        while (end < n) {
            if (t[end].type == CSCD_TOK_LPAREN || t[end].type == CSCD_TOK_FUNCTION) ++depth;
            if (t[end].type == CSCD_TOK_RPAREN && --depth == 0) break;
            ++end;
        }
        const CscdCssToken* inner = t + start;
        size_t ninner = cscdMinz(end, n) - start;
        *i = cscdMinz(end + 1, n);
        /* nested condition if it starts with '(' or 'not' */
        size_t k = 0;
        while (k < ninner && inner[k].type == CSCD_TOK_WHITESPACE) ++k;
        if (k < ninner && (inner[k].type == CSCD_TOK_LPAREN || (inner[k].type == CSCD_TOK_IDENT && cscdSvIEqC(cscdStrSv(&inner[k].text), "not")))) {
            size_t j = 0;
            return evalCondition(inner, ninner, &j, ctx);
        }
        return evalFeature(inner, ninner, ctx);
    }
    ++*i;
    return false;
}

static bool evalCondition(const CscdCssToken* t, size_t n, size_t* i, const CscdMediaContext* ctx) {
    bool v = evalTerm(t, n, i, ctx);
    for (;;) {
        while (*i < n && t[*i].type == CSCD_TOK_WHITESPACE) ++*i;
        if (*i >= n) return v;
        if (t[*i].type == CSCD_TOK_IDENT && cscdSvIEqC(cscdStrSv(&t[*i].text), "and")) { ++*i; bool r = evalTerm(t, n, i, ctx); v = v && r; continue; }
        if (t[*i].type == CSCD_TOK_IDENT && cscdSvIEqC(cscdStrSv(&t[*i].text), "or")) { ++*i; bool r = evalTerm(t, n, i, ctx); v = v || r; continue; }
        return v;
    }
}

static bool evalPart(const CscdCssToken* t, size_t n, const CscdMediaContext* ctx) {
    bool empty = true;
    for (size_t k = 0; k < n; ++k) empty = empty && t[k].type == CSCD_TOK_WHITESPACE;
    size_t i = 0;
    return empty ? true : evalCondition(t, n, &i, ctx);
}

bool cscdEvalMediaQuery(CscdSv query, const CscdMediaContext* ctx) {
    CscdCssTokenVec toks = {0};
    cscdCssTokenize(query, &toks);
    /* comma separated list: any match */
    bool anyPart = false;
    int depth = 0;
    size_t start = 0;
    for (size_t k = 0; k < toks.len; ++k) {
        const CscdCssToken* t = &toks.items[k];
        if (t->type == CSCD_TOK_LPAREN || t->type == CSCD_TOK_FUNCTION) ++depth;
        if (t->type == CSCD_TOK_RPAREN) --depth;
        if (t->type == CSCD_TOK_COMMA && depth == 0) {
            anyPart = evalPart(toks.items + start, k - start, ctx) || anyPart;
            start = k + 1;
        }
    }
    anyPart = evalPart(toks.items + start, toks.len - start, ctx) || anyPart;
    cscdCssTokenVecFree(&toks);
    return anyPart;
}

/* ---- style sheets --------------------------------------------------------------------------------- */

void cscdContainerCondVecFree(CscdContainerCondVec* v) {
    for (size_t i = 0; i < v->len; ++i) { cscdStrFree(&v->items[i].name); cscdStrFree(&v->items[i].query); }
    cscdVecFree(v);
}

static void containerCondCopy(CscdContainerCondVec* dst, const CscdContainerCondVec* src) {
    for (size_t i = 0; i < src->len; ++i) {
        CscdContainerCondition c = {cscdStrCopy(&src->items[i].name), cscdStrCopy(&src->items[i].query)};
        cscdVecPush(dst, c);
    }
}

void cscdFontFaceRuleVecFree(CscdFontFaceRuleVec* v) {
    for (size_t i = 0; i < v->len; ++i) { cscdStrFree(&v->items[i].family); cscdStrFree(&v->items[i].src); }
    cscdVecFree(v);
}

static CscdFontFaceRule fontFaceCopy(const CscdFontFaceRule* f) {
    CscdFontFaceRule c = *f;
    c.family = cscdStrCopy(&f->family);
    c.src = cscdStrCopy(&f->src);
    return c;
}

static void ruleFree(CscdStyleRule* r) {
    cscdSelectorVecFree(&r->selectors);
    cscdDeclVecFree(&r->declarations);
    cscdStrFree(&r->media);
    cscdContainerCondVecFree(&r->containers);
}

static void keyframesFree(CscdKeyframesRule* k) {
    cscdStrFree(&k->name);
    for (size_t i = 0; i < k->frames.len; ++i) cscdDeclVecFree(&k->frames.items[i].declarations);
    cscdVecFree(&k->frames);
}

static CscdKeyframesRule keyframesCopy(const CscdKeyframesRule* k) {
    CscdKeyframesRule c;
    memset(&c, 0, sizeof c);
    c.name = cscdStrCopy(&k->name);
    for (size_t i = 0; i < k->frames.len; ++i) {
        CscdKeyframe* f = cscdVecPushZero(&c.frames);
        f->offset = k->frames.items[i].offset;
        cscdDeclVecCopy(&f->declarations, &k->frames.items[i].declarations);
    }
    return c;
}

static void sheetDtor(void* p) {
    CscdStyleSheet* s = (CscdStyleSheet*)p;
    for (size_t i = 0; i < s->rules.len; ++i) ruleFree(&s->rules.items[i]);
    cscdVecFree(&s->rules);
    cscdFontFaceRuleVecFree(&s->fontFaces);
    for (size_t i = 0; i < s->keyframes.len; ++i) keyframesFree(&s->keyframes.items[i]);
    cscdVecFree(&s->keyframes);
    cscdStrFree(&s->baseDir);
}

CscdStyleSheet* cscdStyleSheetNew(CscdOrigin origin) {
    CscdStyleSheet* s = (CscdStyleSheet*)cscdRcNew(sizeof(CscdStyleSheet), sheetDtor);
    s->origin = origin;
    return s;
}

typedef struct RawRule {
    CscdStr prelude;
    CscdStr block; /* inside the braces */
    bool atRule;
    CscdStr atName;
} RawRule;
typedef CSCD_VEC(RawRule) RawRuleVec;

static void rawRulesFree(RawRuleVec* v) {
    for (size_t i = 0; i < v->len; ++i) {
        cscdStrFree(&v->items[i].prelude);
        cscdStrFree(&v->items[i].block);
        cscdStrFree(&v->items[i].atName);
    }
    cscdVecFree(v);
}

/* "@name rest": atRule, atName and the rest as the prelude */
static void splitAtName(RawRule* r) {
    if (!r->prelude.len || r->prelude.ptr[0] != '@') return;
    r->atRule = true;
    size_t k = 1;
    while (k < r->prelude.len && (cscdIsAlnum(r->prelude.ptr[k]) || r->prelude.ptr[k] == '-')) ++k;
    r->atName = cscdLower(cscdSvSub(cscdStrSv(&r->prelude), 1, k - 1));
    CscdStr rest = cscdStrDup(cscdSvTrim(cscdSvFrom(cscdStrSv(&r->prelude), k)));
    cscdStrFree(&r->prelude);
    r->prelude = rest;
}

/* Splits CSS text into top-level statements, honoring strings, comments and nesting. */
static void splitRules(CscdSv s, RawRuleVec* out) {
    size_t i = 0;
    const char* p = s.ptr;
    const size_t n = s.len;
    while (i < n) {
        while (i < n && cscdIsSpace(p[i])) ++i;
        if (i >= n) break;
        CscdSv rest = cscdSvFrom(s, i);
        if (cscdSvStartsC(rest, "/*")) {
            size_t e = cscdSvFindC(s, "*/", i + 2);
            i = e == CSCD_NPOS ? n : e + 2;
            continue;
        }
        if (cscdSvStartsC(rest, "<!--")) { i += 4; continue; }
        if (cscdSvStartsC(rest, "-->")) { i += 3; continue; }
        size_t start = i;
        char quote = 0;
        int paren = 0;
        while (i < n) {
            char c = p[i];
            if (quote) { if (c == '\\') ++i; else if (c == quote) quote = 0; ++i; continue; }
            if (c == '"' || c == '\'') { quote = c; ++i; continue; }
            if (c == '/' && i + 1 < n && p[i + 1] == '*') {
                size_t e = cscdSvFindC(s, "*/", i + 2);
                i = e == CSCD_NPOS ? n : e + 2;
                continue;
            }
            if (c == '(') ++paren;
            if (c == ')') --paren;
            if ((c == '{' || c == ';') && paren <= 0) break;
            if (c == '}' && paren <= 0) break;
            ++i;
        }
        if (i > n) i = n;
        RawRule r;
        memset(&r, 0, sizeof r);
        r.prelude = cscdStrDup(cscdSvTrim(cscdSvSub(s, start, i - start)));
        /* drop comments in the prelude */
        stripComments(&r.prelude);
        CscdStr tp = cscdStrDup(cscdSvTrim(cscdStrSv(&r.prelude)));
        cscdStrFree(&r.prelude);
        r.prelude = tp;
        splitAtName(&r);
        if (i >= n) {
            if (r.prelude.len && r.atRule) cscdVecPush(out, r);
            else { cscdStrFree(&r.prelude); cscdStrFree(&r.atName); }
            break;
        }
        if (p[i] == ';' || p[i] == '}') {
            ++i;
            if (r.atRule) cscdVecPush(out, r);
            else { cscdStrFree(&r.prelude); cscdStrFree(&r.atName); }
            continue;
        }
        /* block */
        ++i;
        size_t bstart = i;
        int depth = 1;
        quote = 0;
        while (i < n) {
            char c = p[i];
            if (quote) { if (c == '\\') ++i; else if (c == quote) quote = 0; ++i; continue; }
            if (c == '"' || c == '\'') { quote = c; ++i; continue; }
            if (c == '/' && i + 1 < n && p[i + 1] == '*') {
                size_t e = cscdSvFindC(s, "*/", i + 2);
                i = e == CSCD_NPOS ? n : e + 2;
                continue;
            }
            if (c == '{') ++depth;
            if (c == '}' && --depth == 0) break;
            ++i;
        }
        if (i > n) i = n;
        r.block = cscdStrDup(cscdSvSub(s, bstart, i - bstart));
        if (i < n) ++i;
        cscdVecPush(out, r);
    }
}

static CscdStr extractUrl(CscdSv v) {
    CscdCssValueVec vals = {0};
    cscdCssParseValues(v, false, &vals);
    CscdStr out = {0};
    for (size_t k = 0; k < vals.len; ++k) {
        const CscdCssValue* x = &vals.items[k];
        if (x->tok.type == CSCD_TOK_URL) { out = cscdStrCopy(&x->tok.text); break; }
        if (cscdCssIsFunction(x, "url") && x->args.len) { out = cscdStrCopy(&x->args.items[0].tok.text); break; }
        if (x->tok.type == CSCD_TOK_STRING) { out = cscdStrCopy(&x->tok.text); break; }
    }
    cscdCssValueVecFree(&vals);
    return out;
}

/* Splits a block containing declarations and nested rules (CSS nesting) into the two. */
static void splitNested(CscdSv block, CscdStr* decls, RawRuleVec* nested) {
    size_t i = 0;
    const char* p = block.ptr;
    const size_t n = block.len;
    while (i < n) {
        size_t start = i;
        char quote = 0;
        int paren = 0;
        while (i < n) {
            char c = p[i];
            if (quote) { if (c == '\\') ++i; else if (c == quote) quote = 0; ++i; continue; }
            if (c == '"' || c == '\'') { quote = c; ++i; continue; }
            if (c == '(') ++paren;
            if (c == ')') --paren;
            if ((c == ';' || c == '{') && paren <= 0) break;
            ++i;
        }
        if (i >= n || p[i] == ';') {
            cscdStrAppend(decls, cscdSvSub(block, start, cscdMinz(i, n) - start));
            cscdStrAppendChar(decls, ';');
            ++i;
            continue;
        }
        /* nested rule */
        RawRule r;
        memset(&r, 0, sizeof r);
        r.prelude = cscdStrDup(cscdSvTrim(cscdSvSub(block, start, i - start)));
        ++i;
        size_t bstart = i;
        int depth = 1;
        while (i < n) {
            if (p[i] == '{') ++depth;
            if (p[i] == '}' && --depth == 0) break;
            ++i;
        }
        r.block = cscdStrDup(cscdSvSub(block, bstart, i - bstart));
        if (i < n) ++i;
        splitAtName(&r);
        cscdVecPush(nested, r);
    }
}

static CscdStr combineMedia(CscdSv outer, CscdSv inner) {
    if (!outer.len) return cscdStrDup(inner);
    if (!inner.len) return cscdStrDup(outer);
    return cscdStrf("(%.*s) and (%.*s)", CSCD_SVF(outer), CSCD_SVF(inner));
}

static void splitSelectorTop(CscdSv s, CscdSvVec* v) {
    int depth = 0;
    size_t start = 0;
    for (size_t k = 0; k < s.len; ++k) {
        char c = s.ptr[k];
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (c == ',' && depth == 0) {
            cscdVecPush(v, cscdSvTrim(cscdSvSub(s, start, k - start)));
            start = k + 1;
        }
    }
    cscdVecPush(v, cscdSvTrim(cscdSvFrom(s, start)));
}

/* Resolves a nested selector against its parent: "&:hover" -> "a:hover", ".x" -> "a .x". */
static CscdStr nestSelector(CscdSv parent, CscdSv child) {
    CscdSvVec parents = {0}, children = {0};
    splitSelectorTop(parent, &parents);
    splitSelectorTop(child, &children);
    CscdStr res = {0};
    for (size_t pi = 0; pi < parents.len; ++pi) {
        for (size_t ci = 0; ci < children.len; ++ci) {
            CscdSv p = parents.items[pi], c = children.items[ci];
            if (res.len) cscdStrAppendC(&res, ", ");
            if (cscdSvFindChar(c, '&', 0) != CSCD_NPOS) {
                for (size_t k = 0; k < c.len; ++k) {
                    if (c.ptr[k] == '&') { cscdStrAppendC(&res, ":is("); cscdStrAppend(&res, p); cscdStrAppendChar(&res, ')'); }
                    else cscdStrAppendChar(&res, c.ptr[k]);
                }
            } else {
                cscdStrAppend(&res, p);
                cscdStrAppendChar(&res, ' ');
                cscdStrAppend(&res, c);
            }
        }
    }
    cscdVecFree(&parents);
    cscdVecFree(&children);
    return res;
}

/* How deep style rules and at-rules nest: deeper ones are dropped (each level copies what it holds,
 * and a nested selector grows with every level). */
#define kMaxCssNesting 64

typedef struct SheetBuilder {
    CscdStyleSheet* sheet;
    uint32_t* order;
    int depth;
    int nesting; /* the rule blocks we are in */
    CscdContainerCondVec containers; /* the @container blocks we are in */
} SheetBuilder;

static void buildRules(SheetBuilder* b, const RawRuleVec* rs, CscdSv media, CscdSv parentSel);

static void buildStyle(SheetBuilder* b, CscdSv selText, CscdSv block, CscdSv media) {
    CscdStr decls = {0};
    RawRuleVec nested = {0};
    if (cscdSvFindChar(block, '{', 0) != CSCD_NPOS) splitNested(block, &decls, &nested);
    else cscdStrSet(&decls, block);
    CscdStyleRule rule;
    memset(&rule, 0, sizeof rule);
    if (cscdParseSelectorList(selText, &rule.selectors)) {
        cscdParseDeclarations(cscdStrSv(&decls), &rule.declarations);
        rule.media = cscdStrDup(media);
        containerCondCopy(&rule.containers, &b->containers);
        rule.order = (*b->order)++;
        if (rule.declarations.len) cscdVecPush(&b->sheet->rules, rule);
        else ruleFree(&rule);
    } else ruleFree(&rule);
    if (nested.len) buildRules(b, &nested, media, selText);
    rawRulesFree(&nested);
    cscdStrFree(&decls);
}

static void splitRulesOrNested(CscdSv block, bool nestedContext, RawRuleVec* out) {
    if (!nestedContext) {
        splitRules(block, out);
        return;
    }
    /* inside a style rule, @media holds declarations for the parent selector */
    CscdStr decls = {0};
    RawRuleVec nested = {0};
    splitNested(block, &decls, &nested);
    CscdSv td = cscdSvTrim(cscdStrSv(&decls));
    if (td.len && !cscdSvEqC(td, ";")) {
        RawRule self;
        memset(&self, 0, sizeof self);
        self.prelude = cscdStrDupC("&");
        self.block = cscdStrCopy(&decls);
        cscdVecPush(out, self);
    }
    for (size_t i = 0; i < nested.len; ++i) cscdVecPush(out, nested.items[i]);
    cscdVecFree(&nested);
    cscdStrFree(&decls);
}

static int fontWeightWord(CscdSv s) {
    if (cscdSvEqC(s, "bold")) return 700;
    if (cscdSvEqC(s, "normal")) return 400;
    char* c = cscdSvDupC(s);
    int w = atoi(c);
    cscdFree(c);
    return w;
}

static int compareFrames(const void* a, const void* b, void* ctx) {
    (void)ctx;
    float x = ((const CscdKeyframe*)a)->offset, y = ((const CscdKeyframe*)b)->offset;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void buildAt(SheetBuilder* b, const RawRule* r, CscdSv media, CscdSv parentSel) {
    CscdSv name = cscdStrSv(&r->atName);
    CscdSv prelude = cscdStrSv(&r->prelude);
    if (cscdSvEqC(name, "media")) {
        RawRuleVec inner = {0};
        splitRulesOrNested(cscdStrSv(&r->block), parentSel.len > 0, &inner);
        CscdStr m = combineMedia(media, prelude);
        buildRules(b, &inner, cscdStrSv(&m), parentSel);
        cscdStrFree(&m);
        rawRulesFree(&inner);
    } else if (cscdSvEqC(name, "container")) {
        /* @container [name] <condition> */
        CscdContainerCondition cc;
        memset(&cc, 0, sizeof cc);
        CscdSv pre = cscdSvTrim(prelude);
        size_t sp = 0;
        while (sp < pre.len && (cscdIsAlnum(pre.ptr[sp]) || pre.ptr[sp] == '-' || pre.ptr[sp] == '_')) ++sp;
        CscdStr first = cscdLower(cscdSvSub(pre, 0, sp));
        if (sp > 0 && !cscdStrEqC(&first, "not") && !cscdStrEqC(&first, "style")) {
            cc.name = cscdStrDup(cscdSvSub(pre, 0, sp));
            cc.query = cscdStrDup(cscdSvTrim(cscdSvFrom(pre, sp)));
        } else cc.query = cscdStrDup(pre);
        cscdStrFree(&first);
        cscdVecPush(&b->containers, cc);
        if (r->block.len) {
            RawRuleVec inner = {0};
            splitRulesOrNested(cscdStrSv(&r->block), parentSel.len > 0, &inner);
            buildRules(b, &inner, media, parentSel);
            rawRulesFree(&inner);
        }
        CscdContainerCondition last = cscdVecPop(&b->containers);
        cscdStrFree(&last.name);
        cscdStrFree(&last.query);
    } else if (cscdSvEqC(name, "supports") || cscdSvEqC(name, "layer") || cscdSvEqC(name, "document") || cscdSvEqC(name, "scope")) {
        if (cscdSvEqC(name, "supports") && cscdSvStartsC(prelude, "not ")) return;
        if (r->block.len) {
            RawRuleVec inner = {0};
            splitRulesOrNested(cscdStrSv(&r->block), parentSel.len > 0, &inner);
            buildRules(b, &inner, media, parentSel);
            rawRulesFree(&inner);
        }
    } else if (cscdSvEqC(name, "import")) {
        if (b->depth > 8) return;
        CscdStr url = extractUrl(prelude);
        CscdStr path = cscdJoinPath(cscdStrSv(&b->sheet->baseDir), cscdStrSv(&url));
        CscdStr text = {0};
        if (url.len && cscdReadFile(cscdStrSv(&path), &text)) {
            /* media list after the url */
            CscdStr mq = {0};
            size_t close = cscdSvFindChar(prelude, ')', 0);
            if (cscdSvFindC(prelude, "url(", 0) != CSCD_NPOS && close != CSCD_NPOS) mq = cscdStrDup(cscdSvTrim(cscdSvFrom(prelude, close + 1)));
            else {
                size_t q = cscdSvRFindAny(prelude, "\"'");
                if (q != CSCD_NPOS) mq = cscdStrDup(cscdSvTrim(cscdSvFrom(prelude, q + 1)));
            }
            CscdStyleSheet* sub = cscdStyleSheetNew(b->sheet->origin);
            CscdStr dir = cscdDirName(cscdStrSv(&path));
            cscdStyleSheetParse(sub, cscdStrSv(&text), cscdStrSv(&dir), b->depth + 1);
            cscdStrFree(&dir);
            for (size_t i = 0; i < sub->rules.len; ++i) {
                CscdStyleRule rule = sub->rules.items[i];
                CscdStr inner = combineMedia(cscdStrSv(&mq), cscdStrSv(&rule.media));
                CscdStr all = combineMedia(media, cscdStrSv(&inner));
                cscdStrFree(&inner);
                cscdStrFree(&rule.media);
                rule.media = all;
                CscdContainerCondVec cs = {0};
                containerCondCopy(&cs, &b->containers);
                for (size_t k = 0; k < rule.containers.len; ++k) cscdVecPush(&cs, rule.containers.items[k]);
                cscdVecFree(&rule.containers);
                rule.containers = cs;
                rule.order = (*b->order)++;
                cscdVecPush(&b->sheet->rules, rule);
            }
            sub->rules.len = 0; /* (moved) */
            for (size_t i = 0; i < sub->fontFaces.len; ++i) cscdVecPush(&b->sheet->fontFaces, fontFaceCopy(&sub->fontFaces.items[i]));
            for (size_t i = 0; i < sub->keyframes.len; ++i) cscdVecPush(&b->sheet->keyframes, keyframesCopy(&sub->keyframes.items[i]));
            cscdRelease(sub);
            cscdStrFree(&mq);
        }
        cscdStrFree(&text);
        cscdStrFree(&path);
        cscdStrFree(&url);
    } else if (cscdSvEqC(name, "font-face")) {
        CscdFontFaceRule f;
        memset(&f, 0, sizeof f);
        f.weight = f.weightMax = 400;
        CscdDeclVec decls = {0};
        cscdParseDeclarations(cscdStrSv(&r->block), &decls);
        for (size_t di = 0; di < decls.len; ++di) {
            const CscdDeclaration* d = &decls.items[di];
            if (cscdStrEqC(&d->property, "font-family")) { cscdStrFree(&f.family); f.family = cscdCssUnquote(cscdStrSv(&d->value)); }
            else if (cscdStrEqC(&d->property, "src")) {
                /* first url() whose format we can read */
                CscdCssValueVec vals = {0};
                cscdCssParseValues(cscdStrSv(&d->value), false, &vals);
                CscdCssSlices parts = {0};
                cscdCssSplitCommas(&vals, &parts);
                for (size_t pi = 0; pi < parts.len; ++pi) {
                    CscdSv url = {"", 0};
                    for (size_t k = 0; k < parts.items[pi].len; ++k) {
                        const CscdCssValue* v = &parts.items[pi].items[k];
                        if (v->tok.type == CSCD_TOK_URL) url = cscdStrSv(&v->tok.text);
                        else if (cscdCssIsFunction(v, "url") && v->args.len) url = cscdStrSv(&v->args.items[0].tok.text);
                    }
                    CscdStr lu = cscdLower(url);
                    CscdSv l = cscdStrSv(&lu);
                    bool ok = url.len && (cscdSvEndsC(l, ".ttf") || cscdSvEndsC(l, ".otf") || cscdSvEndsC(l, ".ttc") || cscdSvEndsC(l, ".woff"));
                    cscdStrFree(&lu);
                    if (ok) {
                        cscdStrFree(&f.src);
                        f.src = cscdJoinPath(cscdStrSv(&b->sheet->baseDir), url);
                        break;
                    }
                }
                cscdVecFree(&parts);
                cscdCssValueVecFree(&vals);
            } else if (cscdStrEqC(&d->property, "font-weight")) {
                CscdSvVec ws = {0};
                cscdSplitSpaces(cscdStrSv(&d->value), &ws);
                if (ws.len) {
                    f.weight = fontWeightWord(ws.items[0]);
                    f.weightMax = ws.len > 1 ? fontWeightWord(ws.items[1]) : f.weight;
                }
                cscdVecFree(&ws);
            } else if (cscdStrEqC(&d->property, "font-style"))
                f.italic = cscdSvContains(cscdStrSv(&d->value), "italic") || cscdSvContains(cscdStrSv(&d->value), "oblique");
        }
        cscdDeclVecFree(&decls);
        if (f.family.len && f.src.len) cscdVecPush(&b->sheet->fontFaces, f);
        else { cscdStrFree(&f.family); cscdStrFree(&f.src); }
    } else if (cscdSvEqC(name, "keyframes") || cscdSvEqC(name, "-webkit-keyframes")) {
        CscdKeyframesRule k;
        memset(&k, 0, sizeof k);
        k.name = cscdCssUnquote(prelude);
        RawRuleVec frames = {0};
        splitRules(cscdStrSv(&r->block), &frames);
        for (size_t fi = 0; fi < frames.len; ++fi) {
            const RawRule* fr = &frames.items[fi];
            CscdFloatVec offsets = {0};
            CscdCssValueVec vals = {0};
            cscdCssParseValues(cscdStrSv(&fr->prelude), false, &vals);
            CscdCssSlices parts = {0};
            cscdCssSplitCommas(&vals, &parts);
            for (size_t pi = 0; pi < parts.len; ++pi) {
                const CscdCssValueVec* part = &parts.items[pi];
                if (!part->len) continue;
                if (cscdCssIsIdent(&part->items[0], "from")) cscdVecPush(&offsets, 0.f);
                else if (cscdCssIsIdent(&part->items[0], "to")) cscdVecPush(&offsets, 1.f);
                else if (part->items[0].tok.type == CSCD_TOK_PERCENTAGE) cscdVecPush(&offsets, (float)part->items[0].tok.num / 100.f);
            }
            cscdVecFree(&parts);
            cscdCssValueVecFree(&vals);
            CscdDeclVec decls = {0};
            cscdParseDeclarations(cscdStrSv(&fr->block), &decls);
            for (size_t oi = 0; oi < offsets.len; ++oi) {
                CscdKeyframe* kf = cscdVecPushZero(&k.frames);
                kf->offset = offsets.items[oi];
                cscdDeclVecCopy(&kf->declarations, &decls);
            }
            cscdDeclVecFree(&decls);
            cscdVecFree(&offsets);
        }
        rawRulesFree(&frames);
        cscdSort(k.frames.items, k.frames.len, sizeof(CscdKeyframe), compareFrames, NULL);
        cscdVecPush(&b->sheet->keyframes, k);
    }
    /* @charset, @namespace, @page and unknown at-rules are ignored */
}

static void buildRules(SheetBuilder* b, const RawRuleVec* rs, CscdSv media, CscdSv parentSel) {
    if (b->nesting >= kMaxCssNesting) return;
    ++b->nesting;
    for (size_t i = 0; i < rs->len; ++i) {
        const RawRule* r = &rs->items[i];
        if (r->atRule) { buildAt(b, r, media, parentSel); continue; }
        if (!parentSel.len) buildStyle(b, cscdStrSv(&r->prelude), cscdStrSv(&r->block), media);
        else {
            CscdStr sel = nestSelector(parentSel, cscdStrSv(&r->prelude));
            buildStyle(b, cscdStrSv(&sel), cscdStrSv(&r->block), media);
            cscdStrFree(&sel);
        }
    }
    --b->nesting;
}

void cscdStyleSheetParse(CscdStyleSheet* sheet, CscdSv css, CscdSv baseDir, int depth) {
    cscdStrSet(&sheet->baseDir, baseDir);
    uint32_t order = (uint32_t)sheet->rules.len;
    SheetBuilder b;
    memset(&b, 0, sizeof b);
    b.sheet = sheet;
    b.order = &order;
    b.depth = depth;
    RawRuleVec rules = {0};
    splitRules(css, &rules);
    buildRules(&b, &rules, CSCD_SV(""), CSCD_SV(""));
    rawRulesFree(&rules);
    cscdContainerCondVecFree(&b.containers);
}
