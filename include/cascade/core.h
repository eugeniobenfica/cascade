/* cascade — core types shared by every module: memory, geometry, colors, strings, growable
 * arrays, hash maps, reference counting, UTF-8 and the file helpers. Plain C11, no dependencies
 * beyond the C standard library.
 *
 * Conventions: types are CscdPascalCase, functions cscdCamelCase, macros and enum values CSCD_UPPER.
 * CscdSv is a borrowed view of text (pointer and length, not NUL-terminated); CscdStr owns its text
 * (always NUL-terminated once it holds any). A function that returns a pointer says whether the
 * caller owns it; everything else is borrowed. */
#ifndef CASCADE_CORE_H
#define CASCADE_CORE_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version of these headers (CMakeLists.txt's project(), which CI checks them against). */
#define CSCD_VERSION_MAJOR 1
#define CSCD_VERSION_MINOR 0
#define CSCD_VERSION_PATCH 0

#define CSCD_INLINE static inline

#if defined(__GNUC__) || defined(__clang__)
#define CSCD_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define CSCD_UNUSED __attribute__((unused))
#else
#define CSCD_PRINTF(f, a)
#define CSCD_UNUSED
#endif

/* ---- memory ------------------------------------------------------------------------------ */

/* The allocator every module uses. Running out of memory aborts: no caller checks for NULL. */
void* cscdMalloc(size_t size);
void* cscdCalloc(size_t count, size_t size);
void* cscdRealloc(void* p, size_t size);
void cscdFree(void* p);
typedef void (*CscdFreeFn)(void* p);

#define CSCD_NEW(T) ((T*)cscdCalloc(1, sizeof(T)))
#define CSCD_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* the smaller and the larger (the first operand is kept when one is NaN) */
CSCD_INLINE float cscdMinf(float a, float b) { return b < a ? b : a; }
CSCD_INLINE float cscdMaxf(float a, float b) { return a < b ? b : a; }
CSCD_INLINE float cscdClampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Math that gives the same bits on every platform (the C libraries' sinf, powf... differ in their
 * last bits, and so would the pixels): IEEE double arithmetic with fixed reductions and polynomials,
 * close to the correctly rounded result. sqrtf, floorf and the like are exact everywhere already. */
float cscdSinf(float x);
float cscdCosf(float x);
float cscdTanf(float x);
float cscdAtan2f(float y, float x);
float cscdExpf(float x);
float cscdLogf(float x);
float cscdPowf(float x, float y);
float cscdHypotf(float x, float y);
CSCD_INLINE int cscdMini(int a, int b) { return a < b ? a : b; }
CSCD_INLINE int cscdMaxi(int a, int b) { return a > b ? a : b; }
CSCD_INLINE int cscdClampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* A number of a page's as an int: what an int cannot hold stops at its ends, and NaN is 0. */
CSCD_INLINE int cscdIntOfDouble(double v) { return v >= 2147483647.0 ? 2147483647 : v <= -2147483648.0 ? (-2147483647 - 1) : v == v ? (int)v : 0; }
/* A coordinate as an int, the same on every processor: what is beyond +-2^28 stops there (a float
 * has no whole pixels left so far out, and a few such numbers still add up inside an int), and NaN
 * is 0. */
CSCD_INLINE int cscdIntOf(float v) { return v >= 268435456.f ? 268435456 : v <= -268435456.f ? -268435456 : v == v ? (int)v : 0; }
CSCD_INLINE size_t cscdMinz(size_t a, size_t b) { return a < b ? a : b; }
CSCD_INLINE size_t cscdMaxz(size_t a, size_t b) { return a > b ? a : b; }

/* Reference counted blocks (shared ownership): cscdRcNew gives size zeroed bytes with a count of
 * one; cscdRelease drops one and, at zero, calls dtor (which frees what the block holds, not the
 * block) and frees it. The count is atomic: blocks may be shared between threads. */
void* cscdRcNew(size_t size, CscdFreeFn dtor);
void* cscdRetain(void* p); /* returns p; NULL is allowed */
void cscdRelease(void* p); /* NULL is allowed */
int cscdRcCount(const void* p);

/* ---- geometry ---------------------------------------------------------------------------- */

