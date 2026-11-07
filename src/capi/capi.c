/* The handle API (cascadeC.h): handles in place of pointers, callbacks with user data, UTF-8
 * strings the caller frees, error codes and messages. A thin layer over the native C API, for
 * language bindings that want handles that cannot dangle and a stable ABI. */
#include "cascade/cascade.h"
#include "cascade/cascadeC.h"
#ifdef CASCADE_HAS_WINDOW
#include "cascade/window.h"
#endif
#include "core/thread.h"

#include <stdio.h>
#include <stdlib.h>

static CSCD_THREAD_LOCAL char lastError[256];

static void setError(const char* message) { snprintf(lastError, sizeof lastError, "%s", message); }

static cscd_result fail(cscd_result r, const char* message) {
    setError(message);
    return r;
}

/* a copy the caller frees with cscd_free */
static char* dupSv(CscdSv s) {
    char* p = (char*)malloc(s.len + 1);
    if (!p) return NULL;
    if (s.len) memcpy(p, s.ptr, s.len);
    p[s.len] = 0;
    return p;
}

static char* dupStr(CscdStr* s) {
    char* p = dupSv(cscdStrSv(s));
    cscdStrFree(s);
    return p;
}

static CscdSv str(const char* s) { return s ? cscdSv(s) : CSCD_SV(""); }

static CscdColor color(uint32_t rgba) { return cscdRgba((int)(rgba >> 24), (int)((rgba >> 16) & 255), (int)((rgba >> 8) & 255), (int)(rgba & 255)); }

static CscdModifiers modifiers(unsigned m) {
    CscdModifiers out;
    out.shift = (m & CSCD_MOD_SHIFT) != 0;
    out.ctrl = (m & CSCD_MOD_CTRL) != 0;
    out.alt = (m & CSCD_MOD_ALT) != 0;
    out.meta = (m & CSCD_MOD_META) != 0;
    return out;
}

static CscdCanvas* canvasOf(cscd_canvas* c) { return (CscdCanvas*)c; }

/* A view and the element handles it gave out: slot + 1 in the low half, the slot's generation in the
 * high half. A slot's generation moves on when its element leaves the document, so an old handle no
 * longer finds anything. */
typedef struct Slot { CscdElement* e; uint32_t gen; } Slot;

struct cscd_view {
    CscdView* view;
    bool owned;
    CSCD_VEC(Slot) slots;
    CscdU32Vec freeSlots;
    CscdU64Map index; /* element -> slot */
    CscdImage frame;
    bool framePainted;
    struct Callback* editMenu;   /* (cscd_view_on_edit_menu's) */
    struct Callback* magnifier;
    struct ClipHooks* clipboard; /* (cscd_view_set_clipboard's) */
};

static void dropHooks(cscd_view* v);

static void forget(cscd_view* v, const CscdElement* e) {
    CscdMapVal slot;
    if (!cscdU64MapRemove(&v->index, CSCD_PKEY(e), &slot)) return;
    Slot* s = &v->slots.items[slot.u];
    s->e = NULL;
    ++s->gen;
    cscdVecPush(&v->freeSlots, (uint32_t)slot.u);
}

static void forgetAll(cscd_view* v) {
    for (CscdU64MapSlot* s = NULL; (s = cscdU64MapNext(&v->index, s));) {
        Slot* slot = &v->slots.items[s->val.u];
        slot->e = NULL;
        ++slot->gen;
        cscdVecPush(&v->freeSlots, (uint32_t)s->val.u);
    }
    cscdU64MapClear(&v->index);
    v->framePainted = false;
}

static void nodeRemoved(void* user, CscdNode* n) {
    CscdElement* e = cscdNodeAsElement(n);
    if (e) forget((cscd_view*)user, e);
}

static void documentReplaced(void* user) { forgetAll((cscd_view*)user); }

static cscd_view* wrapView(CscdView* view, bool owned) {
    cscd_view* v = CSCD_NEW(cscd_view);
    v->view = view;
    v->owned = owned;
    cscdViewOnNodeRemoved(view, nodeRemoved, v);
    cscdViewOnDocumentReplaced(view, documentReplaced, v);
    return v;
}

static void unwrapView(cscd_view* v) {
    dropHooks(v);
    cscdViewOnNodeRemoved(v->view, NULL, NULL);
    cscdViewOnDocumentReplaced(v->view, NULL, NULL);
    if (v->owned) cscdViewFree(v->view);
    cscdVecFree(&v->slots);
    cscdVecFree(&v->freeSlots);
    cscdU64MapFree(&v->index);
    cscdImageFini(&v->frame);
    cscdFree(v);
}

static cscd_element handle(cscd_view* v, CscdElement* e) {
    if (!e) return 0;
    bool inserted = false;
    CscdMapVal* at = cscdU64MapPut(&v->index, CSCD_PKEY(e), &inserted);
    if (inserted) {
        uint32_t slot;
        if (v->freeSlots.len) slot = cscdVecPop(&v->freeSlots);
        else {
            slot = (uint32_t)v->slots.len;
            Slot s = {NULL, 1};
            cscdVecPush(&v->slots, s);
        }
        v->slots.items[slot].e = e;
        at->u = slot;
    }
    const uint32_t slot = (uint32_t)at->u;
    return ((uint64_t)v->slots.items[slot].gen << 32) | (uint64_t)(slot + 1);
}

static CscdElement* resolve(const cscd_view* v, cscd_element h) {
    const uint32_t slot = (uint32_t)(h & 0xFFFFFFFFu) - 1, gen = (uint32_t)(h >> 32);
    if (!h || slot >= v->slots.len || v->slots.items[slot].gen != gen) return NULL;
    return v->slots.items[slot].e;
}

/* The element a handle names, or a failure. */
#define CSCD_ELEMENT(var, v, h)                                                                    \
    if (!(v)) return fail(CSCD_ERR_ARGUMENT, "no view");                                           \
    CscdElement* var = resolve((v), (h));                                                          \
    if (!var) return fail(CSCD_ERR_INVALID_HANDLE, "the element is gone (or the handle is not one)")

