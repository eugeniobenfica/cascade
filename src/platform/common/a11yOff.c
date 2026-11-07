/* A build without the screen readers' bridges (CASCADE_ACCESSIBILITY off: UI Automation, AT-SPI,
 * NSAccessibility, UIAccessibility, TalkBack): the window's calls to them do nothing. The accessible
 * tree itself (a11y.c) stays, for a host of its own to offer. */
#include "platform/common/platformInternal.h"

#if defined(_WIN32)
void winA11yAttach(CscdWindow* w) { (void)w; }
bool winA11yGetObject(CscdWindow* w, uintptr_t wp, intptr_t lp, intptr_t* out) {
    (void)w; (void)wp; (void)lp; (void)out;
    return false;
}
void winA11yDetach(CscdWindow* w) { (void)w; }
#elif defined(__APPLE__) && TARGET_OS_IPHONE
void uikitA11yAttach(CscdWindow* w) { (void)w; }
void uikitA11yDetach(CscdWindow* w) { (void)w; }
void* uikitA11yElements(CscdWindow* w) { (void)w; return NULL; }
#elif defined(__APPLE__)
void cocoaA11yAttach(CscdWindow* w) { (void)w; }
void cocoaA11yDetach(CscdWindow* w) { (void)w; }
void* cocoaA11yChildren(CscdWindow* w) { (void)w; return NULL; }
void* cocoaA11yHitTest(CscdWindow* w, double x, double y) { (void)w; (void)x; (void)y; return NULL; }
void* cocoaA11yFocused(CscdWindow* w) { (void)w; return NULL; }
#elif defined(CASCADE_WAYLAND)
void atspiAttach(CscdWindow* w) { (void)w; }
void atspiDetach(CscdWindow* w) { (void)w; }
int platformA11yFd(CscdWindow* w) { (void)w; return -1; }
void platformA11yRead(CscdWindow* w) { (void)w; }
#endif
