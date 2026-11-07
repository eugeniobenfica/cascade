/* The view's state, shared by view.c, widgets.c and paint.c: open menus, the line layout of text
 * fields, and the painter's entry point. Not installed. */
#ifndef CASCADE_VIEW_INTERNAL_H
#define CASCADE_VIEW_INTERNAL_H

#include "cascade/font.h"
#include "cascade/view.h"

typedef struct OpenMenu {
    CscdElement* box;
    CscdMenu items;
    CscdPtrVec rows;     /* CscdElement*, one per item, separators too */
    int highlighted;
    int parentRow;     /* the row one level up that opened this one */
    CscdElement* owner;  /* the menu bar entry it belongs to */
    bool ownsItems;    /* (a submenu's items belong to the item that opened it) */
} OpenMenu;

typedef struct MenuBarEntry { CscdElement* entry; CscdMenuBarFn fn; void* user; CscdFreeFn freeUser; } MenuBarEntry;
typedef struct SourceFile { CscdStr path; long long stamp; } SourceFile;
/* A list that makes only the rows that show (cscdViewBindList). */
typedef struct VirtualList {
    CscdElement* box;
    CscdElement* proto;  /* the row, copied for each item shown (the list's) */
    CscdElement* spacer; /* as tall as every row */
    size_t count;
    float rowHeight;
    CscdListFillFn fill; void* user; CscdFreeFn freeUser;
    CscdPtrVec rows;     /* the rows shown: items first, first + 1... */
    size_t first;
} VirtualList;
typedef struct SheetMedia { CscdStr query; bool held; } SheetMedia; /* a <style> or <link media> */
typedef struct SmoothScroll { CscdElement* e; float dx, dy; } SmoothScroll;
typedef struct Timer { int id; double due; int interval; CscdAction fn; } Timer;
/* An animated image (GIF, WebP) playing: shown is the image the page holds, its pixels the frame's */
typedef struct PlayingImage { CscdImage* shown; CscdAnimatedImage* anim; int frame, played; double due; } PlayingImage;
typedef struct RunningTransition {
    CscdElement* e;
    CscdStr prop;
    CscdComputedStyle *from, *to; /* references */
    double start, duration;
    CscdTransition t; /* (its property borrowed from prop) */
} RunningTransition;
typedef struct RunningAnimation { CscdElement* e; CscdAnimation a; /* (name copied) */ double start; CscdComputedStyle* base; /* the style without it (owned) */ } RunningAnimation;
typedef struct TouchState { int id; float x, y, startX, startY, lastX, lastY; double startT; CscdElement* target; } TouchState;
typedef enum TouchMode { TOUCH_NONE, TOUCH_PENDING, TOUCH_PAN, TOUCH_MOUSE, TOUCH_PINCH, TOUCH_PAGE, TOUCH_DONE, TOUCH_HANDLE } TouchMode;
typedef struct UndoState { CscdStr value; size_t caret, anchor; } UndoState;
typedef CSCD_VEC(UndoState) UndoStateVec;
typedef struct UndoStack { UndoStateVec undo, redo; double lastT; int lastKind; } UndoStack;

/* One hit-testable painted region, recorded while painting. */
typedef struct CscdHitItem {
    CscdElement* element;
    CscdRect rect;  /* local CSS px */
    CscdMatrix m;   /* local -> device */
    CscdRect clip;  /* device */
    bool text;
    CscdLayoutBox* box;
    int frag;
    /* Under a perspective transform, m maps to an offscreen image and h (a 3x3 homography, by rows)
     * maps that image to the device. */
    bool projective;
    float h[9];
} CscdHitItem;
typedef CSCD_VEC(CscdHitItem) CscdHitVec;

struct CscdView {
    /* callbacks */
    CscdWarningFn onWarning; void* onWarningUser;
    CscdNodeFn onNodeRemoved; void* onNodeRemovedUser;
    CscdActionFn onDocumentReplaced; void* onDocumentReplacedUser;
    CscdActionFn onInvalidate; void* onInvalidateUser;
    CscdMenuItemFn onMenuItem; void* onMenuItemUser;
    CscdSetClipboardFn setClipboard; CscdGetClipboardFn getClipboard;
    CscdSetClipboardImageFn setClipboardImage; CscdGetClipboardFilesFn getClipboardFiles;
    void* clipboardUser;
    int tooltipDelayMs;

