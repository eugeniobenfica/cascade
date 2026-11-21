/* X11 window (Linux, the BSDs, XWayland): event loop, Xft.dpi scaling, mouse / keyboard / text input
 * (with input methods through XIM), cursors (themed through libXcursor when it is there), the
 * CLIPBOARD selection (text, pictures, copied files), files and text dropped from other apps (XDND),
 * and frame presentation through a presenter. Only libX11 is linked. */
/* (windowLinux.c chooses between this and Wayland: the platform's calls under X11's names) */
#define windowCreateNative x11WindowCreateNative
#define windowDestroyNative x11WindowDestroyNative
#define windowSurface x11WindowSurface
#define windowShowTitle x11WindowShowTitle
#define windowWake x11WindowWake
#define windowPlaceInputMethod x11WindowPlaceInputMethod
#define cscdWindowShow x11WindowShow
#define cscdWindowResize x11WindowResize
#define cscdWindowMaximize x11WindowMaximize
#define cscdWindowSetFullscreen x11WindowSetFullscreen
#define cscdWindowClose x11WindowClose
#define cscdWindowPump x11WindowPump
#include "platform/common/platformInternal.h"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define XNONE 0L

enum AtomId {
    WM_PROTOCOLS, WM_DELETE_WINDOW, NET_WM_PING, NET_WM_NAME, NET_WM_PID, NET_WM_STATE, NET_WM_STATE_FULLSCREEN,
    NET_WM_STATE_MAXIMIZED_VERT, NET_WM_STATE_MAXIMIZED_HORZ, GTK_THEME_VARIANT, UTF8_STRING, CLIPBOARD, TARGETS, TEXT,
    TEXT_PLAIN_UTF8, TEXT_PLAIN, IMAGE_PNG, URI_LIST, GNOME_COPIED_FILES, INCR, CASCADE_SELECTION, XdndAware, XdndEnter,
    XdndPosition, XdndStatus, XdndLeave, XdndDrop, XdndFinished, XdndSelection, XdndTypeList, XdndActionCopy, kAtomCount
};

static char* kAtomNames[kAtomCount] = {
    "WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_PING", "_NET_WM_NAME", "_NET_WM_PID", "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN",
    "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ", "_GTK_THEME_VARIANT", "UTF8_STRING", "CLIPBOARD", "TARGETS", "TEXT",
    "text/plain;charset=utf-8", "text/plain", "image/png", "text/uri-list", "x-special/gnome-copied-files", "INCR", "CASCADE_SELECTION",
    "XdndAware", "XdndEnter", "XdndPosition", "XdndStatus", "XdndLeave", "XdndDrop", "XdndFinished", "XdndSelection", "XdndTypeList",
    "XdndActionCopy",
};

/* X errors (a drag source that went away, a GLX visual that does not match) are not fatal here. */
static int ignoreErrors(Display* d, XErrorEvent* e) { (void)d; (void)e; return 0; }

/* Device pixels per CSS pixel: the desktop's Xft.dpi (what GNOME, KDE and xrdb set), else 1. */
static float screenScale(Display* dpy) {
    float dpi = 0;
    char* rms = XResourceManagerString(dpy);
    if (rms) {
        XrmInitialize();
        XrmDatabase db = XrmGetStringDatabase(rms);
        if (db) {
            char* type = NULL;
            XrmValue v;
            memset(&v, 0, sizeof v);
            if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &v) && v.addr) dpi = (float)atof(v.addr);
            XrmDestroyDatabase(db);
        }
    }
    return dpi > 0 ? dpi / 96.f : 1.f;
}

static CscdModifiers mods(unsigned state) {
    CscdModifiers m;
    m.shift = (state & ShiftMask) != 0;
    m.ctrl = (state & ControlMask) != 0;
    m.alt = (state & Mod1Mask) != 0;
    m.meta = (state & Mod4Mask) != 0;
    return m;
}

static int heldButtons(unsigned state) {
    return ((state & Button1Mask) ? 1 : 0) | ((state & Button3Mask) ? 2 : 0) | ((state & Button2Mask) ? 4 : 0);
}

/* The key from its unshifted keysym, like a Windows virtual key. */
static CscdKey mapKey(KeySym ks, char name[16]) {
#define K(n, key) { strcpy(name, n); return key; }
    switch (ks) {
    case XK_BackSpace: K("Backspace", CSCD_KEY_BACKSPACE)
    case XK_Tab: case XK_ISO_Left_Tab: K("Tab", CSCD_KEY_TAB)
    case XK_Return: case XK_KP_Enter: K("Enter", CSCD_KEY_ENTER)
    case XK_Escape: K("Escape", CSCD_KEY_ESCAPE)
    case XK_space: K(" ", CSCD_KEY_SPACE)
    case XK_Delete: case XK_KP_Delete: K("Delete", CSCD_KEY_DELETE)
    case XK_Left: case XK_KP_Left: K("ArrowLeft", CSCD_KEY_LEFT)
    case XK_Right: case XK_KP_Right: K("ArrowRight", CSCD_KEY_RIGHT)
    case XK_Up: case XK_KP_Up: K("ArrowUp", CSCD_KEY_UP)
    case XK_Down: case XK_KP_Down: K("ArrowDown", CSCD_KEY_DOWN)
    case XK_Home: case XK_KP_Home: K("Home", CSCD_KEY_HOME)
    case XK_End: case XK_KP_End: K("End", CSCD_KEY_END)
    case XK_Prior: case XK_KP_Prior: K("PageUp", CSCD_KEY_PAGE_UP)
    case XK_Next: case XK_KP_Next: K("PageDown", CSCD_KEY_PAGE_DOWN)
    case XK_Insert: case XK_KP_Insert: K("Insert", CSCD_KEY_INSERT)
    case XK_Shift_L: case XK_Shift_R: K("Shift", CSCD_KEY_SHIFT)
    case XK_Control_L: case XK_Control_R: K("Control", CSCD_KEY_CONTROL)
    case XK_Alt_L: case XK_Alt_R: case XK_Meta_L: case XK_Meta_R: K("Alt", CSCD_KEY_ALT)
    case XK_Super_L: case XK_Super_R: K("Meta", CSCD_KEY_META)
    case XK_Menu: K("ContextMenu", CSCD_KEY_UNKNOWN)
    case XK_a: K("a", CSCD_KEY_A)
    case XK_c: K("c", CSCD_KEY_C)
    case XK_v: K("v", CSCD_KEY_V)
    case XK_x: K("x", CSCD_KEY_X)
    case XK_z: K("z", CSCD_KEY_Z)
    case XK_y: K("y", CSCD_KEY_Y)
    default: break;
    }
#undef K
    if (ks >= XK_F1 && ks <= XK_F12) {
        snprintf(name, 16, "F%d", (int)(ks - XK_F1 + 1));
        return (CscdKey)((int)CSCD_KEY_F1 + (int)(ks - XK_F1));
    }
    /* digits, letters and punctuation: the character the key types without Shift */
    if (ks >= 0x20 && ks < 0x7F) {
        name[0] = (char)ks;
        name[1] = 0;
        return CSCD_KEY_CHARACTER;
    }
    strcpy(name, "Unidentified");
    return CSCD_KEY_UNKNOWN;
}

