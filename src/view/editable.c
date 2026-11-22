/* contenteditable: the page's own text edited in place. The element with the attribute (not "false") is the editing
 * host when it has the focus; its text is that of the text nodes in it, in document order, and the selection is two
 * places in it counted in bytes of that text taken whole (a node that comes or goes elsewhere leaves nothing
 * dangling). The host keeps its text as typed (white-space: pre-wrap in the UA sheet), so a text box's text is its
 * node's data and its fragments say where each byte of it shows. Between two blocks (paragraphs, list items) the text
 * has a line break of its own that no node holds: deleting it joins them. Enter is a line break in the text (in a list
 * item: a new item); bold, italic, underline and links wrap the selection in <b>, <i>, <u>, <a>; lists and headings
 * make blocks of its lines. */
#include "view/viewInternal.h"
#include "text/graphemeInternal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- the host and its text ------------------------------------------------------------------------------------- */

static bool editableAttr(const CscdElement* e) {
    return e && cscdElementHasAttributeC(e, "contenteditable") && !cscdSvIEqC(cscdElementAttrC(e, "contenteditable"), "false");
}

CscdElement* viewEditableHost(CscdView* v) {
    CscdElement* f = v->doc->focused;
    return editableAttr(f) && !cscdElementDisabled(f) ? f : NULL;
}

bool viewIsEditableHost(const CscdElement* e) { return editableAttr(e); }

static bool inlineTag(const CscdElement* e) {
    static const char* const tags[] = {"b", "i", "u", "s", "em", "strong", "span", "code", "sub", "sup", "mark", "small", "a", "font"};
    for (size_t i = 0; i < CSCD_COUNT(tags); ++i) if (cscdElementIs(e, tags[i])) return true;
    return false;
}

static bool blockTag(const CscdElement* e) {
    static const char* const tags[] = {"p", "div", "li", "ul", "ol", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "pre",
                                       "section", "article", "header", "footer", "nav", "aside", "table", "tr", "td", "th"};
    for (size_t i = 0; i < CSCD_COUNT(tags); ++i) if (cscdElementIs(e, tags[i])) return true;
    return false;
}

/* the block a node is in, within the host (the host itself when there is none) */
static CscdElement* blockOf(CscdElement* host, CscdNode* n) {
    for (CscdElement* e = cscdNodeParentElement(n); e && e != host; e = cscdNodeParentElement(&e->node))
        if (blockTag(e) || (!inlineTag(e) && e->style && e->style->display != CSCD_DISPLAY_INLINE)) return e;
    return host;
}

static bool holds(const CscdNode* a, const CscdNode* n) {
    for (; n; n = n->parent) if (n == a) return true;
    return false;
}

static void textNodesIn(CscdNode* n, CscdPtrVec* out) {
    for (size_t i = 0; i < n->children.len; ++i) {
        CscdNode* c = cscdNodeChild(n, i);
        if (cscdNodeIsText(c)) cscdVecPush(out, c);
        else if (cscdNodeIsElement(c) && !cscdElementIs((CscdElement*)c, "template")) textNodesIn(c, out);
    }
}

/* the host's text taken whole, where each of its nodes starts in it, and where the line breaks between blocks are
 * (in the text, held by no node) */
typedef struct HostText {
    CscdPtrVec nodes; /* CscdText* */
    CscdSizeVec starts;
    CscdSizeVec seps;
    CscdStr all;
} HostText;

static void hostText(CscdElement* host, HostText* t) {
    memset(t, 0, sizeof *t);
    textNodesIn(&host->node, &t->nodes);
    CscdElement* block = NULL;
    for (size_t i = 0; i < t->nodes.len; ++i) {
        CscdText* n = (CscdText*)t->nodes.items[i];
        CscdElement* b = blockOf(host, &n->node);
        if (i && b != block) {
            cscdVecPush(&t->seps, t->all.len);
            cscdStrAppendChar(&t->all, '\n');
        }
        block = b;
        cscdVecPush(&t->starts, t->all.len);
        cscdStrAppend(&t->all, cscdStrSv(&n->data));
    }
}

static bool isSep(const HostText* t, size_t pos) {
    for (size_t i = 0; i < t->seps.len; ++i) if (t->seps.items[i] == pos) return true;
    return false;
}

static void hostTextFree(HostText* t) {
    cscdVecFree(&t->nodes);
    cscdVecFree(&t->starts);
    cscdVecFree(&t->seps);
    cscdStrFree(&t->all);
}

/* the node a place is in, and where in it: at a boundary the node before (where typing goes on with its formatting),
 * unless that one ends a line; -1 without text nodes */
static int nodeAt(const HostText* t, size_t pos, size_t* off) {
    int found = -1;
    for (size_t i = 0; i < t->nodes.len; ++i) {
        const CscdText* n = (const CscdText*)t->nodes.items[i];
        const size_t a = t->starts.items[i], z = a + n->data.len;
        if (pos < a) break;
        if (pos <= z) {
            if (found >= 0 && pos == a) {
                /* (a boundary: the one before, unless it ends a line or is empty) */
                const CscdText* prev = (const CscdText*)t->nodes.items[found];
                if (prev->data.len && prev->data.ptr[prev->data.len - 1] != '\n') break;
            }
            found = (int)i;
            *off = pos - a;
            if (pos < z) break;
        }
    }
    if (found < 0 && t->nodes.len) { /* (past the end: the last) */
        found = (int)t->nodes.len - 1;
        *off = ((const CscdText*)t->nodes.items[found])->data.len;
    }
    return found;
}

/* ---- where text shows -------------------------------------------------------------------------------------------- */

typedef struct Frag { const CscdLayoutBox* box; const CscdTextFrag* frag; size_t base; } Frag; /* base: its node's start */
typedef CSCD_VEC(Frag) FragVec;

static void fragsIn(const CscdLayoutBox* b, const HostText* t, FragVec* out) {
    if (b->kind == CSCD_BOX_TEXT && b->textNode) {
        for (size_t i = 0; i < t->nodes.len; ++i)
            if (t->nodes.items[i] == b->textNode) {
                for (size_t k = 0; k < b->textFrags.len; ++k) {
                    Frag f = {b, &b->textFrags.items[k], t->starts.items[i]};
                    cscdVecPush(out, f);
                }
                break;
            }
    }
    for (size_t i = 0; i < b->children.len; ++i) fragsIn(cscdBoxChild(b, i), t, out);
}

/* from layout coordinates to the view's (the host's own offset: its scroll, the page's) */
static CscdPoint hostShift(CscdView* v, CscdElement* host) {
    const CscdRect r = cscdViewElementRect(v, host);
    return cscdPoint(r.x - (float)host->layout->x, r.y - (float)host->layout->y);
}

static float textWidth(const Frag* f, size_t from, size_t to) {
    const CscdLayoutBox* b = f->box;
    if (to <= from) return 0;
    return cscdFontMeasure(cscdSvSub(cscdStrSv(&b->text), from, to - from), &b->font, b->style ? b->style->letterSpacing : 0, 0);
}

/* x of byte off of the fragment's text (layout coordinates) */
static float fragX(const Frag* f, size_t off) {
    const float w = textWidth(f, f->frag->start, cscdMinz(cscdMaxz(off, f->frag->start), f->frag->end));
    return (float)f->frag->x + (f->frag->rtl ? f->frag->width - w : w);
}

/* the host's content box's left and top, and a line's height, in layout coordinates (where an empty host's caret goes) */
static CscdRect hostStart(const CscdElement* host) {
    const CscdLayoutBox* b = host->layout;
    const CscdFont font = cscdFontForStyle(b->style);
    const float h = cscdFontAscent(&font) + cscdFontDescent(&font);
    return cscdRect((float)(b->x + b->border[3] + b->padding[3]), (float)(b->y + b->border[0] + b->padding[0]), 1, h > 0 ? h : 16);
}

/* where an empty node's caret goes: its block's start (layout coordinates); false when it has no box */
static bool emptyStart(CscdElement* host, CscdNode* n, CscdRect* out) {
    CscdElement* b = blockOf(host, n);
    if (!b->layout || !b->style) return false;
    *out = hostStart(b);
    return true;
}

