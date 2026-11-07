/* The window on Wayland. libwayland-client, libxkbcommon and libwayland-cursor are opened at run
 * time (no headers or libraries to build against): the protocol's few structures are declared here,
 * xdg-shell's interfaces written out as wayland-scanner would make them. The window is an
 * xdg_toplevel; the compositor draws its frame when it offers xdg-decoration, else a title bar of our
 * own is drawn on a subsurface above it. Keys go through xkbcommon (keymap, modifiers, the text they
 * type; repeats timed here, as Wayland asks); the clipboard and drags from other apps through the
 * data device; frames are shown through wl_shm buffers (or Vulkan, vulkan_backend.c). */
#include "platform/common/platformInternal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ---- the protocol's structures (wayland-util.h) ----------------------------------------------- */

struct wl_proxy;
struct wl_display;
struct wl_interface;
struct wl_message { const char* name; const char* signature; const struct wl_interface** types; };
struct wl_interface { const char* name; int version; int method_count; const struct wl_message* methods; int event_count; const struct wl_message* events; };
struct wl_array { size_t size, alloc; void* data; };
typedef int32_t wl_fixed_t;
static float fixedToFloat(wl_fixed_t f) { return f / 256.f; }

/* libwayland-cursor's */
struct wl_cursor_image { uint32_t width, height, hotspot_x, hotspot_y, delay; };
struct wl_cursor { unsigned int image_count; struct wl_cursor_image** images; char* name; };
struct wl_cursor_theme;

/* ---- the libraries -------------------------------------------------------------------------- */

static struct {
    bool tried, ok;
    void *lib, *xkbLib, *cursorLib;
    struct wl_display* (*displayConnect)(const char*);
    void (*displayDisconnect)(struct wl_display*);
    int (*displayGetFd)(struct wl_display*);
    int (*displayDispatchPending)(struct wl_display*);
    int (*displayFlush)(struct wl_display*);
    int (*displayRoundtrip)(struct wl_display*);
    int (*displayPrepareRead)(struct wl_display*);
    int (*displayReadEvents)(struct wl_display*);
    void (*displayCancelRead)(struct wl_display*);
    struct wl_proxy* (*marshalFlags)(struct wl_proxy*, uint32_t, const struct wl_interface*, uint32_t, uint32_t, ...);
    struct wl_proxy* (*marshalConstructorVersioned)(struct wl_proxy*, uint32_t, const struct wl_interface*, uint32_t, ...);
    void (*marshal)(struct wl_proxy*, uint32_t, ...);
    int (*addListener)(struct wl_proxy*, void (**)(void), void*);
    void (*proxyDestroy)(struct wl_proxy*);
    uint32_t (*proxyGetVersion)(struct wl_proxy*);
    const struct wl_interface *registry, *compositor, *subcompositor, *subsurface, *surface, *callback, *shm, *shmPool, *buffer, *seat,
        *pointer, *keyboard, *output, *dataDeviceManager, *dataDevice, *dataSource, *dataOffer;
    /* libxkbcommon */
    void* (*xkbContextNew)(int);
    void (*xkbContextUnref)(void*);
    void* (*xkbKeymapNewFromString)(void*, const char*, int, int);
    void (*xkbKeymapUnref)(void*);
    int (*xkbKeymapKeyGetSymsByLevel)(void*, uint32_t, uint32_t, uint32_t, const uint32_t**);
    int (*xkbKeymapKeyRepeats)(void*, uint32_t);
    void* (*xkbStateNew)(void*);
    void (*xkbStateUnref)(void*);
    int (*xkbStateUpdateMask)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    uint32_t (*xkbStateKeyGetOneSym)(void*, uint32_t);
    int (*xkbStateKeyGetUtf8)(void*, uint32_t, char*, size_t);
    int (*xkbStateModNameIsActive)(void*, const char*, int);
    uint32_t (*xkbStateKeyGetLayout)(void*, uint32_t);
    /* libwayland-cursor */
    struct wl_cursor_theme* (*cursorThemeLoad)(const char*, int, void*);
    void (*cursorThemeDestroy)(struct wl_cursor_theme*);
    struct wl_cursor* (*cursorThemeGetCursor)(struct wl_cursor_theme*, const char*);
    void* (*cursorImageGetBuffer)(struct wl_cursor_image*);
} W;

#define SYM(lib, field, name) (*(void**)&W.field = platformSymbol(lib, name))

static bool loadWayland(void) {
    if (W.tried) return W.ok;
    W.tried = true;
    static const char* const libs[] = {"libwayland-client.so.0", "libwayland-client.so", NULL};
    void* l = W.lib = platformOpenLibrary(libs);
    if (!l) return false;
    SYM(l, displayConnect, "wl_display_connect");
    SYM(l, displayDisconnect, "wl_display_disconnect");
    SYM(l, displayGetFd, "wl_display_get_fd");
    SYM(l, displayDispatchPending, "wl_display_dispatch_pending");
    SYM(l, displayFlush, "wl_display_flush");
    SYM(l, displayRoundtrip, "wl_display_roundtrip");
    SYM(l, displayPrepareRead, "wl_display_prepare_read");
    SYM(l, displayReadEvents, "wl_display_read_events");
    SYM(l, displayCancelRead, "wl_display_cancel_read");
    SYM(l, marshalFlags, "wl_proxy_marshal_flags"); /* (1.20 and later; the older calls otherwise) */
    SYM(l, marshalConstructorVersioned, "wl_proxy_marshal_constructor_versioned");
    SYM(l, marshal, "wl_proxy_marshal");
    SYM(l, addListener, "wl_proxy_add_listener");
    SYM(l, proxyDestroy, "wl_proxy_destroy");
    SYM(l, proxyGetVersion, "wl_proxy_get_version");
#define IFACE(field, name) (W.field = (const struct wl_interface*)platformSymbol(l, name))
    IFACE(registry, "wl_registry_interface");
    IFACE(compositor, "wl_compositor_interface");
    IFACE(subcompositor, "wl_subcompositor_interface");
    IFACE(subsurface, "wl_subsurface_interface");
    IFACE(surface, "wl_surface_interface");
    IFACE(callback, "wl_callback_interface");
    IFACE(shm, "wl_shm_interface");
    IFACE(shmPool, "wl_shm_pool_interface");
    IFACE(buffer, "wl_buffer_interface");
    IFACE(seat, "wl_seat_interface");
    IFACE(pointer, "wl_pointer_interface");
    IFACE(keyboard, "wl_keyboard_interface");
    IFACE(output, "wl_output_interface");
    IFACE(dataDeviceManager, "wl_data_device_manager_interface");
    IFACE(dataDevice, "wl_data_device_interface");
    IFACE(dataSource, "wl_data_source_interface");
    IFACE(dataOffer, "wl_data_offer_interface");
#undef IFACE
    if (!W.displayConnect || !W.displayGetFd || !W.displayDispatchPending || !W.displayFlush || !W.displayRoundtrip || !W.displayPrepareRead ||
        !W.displayReadEvents || !W.displayCancelRead || !(W.marshalFlags || (W.marshalConstructorVersioned && W.marshal)) || !W.addListener ||
        !W.proxyDestroy || !W.proxyGetVersion || !W.registry || !W.compositor || !W.surface || !W.shm || !W.shmPool || !W.buffer || !W.seat ||
        !W.pointer || !W.keyboard || !W.output || !W.callback)
        return false;
    static const char* const xkbLibs[] = {"libxkbcommon.so.0", "libxkbcommon.so", NULL};
    if ((W.xkbLib = platformOpenLibrary(xkbLibs))) {
        void* x = W.xkbLib;
        SYM(x, xkbContextNew, "xkb_context_new");
        SYM(x, xkbContextUnref, "xkb_context_unref");
        SYM(x, xkbKeymapNewFromString, "xkb_keymap_new_from_string");
        SYM(x, xkbKeymapUnref, "xkb_keymap_unref");
        SYM(x, xkbKeymapKeyGetSymsByLevel, "xkb_keymap_key_get_syms_by_level");
        SYM(x, xkbKeymapKeyRepeats, "xkb_keymap_key_repeats");
        SYM(x, xkbStateNew, "xkb_state_new");
        SYM(x, xkbStateUnref, "xkb_state_unref");
        SYM(x, xkbStateUpdateMask, "xkb_state_update_mask");
        SYM(x, xkbStateKeyGetOneSym, "xkb_state_key_get_one_sym");
        SYM(x, xkbStateKeyGetUtf8, "xkb_state_key_get_utf8");
        SYM(x, xkbStateModNameIsActive, "xkb_state_mod_name_is_active");
        SYM(x, xkbStateKeyGetLayout, "xkb_state_key_get_layout");
        if (!W.xkbContextNew || !W.xkbKeymapNewFromString || !W.xkbStateNew || !W.xkbStateUpdateMask || !W.xkbStateKeyGetOneSym ||
            !W.xkbStateKeyGetUtf8 || !W.xkbStateModNameIsActive)
            W.xkbContextNew = NULL; /* (keys then come without text) */
    }
    static const char* const cursorLibs[] = {"libwayland-cursor.so.0", "libwayland-cursor.so", NULL};
    if ((W.cursorLib = platformOpenLibrary(cursorLibs))) {
        SYM(W.cursorLib, cursorThemeLoad, "wl_cursor_theme_load");
        SYM(W.cursorLib, cursorThemeDestroy, "wl_cursor_theme_destroy");
        SYM(W.cursorLib, cursorThemeGetCursor, "wl_cursor_theme_get_cursor");
        SYM(W.cursorLib, cursorImageGetBuffer, "wl_cursor_image_get_buffer");
        if (!W.cursorThemeLoad || !W.cursorThemeGetCursor || !W.cursorImageGetBuffer) W.cursorThemeLoad = NULL;
    }
    W.ok = true;
    return true;
}

/* Requests: a new object (its new_id passed as NULL among the arguments), a plain call, a
 * destructor; with wl_proxy_marshal_flags, or the calls libwayland had before it. */
#define WL_NEW(p, op, iface, ...)                                                                                          \
    (W.marshalFlags ? W.marshalFlags((struct wl_proxy*)(p), (op), (iface), W.proxyGetVersion((struct wl_proxy*)(p)), 0, __VA_ARGS__) \
                    : W.marshalConstructorVersioned((struct wl_proxy*)(p), (op), (iface), W.proxyGetVersion((struct wl_proxy*)(p)), __VA_ARGS__))
#define WL_CALL(p, op, ...)                                                                                                \
    (W.marshalFlags ? (void)W.marshalFlags((struct wl_proxy*)(p), (op), NULL, W.proxyGetVersion((struct wl_proxy*)(p)), 0, __VA_ARGS__) \
                    : W.marshal((struct wl_proxy*)(p), (op), __VA_ARGS__))
#define WL_CALL0(p, op)                                                                                                    \
    (W.marshalFlags ? (void)W.marshalFlags((struct wl_proxy*)(p), (op), NULL, W.proxyGetVersion((struct wl_proxy*)(p)), 0) \
                    : W.marshal((struct wl_proxy*)(p), (op)))
