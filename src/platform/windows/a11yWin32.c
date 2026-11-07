/* UI Automation: the page as Windows screen readers (Narrator, NVDA, JAWS) see it, from the view's
 * accessible tree (a11y.c). One small COM object per element asked about, holding the element's
 * accessible number (a pointer could outlive it) and a link to the window that is cut when the
 * window goes, so that an object a reader still holds then answers "not available". Calls arrive on
 * the window's thread (ProviderOptions_UseComThreading). */
#include "platform/common/platformInternal.h"

#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <stddef.h>
#include <stdio.h>

#ifndef UIA_E_ELEMENTNOTAVAILABLE
#define UIA_E_ELEMENTNOTAVAILABLE ((HRESULT)0x80040201)
#endif
#ifndef HeadingLevel1
#define HeadingLevel_None 80050
#define HeadingLevel1 80051
#endif

/* The window, while it is there. */
typedef struct Link {
    LONG refs;
    CscdWindow* w;
} Link;

typedef struct Node {
    IRawElementProviderSimple simple;
    IRawElementProviderFragment fragment;
    IRawElementProviderFragmentRoot root;
    IInvokeProvider invoke;
    IToggleProvider toggle;
    IValueProvider value;
    IRangeValueProvider range;
    LONG refs;
    Link* link;
    uint32_t id; /* 0: the document */
} Node;

typedef struct WinA11y {
    Link* link;
    Node* root;
} WinA11y;

#define NODE_OF(p, member) ((Node*)((char*)(p) - offsetof(Node, member)))

static Node* nodeNew(Link* link, uint32_t id);

static CscdView* viewOf(const Node* n) { return n->link->w ? n->link->w->view : NULL; }

/* The element, or NULL when it (or the window) is gone. */
static CscdElement* elementOf(const Node* n) {
    CscdView* v = viewOf(n);
    if (!v) return NULL;
    return n->id ? cscdAccessibleElement(v, n->id) : cscdAccessibleRoot(v);
}

static Node* nodeFor(Node* from, CscdElement* e) {
    CscdView* v = viewOf(from);
    if (!v || !e) return NULL;
    WinA11y* a = (WinA11y*)from->link->w->a11y;
    if (e == cscdAccessibleRoot(v)) {
        IRawElementProviderSimple_AddRef(&a->root->simple);
        return a->root;
    }
    return nodeNew(from->link, cscdAccessibleId(v, e));
}

static BSTR bstrOf(CscdSv s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.ptr ? s.ptr : "", (int)s.len, NULL, 0);
    BSTR b = SysAllocStringLen(NULL, (UINT)n);
    if (b && n) MultiByteToWideChar(CP_UTF8, 0, s.ptr, (int)s.len, b, n);
    return b;
}

static CscdStr utf8Of(LPCWSTR w) {
    CscdStr out = {0};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n > 1) {
        cscdStrReserve(&out, (size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out.ptr, n, NULL, NULL);
        out.len = (size_t)n - 1;
    }
    return out;
}

/* ---- IUnknown, shared by every interface of a node ------------------------------------------------- */

static HRESULT nodeQuery(Node* n, REFIID riid, void** out) {
    *out = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IRawElementProviderSimple)) *out = &n->simple;
    else if (IsEqualIID(riid, &IID_IRawElementProviderFragment)) *out = &n->fragment;
    else if (IsEqualIID(riid, &IID_IRawElementProviderFragmentRoot) && n->id == 0) *out = &n->root;
    else if (IsEqualIID(riid, &IID_IInvokeProvider)) *out = &n->invoke;
    else if (IsEqualIID(riid, &IID_IToggleProvider)) *out = &n->toggle;
    else if (IsEqualIID(riid, &IID_IValueProvider)) *out = &n->value;
    else if (IsEqualIID(riid, &IID_IRangeValueProvider)) *out = &n->range;
    else return E_NOINTERFACE;
    InterlockedIncrement(&n->refs);
    return S_OK;
}

static ULONG nodeAddRef(Node* n) { return (ULONG)InterlockedIncrement(&n->refs); }

static void linkRelease(Link* l) {
    if (InterlockedDecrement(&l->refs) == 0) cscdFree(l);
}