/* where the caret at pos shows (view coordinates) */
static CscdRect caretAt(CscdView* v, CscdElement* host, const HostText* t, size_t pos) {
    if (!host->layout) return cscdRect(0, 0, 0, 0);
    const CscdPoint d = hostShift(v, host);
    FragVec frags = {0};
    fragsIn(host->layout, t, &frags);
    size_t off = 0;
    const int node = nodeAt(t, pos, &off);
    CscdRect r = hostStart(host);
    bool placed = false;
    const Frag* last = NULL;
    for (size_t i = 0; i < frags.len && node >= 0; ++i) {
        const Frag* f = &frags.items[i];
        if (f->box->textNode != t->nodes.items[node]) continue;
        last = f;
        if (off >= f->frag->start && off <= f->frag->end) {
            r = cscdRect(fragX(f, off), (float)f->frag->top, 1, f->frag->height);
            placed = true;
            if (off < f->frag->end) break; /* (at its end: the next fragment's start, when there is one) */
        }
    }
    if (!placed && last) {
        /* past what shows of its node (after a line break that ends it, or spaces at a wrap): the next line's start */
        r = cscdRect((float)hostStart(host).x, (float)(last->frag->top + last->frag->height), 1, last->frag->height);
        if (off > 0 && off <= ((const CscdText*)t->nodes.items[node])->data.len && ((const CscdText*)t->nodes.items[node])->data.ptr[off - 1] != '\n')
            r = cscdRect(fragX(last, last->frag->end), (float)last->frag->top, 1, last->frag->height);
    } else if (!placed && node >= 0 && !((const CscdText*)t->nodes.items[node])->data.len && emptyStart(host, t->nodes.items[node], &r)) {
        /* (an empty block's) */
    } else if (!placed && node >= 0 && t->all.len) {
        /* (a node with nothing shown: before the next one that shows) */
        for (size_t i = 0; i < frags.len; ++i)
            if (frags.items[i].base > t->starts.items[node]) {
                r = cscdRect(fragX(&frags.items[i], frags.items[i].frag->start), (float)frags.items[i].frag->top, 1, frags.items[i].frag->height);
                break;
            }
    }
    cscdVecFree(&frags);
    return cscdRectTranslated(r, d.x, d.y);
}

/* the place nearest (x, y) (view coordinates) */
static size_t hitAt(CscdView* v, CscdElement* host, const HostText* t, float x, float y) {
    if (!host->layout || !t->nodes.len) return 0;
    const CscdPoint d = hostShift(v, host);
    x -= d.x;
    y -= d.y;
    FragVec frags = {0};
    fragsIn(host->layout, t, &frags);
    /* (an empty block: a line of nothing at its start) */
    CscdTextFrag empties[16];
    CscdLayoutBox emptyBoxes[16];
    int emptyCount = 0;
    for (size_t i = 0; i < t->nodes.len && emptyCount < 16; ++i) {
        CscdRect r;
        if (((const CscdText*)t->nodes.items[i])->data.len || !emptyStart(host, t->nodes.items[i], &r)) continue;
        memset(&empties[emptyCount], 0, sizeof empties[emptyCount]);
        memset(&emptyBoxes[emptyCount], 0, sizeof emptyBoxes[emptyCount]);
        empties[emptyCount].x = r.x;
        empties[emptyCount].top = r.y;
        empties[emptyCount].height = r.h;
        emptyBoxes[emptyCount].textNode = t->nodes.items[i];
        Frag f = {&emptyBoxes[emptyCount], &empties[emptyCount], t->starts.items[i]};
        cscdVecPush(&frags, f);
        ++emptyCount;
    }
    /* the line: the fragments whose height holds y, else the nearest */
    double bestDy = 1e30;
    for (size_t i = 0; i < frags.len; ++i) {
        const CscdTextFrag* f = frags.items[i].frag;
        const double dy = y < f->top ? f->top - y : y > f->top + f->height ? y - (f->top + f->height) : 0;
        if (dy < bestDy) bestDy = dy;
    }
    size_t best = t->all.len;
    double bestDx = 1e30, lineTop = 0;
    bool any = false;
    for (size_t i = 0; i < frags.len; ++i) {
        const Frag* f = &frags.items[i];
        const double dy = y < f->frag->top ? f->frag->top - y : y > f->frag->top + f->frag->height ? y - (f->frag->top + f->frag->height) : 0;
        if (dy > bestDy + 0.01) continue;
        const double left = f->frag->x, right = f->frag->x + f->frag->width;
        const double dx = x < left ? left - x : x > right ? x - right : 0;
        if (dx > bestDx) continue;
        bestDx = dx;
        lineTop = f->frag->top;
        any = true;
        /* the boundary of its characters nearest x */
        if (f->frag->end == f->frag->start) { /* (an empty block's) */
            best = f->base;
            continue;
        }
        const CscdSv s = cscdStrSv(&f->box->text);
        size_t at = f->frag->start;
        double atD = fabs(fragX(f, at) - x);
        for (size_t k = f->frag->start; k < f->frag->end;) {
            const size_t kn = cscdGraphemeNext(s, k);
            const double kd = fabs(fragX(f, kn) - x);
            if (kd < atD) {
                atD = kd;
                at = kn;
            }
            k = kn;
        }
        best = f->base + at;
    }
    /* below the last line, after a line break: the empty line there */
    if (any && t->all.len && t->all.ptr[t->all.len - 1] == '\n') {
        double bottom = lineTop;
        for (size_t i = 0; i < frags.len; ++i) bottom = fmax(bottom, frags.items[i].frag->top + frags.items[i].frag->height);
        if (y > bottom) best = t->all.len;
    }
    cscdVecFree(&frags);
    return best;
}

/* ---- words: letters and digits (and what is not ASCII) together; spaces and line breaks between; punctuation alone ---- */

static int wordClass(char c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 0;
    if (isalnum((unsigned char)c) || (unsigned char)c >= 0x80) return 1;
    return 2;
}
static size_t wordBack(CscdSv s, size_t i) { /* (to the start of the word before i) */
    while (i > 0 && wordClass(s.ptr[i - 1]) == 0) --i;
    if (i > 0 && wordClass(s.ptr[i - 1]) == 2) return i - 1;
    while (i > 0 && wordClass(s.ptr[i - 1]) == 1) --i;
    return i;
}
static size_t wordOn(CscdSv s, size_t i) { /* (to the end of the word at or after i) */
    while (i < s.len && wordClass(s.ptr[i]) == 0) ++i;
    if (i < s.len && wordClass(s.ptr[i]) == 2) return i + 1;
    while (i < s.len && wordClass(s.ptr[i]) == 1) ++i;
    return i;
}

/* ---- the selection --------------------------------------------------------------------------------------------- */

static void selection(const CscdView* v, size_t len, size_t* a, size_t* z) {
    const size_t p = cscdMinz(v->ceAnchor, len), q = cscdMinz(v->ceFocus, len);
    *a = cscdMinz(p, q);
    *z = cscdMaxz(p, q);
}

static void moved(CscdView* v) {
    v->caretTime = viewNow(v);
    /* (the host keeps it too, as a field keeps its own: for screen readers, which ask the element) */
    if (v->ceHost) {
        v->ceHost->selAnchor = v->ceAnchor;
        v->ceHost->caret = v->ceFocus;
    }
    cscdViewInvalidate(v);
}

void viewEditableFocused(CscdView* v, CscdElement* host) {
    if (v->ceHost == host) return;
    v->ceHost = host;
    v->ceComposing = false;
    v->ceHasGoal = false;
    for (size_t i = 0; i < v->ceUndo.len; ++i) cscdStrFree(&v->ceUndo.items[i].html);
    for (size_t i = 0; i < v->ceRedo.len; ++i) cscdStrFree(&v->ceRedo.items[i].html);
    v->ceUndo.len = v->ceRedo.len = 0;
    if (!host) return;
    /* (reached by Tab: at the end of its text) */
    HostText t;
    hostText(host, &t);
    v->ceAnchor = v->ceFocus = t.all.len;
    host->selAnchor = host->caret = t.all.len;
    hostTextFree(&t);
}

/* ---- changes to its text ------------------------------------------------------------------------------------ */

/* a text node's new text, told to the document (only the text: its element's styles stay, when it is the one child) */
static void setData(CscdView* v, CscdText* n, CscdSv data) {
    CscdStr s = cscdStrDup(data); /* (data may be the node's own) */
    CscdElement* parent = cscdNodeParentElement(&n->node);
    const bool onlyText = parent && parent->node.children.len == 1 && n->data.len && s.len && n->node.owner;
    cscdStrFree(&n->data);
    n->data = s;
    if (onlyText) {
        CscdDocument* d = n->node.owner;
        ++d->version;
        cscdNodeTouch(&n->node);
        bool listed = false;
        for (size_t i = 0; i < d->textChanged.len && !listed; ++i) listed = d->textChanged.items[i] == parent;
        if (!listed) cscdVecPush(&d->textChanged, parent);
    } else {
        cscdNodeMarkDirty(&n->node);
    }
    cscdViewInvalidate(v);
}

