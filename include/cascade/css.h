/* cascade — CSS syntax (tokenizer, component values), style sheets, selectors and media queries,
 * following CSS Syntax Level 3 and Selectors Level 4. */
#ifndef CASCADE_CSS_H
#define CASCADE_CSS_H

#include "cascade/core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CscdElement CscdElement;

typedef enum CscdCssTokenType {
    CSCD_TOK_IDENT, CSCD_TOK_FUNCTION, CSCD_TOK_AT_KEYWORD, CSCD_TOK_HASH, CSCD_TOK_STRING, CSCD_TOK_URL, CSCD_TOK_NUMBER,
    CSCD_TOK_PERCENTAGE, CSCD_TOK_DIMENSION, CSCD_TOK_DELIM, CSCD_TOK_COMMA, CSCD_TOK_COLON, CSCD_TOK_SEMICOLON,
    CSCD_TOK_WHITESPACE, CSCD_TOK_LBRACE, CSCD_TOK_RBRACE, CSCD_TOK_LPAREN, CSCD_TOK_RPAREN, CSCD_TOK_LBRACKET,
    CSCD_TOK_RBRACKET, CSCD_TOK_EOF
} CscdCssTokenType;

typedef struct CscdCssToken {
    CscdCssTokenType type;
    CscdStr text;    /* identifier, function name, string, url, unit, hash */
    double num;
    char delim;
    bool integer;
    bool hashId;   /* a hash that is a valid identifier (#id vs #fff) */
} CscdCssToken;
typedef CSCD_VEC(CscdCssToken) CscdCssTokenVec;

void cscdCssTokenize(CscdSv src, CscdCssTokenVec* out); /* appended */
void cscdCssTokenVecFree(CscdCssTokenVec* v);
CscdCssToken cscdCssTokenCopy(const CscdCssToken* t);

/* A component value: a token, or a function / simple block with its contents. A CscdCssValueVec
 * with cap 0 and items set is a view of another's values (cscdCssSplitCommas makes those): it is
 * never freed. */
typedef struct CscdCssValue CscdCssValue;
typedef struct CscdCssValueVec { CscdCssValue* items; size_t len, cap; } CscdCssValueVec;
struct CscdCssValue {
    CscdCssToken tok;
    CscdCssValueVec args; /* for Function, LParen, LBracket and LBrace blocks */
};
typedef CSCD_VEC(CscdCssValueVec) CscdCssSlices;

CSCD_INLINE bool cscdCssIsIdent(const CscdCssValue* v, const char* s) { return v->tok.type == CSCD_TOK_IDENT && cscdSvIEqC(cscdStrSv(&v->tok.text), s); }
CSCD_INLINE bool cscdCssIsFunction(const CscdCssValue* v, const char* s) { return v->tok.type == CSCD_TOK_FUNCTION && cscdSvIEqC(cscdStrSv(&v->tok.text), s); }
CSCD_INLINE bool cscdCssIsComma(const CscdCssValue* v) { return v->tok.type == CSCD_TOK_COMMA; }
CSCD_INLINE bool cscdCssIsDelim(const CscdCssValue* v, char c) { return v->tok.type == CSCD_TOK_DELIM && v->tok.delim == c; }
CSCD_INLINE CscdCssValueVec cscdCssView(const CscdCssValue* items, size_t n) { CscdCssValueVec v = {(CscdCssValue*)items, n, 0}; return v; }

/* Parses a value into component values (appended); whitespace is dropped unless keepSpace. */
void cscdCssParseValues(CscdSv src, bool keepSpace, CscdCssValueVec* out);
void cscdCssValueVecFree(CscdCssValueVec* v);
void cscdCssValueFree(CscdCssValue* v);
CscdCssValue cscdCssValueCopy(const CscdCssValue* v);
/* The comma-separated parts of v, as views of it (out is replaced; free it with cscdVecFree). */
void cscdCssSplitCommas(const CscdCssValueVec* v, CscdCssSlices* out);
void cscdCssSerialize(const CscdCssValue* v, size_t n, CscdStr* out); /* appended */
CscdStr cscdCssUnquote(CscdSv s);

/* ---- selectors ---------------------------------------------------------------------------------- */

typedef struct CscdSelector CscdSelector;
typedef struct CscdSelectorVec { CscdSelector* items; size_t len, cap; } CscdSelectorVec;

typedef enum CscdSimpleKind { CSCD_SEL_UNIVERSAL, CSCD_SEL_TYPE, CSCD_SEL_ID, CSCD_SEL_CLASS, CSCD_SEL_ATTRIBUTE, CSCD_SEL_PSEUDO_CLASS } CscdSimpleKind;
typedef enum CscdAttrOp { CSCD_ATTR_EXISTS, CSCD_ATTR_EQUALS, CSCD_ATTR_INCLUDES, CSCD_ATTR_DASH_MATCH, CSCD_ATTR_PREFIX, CSCD_ATTR_SUFFIX, CSCD_ATTR_SUBSTRING } CscdAttrOp;

