/* The document object model: nodes, elements, attributes, classes, events, form values, queries. */
#include "cascade/dom.h"
#include "cascade/css.h"
#include "cascade/style.h"

/* ---- small things --------------------------------------------------------------------------------- */

void cscdStrPairVecFree(CscdStrPairVec* v) {
    for (size_t i = 0; i < v->len; ++i) { cscdStrFree(&v->items[i].name); cscdStrFree(&v->items[i].value); }
    cscdVecFree(v);
}

void cscdStrPairVecCopy(CscdStrPairVec* dst, const CscdStrPairVec* src) {
    for (size_t i = 0; i < src->len; ++i) {
        CscdStrPair p = {cscdStrCopy(&src->items[i].name), cscdStrCopy(&src->items[i].value)};
        cscdVecPush(dst, p);
    }
}

static void dataTransferDtor(void* p) {
    CscdDataTransfer* d = (CscdDataTransfer*)p;
    cscdStrPairVecFree(&d->items);
    cscdStrVecFree(&d->files);
    cscdStrFree(&d->effectAllowed);
    cscdStrFree(&d->dropEffect);
    cscdRelease(d->dragImage);
}

CscdDataTransfer* cscdDataTransferNew(void) {
    CscdDataTransfer* d = (CscdDataTransfer*)cscdRcNew(sizeof(CscdDataTransfer), dataTransferDtor);
    d->effectAllowed = cscdStrDupC("all");
    d->dropEffect = cscdStrDupC("move");
    return d;
}

void cscdDataTransferSetData(CscdDataTransfer* d, CscdSv type, CscdSv data) {
    for (size_t i = 0; i < d->items.len; ++i)
        if (cscdSvEq(cscdStrSv(&d->items.items[i].name), type)) {
            cscdStrSet(&d->items.items[i].value, data);
            return;
        }
    CscdStrPair p = {cscdStrDup(type), cscdStrDup(data)};
    cscdVecPush(&d->items, p);
}

CscdSv cscdDataTransferGetData(const CscdDataTransfer* d, CscdSv type) {
    for (size_t i = 0; i < d->items.len; ++i)
        if (cscdSvEq(cscdStrSv(&d->items.items[i].name), type)) return cscdStrSv(&d->items.items[i].value);
    if (cscdSvEqC(type, "text")) return cscdDataTransferGetData(d, CSCD_SV("text/plain"));
    return CSCD_SV("");
}

bool cscdDataTransferHasType(const CscdDataTransfer* d, CscdSv type) {
    if (cscdSvEqC(type, "Files")) return d->files.len > 0;
    for (size_t i = 0; i < d->items.len; ++i)
        if (cscdSvEq(cscdStrSv(&d->items.items[i].name), type)) return true;
    return false;
}

void cscdDataTransferClearData(CscdDataTransfer* d) {
    for (size_t i = 0; i < d->items.len; ++i) { cscdStrFree(&d->items.items[i].name); cscdStrFree(&d->items.items[i].value); }
    d->items.len = 0;
}

void cscdDataTransferSetDragImage(CscdDataTransfer* d, CscdImage* image, float x, float y) {
    cscdRetain(image);
    cscdRelease(d->dragImage);
    d->dragImage = image;
    d->imageX = x;
    d->imageY = y;
}

void cscdEventInit(CscdEvent* ev, CscdSv type) {
    memset(ev, 0, sizeof *ev);
    ev->type = cscdStrDup(type);
    ev->pointerType = cscdStrDupC("mouse");
}

void cscdEventFini(CscdEvent* ev) {
    cscdStrFree(&ev->type);
    cscdStrFree(&ev->key);
    cscdStrFree(&ev->pointerType);
    cscdRelease(ev->dataTransfer);
    ev->dataTransfer = NULL;
    cscdVecFree(&ev->touches);
    cscdVecFree(&ev->changedTouches);
}

/* ---- nodes ----------------------------------------------------------------------------------------- */

CscdText* cscdTextNew(CscdSv data) {
    CscdText* t = CSCD_NEW(CscdText);
    t->node.type = CSCD_NODE_TEXT;
    t->data = cscdStrDup(data);
    return t;
}

CscdComment* cscdCommentNew(CscdSv data) {
    CscdComment* c = CSCD_NEW(CscdComment);
    c->node.type = CSCD_NODE_COMMENT;
    c->data = cscdStrDup(data);
    return c;
}

CscdElement* cscdElementNew(CscdSv tag) {
    CscdElement* e = CSCD_NEW(CscdElement);
    e->node.type = CSCD_NODE_ELEMENT;
    e->tag = cscdLower(tag);
    return e;
}

static void dropCallback(void** user, CscdFreeFn* freeUser) {
    if (*freeUser) (*freeUser)(*user);
    *user = NULL;
    *freeUser = NULL;
}

static void freeNode(CscdNode* n);

static void freeChildren(CscdNode* n) {
    for (size_t i = 0; i < n->children.len; ++i) freeNode(cscdNodeChild(n, i));
    cscdVecFree(&n->children);
}

static void freeElement(CscdElement* e) {
    cscdStrFree(&e->tag);
    cscdStrPairVecFree(&e->attributes);
    for (size_t i = 0; i < e->listeners.len; ++i) {
        CscdListener* l = &e->listeners.items[i];
        cscdStrFree(&l->type);
        dropCallback(&l->handler.user, &l->handler.freeUser);
    }
    cscdVecFree(&e->listeners);
    cscdRelease(e->style);
    cscdStrFree(&e->editValue);
    cscdStrVecFree(&e->classes_);
    dropCallback(&e->painter_.user, &e->painter_.freeUser);
    dropCallback(&e->highlighter_.user, &e->highlighter_.freeUser);
    cscdVecFree(&e->spans_);
    cscdStrFree(&e->highlightFor_);
    if (e->templateContent_) freeNode(&e->templateContent_->node);
}

