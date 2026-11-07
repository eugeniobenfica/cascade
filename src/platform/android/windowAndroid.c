/* Android window: cascade's activity (java/org/cascade/CascadeActivity.java, beside this file) holds
 * the page's surface and hands over what the system gives Java; the program's main (renamed
 * cscdAppMain by window.h) runs on a thread of its own with an ALooper. What the activity sends (on
 * the system's main thread) waits in a queue for that thread, which a pipe wakes; the surface is let
 * go of before the callback that takes it away returns. Frames go to the ANativeWindow by Vulkan
 * (VK_KHR_android_surface) or by locking its buffers (the software path).
 *
 * The activity passes touch, mouse and keys (with the characters of the keyboard's own layout), the
 * input method's edits as the focused field's whole text state (cscdViewSetTextState: composition,
 * corrections and suggestions in every language), the system's theme, text size, animation scale and
 * density, and the app's life. This side tells it the keyboard the focused field wants and the
 * field's text as it changes, the page's edge colors for what is under the system's bars, and, while
 * a screen reader runs, a copy of the accessible tree for TalkBack; the floating edit menu and the
 * magnifier over a selection made by touch (the view draws its handles). The clipboard (text,
 * pictures, files), drops from other apps, links, sharing, the file picker and vibration are the
 * activity's too. Files the program reads by relative paths
 * are also looked up among the APK's assets (files.c); standard error goes to the log (logcat, tag
 * "cascade"). */
#include "platform/common/platformInternal.h"

#include <android/asset_manager_jni.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#undef main
int cscdAppMain(int argc, char** argv); /* the program's main (window.h) */
void filesSetAndroidAssets(void* manager); /* (files.c) */

/* what the activity sends (CascadeActivity.java has the same numbers) */
enum {
    E_TOUCH = 1, E_MOUSE = 2, E_KEY = 3, E_TEXT_STATE = 4, E_EDITOR_ACTION = 5, E_FOCUS = 6, E_APP = 7, E_PREFERENCES = 8,
    E_BACK = 9, E_FILES = 10, E_A11Y_ON = 11, E_A11Y_ACTION = 12, E_DESTROY = 13, E_EDIT_ACTION = 14, E_DRAG = 15, E_SAVE = 16,
    E_SURFACE = 100 /* (the surface came, changed or went: the state is read again) */
};
enum { DRAG_ENTER = 0, DRAG_OVER = 1, DRAG_LEAVE = 2, DRAG_DROP = 3 };
enum { S_CREATED = 1, S_CHANGED = 2, S_DESTROYED = 3 };
enum { LOOPER_COMMAND = 1 };

typedef struct Msg {
    int kind;
    int* ints;
    int nInts;
    float* floats;
    int nFloats;
    CscdStr text;
    double time; /* when the activity sent it (nowSeconds) */
} Msg;

static double nowSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* What the activity (the system's main thread) shares with the program's thread. */
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int msgRead, msgWrite;     /* the pipe: "the queue has something" */
    JavaVM* vm;
    jobject activity;          /* a global reference; NULL once destroyed */
    jobject assets;            /* (keeps the AAssetManager alive) */
    ANativeWindow* window;     /* the surface, while there is one (acquired) */
    int surfaceW, surfaceH;
    bool destroyed;
    bool appRunning;          /* the program's thread is in main */
    bool restart;             /* main runs again once it returns (a new activity came meanwhile) */
    ALooper* looper;          /* the program's thread's */
    CscdWindow* win;          /* the window, while it is open */
    bool opening;             /* cscdWindowNew is waiting for an activity's surface */
    ANativeWindow* surfaceInUse; /* what the window has taken: the callbacks wait for it to let go */
    CSCD_VEC(Msg) queue;
    /* the system's settings, as last sent (also before the window is made) */
    CscdSystemPreferences prefs;
    int activityGen;           /* counts the activities: one taking over a window gets its state told again */
    float density;
} A = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, -1, -1};

/* the activity's methods, found once */
static struct {
    jmethodID setFullscreen, setBackdrop, setGestureExclusion, finishApp, setClipboard, getClipboard, setClipboardImage, getClipboardFiles, openUrl, share,
        haptic, pickFiles, saveFile, savedFile, setInput, setTextState, setAccessibleTree, accessibleEvent, showEditMenu, magnify;
} J;

static JNIEnv* appEnv; /* the program's thread's (attached while main runs) */

struct CscdWindowNative {
    CscdWindow* w;
    ANativeWindow* surface;
    bool focused;
    int lastButtons; /* the mouse buttons down (1 left, 2 right, 4 middle) */
    ClickCounter clicks;
    /* the keyboard and the field's text, as the activity has them */
    CscdInputTraits traits;
    bool traitsSent;
    CscdTextState javaState;
    bool javaHasState;
    /* TalkBack */
    bool a11yOn, a11yDirty;
    CscdStr a11yLast; /* the tree last sent, to send it only when it changed */
    double a11ySent;  /* when (frames alone send it a few times a second at most) */
    CSCD_VEC(int) a11yEvents; /* (event, element id) pairs, told once the tree is sent */
    CscdDataTransfer* drop; /* a drag from another app over the page (a reference) */
    int activityGen;        /* the activity last told the window's state (A.activityGen) */
};

static void wakeApp(void) {
    const char c = 1;
    if (A.msgWrite >= 0 && write(A.msgWrite, &c, 1) != 1) { /* (full: the thread has enough to wake for) */ }
}

static void push(Msg m) {
    pthread_mutex_lock(&A.mutex);
    cscdVecPush(&A.queue, m);
    pthread_mutex_unlock(&A.mutex);
    wakeApp();
}

static void msgFree(Msg* m) {
    cscdFree(m->ints);
    cscdFree(m->floats);
    cscdStrFree(&m->text);
}

/* ---- standard error into the log -------------------------------------------------------------------- */

static void* logThread(void* arg) {
    const int fd = (int)(intptr_t)arg;
    char buf[1024];
    size_t have = 0;
    for (;;) {
        const ssize_t n = read(fd, buf + have, sizeof buf - 1 - have);
        if (n <= 0) break;
        have += (size_t)n;
        /* whole lines; a line longer than the buffer goes in pieces */
        size_t start = 0;
        for (size_t i = 0; i < have; ++i)
            if (buf[i] == '\n') {
                buf[i] = 0;
                if (i > start) __android_log_write(ANDROID_LOG_INFO, "cascade", buf + start);
                start = i + 1;
            }
        if (start == 0 && have == sizeof buf - 1) {
            buf[have] = 0;
            __android_log_write(ANDROID_LOG_INFO, "cascade", buf);
            have = 0;
        } else {
            memmove(buf, buf + start, have - start);
            have -= start;
        }
    }
    return NULL;
}

static void redirectStderr(void) {
    static bool done = false;
    if (done) return;
    done = true;
    int fds[2];
    if (pipe(fds) != 0) return;
    setvbuf(stderr, NULL, _IOLBF, 0);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    pthread_t t;
    if (pthread_create(&t, NULL, logThread, (void*)(intptr_t)fds[0]) == 0) pthread_detach(t);
}

/* ---- the software presenter: the frame into the window's buffers ----------------------------------- */

typedef struct SoftwarePresenter { CscdPresenter base; ANativeWindow* window; } SoftwarePresenter;

static CscdBackend swBackend(const CscdPresenter* p) { (void)p; return CSCD_BACKEND_SOFTWARE; }
static const char* swDescription(const CscdPresenter* p) { (void)p; return "Software (ANativeWindow)"; }
static void swResize(CscdPresenter* p, int w, int h) { (void)p; (void)w; (void)h; }

static void swDestroy(CscdPresenter* bp) {
    SoftwarePresenter* p = (SoftwarePresenter*)bp;
    ANativeWindow_release(p->window);
    cscdFree(p);
}

