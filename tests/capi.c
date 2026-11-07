/* cascade-capi-tests: the C API, used from C. */
#include "cascade/cascadeC.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, cscd_last_error()); \
        }                                                                        \
    } while (0)

static int streq_free(char* s, const char* want) {
    int ok = s && strcmp(s, want) == 0;
    cscd_free(s);
    return ok;
}

static int clicks = 0, freed = 0, painted = 0, timer_runs = 0;

static void on_click(cscd_view* v, const cscd_event* ev, void* user) {
    char* tag = cscd_element_tag(v, ev->target);
    if (tag && strcmp(tag, "button") == 0) *(int*)user += 1;
    cscd_free(tag);
}

static void count_free(void* user) { (void)user; ++freed; }

static void paint(cscd_canvas* c, float w, float h, void* user) {
    (void)user;
    ++painted;
    cscd_canvas_fill_rect(c, 0, 0, w, h, 0x00FF00FFu);
    cscd_canvas_fill_path(c, "M 0 0 L 10 0 L 0 10 Z", 0xFF0000FFu);
    cscd_canvas_draw_text(c, "C", 2, h - 2, "sans-serif", 12, 400, 0x000000FFu);
}

static void tick(void* user) { *(int*)user += 1; }

static void stop_input(cscd_view* v, const cscd_event* ev, void* user) {
    (void)v; (void)user;
    cscd_event_prevent_default(ev);
}

static void fillRow(cscd_view* v, cscd_element row, size_t index, void* user) {
    char n[16];
    (void)user;
    snprintf(n, sizeof n, "%u", (unsigned)index);
    cscd_element_fill(v, row, "n", n);
}

static int menu_shows = 0, menu_hides = 0, menu_freed = 0;
static void count_menu_free(void* user) { ++*(int*)user; }
static unsigned menu_actions = 0;
static char clip[64];

static void on_menu(cscd_view* v, int show, const float rect[4], unsigned actions, void* user) {
    (void)v; (void)user;
    if (!show) { ++menu_hides; return; }
    ++menu_shows;
    menu_actions = rect[2] > 0 ? actions : 0;
}

static void clip_set(const char* utf8, void* user) {
    (void)user;
    snprintf(clip, sizeof clip, "%s", utf8);
}

static const char* clip_get(void* user) { (void)user; return clip; }

/* touch, the edit menu and the clipboard, as a binding drives them */
static void test_touch_selection(void) {
    cscd_view* v = cscd_view_new();
    CHECK(cscd_view_set_viewport(v, 400, 200, 1) == CSCD_OK);
    CHECK(cscd_view_load_html(v, "<style>body{margin:0} input{margin:0;width:300px;font-size:20px}</style><input id=f value=\"alpha beta\">", "") == CSCD_OK);
    cscd_view_update(v);
    int pw, ph;
    CHECK(cscd_view_paint(v, &pw, &ph) != NULL); /* (taps find what was painted) */
    cscd_element f = cscd_view_element_by_id(v, "f");
    float r[4];
    CHECK(cscd_element_rect(v, f, r) == CSCD_OK);
    const float x = r[0] + 12, y = r[1] + r[3] / 2;
    CHECK(cscd_view_on_edit_menu(v, on_menu, &menu_freed, count_menu_free) == CSCD_OK);
    CHECK(cscd_view_set_clipboard(v, clip_set, clip_get, NULL, NULL) == CSCD_OK);
    CHECK(cscd_view_edit_action(v, CSCD_EDIT_COPY) == CSCD_ERR_REFUSED); /* (no field has the focus) */
    /* a double tap selects the word, with handles and the menu */
    CHECK(cscd_view_touch_down(v, 1, x, y) == CSCD_OK && cscd_view_touch_up(v, 1, x, y) == CSCD_OK);
    CHECK(cscd_view_touch_down(v, 1, x, y) == CSCD_OK && cscd_view_touch_up(v, 1, x, y) == CSCD_OK);
    CHECK(cscd_view_selection_handles_shown(v) == 1 && menu_shows == 1);
    CHECK((menu_actions & CSCD_EDIT_BIT(CSCD_EDIT_COPY)) && (cscd_view_edit_actions(v) & CSCD_EDIT_BIT(CSCD_EDIT_CUT)));
    CHECK(cscd_view_edit_action(v, CSCD_EDIT_COPY) == CSCD_OK && strcmp(clip, "alpha") == 0); /* (the word, without its space) */
    CHECK(cscd_view_edit_action(v, CSCD_EDIT_SELECT_ALL) == CSCD_OK);
    CHECK(cscd_view_edit_action(v, CSCD_EDIT_CUT) == CSCD_OK && strcmp(clip, "alpha beta") == 0);
    CHECK(streq_free(cscd_element_value(v, f), ""));
    CHECK(cscd_view_edit_action(v, CSCD_EDIT_PASTE) == CSCD_OK && streq_free(cscd_element_value(v, f), "alpha beta"));
    CHECK(cscd_view_edit_action(v, (cscd_edit_action)9) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_hide_selection_handles(v) == CSCD_OK && cscd_view_selection_handles_shown(v) == 0);
    /* the clipboard from code; no picture without a hook for them */
    CHECK(cscd_view_copy_text(v, "from code") == CSCD_OK && streq_free(cscd_view_clipboard_text(v), "from code"));
    const uint32_t px[4] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu};
    CHECK(cscd_view_copy_image(v, px, 2, 2) == CSCD_ERR_REFUSED);
    CHECK(cscd_view_copy_image(v, NULL, 2, 2) == CSCD_ERR_ARGUMENT);
    CHECK(streq_free(cscd_view_clipboard_files(v), ""));
    CHECK(cscd_view_touch_cancel(v) == CSCD_OK && cscd_view_touch_down(NULL, 1, 0, 0) == CSCD_ERR_ARGUMENT);

    cscd_view_delete(v);
    CHECK(menu_freed == 1); /* (the menu's user data, with the view) */
    cscd_view* w = cscd_view_new();
    CHECK(cscd_view_copy_text(w, "x") == CSCD_ERR_REFUSED && streq_free(cscd_view_clipboard_text(w), ""));
    cscd_view_delete(w);
}