/* The same, for functions that return a value (onFail when it fails). */
#define CSCD_ELEMENT_OR(var, v, h, onFail)                                                         \
    if (!(v)) { setError("no view"); return onFail; }                                            \
    CscdElement* var = resolve((v), (h));                                                          \
    if (!var) { setError("the element is gone (or the handle is not one)"); return onFail; }

#define CSCD_VIEW(v) \
    if (!(v)) return fail(CSCD_ERR_ARGUMENT, "no view")

/* A callback's function and user data: free_user runs once, when the callback is dropped. */
typedef struct Callback { cscd_view* v; void* fn; void* user; cscd_free_fn freeUser; } Callback;

static Callback* callback(cscd_view* v, void* fn, void* user, cscd_free_fn freeUser) {
    Callback* c = CSCD_NEW(Callback);
    c->v = v;
    c->fn = fn;
    c->user = user;
    c->freeUser = freeUser;
    return c;
}

static void callbackFree(void* p) {
    Callback* c = (Callback*)p;
    if (c->freeUser) c->freeUser(c->user);
    cscdFree(c);
}

/* ---- library ---------------------------------------------------------------------------------------- */

uint32_t cscd_version(void) { return (CSCD_VERSION_MAJOR << 16) | (CSCD_VERSION_MINOR << 8) | CSCD_VERSION_PATCH; }
const char* cscd_last_error(void) { return lastError; }
void cscd_free(void* p) { free(p); }
void cscd_set_paint_threads(int n) { cscdSetPaintThreads(n); }

void cscd_paint_cache_set_limits(uint64_t shadow_bytes, uint64_t shadow_count, uint64_t glyph_count) {
    CscdPaintCacheLimits l = {(size_t)shadow_bytes, (size_t)shadow_count, (size_t)glyph_count};
    cscdPaintCacheSetLimits(l);
}

void cscd_paint_cache_clear(void) { cscdPaintCacheClear(); }

void cscd_paint_cache_get_stats(cscd_paint_cache_stats* out) {
    if (!out) return;
    const CscdPaintCacheStats s = cscdPaintCacheStats();
    out->shadow_count = s.shadowCount;
    out->shadow_bytes = s.shadowBytes;
    out->glyph_count = s.glyphCount;
    out->shadow_hits = s.shadowHits;
    out->shadow_misses = s.shadowMisses;
    out->shadow_evictions = s.shadowEvictions;
    out->glyph_evictions = s.glyphEvictions;
}

/* ---- views ------------------------------------------------------------------------------------------ */

cscd_view* cscd_view_new(void) { return wrapView(cscdViewNew(), true); }

void cscd_view_delete(cscd_view* v) {
    if (v && v->owned) unwrapView(v);
}

cscd_result cscd_view_load_file(cscd_view* v, const char* path) {
    CSCD_VIEW(v);
    if (!path) return fail(CSCD_ERR_ARGUMENT, "no path");
    return cscdViewLoadFile(v->view, cscdSv(path)) ? CSCD_OK : fail(CSCD_ERR_NOT_FOUND, "cannot read the page");
}

cscd_result cscd_view_load_html(cscd_view* v, const char* html, const char* baseDir) {
    CSCD_VIEW(v);
    if (!html) return fail(CSCD_ERR_ARGUMENT, "no HTML");
    cscdViewLoadHTML(v->view, str(html), str(baseDir));
    return CSCD_OK;
}

cscd_result cscd_view_add_style_sheet(cscd_view* v, const char* css) {
    CSCD_VIEW(v);
    if (!css) return fail(CSCD_ERR_ARGUMENT, "no style sheet");
    cscdViewAddStyleSheet(v->view, str(css), CSCD_SV(""));
    return CSCD_OK;
}

cscd_result cscd_view_set_viewport(cscd_view* v, float width, float height, float dpr) {
    CSCD_VIEW(v);
    if (!(width > 0) || !(height > 0) || !(dpr > 0)) return fail(CSCD_ERR_ARGUMENT, "the size and scale must be positive");
    cscdViewSetViewport(v->view, width, height, dpr);
    return CSCD_OK;
}

cscd_result cscd_view_set_dark_mode(cscd_view* v, int dark) {
    CSCD_VIEW(v);
    cscdViewSetDarkMode(v->view, dark != 0);
    return CSCD_OK;
}

char* cscd_view_title(cscd_view* v) {
    if (!v) return NULL;
    CscdStr t = cscdViewTitle(v->view);
    return dupStr(&t);
}

int cscd_view_update(cscd_view* v) { return v && cscdViewUpdate(v->view) ? 1 : 0; }
int cscd_view_needs_paint(cscd_view* v) { return v && cscdViewNeedsPaint(v->view) ? 1 : 0; }
int cscd_view_next_timer_ms(cscd_view* v) { return v ? cscdViewNextTimerMs(v->view) : -1; }
float cscd_view_content_height(cscd_view* v) { return v ? cscdViewContentHeight(v->view) : 0; }

cscd_result cscd_view_frame_stats(cscd_view* v, cscd_frame_stats* out) {
    if (!v) return fail(CSCD_ERR_ARGUMENT, "no view");
    if (!out) return fail(CSCD_ERR_ARGUMENT, "nowhere to put the stats");
    const CscdFrameStats s = cscdViewFrameStats(v->view);
    out->style_ms = s.styleMs;
    out->layout_ms = s.layoutMs;
    out->paint_ms = s.paintMs;
    out->styles_computed = s.stylesComputed;
    out->styles_reused = s.stylesReused;
    out->painted_area = s.paintedArea;
    out->partial_paint = s.partialPaint;
    return CSCD_OK;
}

const uint32_t* cscd_view_paint(cscd_view* v, int* width, int* height) {
    if (!v) return NULL;
    CscdView* view = v->view;
    const int w = cscdMaxi(1, cscdIntOf(ceilf(cscdViewWidth(view) * cscdViewDevicePixelRatio(view))));
    const int h = cscdMaxi(1, cscdIntOf(ceilf(cscdViewHeight(view) * cscdViewDevicePixelRatio(view))));
    if (v->frame.width != w || v->frame.height != h) {
        cscdImageFini(&v->frame);
        cscdImageInit(&v->frame, w, h);
        v->framePainted = false;
    }
    cscdViewPaint(view, &v->frame, v->framePainted); /* (the frame keeps the last one: only what changed is painted) */
    v->framePainted = true;
    if (width) *width = w;
    if (height) *height = h;
    return v->frame.pixels;
}