/* empty text nodes, and inline elements left with nothing in them, out of the host */
static void tidy(CscdNode* n) {
    for (size_t i = n->children.len; i-- > 0;) {
        CscdNode* c = cscdNodeChild(n, i);
        CscdElement* e = cscdNodeAsElement(c);
        if (e) tidy(c);
        /* (an empty text node alone in a block stays: the caret's place there) */
        const bool holder = cscdNodeIsText(c) && n->children.len == 1 && cscdNodeIsElement(n) && blockTag((CscdElement*)n);
        const bool empty = (cscdNodeIsText(c) && !((CscdText*)c)->data.len && !holder) || (e && inlineTag(e) && !c->children.len);
        if (empty && cscdNodeRemoveChild(n, c)) cscdNodeDestroy(c);
    }
}

/* deletes [a, z) of the host's text: the text nodes in it shortened or gone, the block it ends in joined to the one it
 * starts in */
static void deleteRange(CscdView* v, CscdElement* host, size_t a, size_t z) {
    if (z <= a) return;
    HostText t;
    hostText(host, &t);
    z = cscdMinz(z, t.all.len);
    /* (the nodes the range starts and ends in: their blocks join when they differ) */
    CscdNode *firstNode = NULL, *lastNode = NULL;
    for (size_t i = 0; i < t.nodes.len; ++i) {
        const size_t s = t.starts.items[i], e = s + ((CscdText*)t.nodes.items[i])->data.len;
        if (!firstNode && s <= a && a <= e) firstNode = t.nodes.items[i];
        if (s <= z && z <= e) lastNode = t.nodes.items[i];
    }
    for (size_t i = 0; i < t.nodes.len; ++i) {
        CscdText* n = (CscdText*)t.nodes.items[i];
        const size_t s = t.starts.items[i], e = s + n->data.len;
        if (e <= a || s >= z) continue;
        const size_t from = a > s ? a - s : 0, to = cscdMinz(z, e) - s;
        CscdStr kept = {0};
        cscdStrAppend(&kept, cscdSvSub(cscdStrSv(&n->data), 0, from));
        cscdStrAppend(&kept, cscdSvFrom(cscdStrSv(&n->data), to));
        setData(v, n, cscdStrSv(&kept));
        cscdStrFree(&kept);
    }
    /* (blocks: what is left of the last one joins the first) */
    if (firstNode && lastNode && firstNode != lastNode) {
        CscdElement *from = blockOf(host, firstNode), *to = blockOf(host, lastNode);
        if (from != to && !holds(&to->node, &from->node)) {
            /* what is left of the last block goes after the first's (into it; or, when the first holds it, a list item in
             * the text before its list: before that list) */
            CscdNode* into = &from->node;
            CscdNode* before = NULL;
            if (holds(&from->node, &to->node)) {
                CscdNode* top = &to->node;
                while (top->parent && top->parent != &from->node) top = top->parent;
                before = top;
            }
            while (to->node.children.len) {
                CscdNode* c = cscdNodeRemoveChild(&to->node, cscdNodeChild(&to->node, 0));
                if (c && !(before ? cscdNodeInsertBefore(into, c, before) : cscdNodeAppendChild(into, c))) cscdNodeDestroy(c);
            }
            CscdNode* list = to->node.parent;
            CscdNode* gone = cscdNodeRemoveChild(list, &to->node);
            if (gone) cscdNodeDestroy(gone);
            /* (a list left with no items goes too) */
            CscdElement* le = cscdNodeAsElement(list);
            if (le && (cscdElementIs(le, "ul") || cscdElementIs(le, "ol")) && !list->children.len && list->parent) {
                CscdNode* empty = cscdNodeRemoveChild(list->parent, list);
                if (empty) cscdNodeDestroy(empty);
            }
        }
    }
    hostTextFree(&t);
    tidy(&host->node);
}

/* inserts text at pos (in the node there: its formatting goes on) */
static void insertAt(CscdView* v, CscdElement* host, size_t pos, CscdSv s) {
    HostText t;
    hostText(host, &t);
    size_t off = 0;
    const int i = nodeAt(&t, pos, &off);
    if (i < 0) {
        CscdText* n = cscdTextNew(s);
        if (!cscdNodeAppendChild(&host->node, &n->node)) cscdNodeDestroy(&n->node);
    } else {
        CscdText* n = (CscdText*)t.nodes.items[i];
        CscdStr data = {0};
        cscdStrAppend(&data, cscdSvSub(cscdStrSv(&n->data), 0, off));
        cscdStrAppend(&data, s);
        cscdStrAppend(&data, cscdSvFrom(cscdStrSv(&n->data), off));
        setData(v, n, cscdStrSv(&data));
        cscdStrFree(&data);
    }
    hostTextFree(&t);
}

/* ---- undo ------------------------------------------------------------------------------------------------- */

static void snapshotInto(CscdView* v, CscdElement* host, CeUndoVec* to) {
    CeUndo u = {cscdElementInnerHTML(host), v->ceAnchor, v->ceFocus};
    cscdVecPush(to, u);
    if (to->len > 200) {
        cscdStrFree(&to->items[0].html);
        cscdVecRemove(to, 0);
    }
}

/* kind: 1 typing, 2 deleting (a run of either is one step), 3 anything else */
static void pushUndo(CscdView* v, CscdElement* host, int kind) {
    if (v->ceQuiet) return; /* (a step of something that took its own snapshot) */
    const double now = viewNow(v);
    const bool coalesce = kind != 3 && kind == v->ceLastKind && now - v->ceLastT < 1.0 && v->ceUndo.len;
    v->ceLastKind = kind;
    v->ceLastT = now;
    for (size_t i = 0; i < v->ceRedo.len; ++i) cscdStrFree(&v->ceRedo.items[i].html);
    v->ceRedo.len = 0;
    if (!coalesce) snapshotInto(v, host, &v->ceUndo);
}

static bool undoStep(CscdView* v, CscdElement* host, bool redo) {
    CeUndoVec* from = redo ? &v->ceRedo : &v->ceUndo;
    if (!from->len) return false;
    CeUndo u = from->items[--from->len];
    snapshotInto(v, host, redo ? &v->ceUndo : &v->ceRedo);
    cscdElementSetInnerHTML(host, cscdStrSv(&u.html));
    cscdStrFree(&u.html);
    v->ceAnchor = u.anchor;
    v->ceFocus = u.focus;
    v->ceLastKind = 0;
    return true;
}

/* after a change: the page hears it, the caret shows */
static void changed(CscdView* v, CscdElement* host) {
    v->ceHasGoal = false;
    moved(v);
    const size_t mark = viewWatchMark(v);
    viewWatch(v, &host);
    viewSendSimple(v, "input", host);
    viewUnwatch(v, mark);
}

/* ---- typing ---------------------------------------------------------------------------------------------------- */

/* replaces the selection with s; a line break typed at the very end gets one more after it, so that its line shows
 * (a text's last line break makes no line of its own) */
static void replaceSelection(CscdView* v, CscdElement* host, CscdSv s) {
    HostText t;
    hostText(host, &t);
    size_t a, z;
    selection(v, t.all.len, &a, &z);
    hostTextFree(&t);
    deleteRange(v, host, a, z);
    if (s.len) {
        insertAt(v, host, a, s);
        hostText(host, &t);
        const bool lastBreak = s.ptr[s.len - 1] == '\n' && a + s.len == t.all.len;
        hostTextFree(&t);
        if (lastBreak) insertAt(v, host, a + s.len, CSCD_SV("\n"));
    }
    v->ceAnchor = v->ceFocus = a + s.len;
}

void viewEditableInput(CscdView* v, CscdElement* host, CscdSv s) {
    if (v->ceComposing) { /* (the composition's text goes; the committed text comes in its place) */
        deleteRange(v, host, v->ceCompStart, v->ceCompStart + v->ceCompLen);
        v->ceAnchor = v->ceFocus = v->ceCompStart;
        v->ceComposing = false;
    } else {
        pushUndo(v, host, 1);
    }
    replaceSelection(v, host, s);
    changed(v, host);
}

void viewEditableComposition(CscdView* v, CscdElement* host, CscdSv s) {
    if (!v->ceComposing) {
        if (!s.len) return;
        pushUndo(v, host, 3);
        HostText t;
        hostText(host, &t);
        size_t a, z;
        selection(v, t.all.len, &a, &z);
        hostTextFree(&t);
        deleteRange(v, host, a, z);
        v->ceCompStart = a;
        v->ceCompLen = 0;
        v->ceComposing = true;
    }
    deleteRange(v, host, v->ceCompStart, v->ceCompStart + v->ceCompLen);
    insertAt(v, host, v->ceCompStart, s);
    v->ceCompLen = s.len;
    v->ceAnchor = v->ceFocus = v->ceCompStart + s.len;
    if (!s.len) v->ceComposing = false;
    moved(v);
}

