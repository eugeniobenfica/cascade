/* cascade — the C API: views, elements, events, painters, timers and windows,
 * for C programs and for other languages (Python's ctypes, Rust, C#, Go, Zig...).
 *
 * Elements are handles (cscd_element), not pointers: a handle stops working when its element leaves
 * the document (removed, replaced by setting HTML or text, or a new page loaded), and the
 * functions given it then fail with CSCD_ERR_INVALID_HANDLE instead of touching freed memory. Ask
 * the view again for an element that may have been rebuilt.
 *
 * Text is UTF-8. Text a function returns as char* is the caller's: free it with cscd_free.
 * Functions that do something return a cscd_result; those that return a value return 0 / NULL when
 * they fail, and cscd_last_error says why. Call everything on the thread that made the view (the
 * window's thread), except cscd_window_post. A thin layer over the native C API (cascade.h), for
 * bindings that want handles that cannot dangle and a stable ABI.
 */
#ifndef CASCADE_C_H
#define CASCADE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CSCD_C_API_VERSION 1

/* CASCADE_C_EXPORTS while building the shared library (cascade_c); CASCADE_C_DLL in Windows
 * programs that link to it; neither with the static library. */
#if defined(_WIN32) && defined(CASCADE_C_EXPORTS)
#define CSCD_API __declspec(dllexport)
#elif defined(_WIN32) && defined(CASCADE_C_DLL)
#define CSCD_API __declspec(dllimport)
#elif defined(CASCADE_C_EXPORTS)
#define CSCD_API __attribute__((visibility("default")))
#else
#define CSCD_API
#endif

typedef struct cscd_view cscd_view;
typedef struct cscd_window cscd_window;
typedef struct cscd_canvas cscd_canvas;
typedef uint64_t cscd_element; /* 0: no element */

typedef enum cscd_result {
    CSCD_OK = 0,
    CSCD_ERR_INVALID_HANDLE = 1, /* the element left the document, or never was */
    CSCD_ERR_ARGUMENT = 2,       /* a NULL view, a NULL string... */
    CSCD_ERR_NOT_FOUND = 3,      /* a file that cannot be read */
    CSCD_ERR_UNAVAILABLE = 4,    /* built without the window */
    CSCD_ERR_INTERNAL = 5,       /* something failed inside (cscd_last_error has the message) */
    CSCD_ERR_REFUSED = 6         /* not possible now: nothing selected, a read-only field, no clipboard... */
} cscd_result;

/* Modifier keys, as a bit set. */
enum { CSCD_MOD_SHIFT = 1, CSCD_MOD_CTRL = 2, CSCD_MOD_ALT = 4, CSCD_MOD_META = 8 };

/* Keys: the values of CscdKey (view.h), which defines the same names when it comes first. */
#ifdef CASCADE_VIEW_H
typedef CscdKey cscd_key;
#else
#define CASCADE_KEYS_DEFINED
typedef enum cscd_key {
    CSCD_KEY_UNKNOWN, CSCD_KEY_BACKSPACE, CSCD_KEY_TAB, CSCD_KEY_ENTER, CSCD_KEY_ESCAPE, CSCD_KEY_SPACE, CSCD_KEY_DELETE,
    CSCD_KEY_LEFT, CSCD_KEY_RIGHT, CSCD_KEY_UP, CSCD_KEY_DOWN, CSCD_KEY_HOME, CSCD_KEY_END, CSCD_KEY_PAGE_UP, CSCD_KEY_PAGE_DOWN,
    CSCD_KEY_INSERT, CSCD_KEY_SHIFT, CSCD_KEY_CONTROL, CSCD_KEY_ALT, CSCD_KEY_META,
    CSCD_KEY_F1, CSCD_KEY_F2, CSCD_KEY_F3, CSCD_KEY_F4, CSCD_KEY_F5, CSCD_KEY_F6, CSCD_KEY_F7, CSCD_KEY_F8, CSCD_KEY_F9, CSCD_KEY_F10, CSCD_KEY_F11, CSCD_KEY_F12,
    CSCD_KEY_A, CSCD_KEY_C, CSCD_KEY_V, CSCD_KEY_X, CSCD_KEY_Z, CSCD_KEY_Y, CSCD_KEY_CHARACTER
} cscd_key;
#endif

