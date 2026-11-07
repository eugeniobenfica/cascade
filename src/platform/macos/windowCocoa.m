/* Cocoa window (macOS): event loop, Retina scaling, mouse / trackpad / keyboard / text input (with
 * input methods through NSTextInputClient), cursors, the general pasteboard (text, pictures, copied
 * files), files and text dropped from other apps, and frame presentation: Vulkan through MoltenVK
 * when it is installed, OpenGL (NSOpenGLContext) or Core Animation (the software path).
 *
 * Shortcuts follow the Mac: Command is reported as Ctrl and Control as Meta, so "Ctrl+C" in the
 * interface is Command-C (as Qt does).
 * Objective-C with ARC (CMakeLists.txt). The C structs hold the Objective-C objects as retained
 * void pointers (CFBridgingRetain / CFBridgingRelease). */
#include "platform/common/platformInternal.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <math.h>

@class CascadeView;
@class CascadeWindowDelegate;

#define kPostedSubtype 0x4245 /* an application-defined event that only wakes the loop */

static NSString* toNS(const char* s) { return [NSString stringWithUTF8String:s ? s : ""] ?: @""; }

static NSString* svToNS(CscdSv s) {
    return [[NSString alloc] initWithBytes:(s.ptr ? s.ptr : "") length:s.len encoding:NSUTF8StringEncoding] ?: @"";
}

/* appended to out */
static void fromNS(NSString* s, CscdStr* out) {
    if (s) cscdStrAppendC(out, s.UTF8String ?: "");
}

static void removeCR(CscdStr* s) {
    size_t k = 0;
    for (size_t i = 0; i < s->len; ++i) if (s->ptr[i] != '\r') s->ptr[k++] = s->ptr[i];
    cscdStrTruncate(s, k);
}

static CscdModifiers mods(NSEventModifierFlags f) {
    CscdModifiers m;
    m.shift = (f & NSEventModifierFlagShift) != 0;
    m.ctrl = (f & NSEventModifierFlagCommand) != 0;
    m.alt = (f & NSEventModifierFlagOption) != 0;
    m.meta = (f & NSEventModifierFlagControl) != 0;
    return m;
}

/* The key from its key code (kVK_*: the same on every layout) or, for characters, from what it types
 * without modifiers. */
static CscdKey mapKey(NSEvent* e, char name[16]) {
#define K(n, key) { strcpy(name, n); return key; }
    switch (e.keyCode) {
    case 0x33: K("Backspace", CSCD_KEY_BACKSPACE)
    case 0x30: K("Tab", CSCD_KEY_TAB)
    case 0x24: case 0x4C: K("Enter", CSCD_KEY_ENTER)
    case 0x35: K("Escape", CSCD_KEY_ESCAPE)
    case 0x31: K(" ", CSCD_KEY_SPACE)
    case 0x75: K("Delete", CSCD_KEY_DELETE)
    case 0x7B: K("ArrowLeft", CSCD_KEY_LEFT)
    case 0x7C: K("ArrowRight", CSCD_KEY_RIGHT)
    case 0x7E: K("ArrowUp", CSCD_KEY_UP)
    case 0x7D: K("ArrowDown", CSCD_KEY_DOWN)
    case 0x73: K("Home", CSCD_KEY_HOME)
    case 0x77: K("End", CSCD_KEY_END)
    case 0x74: K("PageUp", CSCD_KEY_PAGE_UP)
    case 0x79: K("PageDown", CSCD_KEY_PAGE_DOWN)
    case 0x72: K("Insert", CSCD_KEY_INSERT) /* Help, where Insert is on PC keyboards */
    case 0x38: case 0x3C: K("Shift", CSCD_KEY_SHIFT)
    case 0x37: case 0x36: K("Control", CSCD_KEY_CONTROL) /* Command */
    case 0x3A: case 0x3D: K("Alt", CSCD_KEY_ALT)         /* Option */
    case 0x3B: case 0x3E: K("Meta", CSCD_KEY_META)       /* Control */
    case 0x6E: K("ContextMenu", CSCD_KEY_UNKNOWN)
    default: break;
    }
    static const unsigned short fkeys[12] = {0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F};
    for (int i = 0; i < 12; ++i)
        if (e.keyCode == fkeys[i]) {
            snprintf(name, 16, "F%d", i + 1);
            return (CscdKey)((int)CSCD_KEY_F1 + i);
        }
    if (e.type != NSEventTypeKeyDown && e.type != NSEventTypeKeyUp) K("Unidentified", CSCD_KEY_UNKNOWN)
    NSString* chars = nil;
    if (@available(macOS 10.15, *)) chars = [e charactersByApplyingModifiers:0];
    if (!chars.length) chars = e.charactersIgnoringModifiers;
    if (chars.length == 1) {
        unichar c = [chars.lowercaseString characterAtIndex:0];
        switch (c) {
        case 'a': K("a", CSCD_KEY_A)
        case 'c': K("c", CSCD_KEY_C)
        case 'v': K("v", CSCD_KEY_V)
        case 'x': K("x", CSCD_KEY_X)
        case 'z': K("z", CSCD_KEY_Z)
        case 'y': K("y", CSCD_KEY_Y)
        default: break;
        }
        if (c >= 0x20 && c < 0x7F) {
            name[0] = (char)c;
            name[1] = 0;
            return CSCD_KEY_CHARACTER;
        }
    }
    K("Unidentified", CSCD_KEY_UNKNOWN)
#undef K
}

static NSCursor* privateCursor(SEL sel) {
    if (![NSCursor respondsToSelector:sel]) return nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    id c = [NSCursor performSelector:sel];
#pragma clang diagnostic pop
    return [c isKindOfClass:NSCursor.class] ? c : nil;
}

static NSCursor* nsCursor(CscdCursor c) {
    switch (c) {
    case CSCD_CURSOR_POINTER: return NSCursor.pointingHandCursor;
    case CSCD_CURSOR_TEXT: return NSCursor.IBeamCursor;
    case CSCD_CURSOR_MOVE: case CSCD_CURSOR_GRAB: return NSCursor.openHandCursor;
    case CSCD_CURSOR_GRABBING: return NSCursor.closedHandCursor;
    case CSCD_CURSOR_NOT_ALLOWED: return NSCursor.operationNotAllowedCursor;
    case CSCD_CURSOR_COPY: return NSCursor.dragCopyCursor;
    case CSCD_CURSOR_CROSSHAIR: return NSCursor.crosshairCursor;
    case CSCD_CURSOR_EW_RESIZE: case CSCD_CURSOR_COL_RESIZE: return NSCursor.resizeLeftRightCursor;
    case CSCD_CURSOR_NS_RESIZE: case CSCD_CURSOR_ROW_RESIZE: return NSCursor.resizeUpDownCursor;
    case CSCD_CURSOR_WAIT: case CSCD_CURSOR_PROGRESS: return privateCursor(@selector(_waitCursor)) ?: NSCursor.arrowCursor;
    case CSCD_CURSOR_HELP: return privateCursor(@selector(_helpCursor)) ?: NSCursor.arrowCursor;
    case CSCD_CURSOR_NESW_RESIZE: return privateCursor(@selector(_windowResizeNorthEastSouthWestCursor)) ?: NSCursor.crosshairCursor;
    case CSCD_CURSOR_NWSE_RESIZE: return privateCursor(@selector(_windowResizeNorthWestSouthEastCursor)) ?: NSCursor.crosshairCursor;
    default: return NSCursor.arrowCursor;
    }
}

