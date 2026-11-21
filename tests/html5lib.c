/* html5lib's tree construction tests on cascade's HTML parser: each case's input parsed (a document, or a fragment in
 * the context it names) and the tree it makes written as the tests write theirs, then compared.
 *
 *     cascade-html5lib-tests path/to/tree-construction [--known FILE]
 *
 * With no folder (the tests are not in the repository: CI downloads a pinned commit of html5lib-tests,
 * CASCADE_HTML5LIB_TESTS names it), nothing is run. What cascade's tree does not have is left out of both sides: the
 * doctype (no node for it), namespaces (an SVG or MathML element is its tag name, lowercase; xlink:href an attribute
 * with that name). Cases that need scripting (#script-on) are passed over; a fragment in a foreign context ("svg
 * path") is parsed in it. --known: the cases known to fail ("file.dat:N", one a line, "#" comments); one listed that
 * passes is an error too. */
#include "cascade/cascade.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

static char* readFile(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* d = (char*)malloc((size_t)(n > 0 ? n : 0) + 1);
    *len = d && n > 0 ? fread(d, 1, (size_t)n, f) : 0;
    if (d) d[*len] = 0;
    fclose(f);
    return d;
}

/* ---- the tree, written as the tests write it ----------------------------------------------------------------------- */

static int compareAttrs(const void* a, const void* b) {
    return strcmp(cscdStrC(&((const CscdStrPair*)a)->name), cscdStrC(&((const CscdStrPair*)b)->name));
}

static void indent(CscdStr* out, int depth) {
    cscdStrAppendC(out, "| ");
    for (int i = 0; i < depth; ++i) cscdStrAppendC(out, "  ");
}

static void dumpNode(const CscdNode* n, int depth, CscdStr* out);

static void dumpChildren(const CscdNode* n, int depth, CscdStr* out) {
    for (size_t i = 0; i < n->children.len; ++i) dumpNode(cscdNodeChild(n, i), depth, out);
}

static void dumpNode(const CscdNode* n, int depth, CscdStr* out) {
    if (n->type == CSCD_NODE_TEXT) {
        indent(out, depth);
        cscdStrAppendf(out, "\"%s\"\n", cscdStrC(&((const CscdText*)n)->data));
    } else if (n->type == CSCD_NODE_COMMENT) {
        indent(out, depth);
        cscdStrAppendf(out, "<!-- %s -->\n", cscdStrC(&((const CscdComment*)n)->data));
    } else if (n->type == CSCD_NODE_ELEMENT) {
        const CscdElement* e = (const CscdElement*)n;
        indent(out, depth);
        cscdStrAppendf(out, "<%s>\n", cscdStrC(&e->tag));
        CscdStrPairVec attrs = {0};
        cscdStrPairVecCopy(&attrs, &e->attributes);
        qsort(attrs.items, attrs.len, sizeof(CscdStrPair), compareAttrs);
        for (size_t i = 0; i < attrs.len; ++i) {
            indent(out, depth + 1);
            cscdStrAppendf(out, "%s=\"%s\"\n", cscdStrC(&attrs.items[i].name), cscdStrC(&attrs.items[i].value));
        }
        cscdStrPairVecFree(&attrs);
        if (e->templateContent_ || (e->ns_ == CSCD_NS_HTML && cscdStrEqC(&e->tag, "template"))) { /* (an empty one's too) */
            indent(out, depth + 1);
            cscdStrAppendC(out, "content\n");
            if (e->templateContent_) dumpChildren(&e->templateContent_->node, depth + 2, out);
        }
        dumpChildren(n, depth + 1, out);
    }
}

/* The expected tree, as cascade's can be: no doctype, no namespace (the tag lowercased, "xlink href" as "xlink:href"),
 * the attributes in order again (their names changed may sort otherwise). */
