/* Accessibility, without a platform: what each element is to a screen reader (its role, name, states
 * and value), the tree of the elements that mean something (plain containers left out, their
 * contents lifted to their parent), what can be done to them, and the events the window passes on.
 * The window turns this into UI Automation, NSAccessibility or AT-SPI. */
#include "view/viewInternal.h"

#include <stdlib.h>

/* ---- roles --------------------------------------------------------------------------------------- */

static const struct { const char* name; CscdRole role; } kAriaRoles[] = {
    {"button", CSCD_ROLE_BUTTON}, {"link", CSCD_ROLE_LINK}, {"checkbox", CSCD_ROLE_CHECKBOX}, {"switch", CSCD_ROLE_CHECKBOX},
    {"radio", CSCD_ROLE_RADIO}, {"textbox", CSCD_ROLE_TEXTBOX}, {"searchbox", CSCD_ROLE_TEXTBOX}, {"combobox", CSCD_ROLE_COMBOBOX},
    {"listbox", CSCD_ROLE_LISTBOX}, {"option", CSCD_ROLE_OPTION}, {"slider", CSCD_ROLE_SLIDER}, {"progressbar", CSCD_ROLE_PROGRESS},
    {"heading", CSCD_ROLE_HEADING}, {"list", CSCD_ROLE_LIST}, {"listitem", CSCD_ROLE_LIST_ITEM}, {"img", CSCD_ROLE_IMAGE},
    {"image", CSCD_ROLE_IMAGE}, {"dialog", CSCD_ROLE_DIALOG}, {"alertdialog", CSCD_ROLE_DIALOG}, {"tab", CSCD_ROLE_TAB},
    {"tablist", CSCD_ROLE_TAB_LIST}, {"tabpanel", CSCD_ROLE_TAB_PANEL}, {"menu", CSCD_ROLE_MENU}, {"menubar", CSCD_ROLE_MENU_BAR},
    {"menuitem", CSCD_ROLE_MENU_ITEM}, {"menuitemcheckbox", CSCD_ROLE_MENU_ITEM}, {"menuitemradio", CSCD_ROLE_MENU_ITEM},
    {"table", CSCD_ROLE_TABLE}, {"grid", CSCD_ROLE_TABLE}, {"row", CSCD_ROLE_ROW}, {"cell", CSCD_ROLE_CELL},
    {"gridcell", CSCD_ROLE_CELL}, {"columnheader", CSCD_ROLE_CELL}, {"rowheader", CSCD_ROLE_CELL}, {"separator", CSCD_ROLE_SEPARATOR},
    {"tooltip", CSCD_ROLE_TOOLTIP}, {"group", CSCD_ROLE_GROUP}, {"region", CSCD_ROLE_GROUP}, {"navigation", CSCD_ROLE_GROUP},
    {"main", CSCD_ROLE_GROUP}, {"toolbar", CSCD_ROLE_GROUP}, {"status", CSCD_ROLE_TEXT}, {"alert", CSCD_ROLE_TEXT},
    {"tree", CSCD_ROLE_LIST}, {"treeitem", CSCD_ROLE_LIST_ITEM}, {"spinbutton", CSCD_ROLE_SLIDER},
    {"none", CSCD_ROLE_NONE}, {"presentation", CSCD_ROLE_NONE},
};

static bool hasDirectText(const CscdElement* e) {
    for (size_t i = 0; i < e->node.children.len; ++i) {
        const CscdNode* c = cscdNodeChild(&e->node, i);
        if (c->type != CSCD_NODE_TEXT) continue;
        const CscdStr* t = &((const CscdText*)c)->data;
        for (size_t k = 0; k < t->len; ++k)
            if (!cscdIsSpace(t->ptr[k])) return true;
    }
    return false;
}

static bool inputIs(const CscdElement* e, const char* type) { return cscdSvIEqC(cscdElementAttrC(e, "type"), type); }

