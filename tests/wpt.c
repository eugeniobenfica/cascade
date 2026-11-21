/* web-platform-tests' CSS reftests on cascade, without JavaScript: each test page and the reference it names
 * (<link rel=match> or rel=mismatch) painted at 800x600, compared pixel by pixel (within the test's <meta name=fuzzy>).
 *
 *     cascade-wpt-tests path/to/wpt [--score FILE] [--write-score] [DIR ...]
 *
 * DIRs are folders of the checkout (css/css-flexbox...), walked down (an XHTML page's self-closed elements closed, as XML
 * would: cascade parses HTML only); a page with a script (cascade runs none) or a
 * reftest-wait class is passed over, as are support and reference folders and the references themselves. Prints the
 * score of each folder; with --score, fails when a folder passes fewer tests than the file records (--write-score
 * writes the file from this run instead: the score of a release). With no checkout (CASCADE_WPT names it; CI fetches a
 * pinned commit of the folders), nothing is run. */
#include "cascade/cascade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

static const char* wptRoot;

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

/* the pages' files: from the disk, "/..." from the checkout's root */
static bool loadResource(void* user, CscdSv path, CscdStr* out) {
    (void)user;
    CscdStr p = cscdStrDup(path);
    size_t q = cscdSvFindChar(cscdStrSv(&p), '?', 0);
    if (q != CSCD_NPOS) p.len = q, p.ptr[q] = 0;
    q = cscdSvFindChar(cscdStrSv(&p), '#', 0);
    if (q != CSCD_NPOS) p.len = q, p.ptr[q] = 0;
    size_t len;
    char* d = readFile(cscdStrC(&p), &len);
    if (!d && p.len && p.ptr[0] == '/') {
        CscdStr rooted = cscdStrf("%s%s", wptRoot, cscdStrC(&p));
        d = readFile(cscdStrC(&rooted), &len);
        cscdStrFree(&rooted);
    }
    cscdStrFree(&p);
    if (!d) return false;
    cscdStrAppend(out, (CscdSv){d, len});
    free(d);
    return true;
}

typedef struct Files { CscdStrVec items; } Files;

