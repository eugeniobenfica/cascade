/* Loading a page: its HTML, style sheets, fonts and images (animated ones play), from files or
 * memory, and loading them again when their files change (live reload). */
#include "style/styleInternal.h"
#include "view/viewInternal.h"

#include <stdio.h>
#include <stdlib.h>

/* An image from its bytes; an animated one starts playing (the image is its first frame, and its
 * pixels change as it plays). */
static CscdImage* viewDecodeImage(CscdView* v, const uint8_t* data, size_t size) {
    CscdAnimatedImage* anim = cscdDecodeAnimatedImage(data, size);
    if (!anim) return NULL;
    CscdImage* img = cscdImageClone(anim->frames[0]);
    if (anim->frameCount > 1) {
        PlayingImage p = {img, anim, 0, 0, viewNow(v) + anim->durations[0] / 1000.0};
        cscdVecPush(&v->playing, p);
        viewNotifyInvalidate(v);
    } else cscdRelease(anim);
    return img;
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

CscdImage* viewImage(CscdView* v, CscdSv url) {
    CscdMapVal* known = cscdStrMapGet(&v->images, url);
    if (known) return (CscdImage*)known->p;
    CscdImage* img = NULL;
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
            cscdVecFree(&bytes);
        }
    } else {
        CscdStr file = {0};
        if (cscdReadFile(url, &file)) img = viewDecodeImage(v, (const uint8_t*)file.ptr, file.len);
        cscdStrFree(&file);
        if (!img) viewWarnf(v, CSCD_FEATURE_WARN_FILES, "cannot load the image %.*s", (int)url.len, url.ptr);
    }
    cscdStrMapPut(&v->images, url, NULL)->p = img;
    return img;
}

CscdImage* viewImageLoader(void* view, CscdSv url) { return (CscdImage*)cscdRetain(viewImage((CscdView*)view, url)); }

void cscdViewSetImage(CscdView* v, CscdSv url, CscdImage* img) {
    CscdMapVal* slot = cscdStrMapPut(&v->images, url, NULL);
    for (size_t i = v->playing.len; i-- > 0;)
        if (v->playing.items[i].shown == slot->p) {
            cscdRelease(v->playing.items[i].anim);
            cscdVecRemove(&v->playing, i);
        }
    cscdRelease(slot->p);
    slot->p = cscdRetain(img);
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
    cscdViewLoadHTML(v, cscdStrSv(&html), cscdStrSv(&dir));
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


void cscdViewLoadHTML(CscdView* v, CscdSv html, CscdSv baseDir) {
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
    v->doc = cscdParseHTML(html);
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