/* The features a view can turn off or on (see CscdFeature in view.h for what each does). */
#ifdef CASCADE_VIEW_H
typedef CscdFeature cscd_feature;
#else
#define CASCADE_FEATURES_DEFINED
typedef enum cscd_feature {
    CSCD_FEATURE_PAINT_THREADS, CSCD_FEATURE_PARTIAL_REPAINT, CSCD_FEATURE_ANIMATIONS, CSCD_FEATURE_SMOOTH_SCROLL,
    CSCD_FEATURE_TOOLTIPS, CSCD_FEATURE_LIVE_RELOAD, CSCD_FEATURE_ACCESSIBILITY, CSCD_FEATURE_INSPECTOR, CSCD_FEATURE_STATS,
    CSCD_FEATURE_GPU, CSCD_FEATURE_WARN_FILES, CSCD_FEATURE_WARN_CSS, CSCD_FEATURE_WARN_SELECTORS, CSCD_FEATURE_COUNT
} cscd_feature;
#endif

/* What an edit menu offers (CscdEditAction in view.h); cscd_view_edit_actions returns a bit set of
 * CSCD_EDIT_BIT()s. */
#ifdef CASCADE_VIEW_H
typedef CscdEditAction cscd_edit_action;
#else
#define CASCADE_EDIT_ACTIONS_DEFINED
typedef enum cscd_edit_action { CSCD_EDIT_CUT, CSCD_EDIT_COPY, CSCD_EDIT_PASTE, CSCD_EDIT_SELECT_ALL } cscd_edit_action;
#define CSCD_EDIT_BIT(a) (1u << (unsigned)(a))
#endif

typedef void (*cscd_free_fn)(void* user); /* called once when a callback is dropped, to free user */
typedef void (*cscd_fn)(void* user);

/* ---- library ---------------------------------------------------------------------------- */

CSCD_API uint32_t cscd_version(void);       /* (major << 16) | (minor << 8) | patch */
CSCD_API const char* cscd_last_error(void); /* this thread's last failure; "" when none */
CSCD_API void cscd_free(void* p);
/* Threads that paint a frame by bands (0: the machine's, at most 8; 1: one). */
CSCD_API void cscd_set_paint_threads(int n);
/* What painting keeps between frames (cscdPaintCacheSetLimits in canvas.h): the memory and number of
 * blurred shadows (0: none is kept), the number of glyphs. */
CSCD_API void cscd_paint_cache_set_limits(uint64_t shadow_bytes, uint64_t shadow_count, uint64_t glyph_count);
CSCD_API void cscd_paint_cache_clear(void);
typedef struct cscd_paint_cache_stats {
    uint64_t shadow_count, shadow_bytes, glyph_count, shadow_hits, shadow_misses, shadow_evictions, glyph_evictions;
} cscd_paint_cache_stats;
CSCD_API void cscd_paint_cache_get_stats(cscd_paint_cache_stats* out);

/* ---- views ------------------------------------------------------------------------------ */

CSCD_API cscd_view* cscd_view_new(void);
CSCD_API void cscd_view_delete(cscd_view* v); /* (not a window's view) */

CSCD_API cscd_result cscd_view_load_file(cscd_view* v, const char* path);
CSCD_API cscd_result cscd_view_load_html(cscd_view* v, const char* html, const char* base_dir);
CSCD_API cscd_result cscd_view_add_style_sheet(cscd_view* v, const char* css);
CSCD_API cscd_result cscd_view_set_viewport(cscd_view* v, float width, float height, float dpr);
CSCD_API cscd_result cscd_view_set_dark_mode(cscd_view* v, int dark);
CSCD_API char* cscd_view_title(cscd_view* v);

