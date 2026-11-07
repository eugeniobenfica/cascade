"""cascade for Python: the handle API (cascadeC.h) through ctypes, with the standard library only.

    import cascade
    view = cascade.View()
    view.load_html("<p id=p>Hello</p><button id=b>Go</button>")
    view["#b"].on("click", lambda ev: view["#p"].set_text("Clicked"))
    view["#b"].click()
    print(view["#p"].text)            # Clicked

The library is found next to this file, in CASCADE_LIB, or by the system (cascade_c.dll,
libcascade_c.so, libcascade_c.dylib). Build it with CMake's -DCASCADE_C_SHARED=ON.
"""
import ctypes
import enum
import os
import sys

__all__ = ["View", "Element", "Window", "Canvas", "Event", "Feature", "Key", "EditAction", "CascadeError", "version"]


def _load():
    names = {"win32": ["cascade_c.dll"], "darwin": ["libcascade_c.dylib"]}.get(sys.platform, ["libcascade_c.so"])
    here = os.path.dirname(os.path.abspath(__file__))
    tried = []
    for candidate in ([os.environ["CASCADE_LIB"]] if os.environ.get("CASCADE_LIB") else []) + [os.path.join(here, n) for n in names] + names:
        try:
            return ctypes.CDLL(candidate)
        except OSError as e:
            tried.append("%s (%s)" % (candidate, e))
    raise OSError("cascade_c not found; set CASCADE_LIB. Tried: " + "; ".join(tried))


_lib = _load()
_view_p, _window_p, _canvas_p, _element = ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64
_FREE_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


class _Event(ctypes.Structure):
    _fields_ = [("type", ctypes.c_char_p), ("target", _element), ("current_target", _element), ("x", ctypes.c_float),
                ("y", ctypes.c_float), ("offset_x", ctypes.c_float), ("offset_y", ctypes.c_float), ("delta_x", ctypes.c_float),
                ("delta_y", ctypes.c_float), ("button", ctypes.c_int), ("buttons", ctypes.c_int), ("detail", ctypes.c_int),
                ("key", ctypes.c_char_p), ("mods", ctypes.c_uint), ("internal_", ctypes.c_void_p)]


class _Stats(ctypes.Structure):
    _fields_ = [("style_ms", ctypes.c_double), ("layout_ms", ctypes.c_double), ("paint_ms", ctypes.c_double),
                ("styles_computed", ctypes.c_int), ("styles_reused", ctypes.c_int), ("painted_area", ctypes.c_double),
                ("partial_paint", ctypes.c_int)]


_EVENT_FN = ctypes.CFUNCTYPE(None, _view_p, ctypes.POINTER(_Event), ctypes.c_void_p)
_PAINT_FN = ctypes.CFUNCTYPE(None, _canvas_p, ctypes.c_float, ctypes.c_float, ctypes.c_void_p)
_LIST_FN = ctypes.CFUNCTYPE(None, _view_p, _element, ctypes.c_size_t, ctypes.c_void_p)
_EDIT_MENU_FN = ctypes.CFUNCTYPE(None, _view_p, ctypes.c_int, ctypes.POINTER(ctypes.c_float), ctypes.c_uint, ctypes.c_void_p)
_MAGNIFIER_FN = ctypes.CFUNCTYPE(None, _view_p, ctypes.c_int, ctypes.c_float, ctypes.c_float, ctypes.c_void_p)
_CLIP_SET_FN = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p)
_CLIP_GET_FN = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)  # (the address of a buffer kept alive here)

