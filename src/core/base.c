/* The core's memory, arrays, sorting, strings, UTF-8 and hash maps, and the thread layer. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* (sched_getaffinity, CPU_COUNT, pthread_setname_np) */
#endif
#include "cascade/core.h"
#include "core/thread.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif

/* ---- memory ---------------------------------------------------------------------------------- */

static void outOfMemory(size_t size) {
    fprintf(stderr, "cascade: out of memory (%zu bytes)\n", size);
    abort();
}

void* cscdMalloc(size_t size) {
    void* p = malloc(size ? size : 1);
    if (!p) outOfMemory(size);
    return p;
}

void* cscdCalloc(size_t count, size_t size) {
    void* p = calloc(count ? count : 1, size ? size : 1);
    if (!p) outOfMemory(count * size);
    return p;
}

void* cscdRealloc(void* p, size_t size) {
    void* q = realloc(p, size ? size : 1);
    if (!q) outOfMemory(size);
    return q;
}

void cscdFree(void* p) { free(p); }

/* the count before the block, the block aligned as malloc's are (MSVC's C has no max_align_t) */
typedef union RcHeader {
    struct { volatile int refs; CscdFreeFn dtor; } h;
#if defined(_MSC_VER) && !defined(__clang__)
    struct { long double ld; long long ll; void* p; } align;
#else
    max_align_t align;
#endif
} RcHeader;

void* cscdRcNew(size_t size, CscdFreeFn dtor) {
    RcHeader* h = (RcHeader*)cscdCalloc(1, sizeof(RcHeader) + size);
    h->h.refs = 1;
    h->h.dtor = dtor;
    return h + 1;
}

void* cscdRetain(void* p) {
    if (p) cscdAtomicAdd(&((RcHeader*)p - 1)->h.refs, 1);
    return p;
}

void cscdRelease(void* p) {
    if (!p) return;
    RcHeader* h = (RcHeader*)p - 1;
    if (cscdAtomicAdd(&h->h.refs, -1) == 0) {
        if (h->h.dtor) h->h.dtor(p);
        cscdFree(h);
    }
}

int cscdRcCount(const void* p) { return p ? cscdAtomicLoad(&((RcHeader*)p - 1)->h.refs) : 0; }

/* ---- color ----------------------------------------------------------------------------------- */

CscdColor cscdColorLerp(CscdColor x, CscdColor y, float t) {
    float xa = x.a / 255.f, ya = y.a / 255.f;
    float a = xa + (ya - xa) * t;
    CscdColor out = {0, 0, 0, 0};
    if (a <= 0) return out;
#define CH(xc, yc) cscdClamp8((int)lroundf(((xc) * xa + ((yc) * ya - (xc) * xa) * t) / a))
    out.r = CH(x.r, y.r);
    out.g = CH(x.g, y.g);
    out.b = CH(x.b, y.b);
#undef CH
    out.a = cscdClamp8((int)lroundf(a * 255.f));
    return out;
}

/* ---- arrays ---------------------------------------------------------------------------------- */

void cscdVecGrow_(void** items, size_t* cap, size_t need, size_t elemSize) {
    if (need <= *cap) return;
    const size_t most = SIZE_MAX / (elemSize ? elemSize : 1); /* (n * elemSize must not wrap around) */
    if (need > most) outOfMemory(SIZE_MAX);
    size_t n = *cap ? (*cap <= most / 2 ? *cap * 2 : most) : 4;
    if (n < need) n = need;
    *items = cscdRealloc(*items, n * elemSize);
    *cap = n;
}

void cscdVecResize_(void** items, size_t* len, size_t* cap, size_t n, size_t elemSize) {
    if (n > *len) {
        cscdVecGrow_(items, cap, n, elemSize);
        memset((char*)*items + *len * elemSize, 0, (n - *len) * elemSize);
    }
    *len = n;
}

void cscdVecAppend_(void** items, size_t* len, size_t* cap, const void* src, size_t n, size_t elemSize) {
    if (!n) return;
    if (n > SIZE_MAX - *len) outOfMemory(SIZE_MAX);
    cscdVecGrow_(items, cap, *len + n, elemSize);
    memmove((char*)*items + *len * elemSize, src, n * elemSize);
    *len += n;
}

static void mergeSort(char* a, char* tmp, size_t n, size_t size, CscdCompareFn cmp, void* ctx) {
    if (n < 2) return;
    if (n <= 8) { /* insertion sort, stable */
        for (size_t i = 1; i < n; ++i) {
            size_t j = i;
            memcpy(tmp, a + i * size, size);
            while (j > 0 && cmp(a + (j - 1) * size, tmp, ctx) > 0) {
                memcpy(a + j * size, a + (j - 1) * size, size);
                --j;
            }
            memcpy(a + j * size, tmp, size);
        }
        return;
    }
    size_t h = n / 2;
    mergeSort(a, tmp, h, size, cmp, ctx);
    mergeSort(a + h * size, tmp, n - h, size, cmp, ctx);
    if (cmp(a + (h - 1) * size, a + h * size, ctx) <= 0) return;
    memcpy(tmp, a, n * size);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) {
        if (cmp(tmp + j * size, tmp + i * size, ctx) < 0) memcpy(a + k++ * size, tmp + j++ * size, size);
        else memcpy(a + k++ * size, tmp + i++ * size, size);
    }
    while (i < h) memcpy(a + k++ * size, tmp + i++ * size, size);
    while (j < n) memcpy(a + k++ * size, tmp + j++ * size, size);
}

