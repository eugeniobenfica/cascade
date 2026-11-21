# Changelog

Every release of cascade, newest first. The version follows [Semantic Versioning](https://semver.org):
a new major version can break what compiled against the one before; a minor one adds without
breaking; a patch one only fixes. CMakeLists.txt's `project()` holds the version; the README's
badge, this file, the bindings and the vcpkg port repeat it, and CI checks that they agree.

## Unreleased

- **Conformance suites in CI**: Unicode's bidi tests (all 861,948 cases), html5lib's tree construction tests and
  css-parsing-tests (known failures listed), 58 layout fixtures laid out by Chrome, and web-platform-tests' CSS
  reftests scored by folder (`tests/wpt.score`).
- **Lines of text are whole pixels high**: a font's ascent, descent and line gap are rounded at each size, as browsers
  round them, so boxes after text stay on the pixel grid (2,054 WPT reftests pass instead of 846).
- Fixed: the bidi algorithm took sos and eos from resolved levels; `lab()` and `lch()` were approximated through
  Oklab, and `lab(100% 0 0)` was black; `:nth-child(3n - 1)` matched as 3n+1, and invalid An+B was accepted; a
  declaration's name could be anything before its colon; a flex item with a width did not shrink below it; a relative
  offset moved the boxes after it; `grid-auto-flow: dense` did not fill holes; `minmax(x, auto)` rows ignored their
  contents. New: `color(srgb ...)` and `color(srgb-linear ...)`.

- **Transform animations keep their image**: an element whose transform animates by moving (and scaling) is drawn
  through an image of its own placed on whole device pixels, in a frame and in a page painted from scratch alike;
  a frame that only moves it places the image again. 24 cards sliding: ~3.6 → ~0.3 ms a frame at dpr 2.

- **WOFF2**: `@font-face` and `cscdFontFaceLoad` read WOFF2 files (what Google Fonts and most sites serve), with a
  Brotli decoder of the library's own (RFC 7932: the static dictionary, every context mode, large windows), the
  glyf, loca and hmtx transforms and font collections.
- **Cursive attachment** (GPOS type 3): Nasta'liq and other cursive scripts join and step along their baselines.
- **More cmaps**: variation sequences (format 14: a CJK character and its variation selector draw the variant), format
  13, and Macintosh fonts (Mac OS Roman and Turkish).
- **Kannada and Telugu** as HarfBuzz shapes them (matras before the below-base forms, pre-base forms, a vowel with a
  consonant); fixed: since the last change every Indic syllable took its last consonant as its base.
- Fixed: a default ignorable character (ZWNJ, ZWJ) in a syllable showed the font's glyph for it.

- **More scripts shaped by their models**: Thai and Lao (SARA AM split, NIKHAHIT before the tone marks), Hangul jamo
  (composed into the syllables the font has), Khmer and Myanmar (syllables reordered: pre-base vowels, Coeng Ro, Medial
  Ra, kinzi; their features syllable by syllable), and the **Universal Shaping Engine** for Tibetan, Javanese, Balinese,
  Tai Tham, Sinhala, New Tai Lue and the other Brahmic scripts (clusters by its grammar, a dotted circle on a broken
  one, the repha and pre-base glyphs moved, split vowels decomposed). Marks lose their advances as HarfBuzz's shapers
  have it (before the positioning in Myanmar and the USE scripts, never in Indic, Khmer and Hangul). 1,373 of 1,376
  random syllables of nine scripts shape as HarfBuzz shapes them (`tools/compareShaping.py`), and Unicode's
  text-rendering-tests run in CI (`tools/textRenderingTests.py`, the cases still failing listed with why).
- Fixed: when a lookup a contextual one calls adds glyphs (a multiple substitution), the lookups it calls next see
  them, by index, as HarfBuzz has it (they skipped past them, or were dropped).

- **Arabic shaped by the font**: each letter takes the form Unicode's joining asks for (every Arabic letter, not 42 of
  them: Persian, Urdu, Kurdish...), made by the font's `isol`, `init`, `medi` and `fina` in their stages, then `rlig`
  (lam alef) and the rest, as the OpenType Arabic model says; marks are passed over. A font without those features
  gets the Unicode presentation forms it carries, as before. `cscdArabicJoin` joins by the same rules.

- **Grapheme clusters by UAX #29** (Unicode 16.0): in a text field the caret moves over, and Backspace and Delete take,
  a character as a reader sees it: a letter and its accents, an emoji sequence, a flag, an Indic conjunct. The
  conformance file passes whole (1,093 lines; CI downloads it, `CASCADE_GRAPHEME_TEST`).
