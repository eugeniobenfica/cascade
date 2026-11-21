/* Loading a page: its HTML, style sheets, fonts and images (animated ones play), from files or
 * memory, and loading them again when their files change (live reload). */
#include "style/styleInternal.h"
#include "view/viewInternal.h"
#include "core/thread.h"

#include <stdio.h>
#include <stdlib.h>

/* An image from its frames; an animated one starts playing (the image is its first frame, and its pixels change as
 * it plays). Takes anim. */
static CscdImage* viewImageOf(CscdView* v, CscdAnimatedImage* anim) {
    if (!anim) return NULL;
    CscdImage* img = cscdImageClone(anim->frames[0]);
    if (anim->frameCount > 1) {
        PlayingImage p = {img, anim, 0, 0, viewNow(v) + anim->durations[0] / 1000.0};
        cscdVecPush(&v->playing, p);
        viewNotifyInvalidate(v);
    } else cscdRelease(anim);
    return img;
}

static void svgImageFree(SvgImage* s);
static void svgForget(CscdView* v, const CscdImage* img);

static CscdImage* viewDecodeImage(CscdView* v, const uint8_t* data, size_t size) { return viewImageOf(v, cscdDecodeAnimatedImage(data, size)); }

/* ---- the images held ------------------------------------------------------------------------------------------------
 * Each URL's image is kept while the page may show it again. Past the view's limit (cscdViewSetImageCacheLimit), the
 * images nothing shows any more (only the view holds them: no style, no box, not playing) go, those asked for longest
 * ago first, down to three quarters of the limit; one asked for again is read again. The app's own images
 * (cscdViewSetImage) stay. */

enum { kImageLimitDefault = 256 << 20, kDecodeAfter = 64 << 10 /* bytes of a file: smaller ones are decoded at once */ };

static size_t imageBytesOf(const CscdImage* img) {
    return img ? cscdImagePixelCount(img) * 4 : 0;
}

static bool imagePlaying(const CscdView* v, const CscdImage* img) {
    for (size_t i = 0; i < v->playing.len; ++i)
        if (v->playing.items[i].shown == img) return true;
    return false;
}

/* (keep: the entry just made, whose image is being handed out: never dropped here, though nothing holds it yet) */
static void imagesMakeRoom(CscdView* v, const ImageEntry* keep) {
    const size_t limit = v->imageLimit ? v->imageLimit : (size_t)kImageLimitDefault;
    if (v->imageBytes <= limit) return;
    CscdPtrVec idle = {0}; /* (slots) */
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&v->images, s));) {
        const ImageEntry* e = (const ImageEntry*)s->val.p;
        if (e != keep && e->img && !e->app && !e->decoding && cscdRcRefs(e->img) == 1 && !imagePlaying(v, e->img)) cscdVecPush(&idle, s);
    }
    /* longest unused first */
    for (size_t i = 1; i < idle.len; ++i)
        for (size_t k = i; k > 0 && ((const ImageEntry*)((CscdStrMapSlot*)idle.items[k - 1])->val.p)->used >
                                    ((const ImageEntry*)((CscdStrMapSlot*)idle.items[k])->val.p)->used; --k) {
            void* t = idle.items[k];
            idle.items[k] = idle.items[k - 1];
            idle.items[k - 1] = t;
        }
    CscdStrVec gone = {0};
    for (size_t i = 0; i < idle.len && v->imageBytes > limit / 4 * 3; ++i) {
        CscdStrMapSlot* s = (CscdStrMapSlot*)idle.items[i];
        ImageEntry* e = (ImageEntry*)s->val.p;
        v->imageBytes -= e->bytes;
        svgForget(v, e->img);
        cscdRelease(e->img);
        cscdFree(e);
        s->val.p = NULL;
        cscdVecPush(&gone, cscdStrDup(cscdStrMapKey(s)));
    }
    for (size_t i = 0; i < gone.len; ++i) {
        cscdStrMapRemove(&v->images, cscdStrSv(&gone.items[i]), NULL);
        cscdStrFree(&gone.items[i]);
    }
    cscdVecFree(&gone);
    cscdVecFree(&idle);
}

void cscdViewSetImageCacheLimit(CscdView* v, size_t bytes) {
    v->imageLimit = bytes ? bytes : 1;
    imagesMakeRoom(v, NULL);
}

size_t cscdViewImageCacheBytes(const CscdView* v) { return v->imageBytes; }

/* ---- decoding on other threads (CSCD_FEATURE_ASYNC_IMAGES) ------------------------------------------------------------
 * A large file is read here, and decoded by one of the decoder threads (two, started when first needed, kept for the
 * process): the page shows without it meanwhile, and is styled and laid out again when it comes back
 * (viewTakeDecodedImages, from cscdViewUpdate). The jobs reach the view through its inbox, which they hold a reference
 * to: a view freed meanwhile only drops its own. */

typedef struct Decoded { CscdStr url; CscdAnimatedImage* anim; } Decoded;
struct ImageInbox {
    CscdMutex lock;
    CSCD_VEC(Decoded) done;
};

static void inboxDtor(void* p) {
    ImageInbox* in = (ImageInbox*)p;
    for (size_t i = 0; i < in->done.len; ++i) {
        cscdStrFree(&in->done.items[i].url);
        cscdRelease(in->done.items[i].anim);
    }
    cscdVecFree(&in->done);
    cscdMutexDestroy(&in->lock);
}

