/* Files: reading them, paths, and finding an application's own files (the program's folder, and
 * paths relative to it). */
#include "cascade/core.h"
#include "core/thread.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

#ifdef __ANDROID__
/* An Android program's own files are the APK's assets: relative paths not found on disk are looked
 * up there (windowAndroid.c hands over the activity's asset manager). */
#include <android/asset_manager.h>

static AAssetManager* androidAssets;

void filesSetAndroidAssets(void* manager) { androidAssets = (AAssetManager*)manager; }

static AAsset* openAsset(const char* path) {
    if (!androidAssets || !path[0] || path[0] == '/') return NULL;
    while (path[0] == '.' && path[1] == '/') path += 2;
    return AAssetManager_open(androidAssets, path, AASSET_MODE_BUFFER);
}

static bool readAsset(const char* path, CscdStr* out) {
    AAsset* a = openAsset(path);
    if (!a) return false;
    const off_t n = AAsset_getLength(a);
    cscdStrClear(out);
    cscdStrReserve(out, (size_t)n);
    const int got = n > 0 ? AAsset_read(a, out->ptr, (size_t)n) : 0;
    AAsset_close(a);
    out->len = got > 0 ? (size_t)got : 0;
    out->ptr[out->len] = 0;
    return true;
}
#endif

#ifdef _WIN32
wchar_t* cscdWidePath(CscdSv path) {
    const int n = path.len ? MultiByteToWideChar(CP_UTF8, 0, path.ptr, (int)path.len, NULL, 0) : 0;
    wchar_t* w = (wchar_t*)cscdMalloc(((size_t)n + 1) * sizeof(wchar_t));
    if (n) MultiByteToWideChar(CP_UTF8, 0, path.ptr, (int)path.len, w, n);
    w[n] = 0;
    return w;
}

void cscdAppendWide(CscdStr* out, const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL); /* (with the NUL) */
    if (n <= 1) return;
    cscdStrReserve(out, out->len + (size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out->ptr + out->len, n, NULL, NULL);
    out->len += (size_t)n - 1;
}
#endif

FILE* cscdFileOpen(CscdSv path, const char* mode) {
#ifdef _WIN32
    wchar_t* p = cscdWidePath(path);
    wchar_t m[8];
    size_t k = 0;
    for (; mode[k] && k < 7; ++k) m[k] = (wchar_t)mode[k];
    m[k] = 0;
    FILE* f = _wfopen(p, m);
#else
    char* p = cscdSvDupC(path);
    FILE* f = fopen(p, mode);
#endif
    cscdFree(p);
    return f;
}

CscdStr cscdEnv(const char* name) {
    CscdStr out = {0};
#ifdef _WIN32
    wchar_t* w = cscdWidePath(cscdSv(name));
    const wchar_t* v = _wgetenv(w);
    if (v) cscdAppendWide(&out, v);
    cscdFree(w);
#else
    const char* v = getenv(name);
    if (v) cscdStrSetC(&out, v);
#endif
    return out;
}

static CscdMutex loaderMutex = CSCD_MUTEX_INIT;
static CscdResourceFn loaderFn;
static void* loaderUser;

void cscdAppSetResourceLoader(CscdResourceFn fn, void* user) {
    cscdMutexLock(&loaderMutex);
    loaderFn = fn;
    loaderUser = user;
    cscdMutexUnlock(&loaderMutex);
}

/* the program's loader, when it has the file (called outside the lock: it may read files itself) */
static bool fromLoader(CscdSv path, CscdStr* out) {
    cscdMutexLock(&loaderMutex);
    CscdResourceFn fn = loaderFn;
    void* user = loaderUser;
    cscdMutexUnlock(&loaderMutex);
    if (!fn || !path.len) return false;
    cscdStrClear(out);
    return fn(user, path, out);
}

