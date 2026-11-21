/* contenteditable: the page's own text edited in place. The element with the attribute (not "false") is the editing
 * host when it has the focus; its text is that of the text nodes in it, in document order, and the selection is two
 * places in it counted in bytes of that text taken whole (a node that comes or goes elsewhere leaves nothing
 * dangling). The host keeps its text as typed (white-space: pre-wrap in the UA sheet), so a text box's text is its
 * node's data and its fragments say where each byte of it shows. Enter is a line break in the text; bold, italic and
 * underline wrap the selection in <b>, <i>, <u>. */
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

static void textNodesIn(CscdNode* n, CscdPtrVec* out) {
    for (size_t i = 0; i < n->children.len; ++i) {
        CscdNode* c = cscdNodeChild(n, i);
        if (cscdNodeIsText(c)) cscdVecPush(out, c);
        else if (cscdNodeIsElement(c) && !cscdElementIs((CscdElement*)c, "template")) textNodesIn(c, out);
    }
}

/* the host's text taken whole, and where each of its nodes starts in it */
typedef struct HostText {
    CscdPtrVec nodes; /* CscdText* */
    CscdSizeVec starts;
    CscdStr all;
} HostText;

static void hostText(CscdElement* host, HostText* t) {
    memset(t, 0, sizeof *t);
    textNodesIn(&host->node, &t->nodes);
    for (size_t i = 0; i < t->nodes.len; ++i) {
        const CscdText* n = (const CscdText*)t->nodes.items[i];
        cscdVecPush(&t->starts, t->all.len);
        cscdStrAppend(&t->all, cscdStrSv(&n->data));
    }
}

