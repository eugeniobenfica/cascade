/* Linux and the BSDs: the window on Wayland when the session is a Wayland one (WAYLAND_DISPLAY set)
 * and libwayland-client opens, on X11 otherwise (an X session, or XWayland when Wayland is not to be
 * had). CASCADE_BACKEND=x11 or =wayland chooses. A program uses one: the first window decides. The
 * screen reader's side (AT-SPI) is the same on both. */
#include "platform/common/platformInternal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int system_; /* 0: not chosen yet, 1: X11, 2: Wayland */

static bool wayland(void) {
    if (!system_) {
        const char* force = getenv("CASCADE_BACKEND");
#ifdef CASCADE_X11
        if (force && !strcmp(force, "x11")) system_ = 1;
        else system_ = wlAvailable() ? 2 : 1;
#else
        (void)force;
        system_ = 2;
#endif
    }
    return system_ == 2;
}

#ifndef CASCADE_X11
/* (built without the Xlib headers: Wayland only) */
void x11WindowCreateNative(CscdWindow* w, const CscdWindowOptions* opts) { (void)w; (void)opts; }
void x11WindowDestroyNative(CscdWindow* w) { (void)w; }
void* x11WindowSurface(const CscdWindow* w) { (void)w; return NULL; }
void x11WindowShowTitle(CscdWindow* w, const char* title) { (void)w; (void)title; }
void x11WindowWake(CscdWindow* w) { (void)w; }
void x11WindowPlaceInputMethod(CscdWindow* w) { (void)w; }
void x11WindowShow(CscdWindow* w) { (void)w; }
bool x11WindowHide(CscdWindow* w) { (void)w; return false; }
void x11WindowResize(CscdWindow* w, int width, int height) { (void)w; (void)width; (void)height; }
void x11WindowMaximize(CscdWindow* w) { (void)w; }
void x11WindowSetFullscreen(CscdWindow* w, bool on) { (void)w; (void)on; }
void x11WindowClose(CscdWindow* w) { (void)w; }
bool x11WindowPump(CscdWindow* w, bool wait) { (void)w; (void)wait; return false; }
void x11WindowOrigin(CscdWindow* w, int* x, int* y) { (void)w; *x = *y = 0; }
CscdPresenter* platformGLPresenter(void* nativeWindow, int width, int height) { (void)nativeWindow; (void)width; (void)height; return NULL; }
CscdPresenter* platformSoftwarePresenter(void* nativeWindow, int width, int height) {
    return platformWaylandSoftwarePresenter(nativeWindow, width, height);
}
void* platformChooseGLVisual(void* display, int screen, int* depth) { (void)display; (void)screen; (void)depth; return NULL; }
#endif

void platformWindowOrigin(CscdWindow* w, int* x, int* y) {
    *x = *y = 0;
    if (!wayland()) x11WindowOrigin(w, x, y); /* (Wayland keeps where windows are to itself) */
}

void windowCreateNative(CscdWindow* w, const CscdWindowOptions* opts) {
    if (wayland()) wlWindowCreateNative(w, opts);
    else x11WindowCreateNative(w, opts);
    if (w->hwnd) atspiAttach(w);
}

void windowDestroyNative(CscdWindow* w) {
    atspiDetach(w);
    if (wayland()) wlWindowDestroyNative(w);
    else x11WindowDestroyNative(w);
}

void* windowSurface(const CscdWindow* w) { return wayland() ? wlWindowSurface(w) : x11WindowSurface(w); }
void windowShowTitle(CscdWindow* w, const char* title) { if (wayland()) wlWindowShowTitle(w, title); else x11WindowShowTitle(w, title); }
void windowWake(CscdWindow* w) { if (wayland()) wlWindowWake(w); else x11WindowWake(w); }
void windowPlaceInputMethod(CscdWindow* w) { if (wayland()) wlWindowPlaceInputMethod(w); else x11WindowPlaceInputMethod(w); }
void cscdWindowShow(CscdWindow* w) { if (wayland()) wlWindowShow(w); else x11WindowShow(w); }
void cscdWindowResize(CscdWindow* w, int width, int height) { if (wayland()) wlWindowResize(w, width, height); else x11WindowResize(w, width, height); }
void cscdWindowMaximize(CscdWindow* w) { if (wayland()) wlWindowMaximize(w); else x11WindowMaximize(w); }
void cscdWindowSetFullscreen(CscdWindow* w, bool on) { if (wayland()) wlWindowSetFullscreen(w, on); else x11WindowSetFullscreen(w, on); }
void cscdWindowClose(CscdWindow* w) { if (wayland()) wlWindowClose(w); else x11WindowClose(w); }
bool cscdWindowPump(CscdWindow* w, bool wait) { return wayland() ? wlWindowPump(w, wait) : x11WindowPump(w, wait); }