void cscdSort(void* base, size_t n, size_t size, CscdCompareFn cmp, void* ctx) {
    if (n < 2) return;
    char stackTmp[512];
    char* tmp = n * size <= sizeof stackTmp ? stackTmp : (char*)cscdMalloc(n * size);
    mergeSort((char*)base, tmp, n, size, cmp, ctx);
    if (tmp != stackTmp) cscdFree(tmp);
}

/* ---- text ------------------------------------------------------------------------------------ */

int cscdSvCmp(CscdSv a, CscdSv b) {
    size_t n = a.len < b.len ? a.len : b.len;
    int r = n ? memcmp(a.ptr, b.ptr, n) : 0;
    if (r) return r;
    return a.len < b.len ? -1 : a.len > b.len ? 1 : 0;
}

bool cscdSvIEq(CscdSv a, CscdSv b) {
    if (a.len != b.len) return false;
    for (size_t i = 0; i < a.len; ++i)
        if (cscdToLower(a.ptr[i]) != cscdToLower(b.ptr[i])) return false;
    return true;
}

size_t cscdSvFind(CscdSv s, CscdSv needle, size_t from) {
    if (needle.len == 0) return from <= s.len ? from : CSCD_NPOS;
    if (needle.len > s.len) return CSCD_NPOS;
    for (size_t i = from; i + needle.len <= s.len; ++i)
        if (s.ptr[i] == needle.ptr[0] && memcmp(s.ptr + i, needle.ptr, needle.len) == 0) return i;
    return CSCD_NPOS;
}

size_t cscdSvFindChar(CscdSv s, char c, size_t from) {
    for (size_t i = from; i < s.len; ++i)
        if (s.ptr[i] == c) return i;
    return CSCD_NPOS;
}

size_t cscdSvRFindChar(CscdSv s, char c) {
    for (size_t i = s.len; i-- > 0;)
        if (s.ptr[i] == c) return i;
    return CSCD_NPOS;
}

size_t cscdSvFindAny(CscdSv s, const char* chars, size_t from) {
    for (size_t i = from; i < s.len; ++i)
        if (s.ptr[i] && strchr(chars, s.ptr[i])) return i;
    return CSCD_NPOS;
}

size_t cscdSvRFindAny(CscdSv s, const char* chars) {
    for (size_t i = s.len; i-- > 0;)
        if (s.ptr[i] && strchr(chars, s.ptr[i])) return i;
    return CSCD_NPOS;
}

uint32_t cscdHash(const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

CscdSv cscdSvTrim(CscdSv s) {
    size_t a = 0, b = s.len;
    while (a < b && cscdIsSpace(s.ptr[a])) ++a;
    while (b > a && cscdIsSpace(s.ptr[b - 1])) --b;
    return cscdSvMake(s.ptr + a, b - a);
}

void cscdSplitSpaces(CscdSv s, CscdSvVec* out) {
    size_t i = 0;
    while (i < s.len) {
        while (i < s.len && cscdIsSpace(s.ptr[i])) ++i;
        size_t j = i;
        while (j < s.len && !cscdIsSpace(s.ptr[j])) ++j;
        if (j > i) cscdVecPush(out, cscdSvMake(s.ptr + i, j - i));
        i = j;
    }
}

void cscdStrReserve(CscdStr* s, size_t n) {
    if (n >= SIZE_MAX / 2) outOfMemory(n);
    if (n + 1 <= s->cap) return;
    size_t c = s->cap ? s->cap * 2 : 16;
    if (c < n + 1) c = n + 1;
    s->ptr = (char*)cscdRealloc(s->ptr, c);
    s->cap = c;
}

CscdStr cscdStrDup(CscdSv v) {
    CscdStr s = {0};
    if (v.len) cscdStrAppend(&s, v);
    return s;
}

void cscdStrFree(CscdStr* s) {
    cscdFree(s->ptr);
    s->ptr = NULL;
    s->len = s->cap = 0;
}

void cscdStrClear(CscdStr* s) {
    s->len = 0;
    if (s->ptr) s->ptr[0] = 0;
}

void cscdStrSet(CscdStr* s, CscdSv v) {
    if (s->ptr && v.ptr >= s->ptr && v.ptr < s->ptr + s->cap) { /* (a view of itself) */
        CscdStr t = cscdStrDup(v);
        cscdStrFree(s);
        *s = t;
        return;
    }
    cscdStrClear(s);
    cscdStrAppend(s, v);
}

void cscdStrAppend(CscdStr* s, CscdSv v) {
    if (!v.len) return;
    if (s->ptr && v.ptr >= s->ptr && v.ptr < s->ptr + s->cap) {
        CscdStr t = cscdStrDup(v);
        cscdStrAppend(s, cscdStrSv(&t));
        cscdStrFree(&t);
        return;
    }
    cscdStrReserve(s, s->len + v.len);
    if (v.len) memcpy(s->ptr + s->len, v.ptr, v.len); /* (an empty view may have no pointer) */
    s->len += v.len;
    s->ptr[s->len] = 0;
}

void cscdStrAppendChar(CscdStr* s, char c) {
    cscdStrReserve(s, s->len + 1);
    s->ptr[s->len++] = c;
    s->ptr[s->len] = 0;
}

void cscdStrAppendN(CscdStr* s, char c, size_t n) {
    if (!n) return;
    cscdStrReserve(s, s->len + n);
    memset(s->ptr + s->len, c, n);
    s->len += n;
    s->ptr[s->len] = 0;
}

static void appendv(CscdStr* s, const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    char buf[256];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) { va_end(ap2); return; }
    if ((size_t)n < sizeof buf) {
        cscdStrAppend(s, cscdSvMake(buf, (size_t)n));
    } else {
        cscdStrReserve(s, s->len + (size_t)n);
        vsnprintf(s->ptr + s->len, (size_t)n + 1, fmt, ap2);
        s->len += (size_t)n;
    }
    va_end(ap2);
}

