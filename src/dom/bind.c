/* Data binding: models of named values and lists, and the page kept in step with them through its
 * data-* attributes (see cascade/bind.h). A binding walks its part of the page when its model
 * changed and touches only what differs; data-each keeps a copy of its template per item, reused
 * as the list changes. */
#include "cascade/bind.h"
#include "core/thread.h"
#include "view/viewInternal.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- models ---------------------------------------------------------------------------------- */

typedef struct ModelListener { CscdModelFn fn; void* user; } ModelListener;

struct CscdModel {
    CscdStrMap values; /* name -> CscdStr* */
    CscdStrMap lists;  /* name -> CscdPtrVec* of CscdModel* (references) */
    CscdModel* parent;
    uint64_t version;   /* changed here or in an item below */
    CSCD_VEC(ModelListener) listeners;
};

static void modelDtor(void* p) {
    CscdModel* m = (CscdModel*)p;
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&m->values, s));) {
        cscdStrFree((CscdStr*)s->val.p);
        cscdFree(s->val.p);
    }
    cscdStrMapFree(&m->values);
    for (CscdStrMapSlot* s = NULL; (s = cscdStrMapNext(&m->lists, s));) {
        CscdPtrVec* items = (CscdPtrVec*)s->val.p;
        for (size_t i = 0; i < items->len; ++i) {
            ((CscdModel*)items->items[i])->parent = NULL;
            cscdRelease(items->items[i]);
        }
        cscdVecFree(items);
        cscdFree(items);
    }
    cscdStrMapFree(&m->lists);
    cscdVecFree(&m->listeners);
}

CscdModel* cscdModelNew(void) { return (CscdModel*)cscdRcNew(sizeof(CscdModel), modelDtor); }

static volatile size_t modelClock; /* (versions only ever grow: a change anywhere is newer than all before; views on other threads share it) */

static void touch(CscdModel* m) {
    const uint64_t now = cscdAtomicFetchAddZ(&modelClock, 1) + 1;
    for (; m; m = m->parent) m->version = now;
}

void cscdModelSet(CscdModel* m, CscdSv name, CscdSv text) {
    bool inserted = false;
    CscdMapVal* slot = cscdStrMapPut(&m->values, name, &inserted);
    if (inserted) slot->p = CSCD_NEW(CscdStr);
    CscdStr* s = (CscdStr*)slot->p;
    if (!inserted && cscdSvEq(cscdStrSv(s), text)) return;
    cscdStrSet(s, text);
    touch(m);
}

void cscdModelSetNumber(CscdModel* m, CscdSv name, double value) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.15g", value);
    cscdModelSet(m, name, cscdSv(buf));
}

void cscdModelSetBool(CscdModel* m, CscdSv name, bool value) { cscdModelSet(m, name, value ? CSCD_SV("true") : CSCD_SV("false")); }

CscdSv cscdModelGet(const CscdModel* m, CscdSv name) {
    const CscdMapVal* v = m ? cscdStrMapGet(&m->values, name) : NULL;
    return v ? cscdStrSv((const CscdStr*)v->p) : CSCD_SV("");
}

double cscdModelNumber(const CscdModel* m, CscdSv name) {
    char* c = cscdSvDupC(cscdModelGet(m, name));
    const double d = atof(c);
    cscdFree(c);
    return d;
}

static bool truthy(CscdSv s) { return s.len && !cscdSvEqC(s, "0") && !cscdSvEqC(s, "false"); }

bool cscdModelBool(const CscdModel* m, CscdSv name) { return truthy(cscdModelGet(m, name)); }

static CscdPtrVec* listOf(const CscdModel* m, CscdSv list, bool make) {
    CscdMapVal* v = cscdStrMapGet(&((CscdModel*)m)->lists, list);
    if (v) return (CscdPtrVec*)v->p;
    if (!make) return NULL;
    CscdPtrVec* items = CSCD_NEW(CscdPtrVec);
    cscdStrMapPut(&((CscdModel*)m)->lists, list, NULL)->p = items;
    return items;
}

CscdModel* cscdModelListInsert(CscdModel* m, CscdSv list, size_t at) {
    CscdPtrVec* items = listOf(m, list, true);
    CscdModel* item = cscdModelNew();
    item->parent = m;
    const size_t i = cscdMinz(at, items->len); /* (before the insert: it changes the length) */
    cscdVecInsert(items, i, item);
    touch(m);
    return item;
}

CscdModel* cscdModelListAdd(CscdModel* m, CscdSv list) { return cscdModelListInsert(m, list, (size_t)-1); }

void cscdModelListRemove(CscdModel* m, CscdSv list, size_t at) {
    CscdPtrVec* items = listOf(m, list, false);
    if (!items || at >= items->len) return;
    CscdModel* item = (CscdModel*)items->items[at];
    cscdVecRemove(items, at);
    item->parent = NULL;
    cscdRelease(item);
    touch(m);
}

