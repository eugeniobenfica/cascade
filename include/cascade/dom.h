/* cascade — the document object model: nodes, elements, text, events. Mirrors the parts of the web
 * DOM an interface needs, with C callbacks (a function, its user data and what frees that data) in
 * place of JavaScript. */
#ifndef CASCADE_DOM_H
#define CASCADE_DOM_H

#include "cascade/core.h"
#include "cascade/image.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CscdNode CscdNode;
typedef struct CscdElement CscdElement;
typedef struct CscdDocument CscdDocument;
typedef struct CscdComputedStyle CscdComputedStyle;
typedef struct CscdLayoutBox CscdLayoutBox;
typedef struct CscdCanvas CscdCanvas;

typedef enum CscdNodeType { CSCD_NODE_DOCUMENT, CSCD_NODE_ELEMENT, CSCD_NODE_TEXT, CSCD_NODE_COMMENT } CscdNodeType;

/* A name and a value: attributes, drag data. */
typedef struct CscdStrPair { CscdStr name, value; } CscdStrPair;
typedef CSCD_VEC(CscdStrPair) CscdStrPairVec;
void cscdStrPairVecFree(CscdStrPairVec* v);
void cscdStrPairVecCopy(CscdStrPairVec* dst, const CscdStrPairVec* src);

/* What a drag carries (the web's DataTransfer): typed strings and, from the system (files dragged
 * in from the file manager), file paths. Shared: cscdRetain / cscdRelease. */
typedef struct CscdDataTransfer {
    CscdStrPairVec items; /* type -> data ("text/plain", "application/x-mine", ...) */
    CscdStrVec files;     /* UTF-8 paths */
    CscdStr effectAllowed; /* "all" */
    CscdStr dropEffect;    /* "copy", "move", "link" or "none" */
    CscdImage* dragImage;  /* set in dragstart to replace the snapshot (shared) */
    float imageX, imageY; /* where the pointer holds the image */
} CscdDataTransfer;

CscdDataTransfer* cscdDataTransferNew(void);
void cscdDataTransferSetData(CscdDataTransfer* d, CscdSv type, CscdSv data);
CscdSv cscdDataTransferGetData(const CscdDataTransfer* d, CscdSv type); /* "" when it has none */
bool cscdDataTransferHasType(const CscdDataTransfer* d, CscdSv type);
void cscdDataTransferClearData(CscdDataTransfer* d);
void cscdDataTransferSetDragImage(CscdDataTransfer* d, CscdImage* image, float x, float y); /* retains image */

typedef struct CscdTouchPoint {
    int id;
    float x, y; /* CSS pixels, relative to the viewport */
} CscdTouchPoint;
typedef CSCD_VEC(CscdTouchPoint) CscdTouchVec;

typedef struct CscdEvent {
    CscdStr type;                 /* "click", "mousedown", "input", "change", "keydown", ... */
    CscdElement* target;
    CscdElement* currentTarget;
    float x, y;                 /* CSS pixels, relative to the viewport */
    float offsetX, offsetY;     /* relative to currentTarget's border box */
    float deltaX, deltaY;       /* wheel */
    int button;                 /* 0 left, 1 middle, 2 right */
    int buttons;
    int detail;                 /* click, dblclick, mousedown: how many clicks in a row */
    CscdStr key;                  /* "a", "Enter", "Backspace", "ArrowLeft", ... */
    CscdStr pointerType;          /* "mouse", "touch" or "pen" */
    CscdDataTransfer* dataTransfer; /* drag and drop events (shared) */
    CscdTouchVec touches;         /* touch events: the fingers down */
    CscdTouchVec changedTouches;  /* touch events: the fingers this event is about */
    bool ctrl, shift, alt, meta;
    bool propagationStopped;
    bool defaultPrevented;
} CscdEvent;

/* An event of that type, pointerType "mouse"; cscdEventFini frees what it holds. */
void cscdEventInit(CscdEvent* ev, CscdSv type);
void cscdEventFini(CscdEvent* ev);
CSCD_INLINE void cscdEventStopPropagation(CscdEvent* ev) { ev->propagationStopped = true; }
CSCD_INLINE void cscdEventPreventDefault(CscdEvent* ev) { ev->defaultPrevented = true; }

