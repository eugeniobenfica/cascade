/* Arabic shaping: each letter takes the form its neighbours ask for (Unicode's joining, from Joining_Type), then the
 * font makes it, through its isol, fina, medi and init features applied in their stages and rlig after them, as the
 * OpenType Arabic shaping model says; a font without those features gets the Unicode presentation forms it carries,
 * lam alef as their ligature. Applied to text in its logical order (before right-to-left text is turned around). */
#include "text/fontInternal.h"

#include <string.h>

/* ---- joining --------------------------------------------------------------------------------------------------- */

typedef enum JoiningType { JT_U, JT_R, JT_L, JT_D, JT_C, JT_T } JoiningType;

typedef struct JoiningRange { uint32_t first, last; uint8_t type; } JoiningRange;

static const JoiningRange kJoining[] = {
#include "joiningData.inc"
};

static JoiningType joiningOf(uint32_t cp) {
    if (cp < 0x600) return cp == 0xAD ? JT_T : JT_U; /* (below Arabic: only the soft hyphen joins nothing, transparently) */
    size_t lo = 0, hi = CSCD_COUNT(kJoining);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (cp < kJoining[mid].first) hi = mid;
        else if (cp > kJoining[mid].last) lo = mid + 1;
        else return (JoiningType)kJoining[mid].type;
    }
    return JT_U;
}

enum { FORM_NONE, FORM_ISOL, FORM_FINA, FORM_MEDI, FORM_INIT };

/* the form of each of the n characters (FORM_NONE: one that has none, a mark, a space...) */
static void joiningForms(const uint32_t* cps, size_t n, uint8_t* forms) {
    size_t last = SIZE_MAX; /* (the last character that is not transparent) */
    JoiningType lastType = JT_U;
    for (size_t i = 0; i < n; ++i) {
        const JoiningType t = joiningOf(cps[i]);
        forms[i] = FORM_NONE;
        if (t == JT_T) continue;
        const bool joinsBefore = (t == JT_R || t == JT_D || t == JT_C) && last != SIZE_MAX &&
                                 (lastType == JT_L || lastType == JT_D || lastType == JT_C);
        if (joinsBefore && last != SIZE_MAX) { /* (the one before joins on: its form says so) */
            if (forms[last] == FORM_ISOL) forms[last] = FORM_INIT;
            else if (forms[last] == FORM_FINA) forms[last] = FORM_MEDI;
        }
        if (t == JT_R || t == JT_D || t == JT_L) forms[i] = joinsBefore ? FORM_FINA : FORM_ISOL;
        last = i;
        lastType = t;
    }
}

/* ---- through the font's features ----------------------------------------------------------------------------- */

#define MASK_ISOL (1u << 1)
#define MASK_FINA (1u << 2)
#define MASK_MEDI (1u << 3)
#define MASK_INIT (1u << 4)
#define MASK_FORMS (MASK_ISOL | MASK_FINA | MASK_MEDI | MASK_INIT)

static uint32_t tagOf(const char* s) { return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | (uint32_t)s[3]; }

static bool isArabicScript(uint32_t script) { return script == tagOf("arab"); }

/* the features of a stage among those asked for (what the stage's tags name), on into out */
static size_t pick(const CscdOtFeature* features, size_t n, const char* const* tags, size_t ntags, bool except, CscdOtFeature* out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < n && k < cap; ++i) {
        bool named = false;
        for (size_t t = 0; t < ntags && !named; ++t) named = features[i].tag == tagOf(tags[t]);
        if (named != except) out[k++] = features[i];
    }
    return k;
}