void cscdModelListMove(CscdModel* m, CscdSv list, size_t from, size_t to) {
    CscdPtrVec* items = listOf(m, list, false);
    if (!items || from >= items->len) return;
    to = cscdMinz(to, items->len - 1);
    if (from == to) return;
    void* item = items->items[from];
    cscdVecRemove(items, from);
    cscdVecInsert(items, to, item);
    touch(m);
}

void cscdModelListClear(CscdModel* m, CscdSv list) {
    CscdPtrVec* items = listOf(m, list, false);
    if (!items || !items->len) return;
    for (size_t i = 0; i < items->len; ++i) {
        ((CscdModel*)items->items[i])->parent = NULL;
        cscdRelease(items->items[i]);
    }
    items->len = 0;
    touch(m);
}

size_t cscdModelListCount(const CscdModel* m, CscdSv list) {
    const CscdPtrVec* items = listOf(m, list, false);
    return items ? items->len : 0;
}

CscdModel* cscdModelListItem(const CscdModel* m, CscdSv list, size_t at) {
    const CscdPtrVec* items = listOf(m, list, false);
    return items && at < items->len ? (CscdModel*)items->items[at] : NULL;
}

CscdModel* cscdModelParent(const CscdModel* m) { return m->parent; }

void cscdModelOnChange(CscdModel* m, CscdModelFn fn, void* user) { cscdVecPush(&m->listeners, ((ModelListener){fn, user})); }

/* A value the page changed: set, and the listeners of its model and those above it told. */
static void setFromPage(CscdModel* m, CscdSv name, CscdSv text) {
    if (cscdSvEq(cscdModelGet(m, name), text) && cscdStrMapGet(&m->values, name)) return;
    cscdModelSet(m, name, text);
    cscdRetain(m);
    for (CscdModel* at = m; at; at = at->parent)
        for (size_t i = 0; i < at->listeners.len; ++i) at->listeners.items[i].fn(at->listeners.items[i].user, m, name);
    cscdRelease(m);
}

/* ---- bindings -------------------------------------------------------------------------------- */

/* A data-each: its container, and for each item shown its copy of the template. */
typedef struct Each {
    CscdElement* container;
    CscdPtrVec items; /* CscdModel* (references) */
    CscdPtrVec copies; /* CscdElement*, in the container (NULL: removed by someone else) */
} Each;

typedef struct Binding {
    CscdElement* root; /* NULL: the document */
    CscdModel* model;  /* (a reference) */
    uint64_t shown;    /* the model's version last shown */
    CscdPtrVec eaches; /* Each* */
} Binding;

static void eachFree(Each* e) {
    for (size_t i = 0; i < e->items.len; ++i) cscdRelease(e->items.items[i]);
    cscdVecFree(&e->items);
    cscdVecFree(&e->copies);
    cscdFree(e);
}

static void bindingFree(Binding* b) {
    for (size_t i = 0; i < b->eaches.len; ++i) eachFree((Each*)b->eaches.items[i]);
    cscdVecFree(&b->eaches);
    cscdRelease(b->model);
    cscdFree(b);
}

void viewBindingsClear(CscdView* v) {
    for (size_t i = 0; i < v->bindings.len; ++i) bindingFree((Binding*)v->bindings.items[i]);
    v->bindings.len = 0;
}

void cscdViewBind(CscdView* v, CscdElement* root, CscdModel* m) {
    cscdViewUnbind(v, root);
    Binding* b = CSCD_NEW(Binding);
    b->root = root;
    b->model = (CscdModel*)cscdRetain(m);
    b->shown = (uint64_t)-1; /* (shown at the next update) */
    cscdVecPush(&v->bindings, b);
    cscdViewInvalidate(v);
}

void cscdViewUnbind(CscdView* v, CscdElement* root) {
    for (size_t i = v->bindings.len; i-- > 0;) {
        Binding* b = (Binding*)v->bindings.items[i];
        if (b->root != root) continue;
        bindingFree(b);
        cscdVecRemove(&v->bindings, i);
    }
}

/* An element leaving the document: a binding on it ends; a copy of a data-each is forgotten. */
void viewBindingsNodeRemoved(CscdView* v, CscdNode* n) {
    for (size_t i = v->bindings.len; i-- > 0;) {
        Binding* b = (Binding*)v->bindings.items[i];
        if ((CscdNode*)b->root == n) {
            bindingFree(b);
            cscdVecRemove(&v->bindings, i);
            continue;
        }
        for (size_t k = b->eaches.len; k-- > 0;) {
            Each* e = (Each*)b->eaches.items[k];
            if ((CscdNode*)e->container == n) {
                eachFree(e);
                cscdVecRemove(&b->eaches, k);
                continue;
            }
            for (size_t c = 0; c < e->copies.len; ++c)
                if ((CscdNode*)e->copies.items[c] == n) e->copies.items[c] = NULL;
        }
    }
}