/* ---- formatting ---------------------------------------------------------------------------------------------- */

static CscdElement* ancestorTag(CscdElement* host, CscdNode* n, const char* tag, const char* alias) {
    for (CscdElement* e = cscdNodeParentElement(n); e && e != host; e = cscdNodeParentElement(&e->node))
        if (cscdElementIs(e, tag) || (alias && cscdElementIs(e, alias))) return e;
    return NULL;
}

/* x's parent split around x: what is before x goes in a copy of the parent before it, what is after in one after it */
static void splitAround(CscdNode* x) {
    CscdNode* p = x->parent;
    CscdElement* pe = cscdNodeAsElement(p);
    if (!pe || !p->parent) return;
    const int at = cscdNodeIndexInParent(x);
    if (at > 0) {
        CscdElement* before = cscdElementClone(pe, false);
        if (!cscdNodeInsertBefore(p->parent, &before->node, p)) cscdNodeDestroy(&before->node);
        else
            for (int i = 0; i < at; ++i) {
                CscdNode* c = cscdNodeRemoveChild(p, cscdNodeChild(p, 0));
                if (c && !cscdNodeAppendChild(&before->node, c)) cscdNodeDestroy(c);
            }
    }
    if (p->children.len > 1) {
        CscdElement* after = cscdElementClone(pe, false);
        if (!cscdNodeInsertBefore(p->parent, &after->node, cscdNodeNextSibling(p))) cscdNodeDestroy(&after->node);
        else
            while (p->children.len > 1) {
                CscdNode* c = cscdNodeRemoveChild(p, cscdNodeChild(p, 1));
                if (c && !cscdNodeAppendChild(&after->node, c)) cscdNodeDestroy(c);
            }
    }
}

/* the text node n out of the format element f it is in: f (and each element between them) split around it, then the
 * part of f that holds only it unwrapped; the text before and after n keeps the format */
static void unformat(CscdNode* n, CscdElement* f) {
    CscdNode* x = n;
    while (x->parent && x->parent != &f->node) {
        splitAround(x);
        x = x->parent;
    }
    if (!x->parent) return;
    splitAround(x); /* (f now holds x alone) */
    CscdNode* parent = f->node.parent;
    while (f->node.children.len) {
        CscdNode* c = cscdNodeRemoveChild(&f->node, cscdNodeChild(&f->node, 0));
        if (c && !cscdNodeInsertBefore(parent, c, &f->node)) cscdNodeDestroy(c);
    }
    CscdNode* gone = cscdNodeRemoveChild(parent, &f->node);
    if (gone) cscdNodeDestroy(gone);
}

/* the text node n cut where [a, z) begins and ends (its start s in the host's text): what is outside goes in nodes of
 * its own beside it, n keeps the part inside */
static void cutText(CscdView* v, CscdText* n, size_t s, size_t a, size_t z) {
    const size_t e = s + n->data.len;
    const size_t from = a > s ? a - s : 0, to = cscdMinz(z, e) - s;
    CscdNode* parent = n->node.parent;
    if (to < n->data.len) {
        CscdText* after = cscdTextNew(cscdSvFrom(cscdStrSv(&n->data), to));
        if (!cscdNodeInsertBefore(parent, &after->node, cscdNodeNextSibling(&n->node))) cscdNodeDestroy(&after->node);
    }
    if (from > 0) {
        CscdText* before = cscdTextNew(cscdSvSub(cscdStrSv(&n->data), 0, from));
        if (!cscdNodeInsertBefore(parent, &before->node, &n->node)) cscdNodeDestroy(&before->node);
    }
    CscdStr mid = cscdStrDup(cscdSvSub(cscdStrSv(&n->data), from, to - from));
    setData(v, n, cscdStrSv(&mid));
    cscdStrFree(&mid);
}

/* the selection's text in <tag> (each of its text nodes' part of it wrapped), or out of it when all of it is and only
 * is false (only the selection's part: the text around it keeps it) */
static void applyFormat(CscdView* v, CscdElement* host, const char* tag, const char* alias, bool only, CscdSv href) {
    HostText t;
    hostText(host, &t);
    size_t a, z;
    selection(v, t.all.len, &a, &z);
    if (a == z) {
        hostTextFree(&t);
        return;
    }
    bool all = true;
    for (size_t i = 0; i < t.nodes.len && all; ++i) {
        const CscdText* n = (const CscdText*)t.nodes.items[i];
        const size_t s = t.starts.items[i], e = s + n->data.len;
        if (e > a && s < z && !ancestorTag(host, (CscdNode*)&n->node, tag, alias)) all = false;
    }
    if (!all && !only && !strcmp(tag, "a") && !href.len) { /* (unlink with no link there: nothing) */
        hostTextFree(&t);
        return;
    }
    pushUndo(v, host, 3);
    if (all && only) {
        hostTextFree(&t);
        return;
    }
    if (all) {
        /* (out of it: the selection's text taken out of the elements that hold it, which go on around it) */
        CscdPtrVec picked = {0};
        for (size_t i = 0; i < t.nodes.len; ++i) {
            CscdText* n = (CscdText*)t.nodes.items[i];
            const size_t s = t.starts.items[i], e = s + n->data.len;
            if (e <= a || s >= z) continue;
            cutText(v, n, s, a, z);
            cscdVecPush(&picked, n);
        }
        for (size_t i = 0; i < picked.len; ++i) {
            CscdNode* n = (CscdNode*)picked.items[i];
            CscdElement* f;
            while ((f = ancestorTag(host, n, tag, alias)) != NULL) unformat(n, f);
        }
        cscdVecFree(&picked);
    } else {
        for (size_t i = 0; i < t.nodes.len; ++i) {
            CscdText* n = (CscdText*)t.nodes.items[i];
            const size_t s = t.starts.items[i], e = s + n->data.len;
            if (e <= a || s >= z || ancestorTag(host, &n->node, tag, alias)) continue;
            cutText(v, n, s, a, z); /* (before and after it: nodes of their own) */
            CscdNode* parent = n->node.parent;
            CscdElement* wrap = cscdElementNew(cscdSv(tag));
            if (href.len) cscdElementSetAttribute(wrap, CSCD_SV("href"), href);
            if (!cscdNodeInsertBefore(parent, &wrap->node, &n->node)) {
                cscdNodeDestroy(&wrap->node);
                continue;
            }
            CscdNode* moved = cscdNodeRemoveChild(parent, &n->node);
            if (moved && !cscdNodeAppendChild(&wrap->node, moved)) cscdNodeDestroy(moved);
        }
    }
    hostTextFree(&t);
    tidy(&host->node);
    changed(v, host);
}

/* ---- copying and pasting ----------------------------------------------------------------------------------- */

static void toggleFormat(CscdView* v, CscdElement* host, const char* tag, const char* alias) { applyFormat(v, host, tag, alias, false, CSCD_SV("")); }

static void escapeInto(CscdSv s, CscdStr* out) {
    for (size_t i = 0; i < s.len; ++i) {
        const char c = s.ptr[i];
        if (c == '<') cscdStrAppendC(out, "&lt;");
        else if (c == '>') cscdStrAppendC(out, "&gt;");
        else if (c == '&') cscdStrAppendC(out, "&amp;");
        else if (c == '\n') cscdStrAppendC(out, "<br>");
        else cscdStrAppendChar(out, c);
    }
}

/* the formatting a node's text has (bold, italic, underline, a link), outermost first */
static void formatsOf(CscdElement* host, CscdNode* n, CscdElement** out, int* count) {
    *count = 0;
    CscdElement* found[8];
    int k = 0;
    for (CscdElement* e = cscdNodeParentElement(n); e && e != host && k < 8; e = cscdNodeParentElement(&e->node))
        if (cscdElementIs(e, "b") || cscdElementIs(e, "strong") || cscdElementIs(e, "i") || cscdElementIs(e, "em") || cscdElementIs(e, "u") ||
            (cscdElementIs(e, "a") && cscdElementHasAttributeC(e, "href")))
            found[k++] = e;
    for (int i = k; i-- > 0;) out[(*count)++] = found[i];
}

static void openTag(CscdElement* e, CscdStr* out) {
    const char* name = cscdElementIs(e, "strong") ? "b" : cscdElementIs(e, "em") ? "i" : NULL;
    cscdStrAppendChar(out, '<');
    if (name) cscdStrAppendC(out, name);
    else cscdStrAppend(out, cscdElementTag(e));
    if (cscdElementIs(e, "a")) {
        cscdStrAppendC(out, " href=\"");
        const CscdSv href = cscdElementAttrC(e, "href");
        for (size_t i = 0; i < href.len; ++i) {
            if (href.ptr[i] == '"') cscdStrAppendC(out, "&quot;");
            else if (href.ptr[i] == '&') cscdStrAppendC(out, "&amp;");
            else cscdStrAppendChar(out, href.ptr[i]);
        }
        cscdStrAppendChar(out, '"');
    }
    cscdStrAppendChar(out, '>');
}