typedef struct DecodeJob { ImageInbox* inbox; CscdStr url; CscdStr data; } DecodeJob;
static CscdMutex jobsLock = CSCD_MUTEX_INIT;
static CscdCond jobsReady = CSCD_COND_INIT;
static CSCD_VEC(DecodeJob) jobs;
static int decoders;
enum { kDecoders = 2 };

static void decoderMain(void* arg) {
    (void)arg;
    cscdThreadSetName("cascade decoder");
    for (;;) {
        cscdMutexLock(&jobsLock);
        while (!jobs.len) cscdCondWait(&jobsReady, &jobsLock);
        DecodeJob j = jobs.items[0];
        cscdVecRemove(&jobs, 0);
        cscdMutexUnlock(&jobsLock);
        Decoded d = {j.url, cscdDecodeAnimatedImage((const uint8_t*)j.data.ptr, j.data.len)};
        cscdStrFree(&j.data);
        cscdMutexLock(&j.inbox->lock);
        cscdVecPush(&j.inbox->done, d);
        cscdMutexUnlock(&j.inbox->lock);
        cscdRelease(j.inbox);
    }
}

/* data (taken) decoded on a decoder thread; false when no thread could be started (decode it here) */
static bool decodeLater(CscdView* v, CscdSv url, CscdStr* data) {
    cscdMutexLock(&jobsLock);
    while (decoders < kDecoders) {
        CscdThread t;
        if (!cscdThreadStart(&t, decoderMain, NULL)) break;
        ++decoders;
    }
    if (!decoders) {
        cscdMutexUnlock(&jobsLock);
        return false;
    }
    if (!v->inbox) {
        v->inbox = (ImageInbox*)cscdRcNew(sizeof(ImageInbox), inboxDtor);
        cscdMutexInit(&v->inbox->lock);
    }
    DecodeJob j = {(ImageInbox*)cscdRetain(v->inbox), cscdStrDup(url), *data};
    memset(data, 0, sizeof *data);
    cscdVecPush(&jobs, j);
    cscdCondBroadcast(&jobsReady);
    cscdMutexUnlock(&jobsLock);
    ++v->imagesDecoding;
    return true;
}

bool viewTakeDecodedImages(CscdView* v) {
    if (!v->imagesDecoding || !v->inbox) return false;
    cscdMutexLock(&v->inbox->lock);
    const size_t n = v->inbox->done.len;
    Decoded* got = v->inbox->done.items;
    memset(&v->inbox->done, 0, sizeof v->inbox->done);
    cscdMutexUnlock(&v->inbox->lock);
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        --v->imagesDecoding;
        CscdMapVal* m = cscdStrMapGet(&v->images, cscdStrSv(&got[i].url));
        ImageEntry* e = m ? (ImageEntry*)m->p : NULL;
        if (e && e->decoding) {
            e->decoding = false;
            e->img = viewImageOf(v, got[i].anim);
            e->bytes = imageBytesOf(e->img);
            v->imageBytes += e->bytes;
            if (!e->img) viewWarnf(v, CSCD_FEATURE_WARN_FILES, "cannot load the image %s", cscdStrC(&got[i].url));
            any = true;
        } else cscdRelease(got[i].anim); /* (a page replaced meanwhile) */
        cscdStrFree(&got[i].url);
    }
    cscdFree(got);
    if (!any) return false; /* (room is made as the next image is asked for: these are about to be shown) */
    cscdStyleEngineInvalidateCache(v->styles);
    v->styleDirty = true;
    cscdViewInvalidate(v);
    return true;
}

int cscdViewImagesDecoding(const CscdView* v) { return v->imagesDecoding; }

void viewClearImages(CscdView* v) {
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&v->images, s));) {
        ImageEntry* e = (ImageEntry*)s->val.p;
        if (e) cscdRelease(e->img);
        cscdFree(e);
    }
    cscdStrMapClear(&v->images);
    for (size_t i = 0; i < v->svgImages.len; ++i) svgImageFree((SvgImage*)v->svgImages.items[i]);
    cscdVecFree(&v->svgImages);
    v->imageBytes = 0;
    for (size_t i = 0; i < v->playing.len; ++i) cscdRelease(v->playing.items[i].anim);
    v->playing.len = 0;
}

/* The animated images whose next frame is due, a frame on; true when one changed. */
bool viewAdvanceImages(CscdView* v, double t) {
    if (!viewOn(v, CSCD_FEATURE_ANIMATIONS)) return false;
    bool changed = false;
    for (size_t i = 0; i < v->playing.len; ++i) {
        PlayingImage* p = &v->playing.items[i];
        const CscdAnimatedImage* a = p->anim;
        if (p->due > t) continue;
        if (p->frame + 1 == a->frameCount && a->loops && p->played + 1 >= a->loops) { p->due = INFINITY; continue; }
        if (t - p->due > 1) p->due = t; /* (a long pause: on from now, not every frame missed) */
        while (p->due <= t) {
            if (++p->frame == a->frameCount) { p->frame = 0; ++p->played; }
            p->due += a->durations[p->frame] / 1000.0;
            if (p->frame + 1 == a->frameCount && a->loops && p->played + 1 >= a->loops) break;
        }
        memcpy(p->shown->pixels, a->frames[p->frame]->pixels, cscdImagePixelCount(p->shown) * 4);
        changed = true;
    }
    return changed;
}

