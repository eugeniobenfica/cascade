# The handle API

cascade's own API ([Conventions](conventions.md)) is C, with pointers to elements as the DOM
holds them. `cascade/cascadeC.h` is a second, smaller layer over it for bindings — Python through
`ctypes`, Rust, C#, Go, Zig, Lua — where a pointer that outlives its element is a crash that the
other language cannot catch: elements are **handles** that stop working when their element goes,
every function says whether it worked, and the ABI stays stable. It covers views, elements,
events, painters, timers and the window.

```c
#include "cascade/cascadeC.h"

static void saved(cscd_view* v, const cscd_event* ev, void* user) {
    cscd_element status = cscd_view_query(v, "#status");
    cscd_element_set_text(v, status, "Saved.");
}

int main(void) {
    cscd_window* w = cscd_window_new("My app", 1100, 720);
    cscd_view* ui = cscd_window_view(w);
    cscd_view_load_file(ui, "ui/index.html");
    cscd_element_on(ui, cscd_view_query(ui, "#save"), "click", saved, NULL, NULL);
    int code = cscd_window_run(w);
    cscd_window_delete(w);
    return code;
}
```

Link the program with the `cascade` library: in CMake, `target_link_libraries(app PRIVATE cascade)`.
Both headers can be used in one program, in either order.

## Elements are handles

An element is a `cscd_element`, a 64-bit number, not a pointer. When the element leaves the
document (removed, replaced when its parent's HTML or text is set, or gone with a new page), its
handle stops working: the functions given it return `CSCD_ERR_INVALID_HANDLE` (or 0 / NULL) instead
of reaching freed memory. Asking again for the same element gives the same handle while it stays.

```c
cscd_element list = cscd_view_query(ui, "#list");
cscd_element first = cscd_element_first_child(ui, list);
cscd_element_set_html(ui, list, "<li>new</li>");   /* the old items are gone */
cscd_element_valid(ui, first);                       /* 0 */
```

## Rules

| | |
| --- | --- |
| Text | UTF-8 in and out. A `char*` a function returns is yours: free it with `cscd_free`. |
| Results | Functions that act return `cscd_result` (`CSCD_OK` or why not); those that return something return 0 or NULL when they cannot. `cscd_last_error()` says what went wrong, for this thread. |
| Callbacks | Every callback takes a `void* user`. Where a `cscd_free_fn` is asked for, it runs once when the callback is dropped (its listener removed, its element deleted, the page replaced), to free that data. |
| Threads | Everything on the thread that made the view, except `cscd_window_post`, which may be called from any thread. |
| Failures | Bad arguments and dead handles become an error code with a message, never a crash. |
| Versions | `cscd_version()` gives the library's version, `CSCD_C_API_VERSION` the header's. |

## Without a window

A `cscd_view` on its own renders to pixels, as `cascade-render` does:

```c
cscd_view* v = cscd_view_new();
cscd_view_load_html(v, "<h1>Hello</h1>", NULL);
cscd_view_set_viewport(v, 800, 600, 2);
int w, h;
const uint32_t* pixels = cscd_view_paint(v, &w, &h);   /* 1600 x 1200, premultiplied BGRA */
cscd_view_delete(v);
```

`cscd_view_paint` keeps the frame between calls and paints again only what changed, so a program
that shows the view on its own surface can call it every frame.

## Painters

```c
static void draw(cscd_canvas* c, float w, float h, void* user) {
    cscd_canvas_fill_rect(c, 0, 0, w, h, 0x121118FF);                /* 0xRRGGBBAA */
    cscd_canvas_fill_path(c, "M 0 40 L 40 0 L 80 40 Z", 0x7C3AEDFF);  /* SVG path data */
    cscd_canvas_draw_text(c, "Level", 8, h - 8, "Inter, sans-serif", 14, 600, 0xFFFFFFFF);
}

cscd_element_set_painter(ui, cscd_view_query(ui, "#meter"), draw, NULL, NULL);
cscd_element_invalidate(ui, cscd_view_query(ui, "#meter"));          /* just its box again */
```

## From other languages

Configure with `-DCASCADE_C_SHARED=ON` and the build also makes `cascade_c`, a shared library
(`cascade_c.dll`, `libcascade_c.so`, `libcascade_c.dylib`) that exports the C functions and
nothing else; the library is inside it. Windows programs in C that link to it define
`CASCADE_C_DLL`.

```python
import ctypes
be = ctypes.CDLL("./cascade_c.dll")
view, element = ctypes.c_void_p, ctypes.c_uint64
be.cscd_view_new.restype = view
be.cscd_view_load_html.argtypes = [view, ctypes.c_char_p, ctypes.c_char_p]
be.cscd_view_query.restype = element
be.cscd_view_query.argtypes = [view, ctypes.c_char_p]
be.cscd_element_text.restype = ctypes.c_void_p
be.cscd_element_text.argtypes = [view, element]
be.cscd_element_on.argtypes = [view, element, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
be.cscd_free.argtypes = [ctypes.c_void_p]

v = be.cscd_view_new()
be.cscd_view_load_html(v, "<p id=p>olá</p><button id=b>Go</button>".encode(), None)
text = be.cscd_element_text(v, be.cscd_view_query(v, b"#p"))
print(ctypes.string_at(text).decode())   # olá
be.cscd_free(text)

on_click = ctypes.CFUNCTYPE(None, view, ctypes.c_void_p, ctypes.c_void_p)(lambda v, ev, user: print("clicked"))
be.cscd_element_on(v, be.cscd_view_query(v, b"#b"), b"click", ctypes.cast(on_click, ctypes.c_void_p), None, None)
```

(Keep the `CFUNCTYPE` object alive as long as the listener may be called.)

Ready-made bindings are in [`bindings/`](../bindings): **Python** (`bindings/python/cascade.py`,
the standard library only: `View`, `Element`, `Window`, `Canvas`, events as Python functions,
strings both ways) and **Rust** (`bindings/rust`, a crate with closures for listeners, painters and
list rows). They find the shared
library next to the program, or through `CASCADE_LIB` (Python) and `CASCADE_LIB_DIR` (Rust's
build).

```python
import cascade
view = cascade.View()
view.load_html("<p id=p>Hello</p><button id=b>Go</button>")
view["#b"].on("click", lambda ev: view["#p"].set_text("Clicked"))
```

The whole list of functions is in [`cascadeC.h`](../include/cascade/cascadeC.h), each with a
line on what it does.
