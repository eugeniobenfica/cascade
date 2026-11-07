/* UIKit window (iOS, iPadOS): a UIWindow whose view controller holds the page's view inside the safe
 * area (and above the on-screen keyboard; around it, the page's edge colors), touch, the pointer and
 * scrolling of an iPad's trackpad or mouse, hardware keys (UIPress), the on-screen keyboard through
 * UITextInput (the focused field's text: suggestions, corrections, dictation, the marked text of
 * input methods that compose; the keyboard of the field's kind), the system's edit menu and loupe
 * over a selection made by touch (the view draws its handles), the general pasteboard (text and
 * pictures), files and text dropped from other apps, VoiceOver (a11yUikit.m), the app's life and
 * the system's theme, Dynamic Type and Reduce Motion, and frame presentation through Core Animation
 * (the frame becomes the view's layer contents; the system composites it on the GPU), paced by the
 * screen's refresh (120 Hz on ProMotion screens) while the page animates.
 *
 * The program's main runs on the main thread, inside UIKit (mainUikit.m), and its loop runs UIKit's
 * run loop. Shortcuts follow the Mac: Command is reported as Ctrl and Control as Meta.
 * Objective-C with ARC (CMakeLists.txt). The C structs hold the Objective-C objects as retained void
 * pointers (CFBridgingRetain / CFBridgingRelease). */
#include "platform/common/platformInternal.h"

#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <math.h>
#include <stdio.h>

bool uikitStarted;

@class CascadeView;
@class CascadeViewController;

/* (the display link's target: its firing only wakes the loop) */
@interface CascadeTicker : NSObject
- (void)tick:(CADisplayLink*)link;
@end
@implementation CascadeTicker
- (void)tick:(CADisplayLink*)link {}
@end

static NSString* svToNS(CscdSv s) {
    return [[NSString alloc] initWithBytes:(s.ptr ? s.ptr : "") length:s.len encoding:NSUTF8StringEncoding] ?: @"";
}

static NSString* toNSC(const char* s) { return svToNS(cscdSv(s)); }

/* appended to out */
static void fromNS(NSString* s, CscdStr* out) {
    if (s) cscdStrAppendC(out, s.UTF8String ?: "");
}

/* ---- waking the loop ------------------------------------------------------------------------------ */

/* A run loop source that does nothing: signalled, it brings a waiting cscdWindowPump round (from
 * any thread). */
static CFRunLoopSourceRef wakeSource;

static void wakePerform(void* info) { (void)info; }

static void wakeLoop(void) {
    if (!wakeSource) return;
    CFRunLoopSourceSignal(wakeSource);
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

/* ---- the presenter: Core Animation ------------------------------------------------------------------ */

typedef struct SoftwarePresenter { CscdPresenter base; void* view; /* UIView, retained */ } SoftwarePresenter;

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
    UIView* view = (__bridge UIView*)p->view;
    NSData* data = [NSData dataWithBytes:frame->pixels length:cscdImagePixelCount(frame) * 4];
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image = CGImageCreate((size_t)frame->width, (size_t)frame->height, 8, 32, (size_t)frame->width * 4, space,
                                     (CGBitmapInfo)((uint32_t)kCGBitmapByteOrder32Little | (uint32_t)kCGImageAlphaPremultipliedFirst), provider, NULL, false,
                                     kCGRenderingIntentDefault);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    view.layer.contents = (__bridge id)image;
    view.layer.contentsGravity = kCAGravityResize; /* (the frame is the view's size in device pixels) */
    [CATransaction commit];
    /* out now: while frames follow each other the loop never sleeps, where UIKit would commit */
    [CATransaction flush];
    CGImageRelease(image);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    return true;
}

static const CscdPresenterVtbl swVtbl = {swBackend, swPresent, swResize, swDescription, swDestroy};

CscdPresenter* platformSoftwarePresenter(void* nativeWindow, int width, int height) {
    (void)width; (void)height;
    SoftwarePresenter* p = CSCD_NEW(SoftwarePresenter);
    p->base.vt = &swVtbl;
    p->view = (void*)CFBridgingRetain((__bridge UIView*)nativeWindow);
    return &p->base;
}

/* iOS has neither desktop OpenGL nor a Vulkan loader of its own (Core Animation is on the GPU). */
CscdPresenter* platformGLPresenter(void* nativeWindow, int width, int height) {
    (void)nativeWindow; (void)width; (void)height;
    return NULL;
}
void* platformMetalLayer(void* nsview) { (void)nsview; return NULL; }

/* ---- the window ----------------------------------------------------------------------------------- */

#define kMaxTouches 16

struct CscdWindowNative {
    CscdWindow* w;
    void* window;     /* UIWindow, retained */
    void* controller; /* CascadeViewController, retained */
    void* view;       /* CascadeView, retained */
    bool closeRequested;
    bool keyboardWanted; /* a text field has the focus: the view is first responder */
    float keyboardTop;   /* where the on-screen keyboard starts, in the controller's view (0: none) */
    void* touches[kMaxTouches]; /* the UITouch of each touch id (not retained) */
    int pointerButtons;         /* an iPad pointer's buttons down (1 left, 2 right) */
    CGPoint scrollLast;
    /* the keyboard: the field's traits and its text as UIKit last knew them */
    CscdInputTraits traits;
    CscdTextState kitState;
    bool kitHasState;
    void* displayLink; /* CADisplayLink, retained: wakes the loop at the screen's rate while the page animates */
    void* picker;      /* the document picker's delegate, retained while it is open */
    void* drop;        /* CscdDataTransfer of a drag from another app over the page (a reference) */
    /* touch selection: the system's edit menu (and what it offers) and the loupe */
    bool editMenuShown;
    unsigned editActions;
    CGRect editMenuRect;
    void* editMenu;         /* UIEditMenuInteraction (iOS 16 and later), retained */
    void* editMenuDelegate; /* its delegate, retained */
    void* loupe;            /* UITextLoupeSession (iOS 17 and later), retained while a handle is dragged */
};

static CscdWindowNative* liveNative; /* the window's, while it is open (one window: the screen's) */

#define UIWIN(n) ((__bridge UIWindow*)(n)->window)
#define UIVIEW(n) ((__bridge CascadeView*)(n)->view)
#define UICONTROLLER(n) ((__bridge CascadeViewController*)(n)->controller)

static void nativeResized(CscdWindowNative* n);
static void nativeTouches(CscdWindowNative* n, NSSet<UITouch*>* touches, UIEvent* e, int phase);
static void nativePresses(CscdWindowNative* n, NSSet<UIPress*>* presses, bool down, bool* passOn);
static void nativeText(CscdWindowNative* n, NSString* s);
static void nativeBackspace(CscdWindowNative* n);
static void nativeHover(CscdWindowNative* n, UIHoverGestureRecognizer* g);
static void nativeScroll(CscdWindowNative* n, UIPanGestureRecognizer* g);

/* ---- the focused field's text, as UIKit's text input sees it (UTF-16 offsets) ------------------------- */

@interface CascadePosition : UITextPosition
@property(nonatomic) NSInteger offset;
+ (instancetype)at:(NSInteger)offset;
@end
@implementation CascadePosition
+ (instancetype)at:(NSInteger)offset {
    CascadePosition* p = [CascadePosition new];
    p.offset = offset;
    return p;
}
@end

@interface CascadeRange : UITextRange
@property(nonatomic) NSInteger from, to;
+ (instancetype)from:(NSInteger)a to:(NSInteger)z;
@end
@implementation CascadeRange
+ (instancetype)from:(NSInteger)a to:(NSInteger)z {
    CascadeRange* r = [CascadeRange new];
    r.from = MIN(a, z);
    r.to = MAX(a, z);
    return r;
}
- (UITextPosition*)start { return [CascadePosition at:_from]; }
- (UITextPosition*)end { return [CascadePosition at:_to]; }
- (BOOL)isEmpty { return _from == _to; }
@end

static NSInteger posOf(UITextPosition* p) { return [p isKindOfClass:CascadePosition.class] ? ((CascadePosition*)p).offset : 0; }