/* the application menu: Hide and Quit (Quit closes the front window, asking it first) */
static NSMenuItem* appMenuItem(void) {
    NSMenuItem* appItem = [NSMenuItem new];
    NSMenu* appMenu = [NSMenu new];
    NSString* name = NSProcessInfo.processInfo.processName;
    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:NSMenuItem.separatorItem];
    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(performClose:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    return appItem;
}

static void startApplication(void) {
    static bool started = false;
    if (started) return;
    started = true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSMenu* bar = [NSMenu new];
    [bar addItem:appMenuItem()];
    NSApp.mainMenu = bar;
    [NSApp finishLaunching];
}

static void wakeLoop(void) {
    @autoreleasepool {
        NSEvent* e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0
                                         context:nil subtype:kPostedSubtype data1:0 data2:0];
        [NSApp postEvent:e atStart:NO];
    }
}

/* ---- presenters ------------------------------------------------------------------------------------- */

/* Core Animation: the frame becomes the view's layer contents. */
typedef struct SoftwarePresenter { CscdPresenter base; void* view; /* NSView, retained */ } SoftwarePresenter;

static CscdBackend swBackend(const CscdPresenter* p) { (void)p; return CSCD_BACKEND_SOFTWARE; }
static const char* swDescription(const CscdPresenter* p) { (void)p; return "Software (Core Animation)"; }
static void swResize(CscdPresenter* p, int w, int h) { (void)p; (void)w; (void)h; }

static void swDestroy(CscdPresenter* bp) {
    SoftwarePresenter* p = (SoftwarePresenter*)bp;
    if (p->view) CFBridgingRelease(p->view);
    cscdFree(p);
}

static bool swPresent(CscdPresenter* bp, const CscdImage* frame) {
    SoftwarePresenter* p = (SoftwarePresenter*)bp;
    if (frame->width <= 0 || frame->height <= 0) return true;
    NSView* view = (__bridge NSView*)p->view;
    NSData* data = [NSData dataWithBytes:frame->pixels length:cscdImagePixelCount(frame) * 4];
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image = CGImageCreate((size_t)frame->width, (size_t)frame->height, 8, 32, (size_t)frame->width * 4, space,
                                     (CGBitmapInfo)((uint32_t)kCGBitmapByteOrder32Little | (uint32_t)kCGImageAlphaPremultipliedFirst), provider, NULL, false,
                                     kCGRenderingIntentDefault);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    view.layer.contents = (__bridge id)image;
    view.layer.contentsGravity = kCAGravityResize;
    [CATransaction commit];
    CGImageRelease(image);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    return true;
}

static const CscdPresenterVtbl swVtbl = {swBackend, swPresent, swResize, swDescription, swDestroy};

#ifndef CASCADE_NO_OPENGL /* (NSOpenGLContext: CASCADE_OPENGL) */
typedef struct GLPresenter {
    CscdPresenter base;
    void* view; /* NSView, retained */
    void* ctx;  /* NSOpenGLContext, retained */
    unsigned tex;
    int texW, texH;
    int width, height;
    char description[160];
} GLPresenter;

static CscdBackend glBackend(const CscdPresenter* p) { (void)p; return CSCD_BACKEND_OPENGL; }
static const char* glDescription(const CscdPresenter* p) { return ((const GLPresenter*)p)->description; }

static void glResize(CscdPresenter* bp, int w, int h) {
    GLPresenter* p = (GLPresenter*)bp;
    p->width = w;
    p->height = h;
    [(__bridge NSOpenGLContext*)p->ctx update];
}

static bool glPresent(CscdPresenter* bp, const CscdImage* frame) {
    GLPresenter* p = (GLPresenter*)bp;
    NSOpenGLContext* ctx = (__bridge NSOpenGLContext*)p->ctx;
    [ctx makeCurrentContext];
    glPresentFrame(&p->tex, &p->texW, &p->texH, frame, p->width, p->height, bp);
    [ctx flushBuffer];
    return true;
}

static void glDestroy(CscdPresenter* bp) {
    GLPresenter* p = (GLPresenter*)bp;
    if (p->ctx) {
        NSOpenGLContext* ctx = (__bridge NSOpenGLContext*)p->ctx;
        [ctx makeCurrentContext];
        if (p->tex) glDeleteTexture(p->tex);
        [NSOpenGLContext clearCurrentContext];
        [ctx clearDrawable];
        CFBridgingRelease(p->ctx);
    }
    if (p->view) CFBridgingRelease(p->view);
    cscdFree(p);
}

static const CscdPresenterVtbl glVtbl = {glBackend, glPresent, glResize, glDescription, glDestroy};

static bool glInit(GLPresenter* p, NSView* view, int w, int h) {
    p->width = w;
    p->height = h;
    if (!cscdGlLoadFunctions()) return false;
    NSOpenGLPixelFormatAttribute attrs[] = {NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, NSOpenGLPFAAlphaSize, 8,
                                            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersionLegacy, 0};
    NSOpenGLPixelFormat* format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    if (!format) return false;
    NSOpenGLContext* ctx = [[NSOpenGLContext alloc] initWithFormat:format shareContext:nil];
    if (!ctx) return false;
    p->ctx = (void*)CFBridgingRetain(ctx);
    if ([view.layer isKindOfClass:CAMetalLayer.class]) view.layer = [view makeBackingLayer];
    view.wantsBestResolutionOpenGLSurface = YES;
    ctx.view = view;
    [ctx makeCurrentContext];
    GLint one = 1;
    [ctx setValues:&one forParameter:NSOpenGLContextParameterSwapInterval];
    typedef const unsigned char* (*GetString)(unsigned);
    GetString getString;
    *(void**)&getString = platformSymbol(glLibrary(), "glGetString");
    const unsigned char* r = getString ? getString(0x1F01) : NULL;
    snprintf(p->description, sizeof p->description, "OpenGL (%s)", r ? (const char*)r : "");
    p->tex = cscdGlCreateTexture();
    return p->tex != 0;
}

void* platformMetalLayer(void* nsview) {
    NSView* view = (__bridge NSView*)nsview;
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.contentsScale = view.window ? view.window.backingScaleFactor : 1;
    view.layer = layer;
    view.wantsLayer = YES;
    return (__bridge void*)layer;
}

CscdPresenter* platformGLPresenter(void* nativeWindow, int width, int height) {
    GLPresenter* p = CSCD_NEW(GLPresenter);
    p->base.vt = &glVtbl;
    NSView* view = (__bridge NSView*)nativeWindow;
    p->view = (void*)CFBridgingRetain(view);
    if (!glInit(p, view, width, height)) {
        glDestroy(&p->base);
        return NULL;
    }
    return &p->base;
}
#else
CscdPresenter* platformGLPresenter(void* nativeWindow, int width, int height) {
    (void)nativeWindow; (void)width; (void)height;
    return NULL;
}
#endif

