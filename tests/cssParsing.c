/* cascade's CSS parsing, written as the css-parsing-tests write theirs (tools/cssParsingTests.py compares).
 *
 *     cascade-css-parsing < cases
 *
 * Each line of the input is a mode and a text as hex bytes of UTF-8 ("values 726564"); each line out is the result as
 * JSON:
 * - values: the component values of the text (whitespace kept), as css-parsing-tests' component_value_list
 * - one: the one component value of the text (whitespace around it left out), or ["error", "invalid"]
 * - decls: the declarations of a list of them (["declaration", name, [values], important])
 * - color: the color as [r, g, b, a] (0..255), or null
 * - anb: :nth-child()'s [A, B] for the text, or null
 * A number's representation is not kept by the tokenizer: it is written as "" (the runner leaves it out). */
#include "cascade/cascade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void jsonString(CscdStr* out, CscdSv s) {
    cscdStrAppendC(out, "\"");
    for (size_t i = 0; i < s.len; ++i) {
        const unsigned char c = (unsigned char)s.ptr[i];
        if (c == '"' || c == '\\') cscdStrAppendf(out, "\\%c", c);
        else if (c < 0x20) cscdStrAppendf(out, "\\u%04x", c);
        else cscdStrAppend(out, (CscdSv){(const char*)&s.ptr[i], 1});
    }
    cscdStrAppendC(out, "\"");
}

static void jsonNumber(CscdStr* out, double v) {
    if (v == (double)(long long)v && v > -1e15 && v < 1e15) cscdStrAppendf(out, "%lld", (long long)v);
    else cscdStrAppendf(out, "%.17g", v);
}

static void writeValues(CscdStr* out, const CscdCssValue* v, size_t n);

static void writeValue(CscdStr* out, const CscdCssValue* v) {
    const CscdCssToken* t = &v->tok;
    const CscdSv text = cscdStrSv(&t->text);
    switch (t->type) {
    case CSCD_TOK_IDENT: cscdStrAppendC(out, "[\"ident\", "); jsonString(out, text); cscdStrAppendC(out, "]"); break;
    case CSCD_TOK_AT_KEYWORD: cscdStrAppendC(out, "[\"at-keyword\", "); jsonString(out, text); cscdStrAppendC(out, "]"); break;
    case CSCD_TOK_HASH:
        cscdStrAppendC(out, "[\"hash\", ");
        jsonString(out, text);
        cscdStrAppendf(out, ", \"%s\"]", t->hashId ? "id" : "unrestricted");
        break;
    case CSCD_TOK_STRING: cscdStrAppendC(out, "[\"string\", "); jsonString(out, text); cscdStrAppendC(out, "]"); break;
    case CSCD_TOK_URL: cscdStrAppendC(out, "[\"url\", "); jsonString(out, text); cscdStrAppendC(out, "]"); break;
    case CSCD_TOK_NUMBER:
    case CSCD_TOK_PERCENTAGE:
    case CSCD_TOK_DIMENSION:
        cscdStrAppendf(out, "[\"%s\", \"\", ", t->type == CSCD_TOK_NUMBER ? "number" : t->type == CSCD_TOK_PERCENTAGE ? "percentage" : "dimension");
        jsonNumber(out, t->num);
        cscdStrAppendf(out, ", \"%s\"", t->integer ? "integer" : "number");
        if (t->type == CSCD_TOK_DIMENSION) {
            cscdStrAppendC(out, ", ");
            jsonString(out, text);
        }
        cscdStrAppendC(out, "]");
        break;
    case CSCD_TOK_DELIM: jsonString(out, (CscdSv){&t->delim, 1}); break;
    case CSCD_TOK_COMMA: cscdStrAppendC(out, "\",\""); break;
    case CSCD_TOK_COLON: cscdStrAppendC(out, "\":\""); break;
    case CSCD_TOK_SEMICOLON: cscdStrAppendC(out, "\";\""); break;
    case CSCD_TOK_WHITESPACE: cscdStrAppendC(out, "\" \""); break;
    case CSCD_TOK_FUNCTION:
        cscdStrAppendC(out, "[\"function\", ");
        jsonString(out, text);
        if (v->args.len) cscdStrAppendC(out, ", ");
        writeValues(out, v->args.items, v->args.len);
        cscdStrAppendC(out, "]");
        break;
    case CSCD_TOK_LBRACE:
    case CSCD_TOK_LPAREN:
    case CSCD_TOK_LBRACKET:
        cscdStrAppendf(out, "[\"%s\"", t->type == CSCD_TOK_LBRACE ? "{}" : t->type == CSCD_TOK_LPAREN ? "()" : "[]");
        if (v->args.len) cscdStrAppendC(out, ", ");
        writeValues(out, v->args.items, v->args.len);
        cscdStrAppendC(out, "]");
        break;
    case CSCD_TOK_RBRACE: cscdStrAppendC(out, "[\"error\", \"}\"]"); break;
    case CSCD_TOK_RPAREN: cscdStrAppendC(out, "[\"error\", \")\"]"); break;
    case CSCD_TOK_RBRACKET: cscdStrAppendC(out, "[\"error\", \"]\"]"); break;
    default: cscdStrAppendC(out, "null"); break;
    }
}