typedef struct CscdPoint { float x, y; } CscdPoint;
typedef struct CscdSize { float w, h; } CscdSize;
typedef struct CscdRect { float x, y, w, h; } CscdRect;
typedef struct CscdIRect { int x, y, w, h; } CscdIRect;

CSCD_INLINE CscdPoint cscdPoint(float x, float y) { CscdPoint p = {x, y}; return p; }
CSCD_INLINE CscdRect cscdRect(float x, float y, float w, float h) { CscdRect r = {x, y, w, h}; return r; }
CSCD_INLINE CscdIRect cscdIRect(int x, int y, int w, int h) { CscdIRect r = {x, y, w, h}; return r; }
CSCD_INLINE float cscdRectRight(CscdRect r) { return r.x + r.w; }
CSCD_INLINE float cscdRectBottom(CscdRect r) { return r.y + r.h; }
CSCD_INLINE bool cscdRectEmpty(CscdRect r) { return r.w <= 0 || r.h <= 0; }
CSCD_INLINE bool cscdIRectEmpty(CscdIRect r) { return r.w <= 0 || r.h <= 0; }
CSCD_INLINE bool cscdRectContains(CscdRect r, float px, float py) { return px >= r.x && py >= r.y && px < r.x + r.w && py < r.y + r.h; }
CSCD_INLINE CscdRect cscdRectInset(CscdRect r, float l, float t, float rr, float b) { return cscdRect(r.x + l, r.y + t, r.w - l - rr, r.h - t - b); }
CSCD_INLINE CscdRect cscdRectTranslated(CscdRect r, float dx, float dy) { return cscdRect(r.x + dx, r.y + dy, r.w, r.h); }
CSCD_INLINE CscdRect cscdRectIntersect(CscdRect a, CscdRect o) {
    float l = cscdMaxf(a.x, o.x), t = cscdMaxf(a.y, o.y);
    float r = cscdMinf(cscdRectRight(a), cscdRectRight(o)), b = cscdMinf(cscdRectBottom(a), cscdRectBottom(o));
    return cscdRect(l, t, cscdMaxf(0.f, r - l), cscdMaxf(0.f, b - t));
}
CSCD_INLINE CscdRect cscdRectUnite(CscdRect a, CscdRect o) {
    if (cscdRectEmpty(a)) return o;
    if (cscdRectEmpty(o)) return a;
    float l = cscdMinf(a.x, o.x), t = cscdMinf(a.y, o.y);
    float r = cscdMaxf(cscdRectRight(a), cscdRectRight(o)), b = cscdMaxf(cscdRectBottom(a), cscdRectBottom(o));
    return cscdRect(l, t, r - l, b - t);
}

/* 2D affine transform: [a c e; b d f; 0 0 1], like CSS matrix(a, b, c, d, e, f). */
typedef struct CscdMatrix { float a, b, c, d, e, f; } CscdMatrix;