static bool swPresent(CscdPresenter* bp, const CscdImage* frame) {
    SoftwarePresenter* p = (SoftwarePresenter*)bp;
    if (frame->width <= 0 || frame->height <= 0) return true;
    /* what changed (the system widens it to what the buffer it hands out lacks) */
    ARect dirty = {0, 0, frame->width, frame->height};
    if (bp->partial && bp->damageCount) {
        dirty.left = dirty.top = INT32_MAX;
        dirty.right = dirty.bottom = 0;
        for (size_t i = 0; i < bp->damageCount; ++i) {
            const CscdIRect r = bp->damage[i];
            if (r.w <= 0 || r.h <= 0) continue;
            dirty.left = cscdMini(dirty.left, r.x);
            dirty.top = cscdMini(dirty.top, r.y);
            dirty.right = cscdMaxi(dirty.right, r.x + r.w);
            dirty.bottom = cscdMaxi(dirty.bottom, r.y + r.h);
        }
        if (dirty.right <= dirty.left) return true;
    }
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(p->window, &buf, &dirty) != 0) return true; /* (the surface is going) */
    if (buf.format == WINDOW_FORMAT_RGBA_8888 || buf.format == WINDOW_FORMAT_RGBX_8888) {
        const int x0 = cscdMaxi(0, dirty.left), y0 = cscdMaxi(0, dirty.top);
        const int x1 = cscdMini(cscdMini(buf.width, frame->width), dirty.right);
        const int y1 = cscdMini(cscdMini(buf.height, frame->height), dirty.bottom);
        for (int y = y0; y < y1; ++y) {
            const uint32_t* src = cscdImageRow(frame, y);
            uint32_t* dst = (uint32_t*)buf.bits + (size_t)y * (size_t)buf.stride;
            /* BGRA to RGBA: red and blue change places */
            for (int x = x0; x < x1; ++x) {
                const uint32_t s = src[x];
                dst[x] = (s & 0xFF00FF00u) | ((s >> 16) & 0xFFu) | ((s & 0xFFu) << 16);
            }
        }
    }
    ANativeWindow_unlockAndPost(p->window);
    return true;
}

static const CscdPresenterVtbl swVtbl = {swBackend, swPresent, swResize, swDescription, swDestroy};

CscdPresenter* platformSoftwarePresenter(void* nativeWindow, int width, int height) {
    (void)width; (void)height;
    if (!nativeWindow) return NULL; /* (no surface now) */
    SoftwarePresenter* p = CSCD_NEW(SoftwarePresenter);
    p->base.vt = &swVtbl;
    p->window = (ANativeWindow*)nativeWindow;
    ANativeWindow_acquire(p->window);
    ANativeWindow_setBuffersGeometry(p->window, 0, 0, WINDOW_FORMAT_RGBX_8888); /* (the window's own size) */
    return &p->base;
}

/* OpenGL here would be OpenGL ES, which lacks what gl_backend.c draws with: Vulkan or software. */
CscdPresenter* platformGLPresenter(void* nativeWindow, int width, int height) {
    (void)nativeWindow; (void)width; (void)height;
    return NULL;
}

/* ---- calling the activity (from the program's thread) ------------------------------------------------ */

static jbyteArray jbytes(CscdSv s) {
    jbyteArray a = (*appEnv)->NewByteArray(appEnv, (jsize)s.len);
    if (a && s.len) (*appEnv)->SetByteArrayRegion(appEnv, a, 0, (jsize)s.len, (const jbyte*)s.ptr);
    return a;
}

static void bytesInto(JNIEnv* env, jbyteArray a, CscdStr* out) {
    if (!a) return;
    const jsize n = (*env)->GetArrayLength(env, a);
    const size_t at = out->len;
    cscdStrAppendN(out, ' ', (size_t)n);
    (*env)->GetByteArrayRegion(env, a, 0, n, (jbyte*)out->ptr + at);
}

/* (a Java exception, which would end the program, is reported instead) */
static bool javaFailed(void) {
    if (!(*appEnv)->ExceptionCheck(appEnv)) return false;
    (*appEnv)->ExceptionDescribe(appEnv);
    (*appEnv)->ExceptionClear(appEnv);
    return true;
}

#define CALL_VOID(method, ...)                                                              \
    do {                                                                                    \
        if (appEnv && A.activity) {                                                         \
            (*appEnv)->CallVoidMethod(appEnv, A.activity, J.method, __VA_ARGS__);           \
            javaFailed();                                                                   \
        }                                                                                   \
    } while (0)

static void deleteLocal(void* ref) { if (ref) (*appEnv)->DeleteLocalRef(appEnv, (jobject)ref); }

/* ---- the clipboard: the system's, through the activity ----------------------------------------------- */

static void clipSetText(void* user, CscdSv text) {
    (void)user;
    if (!appEnv || !A.activity) return;
    jbyteArray b = jbytes(text);
    (*appEnv)->CallVoidMethod(appEnv, A.activity, J.setClipboard, b);
    javaFailed();
    deleteLocal(b);
}

static void clipGetText(void* user, CscdStr* out) {
    (void)user;
    if (!appEnv || !A.activity) return;
    jbyteArray b = (jbyteArray)(*appEnv)->CallObjectMethod(appEnv, A.activity, J.getClipboard);
    if (!javaFailed()) bytesInto(appEnv, b, out);
    deleteLocal(b);
}

/* a picture: PNG bytes, which the activity puts where other apps read them (CascadeFiles) */
static void clipSetImage(void* user, const CscdImage* img) {
    (void)user;
    if (!appEnv || !A.activity || img->width <= 0 || img->height <= 0) return;
    CscdBytes png = {0};
    cscdEncodePNG(img, &png);
    jbyteArray b = jbytes(cscdSvMake((const char*)png.items, png.len));
    cscdVecFree(&png);
    CALL_VOID(setClipboardImage, b);
    deleteLocal(b);
}

/* the clipboard's files (copies the activity made), one path per line */
static void splitLines(CscdSv rest, CscdStrVec* out) {
    while (rest.len) {
        const size_t nl = cscdSvFindChar(rest, '\n', 0);
        const CscdSv line = cscdSvSub(rest, 0, nl == CSCD_NPOS ? rest.len : nl);
        rest = nl == CSCD_NPOS ? CSCD_SV("") : cscdSvSub(rest, nl + 1, rest.len - nl - 1);
        if (line.len) cscdVecPush(out, cscdStrDup(line));
    }
}

static void clipGetFiles(void* user, CscdStrVec* out) {
    (void)user;
    if (!appEnv || !A.activity) return;
    jbyteArray b = (jbyteArray)(*appEnv)->CallObjectMethod(appEnv, A.activity, J.getClipboardFiles);
    CscdStr paths = {0};
    if (!javaFailed()) bytesInto(appEnv, b, &paths);
    deleteLocal(b);
    splitLines(cscdStrSv(&paths), out);
    cscdStrFree(&paths);
}

/* ---- touch selection: the system's floating edit menu and magnifier ---------------------------------- */

static void editMenu(void* user, bool show, CscdRect r, unsigned actions) {
    CscdWindow* w = (CscdWindow*)user;
    const float d = w->dpr;
    CALL_VOID(showEditMenu, (jboolean)show, (jint)floorf(r.x * d), (jint)floorf(r.y * d), (jint)ceilf(cscdRectRight(r) * d),
              (jint)ceilf(cscdRectBottom(r) * d), (jint)actions);
}

static void magnify(void* user, bool show, float x, float y) {
    CscdWindow* w = (CscdWindow*)user;
    CALL_VOID(magnify, (jboolean)show, (jfloat)(x * w->dpr), (jfloat)(y * w->dpr));
}

/* ---- the window's state, taken from the activity's ------------------------------------------------- */

static void releaseAll(CscdWindow* w) {
    CscdWindowNative* n = w->native;
    cscdPresenterFree(w->presenter);
    w->presenter = NULL;
    if (n->surface) ANativeWindow_release(n->surface);
    n->surface = NULL;
    pthread_mutex_lock(&A.mutex);
    A.surfaceInUse = NULL;
    if (A.win == w) A.win = NULL;
    pthread_cond_broadcast(&A.cond);
    pthread_mutex_unlock(&A.mutex);
}