void cscdStrAppendf(CscdStr* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    appendv(s, fmt, ap);
    va_end(ap);
}

CscdStr cscdStrf(const char* fmt, ...) {
    CscdStr s = {0};
    va_list ap;
    va_start(ap, fmt);
    appendv(&s, fmt, ap);
    va_end(ap);
    return s;
}

void cscdStrInsert(CscdStr* s, size_t pos, CscdSv v) {
    if (!v.len) return;
    if (pos > s->len) pos = s->len;
    CscdStr t = cscdStrDup(v); /* (v may be a view of s) */
    cscdStrReserve(s, s->len + t.len);
    memmove(s->ptr + pos + t.len, s->ptr + pos, s->len - pos);
    memcpy(s->ptr + pos, t.ptr, t.len);
    s->len += t.len;
    s->ptr[s->len] = 0;
    cscdStrFree(&t);
}

void cscdStrErase(CscdStr* s, size_t pos, size_t n) {
    if (pos >= s->len) return;
    if (n > s->len - pos) n = s->len - pos;
    memmove(s->ptr + pos, s->ptr + pos + n, s->len - pos - n);
    s->len -= n;
    s->ptr[s->len] = 0;
}

void cscdStrTruncate(CscdStr* s, size_t n) {
    if (n < s->len) {
        s->len = n;
        s->ptr[n] = 0;
    }
}

CscdStr cscdLower(CscdSv v) {
    CscdStr s = cscdStrDup(v);
    for (size_t i = 0; i < s.len; ++i) s.ptr[i] = cscdToLower(s.ptr[i]);
    return s;
}

void cscdStrVecFree(CscdStrVec* v) {
    for (size_t i = 0; i < v->len; ++i) cscdStrFree(&v->items[i]);
    cscdVecFree(v);
}

void cscdStrVecCopy(CscdStrVec* dst, const CscdStrVec* src) {
    for (size_t i = 0; i < dst->len; ++i) cscdStrFree(&dst->items[i]);
    dst->len = 0;
    for (size_t i = 0; i < src->len; ++i) cscdVecPush(dst, cscdStrCopy(&src->items[i]));
}

bool cscdStrVecEq(const CscdStrVec* a, const CscdStrVec* b) {
    if (a->len != b->len) return false;
    for (size_t i = 0; i < a->len; ++i)
        if (!cscdStrEq(&a->items[i], &b->items[i])) return false;
    return true;
}

char* cscdSvDupC(CscdSv s) {
    char* p = (char*)cscdMalloc(s.len + 1);
    if (s.len) memcpy(p, s.ptr, s.len);
    p[s.len] = 0;
    return p;
}

/* ---- math the same everywhere ----------------------------------------------------------------------- */

static const double kPiOver2Hi = 1.57079632673412561417e+00, kPiOver2Lo = 6.07710050650619224932e-11;

/* x - k * pi/2 (k the nearest whole number), in two steps; k's last two bits pick the quadrant */
static double quarterTurns(double x, int* quadrant) {
    const double k = floor(x * 0.636619772367581343076 + 0.5);
    *quadrant = fabs(k) < 9e15 ? (int)((long long)k & 3) : 0; /* (beyond, or NaN: no quadrant means anything) */
    return (x - k * kPiOver2Hi) - k * kPiOver2Lo;
}

static double sinPoly(double r) { /* |r| <= pi/4 */
    const double r2 = r * r;
    return r + r * r2 * (-1.0 / 6 + r2 * (1.0 / 120 + r2 * (-1.0 / 5040 + r2 * (1.0 / 362880 + r2 * (-1.0 / 39916800 + r2 * (1.0 / 6227020800.0))))));
}