CscdPresenter* platformSoftwarePresenter(void* nativeWindow, int width, int height) {
    (void)width;
    (void)height;
    SoftwarePresenter* p = CSCD_NEW(SoftwarePresenter);
    p->base.vt = &swVtbl;
    NSView* view = (__bridge NSView*)nativeWindow;
    p->view = (void*)CFBridgingRetain(view);
    if ([view.layer isKindOfClass:CAMetalLayer.class]) view.layer = [view makeBackingLayer]; /* after a Vulkan attempt */
    view.wantsLayer = YES;
    return &p->base;
}

/* ---- the window ------------------------------------------------------------------------------------- */

/* the menu bar's (cscdWindowSetMenuBar): one for the app, whichever window set it */
static struct {
    CscdMenu menu;
    CscdWindow* window;
    void* kept; /* NSMutableArray of CascadeMenuHandler, retained */
} menuBar;
static void menuForgetWindow(void* kept);

struct CscdWindowNative {
    CscdWindow* w;
    void* window;   /* NSWindow, retained */
    void* view;     /* CascadeView, retained */
    void* delegate; /* CascadeWindowDelegate, retained */
    bool closeRequested;
    bool cursorSet, cursorHidden;
    CscdCursor shown;
    void* marked;   /* NSString, retained: the input method's composition */
    int preferencesSeen; /* (preferencesChanged, as last read) */
};

#define NSWIN(n) ((__bridge NSWindow*)(n)->window)
#define NSVIEW(n) ((__bridge CascadeView*)(n)->view)
#define MARKED(n) ((__bridge NSString*)(n)->marked)

static void nativeInvalidate(CscdWindowNative* n);
static void nativeResized(CscdWindowNative* n);
static bool nativeRequestClose(CscdWindowNative* n);
static void nativeMouseDown(CscdWindowNative* n, NSEvent* e, int button);
static void nativeMouseUp(CscdWindowNative* n, NSEvent* e, int button);
static void nativeMouseMove(CscdWindowNative* n, NSEvent* e);
static void nativeMouseLeave(CscdWindowNative* n);
static void nativeWheel(CscdWindowNative* n, NSEvent* e);
static void nativeKeyDown(CscdWindowNative* n, NSEvent* e);
static void nativeKeyUp(CscdWindowNative* n, NSEvent* e);
static void nativeFlagsChanged(CscdWindowNative* n, NSEvent* e);
static void nativeText(CscdWindowNative* n, NSString* s);
static void nativeSetMarked(CscdWindowNative* n, NSString* s);
static void nativeCommand(CscdWindowNative* n, SEL sel);
static NSRect nativeCaretOnScreen(CscdWindowNative* n);
static void nativeFocusLost(CscdWindowNative* n);
static void nativeApplyCursor(CscdWindowNative* n);
static NSDragOperation nativeDragEnter(CscdWindowNative* n, id<NSDraggingInfo> info);
static NSDragOperation nativeDragOver(CscdWindowNative* n, id<NSDraggingInfo> info);
static void nativeDragLeave(CscdWindowNative* n);
static bool nativeDrop(CscdWindowNative* n, id<NSDraggingInfo> info);

@interface CascadeView : NSView <NSTextInputClient>
@property(nonatomic, assign) CscdWindowNative* native;
@end

@implementation CascadeView {
    NSTrackingArea* tracking_;
}
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
/* accessibility: the page's elements (a11yCocoa.m) */
- (BOOL)isAccessibilityElement { return NO; }
- (NSArray*)accessibilityChildren { return _native ? (__bridge_transfer NSArray*)cocoaA11yChildren(_native->w) : @[]; }
- (id)accessibilityHitTest:(NSPoint)point {
    id hit = _native ? (__bridge_transfer id)cocoaA11yHitTest(_native->w, point.x, point.y) : nil;
    return hit ?: self;
}
- (id)accessibilityFocusedUIElement {
    id focused = _native ? (__bridge_transfer id)cocoaA11yFocused(_native->w) : nil;
    return focused ?: self;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer { if (_native) nativeInvalidate(_native); }
- (void)updateTrackingAreas {
    if (tracking_) [self removeTrackingArea:tracking_];
    tracking_ = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                             options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingCursorUpdate |
                                                     NSTrackingActiveAlways | NSTrackingInVisibleRect
                                               owner:self userInfo:nil];
    [self addTrackingArea:tracking_];
    [super updateTrackingAreas];
}
- (void)cursorUpdate:(NSEvent*)e { if (_native) { _native->cursorSet = false; nativeApplyCursor(_native); } }
- (void)mouseDown:(NSEvent*)e { if (_native) nativeMouseDown(_native, e, 0); }
- (void)rightMouseDown:(NSEvent*)e { if (_native) nativeMouseDown(_native, e, 2); }
- (void)otherMouseDown:(NSEvent*)e { if (_native && e.buttonNumber == 2) nativeMouseDown(_native, e, 1); }
- (void)mouseUp:(NSEvent*)e { if (_native) nativeMouseUp(_native, e, 0); }
- (void)rightMouseUp:(NSEvent*)e { if (_native) nativeMouseUp(_native, e, 2); }
- (void)otherMouseUp:(NSEvent*)e { if (_native && e.buttonNumber == 2) nativeMouseUp(_native, e, 1); }
- (void)mouseMoved:(NSEvent*)e { if (_native) nativeMouseMove(_native, e); }
- (void)mouseDragged:(NSEvent*)e { if (_native) nativeMouseMove(_native, e); }
- (void)rightMouseDragged:(NSEvent*)e { if (_native) nativeMouseMove(_native, e); }
- (void)otherMouseDragged:(NSEvent*)e { if (_native) nativeMouseMove(_native, e); }
- (void)mouseExited:(NSEvent*)e { if (_native) nativeMouseLeave(_native); }
- (void)scrollWheel:(NSEvent*)e { if (_native) nativeWheel(_native, e); }
- (void)keyDown:(NSEvent*)e {
    if (!_native) return;
    nativeKeyDown(_native, e);
    if (_native) [self interpretKeyEvents:@[ e ]]; /* text and input methods come back below */
}
- (void)keyUp:(NSEvent*)e { if (_native) nativeKeyUp(_native, e); }
- (void)flagsChanged:(NSEvent*)e { if (_native) nativeFlagsChanged(_native, e); }