static const char* cursorName(CscdCursor c) {
    switch (c) {
    case CSCD_CURSOR_POINTER: return "pointer";
    case CSCD_CURSOR_TEXT: return "text";
    case CSCD_CURSOR_MOVE: return "move";
    case CSCD_CURSOR_GRAB: return "grab";
    case CSCD_CURSOR_GRABBING: return "grabbing";
    case CSCD_CURSOR_NOT_ALLOWED: return "not-allowed";
    case CSCD_CURSOR_COPY: return "copy";
    case CSCD_CURSOR_CROSSHAIR: return "crosshair";
    case CSCD_CURSOR_WAIT: return "wait";
    case CSCD_CURSOR_PROGRESS: return "progress";
    case CSCD_CURSOR_HELP: return "help";
    case CSCD_CURSOR_EW_RESIZE: return "ew-resize";
    case CSCD_CURSOR_NS_RESIZE: return "ns-resize";
    case CSCD_CURSOR_NESW_RESIZE: return "nesw-resize";
    case CSCD_CURSOR_NWSE_RESIZE: return "nwse-resize";
    case CSCD_CURSOR_COL_RESIZE: return "col-resize";
    case CSCD_CURSOR_ROW_RESIZE: return "row-resize";
    default: return "default";
    }
}

static unsigned cursorShape(CscdCursor c) {
    switch (c) {
    case CSCD_CURSOR_POINTER: return XC_hand2;
    case CSCD_CURSOR_TEXT: return XC_xterm;
    case CSCD_CURSOR_MOVE: case CSCD_CURSOR_GRAB: case CSCD_CURSOR_GRABBING: return XC_fleur;
    case CSCD_CURSOR_NOT_ALLOWED: return XC_X_cursor;
    case CSCD_CURSOR_CROSSHAIR: return XC_crosshair;
    case CSCD_CURSOR_WAIT: case CSCD_CURSOR_PROGRESS: return XC_watch;
    case CSCD_CURSOR_HELP: return XC_question_arrow;
    case CSCD_CURSOR_EW_RESIZE: case CSCD_CURSOR_COL_RESIZE: return XC_sb_h_double_arrow;
    case CSCD_CURSOR_NS_RESIZE: case CSCD_CURSOR_ROW_RESIZE: return XC_sb_v_double_arrow;
    case CSCD_CURSOR_NESW_RESIZE: return XC_bottom_left_corner;
    case CSCD_CURSOR_NWSE_RESIZE: return XC_bottom_right_corner;
    default: return XC_left_ptr;
    }
}

static void latin1ToUtf8(const char* s, size_t n, CscdStr* out) {
    for (size_t i = 0; i < n; ++i) cscdUtf8Append(out, (unsigned char)s[i]);
}

/* file:// lines of a text/uri-list (or of GNOME's copied-files list) as paths, appended to files. */
static void filePaths(CscdSv list, CscdStrVec* files) { clipFilePaths(list, files); }

static void removeCR(CscdStr* s) {
    size_t k = 0;
    for (size_t i = 0; i < s->len; ++i) if (s->ptr[i] != '\r') s->ptr[k++] = s->ptr[i];
    cscdStrTruncate(s, k);
}

static void programName(char buf[256]) {
    buf[0] = 0;
    FILE* f = fopen("/proc/self/comm", "r");
    if (f) {
        if (fgets(buf, 256, f)) buf[strcspn(buf, "\n")] = 0;
        fclose(f);
    }
    if (!buf[0]) strcpy(buf, "cascade");
}


#define CURSOR_KINDS 64

struct CscdWindowNative {
    Display* dpy;
    Window win;
    Window root;
    Colormap colormap;
    CscdXlibWindow surface;
    Atom atoms[kAtomCount];
    XIM im;
    XIC ic;
    bool spotIm; /* the input method draws the composition at a spot we give it */
    int wake[2];
    bool closeRequested;
    ClickCounter clicks;
    Time lastTime;
    /* cursors, made on first use */
    void* xcursorLib;
    Cursor (*loadCursor)(Display*, const char*);
    Cursor cursors[CURSOR_KINDS];
    bool cursorMade[CURSOR_KINDS];
    bool cursorShown;
    CscdCursor shown;
    /* the CLIPBOARD selection while this window owns it */
    ClipData clip;
    ClipData dragData; /* (what a drag out carries, as the XdndSelection) */
    /* a drag from another app (XDND) */
    Window dndSource;
    int dndVersion;
    CSCD_VEC(Atom) dndTypes;
    bool dndEntered;
    float dndX, dndY;
};

static Atom atom(const CscdWindowNative* n, enum AtomId a) { return n->atoms[a]; }

static void wakeUp(CscdWindowNative* n) {
    if (n->wake[1] >= 0) {
        char c = 1;
        (void)!write(n->wake[1], &c, 1);
    }
}

static void setTitle(CscdWindowNative* n, const char* title) {
    XStoreName(n->dpy, n->win, title);
    XChangeProperty(n->dpy, n->win, atom(n, NET_WM_NAME), atom(n, UTF8_STRING), 8, PropModeReplace, (const unsigned char*)title, (int)strlen(title));
}

static void sendWmState(CscdWindowNative* n, bool add, Atom a, Atom b) {
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = n->win;
    ev.xclient.message_type = atom(n, NET_WM_STATE);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = add ? 1 : 0;
    ev.xclient.data.l[1] = (long)a;
    ev.xclient.data.l[2] = (long)b;
    ev.xclient.data.l[3] = 1; /* from an application */
    XSendEvent(n->dpy, n->root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(n->dpy);
}

static void sendDnd(CscdWindowNative* n, Window to, enum AtomId type, long l1, long l2, long l3, long l4) {
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = to;
    ev.xclient.message_type = atom(n, type);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)n->win;
    ev.xclient.data.l[1] = l1;
    ev.xclient.data.l[2] = l2;
    ev.xclient.data.l[3] = l3;
    ev.xclient.data.l[4] = l4;
    XSendEvent(n->dpy, to, False, NoEventMask, &ev);
    XFlush(n->dpy);
}

static Cursor cursorFor(CscdWindowNative* n, CscdCursor c) {
    const int k = (int)c & (CURSOR_KINDS - 1);
    if (n->cursorMade[k]) return n->cursors[k];
    Cursor xc = 0;
    if (c == CSCD_CURSOR_NONE) {
        char bits[1] = {0};
        Pixmap blank = XCreateBitmapFromData(n->dpy, n->win, bits, 1, 1);
        XColor black;
        memset(&black, 0, sizeof black);
        xc = XCreatePixmapCursor(n->dpy, blank, blank, &black, &black, 0, 0);
        XFreePixmap(n->dpy, blank);
    } else {
        if (!n->xcursorLib) {
            static const char* const libs[] = {"libXcursor.so.1", "libXcursor.so", NULL};
            n->xcursorLib = platformOpenLibrary(libs);
            *(void**)&n->loadCursor = platformSymbol(n->xcursorLib, "XcursorLibraryLoadCursor");
        }
        if (n->loadCursor) xc = n->loadCursor(n->dpy, cursorName(c));
        if (!xc) xc = XCreateFontCursor(n->dpy, cursorShape(c));
    }
    n->cursors[k] = xc;
    n->cursorMade[k] = true;
    return xc;
}

/* waits up to 50 ms for the connection, until deadline (seconds); false once it has passed */
static bool waitForEvents(CscdWindowNative* n, double deadline) {
    const double left = deadline - cscdNowSeconds();
    if (left <= 0) return false;
    struct pollfd fd = {ConnectionNumber(n->dpy), POLLIN, 0};
    int ms = (int)(left * 1000);
    poll(&fd, 1, ms < 50 ? ms : 50);
    return true;
}

static bool takeProperty(CscdWindowNative* n, Atom prop, CscdStr* out, Atom* typeOut) {
    Atom type = XNONE;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = NULL;
    if (XGetWindowProperty(n->dpy, n->win, prop, 0, LONG_MAX / 4, True, AnyPropertyType, &type, &format, &count, &after, &data) != Success) return false;
    const size_t unit = format == 8 ? 1 : format == 16 ? sizeof(short) : sizeof(long);
    if (data) {
        cscdStrAppend(out, cscdSvMake((const char*)data, count * unit));
        XFree(data);
    }
    if (typeOut) *typeOut = type;
    return true;
}