CSCD_INLINE CscdMatrix cscdMatrix(float a, float b, float c, float d, float e, float f) { CscdMatrix m = {a, b, c, d, e, f}; return m; }
CSCD_INLINE CscdMatrix cscdMatIdentity(void) { return cscdMatrix(1, 0, 0, 1, 0, 0); }
CSCD_INLINE CscdMatrix cscdMatTranslate(float x, float y) { return cscdMatrix(1, 0, 0, 1, x, y); }
CSCD_INLINE CscdMatrix cscdMatScale(float sx, float sy) { return cscdMatrix(sx, 0, 0, sy, 0, 0); }
CSCD_INLINE CscdMatrix cscdMatRotate(float rad) { float cs = cscdCosf(rad), sn = cscdSinf(rad); return cscdMatrix(cs, sn, -sn, cs, 0, 0); }
CSCD_INLINE CscdMatrix cscdMatSkew(float ax, float ay) { return cscdMatrix(1, cscdTanf(ay), cscdTanf(ax), 1, 0, 0); }
/* m * o (o applied first) */
CSCD_INLINE CscdMatrix cscdMatMul(CscdMatrix m, CscdMatrix o) {
    return cscdMatrix(m.a * o.a + m.c * o.b, m.b * o.a + m.d * o.b, m.a * o.c + m.c * o.d, m.b * o.c + m.d * o.d,
                    m.a * o.e + m.c * o.f + m.e, m.b * o.e + m.d * o.f + m.f);
}
CSCD_INLINE CscdPoint cscdMatMap(CscdMatrix m, CscdPoint p) { return cscdPoint(m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f); }
CSCD_INLINE bool cscdMatIsIdentity(CscdMatrix m) { return m.a == 1 && m.b == 0 && m.c == 0 && m.d == 1 && m.e == 0 && m.f == 0; }
CSCD_INLINE bool cscdMatIsTranslate(CscdMatrix m) { return m.a == 1 && m.b == 0 && m.c == 0 && m.d == 1; }
CSCD_INLINE bool cscdMatInvert(CscdMatrix m, CscdMatrix* out) {
    float det = m.a * m.d - m.b * m.c;
    if (fabsf(det) < 1e-12f) return false;
    float id = 1.f / det;
    *out = cscdMatrix(m.d * id, -m.b * id, -m.c * id, m.a * id, (m.c * m.f - m.d * m.e) * id, (m.b * m.e - m.a * m.f) * id);
    return true;
}
CSCD_INLINE CscdRect cscdMatMapRect(CscdMatrix m, CscdRect r) {
    CscdPoint p[4] = {cscdMatMap(m, cscdPoint(r.x, r.y)), cscdMatMap(m, cscdPoint(cscdRectRight(r), r.y)),
                    cscdMatMap(m, cscdPoint(r.x, cscdRectBottom(r))), cscdMatMap(m, cscdPoint(cscdRectRight(r), cscdRectBottom(r)))};
    float l = p[0].x, t = p[0].y, rr = p[0].x, bb = p[0].y;
    for (int i = 0; i < 4; ++i) { l = cscdMinf(l, p[i].x); t = cscdMinf(t, p[i].y); rr = cscdMaxf(rr, p[i].x); bb = cscdMaxf(bb, p[i].y); }
    return cscdRect(l, t, rr - l, bb - t);
}

/* ---- color ------------------------------------------------------------------------------- */

/* Straight (non-premultiplied) 8-bit RGBA color, as CSS writes it. */
typedef struct CscdColor { uint8_t r, g, b, a; } CscdColor;

CSCD_INLINE uint8_t cscdClamp8(int v) { return (uint8_t)cscdClampi(v, 0, 255); }
CSCD_INLINE CscdColor cscdRgba(int r, int g, int b, int a) { CscdColor c = {cscdClamp8(r), cscdClamp8(g), cscdClamp8(b), cscdClamp8(a)}; return c; }
CSCD_INLINE CscdColor cscdRgb(int r, int g, int b) { return cscdRgba(r, g, b, 255); }
CSCD_INLINE CscdColor cscdColorFromFloat(float r, float g, float b, float a) {
    return cscdRgba((int)lroundf(r * 255.f), (int)lroundf(g * 255.f), (int)lroundf(b * 255.f), (int)lroundf(a * 255.f));
}
CSCD_INLINE bool cscdColorEq(CscdColor x, CscdColor y) { return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a; }
CSCD_INLINE CscdColor cscdColorWithAlpha(CscdColor c, float k) { c.a = cscdClamp8((int)lroundf(c.a * k)); return c; }
/* Premultiplied BGRA packed as the framebuffer stores it (little endian B,G,R,A). */
CSCD_INLINE uint32_t cscdColorPremul(CscdColor c) {
    uint32_t pa = c.a;
    uint32_t pr = (c.r * pa + 127) / 255, pg = (c.g * pa + 127) / 255, pb = (c.b * pa + 127) / 255;
    return pb | (pg << 8) | (pr << 16) | (pa << 24);
}
/* Interpolates in premultiplied space, like browsers do for gradients. */
CscdColor cscdColorLerp(CscdColor x, CscdColor y, float t);

/* ---- growable arrays --------------------------------------------------------------------- */

/* CSCD_VEC(T) is a struct { T* items; size_t len, cap; }. Name each kind with a typedef so that the
 * same kind is one type everywhere: typedef CSCD_VEC(float) CscdFloatVec. A zeroed one is empty. */
#define CSCD_VEC(T) struct { T* items; size_t len, cap; }

void cscdVecGrow_(void** items, size_t* cap, size_t need, size_t elemSize);