#define WL_DESTROY(p, op)                                                                                                  \
    do {                                                                                                                   \
        if (!(p)) break;                                                                                                   \
        if (W.marshalFlags) W.marshalFlags((struct wl_proxy*)(p), (op), NULL, W.proxyGetVersion((struct wl_proxy*)(p)), 1); \
        else { W.marshal((struct wl_proxy*)(p), (op)); W.proxyDestroy((struct wl_proxy*)(p)); }                          \
        (p) = NULL;                                                                                                        \
    } while (0)
#define WL_LISTEN(p, listener, data) W.addListener((struct wl_proxy*)(p), (void (**)(void))(listener), (data))

/* ---- xdg-shell and xdg-decoration, as wayland-scanner writes them ---------------------------- */

extern const struct wl_interface xdg_wm_base_interface, xdg_positioner_interface, xdg_surface_interface, xdg_toplevel_interface,
    xdg_popup_interface, zxdg_decoration_manager_v1_interface, zxdg_toplevel_decoration_v1_interface;

/* (their wl_* types filled in once libwayland is open) */
static const struct wl_interface* xdgTypes[] = {
    NULL, NULL, NULL, NULL,                                       /* 0: four plain arguments */
    &xdg_positioner_interface,                                    /* 4: create_positioner */
    &xdg_surface_interface, NULL /* wl_surface */,                /* 5: get_xdg_surface */
    &xdg_toplevel_interface,                                      /* 7: get_toplevel */
    &xdg_popup_interface, &xdg_surface_interface, &xdg_positioner_interface, /* 8: get_popup */
    &xdg_toplevel_interface,                                      /* 11: set_parent */
    NULL /* wl_seat */, NULL, NULL, NULL,                         /* 12: show_window_menu */
    NULL /* wl_seat */, NULL,                                     /* 16: move */
    NULL /* wl_seat */, NULL, NULL,                               /* 18: resize */
    NULL /* wl_output */,                                         /* 21: set_fullscreen */
    &zxdg_toplevel_decoration_v1_interface, &xdg_toplevel_interface, /* 22: get_toplevel_decoration */
    NULL /* wl_seat */, NULL,                                     /* 24: popup grab */
};

static const struct wl_message xdgWmBaseRequests[] = {
    {"destroy", "", xdgTypes}, {"create_positioner", "n", xdgTypes + 4}, {"get_xdg_surface", "no", xdgTypes + 5}, {"pong", "u", xdgTypes}};
static const struct wl_message xdgWmBaseEvents[] = {{"ping", "u", xdgTypes}};
const struct wl_interface xdg_wm_base_interface = {"xdg_wm_base", 2, 4, xdgWmBaseRequests, 1, xdgWmBaseEvents};

static const struct wl_message xdgPositionerRequests[] = {{"destroy", "", xdgTypes}};
const struct wl_interface xdg_positioner_interface = {"xdg_positioner", 2, 1, xdgPositionerRequests, 0, NULL};

static const struct wl_message xdgSurfaceRequests[] = {
    {"destroy", "", xdgTypes}, {"get_toplevel", "n", xdgTypes + 7}, {"get_popup", "n?oo", xdgTypes + 8},
    {"set_window_geometry", "iiii", xdgTypes}, {"ack_configure", "u", xdgTypes}};
static const struct wl_message xdgSurfaceEvents[] = {{"configure", "u", xdgTypes}};
const struct wl_interface xdg_surface_interface = {"xdg_surface", 2, 5, xdgSurfaceRequests, 1, xdgSurfaceEvents};

static const struct wl_message xdgToplevelRequests[] = {
    {"destroy", "", xdgTypes}, {"set_parent", "?o", xdgTypes + 11}, {"set_title", "s", xdgTypes}, {"set_app_id", "s", xdgTypes},
    {"show_window_menu", "ouii", xdgTypes + 12}, {"move", "ou", xdgTypes + 16}, {"resize", "ouu", xdgTypes + 18}, {"set_max_size", "ii", xdgTypes},
    {"set_min_size", "ii", xdgTypes}, {"set_maximized", "", xdgTypes}, {"unset_maximized", "", xdgTypes}, {"set_fullscreen", "?o", xdgTypes + 21},
    {"unset_fullscreen", "", xdgTypes}, {"set_minimized", "", xdgTypes}};
static const struct wl_message xdgToplevelEvents[] = {{"configure", "iia", xdgTypes}, {"close", "", xdgTypes}};
const struct wl_interface xdg_toplevel_interface = {"xdg_toplevel", 2, 14, xdgToplevelRequests, 2, xdgToplevelEvents};

static const struct wl_message xdgPopupRequests[] = {{"destroy", "", xdgTypes}, {"grab", "ou", xdgTypes + 24}};
static const struct wl_message xdgPopupEvents[] = {{"configure", "iiii", xdgTypes}, {"popup_done", "", xdgTypes}};
const struct wl_interface xdg_popup_interface = {"xdg_popup", 2, 2, xdgPopupRequests, 2, xdgPopupEvents};

static const struct wl_message decorationManagerRequests[] = {{"destroy", "", xdgTypes}, {"get_toplevel_decoration", "no", xdgTypes + 22}};
const struct wl_interface zxdg_decoration_manager_v1_interface = {"zxdg_decoration_manager_v1", 1, 2, decorationManagerRequests, 0, NULL};

static const struct wl_message toplevelDecorationRequests[] = {{"destroy", "", xdgTypes}, {"set_mode", "u", xdgTypes}, {"unset_mode", "", xdgTypes}};
static const struct wl_message toplevelDecorationEvents[] = {{"configure", "u", xdgTypes}};
const struct wl_interface zxdg_toplevel_decoration_v1_interface = {"zxdg_toplevel_decoration_v1", 1, 3, toplevelDecorationRequests, 1, toplevelDecorationEvents};

static void fillXdgTypes(void) {
    xdgTypes[6] = W.surface;
    xdgTypes[12] = xdgTypes[16] = xdgTypes[18] = xdgTypes[24] = W.seat;
    xdgTypes[21] = W.output;
}

/* opcodes */
enum { REGISTRY_BIND = 0, COMPOSITOR_CREATE_SURFACE = 0, SUBCOMPOSITOR_GET_SUBSURFACE = 1, SUBSURFACE_DESTROY = 0, SUBSURFACE_SET_POSITION = 1,
       SUBSURFACE_SET_DESYNC = 5, SURFACE_DESTROY = 0, SURFACE_ATTACH = 1, SURFACE_DAMAGE = 2, SURFACE_COMMIT = 6, SURFACE_SET_BUFFER_SCALE = 8,
       SURFACE_DAMAGE_BUFFER = 9, SHM_CREATE_POOL = 0, SHM_POOL_CREATE_BUFFER = 0, SHM_POOL_DESTROY = 1, BUFFER_DESTROY = 0,
       SEAT_GET_POINTER = 0, SEAT_GET_KEYBOARD = 1, POINTER_SET_CURSOR = 0, POINTER_RELEASE = 1, KEYBOARD_RELEASE = 0,
       DDM_CREATE_DATA_SOURCE = 0, DDM_GET_DATA_DEVICE = 1, DATA_DEVICE_SET_SELECTION = 1, DATA_DEVICE_RELEASE = 2,
       DATA_OFFER_ACCEPT = 0, DATA_OFFER_RECEIVE = 1, DATA_OFFER_DESTROY = 2, DATA_OFFER_FINISH = 3, DATA_OFFER_SET_ACTIONS = 4,
       DATA_SOURCE_OFFER = 0, DATA_SOURCE_DESTROY = 1, DISPLAY_GET_REGISTRY = 1, OUTPUT_RELEASE = 3,
       WM_BASE_DESTROY = 0, WM_BASE_GET_XDG_SURFACE = 2, WM_BASE_PONG = 3, XDG_SURFACE_DESTROY = 0, XDG_SURFACE_GET_TOPLEVEL = 1,
       XDG_SURFACE_SET_WINDOW_GEOMETRY = 3, XDG_SURFACE_ACK_CONFIGURE = 4, TOPLEVEL_DESTROY = 0, TOPLEVEL_SET_TITLE = 2, TOPLEVEL_SET_APP_ID = 3,
       TOPLEVEL_SHOW_WINDOW_MENU = 4, TOPLEVEL_MOVE = 5, TOPLEVEL_RESIZE = 6, TOPLEVEL_SET_MAX_SIZE = 7, TOPLEVEL_SET_MIN_SIZE = 8,
       TOPLEVEL_SET_MAXIMIZED = 9, TOPLEVEL_UNSET_MAXIMIZED = 10, TOPLEVEL_SET_FULLSCREEN = 11, TOPLEVEL_UNSET_FULLSCREEN = 12,
       TOPLEVEL_SET_MINIMIZED = 13, DECORATION_MANAGER_DESTROY = 0, DECORATION_MANAGER_GET = 1, DECORATION_DESTROY = 0, DECORATION_SET_MODE = 1 };

/* ---- the window ------------------------------------------------------------------------------- */

#define TITLE_H 30 /* (our own title bar, CSS px) */
#define EDGE 6     /* (the frame's edges that resize it, CSS px) */
#define MAX_OUTPUTS 16

typedef struct Output { void* proxy; uint32_t name; int scale; } Output;

/* A buffer of shared memory for a surface (two for the window, one for the title bar). */
typedef struct ShmBuffer { void* buffer; uint32_t* pixels; int width, height; size_t size; bool busy; } ShmBuffer;

struct CscdWindowNative {
    CscdWindow* w;
    struct wl_display* dpy;
    void *registry, *compositor, *subcompositor, *shm, *seat, *wmBase, *decorationManager, *dataDeviceManager, *dataDevice;
    void *surface, *xdgSurface, *toplevel, *decoration;
    CscdWaylandWindow handle;
    Output outputs[MAX_OUTPUTS];
    int outputCount;
    void* entered[MAX_OUTPUTS]; /* the outputs the window is on */
    int enteredCount;
    int scale;
    /* the size the compositor gave (CSS px; 0: ours), and states */
    int pendingW, pendingH;
    bool maximized, activated, configured;
    uint32_t configureSerial;
    bool serverDecorations;
    /* our title bar */
    void *titleSurface, *titleSubsurface;
    ShmBuffer titleBuffer;
    bool titleDirty;
    char title[256];
    int titleHover; /* 0 none, 1 close, 2 maximize, 3 minimize */
    /* input */
    void *pointer, *keyboard;
    void* pointerSurface; /* the surface the pointer is on */
    float px, py;         /* its position there (CSS px) */
    int buttons;
    uint32_t pointerSerial, inputSerial;
    ClickCounter clicks;
    float wheelX, wheelY;
    void *xkbContext, *xkbKeymap, *xkbState;
    CscdModifiers mods;
    int repeatRate, repeatDelay;
    uint32_t repeatKey;
    double repeatAt;
    /* the cursor */
    struct wl_cursor_theme* cursorTheme;
    void* cursorSurface;
    CscdCursor shown;
    int frameCursor; /* the frame's resize cursor shown instead (-1: none) */
    bool cursorShown;
    /* the clipboard */
    void* selectionOffer;
    CscdStrVec selectionTypes;
    void* ownSource;
    CscdStr clipText;
    /* a drag from another app */
    void* dragOffer;
    CscdStrVec dragTypes;
    bool dragEntered;
    float dragX, dragY;
    uint32_t dragSerial;
    /* the offers being announced, and their types */
    CSCD_VEC(void*) offers;
    CSCD_VEC(CscdStrVec*) offerTypes;
    int wake[2];
    bool closeRequested;
};