_SIGNATURES = {
    "cscd_version": (ctypes.c_uint32, []),
    "cscd_last_error": (ctypes.c_char_p, []),
    "cscd_free": (None, [ctypes.c_void_p]),
    "cscd_set_paint_threads": (None, [ctypes.c_int]),
    "cscd_paint_cache_set_limits": (None, [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64]),
    "cscd_paint_cache_clear": (None, []),
    "cscd_view_new": (_view_p, []),
    "cscd_view_delete": (None, [_view_p]),
    "cscd_view_load_file": (ctypes.c_int, [_view_p, ctypes.c_char_p]),
    "cscd_view_load_html": (ctypes.c_int, [_view_p, ctypes.c_char_p, ctypes.c_char_p]),
    "cscd_view_add_style_sheet": (ctypes.c_int, [_view_p, ctypes.c_char_p]),
    "cscd_view_set_viewport": (ctypes.c_int, [_view_p, ctypes.c_float, ctypes.c_float, ctypes.c_float]),
    "cscd_view_set_dark_mode": (ctypes.c_int, [_view_p, ctypes.c_int]),
    "cscd_view_title": (ctypes.c_void_p, [_view_p]),
    "cscd_view_update": (ctypes.c_int, [_view_p]),
    "cscd_view_needs_paint": (ctypes.c_int, [_view_p]),
    "cscd_view_next_timer_ms": (ctypes.c_int, [_view_p]),
    "cscd_view_content_height": (ctypes.c_float, [_view_p]),
    "cscd_view_paint": (ctypes.POINTER(ctypes.c_uint32), [_view_p, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]),
    "cscd_view_frame_stats": (ctypes.c_int, [_view_p, ctypes.POINTER(_Stats)]),
    "cscd_view_set_feature": (ctypes.c_int, [_view_p, ctypes.c_int, ctypes.c_int]),
    "cscd_view_feature": (ctypes.c_int, [_view_p, ctypes.c_int]),
    "cscd_view_inspect": (ctypes.c_int, [_view_p, _element]),
    "cscd_element_append_copy": (_element, [_view_p, _element, _element]),
    "cscd_element_fill": (ctypes.c_int, [_view_p, _element, ctypes.c_char_p, ctypes.c_char_p]),
    "cscd_view_bind_list": (ctypes.c_int, [_view_p, _element, _element, ctypes.c_size_t, ctypes.c_float, _LIST_FN, ctypes.c_void_p, _FREE_FN]),
    "cscd_view_list_set_count": (ctypes.c_int, [_view_p, _element, ctypes.c_size_t]),
    "cscd_view_list_refresh": (ctypes.c_int, [_view_p, _element]),
    "cscd_view_mouse_move": (ctypes.c_int, [_view_p, ctypes.c_float, ctypes.c_float, ctypes.c_int, ctypes.c_uint]),
    "cscd_view_mouse_down": (ctypes.c_int, [_view_p, ctypes.c_float, ctypes.c_float, ctypes.c_int, ctypes.c_uint, ctypes.c_int]),
    "cscd_view_mouse_up": (ctypes.c_int, [_view_p, ctypes.c_float, ctypes.c_float, ctypes.c_int, ctypes.c_uint]),
    "cscd_view_wheel": (ctypes.c_int, [_view_p, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_uint]),
    "cscd_view_key_down": (ctypes.c_int, [_view_p, ctypes.c_int, ctypes.c_uint, ctypes.c_char_p]),
    "cscd_view_key_up": (ctypes.c_int, [_view_p, ctypes.c_int, ctypes.c_uint, ctypes.c_char_p]),
    "cscd_view_text_input": (ctypes.c_int, [_view_p, ctypes.c_char_p]),
    "cscd_view_touch_down": (ctypes.c_int, [_view_p, ctypes.c_int, ctypes.c_float, ctypes.c_float]),
    "cscd_view_touch_move": (ctypes.c_int, [_view_p, ctypes.c_int, ctypes.c_float, ctypes.c_float]),
    "cscd_view_touch_up": (ctypes.c_int, [_view_p, ctypes.c_int, ctypes.c_float, ctypes.c_float]),
    "cscd_view_touch_cancel": (ctypes.c_int, [_view_p]),
    "cscd_view_on_edit_menu": (ctypes.c_int, [_view_p, _EDIT_MENU_FN, ctypes.c_void_p, _FREE_FN]),
    "cscd_view_on_magnifier": (ctypes.c_int, [_view_p, _MAGNIFIER_FN, ctypes.c_void_p, _FREE_FN]),
    "cscd_view_edit_action": (ctypes.c_int, [_view_p, ctypes.c_int]),
    "cscd_view_edit_actions": (ctypes.c_uint, [_view_p]),
    "cscd_view_selection_handles_shown": (ctypes.c_int, [_view_p]),
    "cscd_view_hide_selection_handles": (ctypes.c_int, [_view_p]),
    "cscd_view_set_clipboard": (ctypes.c_int, [_view_p, _CLIP_SET_FN, _CLIP_GET_FN, ctypes.c_void_p, _FREE_FN]),
    "cscd_view_copy_text": (ctypes.c_int, [_view_p, ctypes.c_char_p]),
    "cscd_view_copy_image": (ctypes.c_int, [_view_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_int, ctypes.c_int]),
    "cscd_view_clipboard_text": (ctypes.c_void_p, [_view_p]),
    "cscd_view_clipboard_files": (ctypes.c_void_p, [_view_p]),
    "cscd_view_set_timeout": (ctypes.c_int, [_view_p, ctypes.c_int, _FN, ctypes.c_void_p, _FREE_FN]),
    "cscd_view_set_interval": (ctypes.c_int, [_view_p, ctypes.c_int, _FN, ctypes.c_void_p, _FREE_FN]),
    "cscd_view_clear_timer": (ctypes.c_int, [_view_p, ctypes.c_int]),
    "cscd_view_query": (_element, [_view_p, ctypes.c_char_p]),
    "cscd_view_query_all": (ctypes.c_size_t, [_view_p, ctypes.c_char_p, ctypes.POINTER(_element), ctypes.c_size_t]),
    "cscd_view_element_by_id": (_element, [_view_p, ctypes.c_char_p]),
    "cscd_view_element_at": (_element, [_view_p, ctypes.c_float, ctypes.c_float]),
    "cscd_view_body": (_element, [_view_p]),
    "cscd_element_valid": (ctypes.c_int, [_view_p, _element]),
    "cscd_element_query": (_element, [_view_p, _element, ctypes.c_char_p]),
    "cscd_element_parent": (_element, [_view_p, _element]),
    "cscd_element_first_child": (_element, [_view_p, _element]),
    "cscd_element_next_sibling": (_element, [_view_p, _element]),
    "cscd_element_tag": (ctypes.c_void_p, [_view_p, _element]),
    "cscd_element_attribute": (ctypes.c_void_p, [_view_p, _element, ctypes.c_char_p]),
    "cscd_element_set_attribute": (ctypes.c_int, [_view_p, _element, ctypes.c_char_p, ctypes.c_char_p]),
    "cscd_element_remove_attribute": (ctypes.c_int, [_view_p, _element, ctypes.c_char_p]),
    "cscd_element_has_class": (ctypes.c_int, [_view_p, _element, ctypes.c_char_p]),
    "cscd_element_add_class": (ctypes.c_int, [_view_p, _element, ctypes.c_char_p]),
    "cscd_element_remove_class": (ctypes.c_int, [_view_p, _element, ctypes.c_char_p]),
    "cscd_element_toggle_class": (ctypes.c_int, [_view_p, _element, ctypes.c_char_p, ctypes.c_int]),
    "cscd_element_set_style": (ctypes.c_int, [_view_p, _element, ctypes.c_char_p, ctypes.c_char_p]),
    "cscd_element_text": (ctypes.c_void_p, [_view_p, _element]),
    "cscd_element_set_text": (ctypes.c_int, [_view_p, _element, ctypes.c_char_p]),
    "cscd_element_html": (ctypes.c_void_p, [_view_p, _element]),
    "cscd_element_set_html": (ctypes.c_int, [_view_p, _element, ctypes.c_char_p]),
    "cscd_element_value": (ctypes.c_void_p, [_view_p, _element]),
    "cscd_element_set_value": (ctypes.c_int, [_view_p, _element, ctypes.c_char_p]),
    "cscd_element_checked": (ctypes.c_int, [_view_p, _element]),
    "cscd_element_set_checked": (ctypes.c_int, [_view_p, _element, ctypes.c_int]),
    "cscd_element_append": (_element, [_view_p, _element, ctypes.c_char_p]),
    "cscd_element_append_html": (_element, [_view_p, _element, ctypes.c_char_p]),
    "cscd_element_remove": (ctypes.c_int, [_view_p, _element]),
    "cscd_element_rect": (ctypes.c_int, [_view_p, _element, ctypes.POINTER(ctypes.c_float)]),
    "cscd_element_focus": (ctypes.c_int, [_view_p, _element]),
    "cscd_element_click": (ctypes.c_int, [_view_p, _element]),
    "cscd_element_scroll_into_view": (ctypes.c_int, [_view_p, _element]),
    "cscd_element_invalidate": (ctypes.c_int, [_view_p, _element]),
    "cscd_element_on": (ctypes.c_int, [_view_p, _element, ctypes.c_char_p, _EVENT_FN, ctypes.c_void_p, _FREE_FN]),
    "cscd_element_off": (ctypes.c_int, [_view_p, _element, ctypes.c_char_p]),
    "cscd_event_prevent_default": (None, [ctypes.POINTER(_Event)]),
    "cscd_event_stop_propagation": (None, [ctypes.POINTER(_Event)]),
    "cscd_element_set_painter": (ctypes.c_int, [_view_p, _element, _PAINT_FN, ctypes.c_void_p, _FREE_FN]),
    "cscd_canvas_save": (None, [_canvas_p]),
    "cscd_canvas_restore": (None, [_canvas_p]),
    "cscd_canvas_translate": (None, [_canvas_p, ctypes.c_float, ctypes.c_float]),
    "cscd_canvas_scale": (None, [_canvas_p, ctypes.c_float, ctypes.c_float]),
    "cscd_canvas_rotate": (None, [_canvas_p, ctypes.c_float]),
    "cscd_canvas_clip_rect": (None, [_canvas_p, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float]),
    "cscd_canvas_set_global_alpha": (None, [_canvas_p, ctypes.c_float]),
    "cscd_canvas_clear": (None, [_canvas_p, ctypes.c_uint32]),
    "cscd_canvas_fill_rect": (None, [_canvas_p, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_uint32]),
    "cscd_canvas_fill_round_rect": (None, [_canvas_p] + [ctypes.c_float] * 5 + [ctypes.c_uint32]),
    "cscd_canvas_stroke_rect": (None, [_canvas_p] + [ctypes.c_float] * 4 + [ctypes.c_uint32, ctypes.c_float]),
    "cscd_canvas_draw_line": (None, [_canvas_p] + [ctypes.c_float] * 4 + [ctypes.c_uint32, ctypes.c_float]),
    "cscd_canvas_fill_circle": (None, [_canvas_p] + [ctypes.c_float] * 3 + [ctypes.c_uint32]),
    "cscd_canvas_fill_path": (None, [_canvas_p, ctypes.c_char_p, ctypes.c_uint32]),
    "cscd_canvas_stroke_path": (None, [_canvas_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_float]),
    "cscd_canvas_draw_text": (None, [_canvas_p, ctypes.c_char_p, ctypes.c_float, ctypes.c_float, ctypes.c_char_p, ctypes.c_float, ctypes.c_int,
                                     ctypes.c_uint32]),
    "cscd_canvas_measure_text": (ctypes.c_float, [_canvas_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_float, ctypes.c_int]),
    "cscd_window_new": (_window_p, [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]),
    "cscd_window_delete": (None, [_window_p]),
    "cscd_window_view": (_view_p, [_window_p]),
    "cscd_window_run": (ctypes.c_int, [_window_p]),
    "cscd_window_pump": (ctypes.c_int, [_window_p, ctypes.c_int]),
    "cscd_window_show": (ctypes.c_int, [_window_p]),
    "cscd_window_close": (ctypes.c_int, [_window_p]),
    "cscd_window_set_title": (ctypes.c_int, [_window_p, ctypes.c_char_p]),
    "cscd_window_backend": (ctypes.c_void_p, [_window_p]),
}
for _name, (_res, _args) in _SIGNATURES.items():
    _f = getattr(_lib, _name)
    _f.restype, _f.argtypes = _res, _args


