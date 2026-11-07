/* What every platform's window shares about presenting: the choice of presenter (Vulkan, then
 * OpenGL, then the system's software path, as far as the build has them: CASCADE_VULKAN,
 * CASCADE_OPENGL) and the shared libraries opened at run time (the GPU APIs, Wayland's). */
#include "platform/common/platformInternal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

const char* cscdBackendName(CscdBackend b) {
    switch (b) {
    case CSCD_BACKEND_VULKAN: return "Vulkan";
    case CSCD_BACKEND_OPENGL: return "OpenGL";
    case CSCD_BACKEND_SOFTWARE: return "Software";
    default: return "Auto";
    }
}

void* platformOpenLibrary(const char* const* names) {
    for (; *names; ++names) {
#ifdef _WIN32
        HMODULE lib = LoadLibraryA(*names);
        if (lib) return (void*)lib;
#else
        void* lib = dlopen(*names, RTLD_NOW | RTLD_LOCAL);
        if (lib) return lib;
#endif
    }
    return NULL;
}

void* platformSymbol(void* lib, const char* name) {
    if (!lib) return NULL;
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

void platformCloseLibrary(void* lib) {
    if (!lib) return;
#ifdef _WIN32
    FreeLibrary((HMODULE)lib);
#else
    dlclose(lib);
#endif
}

/* (a backend the build left out: none, and the next is tried) */
#ifdef CASCADE_NO_VULKAN
CscdPresenter* platformVulkanPresenter(void* nativeWindow, int width, int height) {
    (void)nativeWindow; (void)width; (void)height;
    return NULL;
}
#endif

CscdPresenter* cscdPresenterNew(void* nativeWindow, CscdBackend preferred, int width, int height) {
    CscdPresenter* p = NULL;
#ifdef CASCADE_WAYLAND
    if (nativeWindow && ((const CscdWaylandWindow*)nativeWindow)->mark == CSCD_WAYLAND_MARK) {
        if (preferred != CSCD_BACKEND_SOFTWARE) p = platformVulkanPresenter(nativeWindow, width, height);
        if (!p) p = platformWaylandSoftwarePresenter(nativeWindow, width, height);
        return p;
    }
#endif
    if (preferred == CSCD_BACKEND_VULKAN || preferred == CSCD_BACKEND_AUTO) p = platformVulkanPresenter(nativeWindow, width, height);
    if (!p && (preferred == CSCD_BACKEND_OPENGL || preferred == CSCD_BACKEND_AUTO || preferred == CSCD_BACKEND_VULKAN))
        p = platformGLPresenter(nativeWindow, width, height);
    if (!p && preferred == CSCD_BACKEND_OPENGL) p = platformVulkanPresenter(nativeWindow, width, height);
    if (!p) p = platformSoftwarePresenter(nativeWindow, width, height);
    return p;
}