static void wakeUp(CscdWindowNative* n) {
    if (n->wake[1] >= 0) {
        const char c = 1;
        (void)!write(n->wake[1], &c, 1);
    }
}

static double nowSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int titleHeight(const CscdWindowNative* n) { return n->serverDecorations || n->w->fullscreen ? 0 : TITLE_H; }

/* ---- shared memory buffers ---- */

static void bufferRelease(void* data, void* buffer) {
    (void)buffer;
    ((ShmBuffer*)data)->busy = false;
}
static const struct { void (*release)(void*, void*); } bufferListener = {bufferRelease};

static void shmFree(ShmBuffer* b) {
    WL_DESTROY(b->buffer, BUFFER_DESTROY);
    if (b->pixels) munmap(b->pixels, b->size);
    memset(b, 0, sizeof *b);
}

static int anonymousFile(size_t size) {
    char name[64];
    snprintf(name, sizeof name, "/cascade-%d-%lx", (int)getpid(), (unsigned long)(nowSeconds() * 1e6));
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    while (ftruncate(fd, (off_t)size) < 0) {
        if (errno == EINTR) continue;
        close(fd);
        return -1;
    }
    return fd;
}

/* A buffer of width x height (device pixels), ARGB8888: premultiplied BGRA in memory, the frame's format. */
static bool shmMake(CscdWindowNative* n, ShmBuffer* b, int width, int height) {
    shmFree(b);
    const size_t stride = (size_t)width * 4, size = stride * (size_t)height;
    if (!size) return false;
    const int fd = anonymousFile(size);
    if (fd < 0) return false;
    void* map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return false; }
    void* pool = WL_NEW(n->shm, SHM_CREATE_POOL, W.shmPool, NULL, fd, (int32_t)size);
    b->buffer = WL_NEW(pool, SHM_POOL_CREATE_BUFFER, W.buffer, NULL, 0, width, height, (int32_t)stride, 0 /* ARGB8888 */);
    WL_DESTROY(pool, SHM_POOL_DESTROY);
    close(fd);
    b->pixels = (uint32_t*)map;
    b->width = width;
    b->height = height;
    b->size = size;
    WL_LISTEN(b->buffer, &bufferListener, b);
    return b->buffer != NULL;
}

/* ---- the software presenter: frames into two buffers, taking turns ---- */

typedef struct ShmPresenter {
    CscdPresenter base;
    CscdWindowNative* n;
    ShmBuffer buffers[2];
} ShmPresenter;

static CscdBackend shmBackend(const CscdPresenter* p) { (void)p; return CSCD_BACKEND_SOFTWARE; }
static const char* shmDescription(const CscdPresenter* p) { (void)p; return "Wayland shared memory"; }
static void shmResize(CscdPresenter* p, int width, int height) { (void)p; (void)width; (void)height; }

static bool shmPresent(CscdPresenter* base, const CscdImage* frame) {
    ShmPresenter* p = (ShmPresenter*)base;
    CscdWindowNative* n = p->n;
    if (!n || !n->surface || cscdImageEmpty(frame)) return false;
    ShmBuffer* b = NULL;
    for (int i = 0; i < 2 && !b; ++i)
        if (!p->buffers[i].busy) b = &p->buffers[i];
    if (!b) { /* (both still with the compositor: a third would do, waiting is simpler) */
        W.displayRoundtrip(n->dpy);
        for (int i = 0; i < 2 && !b; ++i)
            if (!p->buffers[i].busy) b = &p->buffers[i];
        if (!b) return false;
    }
    if (b->width != frame->width || b->height != frame->height)
        if (!shmMake(n, b, frame->width, frame->height)) return false;
    memcpy(b->pixels, frame->pixels, cscdImagePixelCount(frame) * 4);
    b->busy = true;
    WL_CALL(n->surface, SURFACE_ATTACH, b->buffer, 0, 0);
    if (W.proxyGetVersion((struct wl_proxy*)n->surface) >= 4) {
        if (base->partial && base->damageCount)
            for (size_t i = 0; i < base->damageCount; ++i)
                WL_CALL(n->surface, SURFACE_DAMAGE_BUFFER, base->damage[i].x, base->damage[i].y, base->damage[i].w, base->damage[i].h);
        else WL_CALL(n->surface, SURFACE_DAMAGE_BUFFER, 0, 0, frame->width, frame->height);
    } else WL_CALL(n->surface, SURFACE_DAMAGE, 0, 0, INT32_MAX, INT32_MAX);
    WL_CALL0(n->surface, SURFACE_COMMIT);
    W.displayFlush(n->dpy);
    return true;
}

static void shmDestroy(CscdPresenter* base) {
    ShmPresenter* p = (ShmPresenter*)base;
    for (int i = 0; i < 2; ++i) shmFree(&p->buffers[i]);
    cscdFree(p);
}

static const CscdPresenterVtbl shmVtbl = {shmBackend, shmPresent, shmResize, shmDescription, shmDestroy};

/* (the window that owns a surface given to cscdPresenterNew) */
static CscdWindowNative* windows[16];

CscdPresenter* platformWaylandSoftwarePresenter(void* nativeWindow, int width, int height) {
    (void)width;
    (void)height;
    const CscdWaylandWindow* h = (const CscdWaylandWindow*)nativeWindow;
    CscdWindowNative* n = NULL;
    for (int i = 0; i < 16 && !n; ++i)
        if (windows[i] && windows[i]->surface == h->surface) n = windows[i];
    if (!n) return NULL;
    ShmPresenter* p = CSCD_NEW(ShmPresenter);
    p->base.vt = &shmVtbl;
    p->n = n;
    return &p->base;
}

/* ---- our title bar (when the compositor draws no frame) ---- */

static CscdRect titleButton(const CscdWindowNative* n, int which) {
    const float w = n->w->clientW / n->w->dpr;
    return cscdRect(w - TITLE_H * which, 0, TITLE_H, TITLE_H); /* 1 close (right), 2 maximize, 3 minimize */
}

static void drawTitleBar(CscdWindowNative* n) {
    CscdWindow* w = n->w;
    if (!n->titleSurface || !n->titleDirty) return;
    n->titleDirty = false;
    if (!titleHeight(n)) {
        WL_CALL(n->titleSurface, SURFACE_ATTACH, NULL, 0, 0);
        WL_CALL0(n->titleSurface, SURFACE_COMMIT);
        return;
    }
    const int s = n->scale, width = w->clientW, height = TITLE_H * s;
    if (n->titleBuffer.busy) { n->titleDirty = true; return; }
    if (n->titleBuffer.width != width || n->titleBuffer.height != height)
        if (!shmMake(n, &n->titleBuffer, width, height)) return;
    CscdImage img = {width, height, n->titleBuffer.pixels};
    CscdCanvas* c = cscdCanvasNew(&img);
    cscdCanvasScale(c, (float)s, (float)s);
    const float lw = (float)width / s;
    const CscdColor bg = n->activated ? cscdRgb(235, 235, 237) : cscdRgb(246, 246, 247), fg = n->activated ? cscdRgb(40, 40, 44) : cscdRgb(140, 140, 146);
    cscdCanvasFillRect(c, cscdRect(0, 0, lw, TITLE_H), bg);
    cscdCanvasFillRect(c, cscdRect(0, TITLE_H - 1, lw, 1), cscdRgb(210, 210, 214));
    CscdStrVec fams = {0};
    cscdVecPush(&fams, cscdStrDupC("system-ui"));
    CscdFont f = cscdFontMatch(&fams, 13, 600, false);
    cscdStrVecFree(&fams);
    const float tw = cscdCanvasMeasureText(c, cscdSv(n->title), &f);
    cscdCanvasDrawText(c, cscdSv(n->title), cscdMaxf(10, (lw - tw) / 2), TITLE_H / 2.f + 5, &f, fg);
    for (int b = 1; b <= 3; ++b) {
        const CscdRect r = titleButton(n, b);
        if (n->titleHover == b) cscdCanvasFillRect(c, r, b == 1 ? cscdRgb(224, 67, 54) : cscdRgb(218, 218, 222));
        const CscdColor ic = n->titleHover == 1 && b == 1 ? cscdRgb(255, 255, 255) : fg;
        const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        CscdPath p = {0};
        if (b == 1) { cscdPathMoveTo(&p, cx - 5, cy - 5); cscdPathLineTo(&p, cx + 5, cy + 5); cscdPathMoveTo(&p, cx + 5, cy - 5); cscdPathLineTo(&p, cx - 5, cy + 5); }
        else if (b == 2) cscdPathAddRect(&p, cscdRect(cx - 5, cy - 5, 10, 10));
        else { cscdPathMoveTo(&p, cx - 5, cy + 4); cscdPathLineTo(&p, cx + 5, cy + 4); }
        cscdCanvasStrokePath(c, &p, ic, 1.2f);
        cscdPathFree(&p);
    }
    cscdCanvasFree(c);
    n->titleBuffer.busy = true;
    WL_CALL(n->titleSurface, SURFACE_SET_BUFFER_SCALE, s);
    WL_CALL(n->titleSurface, SURFACE_ATTACH, n->titleBuffer.buffer, 0, 0);
    WL_CALL(n->titleSurface, SURFACE_DAMAGE, 0, 0, INT32_MAX, INT32_MAX);
    WL_CALL0(n->titleSurface, SURFACE_COMMIT);
}

/* the window's outline for the compositor: the title bar above the page */
static void setGeometry(CscdWindowNative* n) {
    const int th = titleHeight(n);
    WL_CALL(n->xdgSurface, XDG_SURFACE_SET_WINDOW_GEOMETRY, 0, -th, (int)(n->w->clientW / n->w->dpr), (int)(n->w->clientH / n->w->dpr) + th);
    if (n->titleSubsurface) WL_CALL(n->titleSubsurface, SUBSURFACE_SET_POSITION, 0, -th);
}

/* ---- sizes and scale ---- */

static void applySize(CscdWindowNative* n, int cssW, int cssH) {
    CscdWindow* w = n->w;
    const int s = n->scale;
    const int cw = cscdMaxi(1, cssW * s), ch = cscdMaxi(1, cssH * s);
    const bool scaleChanged = w->dpr != (float)s;
    w->dpr = (float)s;
    if (cw == w->clientW && ch == w->clientH && !scaleChanged) return;
    w->clientW = cw;
    w->clientH = ch;
    WL_CALL(n->surface, SURFACE_SET_BUFFER_SCALE, s);
    if (w->presenter) cscdPresenterResize(w->presenter, cw, ch);
    cscdViewSetViewport(w->view, (float)cssW, (float)cssH, (float)s);
    n->titleDirty = true;
    w->dirty = true;
}

