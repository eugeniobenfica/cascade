/* AT-SPI: the page as Linux screen readers (Orca) see it, from the view's accessible tree (a11y.c),
 * over D-Bus spoken here directly (a Unix socket, the EXTERNAL handshake, the wire format). The
 * accessibility bus's address comes from the session bus (org.a11y.Bus); the application embeds
 * itself in the registry, then answers the questions put to its objects: the application at
 * .../accessible/root, the window's frame at .../accessible/frame, each element at .../accessible/e<n>
 * (its accessible number: an element gone answers as defunct). Questions are read and answered in the
 * window's loop; changes go out as signals. */
#include "platform/common/platformInternal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ---- the wire format ------------------------------------------------------------------------- */

typedef struct Out { CscdBytes b; } Out;

static void pad(Out* o, size_t a) { while (o->b.len % a) cscdVecPush(&o->b, 0); }
static void putU8(Out* o, uint8_t v) { cscdVecPush(&o->b, v); }
static void putU32(Out* o, uint32_t v) { pad(o, 4); cscdVecAppend(&o->b, (const uint8_t*)&v, 4); }
static void putI32(Out* o, int32_t v) { putU32(o, (uint32_t)v); }
static void putI16(Out* o, int16_t v) { pad(o, 2); cscdVecAppend(&o->b, (const uint8_t*)&v, 2); }
static void putDouble(Out* o, double v) { pad(o, 8); cscdVecAppend(&o->b, (const uint8_t*)&v, 8); }
static void putStr(Out* o, CscdSv s) {
    putU32(o, (uint32_t)s.len);
    cscdVecAppend(&o->b, (const uint8_t*)s.ptr, s.len);
    putU8(o, 0);
}
static void putStrC(Out* o, const char* s) { putStr(o, cscdSv(s)); }
static void putSig(Out* o, const char* s) {
    putU8(o, (uint8_t)strlen(s));
    cscdVecAppend(&o->b, (const uint8_t*)s, strlen(s));
    putU8(o, 0);
}
/* arrays: the length written once the elements are (the elements' alignment first) */
static size_t arrayBegin(Out* o, size_t align) {
    putU32(o, 0);
    const size_t at = o->b.len - 4;
    pad(o, align);
    return at;
}
static void arrayEnd(Out* o, size_t at, size_t align) {
    size_t start = at + 4;
    while (start % align) ++start;
    const uint32_t n = (uint32_t)(o->b.len - start);
    memcpy(o->b.items + at, &n, 4);
}

/* (so): an object reference */
static void putRef(Out* o, const char* bus, const char* path) {
    pad(o, 8);
    putStrC(o, bus);
    putStrC(o, path);
}

typedef struct In {
    const uint8_t* p;
    size_t n, at;
    bool bad;
} In;

static void skipPad(In* in, size_t a) { while (in->at % a) ++in->at; if (in->at > in->n) in->bad = true; }
static uint32_t getU32(In* in) {
    skipPad(in, 4);
    if (in->at + 4 > in->n) { in->bad = true; return 0; }
    uint32_t v;
    memcpy(&v, in->p + in->at, 4);
    in->at += 4;
    return v;
}
static double getDouble(In* in) {
    skipPad(in, 8);
    if (in->at + 8 > in->n) { in->bad = true; return 0; }
    double v;
    memcpy(&v, in->p + in->at, 8);
    in->at += 8;
    return v;
}
static CscdSv getStr(In* in) {
    const uint32_t len = getU32(in);
    if (in->bad || in->at + len + 1 > in->n) { in->bad = true; return CSCD_SV(""); }
    const CscdSv s = cscdSvMake((const char*)in->p + in->at, len);
    in->at += len + 1;
    return s;
}
static CscdSv getSig(In* in) {
    if (in->at >= in->n) { in->bad = true; return CSCD_SV(""); }
    const uint8_t len = in->p[in->at++];
    if (in->at + len + 1 > in->n) { in->bad = true; return CSCD_SV(""); }
    const CscdSv s = cscdSvMake((const char*)in->p + in->at, len);
    in->at += len + 1;
    return s;
}

/* ---- a connection ---------------------------------------------------------------------------- */

typedef struct Bus {
    int fd;
    uint32_t serial;
    char unique[128];
    CscdBytes in; /* bytes read, not yet a whole message */
} Bus;

typedef struct Msg {
    uint8_t type; /* 1 call, 2 return, 3 error, 4 signal */
    uint32_t serial, replySerial;
    CscdSv path, iface, member, sender, signature;
    In body;
} Msg;

static bool sendAll(int fd, const uint8_t* p, size_t n) {
    while (n) {
        const ssize_t put = send(fd, p, n, MSG_NOSIGNAL);
        if (put < 0 && (errno == EINTR || errno == EAGAIN)) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            poll(&pfd, 1, 100);
            continue;
        }
        if (put <= 0) return false;
        p += put;
        n -= (size_t)put;
    }
    return true;
}

/* A message: header fields (path, interface, member, destination, signature, reply serial) and its body. */
static uint32_t sendMsg(Bus* bus, uint8_t type, const char* path, const char* iface, const char* member, const char* dest, uint32_t replyTo,
                        const char* sig, const Out* body) {
    Out h = {0};
    putU8(&h, 'l');
    putU8(&h, type);
    putU8(&h, type == 1 ? 0 : 1); /* (no reply expected for returns and signals) */
    putU8(&h, 1);
    putU32(&h, body ? (uint32_t)body->b.len : 0);
    putU32(&h, ++bus->serial);
    const size_t fields = arrayBegin(&h, 8);
#define FIELD(code, sigc, put) do { pad(&h, 8); putU8(&h, code); putSig(&h, sigc); put; } while (0)
    if (path) FIELD(1, "o", putStrC(&h, path));
    if (iface) FIELD(2, "s", putStrC(&h, iface));
    if (member) FIELD(3, "s", putStrC(&h, member));
    if (type == 3) FIELD(4, "s", putStrC(&h, member ? "org.freedesktop.DBus.Error.Failed" : "org.freedesktop.DBus.Error.UnknownMethod"));
    if (replyTo) FIELD(5, "u", putU32(&h, replyTo));
    if (dest) FIELD(6, "s", putStrC(&h, dest));
    if (sig && sig[0]) FIELD(8, "g", putSig(&h, sig));
#undef FIELD
    arrayEnd(&h, fields, 8);
    pad(&h, 8);
    if (body) cscdVecAppend(&h.b, body->b.items, body->b.len);
    const bool ok = sendAll(bus->fd, h.b.items, h.b.len);
    cscdVecFree(&h.b);
    return ok ? bus->serial : 0;
}