static Each* eachFor(Binding* b, CscdElement* container) {
    for (size_t i = 0; i < b->eaches.len; ++i)
        if (((Each*)b->eaches.items[i])->container == container) return (Each*)b->eaches.items[i];
    Each* e = CSCD_NEW(Each);
    e->container = container;
    cscdVecPush(&b->eaches, e);
    return e;
}

/* "!name": the name, and whether the test is turned around */
static CscdSv nameOf(CscdSv attr, bool* negate) {
    *negate = attr.len && attr.ptr[0] == '!';
    return *negate ? cscdSvFrom(attr, 1) : attr;
}

static bool test(const CscdModel* m, CscdSv attr) {
    bool negate;
    const CscdSv name = nameOf(attr, &negate);
    return cscdModelBool(m, name) != negate;
}

static void applyTree(Binding* b, CscdElement* e, CscdModel* m);

/* data-each: one copy of the template per item, the copies of items still there kept (and moved
 * into the list's order), the others made or removed. */
static void applyEach(Binding* b, CscdElement* container, CscdModel* m, CscdSv list) {
    CscdElement* tpl = NULL;
    for (size_t i = 0; i < container->node.children.len && !tpl; ++i) {
        CscdElement* c = cscdNodeAsElement(cscdNodeChild(&container->node, i));
        if (c && cscdElementIs(c, "template")) tpl = c;
    }
    if (!tpl) return;
    Each* each = eachFor(b, container);
    const CscdPtrVec* items = listOf(m, list, false);
    const size_t n = items ? items->len : 0;
    /* the copies of items gone */
    for (size_t i = each->items.len; i-- > 0;) {
        bool still = false;
        for (size_t k = 0; k < n && !still; ++k) still = items->items[k] == each->items.items[i];
        CscdElement* copy = (CscdElement*)each->copies.items[i];
        if (!still || !copy) {
            if (copy) cscdElementRemove(copy);
            cscdRelease(each->items.items[i]);
            cscdVecRemove(&each->items, i);
            cscdVecRemove(&each->copies, i);
        }
    }
    /* the list's order: each item's copy (kept or new), placed after the one before it */
    CscdPtrVec items2 = {0}, copies2 = {0};
    CscdNode* after = &tpl->node;
    for (size_t k = 0; k < n; ++k) {
        CscdModel* item = (CscdModel*)items->items[k];
        CscdElement* copy = NULL;
        for (size_t i = 0; i < each->items.len && !copy; ++i)
            if (each->items.items[i] == item) copy = (CscdElement*)each->copies.items[i];
        if (!copy) {
            copy = cscdTemplateInstantiate(tpl);
            if (!copy) break;
        }
        CscdNode* next = cscdNodeNextSibling(after);
        if (copy->node.parent != &container->node || next != &copy->node) cscdNodeInsertBefore(&container->node, &copy->node, next);
        after = &copy->node;
        cscdVecPush(&items2, cscdRetain(item));
        cscdVecPush(&copies2, copy);
    }
    for (size_t i = 0; i < each->items.len; ++i) cscdRelease(each->items.items[i]);
    cscdVecFree(&each->items);
    cscdVecFree(&each->copies);
    each->items = items2;
    each->copies = copies2;
    for (size_t k = 0; k < each->copies.len; ++k) applyTree(b, (CscdElement*)each->copies.items[k], (CscdModel*)each->items.items[k]);
}