/* ---- SVG files as images -----------------------------------------------------------------------------------------
 * An SVG file (img src, background-image...) is a picture drawn at whatever size it is shown: the image the page holds
 * is a raster of it at its own size (its width and height, or what its viewBox's ratio and the default object size
 * give), for what takes pixels as they are; painting asks for it at the size it draws it (viewSvgRaster), drawn by a
 * view of its own. What it has of an intrinsic size (a width, a height, a ratio: each, or none) is the sizing's. */

/* a length as an SVG root's width or height gives it, in pixels; false for none (or a percentage) */
static bool svgLength(CscdSv v, float* out) {
    v = cscdSvTrim(v);
    if (!v.len || cscdSvEndsC(v, "%")) return false;
    char* c = cscdSvDupC(v);
    char* end = NULL;
    const double n = strtod(c, &end);
    const CscdSv unit = cscdSvTrim(cscdSv(end ? end : ""));
    cscdFree(c);
    double k = 1;
    if (cscdSvIEqC(unit, "in")) k = 96;
    else if (cscdSvIEqC(unit, "cm")) k = 96 / 2.54;
    else if (cscdSvIEqC(unit, "mm")) k = 96 / 25.4;
    else if (cscdSvIEqC(unit, "pt")) k = 96.0 / 72;
    else if (cscdSvIEqC(unit, "pc")) k = 16;
    else if (cscdSvIEqC(unit, "em")) k = 16;
    else if (unit.len && !cscdSvIEqC(unit, "px")) return false;
    if (!(n * k >= 0) || n * k > 1e6) return false;
    *out = (float)(n * k);
    return true;
}

static void svgImageFree(SvgImage* s) {
    cscdStrFree(&s->source);
    cscdStrFree(&s->baseDir);
    cscdRelease(s->raster);
    cscdFree(s);
}

/* the picture drawn w by h (pixels: transparent where it draws nothing) */
static CscdImage* svgRender(const SvgImage* s, int w, int h) {
    CscdImage* out = cscdImageNew(w, h);
    if (s->blank) return out;
    CscdDocument* x = cscdParseXHTML(cscdStrSv(&s->source));
    CscdElement* root = cscdDocumentElement(x);
    if (!root || root->ns_ != CSCD_NS_SVG || !cscdElementIs(root, "svg")) {
        cscdDocumentFree(x);
        return out;
    }
    /* (one with a width and a height but no viewBox scales as if it had one of that size, as browsers draw it) */
    if (!cscdElementHasAttributeC(root, "viewBox") && (s->has & SVG_HAS_WIDTH) && (s->has & SVG_HAS_HEIGHT) && s->w > 0 && s->h > 0) {
        CscdStr vb = cscdStrf("0 0 %g %g", (double)s->w, (double)s->h);
        cscdElementSetAttribute(root, CSCD_SV("viewBox"), cscdStrSv(&vb));
        cscdStrFree(&vb);
    }
    /* (its size is the one it is drawn at: its own width and height go) */
    cscdElementRemoveAttribute(root, CSCD_SV("width"));
    cscdElementRemoveAttribute(root, CSCD_SV("height"));
    CscdView* iv = cscdViewNew();
    cscdViewOnWarning(iv, NULL, NULL);
    cscdViewSetTransparent(iv, true);
    cscdViewSetViewport(iv, (float)w, (float)h, 1);
    /* (the picture fills the view: its width and height are the size it is drawn at, as an image's are) */
    cscdViewLoadHTML(iv, CSCD_SV("<!DOCTYPE html><style>html, body { margin: 0; padding: 0; overflow: hidden }"
                                 " body > svg { display: block; width: 100vw !important; height: 100vh !important }</style><body>"),
                     cscdStrSv(&s->baseDir));
    CscdNode* moved = cscdNodeRemoveChild(root->node.parent, &root->node);
    CscdElement* body = cscdDocumentBody(iv->doc);
    if (moved && !(body && cscdNodeAppendChild(&body->node, moved))) cscdNodeDestroy(moved);
    cscdDocumentFree(x);
    cscdViewPaint(iv, out, false);
    cscdViewFree(iv);
    return out;
}

