/* The inspector (CSCD_FEATURE_INSPECTOR): F12 or Ctrl+Shift+I shows, over the page, the boxes of the
 * element under the pointer (margin, border, padding, content) and a panel with what it is, its
 * computed style and the rules that apply to it, the one that wins first. A click pins the element,
 * Esc closes. While it is open the page does not get the pointer. */
#include "view/viewInternal.h"

#include <stdio.h>

/* (in CscdDisplay's order) */
static const char* kDisplayNames[] = {"none", "inline", "block", "inline-block", "flex", "inline-flex", "grid", "inline-grid", "list-item",
                                      "table", "inline-table", "table-row", "table-cell", "table-row-group", "table-header-group",
                                      "table-footer-group", "table-column", "table-column-group", "table-caption", "contents", "flow-root"};
static const char* kPositionNames[] = {"static", "relative", "absolute", "fixed", "sticky"};

static CscdStrVec* fontFamilies(bool mono) {
    static CscdStrVec sans, code;
    CscdStrVec* f = mono ? &code : &sans;
    if (!f->len) {
        const char* names[] = {mono ? "Consolas" : "Segoe UI", mono ? "Menlo" : "Helvetica Neue", mono ? "monospace" : "sans-serif"};
        for (int i = 0; i < 3; ++i) cscdVecPush(f, cscdStrDupC(names[i]));
    }
    return f;
}

bool cscdViewInspecting(const CscdView* v) { return v->inspecting; }

void cscdViewInspect(CscdView* v, CscdElement* e) {
    v->inspecting = e != NULL;
    v->inspected = e;
    v->inspectPinned = e != NULL;
    v->fullPaint = v->dirty = true;
    cscdViewInvalidate(v);
}

bool viewInspectorKey(CscdView* v, CscdKey k, CscdModifiers m, CscdSv keyName) {
    const bool toggle = k == CSCD_KEY_F12 || (m.ctrl && m.shift && (cscdSvEqC(keyName, "i") || cscdSvEqC(keyName, "I")));
    if (toggle && viewOn(v, CSCD_FEATURE_INSPECTOR)) {
        v->inspecting = !v->inspecting;
        v->inspectPinned = false;
        v->inspected = v->inspecting ? cscdViewElementAt(v, v->mouseX, v->mouseY) : NULL;
        v->fullPaint = v->dirty = true;
        cscdViewInvalidate(v);
        return true;
    }
    if (v->inspecting && k == CSCD_KEY_ESCAPE) {
        cscdViewInspect(v, NULL);
        return true;
    }
    return false;
}

bool viewInspectorMouse(CscdView* v, int kind, float x, float y) {
    if (!v->inspecting) return false;
    if (kind == 0 && !v->inspectPinned) { /* move */
        CscdElement* e = cscdViewElementAt(v, x, y);
        if (e != v->inspected) {
            v->inspected = e;
            v->fullPaint = v->dirty = true;
            cscdViewInvalidate(v);
        }
    } else if (kind == 1) { /* press: pin, or let go */
        v->inspectPinned = !v->inspectPinned;
        if (!v->inspectPinned) v->inspected = cscdViewElementAt(v, x, y);
        v->fullPaint = v->dirty = true;
        cscdViewInvalidate(v);
    }
    return true;
}

/* ---- painting -------------------------------------------------------------------------------------- */

static void ring(CscdCanvas* c, CscdRect outer, CscdRect inner, CscdColor color) {
    if (cscdRectEmpty(outer)) return;
    CscdRRect o = cscdRRect(outer), in = cscdRRect(cscdRectEmpty(inner) ? cscdRect(0, 0, 0, 0) : inner);
    CscdShader s = cscdShaderSolid(color);
    cscdCanvasFillRing(c, &o, &in, &s);
}

static CscdRect inset4(CscdRect r, const float d[4], float sign) {
    return cscdRectInset(r, d[0] * sign, d[1] * sign, d[2] * sign, d[3] * sign);
}

typedef struct Panel {
    CscdCanvas* c;
    CscdFont sans, bold, mono;
    float x, y, w, bottom;
} Panel;

static bool line(Panel* p, const CscdFont* f, CscdColor color, float indent, const char* text) {
    const float h = f->size * 1.45f;
    if (p->y + h > p->bottom) return false;
    cscdCanvasDrawText(p->c, cscdSv(text), p->x + indent, p->y + cscdFontAscent(f), f, color);
    p->y += h;
    return true;
}

