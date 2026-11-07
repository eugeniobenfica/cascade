/* The Unicode bidirectional algorithm (UAX #9): which runs of a paragraph go right to left, and the
 * order runs show in on a line. Used by inline layout. Not installed. */
#ifndef CASCADE_BIDI_INTERNAL_H
#define CASCADE_BIDI_INTERNAL_H

#include "cascade/core.h"

typedef enum CscdBidiClass {
    CSCD_BIDI_L, CSCD_BIDI_R, CSCD_BIDI_AL, CSCD_BIDI_EN, CSCD_BIDI_ES, CSCD_BIDI_ET, CSCD_BIDI_AN, CSCD_BIDI_CS, CSCD_BIDI_NSM,
    CSCD_BIDI_BN, CSCD_BIDI_B, CSCD_BIDI_S, CSCD_BIDI_WS, CSCD_BIDI_ON, CSCD_BIDI_LRE, CSCD_BIDI_LRO, CSCD_BIDI_RLE, CSCD_BIDI_RLO,
    CSCD_BIDI_PDF, CSCD_BIDI_LRI, CSCD_BIDI_RLI, CSCD_BIDI_FSI, CSCD_BIDI_PDI
} CscdBidiClass;

CscdBidiClass cscdBidiClass(uint32_t cp);
/* The character that shows in place of cp in right-to-left text ("(" for ")"); cp when none. */
uint32_t cscdBidiMirror(uint32_t cp);
/* Whether text with these code points needs the algorithm at all at paragraph level 0 (it has a
 * right-to-left letter, an Arabic digit or an explicit embedding). */
bool cscdBidiNeeded(const uint32_t* cps, size_t n);

/* The embedding level of each code point of a paragraph (rules P1 to I2; X9's removed characters
 * take the level before them). paragraphLevel: 0 left to right, 1 right to left. */
void cscdBidiLevels(const uint32_t* cps, size_t n, int paragraphLevel, uint8_t* levels);
/* Rule L1 on one line of the paragraph: separators and the white space before them and at the
 * line's end go back to the paragraph level. */
void cscdBidiLineLevels(const uint32_t* cps, size_t n, int paragraphLevel, uint8_t* levels);
/* Rule L2: the logical indices in the order they show, left to right (order is replaced). */
void cscdBidiVisualOrder(const uint8_t* levels, size_t n, CscdSizeVec* order);

#endif
