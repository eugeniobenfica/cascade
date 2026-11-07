/* cascade — the 2D painter: paths, anti-aliased rasterization, gradients, images, text, shadows,
 * clipping and layers, drawing into an image. It is what the HTML renderer paints with and what
 * <canvas> painters receive, so it also serves as the replacement for QPainter / SkCanvas in
 * ported apps. */
#ifndef CASCADE_CANVAS_H
#define CASCADE_CANVAS_H

#include "cascade/core.h"
#include "cascade/image.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CscdFont CscdFont;

typedef enum CscdFillRule { CSCD_FILL_NONZERO, CSCD_FILL_EVENODD } CscdFillRule;
typedef enum CscdLineCap { CSCD_CAP_BUTT, CSCD_CAP_ROUND, CSCD_CAP_SQUARE } CscdLineCap;
typedef enum CscdLineJoin { CSCD_JOIN_MITER, CSCD_JOIN_ROUND, CSCD_JOIN_BEVEL } CscdLineJoin;
typedef enum CscdBlend {
    CSCD_BLEND_SRC_OVER, CSCD_BLEND_MULTIPLY, CSCD_BLEND_SCREEN, CSCD_BLEND_OVERLAY, CSCD_BLEND_DARKEN, CSCD_BLEND_LIGHTEN,
    CSCD_BLEND_COPY, CSCD_BLEND_DST_OUT
} CscdBlend;

/* ---- paths ------------------------------------------------------------------------------------ */

typedef enum CscdPathVerb { CSCD_VERB_MOVE, CSCD_VERB_LINE, CSCD_VERB_QUAD, CSCD_VERB_CUBIC, CSCD_VERB_CLOSE } CscdPathVerb;

/* A path owns its arrays: cscdPathFree it. A zeroed one is empty. */
typedef struct CscdPath {
    CscdBytes verbs; /* CscdPathVerb each */
    CscdPointVec points;
    CscdPoint start_;
} CscdPath;

void cscdPathFree(CscdPath* p);
void cscdPathClear(CscdPath* p);
void cscdPathCopy(CscdPath* dst, const CscdPath* src);
void cscdPathMoveTo(CscdPath* p, float x, float y);
void cscdPathLineTo(CscdPath* p, float x, float y);
void cscdPathQuadTo(CscdPath* p, float cx, float cy, float x, float y);
void cscdPathCubicTo(CscdPath* p, float c1x, float c1y, float c2x, float c2y, float x, float y);
void cscdPathArcTo(CscdPath* p, float rx, float ry, float rotationDeg, bool largeArc, bool sweep, float x, float y); /* SVG arc */
void cscdPathArc(CscdPath* p, float cx, float cy, float r, float startRad, float endRad, bool ccw);               /* canvas arc */
void cscdPathClose(CscdPath* p);
void cscdPathAddRect(CscdPath* p, CscdRect r);
void cscdPathAddRoundRect(CscdPath* p, CscdRect r, const float rx[4], const float ry[4]); /* corners TL TR BR BL */
void cscdPathAddRoundRectR(CscdPath* p, CscdRect r, float radius);
void cscdPathAddEllipse(CscdPath* p, float cx, float cy, float rx, float ry);
CSCD_INLINE void cscdPathAddCircle(CscdPath* p, float cx, float cy, float r) { cscdPathAddEllipse(p, cx, cy, r, r); }
void cscdPathAddPath(CscdPath* p, const CscdPath* src, CscdMatrix m);
CSCD_INLINE bool cscdPathEmpty(const CscdPath* p) { return p->verbs.len == 0; }
CscdRect cscdPathBounds(const CscdPath* p);
CSCD_INLINE CscdPoint cscdPathCurrent(const CscdPath* p) { return p->points.len ? cscdVecLast(&p->points) : cscdPoint(0, 0); }
/* out = p transformed by m (out is replaced). */
void cscdPathTransformed(const CscdPath* p, CscdMatrix m, CscdPath* out);
/* Calls fn for each flattened contour (polyline) in device units. */
typedef void (*CscdContourFn)(void* ctx, const CscdPoint* pts, size_t n, bool closed);
void cscdPathFlatten(const CscdPath* p, CscdMatrix m, float tolerance, CscdContourFn fn, void* ctx);

/* Parses SVG path data ("M10 10 L20 20 ..."), appending to out. */
void cscdParseSvgPath(CscdSv d, CscdPath* out);

/* ---- shaders ---------------------------------------------------------------------------------- */