static void normalizeExpected(const char* doc, CscdStr* out) {
    typedef struct Line { CscdStr text; int depth; bool attr; } Line;
    CSCD_VEC(Line) lines = {0};
    const char* p = doc;
    while (*p) {
        const char* end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        if (!strncmp(p, "| ", 2)) {
            Line l = {{0}, 0, false};
            const char* q = p + 2;
            while (q[0] == ' ' && q[1] == ' ') {
                ++l.depth;
                q += 2;
            }
            cscdStrAppend(&l.text, (CscdSv){q, (size_t)(end - q)});
            cscdVecPush(&lines, l);
        } else if (lines.len) { /* (a text that goes on over lines) */
            cscdStrAppendC(&cscdVecLast(&lines).text, "\n");
            cscdStrAppend(&cscdVecLast(&lines).text, (CscdSv){p, (size_t)(end - p)});
        }
        p = *end ? end + 1 : end;
    }
    for (size_t i = 0; i < lines.len; ++i) {
        Line* l = &lines.items[i];
        const char* t = cscdStrC(&l->text);
        if (!strncmp(t, "<!DOCTYPE", 9)) {
            l->depth = -1;
            continue;
        }
        const size_t tl = strlen(t);
        if (t[0] == '<' && t[1] != '!' && t[1] != '/' && tl && t[tl - 1] == '>') { /* (an element: "<=\"\"" is an attribute) */
            const char* sp = strchr(t, ' ');
            const char* close = strchr(t, '>');
            CscdStr name = {0};
            if (sp && close && sp < close && (!strncmp(t + 1, "svg ", 4) || !strncmp(t + 1, "math ", 5)))
                cscdStrAppend(&name, (CscdSv){sp + 1, (size_t)(close - sp - 1)});
            else if (close)
                cscdStrAppend(&name, (CscdSv){t + 1, (size_t)(close - t - 1)});
            for (size_t k = 0; k < name.len; ++k) name.ptr[k] = (char)tolower((unsigned char)name.ptr[k]);
            cscdStrClear(&l->text);
            cscdStrAppendf(&l->text, "<%s>", cscdStrC(&name));
            cscdStrFree(&name);
        } else if (t[0] != '"' && strncmp(t, "<!--", 4) && strchr(t, '=')) {
            l->attr = true;
            const char* eq = strchr(t, '=');
            const char* sp = strchr(t, ' ');
            if (sp && sp < eq) { /* "xlink href" -> "xlink:href" */
                l->text.ptr[sp - t] = ':';
            }
        }
    }
    /* (attributes of one element: sorted by name, as cascade's are written) */
    for (size_t i = 0; i < lines.len;) {
        if (!lines.items[i].attr) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < lines.len && lines.items[j].attr && lines.items[j].depth == lines.items[i].depth) ++j;
        for (size_t a = i + 1; a < j; ++a)
            for (size_t b = a; b > i && strcmp(cscdStrC(&lines.items[b - 1].text), cscdStrC(&lines.items[b].text)) > 0; --b) {
                const Line tmp = lines.items[b - 1];
                lines.items[b - 1] = lines.items[b];
                lines.items[b] = tmp;
            }
        i = j;
    }
    for (size_t i = 0; i < lines.len; ++i) {
        if (lines.items[i].depth >= 0) {
            indent(out, lines.items[i].depth);
            cscdStrAppend(out, cscdStrSv(&lines.items[i].text));
            cscdStrAppendC(out, "\n");
        }
        cscdStrFree(&lines.items[i].text);
    }
    cscdVecFree(&lines);
}

/* ---- the cases ----------------------------------------------------------------------------------------------------- */

typedef struct Case {
    CscdStr data, document, fragment;
    bool scriptOn;
} Case;

/* the sections of the case at *p: "#data", "#errors", "#document-fragment", "#script-on", "#document"... */
static bool nextCase(const char** p, const char* limit, Case* c) {
    /* (by lengths: a case's data may hold a NUL) */
    const char* s = *p;
    while (s < limit && !(limit - s >= 6 && !memcmp(s, "#data\n", 6) && (s == *p || s[-1] == '\n'))) ++s;
    if (s >= limit) return false;
    s += 6;
    const char* section = NULL; /* (which one the lines go to) */
    CscdStr* into = &c->data;
    bool first = true;
    while (s < limit) {
        const char* end = (const char*)memchr(s, '\n', (size_t)(limit - s));
        if (!end) end = limit;
        const CscdSv line = {s, (size_t)(end - s)};
        if (line.len && line.ptr[0] == '#' && (cscdSvEqC(line, "#errors") || cscdSvEqC(line, "#new-errors") || cscdSvEqC(line, "#document") ||
                                                cscdSvEqC(line, "#document-fragment") || cscdSvEqC(line, "#script-on") || cscdSvEqC(line, "#script-off"))) {
            section = line.ptr;
            into = cscdSvEqC(line, "#document") ? &c->document : cscdSvEqC(line, "#document-fragment") ? &c->fragment : NULL;
            if (cscdSvEqC(line, "#script-on")) c->scriptOn = true;
            first = true;
        } else if (section && !strncmp(section, "#document\n", 10) && line.len == 0 &&
                   (end >= limit - 1 || (limit - end > 6 && !memcmp(end + 1, "#data\n", 6)))) {
            s = end < limit ? end + 1 : end; /* (the blank line after a case: a blank line in a text goes on) */
            break;
        } else if (into) {
            if (!first || into != &c->data) {
                if (!first) cscdStrAppendC(into, "\n");
            }
            cscdStrAppend(into, line);
            first = false;
        }
        if (!section && into == &c->data && first) first = false;
        s = end < limit ? end + 1 : end;
    }
    *p = s;
    return true;
}