/* ---- the page: elements, as a binding walks and changes them ------------------------------------------ */

static void test_elements(void) {
    cscd_view* v = cscd_view_new();
    CHECK(cscd_view_load_html(v, "<style>body{margin:0}</style><main id=m class=a><p id=p class='x y' title=t>text <b>bold</b></p>"
                                  "<input id=c type=checkbox><div id=box style='width:10px;height:10px'></div></main>", "") == CSCD_OK);
    CHECK(cscd_view_set_viewport(v, 200, 200, 1) == CSCD_OK);
    cscd_view_update(v);
    const cscd_element body = cscd_view_body(v), m = cscd_view_element_by_id(v, "m"), p = cscd_view_element_by_id(v, "p");
    CHECK(streq_free(cscd_element_tag(v, body), "body"));
    CHECK(cscd_element_first_child(v, m) == p);
    cscd_element b = cscd_element_query(v, m, "b");
    CHECK(streq_free(cscd_element_tag(v, b), "b"));
    CHECK(cscd_element_query(v, m, "h1") == 0);
    CHECK(cscd_element_first_child(v, b) == 0); /* (its text is not an element) */
    CHECK(cscd_element_parent(v, b) == p);

    CHECK(streq_free(cscd_element_attribute(v, p, "title"), "t"));
    CHECK(cscd_element_remove_attribute(v, p, "title") == CSCD_OK && cscd_element_attribute(v, p, "title") == NULL);
    CHECK(cscd_element_remove_class(v, p, "x") == CSCD_OK && !cscd_element_has_class(v, p, "x") && cscd_element_has_class(v, p, "y"));
    CHECK(cscd_element_toggle_class(v, p, "z", 1) == CSCD_OK && cscd_element_has_class(v, p, "z"));
    CHECK(cscd_element_toggle_class(v, p, "z", 0) == CSCD_OK && !cscd_element_has_class(v, p, "z"));

    char* html = cscd_element_html(v, p);
    CHECK(html && strstr(html, "<b>bold</b>"));
    cscd_free(html);

    const cscd_element box = cscd_view_element_by_id(v, "box");
    CHECK(cscd_element_set_style(v, box, "width", "33px") == CSCD_OK);
    cscd_view_update(v);
    float r[4];
    CHECK(cscd_element_rect(v, box, r) == CSCD_OK && r[2] == 33);

    const cscd_element c = cscd_view_element_by_id(v, "c");
    CHECK(cscd_element_checked(v, c) == 0);
    CHECK(cscd_element_set_checked(v, c, 1) == CSCD_OK && cscd_element_checked(v, c) == 1);
    CHECK(cscd_element_click(v, c) == CSCD_OK && cscd_element_checked(v, c) == 0);

    const cscd_element made = cscd_element_append_html(v, m, "<section id=s><i>in</i></section><u>next</u>");
    CHECK(streq_free(cscd_element_tag(v, made), "section"));
    CHECK(cscd_view_element_by_id(v, "s") == made);
    CHECK(cscd_view_query_all(v, "u", NULL, 0) == 1);

    /* a style sheet from the program, and the theme it sees */
    CHECK(cscd_view_add_style_sheet(v, "#box{min-height:21px} @media (prefers-color-scheme:dark){#box{min-height:42px}}") == CSCD_OK);
    cscd_view_update(v);
    CHECK(cscd_element_rect(v, box, r) == CSCD_OK && r[3] == 21);
    CHECK(cscd_view_set_dark_mode(v, 1) == CSCD_OK);
    cscd_view_update(v);
    CHECK(cscd_element_rect(v, box, r) == CSCD_OK && r[3] == 42);
    CHECK(cscd_view_set_dark_mode(v, 0) == CSCD_OK);
    cscd_view_delete(v);
}

