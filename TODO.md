# To do

What has been done to make cascade stable and fast on large pages, and what is left, in the order it pays off.
Numbers are from a list of 20,000 rows (80,000 elements) at 1280×800, four paint threads.

## Done

### Safety net (`bfd6078`, `33f0e48`, `1e2edc0`)

- [x] The program's allocator and an out-of-memory handler (`cscdSetAllocator`, `cscdSetOutOfMemoryHandler`).
- [x] `cascade-memory-tests`: every block counted (nothing grows across page lives) and failures injected across a frame.
- [x] `CSCD_MAX_TREE_DEPTH` (1024) and loops refused by `cscdNodeAppendChild` / `cscdNodeInsertBefore`.
- [x] Stack guard: building the boxes, layout and painting stop descending when the thread's stack runs low
      (`cscdStackLow`, on the real frame address, so it holds under AddressSanitizer too).
- [x] `cascade-incremental-tests`: the oracle. Every incremental frame against the whole pipeline from scratch, boxes
      and pixels; rules that reach descendants, siblings and `:has()`; planted under-invalidations are caught.
- [x] CI: ThreadSanitizer, `-Wextra`, warnings as errors on MinGW (`CASCADE_WERROR`).
- [x] Nightly fuzz findings fixed and kept in `tests/corpus`: nested `&` selectors doubling (14 GB), a JPEG claiming
      100 megapixels, a shadow at 10^77 px, a glyph at a NaN position.

### Speed

- [x] Style-only attribute changes (`class`, `style`, `id`, `data-*`, `aria-*`, `role`, `title`) repaint only what looks
      different or moved.
- [x] Painting skips boxes whose ink is out of sight (full paint 48.7 → 4.9 ms); a color-only hover updates only its
      boxes' inks (27.5 → 5.5 ms).
- [x] Nine-patch box shadows (hero page cold 40–54 → 20 ms, half the memory).
- [x] `:nth-child` and kin worked out once per list (195 → 3 ms).
- [x] Image cache with a limit (`cscdViewSetImageCacheLimit`); optional decoding on threads (`CSCD_FEATURE_ASYNC_IMAGES`).
- [x] Vulkan uploads only the damaged rectangles (`CASCADE_VULKAN_CHECK=1` verifies by reading back).
- [x] Style invalidation by what the rules reach (`cscdStyleEngineReach`): a class on one row 39 → 0.1 ms of styles.
- [x] The pointer entering the page no longer restyles everything (`cscdStyleEngineStateMatters`): ~235 → 3.4 ms.
- [x] Incremental layout: change stamps (`CscdNode.changedAt`), boxes kept from the last layout
      (`cscdLayoutDocumentFrom`), memo across layouts when a box stays in place: 158 → 7.4 ms.
- [x] Structural changes without a full restyle: inserts, removals and an element's own changes are recorded node by
      node (`CscdDocument.nodeChanges`) and restyle the new subtree, the parent's children only when a rule sees
      places among siblings, the parent alone when an `:empty` rule could match it, the page only with `:has()`.
      A row appended to 20,000: 40 → 0 ms of styles.
- [x] Layout of plain blocks: a block without a formatting context of its own is memoized when no float is around it
      and none comes out of it. 20,000 paragraphs, a class on one: 148 → 8 ms of layout.
- [x] Damage from what layout touched: each box made or laid out again carries what its element drew before; boxes
      kept where they were are passed over with their subtree (a flex or grid item shifted after its memo hit counts as
      moved). The culling table is kept across layouts the same way. A change on one of 20,000 rows: 87 → 15 ms of
      update.

- [x] Painting long lists: the children that can show are found by two binary searches over their inks; whether a
      subtree holds layers is remembered on its boxes (`layersAt`), and layout no longer clamps the scrolling of the
      boxes it kept. Full paint of 20,000 rows 11.5 → 1.2 ms, hit areas 7.8 → 0.02 ms; a class on one row: update
      14 → 6 ms, paint 4 → 0.1 ms.
- [x] Structural changes paint only what changed: the boxes that came (`newIn`) and went (the old tree's boxes neither
      taken nor followed, `followedIn`) are damaged where they are or were. A row appended off screen paints nothing.
- [x] `:has()` without a full restyle: the compounds holding a `:has()` of descendants or children are kept
      (`cscdStyleEngineHasReach`); a change walks up to the ancestors that match one. With a `:has()` rule on 20,000
      rows: a class on one 61 → 6 ms, a row appended 51 → 5.5 ms. (`:has()` of siblings or inside `:is()`: all.)
