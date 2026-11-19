/* cascade-memory-tests: the program's allocator (cscdSetAllocator) and out-of-memory handler. Every block cascade makes
 * goes through the allocator here, which counts them: a page loaded, painted and freed again and again must come back
 * to the same bytes (nothing kept that grows), and a request that fails anywhere in a frame, retried after the handler,
 * must give the same pixels. Its own program: the allocator is set before cascade allocates anything. */
#include "cascade/cascade.h"
#include "../src/core/thread.h" /* (internal: a mutex for the counts, kept by the paint threads too) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- the allocator: a size before each block; counts under a lock; a request that can be made to fail ---------- */

typedef union Header { size_t size; struct { long double ld; long long ll; void* p; } align; } Header; /* (MSVC's C has no max_align_t) */

static CscdMutex lock;
static long long liveBytes, liveBlocks, requests;
static long long failAt = -1; /* the request (counted from 1) that fails once; -1: none */
static int oomCalls;
static size_t oomSize;

static void* countedRealloc(void* user, void* p, size_t size) {
    (void)user;
    cscdMutexLock(&lock);
    const bool fail = ++requests == failAt;
    cscdMutexUnlock(&lock);
    if (fail) return NULL;
    Header* old = p ? (Header*)p - 1 : NULL;
    const size_t was = old ? old->size : 0;
    Header* h = (Header*)realloc(old, sizeof(Header) + size);
    if (!h) return NULL;
    h->size = size;
    cscdMutexLock(&lock);
    liveBytes += (long long)size - (long long)was;
    if (!old) ++liveBlocks;
    cscdMutexUnlock(&lock);
    return h + 1;
}

static void countedFree(void* user, void* p) {
    (void)user;
    Header* h = (Header*)p - 1;
    cscdMutexLock(&lock);
    liveBytes -= (long long)h->size;
    --liveBlocks;
    cscdMutexUnlock(&lock);
    free(h);
}

/* the program "frees memory of its own": the failure goes away, and the request is tried again */
static bool relieve(void* user, size_t size) {
    (void)user;
    cscdMutexLock(&lock);
    ++oomCalls;
    oomSize = size;
    failAt = -1;
    cscdMutexUnlock(&lock);
    return true;
}

static long long counted(long long* what) {
    cscdMutexLock(&lock);
    const long long n = *what;
    cscdMutexUnlock(&lock);
    return n;
}

/* ---- a page with a bit of everything a frame does: the cascade, flex, grid, text, shadows, gradients, layers ---- */

static const char kPage[] =
    "<style>"
    "body { margin: 0; font: 15px sans-serif; background: #f4f3f8; }"
    ".grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; padding: 16px; }"
    ".card { border-radius: 12px; background: white; box-shadow: 0 4px 18px rgba(0,0,0,.25); padding: 12px; }"
    ".card:nth-child(2n) { background: linear-gradient(135deg, #8b5cf6, #22d3ee); color: white; }"
    ".card:nth-child(3n) { opacity: .8; transform: rotate(2deg); }"
    ".row { display: flex; justify-content: space-between; align-items: center; }"
    "b { font-weight: 700; } i { font-style: italic; }"
    "</style>"
    "<div class=grid>"
    "<div class=card><div class=row><b>One</b><span>1</span></div><p>Some text that wraps over a line or two.</p></div>"
    "<div class=card><div class=row><b>Two</b><span>2</span></div><p>Mixed <i>styles</i> and <b>weights</b>.</p></div>"
    "<div class=card><div class=row><b>Three</b><span>3</span></div><p>Right to left: \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d.</p></div>"
    "<div class=card><div class=row><b>Four</b><span>4</span></div><input value=typed></div>"
    "<div class=card><div class=row><b>Five</b><span>5</span></div><button>Press</button></div>"
    "<div class=card><div class=row><b>Six</b><span>6</span></div><progress value=40 max=100></progress></div>"
    "</div>";