/* Asks the owner of a selection for its contents as target, waiting up to a second and a half (other
 * events stay queued). Large contents come in pieces (INCR). */
static bool readSelection(CscdWindowNative* n, Atom selection, Atom target, CscdStr* out) {
    Display* dpy = n->dpy;
    const Atom prop = atom(n, CASCADE_SELECTION);
    XDeleteProperty(dpy, n->win, prop);
    XConvertSelection(dpy, selection, target, prop, n->win, n->lastTime);
    XFlush(dpy);
    const double deadline = cscdNowSeconds() + 1.5;
    XEvent ev;
    for (;;) {
        if (XCheckTypedWindowEvent(dpy, n->win, SelectionNotify, &ev)) {
            if (ev.xselection.selection != selection) continue;
            if (ev.xselection.property == XNONE) return false;
            break;
        }
        if (!waitForEvents(n, deadline)) return false;
    }
    cscdStrClear(out);
    Atom type = XNONE;
    if (!takeProperty(n, prop, out, &type)) return false;
    if (type != atom(n, INCR)) return true;
    /* INCR: each new value of the property is the next piece, an empty one ends it */
    cscdStrClear(out);
    for (;;) {
        if (XCheckTypedWindowEvent(dpy, n->win, PropertyNotify, &ev)) {
            if (ev.xproperty.atom != prop || ev.xproperty.state != PropertyNewValue) continue;
            const size_t before = out->len;
            if (!takeProperty(n, prop, out, NULL)) return false;
            if (out->len == before) return true;
            continue;
        }
        if (!waitForEvents(n, deadline + 5)) return false;
    }
}

static bool ownsClipboard(const CscdWindowNative* n) { return XGetSelectionOwner(n->dpy, atom(n, CLIPBOARD)) == n->win; }

static void takeClipboard(CscdWindowNative* n) {
    XSetSelectionOwner(n->dpy, atom(n, CLIPBOARD), n->win, n->lastTime);
    XFlush(n->dpy);
}

static void answerSelectionRequest(CscdWindowNative* n, const XSelectionRequestEvent* r) {
    Display* dpy = n->dpy;
    XEvent reply;
    memset(&reply, 0, sizeof reply);
    reply.xselection.type = SelectionNotify;
    reply.xselection.requestor = r->requestor;
    reply.xselection.selection = r->selection;
    reply.xselection.target = r->target;
    reply.xselection.time = r->time;
    reply.xselection.property = XNONE;
    const Atom prop = r->property != XNONE ? r->property : r->target;
    /* (the clipboard's, or a drag's we are the source of) */
    const ClipData* from = r->selection == atom(n, CLIPBOARD) ? &n->clip : r->selection == atom(n, XdndSelection) ? &n->dragData : NULL;
    if (from && from->len) {
        const bool text = clipDataFind(from, "UTF8_STRING") != NULL;
        /* the biggest property one request can set (BIG-REQUESTS makes it many megabytes) */
        long words = XExtendedMaxRequestSize(dpy);
        if (!words) words = XMaxRequestSize(dpy);
        const size_t limit = (size_t)words * 4 - 64;
        if (r->target == atom(n, TARGETS)) {
            CSCD_VEC(Atom) list = {0};
            cscdVecPush(&list, atom(n, TARGETS));
            for (size_t i = 0; i < from->len; ++i) cscdVecPush(&list, XInternAtom(dpy, cscdStrC(&from->items[i].mime), False));
            if (text) {
                cscdVecPush(&list, XA_STRING);
                cscdVecPush(&list, atom(n, TEXT));
            }
            XChangeProperty(dpy, r->requestor, prop, XA_ATOM, 32, PropModeReplace, (const unsigned char*)list.items, (int)list.len);
            cscdVecFree(&list);
            reply.xselection.property = prop;
        } else {
            /* (TEXT and STRING: the UTF-8 text, as before) */
            char* name = XGetAtomName(dpy, r->target);
            const bool asText = r->target == atom(n, TEXT) || r->target == XA_STRING;
            const ClipFormat* fmt = asText ? clipDataFind(from, "UTF8_STRING") : name ? clipDataFind(from, name) : NULL;
            if (name) XFree(name);
            if (fmt && fmt->data.len <= limit) {
                const Atom type = r->target == atom(n, TEXT) ? atom(n, UTF8_STRING) : r->target;
                XChangeProperty(dpy, r->requestor, prop, type, 8, PropModeReplace, fmt->data.len ? fmt->data.items : (const unsigned char*)"",
                                (int)fmt->data.len);
                reply.xselection.property = prop;
            }
        }
    }
    XSendEvent(dpy, r->requestor, False, NoEventMask, &reply);
    XFlush(dpy);
}

static bool dndHas(const CscdWindowNative* n, Atom a) {
    for (size_t i = 0; i < n->dndTypes.len; ++i) if (n->dndTypes.items[i] == a) return true;
    return false;
}

/* What a drag from another app carries: its files and its text. */
static CscdDataTransfer* readDragData(CscdWindowNative* n) {
    CscdDataTransfer* dt = cscdDataTransferNew();
    cscdStrSetC(&dt->effectAllowed, "copy");
    CscdStr s = {0};
    if (dndHas(n, atom(n, URI_LIST)) && readSelection(n, atom(n, XdndSelection), atom(n, URI_LIST), &s)) filePaths(cscdStrSv(&s), &dt->files);
    const enum AtomId textTypes[] = {UTF8_STRING, TEXT_PLAIN_UTF8, TEXT_PLAIN};
    for (size_t i = 0; i < CSCD_COUNT(textTypes); ++i) {
        if (!dndHas(n, atom(n, textTypes[i]))) continue;
        if (readSelection(n, atom(n, XdndSelection), atom(n, textTypes[i]), &s)) {
            removeCR(&s);
            cscdDataTransferSetData(dt, CSCD_SV("text/plain"), cscdStrSv(&s));
        }
        break;
    }
    cscdStrFree(&s);
    return dt;
}

static void placeIm(CscdWindowNative* n, CscdRect caret, float dpr) {
    if (!n->ic || !n->spotIm || (cscdRectEmpty(caret) && caret.h <= 0)) return;
    XPoint spot = {(short)(caret.x * dpr), (short)(cscdRectBottom(caret) * dpr)};
    XVaNestedList attr = XVaCreateNestedList(0, XNSpotLocation, &spot, NULL);
    XSetICValues(n->ic, XNPreeditAttributes, attr, NULL);
    XFree(attr);
}

void windowDestroyNative(CscdWindow* w) {
    CscdWindowNative* n = w->native;
    if (n->dpy) {
        for (int i = 0; i < CURSOR_KINDS; ++i) if (n->cursorMade[i] && n->cursors[i]) XFreeCursor(n->dpy, n->cursors[i]);
        if (n->ic) XDestroyIC(n->ic);
        if (n->im) XCloseIM(n->im);
        if (w->hwnd) XDestroyWindow(n->dpy, n->win);
        if (n->colormap) XFreeColormap(n->dpy, n->colormap);
        XCloseDisplay(n->dpy);
    }
    for (int i = 0; i < 2; ++i) if (n->wake[i] >= 0) close(n->wake[i]);
    platformCloseLibrary(n->xcursorLib);
    clipDataFree(&n->clip);
    clipDataFree(&n->dragData);
    cscdVecFree(&n->dndTypes);
    cscdFree(n);
    w->native = NULL;
}

