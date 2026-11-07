/* What reads untrusted bytes, for a fuzzer: HTML and CSS (parsed, laid out, painted, then clicked,
 * typed in, touched and read by a screen reader), images (still and animated), fonts (outlines,
 * color layers, shaping, variations) and zlib, each a program of its own (CASCADE_FUZZ_TARGET)
 * with a corpus of its own: cascade-fuzz-html, -css, -image, -font and -zlib.
 *
 * With clang's libFuzzer (CASCADE_FUZZ=ON; a minute of each on every change, half an hour every night):
 *   cascade-fuzz-image -max_total_time=120 corpus/
 * Built otherwise, it reads each file it is given and runs it once: a crash a fuzzer found, again. */
#include "cascade/cascade.h"
#include "cascade/font.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* What a person does to a page, somewhere the bytes decide: the pointer, keys, typing, touch and
 * a screen reader walking the elements. */
static void walk(CscdView* v, CscdElement* e, int depth) {
    CscdPtrVec kids = {0};
    CscdStr name = {0};
    cscdAccessibleName(v, e, &name);
    cscdAccessibleValue(v, e, &name);
    cscdAccessibleState(v, e);
    cscdStrFree(&name);
    if (depth < 8) {
        cscdAccessibleChildren(v, e, &kids);
        for (size_t i = 0; i < kids.len && i < 16; ++i) walk(v, kids.items[i], depth + 1);
    }
    cscdVecFree(&kids);
}

static void interact(CscdView* v, const uint8_t* data, size_t size) {
    const CscdModifiers none = {0};
    const CscdModifiers ctrl = {.ctrl = true};
    for (size_t i = 0; i < 4; ++i) {
        const float x = (float)(size ? data[(i * 7) % size] : 0) * 320 / 256, y = (float)(size ? data[(i * 13 + 1) % size] : 0) * 240 / 256;
        cscdViewMouseMove(v, x, y, 0, none);
        cscdViewMouseDown(v, x, y, 0, none, 1 + (int)(i & 1));
        cscdViewMouseMove(v, x + 9, y + 3, 1, none);
        cscdViewMouseUp(v, x + 9, y + 3, 0, none);
        cscdViewElementAt(v, x, y);
        cscdViewWheel(v, x, y, 0, 40, none);
        cscdViewKeyDown(v, CSCD_KEY_TAB, none, CSCD_SV("Tab"));
        cscdViewTextInput(v, CSCD_SV("aÃ©"));
        cscdViewKeyDown(v, CSCD_KEY_LEFT, none, CSCD_SV("ArrowLeft"));
        cscdViewKeyDown(v, CSCD_KEY_BACKSPACE, none, CSCD_SV("Backspace"));
        cscdViewKeyDown(v, CSCD_KEY_A, ctrl, CSCD_SV("a"));
        cscdViewKeyDown(v, CSCD_KEY_Z, ctrl, CSCD_SV("z"));
        cscdViewKeyDown(v, CSCD_KEY_ENTER, none, CSCD_SV("Enter"));
        cscdViewKeyDown(v, CSCD_KEY_ESCAPE, none, CSCD_SV("Escape"));
        cscdViewTouchDown(v, 1, x, y);
        cscdViewTouchMove(v, 1, x, y + 30);
        cscdViewTouchUp(v, 1, x, y + 30);
        cscdViewUpdate(v);
    }
    walk(v, cscdAccessibleRoot(v), 0);
}

static void page(const uint8_t* data, size_t size, bool asCss) {
    CscdView* v = cscdViewNew();
    CscdStr html = {0};
    if (asCss) cscdStrAppendC(&html, "<div id=a class=b><p>text <b>bold</b></p><input value=x><table><tr><td>1</td></tr></table></div><style>");
    cscdStrAppend(&html, (CscdSv){(const char*)data, size});
    cscdViewLoadHTML(v, cscdStrSv(&html), CSCD_SV("."));
    cscdViewSetViewport(v, 320, 240, 1);
    cscdViewUpdate(v);
    CscdImage img;
    cscdImageInit(&img, 320, 240);
    cscdViewPaint(v, &img, false);
    interact(v, data, size);
    cscdViewPaint(v, &img, true);
    cscdImageFini(&img);
    cscdStrFree(&html);
    cscdViewFree(v);
}

static void image(const uint8_t* data, size_t size) {
    CscdImage* img = cscdDecodeImage(data, size);
    if (img) cscdRelease(img);
    CscdAnimatedImage* anim = cscdDecodeAnimatedImage(data, size);
    if (anim) cscdRelease(anim);
}

static void glyphs(CscdFontFace* f) {
    CscdColorLayer layers[16];
    for (uint32_t cp = 0x20; cp < 0x80; ++cp) {
        const uint16_t g = cscdFontFaceGlyphIndex(f, cp);
        CscdPath path = {0};
        CscdRect where;
        cscdFontFaceOutline(f, g, &path);
        cscdPathFree(&path);
        cscdFontFaceAdvance(f, g);
        cscdFontFaceKerning(f, g, (uint16_t)(g + 1));
        cscdFontFaceColorLayers(f, g, layers, 16);
        cscdFontFaceGlyphImage(f, g, 16, &where);
    }
}

/* Its tables for layout too: substitutions and positions, in the scripts that use them most. */
static void shape(CscdFontFace* f) {
    static const char* const texts[] = {
        "office AV fi 123", "Ø³ÙØ§Ù ÙÙ", /* Arabic */
        "à¤à¥à¤·à¤¿ à¤°à¥à¤", /* Devanagari */
        "à¦à§à¦·à§", /* Bengali */
    };
    CscdFont font = cscdFontDefault();
    font.face = f;
    CscdShapedGlyphVec out = {0};
    for (size_t i = 0; i < sizeof texts / sizeof *texts; ++i) {
        cscdFontShape(cscdSv(texts[i]), &font, 0, 0, &out);
        cscdFontShapeRtl(cscdSv(texts[i]), &font, 1, 2, &out);
    }
    cscdVecFree(&out);
}

static void font(const uint8_t* data, size_t size) {
    CscdFontFace* f = cscdFontFaceLoad(NULL, data, size, 0);
    if (!f) return;
    glyphs(f);
    shape(f);
    /* a variable font's far corner, where its deltas are whole */
    CscdFontTagValue corner[8];
    size_t axes = 0;
    for (int i = 0; i < cscdFontFaceAxisCount(f) && axes < 8; ++i) {
        float min, def, max;
        if (cscdFontFaceAxis(f, i, &corner[axes].tag, &min, &def, &max)) corner[axes++].value = max;
    }
    if (axes) {
        CscdFontFace* instance = cscdFontFaceVary(f, corner, axes);
        glyphs(instance);
        shape(instance);
    }
    cscdFontFaceFree(f);
}

static void inflate(const uint8_t* data, size_t size) {
    CscdBytes out = {0};
    cscdInflateZlibLimit(data, size, &out, 1 << 24);
    cscdVecFree(&out);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    switch (CASCADE_FUZZ_TARGET) {
    case 0: page(data, size, false); break;
    case 1: page(data, size, true); break;
    case 2: image(data, size); break;
    case 3: font(data, size); break;
    default: inflate(data, size); break;
    }
    return 0;
}

#ifndef CASCADE_LIBFUZZER
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        CscdStr s = {0};
        if (!cscdReadFile(cscdSv(argv[i]), &s)) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            return 1;
        }
        LLVMFuzzerTestOneInput((const uint8_t*)s.ptr, s.len);
        cscdStrFree(&s);
        printf("%s: ok\n", argv[i]);
    }
    return 0;
}
#endif
