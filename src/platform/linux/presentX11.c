/* X11 presenters: OpenGL through GLX (libGL.so.1, opened at run time) and the software path through
 * XPutImage. Only OpenGL 1.1 is used (gl_backend.c). */
#include "platform/common/platformInternal.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <string.h>

#ifndef CASCADE_NO_OPENGL /* (GLX: CASCADE_OPENGL) */
typedef struct __GLXcontextRec* GLXContext;
enum { GLX_USE_GL = 1, GLX_RGBA = 4, GLX_DOUBLEBUFFER = 5, GLX_RED_SIZE = 8, GLX_GREEN_SIZE = 9, GLX_BLUE_SIZE = 10 };

static struct {
    XVisualInfo* (*glXChooseVisual)(Display*, int, int*);
    int (*glXGetConfig)(Display*, XVisualInfo*, int, int*);
    GLXContext (*glXCreateContext)(Display*, XVisualInfo*, GLXContext, int);
    void (*glXDestroyContext)(Display*, GLXContext);
    int (*glXMakeCurrent)(Display*, XID, GLXContext);
    void (*glXSwapBuffers)(Display*, XID);
    void* (*glXGetProcAddressARB)(const unsigned char*);
    const unsigned char* (*glGetString)(unsigned);
} X;

static bool glxLoad(void) {
    if (X.glXCreateContext) return true;
    if (!cscdGlLoadFunctions()) return false;
    void* lib = glLibrary();
#define L(name) *(void**)&X.name = platformSymbol(lib, #name); if (!X.name) return false;
    L(glXChooseVisual) L(glXGetConfig) L(glXDestroyContext) L(glXMakeCurrent) L(glXSwapBuffers) L(glXGetProcAddressARB) L(glGetString)
    L(glXCreateContext)
#undef L
    return true;
}

/* The window's own visual, if OpenGL can draw into it double-buffered. */
static XVisualInfo* glVisualOf(Display* dpy, Window win) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, win, &wa)) return NULL;
    XVisualInfo tmpl;
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.visualid = XVisualIDFromVisual(wa.visual);
    int n = 0;
    XVisualInfo* vi = XGetVisualInfo(dpy, VisualIDMask, &tmpl, &n);
    if (!vi) return NULL;
    int gl = 0, db = 0;
    if (X.glXGetConfig(dpy, vi, GLX_USE_GL, &gl) != 0 || !gl || X.glXGetConfig(dpy, vi, GLX_DOUBLEBUFFER, &db) != 0 || !db) {
        XFree(vi);
        return NULL;
    }
    return vi;
}

/* ---- OpenGL ----------------------------------------------------------------------------------------- */

typedef struct GLPresenter {
    CscdPresenter base;
    Display* dpy;
    Window win;
    GLXContext ctx;
    unsigned tex;
    int texW, texH;
    int width, height;
    char description[160];
} GLPresenter;

static CscdBackend glBackend(const CscdPresenter* p) { (void)p; return CSCD_BACKEND_OPENGL; }
static const char* glDescription(const CscdPresenter* p) { return ((const GLPresenter*)p)->description; }
static void glResize(CscdPresenter* p, int w, int h) { ((GLPresenter*)p)->width = w; ((GLPresenter*)p)->height = h; }

static bool glPresent(CscdPresenter* bp, const CscdImage* frame) {
    GLPresenter* p = (GLPresenter*)bp;
    if (!X.glXMakeCurrent(p->dpy, p->win, p->ctx)) return false;
    glPresentFrame(&p->tex, &p->texW, &p->texH, frame, p->width, p->height, bp);
    X.glXSwapBuffers(p->dpy, p->win);
    return true;
}

static void glDestroy(CscdPresenter* bp) {
    GLPresenter* p = (GLPresenter*)bp;
    if (p->ctx) {
        X.glXMakeCurrent(p->dpy, p->win, p->ctx);
        if (p->tex) glDeleteTexture(p->tex);
        X.glXMakeCurrent(p->dpy, 0, NULL);
        X.glXDestroyContext(p->dpy, p->ctx);
    }
    cscdFree(p);
}

static const CscdPresenterVtbl glVtbl = {glBackend, glPresent, glResize, glDescription, glDestroy};

