/* Win32 window: message loop, per-monitor DPI, mouse / keyboard / touch / text input (with input
 * methods), cursors, clipboard, files dropped from other apps, and frame presentation through a
 * presenter. */
#include "platform/common/platformInternal.h"

#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <imm.h>
#include <ole2.h>
#include <shellapi.h>
#include <winreg.h>

#include <ctype.h>
#include <stdio.h>

static const wchar_t* kClass = L"cascade.window";

/* UTF-8 -> UTF-16, NUL-terminated (the caller frees it) */
static wchar_t* widen(const char* s, int len) {
    if (len < 0) len = (int)strlen(s);
    int n = len ? MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0) : 0;
    wchar_t* w = cscdMalloc(((size_t)n + 1) * sizeof(wchar_t));
    if (n) MultiByteToWideChar(CP_UTF8, 0, s, len, w, n);
    w[n] = 0;
    return w;
}

/* UTF-16 -> UTF-8, appended to out */
static void narrowInto(const wchar_t* w, int len, CscdStr* out) {
    if (len < 0) len = (int)wcslen(w);
    if (!len) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, NULL, 0, NULL, NULL);
    const size_t at = out->len;
    cscdStrAppendN(out, ' ', (size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, len, out->ptr + at, n, NULL, NULL);
}

static void removeCR(CscdStr* s) {
    size_t k = 0;
    for (size_t i = 0; i < s->len; ++i) if (s->ptr[i] != '\r') s->ptr[k++] = s->ptr[i];
    cscdStrTruncate(s, k);
}

static void enableDpiAwareness(void) {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE user = GetModuleHandleA("user32.dll");
    typedef BOOL(WINAPI * SetCtx)(HANDLE);
    SetCtx setCtx = (SetCtx)(void*)GetProcAddress(user, "SetProcessDpiAwarenessContext");
    if (setCtx && setCtx((HANDLE)-4)) return; /* per monitor v2 */
    typedef BOOL(WINAPI * SetAware)(void);
    SetAware setAware = (SetAware)(void*)GetProcAddress(user, "SetProcessDPIAware");
    if (setAware) setAware();
}