class CascadeError(RuntimeError):
    pass


def _check(result):
    if result != 0:
        raise CascadeError(_lib.cscd_last_error().decode("utf-8", "replace") or "error %d" % result)


def _b(s):
    return None if s is None else s.encode("utf-8") if isinstance(s, str) else s


def _take(p):
    """A string the library made (freed here)."""
    if not p:
        return None
    try:
        return ctypes.string_at(p).decode("utf-8", "replace")
    finally:
        _lib.cscd_free(p)


def _actions(bits):
    return {a for a in EditAction if bits & (1 << a)}


def version():
    v = _lib.cscd_version()
    return (v >> 16, (v >> 8) & 255, v & 255)


class Feature(enum.IntEnum):
    PAINT_THREADS = 0
    PARTIAL_REPAINT = 1
    ANIMATIONS = 2
    SMOOTH_SCROLL = 3
    TOOLTIPS = 4
    LIVE_RELOAD = 5
    ACCESSIBILITY = 6
    INSPECTOR = 7
    STATS = 8
    GPU = 9
    WARN_FILES = 10
    WARN_CSS = 11
    WARN_SELECTORS = 12


class EditAction(enum.IntEnum):
    CUT = 0
    COPY = 1
    PASTE = 2
    SELECT_ALL = 3