cscd_result cscd_view_paint_canvas(cscd_view* v, cscd_canvas* c) {
    CSCD_VIEW(v);
    if (!c) return fail(CSCD_ERR_ARGUMENT, "no canvas");
    cscdViewPaintCanvas(v->view, canvasOf(c));
    return CSCD_OK;
}

cscd_result cscd_view_mouse_move(cscd_view* v, float x, float y, int buttons, unsigned mods) {
    CSCD_VIEW(v);
    cscdViewMouseMove(v->view, x, y, buttons, modifiers(mods));
    return CSCD_OK;
}

cscd_result cscd_view_mouse_down(cscd_view* v, float x, float y, int button, unsigned mods, int clicks) {
    CSCD_VIEW(v);
    cscdViewMouseDown(v->view, x, y, button, modifiers(mods), cscdMaxi(1, clicks));
    return CSCD_OK;
}

cscd_result cscd_view_mouse_up(cscd_view* v, float x, float y, int button, unsigned mods) {
    CSCD_VIEW(v);
    cscdViewMouseUp(v->view, x, y, button, modifiers(mods));
    return CSCD_OK;
}

cscd_result cscd_view_mouse_leave(cscd_view* v) {
    CSCD_VIEW(v);
    cscdViewMouseLeave(v->view);
    return CSCD_OK;
}

cscd_result cscd_view_wheel(cscd_view* v, float x, float y, float dx, float dy, unsigned mods) {
    CSCD_VIEW(v);
    cscdViewWheel(v->view, x, y, dx, dy, modifiers(mods));
    return CSCD_OK;
}

cscd_result cscd_view_key_down(cscd_view* v, cscd_key key, unsigned mods, const char* keyName) {
    CSCD_VIEW(v);
    if ((int)key < CSCD_KEY_UNKNOWN || (int)key > CSCD_KEY_CHARACTER) return fail(CSCD_ERR_ARGUMENT, "not a key");
    cscdViewKeyDown(v->view, (CscdKey)key, modifiers(mods), str(keyName));
    return CSCD_OK;
}

cscd_result cscd_view_key_up(cscd_view* v, cscd_key key, unsigned mods, const char* keyName) {
    CSCD_VIEW(v);
    if ((int)key < CSCD_KEY_UNKNOWN || (int)key > CSCD_KEY_CHARACTER) return fail(CSCD_ERR_ARGUMENT, "not a key");
    cscdViewKeyUp(v->view, (CscdKey)key, modifiers(mods), str(keyName));
    return CSCD_OK;
}

cscd_result cscd_view_text_input(cscd_view* v, const char* utf8) {
    CSCD_VIEW(v);
    if (!utf8) return fail(CSCD_ERR_ARGUMENT, "no text");
    cscdViewTextInput(v->view, str(utf8));
    return CSCD_OK;
}

int cscd_view_cursor(cscd_view* v) { return v ? (int)cscdViewCursor(v->view) : 0; }

/* ---- touch, and selecting text by touch -------------------------------------------------------------- */

cscd_result cscd_view_touch_down(cscd_view* v, int id, float x, float y) {
    CSCD_VIEW(v);
    cscdViewTouchDown(v->view, id, x, y);
    return CSCD_OK;
}

cscd_result cscd_view_touch_move(cscd_view* v, int id, float x, float y) {
    CSCD_VIEW(v);
    cscdViewTouchMove(v->view, id, x, y);
    return CSCD_OK;
}

cscd_result cscd_view_touch_up(cscd_view* v, int id, float x, float y) {
    CSCD_VIEW(v);
    cscdViewTouchUp(v->view, id, x, y);
    return CSCD_OK;
}

cscd_result cscd_view_touch_cancel(cscd_view* v) {
    CSCD_VIEW(v);
    cscdViewTouchCancel(v->view);
    return CSCD_OK;
}

static void onEditMenu(void* user, bool show, CscdRect r, unsigned actions) {
    Callback* c = (Callback*)user;
    const float rect[4] = {r.x, r.y, r.w, r.h};
    ((cscd_edit_menu_fn)c->fn)(c->v, show ? 1 : 0, rect, actions, c->user);
}

static void onMagnifier(void* user, bool show, float x, float y) {
    Callback* c = (Callback*)user;
    ((cscd_magnifier_fn)c->fn)(c->v, show ? 1 : 0, x, y, c->user);
}

cscd_result cscd_view_on_edit_menu(cscd_view* v, cscd_edit_menu_fn fn, void* user, cscd_free_fn freeUser) {
    CSCD_VIEW(v);
    Callback* old = v->editMenu;
    v->editMenu = fn ? callback(v, (void*)fn, user, freeUser) : NULL;
    cscdViewOnEditMenu(v->view, fn ? onEditMenu : NULL, v->editMenu); /* (hides a menu the old one showed) */
    if (old) callbackFree(old);
    return CSCD_OK;
}

cscd_result cscd_view_on_magnifier(cscd_view* v, cscd_magnifier_fn fn, void* user, cscd_free_fn freeUser) {
    CSCD_VIEW(v);
    Callback* old = v->magnifier;
    v->magnifier = fn ? callback(v, (void*)fn, user, freeUser) : NULL;
    cscdViewOnMagnifier(v->view, fn ? onMagnifier : NULL, v->magnifier);
    if (old) callbackFree(old);
    return CSCD_OK;
}

cscd_result cscd_view_edit_action(cscd_view* v, cscd_edit_action action) {
    CSCD_VIEW(v);
    if ((int)action < CSCD_EDIT_CUT || (int)action > CSCD_EDIT_SELECT_ALL) return fail(CSCD_ERR_ARGUMENT, "not an edit action");
    return cscdViewEditAction(v->view, (CscdEditAction)action) ? CSCD_OK : fail(CSCD_ERR_REFUSED, "the focused field cannot do that now");
}