/* What the header promises: a handle that is gone, or no view, is an error from every function. */
static void test_bad_arguments(void) {
    cscd_view* v = cscd_view_new();
    CHECK(cscd_view_load_html(v, "<div id=a></div><div id=b></div>", "") == CSCD_OK);
    cscd_view_update(v);
    const cscd_element e = cscd_view_element_by_id(v, "a"), keep = cscd_view_element_by_id(v, "b");
    CHECK(cscd_element_remove(v, e) == CSCD_OK);
    float r[4];
    int fired = 0;
    CHECK(cscd_element_add_class(v, e, "x") == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_remove_class(v, e, "x") == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_toggle_class(v, e, "x", 1) == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_set_attribute(v, e, "a", "b") == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_remove_attribute(v, e, "a") == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_set_style(v, e, "width", "1px") == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_set_html(v, e, "x") == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_set_value(v, e, "x") == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_set_checked(v, e, 1) == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_rect(v, e, r) == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_focus(v, e) == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_click(v, e) == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_scroll_into_view(v, e) == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_invalidate(v, e) == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_on(v, e, "click", on_click, &fired, NULL) == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_off(v, e, "click") == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_set_painter(v, e, paint, NULL, NULL) == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_fill(v, e, "k", "v") == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_view_inspect(v, e) == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_view_list_refresh(v, e) == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_view_list_set_count(v, e, 1) == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_tag(v, e) == NULL && cscd_element_html(v, e) == NULL && cscd_element_value(v, e) == NULL);
    CHECK(cscd_element_attribute(v, e, "a") == NULL);
    CHECK(cscd_element_parent(v, e) == 0 && cscd_element_first_child(v, e) == 0 && cscd_element_next_sibling(v, e) == 0);
    CHECK(cscd_element_query(v, e, "*") == 0 && cscd_element_append(v, e, "p") == 0 && cscd_element_append_html(v, e, "<p>") == 0);
    CHECK(cscd_element_append_copy(v, keep, e) == 0 && cscd_element_append_copy(v, e, keep) == 0);
    CHECK(cscd_element_has_class(v, e, "x") == 0 && cscd_element_checked(v, e) == 0);
    /* a number that never was a handle, and the handle of no element */
    CHECK(cscd_element_valid(v, 0) == 0 && cscd_element_valid(v, 0x7FFFFFFFFFFFull) == 0);
    CHECK(cscd_element_add_class(v, 0x7FFFFFFFFFFFull, "x") == CSCD_ERR_INVALID_HANDLE);
    CHECK(strlen(cscd_last_error()) > 0);
    /* NULL for what is needed */
    CHECK(cscd_element_add_class(v, keep, NULL) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_element_set_attribute(v, keep, NULL, "v") == CSCD_ERR_ARGUMENT);
    CHECK(cscd_element_set_style(v, keep, NULL, "v") == CSCD_ERR_ARGUMENT);
    CHECK(cscd_element_on(v, keep, "click", NULL, NULL, NULL) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_element_on(v, keep, NULL, on_click, &fired, NULL) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_element_rect(v, keep, NULL) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_load_html(v, NULL, "") == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_load_file(v, NULL) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_add_style_sheet(v, NULL) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_text_input(v, NULL) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_paint_canvas(v, NULL) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_set_timeout(v, 1, NULL, NULL, NULL) == 0 && cscd_view_set_interval(v, 1, NULL, NULL, NULL) == 0);
    cscd_view_delete(v);

    /* no view */
    CHECK(cscd_view_load_html(NULL, "x", "") == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_add_style_sheet(NULL, "x") == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_set_viewport(NULL, 1, 1, 1) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_set_dark_mode(NULL, 1) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_mouse_move(NULL, 0, 0, 0, 0) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_mouse_down(NULL, 0, 0, 0, 0, 1) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_mouse_up(NULL, 0, 0, 0, 0) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_mouse_leave(NULL) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_wheel(NULL, 0, 0, 0, 0, 0) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_key_down(NULL, CSCD_KEY_A, 0, "a") == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_key_up(NULL, CSCD_KEY_A, 0, "a") == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_text_input(NULL, "a") == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_touch_move(NULL, 1, 0, 0) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_touch_up(NULL, 1, 0, 0) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_clear_timer(NULL, 1) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_inspect(NULL, 0) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_on_magnifier(NULL, NULL, NULL, NULL) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_paint(NULL, NULL, NULL) == NULL);
    CHECK(cscd_view_title(NULL) == NULL);
    CHECK(cscd_view_update(NULL) == 0 && cscd_view_needs_paint(NULL) == 0);
    CHECK(cscd_view_next_timer_ms(NULL) == -1);
    CHECK(cscd_view_content_height(NULL) == 0 && cscd_view_cursor(NULL) == 0);
    CHECK(cscd_view_body(NULL) == 0 && cscd_view_element_at(NULL, 0, 0) == 0);
    cscd_view_delete(NULL);
    cscd_free(NULL);
}

/* ---- input, as a window hands it to the view ----------------------------------------------------------- */

static int key_downs = 0, key_ups = 0, wheels = 0, outer_clicks = 0, inner_clicks = 0;
static char last_key[16];
static float last_delta = 0, last_offset = -1;
static unsigned last_mods = 0;
static cscd_element last_target = 0, last_current = 0;

static void on_key(cscd_view* v, const cscd_event* ev, void* user) {
    (void)v; (void)user;
    if (strcmp(ev->type, "keydown") == 0) ++key_downs; else ++key_ups;
    snprintf(last_key, sizeof last_key, "%s", ev->key ? ev->key : "");
    last_mods = ev->mods;
}

static void on_wheel(cscd_view* v, const cscd_event* ev, void* user) {
    (void)v; (void)user;
    ++wheels;
    last_delta = ev->delta_y;
}

static void on_outer(cscd_view* v, const cscd_event* ev, void* user) {
    (void)v; (void)user;
    ++outer_clicks;
    last_target = ev->target;
    last_current = ev->current_target;
    last_offset = ev->offset_x;
}

static void on_inner(cscd_view* v, const cscd_event* ev, void* user) {
    (void)v;
    ++inner_clicks;
    if (*(int*)user) cscd_event_stop_propagation(ev);
}