/* Brings style and layout up to date and runs timers and animations; 1 when a paint is due. */
CSCD_API int cscd_view_update(cscd_view* v);
CSCD_API int cscd_view_needs_paint(cscd_view* v);
CSCD_API int cscd_view_next_timer_ms(cscd_view* v); /* -1: nothing due */
CSCD_API float cscd_view_content_height(cscd_view* v);
/* Paints the view into its own frame (width * dpr by height * dpr, premultiplied BGRA, rows
 * packed) and returns the pixels, good until the next paint. Only what changed is painted again. */
CSCD_API const uint32_t* cscd_view_paint(cscd_view* v, int* width, int* height);
/* Draws the view into a canvas (in a painter, or to compose it with something else). */
CSCD_API cscd_result cscd_view_paint_canvas(cscd_view* v, cscd_canvas* c);

/* What the last style pass, layout and paint cost (see CscdFrameStats). */
typedef struct cscd_frame_stats {
    double style_ms, layout_ms, paint_ms;
    int styles_computed, styles_reused;
    double painted_area; /* device pixels */
    int partial_paint;
} cscd_frame_stats;
CSCD_API cscd_result cscd_view_frame_stats(cscd_view* v, cscd_frame_stats* out);

CSCD_API cscd_result cscd_view_set_feature(cscd_view* v, cscd_feature feature, int on);
CSCD_API int cscd_view_feature(cscd_view* v, cscd_feature feature);
/* The inspector over the page, showing e (0: closed). */
CSCD_API cscd_result cscd_view_inspect(cscd_view* v, cscd_element e);

/* A copy of a <template>'s first element (or of any element) appended to parent; its {{key}}s are
 * filled with cscd_element_fill. */
CSCD_API cscd_element cscd_element_append_copy(cscd_view* v, cscd_element parent, cscd_element source);
CSCD_API cscd_result cscd_element_fill(cscd_view* v, cscd_element e, const char* key, const char* value);
/* A list of count rows that makes only those in sight (see cscdViewBindList). */
typedef void (*cscd_list_fill_fn)(cscd_view* v, cscd_element row, size_t index, void* user);
CSCD_API cscd_result cscd_view_bind_list(cscd_view* v, cscd_element box, cscd_element row, size_t count, float row_height,
                                         cscd_list_fill_fn fill, void* user, cscd_free_fn free_user);
CSCD_API cscd_result cscd_view_list_set_count(cscd_view* v, cscd_element box, size_t count);
CSCD_API cscd_result cscd_view_list_refresh(cscd_view* v, cscd_element box);

/* Input, in CSS pixels from the viewport's top-left. buttons: bit 0 left, 1 right, 2 middle. */
CSCD_API cscd_result cscd_view_mouse_move(cscd_view* v, float x, float y, int buttons, unsigned mods);
CSCD_API cscd_result cscd_view_mouse_down(cscd_view* v, float x, float y, int button, unsigned mods, int clicks);
CSCD_API cscd_result cscd_view_mouse_up(cscd_view* v, float x, float y, int button, unsigned mods);
CSCD_API cscd_result cscd_view_mouse_leave(cscd_view* v);
CSCD_API cscd_result cscd_view_wheel(cscd_view* v, float x, float y, float dx, float dy, unsigned mods);
CSCD_API cscd_result cscd_view_key_down(cscd_view* v, cscd_key key, unsigned mods, const char* key_name);
CSCD_API cscd_result cscd_view_key_up(cscd_view* v, cscd_key key, unsigned mods, const char* key_name);
CSCD_API cscd_result cscd_view_text_input(cscd_view* v, const char* utf8);
CSCD_API int cscd_view_cursor(cscd_view* v); /* a CscdCursor value */

/* Touch, in CSS pixels; id tells the fingers apart. One finger scrolls, taps click, a long press
 * opens the context menu (or, on a text field, selects the word), two fingers pan and pinch. */
CSCD_API cscd_result cscd_view_touch_down(cscd_view* v, int id, float x, float y);
CSCD_API cscd_result cscd_view_touch_move(cscd_view* v, int id, float x, float y);
CSCD_API cscd_result cscd_view_touch_up(cscd_view* v, int id, float x, float y);
CSCD_API cscd_result cscd_view_touch_cancel(cscd_view* v);