static double cosPoly(double r) {
    const double r2 = r * r;
    return 1 + r2 * (-0.5 + r2 * (1.0 / 24 + r2 * (-1.0 / 720 + r2 * (1.0 / 40320 + r2 * (-1.0 / 3628800 + r2 * (1.0 / 479001600.0))))));
}

static double sinD(double x) {
    int q;
    const double r = quarterTurns(x, &q);
    switch (q) {
    case 0: return sinPoly(r);
    case 1: return cosPoly(r);
    case 2: return -sinPoly(r);
    default: return -cosPoly(r);
    }
}

static double cosD(double x) {
    int q;
    const double r = quarterTurns(x, &q);
    switch (q) {
    case 0: return cosPoly(r);
    case 1: return -sinPoly(r);
    case 2: return -cosPoly(r);
    default: return sinPoly(r);
    }
}

float cscdSinf(float x) { return (x != x || x - x != 0) ? x - x : (float)sinD(x); }
float cscdCosf(float x) { return (x != x || x - x != 0) ? x - x : (float)cosD(x); }
float cscdTanf(float x) { return (x != x || x - x != 0) ? x - x : (float)(sinD(x) / cosD(x)); }

static double atanD(double x) { /* |x| <= 1 */
    /* atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))): an argument below tan(pi/8), then the series */
    double t = x / (1 + sqrt(1 + x * x));
    t = t / (1 + sqrt(1 + t * t));
    const double t2 = t * t;
    double sum = 0, term = t;
    for (int k = 0; k < 14; ++k) {
        sum += term / (2 * k + 1);
        term *= -t2;
    }
    return 4 * sum;
}

float cscdAtan2f(float yf, float xf) {
    const double y = yf, x = xf, pi = 3.14159265358979323846;
    if (x != x || y != y) return (float)(x + y);
    if (x == 0 && y == 0) return (float)((signbit(x) ? pi : 0) * (signbit(y) ? -1 : 1));
    const double ax = fabs(x), ay = fabs(y);
    double a = ay <= ax ? atanD(ay / ax) : pi / 2 - atanD(ax / ay);
    if (x < 0) a = pi - a;
    return (float)(y < 0 ? -a : a);
}

static double expD(double x) {
    if (x > 709) return HUGE_VAL;
    if (x < -745) return 0;
    const double ln2Hi = 6.93147180369123816490e-01, ln2Lo = 1.90821492927058770002e-10;
    const double k = floor(x * 1.44269504088896338700 + 0.5);
    const double r = (x - k * ln2Hi) - k * ln2Lo; /* |r| <= ln2 / 2 */
    double sum = 1, term = 1;
    for (int n = 1; n <= 14; ++n) {
        term *= r / n;
        sum += term;
    }
    return ldexp(sum, (int)k);
}

static double logD(double x) {
    int e;
    double m = frexp(x, &e); /* x = m 2^e, m in [0.5, 1) */
    if (m < 0.70710678118654752440) { m *= 2; --e; }
    const double s = (m - 1) / (m + 1), s2 = s * s;
    double sum = 0, term = s;
    for (int k = 0; k < 16; ++k) {
        sum += term / (2 * k + 1);
        term *= s2;
    }
    return 2 * sum + e * 0.69314718055994530942;
}

float cscdExpf(float x) { return x != x ? x : (float)expD(x); }

float cscdLogf(float x) {
    if (x != x || x < 0) return (float)NAN;
    if (x == 0) return -HUGE_VALF;
    if (x - x != 0) return x; /* (infinity) */
    return (float)logD(x);
}

float cscdPowf(float xf, float yf) {
    const double x = xf, y = yf;
    if (y == 0) return 1;
    if (x != x || y != y) return (float)(x + y);
    if (x == 1) return 1;
    const bool whole = floor(y) == y;
    if (x == 0) return y > 0 ? 0.f : HUGE_VALF;
    if (x < 0) {
        if (!whole) return (float)NAN;
        const double r = expD(y * logD(-x));
        return (float)(fmod(y, 2) != 0 ? -r : r);
    }
    return (float)expD(y * logD(x));
}

float cscdHypotf(float xf, float yf) {
    const double x = fabs((double)xf), y = fabs((double)yf);
    return (float)sqrt(x * x + y * y); /* (exact enough in double for any float pair) */
}

double cscdParseDouble(CscdSv s, size_t* consumed) {
    char buf[64];
    char* tmp = s.len < sizeof buf ? buf : (char*)cscdMalloc(s.len + 1);
    if (s.len) memcpy(tmp, s.ptr, s.len);
    tmp[s.len] = 0;
    char* end = tmp;
    double v = strtod(tmp, &end);
    if (consumed) *consumed = (size_t)(end - tmp);
    if (tmp != buf) cscdFree(tmp);
    return v;
}