static void test_input(void) {
    cscd_view* v = cscd_view_new();
    CHECK(cscd_view_load_html(v, "<style>body{margin:0} #out{position:absolute;left:10px;top:10px;width:100px;height:40px;cursor:pointer}"
                                  "#out:hover{width:120px} #in{width:50px;height:20px} #f{position:absolute;left:0;top:60px;width:150px}"
                                  "#tall{position:absolute;top:100px;height:2000px;width:10px} #far{position:absolute;top:1500px;height:10px;width:10px}</style>"
                                  "<div id=out><div id=in></div></div><input id=f><div id=tall></div><div id=far></div>", "") == CSCD_OK);
    CHECK(cscd_view_set_viewport(v, 300, 200, 1) == CSCD_OK);
    cscd_view_update(v);
    int w, h;
    CHECK(cscd_view_paint(v, &w, &h) != NULL); /* (hit areas come from painting) */
    const cscd_element out = cscd_view_element_by_id(v, "out"), in = cscd_view_element_by_id(v, "in"), f = cscd_view_element_by_id(v, "f");
    CHECK(cscd_view_element_at(v, 20, 20) == in);
    CHECK(cscd_view_element_at(v, 100, 40) == out);
    CHECK(cscd_view_content_height(v) >= 2100);

    /* hover and the cursor, and leaving */
    const int plain = cscd_view_cursor(v);
    CHECK(cscd_view_mouse_move(v, 20, 20, 0, 0) == CSCD_OK);
    cscd_view_update(v);
    float r[4];
    CHECK(cscd_element_rect(v, out, r) == CSCD_OK && r[2] == 120);
    CHECK(cscd_view_cursor(v) != plain);
    CHECK(cscd_view_mouse_leave(v) == CSCD_OK);
    cscd_view_update(v);
    CHECK(cscd_element_rect(v, out, r) == CSCD_OK && r[2] == 100);
    cscd_view_paint(v, NULL, NULL);

    /* a click bubbles, tells what it hit, and can be stopped */
    int stop = 0;
    CHECK(cscd_element_on(v, out, "click", on_outer, NULL, NULL) == CSCD_OK);
    CHECK(cscd_element_on(v, in, "click", on_inner, &stop, NULL) == CSCD_OK);
    cscd_view_mouse_down(v, 20, 20, 0, 0, 1);
    cscd_view_mouse_up(v, 20, 20, 0, 0);
    CHECK(inner_clicks == 1 && outer_clicks == 1);
    CHECK(last_target == in && last_current == out);
    CHECK(last_offset > 9 && last_offset < 11); /* (from #out's border box) */
    stop = 1;
    cscd_view_mouse_down(v, 20, 20, 0, 0, 1);
    cscd_view_mouse_up(v, 20, 20, 0, 0);
    CHECK(inner_clicks == 2 && outer_clicks == 1);

    /* keys and text reach the focused field */
    CHECK(cscd_element_focus(v, f) == CSCD_OK);
    CHECK(cscd_element_on(v, f, "keydown", on_key, NULL, NULL) == CSCD_OK);
    CHECK(cscd_element_on(v, f, "keyup", on_key, NULL, NULL) == CSCD_OK);
    CHECK(cscd_view_text_input(v, "ab\xC3\xA9") == CSCD_OK);
    CHECK(streq_free(cscd_element_value(v, f), "ab\xC3\xA9"));
    CHECK(cscd_view_key_down(v, CSCD_KEY_BACKSPACE, CSCD_MOD_SHIFT, "Backspace") == CSCD_OK);
    CHECK(cscd_view_key_up(v, CSCD_KEY_BACKSPACE, 0, "Backspace") == CSCD_OK);
    CHECK(key_downs == 1 && key_ups == 1 && strcmp(last_key, "Backspace") == 0);
    CHECK(streq_free(cscd_element_value(v, f), "ab")); /* (the whole character, not its last byte) */
    CHECK(cscd_view_key_down(v, CSCD_KEY_A, CSCD_MOD_CTRL, "a") == CSCD_OK);
    CHECK(last_mods == CSCD_MOD_CTRL);
    /* a listener that prevents the default keeps the key from the field */
    CHECK(cscd_element_set_value(v, f, "ab") == CSCD_OK);
    CHECK(cscd_element_on(v, f, "keydown", stop_input, NULL, NULL) == CSCD_OK);
    CHECK(cscd_view_key_down(v, CSCD_KEY_BACKSPACE, 0, "Backspace") == CSCD_OK);
    CHECK(streq_free(cscd_element_value(v, f), "ab"));
    CHECK(cscd_element_off(v, f, "keydown") == CSCD_OK);

    /* the wheel and the finger scroll the page; scroll_into_view brings an element to it */
    const cscd_element far = cscd_view_element_by_id(v, "far");
    CHECK(cscd_element_on(v, cscd_view_body(v), "wheel", on_wheel, NULL, NULL) == CSCD_OK);
    CHECK(cscd_element_rect(v, far, r) == CSCD_OK);
    const float top = r[1];
    CHECK(cscd_view_set_feature(v, CSCD_FEATURE_SMOOTH_SCROLL, 0) == CSCD_OK);
    CHECK(cscd_view_wheel(v, 200, 150, 0, 120, 0) == CSCD_OK);
    cscd_view_update(v);
    CHECK(wheels == 1 && last_delta != 0);
    CHECK(cscd_element_rect(v, far, r) == CSCD_OK && r[1] < top);
    CHECK(cscd_element_scroll_into_view(v, far) == CSCD_OK);
    cscd_view_update(v);
    CHECK(cscd_element_rect(v, far, r) == CSCD_OK && r[1] >= 0 && r[1] + r[3] <= 200);
    const float before = r[1];
    cscd_view_paint(v, NULL, NULL);
    CHECK(cscd_view_touch_down(v, 1, 200, 50) == CSCD_OK);
    CHECK(cscd_view_touch_move(v, 1, 200, 90) == CSCD_OK);
    CHECK(cscd_view_touch_move(v, 1, 200, 130) == CSCD_OK);
    CHECK(cscd_view_touch_up(v, 1, 200, 130) == CSCD_OK);
    cscd_view_update(v);
    CHECK(cscd_element_rect(v, far, r) == CSCD_OK && r[1] > before); /* the finger drew the page down */
    /* a page clipped at its root scrolls to nothing placed past it, and fixed boxes never count */
    CHECK(cscd_view_load_html(v, "<style>html{overflow:hidden} body{margin:0}</style><div style='position:absolute;top:1000px;height:10px;width:10px'></div>", "") == CSCD_OK);
    cscd_view_update(v);
    CHECK(cscd_view_content_height(v) < 1000);
    CHECK(cscd_view_load_html(v, "<style>body{margin:0}</style><div style='position:fixed;top:1000px;height:10px;width:10px'></div>", "") == CSCD_OK);
    cscd_view_update(v);
    CHECK(cscd_view_content_height(v) < 1000);
    cscd_view_delete(v);
}

/* ---- the frame: timers, repainting, what threads paint ------------------------------------------------- */

static int repaints = 0, interval_runs = 0, timer_freed = 0, row_fills = 0, magnifier_freed = 0;

static void paint_count(cscd_canvas* c, float w, float h, void* user) {
    (void)user;
    ++repaints;
    cscd_canvas_fill_rect(c, 0, 0, w, h, 0x336699FFu);
}