/* ---- clipboard -------------------------------------------------------------------------------------- */

static void clipSetText(void* user, CscdSv text) {
    CscdWindowNative* n = ((CscdWindow*)user)->native;
    clipDataSetText(&n->clip, text);
    takeClipboard(n);
}

static void clipSetHtml(void* user, CscdSv html, CscdSv text) {
    CscdWindowNative* n = ((CscdWindow*)user)->native;
    clipDataSetHtml(&n->clip, html, text);
    takeClipboard(n);
}

static void clipSetImage(void* user, const CscdImage* img) {
    if (img->width <= 0 || img->height <= 0) return;
    CscdWindowNative* n = ((CscdWindow*)user)->native;
    clipDataSetImage(&n->clip, img);
    takeClipboard(n);
}

static void clipSetFiles(void* user, const CscdStrVec* paths) {
    CscdWindowNative* n = ((CscdWindow*)user)->native;
    clipDataSetFiles(&n->clip, paths);
    takeClipboard(n);
}

/* the clipboard's mime: ours when we own it, else asked of its owner (false: it has none) */
static bool clipRead(CscdWindowNative* n, const char* mime, Atom target, CscdStr* out) {
    if (ownsClipboard(n)) {
        const ClipFormat* f = clipDataFind(&n->clip, mime);
        if (!f) return false;
        cscdStrAppend(out, cscdSvMake((const char*)f->data.items, f->data.len));
        return true;
    }
    return readSelection(n, atom(n, CLIPBOARD), target, out);
}

static void clipGetText(void* user, CscdStr* out) {
    CscdWindowNative* n = ((CscdWindow*)user)->native;
    CscdStr s = {0};
    if (!clipRead(n, "UTF8_STRING", atom(n, UTF8_STRING), &s) && !ownsClipboard(n)) {
        CscdStr l = {0};
        cscdStrClear(&s);
        if (readSelection(n, atom(n, CLIPBOARD), XA_STRING, &l)) latin1ToUtf8(l.ptr, l.len, &s);
        cscdStrFree(&l);
    }
    removeCR(&s);
    cscdStrAppend(out, cscdStrSv(&s));
    cscdStrFree(&s);
}

/* text/html: UTF-8, or UTF-16 after a byte order mark (as some browsers put it) */
static void clipGetHtml(void* user, CscdStr* out) {
    CscdWindowNative* n = ((CscdWindow*)user)->native;
    CscdStr s = {0};
    if (clipRead(n, "text/html", XInternAtom(n->dpy, "text/html", False), &s)) {
        if (s.len >= 2 && (unsigned char)s.ptr[0] == 0xFF && (unsigned char)s.ptr[1] == 0xFE) {
            CscdStr u = {0};
            for (size_t i = 2; i + 1 < s.len; i += 2) {
                uint32_t c = (uint8_t)s.ptr[i] | (uint32_t)(uint8_t)s.ptr[i + 1] << 8;
                if (c >= 0xD800 && c < 0xDC00 && i + 3 < s.len) {
                    const uint32_t lo = (uint8_t)s.ptr[i + 2] | (uint32_t)(uint8_t)s.ptr[i + 3] << 8;
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        i += 2;
                    }
                }
                cscdUtf8Append(&u, c);
            }
            cscdStrFree(&s);
            s = u;
        } else if (s.len >= 3 && (unsigned char)s.ptr[0] == 0xEF && (unsigned char)s.ptr[1] == 0xBB && (unsigned char)s.ptr[2] == 0xBF) {
            memmove(s.ptr, s.ptr + 3, s.len - 3);
            cscdStrTruncate(&s, s.len - 3);
        }
        removeCR(&s);
        cscdStrAppend(out, cscdStrSv(&s));
    }
    cscdStrFree(&s);
}

static bool clipGetImage(void* user, CscdImage* out) {
    CscdWindowNative* n = ((CscdWindow*)user)->native;
    CscdStr s = {0};
    bool ok = false;
    if (clipRead(n, "image/png", atom(n, IMAGE_PNG), &s)) {
        CscdImage* img = cscdDecodeImage((const uint8_t*)s.ptr, s.len);
        if (img && img->width > 0 && img->height > 0) {
            cscdImageInit(out, img->width, img->height);
            memcpy(out->pixels, img->pixels, (size_t)img->width * (size_t)img->height * 4);
            ok = true;
        }
        cscdRelease(img);
    }
    cscdStrFree(&s);
    return ok;
}

static void clipGetFiles(void* user, CscdStrVec* out) {
    CscdWindowNative* n = ((CscdWindow*)user)->native;
    CscdStr list = {0};
    if (clipRead(n, "text/uri-list", atom(n, URI_LIST), &list)) filePaths(cscdStrSv(&list), out);
    else if (!ownsClipboard(n) && readSelection(n, atom(n, CLIPBOARD), atom(n, GNOME_COPIED_FILES), &list)) filePaths(cscdStrSv(&list), out); /* "copy" or "cut" first */
    cscdStrFree(&list);
}

/* ---- drags out to other apps: XDND as the source -----------------------------------------------------------------
 * The pointer grabbed until the button goes up; the window under it that speaks XDND (XdndAware) told the drag comes
 * (Enter, the types), where it is (Position) and that it was dropped (Drop); it reads the data as the XdndSelection,
 * answered from what the drag carries (its text, its files). The page hears how it ended: copied, or not. */

/* the window under the root point (x, y) that takes drags (XdndAware: its version), 0 for none, or ours */
static Window xdndTarget(CscdWindowNative* n, int x, int y, int* version) {
    Window at = n->root, child = XNONE;
    int cx, cy;
    for (int depth = 0; depth < 32; ++depth) {
        if (at != n->root) {
            Atom type = XNONE;
            int format = 0;
            unsigned long count = 0, after = 0;
            unsigned char* data = NULL;
            if (XGetWindowProperty(n->dpy, at, atom(n, XdndAware), 0, 1, False, XA_ATOM, &type, &format, &count, &after, &data) == Success &&
                type == XA_ATOM && count == 1 && data) {
                *version = (int)*(Atom*)data;
                XFree(data);
                return at == n->win ? XNONE : at;
            }
            if (data) XFree(data);
        }
        if (!XTranslateCoordinates(n->dpy, n->root, at, x, y, &cx, &cy, &child) || child == XNONE) break;
        at = child;
    }
    return XNONE;
}