static ULONG nodeRelease(Node* n) {
    const LONG r = InterlockedDecrement(&n->refs);
    if (r == 0) {
        linkRelease(n->link);
        cscdFree(n);
    }
    return (ULONG)r;
}

#define UNKNOWN_FOR(Iface, member)                                                                                              \
    static HRESULT STDMETHODCALLTYPE member##Query(Iface* self, REFIID riid, void** out) { return nodeQuery(NODE_OF(self, member), riid, out); } \
    static ULONG STDMETHODCALLTYPE member##AddRef(Iface* self) { return nodeAddRef(NODE_OF(self, member)); }                    \
    static ULONG STDMETHODCALLTYPE member##Release(Iface* self) { return nodeRelease(NODE_OF(self, member)); }

UNKNOWN_FOR(IRawElementProviderSimple, simple)
UNKNOWN_FOR(IRawElementProviderFragment, fragment)
UNKNOWN_FOR(IRawElementProviderFragmentRoot, root)
UNKNOWN_FOR(IInvokeProvider, invoke)
UNKNOWN_FOR(IToggleProvider, toggle)
UNKNOWN_FOR(IValueProvider, value)
UNKNOWN_FOR(IRangeValueProvider, range)

/* ---- IRawElementProviderSimple --------------------------------------------------------------------- */

static long controlType(CscdRole r) {
    switch (r) {
    case CSCD_ROLE_DOCUMENT: return UIA_DocumentControlTypeId;
    case CSCD_ROLE_TEXT: case CSCD_ROLE_HEADING: return UIA_TextControlTypeId;
    case CSCD_ROLE_BUTTON: return UIA_ButtonControlTypeId;
    case CSCD_ROLE_LINK: return UIA_HyperlinkControlTypeId;
    case CSCD_ROLE_CHECKBOX: return UIA_CheckBoxControlTypeId;
    case CSCD_ROLE_RADIO: return UIA_RadioButtonControlTypeId;
    case CSCD_ROLE_TEXTBOX: return UIA_EditControlTypeId;
    case CSCD_ROLE_COMBOBOX: return UIA_ComboBoxControlTypeId;
    case CSCD_ROLE_LISTBOX: case CSCD_ROLE_LIST: return UIA_ListControlTypeId;
    case CSCD_ROLE_OPTION: case CSCD_ROLE_LIST_ITEM: return UIA_ListItemControlTypeId;
    case CSCD_ROLE_SLIDER: return UIA_SliderControlTypeId;
    case CSCD_ROLE_PROGRESS: return UIA_ProgressBarControlTypeId;
    case CSCD_ROLE_IMAGE: return UIA_ImageControlTypeId;
    case CSCD_ROLE_DIALOG: return UIA_WindowControlTypeId;
    case CSCD_ROLE_TAB: return UIA_TabItemControlTypeId;
    case CSCD_ROLE_TAB_LIST: return UIA_TabControlTypeId;
    case CSCD_ROLE_TAB_PANEL: return UIA_PaneControlTypeId;
    case CSCD_ROLE_MENU: return UIA_MenuControlTypeId;
    case CSCD_ROLE_MENU_BAR: return UIA_MenuBarControlTypeId;
    case CSCD_ROLE_MENU_ITEM: return UIA_MenuItemControlTypeId;
    case CSCD_ROLE_TABLE: return UIA_TableControlTypeId;
    case CSCD_ROLE_ROW: case CSCD_ROLE_CELL: return UIA_DataItemControlTypeId;
    case CSCD_ROLE_SEPARATOR: return UIA_SeparatorControlTypeId;
    case CSCD_ROLE_TOOLTIP: return UIA_ToolTipControlTypeId;
    default: return UIA_GroupControlTypeId;
    }
}