/* UTF-8 bytes <-> UTF-16 units of the same text */
static NSInteger unitsBefore(CscdSv utf8, size_t bytes) {
    bytes = cscdMinz(bytes, utf8.len);
    NSString* s = [[NSString alloc] initWithBytes:utf8.ptr length:bytes encoding:NSUTF8StringEncoding];
    return (NSInteger)s.length;
}
static size_t bytesBefore(NSString* s, NSInteger units) {
    units = MAX(0, MIN(units, (NSInteger)s.length));
    return [[s substringToIndex:(NSUInteger)units] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
}

typedef struct KitText {
    NSInteger anchor, caret;    /* the selection */
    NSInteger markFrom, markTo; /* the marked text (markFrom == markTo: none) */
} KitText;

/* the focused field's text, nil without one */
static NSString* kitRead(CscdWindowNative* n, KitText* k) {
    memset(k, 0, sizeof *k);
    if (!n || !n->w) return nil;
    CscdTextState s;
    if (!cscdViewGetTextState(n->w->view, &s)) return nil;
    const CscdSv t = cscdStrSv(&s.text);
    NSString* text = svToNS(t);
    k->anchor = unitsBefore(t, s.selStart);
    k->caret = unitsBefore(t, s.selEnd);
    k->markFrom = unitsBefore(t, s.composeStart);
    k->markTo = unitsBefore(t, s.composeEnd);
    cscdTextStateFini(&s);
    return text;
}

static void kitWrite(CscdWindowNative* n, NSString* text, NSInteger anchor, NSInteger caret, NSInteger markFrom, NSInteger markTo) {
    CscdTextState s;
    memset(&s, 0, sizeof s);
    fromNS(text, &s.text);
    s.selStart = bytesBefore(text, anchor);
    s.selEnd = bytesBefore(text, caret);
    if (markTo > markFrom) {
        s.composeStart = bytesBefore(text, markFrom);
        s.composeEnd = bytesBefore(text, markTo);
    }
    cscdViewSetTextState(n->w->view, &s);
    cscdTextStateFini(&s);
    /* what UIKit did itself is not told back to it */
    cscdTextStateFini(&n->kitState);
    n->kitHasState = cscdViewGetTextState(n->w->view, &n->kitState);
}

static void nativeEnter(CscdWindowNative* n);
static void nativeEditAction(CscdWindowNative* n, CscdEditAction a);

/* the edit menu of iOS 16 and later: the actions the view says it can do, around the selection */
API_AVAILABLE(ios(16.0))
@interface CascadeEditMenuDelegate : NSObject <UIEditMenuInteractionDelegate>
@property(nonatomic, assign) CscdWindowNative* native;
@end

@implementation CascadeEditMenuDelegate
- (UIMenu*)editMenuInteraction:(UIEditMenuInteraction*)i menuForConfiguration:(UIEditMenuConfiguration*)c
              suggestedActions:(NSArray<UIMenuElement*>*)suggested {
    return [UIMenu menuWithChildren:suggested];
}
- (CGRect)editMenuInteraction:(UIEditMenuInteraction*)i targetRectForConfiguration:(UIEditMenuConfiguration*)c {
    return _native ? _native->editMenuRect : CGRectNull; /* (the menu keeps clear of the selection) */
}
@end

@interface CascadeView : UIView <UITextInput>
@property(nonatomic, assign) CscdWindowNative* native;
@property(nonatomic, weak) id<UITextInputDelegate> inputDelegate;
@property(nonatomic, copy) NSDictionary<NSAttributedStringKey, id>* markedTextStyle;
@property(nonatomic, strong) UITextInputStringTokenizer* stringTokenizer;
@end

@implementation CascadeView
- (BOOL)canBecomeFirstResponder { return YES; }
- (void)layoutSubviews {
    [super layoutSubviews];
    if (_native) nativeResized(_native);
}
- (void)touchesBegan:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { if (_native) nativeTouches(_native, t, e, 0); }
- (void)touchesMoved:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { if (_native) nativeTouches(_native, t, e, 1); }
- (void)touchesEnded:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { if (_native) nativeTouches(_native, t, e, 2); }
- (void)touchesCancelled:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { if (_native) nativeTouches(_native, t, e, 3); }
- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)e {
    bool passOn = true;
    if (_native) nativePresses(_native, presses, true, &passOn);
    if (passOn) [super pressesBegan:presses withEvent:e]; /* (typing keys: their text comes to insertText) */
}
- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)e {
    bool passOn = true;
    if (_native) nativePresses(_native, presses, false, &passOn);
    if (passOn) [super pressesEnded:presses withEvent:e];
}
- (void)pressesCancelled:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)e {
    bool passOn = true;
    if (_native) nativePresses(_native, presses, false, &passOn);
    if (passOn) [super pressesCancelled:presses withEvent:e];
}
/* UITextInput: the on-screen keyboard, with its suggestions, corrections, dictation and the marked
 * text of input methods that compose (the text of typing keys comes here too) */
- (BOOL)hasText { return YES; } /* (deleteBackward always comes) */
- (void)insertText:(NSString*)text {
    if (!_native) return;
    if ([text isEqualToString:@"\n"]) {
        nativeEnter(_native);
        return;
    }
    KitText k;
    NSString* s = kitRead(_native, &k);
    if (!s) {
        nativeText(_native, text);
        return;
    }
    /* the marked text, or else the selection, becomes the text */
    const NSInteger a = k.markTo > k.markFrom ? k.markFrom : MIN(k.anchor, k.caret);
    const NSInteger z = k.markTo > k.markFrom ? k.markTo : MAX(k.anchor, k.caret);
    NSString* t = [s stringByReplacingCharactersInRange:NSMakeRange((NSUInteger)a, (NSUInteger)(z - a)) withString:text];
    const NSInteger at = a + (NSInteger)text.length;
    kitWrite(_native, t, at, at, 0, 0);
}
- (void)deleteBackward {
    if (!_native) return;
    KitText k;
    NSString* s = kitRead(_native, &k);
    if (s && k.markTo > k.markFrom) {
        /* (marked text shrinks by its last character) */
        const NSRange last = [s rangeOfComposedCharacterSequenceAtIndex:(NSUInteger)MAX(k.markFrom, k.markTo - 1)];
        NSString* t = [s stringByReplacingCharactersInRange:last withString:@""];
        const NSInteger markTo = (NSInteger)last.location;
        kitWrite(_native, t, markTo, markTo, k.markFrom, markTo);
        return;
    }
    nativeBackspace(_native);
}
- (NSString*)textInRange:(UITextRange*)range {
    KitText k;
    NSString* s = kitRead(_native, &k);
    if (!s || ![range isKindOfClass:CascadeRange.class]) return @"";
    const NSInteger a = MIN(((CascadeRange*)range).from, (NSInteger)s.length), z = MIN(((CascadeRange*)range).to, (NSInteger)s.length);
    return [s substringWithRange:NSMakeRange((NSUInteger)a, (NSUInteger)(z - a))];
}
- (void)replaceRange:(UITextRange*)range withText:(NSString*)text {
    KitText k;
    NSString* s = kitRead(_native, &k);
    if (!s || ![range isKindOfClass:CascadeRange.class]) return;
    const NSInteger a = MIN(((CascadeRange*)range).from, (NSInteger)s.length), z = MIN(((CascadeRange*)range).to, (NSInteger)s.length);
    NSString* t = [s stringByReplacingCharactersInRange:NSMakeRange((NSUInteger)a, (NSUInteger)(z - a)) withString:text];
    const NSInteger at = a + (NSInteger)text.length;
    kitWrite(_native, t, at, at, 0, 0);
}
- (UITextRange*)selectedTextRange {
    KitText k;
    return kitRead(_native, &k) ? [CascadeRange from:k.anchor to:k.caret] : nil;
}
- (void)setSelectedTextRange:(UITextRange*)range {
    KitText k;
    NSString* s = kitRead(_native, &k);
    if (!s || ![range isKindOfClass:CascadeRange.class]) return;
    kitWrite(_native, s, ((CascadeRange*)range).from, ((CascadeRange*)range).to, k.markFrom, k.markTo);
}
- (UITextRange*)markedTextRange {
    KitText k;
    return kitRead(_native, &k) && k.markTo > k.markFrom ? [CascadeRange from:k.markFrom to:k.markTo] : nil;
}
- (void)setMarkedText:(NSString*)marked selectedRange:(NSRange)sel {
    KitText k;
    NSString* s = kitRead(_native, &k);
    if (!s) return;
    if (!marked) marked = @"";
    const NSInteger a = k.markTo > k.markFrom ? k.markFrom : MIN(k.anchor, k.caret);
    const NSInteger z = k.markTo > k.markFrom ? k.markTo : MAX(k.anchor, k.caret);
    NSString* t = [s stringByReplacingCharactersInRange:NSMakeRange((NSUInteger)a, (NSUInteger)(z - a)) withString:marked];
    const NSInteger at = a + (NSInteger)MIN(sel.location, marked.length);
    kitWrite(_native, t, at, at + (NSInteger)sel.length, a, a + (NSInteger)marked.length);
}
- (void)unmarkText {
    KitText k;
    NSString* s = kitRead(_native, &k);
    if (s && k.markTo > k.markFrom) kitWrite(_native, s, k.anchor, k.caret, 0, 0);
}
- (UITextPosition*)beginningOfDocument { return [CascadePosition at:0]; }
- (UITextPosition*)endOfDocument {
    KitText k;
    return [CascadePosition at:(NSInteger)kitRead(_native, &k).length];
}
- (UITextRange*)textRangeFromPosition:(UITextPosition*)a toPosition:(UITextPosition*)z { return [CascadeRange from:posOf(a) to:posOf(z)]; }
- (UITextPosition*)positionFromPosition:(UITextPosition*)p offset:(NSInteger)offset {
    KitText k;
    const NSInteger at = posOf(p) + offset, len = (NSInteger)kitRead(_native, &k).length;
    return at < 0 || at > len ? nil : [CascadePosition at:at];
}
- (UITextPosition*)positionFromPosition:(UITextPosition*)p inDirection:(UITextLayoutDirection)d offset:(NSInteger)offset {
    const bool back = d == UITextLayoutDirectionLeft || d == UITextLayoutDirectionUp;
    return [self positionFromPosition:p offset:back ? -offset : offset];
}
- (NSComparisonResult)comparePosition:(UITextPosition*)a toPosition:(UITextPosition*)z {
    const NSInteger x = posOf(a), y = posOf(z);
    return x < y ? NSOrderedAscending : x > y ? NSOrderedDescending : NSOrderedSame;
}
- (NSInteger)offsetFromPosition:(UITextPosition*)a toPosition:(UITextPosition*)z { return posOf(z) - posOf(a); }
- (id<UITextInputTokenizer>)tokenizer {
    if (!_stringTokenizer) _stringTokenizer = [[UITextInputStringTokenizer alloc] initWithTextInput:self];
    return _stringTokenizer;
}
- (UITextPosition*)positionWithinRange:(UITextRange*)range farthestInDirection:(UITextLayoutDirection)d {
    const bool back = d == UITextLayoutDirectionLeft || d == UITextLayoutDirectionUp;
    return back ? range.start : range.end;
}
- (UITextRange*)characterRangeByExtendingPosition:(UITextPosition*)p inDirection:(UITextLayoutDirection)d {
    KitText k;
    const NSInteger at = posOf(p), len = (NSInteger)kitRead(_native, &k).length;
    const bool back = d == UITextLayoutDirectionLeft || d == UITextLayoutDirectionUp;
    return [CascadeRange from:at to:back ? MAX(0, at - 1) : MIN(len, at + 1)];
}
- (NSWritingDirection)baseWritingDirectionForPosition:(UITextPosition*)p inDirection:(UITextStorageDirection)d { return NSWritingDirectionNatural; }
- (void)setBaseWritingDirection:(NSWritingDirection)wd forRange:(UITextRange*)range {}
/* where the text is: for the keyboard's suggestions and the input method's candidates (the page
 * draws its own caret and selection) */