/* Selecting text by touch (see cscdViewOnEditMenu in view.h): the view draws handles at the ends of
 * a selection made by touch and asks for an edit menu next to it. A window gives its view the
 * system's menu; fn replaces it (rect: x, y, width, height of the selection, CSS pixels; actions:
 * CSCD_EDIT_BIT()s; show 0: hide it). Without one, the view opens a menu of its own. */
typedef void (*cscd_edit_menu_fn)(cscd_view* v, int show, const float rect[4], unsigned actions, void* user);
typedef void (*cscd_magnifier_fn)(cscd_view* v, int show, float x, float y, void* user);
CSCD_API cscd_result cscd_view_on_edit_menu(cscd_view* v, cscd_edit_menu_fn fn, void* user, cscd_free_fn free_user);
CSCD_API cscd_result cscd_view_on_magnifier(cscd_view* v, cscd_magnifier_fn fn, void* user, cscd_free_fn free_user);
/* Cut, copy, paste or select all in the focused text field: CSCD_ERR_REFUSED when it cannot. */
CSCD_API cscd_result cscd_view_edit_action(cscd_view* v, cscd_edit_action action);
CSCD_API unsigned cscd_view_edit_actions(cscd_view* v); /* what it can do now */
CSCD_API int cscd_view_selection_handles_shown(cscd_view* v);
CSCD_API cscd_result cscd_view_hide_selection_handles(cscd_view* v);

/* The clipboard. A window's view has the system's; a view of your own gets yours here (get returns
 * the text, copied at once, or NULL; set receives what text fields copy). The rest goes through
 * whichever the view has: CSCD_ERR_REFUSED (or NULL) without one. Pixels: premultiplied BGRA, rows
 * packed. Files: their paths one per line (on phones, copies; a picture as a PNG file). */
typedef void (*cscd_clipboard_set_fn)(const char* utf8, void* user);
typedef const char* (*cscd_clipboard_get_fn)(void* user);
CSCD_API cscd_result cscd_view_set_clipboard(cscd_view* v, cscd_clipboard_set_fn set, cscd_clipboard_get_fn get, void* user,
                                             cscd_free_fn free_user);
CSCD_API cscd_result cscd_view_copy_text(cscd_view* v, const char* utf8);
CSCD_API cscd_result cscd_view_copy_image(cscd_view* v, const uint32_t* pixels, int width, int height);
CSCD_API char* cscd_view_clipboard_text(cscd_view* v);
CSCD_API char* cscd_view_clipboard_files(cscd_view* v);

/* Timers on the view's thread: they run inside cscd_view_update. Returns the timer's id (0: failed). */
CSCD_API int cscd_view_set_timeout(cscd_view* v, int ms, cscd_fn fn, void* user, cscd_free_fn free_user);
CSCD_API int cscd_view_set_interval(cscd_view* v, int ms, cscd_fn fn, void* user, cscd_free_fn free_user);
CSCD_API cscd_result cscd_view_clear_timer(cscd_view* v, int id);

/* ---- elements --------------------------------------------------------------------------- */

CSCD_API cscd_element cscd_view_query(cscd_view* v, const char* selector); /* the first match, or 0 */
/* All matches: fills out with up to capacity of them and returns how many there are. */
CSCD_API size_t cscd_view_query_all(cscd_view* v, const char* selector, cscd_element* out, size_t capacity);
CSCD_API cscd_element cscd_view_element_by_id(cscd_view* v, const char* id);
CSCD_API cscd_element cscd_view_element_at(cscd_view* v, float x, float y);
CSCD_API cscd_element cscd_view_body(cscd_view* v);
CSCD_API int cscd_element_valid(cscd_view* v, cscd_element e);

CSCD_API cscd_element cscd_element_query(cscd_view* v, cscd_element e, const char* selector);
CSCD_API cscd_element cscd_element_parent(cscd_view* v, cscd_element e);
CSCD_API cscd_element cscd_element_first_child(cscd_view* v, cscd_element e); /* elements only */
CSCD_API cscd_element cscd_element_next_sibling(cscd_view* v, cscd_element e);