static bool x11DragOut(void* user, const CscdDataTransfer* dt) {
    CscdWindow* w = (CscdWindow*)user;
    CscdWindowNative* n = w->native;
    if (!n || !n->dpy || !w->hwnd) return false;
    const CscdSv text = cscdDataTransferGetData(dt, CSCD_SV("text/plain"));
    if (dt->files.len) clipDataSetFiles(&n->dragData, &dt->files);
    else if (text.len) clipDataSetText(&n->dragData, text);
    else return false;
    Display* dpy = n->dpy;
    XSetSelectionOwner(dpy, atom(n, XdndSelection), n->win, n->lastTime);
    /* the types it carries: three in the Enter message, all of them in XdndTypeList */
    CSCD_VEC(Atom) types = {0};
    for (size_t i = 0; i < n->dragData.len; ++i) cscdVecPush(&types, XInternAtom(dpy, cscdStrC(&n->dragData.items[i].mime), False));
    XChangeProperty(dpy, n->win, atom(n, XdndTypeList), XA_ATOM, 32, PropModeReplace, (const unsigned char*)types.items, (int)types.len);
    if (XGrabPointer(dpy, n->win, False, ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, XNONE, XNONE, CurrentTime) != GrabSuccess) {
        cscdVecFree(&types);
        clipDataClear(&n->dragData);
        return false;
    }
    Window target = XNONE;
    int version = 0;
    bool accepted = false, over = false, dropped = false;
    Time time = n->lastTime;
    while (!over) {
        XEvent e;
        XNextEvent(dpy, &e);
        switch (e.type) {
        case MotionNotify: {
            time = e.xmotion.time;
            int v = 0;
            const Window t = xdndTarget(n, e.xmotion.x_root, e.xmotion.y_root, &v);
            if (t != target) {
                if (target) sendDnd(n, target, XdndLeave, 0, 0, 0, 0);
                target = t;
                version = v < 5 ? v : 5;
                accepted = false;
                if (target)
                    sendDnd(n, target, XdndEnter, ((long)version << 24) | (types.len > 3 ? 1 : 0), types.len > 0 ? (long)types.items[0] : 0,
                            types.len > 1 ? (long)types.items[1] : 0, types.len > 2 ? (long)types.items[2] : 0);
            }
            if (target)
                sendDnd(n, target, XdndPosition, 0, ((long)e.xmotion.x_root << 16) | (e.xmotion.y_root & 0xFFFF), (long)time,
                        (long)atom(n, XdndActionCopy));
            break;
        }
        case ButtonRelease:
            time = e.xbutton.time;
            if (target && accepted) {
                sendDnd(n, target, XdndDrop, 0, (long)time, 0, 0);
                dropped = true;
            } else if (target) {
                sendDnd(n, target, XdndLeave, 0, 0, 0, 0);
            }
            over = true;
            break;
        case KeyPress:
            if (XLookupKeysym(&e.xkey, 0) == XK_Escape) {
                if (target) sendDnd(n, target, XdndLeave, 0, 0, 0, 0);
                over = true;
            }
            break;
        case ClientMessage:
            if (e.xclient.message_type == atom(n, XdndStatus) && (Window)e.xclient.data.l[0] == target) accepted = (e.xclient.data.l[1] & 1) != 0;
            break;
        case SelectionRequest:
            answerSelectionRequest(n, &e.xselectionrequest);
            break;
        case Expose:
            w->dirty = true;
            break;
        default:
            break;
        }
    }
    XUngrabPointer(dpy, CurrentTime);
    bool finished = false;
    if (dropped) {
        /* (it reads the data, then says it is done: a few seconds at most) */
        const double deadline = cscdNowSeconds() + 5;
        while (!finished && waitForEvents(n, deadline)) {
            while (XPending(dpy)) {
                XEvent e;
                XNextEvent(dpy, &e);
                if (e.type == SelectionRequest) answerSelectionRequest(n, &e.xselectionrequest);
                else if (e.type == ClientMessage && e.xclient.message_type == atom(n, XdndFinished) && (Window)e.xclient.data.l[0] == target) {
                    finished = version < 5 || (e.xclient.data.l[1] & 1);
                    if (!finished) break;
                } else if (e.type == Expose) w->dirty = true;
            }
            if (finished) break;
        }
    }
    cscdVecFree(&types);
    cscdViewDragOutEnded(w->view, finished ? CSCD_SV("copy") : CSCD_SV("none"));
    return true;
}

/* ---- the window ------------------------------------------------------------------------------------- */

void windowCreateNative(CscdWindow* w, const CscdWindowOptions* opts) {
    CscdWindowNative* n = CSCD_NEW(CscdWindowNative);
    w->native = n;
    n->wake[0] = n->wake[1] = -1;
    n->lastTime = CurrentTime;
    n->shown = CSCD_CURSOR_DEFAULT;
    /* input methods hand text over in the locale's encoding: take the user's (UTF-8) one */
    const char* ctype = setlocale(LC_CTYPE, NULL);
    if (!ctype || !strcmp(ctype, "C") || !strcmp(ctype, "POSIX")) {
        if (!setlocale(LC_CTYPE, "") || !XSupportsLocale()) setlocale(LC_CTYPE, "C.UTF-8");
    }
    XSetLocaleModifiers("");
    n->dpy = XOpenDisplay(NULL);
    if (!n->dpy) {
        fprintf(stderr, "cascade: cannot open the X display (is DISPLAY set?)\n");
        return;
    }
    XSetErrorHandler(ignoreErrors);
    Display* dpy = n->dpy;
    w->display = dpy;
    XInternAtoms(dpy, kAtomNames, kAtomCount, False, n->atoms);
    const int screen = DefaultScreen(dpy);
    n->root = RootWindow(dpy, screen);
    w->dpr = screenScale(dpy);

    /* OpenGL needs the window made with a visual it can draw into */
    Visual* visual = DefaultVisual(dpy, screen);
    int depth = DefaultDepth(dpy, screen);
    if (opts->backend != CSCD_BACKEND_SOFTWARE) {
        void* v = platformChooseGLVisual(dpy, screen, &depth);
        if (v) visual = (Visual*)v;
    }
    n->colormap = XCreateColormap(dpy, n->root, visual, AllocNone);
    const long events = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                        EnterWindowMask | LeaveWindowMask | FocusChangeMask | PropertyChangeMask;
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof swa);
    swa.colormap = n->colormap;
    swa.background_pixmap = XNONE;
    swa.border_pixel = 0;
    swa.bit_gravity = NorthWestGravity;
    swa.event_mask = events;
    w->clientW = cscdMaxi(1, (int)(opts->width * w->dpr));
    w->clientH = cscdMaxi(1, (int)(opts->height * w->dpr));
    n->win = XCreateWindow(dpy, n->root, 0, 0, (unsigned)w->clientW, (unsigned)w->clientH, 0, depth, InputOutput, visual,
                           CWColormap | CWBackPixmap | CWBorderPixel | CWBitGravity | CWEventMask, &swa);
    if (!n->win) return;
    w->hwnd = (void*)(uintptr_t)n->win;
    n->surface.display = dpy;
    n->surface.window = n->win;

    Atom protocols[] = {atom(n, WM_DELETE_WINDOW), atom(n, NET_WM_PING)};
    XSetWMProtocols(dpy, n->win, protocols, 2);
    setTitle(n, opts->title && opts->title[0] ? opts->title : "cascade");
    XSizeHints* hints = XAllocSizeHints();
    if (hints) {
        hints->flags = PMinSize;
        hints->min_width = (int)(opts->minWidth * w->dpr);
        hints->min_height = (int)(opts->minHeight * w->dpr);
        if (!opts->resizable) {
            hints->flags |= PMaxSize;
            hints->min_width = hints->max_width = w->clientW;
            hints->min_height = hints->max_height = w->clientH;
        }
        XSetWMNormalHints(dpy, n->win, hints);
        XFree(hints);
    }
    /* the program's name groups its windows in task bars */
    char name[256];
    programName(name);
    XClassHint cls = {name, name};
    XSetClassHint(dpy, n->win, &cls);
    long pid = (long)getpid();
    XChangeProperty(dpy, n->win, atom(n, NET_WM_PID), XA_CARDINAL, 32, PropModeReplace, (const unsigned char*)&pid, 1);
    if (opts->darkTitleBar) {
        const char dark[] = "dark";
        XChangeProperty(dpy, n->win, atom(n, GTK_THEME_VARIANT), atom(n, UTF8_STRING), 8, PropModeReplace, (const unsigned char*)dark, 4);
    }
    /* files and text dragged in from other apps (XDND version 5) */
    Atom version = 5;
    XChangeProperty(dpy, n->win, atom(n, XdndAware), XA_ATOM, 32, PropModeReplace, (const unsigned char*)&version, 1);
    /* held keys repeat as presses only, without a release before each */
    XkbSetDetectableAutoRepeat(dpy, True, NULL);

    /* input methods: the composition at the caret when the method can, else in its own window */
    if ((n->im = XOpenIM(dpy, NULL, NULL, NULL))) {
        XIMStyles* styles = NULL;
        bool spot = false;
        if (!XGetIMValues(n->im, XNQueryInputStyle, &styles, NULL) && styles) {
            for (int i = 0; i < styles->count_styles; ++i)
                if (styles->supported_styles[i] == (XIMPreeditPosition | XIMStatusNothing)) spot = true;
            XFree(styles);
        }
        if (spot) {
            XPoint origin = {0, 0};
            XVaNestedList attr = XVaCreateNestedList(0, XNSpotLocation, &origin, NULL);
            n->ic = XCreateIC(n->im, XNInputStyle, XIMPreeditPosition | XIMStatusNothing, XNClientWindow, n->win, XNFocusWindow, n->win,
                              XNPreeditAttributes, attr, NULL);
            XFree(attr);
            n->spotIm = n->ic != NULL;
        }
        if (!n->ic) n->ic = XCreateIC(n->im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, n->win, XNFocusWindow, n->win, NULL);
        if (n->ic) {
            long filter = 0;
            XGetICValues(n->ic, XNFilterEvents, &filter, NULL);
            XSelectInput(dpy, n->win, events | filter);
        }
    }
    if (pipe(n->wake) == 0) {
        for (int i = 0; i < 2; ++i) {
            fcntl(n->wake[i], F_SETFL, fcntl(n->wake[i], F_GETFL) | O_NONBLOCK);
            fcntl(n->wake[i], F_SETFD, FD_CLOEXEC);
        }
    } else n->wake[0] = n->wake[1] = -1;

    w->presenter = windowPresenterNew(w, &n->surface, opts->backend, w->clientW, w->clientH);
    cscdViewSetViewport(w->view, w->clientW / w->dpr, w->clientH / w->dpr, w->dpr);
    const CscdClipboardHooks clip = {clipSetText, clipGetText, clipSetHtml, clipGetHtml, clipSetImage, clipGetImage, clipSetFiles, clipGetFiles, w};
    cscdViewSetClipboard(w->view, &clip);
    cscdViewOnDragOut(w->view, x11DragOut, w);
}

