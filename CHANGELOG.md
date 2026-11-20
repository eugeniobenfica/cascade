# Changelog

Every release of cascade, newest first. The version follows [Semantic Versioning](https://semver.org):
a new major version can break what compiled against the one before; a minor one adds without
breaking; a patch one only fixes. CMakeLists.txt's `project()` holds the version; the README's
badge, this file, the bindings and the vcpkg port repeat it, and CI checks that they agree.

## Unreleased

- Layout and painting stop going deeper when the thread's stack is nearly used up, instead of overflowing it: a page of
  nested flex boxes took about 1.6 KB of stack a level, and 1024 of them (`CSCD_MAX_TREE_DEPTH`) crashed a 1 MB stack
  (MSVC's main thread, a macOS secondary thread has 512 KB). What lies deeper is not laid out or painted. The painter's
  helpers are kept out of its recursive walk, which took 1136 bytes a level and takes 432.
- Found by the nightly fuzzer: nested CSS rules whose `&` copied the parent selector more than once doubled it at
  every level (7.9 KB of CSS took 51 s and 14 GB): a nested selector longer than 64 KiB is dropped with what it nests.
  A JPEG of a few hundred bytes that declared a hundred million pixels was decoded from zeros past its end (650 MB,
  3 s): one whose bytes cannot hold a 64th of its blocks is refused. A shadow on a box larger than floats count in
  pixels read past its blurred patches. A glyph at a position that is not a number (a text field's text) is skipped
  instead of converted to an int. The inputs are in `tests/corpus`.

- **Repainting what changed.** A class, an inline style or a `data-`, `aria-`, `id`, `role` or `title` attribute
  changed: the window paints again the elements that look different and the boxes that moved, not all of it (styles
  and layout are still worked out for the whole page). A list's highlighted row costs that row.
- **Painting what shows.** A box whose ink lies outside the area being painted is passed over with all it holds: a page
  of 20,000 rows painted in 4.9 ms instead of 48.7, its hit areas found in 3.1 instead of 42.5.
- **Shadows blurred by their corners** (nine patches): a large shadow costs what a small one does, and boxes of every
  size with the same corners share one. The hero page painted from cold in 20 ms instead of 40 to 54 (85 instead of
  176 to 218 at 2x), its shadows kept in half the memory. A shadow's long sides may differ by a level at most.
- `:nth-child` and its kin worked out once per list: 10,000 items styled in 3 ms instead of 195.
- **Images within a limit**: past 256 MB (`cscdViewSetImageCacheLimit`, `cscdViewImageCacheBytes`) the images nothing
  shows go, the longest unused first. `CSCD_FEATURE_ASYNC_IMAGES` (off) decodes large image files on two decoder threads
  and lays the page out again as they come (`cscdViewImagesDecoding`).
- Vulkan uploads only the rectangles that changed, into an image the device keeps (it copied the whole frame on each
  present). `CASCADE_VULKAN_CHECK=1` reads each frame back and compares it.
- `cscdSetAllocator` and `cscdSetOutOfMemoryHandler`: the program's own allocator, and a handler that frees memory and
  has a failed request tried again, before the process ends.
- `CSCD_MAX_TREE_DEPTH` (1024): `cscdNodeAppendChild` and `cscdNodeInsertBefore` refuse a tree deeper than that (the
  recursions of styling, layout and painting stayed within a thread's stack only for parsed pages) and a loop (an
  element into itself or what it holds); they return NULL.
- Fixed, found by the new oracle of the light paths (`cascade-incremental-tests`, which compares every incremental frame
  with the whole pipeline from scratch): a hovered shadow that grew left its faint tail on screen; a box's scrollbar
  stayed drawn below it when its content shrank (scroll offsets are now kept in range after each layout), and did not
  follow when a text change resized its content; and a shape cut by a repainted area came out a level off from the same
  shape painted whole (coverage is now summed over the shape's whole width, wherever the clip cuts it).
- `:nth-child(an+b)` with numbers near the limits of an int no longer overflows.
- Checks: `cascade-memory-tests` (every block counted through the allocator: a page made and freed again and again
  comes back to the same bytes; a request failing anywhere in a frame is retried with the same pixels),
  `cascade-incremental-tests`, ThreadSanitizer on Linux, `-Wextra`, and warnings as errors on MinGW (`CASCADE_WERROR`).

- Windows: keys with Alt are the page's. Alt and a key beeped (the window has no menu for it), and Alt or F10 let go
  alone put the window in its system menu's mode, where the next key went. Alt+F4 still closes the window and
  Alt+Space still opens its menu.
- Enter in a field of a form presses its default button (HTML's implicit submission): a `<form method=dialog>` used
  to close with no value.
- `cscdViewSetBaseStyleSheet`: a sheet under the document's own, kept from page to page.
- `data-focus="keyboard"`: an element Tab reaches and a click does not; Esc leaves it. Tab goes on from where it was when
  nothing has focus. A menu of the bar opens with Enter, Space or Down.
- Fonts: a CBLC, sbix, gvar, item-variation or mark-to-base table whose counts multiply past 32 bits is refused,
  not read beyond the font.
- A lossy WebP whose frame claims more macroblocks than its bytes hold fails at once, as libwebp's
  "premature end-of-file", instead of decoding millions of them from zeros.
- A gradient placed at an infinite length (a size too big for a float) paints its first color.
- Windows accessibility: a check box's or a field's change, told for an element that is gone, sends nothing instead of
  an unset value.
- `cscdSvStarts`, `cscdSvEnds` and `cscdStrAppend` with an empty view that has no pointer: no `memcmp` or `memcpy` of it.

## [1.0.0] - 2026-11-03

The first release: HTML and CSS interfaces driven from plain C11, on five platforms, with no third-party
libraries.

### The engine

- **HTML and CSS**: an HTML5 parser and the DOM; selectors level 4, custom properties, `calc()`,
  `@media`, `@container`, `@keyframes`, transitions, nesting, `oklch()`, `color-mix()`, counters,
  3D transforms, `backdrop-filter`, masks and `touch-action`.
- **Layout**: block and inline flow, flexbox, grid with named areas and subgrid, tables, floats,
  positioning, sticky, scrolling, writing modes, bidirectional text.
- **Text**: OpenType shaping (Arabic, Indic), color and variable fonts, WOFF, the system's
  fonts.
- **Images**: PNG, JPEG (progressive), GIF and WebP (animated, lossy and lossless).
- **Painting**: an anti-aliased rasterizer that paints the same pixels everywhere, by bands on
  several threads (the library's workers, kept between frames; a large shadow or a picture drawn
  smaller is made once for all the bands), repainting only what changed; painters give any element
  a 2D canvas.
- **Building applications**: events, forms with undo, menus, dialogs, popovers, tooltips, drag and
  drop, touch (with the time of each sample, flings, pinches, and selecting text with handles, the
  system's edit menu and its magnifier), the clipboard from the program, data binding, templates,
  lists of a hundred thousand rows, an inspector (F12), frame statistics, and features an app can
  turn off.
- **The program's own files**: `cscdAppSetResourceLoader` serves the pages, style sheets, images and
  fonts cascade reads from wherever the program keeps them (built into it, an archive) before the
  disk.
- **A scene under the page**: `CscdWindowOptions.presenter` lets an app make the window's presenter
  and draw in the window itself, and `cscdViewSetTransparent` leaves the page's empty parts
  transparent over what it drew ([Rendering](docs/rendering.md#under-the-page-a-scene-of-the-apps)).
- **Paint caches you control**: `cscdPaintCacheSetLimits`, `cscdPaintCacheStats` and
  `cscdPaintCacheClear` for the shadows and glyphs kept between frames; a full cache drops what went
  longest unused and no longer everything at once, which stalled scrolling over many small shadows
  ([Rendering](docs/rendering.md#the-paint-caches)).
- **The handle API** (`cascadeC.h`): handles that cannot dangle and a stable ABI, with
  Python and Rust bindings, each tested by CI.

### Platforms

- **Tier 1**: Windows (Win32), Linux and the BSDs (Wayland or X11, chosen at run time) and macOS
  (Cocoa), each presenting through Vulkan, OpenGL or the system's software path, with input methods,
  the clipboard, drops from other apps and screen readers (UI Automation, AT-SPI, NSAccessibility).
- **Tier 2, experimental**: iOS (UIKit) and Android (an activity of cascade's, in Java), built and
  tested on a simulator and an emulator, Android also on phones (`tools/androidDeviceTest.sh`):
  the system's keyboard of the field's kind with its suggestions and input methods, VoiceOver and
  TalkBack, the safe area and what the page shows around it, the app's life, links, sharing, the
  clipboard's pictures and files, drops from other apps and vibration.
- **The system around the app**: the file picker and a dialog to save a file
  (`cscdWindowSaveFile`) on every platform; the macOS menu bar (`cscdWindowSetMenuBar`); an icon
  in the tray on Windows and macOS (`cscdTrayNew`), with its menu; `cscdWindowHide` for an app that
  lives there. The system's theme, text size and "reduce motion" reach the page.
- **Each platform is a module** of its own (`src/platform/<platform>`); Vulkan, OpenGL and the
  screen readers' bridges can be left out of a build (`CASCADE_VULKAN`, `CASCADE_OPENGL`,
  `CASCADE_ACCESSIBILITY`).

### Distribution

- `cmake --install` installs the library, its headers, a CMake package (`find_package(cascade)` and
  `cascade_app()`) and `cascade.pc` for pkg-config.
- **vcpkg** (`packaging/vcpkg`, each release carries the port with its sources' hash).
- `tools/slim.cmake` makes a copy of the sources with only the platforms you build for: a desktop
  copy has no iOS or Android code.
- Each release carries the SDK of every platform (an XCFramework for iOS), the gallery ready to run
  on desktops and Android, and checksums.

### Checks

- CI builds and tests every platform (MinGW and MSVC on Windows), the bindings, the packages and the
  slim copies; the tests also run under AddressSanitizer and UndefinedBehaviorSanitizer, and
  libFuzzer feeds the HTML and CSS parsers, the image and font decoders and zlib, each a target of
  its own: a minute of each on every change, half an hour each every night, on a corpus that grows
  (`tests/fuzz.c`, `CASCADE_SANITIZE`, `CASCADE_FUZZ`).
- **What listeners do to the page**: `cascade-stress` makes random changes and sends random input
  while listeners remove elements and replace the page, for the sanitizers to watch.
- **The same pixels, proved**: `cascade-tests` paints seven pages at four scales, with a font and
  pictures of its own, and compares their hashes with `tests/pixelsExpected.inc` on every platform,
  on one thread and on four.

[1.0.0]: https://github.com/eugeniobenfica/cascade/releases/tag/v1.0.0
