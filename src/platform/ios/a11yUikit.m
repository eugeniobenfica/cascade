/* UIAccessibility: the page as VoiceOver sees it on iOS, from the view's accessible tree (a11y.c).
 * VoiceOver moves through a flat list, so the page's view offers its controls, text and headings in
 * document order, with the plain containers (groups, lists, tables...) left out and their contents in
 * their place. One element object per accessible element, kept by its accessible number (VoiceOver
 * expects the same object each time), all holding a link to the window that is cut when it goes.
 * Objective-C with ARC (CMakeLists.txt). */
#include "platform/common/platformInternal.h"

#import <UIKit/UIKit.h>

#include <stdio.h>

@interface CscdUIA11yLink : NSObject
@property(nonatomic, assign) CscdWindow* window;
@property(nonatomic, strong) NSMutableDictionary<NSNumber*, id>* nodes;
@property(nonatomic, assign) BOOL layoutPending;
@end
@implementation CscdUIA11yLink
@end

@interface CscdUIA11yNode : UIAccessibilityElement
@property(nonatomic, strong) CscdUIA11yLink* link;
@property(nonatomic, assign) uint32_t number;
@end

static NSString* text(CscdStr* s) {
    NSString* t = [[NSString alloc] initWithBytes:s->ptr length:s->len encoding:NSUTF8StringEncoding];
    cscdStrFree(s);
    return t ?: @"";
}

static UIView* pageView(CscdUIA11yLink* link) {
    return link.window && link.window->native ? (__bridge UIView*)windowSurface(link.window) : nil;
}

@implementation CscdUIA11yNode
- (CscdView*)view { return self.link.window ? self.link.window->view : NULL; }
- (CscdElement*)element { return self.view ? cscdAccessibleElement(self.view, self.number) : NULL; }

- (BOOL)isAccessibilityElement { return self.element != NULL; }

- (NSString*)accessibilityLabel {
    CscdElement* e = self.element;
    CscdStr s = {0};
    if (e) cscdAccessibleName(self.view, e, &s);
    return text(&s);
}

- (NSString*)accessibilityHint {
    CscdElement* e = self.element;
    CscdStr s = {0};
    if (e) cscdAccessibleDescription(self.view, e, &s);
    if (!s.len) {
        cscdStrFree(&s);
        return nil;
    }
    return text(&s);
}

- (NSString*)accessibilityValue {
    CscdElement* e = self.element;
    if (!e) return nil;
    const CscdRole role = cscdAccessibleRole(e);
    if (role == CSCD_ROLE_CHECKBOX || role == CSCD_ROLE_RADIO) return nil; /* (the selected trait says it) */
    CscdStr s = {0};
    cscdAccessibleValue(self.view, e, &s);
    if (!s.len) {
        cscdStrFree(&s);
        return nil;
    }
    return text(&s);
}

- (UIAccessibilityTraits)accessibilityTraits {
    CscdElement* e = self.element;
    if (!e) return UIAccessibilityTraitNone;
    const uint32_t state = cscdAccessibleState(self.view, e);
    UIAccessibilityTraits t = UIAccessibilityTraitNone;
    switch (cscdAccessibleRole(e)) {
    case CSCD_ROLE_BUTTON: case CSCD_ROLE_MENU_ITEM: case CSCD_ROLE_COMBOBOX: case CSCD_ROLE_TAB: case CSCD_ROLE_OPTION:
        t |= UIAccessibilityTraitButton;
        break;
    case CSCD_ROLE_CHECKBOX:
        t |= UIAccessibilityTraitButton;
        if (@available(iOS 17.0, *)) t |= UIAccessibilityTraitToggleButton;
        break;
    case CSCD_ROLE_RADIO: t |= UIAccessibilityTraitButton; break;
    case CSCD_ROLE_LINK: t |= UIAccessibilityTraitLink; break;
    case CSCD_ROLE_HEADING: t |= UIAccessibilityTraitHeader; break;
    case CSCD_ROLE_IMAGE: t |= UIAccessibilityTraitImage; break;
    case CSCD_ROLE_SLIDER: t |= UIAccessibilityTraitAdjustable; break;
    case CSCD_ROLE_PROGRESS: t |= UIAccessibilityTraitUpdatesFrequently; break;
    case CSCD_ROLE_TEXT: t |= UIAccessibilityTraitStaticText; break;
    default: break;
    }
    if (state & (CSCD_A11Y_CHECKED | CSCD_A11Y_SELECTED)) t |= UIAccessibilityTraitSelected;
    if (state & CSCD_A11Y_DISABLED) t |= UIAccessibilityTraitNotEnabled;
    return t;
}

- (CGRect)accessibilityFrame {
    CscdElement* e = self.element;
    UIView* view = pageView(self.link);
    if (!e || !view) return CGRectZero;
    const CscdRect r = cscdViewElementRect(self.view, e); /* (CSS pixels are the view's points) */
    return UIAccessibilityConvertFrameToScreenCoordinates(CGRectMake(r.x, r.y, r.w, r.h), view);
}

- (BOOL)accessibilityActivate {
    CscdElement* e = self.element;
    if (!e) return NO;
    const CscdAccessibleAction a = cscdAccessibleRole(e) == CSCD_ROLE_TEXTBOX ? CSCD_A11Y_FOCUS : CSCD_A11Y_ACTIVATE;
    return cscdAccessibleDo(self.view, e, a, CSCD_SV(""));
}