- (CGRect)rangeRect:(UITextRange*)range {
    KitText k;
    NSString* s = kitRead(_native, &k);
    if (!s || ![range isKindOfClass:CascadeRange.class]) return CGRectZero;
    const size_t a = bytesBefore(s, ((CascadeRange*)range).from), z = bytesBefore(s, ((CascadeRange*)range).to);
    const CscdRect r = cscdViewTextRangeRect(_native->w->view, a, z);
    return CGRectMake(r.x, r.y, r.w, r.h);
}
- (CGRect)firstRectForRange:(UITextRange*)range { return [self rangeRect:range]; }
- (CGRect)caretRectForPosition:(UITextPosition*)p {
    CGRect r = [self rangeRect:[CascadeRange from:posOf(p) to:posOf(p)]];
    r.size.width = 2;
    return r;
}
- (NSArray<UITextSelectionRect*>*)selectionRectsForRange:(UITextRange*)range { return @[]; }
- (UITextPosition*)closestPositionToPoint:(CGPoint)point {
    KitText k;
    NSString* s = kitRead(_native, &k);
    if (!s) return [CascadePosition at:0];
    CscdTextState st;
    if (!cscdViewGetTextState(_native->w->view, &st)) return [CascadePosition at:0];
    const size_t at = cscdViewTextOffsetAt(_native->w->view, (float)point.x, (float)point.y);
    const NSInteger units = unitsBefore(cscdStrSv(&st.text), at);
    cscdTextStateFini(&st);
    return [CascadePosition at:units];
}
- (UITextPosition*)closestPositionToPoint:(CGPoint)point withinRange:(UITextRange*)range {
    const NSInteger at = posOf([self closestPositionToPoint:point]);
    if (![range isKindOfClass:CascadeRange.class]) return [CascadePosition at:at];
    return [CascadePosition at:MAX(((CascadeRange*)range).from, MIN(((CascadeRange*)range).to, at))];
}
- (UITextRange*)characterRangeAtPoint:(CGPoint)point {
    const NSInteger at = posOf([self closestPositionToPoint:point]);
    return [CascadeRange from:at to:at];
}
/* the keyboard the field asks for (its type, inputmode, enterkeyhint, autocapitalize, autocorrect) */
- (UIKeyboardType)keyboardType {
    switch (_native ? _native->traits.kind : CSCD_INPUT_TEXT) {
    case CSCD_INPUT_NUMBER: return UIKeyboardTypeNumberPad;
    case CSCD_INPUT_DECIMAL: return UIKeyboardTypeDecimalPad;
    case CSCD_INPUT_TEL: return UIKeyboardTypePhonePad;
    case CSCD_INPUT_EMAIL: return UIKeyboardTypeEmailAddress;
    case CSCD_INPUT_URL: return UIKeyboardTypeURL;
    case CSCD_INPUT_SEARCH: return UIKeyboardTypeWebSearch;
    default: return UIKeyboardTypeDefault;
    }
}
- (UIReturnKeyType)returnKeyType {
    switch (_native ? _native->traits.enter : CSCD_ENTER_DEFAULT) {
    case CSCD_ENTER_DONE: return UIReturnKeyDone;
    case CSCD_ENTER_GO: return UIReturnKeyGo;
    case CSCD_ENTER_NEXT: return UIReturnKeyNext;
    case CSCD_ENTER_SEARCH: return UIReturnKeySearch;
    case CSCD_ENTER_SEND: return UIReturnKeySend;
    default: return UIReturnKeyDefault;
    }
}
- (UITextAutocorrectionType)autocorrectionType {
    return _native && _native->traits.autocorrect ? UITextAutocorrectionTypeYes : UITextAutocorrectionTypeNo;
}
- (UITextSpellCheckingType)spellCheckingType {
    return _native && _native->traits.autocorrect ? UITextSpellCheckingTypeYes : UITextSpellCheckingTypeNo;
}
- (UITextAutocapitalizationType)autocapitalizationType {
    switch (_native ? _native->traits.capitalize : CSCD_CAPITALIZE_NONE) {
    case CSCD_CAPITALIZE_SENTENCES: return UITextAutocapitalizationTypeSentences;
    case CSCD_CAPITALIZE_WORDS: return UITextAutocapitalizationTypeWords;
    case CSCD_CAPITALIZE_CHARACTERS: return UITextAutocapitalizationTypeAllCharacters;
    default: return UITextAutocapitalizationTypeNone;
    }
}
- (BOOL)isSecureTextEntry { return _native && _native->traits.kind == CSCD_INPUT_PASSWORD; }
- (UITextSmartQuotesType)smartQuotesType { return [self autocorrectionType] == UITextAutocorrectionTypeYes ? UITextSmartQuotesTypeDefault : UITextSmartQuotesTypeNo; }
- (UITextSmartDashesType)smartDashesType { return [self autocorrectionType] == UITextAutocorrectionTypeYes ? UITextSmartDashesTypeDefault : UITextSmartDashesTypeNo; }
/* the edit menu's actions: UIKit asks which the view can do while it shows (and only then: the
 * hardware keyboard's shortcuts are the page's) */
- (BOOL)canPerformAction:(SEL)a withSender:(id)sender {
    const bool edit = a == @selector(cut:) || a == @selector(copy:) || a == @selector(paste:) || a == @selector(selectAll:);
    if (!edit) return [super canPerformAction:a withSender:sender];
    if (!_native || !_native->editMenuShown) return NO;
    const unsigned bits = _native->editActions;
    if (a == @selector(cut:)) return (bits & CSCD_EDIT_BIT(CSCD_EDIT_CUT)) != 0;
    if (a == @selector(copy:)) return (bits & CSCD_EDIT_BIT(CSCD_EDIT_COPY)) != 0;
    if (a == @selector(paste:)) return (bits & CSCD_EDIT_BIT(CSCD_EDIT_PASTE)) != 0;
    return (bits & CSCD_EDIT_BIT(CSCD_EDIT_SELECT_ALL)) != 0;
}
- (void)cut:(id)sender { if (_native) nativeEditAction(_native, CSCD_EDIT_CUT); }
- (void)copy:(id)sender { if (_native) nativeEditAction(_native, CSCD_EDIT_COPY); }
- (void)paste:(id)sender { if (_native) nativeEditAction(_native, CSCD_EDIT_PASTE); }
- (void)selectAll:(id)sender { if (_native) nativeEditAction(_native, CSCD_EDIT_SELECT_ALL); }
/* VoiceOver */
- (BOOL)isAccessibilityElement { return NO; }
- (NSArray*)accessibilityElements { return _native ? (__bridge_transfer NSArray*)uikitA11yElements(_native->w) : @[]; }
- (void)hovered:(UIHoverGestureRecognizer*)g { if (_native) nativeHover(_native, g); }
- (void)scrolled:(UIPanGestureRecognizer*)g { if (_native) nativeScroll(_native, g); }
@end

static void nativeAppEvent(CscdWindowNative* n, CscdAppEvent e);
static void nativePreferences(CscdWindowNative* n);
static UIDropProposal* nativeDragOver(CscdWindowNative* n, id<UIDropSession> session, bool enter);
static void nativeDragLeave(CscdWindowNative* n);
static void nativeDrop(CscdWindowNative* n, id<UIDropSession> session);

static UIColor* colorOf(uint32_t argb);

/* The controller's view: around the page (under the status bar, beside the notch in landscape,
 * above the home indicator and the keyboard) the page's edge colors: its top row's above it, its
 * bottom row's below, and beside it each row continuing the page's edge there. */
@interface CascadeBackdropView : UIView
@property(nonatomic, assign) uint32_t top, bottom;
@property(nonatomic, copy) NSData* left;  /* (first row, color) uint32 pairs, rows in the page's device pixels */
@property(nonatomic, copy) NSData* right;
@property(nonatomic, weak) UIView* page;
@end

@implementation CascadeBackdropView
static void fillRect(CGContextRef g, uint32_t argb, CGRect r) {
    if (r.size.width <= 0 || r.size.height <= 0) return;
    CGContextSetFillColorWithColor(g, colorOf(argb).CGColor);
    CGContextFillRect(g, r);
}
- (void)side:(NSData*)runs from:(CGFloat)x0 to:(CGFloat)x1 page:(CGRect)p context:(CGContextRef)g {
    if (x1 <= x0) return;
    const uint32_t* r = runs.bytes;
    const size_t n = runs.length / sizeof(uint32_t);
    if (n < 2) { fillRect(g, _top, CGRectMake(x0, p.origin.y, x1 - x0, p.size.height)); return; }
    const CGFloat scale = _page.contentScaleFactor > 0 ? _page.contentScaleFactor : 1;
    for (size_t i = 0; i + 1 < n; i += 2) {
        const CGFloat y0 = p.origin.y + r[i] / scale, y1 = i + 2 < n ? MIN(CGRectGetMaxY(p), p.origin.y + r[i + 2] / scale) : CGRectGetMaxY(p);
        if (y0 >= CGRectGetMaxY(p)) break;
        fillRect(g, r[i + 1], CGRectMake(x0, y0, x1 - x0, y1 - y0));
    }
}
- (void)drawRect:(CGRect)dirty {
    CGContextRef g = UIGraphicsGetCurrentContext();
    const CGRect b = self.bounds, p = _page ? _page.frame : b;
    fillRect(g, _top, CGRectMake(0, 0, b.size.width, CGRectGetMinY(p)));
    fillRect(g, _bottom, CGRectMake(0, CGRectGetMaxY(p), b.size.width, CGRectGetMaxY(b) - CGRectGetMaxY(p)));
    [self side:_left from:0 to:CGRectGetMinX(p) page:p context:g];
    [self side:_right from:CGRectGetMaxX(p) to:CGRectGetMaxX(b) page:p context:g];
}
@end