_REFUSED = 6  # CSCD_ERR_REFUSED: not possible now (an answer, not an error)


class Key(enum.IntEnum):
    UNKNOWN, BACKSPACE, TAB, ENTER, ESCAPE, SPACE, DELETE, LEFT, RIGHT, UP, DOWN, HOME, END, PAGE_UP, PAGE_DOWN, INSERT, SHIFT, CONTROL, \
        ALT, META, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, A, C, V, X, Z, Y, CHARACTER = range(39)


class Event:
    """What a listener gets; prevent_default and stop_propagation work while the listener runs."""

    def __init__(self, view, raw):
        e = raw.contents
        self._raw = raw
        self.type = (e.type or b"").decode()
        self.target = Element(view, e.target) if e.target else None
        self.current_target = Element(view, e.current_target) if e.current_target else None
        self.x, self.y, self.offset_x, self.offset_y = e.x, e.y, e.offset_x, e.offset_y
        self.delta_x, self.delta_y = e.delta_x, e.delta_y
        self.button, self.buttons, self.detail, self.mods = e.button, e.buttons, e.detail, e.mods
        self.key = (e.key or b"").decode()

    def prevent_default(self):
        _lib.cscd_event_prevent_default(self._raw)

    def stop_propagation(self):
        _lib.cscd_event_stop_propagation(self._raw)