- (void)step:(double)direction {
    CscdElement* e = self.element;
    double value, min, max, step;
    if (!e || !cscdAccessibleRange(e, &value, &min, &max, &step)) return;
    if (step <= 0) step = (max - min) / 20;
    value = fmax(min, fmin(max, value + direction * step));
    char buf[64];
    snprintf(buf, sizeof buf, "%g", value);
    cscdAccessibleDo(self.view, e, CSCD_A11Y_SET_VALUE, cscdSv(buf));
}
- (void)accessibilityIncrement { [self step:1]; }
- (void)accessibilityDecrement { [self step:-1]; }

- (BOOL)accessibilityScroll:(UIAccessibilityScrollDirection)direction { return NO; }
@end

static CscdUIA11yNode* nodeFor(CscdUIA11yLink* link, CscdElement* e) {
    const uint32_t number = cscdAccessibleId(link.window->view, e);
    NSNumber* key = @(number);
    CscdUIA11yNode* node = link.nodes[key];
    if (!node) {
        node = [[CscdUIA11yNode alloc] initWithAccessibilityContainer:pageView(link)];
        node.link = link;
        node.number = number;
        link.nodes[key] = node;
    }
    return node;
}

/* What VoiceOver stops at by itself; the other roles are containers, read through their contents. */
static bool isStop(CscdRole role) {
    switch (role) {
    case CSCD_ROLE_TEXT: case CSCD_ROLE_BUTTON: case CSCD_ROLE_LINK: case CSCD_ROLE_CHECKBOX: case CSCD_ROLE_RADIO:
    case CSCD_ROLE_TEXTBOX: case CSCD_ROLE_COMBOBOX: case CSCD_ROLE_OPTION: case CSCD_ROLE_SLIDER: case CSCD_ROLE_PROGRESS:
    case CSCD_ROLE_HEADING: case CSCD_ROLE_IMAGE: case CSCD_ROLE_TAB: case CSCD_ROLE_MENU_ITEM:
        return true;
    default: return false;
    }
}

static void collect(CscdUIA11yLink* link, CscdElement* parent, NSMutableArray* out, int depth) {
    if (depth > 64) return;
    CscdView* view = link.window->view;
    CscdPtrVec kids = {0};
    cscdAccessibleChildren(view, parent, &kids);
    for (size_t i = 0; i < kids.len; ++i) {
        CscdElement* e = (CscdElement*)kids.items[i];
        const CscdRole role = cscdAccessibleRole(e);
        if (role == CSCD_ROLE_SEPARATOR) continue;
        if (isStop(role)) {
            [out addObject:nodeFor(link, e)];
            continue;
        }
        const NSUInteger before = out.count;
        collect(link, e, out, depth + 1);
        if (out.count == before) {
            /* an empty container with a name of its own (a labelled group, a dialog's title) */
            CscdStr name = {0};
            cscdAccessibleName(view, e, &name);
            if (name.len) [out addObject:nodeFor(link, e)];
            cscdStrFree(&name);
        }
    }
    cscdVecFree(&kids);
}

void* uikitA11yElements(CscdWindow* w) {
    CscdUIA11yLink* link = (__bridge CscdUIA11yLink*)w->a11y;
    NSMutableArray* out = [NSMutableArray array];
    if (link && link.window) collect(link, NULL, out, 0);
    return (void*)CFBridgingRetain(out);
}

static void onAccessibleEvent(void* user, CscdElement* e, CscdAccessibleEvent event) {
    CscdUIA11yLink* link = (__bridge CscdUIA11yLink*)user;
    if (!link.window || !UIAccessibilityIsVoiceOverRunning()) return;
    if (event == CSCD_A11Y_EVENT_FOCUS && e) {
        UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, nodeFor(link, e));
    } else if (event == CSCD_A11Y_EVENT_STRUCTURE && !link.layoutPending) {
        /* once for a burst of changes: the elements of removed ones go, VoiceOver reads the page again */
        link.layoutPending = YES;
        dispatch_async(dispatch_get_main_queue(), ^{
            link.layoutPending = NO;
            if (!link.window) return;
            for (NSNumber* key in link.nodes.allKeys)
                if (!cscdAccessibleElement(link.window->view, key.unsignedIntValue)) [link.nodes removeObjectForKey:key];
            UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, nil);
        });
    }
}

void uikitA11yAttach(CscdWindow* w) {
    CscdUIA11yLink* link = [CscdUIA11yLink new];
    link.window = w;
    link.nodes = [NSMutableDictionary dictionary];
    w->a11y = (void*)CFBridgingRetain(link);
    cscdViewOnAccessibleEvent(w->view, onAccessibleEvent, w->a11y);
}

void uikitA11yDetach(CscdWindow* w) {
    if (!w->a11y) return;
    cscdViewOnAccessibleEvent(w->view, NULL, NULL);
    CscdUIA11yLink* link = (CscdUIA11yLink*)CFBridgingRelease(w->a11y);
    link.window = NULL; /* (elements VoiceOver still holds answer nothing) */
    [link.nodes removeAllObjects];
    w->a11y = NULL;
}