CSCD_API char* cscd_element_tag(cscd_view* v, cscd_element e);
CSCD_API char* cscd_element_attribute(cscd_view* v, cscd_element e, const char* name); /* NULL when it has none */
CSCD_API cscd_result cscd_element_set_attribute(cscd_view* v, cscd_element e, const char* name, const char* value);
CSCD_API cscd_result cscd_element_remove_attribute(cscd_view* v, cscd_element e, const char* name);
CSCD_API int cscd_element_has_class(cscd_view* v, cscd_element e, const char* name);
CSCD_API cscd_result cscd_element_add_class(cscd_view* v, cscd_element e, const char* name);
CSCD_API cscd_result cscd_element_remove_class(cscd_view* v, cscd_element e, const char* name);
CSCD_API cscd_result cscd_element_toggle_class(cscd_view* v, cscd_element e, const char* name, int on);
CSCD_API cscd_result cscd_element_set_style(cscd_view* v, cscd_element e, const char* property, const char* value);

CSCD_API char* cscd_element_text(cscd_view* v, cscd_element e);
CSCD_API cscd_result cscd_element_set_text(cscd_view* v, cscd_element e, const char* text);
CSCD_API char* cscd_element_html(cscd_view* v, cscd_element e);
CSCD_API cscd_result cscd_element_set_html(cscd_view* v, cscd_element e, const char* html);
CSCD_API char* cscd_element_value(cscd_view* v, cscd_element e);
CSCD_API cscd_result cscd_element_set_value(cscd_view* v, cscd_element e, const char* value);
CSCD_API int cscd_element_checked(cscd_view* v, cscd_element e);
CSCD_API cscd_result cscd_element_set_checked(cscd_view* v, cscd_element e, int checked);

CSCD_API cscd_element cscd_element_append(cscd_view* v, cscd_element parent, const char* tag);
CSCD_API cscd_element cscd_element_append_html(cscd_view* v, cscd_element parent, const char* html); /* the first element made */
CSCD_API cscd_result cscd_element_remove(cscd_view* v, cscd_element e);

/* The border box after layout, relative to the viewport: x, y, width, height. */
CSCD_API cscd_result cscd_element_rect(cscd_view* v, cscd_element e, float out[4]);
CSCD_API cscd_result cscd_element_focus(cscd_view* v, cscd_element e);
CSCD_API cscd_result cscd_element_click(cscd_view* v, cscd_element e);
CSCD_API cscd_result cscd_element_scroll_into_view(cscd_view* v, cscd_element e);
/* Its painter's drawing changed: just its box is painted again. */
CSCD_API cscd_result cscd_element_invalidate(cscd_view* v, cscd_element e);

/* ---- events ----------------------------------------------------------------------------- */

typedef struct cscd_event {
    const char* type; /* "click", "input", "keydown"... */
    cscd_element target, current_target;
    float x, y;               /* CSS pixels, viewport */
    float offset_x, offset_y; /* from current_target's border box */
    float delta_x, delta_y;   /* wheel */
    int button, buttons, detail;
    const char* key; /* "a", "Enter", "ArrowLeft"... */
    unsigned mods;
    void* internal_; /* (for cscd_event_prevent_default and cscd_event_stop_propagation) */
} cscd_event;

typedef void (*cscd_event_fn)(cscd_view* v, const cscd_event* event, void* user);

/* Listens for events of a type ("click", "input", "change", "keydown"...) on e and its children.
 * free_user (may be NULL) runs when the listener goes: removed, or its element deleted. */
CSCD_API cscd_result cscd_element_on(cscd_view* v, cscd_element e, const char* type, cscd_event_fn fn, void* user, cscd_free_fn free_user);
CSCD_API cscd_result cscd_element_off(cscd_view* v, cscd_element e, const char* type); /* every listener of that type */
CSCD_API void cscd_event_prevent_default(const cscd_event* event);
CSCD_API void cscd_event_stop_propagation(const cscd_event* event);

/* ---- painters --------------------------------------------------------------------------- */