static void count_timer_free(void* user) { (void)user; ++timer_freed; }
static void count_row(cscd_view* v, cscd_element row, size_t index, void* user) {
    (void)v; (void)row; (void)index; (void)user;
    ++row_fills;
}
static void count_magnifier_free(void* user) { (void)user; ++magnifier_freed; }
static void on_magnifier(cscd_view* v, int show, float x, float y, void* user) { (void)v; (void)show; (void)x; (void)y; (void)user; }

static void test_frame(void) {
    cscd_view* v = cscd_view_new();
    CHECK(cscd_view_load_html(v, "<style>body{margin:0} #p{width:40px;height:40px}</style><template id=t><div class=row></div></template>"
                                  "<div id=p></div><div id=l style='height:50px;overflow:auto'></div>", "") == CSCD_OK);
    CHECK(cscd_view_set_viewport(v, 100, 100, 1) == CSCD_OK);
    cscd_view_update(v);
    const cscd_element p = cscd_view_element_by_id(v, "p");
    CHECK(cscd_element_set_painter(v, p, paint_count, NULL, NULL) == CSCD_OK);
    CHECK(cscd_view_needs_paint(v) == 1);
    cscd_view_paint(v, NULL, NULL);
    CHECK(repaints == 1 && cscd_view_needs_paint(v) == 0);
    CHECK(cscd_element_invalidate(v, p) == CSCD_OK && cscd_view_needs_paint(v) == 1);
    cscd_view_paint(v, NULL, NULL);
    CHECK(repaints == 2);

    /* an interval runs until it is cleared, and its user data goes with it */
    CHECK(cscd_view_next_timer_ms(v) == -1);
    const int id = cscd_view_set_interval(v, 1, tick, &interval_runs, count_timer_free);
    CHECK(id != 0 && cscd_view_next_timer_ms(v) >= 0);
    for (long i = 0; i < 2000000000L && interval_runs < 2; ++i) cscd_view_update(v); /* (a millisecond apart, at least) */
    CHECK(interval_runs >= 2);
    const int runs = interval_runs;
    CHECK(cscd_view_clear_timer(v, id) == CSCD_OK && timer_freed == 1);
    cscd_view_update(v);
    CHECK(interval_runs == runs && cscd_view_next_timer_ms(v) == -1);
    const int later = cscd_view_set_timeout(v, 100000, tick, &interval_runs, count_timer_free);
    CHECK(later != 0 && cscd_view_clear_timer(v, later) == CSCD_OK && timer_freed == 2);

    /* a list fills its rows again when asked */
    const cscd_element l = cscd_view_element_by_id(v, "l");
    CHECK(cscd_view_bind_list(v, l, cscd_view_query(v, "#t"), 100, 10, count_row, NULL, NULL) == CSCD_OK);
    cscd_view_update(v);
    const int filled = row_fills;
    CHECK(filled > 0);
    CHECK(cscd_view_list_refresh(v, l) == CSCD_OK);
    cscd_view_update(v);
    CHECK(row_fills > filled);

    /* a callback that is replaced, or goes with the view, has its user data freed */
    CHECK(cscd_view_on_magnifier(v, on_magnifier, NULL, count_magnifier_free) == CSCD_OK);
    CHECK(cscd_view_on_magnifier(v, on_magnifier, NULL, count_magnifier_free) == CSCD_OK && magnifier_freed == 1);

    /* the inspector opens on an element and closes */
    CHECK(cscd_view_inspect(v, p) == CSCD_OK);
    cscd_view_update(v);
    CHECK(cscd_view_paint(v, NULL, NULL) != NULL);
    CHECK(cscd_view_inspect(v, 0) == CSCD_OK);
    cscd_view_delete(v);
    CHECK(magnifier_freed == 2);
}

static void test_threads(void) {
    /* large enough to be painted by bands */
    static const char page[] = "<style>body{margin:0;background:linear-gradient(#fff,#39c)} div{margin:20px;height:300px;border-radius:30px;"
                               "background:#e83;box-shadow:0 4px 20px #0008;mask-image:linear-gradient(#000,transparent)}</style><div></div><div></div>";
    uint32_t* first = NULL;
    size_t size = 0;
    for (int threads = 1; threads <= 4; threads += 3) {
        cscd_set_paint_threads(threads);
        cscd_view* v = cscd_view_new();
        CHECK(cscd_view_load_html(v, page, "") == CSCD_OK);
        CHECK(cscd_view_set_viewport(v, 800, 700, 1) == CSCD_OK);
        cscd_view_update(v);
        int w = 0, h = 0;
        const uint32_t* px = cscd_view_paint(v, &w, &h);
        CHECK(px != NULL && w == 800 && h == 700);
        if (px && !first) {
            size = (size_t)w * (size_t)h * 4;
            first = (uint32_t*)malloc(size);
            memcpy(first, px, size);
        } else if (px && first) {
            CHECK(memcmp(first, px, size) == 0);
        }
        cscd_view_delete(v);
    }
    cscd_set_paint_threads(0);
    free(first);
    /* what painting keeps */
    cscd_paint_cache_stats stats;
    cscd_paint_cache_get_stats(&stats);
    CHECK(stats.shadow_misses > 0);
    cscd_paint_cache_set_limits(64u << 20, 512, 4096);
    cscd_paint_cache_clear();
    cscd_paint_cache_get_stats(&stats);
    CHECK(stats.shadow_count == 0 && stats.glyph_count == 0);
    cscd_paint_cache_set_limits(128u << 20, 1024, 8192);
}

/* ---- painters: every call of the canvas, each where the page can see it --------------------------------- */

static float measured = 0, measured_twice = 0;
static cscd_view* painted_view = NULL;
static const uint32_t image_pixels[4] = {0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u}; /* opaque red, as BGRA */