unsigned cscd_view_edit_actions(cscd_view* v) { return v ? cscdViewEditActions(v->view) : 0; }
int cscd_view_selection_handles_shown(cscd_view* v) { return v && cscdViewSelectionHandlesShown(v->view) ? 1 : 0; }

cscd_result cscd_view_hide_selection_handles(cscd_view* v) {
    CSCD_VIEW(v);
    cscdViewHideSelectionHandles(v->view);
    return CSCD_OK;
}

/* ---- the clipboard ------------------------------------------------------------------------------------ */

typedef struct ClipHooks { cscd_clipboard_set_fn set; cscd_clipboard_get_fn get; void* user; cscd_free_fn freeUser; } ClipHooks;

static void clipSet(void* user, CscdSv text) {
    ClipHooks* h = (ClipHooks*)user;
    if (!h->set) return;
    char* s = dupSv(text); /* (0-terminated for the callback) */
    if (s) h->set(s, h->user);
    free(s);
}

static void clipGet(void* user, CscdStr* out) {
    ClipHooks* h = (ClipHooks*)user;
    const char* s = h->get ? h->get(h->user) : NULL;
    if (s) cscdStrAppendC(out, s);
}

static void clipHooksFree(ClipHooks* h) {
    if (!h) return;
    if (h->freeUser) h->freeUser(h->user);
    cscdFree(h);
}

cscd_result cscd_view_set_clipboard(cscd_view* v, cscd_clipboard_set_fn set, cscd_clipboard_get_fn get, void* user, cscd_free_fn freeUser) {
    CSCD_VIEW(v);
    ClipHooks* old = v->clipboard;
    v->clipboard = NULL;
    if (set || get) {
        ClipHooks* h = CSCD_NEW(ClipHooks);
        h->set = set;
        h->get = get;
        h->user = user;
        h->freeUser = freeUser;
        v->clipboard = h;
    }
    cscdViewSetClipboardHooks(v->view, v->clipboard && set ? clipSet : NULL, v->clipboard && get ? clipGet : NULL, NULL, NULL, v->clipboard);
    clipHooksFree(old);
    return CSCD_OK;
}

cscd_result cscd_view_copy_text(cscd_view* v, const char* utf8) {
    CSCD_VIEW(v);
    return cscdViewCopyText(v->view, str(utf8)) ? CSCD_OK : fail(CSCD_ERR_REFUSED, "no clipboard");
}

cscd_result cscd_view_copy_image(cscd_view* v, const uint32_t* pixels, int width, int height) {
    CSCD_VIEW(v);
    if (!pixels || width <= 0 || height <= 0) return fail(CSCD_ERR_ARGUMENT, "no pixels");
    CscdImage* img = cscdImageNew(width, height);
    memcpy(img->pixels, pixels, (size_t)width * height * 4);
    const bool ok = cscdViewCopyImage(v->view, img);
    cscdRelease(img);
    return ok ? CSCD_OK : fail(CSCD_ERR_REFUSED, "no clipboard for pictures");
}

char* cscd_view_clipboard_text(cscd_view* v) {
    if (!v) { setError("no view"); return NULL; }
    CscdStr s = {0};
    cscdViewClipboardText(v->view, &s);
    return dupStr(&s);
}

char* cscd_view_clipboard_files(cscd_view* v) {
    if (!v) { setError("no view"); return NULL; }
    CscdStrVec files = {0};
    cscdViewClipboardFiles(v->view, &files);
    CscdStr all = {0};
    for (size_t i = 0; i < files.len; ++i) {
        if (i) cscdStrAppendChar(&all, '\n');
        cscdStrAppend(&all, cscdStrSv(&files.items[i]));
    }
    cscdStrVecFree(&files);
    return dupStr(&all);
}

/* (the hooks given here go with the view, or with the wrapper of a window's view) */
static void dropHooks(cscd_view* v) {
    if (v->editMenu) cscd_view_on_edit_menu(v, NULL, NULL, NULL);
    if (v->magnifier) cscd_view_on_magnifier(v, NULL, NULL, NULL);
    if (v->clipboard) cscd_view_set_clipboard(v, NULL, NULL, NULL, NULL);
}

static void runTimer(void* user) {
    Callback* c = (Callback*)user;
    ((cscd_fn)c->fn)(c->user);
}

int cscd_view_set_timeout(cscd_view* v, int ms, cscd_fn fn, void* user, cscd_free_fn freeUser) {
    if (!v || !fn) { setError("no view or no function"); return 0; }
    CscdAction a = {runTimer, callback(v, (void*)fn, user, freeUser), callbackFree};
    return cscdViewSetTimeout(v->view, a, ms);
}

int cscd_view_set_interval(cscd_view* v, int ms, cscd_fn fn, void* user, cscd_free_fn freeUser) {
    if (!v || !fn) { setError("no view or no function"); return 0; }
    CscdAction a = {runTimer, callback(v, (void*)fn, user, freeUser), callbackFree};
    return cscdViewSetInterval(v->view, a, ms);
}

cscd_result cscd_view_clear_timer(cscd_view* v, int id) {
    CSCD_VIEW(v);
    cscdViewClearTimer(v->view, id);
    return CSCD_OK;
}

/* ---- elements --------------------------------------------------------------------------------------- */

cscd_element cscd_view_query(cscd_view* v, const char* selector) {
    if (!v || !selector) return 0;
    return handle(v, cscdViewQuerySelector(v->view, cscdSv(selector)));
}

size_t cscd_view_query_all(cscd_view* v, const char* selector, cscd_element* out, size_t capacity) {
    if (!v || !selector) return 0;
    CscdPtrVec all = {0};
    cscdViewQuerySelectorAll(v->view, cscdSv(selector), &all);
    for (size_t i = 0; i < all.len && i < capacity && out; ++i) out[i] = handle(v, (CscdElement*)all.items[i]);
    const size_t n = all.len;
    cscdVecFree(&all);
    return n;
}

cscd_element cscd_view_element_by_id(cscd_view* v, const char* id) {
    if (!v || !id) return 0;
    return handle(v, cscdViewGetElementById(v->view, cscdSv(id)));
}

cscd_element cscd_view_element_at(cscd_view* v, float x, float y) {
    if (!v) return 0;
    return handle(v, cscdViewElementAt(v->view, x, y));
}

