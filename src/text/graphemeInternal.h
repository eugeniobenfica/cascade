/* Grapheme clusters by Unicode's rules (UAX #29, Unicode 16.0): what a reader takes for one character (a letter and
 * its accents, a Hangul syllable, an emoji sequence, a flag, an Indic conjunct), which the caret steps over and
 * deleting takes whole. Internal: the view's editing asks it; the tests run its conformance file against it. */
#ifndef CASCADE_GRAPHEME_INTERNAL_H
#define CASCADE_GRAPHEME_INTERNAL_H

#include "cascade/core.h"

typedef enum CscdGraphemeClass {
    GB_OTHER, GB_CR, GB_LF, GB_CONTROL, GB_EXTEND, GB_ZWJ, GB_RI, GB_PREPEND, GB_SPACING_MARK, GB_L, GB_V, GB_T, GB_LV, GB_LVT
} CscdGraphemeClass;

/* (with the class: Extended_Pictographic, and the Indic_Conjunct_Break values) */
enum { GBF_PICTOGRAPHIC = 1, GBF_CONSONANT = 2, GBF_LINKER = 4, GBF_INCB_EXTEND = 8 };

CscdGraphemeClass cscdGraphemeClass(uint32_t cp, uint8_t* flags);

/* Between the n code points of cps: out[i] (n + 1 of them) is whether a cluster ends before cps[i] (out[0], out[n]: the
 * ends of the text, true when n > 0). */
void cscdGraphemeBreaks(const uint32_t* cps, size_t n, bool* out);

/* In UTF-8 text: the byte offset of the cluster boundary after i (s.len at the end), and before i (0 at the start). */
size_t cscdGraphemeNext(CscdSv s, size_t i);
size_t cscdGraphemePrev(CscdSv s, size_t i);

#endif