static void paint_all(cscd_canvas* c, float w, float h, void* user) {
    (void)w; (void)h; (void)user;
    cscd_canvas_clear(c, 0xFFFFFFFFu);
    /* save, translate, restore: the second square is where it would have been */
    cscd_canvas_save(c);
    cscd_canvas_translate(c, 10, 10);
    cscd_canvas_fill_rect(c, 0, 0, 20, 20, 0xFF0000FFu);
    cscd_canvas_restore(c);
    cscd_canvas_fill_rect(c, 40, 10, 20, 20, 0x00FF00FFu);
    /* scale */
    cscd_canvas_save(c);
    cscd_canvas_scale(c, 2, 2);
    cscd_canvas_fill_rect(c, 40, 5, 10, 10, 0x0000FFFFu);
    cscd_canvas_restore(c);
    /* rotate: a quarter turn about the origin puts x on y */
    cscd_canvas_save(c);
    cscd_canvas_translate(c, 150, 10);
    cscd_canvas_rotate(c, 1.57079633f);
    cscd_canvas_fill_rect(c, 0, -20, 20, 20, 0xFF00FFFFu);
    cscd_canvas_restore(c);
    /* clip: only the half inside */
    cscd_canvas_save(c);
    cscd_canvas_clip_rect(c, 10, 50, 10, 20);
    cscd_canvas_fill_rect(c, 10, 50, 20, 20, 0x000000FFu);
    cscd_canvas_restore(c);
    /* global alpha */
    cscd_canvas_save(c);
    cscd_canvas_set_global_alpha(c, 0.5f);
    cscd_canvas_fill_rect(c, 40, 50, 20, 20, 0x000000FFu);
    cscd_canvas_restore(c);
    cscd_canvas_fill_round_rect(c, 70, 50, 40, 20, 10, 0x008800FFu);
    cscd_canvas_stroke_rect(c, 120, 50, 30, 20, 0x880000FFu, 2);
    cscd_canvas_draw_line(c, 10, 85, 60, 85, 0x000088FFu, 4);
    cscd_canvas_fill_circle(c, 90, 90, 10, 0x884400FFu);
    cscd_canvas_stroke_path(c, "M 120 90 L 150 90", 0x008888FFu, 4);
    cscd_canvas_draw_image(c, image_pixels, 2, 2, 10, 110, 20, 20);
    cscd_canvas_fill_path(c, "M 40 110 L 60 110 L 60 130 L 40 130 Z", 0x444444FFu);
    measured = cscd_canvas_measure_text(c, "wide", "sans-serif", 16, 400);
    measured_twice = cscd_canvas_measure_text(c, "widewide", "sans-serif", 16, 400);
    CHECK(cscd_canvas_measure_text(NULL, "x", "sans-serif", 16, 400) == 0);
    /* (a view paints its whole page, background and all: this part of it) */
    cscd_canvas_save(c);
    cscd_canvas_clip_rect(c, 100, 150, 30, 30);
    if (painted_view) CHECK(cscd_view_paint_canvas(painted_view, c) == CSCD_OK);
    cscd_canvas_restore(c);
}

static uint32_t at(const uint32_t* px, int width, float x, float y) { return px[(int)y * width + (int)x]; }

static void test_canvas(void) {
    cscd_view* v = cscd_view_new();
    CHECK(cscd_view_load_html(v, "<style>body{margin:0} #c{width:200px;height:200px}</style><div id=c></div>", "") == CSCD_OK);
    CHECK(cscd_view_set_viewport(v, 200, 200, 1) == CSCD_OK);
    cscd_view_update(v);
    /* a second view, drawn into the first */
    painted_view = cscd_view_new();
    CHECK(cscd_view_load_html(painted_view, "<style>body{margin:0;background:transparent} div{position:absolute;left:100px;top:150px;width:30px;height:30px;background:#123456}</style><div></div>", "") == CSCD_OK);
    CHECK(cscd_view_set_viewport(painted_view, 200, 200, 1) == CSCD_OK);
    cscd_view_update(painted_view);
    CHECK(cscd_element_set_painter(v, cscd_view_element_by_id(v, "c"), paint_all, NULL, NULL) == CSCD_OK);
    int w = 0, h = 0;
    const uint32_t* px = cscd_view_paint(v, &w, &h);
    CHECK(px != NULL && w == 200 && h == 200);
    if (px) {
        CHECK(at(px, w, 20, 20) == 0xFFFF0000u);   /* translated */
        CHECK(at(px, w, 50, 20) == 0xFF00FF00u);   /* restored */
        CHECK(at(px, w, 99, 20) == 0xFF0000FFu);   /* scaled: 80..99 by 10..29 */
        CHECK(at(px, w, 90, 20) == 0xFF0000FFu);
        CHECK(at(px, w, 160, 20) == 0xFFFF00FFu);  /* turned */
        CHECK(at(px, w, 15, 60) == 0xFF000000u);   /* inside the clip */
        CHECK(at(px, w, 25, 60) == 0xFFFFFFFFu);   /* outside */
        const uint32_t half = at(px, w, 50, 60) & 255u;
        CHECK(half > 120 && half < 135);           /* half opaque black over white */
        CHECK(at(px, w, 90, 60) == 0xFF008800u);   /* the round rectangle */
        CHECK(at(px, w, 71, 51) == 0xFFFFFFFFu);   /* its corner */
        CHECK(at(px, w, 120, 60) == 0xFF880000u);  /* the stroke */
        CHECK(at(px, w, 135, 60) == 0xFFFFFFFFu);  /* inside it, nothing */
        CHECK(at(px, w, 30, 85) == 0xFF000088u);   /* the line */
        CHECK(at(px, w, 90, 90) == 0xFF884400u);   /* the circle */
        CHECK(at(px, w, 135, 90) == 0xFF008888u);  /* the stroked path */
        CHECK(at(px, w, 20, 120) == 0xFFFF0000u);  /* the picture */
        CHECK(at(px, w, 50, 120) == 0xFF444444u);  /* the filled path */
        CHECK(at(px, w, 115, 165) == 0xFF123456u); /* the other view */
    }
    CHECK(measured > 0 && measured_twice > measured);
    cscd_view_delete(painted_view);
    painted_view = NULL;
    cscd_view_delete(v);
    /* no canvas: nothing happens */
    cscd_canvas_save(NULL);
    cscd_canvas_restore(NULL);
    cscd_canvas_translate(NULL, 1, 1);
    cscd_canvas_fill_rect(NULL, 0, 0, 1, 1, 0);
    cscd_canvas_fill_path(NULL, "M 0 0", 0);
    cscd_canvas_draw_text(NULL, "x", 0, 0, "sans-serif", 12, 400, 0);
    cscd_canvas_draw_image(NULL, image_pixels, 2, 2, 0, 0, 1, 1);
}