/* An SVG file's image (NULL when it is not one): its intrinsic size read, a raster at its default size made. */
static CscdImage* svgImageOf(CscdView* v, CscdSv text, CscdSv baseDir) {
    CscdDocument* x = cscdParseXHTML(text);
    CscdElement* root = cscdDocumentElement(x);
    if (!root || root->ns_ != CSCD_NS_SVG || !cscdElementIs(root, "svg")) {
        cscdDocumentFree(x);
        return NULL;
    }
    SvgImage* s = CSCD_NEW(SvgImage);
    s->source = cscdStrDup(text);
    s->baseDir = cscdStrDup(baseDir);
    if (svgLength(cscdElementAttrC(root, "width"), &s->w)) s->has |= SVG_HAS_WIDTH;
    if (svgLength(cscdElementAttrC(root, "height"), &s->h)) s->has |= SVG_HAS_HEIGHT;
    {
        float vb[4];
        int n = 0;
        CscdSv b = cscdElementAttrC(root, "viewBox");
        while (n < 4 && b.len) {
            b = cscdSvTrim(b);
            size_t k = 0;
            while (k < b.len && b.ptr[k] != ' ' && b.ptr[k] != ',' && b.ptr[k] != '\t' && b.ptr[k] != '\n') ++k;
            char* c = cscdSvDupC(cscdSvSub(b, 0, k));
            vb[n++] = (float)atof(c);
            cscdFree(c);
            b = cscdSvFrom(b, k < b.len ? k + 1 : k);
        }
        if (n == 4 && (vb[2] <= 0 || vb[3] <= 0)) s->blank = true;
        if ((s->has & SVG_HAS_WIDTH) && (s->has & SVG_HAS_HEIGHT)) {
            if (s->w > 0 && s->h > 0) {
                s->ratio = s->w / s->h;
                s->has |= SVG_HAS_RATIO;
            }
        } else if (n == 4 && vb[2] > 0 && vb[3] > 0) {
            s->ratio = vb[2] / vb[3];
            s->has |= SVG_HAS_RATIO;
        }
    }
    cscdDocumentFree(x);
    /* its default size: what it has, the rest from its ratio or the default object size (300 by 150) */
    float w = s->w, h = s->h;
    const bool hw = s->has & SVG_HAS_WIDTH, hh = s->has & SVG_HAS_HEIGHT, hr = s->has & SVG_HAS_RATIO;
    if (!hw && !hh) {
        w = 300;
        h = hr ? w / s->ratio : 150;
    } else if (!hw) w = hr ? h * s->ratio : 300;
    else if (!hh) h = hr ? w / s->ratio : 150;
    const int iw = cscdMaxi(1, cscdMini(4096, (int)lroundf(w))), ih = cscdMaxi(1, cscdMini(4096, (int)lroundf(h)));
    s->img = svgRender(s, iw, ih);
    cscdVecPush(&v->svgImages, s);
    return s->img;
}

const SvgImage* viewSvgImage(const CscdView* v, const CscdImage* img) {
    if (!img) return NULL;
    for (size_t i = 0; i < v->svgImages.len; ++i)
        if (((const SvgImage*)v->svgImages.items[i])->img == img) return (const SvgImage*)v->svgImages.items[i];
    return NULL;
}

CscdImage* viewSvgRaster(CscdView* v, const CscdImage* img, int w, int h) {
    SvgImage* s = (SvgImage*)viewSvgImage(v, img);
    if (!s || w <= 0 || h <= 0 || (size_t)w * (size_t)h > (size_t)1 << 24) return NULL;
    if (w == img->width && h == img->height) return s->img;
    if (!s->raster || s->rw != w || s->rh != h) {
        cscdRelease(s->raster);
        s->raster = svgRender(s, w, h);
        s->rw = w;
        s->rh = h;
    }
    return s->raster;
}

static void svgForget(CscdView* v, const CscdImage* img) {
    for (size_t i = v->svgImages.len; i-- > 0;)
        if (((SvgImage*)v->svgImages.items[i])->img == img) {
            svgImageFree((SvgImage*)v->svgImages.items[i]);
            cscdVecRemove(&v->svgImages, i);
        }
}

/* whether a file or data is an SVG one (by its name or type, or by its start) */
static bool looksSvg(CscdSv url, CscdSv data) {
    if (cscdSvEndsC(url, ".svg") || cscdSvEndsC(url, ".SVG") || cscdSvStartsC(url, "data:image/svg+xml")) return true;
    const CscdSv head = cscdSvSub(data, 0, cscdMinz(data.len, 512));
    return cscdSvFind(head, CSCD_SV("<svg"), 0) != CSCD_NPOS;
}