static void resize(CscdWindow* w, int pw, int ph, float dpr) {
    if (pw == w->clientW && ph == w->clientH && dpr == w->dpr) return;
    w->clientW = pw;
    w->clientH = ph;
    w->dpr = dpr;
    if (pw > 0 && ph > 0) {
        if (w->presenter) cscdPresenterResize(w->presenter, pw, ph);
        cscdViewSetViewport(w->view, (float)pw / dpr, (float)ph / dpr, dpr);
    }
    w->dirty = true;
}

/* the surface: taken when it comes, let go of when it goes (the activity waits for that). The window
 * holds a reference of its own, taken with the lock held: a surface that goes between reading it and
 * making the presenter is not freed under it. */
static void syncSurface(CscdWindow* w) {
    CscdWindowNative* n = w->native;
    pthread_mutex_lock(&A.mutex);
    const bool destroyed = A.destroyed;
    ANativeWindow* surface = destroyed ? NULL : A.window;
    const int sw = A.surfaceW, sh = A.surfaceH;
    const bool changed = !destroyed && surface != n->surface;
    if (changed) {
        if (surface) ANativeWindow_acquire(surface);
        A.surfaceInUse = surface;
        pthread_cond_broadcast(&A.cond);
    }
    pthread_mutex_unlock(&A.mutex);
    if (destroyed) {
        releaseAll(w);
        w->hwnd = NULL;
        return;
    }
    if (changed) {
        cscdPresenterFree(w->presenter);
        w->presenter = NULL;
        if (n->surface) ANativeWindow_release(n->surface);
        n->surface = surface;
        if (surface) w->presenter = windowPresenterNew(w, surface, w->requested, sw, sh);
        w->clientW = w->clientH = 0; /* (sized again below) */
    }
    resize(w, surface ? sw : 0, surface ? sh : 0, A.density > 0 ? A.density : 1);
}

/* ---- input ---------------------------------------------------------------------------------------- */

static CscdModifiers modsOf(int32_t meta) {
    CscdModifiers m;
    m.shift = (meta & AMETA_SHIFT_ON) != 0;
    m.ctrl = (meta & AMETA_CTRL_ON) != 0;
    m.alt = (meta & AMETA_ALT_ON) != 0;
    m.meta = (meta & AMETA_META_ON) != 0;
    return m;
}

/* The key from its key code, with its name (KeyboardEvent.key); the characters keys type come from
 * the keyboard's own layout (the activity's KeyCharacterMap). */
static CscdKey mapKey(int32_t code, char name[16]) {
#define K(n, key) { strcpy(name, n); return key; }
    switch (code) {
    case AKEYCODE_DEL: K("Backspace", CSCD_KEY_BACKSPACE)
    case AKEYCODE_FORWARD_DEL: K("Delete", CSCD_KEY_DELETE)
    case AKEYCODE_TAB: K("Tab", CSCD_KEY_TAB)
    case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: case AKEYCODE_DPAD_CENTER: K("Enter", CSCD_KEY_ENTER)
    case AKEYCODE_ESCAPE: K("Escape", CSCD_KEY_ESCAPE)
    case AKEYCODE_DPAD_LEFT: K("ArrowLeft", CSCD_KEY_LEFT)
    case AKEYCODE_DPAD_RIGHT: K("ArrowRight", CSCD_KEY_RIGHT)
    case AKEYCODE_DPAD_UP: K("ArrowUp", CSCD_KEY_UP)
    case AKEYCODE_DPAD_DOWN: K("ArrowDown", CSCD_KEY_DOWN)
    case AKEYCODE_MOVE_HOME: K("Home", CSCD_KEY_HOME)
    case AKEYCODE_MOVE_END: K("End", CSCD_KEY_END)
    case AKEYCODE_PAGE_UP: K("PageUp", CSCD_KEY_PAGE_UP)
    case AKEYCODE_PAGE_DOWN: K("PageDown", CSCD_KEY_PAGE_DOWN)
    case AKEYCODE_INSERT: K("Insert", CSCD_KEY_INSERT)
    case AKEYCODE_SHIFT_LEFT: case AKEYCODE_SHIFT_RIGHT: K("Shift", CSCD_KEY_SHIFT)
    case AKEYCODE_CTRL_LEFT: case AKEYCODE_CTRL_RIGHT: K("Control", CSCD_KEY_CONTROL)
    case AKEYCODE_ALT_LEFT: case AKEYCODE_ALT_RIGHT: K("Alt", CSCD_KEY_ALT)
    case AKEYCODE_META_LEFT: case AKEYCODE_META_RIGHT: K("Meta", CSCD_KEY_META)
    case AKEYCODE_MENU: K("ContextMenu", CSCD_KEY_UNKNOWN)
    case AKEYCODE_SPACE: K(" ", CSCD_KEY_SPACE)
    default: break;
    }
    if (code >= AKEYCODE_F1 && code <= AKEYCODE_F12) {
        snprintf(name, 16, "F%d", code - AKEYCODE_F1 + 1);
        return (CscdKey)((int)CSCD_KEY_F1 + (code - AKEYCODE_F1));
    }
    /* letters by their key, whatever the layout types (shortcuts: Ctrl+A, C, V, X, Z, Y) */
    static const struct { int32_t code; CscdKey key; } kLetters[] = {
        {AKEYCODE_A, CSCD_KEY_A}, {AKEYCODE_C, CSCD_KEY_C}, {AKEYCODE_V, CSCD_KEY_V},
        {AKEYCODE_X, CSCD_KEY_X}, {AKEYCODE_Z, CSCD_KEY_Z}, {AKEYCODE_Y, CSCD_KEY_Y}};
    for (size_t i = 0; i < CSCD_COUNT(kLetters); ++i)
        if (kLetters[i].code == code) {
            name[0] = (char)('a' + (code - AKEYCODE_A));
            name[1] = 0;
            return kLetters[i].key;
        }
    name[0] = 0;
    return CSCD_KEY_CHARACTER;
#undef K
}

static void onKey(CscdWindow* w, const Msg* m) {
    const int action = m->ints[0], code = m->ints[1], meta = m->ints[2], repeat = m->ints[3];
    const uint32_t unicode = (uint32_t)m->ints[4];
    char name[16];
    CscdKey k = mapKey(code, name);
    const CscdModifiers mods = modsOf(meta);
    /* what the key types, in the layout's own characters (a dead accent waits for the next key) */
    char text[8] = {0};
    if (unicode && !(unicode & 0x80000000u) && unicode >= 0x20 && unicode != 0x7F) {
        CscdStr s = {0};
        cscdUtf8Append(&s, unicode);
        if (s.len < sizeof text) memcpy(text, s.ptr, s.len);
        cscdStrFree(&s);
    }
    if (k == CSCD_KEY_CHARACTER) {
        if (!text[0] && !(action == AKEY_EVENT_ACTION_MULTIPLE && m->text.len)) return;
        if (text[0]) strcpy(name, text);
    }
    if (action == AKEY_EVENT_ACTION_MULTIPLE && m->text.len && code == AKEYCODE_UNKNOWN) {
        cscdViewTextInput(w->view, cscdStrSv(&m->text)); /* (a string the input method sent as keys) */
        return;
    }
    if (action == AKEY_EVENT_ACTION_UP) {
        cscdViewKeyUp(w->view, k, mods, cscdSv(name));
        return;
    }
    const int times = action == AKEY_EVENT_ACTION_MULTIPLE ? cscdMaxi(1, repeat) : 1;
    for (int i = 0; i < times; ++i) {
        cscdViewKeyDown(w->view, k, mods, cscdSv(name));
        /* Enter types a line break, as on the other systems; shortcuts type nothing */
        if (mods.ctrl || mods.meta || (mods.alt && !text[0])) continue;
        if (k == CSCD_KEY_ENTER) cscdViewTextInput(w->view, CSCD_SV("\n"));
        else if (k == CSCD_KEY_TAB) continue;
        else if (text[0]) cscdViewTextInput(w->view, cscdSv(text));
    }
}