cscd_element cscd_view_body(cscd_view* v) {
    if (!v) return 0;
    return handle(v, cscdDocumentBody(cscdViewDocument(v->view)));
}

int cscd_element_valid(cscd_view* v, cscd_element e) { return v && resolve(v, e) ? 1 : 0; }

cscd_element cscd_element_query(cscd_view* v, cscd_element h, const char* selector) {
    CSCD_ELEMENT_OR(e, v, h, 0);
    if (!selector) return 0;
    return handle(v, cscdElementQuerySelector(e, cscdSv(selector)));
}

cscd_element cscd_element_parent(cscd_view* v, cscd_element h) {
    CSCD_ELEMENT_OR(e, v, h, 0);
    return handle(v, cscdNodeParentElement(&e->node));
}

cscd_element cscd_element_first_child(cscd_view* v, cscd_element h) {
    CSCD_ELEMENT_OR(e, v, h, 0);
    return handle(v, cscdNodeFirstElementChild(&e->node));
}

cscd_element cscd_element_next_sibling(cscd_view* v, cscd_element h) {
    CSCD_ELEMENT_OR(e, v, h, 0);
    return handle(v, cscdNodeNextElementSibling(&e->node));
}

char* cscd_element_tag(cscd_view* v, cscd_element h) {
    CSCD_ELEMENT_OR(e, v, h, NULL);
    return dupSv(cscdStrSv(&e->tag));
}

char* cscd_element_attribute(cscd_view* v, cscd_element h, const char* name) {
    CSCD_ELEMENT_OR(e, v, h, NULL);
    if (!name) return NULL;
    const CscdStr* a = cscdElementGetAttribute(e, cscdSv(name));
    return a ? dupSv(cscdStrSv(a)) : NULL;
}

cscd_result cscd_element_set_attribute(cscd_view* v, cscd_element h, const char* name, const char* value) {
    CSCD_ELEMENT(e, v, h);
    if (!name) return fail(CSCD_ERR_ARGUMENT, "no name");
    cscdElementSetAttribute(e, cscdSv(name), str(value));
    return CSCD_OK;
}

cscd_result cscd_element_remove_attribute(cscd_view* v, cscd_element h, const char* name) {
    CSCD_ELEMENT(e, v, h);
    if (!name) return fail(CSCD_ERR_ARGUMENT, "no name");
    cscdElementRemoveAttribute(e, cscdSv(name));
    return CSCD_OK;
}

int cscd_element_has_class(cscd_view* v, cscd_element h, const char* name) {
    CSCD_ELEMENT_OR(e, v, h, 0);
    return name && cscdElementHasClass(e, cscdSv(name)) ? 1 : 0;
}

cscd_result cscd_element_add_class(cscd_view* v, cscd_element h, const char* name) {
    CSCD_ELEMENT(e, v, h);
    if (!name) return fail(CSCD_ERR_ARGUMENT, "no name");
    cscdElementAddClass(e, cscdSv(name));
    return CSCD_OK;
}

cscd_result cscd_element_remove_class(cscd_view* v, cscd_element h, const char* name) {
    CSCD_ELEMENT(e, v, h);
    if (!name) return fail(CSCD_ERR_ARGUMENT, "no name");
    cscdElementRemoveClass(e, cscdSv(name));
    return CSCD_OK;
}

cscd_result cscd_element_toggle_class(cscd_view* v, cscd_element h, const char* name, int on) {
    CSCD_ELEMENT(e, v, h);
    if (!name) return fail(CSCD_ERR_ARGUMENT, "no name");
    cscdElementSetClass(e, cscdSv(name), on != 0);
    return CSCD_OK;
}

cscd_result cscd_element_set_style(cscd_view* v, cscd_element h, const char* property, const char* value) {
    CSCD_ELEMENT(e, v, h);
    if (!property) return fail(CSCD_ERR_ARGUMENT, "no property");
    cscdElementSetStyle(e, cscdSv(property), str(value));
    return CSCD_OK;
}

char* cscd_element_text(cscd_view* v, cscd_element h) {
    CSCD_ELEMENT_OR(e, v, h, NULL);
    CscdStr t = {0};
    cscdNodeTextContent(&e->node, &t);
    return dupStr(&t);
}

cscd_result cscd_element_set_text(cscd_view* v, cscd_element h, const char* text) {
    CSCD_ELEMENT(e, v, h);
    cscdElementSetText(e, str(text));
    return CSCD_OK;
}

char* cscd_element_html(cscd_view* v, cscd_element h) {
    CSCD_ELEMENT_OR(e, v, h, NULL);
    CscdStr t = cscdElementInnerHTML(e);
    return dupStr(&t);
}

cscd_result cscd_element_set_html(cscd_view* v, cscd_element h, const char* html) {
    CSCD_ELEMENT(e, v, h);
    cscdElementSetInnerHTML(e, str(html));
    return CSCD_OK;
}

char* cscd_element_value(cscd_view* v, cscd_element h) {
    CSCD_ELEMENT_OR(e, v, h, NULL);
    CscdStr t = cscdElementValue(e);
    return dupStr(&t);
}

cscd_result cscd_element_set_value(cscd_view* v, cscd_element h, const char* value) {
    CSCD_ELEMENT(e, v, h);
    cscdElementSetValue(e, str(value));
    return CSCD_OK;
}

int cscd_element_checked(cscd_view* v, cscd_element h) {
    CSCD_ELEMENT_OR(e, v, h, 0);
    return cscdElementChecked(e) ? 1 : 0;
}

cscd_result cscd_element_set_checked(cscd_view* v, cscd_element h, int checked) {
    CSCD_ELEMENT(e, v, h);
    cscdElementSetChecked(e, checked != 0);
    return CSCD_OK;
}

cscd_element cscd_element_append(cscd_view* v, cscd_element h, const char* tag) {
    CSCD_ELEMENT_OR(e, v, h, 0);
    if (!tag || !*tag) { setError("no tag"); return 0; }
    return handle(v, cscdElementAppendElement(e, cscdSv(tag)));
}

cscd_element cscd_element_append_html(cscd_view* v, cscd_element h, const char* html) {
    CSCD_ELEMENT_OR(e, v, h, 0);
    return handle(v, cscdElementAppendHTML(e, str(html)));
}