static void closeTag(CscdElement* e, CscdStr* out) {
    const char* name = cscdElementIs(e, "strong") ? "b" : cscdElementIs(e, "em") ? "i" : NULL;
    cscdStrAppendC(out, "</");
    if (name) cscdStrAppendC(out, name);
    else cscdStrAppend(out, cscdElementTag(e));
    cscdStrAppendChar(out, '>');
}

/* [a, z) as HTML: its text, with its bold, italic, underline and links (a line break between blocks: <br>) */
static void selectionHtml(CscdElement* host, const HostText* t, size_t a, size_t z, CscdStr* out) {
    for (size_t i = 0; i < t->nodes.len; ++i) {
        CscdText* n = (CscdText*)t->nodes.items[i];
        const size_t s = t->starts.items[i], e = s + n->data.len;
        if (s > a && s - 1 < z && s > 0 && isSep(t, s - 1) && out->len) cscdStrAppendC(out, "<br>");
        if (e <= a || s >= z) continue;
        CscdElement* tags[8];
        int count;
        formatsOf(host, &n->node, tags, &count);
        for (int k = 0; k < count; ++k) openTag(tags[k], out);
        const size_t from = a > s ? a - s : 0, to = cscdMinz(z, e) - s;
        escapeInto(cscdSvSub(cscdStrSv(&n->data), from, to - from), out);
        for (int k = count; k-- > 0;) closeTag(tags[k], out);
    }
}

static bool copySelection(CscdView* v, CscdElement* host) {
    HostText t;
    hostText(host, &t);
    size_t a, z;
    selection(v, t.all.len, &a, &z);
    bool copied = false;
    if (a < z) {
        CscdStr html = {0};
        selectionHtml(host, &t, a, z, &html);
        copied = cscdViewCopyHtml(v, cscdStrSv(&html), cscdSvSub(cscdStrSv(&t.all), a, z - a));
        cscdStrFree(&html);
    }
    hostTextFree(&t);
    return copied;
}

/* pasted HTML as runs of text with their formatting (blocks and <br> as line breaks), for the host to take */
typedef struct Run { CscdStr text; bool b, i, u; CscdStr href; } Run;
typedef CSCD_VEC(Run) RunVec;

static void runsOf(CscdNode* n, bool b, bool i, bool u, CscdSv href, RunVec* out) {
    for (size_t k = 0; k < n->children.len; ++k) {
        CscdNode* c = cscdNodeChild(n, k);
        if (cscdNodeIsText(c)) {
            Run r = {cscdStrCopy(&((CscdText*)c)->data), b, i, u, cscdStrDup(href)};
            if (r.text.len) cscdVecPush(out, r);
            else cscdStrFree(&r.text);
            continue;
        }
        CscdElement* e = cscdNodeAsElement(c);
        if (!e || cscdElementIs(e, "script") || cscdElementIs(e, "style") || cscdElementIs(e, "template") || cscdElementIs(e, "head")) continue;
        if (cscdElementIs(e, "br")) {
            Run r = {cscdStrDupC("\n"), b, i, u, {0}};
            cscdVecPush(out, r);
            continue;
        }
        static const char* const blocks[] = {"p", "div", "li", "h1", "h2", "h3", "h4", "h5", "h6", "tr", "blockquote", "pre"};
        bool block = false;
        for (size_t q = 0; q < CSCD_COUNT(blocks); ++q) block = block || cscdElementIs(e, blocks[q]);
        const CscdStr* lastRun = out->len ? &out->items[out->len - 1].text : NULL;
        if (block && lastRun && !(lastRun->len && lastRun->ptr[lastRun->len - 1] == '\n')) {
            Run r = {cscdStrDupC("\n"), b, i, u, {0}};
            cscdVecPush(out, r);
        }
        runsOf(c, b || cscdElementIs(e, "b") || cscdElementIs(e, "strong"), i || cscdElementIs(e, "i") || cscdElementIs(e, "em"),
               u || cscdElementIs(e, "u"), cscdElementIs(e, "a") && cscdElementHasAttributeC(e, "href") ? cscdElementAttrC(e, "href") : href, out);
    }
}

static void paste(CscdView* v, CscdElement* host) {
    CscdStr html = {0}, text = {0};
    cscdViewClipboardHtml(v, &html);
    RunVec runs = {0};
    if (html.len) {
        CscdElement* holder = cscdElementNew(CSCD_SV("div"));
        cscdElementSetInnerHTML(holder, cscdStrSv(&html));
        runsOf(&holder->node, false, false, false, CSCD_SV(""), &runs);
        cscdNodeDestroy(&holder->node);
    }
    if (!runs.len) {
        cscdViewClipboardText(v, &text);
        if (text.len) {
            Run r = {cscdStrCopy(&text), false, false, false, {0}};
            cscdVecPush(&runs, r);
        }
    }
    if (runs.len) {
        pushUndo(v, host, 3);
        /* the text at once (where the caret is: its formatting there), then each run's own over its part of it */
        CscdStr all = {0};
        for (size_t k = 0; k < runs.len; ++k) cscdStrAppend(&all, cscdStrSv(&runs.items[k].text));
        size_t at = cscdMinz(v->ceAnchor, v->ceFocus);
        replaceSelection(v, host, cscdStrSv(&all));
        const size_t end = v->ceFocus;
        v->ceQuiet = true; /* (one step for the whole paste) */
        for (size_t k = 0; k < runs.len; ++k) {
            const Run* r = &runs.items[k];
            const char* tags[3] = {r->b ? "b" : NULL, r->i ? "i" : NULL, r->u ? "u" : NULL};
            for (int q = 0; q < 3; ++q)
                if (tags[q]) {
                    v->ceAnchor = at;
                    v->ceFocus = at + r->text.len;
                    applyFormat(v, host, tags[q], NULL, true, CSCD_SV(""));
                }
            if (r->href.len) {
                v->ceAnchor = at;
                v->ceFocus = at + r->text.len;
                applyFormat(v, host, "a", NULL, true, cscdStrSv(&r->href));
            }
            at += r->text.len;
        }
        v->ceQuiet = false;
        v->ceAnchor = v->ceFocus = end;
        cscdStrFree(&all);
        changed(v, host);
    }
    for (size_t k = 0; k < runs.len; ++k) {
        cscdStrFree(&runs.items[k].text);
        cscdStrFree(&runs.items[k].href);
    }
    cscdVecFree(&runs);
    cscdStrFree(&html);
    cscdStrFree(&text);
}

/* ---- blocks: list items, lists, headings ------------------------------------------------------------------------ */

/* Splits the host's tree at pos up to block: the text node there and the inline elements around it in two; returns the
 * node in block that starts the second part (NULL: pos is at the block's end). */
static CscdNode* splitTo(CscdView* v, CscdElement* host, CscdElement* block, size_t pos) {
    HostText t;
    hostText(host, &t);
    size_t off = 0;
    const int i = nodeAt(&t, pos, &off);
    if (i < 0) {
        hostTextFree(&t);
        return NULL;
    }
    CscdText* n = (CscdText*)t.nodes.items[i];
    hostTextFree(&t);
    if (!holds(&block->node, &n->node)) return NULL;
    /* (the text node: its part from off on, a node of its own after it) */
    CscdNode* right = NULL;
    if (off < n->data.len) {
        CscdText* after = cscdTextNew(cscdSvFrom(cscdStrSv(&n->data), off));
        CscdStr before = cscdStrDup(cscdSvSub(cscdStrSv(&n->data), 0, off));
        setData(v, n, cscdStrSv(&before));
        cscdStrFree(&before);
        if (!cscdNodeInsertBefore(n->node.parent, &after->node, cscdNodeNextSibling(&n->node))) cscdNodeDestroy(&after->node);
        else right = &after->node;
    } else {
        right = cscdNodeNextSibling(&n->node);
    }
    /* (each inline element above it: the part from right on into a copy of it, after it) */
    CscdNode* at = &n->node;
    while (at->parent && at->parent != &block->node) {
        CscdNode* parent = at->parent;
        if (right) {
            CscdElement* copy = cscdElementClone((CscdElement*)parent, false);
            if (copy && cscdNodeInsertBefore(parent->parent, &copy->node, cscdNodeNextSibling(parent))) {
                while (right) {
                    CscdNode* next = cscdNodeNextSibling(right);
                    CscdNode* moving = cscdNodeRemoveChild(parent, right);
                    if (moving && !cscdNodeAppendChild(&copy->node, moving)) cscdNodeDestroy(moving);
                    right = next;
                }
                right = &copy->node;
            } else if (copy) {
                cscdNodeDestroy(&copy->node);
            }
        } else {
            right = cscdNodeNextSibling(parent);
        }
        at = parent;
    }
    return right;
}