static int buttonBit(int32_t state) {
    return ((state & AMOTION_EVENT_BUTTON_PRIMARY) ? 1 : 0) | ((state & AMOTION_EVENT_BUTTON_SECONDARY) ? 2 : 0) |
           ((state & AMOTION_EVENT_BUTTON_TERTIARY) ? 4 : 0);
}

static void onMouse(CscdWindow* w, const Msg* m) {
    CscdWindowNative* n = w->native;
    const int kind = m->ints[0];
    const float x = m->floats[0] / w->dpr, y = m->floats[1] / w->dpr;
    const CscdModifiers mods = modsOf(m->ints[2]);
    if (kind == AMOTION_EVENT_ACTION_SCROLL) {
        cscdViewWheel(w->view, x, y, m->floats[2] * 40, -m->floats[3] * 40, mods); /* (a notch: a line, as on the other systems) */
        return;
    }
    if (kind == AMOTION_EVENT_ACTION_HOVER_EXIT) {
        if (!n->lastButtons) cscdViewMouseLeave(w->view);
        return;
    }
    /* presses and releases: the buttons that changed (1 left, 2 right, 4 middle; the view's numbers
     * are 0 left, 1 middle, 2 right) */
    int buttons = buttonBit(m->ints[1]);
    if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_CANCEL) buttons = 0;
    else if (kind == AMOTION_EVENT_ACTION_DOWN && !buttons) buttons = 1; /* (a stylus or an old mouse driver) */
    static const int bits[3] = {1, 4, 2};
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const unsigned time = (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    if (kind == AMOTION_EVENT_ACTION_MOVE || kind == AMOTION_EVENT_ACTION_HOVER_MOVE || kind == AMOTION_EVENT_ACTION_HOVER_ENTER)
        cscdViewMouseMove(w->view, x, y, buttons, mods);
    for (int b = 0; b < 3; ++b) {
        const bool was = (n->lastButtons & bits[b]) != 0, now = (buttons & bits[b]) != 0;
        if (now && !was) cscdViewMouseDown(w->view, x, y, b, mods, clickPress(&n->clicks, time, (int)m->floats[0], (int)m->floats[1], 500));
        else if (was && !now) cscdViewMouseUp(w->view, x, y, b, mods);
    }
    n->lastButtons = buttons;
}

/* ints: the action, its pointer's index, the pointers' count n, their ids, the samples' count; floats: each
 * sample (the moves the system gathered since the last event, then the event's own) as how many ms before
 * it was sent, then the pointers' x and y */
static void onTouch(CscdWindow* w, const Msg* m) {
    const int action = m->ints[0], index = m->ints[1], count = m->ints[2];
    if (count <= 0 || count > 64 || m->nInts < 4 + count) return;
    const int stride = 1 + 2 * count, samples = cscdMini(m->ints[3 + count], m->nFloats / stride);
    if (samples <= 0) return;
    const float dpr = w->dpr;
    CscdView* v = w->view;
    const double now = nowSeconds();
    for (int s = 0; s < samples; ++s) {
        const float* f = m->floats + (size_t)s * (size_t)stride;
        viewSetInputAge(v, now - (m->time - f[0] / 1000.0));
#define ID(i) m->ints[3 + (i)]
#define X(i) (f[1 + 2 * (i)] / dpr)
#define Y(i) (f[2 + 2 * (i)] / dpr)
        switch (action) {
        case AMOTION_EVENT_ACTION_DOWN: case AMOTION_EVENT_ACTION_POINTER_DOWN:
            if (index >= 0 && index < count) cscdViewTouchDown(v, ID(index), X(index), Y(index));
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            for (int i = 0; i < count; ++i) cscdViewTouchMove(v, ID(i), X(i), Y(i));
            break;
        case AMOTION_EVENT_ACTION_UP: case AMOTION_EVENT_ACTION_POINTER_UP:
            if (index >= 0 && index < count) cscdViewTouchUp(v, ID(index), X(index), Y(index));
            break;
        case AMOTION_EVENT_ACTION_CANCEL: cscdViewTouchCancel(v); break;
        default: break;
        }
#undef ID
#undef X
#undef Y
        if (action != AMOTION_EVENT_ACTION_MOVE) break; /* (only a move gathers samples) */
    }
    viewSetInputAge(v, 0);
}

/* The keyboard's action key (Go, Search, Send, Next, Done): Next and Previous move the focus, the
 * others are the Enter key (a form's submission, a search field's search); Done also lets the
 * keyboard go. */
static void onEditorAction(CscdWindow* w, int enter) {
    CscdView* v = w->view;
    const CscdModifiers none = {false, false, false, false}, shift = {true, false, false, false};
    if (enter == CSCD_ENTER_NEXT || enter == CSCD_ENTER_PREVIOUS) {
        const CscdModifiers m = enter == CSCD_ENTER_PREVIOUS ? shift : none;
        cscdViewKeyDown(v, CSCD_KEY_TAB, m, CSCD_SV("Tab"));
        cscdViewKeyUp(v, CSCD_KEY_TAB, m, CSCD_SV("Tab"));
        return;
    }
    cscdViewKeyDown(v, CSCD_KEY_ENTER, none, CSCD_SV("Enter"));
    cscdViewKeyUp(v, CSCD_KEY_ENTER, none, CSCD_SV("Enter"));
    if (enter == CSCD_ENTER_DONE) cscdViewBlur(v);
}

/* Back closes what is open over the page (Escape), the selection's handles, lets the keyboard go, or
 * else ends the app. */
static void onBack(CscdWindow* w) {
    CscdView* v = w->view;
    const CscdModifiers none = {false, false, false, false};
    if (cscdViewSelectionHandlesShown(v)) cscdViewHideSelectionHandles(v);
    else if (cscdViewMenuOpen(v) || cscdViewModalDialog(v)) {
        cscdViewKeyDown(v, CSCD_KEY_ESCAPE, none, CSCD_SV("Escape"));
        cscdViewKeyUp(v, CSCD_KEY_ESCAPE, none, CSCD_SV("Escape"));
    } else if (cscdViewCaretRect(v).h > 0) cscdViewBlur(v);
    else cscdWindowClose(w);
}

/* ---- dragged in from other apps ------------------------------------------------------------------------ */

/* While the drag is over the page, the page sees its kinds (text, or not); at the drop, what it
 * carries: its text and the activity's copies of its files. */
static void onDrag(CscdWindow* w, const Msg* m) {
    CscdWindowNative* n = w->native;
    CscdView* v = w->view;
    const int kind = m->ints[0];
    const float x = m->nFloats >= 2 ? m->floats[0] / w->dpr : 0, y = m->nFloats >= 2 ? m->floats[1] / w->dpr : 0;
    if (kind == DRAG_ENTER || (kind == DRAG_OVER && !n->drop)) {
        CscdDataTransfer* dt = cscdDataTransferNew();
        cscdStrSetC(&dt->effectAllowed, "copy");
        if (m->ints[1]) cscdDataTransferSetData(dt, CSCD_SV("text/plain"), CSCD_SV(""));
        if (n->drop) cscdRelease(n->drop);
        n->drop = dt;
        cscdViewExternalDragEnter(v, dt, x, y);
    }
    if (kind == DRAG_ENTER || kind == DRAG_OVER) {
        cscdViewExternalDragOver(v, x, y);
        return;
    }
    cscdViewExternalDragLeave(v);
    if (n->drop) cscdRelease(n->drop);
    n->drop = NULL;
    if (kind != DRAG_DROP) return;
    /* the text, a 0 byte, the paths one per line */
    const CscdSv all = cscdStrSv(&m->text);
    const size_t zero = cscdSvFindChar(all, '\0', 0);
    const CscdSv text = cscdSvSub(all, 0, zero == CSCD_NPOS ? all.len : zero);
    CscdDataTransfer* dt = cscdDataTransferNew();
    cscdStrSetC(&dt->effectAllowed, "copy");
    if (zero != CSCD_NPOS) splitLines(cscdSvSub(all, zero + 1, all.len - zero - 1), &dt->files);
    if (text.len) cscdDataTransferSetData(dt, CSCD_SV("text/plain"), text);
    cscdViewExternalDragEnter(v, dt, x, y);
    cscdViewExternalDragOver(v, x, y);
    cscdViewExternalDrop(v, x, y);
    cscdRelease(dt);
}