cscd_result cscd_element_remove(cscd_view* v, cscd_element h) {
    CSCD_ELEMENT(e, v, h);
    cscdElementRemove(e);
    return CSCD_OK;
}

cscd_result cscd_element_rect(cscd_view* v, cscd_element h, float out[4]) {
    CSCD_ELEMENT(e, v, h);
    if (!out) return fail(CSCD_ERR_ARGUMENT, "nowhere to put it");
    const CscdRect r = cscdViewElementRect(v->view, e);
    out[0] = r.x, out[1] = r.y, out[2] = r.w, out[3] = r.h;
    return CSCD_OK;
}

cscd_result cscd_element_focus(cscd_view* v, cscd_element h) {
    CSCD_ELEMENT(e, v, h);
    cscdViewFocus(v->view, e);
    return CSCD_OK;
}

cscd_result cscd_element_click(cscd_view* v, cscd_element h) {
    CSCD_ELEMENT(e, v, h);
    cscdElementClick(e);
    return CSCD_OK;
}

cscd_result cscd_element_scroll_into_view(cscd_view* v, cscd_element h) {
    CSCD_ELEMENT(e, v, h);
    cscdViewScrollIntoView(v->view, e);
    return CSCD_OK;
}

cscd_result cscd_element_invalidate(cscd_view* v, cscd_element h) {
    CSCD_ELEMENT(e, v, h);
    cscdViewInvalidateElement(v->view, e);
    return CSCD_OK;
}

/* ---- events ----------------------------------------------------------------------------------------- */

static void onEvent(void* user, CscdEvent* ev) {
    Callback* c = (Callback*)user;
    cscd_view* v = c->v;
    cscd_event out;
    memset(&out, 0, sizeof out);
    out.type = cscdStrC(&ev->type);
    out.target = handle(v, ev->target);
    out.current_target = handle(v, ev->currentTarget);
    out.x = ev->x, out.y = ev->y;
    out.offset_x = ev->offsetX, out.offset_y = ev->offsetY;
    out.delta_x = ev->deltaX, out.delta_y = ev->deltaY;
    out.button = ev->button, out.buttons = ev->buttons, out.detail = ev->detail;
    out.key = cscdStrC(&ev->key);
    out.mods = (ev->shift ? CSCD_MOD_SHIFT : 0) | (ev->ctrl ? CSCD_MOD_CTRL : 0) | (ev->alt ? CSCD_MOD_ALT : 0) | (ev->meta ? CSCD_MOD_META : 0);
    out.internal_ = ev;
    ((cscd_event_fn)c->fn)(v, &out, c->user);
}

cscd_result cscd_element_on(cscd_view* v, cscd_element h, const char* type, cscd_event_fn fn, void* user, cscd_free_fn freeUser) {
    CSCD_ELEMENT(e, v, h);
    if (!type || !fn) return fail(CSCD_ERR_ARGUMENT, "no event type or no function");
    CscdEventHandler eh = {onEvent, callback(v, (void*)fn, user, freeUser), callbackFree};
    cscdElementAddEventListener(e, cscdSv(type), eh, false);
    return CSCD_OK;
}

cscd_result cscd_element_off(cscd_view* v, cscd_element h, const char* type) {
    CSCD_ELEMENT(e, v, h);
    if (!type) return fail(CSCD_ERR_ARGUMENT, "no event type");
    cscdElementRemoveEventListeners(e, cscdSv(type));
    return CSCD_OK;
}

void cscd_event_prevent_default(const cscd_event* ev) {
    if (ev && ev->internal_) cscdEventPreventDefault((CscdEvent*)ev->internal_);
}

void cscd_event_stop_propagation(const cscd_event* ev) {
    if (ev && ev->internal_) cscdEventStopPropagation((CscdEvent*)ev->internal_);
}

/* ---- painters --------------------------------------------------------------------------------------- */

static void onPaint(void* user, CscdCanvas* canvas, float w, float h) {
    Callback* c = (Callback*)user;
    ((cscd_paint_fn)c->fn)((cscd_canvas*)canvas, w, h, c->user);
}

cscd_result cscd_view_set_feature(cscd_view* v, cscd_feature feature, int on) {
    CSCD_VIEW(v);
    if ((int)feature < 0 || feature >= CSCD_FEATURE_COUNT) return fail(CSCD_ERR_ARGUMENT, "no such feature");
    cscdViewSetFeature(v->view, (CscdFeature)feature, on != 0);
    return CSCD_OK;
}

int cscd_view_feature(cscd_view* v, cscd_feature feature) { return v && cscdViewFeature(v->view, (CscdFeature)feature); }

cscd_result cscd_view_inspect(cscd_view* v, cscd_element h) {
    CSCD_VIEW(v);
    CscdElement* e = h ? resolve(v, h) : NULL;
    if (h && !e) return fail(CSCD_ERR_INVALID_HANDLE, "the element is gone (or the handle is not one)");
    cscdViewInspect(v->view, e);
    return CSCD_OK;
}

cscd_element cscd_element_append_copy(cscd_view* v, cscd_element parent, cscd_element source) {
    if (!v) return 0;
    CscdElement* p = resolve(v, parent);
    CscdElement* s = resolve(v, source);
    if (!p || !s) { setError("the element is gone (or the handle is not one)"); return 0; }
    CscdElement* copy = cscdElementIs(s, "template") ? cscdTemplateInstantiate(s) : cscdElementClone(s, true);
    if (!copy) { setError("the template is empty"); return 0; }
    cscdNodeAppendChild(&p->node, &copy->node);
    return handle(v, copy);
}

cscd_result cscd_element_fill(cscd_view* v, cscd_element h, const char* key, const char* value) {
    CSCD_ELEMENT(e, v, h);
    if (!key || !value) return fail(CSCD_ERR_ARGUMENT, "no key or value");
    cscdElementFill(e, cscdSv(key), cscdSv(value));
    return CSCD_OK;
}

static void onListFill(void* user, CscdElement* row, size_t index) {
    Callback* c = (Callback*)user;
    ((cscd_list_fill_fn)c->fn)(c->v, handle(c->v, row), index, c->user);
}