static uint64_t hashPixels(const CscdImage* img) {
    uint64_t h = 14695981039346656037ull; /* FNV-1a */
    for (int y = 0; y < img->height; ++y) {
        const uint32_t* row = cscdImageRow(img, y);
        for (int x = 0; x < img->width; ++x) h = (h ^ row[x]) * 1099511628211ull;
    }
    return h;
}

/* a whole life: made, loaded, changed, painted twice, freed */
static uint64_t frame(void) {
    CscdView* v = cscdViewNew();
    cscdViewLoadHTML(v, (CscdSv){kPage, sizeof kPage - 1}, CSCD_SV("."));
    cscdViewSetViewport(v, 640, 360, 1);
    cscdViewUpdate(v);
    CscdImage img;
    cscdImageInit(&img, 640, 360);
    cscdViewPaint(v, &img, false);
    cscdElementToggleClass(cscdViewGet(v, CSCD_SV(".card")), CSCD_SV("card"));
    cscdElementToggleClass(cscdViewGet(v, CSCD_SV(".grid > div")), CSCD_SV("card"));
    cscdViewUpdate(v);
    cscdViewPaint(v, &img, true);
    const uint64_t h = hashPixels(&img);
    cscdImageFini(&img);
    cscdViewFree(v);
    return h;
}

int main(void) {
    cscdMutexInit(&lock);
    const CscdAllocator counting = {countedRealloc, countedFree, NULL};
    CHECK(!cscdSetAllocator(&(CscdAllocator){NULL, countedFree, NULL})); /* (a function missing) */
    CHECK(cscdSetAllocator(&counting));
    cscdSetOutOfMemoryHandler(relieve, NULL);

    /* every block is the allocator's */
    void* p = cscdMalloc(100);
    CHECK(counted(&liveBlocks) == 1 && counted(&liveBytes) == 100);
    p = cscdRealloc(p, 300);
    CHECK(counted(&liveBlocks) == 1 && counted(&liveBytes) == 300);
    cscdFree(p);
    CHECK(counted(&liveBlocks) == 0 && counted(&liveBytes) == 0);
    unsigned char* z = (unsigned char*)cscdCalloc(16, 4);
    bool zeros = true;
    for (int i = 0; i < 64; ++i) zeros = zeros && z[i] == 0;
    CHECK(zeros);
    cscdFree(z);
    CHECK(!cscdSetAllocator(NULL)); /* (blocks made: too late to change it) */

    /* nothing that grows: the first lives fill the process's caches (fonts, glyphs, shadows); later ones leave the
     * same bytes behind, on one thread and on several */
    for (int threads = 1; threads <= 4; threads += 3) {
        cscdSetPaintThreads(threads);
        const uint64_t first = frame();
        frame();
        const long long settled = counted(&liveBytes), blocks = counted(&liveBlocks);
        for (int i = 0; i < 3; ++i) CHECK(frame() == first);
        CHECK(counted(&liveBytes) == settled);
        CHECK(counted(&liveBlocks) == blocks);
        if (counted(&liveBytes) != settled) printf("  %d threads: %lld bytes kept, then %lld\n", threads, settled, counted(&liveBytes));
    }

    /* a request that fails, wherever in a life it comes: the handler is told (with the size), the request is tried
     * again, and the pixels are the same */
    cscdSetPaintThreads(1);
    const long long before = counted(&requests);
    const uint64_t expected = frame();
    const long long total = counted(&requests) - before;
    CHECK(total > 1000);
    const long long step = total / 97 + 1;
    int tried = 0, same = 0, told = 0;
    for (long long n = 1; n <= total; n += n < 64 ? 1 : step) {
        cscdMutexLock(&lock);
        oomCalls = 0;
        oomSize = 0;
        failAt = requests + n;
        cscdMutexUnlock(&lock);
        const uint64_t h = frame();
        ++tried;
        same += h == expected;
        told += oomCalls == 1 && oomSize > 0;
        cscdMutexLock(&lock);
        failAt = -1;
        cscdMutexUnlock(&lock);
    }
    CHECK(same == tried);
    CHECK(told == tried);
    printf("%d failed requests retried in a frame of %lld\n", tried, total);

    cscdSetOutOfMemoryHandler(NULL, NULL);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