CscdRole cscdAccessibleRole(const CscdElement* e) {
    if (!e) return CSCD_ROLE_NONE;
    const CscdSv aria = cscdElementAttrC(e, "role");
    if (aria.len) {
        CscdSv first = aria; /* (the first of a list of roles) */
        size_t sp = cscdSvFindC(first, " ", 0);
        if (sp != CSCD_NPOS) first = cscdSvSub(first, 0, sp);
        for (size_t i = 0; i < CSCD_COUNT(kAriaRoles); ++i)
            if (cscdSvIEqC(first, kAriaRoles[i].name)) return kAriaRoles[i].role;
    }
    const CscdSv t = cscdStrSv(&e->tag);
    if (cscdSvEqC(t, "button") || cscdSvEqC(t, "summary")) return CSCD_ROLE_BUTTON;
    if (cscdSvEqC(t, "a")) return cscdElementHasAttributeC(e, "href") ? CSCD_ROLE_LINK : (hasDirectText(e) ? CSCD_ROLE_TEXT : CSCD_ROLE_NONE);
    if (cscdSvEqC(t, "input")) {
        if (inputIs(e, "hidden")) return CSCD_ROLE_NONE;
        if (inputIs(e, "checkbox")) return CSCD_ROLE_CHECKBOX;
        if (inputIs(e, "radio")) return CSCD_ROLE_RADIO;
        if (inputIs(e, "range")) return CSCD_ROLE_SLIDER;
        if (inputIs(e, "button") || inputIs(e, "submit") || inputIs(e, "reset")) return CSCD_ROLE_BUTTON;
        return CSCD_ROLE_TEXTBOX;
    }
    if (cscdSvEqC(t, "textarea")) return CSCD_ROLE_TEXTBOX;
    if (cscdSvEqC(t, "select")) return CSCD_ROLE_COMBOBOX;
    if (cscdSvEqC(t, "option")) return CSCD_ROLE_OPTION;
    if (cscdSvEqC(t, "progress") || cscdSvEqC(t, "meter")) return CSCD_ROLE_PROGRESS;
    if (t.len == 2 && t.ptr[0] == 'h' && t.ptr[1] >= '1' && t.ptr[1] <= '6') return CSCD_ROLE_HEADING;
    if (cscdSvEqC(t, "ul") || cscdSvEqC(t, "ol") || cscdSvEqC(t, "menu")) return CSCD_ROLE_LIST;
    if (cscdSvEqC(t, "li")) return CSCD_ROLE_LIST_ITEM;
    if (cscdSvEqC(t, "img") || cscdSvEqC(t, "svg") || cscdSvEqC(t, "canvas")) return CSCD_ROLE_IMAGE;
    if (cscdSvEqC(t, "dialog")) return CSCD_ROLE_DIALOG;
    if (cscdSvEqC(t, "table")) return CSCD_ROLE_TABLE;
    if (cscdSvEqC(t, "tr")) return CSCD_ROLE_ROW;
    if (cscdSvEqC(t, "td") || cscdSvEqC(t, "th")) return CSCD_ROLE_CELL;
    if (cscdSvEqC(t, "hr")) return CSCD_ROLE_SEPARATOR;
    if (cscdSvEqC(t, "nav") || cscdSvEqC(t, "main") || cscdSvEqC(t, "section") || cscdSvEqC(t, "aside") || cscdSvEqC(t, "header") ||
        cscdSvEqC(t, "footer") || cscdSvEqC(t, "form") || cscdSvEqC(t, "fieldset") || cscdSvEqC(t, "details") || cscdSvEqC(t, "figure"))
        return CSCD_ROLE_GROUP;
    if (cscdSvEqC(t, "body")) return CSCD_ROLE_DOCUMENT;
    /* a plain element: text when it holds text of its own, left out otherwise (its contents rise) */
    if (isFocusable(e)) return CSCD_ROLE_GROUP;
    return hasDirectText(e) ? CSCD_ROLE_TEXT : CSCD_ROLE_NONE;
}

/* Elements a reader never sees: not shown, or hidden from it. */
static bool hidden(const CscdElement* e) {
    if (cscdSvEqC(cscdElementAttrC(e, "aria-hidden"), "true") || cscdElementHasAttributeC(e, "hidden")) return true;
    if (cscdElementIs(e, "template") || cscdElementIs(e, "script") || cscdElementIs(e, "style") || cscdElementIs(e, "head")) return true;
    if (e->style && (e->style->display == CSCD_DISPLAY_NONE || e->style->visibility != 0)) return true;
    return !e->style && !cscdElementIs(e, "html"); /* (not styled: not in a shown document) */
}