typedef struct CscdGradientStop {
    float offset; /* 0..1 */
    CscdColor color;
} CscdGradientStop;
typedef CSCD_VEC(CscdGradientStop) CscdGradientStopVec;

typedef enum CscdShaderKind { CSCD_SHADER_SOLID, CSCD_SHADER_LINEAR, CSCD_SHADER_RADIAL, CSCD_SHADER_CONIC, CSCD_SHADER_IMAGE } CscdShaderKind;

/* How a fill is colored: solid, gradients or an image. A shader owns its stops and holds a
 * reference to its image: cscdShaderFini what cscdShaderLinear, cscdShaderRadial, cscdShaderImage and
 * cscdShaderCopy made (a solid one needs nothing). */
typedef struct CscdShader {
    CscdShaderKind kind;
    CscdColor color;
    CscdGradientStopVec stops;
    bool repeating;
    CscdPoint p0, p1;        /* linear: start and end in local coordinates */
    CscdPoint center;        /* radial / conic */
    float rx, ry;          /* radial radii */
    float startAngle;      /* conic, radians, 0 = up */
    CscdImage* image;        /* shared */
    CscdRect imageRect;      /* where the image is placed in local coordinates (one tile) */
    bool repeatX, repeatY;
    bool smooth;
    float opacity;
} CscdShader;

CscdShader cscdShaderSolid(CscdColor c);
/* The stops are copied. */
CscdShader cscdShaderLinear(CscdPoint a, CscdPoint b, const CscdGradientStop* stops, size_t n);
CscdShader cscdShaderRadial(CscdPoint c, float rx, float ry, const CscdGradientStop* stops, size_t n);
/* The image is retained. */
CscdShader cscdShaderImage(CscdImage* img, CscdRect dst, bool repeatX, bool repeatY);
CscdShader cscdShaderCopy(const CscdShader* s);
void cscdShaderFini(CscdShader* s);

typedef struct CscdStrokeStyle {
    float width;
    CscdLineCap cap;
    CscdLineJoin join;
    float miterLimit;
    CscdFloatVec dash; /* owned */
    float dashOffset;
} CscdStrokeStyle;

/* width 1, butt caps, miter joins, miter limit 4, no dashes */
CscdStrokeStyle cscdStrokeStyle(float width);
void cscdStrokeStyleFini(CscdStrokeStyle* s);

typedef struct CscdRRect {
    CscdRect rect;
    float rx[4]; /* TL TR BR BL */
    float ry[4];
} CscdRRect;

CSCD_INLINE CscdRRect cscdRRect(CscdRect r) { CscdRRect rr; memset(&rr, 0, sizeof rr); rr.rect = r; return rr; }
CSCD_INLINE bool cscdRRectIsRect(const CscdRRect* r) { for (int i = 0; i < 4; ++i) if (r->rx[i] > 0 && r->ry[i] > 0) return false; return true; }
void cscdRRectNormalize(CscdRRect* r); /* scales radii down when they overlap (CSS rules) */
CscdRRect cscdRRectInset(const CscdRRect* r, float t, float rr, float b, float l); /* inner border edge */

/* Coverage mask, 8 bits per pixel, over the area of the target at (x, y): nothing outside it. */
typedef struct CscdMask {
    int x, y;
    int width, height;
    CscdBytes a;
} CscdMask;

CSCD_INLINE uint8_t cscdMaskAt(const CscdMask* m, int px, int py) {
    px -= m->x;
    py -= m->y;
    return (unsigned)px < (unsigned)m->width && (unsigned)py < (unsigned)m->height ? m->a.items[(size_t)py * m->width + px] : 0;
}

/* ---- canvas ----------------------------------------------------------------------------------- */

typedef struct CscdCanvas CscdCanvas;
typedef struct CscdDisplayList CscdDisplayList;

CscdCanvas* cscdCanvasNew(CscdImage* target);
/* Draws only inside region of the target: the starting clip, and the most cscdCanvasClear fills. */
CscdCanvas* cscdCanvasNewRegion(CscdImage* target, CscdIRect region);
/* Records into list instead of drawing: target gives the size and is not touched. The matrix and
 * the clip are kept as they would be (deviceClipBounds, clipIsEmpty work), and cscdDisplayListReplay
 * draws the same pixels later. */
CscdCanvas* cscdCanvasNewRecording(CscdImage* target, CscdDisplayList* list);
void cscdCanvasFree(CscdCanvas* c);