static void writeValues(CscdStr* out, const CscdCssValue* v, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (i) cscdStrAppendC(out, ", ");
        writeValue(out, &v[i]);
    }
}

int main(void) {
    static char line[1 << 20];
    while (fgets(line, sizeof line, stdin)) {
        char* sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        const char* mode = line;
        CscdStr text = {0};
        for (const char* p = sp + 1; p[0] && p[1] && p[0] != '\n' && p[0] != '\r'; p += 2) {
            char hex[3] = {p[0], p[1], 0};
            const char c = (char)strtol(hex, NULL, 16);
            cscdStrAppend(&text, (CscdSv){&c, 1});
        }
        CscdStr out = {0};
        if (!strcmp(mode, "values") || !strcmp(mode, "one")) {
            CscdCssValueVec vals = {0};
            cscdCssParseValues(cscdStrSv(&text), true, &vals);
            if (!strcmp(mode, "values")) {
                cscdStrAppendC(&out, "[");
                writeValues(&out, vals.items, vals.len);
                cscdStrAppendC(&out, "]");
            } else {
                size_t a = 0, b = vals.len;
                while (a < b && vals.items[a].tok.type == CSCD_TOK_WHITESPACE) ++a;
                while (b > a && vals.items[b - 1].tok.type == CSCD_TOK_WHITESPACE) --b;
                if (b - a == 1) writeValue(&out, &vals.items[a]);
                else cscdStrAppendf(&out, "[\"error\", \"%s\"]", a == b ? "empty" : "extra-input");
            }
            cscdCssValueVecFree(&vals);
        } else if (!strcmp(mode, "decls")) {
            CscdDeclVec decls = {0};
            cscdParseDeclarations(cscdStrSv(&text), &decls);
            cscdStrAppendC(&out, "[");
            for (size_t i = 0; i < decls.len; ++i) {
                if (i) cscdStrAppendC(&out, ", ");
                cscdStrAppendC(&out, "[\"declaration\", ");
                jsonString(&out, cscdStrSv(&decls.items[i].property));
                cscdStrAppendC(&out, ", [");
                CscdCssValueVec vals = {0};
                cscdCssParseValues(cscdStrSv(&decls.items[i].value), true, &vals);
                writeValues(&out, vals.items, vals.len);
                cscdCssValueVecFree(&vals);
                cscdStrAppendf(&out, "], %s]", decls.items[i].important ? "true" : "false");
            }
            cscdStrAppendC(&out, "]");
            cscdDeclVecFree(&decls);
        } else if (!strcmp(mode, "color")) {
            CscdColor c;
            if (cscdParseColorString(cscdStrSv(&text), &c)) cscdStrAppendf(&out, "[%d, %d, %d, %d]", c.r, c.g, c.b, c.a);
            else cscdStrAppendC(&out, "null");
        } else if (!strcmp(mode, "anb")) {
            CscdStr sel = cscdStrf(":nth-child(%s)", cscdStrC(&text));
            CscdSelectorVec list = {0};
            const bool ok = cscdParseSelectorList(cscdStrSv(&sel), &list) && list.len == 1 && list.items[0].compounds.len == 1 &&
                            list.items[0].compounds.items[0].parts.len == 1;
            if (ok) {
                const CscdSimpleSelector* s = &list.items[0].compounds.items[0].parts.items[0];
                cscdStrAppendf(&out, "[%d, %d]", s->nthA, s->nthB);
            } else cscdStrAppendC(&out, "null");
            cscdSelectorVecFree(&list);
            cscdStrFree(&sel);
        } else cscdStrAppendC(&out, "null");
        printf("%s\n", cscdStrC(&out));
        fflush(stdout);
        cscdStrFree(&out);
        cscdStrFree(&text);
    }
    return 0;
}
