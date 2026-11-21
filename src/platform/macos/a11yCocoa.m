/* NSAccessibility: the page as VoiceOver sees it, from the view's accessible tree (a11y.c). One
 * element object per accessible element, kept by its accessible number (VoiceOver expects the same
 * object each time), all holding a link to the window that is cut when the window goes. */
#include "platform/common/platformInternal.h"

#import <AppKit/AppKit.h>
#include <string.h>

@interface CscdA11yLink : NSObject
@property(nonatomic, assign) CscdWindow* window;
@property(nonatomic, strong) NSMutableDictionary<NSNumber*, id>* nodes;
@end
@implementation CscdA11yLink
@end

@interface CscdA11yNode : NSAccessibilityElement
@property(nonatomic, strong) CscdA11yLink* link;
@property(nonatomic, assign) uint32_t number;
@end

static NSString* text(CscdStr* s) {
    NSString* t = [[NSString alloc] initWithBytes:s->ptr length:s->len encoding:NSUTF8StringEncoding];
    cscdStrFree(s);
    return t ?: @"";
}

static CscdA11yNode* nodeFor(CscdA11yLink* link, CscdElement* e);

/* ---- a text field's text, as VoiceOver reads it: UTF-16 places (NSString's), bytes of the field's text (cascade's) ---- */

static NSUInteger utf16At(NSString* s, size_t bytes) {
    const char* u = s.UTF8String;
    const size_t len = u ? strlen(u) : 0;
    NSString* head = [[NSString alloc] initWithBytes:u length:bytes < len ? bytes : len encoding:NSUTF8StringEncoding];
    return head.length;
}