- [x] SIMD: translucent fills and layers drawn back with SSE2 / NEON, the blur of four channels at once; bit-exact
      with the scalar code (`cscdPaintSimdSelfCheck`, in the tests). Solid runs were already stored whole.
- [x] Scrolling the page moves the last frame and paints what comes into view and the scrollbar (whole device pixels,
      nothing fixed, sticky or with a backdrop filter): 6–16% of the window instead of all of it.
- [x] Moved boxes without a relayout, when exact: a kept box at another place is shifted when every place along the
      shift is on the 1/64 px grid under 2^17 px (`shiftsExactly`); else laid out again.
- [x] Layout units: every place and size layout keeps is snapped to 1/64 px (`laySnap`: edges, inputs, sizes, lines,
      fragments, every shift), so a box measured at one place and put at another, or kept and moved, is where a layout
      from nothing puts it, to the bit. The oracle now compares geometry exactly (it allowed 0.01 px, and drift of a few
      ulps showed as a pixel a level off at 1.5x). Rows moved by a change above them: 1,000 rows 4 → 0.5 ms.
- [x] `:has()` takes relative selectors (`:has(+ .x)`, `:has(~ .x .y)`, one combinator each, matched from the element,
      not the whole page), and a change restyles only the elements whose `:has()` could see it: ancestors, and with a
      `:has()` of siblings the siblings before it and before each ancestor; inside `:is()`/`:not()` too.
- [x] An opaque scroller scrolls by moving its last frame inside its padding box (nothing over it, nothing around it
      blending or moving it, nothing in it fixed or sticky): only what comes into view, its scrollbars and its edges are
      painted.
- [x] SIMD for glyphs, color glyphs and the edges of shapes (one color through a row of coverage), bit-exact.

### Fixed on the way

- [x] Damage of a grown shadow, scroll offsets kept in range, scrollbars of resized scrollers, coverage of clipped
      shapes (all found by the oracle).
- [x] The image cache could drop the image it was handing out.
- [x] The page's scrollbar was not repainted when a light path changed the content's height.
- [x] A kept box laid out again started from its old geometry (inline boxes with no text were shifted twice).
- [x] Changes recorded for an element that was freed without being taken out of the document first (a menu, a
      cut-off subtree) were read afterwards.
- [x] A flex or grid item kept from the last layout and measured elsewhere recorded what it drew before at the place it
      was measured: its old place was not repainted.
- [x] A progress bar's value set in the same frame as an insert was not repainted.
- [x] A menu laying out the page in the middle of a click remade the hit areas under the event.

## Next

### Performance

- [x] **Layout in doubles.** Places (`x`, `y`, baselines, fragments, lines, `overflow` as `CscdBoxRect`) are doubles,
      sizes floats; doubles hold the 1/64 px grid exactly far past the ±2^25 px layout units saturate at, so a box is
      moved, not laid out again, wherever it is, and the incremental layout is the one from scratch to the bit on a page
      of any length (it was 1/32 px off past 2^17 px). The oracle puts half a page past 270,000 px for a quarter of its
      seeds. 20,000 rows, a row taller at the top: ~76 → ~28 ms.
- [x] A class or attribute change that only changes how things look is restyled in place, without a layout (20,000
      rows: ~6 → 0.5 ms).
- [x] Rows moved by a change above them: whether a moved box is on the grid is looked at once after it is laid out
      (`CscdLayoutBox.gridState`), and the inks of boxes only moved are moved, not measured again. What is left is
      the move itself, O(rows below) with absolute places (~0.7 µs a row): relative places would make it O(1).
- [x] **Animations paint only what they change.** A step that moves no layout damages where each animated element was
      and is drawn (its transform included), not the window; the window was painted whole every frame of any animation.
      An element animated by its opacity alone keeps what it holds as an image from frame to frame, drawn back with its
      opacity (the same pixels as its layer): 60 paragraphs fading, ~4.3 → ~0.4 ms a frame. Inherited values an
      animation sets (color, font size) are inherited by the elements in it every frame. The oracle animates a third
      of its seeds on a clock of its own.
- [x] A transform animation kept as an image too, without sampling it: an element whose transform animates (and
      nothing else of it but its opacity, nothing in it or around it), as long as the transform only moves and scales
      it, is drawn through an image of its own, what it holds as the transform scales it, placed where the transform
      moves it to the nearest device pixel. A page painted from scratch draws it the same way (the same pixels: the
      promise holds, the oracle says so), so a frame that only moves it places the image again; its hit areas are
      recorded where it is. 24 cards sliding, a frame: ~3.6 → ~0.3 ms (dpr 2).