/* ---- names --------------------------------------------------------------------------------------- */

/* (white space collapsed to single spaces as it comes; the caller trims the end) */
static void breakInto(CscdStr* out) {
    if (out->len && out->ptr[out->len - 1] != ' ') cscdStrAppendChar(out, ' ');
}

static void collapseInto(CscdSv text, CscdStr* out) {
    for (size_t i = 0; i < text.len; ++i) {
        if (cscdIsSpace(text.ptr[i])) breakInto(out);
        else cscdStrAppendChar(out, text.ptr[i]);
    }
}

/* the text of e and what it holds, as a reader says it (hidden parts left out, alt texts in) */
static void visibleText(const CscdNode* n, CscdStr* out) {
    if (n->type == CSCD_NODE_TEXT) { collapseInto(cscdStrSv(&((const CscdText*)n)->data), out); return; }
    const CscdElement* e = cscdNodeAsElementC(n);
    if (e && hidden(e)) return;
    if (e && cscdElementIs(e, "img")) { collapseInto(cscdElementAttrC(e, "alt"), out); return; }
    /* (a block's text is apart from what is next to it; inline elements run on) */
    const bool block = e && e->style && e->style->display != CSCD_DISPLAY_INLINE && e->style->display != CSCD_DISPLAY_CONTENTS;
    if (block) breakInto(out);
    for (size_t i = 0; i < n->children.len; ++i) visibleText(cscdNodeChild(n, i), out);
    if (block) breakInto(out);
}

static void textOfIds(const CscdView* v, CscdSv ids, CscdStr* out) {
    for (CscdSv rest = ids; rest.len;) {
        rest = cscdSvTrim(rest);
        size_t sp = 0;
        while (sp < rest.len && !cscdIsSpace(rest.ptr[sp])) ++sp;
        const CscdElement* t = cscdDocumentGetElementById(v->doc, cscdSvSub(rest, 0, sp));
        if (t) visibleText(&t->node, out);
        rest = cscdSvFrom(rest, sp);
    }
}

static const CscdElement* labelFor(const CscdView* v, const CscdElement* e) {
    const CscdSv id = cscdElementAttrC(e, "id");
    if (id.len) {
        CscdPtrVec labels = {0};
        cscdDocumentQuerySelectorAll(v->doc, CSCD_SV("label"), &labels);
        const CscdElement* found = NULL;
        for (size_t i = 0; i < labels.len && !found; ++i)
            if (cscdSvEq(cscdElementAttrC((const CscdElement*)labels.items[i], "for"), id)) found = labels.items[i];
        cscdVecFree(&labels);
        if (found) return found;
    }
    for (const CscdElement* a = cscdNodeParentElement(&e->node); a; a = cscdNodeParentElement(&a->node))
        if (cscdElementIs(a, "label")) return a;
    return NULL;
}

void cscdAccessibleName(const CscdView* v, const CscdElement* e, CscdStr* out) {
    const size_t from = out->len;
    const CscdSv by = cscdElementAttrC(e, "aria-labelledby");
    if (by.len) textOfIds(v, by, out);
    if (out->len == from) collapseInto(cscdElementAttrC(e, "aria-label"), out);
    const CscdRole role = cscdAccessibleRole(e);
    if (out->len == from && (role == CSCD_ROLE_TEXTBOX || role == CSCD_ROLE_CHECKBOX || role == CSCD_ROLE_RADIO ||
                             role == CSCD_ROLE_SLIDER || role == CSCD_ROLE_COMBOBOX || role == CSCD_ROLE_PROGRESS)) {
        const CscdElement* label = labelFor(v, e);
        if (label) visibleText(&label->node, out);
    }
    if (out->len == from && cscdElementIs(e, "img")) collapseInto(cscdElementAttrC(e, "alt"), out);
    if (out->len == from && cscdElementIs(e, "input") && (inputIs(e, "button") || inputIs(e, "submit") || inputIs(e, "reset")))
        collapseInto(cscdElementAttrC(e, "value"), out);
    if (out->len == from && (role == CSCD_ROLE_BUTTON || role == CSCD_ROLE_LINK || role == CSCD_ROLE_HEADING || role == CSCD_ROLE_TEXT ||
                             role == CSCD_ROLE_OPTION || role == CSCD_ROLE_TAB || role == CSCD_ROLE_MENU_ITEM ||
                             role == CSCD_ROLE_CELL || role == CSCD_ROLE_TOOLTIP))
        visibleText(&e->node, out);
    if (out->len == from && cscdElementIs(e, "dialog")) {
        const CscdElement* h = cscdElementQuerySelector((CscdElement*)e, CSCD_SV("h1, h2, h3"));
        if (h) visibleText(&h->node, out);
    }
    if (out->len == from) collapseInto(cscdElementAttrC(e, "title"), out);
    if (out->len == from && role == CSCD_ROLE_TEXTBOX) collapseInto(cscdElementAttrC(e, "placeholder"), out);
    /* (trimmed) */
    while (out->len > from && out->ptr[out->len - 1] == ' ') out->ptr[--out->len] = 0;
    if (out->len > from && out->ptr[from] == ' ') cscdStrErase(out, from, 1);
}