static void colorText(char* out, size_t n, CscdColor c) {
    if (c.a == 255) snprintf(out, n, "#%02x%02x%02x", c.r, c.g, c.b);
    else snprintf(out, n, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
}

static void describe(const CscdElement* e, char* out, size_t n) {
    int k = snprintf(out, n, "%s", cscdStrC(&e->tag));
    const CscdSv id = cscdElementAttrC(e, "id");
    if (id.len && k < (int)n) k += snprintf(out + k, n - k, "#%.*s", (int)id.len, id.ptr);
    const CscdStrVec* cls = cscdElementClassList(e);
    for (size_t i = 0; i < cls->len && k < (int)n; ++i) k += snprintf(out + k, n - k, ".%s", cscdStrC(&cls->items[i]));
}

void viewInspectorPaint(CscdView* v, CscdCanvas* c) {
    if (!v->inspecting) return;
    CscdElement* e = v->inspected;
    cscdCanvasSave(c);
    /* the boxes, as browsers' tools color them */
    if (e && e->layout) {
        const CscdLayoutBox* b = e->layout;
        const CscdRect border = cscdViewElementRect(v, e);
        const CscdRect margin = inset4(border, b->margin, -1), padding = inset4(border, b->border, 1), content = inset4(padding, b->padding, 1);
        ring(c, margin, border, cscdRgba(246, 178, 107, 115));
        ring(c, border, padding, cscdRgba(255, 229, 153, 128));
        ring(c, padding, content, cscdRgba(147, 196, 125, 115));
        cscdCanvasFillRect(c, content, cscdRgba(111, 168, 220, 128));
    }
    /* the panel */
    const float w = cscdMinf(380.f, cscdMaxf(220.f, v->vw * 0.45f));
    Panel p;
    p.c = c;
    p.sans = cscdFontMatch(fontFamilies(false), 12, 400, false);
    p.bold = cscdFontMatch(fontFamilies(false), 12, 700, false);
    p.mono = cscdFontMatch(fontFamilies(true), 11, 400, false);
    p.x = v->vw - w + 12;
    p.y = 10;
    p.w = w - 24;
    p.bottom = v->vh - 28;
    cscdCanvasFillRect(c, cscdRect(v->vw - w, 0, w, v->vh), cscdRgba(24, 25, 30, 236));
    cscdCanvasFillRect(c, cscdRect(v->vw - w, 0, 1, v->vh), cscdRgba(255, 255, 255, 40));
    cscdCanvasClipRect(c, cscdRect(v->vw - w, 0, w, v->vh));
    const CscdColor text = cscdRgb(222, 224, 230), dim = cscdRgb(140, 144, 156), accent = cscdRgb(156, 140, 255), value = cscdRgb(130, 200, 255);
    char buf[512];
    if (!e) line(&p, &p.sans, dim, 0, "Point at an element.");
    else {
        describe(e, buf, sizeof buf);
        line(&p, &p.bold, accent, 0, buf);
        const CscdComputedStyle* s = e->style;
        if (e->layout) {
            const CscdLayoutBox* b = e->layout;
            const CscdRect r = cscdViewElementRect(v, e);
            snprintf(buf, sizeof buf, "%.1f x %.1f  at %.1f, %.1f", r.w, r.h, r.x, r.y);
            line(&p, &p.mono, text, 0, buf);
            snprintf(buf, sizeof buf, "margin  %g %g %g %g", b->margin[0], b->margin[1], b->margin[2], b->margin[3]);
            line(&p, &p.mono, cscdRgb(246, 178, 107), 0, buf);
            snprintf(buf, sizeof buf, "border  %g %g %g %g", b->border[0], b->border[1], b->border[2], b->border[3]);
            line(&p, &p.mono, cscdRgb(255, 229, 153), 0, buf);
            snprintf(buf, sizeof buf, "padding %g %g %g %g", b->padding[0], b->padding[1], b->padding[2], b->padding[3]);
            line(&p, &p.mono, cscdRgb(147, 196, 125), 0, buf);
        } else line(&p, &p.mono, dim, 0, "no box (display: none, or not laid out)");
        if (s) {
            const char* d = s->display < CSCD_COUNT(kDisplayNames) ? kDisplayNames[s->display] : "?";
            const char* pos = s->position < CSCD_COUNT(kPositionNames) ? kPositionNames[s->position] : "?";
            snprintf(buf, sizeof buf, "display %s  position %s  opacity %g", d, pos, s->opacity);
            line(&p, &p.mono, text, 0, buf);
            snprintf(buf, sizeof buf, "font %gpx %s %d%s", s->font.size, s->font.families.len ? cscdStrC(&s->font.families.items[0]) : "", s->font.weight,
                     s->font.italic ? " italic" : "");
            line(&p, &p.mono, text, 0, buf);
            char fg[16], bg[16];
            colorText(fg, sizeof fg, s->color);
            colorText(bg, sizeof bg, s->backgroundColor);
            snprintf(buf, sizeof buf, "color %s  background %s", fg, bg);
            line(&p, &p.mono, text, 0, buf);
        }
        p.y += 6;
        line(&p, &p.bold, dim, 0, "Rules (the first wins)");
        CscdMatchedRuleVec rules = {0};
        cscdStyleEngineMatchedRules(v->styles, e, &rules);
        bool room = true;
        for (size_t i = rules.len; i-- > 0 && room;) {
            const CscdMatchedRule* r = &rules.items[i];
            const uint32_t sp = r->selector->specificity;
            snprintf(buf, sizeof buf, "%s  (%u,%u,%u)%s", r->selector->text.len ? cscdStrC(&r->selector->text) : "?", sp >> 16, (sp >> 8) & 255,
                     sp & 255, r->origin == CSCD_ORIGIN_USER_AGENT ? "  built in" : "");
            room = line(&p, &p.mono, accent, 0, buf);
            const CscdDeclVec* decls = &r->rule->declarations;
            for (size_t k = 0; k < decls->len && room; ++k) {
                const CscdDeclaration* d = &decls->items[k];
                snprintf(buf, sizeof buf, "%s: %.60s%s%s", cscdStrC(&d->property), cscdStrC(&d->value), d->value.len > 60 ? "..." : "",
                         d->important ? " !important" : "");
                room = line(&p, &p.mono, value, 10, buf);
            }
            p.y += 3;
        }
        const CscdStr* inl = cscdElementGetAttribute(e, CSCD_SV("style"));
        if (inl && room) {
            line(&p, &p.mono, accent, 0, "style attribute");
            snprintf(buf, sizeof buf, "%.200s", cscdStrC(inl));
            line(&p, &p.mono, value, 10, buf);
        }
        cscdVecFree(&rules);
    }
    p.y = v->vh - 22;
    p.bottom = v->vh;
    line(&p, &p.sans, dim, 0, v->inspectPinned ? "Pinned: click to follow the pointer again. Esc closes." : "Click pins the element. Esc closes.");
    cscdCanvasRestore(c);
}