- [ ] A GPU compositor: the layers that move put together by the GPU (the frame behind them neither painted again nor
      uploaded). Its blending would have to give the CPU's pixels to the bit for the promise to hold (opacity 1 and
      whole-pixel places only, an exact integer shader; OpenGL 1.1's fixed blending does not promise it), with what is
      painted above a moving layer split into layers of its own, and checked on CI's software GPUs. What it would save
      is now small: a frame of moving layers is a fraction of a millisecond on the CPU.
- [x] SIMD for the one-channel shadow blur, scaled images (bilinear), radial, linear and conic gradients, bit-exact
      (self-checked against the scalar code).

### Correctness and robustness

- [x] The NaN in text fields: a length past what floats hold (1e39px) was infinite, and places made from it NaN;
      layout units saturate at ±2^25 px, as browsers'.
- [x] The style recursion stops where the thread's stack runs low, as layout and painting do.
- [x] MSVC C4244 warnings fixed (and a key name read before it was written on Wayland); warnings as errors on MSVC,
      Linux and macOS CI (not yet seen green on CI).
- [x] Back up `CSCD_MAX_TREE_DEPTH` with a test on a 512 KB thread (macOS and iOS secondary threads).

### Text

- [x] Line breaking by UAX #14 (Unicode 16.0's rules, `src/text/lineBreak.c`; the table made by
      `tools/genLineBreak.py`), across the elements of a line; all 16,666 lines of `LineBreakTest.txt` pass, and CI
      downloads it. A page of mixed text lays out ~25% slower, from the opportunities it had missed (after a URL's
      slashes, around punctuation): more pieces to measure.
- [x] Grapheme clusters by UAX #29 (Unicode 16.0, `src/text/grapheme.c`, table by `tools/genGrapheme.py`): the caret,
      Backspace and Delete, touch handles and a field's wrapping step over a character as a reader sees it (accents,
      emoji sequences, flags, Indic conjuncts); all 1,093 lines of `GraphemeBreakTest.txt` pass, CI downloads it.
- [x] Arabic through the font's `isol`/`init`/`medi`/`fina` features in their stages, then `rlig`, by Unicode's joining
      (`Joining_Type` of every code point, `tools/genArabicJoining.py`; `src/text/fontArabic.c`); a font without them
      gets the presentation forms it has. The test font has Arabic glyphs and features, and a second one only
      presentation forms.