CscdImage* viewImage(CscdView* v, CscdSv url) {
    CscdMapVal* known = cscdStrMapGet(&v->images, url);
    if (known && known->p) {
        ImageEntry* e = (ImageEntry*)known->p;
        e->used = ++v->imageClock;
        return e->img;
    }
    CscdImage* img = NULL;
    bool later = false;
    if (cscdSvStartsC(url, "data:")) {
        size_t comma = cscdSvFindChar(url, ',', 0);
        if (comma != CSCD_NPOS && cscdSvFind(cscdSvSub(url, 0, comma), CSCD_SV(";base64"), 0) != CSCD_NPOS) {
            static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            CscdBytes bytes = {0};
            uint32_t acc = 0;
            int bits = 0;
            for (size_t i = comma + 1; i < url.len; ++i) {
                const char* p = url.ptr[i] ? strchr(b64, url.ptr[i]) : NULL;
                if (!p) continue;
                acc = (acc << 6) | (uint32_t)(p - b64);
                bits += 6;
                if (bits >= 8) { bits -= 8; cscdVecPush(&bytes, (uint8_t)((acc >> bits) & 0xFF)); }
            }
            img = viewDecodeImage(v, bytes.items, bytes.len);
            if (!img && looksSvg(url, (CscdSv){(const char*)bytes.items, bytes.len})) img = svgImageOf(v, (CscdSv){(const char*)bytes.items, bytes.len}, CSCD_SV(""));
            cscdVecFree(&bytes);
        } else if (comma != CSCD_NPOS && looksSvg(url, CSCD_SV(""))) { /* (data:image/svg+xml,<svg ...>, percent-encoded) */
            CscdStr text = {0};
            for (size_t i = comma + 1; i < url.len; ++i) {
                if (url.ptr[i] == '%' && i + 2 < url.len && cscdIsXDigit(url.ptr[i + 1]) && cscdIsXDigit(url.ptr[i + 2])) {
                    const char hex[3] = {url.ptr[i + 1], url.ptr[i + 2], 0};
                    cscdStrAppendChar(&text, (char)strtol(hex, NULL, 16));
                    i += 2;
                } else cscdStrAppendChar(&text, url.ptr[i]);
            }
            img = svgImageOf(v, cscdStrSv(&text), CSCD_SV(""));
            cscdStrFree(&text);
        }
    } else {
        CscdStr file = {0};
        const bool read = cscdReadFile(url, &file);
        if (read && file.len >= kDecodeAfter && viewOn(v, CSCD_FEATURE_ASYNC_IMAGES)) later = decodeLater(v, url, &file);
        if (read && !later && looksSvg(url, cscdStrSv(&file))) {
            CscdStr dir = cscdDirName(url);
            img = svgImageOf(v, cscdStrSv(&file), cscdStrSv(&dir));
            cscdStrFree(&dir);
        }
        if (read && !later && !img) img = viewDecodeImage(v, (const uint8_t*)file.ptr, file.len);
        cscdStrFree(&file);
        if (!img && !later) viewWarnf(v, CSCD_FEATURE_WARN_FILES, "cannot load the image %.*s", (int)url.len, url.ptr);
    }
    ImageEntry* e = CSCD_NEW(ImageEntry);
    e->img = img;
    e->bytes = imageBytesOf(img);
    e->used = ++v->imageClock;
    e->decoding = later;
    v->imageBytes += e->bytes;
    cscdStrMapPut(&v->images, url, NULL)->p = e;
    if (e->bytes) imagesMakeRoom(v, e);
    return img;
}

CscdImage* viewImageLoader(void* view, CscdSv url) { return (CscdImage*)cscdRetain(viewImage((CscdView*)view, url)); }

void cscdViewSetImage(CscdView* v, CscdSv url, CscdImage* img) {
    CscdMapVal* slot = cscdStrMapPut(&v->images, url, NULL);
    ImageEntry* e = (ImageEntry*)slot->p;
    if (!e) slot->p = e = CSCD_NEW(ImageEntry);
    for (size_t i = v->playing.len; i-- > 0;)
        if (v->playing.items[i].shown == e->img) {
            cscdRelease(v->playing.items[i].anim);
            cscdVecRemove(&v->playing, i);
        }
    v->imageBytes -= e->bytes;
    if (e->img != img) svgForget(v, e->img);
    cscdRelease(e->img);
    e->img = (CscdImage*)cscdRetain(img);
    e->bytes = imageBytesOf(img);
    e->app = true;
    e->decoding = false; /* (one on its way is dropped when it comes) */
    e->used = ++v->imageClock;
    v->imageBytes += e->bytes;
    cscdStyleEngineInvalidateCache(v->styles);
    v->styleDirty = true;
    cscdViewInvalidate(v);
}

bool cscdViewAddFont(CscdView* v, CscdSv family, CscdSv path, int weight, bool italic) {
    CscdStr found = cscdAppFindFile(path);
    bool ok = cscdFontAdd(family, cscdStrSv(&found), weight, italic, weight);
    cscdStrFree(&found);
    if (!ok) viewWarnf(v, CSCD_FEATURE_WARN_FILES, "cannot load the font %.*s (%.*s)", (int)path.len, path.ptr, (int)family.len, family.ptr);
    cscdStyleEngineInvalidateCache(v->styles);
    v->styleDirty = true;
    cscdViewInvalidate(v);
    return ok;
}

CscdElement* cscdViewGet(CscdView* v, CscdSv selector) {
    CscdElement* e = selector.len ? cscdDocumentQuerySelector(v->doc, selector) : NULL;
    if (!e && selector.len && cscdSvFindAny(selector, "#.[:> ~+*,", 0) == CSCD_NPOS) e = cscdDocumentGetElementById(v->doc, selector);
    if (e) return e;
    viewWarnf(v, CSCD_FEATURE_WARN_SELECTORS, "no element matches \"%.*s\"", (int)selector.len, selector.ptr);
    CscdElement* m = cscdElementNew(CSCD_SV("div")); /* (each kept: pointers to it stay valid) */
    cscdVecPush(&v->missing, m);
    return m;
}