#define cscdVecReserve(v, n) cscdVecGrow_((void**)&(v)->items, &(v)->cap, (n), sizeof *(v)->items)
#define cscdVecPush(v, ...) (cscdVecReserve((v), (v)->len + 1), (v)->items[(v)->len++] = (__VA_ARGS__))
/* Appends a zeroed element and returns a pointer to it. */
#define cscdVecPushZero(v) (cscdVecReserve((v), (v)->len + 1), memset(&(v)->items[(v)->len], 0, sizeof *(v)->items), &(v)->items[(v)->len++])
#define cscdVecLast(v) ((v)->items[(v)->len - 1])
#define cscdVecPop(v) ((v)->items[--(v)->len])
#define cscdVecClear(v) ((v)->len = 0)
#define cscdVecFree(v) (cscdFree((v)->items), (v)->items = NULL, (v)->len = (v)->cap = 0)
/* New elements are zeroed. */
#define cscdVecResize(v, n) cscdVecResize_((void**)&(v)->items, &(v)->len, &(v)->cap, (n), sizeof *(v)->items)
#define cscdVecInsert(v, i, ...) (cscdVecReserve((v), (v)->len + 1), \
    memmove(&(v)->items[(i) + 1], &(v)->items[(i)], ((v)->len - (i)) * sizeof *(v)->items), (v)->len++, (v)->items[(i)] = (__VA_ARGS__))
#define cscdVecRemove(v, i) (memmove(&(v)->items[(i)], &(v)->items[(i) + 1], ((v)->len - (i) - 1) * sizeof *(v)->items), (v)->len--)
#define cscdVecRemoveRange(v, a, b) (memmove(&(v)->items[(a)], &(v)->items[(b)], ((v)->len - (b)) * sizeof *(v)->items), (v)->len -= (b) - (a))
#define cscdVecAppend(v, src, n) cscdVecAppend_((void**)&(v)->items, &(v)->len, &(v)->cap, (src), (n), sizeof *(v)->items)
/* A copy of src's elements (shallow) into dst, replacing what dst held. */
#define cscdVecCopy(dst, src) ((dst)->len = 0, cscdVecAppend((dst), (src)->items, (src)->len))
#define cscdVecForEach(T, it, v) for (T* it = (v)->items; it < (v)->items + (v)->len; ++it)

void cscdVecResize_(void** items, size_t* len, size_t* cap, size_t n, size_t elemSize);
void cscdVecAppend_(void** items, size_t* len, size_t* cap, const void* src, size_t n, size_t elemSize);

typedef CSCD_VEC(float) CscdFloatVec;
typedef CSCD_VEC(int) CscdIntVec;
typedef CSCD_VEC(uint8_t) CscdBytes;
typedef CSCD_VEC(uint16_t) CscdU16Vec;
typedef CSCD_VEC(CscdIRect) CscdIRectVec;
typedef CSCD_VEC(uint32_t) CscdU32Vec;
typedef CSCD_VEC(size_t) CscdSizeVec;
typedef CSCD_VEC(void*) CscdPtrVec;
typedef CSCD_VEC(CscdPoint) CscdPointVec;
typedef CSCD_VEC(CscdRect) CscdRectVec;

/* Sorts n elements of size bytes, keeping equal ones in their order (a merge sort). */
typedef int (*CscdCompareFn)(const void* a, const void* b, void* ctx);
void cscdSort(void* base, size_t n, size_t size, CscdCompareFn cmp, void* ctx);

/* ---- text -------------------------------------------------------------------------------- */

/* A borrowed view of text. */
typedef struct CscdSv { const char* ptr; size_t len; } CscdSv;

/* An owned, growable string, NUL-terminated once it holds anything; zeroed is empty. */
typedef struct CscdStr { char* ptr; size_t len, cap; } CscdStr;
typedef CSCD_VEC(CscdStr) CscdStrVec;
typedef CSCD_VEC(CscdSv) CscdSvVec;

#define CSCD_SV(lit) ((CscdSv){"" lit, sizeof(lit) - 1})
#define CSCD_NPOS ((size_t)-1)
/* For printf: printf("%.*s", CSCD_SVF(s)) */
#define CSCD_SVF(s) (int)(s).len, (s).ptr