@interface CascadeViewController : UIViewController <UIDropInteractionDelegate>
@property(nonatomic, assign) CscdWindowNative* native;
@property(nonatomic, strong) CascadeView* pageView;
@property(nonatomic, assign) BOOL statusBarHidden;
@property(nonatomic, assign) BOOL lightTop;      /* the page's top is light: dark status bar text */
@end

@implementation CascadeViewController
- (void)loadView {
    CascadeBackdropView* root = [CascadeBackdropView new];
    root.contentMode = UIViewContentModeRedraw;
    /* (until the page's first frame, the theme's: no white flash before a dark page) */
    BOOL dark = NO;
    if (@available(iOS 13.0, *)) dark = UITraitCollection.currentTraitCollection.userInterfaceStyle == UIUserInterfaceStyleDark;
    root.top = root.bottom = dark ? 0xFF000000u : 0xFFFFFFFFu;
    self.lightTop = !dark;
    self.pageView = [CascadeView new];
    self.pageView.multipleTouchEnabled = YES;
    self.pageView.backgroundColor = dark ? UIColor.blackColor : UIColor.whiteColor;
    root.page = self.pageView;
    [root addSubview:self.pageView];
    self.view = root;
    /* files and text dragged in from other apps (an iPad's split screen) */
    [self.pageView addInteraction:[[UIDropInteraction alloc] initWithDelegate:self]];
    /* the app's life and the system's settings */
    NSNotificationCenter* c = NSNotificationCenter.defaultCenter;
    [c addObserver:self selector:@selector(background:) name:UIApplicationDidEnterBackgroundNotification object:nil];
    [c addObserver:self selector:@selector(foreground:) name:UIApplicationWillEnterForegroundNotification object:nil];
    [c addObserver:self selector:@selector(lowMemory:) name:UIApplicationDidReceiveMemoryWarningNotification object:nil];
    [c addObserver:self selector:@selector(preferences:) name:UIAccessibilityReduceMotionStatusDidChangeNotification object:nil];
    [c addObserver:self selector:@selector(preferences:) name:UIContentSizeCategoryDidChangeNotification object:nil];
    /* an iPad's pointer: where it hovers, and the scrolling of its trackpad or wheel */
    UIHoverGestureRecognizer* hover = [[UIHoverGestureRecognizer alloc] initWithTarget:self.pageView action:@selector(hovered:)];
    [self.pageView addGestureRecognizer:hover];
    if (@available(iOS 13.4, *)) {
        UIPanGestureRecognizer* scroll = [[UIPanGestureRecognizer alloc] initWithTarget:self.pageView action:@selector(scrolled:)];
        scroll.allowedScrollTypesMask = UIScrollTypeMaskAll;
        scroll.allowedTouchTypes = @[]; /* (scrolling only: touches stay the page's) */
        scroll.maximumNumberOfTouches = 0;
        [self.pageView addGestureRecognizer:scroll];
    }
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(keyboardChanged:) name:UIKeyboardWillChangeFrameNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(keyboardHidden:) name:UIKeyboardWillHideNotification object:nil];
}
- (void)dealloc { [NSNotificationCenter.defaultCenter removeObserver:self]; }
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    /* the page inside the safe area (clear of the notch and the home indicator) and above the keyboard */
    CGRect r = UIEdgeInsetsInsetRect(self.view.bounds, self.view.safeAreaInsets);
    if (_native && _native->keyboardTop > 0 && _native->keyboardTop < CGRectGetMaxY(r))
        r.size.height = MAX(0, _native->keyboardTop - r.origin.y);
    if (!CGRectEqualToRect(self.pageView.frame, r)) {
        self.pageView.frame = r;
        [self.view setNeedsDisplay]; /* (what is around the page moved with it) */
    }
}
- (void)background:(NSNotification*)note { if (_native) nativeAppEvent(_native, CSCD_APP_BACKGROUND); }
- (void)foreground:(NSNotification*)note { if (_native) nativeAppEvent(_native, CSCD_APP_FOREGROUND); }
- (void)lowMemory:(NSNotification*)note { if (_native) nativeAppEvent(_native, CSCD_APP_LOW_MEMORY); }
- (void)preferences:(NSNotification*)note { if (_native) nativePreferences(_native); }
- (void)traitCollectionDidChange:(UITraitCollection*)previous {
    [super traitCollectionDidChange:previous];
    if (_native) nativePreferences(_native);
}
- (UIStatusBarStyle)preferredStatusBarStyle {
    if (@available(iOS 13.0, *)) return self.lightTop ? UIStatusBarStyleDarkContent : UIStatusBarStyleLightContent;
    return UIStatusBarStyleDefault;
}
/* UIDropInteractionDelegate */
- (BOOL)dropInteraction:(UIDropInteraction*)i canHandleSession:(id<UIDropSession>)session { return _native != NULL; }
- (void)dropInteraction:(UIDropInteraction*)i sessionDidEnter:(id<UIDropSession>)session { if (_native) nativeDragOver(_native, session, true); }
- (UIDropProposal*)dropInteraction:(UIDropInteraction*)i sessionDidUpdate:(id<UIDropSession>)session {
    return _native ? nativeDragOver(_native, session, false) : [[UIDropProposal alloc] initWithDropOperation:UIDropOperationCancel];
}
- (void)dropInteraction:(UIDropInteraction*)i sessionDidExit:(id<UIDropSession>)session { if (_native) nativeDragLeave(_native); }
- (void)dropInteraction:(UIDropInteraction*)i performDrop:(id<UIDropSession>)session { if (_native) nativeDrop(_native, session); }
- (void)keyboardChanged:(NSNotification*)note {
    if (!_native) return;
    const CGRect onScreen = [note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    const CGRect here = [self.view convertRect:onScreen fromView:nil];
    /* only a keyboard along the bottom takes room from the page: an iPad's floating or split one
     * floats over it */
    const bool docked = CGRectIntersectsRect(here, self.view.bounds) && CGRectGetMaxY(here) >= CGRectGetMaxY(self.view.bounds) - 1 &&
                        here.size.width >= self.view.bounds.size.width - 1;
    _native->keyboardTop = docked ? (float)here.origin.y : 0;
    [self.view setNeedsLayout];
}
- (void)keyboardHidden:(NSNotification*)note {
    if (!_native) return;
    _native->keyboardTop = 0;
    [self.view setNeedsLayout];
}
/* keys while no text field has the focus (the controller is first responder then: no on-screen keyboard) */
- (BOOL)canBecomeFirstResponder { return YES; }
- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)e {
    bool passOn = true;
    if (_native) nativePresses(_native, presses, true, &passOn);
    if (passOn) [super pressesBegan:presses withEvent:e];
}
- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)e {
    bool passOn = true;
    if (_native) nativePresses(_native, presses, false, &passOn);
    if (passOn) [super pressesEnded:presses withEvent:e];
}
- (void)pressesCancelled:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)e {
    bool passOn = true;
    if (_native) nativePresses(_native, presses, false, &passOn);
    if (passOn) [super pressesCancelled:presses withEvent:e];
}
- (BOOL)prefersStatusBarHidden { return self.statusBarHidden; }
- (BOOL)prefersHomeIndicatorAutoHidden { return self.statusBarHidden; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskAll; }
@end

static void nativeResized(CscdWindowNative* n) {
    CscdWindow* w = n->w;
    if (!w || !w->hwnd) return;
    CascadeView* view = UIVIEW(n);
    const CGSize size = view.bounds.size;
    const float dpr = (float)view.contentScaleFactor;
    const int pw = (int)lround(size.width * dpr), ph = (int)lround(size.height * dpr);
    if (pw == w->clientW && ph == w->clientH && dpr == w->dpr) return;
    w->clientW = pw;
    w->clientH = ph;
    w->dpr = dpr;
    if (pw > 0 && ph > 0) {
        if (w->presenter) cscdPresenterResize(w->presenter, pw, ph);
        cscdViewSetViewport(w->view, (float)size.width, (float)size.height, dpr);
        windowRender(w); /* (rotations and the keyboard animate: the page follows at once) */
    }
}

static CscdModifiers modsOf(UIKeyModifierFlags f) {
    CscdModifiers m;
    m.shift = (f & UIKeyModifierShift) != 0;
    m.ctrl = (f & UIKeyModifierCommand) != 0;
    m.alt = (f & UIKeyModifierAlternate) != 0;
    m.meta = (f & UIKeyModifierControl) != 0;
    return m;
}

/* ---- touch, and an iPad's pointer ------------------------------------------------------------------- */

static int touchId(CscdWindowNative* n, UITouch* t, bool add) {
    for (int i = 0; i < kMaxTouches; ++i)
        if (n->touches[i] == (__bridge void*)t) return i;
    if (!add) return -1;
    for (int i = 0; i < kMaxTouches; ++i)
        if (!n->touches[i]) {
            n->touches[i] = (__bridge void*)t;
            return i;
        }
    return -1;
}

