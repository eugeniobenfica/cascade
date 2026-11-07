/* cascade — raster images: decoding PNG, JPEG (baseline and progressive), GIF and WebP (lossless
 * and lossy, with alpha, still and animated), BMP and encoding PNG, all implemented here (inflate,
 * deflate-stored, Huffman, IDCT, VP8). */
#ifndef CASCADE_IMAGE_H
#define CASCADE_IMAGE_H

#include "cascade/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pixels are premultiplied BGRA, the framebuffer's format, width * height of them by rows.
 * An image is either shared (made by cscdImageNew or a decoder: cscdRetain / cscdRelease it) or a value
 * the caller holds (cscdImageInit / cscdImageFini); the two are never mixed. */
typedef struct CscdImage {
    int width, height;
    uint32_t* pixels;
} CscdImage;

CscdImage* cscdImageNew(int width, int height); /* shared, transparent */
CscdImage* cscdImageClone(const CscdImage* img);  /* a shared copy */
void cscdImageInit(CscdImage* img, int width, int height);
void cscdImageFini(CscdImage* img);
/* New size, all pixels transparent. */
void cscdImageResize(CscdImage* img, int width, int height);
CSCD_INLINE uint32_t* cscdImageRow(const CscdImage* img, int y) { return img->pixels + (size_t)y * img->width; }
CSCD_INLINE bool cscdImageEmpty(const CscdImage* img) { return !img || img->width <= 0 || img->height <= 0; }
CSCD_INLINE size_t cscdImagePixelCount(const CscdImage* img) { return (size_t)img->width * img->height; }

/* Shared images, NULL when the data is not an image this decodes. */
CscdImage* cscdDecodeImage(const uint8_t* data, size_t size);
CscdImage* cscdLoadImage(CscdSv path);
/* Animations (GIF, animated WebP): every frame composed onto the whole canvas, ready to show, with
 * how long it shows. Any other image this decodes is an animation of one frame (duration 0).
 * Shared (cscdRelease); NULL when the data is not an image this decodes. */
typedef struct CscdAnimatedImage {
    int width, height;
    int frameCount;
    CscdImage** frames; /* shared images, width x height each */
    int* durations;     /* milliseconds */
    int loops;          /* how many times it plays; 0: forever */
} CscdAnimatedImage;
CscdAnimatedImage* cscdDecodeAnimatedImage(const uint8_t* data, size_t size);
CscdAnimatedImage* cscdLoadAnimatedImage(CscdSv path);
/* Writes premultiplied BGRA pixels as an RGBA PNG. */
bool cscdSavePNG(CscdSv path, const CscdImage* img);
void cscdEncodePNG(const CscdImage* img, CscdBytes* out);

/* zlib / DEFLATE, used by PNG and exposed for anyone who needs it (the output is appended). */
bool cscdInflateZlib(const uint8_t* data, size_t size, CscdBytes* out);
bool cscdInflateRaw(const uint8_t* data, size_t size, CscdBytes* out, size_t* consumed);
/* As cscdInflateZlib, but false when out would grow past maxSize bytes in all (for data from
 * elsewhere: a small stream can inflate to gigabytes). */
bool cscdInflateZlibLimit(const uint8_t* data, size_t size, CscdBytes* out, size_t maxSize);

#ifdef __cplusplus
}
#endif

#endif