/* One element's data-* attributes, against model m. */
static void applyElement(CscdElement* e, CscdModel* m) {
    for (size_t i = 0; i < e->attributes.len; ++i) {
        const CscdSv name = cscdStrSv(&e->attributes.items[i].name), value = cscdStrSv(&e->attributes.items[i].value);
        if (!cscdSvStartsC(name, "data-")) continue;
        bool negate;
        const CscdSv field = nameOf(value, &negate);
        if (cscdSvEqC(name, "data-text")) {
            const CscdSv t = cscdModelGet(m, field);
            CscdStr now = {0};
            cscdNodeTextContent(&e->node, &now);
            if (!cscdSvEq(cscdStrSv(&now), t)) cscdElementSetText(e, t);
            cscdStrFree(&now);
        } else if (cscdSvEqC(name, "data-value")) {
            const CscdSv t = cscdModelGet(m, field);
            CscdStr now = cscdElementValue(e);
            if (!cscdSvEq(cscdStrSv(&now), t)) cscdElementSetValue(e, t);
            cscdStrFree(&now);
        } else if (cscdSvEqC(name, "data-checked")) {
            const bool on = test(m, value);
            if (cscdElementChecked(e) != on) cscdElementSetChecked(e, on);
        } else if (cscdSvEqC(name, "data-show") || cscdSvEqC(name, "data-hide")) {
            const bool shown = test(m, value) == cscdSvEqC(name, "data-show");
            if (shown == cscdElementHasAttributeC(e, "hidden")) {
                if (shown) cscdElementRemoveAttribute(e, CSCD_SV("hidden"));
                else cscdElementSetAttribute(e, CSCD_SV("hidden"), CSCD_SV(""));
            }
        } else if (cscdSvStartsC(name, "data-class-") && name.len > 11) {
            const CscdSv cls = cscdSvFrom(name, 11);
            const bool on = test(m, value);
            if (cscdElementHasClass(e, cls) != on) cscdElementSetClass(e, cls, on);
        } else if (cscdSvStartsC(name, "data-attr-") && name.len > 10) {
            CscdStr attr = cscdStrDup(cscdSvFrom(name, 10)), t = cscdStrDup(cscdModelGet(m, field));
            const CscdStr* now = cscdElementGetAttribute(e, cscdStrSv(&attr));
            if (!t.len) { if (now) cscdElementRemoveAttribute(e, cscdStrSv(&attr)); }
            else if (!now || !cscdStrEq(now, &t)) cscdElementSetAttribute(e, cscdStrSv(&attr), cscdStrSv(&t));
            cscdStrFree(&attr);
            cscdStrFree(&t);
        }
    }
}

static void applyTree(Binding* b, CscdElement* e, CscdModel* m) {
    applyElement(e, m); /* (attribute changes above do not change the children: walked after) */
    if (cscdElementHasAttributeC(e, "data-each")) {
        CscdStr list = cscdStrDup(cscdElementAttrC(e, "data-each"));
        applyEach(b, e, m, cscdStrSv(&list));
        cscdStrFree(&list);
        return;
    }
    for (size_t i = 0; i < e->node.children.len; ++i) {
        CscdElement* c = cscdNodeAsElement(cscdNodeChild(&e->node, i));
        if (c && !cscdElementIs(c, "template")) applyTree(b, c, m);
    }
}

void viewApplyBindings(CscdView* v) {
    for (size_t i = 0; i < v->bindings.len; ++i) {
        Binding* b = (Binding*)v->bindings.items[i];
        if (b->shown == b->model->version) continue;
        b->shown = b->model->version;
        CscdElement* root = b->root ? b->root : cscdDocumentElement(v->doc);
        if (root) applyTree(b, root, b->model);
    }
}

/* The model an element is bound to: the item of the data-each copy it is in, else its binding's. */
static CscdModel* modelFor(CscdView* v, CscdElement* e, Binding** out) {
    for (CscdElement* a = e; a; a = cscdNodeParentElement(&a->node))
        for (size_t i = 0; i < v->bindings.len; ++i) {
            Binding* b = (Binding*)v->bindings.items[i];
            for (size_t k = 0; k < b->eaches.len; ++k) {
                const Each* each = (const Each*)b->eaches.items[k];
                for (size_t c = 0; c < each->copies.len; ++c)
                    if (each->copies.items[c] == a) { *out = b; return (CscdModel*)each->items.items[c]; }
            }
            if (b->root == a || (!b->root && !cscdNodeParentElement(&a->node))) { *out = b; return b->model; }
        }
    return NULL;
}

CscdModel* cscdViewModelOf(CscdView* v, CscdElement* e) {
    Binding* b;
    CscdModel* m = e ? modelFor(v, e, &b) : NULL;
    return m && b && m != b->model ? m : NULL;
}

/* A field typed in, or a box ticked: its data-value or data-checked sets the model first, so the
 * page's listeners see the model as it is now. */
void viewBindingsFromPage(CscdView* v, CscdElement* e) {
    if (!v->bindings.len || !(cscdElementHasAttributeC(e, "data-value") || cscdElementHasAttributeC(e, "data-checked"))) return;
    Binding* b = NULL;
    CscdModel* m = modelFor(v, e, &b);
    if (!m) return;
    bool negate;
    if (cscdElementHasAttributeC(e, "data-value")) {
        CscdStr name = cscdStrDup(nameOf(cscdElementAttrC(e, "data-value"), &negate));
        CscdStr value = cscdElementValue(e);
        setFromPage(m, cscdStrSv(&name), cscdStrSv(&value));
        cscdStrFree(&value);
        cscdStrFree(&name);
    }
    if (cscdElementHasAttributeC(e, "data-checked")) {
        CscdStr name = cscdStrDup(nameOf(cscdElementAttrC(e, "data-checked"), &negate));
        setFromPage(m, cscdStrSv(&name), cscdElementChecked(e) != negate ? CSCD_SV("true") : CSCD_SV("false"));
        cscdStrFree(&name);
    }
}