static void freeDocument(CscdDocument* d);

static void freeNode(CscdNode* n) {
    freeChildren(n);
    switch (n->type) {
    case CSCD_NODE_ELEMENT: freeElement((CscdElement*)n); break;
    case CSCD_NODE_TEXT: cscdStrFree(&((CscdText*)n)->data); break;
    case CSCD_NODE_COMMENT: cscdStrFree(&((CscdComment*)n)->data); break;
    case CSCD_NODE_DOCUMENT: freeDocument((CscdDocument*)n); break;
    }
    cscdFree(n);
}

void cscdNodeDestroy(CscdNode* n) {
    if (!n) return;
    CscdDocument* d = n->owner;
    if (d && n != &d->node && d->dispatchDepth > 0) {
        cscdVecPush(&d->graveyard, n);
        return;
    }
    freeNode(n);
}

static void setOwner(CscdNode* n, CscdDocument* d) {
    n->owner = d;
    for (size_t i = 0; i < n->children.len; ++i) setOwner(cscdNodeChild(n, i), d);
}

static void notifyRemoved(CscdDocument* d, CscdNode* n) {
    if (!d) return;
    if ((CscdNode*)d->focused == n) d->focused = NULL;
    if ((CscdNode*)d->hovered == n) d->hovered = NULL;
    if ((CscdNode*)d->active == n) d->active = NULL;
    if (d->onNodeRemoved) d->onNodeRemoved(d->onNodeRemovedUser, n);
    for (size_t i = 0; i < n->children.len; ++i) notifyRemoved(d, cscdNodeChild(n, i));
}

CscdElement* cscdNodeParentElement(const CscdNode* n) { return n->parent && n->parent->type == CSCD_NODE_ELEMENT ? (CscdElement*)n->parent : NULL; }

static CscdDocument* docOf(CscdNode* n) { return n->type == CSCD_NODE_DOCUMENT ? (CscdDocument*)n : n->owner; }

CscdNode* cscdNodeAppendChild(CscdNode* parent, CscdNode* child) {
    if (child->parent) cscdNodeRemoveChild(child->parent, child);
    child->parent = parent;
    setOwner(child, docOf(parent));
    child->indexHint = parent->children.len;
    cscdVecPush(&parent->children, child);
    cscdNodeMarkDirty(parent);
    return child;
}

CscdNode* cscdNodeInsertBefore(CscdNode* parent, CscdNode* child, CscdNode* ref) {
    if (!ref) return cscdNodeAppendChild(parent, child);
    if (child->parent) cscdNodeRemoveChild(child->parent, child);
    child->parent = parent;
    setOwner(child, docOf(parent));
    const int at = ref->parent == parent ? cscdNodeIndexInParent(ref) : -1;
    if (at >= 0) {
        cscdVecInsert(&parent->children, (size_t)at, child);
        child->indexHint = (size_t)at;
        cscdNodeMarkDirty(parent);
        return child;
    }
    child->indexHint = parent->children.len;
    cscdVecPush(&parent->children, child);
    cscdNodeMarkDirty(parent);
    return child;
}

CscdNode* cscdNodeRemoveChild(CscdNode* parent, CscdNode* child) {
    const int i = child->parent == parent ? cscdNodeIndexInParent(child) : -1;
    if (i < 0) return NULL;
    cscdVecRemove(&parent->children, (size_t)i);
    notifyRemoved(parent->owner, child);
    child->parent = NULL;
    cscdNodeMarkDirty(parent);
    return child;
}

void cscdNodeClearChildren(CscdNode* n) {
    for (size_t i = 0; i < n->children.len; ++i) notifyRemoved(n->owner, cscdNodeChild(n, i));
    for (size_t i = 0; i < n->children.len; ++i) {
        CscdNode* c = cscdNodeChild(n, i);
        c->parent = NULL;
        cscdNodeDestroy(c);
    }
    n->children.len = 0;
    cscdNodeMarkDirty(n);
}

int cscdNodeIndexInParent(const CscdNode* n) {
    const CscdNode* p = n->parent;
    if (!p) return -1;
    /* where it was last (insertions and removals before it move it: then it is looked for, and noted) */
    const size_t hint = n->indexHint;
    if (hint < p->children.len && cscdNodeChild(p, hint) == n) return (int)hint;
    for (size_t i = 0; i < p->children.len; ++i)
        if (cscdNodeChild(p, i) == n) {
            ((CscdNode*)n)->indexHint = i;
            return (int)i;
        }
    return -1;
}

CscdNode* cscdNodePreviousSibling(const CscdNode* n) {
    int i = cscdNodeIndexInParent(n);
    return i > 0 ? cscdNodeChild(n->parent, (size_t)i - 1) : NULL;
}

CscdNode* cscdNodeNextSibling(const CscdNode* n) {
    int i = cscdNodeIndexInParent(n);
    return i >= 0 && (size_t)i + 1 < n->parent->children.len ? cscdNodeChild(n->parent, (size_t)i + 1) : NULL;
}

CscdElement* cscdNodePreviousElementSibling(const CscdNode* n) {
    for (int i = cscdNodeIndexInParent(n) - 1; i >= 0; --i) {
        CscdNode* c = cscdNodeChild(n->parent, (size_t)i);
        if (cscdNodeIsElement(c)) {
            c->indexHint = (size_t)i;
            return (CscdElement*)c;
        }
    }
    return NULL;
}

