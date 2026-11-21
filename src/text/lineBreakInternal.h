/* Line breaking by the Unicode Line Breaking Algorithm (UAX #14, Unicode 16.0): where a line may break, or must,
 * between the characters of a text. Internal: the layout's inline formatting asks it; the tests run its conformance
 * file against it. */
#ifndef CASCADE_LINE_BREAK_INTERNAL_H
#define CASCADE_LINE_BREAK_INTERNAL_H

#include "cascade/core.h"

/* the classes, resolved as rule LB1 does (no AI, SA, SG, XX or CJ: AL, CM, AL, AL, NS) */
typedef enum CscdLineBreakClass {
    LB_AL, LB_BK, LB_CR, LB_LF, LB_NL, LB_SP, LB_ZW, LB_ZWJ, LB_CM, LB_WJ, LB_GL, LB_BA, LB_BB, LB_HY, LB_HH, LB_CB, LB_CL,
    LB_CP, LB_EX, LB_IN, LB_NS, LB_OP, LB_QU, LB_IS, LB_NU, LB_PO, LB_PR, LB_SY, LB_EB, LB_EM, LB_H2, LB_H3, LB_HL, LB_ID,
    LB_JL, LB_JV, LB_JT, LB_RI, LB_AK, LB_AP, LB_AS, LB_VF, LB_VI, LB_B2
} CscdLineBreakClass;

/* (flags the class comes with: East Asian width F, W or H; an initial or final quotation mark; an unassigned code
 * point that is Extended_Pictographic) */
enum { LBF_EAST_ASIAN = 1, LBF_PI = 2, LBF_PF = 4, LBF_PICTOGRAPHIC = 8 };

CscdLineBreakClass cscdLineBreakClass(uint32_t cp, uint8_t* flags);

enum { CSCD_LB_NONE = 0, CSCD_LB_ALLOWED = 1, CSCD_LB_MANDATORY = 2 };

/* Between the n code points of cps: out[i] (n + 1 of them) is what may happen before cps[i], out[n] at the end
 * (CSCD_LB_MANDATORY, rule LB3), out[0] at the start (CSCD_LB_NONE, LB2). */
void cscdLineBreaks(const uint32_t* cps, size_t n, uint8_t* out);

#endif