    /* Pointers nulled when their element leaves the document: a listener may remove elements
     * (rebuild a panel) while an event is on its way through them. */
    CSCD_VEC(CscdElement**) watched;
    CscdPtrVec missing; /* what cscdViewGet returned when nothing matched (CscdElement*, owned) */
    CSCD_VEC(SourceFile) sources; /* the sheet files in use, with their modification times */
    CSCD_VEC(VirtualList*) lists;
    bool inspecting, inspectPinned;
    CscdU64Map a11yIds, a11yElements; /* element <-> number (cscdAccessibleId) */
    uint64_t a11yNext;
    CscdAccessibleEventFn onA11y; void* onA11yUser;
    CscdElement* inspected;
    CSCD_VEC(SheetMedia) sheetMedia; /* the media attributes of the document's sheets, as last evaluated */
    CscdFrameStats stats;
    CscdIRectVec paintDamage; /* (of the last paint, device px) */
    bool paintDamageAll;
    uint32_t features; /* bits: CscdFeature */
    CscdStrMap unknownProperties;    /* the properties of the sheets the engine does not know (warned once) */
    CscdStrVec authorSheetFiles;  /* the file of each added sheet ("" for text) */
    int reloadTimer;

    CscdDocument* doc;
    CscdStyleEngine* styles;
    CscdLayoutBox* root;
    CscdPtrVec authorSheets; /* CscdStyleSheet*, references */
    void* baseSheet;         /* a CscdStyleSheet*, under the document's; or NULL */
    size_t tabPlace;         /* where Tab went last, among what takes focus, from 1; 0: nowhere yet */
    CscdStrMap images;       /* url -> CscdImage* (a reference, or NULL) */
    CSCD_VEC(PlayingImage) playing;
    CscdPtrVec bindings;     /* bind.c's Binding*, owned */
    float vw, vh, dpr;
    bool dark;
    bool darkByApp;          /* cscdViewSetDarkMode was called: the system's theme no longer changes it */
    bool systemDark;         /* the system's theme is dark (prefers-color-scheme; the canvas when the page allows it) */
    float textScale;         /* the system's text size (cscdViewSetSystemPreferences) */
    uint32_t featuresByApp;  /* bits: the features cscdViewSetFeature set (the system's preferences leave them) */
    uint64_t styledVersion;
    bool layoutDirty, styleDirty, dirty;
    bool fullPaint;          /* something other than invalidate(element) asked for a repaint */
    CscdRectVec damage;        /* the areas invalidate(element) asked for, in viewport px */
    bool transparent;        /* cscdViewSetTransparent: no canvas under the page */
    bool incrementalValid;   /* the last paint was into the incremental target */
    bool recordHits;         /* false while painting part of the page (the hits stay) */
    CscdPtrVec stateChanged;   /* elements whose :hover / :active changed since the last restyle */
    bool hitsStale;          /* the layout changed under the hit areas (a scroll) */
    CSCD_VEC(SmoothScroll) smooth; /* wheel scrolls still on their way */
    double smoothTime;
    float scrollX, scrollY;
    CscdCursor cursor;
    CscdHitVec hits;
    CscdU64Map hitsGone;    /* the elements that left the document since the hits were recorded */
    float mouseX, mouseY;
    int buttons;
    CscdElement* pressed;
    CscdElement* dragRange;
    CscdElement* scrollDrag;
    float scrollDragStart, scrollDragOrigin;
    bool scrollDragX;
    bool selecting;
    double caretTime;
    double lastClick;
    int downClicks;
    CSCD_VEC(Timer) timers;
    int nextTimer;
    CSCD_VEC(RunningTransition) transitions;
    CSCD_VEC(RunningAnimation) animations;
    double epoch;
    bool caretShown; /* (the caret blink state last painted) */