/* NSTextInputClient */
- (BOOL)hasMarkedText { return _native && MARKED(_native).length > 0; }
- (NSRange)markedRange { return [self hasMarkedText] ? NSMakeRange(0, MARKED(_native).length) : NSMakeRange(NSNotFound, 0); }
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (void)setMarkedText:(id)s selectedRange:(NSRange)sel replacementRange:(NSRange)rep {
    if (_native) nativeSetMarked(_native, [s isKindOfClass:NSAttributedString.class] ? [s string] : s);
}
- (void)unmarkText { if (_native) nativeSetMarked(_native, nil); }
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return @[]; }
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)actual { return nil; }
- (void)insertText:(id)s replacementRange:(NSRange)rep {
    if (_native) nativeText(_native, [s isKindOfClass:NSAttributedString.class] ? [s string] : s);
}
- (NSUInteger)characterIndexForPoint:(NSPoint)p { return NSNotFound; }
- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)actual { return _native ? nativeCaretOnScreen(_native) : NSZeroRect; }
- (void)doCommandBySelector:(SEL)sel { if (_native) nativeCommand(_native, sel); }

/* files and text dragged in from other apps */
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info { return _native ? nativeDragEnter(_native, info) : NSDragOperationNone; }
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info { return _native ? nativeDragOver(_native, info) : NSDragOperationNone; }
- (void)draggingExited:(id<NSDraggingInfo>)info { if (_native) nativeDragLeave(_native); }
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)info { return YES; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info { return _native && nativeDrop(_native, info); }
@end

@interface CascadeWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) CscdWindowNative* native;
@end

@implementation CascadeWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender { return _native ? nativeRequestClose(_native) : YES; }
- (void)windowWillClose:(NSNotification*)n {
    if (_native && _native->w) {
        CscdWindowNative* native = _native;
        _native = NULL;
        NSVIEW(native).native = NULL;
        nativeRequestClose(native); /* closed some other way: let go of it all the same */
    }
}
- (void)windowDidResize:(NSNotification*)n { if (_native) nativeResized(_native); }
- (void)windowDidChangeBackingProperties:(NSNotification*)n { if (_native) nativeResized(_native); }
- (void)windowDidResignKey:(NSNotification*)n { if (_native) nativeFocusLost(_native); }
- (void)windowDidEnterFullScreen:(NSNotification*)n { if (_native) nativeResized(_native); }
- (void)windowDidExitFullScreen:(NSNotification*)n { if (_native) nativeResized(_native); }
- (void)windowDidMiniaturize:(NSNotification*)n { if (_native) windowAppEvent(_native->w, CSCD_APP_BACKGROUND); }
- (void)windowDidDeminiaturize:(NSNotification*)n { if (_native) windowAppEvent(_native->w, CSCD_APP_FOREGROUND); }
@end

static void nativeInvalidate(CscdWindowNative* n) {
    if (!n->w || n->w->dirty) return;
    n->w->dirty = true;
    wakeLoop(); /* invalidated from inside the run loop (a drag, a timer): the loop must come round */
}

static NSPoint local(CscdWindowNative* n, NSEvent* e) { return [NSVIEW(n) convertPoint:e.locationInWindow fromView:nil]; }

static void nativeResized(CscdWindowNative* n) {
    CscdWindow* w = n->w;
    if (!w->hwnd) return;
    NSWindow* window = NSWIN(n);
    CascadeView* view = NSVIEW(n);
    const NSSize size = view.bounds.size;
    const float dpr = (float)window.backingScaleFactor;
    const int pw = (int)lround(size.width * dpr), ph = (int)lround(size.height * dpr);
    w->fullscreen = (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (pw == w->clientW && ph == w->clientH && dpr == w->dpr) return;
    w->clientW = pw;
    w->clientH = ph;
    w->dpr = dpr;
    if ([view.layer isKindOfClass:CAMetalLayer.class]) view.layer.contentsScale = dpr;
    if (pw > 0 && ph > 0) {
        if (w->presenter) cscdPresenterResize(w->presenter, pw, ph);
        cscdViewSetViewport(w->view, (float)size.width, (float)size.height, dpr);
        windowRender(w); /* while the user drags the edge AppKit runs its own loop */
    }
}

static bool nativeRequestClose(CscdWindowNative* n) {
    CscdWindow* w = n->w;
    if (!w || !w->hwnd) return true;
    if (w->onClose && !w->onClose(w->onCloseUser)) return false;
    cscdPresenterFree(w->presenter);
    w->presenter = NULL;
    w->hwnd = NULL;
    return true;
}

static void nativeMouseDown(CscdWindowNative* n, NSEvent* e, int button) {
    [NSWIN(n) makeFirstResponder:NSVIEW(n)];
    const NSPoint p = local(n, e);
    cscdViewMouseDown(n->w->view, (float)p.x, (float)p.y, button, mods(e.modifierFlags), (int)(e.clickCount > 1 ? e.clickCount : 1));
}

static void nativeMouseUp(CscdWindowNative* n, NSEvent* e, int button) {
    const NSPoint p = local(n, e);
    cscdViewMouseUp(n->w->view, (float)p.x, (float)p.y, button, mods(e.modifierFlags));
}

static void nativeMouseMove(CscdWindowNative* n, NSEvent* e) {
    const NSPoint p = local(n, e);
    /* pressedMouseButtons: 1 left, 2 right, 4 middle, as cscdViewMouseMove takes them */
    cscdViewMouseMove(n->w->view, (float)p.x, (float)p.y, (int)(NSEvent.pressedMouseButtons & 7), mods(e.modifierFlags));
    nativeApplyCursor(n);
}

static void nativeMouseLeave(CscdWindowNative* n) {
    if (NSEvent.pressedMouseButtons & 7) return; /* a drag keeps the pointer */
    cscdViewMouseLeave(n->w->view);
    if (n->cursorHidden) {
        [NSCursor unhide];
        n->cursorHidden = false;
    }
    n->cursorSet = false;
}

static void nativeWheel(CscdWindowNative* n, NSEvent* e) {
    const NSPoint p = local(n, e);
    CGFloat dx = e.scrollingDeltaX, dy = e.scrollingDeltaY;
    if (!e.hasPreciseScrollingDeltas) { dx *= 40; dy *= 40; } /* a mouse wheel's lines */
    cscdViewWheel(n->w->view, (float)p.x, (float)p.y, (float)-dx, (float)-dy, mods(e.modifierFlags));
}

static void nativeKeyDown(CscdWindowNative* n, NSEvent* e) {
    char name[16];
    CscdKey k = mapKey(e, name);
    cscdViewKeyDown(n->w->view, k, mods(e.modifierFlags), cscdSv(name));
}

static void nativeKeyUp(CscdWindowNative* n, NSEvent* e) {
    char name[16];
    CscdKey k = mapKey(e, name);
    cscdViewKeyUp(n->w->view, k, mods(e.modifierFlags), cscdSv(name));
}

static void nativeFlagsChanged(CscdWindowNative* n, NSEvent* e) {
    char name[16];
    CscdKey k = mapKey(e, name);
    NSEventModifierFlags flag = 0;
    switch (k) {
    case CSCD_KEY_SHIFT: flag = NSEventModifierFlagShift; break;
    case CSCD_KEY_CONTROL: flag = NSEventModifierFlagCommand; break;
    case CSCD_KEY_ALT: flag = NSEventModifierFlagOption; break;
    case CSCD_KEY_META: flag = NSEventModifierFlagControl; break;
    default: return;
    }
    if (e.modifierFlags & flag) cscdViewKeyDown(n->w->view, k, mods(e.modifierFlags), cscdSv(name));
    else cscdViewKeyUp(n->w->view, k, mods(e.modifierFlags), cscdSv(name));
}

static void nativeText(CscdWindowNative* n, NSString* s) {
    CscdView* view = n->w->view;
    if (MARKED(n).length) {
        CFBridgingRelease(n->marked);
        n->marked = NULL;
        cscdViewSetComposition(view, CSCD_SV(""));
    }
    /* Command and Control chords are shortcuts (they came as key events), not text */
    const NSEventModifierFlags f = NSApp.currentEvent.modifierFlags;
    if (NSApp.currentEvent.type == NSEventTypeKeyDown && (f & (NSEventModifierFlagCommand | NSEventModifierFlagControl))) return;
    CscdStr all = {0}, clean = {0}, out = {0};
    fromNS(s, &all);
    for (size_t i = 0; i < all.len; ++i) {
        const char c = all.ptr[i];
        if (c == '\r') cscdStrAppendChar(&clean, '\n');
        else if ((unsigned char)c >= 0x20 && c != 0x7F) cscdStrAppendChar(&clean, c);
    }
    /* the keys AppKit sends for arrows and the like are in the private use area */
    for (size_t i = 0; i < clean.len;) {
        const size_t start = i;
        const uint32_t cp = cscdUtf8Next(cscdStrSv(&clean), &i);
        if (cp >= 0xF700 && cp <= 0xF8FF) continue;
        cscdStrAppend(&out, cscdSvSub(cscdStrSv(&clean), start, i - start));
    }
    if (out.len) cscdViewTextInput(view, cscdStrSv(&out));
    cscdStrFree(&all);
    cscdStrFree(&clean);
    cscdStrFree(&out);
}

static void nativeSetMarked(CscdWindowNative* n, NSString* s) {
    if (n->marked) CFBridgingRelease(n->marked);
    n->marked = s.length ? (void*)CFBridgingRetain([s copy]) : NULL;
    CscdStr text = {0};
    fromNS(MARKED(n), &text);
    cscdViewSetComposition(n->w->view, cscdStrSv(&text));
    cscdStrFree(&text);
}

static void nativeCommand(CscdWindowNative* n, SEL sel) {
    /* Return and Enter type a line break, as WM_CHAR '\r' does on Windows */
    if (sel == @selector(insertNewline:) && !(NSApp.currentEvent.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)))
        cscdViewTextInput(n->w->view, CSCD_SV("\n"));
}