long cscdParseLong(CscdSv s, size_t* consumed) {
    char buf[64];
    char* tmp = s.len < sizeof buf ? buf : (char*)cscdMalloc(s.len + 1);
    if (s.len) memcpy(tmp, s.ptr, s.len);
    tmp[s.len] = 0;
    char* end = tmp;
    long v = strtol(tmp, &end, 10);
    if (consumed) *consumed = (size_t)(end - tmp);
    if (tmp != buf) cscdFree(tmp);
    return v;
}

/* ---- UTF-8 ----------------------------------------------------------------------------------- */

void cscdUtf8Append(CscdStr* out, uint32_t cp) {
    char b[4];
    size_t n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    cscdStrAppend(out, cscdSvMake(b, n));
}

void cscdUtf8Decode(CscdSv s, CscdU32Vec* out) {
    cscdVecReserve(out, out->len + s.len);
    for (size_t i = 0; i < s.len;) cscdVecPush(out, cscdUtf8Next(s, &i));
}

void cscdUtf8Encode(const uint32_t* cps, size_t n, CscdStr* out) {
    for (size_t i = 0; i < n; ++i) cscdUtf8Append(out, cps[i]);
}

/* ---- hash maps (open addressing, linear probing) -------------------------------------------- */

enum { SLOT_EMPTY = 0, SLOT_FULL = 1, SLOT_DEAD = 2 };

static void strMapRehash(CscdStrMap* m, size_t cap) {
    CscdStrMapSlot* old = m->slots;
    size_t oldCap = m->cap;
    m->slots = (CscdStrMapSlot*)cscdCalloc(cap, sizeof(CscdStrMapSlot));
    m->cap = cap;
    m->used = m->len;
    for (size_t i = 0; i < oldCap; ++i) {
        if (old[i].state != SLOT_FULL) continue;
        size_t j = old[i].hash & (cap - 1);
        while (m->slots[j].state == SLOT_FULL) j = (j + 1) & (cap - 1);
        m->slots[j] = old[i];
    }
    cscdFree(old);
}

static CscdStrMapSlot* strMapFind(const CscdStrMap* m, CscdSv key, uint32_t h) {
    if (!m->cap) return NULL;
    size_t j = h & (m->cap - 1);
    for (;;) {
        CscdStrMapSlot* s = &m->slots[j];
        if (s->state == SLOT_EMPTY) return NULL;
        if (s->state == SLOT_FULL && s->hash == h && s->keyLen == key.len && (key.len == 0 || memcmp(s->key, key.ptr, key.len) == 0)) return s;
        j = (j + 1) & (m->cap - 1);
    }
}

CscdMapVal* cscdStrMapGet(const CscdStrMap* m, CscdSv key) {
    CscdStrMapSlot* s = strMapFind(m, key, cscdHash(key.ptr, key.len));
    return s ? &s->val : NULL;
}

CscdMapVal* cscdStrMapPut(CscdStrMap* m, CscdSv key, bool* inserted) {
    uint32_t h = cscdHash(key.ptr, key.len);
    CscdStrMapSlot* s = strMapFind(m, key, h);
    if (s) {
        if (inserted) *inserted = false;
        return &s->val;
    }
    if ((m->used + 1) * 4 >= m->cap * 3) strMapRehash(m, m->cap ? (m->len * 2 >= m->cap ? m->cap * 2 : m->cap) : 16);
    size_t j = h & (m->cap - 1);
    while (m->slots[j].state == SLOT_FULL) j = (j + 1) & (m->cap - 1);
    s = &m->slots[j];
    if (s->state == SLOT_EMPTY) m->used++;
    s->state = SLOT_FULL;
    s->hash = h;
    s->key = cscdSvDupC(key);
    s->keyLen = key.len;
    memset(&s->val, 0, sizeof s->val);
    m->len++;
    if (inserted) *inserted = true;
    return &s->val;
}

bool cscdStrMapRemove(CscdStrMap* m, CscdSv key, CscdMapVal* old) {
    CscdStrMapSlot* s = strMapFind(m, key, cscdHash(key.ptr, key.len));
    if (!s) return false;
    if (old) *old = s->val;
    cscdFree(s->key);
    s->key = NULL;
    s->state = SLOT_DEAD;
    m->len--;
    return true;
}

void cscdStrMapClear(CscdStrMap* m) {
    for (size_t i = 0; i < m->cap; ++i) {
        if (m->slots[i].state == SLOT_FULL) cscdFree(m->slots[i].key);
        m->slots[i].state = SLOT_EMPTY;
    }
    m->len = m->used = 0;
}

void cscdStrMapFree(CscdStrMap* m) {
    cscdStrMapClear(m);
    cscdFree(m->slots);
    m->slots = NULL;
    m->cap = 0;
}

CscdStrMapSlot* cscdStrMapNext(const CscdStrMap* m, CscdStrMapSlot* prev) {
    size_t i = prev ? (size_t)(prev - m->slots) + 1 : 0;
    for (; i < m->cap; ++i)
        if (m->slots[i].state == SLOT_FULL) return &m->slots[i];
    return NULL;
}

static uint32_t hash64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (uint32_t)k;
}