CSCD_INLINE CscdSv cscdSvMake(const char* p, size_t n) { CscdSv s = {p, n}; return s; }
CSCD_INLINE CscdSv cscdSv(const char* cstr) { CscdSv s = {cstr ? cstr : "", cstr ? strlen(cstr) : 0}; return s; }
CSCD_INLINE CscdSv cscdStrSv(const CscdStr* s) { return cscdSvMake(s->ptr ? s->ptr : "", s->len); }
CSCD_INLINE const char* cscdStrC(const CscdStr* s) { return s->ptr ? s->ptr : ""; }
CSCD_INLINE bool cscdSvEmpty(CscdSv s) { return s.len == 0; }
CSCD_INLINE CscdSv cscdSvSub(CscdSv s, size_t pos, size_t n) {
    if (pos > s.len) pos = s.len;
    if (n > s.len - pos) n = s.len - pos;
    return cscdSvMake(s.ptr + pos, n);
}
CSCD_INLINE CscdSv cscdSvFrom(CscdSv s, size_t pos) { return cscdSvSub(s, pos, CSCD_NPOS); }
CSCD_INLINE bool cscdSvEq(CscdSv a, CscdSv b) { return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0); }
CSCD_INLINE bool cscdSvEqC(CscdSv a, const char* b) { return cscdSvEq(a, cscdSv(b)); }
int cscdSvCmp(CscdSv a, CscdSv b);
bool cscdSvIEq(CscdSv a, CscdSv b); /* ASCII case-insensitive */
CSCD_INLINE bool cscdSvIEqC(CscdSv a, const char* b) { return cscdSvIEq(a, cscdSv(b)); }
CSCD_INLINE bool cscdSvStarts(CscdSv s, CscdSv p) { return s.len >= p.len && (p.len == 0 || memcmp(s.ptr, p.ptr, p.len) == 0); }
CSCD_INLINE bool cscdSvStartsC(CscdSv s, const char* p) { return cscdSvStarts(s, cscdSv(p)); }
CSCD_INLINE bool cscdSvEnds(CscdSv s, CscdSv p) { return s.len >= p.len && (p.len == 0 || memcmp(s.ptr + s.len - p.len, p.ptr, p.len) == 0); }
CSCD_INLINE bool cscdSvEndsC(CscdSv s, const char* p) { return cscdSvEnds(s, cscdSv(p)); }
size_t cscdSvFind(CscdSv s, CscdSv needle, size_t from);     /* CSCD_NPOS when absent */
size_t cscdSvFindChar(CscdSv s, char c, size_t from);
size_t cscdSvRFindChar(CscdSv s, char c);
size_t cscdSvFindAny(CscdSv s, const char* chars, size_t from);
size_t cscdSvRFindAny(CscdSv s, const char* chars);
CSCD_INLINE size_t cscdSvFindC(CscdSv s, const char* needle, size_t from) { return cscdSvFind(s, cscdSv(needle), from); }
CSCD_INLINE bool cscdSvContains(CscdSv s, const char* needle) { return cscdSvFind(s, cscdSv(needle), 0) != CSCD_NPOS; }
uint32_t cscdHash(const void* data, size_t len);