static void nativeTouches(CscdWindowNative* n, NSSet<UITouch*>* touches, UIEvent* e, int phase) {
    CscdView* v = n->w->view;
    CascadeView* view = UIVIEW(n);
    for (UITouch* t in touches) {
        const CGPoint p = [t locationInView:view];
        const float x = (float)p.x, y = (float)p.y;
        bool pointer = false;
        if (@available(iOS 13.4, *)) pointer = t.type == UITouchTypeIndirectPointer;
        if (pointer) {
            /* a trackpad's or mouse's click: the mouse, as on a desktop (the secondary button is the right one) */
            int button = 0;
            if (@available(iOS 13.4, *)) button = (e.buttonMask & UIEventButtonMaskSecondary) ? 2 : 0;
            UIKeyModifierFlags flags = 0;
            if (@available(iOS 13.4, *)) flags = e.modifierFlags;
            const CscdModifiers m = modsOf(flags);
            if (phase == 0) {
                n->pointerButtons = button == 2 ? 2 : 1;
                cscdViewMouseDown(v, x, y, button, m, (int)cscdMaxi(1, (int)t.tapCount));
            } else if (phase == 1) {
                cscdViewMouseMove(v, x, y, n->pointerButtons, m);
            } else {
                cscdViewMouseUp(v, x, y, n->pointerButtons == 2 ? 2 : 0, m);
                n->pointerButtons = 0;
            }
            continue;
        }
        const int id = touchId(n, t, phase == 0);
        if (id < 0) continue;
        const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime; /* (UITouch's timestamps' clock) */
        viewSetInputAge(v, now - t.timestamp);
        if (phase == 0) cscdViewTouchDown(v, id, x, y);
        else if (phase == 1) {
            /* the samples the screen took since the last move (it samples faster than frames come), each at
             * its time: a stroke's points and a fling's speed */
            NSArray<UITouch*>* samples = [e coalescedTouchesForTouch:t];
            if (!samples.count) samples = @[ t ];
            for (UITouch* c in samples) {
                const CGPoint q = [c locationInView:view];
                viewSetInputAge(v, now - c.timestamp);
                cscdViewTouchMove(v, id, (float)q.x, (float)q.y);
            }
        } else if (phase == 2) {
            cscdViewTouchUp(v, id, x, y);
            n->touches[id] = NULL;
        } else {
            /* (the system took the touches, a call or Control Center: the view lets go of all of them) */
            cscdViewTouchCancel(v);
            memset(n->touches, 0, sizeof n->touches);
            break;
        }
    }
    viewSetInputAge(v, 0);
}

static void nativeHover(CscdWindowNative* n, UIHoverGestureRecognizer* g) {
    const CGPoint p = [g locationInView:UIVIEW(n)];
    UIKeyModifierFlags flags = 0;
    if (@available(iOS 13.4, *)) flags = g.modifierFlags;
    if (g.state == UIGestureRecognizerStateEnded || g.state == UIGestureRecognizerStateCancelled) {
        if (!n->pointerButtons) cscdViewMouseLeave(n->w->view);
    } else cscdViewMouseMove(n->w->view, (float)p.x, (float)p.y, n->pointerButtons, modsOf(flags));
}

static void nativeScroll(CscdWindowNative* n, UIPanGestureRecognizer* g) {
    CascadeView* view = UIVIEW(n);
    const CGPoint t = [g translationInView:view];
    if (g.state == UIGestureRecognizerStateBegan) n->scrollLast = CGPointZero;
    const CGFloat dx = t.x - n->scrollLast.x, dy = t.y - n->scrollLast.y;
    n->scrollLast = t;
    if (dx == 0 && dy == 0) return;
    const CGPoint p = [g locationInView:view];
    UIKeyModifierFlags flags = 0;
    if (@available(iOS 13.4, *)) flags = g.modifierFlags;
    /* the content follows the fingers, as on the Mac */
    cscdViewWheel(n->w->view, (float)p.x, (float)p.y, (float)-dx, (float)-dy, modsOf(flags));
}

/* ---- keys and text ---------------------------------------------------------------------------------- */

/* The key from its HID usage (the same on every layout) or, for characters, from what it types
 * without modifiers. typing: its text comes through insertText (the key goes on to UIKit). */
