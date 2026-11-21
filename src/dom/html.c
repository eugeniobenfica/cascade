/* The HTML parser, as the WHATWG specification has it ("Parsing HTML documents"): its tokenizer, state by state, and
 * its tree construction, insertion mode by insertion mode (the stack of open elements, the list of active formatting
 * elements with the adoption agency and the Noah's Ark clause, foster parenting, templates, framesets, select, foreign
 * content in SVG and MathML, fragments in their context). Scripting is off (cascade runs none): <noscript> is markup.
 * What cascade's tree does not keep is left out: the doctype (only whether the document is in quirks mode, which the
 * tree construction asks), namespaces as names (an element's is a field of it). */
#include "cascade/dom.h"
#include "core/thread.h"

#include <string.h>

/* ---- character references ---------------------------------------------------------------------------------------- */

typedef struct NamedRef { const char* name; const char* text; } NamedRef;
static const NamedRef kNamedRefs[] = {
#include "dom/htmlEntities.inc"
};

static CscdStrMap refMap;
static CscdOnce refOnce = CSCD_ONCE_INIT;
static void initRefs(void) {
    for (size_t i = 0; i < CSCD_COUNT(kNamedRefs); ++i) cscdStrMapPut(&refMap, cscdSv(kNamedRefs[i].name), NULL)->p = (void*)kNamedRefs[i].text;
}

/* the longest name at s (up to 32 bytes, the longest a name is), its length; 0 for none */
static size_t matchRef(CscdSv s, const char** text) {
    cscdCallOnce(&refOnce, initRefs);
    size_t n = 0;
    while (n < s.len && n < 32 && (cscdIsAlnum(s.ptr[n]) || (n && s.ptr[n] == ';' && (++n, false)))) ++n;
    for (size_t k = cscdMinz(n, s.len); k > 0; --k) {
        CscdMapVal* v = cscdStrMapGet(&refMap, cscdSvSub(s, 0, k));
        if (v) {
            *text = (const char*)v->p;
            return k;
        }
    }
    return 0;
}

/* a numeric reference's code point as the specification turns it (C1 controls as Windows-1252 has them) */
static uint32_t numericRef(uint64_t cp) {
    static const uint16_t kC1[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                     0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                     0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
    if (cp >= 0x80 && cp <= 0x9F) return kC1[cp - 0x80];
    return (uint32_t)cp;
}

/* The character reference at s[0] (an '&'): what it stands for appended, how many bytes it took (0: none, the '&' is
 * itself). In an attribute, a name without its semicolon followed by '=' or a letter or digit is left as it is. */
static size_t charRef(CscdSv s, bool inAttribute, CscdStr* out) {
    if (s.len < 2) return 0;
    if (s.ptr[1] == '#') {
        size_t i = 2;
        const bool hex = i < s.len && (s.ptr[i] == 'x' || s.ptr[i] == 'X');
        if (hex) ++i;
        const size_t digits = i;
        uint64_t cp = 0;
        while (i < s.len && (hex ? cscdIsXDigit(s.ptr[i]) : cscdIsDigit(s.ptr[i]))) {
            const char c = s.ptr[i++];
            const int d = c <= '9' ? c - '0' : (cscdToLower(c) - 'a' + 10);
            if (cp <= 0x10FFFF) cp = cp * (hex ? 16 : 10) + (uint64_t)d;
        }
        if (i == digits) return 0;
        if (i < s.len && s.ptr[i] == ';') ++i;
        cscdUtf8Append(out, numericRef(cp));
        return i;
    }
    if (!cscdIsAlnum(s.ptr[1])) return 0;
    const char* text = NULL;
    const size_t k = matchRef(cscdSvFrom(s, 1), &text);
    if (!k) return 0;
    const bool semicolon = s.ptr[k] == ';';
    if (inAttribute && !semicolon && 1 + k < s.len && (s.ptr[1 + k] == '=' || cscdIsAlnum(s.ptr[1 + k]))) return 0;
    cscdStrAppendC(out, text);
    return 1 + k;
}

void cscdDecodeEntities(CscdSv s, CscdStr* out) {
    if (cscdSvFindChar(s, '&', 0) == CSCD_NPOS) {
        cscdStrAppend(out, s);
        return;
    }
    cscdStrReserve(out, out->len + s.len);
    for (size_t i = 0; i < s.len;) {
        if (s.ptr[i] == '&') {
            const size_t k = charRef(cscdSvFrom(s, i), false, out);
            if (k) {
                i += k;
                continue;
            }
        }
        cscdStrAppendChar(out, s.ptr[i++]);
    }
}

void cscdEscapeHTML(CscdSv s, bool attribute, CscdStr* out) {
    for (size_t i = 0; i < s.len; ++i) {
        const char c = s.ptr[i];
        switch (c) {
        case '&': cscdStrAppendC(out, "&amp;"); break;
        case '<': cscdStrAppendC(out, attribute ? "<" : "&lt;"); break;
        case '>': cscdStrAppendC(out, attribute ? ">" : "&gt;"); break;
        case '"':
            if (attribute) { cscdStrAppendC(out, "&quot;"); break; }
            cscdStrAppendChar(out, c);
            break;
        default:
            if ((unsigned char)c == 0xC2 && i + 1 < s.len && (unsigned char)s.ptr[i + 1] == 0xA0) { /* (a no-break space) */
                cscdStrAppendC(out, "&nbsp;");
                ++i;
            } else {
                cscdStrAppendChar(out, c);
            }
        }
    }
}

/* ---- the tokenizer ------------------------------------------------------------------------------------------------- */

typedef enum TokKind { TOK_DOCTYPE, TOK_START, TOK_END, TOK_COMMENT, TOK_CHARS, TOK_EOF } TokKind;

typedef struct Token {
    TokKind kind;
    CscdStr name;  /* a tag's (lowercase), a doctype's */
    CscdStr data;  /* characters, a comment */
    CscdStrPairVec attrs;
    bool selfClosing, selfClosingAcked;
    /* a doctype's */
    CscdStr publicId, systemId;
    bool hasPublic, hasSystem, forceQuirks;
} Token;

static void tokenFree(Token* t) {
    cscdStrFree(&t->name);
    cscdStrFree(&t->data);
    cscdStrPairVecFree(&t->attrs);
    cscdStrFree(&t->publicId);
    cscdStrFree(&t->systemId);
}

typedef enum TzState {
    TZ_DATA, TZ_RCDATA, TZ_RAWTEXT, TZ_SCRIPT, TZ_PLAINTEXT
} TzState;

typedef struct Tokenizer {
    CscdStr in; /* (the input, its line breaks made LF) */
    size_t i;
    TzState state;
    CscdStr lastStart; /* (the last start tag's name: the "appropriate end tag") */
    bool cdataAllowed; /* (the adjusted current node is not HTML: <![CDATA[ is a section) */
} Tokenizer;

static bool atEnd(const Tokenizer* tz) { return tz->i >= tz->in.len; }
static char peekAt(const Tokenizer* tz, size_t k) { return tz->i + k < tz->in.len ? tz->in.ptr[tz->i + k] : 0; }
static bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }
static bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* appends a byte, a NUL as U+FFFD (where the specification replaces it) */
static void putChar(CscdStr* s, char c, bool replaceNul) {
    if (c == 0 && replaceNul) cscdStrAppendC(s, "\xEF\xBF\xBD");
    else cscdStrAppendChar(s, c);
}

/* whether the input at i is "</" + the appropriate end tag's name, then a space, '/' or '>' */
static bool appropriateEnd(const Tokenizer* tz, size_t at) {
    const CscdSv s = cscdStrSv(&tz->in);
    if (at + 2 > s.len || s.ptr[at] != '<' || s.ptr[at + 1] != '/' || !tz->lastStart.len) return false;
    const size_t n = tz->lastStart.len;
    if (at + 2 + n > s.len) return false;
    for (size_t k = 0; k < n; ++k)
        if (cscdToLower(s.ptr[at + 2 + k]) != tz->lastStart.ptr[k]) return false;
    const char after = at + 2 + n < s.len ? s.ptr[at + 2 + n] : 0;
    return isWs(after) || after == '/' || after == '>';
}

static void tagAttributes(Tokenizer* tz, Token* t);

/* a start or end tag from the input at "<" or "</" (its name's first letter next) */
static void tagToken(Tokenizer* tz, Token* t, bool end) {
    memset(t, 0, sizeof *t);
    t->kind = end ? TOK_END : TOK_START;
    tz->i += end ? 2 : 1;
    while (!atEnd(tz)) {
        const char c = tz->in.ptr[tz->i];
        if (isWs(c) || c == '/' || c == '>') break;
        putChar(&t->name, cscdToLower(c), true);
        ++tz->i;
    }
    tagAttributes(tz, t);
}

/* from after a tag's name to its '>' (or the end: then no tag at all) */
static void tagAttributes(Tokenizer* tz, Token* t) {
    CscdStrMap seen = {0};
    for (;;) {
        while (!atEnd(tz) && isWs(tz->in.ptr[tz->i])) ++tz->i;
        if (atEnd(tz)) {
            t->kind = TOK_EOF; /* (eof in a tag: nothing) */
            break;
        }
        char c = tz->in.ptr[tz->i];
        if (c == '>') {
            ++tz->i;
            break;
        }
        if (c == '/') {
            ++tz->i;
            if (!atEnd(tz) && tz->in.ptr[tz->i] == '>') {
                t->selfClosing = true;
                ++tz->i;
                break;
            }
            continue;
        }
        /* an attribute's name (a '=' first is part of it) */
        CscdStr name = {0}, value = {0};
        if (c == '=') {
            cscdStrAppendChar(&name, '=');
            ++tz->i;
        }
        while (!atEnd(tz)) {
            c = tz->in.ptr[tz->i];
            if (isWs(c) || c == '/' || c == '>' || c == '=') break;
            putChar(&name, cscdToLower(c), true);
            ++tz->i;
        }
        while (!atEnd(tz) && isWs(tz->in.ptr[tz->i])) ++tz->i;
        if (!atEnd(tz) && tz->in.ptr[tz->i] == '=') {
            ++tz->i;
            while (!atEnd(tz) && isWs(tz->in.ptr[tz->i])) ++tz->i;
            if (!atEnd(tz) && (tz->in.ptr[tz->i] == '"' || tz->in.ptr[tz->i] == '\'')) {
                const char q = tz->in.ptr[tz->i++];
                while (!atEnd(tz) && tz->in.ptr[tz->i] != q) {
                    if (tz->in.ptr[tz->i] == '&') {
                        const size_t k = charRef(cscdSvFrom(cscdStrSv(&tz->in), tz->i), true, &value);
                        if (k) {
                            tz->i += k;
                            continue;
                        }
                    }
                    putChar(&value, tz->in.ptr[tz->i++], true);
                }
                if (!atEnd(tz)) ++tz->i;
                /* (after a quoted value a space, '/' or '>' should follow: anything else starts the next name) */
            } else {
                while (!atEnd(tz)) {
                    c = tz->in.ptr[tz->i];
                    if (isWs(c) || c == '>') break;
                    if (c == '&') {
                        const size_t k = charRef(cscdSvFrom(cscdStrSv(&tz->in), tz->i), true, &value);
                        if (k) {
                            tz->i += k;
                            continue;
                        }
                    }
                    putChar(&value, c, true);
                    ++tz->i;
                }
            }
        }
        /* (a name seen already: dropped, the first one stays) */
        bool inserted = false;
        cscdStrMapPut(&seen, cscdStrSv(&name), &inserted);
        if (inserted && t->kind == TOK_START) {
            CscdStrPair p = {name, value};
            cscdVecPush(&t->attrs, p);
        } else {
            cscdStrFree(&name);
            cscdStrFree(&value);
        }
    }
    cscdStrMapFree(&seen);
    if (t->kind == TOK_END) { /* (an end tag's attributes are nothing) */
        cscdStrPairVecFree(&t->attrs);
        memset(&t->attrs, 0, sizeof t->attrs);
    }
}

/* <!-- ... -->, as the comment states read it; tz->i at "<!--" */
static void commentToken(Tokenizer* tz, Token* t) {
    memset(t, 0, sizeof *t);
    t->kind = TOK_COMMENT;
    const CscdSv s = cscdStrSv(&tz->in);
    size_t i = tz->i + 4;
    /* comment start: "<!-->" and "<!--->" end at once */
    if (i < s.len && s.ptr[i] == '>') { tz->i = i + 1; return; }
    if (i + 1 < s.len && s.ptr[i] == '-' && s.ptr[i + 1] == '>') { tz->i = i + 2; return; }
    for (;;) {
        if (i >= s.len) { tz->i = s.len; return; }
        const char c = s.ptr[i];
        if (c == '-' && i + 1 < s.len && s.ptr[i + 1] == '-') {
            /* comment end: "-->", "--!>", or more dashes (one of them text) */
            size_t j = i + 2;
            if (j < s.len && s.ptr[j] == '>') { tz->i = j + 1; return; }
            if (j < s.len && s.ptr[j] == '!' && j + 1 < s.len && s.ptr[j + 1] == '>') { tz->i = j + 2; return; }
            if (j >= s.len || (j + 1 >= s.len && s.ptr[j] == '!')) { tz->i = s.len; return; }
            if (s.ptr[j] == '-') { /* ("---": one dash is text, the rest goes on) */
                cscdStrAppendChar(&t->data, '-');
                ++i;
                continue;
            }
            if (s.ptr[j] == '!') { /* ("--!" then not '>': all of it text) */
                cscdStrAppendC(&t->data, "--!");
                i = j + 1;
                continue;
            }
            cscdStrAppendC(&t->data, "--");
            i = j;
            continue;
        }
        putChar(&t->data, c, true);
        ++i;
    }
}

/* <!DOCTYPE ...>: up to its '>' (any one, even quoted: the specification ends it there too) */
static void doctypeToken(Tokenizer* tz, Token* t) {
    memset(t, 0, sizeof *t);
    t->kind = TOK_DOCTYPE;
    const CscdSv s = cscdStrSv(&tz->in);
    size_t i = tz->i + 9, end = cscdSvFindChar(s, '>', i);
    if (end == CSCD_NPOS) {
        end = s.len;
        t->forceQuirks = true;
    }
    tz->i = end < s.len ? end + 1 : s.len;
    while (i < end && isWs(s.ptr[i])) ++i;
    if (i >= end) {
        t->forceQuirks = true;
        return;
    }
    while (i < end && !isWs(s.ptr[i])) putChar(&t->name, cscdToLower(s.ptr[i++]), true);
    while (i < end && isWs(s.ptr[i])) ++i;
    if (i >= end) return;
    const CscdSv rest = cscdSvSub(s, i, end - i);
    const bool pub = cscdSvIEqC(cscdSvSub(rest, 0, 6), "public"), sys = cscdSvIEqC(cscdSvSub(rest, 0, 6), "system");
    if (!pub && !sys) {
        t->forceQuirks = true;
        return;
    }
    i += 6;
    for (int k = 0; k < 2; ++k) {
        while (i < end && isWs(s.ptr[i])) ++i;
        if (i >= end) {
            if (k == 0) t->forceQuirks = true;
            return;
        }
        if (s.ptr[i] != '"' && s.ptr[i] != '\'') {
            t->forceQuirks = true;
            return;
        }
        const char q = s.ptr[i++];
        CscdStr* into = (k == 0 && pub) ? &t->publicId : &t->systemId;
        if (k == 0 && pub) t->hasPublic = true;
        else t->hasSystem = true;
        const size_t close = cscdSvFindChar(s, q, i);
        if (close == CSCD_NPOS || close > end) {
            cscdStrAppend(into, cscdSvSub(s, i, end - i));
            t->forceQuirks = true;
            return;
        }
        cscdStrAppend(into, cscdSvSub(s, i, close - i));
        i = close + 1;
        if (!(k == 0 && pub)) return; /* (a system id only follows a public one) */
    }
}