/* One whole message from the bytes read, taken out of them (false: not all there yet). */
static bool takeMsg(Bus* bus, CscdBytes* raw, Msg* m) {
    if (bus->in.len < 16) return false;
    const uint8_t* p = bus->in.items;
    if (p[0] != 'l') { bus->in.len = 0; return false; } /* (big-endian peers: not met on the buses this talks to) */
    uint32_t bodyLen, fieldsLen;
    memcpy(&bodyLen, p + 4, 4);
    memcpy(&fieldsLen, p + 12, 4);
    size_t headerEnd = 16 + fieldsLen;
    while (headerEnd % 8) ++headerEnd;
    if (fieldsLen > (1u << 26) || bodyLen > (1u << 27)) { bus->in.len = 0; return false; }
    if (bus->in.len < headerEnd + bodyLen) return false;
    raw->len = 0;
    cscdVecAppend(raw, p, headerEnd + bodyLen);
    cscdVecRemoveRange(&bus->in, 0, headerEnd + bodyLen);
    memset(m, 0, sizeof *m);
    m->type = raw->items[1];
    memcpy(&m->serial, raw->items + 8, 4);
    In h = {raw->items, 16 + fieldsLen, 16, false};
    while (h.at < h.n && !h.bad) {
        skipPad(&h, 8);
        if (h.at >= h.n) break;
        const uint8_t code = h.p[h.at++];
        const CscdSv sig = getSig(&h);
        if (sig.len != 1) { h.bad = true; break; }
        switch (sig.ptr[0]) {
        case 'o': case 's': {
            const CscdSv s = getStr(&h);
            if (code == 1) m->path = s;
            else if (code == 2) m->iface = s;
            else if (code == 3) m->member = s;
            else if (code == 7) m->sender = s;
            break;
        }
        case 'g': {
            const CscdSv s = getSig(&h);
            if (code == 8) m->signature = s;
            break;
        }
        case 'u': {
            const uint32_t v = getU32(&h);
            if (code == 5) m->replySerial = v;
            break;
        }
        default: h.bad = true; break;
        }
    }
    m->body = (In){raw->items, headerEnd + bodyLen, headerEnd, false};
    return true;
}

static bool readMore(Bus* bus, int timeoutMs) {
    struct pollfd pfd = {bus->fd, POLLIN, 0};
    if (poll(&pfd, 1, timeoutMs) <= 0) return false;
    uint8_t buf[8192];
    const ssize_t got = recv(bus->fd, buf, sizeof buf, MSG_DONTWAIT);
    if (got <= 0) return false;
    cscdVecAppend(&bus->in, buf, (size_t)got);
    return true;
}

/* A call's reply, waited for (other messages meanwhile are dropped: only used while connecting). */
static bool callWait(Bus* bus, const char* dest, const char* path, const char* iface, const char* member, const char* sig, const Out* body,
                     CscdBytes* raw, Msg* reply) {
    const uint32_t serial = sendMsg(bus, 1, path, iface, member, dest, 0, sig, body);
    if (!serial) return false;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        while (takeMsg(bus, raw, reply))
            if ((reply->type == 2 || reply->type == 3) && reply->replySerial == serial) return reply->type == 2;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        const long ms = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms > 2000 || !readMore(bus, (int)(2000 - ms))) return false;
    }
}

/* Connects to a bus address ("unix:path=..." or "unix:abstract=...", several separated by ';'),
 * authenticates (EXTERNAL: our uid) and says hello. */