API_AVAILABLE(ios(13.4))
static CscdKey mapKey(UIKey* key, char name[16], bool* typing) {
#define K(s, k) { strcpy(name, s); return k; }
    *typing = false;
    switch (key.keyCode) {
    case UIKeyboardHIDUsageKeyboardDeleteOrBackspace: K("Backspace", CSCD_KEY_BACKSPACE)
    case UIKeyboardHIDUsageKeyboardTab: K("Tab", CSCD_KEY_TAB)
    case UIKeyboardHIDUsageKeyboardReturnOrEnter: case UIKeyboardHIDUsageKeypadEnter: K("Enter", CSCD_KEY_ENTER)
    case UIKeyboardHIDUsageKeyboardEscape: K("Escape", CSCD_KEY_ESCAPE)
    case UIKeyboardHIDUsageKeyboardDeleteForward: K("Delete", CSCD_KEY_DELETE)
    case UIKeyboardHIDUsageKeyboardLeftArrow: K("ArrowLeft", CSCD_KEY_LEFT)
    case UIKeyboardHIDUsageKeyboardRightArrow: K("ArrowRight", CSCD_KEY_RIGHT)
    case UIKeyboardHIDUsageKeyboardUpArrow: K("ArrowUp", CSCD_KEY_UP)
    case UIKeyboardHIDUsageKeyboardDownArrow: K("ArrowDown", CSCD_KEY_DOWN)
    case UIKeyboardHIDUsageKeyboardHome: K("Home", CSCD_KEY_HOME)
    case UIKeyboardHIDUsageKeyboardEnd: K("End", CSCD_KEY_END)
    case UIKeyboardHIDUsageKeyboardPageUp: K("PageUp", CSCD_KEY_PAGE_UP)
    case UIKeyboardHIDUsageKeyboardPageDown: K("PageDown", CSCD_KEY_PAGE_DOWN)
    case UIKeyboardHIDUsageKeyboardInsert: K("Insert", CSCD_KEY_INSERT)
    case UIKeyboardHIDUsageKeyboardLeftShift: case UIKeyboardHIDUsageKeyboardRightShift: K("Shift", CSCD_KEY_SHIFT)
    case UIKeyboardHIDUsageKeyboardLeftGUI: case UIKeyboardHIDUsageKeyboardRightGUI: K("Control", CSCD_KEY_CONTROL) /* Command */
    case UIKeyboardHIDUsageKeyboardLeftAlt: case UIKeyboardHIDUsageKeyboardRightAlt: K("Alt", CSCD_KEY_ALT)       /* Option */
    case UIKeyboardHIDUsageKeyboardLeftControl: case UIKeyboardHIDUsageKeyboardRightControl: K("Meta", CSCD_KEY_META) /* Control */
    default: break;
    }
    if (key.keyCode >= UIKeyboardHIDUsageKeyboardF1 && key.keyCode <= UIKeyboardHIDUsageKeyboardF12) {
        const int i = (int)(key.keyCode - UIKeyboardHIDUsageKeyboardF1);
        snprintf(name, 16, "F%d", i + 1);
        return (CscdKey)((int)CSCD_KEY_F1 + i);
    }
    const UIKeyModifierFlags f = key.modifierFlags;
    *typing = key.characters.length > 0 && !(f & (UIKeyModifierCommand | UIKeyModifierControl));
    if (key.keyCode == UIKeyboardHIDUsageKeyboardSpacebar) K(" ", CSCD_KEY_SPACE)
    NSString* chars = key.charactersIgnoringModifiers;
    if (chars.length == 1) {
        const unichar c = [chars.lowercaseString characterAtIndex:0];
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
    if (chars.length) {
        snprintf(name, 16, "%s", chars.UTF8String ?: "");
        return CSCD_KEY_CHARACTER;
    }
    K("Unidentified", CSCD_KEY_UNKNOWN)
#undef K
}

static void nativePresses(CscdWindowNative* n, NSSet<UIPress*>* presses, bool down, bool* passOn) {
    *passOn = true;
    if (@available(iOS 13.4, *)) {
        CscdView* v = n->w->view;
        for (UIPress* press in presses) {
            UIKey* key = press.key;
            if (!key) continue;
            char name[16];
            bool typing;
            const CscdKey k = mapKey(key, name, &typing);
            const CscdModifiers m = modsOf(key.modifierFlags);
            if (!down) {
                cscdViewKeyUp(v, k, m, cscdSv(name));
                continue;
            }
            cscdViewKeyDown(v, k, m, cscdSv(name));
            /* Return types a line break, as on the other systems; typing keys type through
             * insertText; the rest (arrows, Backspace, shortcuts) stay here */
            if (k == CSCD_KEY_ENTER && !m.ctrl && !m.meta) cscdViewTextInput(v, CSCD_SV("\n"));
            if (!typing || k == CSCD_KEY_ENTER || k == CSCD_KEY_TAB) *passOn = false;
        }
    }
}

/* The on-screen keyboard's Return: the key and its line break; Next and Previous move the focus, Done
 * also lets the keyboard go. */
static void nativeEnter(CscdWindowNative* n) {
    CscdView* v = n->w->view;
    const CscdModifiers none = {false, false, false, false}, shift = {true, false, false, false};
    const CscdEnterKey enter = n->traits.enter;
    if (enter == CSCD_ENTER_NEXT || enter == CSCD_ENTER_PREVIOUS) {
        const CscdModifiers m = enter == CSCD_ENTER_PREVIOUS ? shift : none;
        cscdViewKeyDown(v, CSCD_KEY_TAB, m, CSCD_SV("Tab"));
        cscdViewKeyUp(v, CSCD_KEY_TAB, m, CSCD_SV("Tab"));
        return;
    }
    cscdViewKeyDown(v, CSCD_KEY_ENTER, none, CSCD_SV("Enter"));
    cscdViewTextInput(v, CSCD_SV("\n"));
    cscdViewKeyUp(v, CSCD_KEY_ENTER, none, CSCD_SV("Enter"));
    if (enter == CSCD_ENTER_DONE) cscdViewBlur(v);
}

static void nativeText(CscdWindowNative* n, NSString* s) {
    CscdView* v = n->w->view;
    if ([s isEqualToString:@"\n"]) {
        nativeEnter(n);
        return;
    }
    CscdStr text = {0};
    fromNS(s, &text);
    if (text.len) cscdViewTextInput(v, cscdStrSv(&text));
    cscdStrFree(&text);
}

static void nativeBackspace(CscdWindowNative* n) {
    /* (the on-screen keyboard's: a hardware Backspace came as a key and stopped there) */
    const CscdModifiers none = {false, false, false, false};
    cscdViewKeyDown(n->w->view, CSCD_KEY_BACKSPACE, none, CSCD_SV("Backspace"));
    cscdViewKeyUp(n->w->view, CSCD_KEY_BACKSPACE, none, CSCD_SV("Backspace"));
}

static bool textStateEq(const CscdTextState* a, const CscdTextState* b) {
    return cscdStrEq(&a->text, &b->text) && a->selStart == b->selStart && a->selEnd == b->selEnd && a->composeStart == b->composeStart &&
           a->composeEnd == b->composeEnd;
}

/* The on-screen keyboard: up while a text field has the focus (the view is first responder, taking
 * text), of the kind the field asks for; otherwise the controller takes the keys. UIKit is told when
 * the field's text or selection changed other than through it (keys, touches, the program). */
static void updateKeyboard(CscdWindowNative* n) {
    if (!n->w->shown) return;
    CascadeView* view = UIVIEW(n);
    CascadeViewController* controller = UICONTROLLER(n);
    const CscdInputTraits t = cscdViewInputTraits(n->w->view);
    const bool text = t.kind != CSCD_INPUT_NONE;
    const bool traitsChanged = !cscdInputTraitsEq(t, n->traits);
    n->traits = t;
    CscdTextState now;
    const bool field = cscdViewGetTextState(n->w->view, &now);
    if (field && n->kitHasState && !textStateEq(&now, &n->kitState) && view.isFirstResponder) {
        const bool textChanged = !cscdStrEq(&now.text, &n->kitState.text);
        id<UITextInputDelegate> d = view.inputDelegate;
        if (textChanged) [d textWillChange:view];
        [d selectionWillChange:view];
        [d selectionDidChange:view];
        if (textChanged) [d textDidChange:view];
    }
    cscdTextStateFini(&n->kitState);
    n->kitState = now;
    n->kitHasState = field;
    if (text == n->keyboardWanted && (text ? view.isFirstResponder : controller.isFirstResponder)) {
        if (text && traitsChanged) [view reloadInputViews]; /* (another field: another keyboard) */
        return;
    }
    n->keyboardWanted = text;
    if (text) [view becomeFirstResponder];
    else [controller becomeFirstResponder];
}

/* ---- the app's life and the system's settings ------------------------------------------------------- */

static void nativeAppEvent(CscdWindowNative* n, CscdAppEvent e) {
    if (!n->w || !n->w->hwnd) return;
    windowAppEvent(n->w, e);
    wakeLoop();
}

/* the theme, Reduce Motion and the text size (Dynamic Type: the body text's size against its default) */
static void nativePreferences(CscdWindowNative* n) {
    if (!n->w || !n->w->hwnd) return;
    UITraitCollection* traits = UICONTROLLER(n).traitCollection;
    CscdSystemPreferences p = {false, false, 1};
    if (@available(iOS 13.0, *)) p.dark = traits.userInterfaceStyle == UIUserInterfaceStyleDark;
    p.reducedMotion = UIAccessibilityIsReduceMotionEnabled();
    p.textScale = (float)([[UIFontMetrics defaultMetrics] scaledValueForValue:17 compatibleWithTraitCollection:traits] / 17);
    windowSystemPreferences(n->w, p);
    wakeLoop();
}

/* ---- dragged in from other apps ------------------------------------------------------------------- */

static UIDropProposal* nativeDragOver(CscdWindowNative* n, id<UIDropSession> session, bool enter) {
    CscdView* v = n->w->view;
    const CGPoint p = [session locationInView:UIVIEW(n)];
    if (enter || !n->drop) {
        /* (what it carries is only known at the drop: its kinds stand in until then) */
        CscdDataTransfer* dt = cscdDataTransferNew();
        cscdStrSetC(&dt->effectAllowed, "copy");
        if ([session canLoadObjectsOfClass:NSString.class]) cscdDataTransferSetData(dt, CSCD_SV("text/plain"), CSCD_SV(""));
        if (n->drop) cscdRelease(n->drop);
        n->drop = dt;
        cscdViewExternalDragEnter(v, dt, (float)p.x, (float)p.y);
    }
    const bool accepted = cscdViewExternalDragOver(v, (float)p.x, (float)p.y);
    wakeLoop();
    return [[UIDropProposal alloc] initWithDropOperation:accepted ? UIDropOperationCopy : UIDropOperationForbidden];
}

static void nativeDragLeave(CscdWindowNative* n) {
    cscdViewExternalDragLeave(n->w->view);
    if (n->drop) cscdRelease(n->drop);
    n->drop = NULL;
    wakeLoop();
}

/* The drop: its text and files (copied out of the other app's reach) are loaded, then the page gets
 * them where the drop was. */
static void nativeDrop(CscdWindowNative* n, id<UIDropSession> session) {
    const CGPoint p = [session locationInView:UIVIEW(n)];
    NSMutableArray<NSString*>* texts = [NSMutableArray array];
    NSMutableArray<NSString*>* files = [NSMutableArray array];
    dispatch_group_t group = dispatch_group_create();
    NSString* dir = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    for (UIDragItem* item in session.items) {
        NSItemProvider* provider = item.itemProvider;
        if ([provider canLoadObjectOfClass:NSString.class] && ![provider hasItemConformingToTypeIdentifier:@"public.file-url"]) {
            dispatch_group_enter(group);
            [provider loadObjectOfClass:NSString.class
                      completionHandler:^(id<NSItemProviderReading> object, NSError* error) {
                          if ([(NSObject*)object isKindOfClass:NSString.class]) @synchronized(texts) { [texts addObject:(NSString*)object]; }
                          dispatch_group_leave(group);
                      }];
            continue;
        }
        NSString* type = provider.registeredTypeIdentifiers.firstObject;
        if (!type) continue;
        dispatch_group_enter(group);
        [provider loadFileRepresentationForTypeIdentifier:type
                                        completionHandler:^(NSURL* url, NSError* error) {
                                            /* (the file goes when this returns: a copy stays) */
                                            if (url) {
                                                NSString* to = [dir stringByAppendingPathComponent:url.lastPathComponent];
                                                if ([NSFileManager.defaultManager copyItemAtPath:url.path toPath:to error:nil])
                                                    @synchronized(files) { [files addObject:to]; }
                                            }
                                            dispatch_group_leave(group);
                                        }];
    }
    dispatch_group_notify(group, dispatch_get_main_queue(), ^{
        if (n != liveNative || !n->w || !n->w->hwnd) return; /* (the window closed meanwhile) */
        CscdView* v = n->w->view;
        CscdDataTransfer* dt = cscdDataTransferNew();
        cscdStrSetC(&dt->effectAllowed, "copy");
        for (NSString* f in files) {
            CscdStr s = {0};
            fromNS(f, &s);
            cscdVecPush(&dt->files, s);
        }
        if (texts.count) {
            CscdStr s = {0};
            fromNS([texts componentsJoinedByString:@"\n"], &s);
            cscdDataTransferSetData(dt, CSCD_SV("text/plain"), cscdStrSv(&s));
            cscdStrFree(&s);
        }
        /* the page sees the drag again, now with what it carries, and the drop */
        cscdViewExternalDragLeave(v);
        cscdViewExternalDragEnter(v, dt, (float)p.x, (float)p.y);
        cscdViewExternalDragOver(v, (float)p.x, (float)p.y);
        cscdViewExternalDrop(v, (float)p.x, (float)p.y);
        cscdRelease(dt);
        if (n->drop) cscdRelease(n->drop);
        n->drop = NULL;
        wakeLoop();
    });
}

/* ---- clipboard -------------------------------------------------------------------------------------- */

static void clipSetText(void* user, CscdSv text) {
    (void)user;
    UIPasteboard.generalPasteboard.string = svToNS(text);
}

static void clipGetText(void* user, CscdStr* out) {
    (void)user;
    CscdStr s = {0};
    fromNS(UIPasteboard.generalPasteboard.string, &s);
    size_t k = 0; /* (no carriage returns) */
    for (size_t i = 0; i < s.len; ++i) if (s.ptr[i] != '\r') s.ptr[k++] = s.ptr[i];
    cscdStrTruncate(&s, k);
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
    [UIPasteboard.generalPasteboard setData:data forPasteboardType:@"public.png"];
}

/* the pasteboard's pictures, as PNG files in a folder of the temporary one (other apps' files are
 * not reachable by path on iOS) */
static void clipGetFiles(void* user, CscdStrVec* out) {
    (void)user;
    UIPasteboard* pb = UIPasteboard.generalPasteboard;
    if (!pb.hasImages) return;
    NSString* dir = [[NSTemporaryDirectory() stringByAppendingPathComponent:@"pasted"] stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    if (![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil]) return;
    NSInteger k = 0;
    for (UIImage* image in pb.images) {
        NSData* png = UIImagePNGRepresentation(image);
        NSString* path = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"pasted-%ld.png", (long)++k]];
        if (!png || ![png writeToFile:path atomically:YES]) continue;
        CscdStr s = {0};
        fromNS(path, &s);
        cscdVecPush(out, s);
    }
}

/* ---- touch selection: the system's edit menu and loupe ---------------------------------------------- */

static void nativeEditAction(CscdWindowNative* n, CscdEditAction a) {
    if (!n->w || !n->w->hwnd) return;
    cscdViewEditAction(n->w->view, a);
    wakeLoop();
}