/* The next token. Characters come as one token for a run of them (the tree construction splits it where it must). */
static void nextToken(Tokenizer* tz, Token* t) {
    memset(t, 0, sizeof *t);
    const CscdSv s = cscdStrSv(&tz->in);
    if (atEnd(tz)) {
        t->kind = TOK_EOF;
        return;
    }
    if (tz->state == TZ_PLAINTEXT) {
        t->kind = TOK_CHARS;
        for (; tz->i < s.len; ++tz->i) putChar(&t->data, s.ptr[tz->i], true);
        return;
    }
    if (tz->state == TZ_RCDATA || tz->state == TZ_RAWTEXT || tz->state == TZ_SCRIPT) {
        if (appropriateEnd(tz, tz->i)) {
            tagToken(tz, t, true);
            tz->state = TZ_DATA;
            return;
        }
        t->kind = TOK_CHARS;
        /* script data: "<!--" opens an escaped part, in which "<script" opens a double-escaped one, where its end tag
         * does not end the script; the ends: "-->" closes the escape, "</script" the double escape */
        int escape = 0; /* 0 none, 1 escaped, 2 double escaped */
        while (tz->i < s.len) {
            if (s.ptr[tz->i] == '<' && escape != 2 && appropriateEnd(tz, tz->i)) break;
            const char c = s.ptr[tz->i];
            if (tz->state == TZ_SCRIPT) {
                if (escape == 0 && c == '<' && tz->i + 4 <= s.len && !strncmp(s.ptr + tz->i, "<!--", 4)) {
                    cscdStrAppendC(&t->data, "<!--");
                    tz->i += 4;
                    escape = 1;
                    /* ("<!--" right after: its dashes may be the "-->" that closes it) */
                    continue;
                }
                if (escape && c == '-' && tz->i + 2 < s.len && s.ptr[tz->i + 1] == '-' && s.ptr[tz->i + 2] == '>') {
                    cscdStrAppendC(&t->data, "-->");
                    tz->i += 3;
                    escape = 0;
                    continue;
                }
                if (escape && c == '<') {
                    /* "<script" or "</script" followed by a space, '/' or '>' */
                    const bool closing = tz->i + 1 < s.len && s.ptr[tz->i + 1] == '/';
                    const size_t nameAt = tz->i + (closing ? 2 : 1);
                    if (nameAt + 6 <= s.len && cscdSvIEqC(cscdSvSub(s, nameAt, 6), "script")) {
                        const char after = nameAt + 6 < s.len ? s.ptr[nameAt + 6] : 0;
                        if (isWs(after) || after == '/' || after == '>') {
                            if (escape == 1 && !closing) escape = 2;
                            else if (escape == 2 && closing) escape = 1;
                            cscdStrAppend(&t->data, cscdSvSub(s, tz->i, nameAt + 6 - tz->i));
                            tz->i = nameAt + 6;
                            continue;
                        }
                    }
                }
            }
            if (c == '&' && tz->state == TZ_RCDATA) {
                const size_t k = charRef(cscdSvFrom(s, tz->i), false, &t->data);
                if (k) {
                    tz->i += k;
                    continue;
                }
            }
            putChar(&t->data, c, true);
            ++tz->i;
        }
        if (!t->data.len) nextToken(tz, t);
        return;
    }
    /* data */
    if (s.ptr[tz->i] == '<') {
        const char c = peekAt(tz, 1);
        if (isAlpha(c)) {
            tagToken(tz, t, false);
            if (t->kind == TOK_START) cscdStrSet(&tz->lastStart, cscdStrSv(&t->name));
            return;
        }
        if (c == '/') {
            const char d = peekAt(tz, 2);
            if (isAlpha(d)) {
                tagToken(tz, t, true);
                return;
            }
            if (d == '>') { /* "</>": nothing */
                tz->i += 3;
                nextToken(tz, t);
                return;
            }
            if (d == 0 && tz->i + 2 >= s.len) { /* "</" at the end: text */
                t->kind = TOK_CHARS;
                cscdStrAppendC(&t->data, "</");
                tz->i += 2;
                return;
            }
            /* a bogus comment, up to '>' */
            t->kind = TOK_COMMENT;
            size_t e = cscdSvFindChar(s, '>', tz->i + 2);
            if (e == CSCD_NPOS) e = s.len;
            for (size_t k = tz->i + 2; k < e; ++k) putChar(&t->data, s.ptr[k], true);
            tz->i = e < s.len ? e + 1 : s.len;
            return;
        }
        if (c == '!') {
            if (tz->i + 4 <= s.len && !strncmp(s.ptr + tz->i, "<!--", 4)) {
                commentToken(tz, t);
                return;
            }
            if (tz->i + 9 <= s.len && cscdSvIEqC(cscdSvSub(s, tz->i + 2, 7), "doctype")) {
                doctypeToken(tz, t);
                return;
            }
            if (tz->cdataAllowed && tz->i + 9 <= s.len && !strncmp(s.ptr + tz->i + 2, "[CDATA[", 7)) {
                t->kind = TOK_CHARS;
                size_t e = cscdSvFind(s, CSCD_SV("]]>"), tz->i + 9);
                if (e == CSCD_NPOS) e = s.len;
                cscdStrAppend(&t->data, cscdSvSub(s, tz->i + 9, e - (tz->i + 9)));
                tz->i = e < s.len ? e + 3 : s.len;
                if (!t->data.len) nextToken(tz, t);
                return;
            }
            /* a bogus comment */
            t->kind = TOK_COMMENT;
            size_t e = cscdSvFindChar(s, '>', tz->i + 2);
            if (e == CSCD_NPOS) e = s.len;
            for (size_t k = tz->i + 2; k < e; ++k) putChar(&t->data, s.ptr[k], true);
            tz->i = e < s.len ? e + 1 : s.len;
            return;
        }
        if (c == '?') { /* a bogus comment, "?" its first character */
            t->kind = TOK_COMMENT;
            size_t e = cscdSvFindChar(s, '>', tz->i + 1);
            if (e == CSCD_NPOS) e = s.len;
            for (size_t k = tz->i + 1; k < e; ++k) putChar(&t->data, s.ptr[k], true);
            tz->i = e < s.len ? e + 1 : s.len;
            return;
        }
    }
    t->kind = TOK_CHARS;
    for (bool first = true; tz->i < s.len; first = false) {
        const char c = s.ptr[tz->i];
        if (c == '<' && !first) break;
        if (c == '<' && first) {
            /* (a '<' that starts nothing: text) */
            cscdStrAppendChar(&t->data, '<');
            ++tz->i;
            continue;
        }
        if (c == '&') {
            const size_t k = charRef(cscdSvFrom(s, tz->i), false, &t->data);
            if (k) {
                tz->i += k;
                continue;
            }
        }
        putChar(&t->data, c, false); /* (a NUL in data stays: the tree construction drops it) */
        ++tz->i;
    }
}

/* ---- element kinds ------------------------------------------------------------------------------------------------- */

static bool tagIn(CscdSv t, const char* const* list, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (cscdSvEqC(t, list[i])) return true;
    return false;
}
#define TAG_IN(t, list) tagIn((t), (list), CSCD_COUNT(list))

static bool isHtml(const CscdElement* e, const char* tag) { return e && e->ns_ == CSCD_NS_HTML && cscdStrEqC(&e->tag, tag); }
static bool isHtmlIn(const CscdElement* e, const char* const* list, size_t n) {
    return e && e->ns_ == CSCD_NS_HTML && tagIn(cscdStrSv(&e->tag), list, n);
}
#define HTML_IN(e, list) isHtmlIn((e), (list), CSCD_COUNT(list))

static const char* const kSpecialHtml[] = {
    "address", "applet", "area", "article", "aside", "base", "basefont", "bgsound", "blockquote", "body", "br", "button",
    "caption", "center", "col", "colgroup", "dd", "details", "dir", "div", "dl", "dt", "embed", "fieldset", "figcaption",
    "figure", "footer", "form", "frame", "frameset", "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hgroup", "hr",
    "html", "iframe", "img", "input", "keygen", "li", "link", "listing", "main", "marquee", "menu", "meta", "nav",
    "noembed", "noframes", "noscript", "object", "ol", "p", "param", "plaintext", "pre", "script", "search", "section",
    "select", "source", "style", "summary", "table", "tbody", "td", "template", "textarea", "tfoot", "th", "thead",
    "title", "tr", "track", "ul", "wbr", "xmp"};

static bool isSpecial(const CscdElement* e) {
    if (e->ns_ == CSCD_NS_HTML) return tagIn(cscdStrSv(&e->tag), kSpecialHtml, CSCD_COUNT(kSpecialHtml));
    if (e->ns_ == CSCD_NS_MATHML)
        return cscdStrEqC(&e->tag, "mi") || cscdStrEqC(&e->tag, "mo") || cscdStrEqC(&e->tag, "mn") || cscdStrEqC(&e->tag, "ms") ||
               cscdStrEqC(&e->tag, "mtext") || cscdStrEqC(&e->tag, "annotation-xml");
    return cscdStrEqC(&e->tag, "foreignobject") || cscdStrEqC(&e->tag, "desc") || cscdStrEqC(&e->tag, "title");
}

static const char* const kFormatting[] = {"a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u"};

static bool mathmlTextPoint(const CscdElement* e) {
    return e->ns_ == CSCD_NS_MATHML && (cscdStrEqC(&e->tag, "mi") || cscdStrEqC(&e->tag, "mo") || cscdStrEqC(&e->tag, "mn") ||
                                         cscdStrEqC(&e->tag, "ms") || cscdStrEqC(&e->tag, "mtext"));
}

static bool htmlIntegrationPoint(const CscdElement* e) {
    if (e->ns_ == CSCD_NS_MATHML && cscdStrEqC(&e->tag, "annotation-xml")) {
        const CscdSv enc = cscdElementAttrC(e, "encoding");
        return cscdSvIEqC(enc, "text/html") || cscdSvIEqC(enc, "application/xhtml+xml");
    }
    return e->ns_ == CSCD_NS_SVG && (cscdStrEqC(&e->tag, "foreignobject") || cscdStrEqC(&e->tag, "desc") || cscdStrEqC(&e->tag, "title"));
}

/* ---- the tree construction ----------------------------------------------------------------------------------------- */

typedef enum Mode {
    M_INITIAL, M_BEFORE_HTML, M_BEFORE_HEAD, M_IN_HEAD, M_IN_HEAD_NOSCRIPT, M_AFTER_HEAD, M_IN_BODY, M_TEXT, M_IN_TABLE,
    M_IN_TABLE_TEXT, M_IN_CAPTION, M_IN_COLUMN_GROUP, M_IN_TABLE_BODY, M_IN_ROW, M_IN_CELL,
    M_IN_TEMPLATE, M_AFTER_BODY, M_IN_FRAMESET, M_AFTER_FRAMESET, M_AFTER_AFTER_BODY, M_AFTER_AFTER_FRAMESET
} Mode;

typedef struct TreeBuilder {
    CscdNode* document;     /* (the document's node, or a fragment's holder) */
    CscdPtrVec stack;       /* CscdElement*: the stack of open elements */
    CscdPtrVec formatting;  /* CscdElement*, NULL for a marker: the list of active formatting elements */
    CSCD_VEC(Mode) templateModes;
    Mode mode, original;
    CscdElement* head;      /* the head element pointer */
    CscdElement* form;      /* the form element pointer */
    CscdElement* context;   /* (a fragment's context element) */
    bool framesetOk, fosterParenting, quirks, ignoreLf, fragment, done;
    CscdStr pendingTable;   /* (in table text: the characters waiting) */
    bool pendingNonSpace;
    Tokenizer* tz;
    size_t maxDepth;        /* (how many elements are kept open at most: deeper ones go side by side) */
} TreeBuilder;

static CscdElement* current(const TreeBuilder* tb) { return tb->stack.len ? (CscdElement*)cscdVecLast(&tb->stack) : NULL; }

static CscdElement* adjustedCurrent(const TreeBuilder* tb) {
    if (tb->fragment && tb->stack.len == 1) return tb->context;
    return current(tb);
}

static bool onStack(const TreeBuilder* tb, const CscdElement* e) {
    for (size_t i = 0; i < tb->stack.len; ++i) if (tb->stack.items[i] == e) return true;
    return false;
}

static void removeFromStack(TreeBuilder* tb, const CscdElement* e) {
    for (size_t i = tb->stack.len; i-- > 0;)
        if (tb->stack.items[i] == e) {
            cscdVecRemove(&tb->stack, i);
            return;
        }
}

static bool templateOnStack(const TreeBuilder* tb) {
    for (size_t i = 0; i < tb->stack.len; ++i) if (isHtml((const CscdElement*)tb->stack.items[i], "template")) return true;
    return false;
}

/* scopes: "has an element in scope" and its variants */
typedef enum Scope { S_DEFAULT, S_LIST, S_BUTTON, S_TABLE } Scope;

static bool scopeBoundary(const CscdElement* e, Scope scope) {
    static const char* const html[] = {"applet", "caption", "html", "table", "td", "th", "marquee", "object", "select", "template"};
    if (scope == S_TABLE) return isHtml(e, "html") || isHtml(e, "table") || isHtml(e, "template");
    if (HTML_IN(e, html)) return true;
    if (e->ns_ == CSCD_NS_MATHML && (mathmlTextPoint(e) || cscdStrEqC(&e->tag, "annotation-xml"))) return true;
    if (e->ns_ == CSCD_NS_SVG && (cscdStrEqC(&e->tag, "foreignobject") || cscdStrEqC(&e->tag, "desc") || cscdStrEqC(&e->tag, "title"))) return true;
    if (scope == S_LIST && (isHtml(e, "ol") || isHtml(e, "ul"))) return true;
    if (scope == S_BUTTON && isHtml(e, "button")) return true;
    return false;
}

static bool inScope(const TreeBuilder* tb, const char* tag, Scope scope) {
    for (size_t i = tb->stack.len; i-- > 0;) {
        const CscdElement* e = (const CscdElement*)tb->stack.items[i];
        if (isHtml(e, tag)) return true;
        if (scopeBoundary(e, scope)) return false;
    }
    return false;
}

static bool elementInScope(const TreeBuilder* tb, const CscdElement* target, Scope scope) {
    for (size_t i = tb->stack.len; i-- > 0;) {
        const CscdElement* e = (const CscdElement*)tb->stack.items[i];
        if (e == target) return true;
        if (scopeBoundary(e, scope)) return false;
    }
    return false;
}

