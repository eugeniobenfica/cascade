/* cascade — a native window that shows a CscdView, with input, DPI scaling, clipboard, cursors, drag
 * and drop and Vulkan / OpenGL / software presentation. Win32 on Windows (windowWin32.c), Wayland or
 * X11 on Linux and the BSDs (windowWayland.c, windowX11.c), Cocoa on macOS (windowCocoa.m), UIKit
 * on iOS (windowUikit.m), cascade's activity on Android (windowAndroid.c and, in Java,
 * src/platform/android). */
#ifndef CASCADE_WINDOW_H
#define CASCADE_WINDOW_H

#include "cascade/gpu.h"
#include "cascade/view.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

/* iOS and Android start the program themselves (UIApplicationMain, cascade's activity) and hand it
 * over once they are ready. There this header renames the program's main to cscdAppMain, which
 * cascade calls: the same main(int argc, char** argv) runs everywhere. (In C++, main must take
 * those two arguments.) */
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define CSCD_MOBILE 1
#define main cscdAppMain
#ifdef __cplusplus
extern "C" int cscdAppMain(int argc, char** argv);
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Makes the window's presenter, for an app that draws in the window itself: a 3D engine showing its
 * scene under the page, say. surface is what cscdPresenterNew takes, the size is in device pixels.
 * Called when the window has its surface, and again whenever it needs a new presenter: Android gives
 * a new surface after the app comes back, and a presenter whose present returned false (a lost
 * device) is freed and made again. The window owns what this returns, and destroys it before its
 * surface goes. NULL: cascade's own presenter. See cscdViewSetTransparent. */
typedef CscdPresenter* (*CscdPresenterFn)(void* user, void* surface, int width, int height);

typedef struct CscdWindowOptions {
    const char* title;   /* NULL or "": the page's <title> ("cascade" until there is one) */
    int width, height;   /* client size in CSS pixels */
    CscdBackend backend;
    bool resizable;
    bool darkTitleBar;
    int minWidth, minHeight;
    /* false: the window stays hidden until cscdWindowShow or cscdWindowMaximize, so that the app can
     * load its first page and the window never shows an empty one. */
    bool visible;
    /* A page to load before the window shows ("ui/index.html": found from the working folder or next
     * to the program). Its <title> becomes the window's title when title is empty. */
    const char* page;
    /* Style sheet files are read again when they change on disk (CSCD_FEATURE_LIVE_RELOAD). -1: on in
     * debug builds of a program made with cascade_app() in CMake, off otherwise. */
    int liveReload;
    /* NULL: cascade chooses by backend. Set: backend and CSCD_FEATURE_GPU no longer choose. */
    CscdPresenterFn presenter;
    void* presenterUser;
} CscdWindowOptions;

/* The defaults: "cascade", 1024 x 720, Auto, resizable, visible, minimum 200 x 150. */
CscdWindowOptions cscdWindowOptions(void);

typedef struct CscdWindow CscdWindow;

CscdWindow* cscdWindowNew(const CscdWindowOptions* opts);
void cscdWindowFree(CscdWindow* w);

CscdView* cscdWindowView(CscdWindow* w);
/* Runs the message loop until the window closes; returns the exit code. */
int cscdWindowRun(CscdWindow* w);
/* Processes pending events once; with wait it sleeps until there is something to do. Returns false
 * once the window has been closed. */
bool cscdWindowPump(CscdWindow* w, bool wait);
void cscdWindowClose(CscdWindow* w);
bool cscdWindowIsOpen(const CscdWindow* w);
void cscdWindowSetTitle(CscdWindow* w, const char* title);
void cscdWindowResize(CscdWindow* w, int width, int height);
void cscdWindowMaximize(CscdWindow* w); /* shows the window too, when it was created hidden */
void cscdWindowShow(CscdWindow* w);
void cscdWindowSetFullscreen(CscdWindow* w, bool on);
bool cscdWindowIsFullscreen(const CscdWindow* w);
CscdBackend cscdWindowBackend(const CscdWindow* w);
const char* cscdWindowBackendDescription(const CscdWindow* w);
/* Shows frames through this backend from now on; what could be had in the end: cscdWindowBackend.
 * Not from inside a frame (cscdWindowOnFrame). */
void cscdWindowSetBackend(CscdWindow* w, CscdBackend backend);
/* The HWND (Windows), the X11 Window id (Linux), the NSWindow* (macOS), the UIWindow* (iOS) or the
 * activity (Android: a JNI global reference to the org.cascade.CascadeActivity). */
void* cscdWindowNativeHandle(const CscdWindow* w);
/* The X11 Display* the window is on (Linux); NULL elsewhere. */
void* cscdWindowNativeDisplay(const CscdWindow* w);
/* Device pixels per CSS pixel. */
float cscdWindowDevicePixelRatio(const CscdWindow* w);