CscdElement* cscdNodeNextElementSibling(const CscdNode* n) {
    const int at = cscdNodeIndexInParent(n);
    if (at < 0) return NULL;
    for (size_t i = (size_t)at + 1; i < n->parent->children.len; ++i) {
        CscdNode* c = cscdNodeChild(n->parent, i);
        if (cscdNodeIsElement(c)) {
            c->indexHint = i;
            return (CscdElement*)c;
        }
    }
    return NULL;
}

CscdElement* cscdNodeFirstElementChild(const CscdNode* n) {
    for (size_t i = 0; i < n->children.len; ++i)
        if (cscdNodeIsElement(cscdNodeChild(n, i))) return (CscdElement*)cscdNodeChild(n, i);
    return NULL;
}

void cscdNodeTextContent(const CscdNode* n, CscdStr* out) {
    if (n->type == CSCD_NODE_TEXT) {
        cscdStrAppend(out, cscdStrSv(&((const CscdText*)n)->data));
        return;
    }
    for (size_t i = 0; i < n->children.len; ++i) {
        const CscdNode* c = cscdNodeChild(n, i);
        if (c->type != CSCD_NODE_COMMENT) cscdNodeTextContent(c, out);
    }
}

void cscdNodeMarkDirty(CscdNode* n) {
    CscdDocument* d = docOf(n);
    if (d) {
        ++d->version;
        d->structural = true;
    }
}

/* ---- elements -------------------------------------------------------------------------------------- */

const CscdStr* cscdElementGetAttribute(const CscdElement* e, CscdSv name) {
    for (size_t i = 0; i < e->attributes.len; ++i)
        if (cscdSvEq(cscdStrSv(&e->attributes.items[i].name), name)) return &e->attributes.items[i].value;
    return NULL;
}

CscdSv cscdElementAttr(const CscdElement* e, CscdSv name) {
    const CscdStr* v = cscdElementGetAttribute(e, name);
    return v ? cscdStrSv(v) : CSCD_SV("");
}

/* an attribute only the element's painting reads (a progress bar's or a meter's numbers) */
static bool paintOnlyAttribute(const CscdElement* e, const CscdStr* name) {
    if (cscdElementIs(e, "progress")) return cscdStrEqC(name, "value") || cscdStrEqC(name, "max");
    if (cscdElementIs(e, "meter"))
        return cscdStrEqC(name, "value") || cscdStrEqC(name, "max") || cscdStrEqC(name, "min") || cscdStrEqC(name, "low") ||
               cscdStrEqC(name, "high") || cscdStrEqC(name, "optimum");
    return false;
}

void cscdElementSetAttribute(CscdElement* e, CscdSv name, CscdSv value) {
    CscdStr n = cscdLower(name);
    if (cscdStrEqC(&n, "class")) e->classesValid_ = false;
    for (size_t i = 0; i < e->attributes.len; ++i) {
        CscdStrPair* a = &e->attributes.items[i];
        if (cscdStrEq(&a->name, &n)) {
            const bool paintOnly = paintOnlyAttribute(e, &n) && e->node.owner;
            cscdStrFree(&n);
            if (cscdSvEq(cscdStrSv(&a->value), value)) return;
            cscdStrSet(&a->value, value);
            if (paintOnly) { /* (a value that changes every frame does not restyle the page each time) */
                ++e->node.owner->version;
                cscdVecPush(&e->node.owner->paintChanged, e);
            } else cscdNodeMarkDirty(&e->node);
            return;
        }
    }
    bool checked = cscdStrEqC(&n, "checked");
    CscdStrPair p = {n, cscdStrDup(value)};
    cscdVecPush(&e->attributes, p);
    if (checked && cscdElementIs(e, "input")) e->state |= CSCD_STATE_CHECKED;
    cscdNodeMarkDirty(&e->node);
}

void cscdElementRemoveAttribute(CscdElement* e, CscdSv name) {
    for (size_t i = 0; i < e->attributes.len; ++i) {
        if (cscdSvEq(cscdStrSv(&e->attributes.items[i].name), name)) {
            if (cscdSvEqC(name, "class")) e->classesValid_ = false;
            cscdStrFree(&e->attributes.items[i].name);
            cscdStrFree(&e->attributes.items[i].value);
            cscdVecRemove(&e->attributes, i);
            cscdNodeMarkDirty(&e->node);
            return;
        }
    }
}

const CscdStrVec* cscdElementClassList(const CscdElement* ce) {
    CscdElement* e = (CscdElement*)ce; /* (a cache) */
    if (!e->classesValid_) {
        for (size_t i = 0; i < e->classes_.len; ++i) cscdStrFree(&e->classes_.items[i]);
        e->classes_.len = 0;
        const CscdStr* c = cscdElementGetAttribute(e, CSCD_SV("class"));
        if (c) {
            CscdSvVec words = {0};
            cscdSplitSpaces(cscdStrSv(c), &words);
            for (size_t i = 0; i < words.len; ++i) {
                bool dup = false;
                for (size_t k = 0; k < e->classes_.len && !dup; ++k) dup = cscdSvEq(cscdStrSv(&e->classes_.items[k]), words.items[i]);
                if (!dup) cscdVecPush(&e->classes_, cscdStrDup(words.items[i]));
            }
            cscdVecFree(&words);
        }
        e->classesValid_ = true;
    }
    return &e->classes_;
}

bool cscdElementHasClass(const CscdElement* e, CscdSv c) {
    const CscdStrVec* l = cscdElementClassList(e);
    for (size_t i = 0; i < l->len; ++i)
        if (cscdSvEq(cscdStrSv(&l->items[i]), c)) return true;
    return false;
}

