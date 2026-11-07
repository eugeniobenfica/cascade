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
void cscdYuvFree(CscdYuv* yuv);

#endif