static NSRect nativeCaretOnScreen(CscdWindowNative* n) {
    const CscdRect r = cscdViewCaretRect(n->w->view);
    NSRect inView = NSMakeRect(r.x, r.y, cscdMaxf(1.f, r.w), cscdMaxf(1.f, r.h));
    return [NSWIN(n) convertRectToScreen:[NSVIEW(n) convertRect:inView toView:nil]];
}

static void nativeFocusLost(CscdWindowNative* n) {
    /* menus and tooltips do not outlive the window's turn in front */
    cscdViewCloseMenus(n->w->view);
    cscdViewHideTooltip(n->w->view);
}

static void nativeApplyCursor(CscdWindowNative* n) {
    const CscdCursor c = cscdViewCursor(n->w->view);
    if (n->cursorSet && c == n->shown) return;
    /* only while the pointer is over the view */
    NSWindow* window = NSWIN(n);
    CascadeView* view = NSVIEW(n);
    const NSPoint p = [view convertPoint:window.mouseLocationOutsideOfEventStream fromView:nil];
    if (!NSPointInRect(p, view.bounds) || !window.isKeyWindow) return;
    n->shown = c;
    n->cursorSet = true;
    if (c == CSCD_CURSOR_NONE) {
        if (!n->cursorHidden) {
            [NSCursor hide];
            n->cursorHidden = true;
        }
        return;
    }
    if (n->cursorHidden) {
        [NSCursor unhide];
        n->cursorHidden = false;
    }
    [nsCursor(c) set];
}