void cscdElementAddClass(CscdElement* e, CscdSv c) {
    if (cscdElementHasClass(e, c)) return;
    CscdStr cur = cscdStrDup(cscdElementAttrC(e, "class"));
    if (cur.len) cscdStrAppendChar(&cur, ' ');
    cscdStrAppend(&cur, c);
    cscdElementSetAttribute(e, CSCD_SV("class"), cscdStrSv(&cur));
    cscdStrFree(&cur);
}

void cscdElementRemoveClass(CscdElement* e, CscdSv c) {
    if (!cscdElementHasClass(e, c)) return;
    CscdStr out = {0};
    const CscdStrVec* l = cscdElementClassList(e);
    for (size_t i = 0; i < l->len; ++i) {
        if (cscdSvEq(cscdStrSv(&l->items[i]), c)) continue;
        if (out.len) cscdStrAppendChar(&out, ' ');
        cscdStrAppend(&out, cscdStrSv(&l->items[i]));
    }
    cscdElementSetAttribute(e, CSCD_SV("class"), cscdStrSv(&out));
    cscdStrFree(&out);
}

bool cscdElementToggleClass(CscdElement* e, CscdSv c) {
    if (cscdElementHasClass(e, c)) {
        cscdElementRemoveClass(e, c);
        return false;
    }
    cscdElementAddClass(e, c);
    return true;
}

void cscdElementSetStyle(CscdElement* e, CscdSv property, CscdSv value) {
    CscdDeclVec decls = {0};
    cscdParseDeclarations(cscdElementAttrC(e, "style"), &decls);
    CscdStr p = cscdLower(property);
    bool found = false;
    for (size_t i = 0; i < decls.len; ++i)
        if (cscdStrEq(&decls.items[i].property, &p)) { cscdStrSet(&decls.items[i].value, value); found = true; }
    if (!found && value.len) {
        CscdDeclaration d = {cscdStrCopy(&p), cscdStrDup(value), false};
        cscdVecPush(&decls, d);
    }
    CscdStr out = {0};
    for (size_t i = 0; i < decls.len; ++i) {
        const CscdDeclaration* d = &decls.items[i];
        if (!d->value.len) continue;
        cscdStrAppend(&out, cscdStrSv(&d->property));
        cscdStrAppendC(&out, ": ");
        cscdStrAppend(&out, cscdStrSv(&d->value));
        if (d->important) cscdStrAppendC(&out, " !important");
        cscdStrAppendC(&out, "; ");
    }
    cscdElementSetAttribute(e, CSCD_SV("style"), cscdSvTrim(cscdStrSv(&out)));
    cscdStrFree(&out);
    cscdStrFree(&p);
    cscdDeclVecFree(&decls);
}

CscdStr cscdElementGetStyle(const CscdElement* e, CscdSv property) {
    CscdDeclVec decls = {0};
    cscdParseDeclarations(cscdElementAttrC(e, "style"), &decls);
    CscdStr r = {0};
    for (size_t i = 0; i < decls.len; ++i)
        if (cscdSvEq(cscdStrSv(&decls.items[i].property), property)) { r = cscdStrCopy(&decls.items[i].value); break; }
    cscdDeclVecFree(&decls);
    return r;
}

void cscdElementSetText(CscdElement* e, CscdSv text) {
    /* one text replacing another: the styles stay (an empty one would change :empty) */
    CscdNode* n = &e->node;
    if (n->children.len == 1 && cscdNodeIsText(cscdNodeChild(n, 0)) && n->owner && text.len) {
        CscdText* t = (CscdText*)cscdNodeChild(n, 0);
        if (cscdSvEq(cscdStrSv(&t->data), text)) return;
        if (t->data.len) {
            cscdStrSet(&t->data, text);
            ++n->owner->version;
            cscdVecPush(&n->owner->textChanged, e);
            return;
        }
    }
    cscdNodeClearChildren(n);
    cscdNodeAppendChild(n, &cscdTextNew(text)->node);
}

/* ---- templates and copies -------------------------------------------------------------------------- */

void cscdDomAdoptTemplates(CscdNode* root) {
    for (size_t i = 0; i < root->children.len; ++i) cscdDomAdoptTemplates(cscdNodeChild(root, i));
    CscdElement* e = cscdNodeAsElement(root);
    if (!e || !cscdElementIs(e, "template") || !root->children.len) return;
    if (!e->templateContent_) e->templateContent_ = cscdElementNew(CSCD_SV("template-content"));
    while (root->children.len) {
        CscdNode* c = cscdNodeChild(root, 0);
        cscdVecRemove(&root->children, 0);
        c->parent = NULL;
        setOwner(c, NULL);
        cscdNodeAppendChild(&e->templateContent_->node, c);
    }
}

static CscdNode* cloneNode(const CscdNode* n, bool deep) {
    CscdNode* out = NULL;
    switch (n->type) {
    case CSCD_NODE_TEXT: out = &cscdTextNew(cscdStrSv(&((const CscdText*)n)->data))->node; break;
    case CSCD_NODE_COMMENT: out = &cscdCommentNew(cscdStrSv(&((const CscdComment*)n)->data))->node; break;
    case CSCD_NODE_ELEMENT: {
        const CscdElement* e = (const CscdElement*)n;
        CscdElement* c = cscdElementNew(cscdStrSv(&e->tag));
        cscdStrPairVecCopy(&c->attributes, &e->attributes);
        if (deep && e->templateContent_) c->templateContent_ = (CscdElement*)cloneNode(&e->templateContent_->node, true);
        out = &c->node;
        break;
    }
    default: return NULL; /* (a document is not copied) */
    }
    if (deep)
        for (size_t i = 0; i < n->children.len; ++i) {
            CscdNode* c = cloneNode(cscdNodeChild(n, i), true);
            if (c) cscdNodeAppendChild(out, c);
        }
    return out;
}