void* windowSurface(const CscdWindow* w) { return &w->native->surface; }

void windowShowTitle(CscdWindow* w, const char* title) { setTitle(w->native, title); }

void windowWake(CscdWindow* w) { wakeUp(w->native); }

void x11WindowOrigin(CscdWindow* w, int* x, int* y) {
    CscdWindowNative* n = w->native;
    Window child;
    *x = *y = 0;
    if (n && n->dpy && w->hwnd) XTranslateCoordinates(n->dpy, n->win, n->root, 0, 0, x, y, &child);
}

static Bool isMapOf(Display* d, XEvent* e, XPointer win) {
    (void)d;
    return (Bool)(e->type == MapNotify && e->xmap.window == *(Window*)win);
}

void cscdWindowShow(CscdWindow* w) {
    if (!w->hwnd || w->shown) return;
    w->shown = true;
    CscdWindowNative* n = w->native;
    XMapWindow(n->dpy, n->win);
    /* shown before returning, so that cscdWindowMaximize and full screen reach the window manager */
    XEvent ev;
    XIfEvent(n->dpy, &ev, isMapOf, (XPointer)&n->win);
}

/* ---- popups in windows of their own: menus, <select> lists and tooltips past the window's edges ----------------
 * Override-redirect windows (the window manager leaves them where they are put), with a visual that has alpha where
 * the server has one, for the shadow; without a compositing manager they are made opaque over white. Their pointer
 * events come through the window's loop, moved to where they are in the client area. */

typedef struct X11Popup {
    Window win;
    GC gc;
    Visual* visual;
    int depth;
    Colormap colormap;
    int width, height;
} X11Popup;

bool x11PopupArea(CscdWindow* w, CscdIRect* area) {
    CscdWindowNative* n = w->native;
    if (!n || !n->dpy || !w->hwnd || !w->shown) return false;
    int ox, oy;
    x11WindowOrigin(w, &ox, &oy);
    const int screen = DefaultScreen(n->dpy);
    *area = cscdIRect(-ox, -oy, DisplayWidth(n->dpy, screen), DisplayHeight(n->dpy, screen));
    return area->w > 0 && area->h > 0;
}

void* x11PopupShow(CscdWindow* w, void* popup, CscdIRect rect, const CscdImage* image, bool input) {
    (void)input; /* (a tooltip's events go to the view too, which finds the page under it) */
    CscdWindowNative* n = w->native;
    Display* dpy = n->dpy;
    X11Popup* p = (X11Popup*)popup;
    const int screen = DefaultScreen(dpy);
    if (!p) {
        p = CSCD_NEW(X11Popup);
        XVisualInfo vi;
        if (XMatchVisualInfo(dpy, screen, 32, TrueColor, &vi)) {
            p->visual = vi.visual;
            p->depth = 32;
        } else {
            p->visual = DefaultVisual(dpy, screen);
            p->depth = DefaultDepth(dpy, screen);
        }
        p->colormap = XCreateColormap(dpy, n->root, p->visual, AllocNone);
        XSetWindowAttributes swa;
        memset(&swa, 0, sizeof swa);
        swa.override_redirect = True;
        swa.colormap = p->colormap;
        swa.border_pixel = 0;
        swa.background_pixel = 0;
        swa.save_under = True;
        swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask;
        p->win = XCreateWindow(dpy, n->root, 0, 0, 1, 1, 0, p->depth, InputOutput, p->visual,
                               CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel | CWSaveUnder | CWEventMask, &swa);
        if (!p->win) {
            XFreeColormap(dpy, p->colormap);
            cscdFree(p);
            return NULL;
        }
        XSetTransientForHint(dpy, p->win, n->win);
        p->gc = XCreateGC(dpy, p->win, 0, NULL);
    }
    int ox, oy;
    x11WindowOrigin(w, &ox, &oy);
    XMoveResizeWindow(dpy, p->win, ox + rect.x, oy + rect.y, (unsigned)rect.w, (unsigned)rect.h);
    if (p->width == 0) XMapRaised(dpy, p->win);
    p->width = rect.w;
    p->height = rect.h;
    /* its pixels: premultiplied BGRA as X's 32-bit ARGB has them; over white where nothing composites them */
    char* data = (char*)malloc((size_t)rect.w * (size_t)rect.h * 4);
    if (data) {
        char cm[32];
        snprintf(cm, sizeof cm, "_NET_WM_CM_S%d", screen);
        const bool composited = p->depth == 32 && XGetSelectionOwner(dpy, XInternAtom(dpy, cm, False)) != XNONE;
        for (int y = 0; y < rect.h; ++y) {
            uint32_t* to = (uint32_t*)data + (size_t)y * (size_t)rect.w;
            const uint32_t* from = y < image->height ? cscdImageRow(image, y) : NULL;
            for (int x = 0; x < rect.w; ++x) {
                uint32_t px = from && x < image->width ? from[x] : 0;
                if (!composited) {
                    const uint32_t a = px >> 24, k = 255 - a; /* (premultiplied over white) */
                    px = 0xFF000000u | (((px >> 16 & 255) + k) << 16) | (((px >> 8 & 255) + k) << 8) | ((px & 255) + k);
                }
                to[x] = px;
            }
        }
        XImage* img = XCreateImage(dpy, p->visual, (unsigned)p->depth, ZPixmap, 0, data, (unsigned)rect.w, (unsigned)rect.h, 32, 0);
        if (img) {
            XPutImage(dpy, p->win, p->gc, img, 0, 0, 0, 0, (unsigned)rect.w, (unsigned)rect.h);
            XDestroyImage(img); /* (frees data) */
        } else {
            free(data);
        }
    }
    XFlush(dpy);
    return p;
}