class Canvas:
    """The 2D canvas a painter draws on (only while the painter runs). Colors are 0xRRGGBBAA."""

    def __init__(self, p):
        self._p = p

    def save(self): _lib.cscd_canvas_save(self._p)
    def restore(self): _lib.cscd_canvas_restore(self._p)
    def translate(self, x, y): _lib.cscd_canvas_translate(self._p, x, y)
    def scale(self, sx, sy): _lib.cscd_canvas_scale(self._p, sx, sy)
    def rotate(self, radians): _lib.cscd_canvas_rotate(self._p, radians)
    def clip_rect(self, x, y, w, h): _lib.cscd_canvas_clip_rect(self._p, x, y, w, h)
    def set_global_alpha(self, a): _lib.cscd_canvas_set_global_alpha(self._p, a)
    def clear(self, rgba): _lib.cscd_canvas_clear(self._p, rgba)
    def fill_rect(self, x, y, w, h, rgba): _lib.cscd_canvas_fill_rect(self._p, x, y, w, h, rgba)
    def fill_round_rect(self, x, y, w, h, r, rgba): _lib.cscd_canvas_fill_round_rect(self._p, x, y, w, h, r, rgba)
    def stroke_rect(self, x, y, w, h, rgba, width=1): _lib.cscd_canvas_stroke_rect(self._p, x, y, w, h, rgba, width)
    def draw_line(self, x0, y0, x1, y1, rgba, width=1): _lib.cscd_canvas_draw_line(self._p, x0, y0, x1, y1, rgba, width)
    def fill_circle(self, x, y, r, rgba): _lib.cscd_canvas_fill_circle(self._p, x, y, r, rgba)
    def fill_path(self, svg_path, rgba): _lib.cscd_canvas_fill_path(self._p, _b(svg_path), rgba)
    def stroke_path(self, svg_path, rgba, width=1): _lib.cscd_canvas_stroke_path(self._p, _b(svg_path), rgba, width)

    def draw_text(self, text, x, y, family="sans-serif", size=13, weight=400, rgba=0x000000FF):
        _lib.cscd_canvas_draw_text(self._p, _b(text), x, y, _b(family), size, weight, rgba)

    def measure_text(self, text, family="sans-serif", size=13, weight=400):
        return _lib.cscd_canvas_measure_text(self._p, _b(text), _b(family), size, weight)