CscdElement* cscdElementClone(const CscdElement* e, bool deep) { return e ? (CscdElement*)cloneNode(&e->node, deep) : NULL; }

CscdElement* cscdTemplateContent(CscdElement* tpl) {
    if (!tpl) return NULL;
    if (!tpl->templateContent_) tpl->templateContent_ = cscdElementNew(CSCD_SV("template-content"));
    return tpl->templateContent_;
}

CscdElement* cscdTemplateInstantiate(CscdElement* tpl) {
    CscdElement* first = cscdNodeFirstElementChild(&cscdTemplateContent(tpl)->node);
    return first ? cscdElementClone(first, true) : NULL;
}

static bool replaceAll(CscdStr* s, CscdSv pattern, CscdSv value) {
    if (cscdSvFind(cscdStrSv(s), pattern, 0) == CSCD_NPOS) return false;
    CscdStr out = {0};
    size_t at = 0;
    for (size_t hit; (hit = cscdSvFind(cscdStrSv(s), pattern, at)) != CSCD_NPOS; at = hit + pattern.len) {
        cscdStrAppend(&out, cscdSvSub(cscdStrSv(s), at, hit - at));
        cscdStrAppend(&out, value);
    }
    cscdStrAppend(&out, cscdSvFrom(cscdStrSv(s), at));
    cscdStrFree(s);
    *s = out;
    return true;
}

static void fillNode(CscdNode* n, CscdSv pattern, CscdSv value, bool* changed) {
    if (n->type == CSCD_NODE_TEXT) {
        if (replaceAll(&((CscdText*)n)->data, pattern, value)) *changed = true;
    } else if (n->type == CSCD_NODE_ELEMENT) {
        CscdElement* e = (CscdElement*)n;
        for (size_t i = 0; i < e->attributes.len; ++i) {
            CscdStr* v = &e->attributes.items[i].value;
            if (!replaceAll(v, pattern, value)) continue;
            if (cscdStrEqC(&e->attributes.items[i].name, "class")) e->classesValid_ = false;
            *changed = true;
        }
    }
    for (size_t i = 0; i < n->children.len; ++i) fillNode(cscdNodeChild(n, i), pattern, value, changed);
}

void cscdElementFill(CscdElement* e, CscdSv key, CscdSv value) {
    if (!e || !key.len) return;
    CscdStr pattern = cscdStrDupC("{{");
    cscdStrAppend(&pattern, key);
    cscdStrAppendC(&pattern, "}}");
    bool changed = false;
    fillNode(&e->node, cscdStrSv(&pattern), value, &changed);
    cscdStrFree(&pattern);
    if (changed && e->node.owner) cscdNodeMarkDirty(&e->node);
}

void cscdElementSetInnerHTML(CscdElement* e, CscdSv html) {
    cscdNodeClearChildren(&e->node);
    cscdParseHTMLFragment(html, e);
    cscdNodeMarkDirty(&e->node);
}

