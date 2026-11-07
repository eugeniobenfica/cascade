# Getting started

## Build

cascade is a CMake project with no third-party libraries. A C11 compiler is all it needs (GCC, Clang or
MSVC; Objective-C for the Apple windows, which every Mac compiler has). Each release also carries
the built SDK of every platform: see [Add it to a project](#add-it-to-a-project).

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build                 # cascade-tests and cascade-capi-tests
./build/cascade-gallery                # the example interface
```

| Option | Default | What |
| --- | --- | --- |
| `CASCADE_WINDOW` | `ON` | The native window of the target's platform and its presenters. Off: the library renders into images only. |
| `CASCADE_VULKAN`, `CASCADE_OPENGL` | `ON` | Those presenters; the system's software path is always there. |
| `CASCADE_ACCESSIBILITY` | `ON` | The bridges to the platform's screen readers. |
| `CASCADE_INSTALL` | top level | `cmake --install`: the library, its headers and its CMake package. |
| `CASCADE_BUILD_EXAMPLES` | top level | `cascade-gallery`, the example interface. |
| `CASCADE_BUILD_TOOLS` | top level | `cascade-render`, HTML to PNG from the command line. |
| `CASCADE_BUILD_TESTS` | top level | `cascade-tests`, and `cascade-capi-tests` (the handle API). |
| `CASCADE_C_SHARED` | `OFF` | `cascade_c`, a shared library of the [handle API](cApi.md), for other languages to load. |
| `CASCADE_TARGET` | `cascade` | The library target's name, for projects that already have a target called `cascade`. |
| `CASCADE_SANITIZE` | empty | Sanitizers to build with, GCC and Clang (`address,undefined`): the library and what links it. |
| `CASCADE_FUZZ` | `OFF` | `cascade-fuzz-html`, `-css`, `-image`, `-font`, `-zlib` ([tests/fuzz.c](../tests/fuzz.c)): with Clang, a libFuzzer target; otherwise a program that runs the files it is given. |

The examples, tools and tests are built by default only when cascade is the top-level project.

**Linux and the BSDs** need the Xlib headers to build the window: `libx11-dev` (Debian, Ubuntu)
or `libX11-devel` (Fedora). Nothing else: Vulkan (`libvulkan.so.1`) and OpenGL (`libGL.so.1`) are
opened when the program runs.

## Add it to a project

From a release's SDK, or from `cmake --install build --prefix <dir>`: put its folder in
`CMAKE_PREFIX_PATH`, then

```cmake
find_package(cascade 1 REQUIRED)
add_executable(my_app WIN32 main.c)
cascade_app(my_app UI ui)
```

As a subdirectory (a copy, a git submodule or a git subtree):

```cmake
add_subdirectory(third_party/cascade)
add_executable(my_app main.c)
cascade_app(my_app UI ui)       # links cascade and puts ui/ next to the program
```

Or fetched at configure time:

```cmake
include(FetchContent)
FetchContent_Declare(cascade GIT_REPOSITORY https://github.com/eugeniobenfica/cascade.git GIT_TAG v1.0.0)
FetchContent_MakeAvailable(cascade)
add_executable(my_app main.c)
cascade_app(my_app UI ui)
```

### Only your platforms

A build already compiles only the target's module, but a copy of the repository carries all of
them. To vendor cascade with only what your platforms need, make a slim copy:

```bash
cmake -DPLATFORMS=windows -DOUT=third_party/cascade -P tools/slim.cmake
cmake -DPLATFORMS="desktop" -DWITH="examples;tests" -DOUT=../cascade-desktop -P tools/slim.cmake
cmake -DPLATFORMS=none -DOUT=../cascade-engine -P tools/slim.cmake    # the engine alone, no window
```

`PLATFORMS` takes `windows`, `linux`, `macos`, `ios`, `android`, `desktop` (the first three),
`mobile` (the last two), `all` or `none`; `WITH` adds `examples`, `tests`, `tools`, `bindings` or
`docs`. A desktop copy has no iOS or Android code, and a Windows one alone is about half the
repository. The copy builds as the full tree does; its options follow what is in it (no tests in the
copy, none built), and configuring it for a platform it left out stops with a message saying so.

### Package managers and pkg-config

With **vcpkg**, the port is in `packaging/vcpkg/ports/cascade` (each release carries it with its
sources' hash, `cascade-<version>-vcpkg-port.zip`): use it as an overlay port, then
`find_package(cascade CONFIG REQUIRED)` as above.

```bash
vcpkg install cascade --overlay-ports=packaging/vcpkg/ports
```

Builds without CMake use **pkg-config**: `cmake --install` puts `cascade.pc` in `lib/pkgconfig`,
with what the library links on that platform (it is static).

```bash
cc main.c $(pkg-config --cflags --libs cascade) -o app
```

`cascade_app(<target> [UI <folder>])` does what an application needs:

- links `cascade::cascade`;
- copies the interface folder next to the program, again whenever a file in it changes, so the
  program finds it wherever it is started from;
- in **debug builds**, makes the program read its interface from the source folder instead,
  with live reload: save a `.css` file and the window shows it, with the page as it was;
- with **MinGW**, links the runtime into the program, so it starts outside the MinGW shell.

`target_link_libraries(my_app PRIVATE cascade::cascade)` links the library alone. The headers
compile as C and as C++ (they are wrapped in `extern "C"`).

Every name starts with `Cscd` (types), `cscd` (functions) or `CSCD_` (constants and macros); see
[Conventions](conventions.md). `#include "cascade/cascade.h"` brings in the DOM, the view and the
canvas; `cascade/window.h` adds the native window.

## The first window

A window owns a `CscdView`, which owns the document. Load a page, find its elements, answer their
events, and run the loop:

```c
#include "cascade/cascade.h"
#include "cascade/window.h"

typedef struct Notes { CscdElement* list; CscdElement* field; } Notes;

static void onKey(void* user, CscdEvent* ev) {
    Notes* app = user;
    CscdStr text = cscdElementValue(app->field);
    if (cscdStrEqC(&ev->key, "Enter") && text.len) {
        cscdElementSetText(cscdElementAppendElement(app->list, CSCD_SV("li")), cscdStrSv(&text));
        cscdElementSetValue(app->field, CSCD_SV(""));
    }
    cscdStrFree(&text);
}

int main(void) {
    CscdWindowOptions opts = cscdWindowOptions();
    opts.title = "Notes";
    opts.width = 960;
    opts.height = 640;
    opts.darkTitleBar = true;
    CscdWindow* win = cscdWindowNew(&opts);

    CscdView* ui = cscdWindowView(win);
    cscdViewLoadFile(ui, CSCD_SV("ui/notes.html"));

    Notes app = {cscdViewGet(ui, CSCD_SV("#list")), cscdViewGet(ui, CSCD_SV("#new"))};
    cscdElementOn(app.field, CSCD_SV("keydown"), onKey, &app);
    int code = cscdWindowRun(win);
    cscdWindowFree(win);
    return code;
}
```

```html
<!-- ui/notes.html -->
<!doctype html>
<title>Notes</title>
<link rel="stylesheet" href="notes.css">
<input id="new" placeholder="Write a note, press Enter">
<ul id="list"></ul>
```

Paths in the page (`<link>`, `<img src>`, `@font-face`, `url()`) resolve against the HTML file's
folder. `cscdViewLoadHTML(view, source, baseDir)` loads a page from a string instead.

### Finding the interface files

A relative path given to `cscdViewLoadFile`, `cscdViewAddStyleSheetFile` or `cscdViewAddFont` that is
not found from the working folder is looked for next to the program (and in a macOS bundle's
`Resources`): start the program from anywhere and `"ui/index.html"` is found. `cscdAppFindFile(path)`
does the same for your own files, and `cscdExecutableDir()` is the program's folder (both return a
`CscdStr` you free).

### A window shown when it is ready

Give the window its page, and it loads it before it first shows, so it never shows an empty
frame:

```c
CscdWindowOptions opts = cscdWindowOptions();
opts.title = "";                 /* empty: the page's <title> */
opts.page = "ui/index.html";
CscdWindow* win = cscdWindowNew(&opts);
/* cscdViewGet(cscdWindowView(win), ...) — the page is there already */
```

To show it later (maximized, say), make it with `opts.visible = false` and call
`cscdWindowMaximize(win)` or `cscdWindowShow(win)`.

### When something is missing

A page, style sheet, image or font that cannot be read is reported, with the path that was
tried:

```
cascade: cannot read the style sheet theme.css (C:/apps/notes/ui/theme.css)
```

The messages go to stderr and, on Windows, to the debugger's output. Send them elsewhere with
`cscdViewOnWarning(view, fn, user)`, or give it a NULL function to silence them. `cscdViewGet` never
returns NULL: a selector that matches nothing warns and gives a detached element, so a typo shows
up as a message instead of a crash.

A property in a style sheet that cascade does not compute is reported too, once per load, with the
one it probably meant (vendor-prefixed and custom properties are left alone):

```
cascade: unknown CSS property widht (in ui/style.css): ignored; did you mean width?
```

### Live reload

`cscdViewSetFeature(view, CSCD_FEATURE_LIVE_RELOAD, true)` reads the page's style sheet files again when they change on
disk; the document, its state and your handlers stay as they are. A window turns it on by itself
in debug builds of a program made with `cascade_app()` (`CscdWindowOptions.liveReload`: `1` on, `0`
off, `-1` that default). Changes to the HTML need the program started again.

### Files from the program itself

An application that keeps its interface inside the program (an array the build made, an archive)
gives cascade a loader. Every file cascade reads (pages, style sheets, `@import`, images,
`@font-face`) is first asked of it, with the path as cascade would open it; it fills the bytes and
returns true, or returns false and the file is read from disk. It may be called from several
threads at once.

```c
static bool fromBundle(void* user, CscdSv path, CscdStr* out) {
    const Asset* a = findAsset(user, path);         /* (the program's own table) */
    if (!a) return false;
    cscdStrAppend(out, (CscdSv){(const char*)a->data, a->size});
    return true;
}
cscdAppSetResourceLoader(fromBundle, &assets);
cscdViewLoadFile(ui, CSCD_SV("ui/index.html"));   /* ui/app.css, ui/logo.png... come from it too */
```

## Without a window

A `CscdView` renders on its own: give it a viewport and paint it into an image.

```c
CscdView* view = cscdViewNew();
cscdViewLoadFile(view, CSCD_SV("report.html"));
cscdViewSetViewport(view, 800, 1000, 2.0f);     /* CSS pixels and the device pixel ratio */
CscdImage img;
cscdImageInit(&img, 1600, 2000);                 /* width * dpr x height * dpr */
cscdViewPaint(view, &img, false);
cscdSavePNG(CSCD_SV("report.png"), &img);
cscdImageFini(&img);
cscdViewFree(view);
```

`cascade-render page.html out.png [width] [height] [dpr]` does the same from the command line
(height 0: the page's own height).

The pixels are premultiplied BGRA, ready for most GPU textures and for Skia's `kBGRA_8888`.

## Inside a renderer you already have

To show the page in an OpenGL context you own (a game engine, an application being ported), paint
it into an image every frame and upload it:

```c
cscdGlLoadFunctions();                       /* once the context is current */
unsigned tex = cscdGlCreateTexture();

/* every frame */
if (cscdViewUpdate(view)) {                  /* styles, layout, timers and animations up to date */
    cscdViewPaint(view, &frame, true);       /* (only what changed is painted again) */
    cscdGlUpload(tex, &frame);
}
cscdGlDrawFullscreen(tex, width, height);
```

Feed it input in CSS pixels: `cscdViewMouseMove`, `cscdViewMouseDown`, `cscdViewMouseUp`, `cscdViewWheel`,
`cscdViewKeyDown`, `cscdViewKeyUp`, `cscdViewTextInput`, and the touch and drag calls
([The interface](interface.md)).

## Next

- [Conventions](conventions.md): strings, vectors, ownership and callbacks.
- [HTML and CSS](htmlAndCss.md): what pages can use.
- [The interface](interface.md): events, menus, dialogs and the rest of an application's needs.
- [Drawing](drawing.md): painting your own elements.