static void readFiles(NSPasteboard* pb, CscdStrVec* out) {
    NSArray<NSURL*>* urls = [pb readObjectsForClasses:@[ NSURL.class ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    for (NSURL* url in urls) {
        CscdStr path = {0};
        fromNS(url.path, &path);
        cscdVecPush(out, path);
    }
}

static CscdDataTransfer* dragData(NSPasteboard* pb) {
    CscdDataTransfer* dt = cscdDataTransferNew();
    cscdStrSetC(&dt->effectAllowed, "copy");
    readFiles(pb, &dt->files);
    NSString* s = [pb stringForType:NSPasteboardTypeString];
    if (s) {
        CscdStr t = {0};
        fromNS(s, &t);
        removeCR(&t);
        cscdDataTransferSetData(dt, CSCD_SV("text/plain"), cscdStrSv(&t));
        cscdStrFree(&t);
    }
    return dt;
}

static NSDragOperation nativeDragEnter(CscdWindowNative* n, id<NSDraggingInfo> info) {
    const NSPoint p = [NSVIEW(n) convertPoint:info.draggingLocation fromView:nil];
    CscdDataTransfer* dt = dragData(info.draggingPasteboard);
    cscdViewExternalDragEnter(n->w->view, dt, (float)p.x, (float)p.y);
    cscdRelease(dt);
    return nativeDragOver(n, info);
}

static NSDragOperation nativeDragOver(CscdWindowNative* n, id<NSDraggingInfo> info) {
    const NSPoint p = [NSVIEW(n) convertPoint:info.draggingLocation fromView:nil];
    return cscdViewExternalDragOver(n->w->view, (float)p.x, (float)p.y) ? NSDragOperationCopy : NSDragOperationNone;
}

static void nativeDragLeave(CscdWindowNative* n) { cscdViewExternalDragLeave(n->w->view); }

static bool nativeDrop(CscdWindowNative* n, id<NSDraggingInfo> info) {
    const NSPoint p = [NSVIEW(n) convertPoint:info.draggingLocation fromView:nil];
    const bool ok = cscdViewExternalDrop(n->w->view, (float)p.x, (float)p.y);
    [NSApp activateIgnoringOtherApps:YES];
    return ok;
}

void windowDestroyNative(CscdWindow* w) {
    cocoaA11yDetach(w);
    if (menuBar.window == w) {
        menuBar.window = NULL;
        if (menuBar.kept) menuForgetWindow(menuBar.kept);
    }
    @autoreleasepool {
        CscdWindowNative* n = w->native;
        n->w = NULL;
        NSVIEW(n).native = NULL;
        ((__bridge CascadeWindowDelegate*)n->delegate).native = NULL;
        NSWIN(n).delegate = nil;
        if (n->cursorHidden) [NSCursor unhide];
        if (w->hwnd) [NSWIN(n) close];
        w->hwnd = NULL;
        if (n->marked) CFBridgingRelease(n->marked);
        CFBridgingRelease(n->delegate);
        CFBridgingRelease(n->view);
        CFBridgingRelease(n->window);
        cscdFree(n);
        w->native = NULL;
    }
}

/* ---- clipboard -------------------------------------------------------------------------------------- */

static void clipSetText(void* user, CscdSv text) {
    (void)user;
    NSPasteboard* pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    [pb setString:svToNS(text) forType:NSPasteboardTypeString];
}

static void clipGetText(void* user, CscdStr* out) {
    (void)user;
    CscdStr s = {0};
    fromNS([NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString], &s);
    removeCR(&s);
    cscdStrAppend(out, cscdStrSv(&s));
    cscdStrFree(&s);
}

static void clipSetImage(void* user, const CscdImage* img) {
    (void)user;
    if (img->width <= 0 || img->height <= 0) return;
    CscdBytes png = {0};
    cscdEncodePNG(img, &png);
    NSData* data = [NSData dataWithBytes:png.items length:png.len];
    cscdVecFree(&png);
    NSPasteboard* pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    [pb setData:data forType:NSPasteboardTypePNG];
    /* TIFF too, for the apps that only paste that */
    NSImage* image = [[NSImage alloc] initWithData:data];
    if (image) [pb setData:image.TIFFRepresentation forType:NSPasteboardTypeTIFF];
}

static void clipGetFiles(void* user, CscdStrVec* out) {
    (void)user;
    readFiles(NSPasteboard.generalPasteboard, out);
}

/* ---- the system's preferences: the appearance and Reduce motion ------------------------------------ */

static int preferencesChanged; /* (counts the system's notices) */

static void watchPreferences(void) {
    static bool watching;
    if (watching) return;
    watching = true;
    void (^changed)(NSNotification*) = ^(NSNotification* note) {
        (void)note;
        ++preferencesChanged;
        wakeLoop();
    };
    [NSDistributedNotificationCenter.defaultCenter addObserverForName:@"AppleInterfaceThemeChangedNotification" object:nil
                                                                  queue:NSOperationQueue.mainQueue usingBlock:changed];
    [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification object:nil
                                                                  queue:NSOperationQueue.mainQueue usingBlock:changed];
}

static void readPreferences(CscdWindow* w) {
    CscdSystemPreferences p = {false, false, 1};
    if (@available(macOS 10.14, *)) {
        NSAppearanceName name = [NSApp.effectiveAppearance bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
        p.dark = [name isEqualToString:NSAppearanceNameDarkAqua];
    }
    p.reducedMotion = NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion;
    w->native->preferencesSeen = preferencesChanged;
    windowSystemPreferences(w, p);
}

void windowCreateNative(CscdWindow* w, const CscdWindowOptions* opts) {
    @autoreleasepool {
        startApplication();
        CscdWindowNative* n = CSCD_NEW(CscdWindowNative);
        w->native = n;
        n->w = w;
        n->shown = CSCD_CURSOR_DEFAULT;
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        if (opts->resizable) style |= NSWindowStyleMaskResizable;
        const NSRect frame = NSMakeRect(0, 0, opts->width, opts->height);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame styleMask:style backing:NSBackingStoreBuffered defer:NO];
        window.releasedWhenClosed = NO;
        window.title = toNS(opts->title && opts->title[0] ? opts->title : "cascade");
        window.contentMinSize = NSMakeSize(opts->minWidth, opts->minHeight);
        window.acceptsMouseMovedEvents = YES;
        if (opts->resizable) window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        if (opts->darkTitleBar) window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        CascadeView* view = [[CascadeView alloc] initWithFrame:frame];
        view.native = n;
        view.wantsLayer = YES;
        window.contentView = view;
        CascadeWindowDelegate* delegate = [CascadeWindowDelegate new];
        delegate.native = n;
        window.delegate = delegate;
        [view registerForDraggedTypes:@[ NSPasteboardTypeFileURL, NSPasteboardTypeString ]];
        [window makeFirstResponder:view];
        [window center];
        n->window = (void*)CFBridgingRetain(window);
        n->view = (void*)CFBridgingRetain(view);
        n->delegate = (void*)CFBridgingRetain(delegate);
        w->hwnd = n->window;

        w->dpr = (float)window.backingScaleFactor;
        const NSSize size = view.bounds.size;
        w->clientW = cscdMaxi(1, (int)lround(size.width * w->dpr));
        w->clientH = cscdMaxi(1, (int)lround(size.height * w->dpr));
        w->presenter = windowPresenterNew(w, n->view, opts->backend, w->clientW, w->clientH);
        cscdViewSetViewport(w->view, (float)size.width, (float)size.height, w->dpr);
        cscdViewSetClipboardHooks(w->view, clipSetText, clipGetText, clipSetImage, clipGetFiles, w);
        watchPreferences();
        readPreferences(w);
        cocoaA11yAttach(w);
    }
}

void* windowSurface(const CscdWindow* w) { return w->native->view; }

void windowShowTitle(CscdWindow* w, const char* title) { NSWIN(w->native).title = toNS(title); }

void windowWake(CscdWindow* w) {
    (void)w;
    wakeLoop();
}

void cscdWindowShow(CscdWindow* w) {
    if (!w->hwnd || w->shown) return;
    w->shown = true;
    [NSWIN(w->native) makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

void windowPlaceInputMethod(CscdWindow* w) {
    if (w->native && w->native->view) [NSVIEW(w->native).inputContext invalidateCharacterCoordinates];
}

void cscdWindowResize(CscdWindow* w, int width, int height) {
    if (w->hwnd) [NSWIN(w->native) setContentSize:NSMakeSize(width, height)];
}

void cscdWindowMaximize(CscdWindow* w) {
    if (!w->hwnd || w->fullscreen) return;
    if (!NSWIN(w->native).isZoomed) [NSWIN(w->native) zoom:nil];
    cscdWindowShow(w);
}

void cscdWindowSetFullscreen(CscdWindow* w, bool on) {
    if (!w->hwnd || on == w->fullscreen) return;
    w->fullscreen = on;
    [NSWIN(w->native) toggleFullScreen:nil];
}

void cscdWindowClose(CscdWindow* w) {
    if (!w->hwnd) return;
    w->native->closeRequested = true; /* asked from the loop, like the close button */
    wakeLoop();
}

bool cscdWindowPump(CscdWindow* w, bool wait) {
    if (!w->hwnd || !w->native) return false;
    CscdWindowNative* n = w->native;
    @autoreleasepool {
        NSDate* until = NSDate.distantPast;
        if (wait && !windowHasPosted(w) && !w->dirty && !n->closeRequested && !cscdViewNeedsPaint(w->view)) {
            int ms = cscdViewNextTimerMs(w->view);
            if (w->onFrame) ms = 0;
            until = ms < 0 ? NSDate.distantFuture : [NSDate dateWithTimeIntervalSinceNow:ms / 1000.0];
        }
        NSEvent* e;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES])) {
            until = NSDate.distantPast;
            if (e.type == NSEventTypeApplicationDefined && e.subtype == kPostedSubtype) continue; /* only woke the loop */
            [NSApp sendEvent:e];
            if (!w->hwnd) break;
        }
        [NSApp updateWindows];
        if (w->hwnd && n->closeRequested) {
            n->closeRequested = false;
            if (nativeRequestClose(n)) {
                ((__bridge CascadeWindowDelegate*)n->delegate).native = NULL;
                NSVIEW(n).native = NULL;
                [NSWIN(n) close];
            }
        }
        if (!w->hwnd) return false;
        if (n->preferencesSeen != preferencesChanged) readPreferences(w);
        windowRunPosted(w);
        if (!w->hwnd) return false;
        if (cscdViewUpdate(w->view) || w->onFrame || w->dirty) windowRender(w);
        nativeApplyCursor(n);
    }
    return w->hwnd != NULL;
}

/* ---- the system around the app ------------------------------------------------------------------- */

bool platformOpenUrl(CscdWindow* w, const char* url) {
    (void)w;
    @autoreleasepool {
        NSURL* u = [NSURL URLWithString:toNS(url)];
        return u && [NSWorkspace.sharedWorkspace openURL:u];
    }
}

bool platformShareText(CscdWindow* w, const char* text) { (void)w; (void)text; return false; }
void platformHaptic(CscdWindow* w, CscdHaptic kind) { (void)w; (void)kind; }

bool platformHide(CscdWindow* w) {
    [NSWIN(w->native) orderOut:nil];
    return true;
}

/* ---- the system's menus: the menu bar's, the tray's ------------------------------------------------- */

/* A shortcut as AppKit has it: "Ctrl+Shift+S" is Command-Shift-S (Ctrl and Cmd are both Command). */
static NSString* keyEquivalent(CscdSv shortcut, NSEventModifierFlags* mods) {
    *mods = 0;
    NSString* key = @"";
    while (shortcut.len) {
        size_t plus = cscdSvFindChar(shortcut, '+', 1); /* (from 1: "+" alone, or "Ctrl++", is the key) */
        const CscdSv part = cscdSvSub(shortcut, 0, plus == CSCD_NPOS ? shortcut.len : plus);
        shortcut = plus == CSCD_NPOS ? CSCD_SV("") : cscdSvSub(shortcut, plus + 1, shortcut.len - plus - 1);
        if (cscdSvIEqC(part, "ctrl") || cscdSvIEqC(part, "cmd") || cscdSvIEqC(part, "command") || cscdSvIEqC(part, "meta")) *mods |= NSEventModifierFlagCommand;
        else if (cscdSvIEqC(part, "shift")) *mods |= NSEventModifierFlagShift;
        else if (cscdSvIEqC(part, "alt") || cscdSvIEqC(part, "option")) *mods |= NSEventModifierFlagOption;
        else if (cscdSvIEqC(part, "control")) *mods |= NSEventModifierFlagControl;
        else if (part.len == 1) key = [svToNS(part) lowercaseString];
        else if ((part.ptr[0] == 'F' || part.ptr[0] == 'f') && part.len <= 3 && atoi(part.ptr + 1) >= 1 && atoi(part.ptr + 1) <= 12) {
            const unichar c = (unichar)(NSF1FunctionKey + atoi(part.ptr + 1) - 1);
            key = [NSString stringWithCharacters:&c length:1];
        } else {
            static const struct { const char* name; unichar c; } kKeys[] = {
                {"enter", '\r'}, {"return", '\r'}, {"esc", 27}, {"escape", 27}, {"tab", '\t'}, {"space", ' '},
                {"backspace", NSBackspaceCharacter}, {"delete", NSDeleteCharacter}, {"del", NSDeleteCharacter},
                {"up", NSUpArrowFunctionKey}, {"down", NSDownArrowFunctionKey}, {"left", NSLeftArrowFunctionKey},
                {"right", NSRightArrowFunctionKey}, {"home", NSHomeFunctionKey}, {"end", NSEndFunctionKey},
                {"pageup", NSPageUpFunctionKey}, {"pagedown", NSPageDownFunctionKey}};
            for (size_t i = 0; i < CSCD_COUNT(kKeys); ++i)
                if (cscdSvIEqC(part, kKeys[i].name)) key = [NSString stringWithCharacters:&kKeys[i].c length:1];
        }
    }
    return key;
}

/* What a native menu does: runs the chosen item's action (target of every item), fills a submenu
 * as it opens (delegate of a menu whose item has a fill). Each holds what it built. */
@interface CascadeMenuHandler : NSObject <NSMenuDelegate>
@property(nonatomic, assign) CscdWindow* window;
@property(nonatomic, assign) const CscdMenuItem* fillFrom;
@property(nonatomic, strong) NSMutableArray* kept; /* (the handlers of the menus inside) */
- (void)choose:(NSMenuItem*)item;
@end

static void buildNSMenu(NSMenu* out, const CscdMenu* m, CscdWindow* w, NSMutableArray* kept);
static NSMenu* submenuOf(const CscdMenuItem* it, CscdWindow* w, NSMutableArray* kept);

@implementation CascadeMenuHandler {
    CscdMenu filled;
}
- (void)choose:(NSMenuItem*)item {
    windowMenuChosen(self.window, (const CscdMenuItem*)[(NSValue*)item.representedObject pointerValue]);
}
- (void)menuNeedsUpdate:(NSMenu*)menu {
    if (!self.fillFrom || !self.fillFrom->fill.fn) return;
    [menu removeAllItems];
    cscdMenuFree(&filled);
    self.fillFrom->fill.fn(self.fillFrom->fill.user, &filled);
    self.kept = [NSMutableArray array];
    buildNSMenu(menu, &filled, self.window, self.kept);
}
- (void)dealloc { cscdMenuFree(&filled); }
@end

/* the window gone: the menus' items still run their actions, with nothing to redraw */
static void menuForgetWindow(void* keptPtr) {
    for (CascadeMenuHandler* h in (__bridge NSArray*)keptPtr) {
        h.window = NULL;
        if (h.kept) menuForgetWindow((__bridge void*)h.kept);
    }
}

/* the menu an item opens: its submenu, or one its fill fills as it opens */
static NSMenu* submenuOf(const CscdMenuItem* it, CscdWindow* w, NSMutableArray* kept) {
    NSMenu* sub = [[NSMenu alloc] initWithTitle:svToNS(cscdStrSv(&it->label))];
    if (it->submenu.len) {
        buildNSMenu(sub, &it->submenu, w, kept);
    } else if (it->fill.fn) {
        CascadeMenuHandler* filler = [CascadeMenuHandler new];
        filler.window = w;
        filler.fillFrom = it;
        [kept addObject:filler];
        sub.delegate = filler;
    }
    return sub;
}

static void buildNSMenu(NSMenu* out, const CscdMenu* m, CscdWindow* w, NSMutableArray* kept) {
    CascadeMenuHandler* target = [CascadeMenuHandler new];
    target.window = w;
    [kept addObject:target];
    out.autoenablesItems = NO;
    for (size_t i = 0; i < m->len; ++i) {
        const CscdMenuItem* it = &m->items[i];
        if (it->separator) {
            [out addItem:NSMenuItem.separatorItem];
            continue;
        }
        NSEventModifierFlags mods = 0;
        NSString* key = keyEquivalent(cscdStrSv(&it->shortcut), &mods);
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:svToNS(cscdStrSv(&it->label)) action:NULL keyEquivalent:key];
        item.keyEquivalentModifierMask = mods;
        item.enabled = it->enabled;
        if (it->checkable) item.state = it->checked ? NSControlStateValueOn : NSControlStateValueOff;
        if (it->tooltip.len) item.toolTip = svToNS(cscdStrSv(&it->tooltip));
        if (it->submenu.len || it->fill.fn) {
            item.submenu = submenuOf(it, w, kept);
        } else {
            item.target = target;
            item.action = @selector(choose:);
            item.representedObject = [NSValue valueWithPointer:it];
        }
        [out addItem:item];
    }
}