static bool isDir(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static void walk(const char* dir, CscdStrVec* out) {
    CscdStrVec names = {0};
#ifdef _WIN32
    CscdStr pattern = cscdStrf("%s/*", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(cscdStrC(&pattern), &fd);
    if (h != -1) {
        do cscdVecPush(&names, cscdStrDup(cscdSv(fd.name)));
        while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
    cscdStrFree(&pattern);
#else
    DIR* d = opendir(dir);
    for (struct dirent* e; d && (e = readdir(d));) cscdVecPush(&names, cscdStrDup(cscdSv(e->d_name)));
    if (d) closedir(d);
#endif
    for (size_t a = 1; a < names.len; ++a)
        for (size_t b = a; b > 0 && strcmp(cscdStrC(&names.items[b - 1]), cscdStrC(&names.items[b])) > 0; --b) {
            const CscdStr t = names.items[b - 1];
            names.items[b - 1] = names.items[b];
            names.items[b] = t;
        }
    for (size_t i = 0; i < names.len; ++i) {
        const char* n = cscdStrC(&names.items[i]);
        if (n[0] == '.') continue;
        CscdStr path = cscdStrf("%s/%s", dir, n);
        if (isDir(cscdStrC(&path))) {
            if (strcmp(n, "support") && strcmp(n, "reference") && strcmp(n, "resources")) walk(cscdStrC(&path), out);
            cscdStrFree(&path);
            continue;
        }
        const CscdSv sv = cscdSv(n);
        const bool page = cscdSvEndsC(sv, ".html") || cscdSvEndsC(sv, ".htm") || cscdSvEndsC(sv, ".xht") || cscdSvEndsC(sv, ".xhtml");
        const bool ref = cscdSvFindC(sv, "-ref", 0) != CSCD_NPOS || cscdSvFindC(sv, "-notref", 0) != CSCD_NPOS;
        if (page && !ref) cscdVecPush(out, path);
        else cscdStrFree(&path);
    }
    for (size_t i = 0; i < names.len; ++i) cscdStrFree(&names.items[i]);
    cscdVecFree(&names);
}

/* an attribute's value inside a tag found at p (rel=match href=...): its text, or empty */
static CscdStr attrOf(const char* tagStart, const char* tagEnd, const char* name) {
    CscdStr out = {0};
    const size_t nl = strlen(name);
    for (const char* p = tagStart; p + nl < tagEnd; ++p) {
        if (strncmp(p, name, nl) || (p > tagStart && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n')) continue;
        const char* q = p + nl;
        while (*q == ' ') ++q;
        if (*q != '=') continue;
        ++q;
        while (*q == ' ') ++q;
        const char quote = (*q == '"' || *q == '\'') ? *q++ : 0;
        const char* e = q;
        while (e < tagEnd && (quote ? *e != quote : (*e != ' ' && *e != '>' && *e != '/'))) ++e;
        cscdStrAppend(&out, (CscdSv){q, (size_t)(e - q)});
        return out;
    }
    return out;
}

typedef struct Ref { CscdStr href; bool mismatch; int maxDiffLo, maxDiffHi, pixelsLo, pixelsHi; } Ref;

/* the references of a test page (match or mismatch) and its fuzzy allowance */
static bool parseTest(const char* text, Ref* ref) {
    memset(ref, 0, sizeof *ref);
    ref->maxDiffHi = ref->pixelsHi = 0;
    bool found = false;
    for (const char* p = text; (p = strstr(p, "<link")) != NULL; ++p) {
        const char* end = strchr(p, '>');
        if (!end) break;
        CscdStr rel = attrOf(p, end, "rel");
        if (cscdStrEqC(&rel, "match") || cscdStrEqC(&rel, "mismatch")) {
            if (!found) {
                ref->href = attrOf(p, end, "href");
                ref->mismatch = cscdStrEqC(&rel, "mismatch");
                found = ref->href.len > 0;
            }
        }
        cscdStrFree(&rel);
    }
    for (const char* p = text; (p = strstr(p, "<meta")) != NULL; ++p) {
        const char* end = strchr(p, '>');
        if (!end) break;
        CscdStr name = attrOf(p, end, "name");
        if (cscdStrEqC(&name, "fuzzy")) {
            CscdStr content = attrOf(p, end, "content");
            const char* c = cscdStrC(&content);
            const char* colon = strchr(c, ':'); /* ("ref.html:maxDifference=..." names a reference: taken for any) */
            if (colon && colon < strchr(c, '=')) c = colon + 1;
            const char* md = strstr(c, "maxDifference=");
            const char* tp = strstr(c, "totalPixels=");
            int a = 0, b = 0;
            if (md && sscanf(md + 14, "%d-%d", &a, &b) >= 1) { ref->maxDiffLo = a; ref->maxDiffHi = b ? b : a; }
            else if (sscanf(c, "%d-%d;", &a, &b) >= 1) { ref->maxDiffLo = a; ref->maxDiffHi = b ? b : a; }
            a = b = 0;
            if (tp && sscanf(tp + 12, "%d-%d", &a, &b) >= 1) { ref->pixelsLo = a; ref->pixelsHi = b ? b : a; }
            else {
                const char* semi = strchr(c, ';');
                if (semi && sscanf(semi + 1, "%d-%d", &a, &b) >= 1) { ref->pixelsLo = a; ref->pixelsHi = b ? b : a; }
            }
            cscdStrFree(&content);
        }
        cscdStrFree(&name);
    }
    return found;
}

/* An XHTML page as HTML: its self-closed elements (<div/>, which XML closes and HTML does not) written as an empty
 * pair, the void ones left as they are. (Browsers parse .xht as XML; cascade has the HTML parser only.) */
static void xhtmlAsHtml(char** text, size_t* len) {
    static const char* const kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
    CscdStr out = {0};
    const char* p = *text;
    const char* end = *text + *len;
    while (p < end) {
        /* (CDATA sections: their text, as XML gives it, without the markers HTML would leave in a style sheet) */
        if (end - p >= 9 && !strncmp(p, "<![CDATA[", 9)) {
            p += 9;
            continue;
        }
        if (end - p >= 3 && !strncmp(p, "]]>", 3)) {
            p += 3;
            continue;
        }
        if (p[0] == '<' && p + 1 < end && ((p[1] >= 'a' && p[1] <= 'z') || (p[1] >= 'A' && p[1] <= 'Z'))) {
            const char* q = p + 1;
            while (q < end && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r' && *q != '/' && *q != '>') ++q;
            const CscdSv name = {p + 1, (size_t)(q - p - 1)};
            char quote = 0;
            while (q < end && (quote || *q != '>')) {
                if (quote && *q == quote) quote = 0;
                else if (!quote && (*q == '"' || *q == '\'')) quote = *q;
                ++q;
            }
            if (q < end && q[-1] == '/') {
                bool isVoid = false;
                for (size_t k = 0; k < CSCD_COUNT(kVoid) && !isVoid; ++k) isVoid = cscdSvIEqC(name, kVoid[k]);
                if (!isVoid) {
                    cscdStrAppend(&out, (CscdSv){p, (size_t)(q - 1 - p)});
                    cscdStrAppendf(&out, "></%.*s>", (int)name.len, name.ptr);
                    p = q + 1;
                    continue;
                }
            }
            if (q < end) ++q;
            cscdStrAppend(&out, (CscdSv){p, (size_t)(q - p)});
            p = q;
            continue;
        }
        cscdStrAppend(&out, (CscdSv){p, 1});
        ++p;
    }
    free(*text);
    *len = out.len;
    *text = (char*)malloc(out.len + 1);
    memcpy(*text, out.ptr ? out.ptr : "", out.len);
    (*text)[out.len] = 0;
    cscdStrFree(&out);
}

static void render(const char* path, CscdImage* img) {
    size_t len;
    char* text = readFile(path, &len);
    if (text && (cscdSvEndsC(cscdSv(path), ".xht") || cscdSvEndsC(cscdSv(path), ".xhtml"))) xhtmlAsHtml(&text, &len);
    CscdView* v = cscdViewNew();
    cscdViewOnWarning(v, NULL, NULL);
    cscdViewSetViewport(v, 800, 600, 1);
    /* (Ahem, the font the tests take to be installed: from the checkout) */
    CscdStr ahem = cscdStrf("%s/fonts/Ahem.ttf", wptRoot);
    cscdViewAddFont(v, CSCD_SV("Ahem"), cscdStrSv(&ahem), 400, false);
    cscdStrFree(&ahem);
    const char* slash = strrchr(path, '/');
    CscdStr dir = slash ? cscdStrDup((CscdSv){path, (size_t)(slash - path)}) : cscdStrDup(CSCD_SV("."));
    cscdViewLoadHTML(v, (CscdSv){text ? text : "", text ? len : 0}, cscdStrSv(&dir));
    cscdViewUpdate(v);
    cscdViewPaint(v, img, false);
    cscdViewFree(v);
    cscdStrFree(&dir);
    free(text);
}

typedef struct Score { CscdStr dir; int passed, total; } Score;

int main(int argc, char** argv) {
    const char* root = NULL;
    const char* scorePath = NULL;
    bool writeScore = false;
    CscdStrVec dirs = {0};
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--score") && i + 1 < argc) scorePath = argv[++i];
        else if (!strcmp(argv[i], "--write-score")) writeScore = true;
        else if (!root) root = argv[i];
        else cscdVecPush(&dirs, cscdStrDup(cscdSv(argv[i])));
    }
    if (!root) root = getenv("CASCADE_WPT");
    if (!root || !*root) {
        printf("web-platform-tests: not run (no checkout: CASCADE_WPT)\n");
        return 0;
    }
    wptRoot = root;
    cscdAppSetResourceLoader(loadResource, NULL);
    /* the folders: those given, or those the score file has */
    CSCD_VEC(Score) recorded = {0};
    if (scorePath) {
        size_t len;
        char* s = readFile(scorePath, &len);
        for (char* line = s ? strtok(s, "\r\n") : NULL; line; line = strtok(NULL, "\r\n")) {
            if (line[0] == '#') continue;
            char name[512];
            int p, t;
            if (sscanf(line, "%511s %d %d", name, &p, &t) == 3) {
                Score sc = {cscdStrDup(cscdSv(name)), p, t};
                cscdVecPush(&recorded, sc);
            }
        }
        free(s);
    }
    if (!dirs.len)
        for (size_t i = 0; i < recorded.len; ++i) cscdVecPush(&dirs, cscdStrDup(cscdStrSv(&recorded.items[i].dir)));
    CscdImage a, b;
    cscdImageInit(&a, 800, 600);
    cscdImageInit(&b, 800, 600);
    bool worse = false;
    CscdStr scoreOut = cscdStrf("# web-platform-tests reftests cascade passes (tests/wpt.c): folder, passed, run\n");
    for (size_t di = 0; di < dirs.len; ++di) {
        CscdStrVec tests = {0};
        CscdStr top = cscdStrf("%s/%s", root, cscdStrC(&dirs.items[di]));
        walk(cscdStrC(&top), &tests);
        int passed = 0, run = 0, skipped = 0;
        for (size_t ti = 0; ti < tests.len; ++ti) {
            const char* path = cscdStrC(&tests.items[ti]);
            size_t len;
            char* text = readFile(path, &len);
            Ref ref;
            const bool reftest = text && parseTest(text, &ref);
            const bool scripted = text && (strstr(text, "<script") || strstr(text, "reftest-wait") || strstr(text, "<svg"));
            free(text);
            if (!reftest || scripted) {
                if (reftest) cscdStrFree(&ref.href);
                if (reftest) ++skipped;
                continue;
            }
            const char* slash = strrchr(path, '/');
            CscdStr refPath = ref.href.ptr[0] == '/' ? cscdStrf("%s%s", root, cscdStrC(&ref.href))
                                                     : cscdStrf("%.*s/%s", (int)(slash - path), path, cscdStrC(&ref.href));
            size_t refLen;
            char* refText = readFile(cscdStrC(&refPath), &refLen);
            const bool refScripted = refText && strstr(refText, "<script");
            free(refText);
            if (!refText || refScripted) {
                ++skipped;
                cscdStrFree(&refPath);
                cscdStrFree(&ref.href);
                continue;
            }
            render(path, &a);
            render(cscdStrC(&refPath), &b);
            int maxDiff = 0, pixels = 0;
            for (int y = 0; y < 600; ++y) {
                const uint8_t *pa = (const uint8_t*)cscdImageRow(&a, y), *pb = (const uint8_t*)cscdImageRow(&b, y);
                for (int x = 0; x < 800 * 4; x += 4) {
                    int d = 0;
                    for (int k = 0; k < 4; ++k) {
                        const int e = abs(pa[x + k] - pb[x + k]);
                        if (e > d) d = e;
                    }
                    if (d) {
                        ++pixels;
                        if (d > maxDiff) maxDiff = d;
                    }
                }
            }
            const bool same = pixels == 0 || (maxDiff >= ref.maxDiffLo && maxDiff <= ref.maxDiffHi && pixels >= ref.pixelsLo &&
                                              pixels <= ref.pixelsHi);
            const bool ok = ref.mismatch ? !same : same;
            ++run;
            passed += ok;
            if (!ok && getenv("CASCADE_WPT_LIST")) printf("FAIL %s (%d pixels, by up to %d)\n", path + strlen(root) + 1, pixels, maxDiff);
            static int dumped;
            if (!ok && getenv("CASCADE_WPT_DUMP") && !dumped++) { /* (the first failure's two pictures, to look at) */
                cscdSavePNG(CSCD_SV("wpt-test.png"), &a);
                cscdSavePNG(CSCD_SV("wpt-ref.png"), &b);
            }
            cscdStrFree(&refPath);
            cscdStrFree(&ref.href);
        }
        int was = -1;
        for (size_t i = 0; i < recorded.len; ++i)
            if (cscdStrEq(&recorded.items[i].dir, &dirs.items[di])) was = recorded.items[i].passed;
        printf("%-40s %4d of %4d reftests pass (%d passed over)%s\n", cscdStrC(&dirs.items[di]), passed, run, skipped,
               was < 0 ? "" : passed < was ? "  FEWER than recorded" : passed > was ? "  more than recorded" : "");
        if (was >= 0 && passed < was) worse = true;
        cscdStrAppendf(&scoreOut, "%s %d %d\n", cscdStrC(&dirs.items[di]), passed, run);
        for (size_t i = 0; i < tests.len; ++i) cscdStrFree(&tests.items[i]);
        cscdVecFree(&tests);
        cscdStrFree(&top);
    }
    if (writeScore && scorePath) {
        FILE* f = fopen(scorePath, "wb");
        if (f) {
            fwrite(scoreOut.ptr, 1, scoreOut.len, f);
            fclose(f);
        }
    }
    cscdStrFree(&scoreOut);
    cscdImageFini(&a);
    cscdImageFini(&b);
    for (size_t i = 0; i < dirs.len; ++i) cscdStrFree(&dirs.items[i]);
    cscdVecFree(&dirs);
    for (size_t i = 0; i < recorded.len; ++i) cscdStrFree(&recorded.items[i].dir);
    cscdVecFree(&recorded);
    return worse ? 1 : 0;
}