static size_t bytesAt(NSString* s, NSUInteger utf16) {
    if (utf16 > s.length) utf16 = s.length;
    return [[s substringToIndex:utf16] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
}

/* the line (by line breaks) that holds place i, and its range */
static NSRange lineRange(NSString* s, NSUInteger i, NSUInteger* line) {
    NSUInteger start = 0, n = 0;
    for (;;) {
        const NSRange nl = [s rangeOfString:@"\n" options:0 range:NSMakeRange(start, s.length - start)];
        const NSUInteger end = nl.location == NSNotFound ? s.length : nl.location + 1;
        if (i < end || nl.location == NSNotFound) {
            if (line) *line = n;
            return NSMakeRange(start, end - start);
        }
        start = end;
        ++n;
    }
}


@implementation CscdA11yNode
- (CscdView*)view { return self.link.window ? self.link.window->view : NULL; }
- (CscdElement*)element { return self.view ? cscdAccessibleElement(self.view, self.number) : NULL; }

- (NSAccessibilityRole)accessibilityRole {
    CscdElement* e = self.element;
    if (!e) return NSAccessibilityUnknownRole;
    const uint32_t state = cscdAccessibleState(self.view, e);
    switch (cscdAccessibleRole(e)) {
    case CSCD_ROLE_BUTTON: return NSAccessibilityButtonRole;
    case CSCD_ROLE_LINK: return NSAccessibilityLinkRole;
    case CSCD_ROLE_CHECKBOX: return NSAccessibilityCheckBoxRole;
    case CSCD_ROLE_RADIO: case CSCD_ROLE_TAB: return NSAccessibilityRadioButtonRole;
    case CSCD_ROLE_TEXTBOX: return (state & CSCD_A11Y_MULTILINE) ? NSAccessibilityTextAreaRole : NSAccessibilityTextFieldRole;
    case CSCD_ROLE_COMBOBOX: return NSAccessibilityPopUpButtonRole;
    case CSCD_ROLE_LISTBOX: case CSCD_ROLE_LIST: return NSAccessibilityListRole;
    case CSCD_ROLE_SLIDER: return NSAccessibilitySliderRole;
    case CSCD_ROLE_PROGRESS: return NSAccessibilityProgressIndicatorRole;
    case CSCD_ROLE_HEADING: return @"AXHeading";
    case CSCD_ROLE_IMAGE: return NSAccessibilityImageRole;
    case CSCD_ROLE_TAB_LIST: return NSAccessibilityTabGroupRole;
    case CSCD_ROLE_MENU: return NSAccessibilityMenuRole;
    case CSCD_ROLE_MENU_BAR: return NSAccessibilityMenuBarRole;
    case CSCD_ROLE_MENU_ITEM: return NSAccessibilityMenuItemRole;
    case CSCD_ROLE_TABLE: return NSAccessibilityTableRole;
    case CSCD_ROLE_ROW: return NSAccessibilityRowRole;
    case CSCD_ROLE_CELL: return NSAccessibilityCellRole;
    case CSCD_ROLE_TEXT: return NSAccessibilityStaticTextRole;
    default: return NSAccessibilityGroupRole;
    }
}

- (NSString*)accessibilityLabel {
    CscdElement* e = self.element;
    CscdStr s = {0};
    if (e) cscdAccessibleName(self.view, e, &s);
    return text(&s);
}

- (NSString*)accessibilityHelp {
    CscdElement* e = self.element;
    CscdStr s = {0};
    if (e) cscdAccessibleDescription(self.view, e, &s);
    if (!s.len) {
        cscdStrFree(&s);
        return nil;
    }
    return text(&s);
}

- (id)accessibilityValue {
    CscdElement* e = self.element;
    if (!e) return nil;
    const CscdRole role = cscdAccessibleRole(e);
    const uint32_t state = cscdAccessibleState(self.view, e);
    if (role == CSCD_ROLE_CHECKBOX || role == CSCD_ROLE_RADIO) return @((state & CSCD_A11Y_MIXED) ? 2 : (state & CSCD_A11Y_CHECKED) ? 1 : 0);
    double value, min, max, step;
    if (cscdAccessibleRange(e, &value, &min, &max, &step)) return @(value);
    if (role == CSCD_ROLE_HEADING) return @(cscdAccessibleLevel(e));
    if (state & CSCD_A11Y_PASSWORD) return @"";
    CscdStr s = {0};
    cscdAccessibleValue(self.view, e, &s);
    return text(&s);
}

- (void)setAccessibilityValue:(id)value {
    CscdElement* e = self.element;
    if (!e) return;
    NSString* s = [value isKindOfClass:[NSString class]] ? value : [value description];
    cscdAccessibleDo(self.view, e, CSCD_A11Y_SET_VALUE, cscdSv(s.UTF8String));
}

- (id)accessibilityMinValue {
    double value, min, max, step;
    CscdElement* e = self.element;
    return e && cscdAccessibleRange(e, &value, &min, &max, &step) ? @(min) : nil;
}

- (id)accessibilityMaxValue {
    double value, min, max, step;
    CscdElement* e = self.element;
    return e && cscdAccessibleRange(e, &value, &min, &max, &step) ? @(max) : nil;
}

- (BOOL)isAccessibilityEnabled {
    CscdElement* e = self.element;
    return e && !(cscdAccessibleState(self.view, e) & CSCD_A11Y_DISABLED);
}

- (BOOL)isAccessibilityFocused {
    CscdElement* e = self.element;
    return e && (cscdAccessibleState(self.view, e) & CSCD_A11Y_FOCUSED);
}

- (void)setAccessibilityFocused:(BOOL)focused {
    CscdElement* e = self.element;
    if (e && focused) cscdAccessibleDo(self.view, e, CSCD_A11Y_FOCUS, CSCD_SV(""));
}

- (BOOL)accessibilityPerformPress {
    CscdElement* e = self.element;
    return e && cscdAccessibleDo(self.view, e, CSCD_A11Y_ACTIVATE, CSCD_SV(""));
}

/* (a text field: its text by character and line, its selection) */
- (BOOL)isTextField {
    CscdElement* e = self.element;
    return e && cscdAccessibleRole(e) == CSCD_ROLE_TEXTBOX;
}

- (NSString*)fieldText {
    CscdElement* e = self.element;
    if (!e || (cscdAccessibleState(self.view, e) & CSCD_A11Y_PASSWORD)) return @"";
    CscdStr s = {0};
    cscdAccessibleValue(self.view, e, &s);
    return text(&s);
}

- (NSInteger)accessibilityNumberOfCharacters { return self.isTextField ? (NSInteger)self.fieldText.length : 0; }

- (NSRange)accessibilitySelectedTextRange {
    CscdElement* e = self.element;
    size_t a = 0, z = 0;
    if (!e || !cscdAccessibleTextSelection(e, &a, &z)) return NSMakeRange(0, 0);
    NSString* s = self.fieldText;
    const NSUInteger p = utf16At(s, a), q = utf16At(s, z);
    return NSMakeRange(p, q - p);
}

- (void)setAccessibilitySelectedTextRange:(NSRange)range {
    CscdElement* e = self.element;
    if (!e || !self.isTextField) return;
    NSString* s = self.fieldText;
    cscdAccessibleSelectText(self.view, e, bytesAt(s, range.location), bytesAt(s, range.location + range.length));
}

- (NSString*)accessibilitySelectedText {
    if (!self.isTextField) return nil;
    NSString* s = self.fieldText;
    const NSRange r = self.accessibilitySelectedTextRange;
    return NSMaxRange(r) <= s.length ? [s substringWithRange:r] : @"";
}

- (NSRange)accessibilityVisibleCharacterRange { return self.isTextField ? NSMakeRange(0, self.fieldText.length) : NSMakeRange(0, 0); }

- (NSInteger)accessibilityInsertionPointLineNumber {
    if (!self.isTextField) return 0;
    const NSRange sel = self.accessibilitySelectedTextRange;
    NSUInteger line = 0;
    lineRange(self.fieldText, NSMaxRange(sel), &line);
    return (NSInteger)line;
}

- (NSInteger)accessibilityLineForIndex:(NSInteger)index {
    NSUInteger line = 0;
    if (self.isTextField && index >= 0) lineRange(self.fieldText, (NSUInteger)index, &line);
    return (NSInteger)line;
}

- (NSRange)accessibilityRangeForLine:(NSInteger)line {
    if (!self.isTextField || line < 0) return NSMakeRange(0, 0);
    NSString* s = self.fieldText;
    NSUInteger start = 0;
    for (NSInteger k = 0; k < line; ++k) {
        const NSRange nl = [s rangeOfString:@"\n" options:0 range:NSMakeRange(start, s.length - start)];
        if (nl.location == NSNotFound) return NSMakeRange(s.length, 0);
        start = nl.location + 1;
    }
    return lineRange(s, start, NULL);
}

- (NSString*)accessibilityStringForRange:(NSRange)range {
    if (!self.isTextField) return nil;
    NSString* s = self.fieldText;
    return NSMaxRange(range) <= s.length ? [s substringWithRange:range] : nil;
}

- (NSRange)accessibilityRangeForIndex:(NSInteger)index {
    if (!self.isTextField || index < 0) return NSMakeRange(0, 0);
    NSString* s = self.fieldText;
    return (NSUInteger)index < s.length ? [s rangeOfComposedCharacterSequenceAtIndex:(NSUInteger)index] : NSMakeRange(s.length, 0);
}

- (NSRect)accessibilityFrameForRange:(NSRange)range {
    CscdElement* e = self.element;
    CscdWindow* w = self.link.window;
    if (!e || !w || !self.isTextField) return NSZeroRect;
    NSString* s = self.fieldText;
    CscdRectVec rects = {0};
    cscdAccessibleTextRects(w->view, e, bytesAt(s, range.location), bytesAt(s, NSMaxRange(range)), &rects);
    CscdRect all = rects.len ? rects.items[0] : cscdViewElementRect(w->view, e);
    for (size_t i = 1; i < rects.len; ++i) all = cscdRectUnite(all, rects.items[i]);
    cscdVecFree(&rects);
    NSView* nsview = (__bridge NSView*)windowSurface(w); /* (flipped: y goes down, as in the page) */
    const NSRect inWindow = [nsview convertRect:NSMakeRect(all.x, all.y, all.w, all.h) toView:nil];
    return [nsview.window convertRectToScreen:inWindow];
}

- (NSRange)accessibilityRangeForPosition:(NSPoint)point {
    CscdElement* e = self.element;
    CscdWindow* w = self.link.window;
    if (!e || !w || !self.isTextField) return NSMakeRange(0, 0);
    NSView* nsview = (__bridge NSView*)windowSurface(w);
    const NSPoint inWindow = [nsview.window convertPointFromScreen:point];
    const NSPoint p = [nsview convertPoint:inWindow fromView:nil];
    const NSUInteger at = utf16At(self.fieldText, cscdAccessibleTextOffsetAt(w->view, e, (float)p.x, (float)p.y));
    return NSMakeRange(at, 0);
}

- (NSRect)accessibilityFrame {
    CscdElement* e = self.element;
    CscdWindow* w = self.link.window;
    if (!e || !w) return NSZeroRect;
    const CscdRect r = cscdViewElementRect(w->view, e);
    NSView* nsview = (__bridge NSView*)windowSurface(w); /* (flipped: y goes down, as in the page) */
    const NSRect inWindow = [nsview convertRect:NSMakeRect(r.x, r.y, r.w, r.h) toView:nil];
    return [nsview.window convertRectToScreen:inWindow];
}

- (id)accessibilityParent {
    CscdElement* e = self.element;
    CscdWindow* w = self.link.window;
    if (!e || !w) return nil;
    CscdElement* p = cscdAccessibleParent(w->view, e);
    if (!p || p == cscdAccessibleRoot(w->view)) return (__bridge id)windowSurface(w);
    return nodeFor(self.link, p);
}

- (NSArray*)accessibilityChildren {
    CscdElement* e = self.element;
    CscdWindow* w = self.link.window;
    if (!e || !w) return @[];
    CscdPtrVec kids = {0};
    cscdAccessibleChildren(w->view, e, &kids);
    NSMutableArray* out = [NSMutableArray arrayWithCapacity:kids.len];
    for (size_t i = 0; i < kids.len; ++i) [out addObject:nodeFor(self.link, (CscdElement*)kids.items[i])];
    cscdVecFree(&kids);
    return out;
}
@end

static CscdA11yNode* nodeFor(CscdA11yLink* link, CscdElement* e) {
    const uint32_t n = cscdAccessibleId(link.window->view, e);
    CscdA11yNode* node = link.nodes[@(n)];
    if (!node) {
        node = [CscdA11yNode new];
        node.link = link;
        node.number = n;
        link.nodes[@(n)] = node;
    }
    return node;
}

/* ---- the window's side ---------------------------------------------------------------------------- */

static CscdA11yLink* linkOf(CscdWindow* w) { return w->a11y ? (__bridge CscdA11yLink*)w->a11y : nil; }

void* cocoaA11yChildren(CscdWindow* w) {
    CscdA11yLink* link = linkOf(w);
    NSMutableArray* out = [NSMutableArray array];
    if (link && cscdViewFeature(w->view, CSCD_FEATURE_ACCESSIBILITY)) {
        CscdPtrVec kids = {0};
        cscdAccessibleChildren(w->view, NULL, &kids);
        for (size_t i = 0; i < kids.len; ++i) [out addObject:nodeFor(link, (CscdElement*)kids.items[i])];
        cscdVecFree(&kids);
    }
    return (__bridge_retained void*)out;
}

void* cocoaA11yHitTest(CscdWindow* w, double x, double y) {
    CscdA11yLink* link = linkOf(w);
    if (!link) return NULL;
    NSView* nsview = (__bridge NSView*)windowSurface(w);
    const NSPoint inWindow = [nsview.window convertPointFromScreen:NSMakePoint(x, y)];
    const NSPoint p = [nsview convertPoint:inWindow fromView:nil];
    CscdElement* e = cscdAccessibleAt(w->view, (float)p.x, (float)p.y);
    if (!e || e == cscdAccessibleRoot(w->view)) return NULL;
    return (__bridge_retained void*)nodeFor(link, e);
}

void* cocoaA11yFocused(CscdWindow* w) {
    CscdA11yLink* link = linkOf(w);
    CscdElement* f = link ? cscdViewDocument(w->view)->focused : NULL;
    if (!f || !cscdAccessibleIncluded(f)) return NULL;
    return (__bridge_retained void*)nodeFor(link, f);
}

static void onEvent(void* user, CscdElement* e, CscdAccessibleEvent event) {
    CscdWindow* w = (CscdWindow*)user;
    CscdA11yLink* link = linkOf(w);
    if (!link) return;
    if (!e || (event == CSCD_A11Y_EVENT_STRUCTURE && e == cscdAccessibleRoot(w->view))) {
        NSAccessibilityPostNotification((__bridge id)windowSurface(w), NSAccessibilityLayoutChangedNotification);
        return;
    }
    if (event == CSCD_A11Y_EVENT_LIVE) {
        /* a live region's text, said (polite or assertive) */
        CscdStr text = {0};
        cscdNodeTextContent(cscdElementNode(e), &text);
        NSString* said = [[NSString alloc] initWithBytes:text.ptr ? text.ptr : "" length:text.len encoding:NSUTF8StringEncoding];
        cscdStrFree(&text);
        if (said.length)
            NSAccessibilityPostNotificationWithUserInfo(NSApp, NSAccessibilityAnnouncementRequestedNotification, @{
                NSAccessibilityAnnouncementKey : said,
                NSAccessibilityPriorityKey : @(cscdAccessibleLive(e) == 2 ? NSAccessibilityPriorityHigh : NSAccessibilityPriorityMedium)
            });
        return;
    }
    if (!cscdAccessibleIncluded(e)) return;
    CscdA11yNode* node = nodeFor(link, e);
    NSAccessibilityNotificationName what = event == CSCD_A11Y_EVENT_FOCUS ? NSAccessibilityFocusedUIElementChangedNotification
                                           : event == CSCD_A11Y_EVENT_STRUCTURE ? NSAccessibilityLayoutChangedNotification
                                           : event == CSCD_A11Y_EVENT_NAME ? NSAccessibilityTitleChangedNotification
                                                                           : NSAccessibilityValueChangedNotification;
    NSAccessibilityPostNotification(node, what);
}

void cocoaA11yAttach(CscdWindow* w) {
    CscdA11yLink* link = [CscdA11yLink new];
    link.window = w;
    link.nodes = [NSMutableDictionary dictionary];
    w->a11y = (__bridge_retained void*)link;
    cscdViewOnAccessibleEvent(w->view, onEvent, w);
}

void cocoaA11yDetach(CscdWindow* w) {
    if (!w->a11y) return;
    cscdViewOnAccessibleEvent(w->view, NULL, NULL);
    CscdA11yLink* link = (__bridge_transfer CscdA11yLink*)w->a11y;
    w->a11y = NULL;
    link.window = NULL; /* (elements a reader still holds answer nothing from now on) */
    [link.nodes removeAllObjects];
}