static void u64MapRehash(CscdU64Map* m, size_t cap) {
    CscdU64MapSlot* old = m->slots;
    size_t oldCap = m->cap;
    m->slots = (CscdU64MapSlot*)cscdCalloc(cap, sizeof(CscdU64MapSlot));
    m->cap = cap;
    m->used = m->len;
    for (size_t i = 0; i < oldCap; ++i) {
        if (old[i].state != SLOT_FULL) continue;
        size_t j = hash64(old[i].key) & (cap - 1);
        while (m->slots[j].state == SLOT_FULL) j = (j + 1) & (cap - 1);
        m->slots[j] = old[i];
    }
    cscdFree(old);
}

static CscdU64MapSlot* u64MapFind(const CscdU64Map* m, uint64_t key) {
    if (!m->cap) return NULL;
    size_t j = hash64(key) & (m->cap - 1);
    for (;;) {
        CscdU64MapSlot* s = &m->slots[j];
        if (s->state == SLOT_EMPTY) return NULL;
        if (s->state == SLOT_FULL && s->key == key) return s;
        j = (j + 1) & (m->cap - 1);
    }
}

CscdMapVal* cscdU64MapGet(const CscdU64Map* m, uint64_t key) {
    CscdU64MapSlot* s = u64MapFind(m, key);
    return s ? &s->val : NULL;
}

CscdMapVal* cscdU64MapPut(CscdU64Map* m, uint64_t key, bool* inserted) {
    CscdU64MapSlot* s = u64MapFind(m, key);
    if (s) {
        if (inserted) *inserted = false;
        return &s->val;
    }
    if ((m->used + 1) * 4 >= m->cap * 3) u64MapRehash(m, m->cap ? (m->len * 2 >= m->cap ? m->cap * 2 : m->cap) : 16);
    size_t j = hash64(key) & (m->cap - 1);
    while (m->slots[j].state == SLOT_FULL) j = (j + 1) & (m->cap - 1);
    s = &m->slots[j];
    if (s->state == SLOT_EMPTY) m->used++;
    s->state = SLOT_FULL;
    s->key = key;
    memset(&s->val, 0, sizeof s->val);
    m->len++;
    if (inserted) *inserted = true;
    return &s->val;
}

bool cscdU64MapRemove(CscdU64Map* m, uint64_t key, CscdMapVal* old) {
    CscdU64MapSlot* s = u64MapFind(m, key);
    if (!s) return false;
    if (old) *old = s->val;
    s->state = SLOT_DEAD;
    m->len--;
    return true;
}

void cscdU64MapClear(CscdU64Map* m) {
    if (m->cap) memset(m->slots, 0, m->cap * sizeof(CscdU64MapSlot));
    m->len = m->used = 0;
}

void cscdU64MapFree(CscdU64Map* m) {
    cscdFree(m->slots);
    m->slots = NULL;
    m->cap = m->len = m->used = 0;
}

CscdU64MapSlot* cscdU64MapNext(const CscdU64Map* m, CscdU64MapSlot* prev) {
    size_t i = prev ? (size_t)(prev - m->slots) + 1 : 0;
    for (; i < m->cap; ++i)
        if (m->slots[i].state == SLOT_FULL) return &m->slots[i];
    return NULL;
}

/* ---- threads --------------------------------------------------------------------------------- */

#ifdef _WIN32

void cscdMutexInit(CscdMutex* m) { InitializeSRWLock((PSRWLOCK)&m->srw); }
void cscdMutexDestroy(CscdMutex* m) { (void)m; }
void cscdMutexLock(CscdMutex* m) { AcquireSRWLockExclusive((PSRWLOCK)&m->srw); }
bool cscdMutexTryLock(CscdMutex* m) { return TryAcquireSRWLockExclusive((PSRWLOCK)&m->srw) != 0; }
void cscdMutexUnlock(CscdMutex* m) { ReleaseSRWLockExclusive((PSRWLOCK)&m->srw); }
void cscdRwLockInit(CscdRwLock* l) { InitializeSRWLock((PSRWLOCK)&l->srw); }
void cscdRwLockDestroy(CscdRwLock* l) { (void)l; }
void cscdRwLockRead(CscdRwLock* l) { AcquireSRWLockShared((PSRWLOCK)&l->srw); }
void cscdRwUnlockRead(CscdRwLock* l) { ReleaseSRWLockShared((PSRWLOCK)&l->srw); }
void cscdRwLockWrite(CscdRwLock* l) { AcquireSRWLockExclusive((PSRWLOCK)&l->srw); }
void cscdRwUnlockWrite(CscdRwLock* l) { ReleaseSRWLockExclusive((PSRWLOCK)&l->srw); }
void cscdCondInit(CscdCond* c) { InitializeConditionVariable((PCONDITION_VARIABLE)&c->cv); }
void cscdCondDestroy(CscdCond* c) { (void)c; }
void cscdCondWait(CscdCond* c, CscdMutex* m) { SleepConditionVariableSRW((PCONDITION_VARIABLE)&c->cv, (PSRWLOCK)&m->srw, INFINITE, 0); }
void cscdCondBroadcast(CscdCond* c) { WakeAllConditionVariable((PCONDITION_VARIABLE)&c->cv); }