static void editMenu(void* user, bool show, CscdRect r, unsigned actions) {
    CscdWindowNative* n = ((CscdWindow*)user)->native;
    if (!n || !n->view) return;
    CascadeView* view = UIVIEW(n);
    const bool wasShown = n->editMenuShown;
    n->editMenuShown = show;
    n->editActions = actions;
    n->editMenuRect = CGRectMake(r.x, r.y, r.w, r.h);
    if (@available(iOS 16.0, *)) {
        UIEditMenuInteraction* menu = n->editMenu ? (__bridge UIEditMenuInteraction*)n->editMenu : nil;
        if (!menu) {
            if (!show) return;
            CascadeEditMenuDelegate* d = [CascadeEditMenuDelegate new];
            d.native = n;
            menu = [[UIEditMenuInteraction alloc] initWithDelegate:d];
            [view addInteraction:menu];
            n->editMenu = (void*)CFBridgingRetain(menu);
            n->editMenuDelegate = (void*)CFBridgingRetain(d);
        }
        if (!show) {
            [menu dismissMenu];
            return;
        }
        if (wasShown) {
            [menu updateVisibleMenuPositionAnimated:NO]; /* (the selection moved: the menu follows) */
            return;
        }
        const CGPoint at = CGPointMake(CGRectGetMidX(n->editMenuRect), CGRectGetMinY(n->editMenuRect));
        [menu presentEditMenuWithConfiguration:[UIEditMenuConfiguration configurationWithIdentifier:nil sourcePoint:at]];
        return;
    }
    UIMenuController* mc = UIMenuController.sharedMenuController;
    if (!show) {
        [mc hideMenuFromView:view];
        return;
    }
    if (!view.isFirstResponder) [view becomeFirstResponder]; /* (the menu asks the first responder) */
    [mc showMenuFromView:view rect:n->editMenuRect];
}

/* the loupe over a dragged handle (iOS 17 and later) */
static void magnify(void* user, bool show, float x, float y) {
    CscdWindowNative* n = ((CscdWindow*)user)->native;
    if (!n || !n->view) return;
    if (@available(iOS 17.0, *)) {
        UITextLoupeSession* s = n->loupe ? (__bridge UITextLoupeSession*)n->loupe : nil;
        if (!show) {
            if (s) [s invalidate];
            if (n->loupe) CFBridgingRelease(n->loupe);
            n->loupe = NULL;
            return;
        }
        const CGPoint p = CGPointMake(x, y);
        if (!s) {
            s = [UITextLoupeSession beginLoupeSessionAtPoint:p fromSelectionWidgetView:nil inView:UIVIEW(n)];
            if (s) n->loupe = (void*)CFBridgingRetain(s);
        } else [s moveToPoint:p withCaretRect:CGRectMake(x - 1, y - 10, 2, 20) trackingCaret:YES];
    }
}

/* ---- the window ----------------------------------------------------------------------------------- */

static UIWindowScene* windowScene(void) {
    for (UIScene* s in UIApplication.sharedApplication.connectedScenes)
        if ([s isKindOfClass:UIWindowScene.class]) return (UIWindowScene*)s;
    return nil;
}

void windowCreateNative(CscdWindow* w, const CscdWindowOptions* opts) {
    @autoreleasepool {
        CscdWindowNative* n = CSCD_NEW(CscdWindowNative);
        w->native = n;
        n->w = w;
        if (!uikitStarted) {
            fprintf(stderr, "cascade: UIKit is not running (the program's main must come from a file that includes cascade/window.h)\n");
            return;
        }
        if (!wakeSource) {
            CFRunLoopSourceContext ctx = {0};
            ctx.perform = wakePerform;
            wakeSource = CFRunLoopSourceCreate(NULL, 0, &ctx);
            CFRunLoopAddSource(CFRunLoopGetMain(), wakeSource, kCFRunLoopCommonModes);
        }
        /* with scenes (UIApplicationSceneManifest), the window belongs to one: it connects soon after launch */
        UIWindowScene* scene = windowScene();
        if (!scene && [NSBundle.mainBundle objectForInfoDictionaryKey:@"UIApplicationSceneManifest"]) {
            NSDate* until = [NSDate dateWithTimeIntervalSinceNow:2];
            while (!scene && until.timeIntervalSinceNow > 0) {
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
                scene = windowScene();
            }
        }
        UIWindow* window = scene ? [[UIWindow alloc] initWithWindowScene:scene] : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
        CascadeViewController* controller = [CascadeViewController new];
        window.rootViewController = controller;
        [controller loadViewIfNeeded];
        CascadeView* view = controller.pageView;
        n->window = (void*)CFBridgingRetain(window);
        n->controller = (void*)CFBridgingRetain(controller);
        n->view = (void*)CFBridgingRetain(view);
        w->hwnd = n->window;

        controller.view.frame = window.bounds;
        [controller.view layoutIfNeeded];
        w->dpr = (float)(view.contentScaleFactor > 0 ? view.contentScaleFactor : UIScreen.mainScreen.scale);
        const CGSize size = view.bounds.size;
        w->clientW = cscdMaxi(1, (int)lround(size.width * w->dpr));
        w->clientH = cscdMaxi(1, (int)lround(size.height * w->dpr));
        w->presenter = windowPresenterNew(w, (__bridge void*)view, opts->backend, w->clientW, w->clientH);
        cscdViewSetViewport(w->view, (float)size.width, (float)size.height, w->dpr);
        cscdViewSetClipboardHooks(w->view, clipSetText, clipGetText, clipSetImage, clipGetFiles, w);
        cscdViewOnEditMenu(w->view, editMenu, w);
        cscdViewOnMagnifier(w->view, magnify, w);
        view.native = n;
        controller.native = n;
        liveNative = n;
        nativePreferences(n);
        /* animations at the screen's rate (120 Hz on ProMotion screens): the link wakes the loop */
        CADisplayLink* link = [CADisplayLink displayLinkWithTarget:[CascadeTicker new] selector:@selector(tick:)];
        if (@available(iOS 15.0, *)) link.preferredFrameRateRange = CAFrameRateRangeMake(60, 120, 120);
        else link.preferredFramesPerSecond = 0; /* (the screen's maximum) */
        link.paused = YES;
        [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        n->displayLink = (void*)CFBridgingRetain(link);
        uikitA11yAttach(w);
    }
}

void windowDestroyNative(CscdWindow* w) {
    CscdWindowNative* n = w->native;
    if (!n) return;
    @autoreleasepool {
        if (liveNative == n) liveNative = NULL;
        if (n->displayLink) {
            [(__bridge CADisplayLink*)n->displayLink invalidate];
            CFBridgingRelease(n->displayLink);
        }
        if (n->picker) CFBridgingRelease(n->picker);
        if (n->drop) cscdRelease(n->drop);
        cscdViewOnEditMenu(w->view, NULL, NULL); /* (the menu and the loupe go with the window) */
        cscdViewOnMagnifier(w->view, NULL, NULL);
        magnify(w, false, 0, 0);
        if (n->editMenuDelegate) {
            if (@available(iOS 16.0, *)) ((__bridge CascadeEditMenuDelegate*)n->editMenuDelegate).native = NULL;
            CFBridgingRelease(n->editMenuDelegate);
        }
        if (n->editMenu) CFBridgingRelease(n->editMenu);
        cscdTextStateFini(&n->kitState);
        if (n->window) {
            uikitA11yDetach(w);
            UIVIEW(n).native = NULL;
            UICONTROLLER(n).native = NULL;
            [UIVIEW(n) resignFirstResponder];
            UIWIN(n).hidden = YES;
            CFBridgingRelease(n->view);
            CFBridgingRelease(n->controller);
            CFBridgingRelease(n->window);
        }
        w->hwnd = NULL;
        cscdFree(n);
        w->native = NULL;
    }
}

void* windowSurface(const CscdWindow* w) { return w->native->view; }
void windowShowTitle(CscdWindow* w, const char* title) { (void)w; (void)title; /* (no title bar) */ }
void windowPlaceInputMethod(CscdWindow* w) { (void)w; }
void windowWake(CscdWindow* w) { (void)w; wakeLoop(); }

void cscdWindowShow(CscdWindow* w) {
    if (!w->hwnd || w->shown) return;
    w->shown = true;
    [UIWIN(w->native) makeKeyAndVisible];
    w->dirty = true;
}

void cscdWindowResize(CscdWindow* w, int width, int height) { (void)w; (void)width; (void)height; /* (the screen's size) */ }
void cscdWindowMaximize(CscdWindow* w) { cscdWindowShow(w); }

void cscdWindowSetFullscreen(CscdWindow* w, bool on) {
    if (!w->hwnd || on == w->fullscreen) return;
    w->fullscreen = on;
    CascadeViewController* c = UICONTROLLER(w->native);
    c.statusBarHidden = on;
    [c setNeedsStatusBarAppearanceUpdate];
    [c setNeedsUpdateOfHomeIndicatorAutoHidden];
}

void cscdWindowClose(CscdWindow* w) {
    if (!w->hwnd) return;
    w->native->closeRequested = true; /* asked from the loop */
    wakeLoop();
}

bool cscdWindowPump(CscdWindow* w, bool wait) {
    if (!w->hwnd || !w->native) return false;
    CscdWindowNative* n = w->native;
    @autoreleasepool {
        CFTimeInterval seconds = 0;
        if (wait && !windowHasPosted(w) && !w->dirty && !n->closeRequested && !(cscdViewNeedsPaint(w->view) && !w->background)) {
            const int ms = w->onFrame && !w->background ? 0 : cscdViewNextTimerMs(w->view);
            seconds = ms < 0 ? 1e9 : ms / 1000.0;
        }
        /* while the page animates, the screen's refresh paces the frames */
        if (n->displayLink) ((__bridge CADisplayLink*)n->displayLink).paused = w->background || !(cscdViewAnimating(w->view) || w->onFrame);
        /* until something is handled (or the next timer), then whatever else is waiting */
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, true);
        while (w->hwnd && CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) == kCFRunLoopRunHandledSource) {}
        if (w->hwnd && n->closeRequested) {
            n->closeRequested = false;
            if (!w->onClose || w->onClose(w->onCloseUser)) {
                cscdPresenterFree(w->presenter);
                w->presenter = NULL;
                UIWIN(n).hidden = YES;
                w->hwnd = NULL;
            }
        }
        if (!w->hwnd) return false;
        windowRunPosted(w);
        if (!w->hwnd) return false;
        const bool render = cscdViewUpdate(w->view) || w->onFrame || w->dirty;
        if (render) windowRender(w);
        updateKeyboard(n);
    }
    return w->hwnd != NULL;
}