void x11PopupHide(CscdWindow* w, void* popup) {
    X11Popup* p = (X11Popup*)popup;
    if (!p) return;
    CscdWindowNative* n = w->native;
    if (n && n->dpy) {
        XFreeGC(n->dpy, p->gc);
        XDestroyWindow(n->dpy, p->win);
        XFreeColormap(n->dpy, p->colormap);
        XFlush(n->dpy);
    }
    cscdFree(p);
}

/* An event in one of the popups' windows: its pointer's place moved to the client area's (true); false for another. */
static bool fromPopup(CscdWindow* w, XEvent* e) {
    for (size_t i = 0; i < w->popups.len; ++i) {
        const X11Popup* p = (const X11Popup*)w->popups.items[i].native;
        if (!p || p->win != e->xany.window) continue;
        const int dx = w->popups.items[i].rect.x, dy = w->popups.items[i].rect.y;
        switch (e->type) {
        case MotionNotify: e->xmotion.x += dx; e->xmotion.y += dy; break;
        case ButtonPress: case ButtonRelease: e->xbutton.x += dx; e->xbutton.y += dy; break;
        case EnterNotify: case LeaveNotify: e->xcrossing.x += dx; e->xcrossing.y += dy; break;
        default: break;
        }
        return true;
    }
    return false;
}

bool x11WindowHide(CscdWindow* w) {
    CscdWindowNative* n = w->native;
    XUnmapWindow(n->dpy, n->win);
    XFlush(n->dpy);
    return true;
}

void windowPlaceInputMethod(CscdWindow* w) {
    if (w->native && w->hwnd) placeIm(w->native, cscdViewCaretRect(w->view), w->dpr);
}

void cscdWindowResize(CscdWindow* w, int width, int height) {
    if (!w->hwnd) return;
    XResizeWindow(w->native->dpy, w->native->win, (unsigned)cscdMaxi(1, (int)(width * w->dpr)), (unsigned)cscdMaxi(1, (int)(height * w->dpr)));
    XFlush(w->native->dpy);
}

void cscdWindowMaximize(CscdWindow* w) {
    if (!w->hwnd || w->fullscreen) return;
    cscdWindowShow(w);
    sendWmState(w->native, true, atom(w->native, NET_WM_STATE_MAXIMIZED_VERT), atom(w->native, NET_WM_STATE_MAXIMIZED_HORZ));
}

void cscdWindowSetFullscreen(CscdWindow* w, bool on) {
    if (!w->hwnd || on == w->fullscreen) return;
    w->fullscreen = on;
    sendWmState(w->native, on, atom(w->native, NET_WM_STATE_FULLSCREEN), XNONE);
}

void cscdWindowClose(CscdWindow* w) {
    if (!w->hwnd) return;
    w->native->closeRequested = true; /* asked from the loop, like the close button */
    wakeUp(w->native);
}

static void keyPress(CscdWindow* w, XKeyEvent* key) {
    CscdWindowNative* n = w->native;
    n->lastTime = key->time;
    CscdStr text = {0};
    KeySym typed = 0;
    char buf[256];
    if (n->ic) {
        Status st = 0;
        int len = Xutf8LookupString(n->ic, key, buf, sizeof buf, &typed, &st);
        if (st == XBufferOverflow) {
            char* big = cscdMalloc((size_t)len + 1);
            len = Xutf8LookupString(n->ic, key, big, len, &typed, &st);
            cscdStrAppend(&text, cscdSvMake(big, (size_t)cscdMaxi(0, len)));
            cscdFree(big);
        } else if (st == XLookupChars || st == XLookupBoth) {
            cscdStrAppend(&text, cscdSvMake(buf, (size_t)cscdMaxi(0, len)));
        }
    } else {
        int len = XLookupString(key, buf, sizeof buf, &typed, NULL);
        latin1ToUtf8(buf, (size_t)cscdMaxi(0, len), &text);
    }
    char name[16];
    const CscdModifiers m = mods(key->state);
    const KeySym base = XLookupKeysym(key, 0);
    CscdKey k = mapKey(base ? base : typed, name);
    cscdViewKeyDown(w->view, k, m, cscdSv(name));
    /* text, without what Ctrl / Alt chords and control keys produce (they come as key events) */
    if (text.len && !m.ctrl && !m.alt) {
        CscdStr clean = {0};
        for (size_t i = 0; i < text.len; ++i) {
            const char c = text.ptr[i];
            if (c == '\r') cscdStrAppendChar(&clean, '\n');
            else if ((unsigned char)c >= 0x20 && c != 0x7F) cscdStrAppendChar(&clean, c);
        }
        if (clean.len) cscdViewTextInput(w->view, cscdStrSv(&clean));
        cscdStrFree(&clean);
    }
    cscdStrFree(&text);
    windowPlaceInputMethod(w);
}

static void clientMessage(CscdWindow* w, XEvent* e) {
    CscdWindowNative* n = w->native;
    Display* dpy = n->dpy;
    const float dpr = w->dpr;
    const Atom type = e->xclient.message_type;
    const long* l = e->xclient.data.l;
    if (type == atom(n, WM_PROTOCOLS)) {
        if ((Atom)l[0] == atom(n, WM_DELETE_WINDOW)) {
            n->closeRequested = true;
        } else if ((Atom)l[0] == atom(n, NET_WM_PING)) {
            XEvent reply = *e;
            reply.xclient.window = n->root;
            XSendEvent(dpy, n->root, False, SubstructureNotifyMask | SubstructureRedirectMask, &reply);
        }
    } else if (type == atom(n, XdndEnter)) {
        n->dndSource = (Window)l[0];
        n->dndVersion = (int)((unsigned long)l[1] >> 24);
        n->dndTypes.len = 0;
        n->dndEntered = false;
        if (l[1] & 1) {
            /* more than three types: in the source's XdndTypeList */
            Atom type2 = XNONE;
            int format = 0;
            unsigned long count = 0, after = 0;
            unsigned char* data = NULL;
            if (XGetWindowProperty(dpy, n->dndSource, atom(n, XdndTypeList), 0, 1024, False, XA_ATOM, &type2, &format, &count, &after, &data) == Success && data) {
                cscdVecAppend(&n->dndTypes, (Atom*)data, count);
                XFree(data);
            }
        } else {
            for (int i = 2; i <= 4; ++i) if (l[i]) cscdVecPush(&n->dndTypes, (Atom)l[i]);
        }
    } else if (type == atom(n, XdndPosition) && (Window)l[0] == n->dndSource) {
        int x = 0, y = 0;
        Window child;
        XTranslateCoordinates(dpy, n->root, n->win, (int)((unsigned long)l[2] >> 16), (int)(l[2] & 0xFFFF), &x, &y, &child);
        n->dndX = x / dpr;
        n->dndY = y / dpr;
        if (n->dndVersion >= 1 && l[3]) n->lastTime = (Time)l[3];
        if (!n->dndEntered) {
            n->dndEntered = true;
            CscdDataTransfer* dt = readDragData(n);
            cscdViewExternalDragEnter(w->view, dt, n->dndX, n->dndY);
            cscdRelease(dt);
        }
        const bool accept = cscdViewExternalDragOver(w->view, n->dndX, n->dndY);
        /* bit 1: keep sending positions, the answer depends on the element under the pointer */
        sendDnd(n, n->dndSource, XdndStatus, (accept ? 1 : 0) | 2, 0, 0, accept ? (long)atom(n, XdndActionCopy) : XNONE);
        w->dirty = true;
    } else if (type == atom(n, XdndLeave) && (Window)l[0] == n->dndSource) {
        if (n->dndEntered) cscdViewExternalDragLeave(w->view);
        n->dndEntered = false;
        n->dndSource = 0;
    } else if (type == atom(n, XdndDrop) && (Window)l[0] == n->dndSource) {
        if (n->dndVersion >= 1 && l[2]) n->lastTime = (Time)l[2];
        const bool ok = n->dndEntered && cscdViewExternalDrop(w->view, n->dndX, n->dndY);
        sendDnd(n, n->dndSource, XdndFinished, ok ? 1 : 0, ok ? (long)atom(n, XdndActionCopy) : XNONE, 0, 0);
        n->dndEntered = false;
        n->dndSource = 0;
        XRaiseWindow(dpy, n->win);
    }
}