/* ---- the system around the app: the desktop's own programs --------------------------------------- */

bool platformShareText(CscdWindow* w, const char* text) { (void)w; (void)text; return false; }
void platformHaptic(CscdWindow* w, CscdHaptic kind) { (void)w; (void)kind; }

/* Runs a program (looked up in PATH) without a shell; out: what it printed (NULL: not waited for,
 * left to run on its own). Returns its exit status, -1 when it could not be started. */
static int run(char* const argv[], CscdStr* out) {
    int fds[2] = {-1, -1};
    if (out && pipe(fds) != 0) return -1;
    const pid_t pid = fork();
    if (pid < 0) {
        if (out) { close(fds[0]); close(fds[1]); }
        return -1;
    }
    if (pid == 0) {
        if (out) {
            dup2(fds[1], STDOUT_FILENO);
            close(fds[0]);
            close(fds[1]);
        } else {
            /* (a grandchild, so that nobody has to wait for it) */
            setsid();
            if (fork() != 0) _exit(0);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (out) {
        close(fds[1]);
        char buf[4096];
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof buf)) > 0) cscdStrAppend(out, cscdSvMake(buf, (size_t)n));
        close(fds[0]);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static bool inPath(const char* program) {
    const char* path = getenv("PATH");
    if (!path) return false;
    CscdSv rest = cscdSv(path);
    while (rest.len) {
        size_t colon = cscdSvFindChar(rest, ':', 0);
        const CscdSv dir = cscdSvSub(rest, 0, colon == CSCD_NPOS ? rest.len : colon);
        rest = colon == CSCD_NPOS ? CSCD_SV("") : cscdSvSub(rest, colon + 1, rest.len - colon - 1);
        CscdStr p = cscdStrDup(dir);
        cscdStrAppendChar(&p, '/');
        cscdStrAppendC(&p, program);
        const bool found = access(cscdStrC(&p), X_OK) == 0;
        cscdStrFree(&p);
        if (found) return true;
    }
    return false;
}

/* (a tray needs StatusNotifierItem over D-Bus, and no menu bar is the system's: not yet here) */
bool platformSetMenuBar(CscdWindow* w, CscdMenu* menu) { (void)w; (void)menu; return false; }
bool platformTrayCreate(CscdTray* t, const CscdImage* icon, const char* tooltip) { (void)t; (void)icon; (void)tooltip; return false; }
void platformTraySet(CscdTray* t, const CscdImage* icon, const char* tooltip) { (void)t; (void)icon; (void)tooltip; }
void platformTrayDestroy(CscdTray* t) { (void)t; }

bool platformHide(CscdWindow* w) { return !wayland() && x11WindowHide(w); }

bool platformOpenUrl(CscdWindow* w, const char* url) {
    (void)w;
    if (!inPath("xdg-open")) return false;
    char* argv[] = {(char*)"xdg-open", (char*)url, NULL};
    return run(argv, NULL) == 0;
}

/* the patterns of an accept list, for the choosers: "*.png *.jpg" (empty: any file) */
static CscdStr acceptPatterns(const char* accept) {
    CscdStrVec exts = {0};
    windowAcceptExtensions(accept, &exts);
    CscdStr patterns = {0};
    for (size_t i = 0; i < exts.len; ++i) {
        if (i) cscdStrAppendChar(&patterns, ' ');
        cscdStrAppendC(&patterns, "*.");
        cscdStrAppend(&patterns, cscdStrSv(&exts.items[i]));
    }
    cscdStrVecFree(&exts);
    return patterns;
}

/* The desktop's file chooser through zenity (GNOME and most others) or kdialog (KDE), whichever is
 * installed; the window waits while it is open. */
bool platformPickFiles(CscdWindow* w, const char* accept, bool multiple) {
    const bool zenity = inPath("zenity"), kdialog = !zenity && inPath("kdialog");
    if (!zenity && !kdialog) return false;
    CscdStr patterns = acceptPatterns(accept);
    CscdStr filter = cscdStrDupC("--file-filter=Files | ");
    cscdStrAppend(&filter, cscdStrSv(&patterns));
    char* argv[10];
    int n = 0;
    if (zenity) {
        argv[n++] = (char*)"zenity";
        argv[n++] = (char*)"--file-selection";
        if (multiple) {
            argv[n++] = (char*)"--multiple";
            argv[n++] = (char*)"--separator=\n";
        }
        if (patterns.len) argv[n++] = (char*)cscdStrC(&filter);
    } else {
        argv[n++] = (char*)"kdialog";
        argv[n++] = (char*)"--getopenfilename";
        argv[n++] = (char*)".";
        argv[n++] = (char*)(patterns.len ? cscdStrC(&patterns) : "*");
        if (multiple) {
            argv[n++] = (char*)"--multiple";
            argv[n++] = (char*)"--separate-output";
        }
    }
    argv[n] = NULL;
    CscdStr out = {0};
    const int status = run(argv, &out);
    cscdStrFree(&filter);
    cscdStrFree(&patterns);
    CscdStrVec paths = {0};
    if (status == 0) {
        CscdSv rest = cscdStrSv(&out);
        while (rest.len) {
            size_t nl = cscdSvFindChar(rest, '\n', 0);
            const CscdSv line = cscdSvSub(rest, 0, nl == CSCD_NPOS ? rest.len : nl);
            rest = nl == CSCD_NPOS ? CSCD_SV("") : cscdSvSub(rest, nl + 1, rest.len - nl - 1);
            if (line.len) cscdVecPush(&paths, cscdStrDup(line));
        }
    }
    cscdStrFree(&out);
    windowFilesPicked(w, &paths);
    cscdStrVecFree(&paths);
    return status >= 0;
}

/* The same choosers, saving: zenity --save, kdialog --getsavefilename (both ask before replacing a
 * file). */
bool platformSaveFile(CscdWindow* w, const char* name, const char* accept) {
    const bool zenity = inPath("zenity"), kdialog = !zenity && inPath("kdialog");
    if (!zenity && !kdialog) return false;
    CscdStr patterns = acceptPatterns(accept);
    CscdStr filter = cscdStrDupC("--file-filter=Files | ");
    cscdStrAppend(&filter, cscdStrSv(&patterns));
    CscdStr filename = cscdStrDupC("--filename=");
    cscdStrAppendC(&filename, name);
    char* argv[10];
    int n = 0;
    if (zenity) {
        argv[n++] = (char*)"zenity";
        argv[n++] = (char*)"--file-selection";
        argv[n++] = (char*)"--save";
        argv[n++] = (char*)"--confirm-overwrite"; /* (older zenity; newer ones ask anyway) */
        if (name[0]) argv[n++] = (char*)cscdStrC(&filename);
        if (patterns.len) argv[n++] = (char*)cscdStrC(&filter);
    } else {
        argv[n++] = (char*)"kdialog";
        argv[n++] = (char*)"--getsavefilename";
        argv[n++] = (char*)(name[0] ? name : ".");
        argv[n++] = (char*)(patterns.len ? cscdStrC(&patterns) : "*");
    }
    argv[n] = NULL;
    CscdStr out = {0};
    const int status = run(argv, &out);
    cscdStrFree(&filter);
    cscdStrFree(&filename);
    cscdStrFree(&patterns);
    while (out.len && (out.ptr[out.len - 1] == '\n' || out.ptr[out.len - 1] == '\r')) out.ptr[--out.len] = 0;
    windowSaveChosen(w, status == 0 && out.len ? cscdStrC(&out) : NULL);
    cscdStrFree(&out);
    return status >= 0;
}