    /* menus */
    CscdPtrVec menus; /* OpenMenu*, owned */
    CSCD_VEC(MenuBarEntry) menuBar;
    CscdElement* menuBarOpen;
    bool menuSwallowUp;
    /* tooltips */
    CscdElement* tipFor;
    CscdElement* tip;
    int tipTimer;
    /* drag and drop */
    CscdElement* dragSource;
    CscdElement* dragOver;
    float dragStartX, dragStartY;
    bool dragActive, dragExternal, dropAllowed;
    CscdDataTransfer* dragData;
    CscdImage* dragGhost;
    float ghostX, ghostY, ghostScale;
    /* touch */
    CSCD_VEC(TouchState) touches;
    TouchMode touchMode;
    CscdElement* touchScroll;
    bool touchScrollDoc;
    float touchVX, touchVY, pinchDist, pinchX, pinchY;
    float panAccX, panAccY; /* moved since the velocity was last measured */
    double momentumT;       /* the glide's last step */
    double inputAge;        /* how long ago the touch being handled happened (viewSetInputAge) */
    double touchLastT, lastTap;
    int longPressTimer, momentumTimer, tapClicks;
    bool touchWheel; /* the wheel events two fingers make say "touch" */
    /* touch selection: the handles of a field's selection made by touch, and the edit menu */
    CscdElement* handlesFor;  /* the field they show in (NULL: none) */
    int handleDragged;        /* 0 none, 1 the selection's start, 2 its end, 3 the caret's */
    float handleGrabX, handleGrabY; /* where the finger holds the handle, from the caret's foot */
    int handleTimer;           /* hides a lone caret handle after a while */
    bool handleMoved, handleMenuBefore; /* (the finger on a handle has dragged it; the menu showed when it came) */
    bool editMenuShown;
    CscdRect editMenuRect;    /* as last told */
    unsigned editMenuActions;
    CscdEditMenuFn onEditMenu; void* onEditMenuUser;
    CscdMagnifierFn onMagnifier; void* onMagnifierUser;
    /* splitters */
    CscdElement* splitter;
    CscdElement* splitPrev;
    CscdElement* splitNext;
    bool splitRow;
    float splitStart, splitPrevSize, splitNextSize;
    /* popovers open, the last on top */
    CscdPtrVec popovers;
    /* dialogs: the modal ones open, the focus each took */
    CscdPtrVec modals;
    CscdPtrVec modalReturnFocus;
    /* editing */
    CscdStr composition;
    size_t markStart, markEnd; /* the focused field's text an input method is still composing (cscdViewSetTextState) */
    CscdU64Map undo; /* CscdElement* -> UndoStack* */
};

/* ---- what the view's files share (each group is defined in the file it names) ---- */

/* view.c */
double viewNow(const CscdView* v);
CSCD_INLINE CscdElement* parentEl(const CscdElement* e) { return cscdNodeParentElement(&e->node); }
void viewWarn(CscdView* v, CscdSv message);
void viewWarnf(CscdView* v, CscdFeature kind, const char* fmt, ...); /* (kind: a CSCD_FEATURE_WARN_) */
CSCD_INLINE bool viewOn(const CscdView* v, CscdFeature f) { return (v->features >> f) & 1u; }
bool viewDark(const CscdView* v); /* the canvas and the view's own parts dark (the app's choice, or the system's when the page's color-scheme has dark) */
bool cscdViewCaretVisible(const CscdView* v); /* (the caret's blink) */
void viewWatch(CscdView* v, CscdElement** e);
size_t viewWatchMark(const CscdView* v);
void viewUnwatch(CscdView* v, size_t mark);
void viewClearHits(CscdView* v);
void viewNotifyInvalidate(CscdView* v);
CscdRect viewPaintArea(CscdView* v, CscdElement* e);
void viewAddDamage(CscdView* v, CscdRect r);
void viewUnhookDocument(CscdView* v);
void viewHookDocument(CscdView* v);
void viewClearUndo(CscdView* v);
void viewClearTransitions(CscdView* v);
void viewClearSources(CscdView* v);
void viewClearMenus(CscdView* v);
void viewClearAnimations(CscdView* v);

/* viewLoad.c */
CscdImage* viewImage(CscdView* v, CscdSv url); /* borrowed (the view keeps it) */
CscdImage* viewImageLoader(void* view, CscdSv url); /* a new reference (layout and style) */
CscdMediaContext viewMediaOf(const CscdView* v);
void viewCollectSheets(CscdView* v);
bool viewAdvanceImages(CscdView* v, double t);
void viewSetLiveReload(CscdView* v, bool on);