static const char* kVoidTags[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"};

static void serialize(const CscdNode* n, CscdStr* out) {
    if (n->type == CSCD_NODE_TEXT) {
        const CscdElement* p = cscdNodeParentElement(n);
        bool raw = p && (cscdElementIs(p, "script") || cscdElementIs(p, "style"));
        if (raw) cscdStrAppend(out, cscdStrSv(&((const CscdText*)n)->data));
        else cscdEscapeHTML(cscdStrSv(&((const CscdText*)n)->data), false, out);
        return;
    }
    if (n->type == CSCD_NODE_COMMENT) {
        cscdStrAppendC(out, "<!--");
        cscdStrAppend(out, cscdStrSv(&((const CscdComment*)n)->data));
        cscdStrAppendC(out, "-->");
        return;
    }
    if (n->type != CSCD_NODE_ELEMENT) {
        for (size_t i = 0; i < n->children.len; ++i) serialize(cscdNodeChild(n, i), out);
        return;
    }
    const CscdElement* e = (const CscdElement*)n;
    cscdStrAppendChar(out, '<');
    cscdStrAppend(out, cscdStrSv(&e->tag));
    for (size_t i = 0; i < e->attributes.len; ++i) {
        cscdStrAppendChar(out, ' ');
        cscdStrAppend(out, cscdStrSv(&e->attributes.items[i].name));
        cscdStrAppendC(out, "=\"");
        cscdEscapeHTML(cscdStrSv(&e->attributes.items[i].value), true, out);
        cscdStrAppendChar(out, '"');
    }
    cscdStrAppendChar(out, '>');
    for (size_t i = 0; i < CSCD_COUNT(kVoidTags); ++i)
        if (cscdElementIs(e, kVoidTags[i])) return;
    const CscdNode* kids = e->templateContent_ ? &e->templateContent_->node : n; /* (a template: its content) */
    for (size_t i = 0; i < kids->children.len; ++i) serialize(cscdNodeChild(kids, i), out);
    cscdStrAppendC(out, "</");
    cscdStrAppend(out, cscdStrSv(&e->tag));
    cscdStrAppendChar(out, '>');
}

CscdStr cscdElementInnerHTML(const CscdElement* e) {
    CscdStr out = {0};
    const CscdNode* kids = e->templateContent_ ? &e->templateContent_->node : &e->node;
    for (size_t i = 0; i < kids->children.len; ++i) serialize(cscdNodeChild(kids, i), &out);
    return out;
}

CscdStr cscdElementOuterHTML(const CscdElement* e) {
    CscdStr out = {0};
    serialize(&e->node, &out);
    return out;
}

CscdElement* cscdElementAppendElement(CscdElement* e, CscdSv tag) {
    return (CscdElement*)cscdNodeAppendChild(&e->node, &cscdElementNew(tag)->node);
}

CscdText* cscdElementAppendText(CscdElement* e, CscdSv text) { return (CscdText*)cscdNodeAppendChild(&e->node, &cscdTextNew(text)->node); }

static const CscdElement* findSelected(const CscdElement* p, const CscdElement** first) {
    for (size_t i = 0; i < p->node.children.len; ++i) {
        const CscdElement* ce = cscdNodeAsElementC(cscdNodeChild(&p->node, i));
        if (!ce) continue;
        if (cscdElementIs(ce, "option")) {
            if (!*first) *first = ce;
            if (ce->state & CSCD_STATE_CHECKED) return ce;
        } else {
            const CscdElement* r = findSelected(ce, first);
            if (r) return r;
        }
    }
    return NULL;
}

static CscdStr optionValue(const CscdElement* o) {
    if (cscdElementHasAttributeC(o, "value")) return cscdStrDup(cscdElementAttrC(o, "value"));
    CscdStr t = {0};
    cscdNodeTextContent(&o->node, &t);
    CscdStr r = cscdStrDup(cscdSvTrim(cscdStrSv(&t)));
    cscdStrFree(&t);
    return r;
}

CscdStr cscdElementValue(const CscdElement* e) {
    if (cscdElementIs(e, "input") || cscdElementIs(e, "textarea")) {
        if (e->valueDirty) return cscdStrCopy(&e->editValue);
        if (cscdElementIs(e, "textarea")) {
            CscdStr t = {0};
            cscdNodeTextContent(&e->node, &t);
            return t;
        }
        return cscdStrDup(cscdElementAttrC(e, "value"));
    }
    if (cscdElementIs(e, "select")) {
        const CscdElement* first = NULL;
        const CscdElement* sel = findSelected(e, &first);
        if (!sel) sel = first;
        if (!sel) return (CscdStr){0};
        return optionValue(sel);
    }
    if (cscdElementIs(e, "option")) return optionValue(e);
    return cscdStrDup(cscdElementAttrC(e, "value"));
}

static void selectWalk(CscdElement* p, CscdSv v) {
    for (size_t i = 0; i < p->node.children.len; ++i) {
        CscdElement* ce = cscdNodeAsElement(cscdNodeChild(&p->node, i));
        if (!ce) continue;
        if (cscdElementIs(ce, "option")) {
            CscdStr ov = cscdElementValue(ce);
            bool on = cscdSvEq(cscdStrSv(&ov), v);
            cscdStrFree(&ov);
            if (on) ce->state |= CSCD_STATE_CHECKED;
            else ce->state &= ~(uint32_t)CSCD_STATE_CHECKED;
        } else selectWalk(ce, v);
    }
}

void cscdElementSetValue(CscdElement* e, CscdSv v) {
    if (cscdElementIs(e, "select")) {
        selectWalk(e, v);
        cscdNodeMarkDirty(&e->node);
        return;
    }
    cscdStrSet(&e->editValue, v);
    e->valueDirty = true;
    e->caret = e->selAnchor = e->editValue.len;
    cscdNodeMarkDirty(&e->node);
}

void cscdElementSetChecked(CscdElement* e, bool c) {
    if (c == cscdElementChecked(e)) return;
    if (c) {
        e->state |= CSCD_STATE_CHECKED;
        /* radio groups */
        if (cscdElementIs(e, "input") && cscdSvIEqC(cscdElementAttrC(e, "type"), "radio") && e->node.owner && cscdElementHasAttributeC(e, "name")) {
            CscdSv name = cscdElementAttrC(e, "name");
            CscdPtrVec radios = {0};
            cscdDocumentQuerySelectorAll(e->node.owner, CSCD_SV("input[type=radio]"), &radios);
            for (size_t i = 0; i < radios.len; ++i) {
                CscdElement* other = (CscdElement*)radios.items[i];
                if (other != e && cscdSvEq(cscdElementAttrC(other, "name"), name)) other->state &= ~(uint32_t)CSCD_STATE_CHECKED;
            }
            cscdVecFree(&radios);
        }
    } else e->state &= ~(uint32_t)CSCD_STATE_CHECKED;
    cscdNodeMarkDirty(&e->node);
}

static void collect(CscdNode* root, const CscdSelectorVec* sels, CscdPtrVec* out, bool first) {
    for (size_t i = 0; i < root->children.len; ++i) {
        CscdElement* e = cscdNodeAsElement(cscdNodeChild(root, i));
        if (!e) continue;
        if (cscdMatchSelectorList(sels, e)) {
            cscdVecPush(out, e);
            if (first) return;
        }
        collect(&e->node, sels, out, first);
        if (first && out->len) return;
    }
}

static CscdElement* findId(CscdNode* root, CscdSv id) {
    for (size_t i = 0; i < root->children.len; ++i) {
        CscdElement* e = cscdNodeAsElement(cscdNodeChild(root, i));
        if (!e) continue;
        const CscdStr* v = cscdElementGetAttribute(e, CSCD_SV("id"));
        if (v && cscdSvEq(cscdStrSv(v), id)) return e;
        CscdElement* r = findId(&e->node, id);
        if (r) return r;
    }
    return NULL;
}

CscdElement* cscdElementQuerySelector(CscdElement* e, CscdSv selector) {
    CscdSelectorVec sels = {0};
    CscdElement* r = NULL;
    if (cscdParseSelectorList(selector, &sels)) {
        CscdPtrVec out = {0};
        collect(&e->node, &sels, &out, true);
        if (out.len) r = (CscdElement*)out.items[0];
        cscdVecFree(&out);
    }
    cscdSelectorVecFree(&sels);
    return r;
}

void cscdElementQuerySelectorAll(CscdElement* e, CscdSv selector, CscdPtrVec* out) {
    CscdSelectorVec sels = {0};
    if (cscdParseSelectorList(selector, &sels)) collect(&e->node, &sels, out, false);
    cscdSelectorVecFree(&sels);
}

CscdElement* cscdElementGetElementById(CscdElement* e, CscdSv id) { return findId(&e->node, id); }

bool cscdElementMatches(const CscdElement* e, CscdSv selector) {
    CscdSelectorVec sels = {0};
    bool r = cscdParseSelectorList(selector, &sels) && cscdMatchSelectorList(&sels, e);
    cscdSelectorVecFree(&sels);
    return r;
}

CscdElement* cscdElementClosest(CscdElement* e, CscdSv selector) {
    CscdSelectorVec sels = {0};
    CscdElement* r = NULL;
    if (cscdParseSelectorList(selector, &sels))
        for (CscdElement* a = e; a; a = cscdNodeParentElement(&a->node))
            if (cscdMatchSelectorList(&sels, a)) { r = a; break; }
    cscdSelectorVecFree(&sels);
    return r;
}

/* onInput / onChange / onChecked: a listener that gives the value */
typedef struct ValueAdapter {
    CscdElement* e;
    CscdValueFn valueFn;
    CscdCheckedFn checkedFn;
    void* user;
    CscdFreeFn freeUser;
} ValueAdapter;

static void adapterFree(void* p) {
    ValueAdapter* a = (ValueAdapter*)p;
    if (a->freeUser) a->freeUser(a->user);
    cscdFree(a);
}

static void adapterCall(void* p, CscdEvent* ev) {
    (void)ev;
    ValueAdapter* a = (ValueAdapter*)p;
    if (a->checkedFn) {
        a->checkedFn(a->user, cscdElementChecked(a->e));
        return;
    }
    CscdStr v = cscdElementValue(a->e);
    a->valueFn(a->user, cscdStrSv(&v));
    cscdStrFree(&v);
}

static void addAdapter(CscdElement* e, const char* type, CscdValueFn vf, CscdCheckedFn cf, void* user, CscdFreeFn freeUser) {
    ValueAdapter* a = CSCD_NEW(ValueAdapter);
    a->e = e;
    a->valueFn = vf;
    a->checkedFn = cf;
    a->user = user;
    a->freeUser = freeUser;
    CscdEventHandler h = {adapterCall, a, adapterFree};
    cscdElementAddEventListener(e, cscdSv(type), h, false);
}

void cscdElementOnInput(CscdElement* e, CscdValueFn fn, void* user, CscdFreeFn freeUser) { addAdapter(e, "input", fn, NULL, user, freeUser); }
void cscdElementOnChange(CscdElement* e, CscdValueFn fn, void* user, CscdFreeFn freeUser) { addAdapter(e, "change", fn, NULL, user, freeUser); }
void cscdElementOnChecked(CscdElement* e, CscdCheckedFn fn, void* user, CscdFreeFn freeUser) { addAdapter(e, "change", NULL, fn, user, freeUser); }

CscdElement* cscdElementAppendHTML(CscdElement* e, CscdSv html) {
    const size_t before = e->node.children.len;
    cscdParseHTMLFragment(html, e);
    for (size_t i = before; i < e->node.children.len; ++i) {
        CscdElement* c = cscdNodeAsElement(cscdNodeChild(&e->node, i));
        if (c) return c;
    }
    return NULL;
}

void cscdElementRemove(CscdElement* e) {
    if (e->node.parent) {
        cscdNodeRemoveChild(e->node.parent, &e->node);
        cscdNodeDestroy(&e->node);
    }
}

void cscdElementAddEventListener(CscdElement* e, CscdSv type, CscdEventHandler h, bool capture) {
    CscdListener l = {cscdStrDup(type), h, capture};
    cscdVecPush(&e->listeners, l);
}

void cscdElementRemoveEventListeners(CscdElement* e, CscdSv type) {
    size_t w = 0;
    for (size_t i = 0; i < e->listeners.len; ++i) {
        CscdListener* l = &e->listeners.items[i];
        if (cscdSvEq(cscdStrSv(&l->type), type)) {
            cscdStrFree(&l->type);
            dropCallback(&l->handler.user, &l->handler.freeUser);
        } else e->listeners.items[w++] = *l;
    }
    e->listeners.len = w;
}

void cscdElementCallListeners(CscdElement* e, CscdEvent* ev, bool capture) {
    /* handlers may add listeners: the ones there now, copied */
    size_t n = e->listeners.len;
    if (!n) return;
    CscdListener* ls = (CscdListener*)cscdMalloc(n * sizeof(CscdListener));
    memcpy(ls, e->listeners.items, n * sizeof(CscdListener));
    CscdSv type = cscdStrSv(&ev->type);
    for (size_t i = 0; i < n; ++i)
        if (ls[i].capture == capture && cscdSvEq(cscdStrSv(&ls[i].type), type)) ls[i].handler.fn(ls[i].handler.user, ev);
    cscdFree(ls);
}

void cscdElementClick(CscdElement* e) {
    CscdDocument* d = e->node.owner;
    if (d && d->onClick) {
        d->onClick(d->onClickUser, e);
        return;
    }
    CscdEvent ev;
    cscdEventInit(&ev, CSCD_SV("click"));
    ev.target = e;
    CscdPtrVec path = {0};
    for (CscdElement* a = e; a; a = cscdNodeParentElement(&a->node)) cscdVecPush(&path, a);
    if (d) cscdDocumentBeginDispatch(d);
    for (size_t i = 0; i < path.len; ++i) {
        CscdElement* a = (CscdElement*)path.items[i];
        ev.currentTarget = a;
        cscdElementCallListeners(a, &ev, false);
        if (ev.propagationStopped) break;
    }
    if (d) cscdDocumentEndDispatch(d);
    cscdVecFree(&path);
    cscdEventFini(&ev);
}

void cscdElementSetPainter(CscdElement* e, CscdPainter p) {
    dropCallback(&e->painter_.user, &e->painter_.freeUser);
    e->painter_ = p;
    cscdNodeMarkDirty(&e->node);
}

void cscdElementSetHighlighter(CscdElement* e, CscdHighlighter h) {
    dropCallback(&e->highlighter_.user, &e->highlighter_.freeUser);
    e->highlighter_ = h;
    cscdStrClear(&e->highlightFor_);
    cscdNodeMarkDirty(&e->node);
}

static int compareSpans(const void* a, const void* b, void* ctx) {
    (void)ctx;
    size_t x = ((const CscdTextSpan*)a)->start, y = ((const CscdTextSpan*)b)->start;
    return x < y ? -1 : x > y ? 1 : 0;
}

const CscdTextSpanVec* cscdElementHighlights(CscdElement* e, CscdSv text) {
    if (!e->highlighter_.fn) {
        cscdVecClear(&e->spans_);
        return &e->spans_;
    }
    if (!cscdSvEq(text, cscdStrSv(&e->highlightFor_)) || !text.len) {
        cscdVecClear(&e->spans_);
        e->highlighter_.fn(e->highlighter_.user, text, &e->spans_);
        cscdSort(e->spans_.items, e->spans_.len, sizeof(CscdTextSpan), compareSpans, NULL);
        cscdStrSet(&e->highlightFor_, text);
    }
    return &e->spans_;
}

/* ---- documents ------------------------------------------------------------------------------------- */

CscdDocument* cscdDocumentNew(void) {
    CscdDocument* d = CSCD_NEW(CscdDocument);
    d->node.type = CSCD_NODE_DOCUMENT;
    d->node.owner = d;
    d->version = 1;
    d->structural = true;
    return d;
}

static void freeDocument(CscdDocument* d) {
    for (size_t i = 0; i < d->graveyard.len; ++i) freeNode((CscdNode*)d->graveyard.items[i]);
    cscdVecFree(&d->graveyard);
    cscdStrFree(&d->baseDir);
    cscdVecFree(&d->textChanged);
    cscdVecFree(&d->paintChanged);
}

void cscdDocumentFree(CscdDocument* d) {
    if (!d) return;
    d->onNodeRemoved = NULL;
    d->onClick = NULL;
    if (d->dispatchDepth > 0) d->retired = true; /* (a listener replaced the page it is called from) */
    else freeNode(&d->node);
}

void cscdDocumentBeginDispatch(CscdDocument* d) { ++d->dispatchDepth; }

void cscdDocumentEndDispatch(CscdDocument* d) {
    if (--d->dispatchDepth > 0) return;
    d->dispatchDepth = 0;
    while (d->graveyard.len) {
        CscdNode* n = (CscdNode*)cscdVecPop(&d->graveyard);
        freeNode(n);
    }
    if (d->retired) freeNode(&d->node);
}

CscdElement* cscdDocumentElement(const CscdDocument* d) { return cscdNodeFirstElementChild(&d->node); }

static CscdElement* childTag(const CscdElement* p, const char* tag) {
    if (!p) return NULL;
    for (size_t i = 0; i < p->node.children.len; ++i) {
        CscdElement* c = cscdNodeAsElement(cscdNodeChild(&p->node, i));
        if (c && cscdElementIs(c, tag)) return c;
    }
    return NULL;
}

CscdElement* cscdDocumentBody(const CscdDocument* d) { return childTag(cscdDocumentElement(d), "body"); }
CscdElement* cscdDocumentHead(const CscdDocument* d) { return childTag(cscdDocumentElement(d), "head"); }
CscdElement* cscdDocumentGetElementById(CscdDocument* d, CscdSv id) { return findId(&d->node, id); }

CscdElement* cscdDocumentQuerySelector(CscdDocument* d, CscdSv selector) {
    CscdElement* root = cscdDocumentElement(d);
    if (!root) return NULL;
    if (cscdElementMatches(root, selector)) return root;
    return cscdElementQuerySelector(root, selector);
}

void cscdDocumentQuerySelectorAll(CscdDocument* d, CscdSv selector, CscdPtrVec* out) {
    CscdElement* root = cscdDocumentElement(d);
    if (!root) return;
    if (cscdElementMatches(root, selector)) cscdVecPush(out, root);
    cscdElementQuerySelectorAll(root, selector, out);
}

CscdElement* cscdDocumentCreateElement(CscdDocument* d, CscdSv tag) {
    CscdElement* e = cscdElementNew(tag);
    e->node.owner = d;
    return e;
}

CscdText* cscdDocumentCreateTextNode(CscdDocument* d, CscdSv text) {
    CscdText* t = cscdTextNew(text);
    t->node.owner = d;
    return t;
}

CscdStr cscdDocumentTitle(const CscdDocument* d) {
    CscdElement* t = childTag(cscdDocumentHead(d), "title");
    if (!t) return (CscdStr){0};
    CscdStr s = {0};
    cscdNodeTextContent(&t->node, &s);
    CscdStr r = cscdStrDup(cscdSvTrim(cscdStrSv(&s)));
    cscdStrFree(&s);
    return r;
}
