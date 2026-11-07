/* Windows presenters: OpenGL through WGL and the software path through GDI. Only OpenGL 1.1 plus
 * wglSwapIntervalEXT is used (gl_backend.c). */
#include "platform/common/platformInternal.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>

#ifndef CASCADE_NO_OPENGL /* (OpenGL through WGL: CASCADE_OPENGL) */
static struct {
    HGLRC(WINAPI* wglCreateContext)(HDC);
    BOOL(WINAPI* wglMakeCurrent)(HDC, HGLRC);
    BOOL(WINAPI* wglDeleteContext)(HGLRC);
    PROC(WINAPI* wglGetProcAddress)(LPCSTR);
    const unsigned char*(WINAPI* glGetString)(unsigned);
    BOOL(WINAPI* wglSwapIntervalEXT)(int);
} W;

static bool wglLoad(void) {
    if (W.wglCreateContext) return true;
    if (!cscdGlLoadFunctions()) return false;
    void* lib = glLibrary();
#define L(name) *(void**)&W.name = platformSymbol(lib, #name); if (!W.name) return false;
    L(wglMakeCurrent) L(wglDeleteContext) L(wglGetProcAddress) L(glGetString) L(wglCreateContext)
#undef L
    return true;
}

/* ---- OpenGL ----------------------------------------------------------------------------------------- */

typedef struct GLPresenter {
    CscdPresenter base;
    HWND hwnd;
    HDC dc;
    HGLRC rc;
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
    W.wglMakeCurrent(p->dc, p->rc);
    glPresentFrame(&p->tex, &p->texW, &p->texH, frame, p->width, p->height, bp);
    return SwapBuffers(p->dc) != FALSE;
}

static void glDestroy(CscdPresenter* bp) {
    GLPresenter* p = (GLPresenter*)bp;
    if (p->rc) {
        W.wglMakeCurrent(p->dc, p->rc);
        if (p->tex) glDeleteTexture(p->tex);
        W.wglMakeCurrent(NULL, NULL);
        W.wglDeleteContext(p->rc);
    }
    if (p->dc) ReleaseDC(p->hwnd, p->dc);
    cscdFree(p);
}

static const CscdPresenterVtbl glVtbl = {glBackend, glPresent, glResize, glDescription, glDestroy};

static bool glInit(GLPresenter* p, HWND h, int w, int ht) {
    p->hwnd = h;
    p->width = w;
    p->height = ht;
    if (!wglLoad()) return false;
    p->dc = GetDC(h);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(p->dc, &pfd);
    if (!pf || !SetPixelFormat(p->dc, pf, &pfd)) return false;
    p->rc = W.wglCreateContext(p->dc);
    if (!p->rc || !W.wglMakeCurrent(p->dc, p->rc)) return false;
    *(void**)&W.wglSwapIntervalEXT = (void*)W.wglGetProcAddress("wglSwapIntervalEXT");
    if (W.wglSwapIntervalEXT) W.wglSwapIntervalEXT(1);
    const unsigned char* r = W.glGetString(0x1F01);
    snprintf(p->description, sizeof p->description, "OpenGL (%s)", r ? (const char*)r : "");
    p->tex = cscdGlCreateTexture();
    return p->tex != 0;
}

CscdPresenter* platformGLPresenter(void* nativeWindow, int width, int height) {
    GLPresenter* p = CSCD_NEW(GLPresenter);
    p->base.vt = &glVtbl;
    if (!glInit(p, (HWND)nativeWindow, width, height)) {
        glDestroy(&p->base);
        return NULL;
    }
    return &p->base;
}
#else
CscdPresenter* platformGLPresenter(void* nativeWindow, int width, int height) {
    (void)nativeWindow; (void)width; (void)height;
    return NULL;
}
#endif

/* ---- GDI -------------------------------------------------------------------------------------------- */

typedef struct SoftwarePresenter { CscdPresenter base; HWND hwnd; } SoftwarePresenter;

static CscdBackend swBackend(const CscdPresenter* p) { (void)p; return CSCD_BACKEND_SOFTWARE; }
static const char* swDescription(const CscdPresenter* p) { (void)p; return "Software (GDI)"; }
static void swResize(CscdPresenter* p, int w, int h) { (void)p; (void)w; (void)h; }
static void swDestroy(CscdPresenter* p) { cscdFree(p); }

static bool swPresent(CscdPresenter* bp, const CscdImage* frame) {
    SoftwarePresenter* p = (SoftwarePresenter*)bp;
    HDC dc = GetDC(p->hwnd);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = frame->width;
    bi.bmiHeader.biHeight = -frame->height; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, 0, 0, (DWORD)frame->width, (DWORD)frame->height, 0, 0, 0, (UINT)frame->height, frame->pixels, &bi, DIB_RGB_COLORS);
    ReleaseDC(p->hwnd, dc);
    return true;
}

static const CscdPresenterVtbl swVtbl = {swBackend, swPresent, swResize, swDescription, swDestroy};

CscdPresenter* platformSoftwarePresenter(void* nativeWindow, int width, int height) {
    (void)width;
    (void)height;
    SoftwarePresenter* p = CSCD_NEW(SoftwarePresenter);
    p->base.vt = &swVtbl;
    p->hwnd = (HWND)nativeWindow;
    return &p->base;
}