/* the element that holds pos's node with that tag (within the host), or NULL */
static CscdElement* holderAt(CscdElement* host, size_t pos, const char* tag) {
    HostText t;
    hostText(host, &t);
    size_t off = 0;
    const int i = nodeAt(&t, pos, &off);
    CscdElement* found = i >= 0 ? ancestorTag(host, t.nodes.items[i], tag, NULL) : NULL;
    hostTextFree(&t);
    return found;
}

/* the element with that tag that holds the character at pos (not the place: the character after it), or NULL */
static CscdElement* holderOfChar(CscdElement* host, size_t pos, const char* tag) {
    HostText t;
    hostText(host, &t);
    CscdElement* found = NULL;
    for (size_t i = 0; i < t.nodes.len; ++i) {
        const size_t s = t.starts.items[i], e = s + ((CscdText*)t.nodes.items[i])->data.len;
        if (s <= pos && pos < e) {
            found = ancestorTag(host, t.nodes.items[i], tag, NULL);
            break;
        }
    }
    hostTextFree(&t);
    return found;
}

/* a block with nothing in it gets the empty text node the caret goes in */
static void caretHolder(CscdElement* block) {
    CscdPtrVec texts = {0};
    textNodesIn(&block->node, &texts);
    if (!texts.len) {
        CscdText* empty = cscdTextNew(CSCD_SV(""));
        if (!cscdNodeAppendChild(&block->node, &empty->node)) cscdNodeDestroy(&empty->node);
    }
    cscdVecFree(&texts);
}

/* Enter in a list item: a new item from the caret on (an empty one: out of the list, a line after it) */
static bool enterInItem(CscdView* v, CscdElement* host, size_t pos) {
    CscdElement* li = holderAt(host, pos, "li");
    if (!li) return false;
    CscdStr text = {0};
    cscdNodeTextContent(&li->node, &text);
    const bool empty = !text.len;
    cscdStrFree(&text);
    CscdNode* list = li->node.parent;
    if (empty && list && list->parent) {
        /* (out of the list: the item goes, an empty line after the list) */
        CscdText* line = cscdTextNew(CSCD_SV(""));
        CscdElement* p = cscdElementNew(CSCD_SV("div"));
        cscdNodeAppendChild(&p->node, &line->node);
        if (!cscdNodeInsertBefore(list->parent, &p->node, cscdNodeNextSibling(list))) cscdNodeDestroy(&p->node);
        CscdNode* gone = cscdNodeRemoveChild(list, &li->node);
        if (gone) cscdNodeDestroy(gone);
        if (!list->children.len) {
            CscdNode* l = cscdNodeRemoveChild(list->parent, list);
            if (l) cscdNodeDestroy(l);
        }
        cscdNodeMarkDirty(&host->node);
        v->ceAnchor = v->ceFocus = pos; /* (the line where the item was) */
        return true;
    }
    CscdNode* right = splitTo(v, host, li, pos);
    CscdElement* next = cscdElementClone(li, false);
    if (!next) return false;
    if (!cscdNodeInsertBefore(list, &next->node, cscdNodeNextSibling(&li->node))) {
        cscdNodeDestroy(&next->node);
        return false;
    }
    while (right) {
        CscdNode* after = cscdNodeNextSibling(right);
        CscdNode* moving = cscdNodeRemoveChild(&li->node, right);
        if (moving && !cscdNodeAppendChild(&next->node, moving)) cscdNodeDestroy(moving);
        right = after;
    }
    caretHolder(li);
    caretHolder(next);
    tidy(&host->node);
    cscdNodeMarkDirty(&host->node);
    v->ceAnchor = v->ceFocus = pos + 1; /* (past the line break between them: the new item's start) */
    return true;
}

/* the lines [a, z) lies on: from the start of the first to the end of the last (not their line breaks) */
static void linesAround(const HostText* t, size_t a, size_t z, size_t* from, size_t* to) {
    const CscdSv s = cscdStrSv(&t->all);
    size_t p = cscdMinz(a, s.len), q = cscdMinz(z, s.len);
    while (p > 0 && s.ptr[p - 1] != '\n') --p;
    while (q < s.len && s.ptr[q] != '\n') ++q;
    *from = p;
    *to = q;
}

/* Puts element where pos is in the host's tree: the text there split, the inline elements around it too. */
static void placeBlock(CscdView* v, CscdElement* host, size_t pos, CscdElement* e) {
    HostText t;
    hostText(host, &t);
    size_t off = 0;
    const int i = nodeAt(&t, pos, &off);
    CscdElement* block = i >= 0 ? blockOf(host, t.nodes.items[i]) : host;
    hostTextFree(&t);
    CscdNode* right = i >= 0 ? splitTo(v, host, block, pos) : NULL;
    if (!(right ? cscdNodeInsertBefore(&block->node, &e->node, right) : cscdNodeAppendChild(&block->node, &e->node))) cscdNodeDestroy(&e->node);
}

/* The lines the selection is on as blocks: each one a list item of a new <ul> or <ol> (list), or each a <tag>
 * (a heading, a paragraph); their text, with its formatting, moved in. */
static void linesToBlocks(CscdView* v, CscdElement* host, const char* list, const char* tag) {
    HostText t;
    hostText(host, &t);
    size_t a, z, from, to;
    selection(v, t.all.len, &a, &z);
    linesAround(&t, a, z, &from, &to);
    /* each line's HTML */
    CscdStrVec lines = {0};
    for (size_t p = from; p <= to;) {
        size_t q = p;
        while (q < to && t.all.ptr[q] != '\n') ++q;
        CscdStr html = {0};
        selectionHtml(host, &t, p, q, &html);
        cscdVecPush(&lines, html);
        if (q >= to) break;
        p = q + 1;
    }
    /* (the line breaks of the text around them go: the new blocks separate the lines) */
    const bool breakBefore = from > 0 && !isSep(&t, from - 1);
    const bool breakAfter = to < t.all.len && !isSep(&t, to);
    hostTextFree(&t);
    pushUndo(v, host, 3);
    deleteRange(v, host, from, to + (breakAfter ? 1 : 0));
    if (breakBefore) {
        deleteRange(v, host, from - 1, from);
        --from;
    }
    CscdElement* outer = list ? cscdElementNew(cscdSv(list)) : NULL;
    for (size_t k = 0; k < lines.len; ++k) {
        CscdElement* item = cscdElementNew(cscdSv(list ? "li" : tag));
        cscdElementSetInnerHTML(item, cscdStrSv(&lines.items[k]));
        caretHolder(item);
        if (outer) {
            if (!cscdNodeAppendChild(&outer->node, &item->node)) cscdNodeDestroy(&item->node);
        } else {
            placeBlock(v, host, from, item);
            /* (the next one after it) */
            CscdStr text = {0};
            cscdNodeTextContent(&item->node, &text);
            from += text.len + 1;
            cscdStrFree(&text);
        }
    }
    if (outer) placeBlock(v, host, from, outer);
    for (size_t k = 0; k < lines.len; ++k) cscdStrFree(&lines.items[k]);
    cscdVecFree(&lines);
    tidy(&host->node);
    cscdNodeMarkDirty(&host->node);
    changed(v, host);
}

/* Blocks of that tag that hold the selection made lines again: their contents in the text, a line break between
 * them (a list: its items). */
static void blocksToLines(CscdView* v, CscdElement* host, CscdElement* block) {
    pushUndo(v, host, 3);
    CscdNode* parent = block->node.parent;
    const bool list = cscdElementIs(block, "ul") || cscdElementIs(block, "ol");
    CscdPtrVec items = {0};
    if (list) {
        for (size_t i = 0; i < block->node.children.len; ++i)
            if (cscdNodeIsElement(cscdNodeChild(&block->node, i))) cscdVecPush(&items, cscdNodeChild(&block->node, i));
    } else {
        cscdVecPush(&items, &block->node);
    }
    /* (in a block of their own when the list was not alone in the host's text: a div a line) */
    for (size_t k = 0; k < items.len; ++k) {
        CscdNode* item = (CscdNode*)items.items[k];
        CscdElement* line = cscdElementNew(CSCD_SV("div"));
        while (item->children.len) {
            CscdNode* c = cscdNodeRemoveChild(item, cscdNodeChild(item, 0));
            if (c && !cscdNodeAppendChild(&line->node, c)) cscdNodeDestroy(c);
        }
        caretHolder(line);
        if (!cscdNodeInsertBefore(parent, &line->node, &block->node)) cscdNodeDestroy(&line->node);
    }
    cscdVecFree(&items);
    CscdNode* gone = cscdNodeRemoveChild(parent, &block->node);
    if (gone) cscdNodeDestroy(gone);
    tidy(&host->node);
    cscdNodeMarkDirty(&host->node);
    changed(v, host);
}