typedef struct CscdSimpleSelector {
    CscdSimpleKind kind;
    CscdAttrOp op;
    CscdStr name;  /* tag, id, class, attribute name or pseudo-class */
    CscdStr value; /* attribute value */
    bool caseInsensitive;
    int nthA, nthB; /* :nth-child(An+B) */
    CscdSelectorVec args; /* :not(), :is(), :where(), :has() */
} CscdSimpleSelector;

typedef enum CscdCombinator { CSCD_COMB_NONE, CSCD_COMB_DESCENDANT, CSCD_COMB_CHILD, CSCD_COMB_ADJACENT, CSCD_COMB_SIBLING } CscdCombinator;

typedef struct CscdCompound {
    CscdCombinator combinator; /* relation to the compound on its left */
    CSCD_VEC(CscdSimpleSelector) parts;
} CscdCompound;

struct CscdSelector {
    CSCD_VEC(CscdCompound) compounds; /* left to right */
    CscdStr pseudoElement;          /* "before", "after", "placeholder", "marker"... */
    uint32_t specificity;         /* (a << 16) | (b << 8) | c */
    CscdStr text;                 /* as written (trimmed), for tools such as the inspector */
};

bool cscdParseSelectorList(CscdSv text, CscdSelectorVec* out); /* appended */
bool cscdMatchSelector(const CscdSelector* s, const CscdElement* e);
bool cscdMatchSelectorList(const CscdSelectorVec* list, const CscdElement* e);
void cscdSelectorVecFree(CscdSelectorVec* v);
void cscdSelectorVecCopy(CscdSelectorVec* dst, const CscdSelectorVec* src); /* deep, appended */

/* ---- style sheets ------------------------------------------------------------------------------- */

typedef struct CscdDeclaration {
    CscdStr property; /* lowercase, custom properties keep their case */
    CscdStr value;
    bool important;
} CscdDeclaration;
typedef CSCD_VEC(CscdDeclaration) CscdDeclVec;

void cscdParseDeclarations(CscdSv block, CscdDeclVec* out); /* appended */
void cscdDeclVecFree(CscdDeclVec* v);
void cscdDeclVecCopy(CscdDeclVec* dst, const CscdDeclVec* src); /* deep, appended */

typedef struct CscdMediaContext {
    float width, height; /* viewport, CSS pixels */
    float dpr;
    bool dark;
    bool print;
    bool reducedMotion;
    float textScale; /* the system's text size (1: as is): font-size keywords and the root's default follow it */
} CscdMediaContext;

CSCD_INLINE CscdMediaContext cscdMediaContextDefault(void) { CscdMediaContext m = {1024, 768, 1, false, false, false, 1}; return m; }
bool cscdEvalMediaQuery(CscdSv query, const CscdMediaContext* ctx);

/* An enclosing @container: the nearest container (of that name, when there is one) must match. */
typedef struct CscdContainerCondition {
    CscdStr name;
    CscdStr query; /* a media-query-like condition on the container's size */
} CscdContainerCondition;
typedef CSCD_VEC(CscdContainerCondition) CscdContainerCondVec;
void cscdContainerCondVecFree(CscdContainerCondVec* v);

typedef struct CscdStyleRule {
    CscdSelectorVec selectors;
    CscdDeclVec declarations;
    CscdStr media;                   /* enclosing @media conditions joined with " and ", empty = always */
    CscdContainerCondVec containers; /* enclosing @container conditions, all must hold */
    uint32_t order;                /* source order inside the cascade */
} CscdStyleRule;

typedef struct CscdFontFaceRule {
    CscdStr family;
    CscdStr src; /* resolved file path */
    int weight, weightMax;
    bool italic;
} CscdFontFaceRule;
typedef CSCD_VEC(CscdFontFaceRule) CscdFontFaceRuleVec;
void cscdFontFaceRuleVecFree(CscdFontFaceRuleVec* v);

typedef struct CscdKeyframe {
    float offset; /* 0..1 */
    CscdDeclVec declarations;
} CscdKeyframe;

typedef struct CscdKeyframesRule {
    CscdStr name;
    CSCD_VEC(CscdKeyframe) frames;
} CscdKeyframesRule;

typedef enum CscdOrigin { CSCD_ORIGIN_USER_AGENT = 0, CSCD_ORIGIN_AUTHOR = 1 } CscdOrigin;

/* Shared (cscdRetain / cscdRelease). */
typedef struct CscdStyleSheet {
    CSCD_VEC(CscdStyleRule) rules;
    CscdFontFaceRuleVec fontFaces;
    CSCD_VEC(CscdKeyframesRule) keyframes;
    CscdOrigin origin;
    CscdStr baseDir;
} CscdStyleSheet;

CscdStyleSheet* cscdStyleSheetNew(CscdOrigin origin);
/* Parses CSS text. @import loads files relative to baseDir. */
void cscdStyleSheetParse(CscdStyleSheet* sheet, CscdSv css, CscdSv baseDir, int depth);

#ifdef __cplusplus
}
#endif

#endif