void cscdAccessibleDescription(const CscdView* v, const CscdElement* e, CscdStr* out) {
    const CscdSv by = cscdElementAttrC(e, "aria-describedby");
    if (by.len) textOfIds(v, by, out);
    else if (cscdElementHasAttributeC(e, "aria-label") || cscdElementHasAttributeC(e, "aria-labelledby")) collapseInto(cscdElementAttrC(e, "title"), out);
}

/* ---- states and values --------------------------------------------------------------------------- */

uint32_t cscdAccessibleState(const CscdView* v, const CscdElement* e) {
    uint32_t s = 0;
    if (isFocusable(e)) s |= CSCD_A11Y_FOCUSABLE;
    if (v->doc->focused == e) s |= CSCD_A11Y_FOCUSED;
    const CscdRole role = cscdAccessibleRole(e);
    const CscdSv ariaChecked = cscdElementAttrC(e, "aria-checked");
    if ((role == CSCD_ROLE_CHECKBOX || role == CSCD_ROLE_RADIO || role == CSCD_ROLE_MENU_ITEM) &&
        (cscdElementChecked(e) || cscdSvEqC(ariaChecked, "true")))
        s |= CSCD_A11Y_CHECKED;
    if (cscdSvEqC(ariaChecked, "mixed") || (role == CSCD_ROLE_CHECKBOX && cscdElementHasAttributeC(e, "indeterminate"))) s |= CSCD_A11Y_MIXED;
    if ((e->state & CSCD_STATE_DISABLED) || cscdElementHasAttributeC(e, "disabled") || cscdSvEqC(cscdElementAttrC(e, "aria-disabled"), "true"))
        s |= CSCD_A11Y_DISABLED;
    const CscdSv expanded = cscdElementAttrC(e, "aria-expanded");
    if (cscdSvEqC(expanded, "true")) s |= CSCD_A11Y_EXPANDED;
    else if (cscdSvEqC(expanded, "false")) s |= CSCD_A11Y_COLLAPSED;
    if (cscdElementIs(e, "summary")) {
        const CscdElement* d = cscdNodeParentElement(&e->node);
        if (d && cscdElementIs(d, "details")) s |= cscdElementHasAttributeC(d, "open") ? CSCD_A11Y_EXPANDED : CSCD_A11Y_COLLAPSED;
    }
    if (cscdSvEqC(cscdElementAttrC(e, "aria-selected"), "true") || (role == CSCD_ROLE_OPTION && cscdElementChecked(e))) s |= CSCD_A11Y_SELECTED;
    if (cscdElementHasAttributeC(e, "readonly") || cscdSvEqC(cscdElementAttrC(e, "aria-readonly"), "true")) s |= CSCD_A11Y_READONLY;
    if (cscdElementHasAttributeC(e, "required") || cscdSvEqC(cscdElementAttrC(e, "aria-required"), "true")) s |= CSCD_A11Y_REQUIRED;
    if (cscdElementIs(e, "textarea")) s |= CSCD_A11Y_MULTILINE;
    if (cscdElementIs(e, "input") && inputIs(e, "password")) s |= CSCD_A11Y_PASSWORD;
    return s;
}