/* viewAnimate.c */
void viewTickAnimations(CscdView* v, double t);
void viewStartTransitions(CscdView* v, CscdElement* e, const CscdComputedStyle* before, const CscdComputedStyle* after);
void viewRemoveAnimation(CscdView* v, size_t i);

/* viewRestyle.c */
void viewRestyleAll(CscdView* v);
void viewRelayout(CscdView* v);
void viewLayoutNow(CscdView* v);
void viewRecordHits(CscdView* v);
void viewCenterModals(CscdView* v);
void viewRestyleStates(CscdView* v);
bool viewRestyle(CscdView* v, CscdPtrVec* roots);
bool viewRepaintAttributes(CscdView* v);
bool viewRelayoutText(CscdView* v);
bool viewContainsPtr(const CscdPtrVec* v, const void* p);

/* viewScroll.c */
void viewScrollBy(CscdView* v, CscdElement* e, float dx, float dy);
void viewScrolled(CscdView* v, CscdElement* e);
void viewAdvanceScrolls(CscdView* v, double t);

/* viewEvents.c */
void viewDispatch(CscdView* v, CscdEvent* ev, CscdElement* target);
void viewDefaultAction(CscdView* v, CscdEvent* ev);
void viewSetHover(CscdView* v, CscdElement* e);
void viewSetActive(CscdView* v, CscdElement* e);
void viewSubmitForm(CscdView* v, CscdElement* submitter);
void viewSendSimple(CscdView* v, const char* type, CscdElement* target);
void viewInputAndChange(CscdView* v, CscdElement* e);
float viewAttrNumber(const CscdElement* e, const char* name, float def);

/* viewKeys.c */
void viewEditInput(CscdView* v, CscdElement* e, CscdSv insert, int deleteDir, bool word);
bool viewContextKey(CscdView* v);

/* widgets.c */
/* the focused text field, if it shows; where offset pos of a field's text shows, as a caret
 * there would (relative to the viewport), and on which of a textarea's lines */
CscdElement* viewFocusedField(CscdView* v);
CscdRect viewFieldPosRect(CscdView* v, CscdElement* e, size_t pos, size_t* line);
bool viewUpdateLists(CscdView* v); /* the rows that show now made (true when some changed) */
void viewFreeLists(CscdView* v);
void viewPushUndo(CscdView* v, CscdElement* e, int kind);
bool viewUndoEdit(CscdView* v, CscdElement* e, bool redo);
void viewEnsureCaretVisible(CscdView* v, CscdElement* e);
size_t viewTextHit(CscdView* v, CscdElement* f, float x, float y);
void viewTextAreaRange(CscdView* v, CscdElement* e, float* maxX, float* maxY);
void viewOpenMenu(CscdView* v, CscdMenu items, CscdRect anchor, bool beside, float minWidth, int parentRow, CscdElement* owner);
void viewOpenMenuBar(CscdView* v, CscdElement* entry);
void viewNodeRemoved(CscdView* v, CscdNode* n);
bool viewBlockedByModal(const CscdView* v, CscdElement* target);
void viewPlaceModal(CscdView* v, CscdElement* dialog);
void viewLightDismiss(CscdView* v, CscdElement* hit);
void viewCloseMenusFrom(CscdView* v, size_t level);
bool viewMenuMouse(CscdView* v, int kind, float x, float y, int button);
bool viewMenuKey(CscdView* v, CscdKey k, CscdSv keyName);
void viewHighlightMenuRow(CscdView* v, size_t level, int row, bool openSub);
void viewActivateMenuRow(CscdView* v, size_t level, int row);
void viewTooltipMove(CscdView* v, CscdElement* hit);
void viewBeginDrag(CscdView* v, float x, float y);
void viewDragMoveTo(CscdView* v, float x, float y);
void viewEndDrag(CscdView* v, bool drop);
bool viewSplitterDown(CscdView* v, CscdElement* hit, float x, float y);
void viewSplitterMove(CscdView* v, float x, float y);
void viewSplitterUp(CscdView* v);
void openMenuFree(OpenMenu* m);