- The image decoders share one size limit (`cscdImageMaxPixels`, internal; 100 million pixels, as before); the fuzzers
  lower it, where a valid image of tens of millions of pixels only took time (a JPEG timed out under the sanitizers).

- **Line breaking by UAX #14** (Unicode 16.0): lines break where Unicode's rules let them, across the elements of a
  line (`foo<b>bar</b>` is one word), no line starts with a closing mark or a small kana, a URL breaks after its
  slashes, numbers and their signs stay together; soft hyphens, `hyphens: none` and `word-break: break-all` as before.
  The Unicode conformance file passes whole (16,666 lines; CI downloads it, `CASCADE_LINEBREAK_TEST`).
- Fixed: the stack reserve under sanitizers (ThreadSanitizer's larger frames overflowed a 512 KB thread).

- **Places in layout are doubles** (`CscdLayoutBox.x`/`y`, baselines, text fragments, lines; `inlineFrags[].rect` and
  `overflow` are a `CscdBoxRect` of doubles, `cscdBoxRectF` gives it as floats); sizes stay floats. A box is moved
  instead of laid out again wherever it is on the page, and the layout made from the last one is the one made from
  scratch to the bit past 2^17 px too. `cscdBoxShift` takes doubles. 20,000 rows, a row taller at the top: ~76 → ~28 ms.
- **A change that only changes how things look** (a class, an attribute) is restyled in place, without a layout.
- **Animations paint only what they change**: the window was painted whole every frame of any animation. An opacity
  animation draws back an image of what its element holds, kept from frame to frame (same pixels). Values an animation
  sets that are inherited (color, font size) are inherited every frame.
- **SSE2 / NEON** for the shadow blur, scaled images and radial, linear and conic gradients.
- **Lengths past what floats hold** (1e39px) lay out within ±2^25 px: they made NaN places (a field's text drawn at NaN).
- The style recursion stops before the thread's stack runs out (a 512 KB thread with 1024 levels: tested).
- Fixed: the frame moved on scroll while something turned was in it (its device places round otherwise: a pixel a level
  off); the key name of a key released on Wayland could be read before it was written; 4 MSVC warnings, and warnings
  are errors on every CI platform.
- **Layout units.** Every place and size layout keeps is on the 1/64 px grid (`laySnap`), as browsers' LayoutUnit: a
  box measured at one place and put at another, or kept from the last layout and moved, is exactly where laying it out
  there puts it. Moved rows are shifted, not laid out again (1,000 rows below a change: 4 ms → 0.5); the oracle compares
  geometry to the bit. Pixel references moved by at most 4 levels on a few edges.
- **`:has()` takes relative selectors** (`:has(+ .x)`, `:has(~ .x .y)`, `:has(> .a, + .b)`), matched from the element
  rather than anywhere in the page; a change restyles the elements whose `:has()` could see it, siblings before it
  included, inside `:is()`/`:not()` too (`cscdStyleEngineHasReach(se, e, asSibling)`, `cscdStyleEngineHasSiblings`).
- **Opaque scrollers scroll by moving their last frame** inside their padding box, when nothing else is drawn over it.
- **SSE2 / NEON** for glyphs, color glyphs and the edges of filled shapes.
- Fixed: a few ulps of drift between a box laid out in place and the same box measured elsewhere and moved, which showed
  as an edge a level off at 1.5x.

- **Long lists paint what shows.** The children of a box with many are found between two binary searches over their
  inks, and whether a subtree holds a positioned box or a stacking context is remembered on its boxes (kept with them
  across layouts): a full paint of 20,000 rows 11.5 ms → 1.2, its hit areas 7.8 → 0.02. Layout no longer clamps the
  scrolling of the boxes it kept. A class on one row: update 14 ms → 6, paint 4 → 0.1.
- **Inserts and removals paint only what came and went** (`CscdLayoutBox.newIn`, `followedIn`), not the whole window.
- **`:has()` without a full restyle**: a change restyles the ancestors that match a compound holding a `:has()` of
  descendants or children (`cscdStyleEngineHasReach`, `CSCD_REACH_HAS`). With a `:has()` rule on 20,000 rows, a class
  on one: 61 ms → 6. A `:has()` of siblings, or inside `:is()`/`:not()`, still restyles everything.
- **SSE2 / NEON** for translucent fills and layers drawn back; the blur filter blurs the four channels at once. The
  same bits as the scalar code (`cscdPaintSimdSelfCheck`, run by the tests).
- **The page scrolls by moving the last frame** and painting what comes into view and the scrollbar, when it moved by
  whole device pixels and nothing in it is fixed, sticky or reads what is behind it.
- **Moved boxes are shifted when that is exact**: a box kept from the last layout at another place, when every place
  along the shift is on the 1/64 px grid (`shiftsExactly`); otherwise laid out again as before.
- Fixed: a flex or grid item kept and measured elsewhere recorded what it drew at the wrong place (its old place was not
  repainted); a progress value set in the same frame as an insert was not repainted; a menu laying out the page in the
  middle of a click remade the hit areas under it.
- The oracle: `:has()` rules reached by inserts and attributes, an anchor that is not the subject, an opacity hover, and
  half its pages without the sticky header (so that scrolling moves the frame). Planted bugs in each new path fail it.

- **Structural changes restyle only what the rules see.** Inserts, removals and an element's own changes (an attribute
  layout reads, a value, a check) are recorded node by node (`CscdDocument.nodeChanges`): the new subtree is styled,
  the parent's children only when a rule sees places among siblings (`+ ~ :nth-* :first-child`), the parent alone when
  an `:empty` rule could match it (`cscdStyleEngineEmptyMatters`), the whole page only with `:has()`
  (`cscdStyleEngineStructureReach`). A row appended to a list of 20,000: 40 ms of styles → 0.
- **Plain blocks are memoized too**: a block without a formatting context of its own, when no float is around it and
  none comes out of it. 20,000 paragraphs, a class on one: 148 ms of layout → 8.
- **The damage comes from what layout touched.** Each box layout makes or lays out again carries what its element drew
  before (`prevInk`, `prevLook`); a box kept where it was is passed over with all it holds, and one a flex or grid
  parent shifted after its memo is damaged where it was and is. The painter's culling table is kept across layouts the
  same way. A class on one of 20,000 rows: the whole update 87 ms → 15 (it compared every element before and after).
- Fixed: the page's scrollbar was not repainted when a light path changed the content's height; a box kept from the last
  layout and laid out again started from its old geometry (an inline box with no text was shifted twice); changes
  recorded for an element freed without being taken out of the document first (a closed menu, a cut-off subtree, also
  attribute changes) were read afterwards.
- The oracle runs three versions of its page (with `:has()`, with rules on places among siblings, with neither), with
  floats, plain paragraphs, inserts and `hidden`; planted under-invalidations in each new path were caught.

- **Incremental layout.** Each change stamps its element and their ancestors with the layout epoch
  (`CscdNode.changedAt`, `cscdNodeTouch`), and a style that is not the one it had stamps its element too. The next
  layout (`cscdLayoutDocumentFrom`) takes from the last one the boxes of the block, flex and grid elements with nothing
  new in them, and the memo of one with its own formatting context, given the same room at the same place, skips its
  layout. On a list of 20,000 rows: a class on one row lays out in 7.4 ms instead of 158, a row's text in 7.6 instead
  of 156, a row appended in 7.2 instead of 159.
- **Styles restyled as far as the rules reach.** Class, id and attribute names are gathered from the selectors as the
  index is built, with how far a change of each reaches (the element, its descendants, its following siblings, or the
  whole page for `:has()`); a change restyles that and no more, and nothing when no rule names it. A class on one of
  80,000 elements: 0.1 ms of styles instead of 39 (`cscdStyleEngineReach`).
- **The pointer coming onto the page** no longer restyles and lays out everything: a `:hover` change matters only to an
  element some rule's `:hover` compound could match (`cscdStyleEngineStateMatters`), and the root's never does for a
  page's `.row:hover`. From about 235 ms to 3.4 on that list.
- The oracle of the light paths (`cascade-incremental-tests`) has rules that reach descendants, siblings and `:has()`, and
  data- attributes; four kinds of under-invalidation planted in the reach were each caught.
- Fixed: the image cache could drop the image it was handing out when over its limit and the last layout still held the
  others; building the boxes now stops, as layout does, where the thread's stack runs low.

- The stack check reads where the frame really is (`__builtin_frame_address`, `_AddressOfReturnAddress`), and judges
  only an address inside the thread's stack: under AddressSanitizer, whose use-after-return checks keep locals in a
  "fake stack" on the heap, every box was laid out empty on Linux, and the check never stopped a deep page on Windows.
- A hover that only changes colors keeps what painting knows of the rest of the page: on a list of 20,000 rows, moving
  from one row to the next painted in 5.5 ms instead of 27.5 (what each box draws was worked out again for all of them).
- `cascade-memory-tests` waits for the paint threads' own glyph caches to fill (which thread paints which rows changes
  from frame to frame) instead of failing on them now and then: the bytes must come to rest within 40 lives.

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