int cscdAccessibleLevel(const CscdElement* e) {
    const CscdSv lvl = cscdElementAttrC(e, "aria-level");
    if (lvl.len) return (int)cscdParseLong(lvl, NULL);
    if (e->tag.len == 2 && e->tag.ptr[0] == 'h' && e->tag.ptr[1] >= '1' && e->tag.ptr[1] <= '6') return e->tag.ptr[1] - '0';
    return 0;
}

static double numberAttr(const CscdElement* e, const char* name, double fallback) {
    const CscdSv s = cscdElementAttrC(e, name);
    size_t used = 0;
    const double d = s.len ? cscdParseDouble(s, &used) : 0;
    return used ? d : fallback;
}

bool cscdAccessibleRange(const CscdElement* e, double* value, double* min, double* max, double* step) {
    const CscdRole role = cscdAccessibleRole(e);
    if (role != CSCD_ROLE_SLIDER && role != CSCD_ROLE_PROGRESS) return false;
    const bool aria = cscdElementHasAttributeC(e, "aria-valuenow");
    *min = numberAttr(e, aria ? "aria-valuemin" : "min", 0);
    *max = numberAttr(e, aria ? "aria-valuemax" : "max", cscdElementIs(e, "input") ? 100 : 1);
    *step = numberAttr(e, "step", cscdElementIs(e, "input") ? 1 : 0);
    if (aria) *value = numberAttr(e, "aria-valuenow", *min);
    else {
        CscdStr v = cscdElementValue(e);
        size_t used = 0;
        const double d = v.len ? cscdParseDouble(cscdStrSv(&v), &used) : 0;
        cscdStrFree(&v);
        *value = used ? d : numberAttr(e, "value", (*min + *max) / 2);
    }
    return true;
}

void cscdAccessibleValue(const CscdView* v, const CscdElement* e, CscdStr* out) {
    (void)v;
    const CscdRole role = cscdAccessibleRole(e);
    if (role == CSCD_ROLE_TEXTBOX || role == CSCD_ROLE_COMBOBOX) {
        CscdStr val = cscdElementValue(e);
        if (cscdElementIs(e, "select")) { /* (the chosen option's text) */
            const CscdElement* chosen = cscdElementQuerySelector((CscdElement*)e, CSCD_SV("option:checked"));
            if (chosen) {
                cscdStrClear(&val);
                visibleText(&chosen->node, &val);
                while (val.len && val.ptr[val.len - 1] == ' ') val.ptr[--val.len] = 0;
            }
        }
        cscdStrAppend(out, cscdStrSv(&val));
        cscdStrFree(&val);
    } else if (role == CSCD_ROLE_LINK) cscdStrAppend(out, cscdElementAttrC(e, "href"));
    else {
        double val, min, max, step;
        if (cscdAccessibleRange(e, &val, &min, &max, &step)) {
            const CscdSv text = cscdElementAttrC(e, "aria-valuetext");
            if (text.len) cscdStrAppend(out, text);
            else cscdStrAppendf(out, "%g", val);
        }
    }
}

/* ---- the tree ------------------------------------------------------------------------------------ */

bool cscdAccessibleIncluded(const CscdElement* e) { return e && !hidden(e) && cscdAccessibleRole(e) != CSCD_ROLE_NONE; }

static void childrenOf(const CscdElement* e, CscdPtrVec* out) {
    for (size_t i = 0; i < e->node.children.len; ++i) {
        CscdElement* c = cscdNodeAsElement(cscdNodeChild(&e->node, i));
        if (!c || hidden(c)) continue;
        if (cscdAccessibleRole(c) != CSCD_ROLE_NONE) cscdVecPush(out, c);
        else childrenOf(c, out); /* (a plain container: its contents rise) */
    }
}

CscdElement* cscdAccessibleRoot(const CscdView* v) { return cscdDocumentBody(v->doc); }

void cscdAccessibleChildren(const CscdView* v, const CscdElement* e, CscdPtrVec* out) {
    if (!e) e = cscdAccessibleRoot(v);
    if (!e) return;
    childrenOf(e, out);
}