/* Return false to keep the window open. */
typedef bool (*CscdCloseFn)(void* user);
void cscdWindowOnClose(CscdWindow* w, CscdCloseFn fn, void* user);
/* Called once per frame before painting (game loops, live data). */
void cscdWindowOnFrame(CscdWindow* w, CscdActionFn fn, void* user);
/* Runs fn on the window's thread soon (taken); callable from any thread (background work handing its
 * result to the interface). */
void cscdWindowPost(CscdWindow* w, CscdAction fn);

/* ---- the app and the system around it ---------------------------------------------------------------
 * What phones do to apps: send them to the background (save what the user would lose there: the
 * system may end a background app without warning), bring them back, and ask them to free memory.
 * The window stops drawing while the app is in the background. On desktops the window sends
 * BACKGROUND when it is minimized and FOREGROUND when it comes back. */
typedef enum CscdAppEvent { CSCD_APP_BACKGROUND, CSCD_APP_FOREGROUND, CSCD_APP_LOW_MEMORY } CscdAppEvent;
typedef void (*CscdAppEventFn)(void* user, CscdAppEvent event);
void cscdWindowOnAppEvent(CscdWindow* w, CscdAppEventFn fn, void* user);

/* Opens a link with the system (the browser, the mail app for mailto:, the dialer for tel:). */
bool cscdWindowOpenUrl(CscdWindow* w, const char* url);
/* The system's share sheet with this text (iOS, Android); false where there is none. */
bool cscdWindowShareText(CscdWindow* w, const char* text);
/* The system's file picker. accept: what the accept attribute of <input type=file> takes (".pdf,.png",
 * "image/png", whole kinds as "image/" with a star; NULL or "": any).
 * fn gets the files chosen (paths the program can read: copies, on phones), none when the user
 * cancelled; it is called once, perhaps before this returns. false: no picker here (fn is not
 * called). */
typedef void (*CscdPickFilesFn)(void* user, const CscdStrVec* paths);
bool cscdWindowPickFiles(CscdWindow* w, const char* accept, bool multiple, CscdPickFilesFn fn, void* user);
/* The system's dialog to save a file. name: the name it offers ("report.pdf", NULL: none); accept:
 * the kinds offered, as cscdWindowPickFiles takes them (the first gives the extension a name typed
 * without one gets). fn gets the path to write the file at, or NULL when the user cancelled, and
 * returns whether it wrote it; it is called once, perhaps before this returns. On phones the path is
 * a file of the app's own that the system then copies where the user chose; on iOS fn writes first
 * and the place is chosen after (cancelling then leaves the file nowhere). false: no dialog here (fn
 * is not called). */
typedef bool (*CscdSaveFileFn)(void* user, const char* path);
bool cscdWindowSaveFile(CscdWindow* w, const char* name, const char* accept, CscdSaveFileFn fn, void* user);
/* Hides the window (cscdWindowShow shows it again): an app that lives in the tray. false where a
 * window cannot be hidden (Linux's Wayland, phones). */
bool cscdWindowHide(CscdWindow* w);

/* ---- the system's menus and tray ---- */

/* The application's menu bar where the system has one of its own: macOS's, at the top of the
 * screen. Each item of menu is a menu of the bar: its label the title, its submenu (or fill, called
 * as it opens) the items, with checks, separators, disabled items and shortcuts ("Ctrl+S",
 * "Cmd+Shift+Z": Ctrl and Cmd are both Command there). The app's own menu (Hide, Quit) stays first.
 * The menu is taken (freed at once when false); another call replaces it. false where there is no
 * such bar (Windows, Linux, phones: put one in the page with cscdViewBindMenuBar). */
bool cscdWindowSetMenuBar(CscdWindow* w, CscdMenu menu);

/* An icon in the system's tray: the notification area (Windows), a status item in the menu bar
 * (macOS). icon: its picture (scaled to the tray's size; copied); tooltip: its name. A click runs
 * the onClick action; a right click opens its menu (a click too, when it has no action). The
 * menu's actions and the click run on the window's thread, as its events do. NULL where there is
 * no tray (Linux, phones). Free it before the window. */
typedef struct CscdTray CscdTray;
CscdTray* cscdTrayNew(CscdWindow* w, const CscdImage* icon, const char* tooltip);
void cscdTraySetIcon(CscdTray* t, const CscdImage* icon);
void cscdTraySetTooltip(CscdTray* t, const char* tooltip);
void cscdTraySetMenu(CscdTray* t, CscdMenu menu);    /* taken; replaces the one before */
void cscdTrayOnClick(CscdTray* t, CscdAction action); /* taken */
void cscdTrayFree(CscdTray* t);

/* A short vibration that answers a touch (phones; nothing elsewhere). */
typedef enum CscdHaptic { CSCD_HAPTIC_TAP, CSCD_HAPTIC_SELECTION, CSCD_HAPTIC_SUCCESS, CSCD_HAPTIC_WARNING, CSCD_HAPTIC_ERROR } CscdHaptic;
void cscdWindowHaptic(CscdWindow* w, CscdHaptic kind);

#ifdef __cplusplus
}
#endif

#endif