/* Callbacks: a function, the user data it is given, and what frees that data when the callback is
 * dropped (NULL: nothing to free). */
typedef void (*CscdEventFn)(void* user, CscdEvent* ev);
typedef struct CscdEventHandler { CscdEventFn fn; void* user; CscdFreeFn freeUser; } CscdEventHandler;
/* Paint callback for <canvas> elements (and any element with a painter): draws in the element's
 * content box, origin at its top-left, in CSS pixels. */
typedef void (*CscdPaintFn)(void* user, CscdCanvas* canvas, float width, float height);
typedef struct CscdPainter { CscdPaintFn fn; void* user; CscdFreeFn freeUser; } CscdPainter;
/* Colors a span of a text control's value (syntax highlighting). */
typedef struct CscdTextSpan {
    size_t start, end; /* byte offsets in the value */
    CscdColor color;
    bool bold;
} CscdTextSpan;
typedef CSCD_VEC(CscdTextSpan) CscdTextSpanVec;
typedef void (*CscdHighlightFn)(void* user, CscdSv text, CscdTextSpanVec* spans);
typedef struct CscdHighlighter { CscdHighlightFn fn; void* user; CscdFreeFn freeUser; } CscdHighlighter;
/* A value callback: the element's value as text, or whether it is checked. */
typedef void (*CscdValueFn)(void* user, CscdSv value);
typedef void (*CscdCheckedFn)(void* user, bool checked);

/* ---- nodes -------------------------------------------------------------------------------------- */

struct CscdNode {
    CscdNodeType type;
    CscdNode* parent;
    CscdPtrVec children; /* CscdNode*, owned */
    CscdDocument* owner;
    size_t indexHint; /* where it was last seen among its parent's children (checked before it is used) */
};

typedef struct CscdText { CscdNode node; CscdStr data; } CscdText;
typedef struct CscdComment { CscdNode node; CscdStr data; } CscdComment;

CSCD_INLINE bool cscdNodeIsElement(const CscdNode* n) { return n && n->type == CSCD_NODE_ELEMENT; }
CSCD_INLINE bool cscdNodeIsText(const CscdNode* n) { return n && n->type == CSCD_NODE_TEXT; }
CSCD_INLINE CscdElement* cscdNodeAsElement(CscdNode* n) { return cscdNodeIsElement(n) ? (CscdElement*)n : NULL; }
CSCD_INLINE const CscdElement* cscdNodeAsElementC(const CscdNode* n) { return cscdNodeIsElement(n) ? (const CscdElement*)n : NULL; }
CSCD_INLINE CscdText* cscdNodeAsText(CscdNode* n) { return cscdNodeIsText(n) ? (CscdText*)n : NULL; }
CSCD_INLINE CscdNode* cscdNodeChild(const CscdNode* n, size_t i) { return (CscdNode*)n->children.items[i]; }
#define CSCD_NODE(x) (&(x)->node)

CscdText* cscdTextNew(CscdSv data);
CscdComment* cscdCommentNew(CscdSv data);
CscdElement* cscdNodeParentElement(const CscdNode* n);
/* Takes child (out of its old parent first); returns it. */
CscdNode* cscdNodeAppendChild(CscdNode* parent, CscdNode* child);
CscdNode* cscdNodeInsertBefore(CscdNode* parent, CscdNode* child, CscdNode* ref);
/* Takes child out; the caller owns it then (cscdNodeDestroy). NULL when it is not a child. */
CscdNode* cscdNodeRemoveChild(CscdNode* parent, CscdNode* child);
void cscdNodeClearChildren(CscdNode* n); /* (they are destroyed) */
int cscdNodeIndexInParent(const CscdNode* n);
CscdNode* cscdNodePreviousSibling(const CscdNode* n);
CscdNode* cscdNodeNextSibling(const CscdNode* n);
CscdElement* cscdNodePreviousElementSibling(const CscdNode* n);
CscdElement* cscdNodeNextElementSibling(const CscdNode* n);
CscdElement* cscdNodeFirstElementChild(const CscdNode* n);
void cscdNodeTextContent(const CscdNode* n, CscdStr* out); /* appended */
void cscdNodeMarkDirty(CscdNode* n); /* asks the owning document to restyle and relayout */
/* Frees a node that has no parent, with everything under it. While an event is on its way through
 * the document, freeing waits until it is done (a handler may remove its own element). */