class Element:
    """A handle: it stays safe after the element is gone (calls then raise CascadeError)."""

    def __init__(self, view, handle):
        self.view, self.handle = view, handle

    def __eq__(self, other): return isinstance(other, Element) and other.handle == self.handle
    def __hash__(self): return hash(self.handle)
    def __repr__(self): return "<Element %s>" % (self.tag if self.valid else "gone")

    def _e(self, h):
        return Element(self.view, h) if h else None

    @property
    def _v(self): return self.view._p

    @property
    def valid(self): return bool(_lib.cscd_element_valid(self._v, self.handle))
    @property
    def tag(self): return _take(_lib.cscd_element_tag(self._v, self.handle))
    @property
    def parent(self): return self._e(_lib.cscd_element_parent(self._v, self.handle))
    @property
    def children(self):
        out, c = [], _lib.cscd_element_first_child(self._v, self.handle)
        while c:
            out.append(Element(self.view, c))
            c = _lib.cscd_element_next_sibling(self._v, c)
        return out

    def query(self, selector): return self._e(_lib.cscd_element_query(self._v, self.handle, _b(selector)))
    def attribute(self, name): return _take(_lib.cscd_element_attribute(self._v, self.handle, _b(name)))
    def set_attribute(self, name, value): _check(_lib.cscd_element_set_attribute(self._v, self.handle, _b(name), _b(str(value))))
    def remove_attribute(self, name): _check(_lib.cscd_element_remove_attribute(self._v, self.handle, _b(name)))
    def has_class(self, name): return bool(_lib.cscd_element_has_class(self._v, self.handle, _b(name)))
    def add_class(self, name): _check(_lib.cscd_element_add_class(self._v, self.handle, _b(name)))
    def remove_class(self, name): _check(_lib.cscd_element_remove_class(self._v, self.handle, _b(name)))
    def toggle_class(self, name, on): _check(_lib.cscd_element_toggle_class(self._v, self.handle, _b(name), int(bool(on))))
    def set_style(self, prop, value): _check(_lib.cscd_element_set_style(self._v, self.handle, _b(prop), _b(str(value))))

    @property
    def text(self): return _take(_lib.cscd_element_text(self._v, self.handle))
    def set_text(self, text): _check(_lib.cscd_element_set_text(self._v, self.handle, _b(text)))
    @property
    def html(self): return _take(_lib.cscd_element_html(self._v, self.handle))
    def set_html(self, html): _check(_lib.cscd_element_set_html(self._v, self.handle, _b(html)))
    @property
    def value(self): return _take(_lib.cscd_element_value(self._v, self.handle))
    def set_value(self, value): _check(_lib.cscd_element_set_value(self._v, self.handle, _b(str(value))))
    @property
    def checked(self): return bool(_lib.cscd_element_checked(self._v, self.handle))
    def set_checked(self, on): _check(_lib.cscd_element_set_checked(self._v, self.handle, int(bool(on))))

    def append(self, tag): return self._e(_lib.cscd_element_append(self._v, self.handle, _b(tag)))
    def append_html(self, html): return self._e(_lib.cscd_element_append_html(self._v, self.handle, _b(html)))
    def append_copy(self, source):
        """A copy of source (a <template>'s first element, or any element) appended here."""
        h = _lib.cscd_element_append_copy(self._v, self.handle, source.handle)
        if not h:
            raise CascadeError(_lib.cscd_last_error().decode())
        return Element(self.view, h)
    def fill(self, **values):
        """Replaces {{key}} in the text and attributes of this element and what it holds."""
        for k, v in values.items():
            _check(_lib.cscd_element_fill(self._v, self.handle, _b(k), _b(str(v))))
    def remove(self): _check(_lib.cscd_element_remove(self._v, self.handle))

    @property
    def rect(self):
        r = (ctypes.c_float * 4)()
        _check(_lib.cscd_element_rect(self._v, self.handle, r))
        return tuple(r)

    def focus(self): _check(_lib.cscd_element_focus(self._v, self.handle))
    def click(self): _check(_lib.cscd_element_click(self._v, self.handle))
    def scroll_into_view(self): _check(_lib.cscd_element_scroll_into_view(self._v, self.handle))
    def invalidate(self): _check(_lib.cscd_element_invalidate(self._v, self.handle))

    def on(self, type, fn):
        """fn(event) for each event of that type here or under (as addEventListener)."""
        view = self.view
        cb = _EVENT_FN(lambda v, raw, user: fn(Event(view, raw)))
        view._keep(cb)
        _check(_lib.cscd_element_on(self._v, self.handle, _b(type), cb, None, _FREE_FN()))
        return fn

    def off(self, type): _check(_lib.cscd_element_off(self._v, self.handle, _b(type)))

    def set_painter(self, fn):
        """fn(canvas, width, height) draws the element's content whenever it is painted."""
        cb = _PAINT_FN(lambda c, w, h, user: fn(Canvas(c), w, h))
        self.view._keep(cb)
        _check(_lib.cscd_element_set_painter(self._v, self.handle, cb, None, _FREE_FN()))