static float windowDpi(HWND hwnd) {
    typedef UINT(WINAPI * GetDpi)(HWND);
    static GetDpi f = NULL;
    static bool looked = false;
    if (!looked) {
        f = (GetDpi)(void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
        looked = true;
    }
    if (f && hwnd) return f(hwnd) / 96.f;
    HDC dc = GetDC(NULL);
    float d = GetDeviceCaps(dc, LOGPIXELSX) / 96.f;
    ReleaseDC(NULL, dc);
    return d;
}

static CscdModifiers mods(void) {
    CscdModifiers m;
    m.shift = GetKeyState(VK_SHIFT) < 0;
    m.ctrl = GetKeyState(VK_CONTROL) < 0;
    m.alt = GetKeyState(VK_MENU) < 0;
    m.meta = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
    return m;
}

static CscdKey mapKey(WPARAM vk, char name[16]) {
#define K(code, n, key) case code: strcpy(name, n); return key;
    switch (vk) {
    K(VK_BACK, "Backspace", CSCD_KEY_BACKSPACE)
    K(VK_TAB, "Tab", CSCD_KEY_TAB)
    K(VK_RETURN, "Enter", CSCD_KEY_ENTER)
    K(VK_ESCAPE, "Escape", CSCD_KEY_ESCAPE)
    K(VK_SPACE, " ", CSCD_KEY_SPACE)
    K(VK_DELETE, "Delete", CSCD_KEY_DELETE)
    K(VK_LEFT, "ArrowLeft", CSCD_KEY_LEFT)
    K(VK_RIGHT, "ArrowRight", CSCD_KEY_RIGHT)
    K(VK_UP, "ArrowUp", CSCD_KEY_UP)
    K(VK_DOWN, "ArrowDown", CSCD_KEY_DOWN)
    K(VK_HOME, "Home", CSCD_KEY_HOME)
    K(VK_END, "End", CSCD_KEY_END)
    K(VK_PRIOR, "PageUp", CSCD_KEY_PAGE_UP)
    K(VK_NEXT, "PageDown", CSCD_KEY_PAGE_DOWN)
    K(VK_INSERT, "Insert", CSCD_KEY_INSERT)
    K(VK_SHIFT, "Shift", CSCD_KEY_SHIFT)
    K(VK_CONTROL, "Control", CSCD_KEY_CONTROL)
    K(VK_MENU, "Alt", CSCD_KEY_ALT)
    K(VK_APPS, "ContextMenu", CSCD_KEY_UNKNOWN)
    K('A', "a", CSCD_KEY_A)
    K('C', "c", CSCD_KEY_C)
    K('V', "v", CSCD_KEY_V)
    K('X', "x", CSCD_KEY_X)
    K('Z', "z", CSCD_KEY_Z)
    K('Y', "y", CSCD_KEY_Y)
    default: break;
    }
#undef K
    if (vk >= VK_F1 && vk <= VK_F12) {
        snprintf(name, 16, "F%d", (int)(vk - VK_F1 + 1));
        return (CscdKey)((int)CSCD_KEY_F1 + (int)(vk - VK_F1));
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        name[0] = (char)tolower((int)vk);
        name[1] = 0;
        return CSCD_KEY_CHARACTER;
    }
    /* punctuation ("[", "=", "-", ","...): the character the key types without Shift */
    UINT ch = MapVirtualKeyW((UINT)vk, 2 /* MAPVK_VK_TO_CHAR */) & 0x7FFF;
    if (ch >= 0x20 && ch < 0x7F) {
        name[0] = (char)ch;
        name[1] = 0;
        return CSCD_KEY_CHARACTER;
    }
    strcpy(name, "Unidentified");
    return CSCD_KEY_UNKNOWN;
}

/* Touch arrives as pointer messages (Windows 8 and later; looked up so older systems still run). */
enum { kPointerUpdate = 0x0245, kPointerDown = 0x0246, kPointerUp = 0x0247, kPointerCaptureChanged = 0x024C };
#define kPointerTouch 2
#define kPointerCanceled 0x8000u
typedef struct PointerInfoHead { DWORD type; UINT32 id; UINT32 frameId; UINT32 flags; } PointerInfoHead;

static bool touchPointer(WPARAM wp) {
    typedef BOOL(WINAPI * GetType)(UINT32, DWORD*);
    static GetType f = NULL;
    static bool looked = false;
    if (!looked) {
        f = (GetType)(void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetPointerType");
        looked = true;
    }
    DWORD type = 0;
    return f && f(LOWORD(wp), &type) && type == kPointerTouch;
}

static bool pointerCanceled(WPARAM wp) {
    typedef BOOL(WINAPI * GetInfo)(UINT32, void*);
    static GetInfo f = NULL;
    static bool looked = false;
    if (!looked) {
        f = (GetInfo)(void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetPointerInfo");
        looked = true;
    }
    union { unsigned char bytes[256]; double align; } info;
    memset(&info, 0, sizeof info);
    if (!f || !f(LOWORD(wp), info.bytes)) return false;
    return (((PointerInfoHead*)info.bytes)->flags & kPointerCanceled) != 0;
}

static void dropFiles(HDROP drop, CscdStrVec* out) {
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    for (UINT i = 0; i < n; ++i) {
        UINT len = DragQueryFileW(drop, i, NULL, 0);
        wchar_t* path = cscdMalloc(((size_t)len + 1) * sizeof(wchar_t));
        DragQueryFileW(drop, i, path, len + 1);
        CscdStr s = {0};
        narrowInto(path, (int)len, &s);
        cscdVecPush(out, s);
        cscdFree(path);
    }
}

/* Files (CF_HDROP) and text (CF_UNICODETEXT) from a drag that started in another app. */
static CscdDataTransfer* readDragData(IDataObject* data) {
    CscdDataTransfer* dt = cscdDataTransferNew();
    cscdStrSetC(&dt->effectAllowed, "copy");
    FORMATETC files = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM med;
    memset(&med, 0, sizeof med);
    if (SUCCEEDED(IDataObject_GetData(data, &files, &med))) {
        dropFiles((HDROP)med.hGlobal, &dt->files);
        ReleaseStgMedium(&med);
    }
    FORMATETC text = {CF_UNICODETEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (SUCCEEDED(IDataObject_GetData(data, &text, &med))) {
        const wchar_t* p = (const wchar_t*)GlobalLock(med.hGlobal);
        if (p) {
            CscdStr t = {0};
            narrowInto(p, -1, &t);
            removeCR(&t);
            cscdDataTransferSetData(dt, CSCD_SV("text/plain"), cscdStrSv(&t));
            cscdStrFree(&t);
            GlobalUnlock(med.hGlobal);
        }
        ReleaseStgMedium(&med);
    }
    return dt;
}

/* ---- the OLE drop target ---------------------------------------------------------------------------- */

typedef struct DropTarget {
    IDropTarget iface;
    LONG refs;
    HWND hwnd;
    CscdWindow* window;
} DropTarget;

static POINT localPoint(const DropTarget* d, POINTL pt) {
    POINT p = {pt.x, pt.y};
    ScreenToClient(d->hwnd, &p);
    return p;
}

static HRESULT STDMETHODCALLTYPE dtQueryInterface(IDropTarget* self, REFIID riid, void** out) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
        *out = self;
        IDropTarget_AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dtAddRef(IDropTarget* self) { return (ULONG)InterlockedIncrement(&((DropTarget*)self)->refs); }

static ULONG STDMETHODCALLTYPE dtRelease(IDropTarget* self) {
    LONG n = InterlockedDecrement(&((DropTarget*)self)->refs);
    if (n == 0) cscdFree(self);
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE dtDragEnter(IDropTarget* self, IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) {
    (void)keys;
    DropTarget* d = (DropTarget*)self;
    const float dpr = d->window->dpr;
    POINT p = localPoint(d, pt);
    CscdDataTransfer* dt = readDragData(data);
    cscdViewExternalDragEnter(d->window->view, dt, p.x / dpr, p.y / dpr);
    cscdRelease(dt);
    *effect = cscdViewExternalDragOver(d->window->view, p.x / dpr, p.y / dpr) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dtDragOver(IDropTarget* self, DWORD keys, POINTL pt, DWORD* effect) {
    (void)keys;
    DropTarget* d = (DropTarget*)self;
    const float dpr = d->window->dpr;
    POINT p = localPoint(d, pt);
    *effect = cscdViewExternalDragOver(d->window->view, p.x / dpr, p.y / dpr) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dtDragLeave(IDropTarget* self) {
    cscdViewExternalDragLeave(((DropTarget*)self)->window->view);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dtDrop(IDropTarget* self, IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) {
    (void)data;
    (void)keys;
    DropTarget* d = (DropTarget*)self;
    const float dpr = d->window->dpr;
    POINT p = localPoint(d, pt);
    *effect = cscdViewExternalDrop(d->window->view, p.x / dpr, p.y / dpr) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    SetForegroundWindow(d->hwnd);
    return S_OK;
}

static IDropTargetVtbl dropTargetVtbl = {dtQueryInterface, dtAddRef, dtRelease, dtDragEnter, dtDragOver, dtDragLeave, dtDrop};

static LPCWSTR cursorId(CscdCursor c) {
    switch (c) {
    case CSCD_CURSOR_POINTER: return IDC_HAND;
    case CSCD_CURSOR_TEXT: return IDC_IBEAM;
    case CSCD_CURSOR_MOVE: case CSCD_CURSOR_GRAB: case CSCD_CURSOR_GRABBING: return IDC_SIZEALL;
    case CSCD_CURSOR_NOT_ALLOWED: return IDC_NO;
    case CSCD_CURSOR_COPY: return IDC_ARROW;
    case CSCD_CURSOR_CROSSHAIR: return IDC_CROSS;
    case CSCD_CURSOR_WAIT: return IDC_WAIT;
    case CSCD_CURSOR_PROGRESS: return IDC_APPSTARTING;
    case CSCD_CURSOR_HELP: return IDC_HELP;
    case CSCD_CURSOR_EW_RESIZE: case CSCD_CURSOR_COL_RESIZE: return IDC_SIZEWE;
    case CSCD_CURSOR_NS_RESIZE: case CSCD_CURSOR_ROW_RESIZE: return IDC_SIZENS;
    case CSCD_CURSOR_NESW_RESIZE: return IDC_SIZENESW;
    case CSCD_CURSOR_NWSE_RESIZE: return IDC_SIZENWSE;
    default: return IDC_ARROW;
    }
}

#define kPosted (WM_APP + 0x42)

struct CscdWindowNative {
    CscdWindow* owner;
    bool tracking;
    uint32_t highSurrogate;
    DropTarget* dropTarget; /* files and text dragged in from other apps */
    int savedFrame[4];      /* the window's place before going full screen */
    long savedStyle;
    int minWidth, minHeight;
    ClickCounter clicks;
};

/* ---- clipboard -------------------------------------------------------------------------------------- */

static void clipSetText(void* user, CscdSv text) {
    CscdWindow* w = (CscdWindow*)user;
    if (!OpenClipboard((HWND)w->hwnd)) return;
    EmptyClipboard();
    wchar_t* wide = widen(text.ptr ? text.ptr : "", (int)text.len);
    const size_t bytes = (wcslen(wide) + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        memcpy(GlobalLock(mem), wide, bytes);
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    cscdFree(wide);
    CloseClipboard();
}

static void clipGetText(void* user, CscdStr* out) {
    CscdWindow* w = (CscdWindow*)user;
    if (!OpenClipboard((HWND)w->hwnd)) return;
    CscdStr s = {0};
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            narrowInto(p, -1, &s);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    removeCR(&s);
    cscdStrAppend(out, cscdStrSv(&s));
    cscdStrFree(&s);
}

static uint32_t unpremul(uint32_t c, uint32_t a) { return a ? cscdMini(255, (int)((c * 255 + a / 2) / a)) : 0; }

static void clipSetImage(void* user, const CscdImage* img) {
    CscdWindow* w = (CscdWindow*)user;
    if (img->width <= 0 || img->height <= 0 || !OpenClipboard((HWND)w->hwnd)) return;
    EmptyClipboard();
    /* CF_DIB: a bottom-up 32-bit bitmap, straight (not premultiplied) colors */
    const size_t bytes = sizeof(BITMAPINFOHEADER) + (size_t)img->width * img->height * 4;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        BITMAPINFOHEADER* head = (BITMAPINFOHEADER*)GlobalLock(mem);
        memset(head, 0, sizeof *head);
        head->biSize = sizeof(BITMAPINFOHEADER);
        head->biWidth = img->width;
        head->biHeight = img->height;
        head->biPlanes = 1;
        head->biBitCount = 32;
        head->biCompression = BI_RGB;
        uint32_t* out = (uint32_t*)(head + 1);
        for (int y = 0; y < img->height; ++y) {
            const uint32_t* row = cscdImageRow(img, img->height - 1 - y);
            for (int x = 0; x < img->width; ++x) {
                const uint32_t p = row[x], a = p >> 24;
                out[(size_t)y * img->width + x] = (a << 24) | (unpremul((p >> 16) & 255, a) << 16) | (unpremul((p >> 8) & 255, a) << 8) | unpremul(p & 255, a);
            }
        }
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_DIB, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

static void clipGetFiles(void* user, CscdStrVec* out) {
    CscdWindow* w = (CscdWindow*)user;
    if (!OpenClipboard((HWND)w->hwnd)) return;
    HANDLE h = GetClipboardData(CF_HDROP);
    if (h) dropFiles((HDROP)h, out);
    CloseClipboard();
}

/* ---- the window ------------------------------------------------------------------------------------- */

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static void readPreferences(CscdWindow* w);

void windowCreateNative(CscdWindow* w, const CscdWindowOptions* opts) {
    enableDpiAwareness();
    HINSTANCE inst = GetModuleHandleW(NULL);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC | CS_DBLCLKS;
        wc.lpfnWndProc = wndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    CscdWindowNative* n = CSCD_NEW(CscdWindowNative);
    w->native = n;
    n->owner = w;
    n->minWidth = opts->minWidth;
    n->minHeight = opts->minHeight;
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!opts->resizable) style &= ~(DWORD)(WS_THICKFRAME | WS_MAXIMIZEBOX);
    /* size for the primary monitor's DPI; WM_DPICHANGED corrects it on other monitors */
    float dpi = windowDpi(NULL);
    RECT r = {0, 0, (LONG)(opts->width * dpi), (LONG)(opts->height * dpi)};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    wchar_t* title = widen(opts->title && opts->title[0] ? opts->title : "cascade", -1);
    HWND hwnd = CreateWindowExW(0, kClass, title, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, w);
    cscdFree(title);
    w->hwnd = hwnd;
    if (!hwnd) return;
    w->dpr = windowDpi(hwnd);
    if (opts->darkTitleBar) {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        if (dwm) {
            typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
            SetAttr f = (SetAttr)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
            BOOL on = TRUE;
            if (f) f(hwnd, 20, &on, sizeof on);
        }
    }
    RECT cr;
    GetClientRect(hwnd, &cr);
    w->clientW = cr.right;
    w->clientH = cr.bottom;
    w->presenter = windowPresenterNew(w, hwnd, opts->backend, w->clientW, w->clientH);
    cscdViewSetViewport(w->view, w->clientW / w->dpr, w->clientH / w->dpr, w->dpr);
    cscdViewSetClipboardHooks(w->view, clipSetText, clipGetText, clipSetImage, clipGetFiles, w);
    readPreferences(w);
    winA11yAttach(w);
    /* files and text dragged in from other apps */
    if (SUCCEEDED(OleInitialize(NULL))) {
        DropTarget* target = CSCD_NEW(DropTarget);
        target->iface.lpVtbl = &dropTargetVtbl;
        target->refs = 1;
        target->hwnd = hwnd;
        target->window = w;
        if (SUCCEEDED(RegisterDragDrop(hwnd, &target->iface))) n->dropTarget = target;
        else IDropTarget_Release(&target->iface);
    }
    /* Without OLE (the thread's COM was already made multithreaded, e.g. by an audio library started
     * first) files still arrive, as WM_DROPFILES: no hovering, only the drop. */
    if (!n->dropTarget) {
        fprintf(stderr, "cascade: OLE drag and drop unavailable on this thread (create the window before COM is initialized "
                        "multithreaded); dropped files still arrive\n");
        DragAcceptFiles(hwnd, TRUE);
    }
}

void windowDestroyNative(CscdWindow* w) {
    CscdWindowNative* n = w->native;
    winA11yDetach(w);
    if (n->dropTarget) {
        if (w->hwnd) RevokeDragDrop((HWND)w->hwnd);
        IDropTarget_Release(&n->dropTarget->iface);
        n->dropTarget = NULL;
    }
    if (w->hwnd) DestroyWindow((HWND)w->hwnd);
    w->native = NULL;
    cscdFree(n);
}

void* windowSurface(const CscdWindow* w) { return w->hwnd; }

void windowShowTitle(CscdWindow* w, const char* title) {
    wchar_t* t = widen(title, -1);
    SetWindowTextW((HWND)w->hwnd, t);
    cscdFree(t);
}

void windowWake(CscdWindow* w) { PostMessageW((HWND)w->hwnd, kPosted, 0, 0); }

void cscdWindowShow(CscdWindow* w) {
    if (!w->hwnd || w->shown) return;
    w->shown = true;
    ShowWindow((HWND)w->hwnd, SW_SHOW);
    UpdateWindow((HWND)w->hwnd);
}

void windowPlaceInputMethod(CscdWindow* w) {
    HWND hwnd = (HWND)w->hwnd;
    const float dpr = w->dpr;
    CscdRect r = cscdViewCaretRect(w->view);
    if (cscdRectEmpty(r) && r.h <= 0) return;
    HIMC imc = ImmGetContext(hwnd);
    if (!imc) return;
    COMPOSITIONFORM cf;
    memset(&cf, 0, sizeof cf);
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos.x = (LONG)(r.x * dpr);
    cf.ptCurrentPos.y = (LONG)(r.y * dpr);
    ImmSetCompositionWindow(imc, &cf);
    CANDIDATEFORM cand;
    memset(&cand, 0, sizeof cand);
    cand.dwIndex = 0;
    cand.dwStyle = CFS_EXCLUDE;
    cand.ptCurrentPos.x = (LONG)(r.x * dpr);
    cand.ptCurrentPos.y = (LONG)(cscdRectBottom(r) * dpr);
    cand.rcArea.left = (LONG)(r.x * dpr);
    cand.rcArea.top = (LONG)(r.y * dpr);
    cand.rcArea.right = (LONG)(cscdRectRight(r) * dpr + 1);
    cand.rcArea.bottom = (LONG)(cscdRectBottom(r) * dpr);
    ImmSetCandidateWindow(imc, &cand);
    ImmReleaseContext(hwnd, imc);
}

void cscdWindowResize(CscdWindow* w, int width, int height) {
    if (!w->hwnd) return;
    RECT r = {0, 0, (LONG)(width * w->dpr), (LONG)(height * w->dpr)};
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongW((HWND)w->hwnd, GWL_STYLE), FALSE, 0);
    SetWindowPos((HWND)w->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

void cscdWindowMaximize(CscdWindow* w) {
    if (!w->hwnd || w->fullscreen) return;
    w->shown = true;
    ShowWindow((HWND)w->hwnd, SW_MAXIMIZE);
}

void cscdWindowSetFullscreen(CscdWindow* w, bool on) {
    HWND hwnd = (HWND)w->hwnd;
    if (!hwnd || on == w->fullscreen) return;
    w->fullscreen = on;
    CscdWindowNative* n = w->native;
    if (on) {
        /* the whole monitor, without a frame; the place it had comes back afterwards */
        RECT r;
        GetWindowRect(hwnd, &r);
        n->savedFrame[0] = r.left, n->savedFrame[1] = r.top, n->savedFrame[2] = r.right - r.left, n->savedFrame[3] = r.bottom - r.top;
        n->savedStyle = GetWindowLongW(hwnd, GWL_STYLE);
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, n->savedStyle & ~(long)(WS_CAPTION | WS_THICKFRAME));
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, n->savedStyle);
        SetWindowPos(hwnd, NULL, n->savedFrame[0], n->savedFrame[1], n->savedFrame[2], n->savedFrame[3], SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}

void cscdWindowClose(CscdWindow* w) {
    if (w->hwnd) PostMessageW((HWND)w->hwnd, WM_CLOSE, 0, 0);
}

bool cscdWindowPump(CscdWindow* w, bool wait) {
    if (!w->hwnd) return false;
    MSG msg;
    if (wait && !w->dirty && !cscdViewNeedsPaint(w->view)) {
        int ms = cscdViewNextTimerMs(w->view);
        if (w->onFrame) ms = 0;
        MsgWaitForMultipleObjectsEx(0, NULL, ms < 0 ? INFINITE : (DWORD)ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            w->hwnd = NULL;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!w->hwnd) return false;
    if (cscdViewUpdate(w->view) || w->onFrame || w->dirty) windowRender(w);
    return w->hwnd != NULL;
}

/* the composition string of the given kind (GCS_RESULTSTR, GCS_COMPSTR), in UTF-8 */
static CscdStr imeString(HIMC imc, DWORD which) {
    CscdStr out = {0};
    LONG bytes = ImmGetCompositionStringW(imc, which, NULL, 0);
    if (bytes <= 0) return out;
    wchar_t* w = cscdMalloc((size_t)bytes + sizeof(wchar_t));
    ImmGetCompositionStringW(imc, which, w, (DWORD)bytes);
    narrowInto(w, (int)(bytes / sizeof(wchar_t)), &out);
    cscdFree(w);
    return out;
}

static LRESULT handle(CscdWindow* wnd, CscdWindowNative* n, UINT msg, WPARAM wp, LPARAM lp) {
    HWND hwnd = (HWND)wnd->hwnd;
    CscdView* view = wnd->view;
    const float dpr = wnd->dpr;
#define PX(l) (GET_X_LPARAM(l) / dpr)
#define PY(l) (GET_Y_LPARAM(l) / dpr)
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        windowRender(wnd);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: {
        /* minimized: in the background until it comes back */
        if (wp == SIZE_MINIMIZED) {
            windowAppEvent(wnd, CSCD_APP_BACKGROUND);
            return 0;
        }
        if (wnd->background) windowAppEvent(wnd, CSCD_APP_FOREGROUND);
        int w = LOWORD(lp), h = HIWORD(lp);
        if (w == wnd->clientW && h == wnd->clientH) return 0;
        wnd->clientW = w;
        wnd->clientH = h;
        if (w > 0 && h > 0) {
            if (wnd->presenter) cscdPresenterResize(wnd->presenter, w, h);
            cscdViewSetViewport(view, w / dpr, h / dpr, dpr);
            windowRender(wnd);
        }
        return 0;
    }
    case WM_SETTINGCHANGE: /* (the theme, animation effects or text size may have changed) */
    case WM_THEMECHANGED:
        readPreferences(wnd);
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = (LONG)(n->minWidth * dpr);
        mm->ptMinTrackSize.y = (LONG)(n->minHeight * dpr);
        return 0;
    }
    case 0x02E0: { /* WM_DPICHANGED */
        wnd->dpr = HIWORD(wp) / 96.f;
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        cscdViewSetViewport(view, wnd->clientW / wnd->dpr, wnd->clientH / wnd->dpr, wnd->dpr);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!n->tracking) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
            n->tracking = true;
        }
        int buttons = ((wp & MK_LBUTTON) ? 1 : 0) | ((wp & MK_RBUTTON) ? 2 : 0) | ((wp & MK_MBUTTON) ? 4 : 0);
        cscdViewMouseMove(view, PX(lp), PY(lp), buttons, mods());
        return 0;
    }
    case WM_MOUSELEAVE:
        n->tracking = false;
        cscdViewMouseLeave(view);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: {
        SetCapture(hwnd);
        SetFocus(hwnd);
        int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) ? 0 : (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK) ? 1 : 2;
        const int count = clickPress(&n->clicks, (unsigned)GetMessageTime(), GET_X_LPARAM(lp), GET_Y_LPARAM(lp), GetDoubleClickTime());
        cscdViewMouseDown(view, PX(lp), PY(lp), button, mods(), count);
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        ReleaseCapture();
        int button = msg == WM_LBUTTONUP ? 0 : msg == WM_MBUTTONUP ? 1 : 2;
        cscdViewMouseUp(view, PX(lp), PY(lp), button, mods());
        return 0;
    }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        float delta = -(float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 100.f;
        if (msg == WM_MOUSEHWHEEL) cscdViewWheel(view, p.x / dpr, p.y / dpr, -delta, 0, mods());
        else cscdViewWheel(view, p.x / dpr, p.y / dpr, 0, delta, mods());
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (cscdViewCursor(view) == CSCD_CURSOR_NONE) SetCursor(NULL);
            else SetCursor(LoadCursorW(NULL, cursorId(cscdViewCursor(view))));
            return TRUE;
        }
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        char name[16];
        CscdKey k = mapKey(wp, name);
        cscdViewKeyDown(view, k, mods(), cscdSv(name));
        if (msg == WM_SYSKEYDOWN && wp != VK_F10) break;
        return 0;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        char name[16];
        CscdKey k = mapKey(wp, name);
        cscdViewKeyUp(view, k, mods(), cscdSv(name));
        if (msg == WM_SYSKEYUP) break;
        return 0;
    }
    case WM_CHAR: {
        uint32_t c = (uint32_t)wp;
        if (c >= 0xD800 && c < 0xDC00) { n->highSurrogate = c; return 0; }
        if (c >= 0xDC00 && c < 0xE000 && n->highSurrogate) {
            c = 0x10000 + ((n->highSurrogate - 0xD800) << 10) + (c - 0xDC00);
            n->highSurrogate = 0;
        }
        if (c < 0x20 && c != '\r' && c != '\t') return 0; /* control characters come as key events */
        if (c == '\t') return 0;
        CscdStr s = {0};
        cscdUtf8Append(&s, c == '\r' ? '\n' : c);
        cscdViewTextInput(view, cscdStrSv(&s));
        cscdStrFree(&s);
        return 0;
    }
    case kPointerDown: case kPointerUpdate: case kPointerUp: {
        /* fingers; a pen and the mouse go on as mouse messages */
        if (!touchPointer(wp)) break;
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        int id = (int)LOWORD(wp);
        if (msg == kPointerDown) cscdViewTouchDown(view, id, p.x / dpr, p.y / dpr);
        else if (msg == kPointerUpdate) cscdViewTouchMove(view, id, p.x / dpr, p.y / dpr);
        else if (pointerCanceled(wp)) cscdViewTouchCancel(view);
        else cscdViewTouchUp(view, id, p.x / dpr, p.y / dpr);
        return 0;
    }
    case kPointerCaptureChanged:
        if (touchPointer(wp)) {
            cscdViewTouchCancel(view);
            return 0;
        }
        break;
    case WM_IME_SETCONTEXT:
        /* the composition is drawn in the text field itself */
        lp &= ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW;
        break;
    case WM_IME_STARTCOMPOSITION:
        windowPlaceInputMethod(wnd);
        return 0;
    case WM_IME_COMPOSITION: {
        HIMC imc = ImmGetContext(hwnd);
        if (!imc) break;
        if (lp & GCS_RESULTSTR) {
            CscdStr text = imeString(imc, GCS_RESULTSTR);
            cscdViewSetComposition(view, CSCD_SV(""));
            if (text.len) cscdViewTextInput(view, cscdStrSv(&text));
            cscdStrFree(&text);
        }
        if (lp & GCS_COMPSTR) {
            CscdStr comp = imeString(imc, GCS_COMPSTR);
            cscdViewSetComposition(view, cscdStrSv(&comp));
            cscdStrFree(&comp);
        }
        ImmReleaseContext(hwnd, imc);
        windowPlaceInputMethod(wnd);
        return 0;
    }
    case WM_IME_ENDCOMPOSITION:
        cscdViewSetComposition(view, CSCD_SV(""));
        return 0;
    case WM_ACTIVATE:
        /* menus and tooltips do not outlive the window's turn in front */
        if (LOWORD(wp) == WA_INACTIVE) {
            cscdViewCloseMenus(view);
            cscdViewHideTooltip(view);
        }
        break;
    case kPosted:
        windowRunPosted(wnd);
        /* repaint through WM_PAINT, which also comes while Windows runs its own loop (a resize) */
        if (wnd->dirty && wnd->hwnd) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_CANCELMODE:
        cscdViewCancelDrag(view);
        break;
    case WM_KILLFOCUS:
        return 0;
    case WM_DROPFILES: {
        /* files dropped while OLE is unavailable: the same events as a drag that ends at once */
        HDROP drop = (HDROP)wp;
        POINT pt = {0, 0};
        DragQueryPoint(drop, &pt);
        CscdDataTransfer* dt = cscdDataTransferNew();
        cscdStrSetC(&dt->effectAllowed, "copy");
        dropFiles(drop, &dt->files);
        DragFinish(drop);
        const float x = pt.x / dpr, y = pt.y / dpr;
        cscdViewExternalDragEnter(view, dt, x, y);
        cscdRelease(dt);
        cscdViewExternalDragOver(view, x, y);
        cscdViewExternalDrop(view, x, y);
        return 0;
    }
    case WM_CLOSE:
        if (wnd->onClose && !wnd->onClose(wnd->onCloseUser)) return 0;
        cscdPresenterFree(wnd->presenter);
        wnd->presenter = NULL;
        DestroyWindow(hwnd);
        return 0;
    case WM_GETOBJECT: {
        intptr_t r;
        if (winA11yGetObject(wnd, (uintptr_t)wp, (intptr_t)lp, &r)) return (LRESULT)r;
        break;
    }
    case WM_DESTROY:
        winA11yDetach(wnd);
        wnd->hwnd = NULL;
        return 0;
    default: break;
    }
#undef PX
#undef PY
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    CscdWindow* self;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (CscdWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd = hwnd;
    } else {
        self = (CscdWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self || !self->native) return DefWindowProcW(hwnd, msg, wp, lp);
    return handle(self, self->native, msg, wp, lp);
}

/* ---- the system around the app ------------------------------------------------------------------- */

static DWORD regDword(const wchar_t* key, const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof value;
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, NULL, &value, &size) == ERROR_SUCCESS ? value : fallback;
}

/* the apps' theme (Settings > Personalization > Colors), animation effects and text size
 * (Accessibility > Text size) */
static void readPreferences(CscdWindow* w) {
    CscdSystemPreferences p;
    p.dark = regDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", 1) == 0;
    BOOL animate = TRUE;
    p.reducedMotion = SystemParametersInfoW(0x1042 /* SPI_GETCLIENTAREAANIMATION */, 0, &animate, 0) && !animate;
    p.textScale = (float)regDword(L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", 100) / 100.f;
    windowSystemPreferences(w, p);
}

bool platformOpenUrl(CscdWindow* w, const char* url) {
    wchar_t* u = widen(url, -1);
    const INT_PTR r = (INT_PTR)ShellExecuteW((HWND)w->hwnd, L"open", u, NULL, NULL, SW_SHOWNORMAL);
    cscdFree(u);
    return r > 32;
}

bool platformShareText(CscdWindow* w, const char* text) { (void)w; (void)text; return false; }
void platformHaptic(CscdWindow* w, CscdHaptic kind) { (void)w; (void)kind; }

bool platformHide(CscdWindow* w) {
    ShowWindow((HWND)w->hwnd, SW_HIDE);
    return true;
}

/* ---- the tray: the notification area's icon, with a hidden window of its own for its messages ------- */

bool platformSetMenuBar(CscdWindow* w, CscdMenu* menu) { (void)w; (void)menu; return false; /* (the page's: cscdViewBindMenuBar) */ }

#define kTrayMessage (WM_APP + 0x43)

typedef struct TrayNative {
    HWND hwnd;
    HICON icon;
    NOTIFYICONDATAW nid;
} TrayNative;

static UINT taskbarCreated; /* (Explorer started again: the icons go back in) */

/* an icon of the image: 32-bit, straight alpha */
static HICON iconOf(const CscdImage* img) {
    if (cscdImageEmpty(img)) return NULL;
    BITMAPV5HEADER bi;
    memset(&bi, 0, sizeof bi);
    bi.bV5Size = sizeof bi;
    bi.bV5Width = img->width;
    bi.bV5Height = -img->height;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color) return NULL;
    uint32_t* out = (uint32_t*)bits;
    for (size_t i = 0, n = cscdImagePixelCount(img); i < n; ++i) {
        const uint32_t p = img->pixels[i], a = p >> 24;
        if (!a) {
            out[i] = 0;
            continue;
        }
        const uint32_t r = ((p >> 16) & 255) * 255 / a, g = ((p >> 8) & 255) * 255 / a, b = (p & 255) * 255 / a;
        out[i] = a << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
    }
    HBITMAP mask = CreateBitmap(img->width, img->height, 1, 1, NULL);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

typedef CSCD_VEC(const CscdMenuItem*) MenuItemPtrs;
typedef CSCD_VEC(CscdMenu) MenuVec;

/* a popup menu of m: each item's command is its index in ids, plus one; menus filled as they are
 * built are kept in filled until the menu is done */
static HMENU buildMenu(const CscdMenu* m, MenuItemPtrs* ids, MenuVec* filled) {
    HMENU h = CreatePopupMenu();
    for (size_t i = 0; i < m->len; ++i) {
        const CscdMenuItem* it = &m->items[i];
        if (it->separator) {
            AppendMenuW(h, MF_SEPARATOR, 0, NULL);
            continue;
        }
        CscdStr label = cscdStrCopy(&it->label);
        if (it->shortcut.len) {
            cscdStrAppendChar(&label, '\t');
            cscdStrAppend(&label, cscdStrSv(&it->shortcut));
        }
        wchar_t* text = widen(cscdStrC(&label), -1);
        cscdStrFree(&label);
        UINT flags = MF_STRING | (it->enabled ? 0 : MF_GRAYED) | (it->checkable && it->checked ? MF_CHECKED : 0);
        const CscdMenu* sub = &it->submenu;
        if (!sub->len && it->fill.fn) {
            CscdMenu made = {0};
            it->fill.fn(it->fill.user, &made);
            cscdVecPush(filled, made);
            sub = &cscdVecLast(filled);
        }
        if (sub->len || it->fill.fn) {
            AppendMenuW(h, flags | MF_POPUP, (UINT_PTR)buildMenu(sub, ids, filled), text);
        } else {
            cscdVecPush(ids, it);
            AppendMenuW(h, flags, (UINT_PTR)ids->len, text);
        }
        cscdFree(text);
    }
    return h;
}

static void showTrayMenu(CscdTray* t, int x, int y) {
    TrayNative* n = (TrayNative*)t->native;
    MenuItemPtrs ids = {0};
    MenuVec filled = {0};
    HMENU menu = buildMenu(&t->menu, &ids, &filled);
    SetForegroundWindow(n->hwnd); /* (or the menu stays open when the user clicks elsewhere) */
    const UINT chosen = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, x, y, 0, n->hwnd, NULL);
    PostMessageW(n->hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (chosen && chosen <= ids.len) windowMenuChosen(t->w, ids.items[chosen - 1]);
    cscdVecFree(&ids);
    for (size_t i = 0; i < filled.len; ++i) cscdMenuFree(&filled.items[i]);
    cscdVecFree(&filled);
}

static LRESULT CALLBACK trayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    CscdTray* t = (CscdTray*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (t && msg == kTrayMessage) {
        /* (NOTIFYICON_VERSION_4: the event in LOWORD(lp), where in wp) */
        const UINT event = LOWORD(lp);
        if (event == NIN_SELECT || event == NIN_KEYSELECT) {
            if (t->click.fn) windowTrayClicked(t);
            else if (t->menu.len) showTrayMenu(t, GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
        } else if (event == WM_CONTEXTMENU && t->menu.len) {
            showTrayMenu(t, GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
        }
        return 0;
    }
    if (t && taskbarCreated && msg == taskbarCreated) {
        TrayNative* n = (TrayNative*)t->native;
        Shell_NotifyIconW(NIM_ADD, &n->nid);
        Shell_NotifyIconW(NIM_SETVERSION, &n->nid);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void traySetTip(TrayNative* n, const char* tooltip) {
    wchar_t* tip = widen(tooltip, -1);
    wcsncpy(n->nid.szTip, tip, CSCD_COUNT(n->nid.szTip) - 1);
    n->nid.szTip[CSCD_COUNT(n->nid.szTip) - 1] = 0;
    cscdFree(tip);
}

bool platformTrayCreate(CscdTray* t, const CscdImage* icon, const char* tooltip) {
    static bool registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = trayProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"cascade-tray";
        registered = RegisterClassW(&wc) != 0;
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    }
    /* a top-level window never shown (a message-only one would not hear TaskbarCreated) */
    HWND hwnd = CreateWindowExW(0, L"cascade-tray", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!hwnd) return false;
    TrayNative* n = cscdCalloc(1, sizeof *n);
    n->hwnd = hwnd;
    n->icon = iconOf(icon);
    n->nid.cbSize = sizeof n->nid;
    n->nid.hWnd = hwnd;
    n->nid.uID = 1;
    n->nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP | (n->icon ? NIF_ICON : 0);
    n->nid.uCallbackMessage = kTrayMessage;
    n->nid.hIcon = n->icon;
    n->nid.uVersion = NOTIFYICON_VERSION_4;
    traySetTip(n, tooltip);
    t->native = n;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)t);
    if (!Shell_NotifyIconW(NIM_ADD, &n->nid)) {
        DestroyWindow(hwnd);
        if (n->icon) DestroyIcon(n->icon);
        cscdFree(n);
        t->native = NULL;
        return false;
    }
    Shell_NotifyIconW(NIM_SETVERSION, &n->nid);
    return true;
}

void platformTraySet(CscdTray* t, const CscdImage* icon, const char* tooltip) {
    TrayNative* n = (TrayNative*)t->native;
    if (icon) {
        HICON old = n->icon;
        n->icon = iconOf(icon);
        n->nid.hIcon = n->icon;
        n->nid.uFlags |= NIF_ICON;
        Shell_NotifyIconW(NIM_MODIFY, &n->nid);
        if (old) DestroyIcon(old);
    }
    if (tooltip) {
        traySetTip(n, tooltip);
        Shell_NotifyIconW(NIM_MODIFY, &n->nid);
    }
}

void platformTrayDestroy(CscdTray* t) {
    TrayNative* n = (TrayNative*)t->native;
    if (!n) return;
    Shell_NotifyIconW(NIM_DELETE, &n->nid);
    SetWindowLongPtrW(n->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(n->hwnd);
    if (n->icon) DestroyIcon(n->icon);
    cscdFree(n);
    t->native = NULL;
}

/* GetOpenFileNameW and GetSaveFileNameW, from comdlg32.dll (opened when first asked for) */
typedef BOOL(WINAPI * FileDialogFn)(OPENFILENAMEW*);
static FileDialogFn fileDialog(const char* name) {
    static HMODULE dlg;
    if (!dlg) dlg = LoadLibraryW(L"comdlg32.dll");
    return dlg ? (FileDialogFn)(void*)GetProcAddress(dlg, name) : NULL;
}

/* the dialogs' filter: "Files\0*.png;*.jpg\0All files\0*.*\0\0"; *firstExt (when asked): the first
 * extension, or NULL */
static wchar_t* fileFilter(const char* accept, wchar_t** firstExt) {
    CscdStrVec exts = {0};
    windowAcceptExtensions(accept, &exts);
    if (firstExt) *firstExt = exts.len ? widen(exts.items[0].ptr, (int)exts.items[0].len) : NULL;
    CscdStr filter = {0};
    if (exts.len) {
        cscdStrAppendC(&filter, "Files");
        cscdStrAppendChar(&filter, 0);
        for (size_t i = 0; i < exts.len; ++i) {
            if (i) cscdStrAppendChar(&filter, ';');
            cscdStrAppendC(&filter, "*.");
            cscdStrAppend(&filter, cscdStrSv(&exts.items[i]));
        }
        cscdStrAppendChar(&filter, 0);
    }
    cscdStrAppendC(&filter, "All files");
    cscdStrAppendChar(&filter, 0);
    cscdStrAppendC(&filter, "*.*");
    cscdStrAppendChar(&filter, 0);
    cscdStrAppendChar(&filter, 0);
    cscdStrVecFree(&exts);
    wchar_t* wfilter = widen(filter.ptr, (int)filter.len);
    cscdStrFree(&filter);
    return wfilter;
}

bool platformPickFiles(CscdWindow* w, const char* accept, bool multiple) {
    FileDialogFn open = fileDialog("GetOpenFileNameW");
    if (!open) return false;
    wchar_t* wfilter = fileFilter(accept, NULL);
    enum { CHARS = 32768 };
    wchar_t* names = cscdCalloc(CHARS, sizeof(wchar_t));
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = (HWND)w->hwnd;
    ofn.lpstrFilter = wfilter;
    ofn.lpstrFile = names;
    ofn.nMaxFile = CHARS;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | (multiple ? OFN_ALLOWMULTISELECT : 0);
    CscdStrVec paths = {0};
    if (open(&ofn)) {
        /* one path; or with several: the folder, then each name */
        const wchar_t* first = names;
        const wchar_t* next = first + wcslen(first) + 1;
        if (!*next) {
            CscdStr p = {0};
            narrowInto(first, -1, &p);
            cscdVecPush(&paths, p);
        } else
            for (; *next; next += wcslen(next) + 1) {
                CscdStr p = {0};
                narrowInto(first, -1, &p);
                cscdStrAppendChar(&p, '\\');
                narrowInto(next, -1, &p);
                cscdVecPush(&paths, p);
            }
    }
    cscdFree(names);
    cscdFree(wfilter);
    windowFilesPicked(w, &paths);
    cscdStrVecFree(&paths);
    return true;
}

bool platformSaveFile(CscdWindow* w, const char* name, const char* accept) {
    FileDialogFn save = fileDialog("GetSaveFileNameW");
    if (!save) return false;
    wchar_t* ext = NULL;
    wchar_t* wfilter = fileFilter(accept, &ext);
    enum { CHARS = 32768 };
    wchar_t* file = cscdCalloc(CHARS, sizeof(wchar_t));
    wchar_t* wname = widen(name, -1);
    wcsncpy(file, wname, CHARS - 1);
    cscdFree(wname);
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = (HWND)w->hwnd;
    ofn.lpstrFilter = wfilter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = CHARS;
    ofn.lpstrDefExt = ext; /* (a name typed without an extension gets it) */
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOREADONLYRETURN;
    CscdStr path = {0};
    if (save(&ofn)) narrowInto(file, -1, &path);
    cscdFree(file);
    cscdFree(wfilter);
    cscdFree(ext);
    windowSaveChosen(w, path.len ? cscdStrC(&path) : NULL);
    cscdStrFree(&path);
    return true;
}