static void updateScale(CscdWindowNative* n) {
    int s = 1;
    for (int i = 0; i < n->enteredCount; ++i)
        for (int k = 0; k < n->outputCount; ++k)
            if (n->outputs[k].proxy == n->entered[i]) s = cscdMaxi(s, n->outputs[k].scale);
    if (s == n->scale) return;
    const int cssW = (int)(n->w->clientW / n->w->dpr), cssH = (int)(n->w->clientH / n->w->dpr);
    n->scale = s;
    if (n->cursorTheme && W.cursorThemeDestroy) { W.cursorThemeDestroy(n->cursorTheme); n->cursorTheme = NULL; }
    n->cursorShown = false;
    applySize(n, cssW, cssH);
}

/* ---- output ---- */

static void outputGeometry(void* d, void* o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub, const char* make, const char* model, int32_t t) {
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)t;
}
static void outputMode(void* d, void* o, uint32_t flags, int32_t w, int32_t h, int32_t r) { (void)d; (void)o; (void)flags; (void)w; (void)h; (void)r; }
static void outputDone(void* d, void* o) { (void)o; updateScale((CscdWindowNative*)d); }
static void outputScale(void* d, void* o, int32_t factor) {
    CscdWindowNative* n = (CscdWindowNative*)d;
    for (int i = 0; i < n->outputCount; ++i)
        if (n->outputs[i].proxy == o) n->outputs[i].scale = cscdMaxi(1, factor);
}
static void outputName(void* d, void* o, const char* s) { (void)d; (void)o; (void)s; }
static const struct {
    void (*geometry)(void*, void*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t);
    void (*mode)(void*, void*, uint32_t, int32_t, int32_t, int32_t);
    void (*done)(void*, void*);
    void (*scale)(void*, void*, int32_t);
    void (*name)(void*, void*, const char*);
    void (*description)(void*, void*, const char*);
} outputListener = {outputGeometry, outputMode, outputDone, outputScale, outputName, outputName};

/* ---- the surface ---- */

static void surfaceEnter(void* d, void* s, void* output) {
    (void)s;
    CscdWindowNative* n = (CscdWindowNative*)d;
    if (n->enteredCount < MAX_OUTPUTS) n->entered[n->enteredCount++] = output;
    updateScale(n);
}
static void surfaceLeave(void* d, void* s, void* output) {
    (void)s;
    CscdWindowNative* n = (CscdWindowNative*)d;
    for (int i = 0; i < n->enteredCount; ++i)
        if (n->entered[i] == output) { n->entered[i] = n->entered[--n->enteredCount]; break; }
    updateScale(n);
}
static void surfacePreferredScale(void* d, void* s, int32_t factor) { (void)d; (void)s; (void)factor; }
static void surfacePreferredTransform(void* d, void* s, uint32_t t) { (void)d; (void)s; (void)t; }
static const struct {
    void (*enter)(void*, void*, void*);
    void (*leave)(void*, void*, void*);
    void (*preferredScale)(void*, void*, int32_t);
    void (*preferredTransform)(void*, void*, uint32_t);
} surfaceListener = {surfaceEnter, surfaceLeave, surfacePreferredScale, surfacePreferredTransform};

/* ---- xdg ---- */

static void wmPing(void* d, void* wm, uint32_t serial) { (void)d; WL_CALL(wm, WM_BASE_PONG, serial); }
static const struct { void (*ping)(void*, void*, uint32_t); } wmListener = {wmPing};

static void xdgConfigure(void* d, void* xs, uint32_t serial) {
    CscdWindowNative* n = (CscdWindowNative*)d;
    WL_CALL(xs, XDG_SURFACE_ACK_CONFIGURE, serial);
    n->configured = true;
    CscdWindow* w = n->w;
    int cssW = n->pendingW, cssH = n->pendingH;
    if (cssW <= 0 || cssH <= 0) { cssW = (int)(w->clientW / w->dpr); cssH = (int)(w->clientH / w->dpr); }
    else cssH -= titleHeight(n); /* (the size given is the window's, title bar included) */
    applySize(n, cssW, cscdMaxi(1, cssH));
    setGeometry(n);
    n->titleDirty = true;
    w->dirty = true;
}
static const struct { void (*configure)(void*, void*, uint32_t); } xdgSurfaceListener = {xdgConfigure};

static void toplevelConfigure(void* d, void* t, int32_t width, int32_t height, struct wl_array* states) {
    (void)t;
    CscdWindowNative* n = (CscdWindowNative*)d;
    n->pendingW = width;
    n->pendingH = height;
    bool maximized = false, activated = false, fullscreen = false;
    for (uint32_t* s = (uint32_t*)states->data; (char*)s < (char*)states->data + states->size; ++s) {
        if (*s == 1) maximized = true;
        else if (*s == 2) fullscreen = true;
        else if (*s == 4) activated = true;
    }
    if (activated != n->activated || maximized != n->maximized) n->titleDirty = true;
    if (!activated && n->activated) {
        cscdViewCloseMenus(n->w->view);
        cscdViewHideTooltip(n->w->view);
    }
    n->maximized = maximized;
    n->activated = activated;
    n->w->fullscreen = fullscreen;
}
static void toplevelClose(void* d, void* t) { (void)t; ((CscdWindowNative*)d)->closeRequested = true; }
static void toplevelBounds(void* d, void* t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void toplevelCapabilities(void* d, void* t, struct wl_array* a) { (void)d; (void)t; (void)a; }
static const struct {
    void (*configure)(void*, void*, int32_t, int32_t, struct wl_array*);
    void (*close)(void*, void*);
    void (*bounds)(void*, void*, int32_t, int32_t);
    void (*capabilities)(void*, void*, struct wl_array*);
} toplevelListener = {toplevelConfigure, toplevelClose, toplevelBounds, toplevelCapabilities};

static void decorationConfigure(void* d, void* dec, uint32_t mode) {
    (void)dec;
    CscdWindowNative* n = (CscdWindowNative*)d;
    n->serverDecorations = mode == 2;
    n->titleDirty = true;
}
static const struct { void (*configure)(void*, void*, uint32_t); } decorationListener = {decorationConfigure};

/* ---- keys ---- */

/* The key from its keysym (the X11 values), as on the other systems. */
static CscdKey mapKey(uint32_t ks, char name[16]) {
#define K(n, key) do { strcpy(name, n); return key; } while (0)
    switch (ks) {
    case 0xff08: K("Backspace", CSCD_KEY_BACKSPACE);
    case 0xff09: case 0xfe20: K("Tab", CSCD_KEY_TAB);
    case 0xff0d: case 0xff8d: K("Enter", CSCD_KEY_ENTER);
    case 0xff1b: K("Escape", CSCD_KEY_ESCAPE);
    case 0x20: K(" ", CSCD_KEY_SPACE);
    case 0xffff: case 0xff9f: K("Delete", CSCD_KEY_DELETE);
    case 0xff51: case 0xff96: K("ArrowLeft", CSCD_KEY_LEFT);
    case 0xff53: case 0xff98: K("ArrowRight", CSCD_KEY_RIGHT);
    case 0xff52: case 0xff97: K("ArrowUp", CSCD_KEY_UP);
    case 0xff54: case 0xff99: K("ArrowDown", CSCD_KEY_DOWN);
    case 0xff50: case 0xff95: K("Home", CSCD_KEY_HOME);
    case 0xff57: case 0xff9c: K("End", CSCD_KEY_END);
    case 0xff55: case 0xff9a: K("PageUp", CSCD_KEY_PAGE_UP);
    case 0xff56: case 0xff9b: K("PageDown", CSCD_KEY_PAGE_DOWN);
    case 0xff63: case 0xff9e: K("Insert", CSCD_KEY_INSERT);
    case 0xffe1: case 0xffe2: K("Shift", CSCD_KEY_SHIFT);
    case 0xffe3: case 0xffe4: K("Control", CSCD_KEY_CONTROL);
    case 0xffe9: case 0xffea: case 0xffe7: case 0xffe8: K("Alt", CSCD_KEY_ALT);
    case 0xffeb: case 0xffec: K("Meta", CSCD_KEY_META);
    case 0xff67: K("ContextMenu", CSCD_KEY_UNKNOWN);
    case 'a': K("a", CSCD_KEY_A);
    case 'c': K("c", CSCD_KEY_C);
    case 'v': K("v", CSCD_KEY_V);
    case 'x': K("x", CSCD_KEY_X);
    case 'z': K("z", CSCD_KEY_Z);
    case 'y': K("y", CSCD_KEY_Y);
    default: break;
    }
#undef K
    if (ks >= 0xffbe && ks <= 0xffc9) {
        snprintf(name, 16, "F%d", (int)(ks - 0xffbe + 1));
        return (CscdKey)((int)CSCD_KEY_F1 + (int)(ks - 0xffbe));
    }
    if (ks >= 0x20 && ks < 0x7F) {
        name[0] = (char)ks;
        name[1] = 0;
        return CSCD_KEY_CHARACTER;
    }
    strcpy(name, "Unidentified");
    return CSCD_KEY_UNKNOWN;
}

/* The key's symbol without modifiers (level 0 of its group), like a Windows virtual key. */
static uint32_t baseSym(CscdWindowNative* n, uint32_t code) {
    const uint32_t* syms = NULL;
    if (W.xkbKeymapKeyGetSymsByLevel) {
        const uint32_t layout = W.xkbStateKeyGetLayout ? W.xkbStateKeyGetLayout(n->xkbState, code) : 0;
        if (W.xkbKeymapKeyGetSymsByLevel(n->xkbKeymap, code, layout, 0, &syms) > 0) return syms[0];
    }
    return W.xkbStateKeyGetOneSym(n->xkbState, code);
}

static void keyDown(CscdWindowNative* n, uint32_t key) {
    if (!n->xkbState) return;
    const uint32_t code = key + 8;
    char name[16];
    const CscdKey k = mapKey(baseSym(n, code), name);
    cscdViewKeyDown(n->w->view, k, n->mods, cscdSv(name));
    char text[64];
    const int len = W.xkbStateKeyGetUtf8(n->xkbState, code, text, sizeof text);
    if (len > 0 && !n->mods.ctrl && !n->mods.alt) {
        CscdStr clean = {0};
        for (int i = 0; i < len && i < (int)sizeof text; ++i) {
            const char c = text[i];
            if (c == '\r') cscdStrAppendChar(&clean, '\n');
            else if ((unsigned char)c >= 0x20 && c != 0x7F) cscdStrAppendChar(&clean, c);
        }
        if (clean.len) cscdViewTextInput(n->w->view, cscdStrSv(&clean));
        cscdStrFree(&clean);
    }
}

static void kbKeymap(void* d, void* kb, uint32_t format, int32_t fd, uint32_t size) {
    (void)kb;
    CscdWindowNative* n = (CscdWindowNative*)d;
    if (format != 1 || !W.xkbContextNew) { close(fd); return; }
    char* map = (char*)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    if (!n->xkbContext) n->xkbContext = W.xkbContextNew(0);
    void* keymap = n->xkbContext ? W.xkbKeymapNewFromString(n->xkbContext, map, 1, 0) : NULL;
    munmap(map, size);
    if (!keymap) return;
    if (n->xkbState) W.xkbStateUnref(n->xkbState);
    if (n->xkbKeymap) W.xkbKeymapUnref(n->xkbKeymap);
    n->xkbKeymap = keymap;
    n->xkbState = W.xkbStateNew(keymap);
}
static void kbEnter(void* d, void* kb, uint32_t serial, void* surface, struct wl_array* keys) {
    (void)kb; (void)surface; (void)keys;
    CscdWindowNative* n = (CscdWindowNative*)d;
    n->inputSerial = serial;
    windowPlaceInputMethod(n->w);
}
static void kbLeave(void* d, void* kb, uint32_t serial, void* surface) {
    (void)kb; (void)serial; (void)surface;
    CscdWindowNative* n = (CscdWindowNative*)d;
    n->repeatKey = 0;
    cscdViewCloseMenus(n->w->view);
    cscdViewHideTooltip(n->w->view);
    cscdViewCancelDrag(n->w->view);
}
static void kbKey(void* d, void* kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)kb; (void)time;
    CscdWindowNative* n = (CscdWindowNative*)d;
    n->inputSerial = serial;
    if (state == 1) {
        keyDown(n, key);
        if (n->repeatRate > 0 && (!W.xkbKeymapKeyRepeats || !n->xkbKeymap || W.xkbKeymapKeyRepeats(n->xkbKeymap, key + 8))) {
            n->repeatKey = key + 1;
            n->repeatAt = nowSeconds() + n->repeatDelay / 1000.0;
        }
    } else {
        if (n->repeatKey == key + 1) n->repeatKey = 0;
        if (n->xkbState) {
            char name[16];
            cscdViewKeyUp(n->w->view, mapKey(baseSym(n, key + 8), name), n->mods, cscdSv(name));
        }
    }
    windowPlaceInputMethod(n->w);
}
static void kbModifiers(void* d, void* kb, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    (void)kb; (void)serial;
    CscdWindowNative* n = (CscdWindowNative*)d;
    if (!n->xkbState) return;
    W.xkbStateUpdateMask(n->xkbState, depressed, latched, locked, 0, 0, group);
    n->mods.shift = W.xkbStateModNameIsActive(n->xkbState, "Shift", 8) > 0;
    n->mods.ctrl = W.xkbStateModNameIsActive(n->xkbState, "Control", 8) > 0;
    n->mods.alt = W.xkbStateModNameIsActive(n->xkbState, "Mod1", 8) > 0;
    n->mods.meta = W.xkbStateModNameIsActive(n->xkbState, "Mod4", 8) > 0;
}
static void kbRepeatInfo(void* d, void* kb, int32_t rate, int32_t delay) {
    (void)kb;
    CscdWindowNative* n = (CscdWindowNative*)d;
    n->repeatRate = rate;
    n->repeatDelay = delay;
}
static const struct {
    void (*keymap)(void*, void*, uint32_t, int32_t, uint32_t);
    void (*enter)(void*, void*, uint32_t, void*, struct wl_array*);
    void (*leave)(void*, void*, uint32_t, void*);
    void (*key)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*modifiers)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*repeatInfo)(void*, void*, int32_t, int32_t);
} keyboardListener = {kbKeymap, kbEnter, kbLeave, kbKey, kbModifiers, kbRepeatInfo};

/* ---- the cursor ---- */

static const char* const* cursorNames(CscdCursor c) {
    static const char* const def[] = {"default", "left_ptr", NULL}, *pointer[] = {"pointer", "hand2", "hand1", NULL}, *text[] = {"text", "xterm", NULL},
                             *move[] = {"move", "fleur", NULL}, *grab[] = {"grab", "openhand", "fleur", NULL}, *grabbing[] = {"grabbing", "closedhand", "fleur", NULL},
                             *no[] = {"not-allowed", "crossed_circle", NULL}, *copy[] = {"copy", NULL}, *cross[] = {"crosshair", "cross", NULL},
                             *wait[] = {"wait", "watch", NULL}, *progress[] = {"progress", "left_ptr_watch", NULL}, *help[] = {"help", "question_arrow", NULL},
                             *ew[] = {"ew-resize", "sb_h_double_arrow", NULL}, *ns[] = {"ns-resize", "sb_v_double_arrow", NULL},
                             *nesw[] = {"nesw-resize", "fd_double_arrow", NULL}, *nwse[] = {"nwse-resize", "bd_double_arrow", NULL},
                             *col[] = {"col-resize", "sb_h_double_arrow", NULL}, *row[] = {"row-resize", "sb_v_double_arrow", NULL};
    switch (c) {
    case CSCD_CURSOR_POINTER: return pointer;
    case CSCD_CURSOR_TEXT: return text;
    case CSCD_CURSOR_MOVE: return move;
    case CSCD_CURSOR_GRAB: return grab;
    case CSCD_CURSOR_GRABBING: return grabbing;
    case CSCD_CURSOR_NOT_ALLOWED: return no;
    case CSCD_CURSOR_COPY: return copy;
    case CSCD_CURSOR_CROSSHAIR: return cross;
    case CSCD_CURSOR_WAIT: return wait;
    case CSCD_CURSOR_PROGRESS: return progress;
    case CSCD_CURSOR_HELP: return help;
    case CSCD_CURSOR_EW_RESIZE: return ew;
    case CSCD_CURSOR_NS_RESIZE: return ns;
    case CSCD_CURSOR_NESW_RESIZE: return nesw;
    case CSCD_CURSOR_NWSE_RESIZE: return nwse;
    case CSCD_CURSOR_COL_RESIZE: return col;
    case CSCD_CURSOR_ROW_RESIZE: return row;
    default: return def;
    }
}

static void showCursor(CscdWindowNative* n, CscdCursor c) {
    if (!n->pointer || !W.cursorThemeLoad) return;
    if (!n->cursorTheme) {
        const char* size = getenv("XCURSOR_SIZE");
        const int px = (size && atoi(size) > 0 ? atoi(size) : 24) * n->scale;
        n->cursorTheme = W.cursorThemeLoad(getenv("XCURSOR_THEME"), px, n->shm);
        if (!n->cursorTheme) return;
    }
    struct wl_cursor* cur = NULL;
    for (const char* const* name = cursorNames(c); *name && !cur; ++name) cur = W.cursorThemeGetCursor(n->cursorTheme, *name);
    if (!cur) cur = W.cursorThemeGetCursor(n->cursorTheme, "left_ptr");
    if (!cur || !cur->image_count) return;
    struct wl_cursor_image* img = cur->images[0];
    void* buffer = W.cursorImageGetBuffer(img);
    if (!buffer) return;
    if (!n->cursorSurface) n->cursorSurface = WL_NEW(n->compositor, COMPOSITOR_CREATE_SURFACE, W.surface, NULL);
    WL_CALL(n->cursorSurface, SURFACE_SET_BUFFER_SCALE, n->scale);
    WL_CALL(n->cursorSurface, SURFACE_ATTACH, buffer, 0, 0);
    WL_CALL(n->cursorSurface, SURFACE_DAMAGE, 0, 0, INT32_MAX, INT32_MAX);
    WL_CALL0(n->cursorSurface, SURFACE_COMMIT);
    WL_CALL(n->pointer, POINTER_SET_CURSOR, n->pointerSerial, n->cursorSurface, (int32_t)(img->hotspot_x / n->scale), (int32_t)(img->hotspot_y / n->scale));
}

/* ---- the pointer ---- */

/* The frame's edge under the pointer on the page (its xdg resize edge; 0: none), when the window
 * draws its own frame and is not maximized. */
static uint32_t edgeAt(const CscdWindowNative* n, float x, float y) {
    if (n->serverDecorations || n->maximized || n->w->fullscreen) return 0;
    const float w = n->w->clientW / n->w->dpr, h = n->w->clientH / n->w->dpr;
    const bool l = x < EDGE, r = x > w - EDGE, b = y > h - EDGE;
    return b ? (l ? 6 : r ? 10 : 2) : l ? 4 : r ? 8 : 0;
}

static CscdCursor edgeCursor(uint32_t edge) {
    switch (edge) {
    case 2: return CSCD_CURSOR_NS_RESIZE;
    case 4: case 8: return CSCD_CURSOR_EW_RESIZE;
    case 6: return CSCD_CURSOR_NESW_RESIZE;
    case 10: return CSCD_CURSOR_NWSE_RESIZE;
    default: return CSCD_CURSOR_DEFAULT;
    }
}