class View:
    """A page: load it, change it, give it input, paint it (a Window shows one by itself)."""

    def __init__(self, _pointer=None, _owned=True):
        self._p = _pointer or _lib.cscd_view_new()
        self._owned = _owned and _pointer is None
        self._callbacks = []

    def _keep(self, cb):
        self._callbacks.append(cb)

    def close(self):
        if self._owned and self._p:
            _lib.cscd_view_delete(self._p)
        self._p = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self): return self
    def __exit__(self, *a): self.close()

    def load_file(self, path): _check(_lib.cscd_view_load_file(self._p, _b(path)))
    def load_html(self, html, base_dir=None): _check(_lib.cscd_view_load_html(self._p, _b(html), _b(base_dir)))
    def add_style_sheet(self, css): _check(_lib.cscd_view_add_style_sheet(self._p, _b(css)))
    def set_viewport(self, width, height, dpr=1.0): _check(_lib.cscd_view_set_viewport(self._p, width, height, dpr))
    def set_dark_mode(self, dark): _check(_lib.cscd_view_set_dark_mode(self._p, int(bool(dark))))
    @property
    def title(self): return _take(_lib.cscd_view_title(self._p))

    def update(self): return bool(_lib.cscd_view_update(self._p))
    @property
    def content_height(self): return _lib.cscd_view_content_height(self._p)

    def paint(self):
        """The frame: (width, height, pixels) with pixels premultiplied BGRA, one int per pixel."""
        w, h = ctypes.c_int(), ctypes.c_int()
        p = _lib.cscd_view_paint(self._p, ctypes.byref(w), ctypes.byref(h))
        return w.value, h.value, p[:w.value * h.value] if p else []

    @property
    def stats(self):
        s = _Stats()
        _check(_lib.cscd_view_frame_stats(self._p, ctypes.byref(s)))
        return {f: getattr(s, f) for f, _ in _Stats._fields_}

    def set_feature(self, feature, on): _check(_lib.cscd_view_set_feature(self._p, int(feature), int(bool(on))))
    def feature(self, feature): return bool(_lib.cscd_view_feature(self._p, int(feature)))
    def inspect(self, element=None): _check(_lib.cscd_view_inspect(self._p, element.handle if element else 0))

    def query(self, selector):
        h = _lib.cscd_view_query(self._p, _b(selector))
        return Element(self, h) if h else None

    def query_all(self, selector):
        n = _lib.cscd_view_query_all(self._p, _b(selector), None, 0)
        out = (_element * max(n, 1))()
        n = _lib.cscd_view_query_all(self._p, _b(selector), out, n)
        return [Element(self, out[i]) for i in range(n)]

    def __getitem__(self, selector):
        e = self.query(selector)
        if e is None:
            raise KeyError(selector)
        return e

    def element_at(self, x, y):
        h = _lib.cscd_view_element_at(self._p, x, y)
        return Element(self, h) if h else None

    @property
    def body(self): return Element(self, _lib.cscd_view_body(self._p))

    def bind_list(self, box, row, count, row_height, fill):
        """A long list that makes only the rows in sight: fill(row_element, index)."""
        view = self
        cb = _LIST_FN(lambda v, r, i, user: fill(Element(view, r), i))
        self._keep(cb)
        _check(_lib.cscd_view_bind_list(self._p, box.handle, row.handle, count, row_height, cb, None, _FREE_FN()))

    def list_set_count(self, box, count): _check(_lib.cscd_view_list_set_count(self._p, box.handle, count))
    def list_refresh(self, box): _check(_lib.cscd_view_list_refresh(self._p, box.handle))

    def mouse_move(self, x, y, buttons=0, mods=0): _check(_lib.cscd_view_mouse_move(self._p, x, y, buttons, mods))
    def mouse_down(self, x, y, button=0, mods=0, clicks=1): _check(_lib.cscd_view_mouse_down(self._p, x, y, button, mods, clicks))
    def mouse_up(self, x, y, button=0, mods=0): _check(_lib.cscd_view_mouse_up(self._p, x, y, button, mods))
    def wheel(self, x, y, dx, dy, mods=0): _check(_lib.cscd_view_wheel(self._p, x, y, dx, dy, mods))
    def key_down(self, key, name="", mods=0): _check(_lib.cscd_view_key_down(self._p, int(key), mods, _b(name)))
    def key_up(self, key, name="", mods=0): _check(_lib.cscd_view_key_up(self._p, int(key), mods, _b(name)))
    def text_input(self, text): _check(_lib.cscd_view_text_input(self._p, _b(text)))

    # touch, and selecting text by touch
    def touch_down(self, id, x, y): _check(_lib.cscd_view_touch_down(self._p, id, x, y))
    def touch_move(self, id, x, y): _check(_lib.cscd_view_touch_move(self._p, id, x, y))
    def touch_up(self, id, x, y): _check(_lib.cscd_view_touch_up(self._p, id, x, y))
    def touch_cancel(self): _check(_lib.cscd_view_touch_cancel(self._p))

    def on_edit_menu(self, fn):
        """fn(show, (x, y, width, height), {EditAction...}): the edit menu by a selection made by touch
        (None: the view's own menu). A window's view has the system's; this replaces it."""
        cb = _EDIT_MENU_FN(lambda v, show, r, actions, user: fn(bool(show), tuple(r[i] for i in range(4)), _actions(actions)))
        if fn:
            self._keep(cb)
        _check(_lib.cscd_view_on_edit_menu(self._p, cb if fn else _EDIT_MENU_FN(), None, _FREE_FN()))

    def on_magnifier(self, fn):
        """fn(show, x, y) while a selection handle is dragged (None: no more)."""
        cb = _MAGNIFIER_FN(lambda v, show, x, y, user: fn(bool(show), x, y))
        if fn:
            self._keep(cb)
        _check(_lib.cscd_view_on_magnifier(self._p, cb if fn else _MAGNIFIER_FN(), None, _FREE_FN()))

    def edit_action(self, action):
        """Cut, copy, paste or select all in the focused text field; False when it cannot now."""
        r = _lib.cscd_view_edit_action(self._p, int(action))
        if r == _REFUSED:
            return False
        _check(r)
        return True

    @property
    def edit_actions(self): return _actions(_lib.cscd_view_edit_actions(self._p))
    @property
    def selection_handles_shown(self): return bool(_lib.cscd_view_selection_handles_shown(self._p))
    def hide_selection_handles(self): _check(_lib.cscd_view_hide_selection_handles(self._p))

    # the clipboard
    def set_clipboard(self, set_text, get_text):
        """A clipboard of your own for a view without a window: set_text(str), get_text() -> str or None."""
        buf = [None]

        def get(user):
            s = get_text()
            if s is None:
                return None
            buf[0] = ctypes.create_string_buffer(_b(s))  # (alive until the next call)
            return ctypes.addressof(buf[0])

        cs, cg = _CLIP_SET_FN(lambda t, user: set_text((t or b"").decode("utf-8", "replace"))), _CLIP_GET_FN(get)
        self._keep((cs, cg, buf))
        _check(_lib.cscd_view_set_clipboard(self._p, cs, cg, None, _FREE_FN()))

    def copy_text(self, text): return _lib.cscd_view_copy_text(self._p, _b(text)) == 0

    def copy_image(self, width, height, pixels):
        """Premultiplied BGRA pixels, one int per pixel, rows packed; False without a clipboard for pictures."""
        arr = (ctypes.c_uint32 * (width * height))(*pixels)
        r = _lib.cscd_view_copy_image(self._p, arr, width, height)
        if r == _REFUSED:
            return False
        _check(r)
        return True

    @property
    def clipboard_text(self): return _take(_lib.cscd_view_clipboard_text(self._p)) or ""
    @property
    def clipboard_files(self):
        s = _take(_lib.cscd_view_clipboard_files(self._p)) or ""
        return [line for line in s.split("\n") if line]

    def set_timeout(self, ms, fn):
        cb = _FN(lambda user: fn())
        self._keep(cb)
        return _lib.cscd_view_set_timeout(self._p, ms, cb, None, _FREE_FN())

    def set_interval(self, ms, fn):
        cb = _FN(lambda user: fn())
        self._keep(cb)
        return _lib.cscd_view_set_interval(self._p, ms, cb, None, _FREE_FN())

    def clear_timer(self, id): _check(_lib.cscd_view_clear_timer(self._p, id))


class Window:
    """A native window that shows a view (Win32, Cocoa, X11)."""

    def __init__(self, title="cascade", width=1024, height=720):
        self._p = _lib.cscd_window_new(_b(title), width, height)
        if not self._p:
            raise CascadeError(_lib.cscd_last_error().decode() or "no window")
        self.view = View(_lib.cscd_window_view(self._p), _owned=False)

    def run(self): return _lib.cscd_window_run(self._p)
    def pump(self, wait=False): return bool(_lib.cscd_window_pump(self._p, int(wait)))
    def show(self): _check(_lib.cscd_window_show(self._p))
    def close(self): _check(_lib.cscd_window_close(self._p))
    def set_title(self, title): _check(_lib.cscd_window_set_title(self._p, _b(title)))
    @property
    def backend(self): return _take(_lib.cscd_window_backend(self._p))

    def __del__(self):
        try:
            if self._p:
                _lib.cscd_window_delete(self._p)
        except Exception:
            pass
        self._p = None