bool platformSetMenuBar(CscdWindow* w, CscdMenu* menu) {
    @autoreleasepool {
        NSMutableArray* kept = [NSMutableArray array];
        NSMenu* bar = [NSMenu new];
        [bar addItem:appMenuItem()];
        CscdMenu tops = *menu;
        for (size_t i = 0; i < tops.len; ++i) {
            const CscdMenuItem* it = &tops.items[i];
            if (it->separator) continue;
            NSMenuItem* top = [[NSMenuItem alloc] initWithTitle:svToNS(cscdStrSv(&it->label)) action:NULL keyEquivalent:@""];
            top.submenu = submenuOf(it, w, kept);
            [bar addItem:top];
        }
        NSApp.mainMenu = bar;
        cscdMenuFree(&menuBar.menu);
        if (menuBar.kept) CFBridgingRelease(menuBar.kept);
        menuBar.menu = tops;
        menuBar.window = w;
        menuBar.kept = (void*)CFBridgingRetain(kept);
        *menu = (CscdMenu){0};
        return true;
    }
}

/* ---- the tray: a status item in the menu bar --------------------------------------------------------- */

@interface CascadeTrayTarget : NSObject
@property(nonatomic, assign) CscdTray* tray;
@property(nonatomic, strong) NSStatusItem* item;
@property(nonatomic, strong) NSMutableArray* kept;
- (void)clicked:(id)sender;
@end
@implementation CascadeTrayTarget
- (void)clicked:(id)sender {
    CscdTray* t = self.tray;
    if (!t) return;
    NSEvent* e = NSApp.currentEvent;
    const bool secondary = e.type == NSEventTypeRightMouseUp || (e.modifierFlags & NSEventModifierFlagControl);
    if (t->click.fn && (!secondary || !t->menu.len)) {
        windowTrayClicked(t);
        return;
    }
    if (!t->menu.len) return;
    NSMenu* menu = [NSMenu new];
    self.kept = [NSMutableArray array];
    buildNSMenu(menu, &t->menu, t->w, self.kept);
    self.item.menu = menu; /* (opened by the click below, then taken off: a left click stays the action's) */
    [self.item.button performClick:nil];
    self.item.menu = nil;
}
@end

