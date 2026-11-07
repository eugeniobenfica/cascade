/* cascade — GPU presentation. The renderer paints into an image; a presenter puts that image on
 * screen with Vulkan, OpenGL or the system's software path (GDI, XPutImage, Core Animation). Both
 * GPU APIs are loaded at run time (vulkan-1.dll / libvulkan.so.1 / MoltenVK, opengl32.dll /
 * libGL.so.1 / OpenGL.framework): no SDK or import library is needed to build, and the program
 * still starts on machines without them. */
#ifndef CASCADE_GPU_H
#define CASCADE_GPU_H

#include "cascade/image.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CscdBackend { CSCD_BACKEND_AUTO, CSCD_BACKEND_VULKAN, CSCD_BACKEND_OPENGL, CSCD_BACKEND_SOFTWARE } CscdBackend;

const char* cscdBackendName(CscdBackend b);

/* A presenter: an object with a table of functions (each kind of presenter fills one). */
typedef struct CscdPresenter CscdPresenter;
typedef struct CscdPresenterVtbl {
    CscdBackend (*backend)(const CscdPresenter* p);
    /* Shows the frame (premultiplied BGRA, the size of the window's client area). */
    bool (*present)(CscdPresenter* p, const CscdImage* frame);
    void (*resize)(CscdPresenter* p, int width, int height);
    const char* (*description)(const CscdPresenter* p);
    void (*destroy)(CscdPresenter* p);
} CscdPresenterVtbl;
struct CscdPresenter {
    const CscdPresenterVtbl* vt;
    /* Set before present by whoever paints the frames (the window does): partial, and only these
     * rectangles (device pixels) differ from the frame presented before. The GPU presenters then
     * upload just them; false: the whole frame. */
    bool partial;
    const CscdIRect* damage;
    size_t damageCount;
};

CSCD_INLINE CscdBackend cscdPresenterBackend(const CscdPresenter* p) { return p->vt->backend(p); }
CSCD_INLINE bool cscdPresenterPresent(CscdPresenter* p, const CscdImage* frame) { return p->vt->present(p, frame); }
CSCD_INLINE void cscdPresenterResize(CscdPresenter* p, int width, int height) { p->vt->resize(p, width, height); }
CSCD_INLINE const char* cscdPresenterDescription(const CscdPresenter* p) { return p->vt->description(p); }
CSCD_INLINE void cscdPresenterFree(CscdPresenter* p) { if (p) p->vt->destroy(p); }

/* An X11 window for cscdPresenterNew on Linux. */
typedef struct CscdXlibWindow {
    void* display; /* Display* */
    unsigned long window;
} CscdXlibWindow;

/* A Wayland surface for cscdPresenterNew on Linux, in place of a CscdXlibWindow*: its mark tells
 * the two apart. (OpenGL is not offered there: Vulkan, then shared memory.) */
#define CSCD_WAYLAND_MARK 0x57594C44ul /* "WYLD" */
typedef struct CscdWaylandWindow {
    unsigned long mark; /* CSCD_WAYLAND_MARK */
    void* display;      /* struct wl_display* */
    void* surface;      /* struct wl_surface* */
} CscdWaylandWindow;

/* Creates a presenter for a native window: an HWND on Windows, a CscdXlibWindow* or a
 * CscdWaylandWindow* on Linux, an NSView* on macOS, a UIView* on iOS, an ANativeWindow* on Android.
 * Auto tries Vulkan, then OpenGL, then the
 * software path. Never NULL. */
CscdPresenter* cscdPresenterNew(void* nativeWindow, CscdBackend preferred, int width, int height);

/* For applications that already own an OpenGL context (Qt / Skia ports, game engines): uploads a
 * frame into a texture of the current context and draws it. */
bool cscdGlLoadFunctions(void); /* after a context is current */
unsigned cscdGlCreateTexture(void);
void cscdGlUpload(unsigned texture, const CscdImage* frame);
void cscdGlDrawFullscreen(unsigned texture, int viewportWidth, int viewportHeight); /* fixed-function quad */

#ifdef __cplusplus
}
#endif

#endif