CSCD_INLINE bool cscdIsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
CSCD_INLINE char cscdToLower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
CSCD_INLINE char cscdToUpper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
CSCD_INLINE bool cscdIsDigit(char c) { return c >= '0' && c <= '9'; }
CSCD_INLINE bool cscdIsAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
CSCD_INLINE bool cscdIsAlnum(char c) { return cscdIsAlpha(c) || cscdIsDigit(c); }
CSCD_INLINE bool cscdIsXDigit(char c) { return cscdIsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
CscdSv cscdSvTrim(CscdSv s);
/* The words between white space, as views of s. */
void cscdSplitSpaces(CscdSv s, CscdSvVec* out);

CscdStr cscdStrDup(CscdSv s);
CSCD_INLINE CscdStr cscdStrDupC(const char* s) { return cscdStrDup(cscdSv(s)); }
CSCD_INLINE CscdStr cscdStrCopy(const CscdStr* s) { return cscdStrDup(cscdStrSv(s)); }
void cscdStrFree(CscdStr* s);
void cscdStrReserve(CscdStr* s, size_t n);
void cscdStrClear(CscdStr* s);
void cscdStrSet(CscdStr* s, CscdSv v);
CSCD_INLINE void cscdStrSetC(CscdStr* s, const char* v) { cscdStrSet(s, cscdSv(v)); }
void cscdStrAppend(CscdStr* s, CscdSv v);
CSCD_INLINE void cscdStrAppendC(CscdStr* s, const char* v) { cscdStrAppend(s, cscdSv(v)); }
void cscdStrAppendChar(CscdStr* s, char c);
void cscdStrAppendN(CscdStr* s, char c, size_t n);
void cscdStrAppendf(CscdStr* s, const char* fmt, ...) CSCD_PRINTF(2, 3);
CscdStr cscdStrf(const char* fmt, ...) CSCD_PRINTF(1, 2);
void cscdStrInsert(CscdStr* s, size_t pos, CscdSv v);
void cscdStrErase(CscdStr* s, size_t pos, size_t n);
void cscdStrTruncate(CscdStr* s, size_t n);
/* Moves the string out, leaving src empty. */
CSCD_INLINE CscdStr cscdStrTake(CscdStr* src) { CscdStr r = *src; src->ptr = NULL; src->len = src->cap = 0; return r; }
CSCD_INLINE bool cscdStrEq(const CscdStr* a, const CscdStr* b) { return cscdSvEq(cscdStrSv(a), cscdStrSv(b)); }
CSCD_INLINE bool cscdStrEqC(const CscdStr* a, const char* b) { return cscdSvEq(cscdStrSv(a), cscdSv(b)); }
CSCD_INLINE bool cscdStrEmpty(const CscdStr* s) { return s->len == 0; }
CscdStr cscdLower(CscdSv s);
/* Frees the strings and the array. */
void cscdStrVecFree(CscdStrVec* v);
void cscdStrVecCopy(CscdStrVec* dst, const CscdStrVec* src); /* deep */
bool cscdStrVecEq(const CscdStrVec* a, const CscdStrVec* b);
/* A copy owned by the caller (cscdFree), NUL-terminated. */
char* cscdSvDupC(CscdSv s);

/* Numbers in text, as strtod reads them; consumed tells how many bytes were read (0: none). */
double cscdParseDouble(CscdSv s, size_t* consumed);
long cscdParseLong(CscdSv s, size_t* consumed);

/* ---- UTF-8 -------------------------------------------------------------------------------- */

/* Decodes one code point at s[*i] and advances *i. Invalid bytes become U+FFFD. */
CSCD_INLINE uint32_t cscdUtf8Next(CscdSv s, size_t* i) {
    unsigned char c = (unsigned char)s.ptr[(*i)++];
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (n < 0) return 0xFFFD;
    uint32_t cp = c & (0x3F >> n);
    for (int k = 0; k < n; ++k) {
        if (*i >= s.len || ((unsigned char)s.ptr[*i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((unsigned char)s.ptr[(*i)++] & 0x3F);
    }
    return cp;
}
void cscdUtf8Append(CscdStr* out, uint32_t cp);
void cscdUtf8Decode(CscdSv s, CscdU32Vec* out); /* appends */
void cscdUtf8Encode(const uint32_t* cps, size_t n, CscdStr* out); /* appends */

/* ---- hash maps ---------------------------------------------------------------------------- */

/* What a map holds per key: a pointer, a number or two floats. */
typedef union CscdMapVal { void* p; uint64_t u; int64_t i; double d; float f[2]; } CscdMapVal;

/* Text keys (copied in). */
typedef struct CscdStrMapSlot { char* key; size_t keyLen; uint32_t hash; uint32_t state; CscdMapVal val; } CscdStrMapSlot;
typedef struct CscdStrMap { CscdStrMapSlot* slots; size_t cap, len, used; } CscdStrMap;
CscdMapVal* cscdStrMapGet(const CscdStrMap* m, CscdSv key);         /* NULL when absent */
CscdMapVal* cscdStrMapPut(CscdStrMap* m, CscdSv key, bool* inserted); /* the value (zeroed when new) */
bool cscdStrMapRemove(CscdStrMap* m, CscdSv key, CscdMapVal* old);
void cscdStrMapClear(CscdStrMap* m);
void cscdStrMapFree(CscdStrMap* m);
/* Iteration: for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(m, s));) */
CscdStrMapSlot* cscdStrMapNext(const CscdStrMap* m, CscdStrMapSlot* prev);
CSCD_INLINE CscdSv cscdStrMapKey(const CscdStrMapSlot* s) { return cscdSvMake(s->key, s->keyLen); }

/* 64-bit keys: numbers or pointers (CSCD_PKEY). */
typedef struct CscdU64MapSlot { uint64_t key; uint32_t state; CscdMapVal val; } CscdU64MapSlot;
typedef struct CscdU64Map { CscdU64MapSlot* slots; size_t cap, len, used; } CscdU64Map;
#define CSCD_PKEY(p) ((uint64_t)(uintptr_t)(p))
CscdMapVal* cscdU64MapGet(const CscdU64Map* m, uint64_t key);
CscdMapVal* cscdU64MapPut(CscdU64Map* m, uint64_t key, bool* inserted);
bool cscdU64MapRemove(CscdU64Map* m, uint64_t key, CscdMapVal* old);
void cscdU64MapClear(CscdU64Map* m);
void cscdU64MapFree(CscdU64Map* m);
CscdU64MapSlot* cscdU64MapNext(const CscdU64Map* m, CscdU64MapSlot* prev);

/* ---- files --------------------------------------------------------------------------------- */

bool cscdReadFile(CscdSv path, CscdStr* out);
/* Where the program's own files come from when they are not on disk: built into the program, in an
 * archive, fetched by the program. Every file cascade reads (pages, style sheets, @import, images,
 * @font-face, cscdLoadImage) is first asked of fn, with the path as cascade would open it (a
 * relative "ui/app.css" stays relative when it is found in no folder): fn fills out (cscdStrSetC,
 * cscdStrAppend...) and returns true, or returns false and the file is read from disk. fn may be
 * called from several threads at once (fonts are read on several); NULL removes it. */
typedef bool (*CscdResourceFn)(void* user, CscdSv path, CscdStr* out);
void cscdAppSetResourceLoader(CscdResourceFn fn, void* user);
CscdStr cscdDirName(CscdSv path);
CscdStr cscdJoinPath(CscdSv base, CscdSv rel);
/* The folder of the running program. */
CscdStr cscdExecutableDir(void);
/* Where an application file is: a relative path is looked for in the folders given to
 * cscdAppAddFolder, then from the working folder, then next to the program (and in a macOS bundle's
 * Resources), so that "ui/index.html" works wherever the program is started from. Returns path
 * itself when it is found nowhere. */
CscdStr cscdAppFindFile(CscdSv path);
void cscdAppAddFolder(CscdSv dir);
/* A value that changes when the file is written (its time, to the finest the system keeps, and its
 * size); -1 when there is no such file. */
long long cscdFileStamp(CscdSv path);

/* How the program was built: cascade_app() in CMake defines CASCADE_APP_SOURCE_DIR for the
 * program, and a debug build then reads its interface from the source folder (the files you edit,
 * with live reload) instead of the copy next to the program. Programs call CSCD_APP_INIT() (the
 * program, and a debug build then reads its interface from the source folder (the files you edit,
 * with live reload) instead of the copy next to the program. The program's own files tell the
 * library at startup (a constructor function this header adds to them). */
void cscdAppSetBuild(bool debug, const char* sourceDir);
bool cscdAppDebugBuild(void);
#ifdef CASCADE_APP_SOURCE_DIR
#ifdef NDEBUG
#define CSCD_APP_DEBUG_ false
#else
#define CSCD_APP_DEBUG_ true
#endif
#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor)) static void cscdAppBuildNote_(void) { cscdAppSetBuild(CSCD_APP_DEBUG_, CASCADE_APP_SOURCE_DIR); }
#elif defined(_MSC_VER)
static void __cdecl cscdAppBuildNote_(void) { cscdAppSetBuild(CSCD_APP_DEBUG_, CASCADE_APP_SOURCE_DIR); }
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) __declspec(selectany) void(__cdecl* cscdAppBuildNotePtr_)(void) = cscdAppBuildNote_;
#pragma comment(linker, "/include:cscdAppBuildNotePtr_")
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif
