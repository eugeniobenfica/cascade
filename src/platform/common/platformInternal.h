/* What the window backends (windowWin32.c, windowX11.c, windowCocoa.m, windowUikit.m,
 * windowAndroid.c) and the presenters share.
 * Not installed. */
#ifndef CASCADE_PLATFORM_INTERNAL_H
#define CASCADE_PLATFORM_INTERNAL_H

#include "cascade/gpu.h"
#include "cascade/window.h"

#include "core/thread.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
/* Phones and tablets: the system draws around the page (windowSetBackdrop); desktops skip that work. */
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define CSCD_PLATFORM_MOBILE 1
#endif

/* (widgets.c: when the touches that follow happened, as viewInternal.h says) */
void viewSetInputAge(CscdView* v, double seconds);

/* Shared libraries opened at run time (LoadLibrary / dlopen): the first of the names (a NULL-ended
 * list) that opens wins. */
void* platformOpenLibrary(const char* const* names);
void* platformSymbol(void* lib, const char* name);
void platformCloseLibrary(void* lib);

/* Double and triple clicks for the systems that do not count them (AppKit does): presses within
 * interval ms of each other and a few device pixels apart. */
typedef struct ClickCounter { int clicks; unsigned lastTime; int lastX, lastY; } ClickCounter;
CSCD_INLINE int clickPress(ClickCounter* c, unsigned time, int x, int y, unsigned interval) {
    if (c->clicks && time - c->lastTime <= interval && x - c->lastX < 4 && c->lastX - x < 4 && y - c->lastY < 4 && c->lastY - y < 4) ++c->clicks;
    else c->clicks = 1;
    c->lastTime = time;
    c->lastX = x;
    c->lastY = y;
    return c->clicks;
}

/* The presenters of each kind for this platform; NULL when the kind is not available. nativeWindow
 * is what cscdPresenterNew takes (gpu.h). */
CscdPresenter* platformVulkanPresenter(void* nativeWindow, int width, int height);
CscdPresenter* platformGLPresenter(void* nativeWindow, int width, int height);
CscdPresenter* platformSoftwarePresenter(void* nativeWindow, int width, int height);

#if defined(__APPLE__)
/* A CAMetalLayer that backs the NSView, for Vulkan through MoltenVK (windowCocoa.m). */
void* platformMetalLayer(void* nsview);
#elif !defined(_WIN32)
/* A 24-bit TrueColor visual OpenGL can draw into with double buffering, for the window to be created
 * with (presentX11.c); NULL without OpenGL, and the default visual is used. */
void* platformChooseGLVisual(void* display, int screen, int* depth);
#endif

#ifdef CASCADE_WAYLAND
/* Wayland's (windowWayland.c): frames through wl_shm buffers */
CscdPresenter* platformWaylandSoftwarePresenter(void* nativeWindow, int width, int height);
#endif

/* The OpenGL library cscdGlLoadFunctions opened (opengl32.dll, libGL.so.1, OpenGL.framework), for the
 * context functions (wgl*, glX*) that live in it too. */
void* glLibrary(void);
void glDeleteTexture(unsigned texture);
/* Draws frame 1:1 at the top-left of a width x height drawable of the current context, uploading it
 * into texture (created on first use; texW / texH track its size). */
void glPresentFrame(unsigned* texture, int* texW, int* texH, const CscdImage* frame, int width, int height, const CscdPresenter* hint);

/* ---- the window ----------------------------------------------------------------------------------- */

/* The platform's side: the Win32 window procedure, the X11 connection or the Cocoa view, with the
 * input method, selections and drags. */
typedef struct CscdWindowNative CscdWindowNative;

/* What the system shows around the page (under a phone's bars, beside its notch, above its home
 * indicator), from the frame's edges: the colors of its top and bottom rows (the page's background
 * when no color rules a row, so what scrolls under a bar does not tint it), and its left and right
 * columns as runs of rows of one color: pairs of (first row, color), top to bottom, rows in device
 * pixels of the frame. Colors are 0xAARRGGBB, opaque. */
typedef struct CscdWindowBackdrop {
    uint32_t top, bottom;
    CscdU32Vec left, right;
} CscdWindowBackdrop;

struct CscdWindow {
    CscdWindowNative* native;
    CscdView* view;
    void* hwnd;    /* the native handle (NULL once closed) */
    void* display;
    bool fullscreen;
    bool shown;
    bool dirty;    /* something asked to be painted again */
    CscdPresenter* presenter;
    CscdImage frame;
    float dpr;
    int clientW, clientH;
    CscdBackend requested;
    CscdPresenterFn makePresenter; void* makePresenterUser; /* (an app's presenter, in place of cascade's) */
    bool gpuApplied;       /* (CSCD_FEATURE_GPU as the presenter was last chosen for it) */
    CscdIRect lastOverlay; /* where the stats panel was in the frame shown last */
    bool titleFromDocument;
    bool rendering;
    CscdStr lastTitle; /* the document's title last shown */
    CscdMutex postMutex;
    CSCD_VEC(CscdAction) posted;
    CscdCloseFn onClose; void* onCloseUser;
    CscdActionFn onFrame; void* onFrameUser;
    CscdAppEventFn onAppEvent; void* onAppEventUser;
    CscdPickFilesFn pickFn; void* pickUser; /* (a file picker open) */
    CscdSaveFileFn saveFn; void* saveUser; /* (a save dialog open) */
    bool background;       /* the app is in the background: nothing is drawn */
    CscdWindowBackdrop backdrop; /* as last told to windowSetBackdrop (top 0: nothing told yet) */
    /* where a drag sideways is the page's (Android, windowSetGestureExclusion): the candidates, found again
     * when the document changes, and the rectangles last told */
    CscdPtrVec sideDrags; /* CscdElement* */
    uint64_t sideDragsVersion;
    CscdIRectVec sideDragRects;
    bool sideDragsTold;
    void* a11y; /* the platform's accessibility objects */
};

/* window.c: what every platform shares */
void windowRender(CscdWindow* w);
#ifdef __APPLE__
/* a11yCocoa.m: NSAccessibility (objects returned retained: __bridge_transfer them) */
void cocoaA11yAttach(CscdWindow* w);
void cocoaA11yDetach(CscdWindow* w);
void* cocoaA11yChildren(CscdWindow* w);                 /* NSArray */
void* cocoaA11yHitTest(CscdWindow* w, double x, double y); /* an element, or NULL (screen coordinates) */
void* cocoaA11yFocused(CscdWindow* w);                  /* an element, or NULL */
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
/* mainUikit.m started UIKit (the program's main was renamed, window.h): windows can be made */
extern bool uikitStarted;
/* a11yUikit.m: UIAccessibility (NSArray returned retained: __bridge_transfer it) */
void uikitA11yAttach(CscdWindow* w);
void uikitA11yDetach(CscdWindow* w);
void* uikitA11yElements(CscdWindow* w); /* NSArray: the page's accessible elements, in order */
#endif
#ifdef _WIN32
/* a11yWin32.c: UI Automation */
void winA11yAttach(CscdWindow* w);
bool winA11yGetObject(CscdWindow* w, uintptr_t wp, intptr_t lp, intptr_t* out); /* (WM_GETOBJECT) */
void winA11yDetach(CscdWindow* w);
#endif
void windowRunPosted(CscdWindow* w);
bool windowHasPosted(CscdWindow* w);
/* what the system did: the app's callback (BACKGROUND stops the drawing, FOREGROUND draws again) */
void windowAppEvent(CscdWindow* w, CscdAppEvent event);
/* the files the picker gave (none: cancelled), to the callback cscdWindowPickFiles was given */
void windowFilesPicked(CscdWindow* w, const CscdStrVec* paths);
/* where the save dialog said to write (NULL: cancelled), to the callback cscdWindowSaveFile was
 * given; whether it wrote the file */
bool windowSaveChosen(CscdWindow* w, const char* path);
/* the file name extensions (lowercase, no dot) an accept attribute names; none: any file */
void windowAcceptExtensions(const char* accept, CscdStrVec* out);
/* the system's theme, motion and text size, for the view */
void windowSystemPreferences(CscdWindow* w, CscdSystemPreferences p);
#ifdef CASCADE_WAYLAND
/* a11yAtspi.c: AT-SPI on the accessibility bus (D-Bus), for Orca: the connection the window's loop
 * waits on too (-1: none), and what came on it (answered without waiting) */