/* The image drawn into. (A recording canvas's: the size only; its pixels are not drawn.) */
CscdImage* cscdCanvasTarget(CscdCanvas* c);
bool cscdCanvasRecording(const CscdCanvas* c);
/* Draws nothing; keeps the matrix and the clip bounds (walking a page to see where things are). */
void cscdCanvasSetDryRun(CscdCanvas* c, bool on);
bool cscdCanvasDryRun(const CscdCanvas* c);
int cscdCanvasWidth(const CscdCanvas* c);
int cscdCanvasHeight(const CscdCanvas* c);

/* state */
void cscdCanvasSave(CscdCanvas* c);
void cscdCanvasRestore(CscdCanvas* c);
int cscdCanvasSaveCount(const CscdCanvas* c);
void cscdCanvasTranslate(CscdCanvas* c, float x, float y);
void cscdCanvasScale(CscdCanvas* c, float sx, float sy);
void cscdCanvasRotate(CscdCanvas* c, float rad);
void cscdCanvasConcat(CscdCanvas* c, CscdMatrix m);
void cscdCanvasSetMatrix(CscdCanvas* c, CscdMatrix m);
CscdMatrix cscdCanvasMatrix(const CscdCanvas* c);
void cscdCanvasClipRect(CscdCanvas* c, CscdRect r);
void cscdCanvasClipRRect(CscdCanvas* c, const CscdRRect* r);
void cscdCanvasClipPath(CscdCanvas* c, const CscdPath* p, CscdFillRule rule);
CscdRect cscdCanvasDeviceClipBounds(const CscdCanvas* c);
bool cscdCanvasClipIsEmpty(const CscdCanvas* c);
/* Draws img through a 3x3 homography h (by rows) from image pixels to this canvas's pixels (a
 * perspective transform), filtered, with soft edges. The matrix is not applied. */
void cscdCanvasDrawImageProjective(CscdCanvas* c, const CscdImage* img, const float h[9]);
/* Layers: everything drawn until popLayer composites at once with opacity / blend / filters. */
void cscdCanvasPushLayer(CscdCanvas* c, float opacity, CscdBlend blend);
void cscdCanvasPopLayer(CscdCanvas* c);
typedef struct CscdFilter { float blur, brightness, grayscale, saturate, invert; } CscdFilter;
CSCD_INLINE CscdFilter cscdFilterNone(void) { CscdFilter f = {0, 1, 0, 1, 0}; return f; }
void cscdCanvasSetLayerFilter(CscdCanvas* c, const CscdFilter* f);
/* backdrop-filter: what is already drawn under shape (current coordinates) filtered in place */
void cscdCanvasFilterBackdrop(CscdCanvas* c, const CscdRRect* shape, const CscdFilter* f);
/* mask-image: the top layer's pixels multiplied by mask's alpha (an image of the target's size) */
void cscdCanvasMaskLayer(CscdCanvas* c, const CscdImage* mask);
void cscdCanvasSetGlobalAlpha(CscdCanvas* c, float a);
float cscdCanvasGlobalAlpha(const CscdCanvas* c);

/* drawing */
void cscdCanvasClear(CscdCanvas* c, CscdColor color);
void cscdCanvasFillRect(CscdCanvas* c, CscdRect r, CscdColor color);
void cscdCanvasFillRectShader(CscdCanvas* c, CscdRect r, const CscdShader* s);
void cscdCanvasFillRRect(CscdCanvas* c, const CscdRRect* r, CscdColor color);
void cscdCanvasFillRRectShader(CscdCanvas* c, const CscdRRect* r, const CscdShader* s);
void cscdCanvasFillRing(CscdCanvas* c, const CscdRRect* outer, const CscdRRect* inner, const CscdShader* s); /* area between the two */
void cscdCanvasFillPath(CscdCanvas* c, const CscdPath* p, CscdColor color, CscdFillRule rule);
void cscdCanvasFillPathShader(CscdCanvas* c, const CscdPath* p, const CscdShader* s, CscdFillRule rule);
void cscdCanvasStrokePath(CscdCanvas* c, const CscdPath* p, CscdColor color, float width);
void cscdCanvasStrokePathShader(CscdCanvas* c, const CscdPath* p, const CscdShader* s, const CscdStrokeStyle* st);
void cscdCanvasStrokeRect(CscdCanvas* c, CscdRect r, CscdColor color, float width);
void cscdCanvasDrawLine(CscdCanvas* c, CscdPoint a, CscdPoint b, CscdColor color, float width, CscdLineCap cap);
void cscdCanvasFillCircle(CscdCanvas* c, CscdPoint center, float r, CscdColor color);
void cscdCanvasDrawImage(CscdCanvas* c, const CscdImage* img, CscdRect dst, bool smooth);
void cscdCanvasDrawImageRect(CscdCanvas* c, const CscdImage* img, CscdRect src, CscdRect dst, bool smooth);
/* The same, with a shared image a recording keeps without copying it (the others copy theirs). */
void cscdCanvasDrawSharedImage(CscdCanvas* c, CscdImage* img, CscdRect src, CscdRect dst, bool smooth);
/* Box shadow of a rounded rectangle (outer or inset), CSS semantics. */
void cscdCanvasDrawBoxShadow(CscdCanvas* c, const CscdRRect* box, float dx, float dy, float blur, float spread, CscdColor color, bool inset);
/* Blurred copy of a path's coverage (text-shadow, drop shadows). */
void cscdCanvasDrawBlurredPath(CscdCanvas* c, const CscdPath* p, float blur, CscdColor color);