- [x] Shapers for Thai and Lao (SARA AM), Hangul jamo (composed into the font's syllables), Khmer, Myanmar and the
      Universal Shaping Engine (`src/text/fontScripts.c`, `fontUse.c`; its categories, decompositions and combining
      classes from the Unicode Indic properties by `tools/genUse.py`): Tibetan, Javanese, Balinese, Tai Tham, Sinhala,
      New Tai Lue and the other Brahmic scripts. `tools/compareShaping.py` shapes random syllables of twelve scripts
      with Windows' fonts by cascade and HarfBuzz: 1,830 of 1,833 the same (Lao: a mark on a mark, 3). Unicode's
      text-rendering-tests run in CI (`tools/textRenderingTests.py`): every Arabic, Balinese, Kannada, Tai Tham and
      cmap case as HarfBuzz shapes it (and the Mac Turkish cmaps HarfBuzz misses); the failures listed in
      `tests/textRenderingTests.known` (Apple's AAT tables, cases where HarfBuzz disagrees with the suite), no others.
- [x] GPOS cursive attachment (lookup type 3), as HarfBuzz joins the glyphs and moves a chain across the line:
      Nasta'liq's letters on their heights (SHARAN-1).
- [x] cmap formats 13 (many characters, one glyph) and 14 (variation sequences: a character and its selector, one
      glyph), and the Macintosh platform's (formats 0 and 6, Mac OS Roman, and Turkish by the subtable's language).
- [x] Kannada and Telugu as HarfBuzz shapes them: their matras before the below-base consonants, a vowel taking a
      consonant after a halant, `pref` forms moved before the base. (Fixed on the way: which consonant is the base was
      tested with the glyphs' masks empty since the last change, so every Indic syllable took its last one.)
- [ ] Lao: a tone mark over another mark, positioned as HarfBuzz does (3 of 153 random texts; HarfBuzz's final
      places do not follow from the mark offsets its lookups give, as far as its trace shows).
- [x] WOFF2 (`src/text/fontWoff2.c`): its Brotli stream (`src/text/brotli.c`, RFC 7932, the static dictionary and
      tables by `tools/genBrotli.py`, each checked against the RFC's CRC-32), glyf, loca and hmtx transforms undone,
      collections. 26 fonts (Latin, CJK, Indic, emoji, CFF) and 3 collections compressed by fontTools read back the
      same glyph for glyph; 600 Brotli streams of every quality and window decode; `@font-face` takes `.woff2`.

### Platform and API

- [x] Opaque public structs: `CscdElement`, `CscdDocument`, `CscdEvent`, `CscdLayoutBox` and `CscdWindowOptions`
      show their fields only where `CASCADE_INTERNAL` is defined (the library, its tests and tools); programs use
      functions (`cscdEventCurrentTarget`, `cscdEventKey`, `cscdElementNode`, `cscdWindowOptionsNew` and its setters...).
      `CASCADE_SHARED` builds the engine as a shared library, and CI's ABI job compares what `include/cascade`
      declares in it with `tests/abi/cascade.abi` (libabigail): a removal or a change fails, an addition passes. The
      next release is 2.0.0.
- [ ] Menus, tooltips and `<select>` lists as real windows (today clipped to the window).
- [ ] IME on Wayland (`text-input-v3`), accelerators, drag out to other apps, rich clipboard formats.
- [ ] UI Automation TextPattern and live regions; push accessibility updates per element instead of the whole tree.
- [ ] `contenteditable` / rich text, and a text field that scales to large documents.

### Conformance

- [x] Layout fixtures compared with Chrome (Taffy's gentest approach): `tools/layoutFixtures.py` lays out 58 pages of
      boxes (block, positioning, flex, grid) in headless Edge and writes where each element went into
      `tests/layoutFixtures.inc`; cascade-tests lays them out the same, every element within half a pixel. On the way:
      a flex item's automatic minimum took its own width (it did not shrink), a relative offset moved what followed it,
      `grid-auto-flow: dense` did not go back for holes, `minmax(x, auto)` rows did not grow to their contents.
- [x] A JS-free runner for WPT reftests in `css/` (`tests/wpt.c`): ten folders of a pinned commit (flexbox, grid,
      CSS2 margins, positioning, normal flow, floats, backgrounds), each test and its reference painted at 800x600 and
      compared within the test's fuzzy allowance; pages with scripts passed over, XHTML's self-closed elements and
      CDATA read as XML would. The score of each folder is in `tests/wpt.score` (2,054 of 3,307 run, from 846: a
      line of text is now whole pixels high, its font's ascent and descent rounded as browsers round them); CI fails
      when a folder passes fewer.
- [x] html5lib tree-construction tests (`tests/html5lib.c`: 857 of 1,645, the rest in `tests/html5lib.known`),
      css-parsing-tests (`tools/cssParsingTests.py`: 2,645 of 2,684; fixed on the way: lab() and lch() as CIE Lab,
      An+B by CSS Syntax, `3n - 1` was 3n+1, declarations named by an identifier only, `color(srgb ...)`), BidiTest
      and BidiCharacterTest (every one of their 861,948 cases; fixed: sos and eos from the levels X1 to X8 give, not
      from those already resolved). CI runs each on a pinned download.
- [ ] HTML's tree builder as the specification has it (the adoption agency, foster parenting, the insertion modes of
      template, frameset, select and foreign content, fragments in their context): 788 html5lib cases fail.
- [ ] The WPT reftests that still fail, folder by folder (`cascade-wpt-tests` with `CASCADE_WPT_LIST=1` lists them,
      `CASCADE_WPT_DUMP=1` saves the first's two pictures).

## How to check a change

- `ctest` (cascade-tests, -capi-tests, -stress, -memory-tests, -incremental-tests, -window-tests, -html5lib-tests,
  -wpt-tests; the conformance suites when their environment variables name them: CASCADE_LINEBREAK_TEST,
  CASCADE_GRAPHEME_TEST, CASCADE_BIDI_TEST, CASCADE_BIDI_CHARACTER_TEST, CASCADE_HTML5LIB_TESTS, CASCADE_WPT).
- `cascade-incremental-tests 1 300 300` for anything that touches restyle, layout or damage; plant the bug you fear
  and see it fail before trusting a pass.
- An AddressSanitizer build (`-DCASCADE_SANITIZE=address`; on Windows with clang64) with
  `ASAN_OPTIONS=detect_stack_use_after_return=1`.
- The fuzz runners over `tests/corpus` (`-DCASCADE_FUZZ=ON`).