bool cscdWindowPump(CscdWindow* w, bool wait) {
    if (!w->hwnd || !w->native) return false;
    CscdWindowNative* n = w->native;
    Display* dpy = n->dpy;
    CscdView* view = w->view;
    if (wait && !w->dirty && !n->closeRequested && !cscdViewNeedsPaint(view) && !XPending(dpy)) {
        int ms = cscdViewNextTimerMs(view);
        if (w->onFrame) ms = 0;
        if (!windowHasPosted(w)) {
            struct pollfd fds[3];
            int nf = 0;
            fds[nf++] = (struct pollfd){ConnectionNumber(dpy), POLLIN, 0};
            if (n->wake[0] >= 0) fds[nf++] = (struct pollfd){n->wake[0], POLLIN, 0};
            if (platformA11yFd(w) >= 0) fds[nf++] = (struct pollfd){platformA11yFd(w), POLLIN, 0};
            poll(fds, (nfds_t)nf, ms < 0 ? -1 : ms);
        }
    }
    platformA11yRead(w); /* (screen readers' questions, answered without waiting) */
    if (n->wake[0] >= 0) {
        char buf[64];
        while (read(n->wake[0], buf, sizeof buf) > 0) {}
    }
    const float dpr = w->dpr;
    while (w->hwnd && XPending(dpy)) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (XFilterEvent(&e, XNONE)) continue; /* the input method's */
        if (w->popups.len && fromPopup(w, &e)) {
            /* (a popup's: the pointer's events as the client area's; it leaving is nothing to the page) */
            if (e.type == Expose) w->dirty = true;
            if (e.type == Expose || e.type == LeaveNotify || e.type == EnterNotify) continue;
        }
        switch (e.type) {
        case Expose:
            if (e.xexpose.count == 0) w->dirty = true;
            break;
        case ConfigureNotify: {
            const int cw = e.xconfigure.width, ch = e.xconfigure.height;
            if (cw == w->clientW && ch == w->clientH) break;
            w->clientW = cw;
            w->clientH = ch;
            if (cw > 0 && ch > 0) {
                if (w->presenter) cscdPresenterResize(w->presenter, cw, ch);
                cscdViewSetViewport(view, cw / dpr, ch / dpr, dpr);
                w->dirty = true;
            }
            break;
        }
        case MotionNotify: {
            n->lastTime = e.xmotion.time;
            /* only the latest of a burst of moves */
            if (XEventsQueued(dpy, QueuedAlready)) {
                XEvent next;
                XPeekEvent(dpy, &next);
                if (next.type == MotionNotify && next.xmotion.window == e.xmotion.window) break;
            }
            cscdViewMouseMove(view, e.xmotion.x / dpr, e.xmotion.y / dpr, heldButtons(e.xmotion.state), mods(e.xmotion.state));
            break;
        }
        case LeaveNotify:
            n->lastTime = e.xcrossing.time;
            /* a drag keeps the pointer (an implicit grab), as SetCapture does on Windows */
            if (e.xcrossing.mode == NotifyNormal && !heldButtons(e.xcrossing.state)) cscdViewMouseLeave(view);
            break;
        case EnterNotify:
            n->lastTime = e.xcrossing.time;
            break;
        case ButtonPress: {
            n->lastTime = e.xbutton.time;
            const unsigned b = e.xbutton.button;
            const float x = e.xbutton.x / dpr, y = e.xbutton.y / dpr;
            const CscdModifiers m = mods(e.xbutton.state);
            if (b >= 4 && b <= 7) {
                /* wheel notches: 4 up, 5 down, 6 left, 7 right */
                if (b == 4 || b == 5) cscdViewWheel(view, x, y, 0, b == 4 ? -100.f : 100.f, m);
                else cscdViewWheel(view, x, y, b == 6 ? -100.f : 100.f, 0, m);
                break;
            }
            if (b > 3) break;
            const int button = b == 1 ? 0 : b == 2 ? 1 : 2;
            /* click counting for double / triple clicks */
            cscdViewMouseDown(view, x, y, button, m, clickPress(&n->clicks, (unsigned)e.xbutton.time, e.xbutton.x, e.xbutton.y, 400));
            break;
        }
        case ButtonRelease: {
            n->lastTime = e.xbutton.time;
            const unsigned b = e.xbutton.button;
            if (b < 1 || b > 3) break;
            cscdViewMouseUp(view, e.xbutton.x / dpr, e.xbutton.y / dpr, b == 1 ? 0 : b == 2 ? 1 : 2, mods(e.xbutton.state));
            break;
        }
        case KeyPress:
            keyPress(w, &e.xkey);
            break;
        case KeyRelease: {
            n->lastTime = e.xkey.time;
            char name[16];
            CscdKey k = mapKey(XLookupKeysym(&e.xkey, 0), name);
            cscdViewKeyUp(view, k, mods(e.xkey.state), cscdSv(name));
            break;
        }
        case FocusIn:
            if (n->ic) XSetICFocus(n->ic);
            windowPlaceInputMethod(w);
            break;
        case FocusOut:
            if (n->ic) XUnsetICFocus(n->ic);
            /* menus and tooltips do not outlive the window's turn in front */
            if (e.xfocus.mode == NotifyNormal && e.xfocus.detail != NotifyInferior) {
                cscdViewCloseMenus(view);
                cscdViewHideTooltip(view);
                cscdViewCancelDrag(view);
            }
            break;
        case PropertyNotify:
            n->lastTime = e.xproperty.time;
            break;
        case SelectionRequest:
            answerSelectionRequest(n, &e.xselectionrequest);
            break;
        case SelectionClear:
            if (e.xselectionclear.selection == atom(n, CLIPBOARD)) clipDataClear(&n->clip);
            break;
        case ClientMessage:
            clientMessage(w, &e);
            break;
        case DestroyNotify:
            if (e.xdestroywindow.window == n->win) w->hwnd = NULL;
            break;
        default: break;
        }
    }
    if (w->hwnd && n->closeRequested) {
        n->closeRequested = false;
        if (!w->onClose || w->onClose(w->onCloseUser)) {
            cscdPresenterFree(w->presenter);
            w->presenter = NULL;
            XDestroyWindow(dpy, n->win);
            XFlush(dpy);
            w->hwnd = NULL;
        }
    }
    if (!w->hwnd) return false;
    windowRunPosted(w);
    if (!w->hwnd) return false;
    if (cscdViewUpdate(view) || w->onFrame || w->dirty) windowRender(w);
    /* the cursor the element under the pointer asks for */
    if (!n->cursorShown || cscdViewCursor(view) != n->shown) {
        n->shown = cscdViewCursor(view);
        n->cursorShown = true;
        XDefineCursor(dpy, n->win, cursorFor(n, n->shown));
    }
    XFlush(dpy);
    return w->hwnd != NULL;
}
