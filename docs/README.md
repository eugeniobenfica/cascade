<p align="center"><img src="assets/logo.svg" width="72" alt="cascade"></p>

# cascade documentation

1. [Getting started](gettingStarted.md) — build, add it to a project (CMake, vcpkg, pkg-config, only your platforms), the first window, rendering without one.
2. [Conventions](conventions.md) — names, strings, vectors, ownership and callbacks in the C API.
3. [HTML and CSS](htmlAndCss.md) — what pages can use, what is not there yet, using styles from the web.
4. [The interface](interface.md) — events, forms, menus, dialogs, saving files, the menu bar and the tray, popovers, tooltips, drag and drop, touch, timers, threads.
5. [Drawing](drawing.md) — painters, the canvas, images and fonts, bringing in another renderer.
6. [Rendering and performance](rendering.md) — the pipeline, what repaints when, keeping frames fast.
7. [Platforms](platforms.md) — Windows, Linux, macOS, iOS and Android: each one's window, its support tier, the build options.
8. [Porting from Qt / Skia](porting.md) — moving a Qt Widgets application over.
9. [The handle API](cApi.md) — `cascadeC.h`, for Python, Rust, C#, Go and anything that wants handles that cannot dangle.
10. [Principles](principles.md) — what goes in and what does not: dependencies, pixels, platforms, versions.
11. [Rasterizing on the GPU](gpuRasterization.md) — a study: the GPU painting frames with the same pixels everywhere.

## The source

The engine is in `src/`, a folder per part; each part's public header is in `include/cascade/`.

| Path | What |
| --- | --- |
| `src/core/` (`core.h`) | `base.c`: memory, strings and views, vectors, maps, sorting, UTF-8; `files.c`: files, the program's own files and its loader; `thread.h`: threads, locks, atomics and the parallel loops |
| `src/dom/` (`dom.h`, `bind.h`) | `html.c`: the HTML parser; `dom.c`: the DOM; `bind.c`: data binding, models and the data-* attributes that show them |
| `src/style/` (`css.h`, `style.h`) | `css.c`: CSS syntax, selectors, media queries, style sheets; `cascade.c`, `style.c`, `properties.c`, `propertiesValues.c`, `styleValues.c`: the cascade and computed values |
| `src/layout/` (`layout.h`) | `layout.c`: the box tree, block, inline and table layout; `flexgrid.c`: flex and grid |
| `src/text/` (`font.h`) | `font.c`: the TrueType, CFF and WOFF reader, matching and fallback, the shaper; `fontOt.c`, `fontIndic.c`: OpenType layout and the Indic syllable model; `fontColor.c`, `fontVar.c`: color glyphs and variable fonts; `bidi.c`: the bidirectional algorithm; `bidiData.inc`, `unicodeData.inc`: tables from `tools/genBidi.py` and `tools/genUnicode.py` |
| `src/image/` (`image.h`) | `image.c`, `imageVp8.c`: inflate, PNG, JPEG, GIF, WebP (VP8 and lossless, animated) and BMP decoding, PNG encoding|
| `src/paint/` (`canvas.h`) | `canvasPath.c`: paths, the stroker, the rasterizer; `canvas.c`: shaders, blur, the glyph cache, display lists, painting by bands; `paint.c`: painting the page's boxes |
| `src/view/` (`view.h`) | `view.c`: the view, the frame, timers, queries, with `viewInternal.h` naming what its files share; `viewLoad.c` loading, `viewAnimate.c` transitions and animations, `viewRestyle.c` styles and layout after a change, `viewScroll.c` scrolling, `viewEvents.c` events and the mouse, `viewKeys.c` the keyboard and editing text, `touch.c` touch and selecting by touch, `widgets.c` menus, tooltips, dialogs, popovers, drag and drop, splitters, long lists, the clipboard, `a11y.c` the accessible tree, `inspector.c` the inspector (F12) |
| `src/capi/` (`cascadeC.h`) | `capi.c`: the handle API |
| `bindings/` | Python and Rust over the handle API |
| `include/cascade/window.h`, `src/platform/common/` | the window: what every platform shares (the frame, the presenter's choice, the posted functions, the system's settings and services) |
| `src/platform/windows/` | Win32, WGL and GDI, UI Automation |
| `src/platform/linux/` | Wayland and X11 (chosen at run time), GLX and shared memory, AT-SPI (D-Bus) |
| `src/platform/macos/` | Cocoa, NSOpenGLContext, Core Animation, NSAccessibility |
| `src/platform/ios/` | UIKit, UITextInput, Core Animation, VoiceOver (tier 2) |
| `src/platform/android/` | the C side and cascade's activity in Java (`java/`: `CascadeActivity` and its parts, `CascadePageView`, `CascadeInputConnection`, `CascadeBackdrop`, `CascadeEditMenu`, `CascadeDrops`, `CascadeAccessibility`, `CascadeFiles`): input method, insets, TalkBack (tier 2) |
| `include/cascade/gpu.h`, `src/platform/gpu/` | the Vulkan and OpenGL presenters (each platform's software path is in its module) |
| `cmake/`, `src/platform/*/platform.cmake` | `cascade_app()`, the installed CMake package and `cascade.pc`, and what each platform's module builds and links |
| `packaging/` | the vcpkg port |
| `examples/gallery` | the example interface |
| `tools/render.c` | `cascade-render`: HTML to PNG |
| `tests/tests.c`, `tests/capi.c`, `tests/testFont.inc` | the tests, and the handle API's; the font the shaping tests use comes from `tools/makeTestFont.py` |
| `tests/testFixtures.inc`, `tests/pixelsExpected.inc` | the JPEGs and fonts the tests decode, from `tools/makeTestFixtures.py`, and the hashes every platform's pixels must give |
| `tests/fuzz.c` | the fuzzers' targets, a program each: parsers and decoders (`CASCADE_FUZZ`) |
| `tools/slim.cmake` | a copy of the sources with only the platforms you build for |
