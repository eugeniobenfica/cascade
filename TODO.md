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

- [ ] **Fixed-point layout units.** Moved boxes are shifted only when nothing rounds; text baselines are rarely on the
      1/64 px grid and long pages pass 2^17 px, so a change above 20,000 rows still lays out every row below (75 ms)
      and repaints the window. Layout in 1/64 px integers (as browsers' LayoutUnit) makes every shift exact; it changes
      the pixel references.
- [ ] **The last 6 ms per change.** Building and laying out still visit every child of a list whose one child changed
      (20,000 cheap checks: 1.5 + 2 ms), and so does the damage walk. Splice the old children vector in place, and find
      the dirty children through the change stamps.
- [ ] **`:has()` of siblings** (`:has(+ .x)`) and inside `:is()`/`:not()`: still a full restyle.
- [ ] **Retained tiles for scrollers and animations.** The page scrolls by moving the last frame; scrollable boxes and
      transform/opacity animations still repaint their area each frame (tiles, then an integer GPU compositor).
- [ ] **More SIMD**: glyph and image blending (per-pixel lookups), the shadow blur, gradients.

### Correctness and robustness

- [ ] Find where the NaN in text fields comes from (seen only with the DejaVu fonts on Linux CI; glyphs are skipped).
- [ ] Guard the style recursion against the stack as layout and painting are (only threads under ~256 KB need it).
- [ ] MSVC C4244 warnings (3), then warnings as errors on MSVC and Linux too.
- [ ] Back up `CSCD_MAX_TREE_DEPTH` with a test on a 512 KB thread (macOS and iOS secondary threads).

### Text

- [ ] Line breaking by UAX #14 (today: spaces, hyphens and CJK), with `LineBreakTest.txt` in CI.
- [ ] Grapheme clusters by UAX #29 for the caret and deletion (today: code points), with `GraphemeBreakTest.txt`.
- [ ] Arabic through the font's `init`/`medi`/`fina` features instead of the presentation-forms table.
- [ ] Shapers for Thai, Khmer, Myanmar, Tibetan, Hangul jamo and the Universal Shaping Engine; run
      unicode-org/text-rendering-tests.
- [ ] WOFF2.

### Platform and API

- [ ] Opaque or versioned public structs (`CscdElement`, `CscdEvent`, `CscdWindowOptions`, `CscdLayoutBox`,
      `CscdDocument`), a shared build of the native API, and an ABI check in CI.
- [ ] Menus, tooltips and `<select>` lists as real windows (today clipped to the window).
- [ ] IME on Wayland (`text-input-v3`), accelerators, drag out to other apps, rich clipboard formats.
- [ ] UI Automation TextPattern and live regions; push accessibility updates per element instead of the whole tree.
- [ ] `contenteditable` / rich text, and a text field that scales to large documents.

### Conformance

- [ ] Layout fixtures compared with Chrome (Taffy's gentest approach).
- [ ] A JS-free runner for WPT reftests in `css/`, scored per release.
- [ ] html5lib tree-construction tests, css-parsing-tests, BidiTest / BidiCharacterTest.

## How to check a change

- `ctest` (cascade-tests, -capi-tests, -stress, -memory-tests, -incremental-tests, -window-tests).
- `cascade-incremental-tests 1 300 300` for anything that touches restyle, layout or damage; plant the bug you fear
  and see it fail before trusting a pass.
- An AddressSanitizer build (`-DCASCADE_SANITIZE=address`; on Windows with clang64) with
  `ASAN_OPTIONS=detect_stack_use_after_return=1`.
- The fuzz runners over `tests/corpus` (`-DCASCADE_FUZZ=ON`).