/* ---- windows: only with a screen, so only when asked for (CASCADE_TEST_WINDOW) -------------------------- */

static int posted = 0, frames = 0, frame_freed = 0;
static void on_post(void* user) { *(int*)user += 1; }
static void on_frame(void* user) { *(int*)user += 1; }
static void count_frame_free(void* user) { (void)user; ++frame_freed; }

static void test_window_without_one(void) {
    CHECK(cscd_window_view(NULL) == NULL && cscd_window_backend(NULL) == NULL);
    CHECK(cscd_window_pump(NULL, 0) == 0 && cscd_window_run(NULL) == 1);
    CHECK(cscd_window_show(NULL) != CSCD_OK && cscd_window_close(NULL) != CSCD_OK);
    CHECK(cscd_window_set_title(NULL, "x") != CSCD_OK && cscd_window_post(NULL, on_post, NULL) != CSCD_OK);
    CHECK(cscd_window_on_frame(NULL, on_frame, NULL, NULL) != CSCD_OK);
    cscd_window_delete(NULL);
}

static void test_window(void) {
    cscd_window* w = cscd_window_new("handle API", 320, 200);
    CHECK(w != NULL);
    if (!w) return;
    cscd_view* v = cscd_window_view(w);
    CHECK(v != NULL);
    CHECK(cscd_view_load_html(v, "<p id=p>in a window</p>", "") == CSCD_OK);
    CHECK(cscd_window_set_title(w, "the title") == CSCD_OK);
    char* backend = cscd_window_backend(w);
    CHECK(backend && backend[0]);
    cscd_free(backend);
    CHECK(cscd_window_show(w) == CSCD_OK);
    CHECK(cscd_window_on_frame(w, on_frame, &frames, count_frame_free) == CSCD_OK);
    CHECK(cscd_window_post(w, on_post, &posted) == CSCD_OK);
    CHECK(cscd_window_post(w, NULL, NULL) == CSCD_ERR_ARGUMENT);
    for (int i = 0; i < 200 && (posted == 0 || frames == 0); ++i) cscd_window_pump(w, 0);
    CHECK(posted == 1);
    CHECK(frames > 0);
    CHECK(cscd_window_close(w) == CSCD_OK);
    for (int i = 0; i < 200 && cscd_window_pump(w, 0); ++i) {}
    CHECK(cscd_window_pump(w, 0) == 0); /* closed */
    cscd_window_delete(w);
    CHECK(frame_freed == 1);
}