/* ---- TalkBack: the accessible tree, copied to the activity --------------------------------------------- */

typedef struct TreeOut {
    CSCD_VEC(int) ints;
    CSCD_VEC(float) floats;
    CscdStr strings;
} TreeOut;

static void walkTree(CscdWindow* w, CscdElement* e, int parent, TreeOut* out, int depth) {
    CscdView* v = w->view;
    CscdPtrVec kids = {0};
    cscdAccessibleChildren(v, e, &kids);
    for (size_t i = 0; i < kids.len && out->ints.len < 10 * 5000; ++i) {
        CscdElement* c = (CscdElement*)kids.items[i];
        const int id = (int)cscdAccessibleId(v, c);
        const CscdRect r = cscdViewElementRect(v, c);
        const float dpr = w->dpr;
        double value = 0, min = 0, max = 0, step = 0;
        const bool range = cscdAccessibleRange(c, &value, &min, &max, &step);
        const int ints[10] = {id, parent, (int)cscdAccessibleRole(c), (int)cscdAccessibleState(v, c), cscdAccessibleLevel(c),
                              (int)floorf(r.x * dpr), (int)floorf(r.y * dpr), (int)ceilf(cscdRectRight(r) * dpr),
                              (int)ceilf(cscdRectBottom(r) * dpr), range ? 1 : 0};
        for (int k = 0; k < 10; ++k) cscdVecPush(&out->ints, ints[k]);
        cscdVecPush(&out->floats, (float)value);
        cscdVecPush(&out->floats, (float)min);
        cscdVecPush(&out->floats, (float)max);
        cscdVecPush(&out->floats, (float)step);
        cscdAccessibleName(v, c, &out->strings);
        cscdStrAppendChar(&out->strings, 0);
        cscdAccessibleDescription(v, c, &out->strings);
        cscdStrAppendChar(&out->strings, 0);
        cscdAccessibleValue(v, c, &out->strings);
        cscdStrAppendChar(&out->strings, 0);
        if (depth < 64) walkTree(w, c, id, out, depth + 1);
    }
    cscdVecFree(&kids);
}

static void sendTree(CscdWindow* w) {
    CscdWindowNative* n = w->native;
    n->a11yDirty = false;
    n->a11ySent = nowSeconds();
    TreeOut t = {0};
    walkTree(w, NULL, -1, &t, 0);
    /* sent only when something in it changed */
    CscdStr all = {0};
    cscdStrAppend(&all, cscdSvMake((const char*)t.ints.items, t.ints.len * sizeof(int)));
    cscdStrAppend(&all, cscdSvMake((const char*)t.floats.items, t.floats.len * sizeof(float)));
    cscdStrAppend(&all, cscdStrSv(&t.strings));
    if (!cscdStrEq(&all, &n->a11yLast) && appEnv && A.activity) {
        jintArray ji = (*appEnv)->NewIntArray(appEnv, (jsize)t.ints.len);
        jfloatArray jf = (*appEnv)->NewFloatArray(appEnv, (jsize)t.floats.len);
        if (ji && t.ints.len) (*appEnv)->SetIntArrayRegion(appEnv, ji, 0, (jsize)t.ints.len, t.ints.items);
        if (jf && t.floats.len) (*appEnv)->SetFloatArrayRegion(appEnv, jf, 0, (jsize)t.floats.len, t.floats.items);
        jbyteArray js = jbytes(cscdStrSv(&t.strings));
        (*appEnv)->CallVoidMethod(appEnv, A.activity, J.setAccessibleTree, ji, jf, js);
        javaFailed();
        deleteLocal(ji);
        deleteLocal(jf);
        deleteLocal(js);
        cscdStrFree(&n->a11yLast);
        n->a11yLast = all;
    } else cscdStrFree(&all);
    cscdVecFree(&t.ints);
    cscdVecFree(&t.floats);
    cscdStrFree(&t.strings);
}

/* (told after the tree is sent again: the event names an element of it) */
static void onAccessibleEvent(void* user, CscdElement* e, CscdAccessibleEvent event) {
    CscdWindow* w = (CscdWindow*)user;
    CscdWindowNative* n = w->native;
    if (!n || !n->a11yOn) return;
    n->a11yDirty = true;
    cscdVecPush(&n->a11yEvents, (int)event);
    cscdVecPush(&n->a11yEvents, e ? (int)cscdAccessibleId(w->view, e) : -1);
}

static void sendAccessibleEvents(CscdWindow* w) {
    CscdWindowNative* n = w->native;
    for (size_t i = 0; i + 1 < n->a11yEvents.len; i += 2) CALL_VOID(accessibleEvent, (jint)n->a11yEvents.items[i], (jint)n->a11yEvents.items[i + 1]);
    n->a11yEvents.len = 0;
}

/* ---- the keyboard and the focused field's text -------------------------------------------------------- */

static bool textStateEq(const CscdTextState* a, const CscdTextState* b) {
    return cscdStrEq(&a->text, &b->text) && a->selStart == b->selStart && a->selEnd == b->selEnd && a->composeStart == b->composeStart &&
           a->composeEnd == b->composeEnd;
}

/* What the activity is told after each turn of the loop: the keyboard the focused field wants (none
 * without one) and the field's text, when either changed other than through the input method. */
static void syncKeyboard(CscdWindow* w) {
    CscdWindowNative* n = w->native;
    const CscdInputTraits t = cscdViewInputTraits(w->view);
    CscdTextState now;
    const bool field = cscdViewGetTextState(w->view, &now);
    if (field && (!n->javaHasState || !textStateEq(&now, &n->javaState))) {
        jbyteArray b = jbytes(cscdStrSv(&now.text));
        CALL_VOID(setTextState, b, (jint)now.selStart, (jint)now.selEnd, (jint)now.composeStart, (jint)now.composeEnd);
        deleteLocal(b);
        cscdTextStateFini(&n->javaState);
        n->javaState = now;
        n->javaHasState = true;
    } else cscdTextStateFini(&now);
    if (!field && n->javaHasState) {
        cscdTextStateFini(&n->javaState);
        n->javaHasState = false;
    }
    if (!n->traitsSent || !cscdInputTraitsEq(t, n->traits)) {
        n->traits = t;
        n->traitsSent = true;
        CALL_VOID(setInput, (jint)t.kind, (jint)t.enter, (jint)t.capitalize, (jboolean)t.autocorrect);
    }
}

/* ---- what the activity sent ------------------------------------------------------------------------- */