/* touch.c */
/* The touches that follow happened this long ago (seconds; 0: now): the platform's own time for moves the
 * system delivered together or the program took late, so that a fling's speed is the finger's. */
void viewSetInputAge(CscdView* v, double seconds);
void viewShowHandles(CscdView* v, CscdElement* field, bool menu); /* (the focused field's selection, or its caret) */
void viewHandlesCheck(CscdView* v);                /* each update: gone with the focus, the menu follows the selection */
void viewPaintHandles(CscdView* v, CscdCanvas* c); /* (over the page) */

/* bind.c */
/* the models bound to the page (shown when they changed; set from fields typed in) */
void viewApplyBindings(CscdView* v);
void viewBindingsFromPage(CscdView* v, CscdElement* e);
void viewBindingsNodeRemoved(CscdView* v, CscdNode* n);
void viewBindingsClear(CscdView* v);

/* inspector.c */
bool viewInspectorKey(CscdView* v, CscdKey k, CscdModifiers m, CscdSv keyName); /* (true: the inspector took it) */
bool viewInspectorMouse(CscdView* v, int kind, float x, float y);                    /* kind: 0 move, 1 press, 2 release */
void viewInspectorPaint(CscdView* v, CscdCanvas* c);                                 /* (over the page) */

/* a11y.c */
void viewA11yNotify(CscdView* v, CscdSv type, CscdElement* target); /* (an event was sent) */
void viewA11yStructure(CscdView* v);                                    /* (the document changed) */
void viewA11yForget(CscdView* v, const CscdElement* e);
void viewA11yFree(CscdView* v);


/* A line of a text field as drawn: [start, end) of the value, without the line break. */
typedef struct EditLine { size_t start, end; } EditLine;
typedef CSCD_VEC(EditLine) EditLineVec;

/* Lines of a value: one per paragraph, wrapped at spaces to width when wrap is on (out replaced). */
void editLines(CscdSv text, const CscdFont* f, float width, bool wrap, float letterSpacing, EditLineVec* out);
size_t editLineOf(const EditLineVec* lines, size_t pos);
/* The position in line closest to x (relative to the line's left edge). */
size_t editHit(CscdSv text, EditLine line, const CscdFont* f, float x, float letterSpacing);
float editLineHeight(const CscdComputedStyle* s, const CscdFont* f);
bool editWraps(const CscdElement* e);
/* A line of a field as it shows: its runs of one direction, left to right, with where each starts
 * (from the line's left edge) and its width. rtl: the field's direction (direction: rtl). */
typedef struct EditRun { size_t start, end; bool rtl; float x, w; } EditRun;
typedef CSCD_VEC(EditRun) EditRunVec;
void editRuns(CscdSv text, EditLine line, const CscdFont* f, float letterSpacing, bool rtl, EditRunVec* out);
float editRunsWidth(const EditRunVec* runs);
/* Where a caret at pos shows; the position nearest x; the spans [x0, x1] that [a, z) shows as
 * (pairs, left to right). */
float editCaretX(const EditRunVec* runs, CscdSv text, const CscdFont* f, float letterSpacing, size_t pos);
size_t editRunsHit(const EditRunVec* runs, CscdSv text, const CscdFont* f, float letterSpacing, float x);
void editRunsSpan(const EditRunVec* runs, CscdSv text, const CscdFont* f, float letterSpacing, size_t a, size_t z, CscdFloatVec* out);
/* How far a single line field moves its text right: text-align (start and end by direction),
 * when the text is narrower than the field. */
float editAlignShift(const CscdComputedStyle* s, float textWidth, float width);

bool isFocusable(const CscdElement* e);
bool isTextControl(const CscdElement* e);
size_t prevChar(CscdSv s, size_t i);
size_t nextChar(CscdSv s, size_t i);
size_t prevWord(CscdSv s, size_t i);
size_t nextWord(CscdSv s, size_t i);

/* paint.c: paints the page's boxes (and records the hit areas when v->recordHits) */
void paintRoot(CscdView* v, CscdCanvas* c, CscdLayoutBox* root);

#endif