static void toggleList(CscdView* v, CscdElement* host, const char* list) {
    HostText t;
    hostText(host, &t);
    size_t a, z;
    selection(v, t.all.len, &a, &z);
    hostTextFree(&t);
    CscdElement* li = holderAt(host, a, "li");
    CscdElement* in = li ? cscdNodeParentElement(&li->node) : NULL;
    if (in && cscdElementIs(in, list)) blocksToLines(v, host, in);
    else if (in) { /* (a list of the other kind: renamed) */
        pushUndo(v, host, 3);
        CscdElement* other = cscdElementNew(cscdSv(list));
        CscdNode* parent = in->node.parent;
        while (in->node.children.len) {
            CscdNode* c = cscdNodeRemoveChild(&in->node, cscdNodeChild(&in->node, 0));
            if (c && !cscdNodeAppendChild(&other->node, c)) cscdNodeDestroy(c);
        }
        if (!cscdNodeInsertBefore(parent, &other->node, &in->node)) cscdNodeDestroy(&other->node);
        CscdNode* gone = cscdNodeRemoveChild(parent, &in->node);
        if (gone) cscdNodeDestroy(gone);
        changed(v, host);
    } else {
        linesToBlocks(v, host, list, NULL);
    }
}

/* the block the caret is in as a <tag> (h1 to h6, p); a heading of that tag already: a paragraph */
static void formatBlock(CscdView* v, CscdElement* host, CscdSv tag) {
    static const char* const ok[] = {"h1", "h2", "h3", "h4", "h5", "h6", "p", "div", "pre", "blockquote"};
    const char* want = NULL;
    for (size_t i = 0; i < CSCD_COUNT(ok); ++i) if (cscdSvIEqC(tag, ok[i])) want = ok[i];
    if (!want) return;
    HostText t;
    hostText(host, &t);
    size_t a, z, off = 0;
    selection(v, t.all.len, &a, &z);
    const int i = nodeAt(&t, a, &off);
    CscdElement* block = i >= 0 ? blockOf(host, t.nodes.items[i]) : host;
    hostTextFree(&t);
    if (block == host || cscdElementIs(block, "li")) {
        linesToBlocks(v, host, NULL, want);
        return;
    }
    pushUndo(v, host, 3);
    if (cscdElementIs(block, want) && want[0] == 'h') want = "p";
    CscdElement* renamed = cscdElementNew(cscdSv(want));
    CscdNode* parent = block->node.parent;
    while (block->node.children.len) {
        CscdNode* c = cscdNodeRemoveChild(&block->node, cscdNodeChild(&block->node, 0));
        if (c && !cscdNodeAppendChild(&renamed->node, c)) cscdNodeDestroy(c);
    }
    if (!cscdNodeInsertBefore(parent, &renamed->node, &block->node)) cscdNodeDestroy(&renamed->node);
    CscdNode* gone = cscdNodeRemoveChild(parent, &block->node);
    if (gone) cscdNodeDestroy(gone);
    cscdNodeMarkDirty(&host->node);
    changed(v, host);
}

/* ---- the keys ----------------------------------------------------------------------------------------------- */

static size_t lineEdge(CscdView* v, CscdElement* host, const HostText* t, size_t pos, bool end) {
    const CscdRect r = caretAt(v, host, t, pos);
    return hitAt(v, host, t, end ? 1e6f : -1e6f, r.y + r.h / 2);
}

bool viewEditableKey(CscdView* v, CscdElement* host, CscdKey k, CscdModifiers m, CscdSv keyName) {
    const bool cmd = m.ctrl || m.meta;
    if (k == CSCD_KEY_UP || k == CSCD_KEY_DOWN || k == CSCD_KEY_HOME || k == CSCD_KEY_END) cscdViewUpdate(v); /* (where the text is now) */
    HostText t;
    hostText(host, &t);
    const CscdSv all = cscdStrSv(&t.all);
    size_t a, z;
    selection(v, t.all.len, &a, &z);
    size_t to = v->ceFocus = cscdMinz(v->ceFocus, t.all.len);
    v->ceAnchor = cscdMinz(v->ceAnchor, t.all.len);
    bool move = false, keepGoal = false, handled = true;
    const bool letter = k == CSCD_KEY_CHARACTER || k == CSCD_KEY_A || k == CSCD_KEY_C || k == CSCD_KEY_V || k == CSCD_KEY_X ||
                        k == CSCD_KEY_Z || k == CSCD_KEY_Y;
    if (cmd && letter && keyName.len == 1) {
        const char c = (char)tolower((unsigned char)keyName.ptr[0]);
        if (c == 'a') { v->ceAnchor = 0; v->ceFocus = t.all.len; moved(v); }
        else if (c == 'c') copySelection(v, host);
        else if (c == 'x') {
            if (a < z && copySelection(v, host)) {
                pushUndo(v, host, 3);
                replaceSelection(v, host, CSCD_SV(""));
                changed(v, host);
            }
        } else if (c == 'v') paste(v, host);
        else if (c == 'z' && !m.shift) { if (undoStep(v, host, false)) changed(v, host); }
        else if (c == 'y' || (c == 'z' && m.shift)) { if (undoStep(v, host, true)) changed(v, host); }
        else if (c == 'b') toggleFormat(v, host, "b", "strong");
        else if (c == 'i') toggleFormat(v, host, "i", "em");
        else if (c == 'u') toggleFormat(v, host, "u", NULL);
        else handled = false;
        hostTextFree(&t);
        return handled;
    }
    switch (k) {
    case CSCD_KEY_LEFT:
        if (a < z && !m.shift) to = a;
        else to = cmd ? wordBack(all, to) : cscdGraphemePrev(all, to);
        move = true;
        break;
    case CSCD_KEY_RIGHT:
        if (a < z && !m.shift) to = z;
        else to = cmd ? wordOn(all, to) : cscdGraphemeNext(all, to);
        move = true;
        break;
    case CSCD_KEY_UP: case CSCD_KEY_DOWN: {
        const CscdRect r = caretAt(v, host, &t, to);
        if (!v->ceHasGoal) {
            v->ceGoalX = r.x;
            v->ceHasGoal = true;
        }
        const float y = k == CSCD_KEY_UP ? r.y - r.h / 2 : r.y + r.h * 1.5f;
        const size_t next = hitAt(v, host, &t, v->ceGoalX, y);
        /* (no line there: the text's start, or its end) */
        const CscdRect nr = caretAt(v, host, &t, next);
        to = fabsf(nr.y - r.y) < 0.5f ? (k == CSCD_KEY_UP ? 0 : t.all.len) : next;
        move = keepGoal = true;
        break;
    }
    case CSCD_KEY_HOME: to = cmd ? 0 : lineEdge(v, host, &t, to, false); move = true; break;
    case CSCD_KEY_END: to = cmd ? t.all.len : lineEdge(v, host, &t, to, true); move = true; break;
    case CSCD_KEY_BACKSPACE: case CSCD_KEY_DELETE: {
        size_t from = a, upto = z;
        if (a == z) {
            if (k == CSCD_KEY_BACKSPACE) from = cmd ? wordBack(all, a) : cscdGraphemePrev(all, a);
            else upto = cmd ? wordOn(all, a) : cscdGraphemeNext(all, a);
        }
        if (from < upto) {
            pushUndo(v, host, 2);
            deleteRange(v, host, from, upto);
            v->ceAnchor = v->ceFocus = from;
            changed(v, host);
        }
        break;
    }
    case CSCD_KEY_ENTER:
        pushUndo(v, host, 3);
        if (holderAt(host, a, "li") && !m.shift) {
            replaceSelection(v, host, CSCD_SV(""));
            enterInItem(v, host, a);
        } else {
            replaceSelection(v, host, CSCD_SV("\n"));
        }
        changed(v, host);
        break;
    default:
        handled = false;
        break;
    }
    if (move) {
        v->ceFocus = to;
        if (!m.shift) v->ceAnchor = to;
        if (!keepGoal) v->ceHasGoal = false;
        v->ceLastKind = 0;
        moved(v);
    }
    hostTextFree(&t);
    return handled;
}

/* ---- the pointer --------------------------------------------------------------------------------------------- */