bool cscdReadFile(CscdSv path, CscdStr* out) {
    if (fromLoader(path, out)) return true;
    FILE* f = cscdFileOpen(path, "rb");
#ifdef __ANDROID__
    if (!f) {
        char* p = cscdSvDupC(path);
        const bool found = readAsset(p, out);
        cscdFree(p);
        return found;
    }
#endif
    if (!f) return false;
    /* the size, then the bytes; what a file that changes meanwhile has is what is read */
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    const long long n = _ftelli64(f);
#else
    fseek(f, 0, SEEK_END);
    const long long n = ftell(f); /* (a long: 64 bits on the 64-bit systems) */
#endif
    rewind(f);
    if (n < 0 || (unsigned long long)n >= SIZE_MAX / 2) {
        fclose(f);
        return false;
    }
    cscdStrClear(out);
    cscdStrReserve(out, (size_t)n);
    size_t got = n ? fread(out->ptr, 1, (size_t)n, f) : 0;
    fclose(f);
    out->len = got;
    out->ptr[got] = 0;
    return true;
}

CscdStr cscdDirName(CscdSv path) {
    size_t p = cscdSvRFindAny(path, "/\\");
    return p == CSCD_NPOS ? cscdStrDup(CSCD_SV("")) : cscdStrDup(cscdSvSub(path, 0, p));
}

CscdStr cscdJoinPath(CscdSv base, CscdSv rel) {
    if (rel.len == 0) return cscdStrDup(base);
    CscdSv r = rel;
    if (cscdSvStartsC(r, "file:///")) r = cscdSvFrom(r, 8);
    else if (cscdSvStartsC(r, "file://")) r = cscdSvFrom(r, 7);
    /* strip query and fragment */
    size_t q = cscdSvFindAny(r, "?#", 0);
    if (q != CSCD_NPOS && !cscdSvStartsC(r, "data:")) r = cscdSvSub(r, 0, q);
    bool absolute = r.len && (r.ptr[0] == '/' || r.ptr[0] == '\\' || (r.len > 1 && r.ptr[1] == ':'));
    /* an address with a scheme ("data:", "https:", an app's own "thumb:12"; RFC 3986: a letter,
     * then letters, digits, "+", "-" or "."): not relative. One letter is a Windows drive. */
    size_t colon = cscdSvFindChar(r, ':', 0);
    bool scheme = colon != CSCD_NPOS && colon >= 2 && cscdIsAlpha(r.ptr[0]);
    for (size_t i = 1; scheme && i < colon; ++i) scheme = cscdIsAlnum(r.ptr[i]) || r.ptr[i] == '+' || r.ptr[i] == '-' || r.ptr[i] == '.';
    if (absolute || scheme || base.len == 0) return cscdStrDup(r);
    CscdSv b = base;
    while (cscdSvStartsC(r, "./")) r = cscdSvFrom(r, 2);
    while (cscdSvStartsC(r, "../")) {
        size_t p = cscdSvRFindAny(b, "/\\");
        if (p == CSCD_NPOS) {
            b = cscdSvMake(b.ptr, 0);
            r = cscdSvFrom(r, 3);
            break;
        }
        b = cscdSvSub(b, 0, p);
        r = cscdSvFrom(r, 3);
    }
    if (b.len == 0) return cscdStrDup(r);
    CscdStr out = cscdStrDup(b);
    cscdStrAppendChar(&out, '/');
    cscdStrAppend(&out, r);
    return out;
}

CscdStr cscdExecutableDir(void) {
#ifdef _WIN32
    enum { MOST = 32768 }; /* (the longest path Windows has) */
    wchar_t* buf = (wchar_t*)cscdMalloc(MOST * sizeof(wchar_t));
    const DWORD n = GetModuleFileNameW(NULL, buf, MOST);
    CscdStr exe = {0};
    if (n && n < MOST) cscdAppendWide(&exe, buf);
    cscdFree(buf);
    if (!exe.len) return cscdStrDupC(".");
    CscdStr dir = cscdDirName(cscdStrSv(&exe));
    cscdStrFree(&exe);
    return dir;
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    return _NSGetExecutablePath(buf, &size) == 0 ? cscdDirName(cscdSv(buf)) : cscdStrDupC(".");
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return cscdStrDupC(".");
    buf[n] = 0;
    return cscdDirName(cscdSv(buf));
#endif
}