typedef struct ThreadStart { CscdThreadFn fn; void* arg; } ThreadStart;

static DWORD WINAPI threadMain(LPVOID p) {
    ThreadStart s = *(ThreadStart*)p;
    cscdFree(p);
    s.fn(s.arg);
    return 0;
}

bool cscdThreadStart(CscdThread* t, CscdThreadFn fn, void* arg) {
    ThreadStart* s = CSCD_NEW(ThreadStart);
    s->fn = fn;
    s->arg = arg;
    t->handle = CreateThread(NULL, 0, threadMain, s, 0, NULL);
    if (!t->handle) cscdFree(s);
    return t->handle != NULL;
}

void cscdThreadJoin(CscdThread* t) {
    if (!t->handle) return;
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    t->handle = NULL;
}

unsigned cscdHardwareThreads(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
}

/* SetThreadDescription: Windows 10 1607 and later, looked up */
void cscdThreadSetName(const char* name) {
    typedef HRESULT(WINAPI * SetDescriptionFn)(HANDLE, PCWSTR);
    static SetDescriptionFn setDescription;
    static bool looked;
    if (!looked) {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        setDescription = k ? (SetDescriptionFn)(void*)GetProcAddress(k, "SetThreadDescription") : NULL;
        looked = true;
    }
    if (!setDescription) return;
    wchar_t wide[64];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64) > 0) setDescription(GetCurrentThread(), wide);
}

uintptr_t cscdThreadId(void) { return (uintptr_t)GetCurrentThreadId(); }

double cscdNowSeconds(void) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

static void yieldThread(void) { SwitchToThread(); }

#else

void cscdMutexInit(CscdMutex* m) { pthread_mutex_init(&m->m, NULL); }
void cscdMutexDestroy(CscdMutex* m) { pthread_mutex_destroy(&m->m); }
void cscdMutexLock(CscdMutex* m) { pthread_mutex_lock(&m->m); }
bool cscdMutexTryLock(CscdMutex* m) { return pthread_mutex_trylock(&m->m) == 0; }
void cscdMutexUnlock(CscdMutex* m) { pthread_mutex_unlock(&m->m); }
void cscdRwLockInit(CscdRwLock* l) { pthread_rwlock_init(&l->l, NULL); }
void cscdRwLockDestroy(CscdRwLock* l) { pthread_rwlock_destroy(&l->l); }
void cscdRwLockRead(CscdRwLock* l) { pthread_rwlock_rdlock(&l->l); }
void cscdRwUnlockRead(CscdRwLock* l) { pthread_rwlock_unlock(&l->l); }
void cscdRwLockWrite(CscdRwLock* l) { pthread_rwlock_wrlock(&l->l); }
void cscdRwUnlockWrite(CscdRwLock* l) { pthread_rwlock_unlock(&l->l); }
void cscdCondInit(CscdCond* c) { pthread_cond_init(&c->c, NULL); }
void cscdCondDestroy(CscdCond* c) { pthread_cond_destroy(&c->c); }
void cscdCondWait(CscdCond* c, CscdMutex* m) { pthread_cond_wait(&c->c, &m->m); }
void cscdCondBroadcast(CscdCond* c) { pthread_cond_broadcast(&c->c); }

typedef struct ThreadStart { CscdThreadFn fn; void* arg; } ThreadStart;

static void* threadMain(void* p) {
    ThreadStart s = *(ThreadStart*)p;
    cscdFree(p);
    s.fn(s.arg);
    return NULL;
}

bool cscdThreadStart(CscdThread* t, CscdThreadFn fn, void* arg) {
    ThreadStart* s = CSCD_NEW(ThreadStart);
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(&t->t, NULL, threadMain, s) != 0) {
        cscdFree(s);
        return false;
    }
    return true;
}

void cscdThreadJoin(CscdThread* t) { pthread_join(t->t, NULL); }

unsigned cscdHardwareThreads(void) {
#ifdef __linux__
    /* the processors the process may use: fewer than the machine's in a container or under taskset */
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0 && CPU_COUNT(&set) > 0) return (unsigned)CPU_COUNT(&set);
#endif
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1;
}

void cscdThreadSetName(const char* name) {
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__)
    char cut[16]; /* (15 bytes and the NUL: the most Linux keeps) */
    snprintf(cut, sizeof cut, "%s", name);
    pthread_setname_np(pthread_self(), cut);
#else
    (void)name;
#endif
}

uintptr_t cscdThreadId(void) { return (uintptr_t)pthread_self(); }

double cscdNowSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void yieldThread(void) { sched_yield(); }

#endif

/* (owner is read by other threads while its owner writes it: atomically) */
void cscdRecMutexLock(CscdRecMutex* m) {
    const size_t self = (size_t)cscdThreadId();
    if (cscdAtomicLoadZ(&m->owner) == self) {
        m->depth++;
        return;
    }
    cscdMutexLock(&m->m);
    cscdAtomicStoreZ(&m->owner, self);
    m->depth = 1;
}

