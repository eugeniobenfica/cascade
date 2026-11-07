/* What the painter's sources share: polygons, the stroker, blurs and the area rasterizer. Not
 * installed. */
#ifndef CASCADE_CANVAS_INTERNAL_H
#define CASCADE_CANVAS_INTERNAL_H

#include "cascade/canvas.h"

#define CSCD_PI 3.14159265358979f

/* Polygons: lists of points. */
typedef CSCD_VEC(CscdPointVec) CscdPolyVec;
void cscdPolyVecFree(CscdPolyVec* v);
void cscdPolyVecClear(CscdPolyVec* v);
/* Appends a copy of pts. */
void cscdPolyVecAdd(CscdPolyVec* v, const CscdPoint* pts, size_t n);

/* Flattens p through m into contours (appended). */
void cscdFlattenInto(const CscdPath* p, CscdMatrix m, float tolerance, CscdPolyVec* out);

/* The stroker: the outline of a polyline as polygons (appended to out). */
void cscdStrokePolyline(const CscdPoint* in, size_t n, bool closed, const CscdStrokeStyle* st, float hw, CscdPolyVec* out);
/* The pieces a dash pattern leaves of a polyline. */
void cscdApplyDash(const CscdPoint* poly, size_t n, bool closed, const float* dash, size_t ndash, float offset, CscdPolyVec* out);

/* Three box blurs approximate a gaussian (W3C filter effects recommend this). */
void cscdGaussianBlur(CscdFloatVec* buf, int w, int h, float sigma);

/* Signed-area accumulation rasterizer (exact area coverage per pixel). A row's coverage depends
 * only on the edges and on the area's left side, never on which rows the area starts or ends at: a
 * page painted by bands of rows comes out the same as painted at once. */
typedef struct CscdAccumulator {
    int x0, y0, w, h;
    CscdFloatVec a;
} CscdAccumulator;
void cscdAccReset(CscdAccumulator* acc, int x, int y, int w, int h);
void cscdAccFree(CscdAccumulator* acc);
void cscdAccAddLine(CscdAccumulator* acc, CscdPoint p0, CscdPoint p1);
void cscdAccAddPolys(CscdAccumulator* acc, const CscdPolyVec* polys);
/* coverage of one row as bytes */
void cscdAccRow(const CscdAccumulator* acc, int y, CscdFillRule rule, uint8_t* out);

#endif