bool cscdViewLoadFile(CscdView* v, CscdSv path) {
    CscdStr found = cscdAppFindFile(path);
    CscdStr html = {0};
    if (!cscdReadFile(cscdStrSv(&found), &html)) {
        if (cscdSvEq(cscdStrSv(&found), path)) viewWarnf(v, CSCD_FEATURE_WARN_FILES, "cannot read the page %.*s", (int)path.len, path.ptr);
        else viewWarnf(v, CSCD_FEATURE_WARN_FILES, "cannot read the page %.*s (%s)", (int)path.len, path.ptr, cscdStrC(&found));
        cscdStrFree(&found);
        cscdStrFree(&html);
        return false;
    }
    CscdStr dir = cscdDirName(cscdStrSv(&found));
    if (cscdSvEndsC(cscdStrSv(&found), ".xht") || cscdSvEndsC(cscdStrSv(&found), ".xhtml")) cscdViewLoadXHTML(v, cscdStrSv(&html), cscdStrSv(&dir));
    else cscdViewLoadHTML(v, cscdStrSv(&html), cscdStrSv(&dir));
    cscdStrFree(&dir);
    cscdStrFree(&found);
    cscdStrFree(&html);
    return true;
}

static void addFontFaces(CscdView* v, const CscdPtrVec* faces) {
    for (size_t i = 0; i < faces->len; ++i) {
        const CscdFontFaceRule* f = (const CscdFontFaceRule*)faces->items[i];
        if (!cscdFontAdd(cscdStrSv(&f->family), cscdStrSv(&f->src), f->weight, f->italic, f->weightMax))
            viewWarnf(v, CSCD_FEATURE_WARN_FILES, "cannot load the font %s (%s)", cscdStrC(&f->src), cscdStrC(&f->family));
    }
}

/* Warns about the properties of a sheet that nothing computes (a misspelling, or one cascade does not
 * have): once per property and load. Vendor-prefixed ones are left alone. */
static void warnUnknownProperties(CscdView* v, const CscdStyleSheet* sh, CscdSv from) {
    for (size_t r = 0; r < sh->rules.len; ++r) {
        const CscdDeclVec* decls = &sh->rules.items[r].declarations;
        for (size_t d = 0; d < decls->len; ++d) {
            const CscdSv p = cscdStrSv(&decls->items[d].property);
            if (!p.len || (p.ptr[0] == '-' && !cscdSvStartsC(p, "--")) || cscdIsKnownProperty(p)) continue;
            bool inserted = false;
            cscdStrMapPut(&v->unknownProperties, p, &inserted);
            if (!inserted) continue;
            const CscdSv near = cscdClosestProperty(p);
            if (near.len) viewWarnf(v, CSCD_FEATURE_WARN_CSS, "unknown CSS property %.*s (in %.*s): ignored; did you mean %.*s?", (int)p.len, p.ptr, (int)from.len, from.ptr, (int)near.len, near.ptr);
            else viewWarnf(v, CSCD_FEATURE_WARN_CSS, "unknown CSS property %.*s (in %.*s): ignored", (int)p.len, p.ptr, (int)from.len, from.ptr);
        }
    }
}

/* Whether a sheet's media attribute holds (noted, to be seen again when the viewport changes). */
static bool sheetMediaHolds(CscdView* v, CscdSv query, const CscdMediaContext* media) {
    if (!query.len) return true;
    SheetMedia m = {cscdStrDup(query), cscdEvalMediaQuery(query, media)};
    cscdVecPush(&v->sheetMedia, m);
    return m.held;
}

static void collectWalk(CscdView* v, CscdNode* n, CscdSv baseDir, const CscdMediaContext* media) {
    CscdElement* e = cscdNodeAsElement(n);
    if (e && cscdElementIs(e, "style")) {
        if (sheetMediaHolds(v, cscdElementAttrC(e, "media"), media)) {
            CscdStyleSheet* sh = cscdStyleSheetNew(CSCD_ORIGIN_AUTHOR);
            CscdStr text = {0};
            cscdNodeTextContent(n, &text);
            cscdStyleSheetParse(sh, cscdStrSv(&text), baseDir, 0);
            cscdStrFree(&text);
            warnUnknownProperties(v, sh, CSCD_SV("a <style>"));
            cscdStyleEngineAddSheet(v->styles, sh);
            cscdRelease(sh);
        }
    } else if (e && cscdElementIs(e, "link")) {
        CscdStr rel = cscdLower(cscdElementAttrC(e, "rel"));
        const bool sheet = cscdSvFind(cscdStrSv(&rel), CSCD_SV("stylesheet"), 0) != CSCD_NPOS;
        cscdStrFree(&rel);
        if (sheet) {
            CscdSv href = cscdElementAttrC(e, "href");
            CscdStr path = cscdJoinPath(baseDir, href);
            SourceFile sf = {cscdStrCopy(&path), cscdFileStamp(cscdStrSv(&path))};
            cscdVecPush(&v->sources, sf);
            CscdStr css = {0};
            if (!sheetMediaHolds(v, cscdElementAttrC(e, "media"), media)) {
                /* (another viewport may use it) */
            } else if (cscdReadFile(cscdStrSv(&path), &css)) {
                CscdStyleSheet* sh = cscdStyleSheetNew(CSCD_ORIGIN_AUTHOR);
                CscdStr dir = cscdDirName(cscdStrSv(&path));
                cscdStyleSheetParse(sh, cscdStrSv(&css), cscdStrSv(&dir), 0);
                cscdStrFree(&dir);
                warnUnknownProperties(v, sh, cscdStrSv(&path));
                cscdStyleEngineAddSheet(v->styles, sh);
                cscdRelease(sh);
            } else viewWarnf(v, CSCD_FEATURE_WARN_FILES, "cannot read the style sheet %.*s (%s)", (int)href.len, href.ptr, cscdStrC(&path));
            cscdStrFree(&css);
            cscdStrFree(&path);
        }
    }
    for (size_t i = 0; i < n->children.len; ++i) collectWalk(v, cscdNodeChild(n, i), baseDir, media);
}