int main(void) {
    CHECK(cscd_version() >= (1u << 16));
    CHECK(strcmp(cscd_last_error(), "") == 0);
    test_touch_selection();
    test_elements();
    test_bad_arguments();
    test_input();
    test_frame();
    test_threads();
    test_canvas();
    test_window_without_one();
    if (getenv("CASCADE_TEST_WINDOW")) test_window();

    cscd_view* v = cscd_view_new();
    CHECK(v != NULL);
    CHECK(cscd_view_load_html(v,
                            "<title>From C</title><style>body{margin:0} #box{width:40px;height:30px}</style>"
                            "<main><button id=go class='a b'>Go</button><div id=box></div>"
                            "<ul id=list><li>one</li><li>two</li></ul><input id=name value=x></main>",
                            "") == CSCD_OK);
    CHECK(cscd_view_set_viewport(v, 200, 120, 1) == CSCD_OK);
    CHECK(cscd_view_set_viewport(v, 0, 120, 1) == CSCD_ERR_ARGUMENT);
    cscd_view_update(v);
    CHECK(streq_free(cscd_view_title(v), "From C"));
    cscd_frame_stats stats;
    CHECK(cscd_view_frame_stats(v, &stats) == CSCD_OK);
    CHECK(stats.styles_computed > 0 && stats.layout_ms >= 0);
    CHECK(cscd_view_frame_stats(v, NULL) == CSCD_ERR_ARGUMENT);

    /* queries and attributes */
    cscd_element go = cscd_view_query(v, "#go");
    CHECK(go != 0);
    CHECK(cscd_element_valid(v, go));
    CHECK(cscd_view_query(v, ".nothing") == 0);
    CHECK(go == cscd_view_element_by_id(v, "go")); /* the same element, the same handle */
    CHECK(streq_free(cscd_element_tag(v, go), "button"));
    CHECK(cscd_element_has_class(v, go, "b"));
    CHECK(cscd_element_add_class(v, go, "c") == CSCD_OK && cscd_element_has_class(v, go, "c"));
    CHECK(cscd_element_set_attribute(v, go, "data-x", "7") == CSCD_OK);
    CHECK(streq_free(cscd_element_attribute(v, go, "data-x"), "7"));
    CHECK(cscd_element_attribute(v, go, "data-none") == NULL);
    CHECK(streq_free(cscd_element_text(v, go), "Go"));

    cscd_element items[4] = {0};
    CHECK(cscd_view_query_all(v, "li", items, 4) == 2);
    CHECK(streq_free(cscd_element_text(v, items[1]), "two"));
    CHECK(cscd_element_next_sibling(v, items[0]) == items[1]);
    CHECK(cscd_element_parent(v, items[0]) == cscd_view_element_by_id(v, "list"));

    /* layout */
    cscd_view_update(v);
    float r[4] = {0};
    CHECK(cscd_element_rect(v, cscd_view_element_by_id(v, "box"), r) == CSCD_OK);
    CHECK(r[2] == 40 && r[3] == 30);

    /* events, with the user data freed once the listener goes */
    CHECK(cscd_element_on(v, go, "click", on_click, &clicks, count_free) == CSCD_OK);
    CHECK(cscd_element_click(v, go) == CSCD_OK);
    CHECK(clicks == 1);
    float gr[4];
    cscd_element_rect(v, go, gr);
    cscd_view_paint(v, NULL, NULL); /* (hit areas come from painting) */
    cscd_view_mouse_down(v, gr[0] + 2, gr[1] + 2, 0, 0, 1);
    cscd_view_mouse_up(v, gr[0] + 2, gr[1] + 2, 0, 0);
    CHECK(clicks == 2);
    CHECK(cscd_element_off(v, go, "click") == CSCD_OK);
    CHECK(freed == 1);
    cscd_element_click(v, go);
    CHECK(clicks == 2);

    /* form values and default actions */
    cscd_element name = cscd_view_element_by_id(v, "name");
    CHECK(streq_free(cscd_element_value(v, name), "x"));
    CHECK(cscd_element_set_value(v, name, "hello") == CSCD_OK);
    CHECK(streq_free(cscd_element_value(v, name), "hello"));
    CHECK(cscd_element_on(v, name, "keydown", stop_input, NULL, NULL) == CSCD_OK);

    /* painters */
    cscd_element box = cscd_view_element_by_id(v, "box");
    CHECK(cscd_element_set_painter(v, box, paint, NULL, count_free) == CSCD_OK);
    int w = 0, h = 0;
    const uint32_t* px = cscd_view_paint(v, &w, &h);
    CHECK(px != NULL && w == 200 && h == 120);
    CHECK(painted >= 1);
    cscd_element_rect(v, box, r);
    if (px) {
        uint32_t mid = px[(int)(r[1] + 20) * w + (int)(r[0] + 30)];
        CHECK(mid == 0xFF00FF00u); /* premultiplied BGRA: opaque green */
    }

    /* handles stop working when their element goes */
    cscd_element list = cscd_view_element_by_id(v, "list");
    CHECK(cscd_element_set_html(v, list, "<li>three</li>") == CSCD_OK);
    CHECK(!cscd_element_valid(v, items[0]));
    CHECK(cscd_element_set_text(v, items[0], "x") == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_text(v, items[0]) == NULL);
    cscd_element added = cscd_element_append(v, list, "li");
    CHECK(added != 0 && added != items[0] && added != items[1]); /* (a reused slot, a new generation) */
    CHECK(cscd_element_set_text(v, added, "four") == CSCD_OK);
    CHECK(cscd_view_query_all(v, "li", NULL, 0) == 2);
    CHECK(cscd_element_remove(v, added) == CSCD_OK);
    CHECK(!cscd_element_valid(v, added));
    CHECK(cscd_element_remove(v, added) == CSCD_ERR_INVALID_HANDLE);
    CHECK(cscd_element_valid(v, go));

    /* timers run inside update */
    CHECK(cscd_view_set_timeout(v, 0, tick, &timer_runs, NULL) != 0);
    cscd_view_update(v);
    CHECK(timer_runs == 1);

    /* a new page: every handle goes, and the painter's user data is freed */
    CHECK(cscd_view_load_html(v, "<p id=p>new</p>", "") == CSCD_OK);
    CHECK(!cscd_element_valid(v, go));
    CHECK(!cscd_element_valid(v, box));
    CHECK(freed == 2);
    cscd_element p = cscd_view_query(v, "#p");
    CHECK(streq_free(cscd_element_text(v, p), "new"));
    CHECK(cscd_view_load_file(v, "no/such/file.html") == CSCD_ERR_NOT_FOUND);

    /* features, copies of a template, a list that makes only the rows in sight */
    CHECK(cscd_view_feature(v, CSCD_FEATURE_ANIMATIONS) == 1);
    CHECK(cscd_view_set_feature(v, CSCD_FEATURE_ANIMATIONS, 0) == CSCD_OK && cscd_view_feature(v, CSCD_FEATURE_ANIMATIONS) == 0);
    CHECK(cscd_view_set_feature(v, (cscd_feature)99, 1) == CSCD_ERR_ARGUMENT);
    CHECK(cscd_view_load_html(v, "<style>#box{height:60px;overflow:auto}</style><template id=t><div class=row>Row {{n}}</div></template>"
                                  "<div id=out></div><div id=box></div>", "") == CSCD_OK);
    cscd_view_update(v);
    cscd_element row = cscd_element_append_copy(v, cscd_view_query(v, "#out"), cscd_view_query(v, "#t"));
    CHECK(row != 0 && cscd_element_fill(v, row, "n", "1") == CSCD_OK);
    CHECK(streq_free(cscd_element_text(v, row), "Row 1"));
    CHECK(cscd_view_bind_list(v, cscd_view_query(v, "#box"), cscd_view_query(v, "#t"), 1000, 20, fillRow, NULL, NULL) == CSCD_OK);
    cscd_view_update(v);
    cscd_element rows[64];
    const size_t shown = cscd_view_query_all(v, "#box .row", rows, 64);
    CHECK(shown >= 3 && shown < 20);
    CHECK(shown && streq_free(cscd_element_text(v, rows[0]), "Row 0"));
    CHECK(cscd_view_list_set_count(v, cscd_view_query(v, "#box"), 2) == CSCD_OK);
    cscd_view_update(v);
    CHECK(cscd_view_query_all(v, "#box .row", rows, 64) == 2);

    cscd_view_delete(v);
    CHECK(cscd_element_valid(NULL, p) == 0);
    CHECK(cscd_view_query(NULL, "p") == 0);
    CHECK(cscd_element_set_text(NULL, p, "x") == CSCD_ERR_ARGUMENT);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