cscd_result cscd_view_bind_list(cscd_view* v, cscd_element boxH, cscd_element rowH, size_t count, float rowHeight, cscd_list_fill_fn fill,
                                void* user, cscd_free_fn freeUser) {
    CSCD_ELEMENT(box, v, boxH);
    CscdElement* row = resolve(v, rowH);
    if (!row) return fail(CSCD_ERR_INVALID_HANDLE, "the row is gone (or the handle is not one)");
    if (!fill) return fail(CSCD_ERR_ARGUMENT, "no fill function");
    cscdViewBindList(v->view, box, row, count, rowHeight, onListFill, callback(v, (void*)fill, user, freeUser), callbackFree);
    return CSCD_OK;
}

cscd_result cscd_view_list_set_count(cscd_view* v, cscd_element boxH, size_t count) {
    CSCD_ELEMENT(box, v, boxH);
    cscdViewListSetCount(v->view, box, count);
    return CSCD_OK;
}

cscd_result cscd_view_list_refresh(cscd_view* v, cscd_element boxH) {
    CSCD_ELEMENT(box, v, boxH);
    cscdViewListRefresh(v->view, box);
    return CSCD_OK;
}

cscd_result cscd_element_set_painter(cscd_view* v, cscd_element h, cscd_paint_fn fn, void* user, cscd_free_fn freeUser) {
    CSCD_ELEMENT(e, v, h);
    CscdPainter p = {NULL, NULL, NULL};
    if (fn) {
        p.fn = onPaint;
        p.user = callback(v, (void*)fn, user, freeUser);
        p.freeUser = callbackFree;
    }
    cscdElementSetPainter(e, p);
    return CSCD_OK;
}

void cscd_canvas_save(cscd_canvas* c) { if (c) cscdCanvasSave(canvasOf(c)); }
void cscd_canvas_restore(cscd_canvas* c) { if (c) cscdCanvasRestore(canvasOf(c)); }
void cscd_canvas_translate(cscd_canvas* c, float x, float y) { if (c) cscdCanvasTranslate(canvasOf(c), x, y); }
void cscd_canvas_scale(cscd_canvas* c, float sx, float sy) { if (c) cscdCanvasScale(canvasOf(c), sx, sy); }
void cscd_canvas_rotate(cscd_canvas* c, float radians) { if (c) cscdCanvasRotate(canvasOf(c), radians); }
void cscd_canvas_clip_rect(cscd_canvas* c, float x, float y, float w, float h) { if (c) cscdCanvasClipRect(canvasOf(c), cscdRect(x, y, w, h)); }
void cscd_canvas_set_global_alpha(cscd_canvas* c, float alpha) { if (c) cscdCanvasSetGlobalAlpha(canvasOf(c), alpha); }
void cscd_canvas_clear(cscd_canvas* c, uint32_t rgba) { if (c) cscdCanvasClear(canvasOf(c), color(rgba)); }

void cscd_canvas_fill_rect(cscd_canvas* c, float x, float y, float w, float h, uint32_t rgba) {
    if (c) cscdCanvasFillRect(canvasOf(c), cscdRect(x, y, w, h), color(rgba));
}

void cscd_canvas_fill_round_rect(cscd_canvas* c, float x, float y, float w, float h, float radius, uint32_t rgba) {
    if (!c) return;
    CscdRRect r = cscdRRect(cscdRect(x, y, w, h));
    for (int i = 0; i < 4; ++i) r.rx[i] = r.ry[i] = radius;
    cscdCanvasFillRRect(canvasOf(c), &r, color(rgba));
}

void cscd_canvas_stroke_rect(cscd_canvas* c, float x, float y, float w, float h, uint32_t rgba, float width) {
    if (c) cscdCanvasStrokeRect(canvasOf(c), cscdRect(x, y, w, h), color(rgba), width);
}

void cscd_canvas_draw_line(cscd_canvas* c, float x0, float y0, float x1, float y1, uint32_t rgba, float width) {
    if (c) cscdCanvasDrawLine(canvasOf(c), cscdPoint(x0, y0), cscdPoint(x1, y1), color(rgba), width, CSCD_CAP_BUTT);
}

void cscd_canvas_fill_circle(cscd_canvas* c, float x, float y, float r, uint32_t rgba) {
    if (c) cscdCanvasFillCircle(canvasOf(c), cscdPoint(x, y), r, color(rgba));
}

void cscd_canvas_fill_path(cscd_canvas* c, const char* svgPath, uint32_t rgba) {
    if (!c || !svgPath) return;
    CscdPath p = {0};
    cscdParseSvgPath(cscdSv(svgPath), &p);
    cscdCanvasFillPath(canvasOf(c), &p, color(rgba), CSCD_FILL_NONZERO);
    cscdPathFree(&p);
}

void cscd_canvas_stroke_path(cscd_canvas* c, const char* svgPath, uint32_t rgba, float width) {
    if (!c || !svgPath) return;
    CscdPath p = {0};
    cscdParseSvgPath(cscdSv(svgPath), &p);
    cscdCanvasStrokePath(canvasOf(c), &p, color(rgba), width);
    cscdPathFree(&p);
}

static CscdFont fontFor(const char* family, float size, int weight) {
    CscdStrVec families = {0};
    CscdSv list = family ? cscdSv(family) : CSCD_SV("sans-serif");
    while (list.len) {
        const size_t comma = cscdSvFindChar(list, ',', 0);
        CscdSv f = cscdSvSub(list, 0, comma);
        while (f.len && (f.ptr[0] == ' ' || f.ptr[0] == '"' || f.ptr[0] == '\'')) { ++f.ptr; --f.len; }
        while (f.len && (f.ptr[f.len - 1] == ' ' || f.ptr[f.len - 1] == '"' || f.ptr[f.len - 1] == '\'')) --f.len;
        if (f.len) cscdVecPush(&families, cscdStrDup(f));
        if (comma == CSCD_NPOS) break;
        list = cscdSvFrom(list, comma + 1);
    }
    if (!families.len) cscdVecPush(&families, cscdStrDupC("sans-serif"));
    CscdFont font = cscdFontMatch(&families, size > 0 ? size : 16, weight > 0 ? weight : 400, false);
    cscdStrVecFree(&families);
    return font;
}