CscdMediaContext viewMediaOf(const CscdView* v) {
    CscdMediaContext m = cscdMediaContextDefault();
    m.width = v->vw;
    m.height = v->vh;
    m.dpr = v->dpr;
    m.dark = v->darkByApp ? v->dark : v->systemDark;
    m.reducedMotion = !viewOn(v, CSCD_FEATURE_ANIMATIONS);
    m.textScale = v->textScale > 0 ? v->textScale : 1;
    return m;
}

/* the base sheet, the document's <style> and <link> sheets, then the added ones */
void viewCollectSheets(CscdView* v) {
    cscdStyleEngineClear(v->styles);
    viewClearSources(v);
    const CscdMediaContext media = viewMediaOf(v);
    if (v->baseSheet) cscdStyleEngineAddSheet(v->styles, (CscdStyleSheet*)v->baseSheet);
    /* style sheets in document order */
    collectWalk(v, &v->doc->node, cscdStrSv(&v->doc->baseDir), &media);
    for (size_t i = 0; i < v->authorSheets.len; ++i) {
        const CscdStr* file = &v->authorSheetFiles.items[i];
        if (file->len) {
            SourceFile sf = {cscdStrCopy(file), cscdFileStamp(cscdStrSv(file))};
            cscdVecPush(&v->sources, sf);
        }
        warnUnknownProperties(v, (const CscdStyleSheet*)v->authorSheets.items[i], file->len ? cscdStrSv(file) : CSCD_SV("an added sheet"));
        cscdStyleEngineAddSheet(v->styles, (CscdStyleSheet*)v->authorSheets.items[i]);
    }
    CscdPtrVec faces = {0};
    cscdStyleEngineFontFaces(v->styles, &faces);
    addFontFaces(v, &faces);
    cscdVecFree(&faces);
}

/* live reload: sheet files changed on disk are read again */
static void checkReload(void* user) {
    CscdView* v = (CscdView*)user;
    bool changed = false;
    for (size_t i = 0; i < v->sources.len; ++i) {
        SourceFile* f = &v->sources.items[i];
        const long long t = cscdFileStamp(cscdStrSv(&f->path));
        if (t != f->stamp) { f->stamp = t; changed = true; }
    }
    if (!changed) return;
    /* the added sheets that came from files are read again; the document's own are in viewCollectSheets */
    for (size_t i = 0; i < v->authorSheets.len; ++i) {
        const CscdStr* file = &v->authorSheetFiles.items[i];
        CscdStr css = {0};
        if (!file->len || !cscdReadFile(cscdStrSv(file), &css)) { cscdStrFree(&css); continue; }
        CscdStyleSheet* sh = cscdStyleSheetNew(CSCD_ORIGIN_AUTHOR);
        CscdStr dir = cscdDirName(cscdStrSv(file));
        cscdStyleSheetParse(sh, cscdStrSv(&css), cscdStrSv(&dir), 0);
        cscdStrFree(&dir);
        cscdStrFree(&css);
        cscdRelease(v->authorSheets.items[i]);
        v->authorSheets.items[i] = sh;
    }
    cscdStrMapClear(&v->unknownProperties); /* (warned again: the page or its files are new) */
    viewCollectSheets(v);
    v->styleDirty = true;
    cscdViewInvalidate(v);
}

void viewSetLiveReload(CscdView* v, bool on) {
    if (on == (v->reloadTimer != 0)) return;
    if (on) {
        for (size_t i = 0; i < v->sources.len; ++i) v->sources.items[i].stamp = cscdFileStamp(cscdStrSv(&v->sources.items[i].path));
        v->reloadTimer = cscdViewSetInterval(v, cscdAction(checkReload, v), 300);
    } else {
        cscdViewClearTimer(v, v->reloadTimer);
        v->reloadTimer = 0;
    }
}


