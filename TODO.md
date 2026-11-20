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

### Fixed on the way

- [x] Damage of a grown shadow, scroll offsets kept in range, scrollbars of resized scrollers, coverage of clipped
      shapes (all found by the oracle).
- [x] The image cache could drop the image it was handing out.
- [x] The page's scrollbar was not repainted when a light path changed the content's height.
- [x] A kept box laid out again started from its old geometry (inline boxes with no text were shifted twice).
- [x] Changes recorded for an element that was freed without being taken out of the document first (a menu, a
      cut-off subtree) were read afterwards.

## Next

### Performance

- [ ] **Structural changes paint only what changed.** A row appended or removed still repaints the whole window (29 ms
      of paint); the damage walk could take boxes that came and went, not only boxes that changed.
- [ ] **The last 7 ms per change.** Building and laying out still visit every child of a list whose one child changed
      (20,000 cheap checks), and so do the damage walk and the hit areas. Splice the old children vector in place, and
      find the dirty children through the change stamps instead of visiting them all.
- [ ] **`:has()` without a full restyle.** Pages with `:has()` still restyle everything on a structural change; walk
      up to the ancestors whose `:has()` could see it (as Blink's invalidation does).
- [ ] **Moved boxes without a relayout.** A change above the rows still lays out every row below (68 ms). Fixed-point
      layout units (as browsers' 1/64 px) would make shifting exact; or lay out at the same relative place and offset.
- [ ] **Painting long lists.** Culling still tests every row; in normal flow, find the first and last visible child by
      their position.
- [ ] **Retained tiles.** Scrolling and transform/opacity animations repaint their area on the CPU each frame; keep
      painted tiles and translate them (CPU blit first, an integer GPU compositor later).
- [ ] **SIMD in the rasterizer** (integer SSE2/NEON, bit-exact with the scalar code) and solid-span fast paths.

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