void cscd_canvas_draw_text(cscd_canvas* c, const char* utf8, float x, float y, const char* family, float size, int weight, uint32_t rgba) {
    if (!c || !utf8) return;
    CscdFont f = fontFor(family, size, weight);
    cscdCanvasDrawText(canvasOf(c), cscdSv(utf8), x, y, &f, color(rgba));
}

float cscd_canvas_measure_text(cscd_canvas* c, const char* utf8, const char* family, float size, int weight) {
    if (!c || !utf8) return 0;
    CscdFont f = fontFor(family, size, weight);
    return cscdCanvasMeasureText(canvasOf(c), cscdSv(utf8), &f);
}

void cscd_canvas_draw_image(cscd_canvas* c, const uint32_t* pixels, int width, int height, float x, float y, float w, float h) {
    if (!c || !pixels || width <= 0 || height <= 0) return;
    CscdImage* img = cscdImageNew(width, height);
    memcpy(img->pixels, pixels, (size_t)width * height * 4);
    cscdCanvasDrawSharedImage(canvasOf(c), img, cscdRect(0, 0, (float)width, (float)height), cscdRect(x, y, w, h), true);
    cscdRelease(img);
}

/* ---- windows ---------------------------------------------------------------------------------------- */

#ifdef CASCADE_HAS_WINDOW

typedef struct FrameCallback { cscd_fn fn; void* user; cscd_free_fn freeUser; } FrameCallback;

struct cscd_window {
    CscdWindow* window;
    cscd_view* view;
    FrameCallback frame;
};

cscd_window* cscd_window_new(const char* title, int width, int height) {
    CscdWindowOptions o = cscdWindowOptions();
    o.title = title ? title : "";
    if (width > 0) o.width = width;
    if (height > 0) o.height = height;
    CscdWindow* win = cscdWindowNew(&o);
    if (!cscdWindowIsOpen(win)) {
        cscdWindowFree(win);
        setError("cannot open a window");
        return NULL;
    }
    cscd_window* w = CSCD_NEW(cscd_window);
    w->window = win;
    w->view = wrapView(cscdWindowView(win), false);
    return w;
}

void cscd_window_delete(cscd_window* w) {
    if (!w) return;
    unwrapView(w->view); /* (first: it lets go of the window's view) */
    cscdWindowFree(w->window);
    if (w->frame.freeUser) w->frame.freeUser(w->frame.user);
    cscdFree(w);
}

cscd_view* cscd_window_view(cscd_window* w) { return w ? w->view : NULL; }
int cscd_window_run(cscd_window* w) { return w ? cscdWindowRun(w->window) : 1; }
int cscd_window_pump(cscd_window* w, int wait) { return w && cscdWindowPump(w->window, wait != 0) ? 1 : 0; }

cscd_result cscd_window_show(cscd_window* w) {
    if (!w) return fail(CSCD_ERR_ARGUMENT, "no window");
    cscdWindowShow(w->window);
    return CSCD_OK;
}

cscd_result cscd_window_close(cscd_window* w) {
    if (!w) return fail(CSCD_ERR_ARGUMENT, "no window");
    cscdWindowClose(w->window);
    return CSCD_OK;
}

cscd_result cscd_window_set_title(cscd_window* w, const char* title) {
    if (!w) return fail(CSCD_ERR_ARGUMENT, "no window");
    cscdWindowSetTitle(w->window, title ? title : "");
    return CSCD_OK;
}

cscd_result cscd_window_post(cscd_window* w, cscd_fn fn, void* user) {
    if (!w || !fn) return fail(CSCD_ERR_ARGUMENT, "no window or no function");
    CscdAction a = {fn, user, NULL};
    cscdWindowPost(w->window, a);
    return CSCD_OK;
}

static void onFrame(void* user) {
    FrameCallback* c = (FrameCallback*)user;
    c->fn(c->user);
}

cscd_result cscd_window_on_frame(cscd_window* w, cscd_fn fn, void* user, cscd_free_fn freeUser) {
    if (!w) return fail(CSCD_ERR_ARGUMENT, "no window");
    if (w->frame.freeUser) w->frame.freeUser(w->frame.user); /* (the one it replaces) */
    w->frame.fn = fn;
    w->frame.user = user;
    w->frame.freeUser = freeUser;
    cscdWindowOnFrame(w->window, fn ? onFrame : NULL, &w->frame);
    return CSCD_OK;
}

char* cscd_window_backend(cscd_window* w) {
    if (!w) return NULL;
    return dupSv(cscdSv(cscdWindowBackendDescription(w->window)));
}

#else

cscd_window* cscd_window_new(const char* title, int width, int height) {
    (void)title; (void)width; (void)height;
    setError("built without the window");
    return NULL;
}
void cscd_window_delete(cscd_window* w) { (void)w; }
cscd_view* cscd_window_view(cscd_window* w) { (void)w; return NULL; }
int cscd_window_run(cscd_window* w) { (void)w; return 1; }
int cscd_window_pump(cscd_window* w, int wait) { (void)w; (void)wait; return 0; }
cscd_result cscd_window_show(cscd_window* w) { (void)w; return fail(CSCD_ERR_UNAVAILABLE, "built without the window"); }
cscd_result cscd_window_close(cscd_window* w) { (void)w; return fail(CSCD_ERR_UNAVAILABLE, "built without the window"); }
cscd_result cscd_window_set_title(cscd_window* w, const char* t) { (void)w; (void)t; return fail(CSCD_ERR_UNAVAILABLE, "built without the window"); }
cscd_result cscd_window_post(cscd_window* w, cscd_fn fn, void* u) { (void)w; (void)fn; (void)u; return fail(CSCD_ERR_UNAVAILABLE, "built without the window"); }
cscd_result cscd_window_on_frame(cscd_window* w, cscd_fn fn, void* u, cscd_free_fn f) {
    (void)w; (void)fn; (void)u; (void)f;
    return fail(CSCD_ERR_UNAVAILABLE, "built without the window");
}
char* cscd_window_backend(cscd_window* w) { (void)w; return NULL; }

#endif