void viewEditablePress(CscdView* v, CscdElement* host, float x, float y, int clicks, bool shift) {
    cscdViewUpdate(v);
    HostText t;
    hostText(host, &t);
    const size_t at = hitAt(v, host, &t, x, y);
    if (clicks >= 3) {
        v->ceAnchor = 0;
        v->ceFocus = t.all.len;
    } else if (clicks == 2) {
        /* (the word the press was on: its characters' run, the space's, or one sign) */
        const CscdSv all = cscdStrSv(&t.all);
        size_t p = cscdMinz(at, all.len), q = p;
        const int cls = p < all.len ? wordClass(all.ptr[p]) : p > 0 ? wordClass(all.ptr[p - 1]) : 0;
        if (p == all.len && p > 0) --p, q = p + 1;
        if (cls == 2) q = cscdMinz(p + 1, all.len);
        else {
            while (p > 0 && wordClass(all.ptr[p - 1]) == cls && all.ptr[p - 1] != '\n') --p;
            while (q < all.len && wordClass(all.ptr[q]) == cls && all.ptr[q] != '\n') ++q;
        }
        v->ceAnchor = p;
        v->ceFocus = q;
    } else {
        v->ceFocus = at;
        if (!shift) v->ceAnchor = at;
    }
    v->ceHasGoal = false;
    v->ceLastKind = 0;
    hostTextFree(&t);
    moved(v);
}

void viewEditableDrag(CscdView* v, CscdElement* host, float x, float y) {
    cscdViewUpdate(v);
    HostText t;
    hostText(host, &t);
    v->ceFocus = hitAt(v, host, &t, x, y);
    hostTextFree(&t);
    moved(v);
}

/* ---- what shows: the selection and the caret, over the page ------------------------------------------------- */

CscdRect viewEditableCaret(CscdView* v, CscdElement* host) {
    cscdViewUpdate(v);
    HostText t;
    hostText(host, &t);
    const CscdRect r = caretAt(v, host, &t, cscdMinz(v->ceFocus, t.all.len));
    hostTextFree(&t);
    return r;
}

void viewPaintEditable(CscdView* v, CscdCanvas* c) {
    CscdElement* host = viewEditableHost(v);
    if (!host || !host->layout) return;
    HostText t;
    hostText(host, &t);
    size_t a, z;
    selection(v, t.all.len, &a, &z);
    const CscdPoint d = hostShift(v, host);
    if (a < z) {
        FragVec frags = {0};
        fragsIn(host->layout, &t, &frags);
        for (size_t i = 0; i < frags.len; ++i) {
            const Frag* f = &frags.items[i];
            const size_t s = f->base + f->frag->start, e = f->base + f->frag->end;
            if (e <= a || s >= z) continue;
            const float x0 = fragX(f, cscdMaxz(a, s) - f->base), x1 = fragX(f, cscdMinz(z, e) - f->base);
            const CscdRect r = cscdRect(fminf(x0, x1) + d.x, (float)f->frag->top + d.y, fabsf(x1 - x0), f->frag->height);
            cscdCanvasFillRect(c, r, cscdRgba(51, 144, 255, 90));
        }
        cscdVecFree(&frags);
    } else if (cscdViewCaretVisible(v)) {
        const CscdRect r = caretAt(v, host, &t, a);
        const CscdColor color = host->style ? (host->style->caretColor.a ? host->style->caretColor : host->style->color) : cscdRgb(0, 0, 0);
        cscdCanvasFillRect(c, cscdRect(roundf(r.x), r.y, 1, r.h), color);
    }
    hostTextFree(&t);
}

/* ---- for screen readers (a11y.c): the host as a text field ------------------------------------------------------ */

void viewEditableText(CscdElement* host, CscdStr* out) {
    HostText t;
    hostText(host, &t);
    cscdStrAppend(out, cscdStrSv(&t.all));
    hostTextFree(&t);
}

void viewEditableRects(CscdView* v, CscdElement* host, size_t a, size_t z, CscdRectVec* out) {
    cscdViewUpdate(v);
    if (!host->layout) return;
    HostText t;
    hostText(host, &t);
    a = cscdMinz(a, t.all.len);
    z = cscdMinz(z, t.all.len);
    if (a > z) {
        const size_t s = a;
        a = z;
        z = s;
    }
    if (a == z) cscdVecPush(out, caretAt(v, host, &t, a));
    else {
        const CscdPoint d = hostShift(v, host);
        FragVec frags = {0};
        fragsIn(host->layout, &t, &frags);
        for (size_t i = 0; i < frags.len; ++i) {
            const Frag* f = &frags.items[i];
            const size_t s = f->base + f->frag->start, e = f->base + f->frag->end;
            if (e <= a || s >= z) continue;
            const float x0 = fragX(f, cscdMaxz(a, s) - f->base), x1 = fragX(f, cscdMinz(z, e) - f->base);
            cscdVecPush(out, cscdRect(fminf(x0, x1) + d.x, (float)f->frag->top + d.y, cscdMaxf(1.f, fabsf(x1 - x0)), f->frag->height));
        }
        cscdVecFree(&frags);
    }
    hostTextFree(&t);
}

size_t viewEditableHit(CscdView* v, CscdElement* host, float x, float y) {
    cscdViewUpdate(v);
    if (!host->layout) return 0;
    HostText t;
    hostText(host, &t);
    const size_t at = hitAt(v, host, &t, x, y);
    hostTextFree(&t);
    return at;
}

bool viewEditableSelect(CscdView* v, CscdElement* host, size_t start, size_t end) {
    if (v->doc->focused != host) cscdViewFocus(v, host);
    if (viewEditableHost(v) != host) return false;
    HostText t;
    hostText(host, &t);
    v->ceAnchor = cscdMinz(start, t.all.len);
    v->ceFocus = cscdMinz(end, t.all.len);
    hostTextFree(&t);
    v->ceHasGoal = false;
    moved(v);
    return true;
}

/* ---- the program's side ------------------------------------------------------------------------------------- */

bool cscdViewEditableSelection(CscdView* v, size_t* start, size_t* end) {
    CscdElement* host = viewEditableHost(v);
    if (!host) return false;
    HostText t;
    hostText(host, &t);
    selection(v, t.all.len, start, end);
    hostTextFree(&t);
    return true;
}

bool cscdViewSetEditableSelection(CscdView* v, size_t start, size_t end) {
    CscdElement* host = viewEditableHost(v);
    if (!host) return false;
    v->ceAnchor = start;
    v->ceFocus = end;
    v->ceHasGoal = false;
    moved(v);
    return true;
}

bool cscdViewEditableCommand(CscdView* v, CscdSv command) { return cscdViewEditableCommandWith(v, command, CSCD_SV("")); }

bool cscdViewEditableCommandWith(CscdView* v, CscdSv command, CscdSv value) {
    CscdElement* host = viewEditableHost(v);
    if (!host) return false;
    cscdViewUpdate(v); /* (the blocks as they are) */
    if (cscdSvEqC(command, "createLink")) {
        if (!value.len) return false;
        /* (a link already over the selection: its address changed) */
        HostText t;
        hostText(host, &t);
        size_t a, z;
        selection(v, t.all.len, &a, &z);
        hostTextFree(&t);
        CscdElement* link = holderOfChar(host, a, "a");
        if (link && link == holderOfChar(host, z > a ? z - 1 : z, "a")) {
            pushUndo(v, host, 3);
            cscdElementSetAttribute(link, CSCD_SV("href"), value);
            changed(v, host);
        } else {
            applyFormat(v, host, "a", NULL, true, value);
        }
    } else if (cscdSvEqC(command, "unlink")) {
        applyFormat(v, host, "a", NULL, false, CSCD_SV(""));
    } else if (cscdSvEqC(command, "insertUnorderedList")) toggleList(v, host, "ul");
    else if (cscdSvEqC(command, "insertOrderedList")) toggleList(v, host, "ol");
    else if (cscdSvEqC(command, "formatBlock")) formatBlock(v, host, value);
    else if (cscdSvEqC(command, "bold")) toggleFormat(v, host, "b", "strong");
    else if (cscdSvEqC(command, "italic")) toggleFormat(v, host, "i", "em");
    else if (cscdSvEqC(command, "underline")) toggleFormat(v, host, "u", NULL);
    else if (cscdSvEqC(command, "undo")) { if (undoStep(v, host, false)) changed(v, host); }
    else if (cscdSvEqC(command, "redo")) { if (undoStep(v, host, true)) changed(v, host); }
    else if (cscdSvEqC(command, "selectAll")) cscdViewSetEditableSelection(v, 0, (size_t)-1);
    else return false;
    return true;
}
