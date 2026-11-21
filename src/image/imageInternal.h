/* What the image decoders share: the lossy WebP (VP8) decoder's planes. Not installed. */
#ifndef CASCADE_IMAGE_INTERNAL_H
#define CASCADE_IMAGE_INTERNAL_H

#include "cascade/image.h"

/* A decoded VP8 key frame: 8-bit Y, U and V planes (U and V at half the size, rounded up). */
typedef struct CscdYuv {
    int width, height;
    int yStride, uvStride;
    uint8_t *y, *u, *v; /* (one block, freed by cscdYuvFree) */
} CscdYuv;

/* A VP8 key frame (the payload of a WebP "VP8 " chunk); false for anything else. */
bool cscdDecodeVP8(const uint8_t* data, size_t size, CscdYuv* out);

/* The most pixels a decoder makes an image of (100 million; a JPEG frame may claim up to 2^28 and is refused past
 * this): what a file declares beyond it is refused before anything is allocated. The fuzzers lower it, where a
 * valid image of tens of millions of pixels only takes time. */
extern size_t cscdImageMaxPixels;
void cscdYuvFree(CscdYuv* yuv);

#endif
