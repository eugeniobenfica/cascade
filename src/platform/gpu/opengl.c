/* OpenGL, shared by every platform that has it (CASCADE_OPENGL). The OpenGL library is opened at run
 * time (opengl32.dll, libGL.so.1, OpenGL.framework); only OpenGL 1.1 entry points are used, so this
 * works on every driver, including Windows' built-in GDI one and Mesa's software rasterizers. The
 * contexts are made per platform: windows/presentWin32.c (WGL), linux/presentX11.c (GLX),
 * macos/windowCocoa.m (NSOpenGLContext). */
#include "platform/common/platformInternal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef _WIN32
#define GLAPI __stdcall
#else
#define GLAPI
#endif
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef double GLdouble;

static struct {
    void* lib;
    void(GLAPI* glGenTextures)(GLsizei, GLuint*);
    void(GLAPI* glDeleteTextures)(GLsizei, const GLuint*);
    void(GLAPI* glBindTexture)(GLenum, GLuint);
    void(GLAPI* glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    void(GLAPI* glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
    void(GLAPI* glTexParameteri)(GLenum, GLenum, GLint);
    void(GLAPI* glPixelStorei)(GLenum, GLint);
    void(GLAPI* glViewport)(GLint, GLint, GLsizei, GLsizei);
    void(GLAPI* glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void(GLAPI* glClear)(GLbitfield);
    void(GLAPI* glEnable)(GLenum);
    void(GLAPI* glDisable)(GLenum);
    void(GLAPI* glBegin)(GLenum);
    void(GLAPI* glEnd)(void);
    void(GLAPI* glTexCoord2f)(GLfloat, GLfloat);
    void(GLAPI* glVertex2f)(GLfloat, GLfloat);
    void(GLAPI* glColor4f)(GLfloat, GLfloat, GLfloat, GLfloat);
    void(GLAPI* glMatrixMode)(GLenum);
    void(GLAPI* glLoadIdentity)(void);
    void(GLAPI* glOrtho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
} G;

enum {
    TEXTURE_2D = 0x0DE1, RGBA8 = 0x8058, BGRA = 0x80E1, UNSIGNED_BYTE = 0x1401, QUADS = 0x0007,
    MIN_FILTER = 0x2801, MAG_FILTER = 0x2800, NEAREST = 0x2600, WRAP_S = 0x2802, WRAP_T = 0x2803, CLAMP_TO_EDGE = 0x812F,
    UNPACK_ALIGNMENT = 0x0CF5, UNPACK_ROW_LENGTH = 0x0CF2, UNPACK_SKIP_ROWS = 0x0CF3, UNPACK_SKIP_PIXELS = 0x0CF4, PROJECTION = 0x1701, MODELVIEW = 0x1700, COLOR_BUFFER_BIT = 0x4000, BLEND = 0x0BE2, DEPTH_TEST = 0x0B71
};

bool cscdGlLoadFunctions(void) {
    if (G.glBegin) return true;
    if (!G.lib) {
        static const char* const names[] = {
#if defined(_WIN32)
            "opengl32.dll",
#elif defined(__APPLE__)
            "/System/Library/Frameworks/OpenGL.framework/OpenGL",
#else
            "libGL.so.1", "libGL.so",
#endif
            NULL};
        G.lib = platformOpenLibrary(names);
    }
    if (!G.lib) return false;
#define L(name) *(void**)&G.name = platformSymbol(G.lib, #name); if (!G.name) return false;
    L(glGenTextures) L(glDeleteTextures) L(glBindTexture) L(glTexImage2D) L(glTexSubImage2D) L(glTexParameteri)
    L(glPixelStorei) L(glViewport) L(glClearColor) L(glClear) L(glEnable) L(glDisable)
    L(glEnd) L(glTexCoord2f) L(glVertex2f) L(glColor4f) L(glMatrixMode) L(glLoadIdentity) L(glOrtho) L(glBegin)
#undef L
    return true;
}

void* glLibrary(void) { return cscdGlLoadFunctions() ? G.lib : NULL; }

unsigned cscdGlCreateTexture(void) {
    if (!cscdGlLoadFunctions()) return 0;
    GLuint t = 0;
    G.glGenTextures(1, &t);
    G.glBindTexture(TEXTURE_2D, t);
    G.glTexParameteri(TEXTURE_2D, MIN_FILTER, NEAREST);
    G.glTexParameteri(TEXTURE_2D, MAG_FILTER, NEAREST);
    G.glTexParameteri(TEXTURE_2D, WRAP_S, CLAMP_TO_EDGE);
    G.glTexParameteri(TEXTURE_2D, WRAP_T, CLAMP_TO_EDGE);
    return t;
}

void glDeleteTexture(unsigned texture) {
    GLuint t = texture;
    if (t) G.glDeleteTextures(1, &t);
}

void cscdGlUpload(unsigned texture, const CscdImage* frame) {
    G.glBindTexture(TEXTURE_2D, texture);
    G.glPixelStorei(UNPACK_ALIGNMENT, 4);
    G.glTexImage2D(TEXTURE_2D, 0, RGBA8, frame->width, frame->height, 0, BGRA, UNSIGNED_BYTE, frame->pixels);
}

static void quad(void) {
    G.glMatrixMode(PROJECTION);
    G.glLoadIdentity();
    G.glOrtho(0, 1, 1, 0, -1, 1); /* y down, like the frame */
    G.glMatrixMode(MODELVIEW);
    G.glLoadIdentity();
    G.glDisable(DEPTH_TEST);
    G.glDisable(BLEND);
    G.glEnable(TEXTURE_2D);
    G.glColor4f(1, 1, 1, 1);
    G.glBegin(QUADS);
    G.glTexCoord2f(0, 0); G.glVertex2f(0, 0);
    G.glTexCoord2f(1, 0); G.glVertex2f(1, 0);
    G.glTexCoord2f(1, 1); G.glVertex2f(1, 1);
    G.glTexCoord2f(0, 1); G.glVertex2f(0, 1);
    G.glEnd();
    G.glDisable(TEXTURE_2D);
}

void cscdGlDrawFullscreen(unsigned texture, int vw, int vh) {
    G.glViewport(0, 0, vw, vh);
    G.glBindTexture(TEXTURE_2D, texture);
    quad();
}

void glPresentFrame(unsigned* texture, int* texW, int* texH, const CscdImage* frame, int width, int height, const CscdPresenter* hint) {
    (void)width;
    if (!*texture) *texture = cscdGlCreateTexture();
    G.glBindTexture(TEXTURE_2D, *texture);
    G.glPixelStorei(UNPACK_ALIGNMENT, 4);
    if (frame->width != *texW || frame->height != *texH) {
        G.glTexImage2D(TEXTURE_2D, 0, RGBA8, frame->width, frame->height, 0, BGRA, UNSIGNED_BYTE, frame->pixels);
        *texW = frame->width;
        *texH = frame->height;
    } else if (hint && hint->partial) {
        /* only what changed: the texture keeps the rest from the frames before */
        G.glPixelStorei(UNPACK_ROW_LENGTH, frame->width);
        for (size_t i = 0; i < hint->damageCount; ++i) {
            const CscdIRect r = hint->damage[i];
            const int x0 = cscdMaxi(0, r.x), y0 = cscdMaxi(0, r.y);
            const int x1 = cscdMini(frame->width, r.x + r.w), y1 = cscdMini(frame->height, r.y + r.h);
            if (x1 <= x0 || y1 <= y0) continue;
            G.glPixelStorei(UNPACK_SKIP_PIXELS, x0);
            G.glPixelStorei(UNPACK_SKIP_ROWS, y0);
            G.glTexSubImage2D(TEXTURE_2D, 0, x0, y0, x1 - x0, y1 - y0, BGRA, UNSIGNED_BYTE, frame->pixels);
        }
        G.glPixelStorei(UNPACK_ROW_LENGTH, 0);
        G.glPixelStorei(UNPACK_SKIP_PIXELS, 0);
        G.glPixelStorei(UNPACK_SKIP_ROWS, 0);
    } else {
        G.glTexSubImage2D(TEXTURE_2D, 0, 0, 0, frame->width, frame->height, BGRA, UNSIGNED_BYTE, frame->pixels);
    }
    /* draw at 1:1 even while the window is being resized */
    G.glClearColor(1, 1, 1, 1);
    G.glClear(COLOR_BUFFER_BIT);
    G.glViewport(0, cscdMaxi(1, height) - frame->height, frame->width, frame->height);
    quad();
}