static void loadDocument(CscdView* v, CscdSv source, bool xhtml, CscdSv baseDir) {
    if (v->onDocumentReplaced) v->onDocumentReplaced(v->onDocumentReplacedUser);
    /* (a listener may be replacing the page it is called from: what its callers watch is gone) */
    for (size_t i = 0; i < v->watched.len; ++i) *v->watched.items[i] = NULL;
    cscdViewHideSelectionHandles(v);
    v->inspected = NULL;
    v->inspectPinned = false;
    cscdU64MapClear(&v->a11yIds);
    cscdU64MapClear(&v->a11yElements);
    viewFreeLists(v); /* (their boxes go with the document) */
    viewUnhookDocument(v);
    /* the interface state points into the old document */
    for (size_t i = 0; i < v->menus.len; ++i) ((OpenMenu*)v->menus.items[i])->box = ((OpenMenu*)v->menus.items[i])->owner = NULL;
    viewClearMenus(v);
    v->menuBarOpen = v->tip = v->tipFor = NULL;
    if (v->tipTimer) cscdViewClearTimer(v, v->tipTimer);
    v->tipTimer = 0;
    v->dragActive = v->dragExternal = false;
    v->dragSource = v->dragOver = NULL;
    cscdRelease(v->dragData);
    v->dragData = NULL;
    cscdRelease(v->dragGhost);
    v->dragGhost = NULL;
    v->touches.len = 0;
    v->touchMode = TOUCH_NONE;
    v->touchScroll = NULL;
    v->splitter = v->splitPrev = v->splitNext = NULL;
    viewClearUndo(v);
    v->modals.len = 0;
    v->modalReturnFocus.len = 0;
    v->popovers.len = 0;
    cscdLayoutBoxFree(v->root);
    v->root = NULL;
    v->inkValid = false;
    v->inkEpoch = 0;
    cscdLayoutForget(v->doc);
    viewClearTransitions(v);
    viewClearAnimations(v);
    viewClearHits(v);
    v->stateChanged.len = 0;
    v->smooth.len = 0;
    v->pressed = v->dragRange = v->scrollDrag = NULL;
    v->tabPlace = 0; /* (a new page: Tab starts from its top) */
    viewBindingsClear(v); /* (bound to the old page) */
    CscdDocument* old = v->doc;
    v->doc = xhtml ? cscdParseXHTML(source) : cscdParseHTML(source);
    cscdDocumentFree(old);
    cscdStrSet(&v->doc->baseDir, baseDir);
    viewHookDocument(v);
    cscdStrMapClear(&v->unknownProperties); /* (warned again: the page or its files are new) */
    viewCollectSheets(v);
    v->scrollX = v->scrollY = 0;
    v->styleDirty = v->layoutDirty = true;
    v->styledVersion = 0;
    cscdViewInvalidate(v);
}

void cscdViewLoadHTML(CscdView* v, CscdSv html, CscdSv baseDir) { loadDocument(v, html, false, baseDir); }
void cscdViewLoadXHTML(CscdView* v, CscdSv xhtml, CscdSv baseDir) { loadDocument(v, xhtml, true, baseDir); }

static void addSheetFile(CscdView* v, CscdSv file) { cscdVecPush(&v->authorSheetFiles, cscdStrDup(file)); }

/* A sheet given by the program, parsed: its unknown properties warned about, its @font-face fonts loaded. */
static CscdStyleSheet* programSheet(CscdView* v, CscdSv css, CscdSv baseDir, CscdSv from) {
    CscdStyleSheet* sh = cscdStyleSheetNew(CSCD_ORIGIN_AUTHOR);
    cscdStyleSheetParse(sh, css, baseDir.len ? baseDir : cscdStrSv(&v->doc->baseDir), 0);
    warnUnknownProperties(v, sh, from);
    CscdPtrVec faces = {0};
    for (size_t i = 0; i < sh->fontFaces.len; ++i) cscdVecPush(&faces, &sh->fontFaces.items[i]);
    addFontFaces(v, &faces);
    cscdVecFree(&faces);
    return sh;
}

static void addSheet(CscdView* v, CscdSv css, CscdSv baseDir, CscdSv from) {
    CscdStyleSheet* sh = programSheet(v, css, baseDir, from);
    cscdVecPush(&v->authorSheets, sh);
    addSheetFile(v, CSCD_SV(""));
    cscdStyleEngineAddSheet(v->styles, sh);
    v->styleDirty = true;
    cscdViewInvalidate(v);
}

void cscdViewSetBaseStyleSheet(CscdView* v, CscdSv css, CscdSv baseDir) {
    CscdStyleSheet* sh = programSheet(v, css, baseDir, CSCD_SV("the base sheet"));
    if (v->baseSheet) cscdRelease(v->baseSheet);
    v->baseSheet = sh;
    viewCollectSheets(v);
    v->styleDirty = true;
    cscdViewInvalidate(v);
}

void cscdViewAddStyleSheet(CscdView* v, CscdSv css, CscdSv baseDir) { addSheet(v, css, baseDir, CSCD_SV("an added sheet")); }

bool cscdViewAddStyleSheetFile(CscdView* v, CscdSv path) {
    CscdStr found = cscdAppFindFile(path);
    CscdStr css = {0};
    if (!cscdReadFile(cscdStrSv(&found), &css)) {
        if (cscdSvEq(cscdStrSv(&found), path)) viewWarnf(v, CSCD_FEATURE_WARN_FILES, "cannot read the style sheet %.*s", (int)path.len, path.ptr);
        else viewWarnf(v, CSCD_FEATURE_WARN_FILES, "cannot read the style sheet %.*s (%s)", (int)path.len, path.ptr, cscdStrC(&found));
        cscdStrFree(&found);
        cscdStrFree(&css);
        return false;
    }
    CscdStr dir = cscdDirName(cscdStrSv(&found));
    addSheet(v, cscdStrSv(&css), cscdStrSv(&dir), cscdStrSv(&found));
    cscdStrFree(&dir);
    cscdStrSet(&cscdVecLast(&v->authorSheetFiles), cscdStrSv(&found));
    SourceFile sf = {cscdStrCopy(&found), cscdFileStamp(cscdStrSv(&found))};
    cscdVecPush(&v->sources, sf);
    cscdStrFree(&found);
    cscdStrFree(&css);
    return true;
}