static bool known(const CscdStrVec* list, const char* id) {
    for (size_t i = 0; i < list->len; ++i)
        if (!strcmp(cscdStrC(&list->items[i]), id)) return true;
    return false;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 && strcmp(argv[1], "--known") ? argv[1] : getenv("CASCADE_HTML5LIB_TESTS");
    const char* knownPath = NULL;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--known")) knownPath = argv[i + 1];
    if (!dir || !*dir) {
        printf("html5lib's tests: not run (no folder: CASCADE_HTML5LIB_TESTS)\n");
        return 0;
    }
    CscdStrVec knownList = {0};
    if (knownPath) {
        size_t len;
        char* k = readFile(knownPath, &len);
        for (char* line = k ? strtok(k, "\r\n") : NULL; line; line = strtok(NULL, "\r\n")) {
            char* hash = strchr(line, '#');
            if (hash) *hash = 0;
            while (*line == ' ') ++line;
            size_t n = strlen(line);
            while (n && line[n - 1] == ' ') line[--n] = 0;
            if (n) cscdVecPush(&knownList, cscdStrDup(cscdSv(line)));
        }
        free(k);
    }
    CscdStrVec files = {0};
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s/*.dat", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h != -1) {
        do cscdVecPush(&files, cscdStrDup(cscdSv(fd.name)));
        while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    DIR* d = opendir(dir);
    for (struct dirent* e; d && (e = readdir(d));) {
        const size_t n = strlen(e->d_name);
        if (n > 4 && !strcmp(e->d_name + n - 4, ".dat")) cscdVecPush(&files, cscdStrDup(cscdSv(e->d_name)));
    }
    if (d) closedir(d);
#endif
    for (size_t a = 1; a < files.len; ++a) /* (in order: the case numbers are by file) */
        for (size_t b = a; b > 0 && strcmp(cscdStrC(&files.items[b - 1]), cscdStrC(&files.items[b])) > 0; --b) {
            const CscdStr t = files.items[b - 1];
            files.items[b - 1] = files.items[b];
            files.items[b] = t;
        }
    int passed = 0, failed = 0, knownFailed = 0, skipped = 0, stale = 0, shown = 0;
    for (size_t fi = 0; fi < files.len; ++fi) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, cscdStrC(&files.items[fi]));
        size_t len;
        char* text = readFile(path, &len);
        if (!text) continue;
        /* (CRLF checkouts: the cases are LF) */
        size_t w = 0;
        for (size_t i = 0; i < len; ++i)
            if (!(text[i] == '\r' && i + 1 < len && text[i + 1] == '\n')) text[w++] = text[i]; /* (a lone CR is the case's) */
        text[w] = 0;
        const char* p = text;
        int index = 0;
        Case c;
        memset(&c, 0, sizeof c);
        while (nextCase(&p, text + w, &c)) {
            ++index;
            char id[256];
            snprintf(id, sizeof id, "%s:%d", cscdStrC(&files.items[fi]), index);
            if (c.scriptOn) {
                ++skipped;
            } else {
                CscdStr got = {0}, want = {0};
                normalizeExpected(cscdStrC(&c.document), &want);
                if (c.fragment.len) {
                    CscdDocument* doc = cscdParseHTML(CSCD_SV(""));
                    /* (a foreign context: "svg path", "math mi") */
                    CscdSv name = cscdStrSv(&c.fragment);
                    uint8_t ns = CSCD_NS_HTML;
                    if (cscdSvStartsC(name, "svg ")) ns = CSCD_NS_SVG, name = cscdSvFrom(name, 4);
                    else if (cscdSvStartsC(name, "math ")) ns = CSCD_NS_MATHML, name = cscdSvFrom(name, 5);
                    CscdElement* ctx = cscdDocumentCreateElement(doc, name);
                    ctx->ns_ = ns;
                    cscdParseHTMLFragment(cscdStrSv(&c.data), ctx);
                    /* (a template's: in its content) */
                    dumpChildren(ctx->templateContent_ ? &ctx->templateContent_->node : &ctx->node, 0, &got);
                    cscdNodeDestroy(&ctx->node);
                    cscdDocumentFree(doc);
                } else {
                    CscdDocument* doc = cscdParseHTML(cscdStrSv(&c.data));
                    dumpChildren(&doc->node, 0, &got);
                    cscdDocumentFree(doc);
                }
                const bool ok = cscdStrEq(&got, &want);
                const bool isKnown = known(&knownList, id);
                if (ok) {
                    ++passed;
                    if (isKnown) {
                        printf("PASS %s: listed as failing (take it off the list)\n", id);
                        ++stale;
                    }
                } else if (isKnown) ++knownFailed;
                else {
                    ++failed;
                    if (shown++ < 12)
                        printf("FAIL %s\n#data\n%s\n#want\n%s#got\n%s\n", id, cscdStrC(&c.data), cscdStrC(&want), cscdStrC(&got));
                    else if (getenv("CASCADE_HTML5LIB_LIST")) printf("FAIL %s\n", id);
                }
                cscdStrFree(&got);
                cscdStrFree(&want);
            }
            cscdStrFree(&c.data);
            cscdStrFree(&c.document);
            cscdStrFree(&c.fragment);
            memset(&c, 0, sizeof c);
        }
        free(text);
    }
    printf("html5lib tree construction: %d of %d cases pass (%d failing as known, %d not; %d passed over)\n", passed,
           passed + failed + knownFailed, knownFailed, failed, skipped);
    for (size_t i = 0; i < files.len; ++i) cscdStrFree(&files.items[i]);
    cscdVecFree(&files);
    for (size_t i = 0; i < knownList.len; ++i) cscdStrFree(&knownList.items[i]);
    cscdVecFree(&knownList);
    return failed || stale ? 1 : 0;
}