/* text */
void cscdCanvasDrawText(CscdCanvas* c, CscdSv utf8, float x, float baselineY, const CscdFont* font, CscdColor color);
void cscdCanvasDrawTextShader(CscdCanvas* c, CscdSv utf8, float x, float baselineY, const CscdFont* font, const CscdShader* s);
float cscdCanvasMeasureText(const CscdCanvas* c, CscdSv utf8, const CscdFont* font);
typedef struct CscdGlyph { uint16_t id; float x, y; const CscdFont* font; } CscdGlyph;
void cscdCanvasDrawGlyphs(CscdCanvas* c, const CscdGlyph* glyphs, size_t n, const CscdShader* s);

/* Low level: blends coverage for rows of the device area with a shader. */
void cscdCanvasBlendSpan(CscdCanvas* c, int x, int y, int len, const uint8_t* cov, const CscdShader* s, CscdMatrix inv);

/* ---- display lists ---------------------------------------------------------------------------- */

/* Drawing recorded by a canvas made on it, to draw later: at once into a canvas, or into an image
 * by bands of rows on several threads, which gives the same pixels as drawing directly. */
CscdDisplayList* cscdDisplayListNew(void);
void cscdDisplayListFree(CscdDisplayList* l);
void cscdDisplayListClear(CscdDisplayList* l);
bool cscdDisplayListEmpty(const CscdDisplayList* l);
size_t cscdDisplayListSize(const CscdDisplayList* l);
/* False when a call reads pixels beyond its own (a blurred layer or backdrop): such a list is drawn
 * by one thread. */
bool cscdDisplayListBandable(const CscdDisplayList* l);
void cscdDisplayListReplay(const CscdDisplayList* l, CscdCanvas* c);
/* Draws into target: the list starts on a canvas made on the whole target. threads: 0 as many as
 * the machine has (at most 8), 1 on this thread only. */
void cscdDisplayListReplayInto(const CscdDisplayList* l, CscdImage* target, int threads);

/* The threads replay uses by default (0: the machine's, at most 8). CASCADE_PAINT_THREADS in the
 * environment sets the starting value. */
void cscdSetPaintThreads(int n);
int cscdPaintThreads(void);

/* What painting keeps between frames, for the whole process: blurred shadows and the pictures of
 * glyphs. When a cache is full what goes is what has gone longest without use, a shadow that takes
 * long to blur again staying longer than a small one; nothing is ever dropped all at once. */
typedef struct CscdPaintCacheLimits {
    size_t shadowBytes; /* memory for shadows (128 MB by default); 0: none is kept */
    size_t shadowCount; /* how many shadows (1024 by default); 0: none is kept */
    size_t glyphCount;  /* how many glyphs (8192 by default, at least 256) */
} CscdPaintCacheLimits;

typedef struct CscdPaintCacheStats {
    size_t shadowCount, shadowBytes, glyphCount;  /* what is kept now */
    uint64_t shadowHits, shadowMisses;            /* found kept, or blurred; since the start */
    uint64_t shadowEvictions, glyphEvictions;     /* dropped to make room */
} CscdPaintCacheStats;

CscdPaintCacheLimits cscdPaintCacheLimits(void);
void cscdPaintCacheSetLimits(CscdPaintCacheLimits limits); /* applies at once */
CscdPaintCacheStats cscdPaintCacheStats(void);
void cscdPaintCacheClear(void); /* drops what is kept (what is being drawn stays until it is done) */

#ifdef __cplusplus
}
#endif

#endif