void cscdRecMutexUnlock(CscdRecMutex* m) {
    if (--m->depth == 0) {
        cscdAtomicStoreZ(&m->owner, 0);
        cscdMutexUnlock(&m->m);
    }
}

/* The threads that come while fn runs spin a little (most calls are short), then sleep until a
 * call ends: a large shadow being blurred for the bands of a frame keeps the others waiting
 * without burning their processors. */
static CscdMutex onceMutex = CSCD_MUTEX_INIT;
static CscdCond onceDone = CSCD_COND_INIT;

void cscdCallOnceArg(CscdOnce* once, void (*fn)(void* arg), void* arg) {
    volatile int* state = (volatile int*)&once->state;
    if (cscdAtomicLoad(state) == 2) return;
    if (cscdAtomicCas(state, 0, 1)) {
        fn(arg);
        cscdMutexLock(&onceMutex);
        cscdAtomicStore(state, 2);
        cscdMutexUnlock(&onceMutex);
        cscdCondBroadcast(&onceDone);
        return;
    }
    for (int spins = 0; spins < 64; ++spins) {
        if (cscdAtomicLoad(state) == 2) return;
        yieldThread();
    }
    cscdMutexLock(&onceMutex);
    while (cscdAtomicLoad(state) != 2) cscdCondWait(&onceDone, &onceMutex);
    cscdMutexUnlock(&onceMutex);
}

/* ---- parallel loops -------------------------------------------------------------------------------
 * The workers are the same from one loop to the next, so that what each keeps per thread (the glyphs
 * it drew) stays warm; the calling thread takes part. */

enum { kMaxWorkers = 63 };

static struct {
    CscdMutex run;          /* (one loop at a time) */
    CscdMutex m;
    CscdCond wake, done;
    CscdThread workers[kMaxWorkers];
    int nworkers;
    CscdParallelFn fn;
    void* ctx;
    volatile int next;       /* the next index to take */
    int count, helpers, busy;
    unsigned generation;     /* (each loop's: a worker takes part in each once) */
} pool = {CSCD_MUTEX_INIT, CSCD_MUTEX_INIT, CSCD_COND_INIT, CSCD_COND_INIT};

static CSCD_THREAD_LOCAL bool inLoop; /* (a loop asked for inside one runs here alone) */

static void takeIndices(CscdParallelFn fn, void* ctx) {
    const bool was = inLoop;
    inLoop = true;
    for (int i; (i = cscdAtomicFetchAdd(&pool.next, 1)) < pool.count;) fn(ctx, i);
    inLoop = was;
}

static void workerMain(void* arg) {
    (void)arg;
    cscdThreadSetName("cascade worker");
    unsigned seen = 0;
    cscdMutexLock(&pool.m);
    for (;;) {
        while (!(pool.generation != seen && pool.helpers > 0)) cscdCondWait(&pool.wake, &pool.m);
        seen = pool.generation;
        --pool.helpers; /* (taken: a loop that asked for fewer threads leaves the rest asleep) */
        CscdParallelFn fn = pool.fn;
        void* ctx = pool.ctx;
        cscdMutexUnlock(&pool.m);
        takeIndices(fn, ctx);
        cscdMutexLock(&pool.m);
        if (--pool.busy == 0) cscdCondBroadcast(&pool.done);
    }
}

void cscdParallelFor(int count, int threads, CscdParallelFn fn, void* ctx) {
    if (count <= 0) return;
    threads = cscdMini(threads, count);
    if (threads <= 1 || inLoop || !cscdMutexTryLock(&pool.run)) {
        for (int i = 0; i < count; ++i) fn(ctx, i);
        return;
    }
    cscdMutexLock(&pool.m);
    while (pool.nworkers < threads - 1 && pool.nworkers < kMaxWorkers) {
        if (!cscdThreadStart(&pool.workers[pool.nworkers], workerMain, NULL)) break;
        pool.nworkers++;
    }
    pool.fn = fn;
    pool.ctx = ctx;
    pool.count = count;
    cscdAtomicStore(&pool.next, 0);
    pool.helpers = cscdMini(threads - 1, pool.nworkers);
    pool.busy = pool.helpers;
    ++pool.generation;
    cscdMutexUnlock(&pool.m);
    cscdCondBroadcast(&pool.wake);
    takeIndices(fn, ctx);
    cscdMutexLock(&pool.m);
    while (pool.busy != 0) cscdCondWait(&pool.done, &pool.m);
    pool.fn = NULL;
    cscdMutexUnlock(&pool.m);
    cscdMutexUnlock(&pool.run);
}

static void callPlain(void* fn) { (*(void (**)(void))fn)(); }

void cscdCallOnce(CscdOnce* once, void (*fn)(void)) {
    if (cscdAtomicLoad((volatile int*)&once->state) == 2) return;
    cscdCallOnceArg(once, callPlain, &fn);
}