bool cscdArabicShape(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const uint32_t* cps, size_t ncps, const CscdOtFeature* features,
                     size_t n) {
    if (!isArabicScript(script) || !buf->len) return false;
    if (!cscdOtHasFeature(face, script, tagOf("init")) && !cscdOtHasFeature(face, script, tagOf("medi")) &&
        !cscdOtHasFeature(face, script, tagOf("fina")) && !cscdOtHasFeature(face, script, tagOf("isol")))
        return false;
    uint8_t* forms = (uint8_t*)cscdMalloc(ncps ? ncps : 1);
    joiningForms(cps, ncps, forms);
    static const uint32_t kMask[] = {0, MASK_ISOL, MASK_FINA, MASK_MEDI, MASK_INIT};
    for (size_t i = 0; i < buf->len; ++i) {
        const uint32_t c = buf->items[i].cluster;
        buf->items[i].mask = (~MASK_FORMS) | (c < ncps ? kMask[forms[c]] : 0);
    }
    cscdFree(forms);
    /* the stages: what composes and the language's forms; each joining form alone; the required ligatures; the rest */
    CscdOtFeature stage[64];
    static const char* const kFirst[] = {"ccmp", "locl"};
    size_t k = pick(features, n, kFirst, 2, false, stage, 64);
    if (k) cscdOtSubstitute(face, script, buf, stage, k);
    static const char* const kForms[] = {"isol", "fina", "medi", "init"};
    static const uint32_t kFormMask[] = {MASK_ISOL, MASK_FINA, MASK_MEDI, MASK_INIT};
    for (int f = 0; f < 4; ++f) {
        const CscdOtFeature one = {tagOf(kForms[f]), 1, kFormMask[f]};
        cscdOtSubstitute(face, script, buf, &one, 1);
    }
    const CscdOtFeature rlig = {tagOf("rlig"), 1, ~0u};
    cscdOtSubstitute(face, script, buf, &rlig, 1);
    static const char* const kDone[] = {"ccmp", "locl", "rlig", "isol", "fina", "medi", "init"};
    k = pick(features, n, kDone, CSCD_COUNT(kDone), true, stage, 64);
    if (k) cscdOtSubstitute(face, script, buf, stage, k);
    return true;
}

/* ---- the Unicode presentation forms -------------------------------------------------------------------------- */

typedef struct ArabicForms { uint16_t letter, isolated; uint8_t forms; } ArabicForms; /* forms: 1 (none), 2 (right-joining), 4 (dual) */
static const ArabicForms kArabic[] = {
    {0x0621, 0xFE80, 1}, {0x0622, 0xFE81, 2}, {0x0623, 0xFE83, 2}, {0x0624, 0xFE85, 2}, {0x0625, 0xFE87, 2}, {0x0626, 0xFE89, 4},
    {0x0627, 0xFE8D, 2}, {0x0628, 0xFE8F, 4}, {0x0629, 0xFE93, 2}, {0x062A, 0xFE95, 4}, {0x062B, 0xFE99, 4}, {0x062C, 0xFE9D, 4},
    {0x062D, 0xFEA1, 4}, {0x062E, 0xFEA5, 4}, {0x062F, 0xFEA9, 2}, {0x0630, 0xFEAB, 2}, {0x0631, 0xFEAD, 2}, {0x0632, 0xFEAF, 2},
    {0x0633, 0xFEB1, 4}, {0x0634, 0xFEB5, 4}, {0x0635, 0xFEB9, 4}, {0x0636, 0xFEBD, 4}, {0x0637, 0xFEC1, 4}, {0x0638, 0xFEC5, 4},
    {0x0639, 0xFEC9, 4}, {0x063A, 0xFECD, 4}, {0x0641, 0xFED1, 4}, {0x0642, 0xFED5, 4}, {0x0643, 0xFED9, 4}, {0x0644, 0xFEDD, 4},
    {0x0645, 0xFEE1, 4}, {0x0646, 0xFEE5, 4}, {0x0647, 0xFEE9, 4}, {0x0648, 0xFEED, 2}, {0x0649, 0xFEEF, 2}, {0x064A, 0xFEF1, 4},
    {0x067E, 0xFB56, 4}, {0x0686, 0xFB7A, 4}, {0x0698, 0xFB8A, 2}, {0x06A9, 0xFB8E, 4}, {0x06AF, 0xFB92, 4}, {0x06CC, 0xFBFC, 4},
};

static const ArabicForms* arabicForms(uint32_t cp) {
    for (size_t i = 0; i < CSCD_COUNT(kArabic); ++i)
        if (kArabic[i].letter == cp) return &kArabic[i];
    return NULL;
}

/* the presentation form of a letter in a form (0: none) */
static uint32_t presentationOf(uint32_t cp, int form) {
    const ArabicForms* f = arabicForms(cp);
    if (!f || form == FORM_NONE) return 0;
    const int offset = form == FORM_ISOL ? 0 : form == FORM_FINA ? 1 : form == FORM_INIT ? 2 : 3;
    if (f->forms == 1 && offset) return 0;
    if (f->forms == 2 && offset > 1) return f->isolated + 1; /* (a right-joining letter: final where it would be medial) */
    return f->isolated + (uint32_t)offset;
}