static bool headingInScope(const TreeBuilder* tb) {
    static const char* const h[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
    for (size_t i = tb->stack.len; i-- > 0;) {
        const CscdElement* e = (const CscdElement*)tb->stack.items[i];
        if (HTML_IN(e, h)) return true;
        if (scopeBoundary(e, S_DEFAULT)) return false;
    }
    return false;
}

static void popUntilTag(TreeBuilder* tb, const char* tag) {
    while (tb->stack.len) {
        CscdElement* e = (CscdElement*)cscdVecPop(&tb->stack);
        if (isHtml(e, tag)) return;
    }
}

static void popUntilOneOf(TreeBuilder* tb, const char* const* tags, size_t n) {
    while (tb->stack.len) {
        CscdElement* e = (CscdElement*)cscdVecPop(&tb->stack);
        if (isHtmlIn(e, tags, n)) return;
    }
}

static void impliedEndTags(TreeBuilder* tb, const char* except, bool thorough) {
    static const char* const implied[] = {"dd", "dt", "li", "optgroup", "option", "p", "rb", "rp", "rt", "rtc"};
    static const char* const more[] = {"caption", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"};
    for (;;) {
        CscdElement* e = current(tb);
        if (!e || (except && isHtml(e, except))) return;
        if (!(HTML_IN(e, implied) || (thorough && HTML_IN(e, more)))) return;
        (void)cscdVecPop(&tb->stack);
    }
}

static void closeP(TreeBuilder* tb) {
    impliedEndTags(tb, "p", false);
    popUntilTag(tb, "p");
}

static void closePInButtonScope(TreeBuilder* tb) {
    if (inScope(tb, "p", S_BUTTON)) closeP(tb);
}

/* ---- inserting ---------------------------------------------------------------------------------------------------- */

/* where a node goes: into parent, before before (NULL: at its end) */
typedef struct Place { CscdNode* parent; CscdNode* before; } Place;

static Place insertionPlace(TreeBuilder* tb, CscdElement* override) {
    CscdElement* target = override ? override : current(tb);
    Place p = {target ? &target->node : tb->document, NULL};
    static const char* const tableish[] = {"table", "tbody", "tfoot", "thead", "tr"};
    if (tb->fosterParenting && HTML_IN(target, tableish)) {
        /* foster parenting: before the last table, unless a template is open after it */
        CscdElement *lastTemplate = NULL, *lastTable = NULL;
        size_t templateAt = 0, tableAt = 0;
        for (size_t i = tb->stack.len; i-- > 0;) {
            CscdElement* e = (CscdElement*)tb->stack.items[i];
            if (!lastTemplate && isHtml(e, "template")) { lastTemplate = e; templateAt = i; }
            if (!lastTable && isHtml(e, "table")) { lastTable = e; tableAt = i; }
        }
        if (lastTemplate && (!lastTable || templateAt > tableAt)) return (Place){&lastTemplate->node, NULL};
        if (!lastTable) return (Place){tb->stack.len ? &((CscdElement*)tb->stack.items[0])->node : tb->document, NULL};
        if (lastTable->node.parent) return (Place){lastTable->node.parent, &lastTable->node};
        return (Place){&((CscdElement*)tb->stack.items[tableAt - 1])->node, NULL};
    }
    return p;
}

static void placeNode(Place p, CscdNode* n) {
    if (!(p.before ? cscdNodeInsertBefore(p.parent, n, p.before) : cscdNodeAppendChild(p.parent, n))) cscdNodeDestroy(n);
}

static void insertChars(TreeBuilder* tb, CscdSv s) {
    if (!s.len) return;
    const Place p = insertionPlace(tb, NULL);
    if (p.parent == tb->document && !tb->fragment) return; /* (text in the document itself: none) */
    /* (next to text: joins it) */
    CscdNode* prev = NULL;
    if (p.before) {
        const int at = cscdNodeIndexInParent(p.before);
        if (at > 0) prev = cscdNodeChild(p.parent, (size_t)at - 1);
    } else if (p.parent->children.len) {
        prev = cscdNodeChild(p.parent, p.parent->children.len - 1);
    }
    if (prev && cscdNodeIsText(prev)) {
        cscdStrAppend(&((CscdText*)prev)->data, s);
        return;
    }
    placeNode(p, &cscdTextNew(s)->node);
}

static void insertComment(TreeBuilder* tb, CscdSv s, CscdNode* into) {
    CscdNode* c = &cscdCommentNew(s)->node;
    if (into) {
        if (!cscdNodeAppendChild(into, c)) cscdNodeDestroy(c);
        return;
    }
    placeNode(insertionPlace(tb, NULL), c);
}

static CscdElement* createElement(Token* t, uint8_t ns) {
    CscdElement* e = cscdElementNew(cscdStrSv(&t->name));
    e->ns_ = ns;
    e->attributes = t->attrs;
    memset(&t->attrs, 0, sizeof t->attrs);
    if (ns == CSCD_NS_HTML) {
        if (cscdElementIs(e, "input") && (cscdSvIEqC(cscdElementAttrC(e, "type"), "checkbox") || cscdSvIEqC(cscdElementAttrC(e, "type"), "radio")) &&
            cscdElementHasAttributeC(e, "checked"))
            e->state |= CSCD_STATE_CHECKED;
        if (cscdElementIs(e, "option") && cscdElementHasAttributeC(e, "selected")) e->state |= CSCD_STATE_CHECKED;
    }
    return e;
}

/* inserts an element for the token where it goes, and opens it (unless the stack is as deep as it goes: then it is
 * left empty, and what follows goes beside it) */
static CscdElement* insertElement(TreeBuilder* tb, Token* t, uint8_t ns) {
    CscdElement* e = createElement(t, ns);
    placeNode(insertionPlace(tb, NULL), &e->node);
    if (tb->stack.len < tb->maxDepth) cscdVecPush(&tb->stack, e);
    return e;
}

static CscdElement* insertTag(TreeBuilder* tb, const char* name) {
    Token t;
    memset(&t, 0, sizeof t);
    t.kind = TOK_START;
    t.name = cscdStrDupC(name);
    CscdElement* e = insertElement(tb, &t, CSCD_NS_HTML);
    tokenFree(&t);
    return e;
}

/* attributes the element does not have yet, from the token (a second <html>, <body>) */
static void addMissingAttributes(CscdElement* e, const Token* t) {
    for (size_t i = 0; i < t->attrs.len; ++i) {
        if (cscdElementGetAttribute(e, cscdStrSv(&t->attrs.items[i].name))) continue;
        CscdStrPair p = {cscdStrCopy(&t->attrs.items[i].name), cscdStrCopy(&t->attrs.items[i].value)};
        cscdVecPush(&e->attributes, p);
    }
    e->classesValid_ = false;
}

/* ---- foreign elements' names and attributes ---------------------------------------------------------------------- */

static void adjustAttributes(Token* t, const char* const (*table)[2], size_t n) {
    for (size_t i = 0; i < t->attrs.len; ++i)
        for (size_t k = 0; k < n; ++k)
            if (cscdStrEqC(&t->attrs.items[i].name, table[k][0])) {
                cscdStrSetC(&t->attrs.items[i].name, table[k][1]);
                break;
            }
}

static void adjustSvgAttributes(Token* t) {
    static const char* const table[][2] = {
        {"attributename", "attributeName"}, {"attributetype", "attributeType"}, {"basefrequency", "baseFrequency"},
        {"baseprofile", "baseProfile"}, {"calcmode", "calcMode"}, {"clippathunits", "clipPathUnits"},
        {"diffuseconstant", "diffuseConstant"}, {"edgemode", "edgeMode"}, {"filterunits", "filterUnits"},
        {"glyphref", "glyphRef"}, {"gradienttransform", "gradientTransform"}, {"gradientunits", "gradientUnits"},
        {"kernelmatrix", "kernelMatrix"}, {"kernelunitlength", "kernelUnitLength"}, {"keypoints", "keyPoints"},
        {"keysplines", "keySplines"}, {"keytimes", "keyTimes"}, {"lengthadjust", "lengthAdjust"},
        {"limitingconeangle", "limitingConeAngle"}, {"markerheight", "markerHeight"}, {"markerunits", "markerUnits"},
        {"markerwidth", "markerWidth"}, {"maskcontentunits", "maskContentUnits"}, {"maskunits", "maskUnits"},
        {"numoctaves", "numOctaves"}, {"pathlength", "pathLength"}, {"patterncontentunits", "patternContentUnits"},
        {"patterntransform", "patternTransform"}, {"patternunits", "patternUnits"}, {"pointsatx", "pointsAtX"},
        {"pointsaty", "pointsAtY"}, {"pointsatz", "pointsAtZ"}, {"preservealpha", "preserveAlpha"},
        {"preserveaspectratio", "preserveAspectRatio"}, {"primitiveunits", "primitiveUnits"}, {"refx", "refX"},
        {"refy", "refY"}, {"repeatcount", "repeatCount"}, {"repeatdur", "repeatDur"}, {"requiredextensions", "requiredExtensions"},
        {"requiredfeatures", "requiredFeatures"}, {"specularconstant", "specularConstant"}, {"specularexponent", "specularExponent"},
        {"spreadmethod", "spreadMethod"}, {"startoffset", "startOffset"}, {"stddeviation", "stdDeviation"},
        {"stitchtiles", "stitchTiles"}, {"surfacescale", "surfaceScale"}, {"systemlanguage", "systemLanguage"},
        {"tablevalues", "tableValues"}, {"targetx", "targetX"}, {"targety", "targetY"}, {"textlength", "textLength"},
        {"viewbox", "viewBox"}, {"viewtarget", "viewTarget"}, {"xchannelselector", "xChannelSelector"},
        {"ychannelselector", "yChannelSelector"}, {"zoomandpan", "zoomAndPan"}};
    adjustAttributes(t, table, CSCD_COUNT(table));
}

static void adjustMathmlAttributes(Token* t) {
    static const char* const table[][2] = {{"definitionurl", "definitionURL"}};
    adjustAttributes(t, table, CSCD_COUNT(table));
}

/* (an SVG element's name as SVG has it: lowercase here, as cascade keeps tag names: foreignobject and its kin stay as
 * they are) */

/* ---- the list of active formatting elements ----------------------------------------------------------------------- */

static bool sameElement(const CscdElement* a, const CscdElement* b) {
    if (!cscdStrEq(&a->tag, &b->tag) || a->ns_ != b->ns_ || a->attributes.len != b->attributes.len) return false;
    for (size_t i = 0; i < a->attributes.len; ++i) {
        const CscdStr* v = cscdElementGetAttribute(b, cscdStrSv(&a->attributes.items[i].name));
        if (!v || !cscdStrEq(v, &a->attributes.items[i].value)) return false;
    }
    return true;
}

static void pushFormatting(TreeBuilder* tb, CscdElement* e) {
    /* (the Noah's Ark clause: three alike after the last marker at most; the earliest goes) */
    size_t alike = 0, earliest = 0;
    for (size_t i = tb->formatting.len; i-- > 0;) {
        CscdElement* f = (CscdElement*)tb->formatting.items[i];
        if (!f) break;
        if (sameElement(f, e)) {
            ++alike;
            earliest = i;
        }
    }
    if (alike >= 3) cscdVecRemove(&tb->formatting, earliest);
    cscdVecPush(&tb->formatting, e);
}

static void pushMarker(TreeBuilder* tb) { cscdVecPush(&tb->formatting, NULL); }

static void clearToMarker(TreeBuilder* tb) {
    while (tb->formatting.len) {
        CscdElement* e = (CscdElement*)cscdVecPop(&tb->formatting);
        if (!e) return;
    }
}

static long formattingIndex(const TreeBuilder* tb, const CscdElement* e) {
    for (size_t i = tb->formatting.len; i-- > 0;) if (tb->formatting.items[i] == e) return (long)i;
    return -1;
}

static void reconstructFormatting(TreeBuilder* tb) {
    if (!tb->formatting.len) return;
    size_t i = tb->formatting.len - 1;
    CscdElement* e = (CscdElement*)tb->formatting.items[i];
    if (!e || onStack(tb, e)) return;
    /* (back to the last one that is a marker or open, then forwards making each anew) */
    while (i > 0) {
        e = (CscdElement*)tb->formatting.items[i - 1];
        if (!e || onStack(tb, e)) break;
        --i;
    }
    for (; i < tb->formatting.len; ++i) {
        CscdElement* old = (CscdElement*)tb->formatting.items[i];
        Token t;
        memset(&t, 0, sizeof t);
        t.name = cscdStrCopy(&old->tag);
        cscdStrPairVecCopy(&t.attrs, &old->attributes);
        CscdElement* made = insertElement(tb, &t, old->ns_);
        tokenFree(&t);
        tb->formatting.items[i] = made;
    }
}

/* the adoption agency algorithm: an end tag of a formatting element (true: done; false: as any other end tag) */
static bool adoptionAgency(TreeBuilder* tb, CscdSv subject) {
    CscdElement* cur = current(tb);
    if (cur && cur->ns_ == CSCD_NS_HTML && cscdSvEq(cscdStrSv(&cur->tag), subject) && formattingIndex(tb, cur) < 0) {
        (void)cscdVecPop(&tb->stack);
        return true;
    }
    for (int outer = 0; outer < 8; ++outer) {
        /* the formatting element: the last with that name after the last marker */
        CscdElement* fe = NULL;
        long feAt = -1;
        for (size_t i = tb->formatting.len; i-- > 0;) {
            CscdElement* f = (CscdElement*)tb->formatting.items[i];
            if (!f) break;
            if (cscdSvEq(cscdStrSv(&f->tag), subject)) {
                fe = f;
                feAt = (long)i;
                break;
            }
        }
        if (!fe) return false;
        if (!onStack(tb, fe)) {
            cscdVecRemove(&tb->formatting, (size_t)feAt);
            return true;
        }
        if (!elementInScope(tb, fe, S_DEFAULT)) return true;
        /* the furthest block: the first special element on the stack after it */
        size_t feStack = 0;
        for (size_t i = 0; i < tb->stack.len; ++i) if (tb->stack.items[i] == fe) feStack = i;
        CscdElement* fb = NULL;
        size_t fbStack = 0;
        for (size_t i = feStack + 1; i < tb->stack.len; ++i)
            if (isSpecial((CscdElement*)tb->stack.items[i])) {
                fb = (CscdElement*)tb->stack.items[i];
                fbStack = i;
                break;
            }
        if (!fb) {
            tb->stack.len = feStack;
            cscdVecRemove(&tb->formatting, (size_t)feAt);
            return true;
        }
        CscdElement* ancestor = (CscdElement*)tb->stack.items[feStack - 1];
        size_t bookmark = (size_t)feAt;
        CscdElement* node = fb;
        CscdElement* last = fb;
        size_t nodeStack = fbStack;
        for (int inner = 1;; ++inner) {
            node = (CscdElement*)tb->stack.items[--nodeStack];
            if (node == fe) break;
            long nodeAt = formattingIndex(tb, node);
            if (inner > 3 && nodeAt >= 0) {
                cscdVecRemove(&tb->formatting, (size_t)nodeAt);
                if ((size_t)nodeAt < bookmark) --bookmark;
                nodeAt = -1;
            }
            if (nodeAt < 0) {
                cscdVecRemove(&tb->stack, nodeStack);
                continue;
            }
            /* a new element for it, in its place in both lists */
            CscdElement* made = cscdElementClone(node, false);
            tb->formatting.items[nodeAt] = made;
            tb->stack.items[nodeStack] = made;
            node = made;
            if (last == fb) bookmark = (size_t)nodeAt + 1;
            CscdNode* moving = last->node.parent ? cscdNodeRemoveChild(last->node.parent, &last->node) : &last->node;
            if (moving && !cscdNodeAppendChild(&node->node, moving)) cscdNodeDestroy(moving);
            last = node;
        }
        /* last into the common ancestor (foster parented where it would be) */
        CscdNode* moving = last->node.parent ? cscdNodeRemoveChild(last->node.parent, &last->node) : &last->node;
        if (moving) placeNode(insertionPlace(tb, ancestor), moving);
        /* a new formatting element takes the furthest block's children, and goes in it */
        CscdElement* made = cscdElementClone(fe, false);
        while (fb->node.children.len) {
            CscdNode* c = cscdNodeRemoveChild(&fb->node, cscdNodeChild(&fb->node, 0));
            if (c && !cscdNodeAppendChild(&made->node, c)) cscdNodeDestroy(c);
        }
        if (!cscdNodeAppendChild(&fb->node, &made->node)) {
            cscdNodeDestroy(&made->node);
            return true;
        }
        const long feNow = formattingIndex(tb, fe);
        if (feNow >= 0) {
            cscdVecRemove(&tb->formatting, (size_t)feNow);
            if ((size_t)feNow < bookmark) --bookmark;
        }
        if (bookmark > tb->formatting.len) bookmark = tb->formatting.len;
        cscdVecInsert(&tb->formatting, bookmark, made);
        removeFromStack(tb, fe);
        for (size_t i = 0; i < tb->stack.len; ++i)
            if (tb->stack.items[i] == fb) {
                cscdVecInsert(&tb->stack, i + 1, made);
                break;
            }
    }
    return true;
}

/* ---- the insertion mode, reset ------------------------------------------------------------------------------------ */

static void resetMode(TreeBuilder* tb) {
    for (size_t i = tb->stack.len; i-- > 0;) {
        CscdElement* node = (CscdElement*)tb->stack.items[i];
        const bool last = i == 0;
        if (last && tb->fragment) node = tb->context;
        if ((isHtml(node, "td") || isHtml(node, "th")) && !last) { tb->mode = M_IN_CELL; return; }
        if (isHtml(node, "tr")) { tb->mode = M_IN_ROW; return; }
        if (isHtml(node, "tbody") || isHtml(node, "thead") || isHtml(node, "tfoot")) { tb->mode = M_IN_TABLE_BODY; return; }
        if (isHtml(node, "caption")) { tb->mode = M_IN_CAPTION; return; }
        if (isHtml(node, "colgroup")) { tb->mode = M_IN_COLUMN_GROUP; return; }
        if (isHtml(node, "table")) { tb->mode = M_IN_TABLE; return; }
        if (isHtml(node, "template")) { tb->mode = tb->templateModes.len ? cscdVecLast(&tb->templateModes) : M_IN_TEMPLATE; return; }
        if (isHtml(node, "head") && !last) { tb->mode = M_IN_HEAD; return; }
        if (isHtml(node, "body")) { tb->mode = M_IN_BODY; return; }
        if (isHtml(node, "frameset")) { tb->mode = M_IN_FRAMESET; return; }
        if (isHtml(node, "html")) { tb->mode = tb->head ? M_AFTER_HEAD : M_BEFORE_HEAD; return; }
        if (last) { tb->mode = M_IN_BODY; return; }
    }
    tb->mode = M_IN_BODY;
}

/* ---- quirks ------------------------------------------------------------------------------------------------------- */

static bool startsWithI(CscdSv s, const char* prefix) {
    const size_t n = strlen(prefix);
    return s.len >= n && cscdSvIEqC(cscdSvSub(s, 0, n), prefix);
}

static bool quirksDoctype(const Token* t) {
    if (t->forceQuirks || !cscdStrEqC(&t->name, "html")) return true;
    const CscdSv pub = cscdStrSv(&t->publicId), sys = cscdStrSv(&t->systemId);
    static const char* const exact[] = {"-//W3O//DTD W3 HTML Strict 3.0//EN//", "-/W3C/DTD HTML 4.0 Transitional/EN", "HTML"};
    for (size_t i = 0; i < CSCD_COUNT(exact) && t->hasPublic; ++i) if (cscdSvIEqC(pub, exact[i])) return true;
    if (t->hasSystem && cscdSvIEqC(sys, "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd")) return true;
    static const char* const prefixes[] = {
        "+//Silmaril//dtd html Pro v0r11 19970101//", "-//AS//DTD HTML 3.0 asWedit + extensions//",
        "-//AdvaSoft Ltd//DTD HTML 3.0 asWedit + extensions//", "-//IETF//DTD HTML 2.0 Level 1//", "-//IETF//DTD HTML 2.0 Level 2//",
        "-//IETF//DTD HTML 2.0 Strict Level 1//", "-//IETF//DTD HTML 2.0 Strict Level 2//", "-//IETF//DTD HTML 2.0 Strict//",
        "-//IETF//DTD HTML 2.0//", "-//IETF//DTD HTML 2.1E//", "-//IETF//DTD HTML 3.0//", "-//IETF//DTD HTML 3.2 Final//",
        "-//IETF//DTD HTML 3.2//", "-//IETF//DTD HTML 3//", "-//IETF//DTD HTML Level 0//", "-//IETF//DTD HTML Level 1//",
        "-//IETF//DTD HTML Level 2//", "-//IETF//DTD HTML Level 3//", "-//IETF//DTD HTML Strict Level 0//",
        "-//IETF//DTD HTML Strict Level 1//", "-//IETF//DTD HTML Strict Level 2//", "-//IETF//DTD HTML Strict Level 3//",
        "-//IETF//DTD HTML Strict//", "-//IETF//DTD HTML//", "-//Metrius//DTD Metrius Presentational//",
        "-//Microsoft//DTD Internet Explorer 2.0 HTML Strict//", "-//Microsoft//DTD Internet Explorer 2.0 HTML//",
        "-//Microsoft//DTD Internet Explorer 2.0 Tables//", "-//Microsoft//DTD Internet Explorer 3.0 HTML Strict//",
        "-//Microsoft//DTD Internet Explorer 3.0 HTML//", "-//Microsoft//DTD Internet Explorer 3.0 Tables//",
        "-//Netscape Comm. Corp.//DTD HTML//", "-//Netscape Comm. Corp.//DTD Strict HTML//", "-//O'Reilly and Associates//DTD HTML 2.0//",
        "-//O'Reilly and Associates//DTD HTML Extended 1.0//", "-//O'Reilly and Associates//DTD HTML Extended Relaxed 1.0//",
        "-//SQ//DTD HTML 2.0 HoTMetaL + extensions//", "-//SoftQuad Software//DTD HoTMetaL PRO 6.0::19990601::extensions to HTML 4.0//",
        "-//SoftQuad//DTD HoTMetaL PRO 4.0::19971010::extensions to HTML 4.0//", "-//Spyglass//DTD HTML 2.0 Extended//",
        "-//Sun Microsystems Corp.//DTD HotJava HTML//", "-//Sun Microsystems Corp.//DTD HotJava Strict HTML//",
        "-//W3C//DTD HTML 3 1995-03-24//", "-//W3C//DTD HTML 3.2 Draft//", "-//W3C//DTD HTML 3.2 Final//", "-//W3C//DTD HTML 3.2//",
        "-//W3C//DTD HTML 3.2S Draft//", "-//W3C//DTD HTML 4.0 Frameset//", "-//W3C//DTD HTML 4.0 Transitional//",
        "-//W3C//DTD HTML Experimental 19960712//", "-//W3C//DTD HTML Experimental 970421//", "-//W3C//DTD W3 HTML//",
        "-//W3O//DTD W3 HTML 3.0//", "-//WebTechs//DTD Mozilla HTML 2.0//", "-//WebTechs//DTD Mozilla HTML//"};
    for (size_t i = 0; i < CSCD_COUNT(prefixes) && t->hasPublic; ++i) if (startsWithI(pub, prefixes[i])) return true;
    if (!t->hasSystem && t->hasPublic &&
        (startsWithI(pub, "-//W3C//DTD HTML 4.01 Frameset//") || startsWithI(pub, "-//W3C//DTD HTML 4.01 Transitional//")))
        return true;
    return false;
}

/* ---- processing tokens -------------------------------------------------------------------------------------------- */

static void process(TreeBuilder* tb, Token* t);
static void processIn(TreeBuilder* tb, Mode mode, Token* t);

static void genericText(TreeBuilder* tb, Token* t, TzState state) {
    insertElement(tb, t, CSCD_NS_HTML);
    tb->tz->state = state;
    tb->original = tb->mode;
    tb->mode = M_TEXT;
}

static bool isAllSpace(CscdSv s) {
    for (size_t i = 0; i < s.len; ++i) if (!isWs(s.ptr[i])) return false;
    return true;
}

static void stopParsing(TreeBuilder* tb) {
    tb->stack.len = 0;
    tb->done = true;
}

/* a token made of part of another's characters */
static Token charsToken(CscdSv s) {
    Token t;
    memset(&t, 0, sizeof t);
    t.kind = TOK_CHARS;
    cscdStrSet(&t.data, s);
    return t;
}

static void endTagAny(TreeBuilder* tb, CscdSv name) {
    for (size_t i = tb->stack.len; i-- > 0;) {
        CscdElement* node = (CscdElement*)tb->stack.items[i];
        if (node->ns_ == CSCD_NS_HTML && cscdSvEq(cscdStrSv(&node->tag), name)) {
            char tag[64];
            snprintf(tag, sizeof tag, "%.*s", (int)name.len, name.ptr);
            impliedEndTags(tb, tag, false);
            while (tb->stack.len > i) tb->stack.len = i;
            return;
        }
        if (isSpecial(node)) return;
    }
}

static void inBody(TreeBuilder* tb, Token* t);

/* in head's rules */
static void inHead(TreeBuilder* tb, Token* t) {
    const CscdSv name = cscdStrSv(&t->name);
    if (t->kind == TOK_CHARS) {
        size_t ws = 0;
        while (ws < t->data.len && isWs(t->data.ptr[ws])) ++ws;
        insertChars(tb, cscdSvSub(cscdStrSv(&t->data), 0, ws));
        if (ws == t->data.len) return;
        Token rest = charsToken(cscdSvFrom(cscdStrSv(&t->data), ws));
        (void)cscdVecPop(&tb->stack); /* (the head) */
        tb->mode = M_AFTER_HEAD;
        process(tb, &rest);
        tokenFree(&rest);
        return;
    }
    if (t->kind == TOK_COMMENT) { insertComment(tb, cscdStrSv(&t->data), NULL); return; }
    if (t->kind == TOK_DOCTYPE) return;
    if (t->kind == TOK_START) {
        if (cscdSvEqC(name, "html")) { inBody(tb, t); return; }
        static const char* const voidHead[] = {"base", "basefont", "bgsound", "link", "meta"};
        if (TAG_IN(name, voidHead)) {
            insertElement(tb, t, CSCD_NS_HTML);
            (void)cscdVecPop(&tb->stack);
            return;
        }
        if (cscdSvEqC(name, "title")) { genericText(tb, t, TZ_RCDATA); return; }
        if (cscdSvEqC(name, "noscript")) {
            insertElement(tb, t, CSCD_NS_HTML);
            tb->mode = M_IN_HEAD_NOSCRIPT;
            return;
        }
        if (cscdSvEqC(name, "noframes") || cscdSvEqC(name, "style")) { genericText(tb, t, TZ_RAWTEXT); return; }
        if (cscdSvEqC(name, "script")) { genericText(tb, t, TZ_SCRIPT); return; }
        if (cscdSvEqC(name, "template")) {
            insertElement(tb, t, CSCD_NS_HTML);
            pushMarker(tb);
            tb->framesetOk = false;
            tb->mode = M_IN_TEMPLATE;
            cscdVecPush(&tb->templateModes, M_IN_TEMPLATE);
            return;
        }
        if (cscdSvEqC(name, "head")) return;
    } else if (t->kind == TOK_END) {
        if (cscdSvEqC(name, "head")) {
            (void)cscdVecPop(&tb->stack);
            tb->mode = M_AFTER_HEAD;
            return;
        }
        if (cscdSvEqC(name, "template")) {
            if (!templateOnStack(tb)) return;
            impliedEndTags(tb, NULL, true);
            popUntilTag(tb, "template");
            clearToMarker(tb);
            if (tb->templateModes.len) --tb->templateModes.len;
            resetMode(tb);
            return;
        }
        if (!(cscdSvEqC(name, "body") || cscdSvEqC(name, "html") || cscdSvEqC(name, "br"))) return;
    }
    (void)cscdVecPop(&tb->stack);
    tb->mode = M_AFTER_HEAD;
    process(tb, t);
}

/* "close the cell" */
static void closeCell(TreeBuilder* tb) {
    impliedEndTags(tb, NULL, false);
    static const char* const cells[] = {"td", "th"};
    popUntilOneOf(tb, cells, 2);
    clearToMarker(tb);
    tb->mode = M_IN_ROW;
}

static void clearBackTo(TreeBuilder* tb, const char* const* tags, size_t n) {
    while (tb->stack.len && !isHtmlIn(current(tb), tags, n) && !isHtml(current(tb), "template") && !isHtml(current(tb), "html")) (void)cscdVecPop(&tb->stack);
}
static void clearToTable(TreeBuilder* tb) { static const char* const t[] = {"table"}; clearBackTo(tb, t, 1); }
static void clearToTableBody(TreeBuilder* tb) { static const char* const t[] = {"tbody", "tfoot", "thead"}; clearBackTo(tb, t, 3); }
static void clearToRow(TreeBuilder* tb) { static const char* const t[] = {"tr"}; clearBackTo(tb, t, 1); }

static void inBody(TreeBuilder* tb, Token* t) {
    const CscdSv name = cscdStrSv(&t->name);
    switch (t->kind) {
    case TOK_CHARS: {
        /* (NULs dropped; the rest in runs) */
        CscdStr text = {0};
        bool nonSpace = false;
        for (size_t i = 0; i < t->data.len; ++i) {
            const char c = t->data.ptr[i];
            if (c == 0) continue;
            if (!isWs(c)) nonSpace = true;
            cscdStrAppendChar(&text, c);
        }
        if (text.len) {
            reconstructFormatting(tb);
            insertChars(tb, cscdStrSv(&text));
            if (nonSpace) tb->framesetOk = false;
        }
        cscdStrFree(&text);
        return;
    }
    case TOK_COMMENT: insertComment(tb, cscdStrSv(&t->data), NULL); return;
    case TOK_DOCTYPE: return;
    case TOK_EOF:
        if (tb->templateModes.len) { processIn(tb, M_IN_TEMPLATE, t); return; }
        stopParsing(tb);
        return;
    case TOK_START: {
        if (cscdSvEqC(name, "html")) {
            if (templateOnStack(tb) || !tb->stack.len) return;
            addMissingAttributes((CscdElement*)tb->stack.items[0], t);
            return;
        }
        static const char* const headish[] = {"base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title"};
        if (TAG_IN(name, headish)) { inHead(tb, t); return; }
        if (cscdSvEqC(name, "body")) {
            if (tb->stack.len < 2 || !isHtml((CscdElement*)tb->stack.items[1], "body") || templateOnStack(tb)) return;
            tb->framesetOk = false;
            addMissingAttributes((CscdElement*)tb->stack.items[1], t);
            return;
        }
        if (cscdSvEqC(name, "frameset")) {
            if (tb->stack.len < 2 || !isHtml((CscdElement*)tb->stack.items[1], "body") || !tb->framesetOk) return;
            CscdElement* body = (CscdElement*)tb->stack.items[1];
            if (body->node.parent) {
                CscdNode* gone = cscdNodeRemoveChild(body->node.parent, &body->node);
                if (gone) cscdNodeDestroy(gone);
            }
            tb->stack.len = 1;
            insertElement(tb, t, CSCD_NS_HTML);
            tb->mode = M_IN_FRAMESET;
            return;
        }
        static const char* const blocks[] = {"address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div", "dl",
                                             "fieldset", "figcaption", "figure", "footer", "header", "hgroup", "main", "menu", "nav", "ol",
                                             "p", "search", "section", "summary", "ul"};
        if (TAG_IN(name, blocks)) {
            closePInButtonScope(tb);
            insertElement(tb, t, CSCD_NS_HTML);
            return;
        }
        static const char* const headings[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
        if (TAG_IN(name, headings)) {
            closePInButtonScope(tb);
            if (HTML_IN(current(tb), headings)) (void)cscdVecPop(&tb->stack);
            insertElement(tb, t, CSCD_NS_HTML);
            return;
        }
        if (cscdSvEqC(name, "pre") || cscdSvEqC(name, "listing")) {
            closePInButtonScope(tb);
            insertElement(tb, t, CSCD_NS_HTML);
            tb->ignoreLf = true;
            tb->framesetOk = false;
            return;
        }
        if (cscdSvEqC(name, "form")) {
            if (tb->form && !templateOnStack(tb)) return;
            closePInButtonScope(tb);
            CscdElement* e = insertElement(tb, t, CSCD_NS_HTML);
            if (!templateOnStack(tb)) tb->form = e;
            return;
        }
        if (cscdSvEqC(name, "li") || cscdSvEqC(name, "dd") || cscdSvEqC(name, "dt")) {
            tb->framesetOk = false;
            const bool li = cscdSvEqC(name, "li");
            for (size_t i = tb->stack.len; i-- > 0;) {
                CscdElement* node = (CscdElement*)tb->stack.items[i];
                if (li ? isHtml(node, "li") : (isHtml(node, "dd") || isHtml(node, "dt"))) {
                    const char* tag = li ? "li" : (isHtml(node, "dd") ? "dd" : "dt");
                    impliedEndTags(tb, tag, false);
                    popUntilTag(tb, tag);
                    break;
                }
                if (isSpecial(node) && !isHtml(node, "address") && !isHtml(node, "div") && !isHtml(node, "p")) break;
            }
            closePInButtonScope(tb);
            insertElement(tb, t, CSCD_NS_HTML);
            return;
        }
        if (cscdSvEqC(name, "plaintext")) {
            closePInButtonScope(tb);
            insertElement(tb, t, CSCD_NS_HTML);
            tb->tz->state = TZ_PLAINTEXT;
            return;
        }
        if (cscdSvEqC(name, "button")) {
            if (inScope(tb, "button", S_DEFAULT)) {
                impliedEndTags(tb, NULL, false);
                popUntilTag(tb, "button");
            }
            reconstructFormatting(tb);
            insertElement(tb, t, CSCD_NS_HTML);
            tb->framesetOk = false;
            return;
        }
        if (cscdSvEqC(name, "a")) {
            for (size_t i = tb->formatting.len; i-- > 0;) {
                CscdElement* f = (CscdElement*)tb->formatting.items[i];
                if (!f) break;
                if (isHtml(f, "a")) {
                    if (!adoptionAgency(tb, CSCD_SV("a"))) endTagAny(tb, CSCD_SV("a"));
                    const long at = formattingIndex(tb, f);
                    if (at >= 0) cscdVecRemove(&tb->formatting, (size_t)at);
                    removeFromStack(tb, f);
                    break;
                }
            }
            reconstructFormatting(tb);
            pushFormatting(tb, insertElement(tb, t, CSCD_NS_HTML));
            return;
        }
        static const char* const fmt[] = {"b", "big", "code", "em", "font", "i", "s", "small", "strike", "strong", "tt", "u"};
        if (TAG_IN(name, fmt)) {
            reconstructFormatting(tb);
            pushFormatting(tb, insertElement(tb, t, CSCD_NS_HTML));
            return;
        }
        if (cscdSvEqC(name, "nobr")) {
            reconstructFormatting(tb);
            if (inScope(tb, "nobr", S_DEFAULT)) {
                if (!adoptionAgency(tb, CSCD_SV("nobr"))) endTagAny(tb, CSCD_SV("nobr"));
                reconstructFormatting(tb);
            }
            pushFormatting(tb, insertElement(tb, t, CSCD_NS_HTML));
            return;
        }
        if (cscdSvEqC(name, "applet") || cscdSvEqC(name, "marquee") || cscdSvEqC(name, "object")) {
            reconstructFormatting(tb);
            insertElement(tb, t, CSCD_NS_HTML);
            pushMarker(tb);
            tb->framesetOk = false;
            return;
        }
        if (cscdSvEqC(name, "table")) {
            if (!tb->quirks) closePInButtonScope(tb);
            insertElement(tb, t, CSCD_NS_HTML);
            tb->framesetOk = false;
            tb->mode = M_IN_TABLE;
            return;
        }
        static const char* const voids[] = {"area", "br", "embed", "img", "keygen", "wbr"};
        if (TAG_IN(name, voids)) {
            reconstructFormatting(tb);
            insertElement(tb, t, CSCD_NS_HTML);
            (void)cscdVecPop(&tb->stack);
            tb->framesetOk = false;
            return;
        }
        if (cscdSvEqC(name, "input")) {
            if (tb->fragment && isHtml(tb->context, "select")) return;
            if (inScope(tb, "select", S_DEFAULT)) popUntilTag(tb, "select");
            reconstructFormatting(tb);
            bool isHidden = false;
            for (size_t i = 0; i < t->attrs.len; ++i)
                if (cscdStrEqC(&t->attrs.items[i].name, "type") && cscdSvIEqC(cscdStrSv(&t->attrs.items[i].value), "hidden")) isHidden = true;
            insertElement(tb, t, CSCD_NS_HTML);
            (void)cscdVecPop(&tb->stack);
            if (!isHidden) tb->framesetOk = false;
            return;
        }
        if (cscdSvEqC(name, "param") || cscdSvEqC(name, "source") || cscdSvEqC(name, "track")) {
            insertElement(tb, t, CSCD_NS_HTML);
            (void)cscdVecPop(&tb->stack);
            return;
        }
        if (cscdSvEqC(name, "hr")) {
            closePInButtonScope(tb);
            if (inScope(tb, "select", S_DEFAULT)) impliedEndTags(tb, NULL, false);
            insertElement(tb, t, CSCD_NS_HTML);
            (void)cscdVecPop(&tb->stack);
            tb->framesetOk = false;
            return;
        }
        if (cscdSvEqC(name, "image")) {
            cscdStrSetC(&t->name, "img");
            inBody(tb, t);
            return;
        }
        if (cscdSvEqC(name, "textarea")) {
            insertElement(tb, t, CSCD_NS_HTML);
            tb->ignoreLf = true;
            tb->tz->state = TZ_RCDATA;
            tb->original = tb->mode;
            tb->framesetOk = false;
            tb->mode = M_TEXT;
            return;
        }
        if (cscdSvEqC(name, "xmp")) {
            closePInButtonScope(tb);
            reconstructFormatting(tb);
            tb->framesetOk = false;
            genericText(tb, t, TZ_RAWTEXT);
            return;
        }
        if (cscdSvEqC(name, "iframe")) {
            tb->framesetOk = false;
            genericText(tb, t, TZ_RAWTEXT);
            return;
        }
        if (cscdSvEqC(name, "noembed")) {
            genericText(tb, t, TZ_RAWTEXT);
            return;
        }
        if (cscdSvEqC(name, "select")) {
            /* (a select in a select ends it; in a select's fragment, nothing) */
            if (tb->fragment && isHtml(tb->context, "select")) return;
            if (inScope(tb, "select", S_DEFAULT)) {
                popUntilTag(tb, "select");
                return;
            }
            reconstructFormatting(tb);
            insertElement(tb, t, CSCD_NS_HTML);
            tb->framesetOk = false;
            return;
        }
        if (cscdSvEqC(name, "optgroup") || cscdSvEqC(name, "option")) {
            if (inScope(tb, "select", S_DEFAULT)) impliedEndTags(tb, cscdSvEqC(name, "option") ? "optgroup" : NULL, false);
            else if (isHtml(current(tb), "option")) (void)cscdVecPop(&tb->stack);
            reconstructFormatting(tb);
            insertElement(tb, t, CSCD_NS_HTML);
            return;
        }
        if (cscdSvEqC(name, "rb") || cscdSvEqC(name, "rtc")) {
            if (inScope(tb, "ruby", S_DEFAULT)) impliedEndTags(tb, NULL, false);
            insertElement(tb, t, CSCD_NS_HTML);
            return;
        }
        if (cscdSvEqC(name, "rp") || cscdSvEqC(name, "rt")) {
            if (inScope(tb, "ruby", S_DEFAULT)) impliedEndTags(tb, "rtc", false);
            insertElement(tb, t, CSCD_NS_HTML);
            return;
        }
        if (cscdSvEqC(name, "math") || cscdSvEqC(name, "svg")) {
            reconstructFormatting(tb);
            const bool math = cscdSvEqC(name, "math");
            if (math) adjustMathmlAttributes(t);
            else adjustSvgAttributes(t);
            insertElement(tb, t, math ? CSCD_NS_MATHML : CSCD_NS_SVG);
            if (t->selfClosing) (void)cscdVecPop(&tb->stack);
            return;
        }
        static const char* const ignored[] = {"caption", "col", "colgroup", "frame", "head", "tbody", "td", "tfoot", "th", "thead", "tr"};
        if (TAG_IN(name, ignored)) return;
        reconstructFormatting(tb);
        insertElement(tb, t, CSCD_NS_HTML);
        return;
    }
    case TOK_END: {
        if (cscdSvEqC(name, "template")) { inHead(tb, t); return; }
        if (cscdSvEqC(name, "body") || cscdSvEqC(name, "html")) {
            if (!inScope(tb, "body", S_DEFAULT)) return;
            tb->mode = M_AFTER_BODY;
            if (cscdSvEqC(name, "html")) process(tb, t);
            return;
        }
        static const char* const blocks[] = {"address", "article", "aside", "blockquote", "button", "center", "details", "dialog", "dir",
                                             "div", "dl", "fieldset", "figcaption", "figure", "footer", "header", "hgroup", "listing",
                                             "main", "menu", "nav", "ol", "pre", "search", "section", "select", "summary", "ul"};
        if (TAG_IN(name, blocks)) {
            char tag[32];
            snprintf(tag, sizeof tag, "%.*s", (int)name.len, name.ptr);
            if (!inScope(tb, tag, S_DEFAULT)) return;
            impliedEndTags(tb, NULL, false);
            popUntilTag(tb, tag);
            return;
        }
        if (cscdSvEqC(name, "form")) {
            if (!templateOnStack(tb)) {
                CscdElement* node = tb->form;
                tb->form = NULL;
                if (!node || !elementInScope(tb, node, S_DEFAULT)) return;
                impliedEndTags(tb, NULL, false);
                removeFromStack(tb, node);
            } else {
                if (!inScope(tb, "form", S_DEFAULT)) return;
                impliedEndTags(tb, NULL, false);
                popUntilTag(tb, "form");
            }
            return;
        }
        if (cscdSvEqC(name, "p")) {
            if (!inScope(tb, "p", S_BUTTON)) insertTag(tb, "p");
            closeP(tb);
            return;
        }
        if (cscdSvEqC(name, "li")) {
            if (!inScope(tb, "li", S_LIST)) return;
            impliedEndTags(tb, "li", false);
            popUntilTag(tb, "li");
            return;
        }
        if (cscdSvEqC(name, "dd") || cscdSvEqC(name, "dt")) {
            const char* tag = cscdSvEqC(name, "dd") ? "dd" : "dt";
            if (!inScope(tb, tag, S_DEFAULT)) return;
            impliedEndTags(tb, tag, false);
            popUntilTag(tb, tag);
            return;
        }
        static const char* const headings[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
        if (TAG_IN(name, headings)) {
            if (!headingInScope(tb)) return;
            impliedEndTags(tb, NULL, false);
            popUntilOneOf(tb, headings, 6);
            return;
        }
        if (TAG_IN(name, kFormatting)) {
            if (!adoptionAgency(tb, name)) endTagAny(tb, name);
            return;
        }
        if (cscdSvEqC(name, "applet") || cscdSvEqC(name, "marquee") || cscdSvEqC(name, "object")) {
            char tag[16];
            snprintf(tag, sizeof tag, "%.*s", (int)name.len, name.ptr);
            if (!inScope(tb, tag, S_DEFAULT)) return;
            impliedEndTags(tb, NULL, false);
            popUntilTag(tb, tag);
            clearToMarker(tb);
            return;
        }
        if (cscdSvEqC(name, "br")) {
            /* (as a <br> start tag without attributes) */
            Token br;
            memset(&br, 0, sizeof br);
            br.kind = TOK_START;
            br.name = cscdStrDupC("br");
            inBody(tb, &br);
            tokenFree(&br);
            return;
        }
        endTagAny(tb, name);
        return;
    }
    }
}

static void inTable(TreeBuilder* tb, Token* t) {
    const CscdSv name = cscdStrSv(&t->name);
    static const char* const textHolders[] = {"table", "tbody", "template", "tfoot", "thead", "tr"};
    if (t->kind == TOK_CHARS && HTML_IN(current(tb), textHolders)) {
        cscdStrClear(&tb->pendingTable);
        tb->pendingNonSpace = false;
        tb->original = tb->mode;
        tb->mode = M_IN_TABLE_TEXT;
        process(tb, t);
        return;
    }
    if (t->kind == TOK_COMMENT) { insertComment(tb, cscdStrSv(&t->data), NULL); return; }
    if (t->kind == TOK_DOCTYPE) return;
    if (t->kind == TOK_START) {
        if (cscdSvEqC(name, "caption")) {
            clearToTable(tb);
            pushMarker(tb);
            insertElement(tb, t, CSCD_NS_HTML);
            tb->mode = M_IN_CAPTION;
            return;
        }
        if (cscdSvEqC(name, "colgroup")) {
            clearToTable(tb);
            insertElement(tb, t, CSCD_NS_HTML);
            tb->mode = M_IN_COLUMN_GROUP;
            return;
        }
        if (cscdSvEqC(name, "col")) {
            clearToTable(tb);
            insertTag(tb, "colgroup");
            tb->mode = M_IN_COLUMN_GROUP;
            process(tb, t);
            return;
        }
        if (cscdSvEqC(name, "tbody") || cscdSvEqC(name, "tfoot") || cscdSvEqC(name, "thead")) {
            clearToTable(tb);
            insertElement(tb, t, CSCD_NS_HTML);
            tb->mode = M_IN_TABLE_BODY;
            return;
        }
        if (cscdSvEqC(name, "td") || cscdSvEqC(name, "th") || cscdSvEqC(name, "tr")) {
            clearToTable(tb);
            insertTag(tb, "tbody");
            tb->mode = M_IN_TABLE_BODY;
            process(tb, t);
            return;
        }
        if (cscdSvEqC(name, "table")) {
            if (!inScope(tb, "table", S_TABLE)) return;
            popUntilTag(tb, "table");
            resetMode(tb);
            process(tb, t);
            return;
        }
        if (cscdSvEqC(name, "style") || cscdSvEqC(name, "script") || cscdSvEqC(name, "template")) { inHead(tb, t); return; }
        if (cscdSvEqC(name, "input")) {
            bool hidden = false;
            for (size_t i = 0; i < t->attrs.len; ++i)
                if (cscdStrEqC(&t->attrs.items[i].name, "type") && cscdSvIEqC(cscdStrSv(&t->attrs.items[i].value), "hidden")) hidden = true;
            if (hidden) {
                insertElement(tb, t, CSCD_NS_HTML);
                (void)cscdVecPop(&tb->stack);
                return;
            }
        }
        if (cscdSvEqC(name, "form")) {
            if (templateOnStack(tb) || tb->form) return;
            tb->form = insertElement(tb, t, CSCD_NS_HTML);
            (void)cscdVecPop(&tb->stack);
            return;
        }
    } else if (t->kind == TOK_END) {
        if (cscdSvEqC(name, "table")) {
            if (!inScope(tb, "table", S_TABLE)) return;
            popUntilTag(tb, "table");
            resetMode(tb);
            return;
        }
        static const char* const ignored[] = {"body", "caption", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead", "tr"};
        if (TAG_IN(name, ignored)) return;
        if (cscdSvEqC(name, "template")) { inHead(tb, t); return; }
    } else if (t->kind == TOK_EOF) {
        inBody(tb, t);
        return;
    }
    /* anything else: as in body, foster parented */
    tb->fosterParenting = true;
    inBody(tb, t);
    tb->fosterParenting = false;
}

static void inTableText(TreeBuilder* tb, Token* t) {
    if (t->kind == TOK_CHARS) {
        for (size_t i = 0; i < t->data.len; ++i) {
            const char c = t->data.ptr[i];
            if (c == 0) continue;
            if (!isWs(c)) tb->pendingNonSpace = true;
            cscdStrAppendChar(&tb->pendingTable, c);
        }
        return;
    }
    if (tb->pendingTable.len) {
        Token pending = charsToken(cscdStrSv(&tb->pendingTable));
        cscdStrClear(&tb->pendingTable);
        if (tb->pendingNonSpace) {
            tb->fosterParenting = true;
            inBody(tb, &pending);
            tb->fosterParenting = false;
        } else {
            insertChars(tb, cscdStrSv(&pending.data));
        }
        tokenFree(&pending);
    }
    tb->mode = tb->original;
    process(tb, t);
}

static void inCaption(TreeBuilder* tb, Token* t) {
    const CscdSv name = cscdStrSv(&t->name);
    static const char* const closers[] = {"caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"};
    const bool endCaption = t->kind == TOK_END && cscdSvEqC(name, "caption");
    if (endCaption || (t->kind == TOK_START && TAG_IN(name, closers)) || (t->kind == TOK_END && cscdSvEqC(name, "table"))) {
        if (!inScope(tb, "caption", S_TABLE)) return;
        impliedEndTags(tb, NULL, false);
        popUntilTag(tb, "caption");
        clearToMarker(tb);
        tb->mode = M_IN_TABLE;
        if (!endCaption) process(tb, t);
        return;
    }
    static const char* const ignored[] = {"body", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead", "tr"};
    if (t->kind == TOK_END && TAG_IN(name, ignored)) return;
    inBody(tb, t);
}

static void inColumnGroup(TreeBuilder* tb, Token* t) {
    const CscdSv name = cscdStrSv(&t->name);
    if (t->kind == TOK_CHARS) {
        size_t ws = 0;
        while (ws < t->data.len && isWs(t->data.ptr[ws])) ++ws;
        insertChars(tb, cscdSvSub(cscdStrSv(&t->data), 0, ws));
        if (ws == t->data.len) return;
        Token rest = charsToken(cscdSvFrom(cscdStrSv(&t->data), ws));
        if (!isHtml(current(tb), "colgroup")) {
            tokenFree(&rest);
            return;
        }
        (void)cscdVecPop(&tb->stack);
        tb->mode = M_IN_TABLE;
        process(tb, &rest);
        tokenFree(&rest);
        return;
    }
    if (t->kind == TOK_COMMENT) { insertComment(tb, cscdStrSv(&t->data), NULL); return; }
    if (t->kind == TOK_DOCTYPE) return;
    if (t->kind == TOK_START && cscdSvEqC(name, "html")) { inBody(tb, t); return; }
    if (t->kind == TOK_START && cscdSvEqC(name, "col")) {
        insertElement(tb, t, CSCD_NS_HTML);
        (void)cscdVecPop(&tb->stack);
        return;
    }
    if (t->kind == TOK_END && cscdSvEqC(name, "colgroup")) {
        if (!isHtml(current(tb), "colgroup")) return;
        (void)cscdVecPop(&tb->stack);
        tb->mode = M_IN_TABLE;
        return;
    }
    if (t->kind == TOK_END && cscdSvEqC(name, "col")) return;
    if ((t->kind == TOK_START || t->kind == TOK_END) && cscdSvEqC(name, "template")) { inHead(tb, t); return; }
    if (t->kind == TOK_EOF) { inBody(tb, t); return; }
    if (!isHtml(current(tb), "colgroup")) return;
    (void)cscdVecPop(&tb->stack);
    tb->mode = M_IN_TABLE;
    process(tb, t);
}

static void inTableBody(TreeBuilder* tb, Token* t) {
    const CscdSv name = cscdStrSv(&t->name);
    if (t->kind == TOK_START && cscdSvEqC(name, "tr")) {
        clearToTableBody(tb);
        insertElement(tb, t, CSCD_NS_HTML);
        tb->mode = M_IN_ROW;
        return;
    }
    if (t->kind == TOK_START && (cscdSvEqC(name, "th") || cscdSvEqC(name, "td"))) {
        clearToTableBody(tb);
        insertTag(tb, "tr");
        tb->mode = M_IN_ROW;
        process(tb, t);
        return;
    }
    if (t->kind == TOK_END && (cscdSvEqC(name, "tbody") || cscdSvEqC(name, "tfoot") || cscdSvEqC(name, "thead"))) {
        char tag[8];
        snprintf(tag, sizeof tag, "%.*s", (int)name.len, name.ptr);
        if (!inScope(tb, tag, S_TABLE)) return;
        clearToTableBody(tb);
        (void)cscdVecPop(&tb->stack);
        tb->mode = M_IN_TABLE;
        return;
    }
    static const char* const closers[] = {"caption", "col", "colgroup", "tbody", "tfoot", "thead"};
    if ((t->kind == TOK_START && TAG_IN(name, closers)) || (t->kind == TOK_END && cscdSvEqC(name, "table"))) {
        if (!inScope(tb, "tbody", S_TABLE) && !inScope(tb, "thead", S_TABLE) && !inScope(tb, "tfoot", S_TABLE)) return;
        clearToTableBody(tb);
        (void)cscdVecPop(&tb->stack);
        tb->mode = M_IN_TABLE;
        process(tb, t);
        return;
    }
    static const char* const ignored[] = {"body", "caption", "col", "colgroup", "html", "td", "th", "tr"};
    if (t->kind == TOK_END && TAG_IN(name, ignored)) return;
    inTable(tb, t);
}

static void inRow(TreeBuilder* tb, Token* t) {
    const CscdSv name = cscdStrSv(&t->name);
    if (t->kind == TOK_START && (cscdSvEqC(name, "th") || cscdSvEqC(name, "td"))) {
        clearToRow(tb);
        insertElement(tb, t, CSCD_NS_HTML);
        tb->mode = M_IN_CELL;
        pushMarker(tb);
        return;
    }
    if (t->kind == TOK_END && cscdSvEqC(name, "tr")) {
        if (!inScope(tb, "tr", S_TABLE)) return;
        clearToRow(tb);
        (void)cscdVecPop(&tb->stack);
        tb->mode = M_IN_TABLE_BODY;
        return;
    }
    static const char* const closers[] = {"caption", "col", "colgroup", "tbody", "tfoot", "thead", "tr"};
    if ((t->kind == TOK_START && TAG_IN(name, closers)) || (t->kind == TOK_END && cscdSvEqC(name, "table"))) {
        if (!inScope(tb, "tr", S_TABLE)) return;
        clearToRow(tb);
        (void)cscdVecPop(&tb->stack);
        tb->mode = M_IN_TABLE_BODY;
        process(tb, t);
        return;
    }
    if (t->kind == TOK_END && (cscdSvEqC(name, "tbody") || cscdSvEqC(name, "tfoot") || cscdSvEqC(name, "thead"))) {
        char tag[8];
        snprintf(tag, sizeof tag, "%.*s", (int)name.len, name.ptr);
        if (!inScope(tb, tag, S_TABLE) || !inScope(tb, "tr", S_TABLE)) return;
        clearToRow(tb);
        (void)cscdVecPop(&tb->stack);
        tb->mode = M_IN_TABLE_BODY;
        process(tb, t);
        return;
    }
    static const char* const ignored[] = {"body", "caption", "col", "colgroup", "html", "td", "th"};
    if (t->kind == TOK_END && TAG_IN(name, ignored)) return;
    inTable(tb, t);
}

static void inCell(TreeBuilder* tb, Token* t) {
    const CscdSv name = cscdStrSv(&t->name);
    if (t->kind == TOK_END && (cscdSvEqC(name, "td") || cscdSvEqC(name, "th"))) {
        const char* tag = cscdSvEqC(name, "td") ? "td" : "th";
        if (!inScope(tb, tag, S_TABLE)) return;
        impliedEndTags(tb, NULL, false);
        popUntilTag(tb, tag);
        clearToMarker(tb);
        tb->mode = M_IN_ROW;
        return;
    }
    static const char* const closers[] = {"caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"};
    if (t->kind == TOK_START && TAG_IN(name, closers)) {
        if (!inScope(tb, "td", S_TABLE) && !inScope(tb, "th", S_TABLE)) return;
        closeCell(tb);
        process(tb, t);
        return;
    }
    static const char* const ignored[] = {"body", "caption", "col", "colgroup", "html"};
    if (t->kind == TOK_END && TAG_IN(name, ignored)) return;
    static const char* const tableEnds[] = {"table", "tbody", "tfoot", "thead", "tr"};
    if (t->kind == TOK_END && TAG_IN(name, tableEnds)) {
        char tag[8];
        snprintf(tag, sizeof tag, "%.*s", (int)name.len, name.ptr);
        if (!inScope(tb, tag, S_TABLE)) return;
        closeCell(tb);
        process(tb, t);
        return;
    }
    inBody(tb, t);
}

static void inTemplate(TreeBuilder* tb, Token* t) {
    const CscdSv name = cscdStrSv(&t->name);
    if (t->kind == TOK_CHARS || t->kind == TOK_COMMENT || t->kind == TOK_DOCTYPE) { inBody(tb, t); return; }
    static const char* const headish[] = {"base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title"};
    if ((t->kind == TOK_START && TAG_IN(name, headish)) || (t->kind == TOK_END && cscdSvEqC(name, "template"))) { inHead(tb, t); return; }
    if (t->kind == TOK_START) {
        Mode next = M_IN_BODY;
        if (cscdSvEqC(name, "caption") || cscdSvEqC(name, "colgroup") || cscdSvEqC(name, "tbody") || cscdSvEqC(name, "tfoot") || cscdSvEqC(name, "thead"))
            next = M_IN_TABLE;
        else if (cscdSvEqC(name, "col")) next = M_IN_COLUMN_GROUP;
        else if (cscdSvEqC(name, "tr")) next = M_IN_TABLE_BODY;
        else if (cscdSvEqC(name, "td") || cscdSvEqC(name, "th")) next = M_IN_ROW;
        if (tb->templateModes.len) --tb->templateModes.len;
        cscdVecPush(&tb->templateModes, next);
        tb->mode = next;
        process(tb, t);
        return;
    }
    if (t->kind == TOK_END) return;
    /* EOF */
    if (!templateOnStack(tb)) {
        stopParsing(tb);
        return;
    }
    popUntilTag(tb, "template");
    clearToMarker(tb);
    if (tb->templateModes.len) --tb->templateModes.len;
    resetMode(tb);
    process(tb, t);
}

/* characters split: the leading spaces, then the rest (for the modes that treat spaces apart) */
static bool splitSpaces(TreeBuilder* tb, Token* t, void (*spaces)(TreeBuilder*, CscdSv), Token* rest) {
    size_t ws = 0;
    while (ws < t->data.len && isWs(t->data.ptr[ws])) ++ws;
    if (ws) spaces(tb, cscdSvSub(cscdStrSv(&t->data), 0, ws));
    if (ws == t->data.len) return false;
    *rest = charsToken(cscdSvFrom(cscdStrSv(&t->data), ws));
    return true;
}

static void spacesInBody(TreeBuilder* tb, CscdSv s) {
    Token t = charsToken(s);
    inBody(tb, &t);
    tokenFree(&t);
}
static void spacesInserted(TreeBuilder* tb, CscdSv s) { insertChars(tb, s); }
static void spacesIgnored(TreeBuilder* tb, CscdSv s) { (void)tb; (void)s; }

static void processIn(TreeBuilder* tb, Mode mode, Token* t) {
    const CscdSv name = cscdStrSv(&t->name);
    Token rest;
    switch (mode) {
    case M_INITIAL:
        if (t->kind == TOK_CHARS) {
            if (!splitSpaces(tb, t, spacesIgnored, &rest)) return;
            tb->quirks = true;
            tb->mode = M_BEFORE_HTML;
            process(tb, &rest);
            tokenFree(&rest);
            return;
        }
        if (t->kind == TOK_COMMENT) { insertComment(tb, cscdStrSv(&t->data), tb->document); return; }
        if (t->kind == TOK_DOCTYPE) {
            tb->quirks = quirksDoctype(t);
            tb->mode = M_BEFORE_HTML;
            return;
        }
        tb->quirks = true;
        tb->mode = M_BEFORE_HTML;
        process(tb, t);
        return;
    case M_BEFORE_HTML:
        if (t->kind == TOK_DOCTYPE) return;
        if (t->kind == TOK_COMMENT) { insertComment(tb, cscdStrSv(&t->data), tb->document); return; }
        if (t->kind == TOK_CHARS) {
            if (!splitSpaces(tb, t, spacesIgnored, &rest)) return;
            insertTag(tb, "html");
            tb->mode = M_BEFORE_HEAD;
            process(tb, &rest);
            tokenFree(&rest);
            return;
        }
        if (t->kind == TOK_START && cscdSvEqC(name, "html")) {
            insertElement(tb, t, CSCD_NS_HTML);
            tb->mode = M_BEFORE_HEAD;
            return;
        }
        if (t->kind == TOK_END && !(cscdSvEqC(name, "head") || cscdSvEqC(name, "body") || cscdSvEqC(name, "html") || cscdSvEqC(name, "br"))) return;
        insertTag(tb, "html");
        tb->mode = M_BEFORE_HEAD;
        process(tb, t);
        return;
    case M_BEFORE_HEAD:
        if (t->kind == TOK_CHARS) {
            if (!splitSpaces(tb, t, spacesIgnored, &rest)) return;
            tb->head = insertTag(tb, "head");
            tb->mode = M_IN_HEAD;
            process(tb, &rest);
            tokenFree(&rest);
            return;
        }
        if (t->kind == TOK_COMMENT) { insertComment(tb, cscdStrSv(&t->data), NULL); return; }
        if (t->kind == TOK_DOCTYPE) return;
        if (t->kind == TOK_START && cscdSvEqC(name, "html")) { inBody(tb, t); return; }
        if (t->kind == TOK_START && cscdSvEqC(name, "head")) {
            tb->head = insertElement(tb, t, CSCD_NS_HTML);
            tb->mode = M_IN_HEAD;
            return;
        }
        if (t->kind == TOK_END && !(cscdSvEqC(name, "head") || cscdSvEqC(name, "body") || cscdSvEqC(name, "html") || cscdSvEqC(name, "br"))) return;
        tb->head = insertTag(tb, "head");
        tb->mode = M_IN_HEAD;
        process(tb, t);
        return;
    case M_IN_HEAD: inHead(tb, t); return;
    case M_IN_HEAD_NOSCRIPT:
        if (t->kind == TOK_DOCTYPE) return;
        if (t->kind == TOK_START && cscdSvEqC(name, "html")) { inBody(tb, t); return; }
        if (t->kind == TOK_END && cscdSvEqC(name, "noscript")) {
            (void)cscdVecPop(&tb->stack);
            tb->mode = M_IN_HEAD;
            return;
        }
        if (t->kind == TOK_CHARS) {
            if (!splitSpaces(tb, t, spacesInserted, &rest)) return;
            (void)cscdVecPop(&tb->stack);
            tb->mode = M_IN_HEAD;
            process(tb, &rest);
            tokenFree(&rest);
            return;
        }
        if (t->kind == TOK_COMMENT) { inHead(tb, t); return; }
        if (t->kind == TOK_START) {
            static const char* const toHead[] = {"basefont", "bgsound", "link", "meta", "noframes", "style"};
            if (TAG_IN(name, toHead)) { inHead(tb, t); return; }
            if (cscdSvEqC(name, "head") || cscdSvEqC(name, "noscript")) return;
        }
        if (t->kind == TOK_END && !cscdSvEqC(name, "br")) return;
        (void)cscdVecPop(&tb->stack);
        tb->mode = M_IN_HEAD;
        process(tb, t);
        return;
    case M_AFTER_HEAD:
        if (t->kind == TOK_CHARS) {
            if (!splitSpaces(tb, t, spacesInserted, &rest)) return;
            insertTag(tb, "body");
            tb->mode = M_IN_BODY;
            process(tb, &rest);
            tokenFree(&rest);
            return;
        }
        if (t->kind == TOK_COMMENT) { insertComment(tb, cscdStrSv(&t->data), NULL); return; }
        if (t->kind == TOK_DOCTYPE) return;
        if (t->kind == TOK_START) {
            if (cscdSvEqC(name, "html")) { inBody(tb, t); return; }
            if (cscdSvEqC(name, "body")) {
                insertElement(tb, t, CSCD_NS_HTML);
                tb->framesetOk = false;
                tb->mode = M_IN_BODY;
                return;
            }
            if (cscdSvEqC(name, "frameset")) {
                insertElement(tb, t, CSCD_NS_HTML);
                tb->mode = M_IN_FRAMESET;
                return;
            }
            static const char* const headish[] = {"base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title"};
            if (TAG_IN(name, headish)) {
                if (tb->head) cscdVecPush(&tb->stack, tb->head);
                CscdElement* head = tb->head;
                inHead(tb, t);
                if (head) removeFromStack(tb, head);
                return;
            }
            if (cscdSvEqC(name, "head")) return;
        }
        if (t->kind == TOK_END) {
            if (cscdSvEqC(name, "template")) { inHead(tb, t); return; }
            if (!(cscdSvEqC(name, "body") || cscdSvEqC(name, "html") || cscdSvEqC(name, "br"))) return;
        }
        insertTag(tb, "body");
        tb->mode = M_IN_BODY;
        process(tb, t);
        return;
    case M_IN_BODY: inBody(tb, t); return;
    case M_TEXT:
        if (t->kind == TOK_CHARS) { insertChars(tb, cscdStrSv(&t->data)); return; }
        if (t->kind == TOK_EOF) {
            (void)cscdVecPop(&tb->stack);
            tb->mode = tb->original;
            process(tb, t);
            return;
        }
        if (t->kind == TOK_END) {
            (void)cscdVecPop(&tb->stack);
            tb->mode = tb->original;
        }
        return;
    case M_IN_TABLE: inTable(tb, t); return;
    case M_IN_TABLE_TEXT: inTableText(tb, t); return;
    case M_IN_CAPTION: inCaption(tb, t); return;
    case M_IN_COLUMN_GROUP: inColumnGroup(tb, t); return;
    case M_IN_TABLE_BODY: inTableBody(tb, t); return;
    case M_IN_ROW: inRow(tb, t); return;
    case M_IN_CELL: inCell(tb, t); return;
    case M_IN_TEMPLATE: inTemplate(tb, t); return;
    case M_AFTER_BODY:
        if (t->kind == TOK_CHARS) {
            if (!splitSpaces(tb, t, spacesInBody, &rest)) return;
            tb->mode = M_IN_BODY;
            process(tb, &rest);
            tokenFree(&rest);
            return;
        }
        if (t->kind == TOK_COMMENT) { insertComment(tb, cscdStrSv(&t->data), tb->stack.len ? &((CscdElement*)tb->stack.items[0])->node : tb->document); return; }
        if (t->kind == TOK_DOCTYPE) return;
        if (t->kind == TOK_START && cscdSvEqC(name, "html")) { inBody(tb, t); return; }
        if (t->kind == TOK_END && cscdSvEqC(name, "html")) {
            if (!tb->fragment) tb->mode = M_AFTER_AFTER_BODY;
            return;
        }
        if (t->kind == TOK_EOF) { stopParsing(tb); return; }
        tb->mode = M_IN_BODY;
        process(tb, t);
        return;
    case M_IN_FRAMESET:
        if (t->kind == TOK_CHARS) {
            for (size_t i = 0; i < t->data.len; ++i)
                if (isWs(t->data.ptr[i])) insertChars(tb, cscdSvSub(cscdStrSv(&t->data), i, 1));
            return;
        }
        if (t->kind == TOK_COMMENT) { insertComment(tb, cscdStrSv(&t->data), NULL); return; }
        if (t->kind == TOK_DOCTYPE) return;
        if (t->kind == TOK_START) {
            if (cscdSvEqC(name, "html")) { inBody(tb, t); return; }
            if (cscdSvEqC(name, "frameset")) { insertElement(tb, t, CSCD_NS_HTML); return; }
            if (cscdSvEqC(name, "frame")) {
                insertElement(tb, t, CSCD_NS_HTML);
                (void)cscdVecPop(&tb->stack);
                return;
            }
            if (cscdSvEqC(name, "noframes")) { inHead(tb, t); return; }
            return;
        }
        if (t->kind == TOK_END && cscdSvEqC(name, "frameset")) {
            if (tb->stack.len == 1) return;
            (void)cscdVecPop(&tb->stack);
            if (!tb->fragment && !isHtml(current(tb), "frameset")) tb->mode = M_AFTER_FRAMESET;
            return;
        }
        if (t->kind == TOK_EOF) stopParsing(tb);
        return;
    case M_AFTER_FRAMESET:
        if (t->kind == TOK_CHARS) {
            for (size_t i = 0; i < t->data.len; ++i)
                if (isWs(t->data.ptr[i])) insertChars(tb, cscdSvSub(cscdStrSv(&t->data), i, 1));
            return;
        }
        if (t->kind == TOK_COMMENT) { insertComment(tb, cscdStrSv(&t->data), NULL); return; }
        if (t->kind == TOK_DOCTYPE) return;
        if (t->kind == TOK_START && cscdSvEqC(name, "html")) { inBody(tb, t); return; }
        if (t->kind == TOK_END && cscdSvEqC(name, "html")) { tb->mode = M_AFTER_AFTER_FRAMESET; return; }
        if (t->kind == TOK_START && cscdSvEqC(name, "noframes")) { inHead(tb, t); return; }
        if (t->kind == TOK_EOF) stopParsing(tb);
        return;
    case M_AFTER_AFTER_BODY:
        if (t->kind == TOK_COMMENT) { insertComment(tb, cscdStrSv(&t->data), tb->document); return; }
        if (t->kind == TOK_CHARS) {
            if (!splitSpaces(tb, t, spacesInBody, &rest)) return;
            tb->mode = M_IN_BODY;
            process(tb, &rest);
            tokenFree(&rest);
            return;
        }
        if (t->kind == TOK_DOCTYPE || (t->kind == TOK_START && cscdSvEqC(name, "html"))) { inBody(tb, t); return; }
        if (t->kind == TOK_EOF) { stopParsing(tb); return; }
        tb->mode = M_IN_BODY;
        process(tb, t);
        return;
    case M_AFTER_AFTER_FRAMESET:
        if (t->kind == TOK_COMMENT) { insertComment(tb, cscdStrSv(&t->data), tb->document); return; }
        if (t->kind == TOK_CHARS) {
            for (size_t i = 0; i < t->data.len; ++i)
                if (isWs(t->data.ptr[i])) {
                    Token one = charsToken(cscdSvSub(cscdStrSv(&t->data), i, 1));
                    inBody(tb, &one);
                    tokenFree(&one);
                }
            return;
        }
        if (t->kind == TOK_DOCTYPE || (t->kind == TOK_START && cscdSvEqC(name, "html"))) { inBody(tb, t); return; }
        if (t->kind == TOK_EOF) { stopParsing(tb); return; }
        if (t->kind == TOK_START && cscdSvEqC(name, "noframes")) { inHead(tb, t); return; }
        return;
    }
}

/* the rules for tokens in foreign content */
static void foreignContent(TreeBuilder* tb, Token* t) {
    const CscdSv name = cscdStrSv(&t->name);
    if (t->kind == TOK_CHARS) {
        CscdStr text = {0};
        bool nonSpace = false;
        for (size_t i = 0; i < t->data.len; ++i) {
            const char c = t->data.ptr[i];
            if (c == 0) cscdStrAppendC(&text, "\xEF\xBF\xBD");
            else {
                if (!isWs(c)) nonSpace = true;
                cscdStrAppendChar(&text, c);
            }
        }
        insertChars(tb, cscdStrSv(&text));
        if (nonSpace) tb->framesetOk = false;
        cscdStrFree(&text);
        return;
    }
    if (t->kind == TOK_COMMENT) { insertComment(tb, cscdStrSv(&t->data), NULL); return; }
    if (t->kind == TOK_DOCTYPE) return;
    static const char* const breakout[] = {"b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt", "em", "embed",
                                           "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li", "listing", "menu", "meta",
                                           "nobr", "ol", "p", "pre", "ruby", "s", "small", "span", "strong", "strike", "sub", "sup", "table",
                                           "tt", "u", "ul", "var"};
    bool fontBreakout = false;
    if (t->kind == TOK_START && cscdSvEqC(name, "font"))
        for (size_t i = 0; i < t->attrs.len; ++i)
            if (cscdStrEqC(&t->attrs.items[i].name, "color") || cscdStrEqC(&t->attrs.items[i].name, "face") || cscdStrEqC(&t->attrs.items[i].name, "size"))
                fontBreakout = true;
    if ((t->kind == TOK_START && (TAG_IN(name, breakout) || fontBreakout)) || (t->kind == TOK_END && (cscdSvEqC(name, "br") || cscdSvEqC(name, "p")))) {
        while (tb->stack.len) {
            CscdElement* c = current(tb);
            if (mathmlTextPoint(c) || htmlIntegrationPoint(c) || c->ns_ == CSCD_NS_HTML) break;
            (void)cscdVecPop(&tb->stack);
        }
        processIn(tb, tb->mode, t);
        return;
    }
    if (t->kind == TOK_START) {
        CscdElement* adjusted = adjustedCurrent(tb);
        const uint8_t ns = adjusted ? adjusted->ns_ : CSCD_NS_HTML;
        if (ns == CSCD_NS_MATHML) adjustMathmlAttributes(t);
        else if (ns == CSCD_NS_SVG) adjustSvgAttributes(t);
        insertElement(tb, t, ns);
        if (t->selfClosing) (void)cscdVecPop(&tb->stack);
        return;
    }
    if (t->kind == TOK_END) {
        for (size_t i = tb->stack.len; i-- > 0;) {
            CscdElement* node = (CscdElement*)tb->stack.items[i];
            if (i == 0) return;
            if (cscdSvIEq(cscdStrSv(&node->tag), name)) {
                tb->stack.len = i;
                return;
            }
            if (i > 0 && ((CscdElement*)tb->stack.items[i - 1])->ns_ == CSCD_NS_HTML) {
                processIn(tb, tb->mode, t);
                return;
            }
        }
    }
}

/* the tree construction dispatcher: foreign content's rules, or the insertion mode's */
static void process(TreeBuilder* tb, Token* t) {
    if (tb->done) return;
    if (t->kind == TOK_CHARS && tb->ignoreLf) {
        tb->ignoreLf = false;
        if (t->data.len && t->data.ptr[0] == '\n') {
            if (t->data.len == 1) return;
            Token rest = charsToken(cscdSvFrom(cscdStrSv(&t->data), 1));
            process(tb, &rest);
            tokenFree(&rest);
            return;
        }
    } else if (t->kind != TOK_CHARS) {
        tb->ignoreLf = false;
    }
    CscdElement* a = adjustedCurrent(tb);
    const bool html = !a || a->ns_ == CSCD_NS_HTML || t->kind == TOK_EOF ||
                      (mathmlTextPoint(a) && t->kind == TOK_START && !cscdStrEqC(&t->name, "mglyph") && !cscdStrEqC(&t->name, "malignmark")) ||
                      (mathmlTextPoint(a) && t->kind == TOK_CHARS) ||
                      (a->ns_ == CSCD_NS_MATHML && cscdStrEqC(&a->tag, "annotation-xml") && t->kind == TOK_START && cscdStrEqC(&t->name, "svg")) ||
                      (htmlIntegrationPoint(a) && (t->kind == TOK_START || t->kind == TOK_CHARS));
    if (html) processIn(tb, tb->mode, t);
    else foreignContent(tb, t);
}

/* ---- a select's selectedcontent --------------------------------------------------------------------------------- */

/* the first element named tag under n (HTML's), not inside another select */
static CscdElement* firstUnder(CscdNode* n, const char* tag, CscdElement** lastSelected) {
    for (size_t i = 0; i < n->children.len; ++i) {
        CscdElement* e = cscdNodeAsElement(cscdNodeChild(n, i));
        if (!e || isHtml(e, "select")) continue;
        if (isHtml(e, tag)) {
            if (!lastSelected) return e;
            if (cscdElementHasAttributeC(e, "selected") || !*lastSelected) *lastSelected = e;
            if (cscdElementHasAttributeC(e, "selected")) lastSelected[1] = e;
        }
        CscdElement* deeper = firstUnder(&e->node, tag, lastSelected);
        if (deeper && !lastSelected) return deeper;
    }
    return NULL;
}

/* Each select's selectedcontent (the first one in it) gets a copy of what its selected option holds (the last one
 * marked selected, or the first), as the specification has it updated while the option is parsed. */
static void fillSelectedContent(CscdNode* n) {
    for (size_t i = 0; i < n->children.len; ++i) fillSelectedContent(cscdNodeChild(n, i));
    CscdElement* select = cscdNodeAsElement(n);
    if (!select || !isHtml(select, "select")) return;
    CscdElement* into = firstUnder(n, "selectedcontent", NULL);
    if (!into) return;
    CscdElement* found[2] = {NULL, NULL}; /* (the first option, the last selected one) */
    firstUnder(n, "option", found);
    CscdElement* option = found[1] ? found[1] : found[0];
    while (into->node.children.len) {
        CscdNode* gone = cscdNodeRemoveChild(&into->node, cscdNodeChild(&into->node, 0));
        if (gone) cscdNodeDestroy(gone);
    }
    if (!option) return;
    CscdElement* copy = cscdElementClone(option, true);
    while (copy->node.children.len) {
        CscdNode* c = cscdNodeRemoveChild(&copy->node, cscdNodeChild(&copy->node, 0));
        if (c && !cscdNodeAppendChild(&into->node, c)) cscdNodeDestroy(c);
    }
    cscdNodeDestroy(&copy->node);
}

/* ---- the parser's entry points ------------------------------------------------------------------------------------ */

/* How deep the parser nests elements (as browsers do): deeper ones go into the element at this depth, side by side,
 * so that a hostile page can make neither the tree deeper than styling, layout and painting recurse safely nor the
 * open elements more than a scope search walks. */
#define kMaxParserDepth 512

/* the input with its line breaks made LF (CR LF and CR) */
static void preprocess(CscdSv html, CscdStr* out) {
    cscdStrReserve(out, html.len);
    for (size_t i = 0; i < html.len; ++i) {
        const char c = html.ptr[i];
        if (c == '\r') {
            cscdStrAppendChar(out, '\n');
            if (i + 1 < html.len && html.ptr[i + 1] == '\n') ++i;
        } else {
            cscdStrAppendChar(out, c);
        }
    }
}

static void run(TreeBuilder* tb, Tokenizer* tz) {
    tb->tz = tz;
    while (!tb->done) {
        CscdElement* a = adjustedCurrent(tb);
        tz->cdataAllowed = a && a->ns_ != CSCD_NS_HTML;
        Token t;
        nextToken(tz, &t);
        process(tb, &t);
        const bool eof = t.kind == TOK_EOF;
        tokenFree(&t);
        if (eof) break;
    }
    cscdVecFree(&tb->stack);
    cscdVecFree(&tb->formatting);
    cscdVecFree(&tb->templateModes);
    cscdStrFree(&tb->pendingTable);
    cscdStrFree(&tz->in);
    cscdStrFree(&tz->lastStart);
}

CscdDocument* cscdParseHTML(CscdSv html) {
    CscdDocument* doc = cscdDocumentNew();
    /* (a UTF-8 byte order mark) */
    if (html.len >= 3 && (unsigned char)html.ptr[0] == 0xEF && (unsigned char)html.ptr[1] == 0xBB && (unsigned char)html.ptr[2] == 0xBF)
        html = cscdSvFrom(html, 3);
    TreeBuilder tb;
    memset(&tb, 0, sizeof tb);
    tb.document = &doc->node;
    tb.maxDepth = kMaxParserDepth;
    tb.framesetOk = true;
    tb.mode = M_INITIAL;
    Tokenizer tz;
    memset(&tz, 0, sizeof tz);
    preprocess(html, &tz.in);
    run(&tb, &tz);
    fillSelectedContent(&doc->node);
    cscdDomAdoptTemplates(&doc->node);
    return doc;
}

/* ---- XHTML: the XML syntax -------------------------------------------------------------------------------------------
 * A page served as XML (.xht, .xhtml) is not parsed by HTML's rules: what it says is what it gets (<div/> is closed, a
 * <td> in a <div> stays there, no head or body is implied), its names keep their case, and an element's namespace is
 * its xmlns's. A page that is not well formed is taken as far as it goes (a browser would show an error instead):
 * an end tag that matches no open element is dropped, one that matches an outer one closes those inside it. */

typedef struct XmlScope { CscdElement* e; uint8_t ns; CscdStrMap prefixes; } XmlScope;

static uint8_t nsOf(CscdSv uri, uint8_t otherwise) {
    if (cscdSvEqC(uri, "http://www.w3.org/2000/svg")) return CSCD_NS_SVG;
    if (cscdSvEqC(uri, "http://www.w3.org/1998/Math/MathML")) return CSCD_NS_MATHML;
    if (cscdSvEqC(uri, "http://www.w3.org/1999/xhtml")) return CSCD_NS_HTML;
    return otherwise;
}

static bool xmlNameChar(char c) { return !isWs(c) && c != '/' && c != '>' && c != '=' && c != '<' && c != 0; }

/* text with its references decoded and, in an attribute's value, its white space made spaces */
static void xmlText(CscdSv s, bool attribute, CscdStr* out) {
    CscdStr decoded = {0};
    cscdDecodeEntities(s, &decoded);
    for (size_t i = 0; i < decoded.len; ++i) {
        const char c = decoded.ptr[i];
        cscdStrAppendChar(out, attribute && (c == '\n' || c == '\t') ? ' ' : c);
    }
    cscdStrFree(&decoded);
}

static void xmlAppend(CscdNode* parent, CscdNode* n) {
    if (!cscdNodeAppendChild(parent, n)) cscdNodeDestroy(n);
}

static void xmlChars(CscdNode* parent, CscdSv s) {
    if (!s.len) return;
    const size_t n = parent->children.len;
    CscdNode* last = n ? cscdNodeChild(parent, n - 1) : NULL;
    if (last && cscdNodeIsText(last)) cscdStrAppend(&((CscdText*)last)->data, s);
    else xmlAppend(parent, &cscdTextNew(s)->node);
}

CscdDocument* cscdParseXHTML(CscdSv xml) {
    CscdDocument* doc = cscdDocumentNew();
    if (xml.len >= 3 && (unsigned char)xml.ptr[0] == 0xEF && (unsigned char)xml.ptr[1] == 0xBB && (unsigned char)xml.ptr[2] == 0xBF)
        xml = cscdSvFrom(xml, 3);
    CscdStr in = {0};
    preprocess(xml, &in);
    const CscdSv s = cscdStrSv(&in);
    CSCD_VEC(XmlScope) opened = {0};
    CscdStr text = {0};
    size_t i = 0;
    while (i < s.len) {
        CscdNode* parent = opened.len ? &cscdVecLast(&opened).e->node : &doc->node;
        if (s.ptr[i] != '<') {
            const size_t lt = cscdSvFindChar(s, '<', i);
            const size_t end = lt == CSCD_NPOS ? s.len : lt;
            if (opened.len) {
                cscdStrClear(&text);
                xmlText(cscdSvSub(s, i, end - i), false, &text);
                xmlChars(parent, cscdStrSv(&text));
            }
            i = end;
            continue;
        }
        const CscdSv rest = cscdSvFrom(s, i);
        if (cscdSvStartsC(rest, "<!--")) {
            size_t e = cscdSvFind(s, CSCD_SV("-->"), i + 4);
            if (e == CSCD_NPOS) e = s.len;
            xmlAppend(parent, &cscdCommentNew(cscdSvSub(s, i + 4, e - (i + 4)))->node);
            i = e == s.len ? e : e + 3;
            continue;
        }
        if (cscdSvStartsC(rest, "<![CDATA[")) {
            size_t e = cscdSvFind(s, CSCD_SV("]]>"), i + 9);
            if (e == CSCD_NPOS) e = s.len;
            if (opened.len) xmlChars(parent, cscdSvSub(s, i + 9, e - (i + 9)));
            i = e == s.len ? e : e + 3;
            continue;
        }
        if (cscdSvStartsC(rest, "<?")) { /* (the XML declaration, processing instructions: nothing cascade does) */
            const size_t e = cscdSvFind(s, CSCD_SV("?>"), i + 2);
            i = e == CSCD_NPOS ? s.len : e + 2;
            continue;
        }
        if (cscdSvStartsC(rest, "<!")) { /* (the doctype, with its internal subset if it has one) */
            int depth = 0;
            for (i += 2; i < s.len; ++i) {
                if (s.ptr[i] == '[') ++depth;
                else if (s.ptr[i] == ']') --depth;
                else if (s.ptr[i] == '>' && depth <= 0) break;
            }
            if (i < s.len) ++i;
            continue;
        }
        if (cscdSvStartsC(rest, "</")) {
            size_t k = i + 2;
            while (k < s.len && xmlNameChar(s.ptr[k])) ++k;
            CscdSv name = cscdSvSub(s, i + 2, k - (i + 2));
            const size_t colon = cscdSvFindChar(name, ':', 0);
            if (colon != CSCD_NPOS) name = cscdSvFrom(name, colon + 1);
            const size_t gt = cscdSvFindChar(s, '>', k);
            i = gt == CSCD_NPOS ? s.len : gt + 1;
            for (size_t o = opened.len; o-- > 0;)
                if (cscdSvIEq(cscdStrSv(&opened.items[o].e->tag), name)) {
                    while (opened.len > o) {
                        cscdStrMapFree(&cscdVecLast(&opened).prefixes);
                        --opened.len;
                    }
                    break;
                }
            continue;
        }
        if (i + 1 >= s.len || !xmlNameChar(s.ptr[i + 1])) { /* (a '<' that starts nothing) */
            if (opened.len) xmlChars(parent, CSCD_SV("<"));
            ++i;
            continue;
        }
        /* a start tag: its name, its attributes (xmlns ones setting namespaces), '>' or "/>" */
        size_t k = i + 1;
        while (k < s.len && xmlNameChar(s.ptr[k])) ++k;
        const CscdSv qname = cscdSvSub(s, i + 1, k - (i + 1));
        CscdStrPairVec attrs = {0};
        bool selfClosing = false;
        while (k < s.len) {
            while (k < s.len && isWs(s.ptr[k])) ++k;
            if (k >= s.len) break;
            if (s.ptr[k] == '>') {
                ++k;
                break;
            }
            if (s.ptr[k] == '/') {
                ++k;
                if (k < s.len && s.ptr[k] == '>') {
                    selfClosing = true;
                    ++k;
                    break;
                }
                continue;
            }
            const size_t nameAt = k;
            while (k < s.len && xmlNameChar(s.ptr[k])) ++k;
            if (k == nameAt) {
                ++k;
                continue;
            }
            CscdStrPair p = {cscdStrDup(cscdSvSub(s, nameAt, k - nameAt)), {0}};
            while (k < s.len && isWs(s.ptr[k])) ++k;
            if (k < s.len && s.ptr[k] == '=') {
                ++k;
                while (k < s.len && isWs(s.ptr[k])) ++k;
                if (k < s.len && (s.ptr[k] == '"' || s.ptr[k] == '\'')) {
                    const char q = s.ptr[k++];
                    size_t e = cscdSvFindChar(s, q, k);
                    if (e == CSCD_NPOS) e = s.len;
                    xmlText(cscdSvSub(s, k, e - k), true, &p.value);
                    k = e < s.len ? e + 1 : e;
                } else {
                    const size_t v = k;
                    while (k < s.len && !isWs(s.ptr[k]) && s.ptr[k] != '>') ++k;
                    xmlText(cscdSvSub(s, v, k - v), true, &p.value);
                }
            }
            bool seen = false; /* (a name twice: the first stays) */
            for (size_t a = 0; a < attrs.len && !seen; ++a) seen = cscdStrEq(&attrs.items[a].name, &p.name);
            if (seen) {
                cscdStrFree(&p.name);
                cscdStrFree(&p.value);
            } else {
                cscdVecPush(&attrs, p);
            }
        }
        i = k;
        /* the namespace: the prefix's, or the default one (xmlns), or the parent's */
        XmlScope scope = {NULL, opened.len ? cscdVecLast(&opened).ns : CSCD_NS_HTML, {0}};
        for (size_t a = 0; a < attrs.len; ++a) {
            const CscdSv an = cscdStrSv(&attrs.items[a].name);
            if (cscdSvEqC(an, "xmlns")) scope.ns = nsOf(cscdStrSv(&attrs.items[a].value), CSCD_NS_HTML);
            else if (cscdSvStartsC(an, "xmlns:")) cscdStrMapPut(&scope.prefixes, cscdSvFrom(an, 6), NULL)->i = nsOf(cscdStrSv(&attrs.items[a].value), CSCD_NS_HTML);
        }
        CscdSv local = qname;
        const size_t colon = cscdSvFindChar(qname, ':', 0);
        if (colon != CSCD_NPOS) {
            const CscdSv prefix = cscdSvSub(qname, 0, colon);
            local = cscdSvFrom(qname, colon + 1);
            const CscdMapVal* v = cscdStrMapGet(&scope.prefixes, prefix);
            for (size_t o = opened.len; !v && o-- > 0;) v = cscdStrMapGet(&opened.items[o].prefixes, prefix);
            if (v) scope.ns = (uint8_t)v->i;
        }
        /* (cascade's tag names are lowercase, as HTML's parser gives them; attributes keep their case) */
        Token t;
        memset(&t, 0, sizeof t);
        for (size_t c = 0; c < local.len; ++c) cscdStrAppendChar(&t.name, cscdToLower(local.ptr[c]));
        t.attrs = attrs;
        CscdElement* e = createElement(&t, scope.ns);
        tokenFree(&t);
        xmlAppend(parent, &e->node);
        if (selfClosing || opened.len >= kMaxParserDepth || cscdNodeDepth(&e->node) >= CSCD_MAX_TREE_DEPTH) {
            cscdStrMapFree(&scope.prefixes);
            continue;
        }
        scope.e = e;
        cscdVecPush(&opened, scope);
        /* (a script's or style's text is as it is: no markup in it is markup, as XHTML writes it in CDATA) */
    }
    for (size_t o = 0; o < opened.len; ++o) cscdStrMapFree(&opened.items[o].prefixes);
    cscdVecFree(&opened);
    cscdStrFree(&text);
    cscdStrFree(&in);
    fillSelectedContent(&doc->node);
    cscdDomAdoptTemplates(&doc->node);
    return doc;
}

void cscdParseHTMLFragment(CscdSv html, CscdElement* parent) {
    /* (what the fragment opens stays within CSCD_MAX_TREE_DEPTH: its elements go under parent, at its depth) */
    const size_t depth = cscdNodeDepth(&parent->node);
    if (depth >= CSCD_MAX_TREE_DEPTH) return;
    TreeBuilder tb;
    memset(&tb, 0, sizeof tb);
    /* the fragment case: an html element of its own holds what is parsed, in the context of parent */
    CscdElement* root = cscdElementNew(CSCD_SV("html"));
    tb.document = &root->node;
    tb.fragment = true;
    tb.context = parent;
    tb.framesetOk = true;
    tb.maxDepth = (CSCD_MAX_TREE_DEPTH - depth < kMaxParserDepth ? CSCD_MAX_TREE_DEPTH - depth : kMaxParserDepth) + 1;
    Tokenizer tz;
    memset(&tz, 0, sizeof tz);
    preprocess(html, &tz.in);
    if (parent->ns_ == CSCD_NS_HTML) {
        static const char* const raw[] = {"style", "xmp", "iframe", "noembed", "noframes"};
        if (cscdElementIs(parent, "title") || cscdElementIs(parent, "textarea")) tz.state = TZ_RCDATA;
        else if (tagIn(cscdStrSv(&parent->tag), raw, CSCD_COUNT(raw))) tz.state = TZ_RAWTEXT;
        else if (cscdElementIs(parent, "script")) tz.state = TZ_SCRIPT;
        else if (cscdElementIs(parent, "plaintext")) tz.state = TZ_PLAINTEXT;
        /* (no end tag is "appropriate" yet: the tokenizer has made no start tag) */
    }
    cscdVecPush(&tb.stack, root);
    if (isHtml(parent, "template")) cscdVecPush(&tb.templateModes, M_IN_TEMPLATE);
    resetMode(&tb);
    /* (the form element pointer: the nearest form the context is in) */
    for (CscdElement* e = parent; e; e = cscdNodeParentElement(&e->node))
        if (isHtml(e, "form")) {
            tb.form = e;
            break;
        }
    run(&tb, &tz);
    while (root->node.children.len) {
        CscdNode* c = cscdNodeRemoveChild(&root->node, cscdNodeChild(&root->node, 0));
        if (c && !cscdNodeAppendChild(&parent->node, c)) cscdNodeDestroy(c);
    }
    cscdNodeDestroy(&root->node);
    fillSelectedContent(&parent->node);
    cscdDomAdoptTemplates(&parent->node);
}