static CscdMutex foldersMutex = CSCD_MUTEX_INIT;
static CscdStrVec appFolders;
static bool debugBuild;

void cscdAppSetBuild(bool debug, const char* sourceDir) {
    debugBuild = debug;
    if (debug) cscdAppAddFolder(cscdSv(sourceDir));
}

bool cscdAppDebugBuild(void) { return debugBuild; }

void cscdAppAddFolder(CscdSv dir) {
    cscdMutexLock(&foldersMutex);
    bool found = false;
    for (size_t i = 0; i < appFolders.len; ++i) found = found || cscdSvEq(cscdStrSv(&appFolders.items[i]), dir);
    if (dir.len && !found) cscdVecPush(&appFolders, cscdStrDup(dir));
    cscdMutexUnlock(&foldersMutex);
}

long long cscdFileStamp(CscdSv path) {
#ifdef _WIN32
    wchar_t* p = cscdWidePath(path);
    WIN32_FILE_ATTRIBUTE_DATA a;
    BOOL ok = GetFileAttributesExW(p, GetFileExInfoStandard, &a);
    cscdFree(p);
    if (!ok) return -1;
    const unsigned long long t = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    return (long long)(t * 31 + a.nFileSizeLow);
#else
    char* p = cscdSvDupC(path);
    struct stat st;
    int r = stat(p, &st);
    cscdFree(p);
    if (r != 0) return -1;
    /* (unsigned: the product wraps, which is all a stamp needs; a signed one would overflow) */
#ifdef __APPLE__
    const unsigned long long ns = (unsigned long long)st.st_mtimespec.tv_sec * 1000000000u + (unsigned long long)st.st_mtimespec.tv_nsec;
#else
    const unsigned long long ns = (unsigned long long)st.st_mtim.tv_sec * 1000000000u + (unsigned long long)st.st_mtim.tv_nsec;
#endif
    return (long long)((ns * 31 + (unsigned long long)st.st_size) & 0x7fffffffffffffffull); /* (never -1) */
#endif
}

static bool exists(const CscdStr* p) {
    FILE* f = cscdFileOpen(cscdStrSv(p), "rb");
    if (f) fclose(f);
#ifdef __ANDROID__
    if (!f) {
        AAsset* a = openAsset(cscdStrC(p));
        if (a) AAsset_close(a);
        return a != NULL;
    }
#endif
    return f != NULL;
}

/* base + "/" + path when that file exists */
static bool tryIn(CscdSv base, CscdSv path, CscdStr* out) {
    CscdStr s = cscdStrDup(base);
    cscdStrAppendChar(&s, '/');
    cscdStrAppend(&s, path);
    if (exists(&s)) {
        *out = s;
        return true;
    }
    cscdStrFree(&s);
    return false;
}

CscdStr cscdAppFindFile(CscdSv path) {
    const bool absolute = path.len && (path.ptr[0] == '/' || path.ptr[0] == '\\' || (path.len > 1 && path.ptr[1] == ':'));
    if (path.len == 0 || absolute) return cscdStrDup(path);
    CscdStr out = {0};
    cscdMutexLock(&foldersMutex);
    for (size_t i = 0; i < appFolders.len; ++i)
        if (tryIn(cscdStrSv(&appFolders.items[i]), path, &out)) {
            cscdMutexUnlock(&foldersMutex);
            return out;
        }
    cscdMutexUnlock(&foldersMutex);
    CscdStr p = cscdStrDup(path);
    if (exists(&p)) return p;
    cscdStrFree(&p);
    CscdStr dir = cscdExecutableDir();
    bool found = tryIn(cscdStrSv(&dir), path, &out);
    if (!found) {
        CscdStr res = cscdStrCopy(&dir);
        cscdStrAppendC(&res, "/../Resources");
        found = tryIn(cscdStrSv(&res), path, &out);
        cscdStrFree(&res);
    }
    cscdStrFree(&dir);
    return found ? out : cscdStrDup(path);
}