/* ---- the system around the app ------------------------------------------------------------------- */

static UIColor* colorOf(uint32_t argb) {
    return [UIColor colorWithRed:((argb >> 16) & 255) / 255.0 green:((argb >> 8) & 255) / 255.0 blue:(argb & 255) / 255.0 alpha:1];
}

/* the page's edge colors under the status bar, beside the notch and above the home indicator */
void windowSetBackdrop(CscdWindow* w, const CscdWindowBackdrop* b) {
    CascadeViewController* c = UICONTROLLER(w->native);
    CascadeBackdropView* v = (CascadeBackdropView*)c.view;
    const uint32_t top = b->top;
    v.top = top;
    v.bottom = b->bottom;
    v.left = [NSData dataWithBytes:b->left.items length:b->left.len * sizeof(uint32_t)];
    v.right = [NSData dataWithBytes:b->right.items length:b->right.len * sizeof(uint32_t)];
    [v setNeedsDisplay];
    const BOOL light = ((top >> 16) & 255) * 299 + ((top >> 8) & 255) * 587 + (top & 255) * 114 > 128000;
    if (light != c.lightTop) {
        c.lightTop = light;
        [c setNeedsStatusBarAppearanceUpdate];
    }
}

bool platformOpenUrl(CscdWindow* w, const char* url) {
    (void)w;
    NSURL* u = [NSURL URLWithString:toNSC(url)];
    if (!u) return false;
    [UIApplication.sharedApplication openURL:u options:@{} completionHandler:nil];
    return true;
}

bool platformShareText(CscdWindow* w, const char* text) {
    CascadeViewController* c = UICONTROLLER(w->native);
    UIActivityViewController* share = [[UIActivityViewController alloc] initWithActivityItems:@[ toNSC(text) ] applicationActivities:nil];
    /* (an iPad shows it as a popover: from the page's middle) */
    share.popoverPresentationController.sourceView = c.pageView;
    share.popoverPresentationController.sourceRect = CGRectMake(CGRectGetMidX(c.pageView.bounds), CGRectGetMidY(c.pageView.bounds), 1, 1);
    [c presentViewController:share animated:YES completion:nil];
    return true;
}

void platformHaptic(CscdWindow* w, CscdHaptic kind) {
    (void)w;
    switch (kind) {
    case CSCD_HAPTIC_SELECTION: [[UISelectionFeedbackGenerator new] selectionChanged]; break;
    case CSCD_HAPTIC_SUCCESS: [[UINotificationFeedbackGenerator new] notificationOccurred:UINotificationFeedbackTypeSuccess]; break;
    case CSCD_HAPTIC_WARNING: [[UINotificationFeedbackGenerator new] notificationOccurred:UINotificationFeedbackTypeWarning]; break;
    case CSCD_HAPTIC_ERROR: [[UINotificationFeedbackGenerator new] notificationOccurred:UINotificationFeedbackTypeError]; break;
    default: [[[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight] impactOccurred]; break;
    }
}

/* The document picker's answer: the files it imported (copies in the app's own folders); or, exporting
 * (cscdWindowSaveFile), only that it is done with the file the program wrote. */
@interface CascadePicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, assign) CscdWindow* window;
@property(nonatomic, strong) NSURL* exported;
@end
@implementation CascadePicker
- (void)finish:(NSArray<NSURL*>*)urls {
    CscdWindow* w = self.window;
    if (self.exported) {
        [NSFileManager.defaultManager removeItemAtURL:self.exported.URLByDeletingLastPathComponent error:nil];
        self.exported = nil;
        if (w && w->native && w->native == liveNative) {
            void* held = w->native->picker;
            w->native->picker = NULL;
            if (held) dispatch_async(dispatch_get_main_queue(), ^{ CFBridgingRelease(held); });
        }
        return;
    }
    if (!w || !w->native || w->native != liveNative) return;
    CscdStrVec paths = {0};
    for (NSURL* u in urls)
        if (u.fileURL) {
            CscdStr p = {0};
            fromNS(u.path, &p);
            cscdVecPush(&paths, p);
        }
    windowFilesPicked(w, &paths);
    cscdStrVecFree(&paths);
    void* held = w->native->picker;
    w->native->picker = NULL;
    wakeLoop();
    /* (let go of once this method has returned) */
    if (held) dispatch_async(dispatch_get_main_queue(), ^{ CFBridgingRelease(held); });
}
- (void)documentPicker:(UIDocumentPickerViewController*)c didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls { [self finish:urls]; }
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)c { [self finish:@[]]; }
@end

/* the kinds of document (UTIs) an accept attribute names; any document without one */
static NSArray<NSString*>* documentTypes(const char* accept) {
    static const struct { const char* accept; const char* uti; } kTypes[] = {
        {"image/*", "public.image"}, {"video/*", "public.movie"}, {"audio/*", "public.audio"}, {"text/*", "public.text"},
        {"font/*", "public.font"}, {"application/pdf", "com.adobe.pdf"}, {".pdf", "com.adobe.pdf"}, {"application/json", "public.json"},
        {".json", "public.json"}, {"image/png", "public.png"}, {".png", "public.png"}, {"image/jpeg", "public.jpeg"}, {".jpg", "public.jpeg"},
        {".jpeg", "public.jpeg"}, {"image/gif", "com.compuserve.gif"}, {".gif", "com.compuserve.gif"}, {"text/plain", "public.plain-text"},
        {".txt", "public.plain-text"}, {"text/html", "public.html"}, {".html", "public.html"}, {"text/csv", "public.comma-separated-values-text"},
        {".csv", "public.comma-separated-values-text"}, {"application/zip", "public.zip-archive"}, {".zip", "public.zip-archive"}};
    NSMutableArray<NSString*>* out = [NSMutableArray array];
    bool unknown = false;
    CscdSv rest = cscdSv(accept);
    while (rest.len) {
        size_t comma = cscdSvFindChar(rest, ',', 0);
        const CscdSv item = cscdSvTrim(cscdSvSub(rest, 0, comma == CSCD_NPOS ? rest.len : comma));
        rest = comma == CSCD_NPOS ? CSCD_SV("") : cscdSvSub(rest, comma + 1, rest.len - comma - 1);
        if (!item.len) continue;
        bool known = false;
        for (size_t i = 0; i < CSCD_COUNT(kTypes); ++i)
            if (cscdSvIEqC(item, kTypes[i].accept)) {
                [out addObject:@(kTypes[i].uti)];
                known = true;
            }
        unknown = unknown || !known;
    }
    if (!out.count || unknown) [out addObject:@"public.item"]; /* (a kind it does not know: any document, rather than none) */
    return out;
}

/* (no tray, no menu bar of the system's, no hiding the app's window) */
bool platformSetMenuBar(CscdWindow* w, CscdMenu* menu) { (void)w; (void)menu; return false; }
bool platformTrayCreate(CscdTray* t, const CscdImage* icon, const char* tooltip) { (void)t; (void)icon; (void)tooltip; return false; }
void platformTraySet(CscdTray* t, const CscdImage* icon, const char* tooltip) { (void)t; (void)icon; (void)tooltip; }
void platformTrayDestroy(CscdTray* t) { (void)t; }
bool platformHide(CscdWindow* w) { (void)w; return false; }

bool platformPickFiles(CscdWindow* w, const char* accept, bool multiple) {
    CscdWindowNative* n = w->native;
    if (n->picker) return false;
    CascadePicker* delegate = [CascadePicker new];
    delegate.window = w;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:documentTypes(accept)
                                                                                                   inMode:UIDocumentPickerModeImport];
#pragma clang diagnostic pop
    picker.allowsMultipleSelection = multiple;
    picker.delegate = delegate;
    n->picker = (void*)CFBridgingRetain(delegate);
    [UICONTROLLER(n) presentViewController:picker animated:YES completion:nil];
    return true;
}

/* Saving: the program writes the file in a folder of its own in the temporary one, then the
 * document picker exports it (Files, iCloud Drive, another app's storage); the folder goes after. */
bool platformSaveFile(CscdWindow* w, const char* name, const char* accept) {
    (void)accept; /* (the file's own extension says its kind) */
    CscdWindowNative* n = w->native;
    if (n->picker) return false;
    @autoreleasepool {
        NSString* file = name[0] ? toNSC(name).lastPathComponent : @"Untitled";
        NSURL* dir = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString] isDirectory:YES];
        [NSFileManager.defaultManager createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSURL* url = [dir URLByAppendingPathComponent:file];
        CscdStr path = {0};
        fromNS(url.path, &path);
        const bool wrote = windowSaveChosen(w, cscdStrC(&path));
        cscdStrFree(&path);
        if (!wrote || ![NSFileManager.defaultManager fileExistsAtPath:url.path]) {
            [NSFileManager.defaultManager removeItemAtURL:dir error:nil];
            return true;
        }
        CascadePicker* delegate = [CascadePicker new];
        delegate.window = w;
        delegate.exported = url;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initWithURL:url
                                                                                              inMode:UIDocumentPickerModeExportToService];
#pragma clang diagnostic pop
        picker.delegate = delegate;
        n->picker = (void*)CFBridgingRetain(delegate);
        [UICONTROLLER(n) presentViewController:picker animated:YES completion:nil];
        return true;
    }
}