static bool busOpen(Bus* bus, const char* address) {
    memset(bus, 0, sizeof *bus);
    bus->fd = -1;
    if (!address) return false;
    CscdSv rest = cscdSv(address);
    while (rest.len && bus->fd < 0) {
        size_t end = cscdSvFindChar(rest, ';', 0);
        if (end == CSCD_NPOS) end = rest.len;
        const CscdSv one = cscdSvSub(rest, 0, end);
        rest = end < rest.len ? cscdSvFrom(rest, end + 1) : CSCD_SV("");
        if (!cscdSvStartsC(one, "unix:")) continue;
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        socklen_t len = 0;
        CscdSv kv = cscdSvFrom(one, 5);
        while (kv.len) {
            size_t comma = cscdSvFindChar(kv, ',', 0);
            if (comma == CSCD_NPOS) comma = kv.len;
            const CscdSv pair = cscdSvSub(kv, 0, comma);
            kv = comma < kv.len ? cscdSvFrom(kv, comma + 1) : CSCD_SV("");
            const bool abstract = cscdSvStartsC(pair, "abstract=");
            if (!abstract && !cscdSvStartsC(pair, "path=")) continue;
            const CscdSv v = cscdSvFrom(pair, abstract ? 9 : 5);
            if (v.len + 2 > sizeof sa.sun_path) continue;
            memcpy(sa.sun_path + (abstract ? 1 : 0), v.ptr, v.len);
            len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + v.len + 1); /* (the leading NUL of an abstract name, or the path's ending one) */
        }
        if (!len) continue;
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        if (connect(fd, (struct sockaddr*)&sa, len) < 0) { close(fd); continue; }
        bus->fd = fd;
    }
    if (bus->fd < 0) return false;
    /* EXTERNAL: the uid, in hex of its decimal digits */
    char uid[32], hex[80] = "", line[160];
    snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
    for (size_t i = 0; uid[i]; ++i) snprintf(hex + 2 * i, 3, "%02x", (unsigned char)uid[i]);
    snprintf(line, sizeof line, "AUTH EXTERNAL %s\r\n", hex);
    const uint8_t zero = 0;
    if (!sendAll(bus->fd, &zero, 1) || !sendAll(bus->fd, (const uint8_t*)line, strlen(line))) return false;
    char answer[256];
    size_t got = 0;
    while (got < sizeof answer - 1 && !memchr(answer, '\n', got)) {
        struct pollfd pfd = {bus->fd, POLLIN, 0};
        if (poll(&pfd, 1, 2000) <= 0) return false;
        const ssize_t r = recv(bus->fd, answer + got, sizeof answer - 1 - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    if (got < 2 || memcmp(answer, "OK", 2) != 0) return false;
    if (!sendAll(bus->fd, (const uint8_t*)"BEGIN\r\n", 7)) return false;
    CscdBytes raw = {0};
    Msg reply;
    bool ok = callWait(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", NULL, NULL, &raw, &reply);
    if (ok) {
        const CscdSv name = getStr(&reply.body);
        ok = !reply.body.bad && name.len < sizeof bus->unique;
        if (ok) { memcpy(bus->unique, name.ptr, name.len); bus->unique[name.len] = 0; }
    }
    cscdVecFree(&raw);
    return ok;
}

static void busClose(Bus* bus) {
    if (bus->fd >= 0) close(bus->fd);
    bus->fd = -1;
    cscdVecFree(&bus->in);
}

/* ---- the accessible objects ------------------------------------------------------------------ */

#define ROOT_PATH "/org/a11y/atspi/accessible/root"
#define FRAME_PATH "/org/a11y/atspi/accessible/frame"
#define NULL_PATH "/org/a11y/atspi/null"
#define ELEM_PREFIX "/org/a11y/atspi/accessible/e"

typedef struct Atspi {
    CscdWindow* w;
    Bus bus;
    char registryBus[128];
    CscdBytes raw;
    int32_t appId;
} Atspi;

/* AT-SPI's roles and states (atspi-constants.h) */
enum { R_APPLICATION = 75, R_FRAME = 23, R_DOCUMENT_WEB = 95, R_PANEL = 39, R_STATIC = 116, R_PUSH_BUTTON = 43, R_LINK = 88,
       R_CHECK_BOX = 7, R_RADIO_BUTTON = 44, R_ENTRY = 79, R_PASSWORD_TEXT = 40, R_COMBO_BOX = 11, R_LIST_BOX = 98, R_LIST_ITEM = 32,
       R_SLIDER = 51, R_PROGRESS_BAR = 42, R_HEADING = 83, R_LIST = 31, R_IMAGE = 27, R_DIALOG = 16, R_PAGE_TAB = 37,
       R_PAGE_TAB_LIST = 38, R_MENU = 33, R_MENU_BAR = 34, R_MENU_ITEM = 35, R_TABLE = 55, R_TABLE_ROW = 90, R_TABLE_CELL = 56,
       R_SEPARATOR = 50, R_TOOL_TIP = 64 };
enum { S_ACTIVE = 1, S_CHECKED = 4, S_COLLAPSED = 5, S_DEFUNCT = 6, S_EDITABLE = 7, S_ENABLED = 8, S_EXPANDABLE = 9, S_EXPANDED = 10,
       S_FOCUSABLE = 11, S_FOCUSED = 12, S_MULTI_LINE = 17, S_SELECTABLE = 22, S_SELECTED = 23, S_SENSITIVE = 24, S_SHOWING = 25,
       S_SINGLE_LINE = 26, S_VISIBLE = 30, S_INDETERMINATE = 32, S_REQUIRED = 33, S_READ_ONLY = 43, S_CHECKABLE = 41 };

static uint32_t roleOf(CscdRole r, uint32_t state) {
    switch (r) {
    case CSCD_ROLE_DOCUMENT: return R_DOCUMENT_WEB;
    case CSCD_ROLE_TEXT: return R_STATIC;
    case CSCD_ROLE_BUTTON: return R_PUSH_BUTTON;
    case CSCD_ROLE_LINK: return R_LINK;
    case CSCD_ROLE_CHECKBOX: return R_CHECK_BOX;
    case CSCD_ROLE_RADIO: return R_RADIO_BUTTON;
    case CSCD_ROLE_TEXTBOX: return (state & CSCD_A11Y_PASSWORD) ? R_PASSWORD_TEXT : R_ENTRY;
    case CSCD_ROLE_COMBOBOX: return R_COMBO_BOX;
    case CSCD_ROLE_LISTBOX: return R_LIST_BOX;
    case CSCD_ROLE_OPTION: case CSCD_ROLE_LIST_ITEM: return R_LIST_ITEM;
    case CSCD_ROLE_SLIDER: return R_SLIDER;
    case CSCD_ROLE_PROGRESS: return R_PROGRESS_BAR;
    case CSCD_ROLE_HEADING: return R_HEADING;
    case CSCD_ROLE_LIST: return R_LIST;
    case CSCD_ROLE_IMAGE: return R_IMAGE;
    case CSCD_ROLE_DIALOG: return R_DIALOG;
    case CSCD_ROLE_TAB: return R_PAGE_TAB;
    case CSCD_ROLE_TAB_LIST: return R_PAGE_TAB_LIST;
    case CSCD_ROLE_MENU: return R_MENU;
    case CSCD_ROLE_MENU_BAR: return R_MENU_BAR;
    case CSCD_ROLE_MENU_ITEM: return R_MENU_ITEM;
    case CSCD_ROLE_TABLE: return R_TABLE;
    case CSCD_ROLE_ROW: return R_TABLE_ROW;
    case CSCD_ROLE_CELL: return R_TABLE_CELL;
    case CSCD_ROLE_SEPARATOR: return R_SEPARATOR;
    case CSCD_ROLE_TOOLTIP: return R_TOOL_TIP;
    default: return R_PANEL;
    }
}

static const char* roleName(uint32_t r) {
    switch (r) {
    case R_APPLICATION: return "application";
    case R_FRAME: return "frame";
    case R_DOCUMENT_WEB: return "document web";
    case R_STATIC: return "static";
    case R_PUSH_BUTTON: return "push button";
    case R_LINK: return "link";
    case R_CHECK_BOX: return "check box";
    case R_RADIO_BUTTON: return "radio button";
    case R_ENTRY: return "entry";
    case R_PASSWORD_TEXT: return "password text";
    case R_COMBO_BOX: return "combo box";
    case R_LIST_BOX: return "list box";
    case R_LIST_ITEM: return "list item";
    case R_SLIDER: return "slider";
    case R_PROGRESS_BAR: return "progress bar";
    case R_HEADING: return "heading";
    case R_LIST: return "list";
    case R_IMAGE: return "image";
    case R_DIALOG: return "dialog";
    case R_PAGE_TAB: return "page tab";
    case R_PAGE_TAB_LIST: return "page tab list";
    case R_MENU: return "menu";
    case R_MENU_BAR: return "menu bar";
    case R_MENU_ITEM: return "menu item";
    case R_TABLE: return "table";
    case R_TABLE_ROW: return "table row";
    case R_TABLE_CELL: return "table cell";
    case R_SEPARATOR: return "separator";
    case R_TOOL_TIP: return "tool tip";
    default: return "panel";
    }
}

/* What a path names: the application (0), the frame (1), an element (2, *e; NULL when it is gone). */
static int objectOf(Atspi* a, CscdSv path, CscdElement** e) {
    *e = NULL;
    if (cscdSvEqC(path, ROOT_PATH)) return 0;
    if (cscdSvEqC(path, FRAME_PATH)) return 1;
    if (!cscdSvStartsC(path, ELEM_PREFIX)) return -1;
    char* num = cscdSvDupC(cscdSvFrom(path, strlen(ELEM_PREFIX)));
    const uint32_t id = (uint32_t)strtoul(num, NULL, 10);
    cscdFree(num);
    *e = cscdAccessibleElement(a->w->view, id);
    return 2;
}

static void elementPath(Atspi* a, const CscdElement* e, char* out, size_t n) {
    if (!e) snprintf(out, n, "%s", FRAME_PATH);
    else snprintf(out, n, ELEM_PREFIX "%u", (unsigned)cscdAccessibleId(a->w->view, e));
}

static void putElementRef(Out* o, Atspi* a, const CscdElement* e) {
    char path[96];
    elementPath(a, e, path, sizeof path);
    putRef(o, a->bus.unique, path);
}

/* The children of an object: the application's is the frame, the frame's the document (the
 * accessible root), an element's its accessible children. */
static void childrenOf(Atspi* a, int kind, CscdElement* e, CscdPtrVec* out) {
    out->len = 0;
    CscdView* v = a->w->view;
    if (kind == 1) { CscdElement* root = cscdAccessibleRoot(v); if (root) cscdVecPush(out, root); }
    else if (kind == 2 && e) cscdAccessibleChildren(v, e, out);
}

static void putChildRef(Out* o, Atspi* a, int kind, void* child) {
    if (kind == 0) putRef(o, a->bus.unique, FRAME_PATH);
    else putElementRef(o, a, (CscdElement*)child);
}

static void stateSet(Atspi* a, int kind, CscdElement* e, uint32_t out[2]) {
    out[0] = out[1] = 0;
#define SET(s) (out[(s) / 32] |= 1u << ((s) % 32))
    if (kind == 2 && !e) { SET(S_DEFUNCT); return; }
    SET(S_ENABLED); SET(S_SENSITIVE); SET(S_SHOWING); SET(S_VISIBLE);
    if (kind == 1) { SET(S_ACTIVE); return; }
    if (kind == 0) return;
    const uint32_t s = cscdAccessibleState(a->w->view, e);
    const CscdRole role = cscdAccessibleRole(e);
    if (s & CSCD_A11Y_FOCUSABLE) SET(S_FOCUSABLE);
    if (s & CSCD_A11Y_FOCUSED) SET(S_FOCUSED);
    if (s & CSCD_A11Y_CHECKED) SET(S_CHECKED);
    if (s & CSCD_A11Y_MIXED) SET(S_INDETERMINATE);
    if (s & CSCD_A11Y_DISABLED) { out[S_ENABLED / 32] &= ~(1u << (S_ENABLED % 32)); out[S_SENSITIVE / 32] &= ~(1u << (S_SENSITIVE % 32)); }
    if (s & CSCD_A11Y_EXPANDED) { SET(S_EXPANDABLE); SET(S_EXPANDED); }
    if (s & CSCD_A11Y_COLLAPSED) { SET(S_EXPANDABLE); SET(S_COLLAPSED); }
    if (s & CSCD_A11Y_SELECTED) SET(S_SELECTED);
    if (s & CSCD_A11Y_READONLY) SET(S_READ_ONLY);
    if (s & CSCD_A11Y_REQUIRED) SET(S_REQUIRED);
    if (role == CSCD_ROLE_CHECKBOX || role == CSCD_ROLE_RADIO) SET(S_CHECKABLE);
    if (role == CSCD_ROLE_OPTION || role == CSCD_ROLE_TAB) SET(S_SELECTABLE);
    if (role == CSCD_ROLE_TEXTBOX) {
        if (!(s & CSCD_A11Y_READONLY)) SET(S_EDITABLE);
        SET((s & CSCD_A11Y_MULTILINE) ? S_MULTI_LINE : S_SINGLE_LINE);
    }
#undef SET
}

static uint32_t roleFor(Atspi* a, int kind, CscdElement* e) {
    if (kind == 0) return R_APPLICATION;
    if (kind == 1) return R_FRAME;
    return e ? roleOf(cscdAccessibleRole(e), cscdAccessibleState(a->w->view, e)) : R_PANEL;
}

static void nameOf(Atspi* a, int kind, CscdElement* e, CscdStr* out) {
    if (kind == 0) {
        char app[256] = "";
        FILE* f = fopen("/proc/self/comm", "r");
        if (f) { if (fgets(app, sizeof app, f)) app[strcspn(app, "\n")] = 0; fclose(f); }
        cscdStrAppendC(out, app[0] ? app : "cascade");
    } else if (kind == 1) cscdStrAppend(out, cscdStrSv(&a->w->lastTitle));
    else if (e) cscdAccessibleName(a->w->view, e, out);
}

static bool hasAction(const CscdElement* e) {
    switch (cscdAccessibleRole(e)) {
    case CSCD_ROLE_BUTTON: case CSCD_ROLE_LINK: case CSCD_ROLE_CHECKBOX: case CSCD_ROLE_RADIO: case CSCD_ROLE_MENU_ITEM: case CSCD_ROLE_TAB:
    case CSCD_ROLE_OPTION: case CSCD_ROLE_COMBOBOX: return true;
    default: return false;
    }
}
static bool hasValue(const CscdElement* e) { return cscdAccessibleRole(e) == CSCD_ROLE_SLIDER || cscdAccessibleRole(e) == CSCD_ROLE_PROGRESS; }
static bool hasText(const CscdElement* e) {
    const CscdRole r = cscdAccessibleRole(e);
    return r == CSCD_ROLE_TEXTBOX || r == CSCD_ROLE_TEXT || r == CSCD_ROLE_HEADING || r == CSCD_ROLE_LINK || r == CSCD_ROLE_BUTTON;
}

/* An element's text for the Text interface: a field's value, else its name. */
static void textOf(Atspi* a, CscdElement* e, CscdStr* out) {
    if (cscdAccessibleRole(e) == CSCD_ROLE_TEXTBOX) cscdAccessibleValue(a->w->view, e, out);
    else cscdAccessibleName(a->w->view, e, out);
}

/* character offsets <-> bytes of UTF-8 text */
static size_t charCount(CscdSv s) {
    size_t n = 0;
    for (size_t i = 0; i < s.len; ++i) n += ((unsigned char)s.ptr[i] & 0xC0) != 0x80;
    return n;
}
static size_t byteAt(CscdSv s, long chars) {
    size_t i = 0;
    for (long c = 0; i < s.len && c < chars; ++c) cscdUtf8Next(s, &i);
    return i;
}

/* window coordinates (device pixels) of an element; with the window's place on screen for screen ones */
static void extentsOf(Atspi* a, int kind, CscdElement* e, uint32_t coordType, int32_t r[4]) {
    CscdWindow* w = a->w;
    CscdRect rect = cscdRect(0, 0, w->clientW / w->dpr, w->clientH / w->dpr);
    if (kind == 2 && e) {
        rect = cscdViewElementRect(w->view, e);
        rect.x -= cscdViewScrollX(w->view);
        rect.y -= cscdViewScrollY(w->view);
    }
    int ox = 0, oy = 0;
    if (coordType == 0) platformWindowOrigin(w, &ox, &oy);
    r[0] = ox + (int32_t)(rect.x * w->dpr);
    r[1] = oy + (int32_t)(rect.y * w->dpr);
    r[2] = (int32_t)(rect.w * w->dpr);
    r[3] = (int32_t)(rect.h * w->dpr);
}

/* ---- properties ---- */

/* One property's value as a variant (false when the object has none such). */
static bool putProperty(Out* o, Atspi* a, int kind, CscdElement* e, CscdSv iface, CscdSv name) {
    CscdStr s = {0};
    bool ok = true;
    if (cscdSvEqC(iface, "org.a11y.atspi.Accessible")) {
        if (cscdSvEqC(name, "Name")) { nameOf(a, kind, e, &s); putSig(o, "s"); putStr(o, cscdStrSv(&s)); }
        else if (cscdSvEqC(name, "Description")) { if (kind == 2 && e) cscdAccessibleDescription(a->w->view, e, &s); putSig(o, "s"); putStr(o, cscdStrSv(&s)); }
        else if (cscdSvEqC(name, "Parent")) {
            putSig(o, "(so)");
            if (kind == 0) putRef(o, a->registryBus[0] ? a->registryBus : "org.a11y.atspi.Registry", ROOT_PATH);
            else if (kind == 1) putRef(o, a->bus.unique, ROOT_PATH);
            else {
                CscdElement* p = e ? cscdAccessibleParent(a->w->view, e) : NULL;
                if (!p || e == cscdAccessibleRoot(a->w->view)) putRef(o, a->bus.unique, FRAME_PATH);
                else putElementRef(o, a, p);
            }
        } else if (cscdSvEqC(name, "ChildCount")) {
            CscdPtrVec c = {0};
            childrenOf(a, kind, e, &c);
            putSig(o, "i");
            putI32(o, kind == 0 ? 1 : (int32_t)c.len);
            cscdVecFree(&c);
        } else if (cscdSvEqC(name, "Locale")) { putSig(o, "s"); putStrC(o, getenv("LANG") ? getenv("LANG") : "C"); }
        else if (cscdSvEqC(name, "AccessibleId")) { putSig(o, "s"); putStrC(o, ""); }
        else if (cscdSvEqC(name, "HelpText")) { putSig(o, "s"); putStrC(o, ""); }
        else ok = false;
    } else if (cscdSvEqC(iface, "org.a11y.atspi.Application") && kind == 0) {
        if (cscdSvEqC(name, "ToolkitName")) { putSig(o, "s"); putStrC(o, "cascade"); }
        else if (cscdSvEqC(name, "Version")) { putSig(o, "s"); putStrC(o, "3"); }
        else if (cscdSvEqC(name, "AtspiVersion")) { putSig(o, "s"); putStrC(o, "2.1"); }
        else if (cscdSvEqC(name, "Id")) { putSig(o, "i"); putI32(o, a->appId); }
        else ok = false;
    } else if (cscdSvEqC(iface, "org.a11y.atspi.Action") && kind == 2 && e) {
        if (cscdSvEqC(name, "NActions")) { putSig(o, "i"); putI32(o, hasAction(e) ? 1 : 0); }
        else ok = false;
    } else if (cscdSvEqC(iface, "org.a11y.atspi.Text") && kind == 2 && e) {
        textOf(a, e, &s);
        if (cscdSvEqC(name, "CharacterCount")) { putSig(o, "i"); putI32(o, (int32_t)charCount(cscdStrSv(&s))); }
        else if (cscdSvEqC(name, "CaretOffset")) {
            putSig(o, "i");
            putI32(o, (int32_t)charCount(cscdSvSub(cscdStrSv(&e->editValue), 0, cscdMinz(e->caret, e->editValue.len))));
        } else ok = false;
    } else if (cscdSvEqC(iface, "org.a11y.atspi.Value") && kind == 2 && e) {
        double value = 0, min = 0, max = 0, step = 0;
        cscdAccessibleRange(e, &value, &min, &max, &step);
        if (cscdSvEqC(name, "CurrentValue")) { putSig(o, "d"); putDouble(o, value); }
        else if (cscdSvEqC(name, "MinimumValue")) { putSig(o, "d"); putDouble(o, min); }
        else if (cscdSvEqC(name, "MaximumValue")) { putSig(o, "d"); putDouble(o, max); }
        else if (cscdSvEqC(name, "MinimumIncrement")) { putSig(o, "d"); putDouble(o, step); }
        else if (cscdSvEqC(name, "Text")) { cscdAccessibleValue(a->w->view, e, &s); putSig(o, "s"); putStr(o, cscdStrSv(&s)); }
        else ok = false;
    } else ok = false;
    cscdStrFree(&s);
    return ok;
}

static const char* const kAccessibleProps[] = {"Name", "Description", "Parent", "ChildCount", "Locale", "AccessibleId", "HelpText", NULL};
static const char* const kApplicationProps[] = {"ToolkitName", "Version", "AtspiVersion", "Id", NULL};
static const char* const kActionProps[] = {"NActions", NULL};
static const char* const kTextProps[] = {"CharacterCount", "CaretOffset", NULL};
static const char* const kValueProps[] = {"CurrentValue", "MinimumValue", "MaximumValue", "MinimumIncrement", "Text", NULL};

/* ---- the questions ---- */

static void replyTo(Atspi* a, const Msg* m, const char* sig, const Out* body) {
    char dest[128];
    snprintf(dest, sizeof dest, "%.*s", (int)m->sender.len, m->sender.ptr);
    sendMsg(&a->bus, 2, NULL, NULL, NULL, dest[0] ? dest : NULL, m->serial, sig, body);
}

static void replyError(Atspi* a, const Msg* m) {
    char dest[128];
    snprintf(dest, sizeof dest, "%.*s", (int)m->sender.len, m->sender.ptr);
    Out body = {0};
    putStrC(&body, "unknown method or object");
    sendMsg(&a->bus, 3, NULL, NULL, NULL, dest[0] ? dest : NULL, m->serial, "s", &body);
    cscdVecFree(&body.b);
}

static void interfacesOf(int kind, CscdElement* e, CscdStrVec* out) {
    cscdVecPush(out, cscdStrDupC("org.a11y.atspi.Accessible"));
    if (kind == 0) { cscdVecPush(out, cscdStrDupC("org.a11y.atspi.Application")); return; }
    cscdVecPush(out, cscdStrDupC("org.a11y.atspi.Component"));
    if (kind != 2 || !e) return;
    if (hasAction(e)) cscdVecPush(out, cscdStrDupC("org.a11y.atspi.Action"));
    if (hasText(e)) cscdVecPush(out, cscdStrDupC("org.a11y.atspi.Text"));
    if (hasValue(e)) cscdVecPush(out, cscdStrDupC("org.a11y.atspi.Value"));
}

static void handleCall(Atspi* a, Msg* m) {
    CscdElement* e = NULL;
    const int kind = objectOf(a, m->path, &e);
    CscdView* v = a->w->view;
    Out o = {0};
    const CscdSv iface = m->iface, member = m->member;
    In* in = &m->body;
    bool done = true;
    if (cscdSvEqC(iface, "org.freedesktop.DBus.Peer") && cscdSvEqC(member, "Ping")) replyTo(a, m, "", NULL);
    else if (cscdSvEqC(iface, "org.freedesktop.DBus.Introspectable") && cscdSvEqC(member, "Introspect")) {
        putStrC(&o, "<node><interface name=\"org.a11y.atspi.Accessible\"/><interface name=\"org.a11y.atspi.Component\"/></node>");
        replyTo(a, m, "s", &o);
    } else if (kind < 0) done = false;
    else if (cscdSvEqC(iface, "org.freedesktop.DBus.Properties")) {
        const CscdSv ifc = getStr(in);
        if (cscdSvEqC(member, "Get")) {
            const CscdSv name = getStr(in);
            if (!in->bad && putProperty(&o, a, kind, e, ifc, name)) replyTo(a, m, "v", &o);
            else done = false;
        } else if (cscdSvEqC(member, "GetAll")) {
            const char* const* names = cscdSvEqC(ifc, "org.a11y.atspi.Accessible") ? kAccessibleProps
                                     : cscdSvEqC(ifc, "org.a11y.atspi.Application") ? kApplicationProps
                                     : cscdSvEqC(ifc, "org.a11y.atspi.Action") ? kActionProps
                                     : cscdSvEqC(ifc, "org.a11y.atspi.Text") ? kTextProps
                                     : cscdSvEqC(ifc, "org.a11y.atspi.Value") ? kValueProps : NULL;
            const size_t arr = arrayBegin(&o, 8);
            for (size_t i = 0; names && names[i]; ++i) {
                Out val = {0};
                if (!putProperty(&val, a, kind, e, ifc, cscdSv(names[i]))) { cscdVecFree(&val.b); continue; }
                cscdVecFree(&val.b);
                pad(&o, 8);
                putStrC(&o, names[i]);
                putProperty(&o, a, kind, e, ifc, cscdSv(names[i]));
            }
            arrayEnd(&o, arr, 8);
            replyTo(a, m, "a{sv}", &o);
        } else if (cscdSvEqC(member, "Set")) {
            const CscdSv name = getStr(in);
            const CscdSv sig = getSig(in);
            if (cscdSvEqC(ifc, "org.a11y.atspi.Application") && cscdSvEqC(name, "Id") && cscdSvEqC(sig, "i")) a->appId = (int32_t)getU32(in);
            else if (cscdSvEqC(ifc, "org.a11y.atspi.Value") && cscdSvEqC(name, "CurrentValue") && cscdSvEqC(sig, "d") && e) {
                char buf[64];
                snprintf(buf, sizeof buf, "%.15g", getDouble(in));
                cscdAccessibleDo(v, e, CSCD_A11Y_SET_VALUE, cscdSv(buf));
                a->w->dirty = true;
            }
            replyTo(a, m, "", NULL);
        } else done = false;
    } else if (cscdSvEqC(iface, "org.a11y.atspi.Accessible")) {
        CscdPtrVec children = {0};
        if (cscdSvEqC(member, "GetChildAtIndex")) {
            const int32_t i = (int32_t)getU32(in);
            childrenOf(a, kind, e, &children);
            if (kind == 0 && i == 0) putRef(&o, a->bus.unique, FRAME_PATH);
            else if (kind != 0 && i >= 0 && (size_t)i < children.len) putChildRef(&o, a, kind, children.items[i]);
            else putRef(&o, "", NULL_PATH);
            replyTo(a, m, "(so)", &o);
        } else if (cscdSvEqC(member, "GetChildren")) {
            childrenOf(a, kind, e, &children);
            const size_t arr = arrayBegin(&o, 8);
            if (kind == 0) putRef(&o, a->bus.unique, FRAME_PATH);
            else for (size_t i = 0; i < children.len; ++i) putChildRef(&o, a, kind, children.items[i]);
            arrayEnd(&o, arr, 8);
            replyTo(a, m, "a(so)", &o);
        } else if (cscdSvEqC(member, "GetIndexInParent")) {
            int32_t index = 0;
            if (kind == 2 && e && e != cscdAccessibleRoot(v)) {
                CscdElement* p = cscdAccessibleParent(v, e);
                childrenOf(a, 2, p, &children);
                index = -1;
                for (size_t i = 0; i < children.len; ++i) if (children.items[i] == e) index = (int32_t)i;
            }
            putI32(&o, index);
            replyTo(a, m, "i", &o);
        } else if (cscdSvEqC(member, "GetRelationSet")) {
            const size_t arr = arrayBegin(&o, 8);
            arrayEnd(&o, arr, 8);
            replyTo(a, m, "a(ua(so))", &o);
        } else if (cscdSvEqC(member, "GetRole")) {
            putU32(&o, roleFor(a, kind, e));
            replyTo(a, m, "u", &o);
        } else if (cscdSvEqC(member, "GetRoleName") || cscdSvEqC(member, "GetLocalizedRoleName")) {
            putStrC(&o, roleName(roleFor(a, kind, e)));
            replyTo(a, m, "s", &o);
        } else if (cscdSvEqC(member, "GetState")) {
            uint32_t s[2];
            stateSet(a, kind, e, s);
            const size_t arr = arrayBegin(&o, 4);
            putU32(&o, s[0]);
            putU32(&o, s[1]);
            arrayEnd(&o, arr, 4);
            replyTo(a, m, "au", &o);
        } else if (cscdSvEqC(member, "GetAttributes")) {
            const size_t arr = arrayBegin(&o, 8);
            pad(&o, 8);
            putStrC(&o, "toolkit");
            putStrC(&o, "cascade");
            if (kind == 2 && e && cscdAccessibleLevel(e) > 0) {
                char lvl[16];
                snprintf(lvl, sizeof lvl, "%d", cscdAccessibleLevel(e));
                pad(&o, 8);
                putStrC(&o, "level");
                putStrC(&o, lvl);
            }
            arrayEnd(&o, arr, 8);
            replyTo(a, m, "a{ss}", &o);
        } else if (cscdSvEqC(member, "GetApplication")) {
            putRef(&o, a->bus.unique, ROOT_PATH);
            replyTo(a, m, "(so)", &o);
        } else if (cscdSvEqC(member, "GetInterfaces")) {
            CscdStrVec ifs = {0};
            interfacesOf(kind, e, &ifs);
            const size_t arr = arrayBegin(&o, 4);
            for (size_t i = 0; i < ifs.len; ++i) putStr(&o, cscdStrSv(&ifs.items[i]));
            arrayEnd(&o, arr, 4);
            cscdStrVecFree(&ifs);
            replyTo(a, m, "as", &o);
        } else done = false;
        cscdVecFree(&children);
    } else if (cscdSvEqC(iface, "org.a11y.atspi.Application")) {
        if (cscdSvEqC(member, "GetLocale")) { putStrC(&o, getenv("LANG") ? getenv("LANG") : "C"); replyTo(a, m, "s", &o); }
        else done = false;
    } else if (cscdSvEqC(iface, "org.a11y.atspi.Component")) {
        int32_t r[4];
        if (cscdSvEqC(member, "GetExtents")) {
            extentsOf(a, kind, e, getU32(in), r);
            pad(&o, 8);
            for (int i = 0; i < 4; ++i) putI32(&o, r[i]);
            replyTo(a, m, "(iiii)", &o);
        } else if (cscdSvEqC(member, "GetPosition")) {
            extentsOf(a, kind, e, getU32(in), r);
            pad(&o, 8);
            putI32(&o, r[0]);
            putI32(&o, r[1]);
            replyTo(a, m, "(ii)", &o);
        } else if (cscdSvEqC(member, "GetSize")) {
            extentsOf(a, kind, e, 1, r);
            pad(&o, 8);
            putI32(&o, r[2]);
            putI32(&o, r[3]);
            replyTo(a, m, "(ii)", &o);
        } else if (cscdSvEqC(member, "Contains") || cscdSvEqC(member, "GetAccessibleAtPoint")) {
            const int32_t x = (int32_t)getU32(in), y = (int32_t)getU32(in);
            const uint32_t type = getU32(in);
            extentsOf(a, kind, e, type, r);
            const bool inside = x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3];
            if (cscdSvEqC(member, "Contains")) { putU32(&o, inside); replyTo(a, m, "b", &o); }
            else {
                int ox = 0, oy = 0;
                if (type == 0) platformWindowOrigin(a->w, &ox, &oy);
                CscdElement* hit = inside ? cscdAccessibleAt(v, (x - ox) / a->w->dpr, (y - oy) / a->w->dpr) : NULL;
                if (hit) putElementRef(&o, a, hit);
                else putRef(&o, "", NULL_PATH);
                replyTo(a, m, "(so)", &o);
            }
        } else if (cscdSvEqC(member, "GetLayer")) { putU32(&o, kind == 1 ? 7 : 3); replyTo(a, m, "u", &o); }
        else if (cscdSvEqC(member, "GetMDIZOrder")) { putI16(&o, 0); replyTo(a, m, "n", &o); }
        else if (cscdSvEqC(member, "GetAlpha")) { putDouble(&o, 1); replyTo(a, m, "d", &o); }
        else if (cscdSvEqC(member, "GrabFocus") || cscdSvEqC(member, "ScrollTo")) {
            const bool ok = kind == 2 && e && cscdAccessibleDo(v, e, cscdSvEqC(member, "GrabFocus") ? CSCD_A11Y_FOCUS : CSCD_A11Y_SCROLL_INTO_VIEW, CSCD_SV(""));
            a->w->dirty = true;
            putU32(&o, ok);
            replyTo(a, m, "b", &o);
        } else done = false;
    } else if (cscdSvEqC(iface, "org.a11y.atspi.Action") && kind == 2 && e) {
        const char* action = cscdAccessibleRole(e) == CSCD_ROLE_LINK ? "jump" : cscdAccessibleRole(e) == CSCD_ROLE_CHECKBOX ? "toggle" : "press";
        if (cscdSvEqC(member, "GetName") || cscdSvEqC(member, "GetLocalizedName") || cscdSvEqC(member, "GetDescription")) {
            putStrC(&o, action);
            replyTo(a, m, "s", &o);
        } else if (cscdSvEqC(member, "GetKeyBinding")) { putStrC(&o, ""); replyTo(a, m, "s", &o); }
        else if (cscdSvEqC(member, "GetActions")) {
            const size_t arr = arrayBegin(&o, 8);
            if (hasAction(e)) { pad(&o, 8); putStrC(&o, action); putStrC(&o, action); putStrC(&o, ""); }
            arrayEnd(&o, arr, 8);
            replyTo(a, m, "a(sss)", &o);
        } else if (cscdSvEqC(member, "DoAction")) {
            const bool ok = cscdAccessibleDo(v, e, CSCD_A11Y_ACTIVATE, CSCD_SV(""));
            a->w->dirty = true;
            putU32(&o, ok);
            replyTo(a, m, "b", &o);
        } else done = false;
    } else if (cscdSvEqC(iface, "org.a11y.atspi.Text") && kind == 2 && e) {
        CscdStr t = {0};
        textOf(a, e, &t);
        const CscdSv s = cscdStrSv(&t);
        if (cscdSvEqC(member, "GetText")) {
            const int32_t from = (int32_t)getU32(in), to = (int32_t)getU32(in);
            const size_t a0 = byteAt(s, from < 0 ? 0 : from), a1 = to < 0 ? s.len : byteAt(s, to);
            putStr(&o, cscdSvSub(s, a0, a1 > a0 ? a1 - a0 : 0));
            replyTo(a, m, "s", &o);
        } else if (cscdSvEqC(member, "GetCharacterAtOffset")) {
            size_t at = byteAt(s, (int32_t)getU32(in));
            putI32(&o, at < s.len ? (int32_t)cscdUtf8Next(s, &at) : 0);
            replyTo(a, m, "i", &o);
        } else if (cscdSvEqC(member, "GetStringAtOffset")) {
            /* granularity 0 char, 1 word, 2 sentence, 3 line, 4 paragraph: the whole text but for characters */
            const int32_t off = (int32_t)getU32(in);
            const uint32_t gran = getU32(in);
            const size_t count = charCount(s);
            int32_t a0 = 0, a1 = (int32_t)count;
            if (gran == 0) { a0 = off; a1 = off + 1 <= (int32_t)count ? off + 1 : (int32_t)count; }
            const size_t b0 = byteAt(s, a0), b1 = byteAt(s, a1);
            putStr(&o, cscdSvSub(s, b0, b1 - b0));
            putI32(&o, a0);
            putI32(&o, a1);
            replyTo(a, m, "sii", &o);
        } else done = false;
        cscdStrFree(&t);
    } else done = false;
    if (!done && m->type == 1) replyError(a, m);
    cscdVecFree(&o.b);
}

/* ---- events: what changed, as signals ---- */

static void emit(Atspi* a, CscdElement* e, const char* member, const char* detail, int32_t d1, char anyKind, const void* any) {
    if (a->bus.fd < 0) return;
    char path[96];
    elementPath(a, e, path, sizeof path);
    Out o = {0};
    putStrC(&o, detail);
    putI32(&o, d1);
    putI32(&o, 0);
    if (anyKind == 's') { putSig(&o, "s"); putStrC(&o, (const char*)any); }
    else if (anyKind == 'r') { putSig(&o, "(so)"); putElementRef(&o, a, (const CscdElement*)any); }
    else { putSig(&o, "i"); putI32(&o, 0); }
    const size_t arr = arrayBegin(&o, 8);
    arrayEnd(&o, arr, 8);
    sendMsg(&a->bus, 4, path, "org.a11y.atspi.Event.Object", member, NULL, 0, "siiva{sv}", &o);
    cscdVecFree(&o.b);
}

static void accessibleEvent(void* user, CscdElement* e, CscdAccessibleEvent event) {
    Atspi* a = (Atspi*)user;
    CscdView* v = a->w->view;
    switch (event) {
    case CSCD_A11Y_EVENT_FOCUS:
        if (e) emit(a, e, "StateChanged", "focused", 1, 0, NULL);
        break;
    case CSCD_A11Y_EVENT_VALUE: {
        if (!e) break;
        CscdStr s = {0};
        cscdAccessibleValue(v, e, &s);
        emit(a, e, "PropertyChange", "accessible-value", 0, 's', cscdStrC(&s));
        if (cscdAccessibleRole(e) == CSCD_ROLE_TEXTBOX) emit(a, e, "TextChanged", "insert", 0, 's', cscdStrC(&s));
        cscdStrFree(&s);
        break;
    }
    case CSCD_A11Y_EVENT_STATE:
        if (e) {
            const uint32_t s = cscdAccessibleState(v, e);
            emit(a, e, "StateChanged", "checked", (s & CSCD_A11Y_CHECKED) ? 1 : 0, 0, NULL);
            if (s & (CSCD_A11Y_EXPANDED | CSCD_A11Y_COLLAPSED)) emit(a, e, "StateChanged", "expanded", (s & CSCD_A11Y_EXPANDED) ? 1 : 0, 0, NULL);
        }
        break;
    case CSCD_A11Y_EVENT_STRUCTURE: {
        CscdElement* root = cscdAccessibleRoot(v);
        if (root) emit(a, e ? e : root, "ChildrenChanged", "add", 0, 'r', e ? e : root);
        break;
    }
    }
}

/* ---- the window's side ---- */

void atspiAttach(CscdWindow* w) {
    if (w->a11y) return;
    const char* off = getenv("NO_AT_BRIDGE");
    if ((off && !strcmp(off, "1")) || !cscdViewFeature(w->view, CSCD_FEATURE_ACCESSIBILITY)) return;
    /* the accessibility bus's address: from the session bus, or the environment */
    char address[512] = "";
    const char* env = getenv("AT_SPI_BUS_ADDRESS");
    if (env) snprintf(address, sizeof address, "%s", env);
    else {
        Bus session;
        if (!busOpen(&session, getenv("DBUS_SESSION_BUS_ADDRESS"))) { busClose(&session); return; }
        CscdBytes raw = {0};
        Msg r;
        if (callWait(&session, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress", NULL, NULL, &raw, &r)) {
            const CscdSv s = getStr(&r.body);
            if (!r.body.bad) snprintf(address, sizeof address, "%.*s", (int)s.len, s.ptr);
        }
        cscdVecFree(&raw);
        busClose(&session);
    }
    if (!address[0]) return;
    Atspi* a = CSCD_NEW(Atspi);
    a->w = w;
    if (!busOpen(&a->bus, address)) {
        busClose(&a->bus);
        cscdFree(a);
        return;
    }
    /* in the registry's tree: the application under the desktop */
    Out o = {0};
    putRef(&o, a->bus.unique, ROOT_PATH);
    Msg r;
    if (callWait(&a->bus, "org.a11y.atspi.Registry", ROOT_PATH, "org.a11y.atspi.Socket", "Embed", "(so)", &o, &a->raw, &r)) {
        skipPad(&r.body, 8);
        const CscdSv bus = getStr(&r.body);
        if (!r.body.bad) snprintf(a->registryBus, sizeof a->registryBus, "%.*s", (int)bus.len, bus.ptr);
    }
    cscdVecFree(&o.b);
    fcntl(a->bus.fd, F_SETFL, fcntl(a->bus.fd, F_GETFL) | O_NONBLOCK);
    w->a11y = a;
    cscdViewOnAccessibleEvent(w->view, accessibleEvent, a);
}

void atspiDetach(CscdWindow* w) {
    Atspi* a = (Atspi*)w->a11y;
    if (!a) return;
    cscdViewOnAccessibleEvent(w->view, NULL, NULL);
    busClose(&a->bus);
    cscdVecFree(&a->raw);
    cscdFree(a);
    w->a11y = NULL;
}

int platformA11yFd(CscdWindow* w) { return w->a11y ? ((Atspi*)w->a11y)->bus.fd : -1; }

void platformA11yRead(CscdWindow* w) {
    Atspi* a = (Atspi*)w->a11y;
    if (!a || a->bus.fd < 0) return;
    while (readMore(&a->bus, 0)) {}
    Msg m;
    while (takeMsg(&a->bus, &a->raw, &m))
        if (m.type == 1) handleCall(a, &m);
}