static void hostTextFree(HostText* t) {
    cscdVecFree(&t->nodes);
    cscdVecFree(&t->starts);
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
    if (!host->layout || !t->all.len) return 0;
    const CscdPoint d = hostShift(v, host);
    x -= d.x;
    y -= d.y;
    FragVec frags = {0};
    fragsIn(host->layout, t, &frags);
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

static bool inlineTag(const CscdElement* e) {
    static const char* const tags[] = {"b", "i", "u", "s", "em", "strong", "span", "code", "sub", "sup", "mark", "small", "a", "font"};
    for (size_t i = 0; i < CSCD_COUNT(tags); ++i) if (cscdElementIs(e, tags[i])) return true;
    return false;
}

/* empty text nodes, and inline elements left with nothing in them, out of the host */
static void tidy(CscdNode* n) {
    for (size_t i = n->children.len; i-- > 0;) {
        CscdNode* c = cscdNodeChild(n, i);
        CscdElement* e = cscdNodeAsElement(c);
        if (e) tidy(c);
        const bool empty = (cscdNodeIsText(c) && !((CscdText*)c)->data.len) || (e && inlineTag(e) && !c->children.len);
        if (empty && cscdNodeRemoveChild(n, c)) cscdNodeDestroy(c);
    }
}

/* the block a node is in, within the host (the host itself when there is none) */
static CscdElement* blockOf(CscdElement* host, CscdNode* n) {
    for (CscdElement* e = cscdNodeParentElement(n); e && e != host; e = cscdNodeParentElement(&e->node))
        if (e->style ? e->style->display != CSCD_DISPLAY_INLINE : !inlineTag(e)) return e;
    return host;
}

static bool holds(const CscdNode* a, const CscdNode* n) {
    for (; n; n = n->parent) if (n == a) return true;
    return false;
}

/* deletes [a, z) of the host's text: the text nodes in it shortened or gone, the block it ends in joined to the one it
 * starts in */
static void deleteRange(CscdView* v, CscdElement* host, size_t a, size_t z) {
    if (z <= a) return;
    HostText t;
    hostText(host, &t);
    z = cscdMinz(z, t.all.len);
    CscdNode *firstNode = NULL, *lastNode = NULL;
    for (size_t i = 0; i < t.nodes.len; ++i) {
        CscdText* n = (CscdText*)t.nodes.items[i];
        const size_t s = t.starts.items[i], e = s + n->data.len;
        if (e <= a || s >= z) {
            if (s >= z && !lastNode && firstNode) lastNode = &n->node;
            continue;
        }
        const size_t from = a > s ? a - s : 0, to = cscdMinz(z, e) - s;
        if (!firstNode) firstNode = &n->node;
        lastNode = &n->node;
        CscdStr kept = {0};
        cscdStrAppend(&kept, cscdSvSub(cscdStrSv(&n->data), 0, from));
        cscdStrAppend(&kept, cscdSvFrom(cscdStrSv(&n->data), to));
        setData(v, n, cscdStrSv(&kept));
        cscdStrFree(&kept);
    }
    /* (blocks: what is left of the last one joins the first) */
    if (firstNode && lastNode && firstNode != lastNode) {
        CscdElement *from = blockOf(host, firstNode), *to = blockOf(host, lastNode);
        if (from != to && !holds(&from->node, &to->node) && !holds(&to->node, &from->node)) {
            while (to->node.children.len) {
                CscdNode* c = cscdNodeRemoveChild(&to->node, cscdNodeChild(&to->node, 0));
                if (c && !cscdNodeAppendChild(&from->node, c)) cscdNodeDestroy(c);
            }
            CscdNode* gone = cscdNodeRemoveChild(to->node.parent, &to->node);
            if (gone) cscdNodeDestroy(gone);
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

/* the selection's text in <tag> (each of its text nodes' part of it wrapped), or out of it when all of it is and only
 * is false */
static void applyFormat(CscdView* v, CscdElement* host, const char* tag, const char* alias, bool only) {
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
    pushUndo(v, host, 3);
    if (all && only) {
        hostTextFree(&t);
        return;
    }
    if (all) {
        /* (out of it: the elements that hold the selection's text unwrapped) */
        for (size_t i = 0; i < t.nodes.len; ++i) {
            CscdText* n = (CscdText*)t.nodes.items[i];
            const size_t s = t.starts.items[i], e = s + n->data.len;
            if (e <= a || s >= z) continue;
            CscdElement* f;
            while ((f = ancestorTag(host, &n->node, tag, alias)) != NULL) {
                CscdNode* parent = f->node.parent;
                while (f->node.children.len) {
                    CscdNode* c = cscdNodeRemoveChild(&f->node, cscdNodeChild(&f->node, 0));
                    if (c && !cscdNodeInsertBefore(parent, c, &f->node)) cscdNodeDestroy(c);
                }
                CscdNode* gone = cscdNodeRemoveChild(parent, &f->node);
                if (gone) cscdNodeDestroy(gone);
            }
        }
    } else {
        for (size_t i = 0; i < t.nodes.len; ++i) {
            CscdText* n = (CscdText*)t.nodes.items[i];
            const size_t s = t.starts.items[i], e = s + n->data.len;
            if (e <= a || s >= z || ancestorTag(host, &n->node, tag, alias)) continue;
            const size_t from = a > s ? a - s : 0, to = cscdMinz(z, e) - s;
            CscdNode* parent = n->node.parent;
            /* (before and after it: nodes of their own) */
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
            CscdElement* wrap = cscdElementNew(cscdSv(tag));
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

static void toggleFormat(CscdView* v, CscdElement* host, const char* tag, const char* alias) { applyFormat(v, host, tag, alias, false); }

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

/* the formatting a node's text has, as tag names (b, i, u), outermost first */
static void formatsOf(CscdElement* host, CscdNode* n, const char** out, int* count) {
    *count = 0;
    const char* found[8];
    int k = 0;
    for (CscdElement* e = cscdNodeParentElement(n); e && e != host && k < 8; e = cscdNodeParentElement(&e->node)) {
        if (cscdElementIs(e, "b") || cscdElementIs(e, "strong")) found[k++] = "b";
        else if (cscdElementIs(e, "i") || cscdElementIs(e, "em")) found[k++] = "i";
        else if (cscdElementIs(e, "u")) found[k++] = "u";
    }
    for (int i = k; i-- > 0;) out[(*count)++] = found[i];
}

/* [a, z) as HTML: its text, with its bold, italic and underline */
static void selectionHtml(CscdElement* host, const HostText* t, size_t a, size_t z, CscdStr* out) {
    for (size_t i = 0; i < t->nodes.len; ++i) {
        CscdText* n = (CscdText*)t->nodes.items[i];
        const size_t s = t->starts.items[i], e = s + n->data.len;
        if (e <= a || s >= z) continue;
        const char* tags[8];
        int count;
        formatsOf(host, &n->node, tags, &count);
        for (int k = 0; k < count; ++k) {
            cscdStrAppendChar(out, '<');
            cscdStrAppendC(out, tags[k]);
            cscdStrAppendChar(out, '>');
        }
        const size_t from = a > s ? a - s : 0, to = cscdMinz(z, e) - s;
        escapeInto(cscdSvSub(cscdStrSv(&n->data), from, to - from), out);
        for (int k = count; k-- > 0;) {
            cscdStrAppendC(out, "</");
            cscdStrAppendC(out, tags[k]);
            cscdStrAppendChar(out, '>');
        }
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
typedef struct Run { CscdStr text; bool b, i, u; } Run;
typedef CSCD_VEC(Run) RunVec;

static void runsOf(CscdNode* n, bool b, bool i, bool u, RunVec* out) {
    for (size_t k = 0; k < n->children.len; ++k) {
        CscdNode* c = cscdNodeChild(n, k);
        if (cscdNodeIsText(c)) {
            Run r = {cscdStrCopy(&((CscdText*)c)->data), b, i, u};
            if (r.text.len) cscdVecPush(out, r);
            else cscdStrFree(&r.text);
            continue;
        }
        CscdElement* e = cscdNodeAsElement(c);
        if (!e || cscdElementIs(e, "script") || cscdElementIs(e, "style") || cscdElementIs(e, "template") || cscdElementIs(e, "head")) continue;
        if (cscdElementIs(e, "br")) {
            Run r = {cscdStrDupC("\n"), b, i, u};
            cscdVecPush(out, r);
            continue;
        }
        static const char* const blocks[] = {"p", "div", "li", "h1", "h2", "h3", "h4", "h5", "h6", "tr", "blockquote", "pre"};
        bool block = false;
        for (size_t q = 0; q < CSCD_COUNT(blocks); ++q) block = block || cscdElementIs(e, blocks[q]);
        const CscdStr* lastRun = out->len ? &out->items[out->len - 1].text : NULL;
        if (block && lastRun && !(lastRun->len && lastRun->ptr[lastRun->len - 1] == '\n')) {
            Run r = {cscdStrDupC("\n"), b, i, u};
            cscdVecPush(out, r);
        }
        runsOf(c, b || cscdElementIs(e, "b") || cscdElementIs(e, "strong"), i || cscdElementIs(e, "i") || cscdElementIs(e, "em"),
               u || cscdElementIs(e, "u"), out);
    }
}

static void paste(CscdView* v, CscdElement* host) {
    CscdStr html = {0}, text = {0};
    cscdViewClipboardHtml(v, &html);
    RunVec runs = {0};
    if (html.len) {
        CscdElement* holder = cscdElementNew(CSCD_SV("div"));
        cscdElementSetInnerHTML(holder, cscdStrSv(&html));
        runsOf(&holder->node, false, false, false, &runs);
        cscdNodeDestroy(&holder->node);
    }
    if (!runs.len) {
        cscdViewClipboardText(v, &text);
        if (text.len) {
            Run r = {cscdStrCopy(&text), false, false, false};
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
                    applyFormat(v, host, tags[q], NULL, true);
                }
            at += r->text.len;
        }
        v->ceQuiet = false;
        v->ceAnchor = v->ceFocus = end;
        cscdStrFree(&all);
        changed(v, host);
    }
    for (size_t k = 0; k < runs.len; ++k) cscdStrFree(&runs.items[k].text);
    cscdVecFree(&runs);
    cscdStrFree(&html);
    cscdStrFree(&text);
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
        replaceSelection(v, host, CSCD_SV("\n"));
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

bool cscdViewEditableCommand(CscdView* v, CscdSv command) {
    CscdElement* host = viewEditableHost(v);
    if (!host) return false;
    if (cscdSvEqC(command, "bold")) toggleFormat(v, host, "b", "strong");
    else if (cscdSvEqC(command, "italic")) toggleFormat(v, host, "i", "em");
    else if (cscdSvEqC(command, "underline")) toggleFormat(v, host, "u", NULL);
    else if (cscdSvEqC(command, "undo")) { if (undoStep(v, host, false)) changed(v, host); }
    else if (cscdSvEqC(command, "redo")) { if (undoStep(v, host, true)) changed(v, host); }
    else if (cscdSvEqC(command, "selectAll")) cscdViewSetEditableSelection(v, 0, (size_t)-1);
    else return false;
    return true;
}