static bool glInit(GLPresenter* p, const CscdXlibWindow* w, int wd, int ht) {
    p->dpy = (Display*)w->display;
    p->win = w->window;
    p->width = wd;
    p->height = ht;
    if (!glxLoad()) return false;
    XVisualInfo* vi = glVisualOf(p->dpy, p->win);
    if (!vi) return false;
    p->ctx = X.glXCreateContext(p->dpy, vi, NULL, 1);
    XFree(vi);
    if (!p->ctx || !X.glXMakeCurrent(p->dpy, p->win, p->ctx)) return false;
    /* vertical sync, through whichever extension the driver has */
    typedef void (*SwapEXT)(Display*, XID, int);
    typedef int (*SwapMESA)(unsigned);
    SwapEXT ext;
    SwapMESA mesa;
    *(void**)&ext = X.glXGetProcAddressARB((const unsigned char*)"glXSwapIntervalEXT");
    *(void**)&mesa = X.glXGetProcAddressARB((const unsigned char*)"glXSwapIntervalMESA");
    if (ext) ext(p->dpy, p->win, 1);
    else if (mesa) mesa(1);
    const unsigned char* r = X.glGetString(0x1F01);
    snprintf(p->description, sizeof p->description, "OpenGL (%s)", r ? (const char*)r : "");
    p->tex = cscdGlCreateTexture();
    return p->tex != 0;
}
#endif

/* ---- XPutImage -------------------------------------------------------------------------------------- */

typedef struct SoftwarePresenter {
    CscdPresenter base;
    Display* dpy;
    Window win;
    Visual* visual;
    int depth;
    GC gc;
} SoftwarePresenter;

static CscdBackend swBackend(const CscdPresenter* p) { (void)p; return CSCD_BACKEND_SOFTWARE; }
static const char* swDescription(const CscdPresenter* p) { (void)p; return "Software (XPutImage)"; }
static void swResize(CscdPresenter* p, int w, int h) { (void)p; (void)w; (void)h; }

static void swDestroy(CscdPresenter* bp) {
    SoftwarePresenter* p = (SoftwarePresenter*)bp;
    if (p->gc) XFreeGC(p->dpy, p->gc);
    cscdFree(p);
}

static bool swPresent(CscdPresenter* bp, const CscdImage* frame) {
    SoftwarePresenter* p = (SoftwarePresenter*)bp;
    if (frame->width <= 0 || frame->height <= 0) return true;
    /* a 32-bit ZPixmap is BGRX in memory on little-endian, like the frame */
    XImage* img = XCreateImage(p->dpy, p->visual, (unsigned)p->depth, ZPixmap, 0, (char*)frame->pixels, (unsigned)frame->width, (unsigned)frame->height, 32,
                               frame->width * 4);
    if (!img) return false;
    XPutImage(p->dpy, p->win, p->gc, img, 0, 0, 0, 0, (unsigned)frame->width, (unsigned)frame->height);
    img->data = NULL; /* the frame's pixels: not for XDestroyImage to free */
    XDestroyImage(img);
    XFlush(p->dpy);
    return true;
}

static const CscdPresenterVtbl swVtbl = {swBackend, swPresent, swResize, swDescription, swDestroy};

#ifndef CASCADE_NO_OPENGL /* (GLX: CASCADE_OPENGL) */
void* platformChooseGLVisual(void* display, int screen, int* depth) {
    if (!glxLoad()) return NULL;
    Display* dpy = (Display*)display;
    int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, 0};
    XVisualInfo* vi = X.glXChooseVisual(dpy, screen, attrs);
    if (!vi) return NULL;
    /* 24 bits: a 32-bit (ARGB) visual would make the window see-through under a compositor */
    Visual* v = vi->depth == 24 && vi->class == TrueColor ? vi->visual : NULL;
    if (v) *depth = vi->depth;
    XFree(vi);
    return v;
}

CscdPresenter* platformGLPresenter(void* nativeWindow, int width, int height) {
    GLPresenter* p = CSCD_NEW(GLPresenter);
    p->base.vt = &glVtbl;
    if (!glInit(p, (const CscdXlibWindow*)nativeWindow, width, height)) {
        glDestroy(&p->base);
        return NULL;
    }
    return &p->base;
}
#else
void* platformChooseGLVisual(void* display, int screen, int* depth) {
    (void)display; (void)screen; (void)depth;
    return NULL;
}
CscdPresenter* platformGLPresenter(void* nativeWindow, int width, int height) {
    (void)nativeWindow; (void)width; (void)height;
    return NULL;
}
#endif

CscdPresenter* platformSoftwarePresenter(void* nativeWindow, int width, int height) {
    (void)width;
    (void)height;
    const CscdXlibWindow* w = (const CscdXlibWindow*)nativeWindow;
    SoftwarePresenter* p = CSCD_NEW(SoftwarePresenter);
    p->base.vt = &swVtbl;
    p->dpy = (Display*)w->display;
    p->win = w->window;
    p->depth = 24;
    XWindowAttributes wa;
    if (XGetWindowAttributes(p->dpy, p->win, &wa)) {
        p->visual = wa.visual;
        p->depth = wa.depth;
    } else p->visual = DefaultVisual(p->dpy, DefaultScreen(p->dpy));
    p->gc = XCreateGC(p->dpy, p->win, 0, NULL);
    return &p->base;
}
