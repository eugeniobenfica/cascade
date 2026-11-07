/* NSAccessibility: the page as VoiceOver sees it, from the view's accessible tree (a11y.c). One
 * element object per accessible element, kept by its accessible number (VoiceOver expects the same
 * object each time), all holding a link to the window that is cut when the window goes. */
#include "platform/common/platformInternal.h"

#import <AppKit/AppKit.h>

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
    if (event == CSCD_A11Y_EVENT_STRUCTURE || !e) {
        NSAccessibilityPostNotification((__bridge id)windowSurface(w), NSAccessibilityLayoutChangedNotification);
        return;
    }
    if (!cscdAccessibleIncluded(e)) return;
    CscdA11yNode* node = nodeFor(link, e);
    NSAccessibilityPostNotification(node, event == CSCD_A11Y_EVENT_FOCUS ? NSAccessibilityFocusedUIElementChangedNotification
                                                                          : NSAccessibilityValueChangedNotification);
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
