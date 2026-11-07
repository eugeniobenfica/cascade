/* cascade — CscdView: a document with its style sheets, layout, painting, input handling, scrolling,
 * transitions and animations. This is the object an application owns; a window (or your own GL /
 * Vulkan surface) shows it. */
#ifndef CASCADE_VIEW_H
#define CASCADE_VIEW_H

#include "cascade/canvas.h"
#include "cascade/dom.h"
#include "cascade/layout.h"
#include "cascade/style.h"

#ifdef __cplusplus
extern "C" {
#endif

/* (cascadeC.h defines the same names, with the same values, when it comes first) */
#ifdef CASCADE_KEYS_DEFINED
typedef cscd_key CscdKey;
#else
typedef enum CscdKey {
    CSCD_KEY_UNKNOWN, CSCD_KEY_BACKSPACE, CSCD_KEY_TAB, CSCD_KEY_ENTER, CSCD_KEY_ESCAPE, CSCD_KEY_SPACE, CSCD_KEY_DELETE, CSCD_KEY_LEFT, CSCD_KEY_RIGHT,
    CSCD_KEY_UP, CSCD_KEY_DOWN, CSCD_KEY_HOME, CSCD_KEY_END, CSCD_KEY_PAGE_UP, CSCD_KEY_PAGE_DOWN, CSCD_KEY_INSERT, CSCD_KEY_SHIFT, CSCD_KEY_CONTROL,
    CSCD_KEY_ALT, CSCD_KEY_META, CSCD_KEY_F1, CSCD_KEY_F2, CSCD_KEY_F3, CSCD_KEY_F4, CSCD_KEY_F5, CSCD_KEY_F6, CSCD_KEY_F7, CSCD_KEY_F8, CSCD_KEY_F9,
    CSCD_KEY_F10, CSCD_KEY_F11, CSCD_KEY_F12, CSCD_KEY_A, CSCD_KEY_C, CSCD_KEY_V, CSCD_KEY_X, CSCD_KEY_Z, CSCD_KEY_Y, CSCD_KEY_CHARACTER
} CscdKey;
#endif

typedef struct CscdModifiers { bool shift, ctrl, alt, meta; } CscdModifiers;

/* A plain callback: a function, its user data and what frees that data when it is dropped. */
typedef void (*CscdActionFn)(void* user);
typedef struct CscdAction { CscdActionFn fn; void* user; CscdFreeFn freeUser; } CscdAction;
CSCD_INLINE CscdAction cscdAction(CscdActionFn fn, void* user) { CscdAction a = {fn, user, NULL}; return a; }
CSCD_INLINE void cscdActionCall(const CscdAction* a) { if (a->fn) a->fn(a->user); }
void cscdActionFree(CscdAction* a); /* frees the user data (freeUser) */

/* ---- menus ----------------------------------------------------------------------------------------- */

typedef struct CscdMenuItem CscdMenuItem;
typedef CSCD_VEC(CscdMenuItem) CscdMenu;
/* fills a submenu as it opens */
typedef void (*CscdMenuFillFn)(void* user, CscdMenu* submenu);
typedef struct CscdMenuFill { CscdMenuFillFn fn; void* user; CscdFreeFn freeUser; } CscdMenuFill;

/* One entry of a popup menu: context menus, menu bars, <select> lists. A menu given to the view
 * becomes the view's (it frees the items and their callbacks' user data). */
struct CscdMenuItem {
    CscdStr label;
    CscdStr shortcut; /* shown at the right ("Ctrl+S") */
    CscdStr icon;     /* put in the icon slot's data-icon (the app paints it in onMenuItem) */
    CscdStr tooltip;
    CscdStr id;       /* put in the row's data-id */
    bool separator;
    bool enabled;   /* (true in the ones made here) */
    bool checkable, checked;
    CscdMenu submenu;
    CscdMenuFill fill;
    CscdAction action;
};

/* An item that calls fn(user) when chosen; label and shortcut are copied. */
CscdMenuItem cscdMenuItem(CscdSv label, CscdActionFn fn, void* user, CscdSv shortcut);
CscdMenuItem cscdMenuSeparator(void);
/* An item that opens submenu (taken). */
CscdMenuItem cscdMenuSub(CscdSv label, CscdMenu submenu);
/* Appends item (taken); returns it in the menu. */
CscdMenuItem* cscdMenuAdd(CscdMenu* m, CscdMenuItem item);
void cscdMenuItemFini(CscdMenuItem* item);
void cscdMenuFree(CscdMenu* m);


/* ---- the view ------------------------------------------------------------------------------------- */

typedef struct CscdView CscdView;

typedef void (*CscdWarningFn)(void* user, CscdSv message);
typedef void (*CscdMenuItemFn)(void* user, CscdElement* row, const CscdMenuItem* item);
typedef void (*CscdMenuBarFn)(void* user, CscdMenu* out); /* the menu of a menu bar entry, appended to out */
typedef void (*CscdSetClipboardFn)(void* user, CscdSv text);
typedef void (*CscdGetClipboardFn)(void* user, CscdStr* out); /* appended */
typedef void (*CscdSetClipboardImageFn)(void* user, const CscdImage* image);
typedef void (*CscdGetClipboardFilesFn)(void* user, CscdStrVec* out); /* appended */

CscdView* cscdViewNew(void);
void cscdViewFree(CscdView* v);

/* content. A relative path that is not found from the working folder is looked for next to the
 * program (cscdAppFindFile), so "ui/index.html" works wherever the program is started from. */
bool cscdViewLoadFile(CscdView* v, CscdSv path);
void cscdViewLoadHTML(CscdView* v, CscdSv html, CscdSv baseDir);
void cscdViewAddStyleSheet(CscdView* v, CscdSv css, CscdSv baseDir); /* baseDir "": the document's */
/* The sheet under the document's own, for what a program's pages share: it stays when the view loads another page. */
void cscdViewSetBaseStyleSheet(CscdView* v, CscdSv css, CscdSv baseDir);
bool cscdViewAddStyleSheetFile(CscdView* v, CscdSv path);
/* What a view does that an application may not want: each one can be turned off (or on). The
 * environment variable CASCADE_FEATURES changes the defaults of every view without rebuilding: a
 * list of names, each with + (on) or - (off), as in "+stats,-gpu,+inspector". */
#ifdef CASCADE_FEATURES_DEFINED
typedef cscd_feature CscdFeature; /* (cascadeC.h came first: the same names and numbers) */
#else
typedef enum CscdFeature {
    CSCD_FEATURE_PAINT_THREADS,   /* "paint-threads": large frames painted by bands on several threads (on) */
    CSCD_FEATURE_PARTIAL_REPAINT, /* "partial-repaint": only invalidated elements painted again (on) */
    CSCD_FEATURE_ANIMATIONS,      /* "animations": transitions and keyframe animations; off, styles change at
                                    * once, animations show their end, and prefers-reduced-motion holds (on) */
    CSCD_FEATURE_SMOOTH_SCROLL,   /* "smooth-scroll": wheel steps glide (on) */
    CSCD_FEATURE_TOOLTIPS,        /* "tooltips": the title attribute shown after a hover (on) */
    CSCD_FEATURE_LIVE_RELOAD,     /* "live-reload": style sheet files read again when they change on disk
                                    * (off; a window turns it on in debug builds) */
    CSCD_FEATURE_ACCESSIBILITY,   /* "accessibility": the page offered to screen readers by the window (on) */
    CSCD_FEATURE_INSPECTOR,       /* "inspector": Ctrl+Shift+I shows the box, style and rules under the
                                    * pointer (off; a window turns it on in debug builds) */
    CSCD_FEATURE_STATS,           /* "stats": the window shows what the last frame cost (off) */
    CSCD_FEATURE_GPU,             /* "gpu": the window composites and paints on the GPU when it can (on) */
    CSCD_FEATURE_WARN_FILES,      /* "warn-files": a page, sheet, image or font that cannot be read (on) */
    CSCD_FEATURE_WARN_CSS,        /* "warn-css": properties nothing computes, with what they probably meant (on) */
    CSCD_FEATURE_WARN_SELECTORS,  /* "warn-selectors": cscdViewGet finding nothing (on) */
    CSCD_FEATURE_COUNT
} CscdFeature;
#endif
void cscdViewSetFeature(CscdView* v, CscdFeature f, bool on);
bool cscdViewFeature(const CscdView* v, CscdFeature f);
const char* cscdFeatureName(CscdFeature f); /* ("paint-threads"...) */
bool cscdFeatureInEnvironment(CscdFeature f); /* CASCADE_FEATURES names it (its choice then stands) */

/* Problems worth knowing about (the CSCD_FEATURE_WARN_ kinds that are on). By default they go to
 * stderr (and to the debugger's output on Windows); a NULL function: silence. */
void cscdViewOnWarning(CscdView* v, CscdWarningFn fn, void* user);
/* Every node that leaves the document, and every page about to replace the document (all its nodes
 * go): language bindings forget the handles they gave out here (the C API does). */
void cscdViewOnNodeRemoved(CscdView* v, CscdNodeFn fn, void* user);
void cscdViewOnDocumentReplaced(CscdView* v, CscdActionFn fn, void* user);
CscdDocument* cscdViewDocument(CscdView* v);
CscdElement* cscdViewGetElementById(CscdView* v, CscdSv id);
CscdElement* cscdViewQuerySelector(CscdView* v, CscdSv selector);
void cscdViewQuerySelectorAll(CscdView* v, CscdSv selector, CscdPtrVec* out);
/* The element a selector names ("#save", ".status", "main > h1"; a bare word is also tried as an
 * id). Never NULL: when nothing matches it warns and returns a detached element that goes nowhere,
 * so a typo shows up as a message instead of a crash. */
CscdElement* cscdViewGet(CscdView* v, CscdSv selector);
CscdStr cscdViewTitle(const CscdView* v); /* the caller's */

/* viewport, in CSS pixels; dpr is the device pixel ratio (1.5 at 150% scaling) */
void cscdViewSetViewport(CscdView* v, float width, float height, float dpr);
float cscdViewWidth(const CscdView* v);
float cscdViewHeight(const CscdView* v);
float cscdViewDevicePixelRatio(const CscdView* v);
void cscdViewSetDarkMode(CscdView* v, bool dark); /* (from then on the system's theme leaves it) */
/* What the system asks of pages: its dark theme (prefers-color-scheme; the default canvas turns dark
 * too when the page's color-scheme names dark, as in browsers), less motion
 * (prefers-reduced-motion: the "animations" feature off) and its text size (the root's default font
 * size and the font-size keywords scale by it; sizes in px stay). The window passes the system's
 * settings here as they change; what the app chose (cscdViewSetDarkMode, cscdViewSetFeature,
 * CASCADE_FEATURES) wins over them. */
typedef struct CscdSystemPreferences { bool dark; bool reducedMotion; float textScale; } CscdSystemPreferences;
void cscdViewSetSystemPreferences(CscdView* v, CscdSystemPreferences p);
CscdSystemPreferences cscdViewSystemPreferences(const CscdView* v); /* (what the view uses now) */

/* Brings style and layout up to date and advances animations. Returns true when a repaint is needed. */
bool cscdViewUpdate(CscdView* v);
/* Paints into a premultiplied BGRA image of the device size (width * dpr x height * dpr).
 * incremental: the target still holds the frame this view painted last (a window's own frame): when
 * only cscdViewInvalidateElement asked since, only those elements' areas are painted. */
void cscdViewPaint(CscdView* v, CscdImage* target, bool incremental);
/* on: the page goes over what the host draws. What the page leaves empty stays transparent in the
 * frame (no white or dark canvas under it), and what is translucent stays so, premultiplied. For a
 * presenter of the app's (CscdWindowOptions.presenter) or a host painting the frame over its own
 * picture; the system's presenters show a frame as if it were opaque. Off by default. */
void cscdViewSetTransparent(CscdView* v, bool on);
void cscdViewPaintCanvas(CscdView* v, CscdCanvas* canvas); /* into an existing canvas (already scaled for the device) */
bool cscdViewNeedsPaint(const CscdView* v);
/* The page's own background (the root's, or body's), opaque: what a host shows around the page, as
 * the windows of phones do under the system's bars. */
CscdColor cscdViewCanvasColor(CscdView* v);

/* What the view's work cost the last time each part ran, to find what makes a page slow. */
typedef struct CscdFrameStats {
    double styleMs;      /* computing styles (all of them, or only where a state changed) */
    double layoutMs;     /* laying the page out (with the passes container queries asked for) */
    double paintMs;      /* painting a frame (recording it and drawing its bands) */
    int stylesComputed;  /* styles the last style pass worked out */
    int stylesReused;    /* styles it found already computed (the same rules, the same parent) */
    double paintedArea;  /* device pixels the last paint covered */
    bool partialPaint;   /* it painted only the areas cscdViewInvalidateElement asked for */
    uint64_t styles, layouts, paints; /* how many times each ran (paints that painted something) */
} CscdFrameStats;
CscdFrameStats cscdViewFrameStats(const CscdView* v);
/* What the last cscdViewPaint changed in the target, in device pixels: false when it may have
 * changed all of it; true with the rectangles appended (none: nothing changed). A host that keeps
 * the frame on the GPU uploads only those. */
bool cscdViewPaintDamage(const CscdView* v, CscdIRectVec* out);
bool cscdViewAnimating(const CscdView* v); /* transitions / animations / caret blink pending */
/* Called whenever the view wants to be repainted (from input, timers, DOM changes). */
void cscdViewOnInvalidate(CscdView* v, CscdActionFn fn, void* user);
void cscdViewInvalidate(CscdView* v);
/* Repaints just this element's box: what its painter draws changed, and nothing of its style or
 * layout (a canvas, a custom-painted panel). Anything else repaints the whole page. */
void cscdViewInvalidateElement(CscdView* v, CscdElement* e);

/* input, in CSS pixels relative to the viewport */
void cscdViewMouseMove(CscdView* v, float x, float y, int buttons, CscdModifiers m);
void cscdViewMouseDown(CscdView* v, float x, float y, int button, CscdModifiers m, int clicks);
void cscdViewMouseUp(CscdView* v, float x, float y, int button, CscdModifiers m);
void cscdViewMouseLeave(CscdView* v);
void cscdViewWheel(CscdView* v, float x, float y, float dx, float dy, CscdModifiers m);
void cscdViewKeyDown(CscdView* v, CscdKey k, CscdModifiers m, CscdSv keyName);
void cscdViewKeyUp(CscdView* v, CscdKey k, CscdModifiers m, CscdSv keyName);
void cscdViewTextInput(CscdView* v, CscdSv utf8);
void cscdViewFocus(CscdView* v, CscdElement* e);
void cscdViewBlur(CscdView* v);
CscdCursor cscdViewCursor(const CscdView* v);

/* Popup menus, made of elements in the document: div.cscd-menu > div.cscd-menu-item
 * (span.cscd-menu-check, span.cscd-menu-icon, span.cscd-menu-label, span.cscd-menu-shortcut,
 * span.cscd-menu-arrow) and div.cscd-menu-separator. Style them with CSS. The menus are taken. */
void cscdViewShowMenu(CscdView* v, CscdMenu items, float x, float y, float minWidth);
void cscdViewShowMenuAt(CscdView* v, CscdElement* anchor, CscdMenu items); /* below the anchor (or above, when there is no room) */
/* A menu bar entry: a press opens its menu; while one is open, hovering another entry of the same
 * bar opens that one instead. */
void cscdViewBindMenuBar(CscdView* v, CscdElement* entry, CscdMenuBarFn items, void* user, CscdFreeFn freeUser);
void cscdViewCloseMenus(CscdView* v);
bool cscdViewMenuOpen(const CscdView* v);
void cscdViewOnMenuItem(CscdView* v, CscdMenuItemFn fn, void* user); /* decorate a row (icon painters) */

/* <dialog> elements, as the web has them: showModal centers it over a dimmed page that does not
 * take input while it is open (Esc sends "cancel", then closes it), showDialog opens it in place;
 * closeDialog sets its returnvalue attribute and sends "close". A <form method=dialog> closes its
 * dialog with the submitting button's value. */
void cscdViewShowModal(CscdView* v, CscdElement* dialog);
void cscdViewShowDialog(CscdView* v, CscdElement* dialog);
void cscdViewCloseDialog(CscdView* v, CscdElement* dialog, CscdSv returnValue);
CscdElement* cscdViewModalDialog(const CscdView* v);

/* Popovers (the popover attribute): showPopover shows one over the page, under anchor (above it
 * when there is no room; at the pointer without one); a press outside it or Esc hides it (light
 * dismiss). It gets "toggle" when it opens and when it closes. */
void cscdViewShowPopover(CscdView* v, CscdElement* popover, CscdElement* anchor);
void cscdViewHidePopover(CscdView* v, CscdElement* popover);
bool cscdViewPopoverOpen(const CscdView* v, const CscdElement* popover);

/* Tooltips: an element's title attribute, shown as div.cscd-tooltip after a short hover. */
void cscdViewSetTooltipDelay(CscdView* v, int ms); /* (650) */
void cscdViewHideTooltip(CscdView* v);

/* Drag and drop. Elements with draggable="true" start a drag (dragstart, drag, dragend on the
 * source; dragenter, dragover, dragleave, drop on the targets; a target accepts by calling
 * cscdEventPreventDefault in dragover). The window feeds drags from the system here. */
bool cscdViewDragging(const CscdView* v);
void cscdViewExternalDragEnter(CscdView* v, CscdDataTransfer* data, float x, float y); /* retains data */
bool cscdViewExternalDragOver(CscdView* v, float x, float y); /* true when the element under the pointer accepts it */
void cscdViewExternalDragLeave(CscdView* v);
bool cscdViewExternalDrop(CscdView* v, float x, float y);
void cscdViewCancelDrag(CscdView* v);

/* Touch, in CSS pixels. touchstart / touchmove / touchend go to the page; unless it calls
 * preventDefault on touchstart, one finger scrolls (with momentum), taps click, a long press opens
 * the context menu, a drag where nothing scrolls becomes a mouse drag, and two fingers pan and pinch
 * (as wheel and ctrl+wheel events, like browsers do). */
void cscdViewTouchDown(CscdView* v, int id, float x, float y);
void cscdViewTouchMove(CscdView* v, int id, float x, float y);
void cscdViewTouchUp(CscdView* v, int id, float x, float y);
void cscdViewTouchCancel(CscdView* v);

/* Input method composition (Chinese, Japanese, Korean...): shown at the caret, underlined. */
void cscdViewSetComposition(CscdView* v, CscdSv utf8);
CscdRect cscdViewCaretRect(CscdView* v); /* the focused text field's caret, relative to the viewport */

/* The focused text field as the on-screen keyboards of phones see it (Android's InputConnection,
 * UIKit's UITextInput): all its text, the selection (anchor and caret) and the part the input method
 * is still composing, shown underlined; offsets in bytes of the UTF-8 text. composeStart ==
 * composeEnd: nothing is being composed. */
typedef struct CscdTextState {
    CscdStr text;
    size_t selStart, selEnd;
    size_t composeStart, composeEnd;
} CscdTextState;
bool cscdViewGetTextState(CscdView* v, CscdTextState* out); /* false: no text field has the focus */
/* The field takes the text, selection and composition given (an "input" event when its text changed;
 * offsets clamped to the text and to whole characters). Nothing happens without a focused field. */
void cscdViewSetTextState(CscdView* v, const CscdTextState* s);
void cscdTextStateFini(CscdTextState* s);
/* The box of a range of the focused field's text (its part on the line where it starts) and the
 * offset nearest a point, relative to the viewport: where an input method puts its candidates and
 * a touch places the caret. */
CscdRect cscdViewTextRangeRect(CscdView* v, size_t start, size_t end);
size_t cscdViewTextOffsetAt(CscdView* v, float x, float y);

/* Selecting text by touch, as phones do. A long press on a text field selects the word there (puts
 * the caret, with no word), a double tap selects a word and a tap on the caret asks for the menu
 * (unless the page's contextmenu or touchstart handler prevents it). The view draws handles at the
 * ends of the selection, or under the caret, which a finger drags, and asks the window for the
 * system's edit menu next to it (fn: show, the selection's box relative to the viewport, the
 * CSCD_EDIT_BIT()s it offers; show false: hide it); without one, the view opens a cscd-menu of its
 * own. The magnifier follows a dragged handle (x, y: the caret's middle). A key, a mouse, typing or
 * the field losing the focus take the handles away. */
typedef enum CscdEditAction { CSCD_EDIT_CUT, CSCD_EDIT_COPY, CSCD_EDIT_PASTE, CSCD_EDIT_SELECT_ALL } CscdEditAction;
#define CSCD_EDIT_BIT(a) (1u << (unsigned)(a))
typedef void (*CscdEditMenuFn)(void* user, bool show, CscdRect around, unsigned actions);
typedef void (*CscdMagnifierFn)(void* user, bool show, float x, float y);
void cscdViewOnEditMenu(CscdView* v, CscdEditMenuFn fn, void* user);
void cscdViewOnMagnifier(CscdView* v, CscdMagnifierFn fn, void* user);
/* Cut, copy, paste or select all in the focused text field (the edit menu's, or a toolbar's): false
 * when the field cannot (nothing selected, read-only, a password copied, no field). */
bool cscdViewEditAction(CscdView* v, CscdEditAction a);
unsigned cscdViewEditActions(CscdView* v); /* the CSCD_EDIT_BIT()s the focused field can do now */
void cscdViewHideSelectionHandles(CscdView* v); /* (and the edit menu) */
bool cscdViewSelectionHandlesShown(const CscdView* v);

/* What keyboard suits the focused field, from its type and its inputmode, enterkeyhint,
 * autocapitalize, autocorrect and spellcheck attributes, as browsers on phones read them. kind NONE:
 * no on-screen keyboard (nothing to type in, or inputmode="none"). */
typedef enum CscdInputKind {
    CSCD_INPUT_NONE, CSCD_INPUT_TEXT, CSCD_INPUT_MULTILINE, CSCD_INPUT_NUMBER, CSCD_INPUT_DECIMAL, CSCD_INPUT_TEL,
    CSCD_INPUT_EMAIL, CSCD_INPUT_URL, CSCD_INPUT_SEARCH, CSCD_INPUT_PASSWORD
} CscdInputKind;
typedef enum CscdEnterKey {
    CSCD_ENTER_DEFAULT, CSCD_ENTER_ENTER, CSCD_ENTER_DONE, CSCD_ENTER_GO, CSCD_ENTER_NEXT, CSCD_ENTER_PREVIOUS,
    CSCD_ENTER_SEARCH, CSCD_ENTER_SEND
} CscdEnterKey;
typedef enum CscdCapitalize { CSCD_CAPITALIZE_NONE, CSCD_CAPITALIZE_SENTENCES, CSCD_CAPITALIZE_WORDS, CSCD_CAPITALIZE_CHARACTERS } CscdCapitalize;
typedef struct CscdInputTraits {
    CscdInputKind kind;
    CscdEnterKey enter;
    CscdCapitalize capitalize;
    bool autocorrect;
} CscdInputTraits;
CscdInputTraits cscdViewInputTraits(CscdView* v);
CSCD_INLINE bool cscdInputTraitsEq(CscdInputTraits a, CscdInputTraits b) {
    return a.kind == b.kind && a.enter == b.enter && a.capitalize == b.capitalize && a.autocorrect == b.autocorrect;
}

/* ---- accessibility ------------------------------------------------------------------------------------
 * What a screen reader sees: the elements that mean something (buttons, fields, headings, text...),
 * with plain containers left out and their contents lifted to their parent. The window offers it to
 * the system (UI Automation, NSAccessibility); another host can walk it with these calls. */
typedef enum CscdRole {
    CSCD_ROLE_NONE, CSCD_ROLE_DOCUMENT, CSCD_ROLE_GROUP, CSCD_ROLE_TEXT, CSCD_ROLE_BUTTON, CSCD_ROLE_LINK, CSCD_ROLE_CHECKBOX,
    CSCD_ROLE_RADIO, CSCD_ROLE_TEXTBOX, CSCD_ROLE_COMBOBOX, CSCD_ROLE_LISTBOX, CSCD_ROLE_OPTION, CSCD_ROLE_SLIDER,
    CSCD_ROLE_PROGRESS, CSCD_ROLE_HEADING, CSCD_ROLE_LIST, CSCD_ROLE_LIST_ITEM, CSCD_ROLE_IMAGE, CSCD_ROLE_DIALOG, CSCD_ROLE_TAB,
    CSCD_ROLE_TAB_LIST, CSCD_ROLE_TAB_PANEL, CSCD_ROLE_MENU, CSCD_ROLE_MENU_BAR, CSCD_ROLE_MENU_ITEM, CSCD_ROLE_TABLE,
    CSCD_ROLE_ROW, CSCD_ROLE_CELL, CSCD_ROLE_SEPARATOR, CSCD_ROLE_TOOLTIP
} CscdRole;
enum {
    CSCD_A11Y_FOCUSABLE = 1, CSCD_A11Y_FOCUSED = 2, CSCD_A11Y_CHECKED = 4, CSCD_A11Y_MIXED = 8, CSCD_A11Y_DISABLED = 16,
    CSCD_A11Y_EXPANDED = 32, CSCD_A11Y_COLLAPSED = 64, CSCD_A11Y_SELECTED = 128, CSCD_A11Y_READONLY = 256, CSCD_A11Y_REQUIRED = 512,
    CSCD_A11Y_MULTILINE = 1024, CSCD_A11Y_PASSWORD = 2048
};
CscdRole cscdAccessibleRole(const CscdElement* e); /* from role="..." or the tag; NONE: left out */
void cscdAccessibleName(const CscdView* v, const CscdElement* e, CscdStr* out);        /* appended */
void cscdAccessibleDescription(const CscdView* v, const CscdElement* e, CscdStr* out); /* appended */
void cscdAccessibleValue(const CscdView* v, const CscdElement* e, CscdStr* out);       /* a field's text, a slider's number */
uint32_t cscdAccessibleState(const CscdView* v, const CscdElement* e);                 /* CSCD_A11Y_ bits */
int cscdAccessibleLevel(const CscdElement* e);                                         /* a heading's (0: none) */
bool cscdAccessibleRange(const CscdElement* e, double* value, double* min, double* max, double* step); /* sliders, progress */
bool cscdAccessibleIncluded(const CscdElement* e);
CscdElement* cscdAccessibleRoot(const CscdView* v); /* (the body) */
void cscdAccessibleChildren(const CscdView* v, const CscdElement* e, CscdPtrVec* out); /* e NULL: the root's; appended */
CscdElement* cscdAccessibleParent(const CscdView* v, const CscdElement* e);
CscdElement* cscdAccessibleAt(CscdView* v, float x, float y); /* viewport CSS px */
/* A number for an element that stays the same while it lives; the element of a number, NULL once it
 * left the document. */
uint32_t cscdAccessibleId(CscdView* v, const CscdElement* e);
CscdElement* cscdAccessibleElement(const CscdView* v, uint32_t id);
typedef enum CscdAccessibleAction { CSCD_A11Y_ACTIVATE, CSCD_A11Y_FOCUS, CSCD_A11Y_SET_VALUE, CSCD_A11Y_SCROLL_INTO_VIEW } CscdAccessibleAction;
bool cscdAccessibleDo(CscdView* v, CscdElement* e, CscdAccessibleAction action, CscdSv value);
/* What changed, for the system to announce (e NULL with STRUCTURE: anywhere). */
typedef enum CscdAccessibleEvent {
    CSCD_A11Y_EVENT_FOCUS, CSCD_A11Y_EVENT_VALUE, CSCD_A11Y_EVENT_STATE, CSCD_A11Y_EVENT_STRUCTURE
} CscdAccessibleEvent;
typedef void (*CscdAccessibleEventFn)(void* user, CscdElement* e, CscdAccessibleEvent event);
void cscdViewOnAccessibleEvent(CscdView* v, CscdAccessibleEventFn fn, void* user);

/* The inspector (CSCD_FEATURE_INSPECTOR): F12 or Ctrl+Shift+I, or from code. e: the element to
 * show (pinned), NULL closes it. */
void cscdViewInspect(CscdView* v, CscdElement* e);
bool cscdViewInspecting(const CscdView* v);

/* Long lists that make only the rows that show (a hundred thousand items scroll as lightly as
 * twenty). box scrolls (overflow: auto, with a height); each item is a copy of row (a <template>'s
 * first element, or any element), rowHeight CSS pixels tall, and fill(user, copy, index) makes it show
 * item index (cscdElementFill helps: a row's {{name}}s). A row scrolled out of sight is dropped and a
 * fresh copy made for the next item, so handle the rows' events on box (event.target). */
typedef void (*CscdListFillFn)(void* user, CscdElement* row, size_t index);
void cscdViewBindList(CscdView* v, CscdElement* box, CscdElement* row, size_t count, float rowHeight, CscdListFillFn fill,
                      void* user, CscdFreeFn freeUser);
void cscdViewListSetCount(CscdView* v, CscdElement* box, size_t count); /* the items changed: rows filled again */
void cscdViewListRefresh(CscdView* v, CscdElement* box);                /* the rows shown filled again */
void cscdViewUnbindList(CscdView* v, CscdElement* box);                 /* (its rows stay as they are) */
size_t cscdViewListFirstShown(const CscdView* v, const CscdElement* box); /* the first item in sight */

/* Elements with a splitter attribute resize the siblings on either side of them when dragged (along
 * the parent's flex direction); they get "input" while dragged and "change" after. */

/* clipboard hooks (the window provides them) */
void cscdViewSetClipboardHooks(CscdView* v, CscdSetClipboardFn set, CscdGetClipboardFn get, CscdSetClipboardImageFn setImage,
                             CscdGetClipboardFilesFn getFiles, void* user);
/* The clipboard from the program, through those hooks: false (or nothing appended) without them.
 * Files are the ones copied in the file manager; on phones, copies of what was copied (a picture
 * as a PNG file), readable by path. */
bool cscdViewCopyText(CscdView* v, CscdSv text);
bool cscdViewCopyImage(CscdView* v, const CscdImage* image);
void cscdViewClipboardText(CscdView* v, CscdStr* out);    /* appended */
void cscdViewClipboardFiles(CscdView* v, CscdStrVec* out); /* appended */

/* queries */
CscdElement* cscdViewElementAt(CscdView* v, float x, float y);
CscdRect cscdViewElementRect(CscdView* v, CscdElement* e); /* border box relative to the viewport */
float cscdViewScrollX(const CscdView* v);
float cscdViewScrollY(const CscdView* v);
void cscdViewScrollTo(CscdView* v, float x, float y);
void cscdViewScrollIntoView(CscdView* v, CscdElement* e);
float cscdViewContentHeight(const CscdView* v);

/* timers on the UI thread: run when cscdViewUpdate is called after the delay. The action is taken
 * (its user data freed when the timer is cleared, or after a timeout has run). */
int cscdViewSetTimeout(CscdView* v, CscdAction fn, int ms);
int cscdViewSetInterval(CscdView* v, CscdAction fn, int ms);
void cscdViewClearTimer(CscdView* v, int id);
int cscdViewNextTimerMs(const CscdView* v); /* milliseconds until the next timer or animation frame, -1 if none */

/* custom fonts and images */
bool cscdViewAddFont(CscdView* v, CscdSv family, CscdSv path, int weight, bool italic);
void cscdViewSetImage(CscdView* v, CscdSv url, CscdImage* image); /* provide images from memory (retained; NULL forgets) */

CscdLayoutBox* cscdViewRootBox(CscdView* v);
CscdStyleEngine* cscdViewStyleEngine(CscdView* v);

#ifdef __cplusplus
}
#endif

#endif