static void handle(CscdWindow* w, Msg* m) {
    CscdWindowNative* n = w->native;
    CscdView* v = w->view;
    switch (m->kind) {
    case E_SURFACE: syncSurface(w); break;
    case E_TOUCH: if (m->nInts >= 4) onTouch(w, m); break;
    case E_MOUSE: if (m->nInts >= 3 && m->nFloats >= 4) onMouse(w, m); break;
    case E_KEY: if (m->nInts >= 5) onKey(w, m); break;
    case E_TEXT_STATE: {
        if (m->nInts < 4) break;
        CscdTextState s = {m->text, (size_t)m->ints[0], (size_t)m->ints[1], (size_t)m->ints[2], (size_t)m->ints[3]};
        cscdViewSetTextState(v, &s);
        /* what the input method has now: not told back to it (unless the field changed it) */
        cscdTextStateFini(&n->javaState);
        n->javaState = s;
        n->javaHasState = true;
        memset(&m->text, 0, sizeof m->text); /* (taken) */
        break;
    }
    case E_EDITOR_ACTION: if (m->nInts >= 1) onEditorAction(w, m->ints[0]); break;
    case E_FOCUS:
        if (m->nInts >= 1) {
            const bool focused = m->ints[0] != 0;
            if (n->focused && !focused) {
                /* menus and tooltips do not outlive the window's turn in front */
                cscdViewCloseMenus(v);
                cscdViewHideTooltip(v);
            }
            n->focused = focused;
        }
        break;
    case E_APP:
        if (m->nInts >= 1) windowAppEvent(w, m->ints[0] == 0 ? CSCD_APP_BACKGROUND : m->ints[0] == 1 ? CSCD_APP_FOREGROUND : CSCD_APP_LOW_MEMORY);
        break;
    case E_PREFERENCES:
        windowSystemPreferences(w, A.prefs);
        resize(w, w->clientW, w->clientH, A.density > 0 ? A.density : 1);
        break;
    case E_BACK: onBack(w); break;
    case E_FILES: {
        CscdStrVec paths = {0};
        if (m->nInts >= 1 && m->ints[0]) splitLines(cscdStrSv(&m->text), &paths);
        windowFilesPicked(w, &paths);
        cscdStrVecFree(&paths);
        break;
    }
    case E_SAVE: {
        /* where to write (a file in the cache: the activity copies it to the place chosen), or cancelled */
        const bool chosen = m->nInts >= 1 && m->ints[0] && m->text.len;
        const bool wrote = windowSaveChosen(w, chosen ? cscdStrC(&m->text) : NULL);
        if (chosen) CALL_VOID(savedFile, (jboolean)wrote);
        break;
    }
    case E_EDIT_ACTION: if (m->nInts >= 1 && m->ints[0] >= 0 && m->ints[0] <= CSCD_EDIT_SELECT_ALL) cscdViewEditAction(v, (CscdEditAction)m->ints[0]); break;
    case E_DRAG: if (m->nInts >= 2) onDrag(w, m); break;
    case E_A11Y_ON:
#ifndef CASCADE_NO_ACCESSIBILITY /* (TalkBack: CASCADE_ACCESSIBILITY) */
        if (m->nInts >= 1) {
            n->a11yOn = m->ints[0] != 0;
            n->a11yDirty = n->a11yOn;
            cscdStrClear(&n->a11yLast);
        }
#endif
        break;
    case E_A11Y_ACTION: {
        if (m->nInts < 2) break;
        CscdElement* e = cscdAccessibleElement(v, (uint32_t)m->ints[0]);
        if (e) cscdAccessibleDo(v, e, (CscdAccessibleAction)m->ints[1], cscdStrSv(&m->text));
        n->a11yDirty = true;
        break;
    }
    case E_DESTROY: syncSurface(w); break;
    default: break;
    }
}

/* ---- the window ----------------------------------------------------------------------------------- */

void windowCreateNative(CscdWindow* w, const CscdWindowOptions* opts) {
    (void)opts;
    CscdWindowNative* n = CSCD_NEW(CscdWindowNative);
    w->native = n;
    n->w = w;
    n->focused = true;
    /* one window, the activity's: wait for its surface (it comes soon after the activity starts). An
     * activity the system made again meanwhile (a configuration change) ends without one: the next
     * activity's is waited for, a few seconds at most */
    pthread_mutex_lock(&A.mutex);
    A.opening = true;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 5;
    for (;;) {
        if (A.window && A.activity && !A.destroyed) break;
        if (A.activity && !A.destroyed) {
            pthread_cond_wait(&A.cond, &A.mutex); /* (the surface comes, or the activity goes) */
            continue;
        }
        if (pthread_cond_timedwait(&A.cond, &A.mutex, &until) != 0) break; /* (no activity came again) */
    }
    A.opening = false;
    const bool open = A.window && A.activity && !A.destroyed && !A.win;
    if (!open)
        fprintf(stderr, "cascade: no window: %s\n", !A.activity || A.destroyed ? "the activity has ended" : A.win ? "one is open already" : "no surface");
    if (open) {
        A.win = w;
        w->hwnd = A.activity;
        n->activityGen = A.activityGen;
    }
    pthread_mutex_unlock(&A.mutex);
    if (!open) return;
    w->dpr = A.density > 0 ? A.density : 1;
    syncSurface(w);
    windowSystemPreferences(w, A.prefs);
    cscdViewSetClipboardHooks(w->view, clipSetText, clipGetText, clipSetImage, clipGetFiles, w);
    cscdViewOnAccessibleEvent(w->view, onAccessibleEvent, w);
    cscdViewOnEditMenu(w->view, editMenu, w);
    cscdViewOnMagnifier(w->view, magnify, w);
}

void windowDestroyNative(CscdWindow* w) {
    CscdWindowNative* n = w->native;
    cscdViewOnEditMenu(w->view, NULL, NULL); /* (its menu goes with the window) */
    cscdViewOnMagnifier(w->view, NULL, NULL);
    if (w->hwnd) releaseAll(w);
    w->hwnd = NULL;
    cscdViewOnAccessibleEvent(w->view, NULL, NULL);
    if (n->drop) cscdRelease(n->drop);
    cscdTextStateFini(&n->javaState);
    cscdStrFree(&n->a11yLast);
    cscdVecFree(&n->a11yEvents);
    cscdFree(n);
    w->native = NULL;
}

void* windowSurface(const CscdWindow* w) { return w->native->surface; }
void windowShowTitle(CscdWindow* w, const char* title) { (void)w; (void)title; /* (no title bar) */ }
void windowPlaceInputMethod(CscdWindow* w) { (void)w; }

void windowWake(CscdWindow* w) {
    (void)w;
    if (A.looper) ALooper_wake(A.looper);
}

static jintArray jints(const CscdU32Vec* v) {
    jintArray a = (*appEnv)->NewIntArray(appEnv, (jsize)v->len);
    if (a && v->len) (*appEnv)->SetIntArrayRegion(appEnv, a, 0, (jsize)v->len, (const jint*)v->items);
    return a;
}

void windowSetBackdrop(CscdWindow* w, const CscdWindowBackdrop* b) {
    (void)w;
    if (!appEnv || !A.activity) return;
    jintArray left = jints(&b->left), right = jints(&b->right);
    CALL_VOID(setBackdrop, (jint)b->top, (jint)b->bottom, left, right);
    deleteLocal(left);
    deleteLocal(right);
}

void windowSetGestureExclusion(CscdWindow* w, const CscdIRect* rects, size_t count) {
    (void)w;
    if (!appEnv || !A.activity) return;
    jintArray a = (*appEnv)->NewIntArray(appEnv, (jsize)(4 * count));
    for (size_t i = 0; a && i < count; ++i) {
        const jint r[4] = {rects[i].x, rects[i].y, rects[i].x + rects[i].w, rects[i].y + rects[i].h};
        (*appEnv)->SetIntArrayRegion(appEnv, a, (jsize)(4 * i), 4, r);
    }
    CALL_VOID(setGestureExclusion, a);
    deleteLocal(a);
}

void cscdWindowShow(CscdWindow* w) {
    w->shown = true;
    w->dirty = true;
}

void cscdWindowResize(CscdWindow* w, int width, int height) { (void)w; (void)width; (void)height; /* (the system sizes it) */ }

void cscdWindowMaximize(CscdWindow* w) { cscdWindowShow(w); }

void cscdWindowSetFullscreen(CscdWindow* w, bool on) {
    if (!w->hwnd) return;
    w->fullscreen = on;
    CALL_VOID(setFullscreen, (jboolean)on);
}