static HRESULT STDMETHODCALLTYPE simpleOptions(IRawElementProviderSimple* self, enum ProviderOptions* ret) {
    (void)self;
    *ret = ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE simplePattern(IRawElementProviderSimple* self, PATTERNID pattern, IUnknown** ret) {
    Node* n = NODE_OF(self, simple);
    *ret = NULL;
    CscdElement* e = elementOf(n);
    if (!e) return UIA_E_ELEMENTNOTAVAILABLE;
    const CscdRole r = cscdAccessibleRole(e);
    IUnknown* p = NULL;
    if (pattern == UIA_InvokePatternId && (r == CSCD_ROLE_BUTTON || r == CSCD_ROLE_LINK || r == CSCD_ROLE_MENU_ITEM || r == CSCD_ROLE_TAB ||
                                          r == CSCD_ROLE_OPTION || r == CSCD_ROLE_RADIO))
        p = (IUnknown*)&n->invoke;
    else if (pattern == UIA_TogglePatternId && r == CSCD_ROLE_CHECKBOX) p = (IUnknown*)&n->toggle;
    else if (pattern == UIA_ValuePatternId && (r == CSCD_ROLE_TEXTBOX || r == CSCD_ROLE_COMBOBOX)) p = (IUnknown*)&n->value;
    else if (pattern == UIA_RangeValuePatternId && (r == CSCD_ROLE_SLIDER || r == CSCD_ROLE_PROGRESS)) p = (IUnknown*)&n->range;
    if (p) {
        nodeAddRef(n);
        *ret = p;
    }
    return S_OK;
}

static void setBool(VARIANT* v, bool b) {
    v->vt = VT_BOOL;
    v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}

static void setText(VARIANT* v, CscdSv s) {
    v->vt = VT_BSTR;
    v->bstrVal = bstrOf(s);
}

static HRESULT STDMETHODCALLTYPE simpleProperty(IRawElementProviderSimple* self, PROPERTYID property, VARIANT* ret) {
    Node* n = NODE_OF(self, simple);
    VariantInit(ret);
    CscdElement* e = elementOf(n);
    if (!e) return UIA_E_ELEMENTNOTAVAILABLE;
    CscdView* v = viewOf(n);
    const CscdRole role = n->id ? cscdAccessibleRole(e) : CSCD_ROLE_DOCUMENT;
    const uint32_t state = cscdAccessibleState(v, e);
    CscdStr s = {0};
    /* (if, not switch: MSVC's headers declare the ids as variables, which a case cannot be) */
    if (property == UIA_ControlTypePropertyId) {
        ret->vt = VT_I4; ret->lVal = controlType(role);
    } else if (property == UIA_NamePropertyId) {
        if (n->id) cscdAccessibleName(v, e, &s);
        else { CscdStr t = cscdViewTitle(v); s = t; }
        setText(ret, cscdStrSv(&s));
    } else if (property == UIA_HelpTextPropertyId) {
        cscdAccessibleDescription(v, e, &s);
        if (s.len) setText(ret, cscdStrSv(&s));
    } else if (property == UIA_AutomationIdPropertyId) {
        if (cscdElementAttrC(e, "id").len) setText(ret, cscdElementAttrC(e, "id"));
    } else if (property == UIA_ClassNamePropertyId) {
        setText(ret, cscdStrSv(&e->tag));
    } else if (property == UIA_FrameworkIdPropertyId) {
        setText(ret, CSCD_SV("cascade"));
    } else if (property == UIA_LocalizedControlTypePropertyId) {
        if (role == CSCD_ROLE_HEADING) setText(ret, CSCD_SV("heading"));
        else if (role == CSCD_ROLE_TAB_PANEL) setText(ret, CSCD_SV("tab panel"));
    } else if (property == UIA_HeadingLevelPropertyId) {
        ret->vt = VT_I4;
        ret->lVal = role == CSCD_ROLE_HEADING ? HeadingLevel1 + cscdClampi(cscdAccessibleLevel(e), 1, 9) - 1 : HeadingLevel_None;
    } else if (property == UIA_IsKeyboardFocusablePropertyId) {
        setBool(ret, (state & CSCD_A11Y_FOCUSABLE) != 0);
    } else if (property == UIA_HasKeyboardFocusPropertyId) {
        setBool(ret, (state & CSCD_A11Y_FOCUSED) != 0);
    } else if (property == UIA_IsEnabledPropertyId) {
        setBool(ret, !(state & CSCD_A11Y_DISABLED));
    } else if (property == UIA_IsPasswordPropertyId) {
        setBool(ret, (state & CSCD_A11Y_PASSWORD) != 0);
    } else if (property == UIA_IsRequiredForFormPropertyId) {
        setBool(ret, (state & CSCD_A11Y_REQUIRED) != 0);
    } else if (property == UIA_IsDialogPropertyId) {
        setBool(ret, role == CSCD_ROLE_DIALOG);
    } else if (property == UIA_IsOffscreenPropertyId) {
        const CscdRect r = cscdViewElementRect(v, e);
        setBool(ret, n->id && (cscdRectRight(r) <= 0 || cscdRectBottom(r) <= 0 || r.x >= cscdViewWidth(v) || r.y >= cscdViewHeight(v)));
    } else if (property == UIA_IsControlElementPropertyId || property == UIA_IsContentElementPropertyId) {
        setBool(ret, true);
    }
    cscdStrFree(&s);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE simpleHost(IRawElementProviderSimple* self, IRawElementProviderSimple** ret) {
    Node* n = NODE_OF(self, simple);
    *ret = NULL;
    if (n->id == 0 && n->link->w && n->link->w->hwnd) return UiaHostProviderFromHwnd((HWND)n->link->w->hwnd, ret);
    return S_OK;
}

static IRawElementProviderSimpleVtbl simpleVtbl = {
    .QueryInterface = simpleQuery, .AddRef = simpleAddRef, .Release = simpleRelease, .get_ProviderOptions = simpleOptions,
    .GetPatternProvider = simplePattern, .GetPropertyValue = simpleProperty, .get_HostRawElementProvider = simpleHost,
};

/* ---- IRawElementProviderFragment ------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE fragmentNavigate(IRawElementProviderFragment* self, enum NavigateDirection dir, IRawElementProviderFragment** ret) {
    Node* n = NODE_OF(self, fragment);
    *ret = NULL;
    CscdElement* e = elementOf(n);
    if (!e) return UIA_E_ELEMENTNOTAVAILABLE;
    CscdView* v = viewOf(n);
    CscdElement* to = NULL;
    if (dir == NavigateDirection_Parent) {
        if (n->id) to = cscdAccessibleParent(v, e);
        if (n->id && !to) to = cscdAccessibleRoot(v);
    } else if (dir == NavigateDirection_FirstChild || dir == NavigateDirection_LastChild) {
        CscdPtrVec kids = {0};
        cscdAccessibleChildren(v, e, &kids);
        if (kids.len) to = (CscdElement*)kids.items[dir == NavigateDirection_FirstChild ? 0 : kids.len - 1];
        cscdVecFree(&kids);
    } else if (n->id) {
        CscdElement* parent = cscdAccessibleParent(v, e);
        CscdPtrVec kids = {0};
        cscdAccessibleChildren(v, parent ? parent : cscdAccessibleRoot(v), &kids);
        for (size_t i = 0; i < kids.len; ++i)
            if (kids.items[i] == e) {
                if (dir == NavigateDirection_NextSibling && i + 1 < kids.len) to = (CscdElement*)kids.items[i + 1];
                if (dir == NavigateDirection_PreviousSibling && i > 0) to = (CscdElement*)kids.items[i - 1];
                break;
            }
        cscdVecFree(&kids);
    }
    Node* t = nodeFor(n, to);
    if (t) *ret = &t->fragment;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE fragmentRuntimeId(IRawElementProviderFragment* self, SAFEARRAY** ret) {
    Node* n = NODE_OF(self, fragment);
    *ret = NULL;
    if (n->id == 0) return S_OK; /* (the window's own) */
    SAFEARRAY* a = SafeArrayCreateVector(VT_I4, 0, 2);
    if (!a) return E_OUTOFMEMORY;
    LONG i = 0, parts[2] = {UiaAppendRuntimeId, (LONG)n->id};
    for (; i < 2; ++i) SafeArrayPutElement(a, &i, &parts[i]);
    *ret = a;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE fragmentBounds(IRawElementProviderFragment* self, struct UiaRect* ret) {
    Node* n = NODE_OF(self, fragment);
    memset(ret, 0, sizeof *ret);
    CscdElement* e = elementOf(n);
    if (!e) return UIA_E_ELEMENTNOTAVAILABLE;
    CscdWindow* w = n->link->w;
    POINT origin = {0, 0};
    ClientToScreen((HWND)w->hwnd, &origin);
    const float dpr = w->dpr > 0 ? w->dpr : 1;
    CscdRect r = n->id ? cscdViewElementRect(w->view, e) : cscdRect(0, 0, cscdViewWidth(w->view), cscdViewHeight(w->view));
    ret->left = origin.x + r.x * dpr;
    ret->top = origin.y + r.y * dpr;
    ret->width = r.w * dpr;
    ret->height = r.h * dpr;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE fragmentEmbedded(IRawElementProviderFragment* self, SAFEARRAY** ret) {
    (void)self;
    *ret = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE fragmentFocus(IRawElementProviderFragment* self) {
    Node* n = NODE_OF(self, fragment);
    CscdElement* e = elementOf(n);
    if (!e) return UIA_E_ELEMENTNOTAVAILABLE;
    cscdAccessibleDo(viewOf(n), e, CSCD_A11Y_FOCUS, CSCD_SV(""));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE fragmentRoot(IRawElementProviderFragment* self, IRawElementProviderFragmentRoot** ret) {
    Node* n = NODE_OF(self, fragment);
    *ret = NULL;
    if (!n->link->w) return UIA_E_ELEMENTNOTAVAILABLE;
    Node* root = ((WinA11y*)n->link->w->a11y)->root;
    nodeAddRef(root);
    *ret = &root->root;
    return S_OK;
}

static IRawElementProviderFragmentVtbl fragmentVtbl = {
    .QueryInterface = fragmentQuery, .AddRef = fragmentAddRef, .Release = fragmentRelease, .Navigate = fragmentNavigate,
    .GetRuntimeId = fragmentRuntimeId, .get_BoundingRectangle = fragmentBounds, .GetEmbeddedFragmentRoots = fragmentEmbedded,
    .SetFocus = fragmentFocus, .get_FragmentRoot = fragmentRoot,
};

/* ---- IRawElementProviderFragmentRoot --------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE rootFromPoint(IRawElementProviderFragmentRoot* self, double x, double y, IRawElementProviderFragment** ret) {
    Node* n = NODE_OF(self, root);
    *ret = NULL;
    CscdWindow* w = n->link->w;
    if (!w) return UIA_E_ELEMENTNOTAVAILABLE;
    POINT p = {(LONG)x, (LONG)y};
    ScreenToClient((HWND)w->hwnd, &p);
    const float dpr = w->dpr > 0 ? w->dpr : 1;
    Node* t = nodeFor(n, cscdAccessibleAt(w->view, p.x / dpr, p.y / dpr));
    if (t) *ret = &t->fragment;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE rootFocus(IRawElementProviderFragmentRoot* self, IRawElementProviderFragment** ret) {
    Node* n = NODE_OF(self, root);
    *ret = NULL;
    CscdView* v = viewOf(n);
    if (!v) return UIA_E_ELEMENTNOTAVAILABLE;
    CscdElement* f = cscdViewDocument(v)->focused;
    if (f && cscdAccessibleIncluded(f)) {
        Node* t = nodeFor(n, f);
        if (t) *ret = &t->fragment;
    }
    return S_OK;
}

static IRawElementProviderFragmentRootVtbl rootVtbl = {
    .QueryInterface = rootQuery, .AddRef = rootAddRef, .Release = rootRelease, .ElementProviderFromPoint = rootFromPoint, .GetFocus = rootFocus,
};

/* ---- patterns ------------------------------------------------------------------------------------- */

static HRESULT doAction(Node* n, CscdAccessibleAction action, CscdSv value) {
    CscdElement* e = elementOf(n);
    if (!e) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!cscdAccessibleDo(viewOf(n), e, action, value)) return UIA_E_ELEMENTNOTENABLED;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE invokeInvoke(IInvokeProvider* self) { return doAction(NODE_OF(self, invoke), CSCD_A11Y_ACTIVATE, CSCD_SV("")); }

static IInvokeProviderVtbl invokeVtbl = {.QueryInterface = invokeQuery, .AddRef = invokeAddRef, .Release = invokeRelease, .Invoke = invokeInvoke};

static HRESULT STDMETHODCALLTYPE toggleToggle(IToggleProvider* self) { return doAction(NODE_OF(self, toggle), CSCD_A11Y_ACTIVATE, CSCD_SV("")); }

static HRESULT STDMETHODCALLTYPE toggleState(IToggleProvider* self, enum ToggleState* ret) {
    Node* n = NODE_OF(self, toggle);
    CscdElement* e = elementOf(n);
    if (!e) return UIA_E_ELEMENTNOTAVAILABLE;
    const uint32_t s = cscdAccessibleState(viewOf(n), e);
    *ret = (s & CSCD_A11Y_MIXED) ? ToggleState_Indeterminate : (s & CSCD_A11Y_CHECKED) ? ToggleState_On : ToggleState_Off;
    return S_OK;
}

static IToggleProviderVtbl toggleVtbl = {
    .QueryInterface = toggleQuery, .AddRef = toggleAddRef, .Release = toggleRelease, .Toggle = toggleToggle, .get_ToggleState = toggleState,
};

static HRESULT STDMETHODCALLTYPE valueSet(IValueProvider* self, LPCWSTR text) {
    CscdStr s = utf8Of(text);
    const HRESULT r = doAction(NODE_OF(self, value), CSCD_A11Y_SET_VALUE, cscdStrSv(&s));
    cscdStrFree(&s);
    return r;
}

static HRESULT STDMETHODCALLTYPE valueGet(IValueProvider* self, BSTR* ret) {
    Node* n = NODE_OF(self, value);
    CscdElement* e = elementOf(n);
    if (!e) return UIA_E_ELEMENTNOTAVAILABLE;
    CscdStr s = {0};
    if (!(cscdAccessibleState(viewOf(n), e) & CSCD_A11Y_PASSWORD)) cscdAccessibleValue(viewOf(n), e, &s);
    *ret = bstrOf(cscdStrSv(&s));
    cscdStrFree(&s);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE valueReadOnly(IValueProvider* self, BOOL* ret) {
    Node* n = NODE_OF(self, value);
    CscdElement* e = elementOf(n);
    if (!e) return UIA_E_ELEMENTNOTAVAILABLE;
    *ret = (cscdAccessibleState(viewOf(n), e) & (CSCD_A11Y_READONLY | CSCD_A11Y_DISABLED)) != 0;
    return S_OK;
}

static IValueProviderVtbl valueVtbl = {
    .QueryInterface = valueQuery, .AddRef = valueAddRef, .Release = valueRelease, .SetValue = valueSet, .get_Value = valueGet,
    .get_IsReadOnly = valueReadOnly,
};

static HRESULT rangeOf(IRangeValueProvider* self, double* value, double* min, double* max, double* step) {
    Node* n = NODE_OF(self, range);
    CscdElement* e = elementOf(n);
    if (!e) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!cscdAccessibleRange(e, value, min, max, step)) *value = *min = *max = *step = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE rangeSet(IRangeValueProvider* self, double value) {
    char buf[32];
    snprintf(buf, sizeof buf, "%g", value);
    return doAction(NODE_OF(self, range), CSCD_A11Y_SET_VALUE, cscdSv(buf));
}

#define RANGE_GET(name, expr)                                                                       \
    static HRESULT STDMETHODCALLTYPE name(IRangeValueProvider* self, double* ret) {                \
        double value = 0, min = 0, max = 0, step = 0;                                               \
        const HRESULT r = rangeOf(self, &value, &min, &max, &step);                                 \
        *ret = (expr);                                                                              \
        return r;                                                                                   \
    }
RANGE_GET(rangeValue, value)
RANGE_GET(rangeMax, max)
RANGE_GET(rangeMin, min)
RANGE_GET(rangeLarge, step > 0 ? step * 10 : (max - min) / 10)
RANGE_GET(rangeSmall, step > 0 ? step : (max - min) / 100)

static HRESULT STDMETHODCALLTYPE rangeReadOnly(IRangeValueProvider* self, BOOL* ret) {
    Node* n = NODE_OF(self, range);
    CscdElement* e = elementOf(n);
    if (!e) return UIA_E_ELEMENTNOTAVAILABLE;
    *ret = cscdAccessibleRole(e) == CSCD_ROLE_PROGRESS || (cscdAccessibleState(viewOf(n), e) & (CSCD_A11Y_READONLY | CSCD_A11Y_DISABLED));
    return S_OK;
}

static IRangeValueProviderVtbl rangeVtbl = {
    .QueryInterface = rangeQuery, .AddRef = rangeAddRef, .Release = rangeRelease, .SetValue = rangeSet, .get_Value = rangeValue,
    .get_IsReadOnly = rangeReadOnly, .get_Maximum = rangeMax, .get_Minimum = rangeMin, .get_LargeChange = rangeLarge,
    .get_SmallChange = rangeSmall,
};

static Node* nodeNew(Link* link, uint32_t id) {
    Node* n = CSCD_NEW(Node);
    n->simple.lpVtbl = &simpleVtbl;
    n->fragment.lpVtbl = &fragmentVtbl;
    n->root.lpVtbl = &rootVtbl;
    n->invoke.lpVtbl = &invokeVtbl;
    n->toggle.lpVtbl = &toggleVtbl;
    n->value.lpVtbl = &valueVtbl;
    n->range.lpVtbl = &rangeVtbl;
    n->refs = 1;
    n->link = link;
    InterlockedIncrement(&link->refs);
    n->id = id;
    return n;
}

/* ---- the window's side ---------------------------------------------------------------------------- */

static void onEvent(void* user, CscdElement* e, CscdAccessibleEvent event) {
    CscdWindow* w = (CscdWindow*)user;
    WinA11y* a = (WinA11y*)w->a11y;
    if (!a || !UiaClientsAreListening()) return;
    if (event == CSCD_A11Y_EVENT_STRUCTURE || !e) {
        UiaRaiseStructureChangedEvent(&a->root->simple, StructureChangeType_ChildrenInvalidated, NULL, 0);
        return;
    }
    if (!cscdAccessibleIncluded(e)) return;
    Node* n = nodeFor(a->root, e);
    if (!n) return;
    if (event == CSCD_A11Y_EVENT_FOCUS) UiaRaiseAutomationEvent(&n->simple, UIA_AutomationFocusChangedEventId);
    else {
        const CscdRole r = cscdAccessibleRole(e);
        VARIANT before, now;
        VariantInit(&before);
        VariantInit(&now);
        PROPERTYID prop = 0;
        if (r == CSCD_ROLE_CHECKBOX) {
            enum ToggleState s = ToggleState_Off;
            if (FAILED(toggleState(&n->toggle, &s))) return; /* (its element is gone: nothing to tell) */
            prop = UIA_ToggleToggleStatePropertyId;
            now.vt = VT_I4;
            now.lVal = s;
        } else if (r == CSCD_ROLE_SLIDER || r == CSCD_ROLE_PROGRESS) {
            prop = UIA_RangeValueValuePropertyId;
            now.vt = VT_R8;
            rangeValue(&n->range, &now.dblVal);
        } else if (r == CSCD_ROLE_TEXTBOX || r == CSCD_ROLE_COMBOBOX) {
            BSTR text = NULL;
            if (FAILED(valueGet(&n->value, &text))) return; /* (as above; and VariantClear must not free what was never set) */
            prop = UIA_ValueValuePropertyId;
            now.vt = VT_BSTR;
            now.bstrVal = text;
        }
        if (prop) UiaRaiseAutomationPropertyChangedEvent(&n->simple, prop, before, now);
        VariantClear(&now);
    }
    nodeRelease(n);
}

void winA11yAttach(CscdWindow* w) {
    WinA11y* a = CSCD_NEW(WinA11y);
    a->link = CSCD_NEW(Link);
    a->link->refs = 1;
    a->link->w = w;
    a->root = nodeNew(a->link, 0);
    w->a11y = a;
    cscdViewOnAccessibleEvent(w->view, onEvent, w);
}

bool winA11yGetObject(CscdWindow* w, uintptr_t wp, intptr_t lp, intptr_t* out) {
    WinA11y* a = (WinA11y*)w->a11y;
    if (!a || (long)lp != UiaRootObjectId || !cscdViewFeature(w->view, CSCD_FEATURE_ACCESSIBILITY)) return false;
    *out = (intptr_t)UiaReturnRawElementProvider((HWND)w->hwnd, (WPARAM)wp, (LPARAM)lp, &a->root->simple);
    return true;
}

void winA11yDetach(CscdWindow* w) {
    WinA11y* a = (WinA11y*)w->a11y;
    if (!a) return;
    cscdViewOnAccessibleEvent(w->view, NULL, NULL);
    if (w->hwnd) UiaReturnRawElementProvider((HWND)w->hwnd, 0, 0, NULL); /* (readers let go of the window) */
    a->link->w = NULL;
    nodeRelease(a->root);
    linkRelease(a->link);
    cscdFree(a);
    w->a11y = NULL;
}