/* Draws e's content box: origin at its top-left, CSS pixels. Colors are 0xRRGGBBAA. */
typedef void (*cscd_paint_fn)(cscd_canvas* c, float width, float height, void* user);
CSCD_API cscd_result cscd_element_set_painter(cscd_view* v, cscd_element e, cscd_paint_fn fn, void* user, cscd_free_fn free_user);

CSCD_API void cscd_canvas_save(cscd_canvas* c);
CSCD_API void cscd_canvas_restore(cscd_canvas* c);
CSCD_API void cscd_canvas_translate(cscd_canvas* c, float x, float y);
CSCD_API void cscd_canvas_scale(cscd_canvas* c, float sx, float sy);
CSCD_API void cscd_canvas_rotate(cscd_canvas* c, float radians);
CSCD_API void cscd_canvas_clip_rect(cscd_canvas* c, float x, float y, float w, float h);
CSCD_API void cscd_canvas_set_global_alpha(cscd_canvas* c, float alpha);
CSCD_API void cscd_canvas_clear(cscd_canvas* c, uint32_t rgba);
CSCD_API void cscd_canvas_fill_rect(cscd_canvas* c, float x, float y, float w, float h, uint32_t rgba);
CSCD_API void cscd_canvas_fill_round_rect(cscd_canvas* c, float x, float y, float w, float h, float radius, uint32_t rgba);
CSCD_API void cscd_canvas_stroke_rect(cscd_canvas* c, float x, float y, float w, float h, uint32_t rgba, float width);
CSCD_API void cscd_canvas_draw_line(cscd_canvas* c, float x0, float y0, float x1, float y1, uint32_t rgba, float width);
CSCD_API void cscd_canvas_fill_circle(cscd_canvas* c, float x, float y, float r, uint32_t rgba);
/* SVG path data ("M 0 0 L 10 10 Z"). */
CSCD_API void cscd_canvas_fill_path(cscd_canvas* c, const char* svg_path, uint32_t rgba);
CSCD_API void cscd_canvas_stroke_path(cscd_canvas* c, const char* svg_path, uint32_t rgba, float width);
/* family: a CSS family list ("Inter, sans-serif"); y: the baseline. */
CSCD_API void cscd_canvas_draw_text(cscd_canvas* c, const char* utf8, float x, float y, const char* family, float size, int weight, uint32_t rgba);
CSCD_API float cscd_canvas_measure_text(cscd_canvas* c, const char* utf8, const char* family, float size, int weight);
/* Premultiplied BGRA pixels, rows packed, drawn into the rectangle (copied). */
CSCD_API void cscd_canvas_draw_image(cscd_canvas* c, const uint32_t* pixels, int width, int height, float x, float y, float w, float h);

/* ---- windows ---------------------------------------------------------------------------- */
/* (NULL / CSCD_ERR_UNAVAILABLE when cascade was built without the window) */

CSCD_API cscd_window* cscd_window_new(const char* title, int width, int height);
CSCD_API void cscd_window_delete(cscd_window* w);
CSCD_API cscd_view* cscd_window_view(cscd_window* w); /* the window's own: do not delete it */
CSCD_API int cscd_window_run(cscd_window* w);                  /* until it closes; returns the exit code */
CSCD_API int cscd_window_pump(cscd_window* w, int wait);       /* one round of events; 0 once it has closed */
CSCD_API cscd_result cscd_window_show(cscd_window* w);
CSCD_API cscd_result cscd_window_close(cscd_window* w);
CSCD_API cscd_result cscd_window_set_title(cscd_window* w, const char* title);
/* Runs fn on the window's thread soon; callable from any thread. */
CSCD_API cscd_result cscd_window_post(cscd_window* w, cscd_fn fn, void* user);
/* fn runs once a frame before painting (NULL: no more). */
CSCD_API cscd_result cscd_window_on_frame(cscd_window* w, cscd_fn fn, void* user, cscd_free_fn free_user);
CSCD_API char* cscd_window_backend(cscd_window* w);

#ifdef __cplusplus
}
#endif

#endif /* CASCADE_C_H */
