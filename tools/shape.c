/* cascade-shape: a font's shaping of texts, glyph by glyph, to compare with another shaper's (tools/compareShaping.py)
 * or with a conformance suite's (tools/textRenderingTests.py).
 *
 *     cascade-shape font.ttf [--rtl] [--index N] < cases.txt
 *
 * Each line of cases.txt is a text as hex code points ("0E01 0E33"); each line out is its glyphs, in the order shown,
 * as "glyph:cluster@x,y+advance" in font units, separated by spaces (cluster: the index of the first code point the
 * glyph stands for). A glyph from another font than this one (a fallback) shows as "?". */
#include "cascade/cascade.h"
#include "cascade/font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char* readAll(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* d = (unsigned char*)malloc(n > 0 ? (size_t)n : 1);
    *size = d && n > 0 ? fread(d, 1, (size_t)n, f) : 0;
    fclose(f);
    return d;
}

int main(int argc, char** argv) {
    const char* path = NULL;
    bool rtl = false;
    int index = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--rtl")) rtl = true;
        else if (!strcmp(argv[i], "--index") && i + 1 < argc) index = atoi(argv[++i]);
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: cascade-shape font.ttf [--rtl] [--index N] < cases.txt\n");
        return 2;
    }
    size_t size = 0;
    unsigned char* data = readAll(path, &size);
    CscdFontFace* face = data ? cscdFontFaceLoad(NULL, data, size, index) : NULL;
    if (!face) {
        fprintf(stderr, "cascade-shape: %s is not a font cascade reads\n", path);
        return 1;
    }
    CscdFont font = cscdFontDefault();
    font.face = face;
    font.size = (float)face->unitsPerEm; /* (a px a font unit) */
    CscdShapedGlyphVec out = {0};
    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        CscdStr text = {0};
        CscdU32Vec starts = {0}; /* (each code point's byte offset: clusters back to code point indices) */
        for (char* p = line;;) {
            char* end;
            const unsigned long cp = strtoul(p, &end, 16);
            if (end == p) break;
            cscdVecPush(&starts, (uint32_t)text.len);
            cscdUtf8Append(&text, (uint32_t)cp);
            p = end;
        }
        if (rtl) cscdFontShapeRtl(cscdStrSv(&text), &font, 0, 0, &out);
        else cscdFontShape(cscdStrSv(&text), &font, 0, 0, &out);
        for (size_t i = 0; i < out.len; ++i) {
            const CscdShapedGlyph* g = &out.items[i];
            size_t c = 0;
            while (c + 1 < starts.len && starts.items[c + 1] <= g->cluster) ++c;
            if (g->face != face && g->glyph) printf("%s?:%zu", i ? " " : "", c);
            else printf("%s%u:%zu@%ld,%ld+%ld", i ? " " : "", (unsigned)g->glyph, c, lroundf(g->x), lroundf(-g->y), lroundf(g->advance));
        }
        printf("\n");
        fflush(stdout);
        cscdStrFree(&text);
        cscdVecFree(&starts);
    }
    cscdVecFree(&out);
    return 0;
}