void cscdNodeDestroy(CscdNode* n);

/* ---- elements ------------------------------------------------------------------------------------ */

/* Interaction state matched by :hover, :active, :focus, :checked, :disabled. */
enum {
    CSCD_STATE_HOVER = 1, CSCD_STATE_ACTIVE = 2, CSCD_STATE_FOCUS = 4, CSCD_STATE_CHECKED = 8,
    CSCD_STATE_DISABLED = 16, CSCD_STATE_FOCUS_VISIBLE = 32
};

typedef struct CscdListener { CscdStr type; CscdEventHandler handler; bool capture; } CscdListener;
typedef CSCD_VEC(CscdListener) CscdListenerVec;

struct CscdElement {
    CscdNode node;
    CscdStr tag; /* lowercase */
    CscdStrPairVec attributes;
    uint32_t state;
    /* geometry after layout, in CSS pixels */
    float scrollLeft, scrollTop;
    /* engine internals */
    CscdListenerVec listeners;
    CscdComputedStyle* style; /* computed style after the cascade (shared) */
    CscdLayoutBox* layout;    /* principal box, or NULL (display:none) */
    CscdStr editValue;        /* current value of text inputs */
    size_t caret;           /* caret position in editValue (bytes) */
    size_t selAnchor;       /* selection anchor in editValue (bytes) */
    bool valueDirty;
    void* userData;
    /* (private) */
    CscdStrVec classes_;
    bool classesValid_;
    CscdPainter painter_;
    CscdHighlighter highlighter_;
    CscdTextSpanVec spans_;
    CscdStr highlightFor_;
    CscdElement* templateContent_; /* a <template>'s content, out of the document */
};

/* A detached element (lowercase tag); it joins a document when appended to one of its nodes. */
CscdElement* cscdElementNew(CscdSv tag);

/* attributes */
const CscdStr* cscdElementGetAttribute(const CscdElement* e, CscdSv name);
CscdSv cscdElementAttr(const CscdElement* e, CscdSv name); /* "" when it has none */
CSCD_INLINE CscdSv cscdElementAttrC(const CscdElement* e, const char* name) { return cscdElementAttr(e, cscdSv(name)); }
CSCD_INLINE bool cscdElementHasAttribute(const CscdElement* e, CscdSv name) { return cscdElementGetAttribute(e, name) != NULL; }
CSCD_INLINE bool cscdElementHasAttributeC(const CscdElement* e, const char* name) { return cscdElementGetAttribute(e, cscdSv(name)) != NULL; }
void cscdElementSetAttribute(CscdElement* e, CscdSv name, CscdSv value);
void cscdElementRemoveAttribute(CscdElement* e, CscdSv name);
CSCD_INLINE CscdSv cscdElementId(const CscdElement* e) { return cscdElementAttr(e, CSCD_SV("id")); }
CSCD_INLINE bool cscdElementIs(const CscdElement* e, const char* tag) { return cscdStrEqC(&e->tag, tag); }

/* classes */
const CscdStrVec* cscdElementClassList(const CscdElement* e);
bool cscdElementHasClass(const CscdElement* e, CscdSv c);
void cscdElementAddClass(CscdElement* e, CscdSv c);
void cscdElementRemoveClass(CscdElement* e, CscdSv c);
bool cscdElementToggleClass(CscdElement* e, CscdSv c); /* returns whether it is now present */
CSCD_INLINE void cscdElementSetClass(CscdElement* e, CscdSv c, bool on) { if (on) cscdElementAddClass(e, c); else cscdElementRemoveClass(e, c); }