void cscdWindowClose(CscdWindow* w) {
    if (!w->hwnd) return;
    if (w->onClose && !w->onClose(w->onCloseUser)) return;
    releaseAll(w);
    w->hwnd = NULL;
    if (appEnv && A.activity && !A.destroyed) {
        (*appEnv)->CallVoidMethod(appEnv, A.activity, J.finishApp);
        javaFailed();
    }
}

/* A new activity took the window over (the system made one again): it knows nothing yet of the
 * backdrop, the keyboard the field wants, its text, the accessible tree or the bars hidden. */
static void newActivity(CscdWindow* w, int gen) {
    CscdWindowNative* n = w->native;
    n->activityGen = gen;
    windowBackdropForget(w);
    n->traitsSent = false;
    cscdTextStateFini(&n->javaState);
    n->javaHasState = false;
    cscdStrClear(&n->a11yLast);
    n->a11yDirty = n->a11yOn;
    if (w->fullscreen) CALL_VOID(setFullscreen, (jboolean)true);
    w->dirty = true;
}

bool cscdWindowPump(CscdWindow* w, bool wait) {
    if (!w->hwnd || !w->native) return false;
    CscdWindowNative* n = w->native;
    pthread_mutex_lock(&A.mutex);
    const int gen = A.activityGen;
    pthread_mutex_unlock(&A.mutex);
    if (gen != n->activityGen) newActivity(w, gen);
    int timeout = 0;
    pthread_mutex_lock(&A.mutex);
    const bool queued = A.queue.len != 0;
    pthread_mutex_unlock(&A.mutex);
    if (wait && !queued && !windowHasPosted(w) && !w->dirty && !(cscdViewNeedsPaint(w->view) && !w->background)) {
        timeout = w->onFrame && !w->background ? 0 : cscdViewNextTimerMs(w->view); /* (-1: until something comes) */
    }
    for (;;) {
        int events = 0;
        void* data = NULL;
        const int id = ALooper_pollOnce(timeout, NULL, &events, &data);
        if (id == ALOOPER_POLL_CALLBACK) continue;
        if (id < 0) break; /* (timed out, or woken) */
        timeout = 0;
        if (id == LOOPER_COMMAND) {
            char buf[64];
            while (read(A.msgRead, buf, sizeof buf) == (ssize_t)sizeof buf) {}
        }
    }
    /* what the activity sent, in order */
    pthread_mutex_lock(&A.mutex);
    Msg* msgs = A.queue.items;
    const size_t count = A.queue.len;
    memset(&A.queue, 0, sizeof A.queue);
    pthread_mutex_unlock(&A.mutex);
    bool input = false;
    for (size_t i = 0; i < count; ++i) {
        if (w->hwnd) {
            handle(w, &msgs[i]);
            input = input || msgs[i].kind != E_SURFACE;
        }
        msgFree(&msgs[i]);
    }
    cscdFree(msgs);
    if (!w->hwnd) return false;
    windowRunPosted(w);
    if (!w->hwnd) return false;
    const bool render = cscdViewUpdate(w->view) || w->onFrame || w->dirty;
    if (render) windowRender(w);
    if (render || input) syncKeyboard(w);
    /* (a frame that only animated, the caret's blink, does not walk the whole tree each time) */
    if (n->a11yOn && (n->a11yDirty || (render && nowSeconds() - n->a11ySent > 0.25))) sendTree(w);
    if (n->a11yEvents.len) sendAccessibleEvents(w);
    return w->hwnd != NULL;
}

/* ---- the system around the app ------------------------------------------------------------------- */

bool platformOpenUrl(CscdWindow* w, const char* url) {
    (void)w;
    if (!appEnv || !A.activity) return false;
    jbyteArray b = jbytes(cscdSv(url));
    const jboolean ok = (*appEnv)->CallBooleanMethod(appEnv, A.activity, J.openUrl, b);
    deleteLocal(b);
    return !javaFailed() && ok;
}

bool platformShareText(CscdWindow* w, const char* text) {
    (void)w;
    if (!appEnv || !A.activity) return false;
    jbyteArray b = jbytes(cscdSv(text));
    const jboolean ok = (*appEnv)->CallBooleanMethod(appEnv, A.activity, J.share, b);
    deleteLocal(b);
    return !javaFailed() && ok;
}

bool platformPickFiles(CscdWindow* w, const char* accept, bool multiple) {
    (void)w;
    if (!appEnv || !A.activity) return false;
    jbyteArray b = jbytes(cscdSv(accept));
    const jboolean ok = (*appEnv)->CallBooleanMethod(appEnv, A.activity, J.pickFiles, b, (jboolean)multiple);
    deleteLocal(b);
    return !javaFailed() && ok;
}

/* (no tray, no menu bar of the system's, no hiding the app's window) */
bool platformSetMenuBar(CscdWindow* w, CscdMenu* menu) { (void)w; (void)menu; return false; }
bool platformTrayCreate(CscdTray* t, const CscdImage* icon, const char* tooltip) { (void)t; (void)icon; (void)tooltip; return false; }
void platformTraySet(CscdTray* t, const CscdImage* icon, const char* tooltip) { (void)t; (void)icon; (void)tooltip; }
void platformTrayDestroy(CscdTray* t) { (void)t; }
bool platformHide(CscdWindow* w) { (void)w; return false; }

bool platformSaveFile(CscdWindow* w, const char* name, const char* accept) {
    (void)w;
    if (!appEnv || !A.activity) return false;
    jbyteArray b = jbytes(cscdSv(name));
    jbyteArray a = jbytes(cscdSv(accept));
    const jboolean ok = (*appEnv)->CallBooleanMethod(appEnv, A.activity, J.saveFile, b, a);
    deleteLocal(b);
    deleteLocal(a);
    return !javaFailed() && ok;
}

void platformHaptic(CscdWindow* w, CscdHaptic kind) {
    (void)w;
    CALL_VOID(haptic, (jint)kind);
}

/* ---- the activity's side (the system's main thread) ------------------------------------------------- */

static int* copyInts(JNIEnv* env, jintArray a, int* n) {
    *n = a ? (*env)->GetArrayLength(env, a) : 0;
    if (!*n) return NULL;
    int* p = cscdMalloc((size_t)*n * sizeof(int));
    (*env)->GetIntArrayRegion(env, a, 0, *n, p);
    return p;
}

static float* copyFloats(JNIEnv* env, jfloatArray a, int* n) {
    *n = a ? (*env)->GetArrayLength(env, a) : 0;
    if (!*n) return NULL;
    float* p = cscdMalloc((size_t)*n * sizeof(float));
    (*env)->GetFloatArrayRegion(env, a, 0, *n, p);
    return p;
}

static void* appThread(void* arg) {
    (void)arg;
    JNIEnv* env = NULL;
    (*A.vm)->AttachCurrentThread(A.vm, &env, NULL);
    appEnv = env;
    ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ALooper_addFd(looper, A.msgRead, LOOPER_COMMAND, ALOOPER_EVENT_INPUT, NULL, NULL);
    A.looper = looper;
    char program[] = "cascade";
    char* argv[] = {program, NULL};
    for (;;) {
        cscdAppMain(1, argv);
        fflush(stdout);
        fflush(stderr);
        pthread_mutex_lock(&A.mutex);
        if (!A.restart) break;
        /* the activity was started again while main was ending: main runs again for it */
        A.restart = false;
        pthread_mutex_unlock(&A.mutex);
    }
    /* main returned: the activity ends too */
    if (A.activity && !A.destroyed) {
        (*env)->CallVoidMethod(env, A.activity, J.finishApp);
        javaFailed();
    }
    A.appRunning = false;
    A.win = NULL;
    pthread_cond_broadcast(&A.cond);
    pthread_mutex_unlock(&A.mutex);
    ALooper_removeFd(looper, A.msgRead);
    A.looper = NULL;
    appEnv = NULL;
    (*A.vm)->DetachCurrentThread(A.vm);
    return NULL;
}