void atspiAttach(CscdWindow* w);
void atspiDetach(CscdWindow* w);
int platformA11yFd(CscdWindow* w);
void platformA11yRead(CscdWindow* w);
/* where the window's client area is on the screen (device pixels; 0, 0 when the system does not tell) */
void platformWindowOrigin(CscdWindow* w, int* x, int* y);
void x11WindowOrigin(CscdWindow* w, int* x, int* y);
/* the window's two systems (windowLinux.c chooses one): Wayland's calls (windowWayland.c) */
bool wlAvailable(void);
void wlWindowCreateNative(CscdWindow* w, const CscdWindowOptions* opts);
void wlWindowDestroyNative(CscdWindow* w);
void* wlWindowSurface(const CscdWindow* w);
void wlWindowShowTitle(CscdWindow* w, const char* title);
void wlWindowWake(CscdWindow* w);
void wlWindowPlaceInputMethod(CscdWindow* w);
void wlWindowShow(CscdWindow* w);
void wlWindowResize(CscdWindow* w, int width, int height);
void wlWindowMaximize(CscdWindow* w);
void wlWindowSetFullscreen(CscdWindow* w, bool on);
void wlWindowClose(CscdWindow* w);
bool wlWindowPump(CscdWindow* w, bool wait);
/* X11's (windowX11.c, its own names renamed) */
void x11WindowCreateNative(CscdWindow* w, const CscdWindowOptions* opts);
void x11WindowDestroyNative(CscdWindow* w);
void* x11WindowSurface(const CscdWindow* w);
void x11WindowShowTitle(CscdWindow* w, const char* title);
void x11WindowWake(CscdWindow* w);
void x11WindowPlaceInputMethod(CscdWindow* w);
void x11WindowShow(CscdWindow* w);
bool x11WindowHide(CscdWindow* w);
void x11WindowResize(CscdWindow* w, int width, int height);
void x11WindowMaximize(CscdWindow* w);
void x11WindowSetFullscreen(CscdWindow* w, bool on);
void x11WindowClose(CscdWindow* w);
bool x11WindowPump(CscdWindow* w, bool wait);
#endif

/* what each platform supplies (windowWin32.c, windowX11.c, windowCocoa.m, windowUikit.m,
 * windowAndroid.c), besides the public
 * cscdWindowShow, cscdWindowResize, cscdWindowMaximize, cscdWindowSetFullscreen, cscdWindowClose and
 * cscdWindowPump */
void windowCreateNative(CscdWindow* w, const CscdWindowOptions* opts);
void windowDestroyNative(CscdWindow* w);
void* windowSurface(const CscdWindow* w);           /* what cscdPresenterNew takes */
/* The window's presenter for a surface: the app's when it makes one, or cascade's. */
CscdPresenter* windowPresenterNew(CscdWindow* w, void* surface, CscdBackend preferred, int width, int height);
void windowShowTitle(CscdWindow* w, const char* title); /* in the title bar */
void windowWake(CscdWindow* w);                     /* brings a waiting pump round */
void windowPlaceInputMethod(CscdWindow* w);
#ifdef CSCD_PLATFORM_MOBILE
/* What the system shows around the page (CscdWindowBackdrop): told when it changes (phones only). */
void windowSetBackdrop(CscdWindow* w, const CscdWindowBackdrop* b);
#endif
#ifdef __ANDROID__
/* Where a drag sideways belongs to the page (device pixels of the frame): the system's back gesture
 * from the screen's sides leaves those places alone. Told when it changes. */
void windowSetGestureExclusion(CscdWindow* w, const CscdIRect* rects, size_t count);
#endif
void windowBackdropForget(CscdWindow* w); /* (told again with the next frame: a new activity, a view of its own) */
/* the system around the app (cscdWindowOpenUrl, ShareText, PickFiles, SaveFile, Haptic) */
bool platformOpenUrl(CscdWindow* w, const char* url);
bool platformShareText(CscdWindow* w, const char* text);
bool platformPickFiles(CscdWindow* w, const char* accept, bool multiple); /* the result: windowFilesPicked */
bool platformSaveFile(CscdWindow* w, const char* name, const char* accept); /* the result: windowSaveChosen */
void platformHaptic(CscdWindow* w, CscdHaptic kind);
bool platformHide(CscdWindow* w);

/* the system's menus and tray (cscdWindowSetMenuBar, cscdTray*): the platform builds its own
 * menus from CscdMenu and tells windowMenuChosen what was chosen */
struct CscdTray {
    CscdWindow* w;
    CscdMenu menu;
    CscdAction click;
    void* native;
};
bool platformSetMenuBar(CscdWindow* w, CscdMenu* menu); /* takes *menu when it returns true */
bool platformTrayCreate(CscdTray* t, const CscdImage* icon, const char* tooltip);
void platformTraySet(CscdTray* t, const CscdImage* icon, const char* tooltip); /* (NULL: as it was) */
void platformTrayDestroy(CscdTray* t);
void windowMenuChosen(CscdWindow* w, const CscdMenuItem* item);
void windowTrayClicked(CscdTray* t);

#endif