/* inline style, as the style attribute: cscdElementSetStyle(e, "width", "20px") */
void cscdElementSetStyle(CscdElement* e, CscdSv property, CscdSv value);
CscdStr cscdElementGetStyle(const CscdElement* e, CscdSv property); /* the caller's */

/* content */
void cscdElementSetText(CscdElement* e, CscdSv text); /* textContent = text */
void cscdElementSetInnerHTML(CscdElement* e, CscdSv html);
CscdStr cscdElementInnerHTML(const CscdElement* e); /* the caller's */

/* A copy of e (detached, the caller's): its tag and attributes, and with deep its children too.
 * Listeners, painters, state and values typed in are not copied. */
CscdElement* cscdElementClone(const CscdElement* e, bool deep);
/* Templates: a <template>'s content is kept out of the document (not styled, laid out or found by
 * queries). Content gives it as an element holding its nodes (the template's, do not free it);
 * Instantiate gives a copy of its first element (the caller's: append it somewhere). */
CscdElement* cscdTemplateContent(CscdElement* tpl);
CscdElement* cscdTemplateInstantiate(CscdElement* tpl);
/* Replaces {{key}} with value in the text and the attribute values of e and everything under it. */
void cscdElementFill(CscdElement* e, CscdSv key, CscdSv value);
CscdStr cscdElementOuterHTML(const CscdElement* e);
CscdElement* cscdElementAppendElement(CscdElement* e, CscdSv tag); /* creates and appends */
CscdText* cscdElementAppendText(CscdElement* e, CscdSv text);
CscdElement* cscdElementAppendHTML(CscdElement* e, CscdSv html); /* parses and appends; returns the first element made */
void cscdElementRemove(CscdElement* e); /* takes it out of the document (and destroys it) */

/* form values (input, textarea, select) */
CscdStr cscdElementValue(const CscdElement* e); /* the caller's */
void cscdElementSetValue(CscdElement* e, CscdSv v);
CSCD_INLINE bool cscdElementChecked(const CscdElement* e) { return (e->state & CSCD_STATE_CHECKED) != 0; }
void cscdElementSetChecked(CscdElement* e, bool c);
CSCD_INLINE bool cscdElementDisabled(const CscdElement* e) { return cscdElementHasAttributeC(e, "disabled"); }

/* queries */
CscdElement* cscdElementQuerySelector(CscdElement* e, CscdSv selector);
void cscdElementQuerySelectorAll(CscdElement* e, CscdSv selector, CscdPtrVec* out); /* CscdElement*, appended */
CscdElement* cscdElementGetElementById(CscdElement* e, CscdSv id);
bool cscdElementMatches(const CscdElement* e, CscdSv selector);
CscdElement* cscdElementClosest(CscdElement* e, CscdSv selector);

/* events */
void cscdElementAddEventListener(CscdElement* e, CscdSv type, CscdEventHandler h, bool capture);
CSCD_INLINE void cscdElementOn(CscdElement* e, CscdSv type, CscdEventFn fn, void* user) {
    CscdEventHandler h = {fn, user, NULL};
    cscdElementAddEventListener(e, type, h, false);
}
CSCD_INLINE void cscdElementOnClick(CscdElement* e, CscdEventFn fn, void* user) { cscdElementOn(e, CSCD_SV("click"), fn, user); }
/* The value as the user changes it ("input": every keystroke or drag), or once it is committed
 * ("change": Enter, blur, a choice in a <select>). */
void cscdElementOnInput(CscdElement* e, CscdValueFn fn, void* user, CscdFreeFn freeUser);
void cscdElementOnChange(CscdElement* e, CscdValueFn fn, void* user, CscdFreeFn freeUser);
void cscdElementOnChecked(CscdElement* e, CscdCheckedFn fn, void* user, CscdFreeFn freeUser); /* a checkbox or radio button changed */
void cscdElementRemoveEventListeners(CscdElement* e, CscdSv type);
/* A click as the user's: listeners, then what it does (checks a box, opens a <details>...). */
void cscdElementClick(CscdElement* e);
/* Calls the listeners of ev's type on e, as dispatch does at e (capture: the capturing ones). */
void cscdElementCallListeners(CscdElement* e, CscdEvent* ev, bool capture);