static void ptrEnter(void* d, void* p, uint32_t serial, void* surface, wl_fixed_t x, wl_fixed_t y) {
    (void)p;
    CscdWindowNative* n = (CscdWindowNative*)d;
    n->pointerSerial = serial;
    n->pointerSurface = surface;
    n->px = fixedToFloat(x);
    n->py = fixedToFloat(y);
    n->cursorShown = false;
}
static void ptrLeave(void* d, void* p, uint32_t serial, void* surface) {
    (void)p; (void)serial;
    CscdWindowNative* n = (CscdWindowNative*)d;
    if (surface == n->surface && !n->buttons) cscdViewMouseLeave(n->w->view);
    if (surface == n->titleSurface && n->titleHover) { n->titleHover = 0; n->titleDirty = true; }
    n->pointerSurface = NULL;
}
static void ptrMotion(void* d, void* p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)p; (void)time;
    CscdWindowNative* n = (CscdWindowNative*)d;
    n->px = fixedToFloat(x);
    n->py = fixedToFloat(y);
    if (n->pointerSurface == n->titleSurface) {
        int hover = 0;
        for (int b = 1; b <= 3; ++b)
            if (cscdRectContains(titleButton(n, b), n->px, n->py)) hover = b;
        if (hover != n->titleHover) { n->titleHover = hover; n->titleDirty = true; }
        return;
    }
    const uint32_t edge = n->buttons ? 0 : edgeAt(n, n->px, n->py);
    if ((int)edge != n->frameCursor) { n->frameCursor = (int)edge; n->cursorShown = false; }
    if (!edge) cscdViewMouseMove(n->w->view, n->px, n->py, n->buttons, n->mods);
}
static void ptrButton(void* d, void* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    (void)p;
    CscdWindowNative* n = (CscdWindowNative*)d;
    n->inputSerial = serial;
    /* BTN_LEFT, BTN_RIGHT, BTN_MIDDLE */
    const int b = button == 0x110 ? 0 : button == 0x111 ? 2 : button == 0x112 ? 1 : -1;
    if (b < 0) return;
    const int bit = b == 0 ? 1 : b == 2 ? 2 : 4;
    if (n->pointerSurface == n->titleSurface) {
        if (state == 1 && b == 0) {
            const int clicks = clickPress(&n->clicks, time, (int)n->px, (int)n->py, 400);
            if (!n->titleHover && clicks == 2) WL_CALL0(n->toplevel, n->maximized ? TOPLEVEL_UNSET_MAXIMIZED : TOPLEVEL_SET_MAXIMIZED);
            else if (!n->titleHover) WL_CALL(n->toplevel, TOPLEVEL_MOVE, n->seat, serial);
        } else if (state == 1 && b == 2) {
            WL_CALL(n->toplevel, TOPLEVEL_SHOW_WINDOW_MENU, n->seat, serial, (int32_t)n->px, (int32_t)(n->py - TITLE_H));
        } else if (state == 0 && b == 0 && n->titleHover) {
            if (n->titleHover == 1) n->closeRequested = true;
            else if (n->titleHover == 2) WL_CALL0(n->toplevel, n->maximized ? TOPLEVEL_UNSET_MAXIMIZED : TOPLEVEL_SET_MAXIMIZED);
            else WL_CALL0(n->toplevel, TOPLEVEL_SET_MINIMIZED);
        }
        return;
    }
    if (state == 1) {
        const uint32_t edge = n->buttons ? 0 : edgeAt(n, n->px, n->py);
        if (edge && b == 0) { WL_CALL(n->toplevel, TOPLEVEL_RESIZE, n->seat, serial, edge); return; }
        n->buttons |= bit;
        cscdViewMouseDown(n->w->view, n->px, n->py, b, n->mods, clickPress(&n->clicks, time, (int)(n->px * n->scale), (int)(n->py * n->scale), 400));
    } else {
        n->buttons &= ~bit;
        cscdViewMouseUp(n->w->view, n->px, n->py, b, n->mods);
    }
}
static void ptrAxis(void* d, void* p, uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)p; (void)time;
    CscdWindowNative* n = (CscdWindowNative*)d;
    /* (a notch is 10 units here; 100 CSS px on the other systems) */
    if (axis == 0) n->wheelY += fixedToFloat(value) * 10;
    else n->wheelX += fixedToFloat(value) * 10;
    if (!n->pointer || W.proxyGetVersion((struct wl_proxy*)n->pointer) < 5) { /* (no frame events: at once) */
        if (n->pointerSurface == n->surface) cscdViewWheel(n->w->view, n->px, n->py, n->wheelX, n->wheelY, n->mods);
        n->wheelX = n->wheelY = 0;
    }
}
static void ptrFrame(void* d, void* p) {
    (void)p;
    CscdWindowNative* n = (CscdWindowNative*)d;
    if ((n->wheelX != 0 || n->wheelY != 0) && n->pointerSurface == n->surface) cscdViewWheel(n->w->view, n->px, n->py, n->wheelX, n->wheelY, n->mods);
    n->wheelX = n->wheelY = 0;
}
static void ptrAxisSource(void* d, void* p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptrAxisStop(void* d, void* p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptrAxisDiscrete(void* d, void* p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct {
    void (*enter)(void*, void*, uint32_t, void*, wl_fixed_t, wl_fixed_t);
    void (*leave)(void*, void*, uint32_t, void*);
    void (*motion)(void*, void*, uint32_t, wl_fixed_t, wl_fixed_t);
    void (*button)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*axis)(void*, void*, uint32_t, uint32_t, wl_fixed_t);
    void (*frame)(void*, void*);
    void (*axisSource)(void*, void*, uint32_t);
    void (*axisStop)(void*, void*, uint32_t, uint32_t);
    void (*axisDiscrete)(void*, void*, uint32_t, int32_t);
    void (*axisValue120)(void*, void*, uint32_t, int32_t);
    void (*axisRelativeDirection)(void*, void*, uint32_t, uint32_t);
} pointerListener = {ptrEnter, ptrLeave, ptrMotion, ptrButton, ptrAxis, ptrFrame, ptrAxisSource, ptrAxisStop, ptrAxisDiscrete, ptrAxisDiscrete, ptrAxisStop};

/* ---- the seat ---- */

static void seatCapabilities(void* d, void* seat, uint32_t caps) {
    CscdWindowNative* n = (CscdWindowNative*)d;
    if ((caps & 1) && !n->pointer) {
        n->pointer = WL_NEW(seat, SEAT_GET_POINTER, W.pointer, NULL);
        WL_LISTEN(n->pointer, &pointerListener, n);
    }
    if ((caps & 2) && !n->keyboard) {
        n->keyboard = WL_NEW(seat, SEAT_GET_KEYBOARD, W.keyboard, NULL);
        WL_LISTEN(n->keyboard, &keyboardListener, n);
    }
}
static void seatName(void* d, void* seat, const char* name) { (void)d; (void)seat; (void)name; }
static const struct { void (*capabilities)(void*, void*, uint32_t); void (*name)(void*, void*, const char*); } seatListener = {seatCapabilities, seatName};

/* ---- the data device: the clipboard and drags in ---- */

static void offerOffer(void* d, void* offer, const char* mime) {
    CscdWindowNative* n = (CscdWindowNative*)d;
    for (size_t i = 0; i < n->offers.len; ++i)
        if (n->offers.items[i] == offer) cscdVecPush(n->offerTypes.items[i], cscdStrDupC(mime));
}
static void offerSourceActions(void* d, void* o, uint32_t a) { (void)d; (void)o; (void)a; }
static void offerAction(void* d, void* o, uint32_t a) { (void)d; (void)o; (void)a; }
static const struct {
    void (*offer)(void*, void*, const char*);
    void (*sourceActions)(void*, void*, uint32_t);
    void (*action)(void*, void*, uint32_t);
} offerListener = {offerOffer, offerSourceActions, offerAction};

/* An offer done with: its types handed out (to the selection or the drag), or dropped. */
static void takeOffer(CscdWindowNative* n, void* offer, CscdStrVec* into) {
    for (size_t i = 0; i < n->offers.len; ++i) {
        if (n->offers.items[i] != offer) continue;
        CscdStrVec* types = n->offerTypes.items[i];
        if (into) {
            cscdStrVecFree(into);
            *into = *types;
        } else cscdStrVecFree(types);
        cscdFree(types);
        cscdVecRemove(&n->offers, i);
        cscdVecRemove(&n->offerTypes, i);
        return;
    }
}

static bool hasType(const CscdStrVec* types, const char* t) {
    for (size_t i = 0; i < types->len; ++i)
        if (cscdStrEqC(&types->items[i], t)) return true;
    return false;
}

/* What an offer holds as mime, read through a pipe (another client writes it). */
static bool receive(CscdWindowNative* n, void* offer, const char* mime, CscdStr* out) {
    int fds[2];
    if (pipe(fds) < 0) return false;
    WL_CALL(offer, DATA_OFFER_RECEIVE, mime, fds[1]);
    close(fds[1]);
    W.displayFlush(n->dpy);
    const double give = nowSeconds() + 2;
    char buf[4096];
    for (;;) {
        struct pollfd pfd = {fds[0], POLLIN, 0};
        const int left = (int)((give - nowSeconds()) * 1000);
        if (left <= 0 || poll(&pfd, 1, left) <= 0) break;
        const ssize_t got = read(fds[0], buf, sizeof buf);
        if (got <= 0) break;
        cscdStrAppend(out, cscdSvMake(buf, (size_t)got));
    }
    close(fds[0]);
    return true;
}

static void devDataOffer(void* d, void* dev, void* offer) {
    (void)dev;
    CscdWindowNative* n = (CscdWindowNative*)d;
    CscdStrVec* types = CSCD_NEW(CscdStrVec);
    cscdVecPush(&n->offers, offer);
    cscdVecPush(&n->offerTypes, types);
    WL_LISTEN(offer, &offerListener, n);
}
static void devEnter(void* d, void* dev, uint32_t serial, void* surface, wl_fixed_t x, wl_fixed_t y, void* offer) {
    (void)dev;
    CscdWindowNative* n = (CscdWindowNative*)d;
    if (n->dragOffer && n->dragOffer != offer) WL_DESTROY(n->dragOffer, DATA_OFFER_DESTROY);
    n->dragOffer = offer;
    n->dragSerial = serial;
    takeOffer(n, offer, &n->dragTypes);
    n->dragEntered = false;
    if (!offer || surface != n->surface) return;
    n->dragX = fixedToFloat(x);
    n->dragY = fixedToFloat(y);
    CscdDataTransfer* dt = cscdDataTransferNew();
    cscdStrSetC(&dt->effectAllowed, "copy");
    CscdStr s = {0};
    if (hasType(&n->dragTypes, "text/uri-list") && receive(n, offer, "text/uri-list", &s)) {
        /* (file:// lines, as on X11) */
        size_t start = 0;
        while (start < s.len) {
            size_t end = cscdSvFindChar(cscdStrSv(&s), '\n', start);
            if (end == CSCD_NPOS) end = s.len;
            CscdSv line = cscdSvSub(cscdStrSv(&s), start, end - start);
            start = end + 1;
            while (line.len && (line.ptr[line.len - 1] == '\r' || !line.ptr[line.len - 1])) --line.len;
            if (!cscdSvStartsC(line, "file://")) continue;
            const size_t slash = cscdSvFindChar(line, '/', 7);
            if (slash == CSCD_NPOS) continue;
            CscdStr path = {0};
            for (size_t i = slash; i < line.len; ++i) {
                if (line.ptr[i] == '%' && i + 2 < line.len) {
                    char hex[3] = {line.ptr[i + 1], line.ptr[i + 2], 0};
                    cscdStrAppendChar(&path, (char)strtol(hex, NULL, 16));
                    i += 2;
                } else cscdStrAppendChar(&path, line.ptr[i]);
            }
            cscdVecPush(&dt->files, path);
        }
    }
    cscdStrClear(&s);
    const char* textTypes[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"};
    for (size_t i = 0; i < CSCD_COUNT(textTypes); ++i)
        if (hasType(&n->dragTypes, textTypes[i])) {
            if (receive(n, offer, textTypes[i], &s)) cscdDataTransferSetData(dt, CSCD_SV("text/plain"), cscdStrSv(&s));
            break;
        }
    cscdStrFree(&s);
    n->dragEntered = true;
    cscdViewExternalDragEnter(n->w->view, dt, n->dragX, n->dragY);
    cscdRelease(dt);
    const bool accept = cscdViewExternalDragOver(n->w->view, n->dragX, n->dragY);
    WL_CALL(offer, DATA_OFFER_ACCEPT, serial, accept && n->dragTypes.len ? cscdStrC(&n->dragTypes.items[0]) : NULL);
    if (W.proxyGetVersion((struct wl_proxy*)offer) >= 3) WL_CALL(offer, DATA_OFFER_SET_ACTIONS, 1u, 1u); /* (copy) */
    n->w->dirty = true;
}
static void devLeave(void* d, void* dev) {
    (void)dev;
    CscdWindowNative* n = (CscdWindowNative*)d;
    if (n->dragEntered) cscdViewExternalDragLeave(n->w->view);
    n->dragEntered = false;
    WL_DESTROY(n->dragOffer, DATA_OFFER_DESTROY);
}
static void devMotion(void* d, void* dev, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)dev; (void)time;
    CscdWindowNative* n = (CscdWindowNative*)d;
    if (!n->dragEntered || !n->dragOffer) return;
    n->dragX = fixedToFloat(x);
    n->dragY = fixedToFloat(y);
    const bool accept = cscdViewExternalDragOver(n->w->view, n->dragX, n->dragY);
    WL_CALL(n->dragOffer, DATA_OFFER_ACCEPT, n->dragSerial, accept && n->dragTypes.len ? cscdStrC(&n->dragTypes.items[0]) : NULL);
    n->w->dirty = true;
}
static void devDrop(void* d, void* dev) {
    (void)dev;
    CscdWindowNative* n = (CscdWindowNative*)d;
    if (n->dragEntered) cscdViewExternalDrop(n->w->view, n->dragX, n->dragY);
    n->dragEntered = false;
    if (n->dragOffer && W.proxyGetVersion((struct wl_proxy*)n->dragOffer) >= 3) WL_CALL0(n->dragOffer, DATA_OFFER_FINISH);
    WL_DESTROY(n->dragOffer, DATA_OFFER_DESTROY);
}
static void devSelection(void* d, void* dev, void* offer) {
    (void)dev;
    CscdWindowNative* n = (CscdWindowNative*)d;
    if (n->selectionOffer && n->selectionOffer != offer) WL_DESTROY(n->selectionOffer, DATA_OFFER_DESTROY);
    n->selectionOffer = offer;
    if (offer) takeOffer(n, offer, &n->selectionTypes);
    else cscdStrVecFree(&n->selectionTypes);
}
static const struct {
    void (*dataOffer)(void*, void*, void*);
    void (*enter)(void*, void*, uint32_t, void*, wl_fixed_t, wl_fixed_t, void*);
    void (*leave)(void*, void*);
    void (*motion)(void*, void*, uint32_t, wl_fixed_t, wl_fixed_t);
    void (*drop)(void*, void*);
    void (*selection)(void*, void*, void*);
} dataDeviceListener = {devDataOffer, devEnter, devLeave, devMotion, devDrop, devSelection};

/* our selection: its text written to whoever asks */
static void sourceTarget(void* d, void* s, const char* mime) { (void)d; (void)s; (void)mime; }
static void sourceSend(void* d, void* s, const char* mime, int32_t fd) {
    (void)s; (void)mime;
    CscdWindowNative* n = (CscdWindowNative*)d;
    for (size_t at = 0; at < n->clipText.len;) {
        const ssize_t put = write(fd, n->clipText.ptr + at, n->clipText.len - at);
        if (put <= 0) break;
        at += (size_t)put;
    }
    close(fd);
}
static void sourceCancelled(void* d, void* s) {
    CscdWindowNative* n = (CscdWindowNative*)d;
    if (n->ownSource == s) n->ownSource = NULL;
    WL_DESTROY(s, DATA_SOURCE_DESTROY);
}
static void sourceDummy(void* d, void* s) { (void)d; (void)s; }
static void sourceAction(void* d, void* s, uint32_t a) { (void)d; (void)s; (void)a; }
static const struct {
    void (*target)(void*, void*, const char*);
    void (*send)(void*, void*, const char*, int32_t);
    void (*cancelled)(void*, void*);
    void (*dropPerformed)(void*, void*);
    void (*finished)(void*, void*);
    void (*action)(void*, void*, uint32_t);
} sourceListener = {sourceTarget, sourceSend, sourceCancelled, sourceDummy, sourceDummy, sourceAction};

static void clipSetText(void* user, CscdSv text) {
    CscdWindowNative* n = ((CscdWindow*)user)->native;
    if (!n->dataDevice) return;
    cscdStrSet(&n->clipText, text);
    void* source = WL_NEW(n->dataDeviceManager, DDM_CREATE_DATA_SOURCE, W.dataSource, NULL);
    WL_LISTEN(source, &sourceListener, n);
    WL_CALL(source, DATA_SOURCE_OFFER, "text/plain;charset=utf-8");
    WL_CALL(source, DATA_SOURCE_OFFER, "UTF8_STRING");
    WL_CALL(source, DATA_SOURCE_OFFER, "text/plain");
    WL_CALL(n->dataDevice, DATA_DEVICE_SET_SELECTION, source, n->inputSerial);
    n->ownSource = source;
    W.displayFlush(n->dpy);
}

static void clipGetText(void* user, CscdStr* out) {
    CscdWindowNative* n = ((CscdWindow*)user)->native;
    if (n->ownSource) { cscdStrAppend(out, cscdStrSv(&n->clipText)); return; }
    if (!n->selectionOffer) return;
    const char* types[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"};
    for (size_t i = 0; i < CSCD_COUNT(types); ++i)
        if (hasType(&n->selectionTypes, types[i])) {
            CscdStr s = {0};
            receive(n, n->selectionOffer, types[i], &s);
            for (size_t k = 0; k < s.len; ++k) if (s.ptr[k] != '\r') cscdStrAppendChar(out, s.ptr[k]);
            cscdStrFree(&s);
            return;
        }
}

static void clipSetImage(void* user, const CscdImage* img) { (void)user; (void)img; } /* (images: text only on Wayland so far) */

static void clipGetFiles(void* user, CscdStrVec* out) {
    CscdWindowNative* n = ((CscdWindow*)user)->native;
    if (n->ownSource || !n->selectionOffer || !hasType(&n->selectionTypes, "text/uri-list")) return;
    CscdStr s = {0};
    receive(n, n->selectionOffer, "text/uri-list", &s);
    size_t start = 0;
    while (start < s.len) {
        size_t end = cscdSvFindChar(cscdStrSv(&s), '\n', start);
        if (end == CSCD_NPOS) end = s.len;
        CscdSv line = cscdSvSub(cscdStrSv(&s), start, end - start);
        start = end + 1;
        while (line.len && line.ptr[line.len - 1] == '\r') --line.len;
        if (cscdSvStartsC(line, "file://") && cscdSvFindChar(line, '/', 7) != CSCD_NPOS) cscdVecPush(out, cscdStrDup(cscdSvFrom(line, cscdSvFindChar(line, '/', 7))));
    }
    cscdStrFree(&s);
}

/* ---- the registry ---- */

static void registryGlobal(void* d, void* registry, uint32_t name, const char* iface, uint32_t version) {
    CscdWindowNative* n = (CscdWindowNative*)d;
#define BIND(iface_, v) W.marshalFlags ? (void*)W.marshalFlags((struct wl_proxy*)registry, REGISTRY_BIND, (iface_), (v), 0, name, (iface_)->name, (v), NULL) \
                                       : (void*)W.marshalConstructorVersioned((struct wl_proxy*)registry, REGISTRY_BIND, (iface_), (v), name, (iface_)->name, (v), NULL)
    if (!strcmp(iface, "wl_compositor")) n->compositor = BIND(W.compositor, cscdMini((int)version, 4));
    else if (!strcmp(iface, "wl_subcompositor") && W.subcompositor) n->subcompositor = BIND(W.subcompositor, 1);
    else if (!strcmp(iface, "wl_shm")) n->shm = BIND(W.shm, 1);
    else if (!strcmp(iface, "xdg_wm_base")) {
        n->wmBase = BIND(&xdg_wm_base_interface, cscdMini((int)version, 2));
        WL_LISTEN(n->wmBase, &wmListener, n);
    } else if (!strcmp(iface, "zxdg_decoration_manager_v1")) n->decorationManager = BIND(&zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(iface, "wl_seat") && !n->seat) {
        n->seat = BIND(W.seat, cscdMini((int)version, 5));
        WL_LISTEN(n->seat, &seatListener, n);
    } else if (!strcmp(iface, "wl_data_device_manager") && W.dataDeviceManager) n->dataDeviceManager = BIND(W.dataDeviceManager, cscdMini((int)version, 3));
    else if (!strcmp(iface, "wl_output") && n->outputCount < MAX_OUTPUTS) {
        Output* o = &n->outputs[n->outputCount++];
        o->name = name;
        o->scale = 1;
        o->proxy = BIND(W.output, cscdMini((int)version, 2));
        WL_LISTEN(o->proxy, &outputListener, n);
    }
#undef BIND
}
static void registryRemove(void* d, void* registry, uint32_t name) {
    (void)registry;
    CscdWindowNative* n = (CscdWindowNative*)d;
    for (int i = 0; i < n->outputCount; ++i)
        if (n->outputs[i].name == name) {
            for (int k = 0; k < n->enteredCount; ++k)
                if (n->entered[k] == n->outputs[i].proxy) n->entered[k] = n->entered[--n->enteredCount];
            W.proxyDestroy((struct wl_proxy*)n->outputs[i].proxy);
            n->outputs[i] = n->outputs[--n->outputCount];
            updateScale(n);
            break;
        }
}
static const struct { void (*global)(void*, void*, uint32_t, const char*, uint32_t); void (*remove)(void*, void*, uint32_t); } registryListener = {
    registryGlobal, registryRemove};

/* ---- the platform's calls ---- */

bool wlAvailable(void) {
    const char* display = getenv("WAYLAND_DISPLAY");
    if (!display || !display[0]) return false;
    if (!loadWayland()) return false;
    struct wl_display* d = W.displayConnect(NULL);
    if (!d) return false;
    W.displayDisconnect(d);
    return true;
}

void wlWindowDestroyNative(CscdWindow* w) {
    CscdWindowNative* n = w->native;
    if (!n) return;
    for (int i = 0; i < 16; ++i) if (windows[i] == n) windows[i] = NULL;
    if (n->dpy) {
        shmFree(&n->titleBuffer);
        WL_DESTROY(n->titleSubsurface, SUBSURFACE_DESTROY);
        WL_DESTROY(n->titleSurface, SURFACE_DESTROY);
        WL_DESTROY(n->decoration, DECORATION_DESTROY);
        WL_DESTROY(n->toplevel, TOPLEVEL_DESTROY);
        WL_DESTROY(n->xdgSurface, XDG_SURFACE_DESTROY);
        WL_DESTROY(n->cursorSurface, SURFACE_DESTROY);
        WL_DESTROY(n->surface, SURFACE_DESTROY);
        if (n->cursorTheme && W.cursorThemeDestroy) W.cursorThemeDestroy(n->cursorTheme);
        if (n->xkbState) W.xkbStateUnref(n->xkbState);
        if (n->xkbKeymap) W.xkbKeymapUnref(n->xkbKeymap);
        if (n->xkbContext) W.xkbContextUnref(n->xkbContext);
        W.displayFlush(n->dpy);
        W.displayDisconnect(n->dpy);
    }
    for (int i = 0; i < 2; ++i) if (n->wake[i] >= 0) close(n->wake[i]);
    cscdStrVecFree(&n->selectionTypes);
    cscdStrVecFree(&n->dragTypes);
    for (size_t i = 0; i < n->offerTypes.len; ++i) { cscdStrVecFree(n->offerTypes.items[i]); cscdFree(n->offerTypes.items[i]); }
    cscdVecFree(&n->offers);
    cscdVecFree(&n->offerTypes);
    cscdStrFree(&n->clipText);
    cscdFree(n);
    w->native = NULL;
}

void wlWindowCreateNative(CscdWindow* w, const CscdWindowOptions* opts) {
    CscdWindowNative* n = CSCD_NEW(CscdWindowNative);
    w->native = n;
    n->w = w;
    n->wake[0] = n->wake[1] = -1;
    n->scale = 1;
    n->frameCursor = -1;
    n->shown = CSCD_CURSOR_DEFAULT;
    if (!loadWayland() || !(n->dpy = W.displayConnect(NULL))) {
        fprintf(stderr, "cascade: cannot connect to the Wayland display\n");
        return;
    }
    fillXdgTypes();
    w->display = n->dpy;
    n->registry = WL_NEW(n->dpy, DISPLAY_GET_REGISTRY, W.registry, NULL);
    WL_LISTEN(n->registry, &registryListener, n);
    W.displayRoundtrip(n->dpy);
    if (!n->compositor || !n->shm || !n->wmBase) {
        fprintf(stderr, "cascade: the Wayland compositor lacks wl_compositor, wl_shm or xdg_wm_base\n");
        return;
    }
    W.displayRoundtrip(n->dpy); /* (the outputs' scales, the seat's capabilities) */
    for (int i = 0; i < n->outputCount; ++i) n->scale = cscdMaxi(n->scale, n->outputs[i].scale); /* (a guess until the window is on one) */
    w->dpr = (float)n->scale;
    if (n->seat && n->dataDeviceManager) {
        n->dataDevice = WL_NEW(n->dataDeviceManager, DDM_GET_DATA_DEVICE, W.dataDevice, NULL, n->seat);
        WL_LISTEN(n->dataDevice, &dataDeviceListener, n);
    }
    n->surface = WL_NEW(n->compositor, COMPOSITOR_CREATE_SURFACE, W.surface, NULL);
    WL_LISTEN(n->surface, &surfaceListener, n);
    WL_CALL(n->surface, SURFACE_SET_BUFFER_SCALE, n->scale);
    n->xdgSurface = WL_NEW(n->wmBase, WM_BASE_GET_XDG_SURFACE, &xdg_surface_interface, NULL, n->surface);
    WL_LISTEN(n->xdgSurface, &xdgSurfaceListener, n);
    n->toplevel = WL_NEW(n->xdgSurface, XDG_SURFACE_GET_TOPLEVEL, &xdg_toplevel_interface, NULL);
    WL_LISTEN(n->toplevel, &toplevelListener, n);
    snprintf(n->title, sizeof n->title, "%s", opts->title && opts->title[0] ? opts->title : "cascade");
    WL_CALL(n->toplevel, TOPLEVEL_SET_TITLE, n->title);
    char app[256] = "";
    FILE* f = fopen("/proc/self/comm", "r");
    if (f) {
        if (fgets(app, sizeof app, f)) app[strcspn(app, "\n")] = 0;
        fclose(f);
    }
    WL_CALL(n->toplevel, TOPLEVEL_SET_APP_ID, app[0] ? app : "cascade");
    if (opts->resizable) WL_CALL(n->toplevel, TOPLEVEL_SET_MIN_SIZE, (int32_t)opts->minWidth, (int32_t)opts->minHeight);
    else {
        WL_CALL(n->toplevel, TOPLEVEL_SET_MIN_SIZE, (int32_t)opts->width, (int32_t)opts->height);
        WL_CALL(n->toplevel, TOPLEVEL_SET_MAX_SIZE, (int32_t)opts->width, (int32_t)opts->height);
    }
    /* the frame: the compositor's when it draws them, else our own title bar on a subsurface */
    if (n->decorationManager) {
        n->decoration = WL_NEW(n->decorationManager, DECORATION_MANAGER_GET, &zxdg_toplevel_decoration_v1_interface, NULL, n->toplevel);
        WL_LISTEN(n->decoration, &decorationListener, n);
        WL_CALL(n->decoration, DECORATION_SET_MODE, 2u);
        n->serverDecorations = true;
    }
    if (n->subcompositor) {
        n->titleSurface = WL_NEW(n->compositor, COMPOSITOR_CREATE_SURFACE, W.surface, NULL);
        n->titleSubsurface = WL_NEW(n->subcompositor, SUBCOMPOSITOR_GET_SUBSURFACE, W.subsurface, NULL, n->titleSurface, n->surface);
        WL_CALL0(n->titleSubsurface, SUBSURFACE_SET_DESYNC);
    } else n->serverDecorations = true; /* (nothing to draw a frame on: none) */
    w->clientW = cscdMaxi(1, (int)(opts->width * w->dpr));
    w->clientH = cscdMaxi(1, (int)(opts->height * w->dpr));
    w->hwnd = n->surface;
    n->handle.mark = CSCD_WAYLAND_MARK;
    n->handle.display = n->dpy;
    n->handle.surface = n->surface;
    for (int i = 0; i < 16; ++i) if (!windows[i]) { windows[i] = n; break; }
    if (pipe(n->wake) == 0) {
        for (int i = 0; i < 2; ++i) {
            fcntl(n->wake[i], F_SETFL, fcntl(n->wake[i], F_GETFL) | O_NONBLOCK);
            fcntl(n->wake[i], F_SETFD, FD_CLOEXEC);
        }
    } else n->wake[0] = n->wake[1] = -1;
    w->presenter = windowPresenterNew(w, &n->handle, opts->backend, w->clientW, w->clientH);
    cscdViewSetViewport(w->view, w->clientW / w->dpr, w->clientH / w->dpr, w->dpr);
    cscdViewSetClipboardHooks(w->view, clipSetText, clipGetText, clipSetImage, clipGetFiles, w);
}

void* wlWindowSurface(const CscdWindow* w) { return &w->native->handle; }

void wlWindowShowTitle(CscdWindow* w, const char* title) {
    CscdWindowNative* n = w->native;
    if (!n || !n->toplevel) return;
    snprintf(n->title, sizeof n->title, "%s", title);
    WL_CALL(n->toplevel, TOPLEVEL_SET_TITLE, n->title);
    n->titleDirty = true;
    wakeUp(n);
}

void wlWindowWake(CscdWindow* w) { if (w->native) wakeUp(w->native); }

void wlWindowShow(CscdWindow* w) {
    CscdWindowNative* n = w->native;
    if (!w->hwnd || w->shown || !n) return;
    w->shown = true;
    /* the first commit has no buffer: the compositor answers with the size to draw at */
    WL_CALL0(n->surface, SURFACE_COMMIT);
    while (!n->configured && W.displayRoundtrip(n->dpy) >= 0) {}
    n->titleDirty = true;
    w->dirty = true;
}

void wlWindowPlaceInputMethod(CscdWindow* w) { (void)w; } /* (text-input-v3 is not spoken: compositors type for us through the keymap) */

void wlWindowResize(CscdWindow* w, int width, int height) {
    CscdWindowNative* n = w->native;
    if (!w->hwnd || !n) return;
    applySize(n, width, height);
    setGeometry(n);
}

void wlWindowMaximize(CscdWindow* w) {
    CscdWindowNative* n = w->native;
    if (!w->hwnd || !n || w->fullscreen) return;
    wlWindowShow(w);
    WL_CALL0(n->toplevel, TOPLEVEL_SET_MAXIMIZED);
}

void wlWindowSetFullscreen(CscdWindow* w, bool on) {
    CscdWindowNative* n = w->native;
    if (!w->hwnd || !n || on == w->fullscreen) return;
    if (on) WL_CALL(n->toplevel, TOPLEVEL_SET_FULLSCREEN, NULL);
    else WL_CALL0(n->toplevel, TOPLEVEL_UNSET_FULLSCREEN);
    n->titleDirty = true;
}

void wlWindowClose(CscdWindow* w) {
    if (!w->hwnd || !w->native) return;
    w->native->closeRequested = true;
    wakeUp(w->native);
}

int wlWindowFd(const CscdWindow* w) { return w->native && w->native->dpy ? W.displayGetFd(w->native->dpy) : -1; }

bool wlWindowPump(CscdWindow* w, bool wait) {
    CscdWindowNative* n = w->native;
    if (!w->hwnd || !n || !n->dpy) return false;
    CscdView* view = w->view;
    /* the events: read when they are there, waited for when nothing else is to do */
    while (W.displayPrepareRead(n->dpy) != 0) W.displayDispatchPending(n->dpy);
    W.displayFlush(n->dpy);
    int ms = 0;
    if (wait && !w->dirty && !n->closeRequested && !cscdViewNeedsPaint(view) && !windowHasPosted(w)) {
        ms = cscdViewNextTimerMs(view);
        if (w->onFrame) ms = 0;
        if (n->repeatKey) {
            const int r = (int)((n->repeatAt - nowSeconds()) * 1000) + 1;
            ms = ms < 0 ? cscdMaxi(0, r) : cscdMini(ms, cscdMaxi(0, r));
        }
    }
    struct pollfd fds[3] = {{W.displayGetFd(n->dpy), POLLIN, 0}, {n->wake[0], POLLIN, 0}, {platformA11yFd(w), POLLIN, 0}};
    const int nfds = fds[2].fd >= 0 ? 3 : 2;
    if (poll(fds, (nfds_t)nfds, ms < 0 ? -1 : ms) > 0 && (fds[0].revents & POLLIN)) W.displayReadEvents(n->dpy);
    else W.displayCancelRead(n->dpy);
    W.displayDispatchPending(n->dpy);
    if (n->wake[0] >= 0) {
        char buf[64];
        while (read(n->wake[0], buf, sizeof buf) > 0) {}
    }
    if (fds[2].fd >= 0 && (fds[2].revents & POLLIN)) platformA11yRead(w);
    /* key repeat */
    if (n->repeatKey && n->repeatRate > 0) {
        const double t = nowSeconds();
        for (int guard = 0; n->repeatKey && n->repeatAt <= t && guard < 8; ++guard) {
            keyDown(n, n->repeatKey - 1);
            n->repeatAt += 1.0 / n->repeatRate;
        }
        if (n->repeatAt < t) n->repeatAt = t + 1.0 / n->repeatRate;
    }
    if (n->closeRequested) {
        n->closeRequested = false;
        if (!w->onClose || w->onClose(w->onCloseUser)) {
            cscdPresenterFree(w->presenter);
            w->presenter = NULL;
            WL_DESTROY(n->titleSubsurface, SUBSURFACE_DESTROY);
            WL_DESTROY(n->titleSurface, SURFACE_DESTROY);
            WL_DESTROY(n->decoration, DECORATION_DESTROY);
            WL_DESTROY(n->toplevel, TOPLEVEL_DESTROY);
            WL_DESTROY(n->xdgSurface, XDG_SURFACE_DESTROY);
            WL_DESTROY(n->surface, SURFACE_DESTROY);
            W.displayFlush(n->dpy);
            w->hwnd = NULL;
        }
    }
    if (!w->hwnd) return false;
    windowRunPosted(w);
    if (!w->hwnd) return false;
    if (n->configured && (cscdViewUpdate(view) || w->onFrame || w->dirty)) windowRender(w);
    drawTitleBar(n);
    /* the cursor the element under the pointer asks for (or the frame's edge) */
    if (n->pointerSurface) {
        const CscdCursor want = n->pointerSurface == n->titleSurface ? CSCD_CURSOR_DEFAULT
                                 : n->frameCursor > 0 ? edgeCursor((uint32_t)n->frameCursor) : cscdViewCursor(view);
        if (!n->cursorShown || want != n->shown) {
            n->shown = want;
            n->cursorShown = true;
            showCursor(n, want);
        }
    }
    W.displayFlush(n->dpy);
    return w->hwnd != NULL;
}