#define JNI_FN(name) __attribute__((visibility("default"))) JNIEXPORT void JNICALL Java_org_cascade_CascadeActivity_##name

JNI_FN(nativeCreate)(JNIEnv* env, jclass cls, jobject activity, jobject assets, jstring internalPath) {
    redirectStderr();
    (*env)->GetJavaVM(env, &A.vm);
    J.setFullscreen = (*env)->GetMethodID(env, cls, "setFullscreen", "(Z)V");
    J.setBackdrop = (*env)->GetMethodID(env, cls, "setBackdrop", "(II[I[I)V");
    J.setGestureExclusion = (*env)->GetMethodID(env, cls, "setGestureExclusion", "([I)V");
    J.finishApp = (*env)->GetMethodID(env, cls, "finishApp", "()V");
    J.setClipboard = (*env)->GetMethodID(env, cls, "setClipboard", "([B)V");
    J.getClipboard = (*env)->GetMethodID(env, cls, "getClipboard", "()[B");
    J.setClipboardImage = (*env)->GetMethodID(env, cls, "setClipboardImage", "([B)V");
    J.getClipboardFiles = (*env)->GetMethodID(env, cls, "getClipboardFiles", "()[B");
    J.showEditMenu = (*env)->GetMethodID(env, cls, "showEditMenu", "(ZIIIII)V");
    J.magnify = (*env)->GetMethodID(env, cls, "magnify", "(ZFF)V");
    J.openUrl = (*env)->GetMethodID(env, cls, "openUrl", "([B)Z");
    J.share = (*env)->GetMethodID(env, cls, "share", "([B)Z");
    J.haptic = (*env)->GetMethodID(env, cls, "haptic", "(I)V");
    J.pickFiles = (*env)->GetMethodID(env, cls, "pickFiles", "([BZ)Z");
    J.saveFile = (*env)->GetMethodID(env, cls, "saveFile", "([B[B)Z");
    J.savedFile = (*env)->GetMethodID(env, cls, "savedFile", "(Z)V");
    J.setInput = (*env)->GetMethodID(env, cls, "setInput", "(IIIZ)V");
    J.setTextState = (*env)->GetMethodID(env, cls, "setTextState", "([BIIII)V");
    J.setAccessibleTree = (*env)->GetMethodID(env, cls, "setAccessibleTree", "([I[F[B)V");
    J.accessibleEvent = (*env)->GetMethodID(env, cls, "accessibleEvent", "(II)V");

    /* the program's files: its internal folder is home (and the font cache's), the APK's assets are
     * read by relative paths */
    const char* dir = internalPath ? (*env)->GetStringUTFChars(env, internalPath, NULL) : NULL;
    if (dir) {
        setenv("HOME", dir, 1);
        setenv("XDG_CACHE_HOME", dir, 1);
        if (chdir(dir) != 0) perror("cascade: the internal data folder");
        (*env)->ReleaseStringUTFChars(env, internalPath, dir);
    }

    pthread_mutex_lock(&A.mutex);
    if (A.assets) (*env)->DeleteGlobalRef(env, A.assets);
    A.assets = (*env)->NewGlobalRef(env, assets);
    filesSetAndroidAssets(AAssetManager_fromJava(env, A.assets));
    if (A.msgRead < 0) {
        int fds[2];
        if (pipe(fds) != 0) {
            pthread_mutex_unlock(&A.mutex);
            __android_log_write(ANDROID_LOG_ERROR, "cascade", "no pipe for the activity's messages");
            return;
        }
        fcntl(fds[0], F_SETFL, O_NONBLOCK);
        fcntl(fds[1], F_SETFL, O_NONBLOCK);
        A.msgRead = fds[0];
        A.msgWrite = fds[1];
    }
    if (A.activity) (*env)->DeleteGlobalRef(env, A.activity);
    A.activity = (*env)->NewGlobalRef(env, activity);
    ++A.activityGen;
    A.destroyed = false;
    A.window = NULL;
    const bool start = !A.appRunning;
    /* a program still running from an activity before, whose window is gone (main is ending): main
     * runs again for this one once it returns; one still making its window takes this activity's */
    if (!start && !A.win && !A.opening) A.restart = true;
    A.appRunning = true;
    pthread_cond_broadcast(&A.cond);
    pthread_mutex_unlock(&A.mutex);
    if (start) {
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 8u << 20); /* (as a desktop main thread has) */
        if (pthread_create(&t, &attr, appThread, NULL) != 0) A.appRunning = false;
        pthread_attr_destroy(&attr);
    }
}

JNI_FN(nativeSurface)(JNIEnv* env, jclass cls, jint kind, jobject surface, jint width, jint height) {
    (void)cls;
    pthread_mutex_lock(&A.mutex);
    if (kind == S_DESTROYED) {
        ANativeWindow* old = A.window;
        A.window = NULL;
        Msg m = {E_SURFACE};
        cscdVecPush(&A.queue, m);
        wakeApp();
        /* the surface goes when this returns: the window lets go of it first */
        while (old && A.surfaceInUse == old && A.win && A.appRunning) pthread_cond_wait(&A.cond, &A.mutex);
        pthread_mutex_unlock(&A.mutex);
        if (old) ANativeWindow_release(old);
        return;
    }
    ANativeWindow* now = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    if (now && now == A.window) ANativeWindow_release(now); /* (the same one, resized: it is held already) */
    else {
        ANativeWindow* old = A.window;
        A.window = now;
        if (old) {
            /* (a new surface without the old one's end: let go of the old once the window has) */
            while (A.surfaceInUse == old && A.win && A.appRunning) {
                Msg m = {E_SURFACE};
                cscdVecPush(&A.queue, m);
                wakeApp();
                pthread_cond_wait(&A.cond, &A.mutex);
            }
            ANativeWindow_release(old);
        }
    }
    if (width > 0 && height > 0) {
        A.surfaceW = width;
        A.surfaceH = height;
    } else if (A.window) {
        A.surfaceW = ANativeWindow_getWidth(A.window);
        A.surfaceH = ANativeWindow_getHeight(A.window);
    }
    pthread_cond_broadcast(&A.cond); /* (cscdWindowNew may be waiting for it) */
    Msg m = {E_SURFACE};
    cscdVecPush(&A.queue, m);
    pthread_mutex_unlock(&A.mutex);
    wakeApp();
}

JNI_FN(nativeEvent)(JNIEnv* env, jclass cls, jint kind, jintArray ints, jfloatArray floats, jbyteArray text) {
    (void)cls;
    Msg m;
    memset(&m, 0, sizeof m);
    m.kind = kind;
    m.time = nowSeconds();
    m.ints = copyInts(env, ints, &m.nInts);
    m.floats = copyFloats(env, floats, &m.nFloats);
    bytesInto(env, text, &m.text);
    if (kind == E_PREFERENCES) {
        /* kept here too: the window takes them when it is made */
        pthread_mutex_lock(&A.mutex);
        if (m.nInts >= 2) {
            A.prefs.dark = m.ints[0] != 0;
            A.prefs.reducedMotion = m.ints[1] != 0;
        }
        if (m.nFloats >= 2) {
            A.prefs.textScale = m.floats[0];
            A.density = m.floats[1];
        }
        pthread_mutex_unlock(&A.mutex);
    }
    if (kind != E_DESTROY) {
        push(m);
        return;
    }
    msgFree(&m);
    pthread_mutex_lock(&A.mutex);
    A.destroyed = true;
    Msg d = {E_DESTROY};
    cscdVecPush(&A.queue, d);
    wakeApp();
    pthread_cond_broadcast(&A.cond);
    /* the window closes first: nothing touches the activity after this returns */
    while (A.win && A.appRunning) pthread_cond_wait(&A.cond, &A.mutex);
    if (A.activity) (*env)->DeleteGlobalRef(env, A.activity);
    A.activity = NULL;
    pthread_mutex_unlock(&A.mutex);
}