/* custom painting (the replacement for a QWidget::paintEvent / Skia canvas) */
void cscdElementSetPainter(CscdElement* e, CscdPainter p);
CSCD_INLINE const CscdPainter* cscdElementPainter(const CscdElement* e) { return &e->painter_; }
/* Syntax highlighting for a <textarea> or text <input>: called when the value changes. */
void cscdElementSetHighlighter(CscdElement* e, CscdHighlighter h);
const CscdTextSpanVec* cscdElementHighlights(CscdElement* e, CscdSv text);
CSCD_INLINE bool cscdElementHasHighlighter(const CscdElement* e) { return e->highlighter_.fn != NULL; }

/* geometry after layout, in CSS pixels relative to the viewport */
CscdRect cscdElementBoundingClientRect(const CscdElement* e);
float cscdElementScrollWidth(const CscdElement* e);
float cscdElementScrollHeight(const CscdElement* e);

/* ---- documents ----------------------------------------------------------------------------------- */

typedef void (*CscdNodeFn)(void* user, CscdNode* n);
typedef void (*CscdElementFn)(void* user, CscdElement* e);

struct CscdDocument {
    CscdNode node;
    CscdStr baseDir;   /* where relative URLs (stylesheets, images, fonts) resolve */
    uint64_t version; /* bumped by every change that needs a restyle */
    /* Since the last restyle: only the text of these elements changed (setText), so styles stay as
     * they are; any other change sets structural. */
    CscdPtrVec textChanged; /* CscdElement* */
    /* Only an attribute that these elements' painting reads changed (a progress bar's or a meter's
     * value or max): unless a selector names it, they are just painted again. */
    CscdPtrVec paintChanged; /* CscdElement* */
    bool structural;
    CscdElement* focused;
    CscdElement* hovered;
    CscdElement* active;
    /* Called when nodes are removed so pointers held by the view stay valid. */
    CscdNodeFn onNodeRemoved;
    void* onNodeRemovedUser;
    /* cscdElementClick goes through here when a view shows the document (its default actions). */
    CscdElementFn onClick;
    void* onClickUser;
    /* (events on their way: nodes freed meanwhile wait in the graveyard; a document replaced
     * meanwhile, retired, goes when they are done) */
    int dispatchDepth;
    CscdPtrVec graveyard;
    bool retired;
};

CscdDocument* cscdDocumentNew(void);
/* Frees the document and its nodes. */
void cscdDocumentFree(CscdDocument* d);
CscdElement* cscdDocumentElement(const CscdDocument* d);
CscdElement* cscdDocumentBody(const CscdDocument* d);
CscdElement* cscdDocumentHead(const CscdDocument* d);
CscdElement* cscdDocumentGetElementById(CscdDocument* d, CscdSv id);
CscdElement* cscdDocumentQuerySelector(CscdDocument* d, CscdSv selector);
void cscdDocumentQuerySelectorAll(CscdDocument* d, CscdSv selector, CscdPtrVec* out);
CscdElement* cscdDocumentCreateElement(CscdDocument* d, CscdSv tag);
CscdText* cscdDocumentCreateTextNode(CscdDocument* d, CscdSv text);
CscdStr cscdDocumentTitle(const CscdDocument* d); /* the caller's */
/* Events on their way: nodes destroyed until the matching end are freed then. */
void cscdDocumentBeginDispatch(CscdDocument* d);
void cscdDocumentEndDispatch(CscdDocument* d);

/* Builds a document from HTML source (HTML5 tokenizer and a tree builder that handles implied and
 * optional tags the way browsers do). */
CscdDocument* cscdParseHTML(CscdSv html);
/* Parses a fragment and appends its nodes to parent. */
void cscdParseHTMLFragment(CscdSv html, CscdElement* parent);
void cscdDomAdoptTemplates(CscdNode* root); /* (the parser moves template contents out of the tree) */
void cscdDecodeEntities(CscdSv s, CscdStr* out); /* appended */
void cscdEscapeHTML(CscdSv s, bool attribute, CscdStr* out); /* appended */

#ifdef __cplusplus
}
#endif

#endif