void cscdArabicPresentationForms(const CscdFontFace* face, uint32_t script, CscdOtBuffer* buf, const uint32_t* cps, size_t ncps) {
    if (!isArabicScript(script) || !buf->len) return;
    uint8_t* forms = (uint8_t*)cscdMalloc(ncps ? ncps : 1);
    joiningForms(cps, ncps, forms);
    size_t w = 0;
    for (size_t i = 0; i < buf->len; ++i) {
        CscdOtGlyph g = buf->items[i];
        const uint32_t c = g.cluster;
        if (c < ncps) {
            const uint32_t cp = cps[c];
            /* lam alef: their ligature, the alef's glyph gone (the next glyph that is not a mark) */
            size_t a = i + 1;
            while (a < buf->len && buf->items[a].cluster < ncps && joiningOf(cps[buf->items[a].cluster]) == JT_T) ++a;
            const uint32_t alef = a < buf->len && buf->items[a].cluster < ncps ? cps[buf->items[a].cluster] : 0;
            const uint32_t lig = cp != 0x0644 ? 0 : alef == 0x0622 ? 0xFEF5 : alef == 0x0623 ? 0xFEF7 : alef == 0x0625 ? 0xFEF9 : alef == 0x0627 ? 0xFEFB : 0;
            const uint16_t ligGlyph = lig ? cscdFontFaceGlyphIndex(face, lig + (forms[c] == FORM_FINA || forms[c] == FORM_MEDI ? 1 : 0)) : 0;
            if (ligGlyph) {
                g.glyph = ligGlyph;
                g.gclass = 0;
                buf->items[w++] = g;
                for (size_t k = i + 1; k < a; ++k) buf->items[w++] = buf->items[k]; /* (the marks between them stay) */
                i = a;
                continue;
            }
            const uint32_t shown = presentationOf(cp, forms[c]);
            const uint16_t glyph = shown ? cscdFontFaceGlyphIndex(face, shown) : 0;
            if (glyph) {
                g.glyph = glyph;
                g.gclass = 0;
            }
        }
        buf->items[w++] = g;
    }
    buf->len = w;
    cscdFree(forms);
}

/* ---- the joined text, as presentation forms (the public helper, for a program's own rendering) ---------------- */

bool cscdArabicJoin(CscdSv text, CscdStr* out, CscdU32Vec* map) {
    bool any = false;
    for (size_t i = 0; i < text.len && !any; ++i) any = (unsigned char)text.ptr[i] >= 0xD8 && (unsigned char)text.ptr[i] <= 0xDB;
    if (!any) return false;
    CscdU32Vec cps = {0}, at = {0};
    for (size_t i = 0; i < text.len;) {
        cscdVecPush(&at, (uint32_t)i);
        cscdVecPush(&cps, cscdUtf8Next(text, &i));
    }
    uint8_t* forms = (uint8_t*)cscdMalloc(cps.len ? cps.len : 1);
    joiningForms(cps.items, cps.len, forms);
    for (size_t i = 0; i < cps.len; ++i) {
        const uint32_t cp = cps.items[i];
        uint32_t shown = cp;
        size_t used = 1;
        if (cp == 0x0644) { /* lam alef */
            size_t nx = i + 1;
            while (nx < cps.len && joiningOf(cps.items[nx]) == JT_T) ++nx;
            const uint32_t a = nx < cps.len ? cps.items[nx] : 0;
            const uint32_t lig = a == 0x0622 ? 0xFEF5 : a == 0x0623 ? 0xFEF7 : a == 0x0625 ? 0xFEF9 : a == 0x0627 ? 0xFEFB : 0;
            if (lig) {
                shown = lig + (forms[i] == FORM_FINA || forms[i] == FORM_MEDI ? 1 : 0);
                used = nx - i + 1;
            }
        }
        if (used == 1) {
            const uint32_t p = presentationOf(cp, forms[i]);
            if (p) shown = p;
        }
        const size_t from = out->len;
        cscdUtf8Append(out, shown);
        for (size_t b = from; b < out->len; ++b) cscdVecPush(map, at.items[i]);
        i += used - 1;
    }
    cscdFree(forms);
    cscdVecFree(&cps);
    cscdVecFree(&at);
    return true;
}