CscdElement* cscdAccessibleParent(const CscdView* v, const CscdElement* e) {
    const CscdElement* root = cscdAccessibleRoot(v);
    if (!e || e == root) return NULL;
    for (CscdElement* a = cscdNodeParentElement(&e->node); a; a = cscdNodeParentElement(&a->node))
        if (a == root || cscdAccessibleIncluded(a)) return a;
    return NULL;
}

CscdElement* cscdAccessibleAt(CscdView* v, float x, float y) {
    for (CscdElement* e = cscdViewElementAt(v, x, y); e; e = cscdNodeParentElement(&e->node))
        if (cscdAccessibleIncluded(e) || e == cscdAccessibleRoot(v)) return e;
    return cscdAccessibleRoot(v);
}

/* ---- handles: numbers that stay with an element, and are refused once it is gone ------------------ */

uint32_t cscdAccessibleId(CscdView* v, const CscdElement* e) {
    if (!e) return 0;
    bool inserted = false;
    CscdMapVal* m = cscdU64MapPut(&v->a11yIds, CSCD_PKEY(e), &inserted);
    if (inserted) {
        m->u = ++v->a11yNext;
        cscdU64MapPut(&v->a11yElements, m->u, NULL)->p = (void*)e;
    }
    return (uint32_t)m->u;
}

CscdElement* cscdAccessibleElement(const CscdView* v, uint32_t id) {
    const CscdMapVal* m = cscdU64MapGet(&v->a11yElements, id);
    return m ? (CscdElement*)m->p : NULL;
}

void viewA11yForget(CscdView* v, const CscdElement* e) {
    CscdMapVal old;
    if (cscdU64MapRemove(&v->a11yIds, CSCD_PKEY(e), &old)) cscdU64MapRemove(&v->a11yElements, old.u, NULL);
}

void viewA11yFree(CscdView* v) {
    cscdU64MapFree(&v->a11yIds);
    cscdU64MapFree(&v->a11yElements);
}

/* ---- actions and events -------------------------------------------------------------------------- */

bool cscdAccessibleDo(CscdView* v, CscdElement* e, CscdAccessibleAction action, CscdSv value) {
    if (!e || (cscdAccessibleState(v, e) & CSCD_A11Y_DISABLED)) return false;
    switch (action) {
    case CSCD_A11Y_ACTIVATE: cscdElementClick(e); return true;
    case CSCD_A11Y_FOCUS: if (!isFocusable(e)) return false; cscdViewFocus(v, e); return true;
    case CSCD_A11Y_SCROLL_INTO_VIEW: cscdViewScrollIntoView(v, e); return true;
    case CSCD_A11Y_SET_VALUE: {
        if (cscdAccessibleState(v, e) & CSCD_A11Y_READONLY) return false;
        cscdElementSetValue(e, value);
        CscdEvent ev;
        cscdEventInit(&ev, CSCD_SV("input"));
        viewDispatch(v, &ev, e);
        cscdEventFini(&ev);
        cscdEventInit(&ev, CSCD_SV("change"));
        viewDispatch(v, &ev, e);
        cscdEventFini(&ev);
        return true;
    }
    }
    return false;
}

void cscdViewOnAccessibleEvent(CscdView* v, CscdAccessibleEventFn fn, void* user) {
    v->onA11y = fn;
    v->onA11yUser = user;
}

void viewA11yNotify(CscdView* v, CscdSv type, CscdElement* target) {
    if (!v->onA11y || !viewOn(v, CSCD_FEATURE_ACCESSIBILITY) || !target) return;
    if (cscdSvEqC(type, "focus")) v->onA11y(v->onA11yUser, target, CSCD_A11Y_EVENT_FOCUS);
    else if (cscdSvEqC(type, "input") || cscdSvEqC(type, "change")) v->onA11y(v->onA11yUser, target, CSCD_A11Y_EVENT_VALUE);
    else if (cscdSvEqC(type, "toggle")) v->onA11y(v->onA11yUser, target, CSCD_A11Y_EVENT_STATE);
}

void viewA11yStructure(CscdView* v) {
    if (v->onA11y && viewOn(v, CSCD_FEATURE_ACCESSIBILITY)) v->onA11y(v->onA11yUser, NULL, CSCD_A11Y_EVENT_STRUCTURE);
}