static NSImage* trayImage(const CscdImage* img) {
    if (cscdImageEmpty(img)) return nil;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef cg = CGBitmapContextCreate(img->pixels, (size_t)img->width, (size_t)img->height, 8, (size_t)img->width * 4, space,
                                            kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    CGImageRef cgImage = cg ? CGBitmapContextCreateImage(cg) : NULL;
    if (cg) CGContextRelease(cg);
    CGColorSpaceRelease(space);
    if (!cgImage) return nil;
    const CGFloat side = NSStatusBar.systemStatusBar.thickness - 4;
    const CGFloat aspect = img->height > 0 ? (CGFloat)img->width / img->height : 1;
    NSImage* out = [[NSImage alloc] initWithCGImage:cgImage size:NSMakeSize(side * aspect, side)];
    CGImageRelease(cgImage);
    return out;
}

bool platformTrayCreate(CscdTray* t, const CscdImage* icon, const char* tooltip) {
    @autoreleasepool {
        NSStatusItem* item = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
        if (!item || !item.button) return false;
        CascadeTrayTarget* target = [CascadeTrayTarget new];
        target.tray = t;
        target.item = item;
        item.button.image = trayImage(icon);
        if (!item.button.image) item.button.title = toNS(tooltip);
        item.button.toolTip = toNS(tooltip);
        item.button.target = target;
        item.button.action = @selector(clicked:);
        [item.button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
        t->native = (void*)CFBridgingRetain(target);
        return true;
    }
}

void platformTraySet(CscdTray* t, const CscdImage* icon, const char* tooltip) {
    @autoreleasepool {
        CascadeTrayTarget* target = (__bridge CascadeTrayTarget*)t->native;
        if (icon) target.item.button.image = trayImage(icon);
        if (tooltip) target.item.button.toolTip = toNS(tooltip);
    }
}

void platformTrayDestroy(CscdTray* t) {
    if (!t->native) return;
    @autoreleasepool {
        CascadeTrayTarget* target = (CascadeTrayTarget*)CFBridgingRelease(t->native);
        target.tray = NULL;
        [NSStatusBar.systemStatusBar removeStatusItem:target.item];
        target.item = nil;
        t->native = NULL;
    }
}

bool platformPickFiles(CscdWindow* w, const char* accept, bool multiple) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.allowsMultipleSelection = multiple;
        panel.canChooseDirectories = NO;
        panel.canChooseFiles = YES;
        CscdStrVec exts = {0};
        windowAcceptExtensions(accept, &exts);
        if (exts.len) {
            NSMutableArray<NSString*>* types = [NSMutableArray array];
            for (size_t i = 0; i < exts.len; ++i) [types addObject:svToNS(cscdStrSv(&exts.items[i]))];
            panel.allowedFileTypes = types; /* (deprecated for UTTypes, still works) */
        }
        cscdStrVecFree(&exts);
        CscdStrVec paths = {0};
        if ([panel runModal] == NSModalResponseOK)
            for (NSURL* u in panel.URLs)
                if (u.fileURL) {
                    CscdStr p = {0};
                    fromNS(u.path, &p);
                    cscdVecPush(&paths, p);
                }
        windowFilesPicked(w, &paths);
        cscdStrVecFree(&paths);
        return true;
    }
}

bool platformSaveFile(CscdWindow* w, const char* name, const char* accept) {
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        if (name[0]) panel.nameFieldStringValue = toNS(name);
        CscdStrVec exts = {0};
        windowAcceptExtensions(accept, &exts);
        if (exts.len) {
            NSMutableArray<NSString*>* types = [NSMutableArray array];
            for (size_t i = 0; i < exts.len; ++i) [types addObject:svToNS(cscdStrSv(&exts.items[i]))];
            panel.allowedFileTypes = types; /* (deprecated for UTTypes, still works; the first goes on a bare name) */
            panel.allowsOtherFileTypes = YES;
        }
        cscdStrVecFree(&exts);
        CscdStr path = {0};
        if ([panel runModal] == NSModalResponseOK && panel.URL.fileURL) fromNS(panel.URL.path, &path);
        windowSaveChosen(w, path.len ? cscdStrC(&path) : NULL);
        cscdStrFree(&path);
        return true;
    }
}
