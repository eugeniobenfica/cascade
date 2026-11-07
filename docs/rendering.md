# Rendering and performance

## The pipeline

```
HTML ──► DOM ──► cascade ──► layout ──► paint ──► present
          ▲      (styles)    (boxes)    (pixels)   (Vulkan / OpenGL / software)
          │
   your code: events, cscdElementSetText, cscdElementSetAttribute, painters
```

1. **Parse** — the HTML becomes the DOM; style sheets are parsed and indexed.
2. **Cascade** — each element gets its computed style (`e->style`, shared between elements with the same rules).
3. **Layout** — a tree of boxes: block, inline, flex, grid and table formatting.
4. **Paint** — the boxes are drawn by cascade's rasterizer (exact-area anti-aliasing) into an
   image of premultiplied BGRA pixels; painters draw their elements' content. A frame with enough
   to paint is first recorded, then drawn by bands of rows on several threads (below).
5. **Present** — the window puts the image on screen through Vulkan, OpenGL or the system's
   software path. The image is the same whichever presents it.

`cscdViewUpdate` brings styles and layout up to date and runs timers and animations; `cscdViewPaint`
paints. A window does both whenever something asks to be repainted.

## What repaints when

A window keeps its last frame and paints again only what changed, when that is known exactly:

| Change | Recomputed | Painted again |
| --- | --- | --- |
| `cscdViewInvalidateElement(view, element)` — a painter draws something new | nothing | that element's box |
| `setText` replacing one text with another | layout | that text's block, and the few boxes that moved |
| `:hover` / `:active` changes that only repaint (colors, backgrounds, shadows, outlines) | the styles of those elements and their descendants; no layout | those elements |
| `:hover` / `:active` changes that change sizes | the styles of those elements and their descendants, and the layout | those elements, and the few boxes that moved |
| A box scrolled | nothing | that box |
| Anything else — classes, attributes, inline styles, new elements | all styles and layout | the whole window |

When more than a handful of boxes move, or the areas cover most of the window, the whole window
is painted: that is quicker than many small areas.

## What is kept between frames

Most of a page is the same from one frame to the next, so cascade keeps what it worked out:

- **Computed styles are shared.** Elements that match the same rules, with the same inline
  style and the same parent style, share one computed style, and the styles are kept from one
  restyle to the next: a class toggled on `<body>` recomputes only the elements whose rules
  changed, and a list of a thousand rows computes a row's style once. A style that uses `vw` or
  `vh` is computed again when the viewport size changes; the others are not.
- **Resizing only lays out again**, unless the new size changes which `@media` rules (or
  `<style media>` / `<link media>` sheets) apply, or a style used viewport units.
- **Values are parsed once.** A declaration's value is tokenized, and a shorthand expanded into
  its longhands, once per text, however many styles its rule takes part in.
- **The system fonts are indexed once.** The first program to start reads every installed
  font's names (on several threads) and keeps them in a small index (`%LOCALAPPDATA%\cascade`,
  `~/Library/Caches/cascade`, or `$XDG_CACHE_HOME/cascade`); later starts read the index and only
  the font files that changed. Set `CASCADE_NO_FONT_CACHE=1` to scan the folders every time.
- **Font files are mapped, not read**: only the parts used are loaded (a 20 MB CJK fallback
  font costs nothing until a character needs it), and kerning pairs are searched where they are.
- **Text widths are remembered** across layouts, per font and size, and a computed style
  remembers the face it was matched to.
- **Inline layout reuses its working memory**: once warm, laying out a paragraph allocates
  almost nothing.
- **Only what can show is drawn**: text outside the clip is not shaped, and an opaque page
  background is the only fill under the page.
- **Layers are reused.** The images that translucent boxes (opacity, filters, blend modes) are
  painted into are kept and cleared only where they were drawn on.
- **Shadows and glyphs are kept**, for the whole process and within limits of yours (below).

### The paint caches

Blurring a shadow is the costliest thing a paint can do: the one of a large dialog takes a fifth of
a second. So each blurred shape is kept, and a glyph's picture too. Full, a cache drops only what
went longest without use, and never everything at once: a shadow counts as used later the larger
it is, so that the small ones a list makes as it scrolls (a new one at each fraction of a pixel)
replace each other and leave the large one in place. A small shadow with no blur is not kept at
all, having nothing costly in it.

```c
CscdPaintCacheLimits limits = cscdPaintCacheLimits();
limits.shadowBytes = 32 << 20;           /* a device with little memory */
cscdPaintCacheSetLimits(limits);

CscdPaintCacheStats s = cscdPaintCacheStats();
printf("%zu shadows, %zu bytes, %llu blurred\n", s.shadowCount, s.shadowBytes, (unsigned long long)s.shadowMisses);
```

The defaults (128 MB and 1024 shadows, 8192 glyphs) suit an editor. `shadowBytes` or `shadowCount` 0
keeps no shadow: the pixels are the same, each frame blurs again. `cscdPaintCacheClear` drops what
is kept, for a program going to the background. A frame that stalls with `shadowMisses` climbing
while nothing new shows means shadows are being blurred over and over: raise the limits, or give
the shape fewer sizes. The handle API has the same calls (`cscd_paint_cache_set_limits`,
`cscd_paint_cache_get_stats`, `cscd_paint_cache_clear`).

## Keeping frames fast

**Measure first.** `cscdViewFrameStats(view)` tells what the last style pass, layout and paint cost
(in milliseconds), how many styles the style pass computed and how many it found already
computed, and how much it painted. A window shows the same in a small panel in its corner with
the `stats` feature (`cscdViewSetFeature(view, CSCD_FEATURE_STATS, true)`, or `CASCADE_FEATURES=+stats`);
`cascade-render` prints them for the page it renders.

- **Repaint painters with `invalidate(element)`**, not `invalidate()`. A playhead moving every
  frame then costs its own box, not the whole window.
- **Change only what changed.** `setText` and `setAttribute` ignore a value equal to the current
  one, but `setInnerHTML` always rebuilds; in code that runs every frame, compare before you
  rebuild content.
- **Prefer text to attributes for values that tick.** A clock or counter with `setText` stays on
  the light path; an attribute or class changing every frame (a tooltip, `data-state`) makes the
  whole page recompute.
- **Give counters a stable width.** A number whose width changes pushes what follows it; a fixed
  width or `min-width` keeps the change to its own box.
- **Draw heavy content on another thread** and show the result from a painter
  ([Drawing](drawing.md)).
- **Prefer opaque backgrounds** for large areas: an opaque solid fill is a copy of pixels, while
  translucent layers (`opacity` below 1, filters, blend modes) are painted into a layer first and
  blended after.
- **Avoid big rounded clips that change often.** A straight clip costs nothing; a rounded one
  makes a mask of its area.

## The same pixels everywhere

A page paints the same pixels on Windows, Linux and macOS, on x86 and ARM, given the same fonts:
the library is built without fused multiply-adds (`-ffp-contract=off`, `/fp:precise`), and the math
whose results the C libraries round differently (`sin`, `pow`, `exp`, `atan2`...) is cascade's own
(`cscdSinf`, `cscdPowf`...), the same bits everywhere. Text uses the system's fonts, so a page
that names a font only one system has looks different on the others; name the font with
`@font-face` where pixels must match.

## The GPU

The frame is painted on the CPU, then presented through Vulkan or OpenGL (or the system's software
path). The GPU keeps the frame from one present to the next, so after a frame that only part of the
page changed (a hover, a caret, a painter invalidated alone) only the rectangles that changed are
uploaded: `cscdViewPaintDamage` names them, and a host with its own GPU surface can do the same.
What is shown is pixel for pixel what a full repaint gives. `CSCD_FEATURE_GPU` (or
`CASCADE_FEATURES=-gpu`) switches a window to the software path.

The GPU does not composite layers (transforms or opacity animated on the GPU, without painting
again), on purpose: a GPU samples, blends and rounds in its own way, which differs from one driver
to the next, and the same page would then show other pixels with Vulkan, OpenGL and the software
path, and on each machine. Every frame is the CPU's, the same everywhere (`-ffp-contract=off` keeps
even the arithmetic the same across compilers); the GPU only shows it. [Rasterizing on the
GPU](gpuRasterization.md) studies how the GPU could paint too, with integer arithmetic that every
driver must compute alike.

## Under the page: a scene of the app's

An app can draw in the window itself and show the page over it, as a 3D engine does with its scene
and its interface. Two things make that possible, and cascade knows nothing of what is drawn:

- `CscdWindowOptions.presenter` gives the window a function that makes its presenter. It receives
  what `cscdPresenterNew` takes (the `HWND`, the `NSView*`, the `ANativeWindow*`...) and the size,
  and the app creates its swapchain there. The window keeps everything else: input, input methods,
  the clipboard, screen readers, the system's scale. It asks again whenever it needs a new presenter:
  Android gives a new surface after the app comes back, and a presenter whose `present` returns
  `false` is freed and made again.
- `cscdViewSetTransparent` leaves out the canvas under the page: what the page leaves empty is
  transparent in the frame, and what is translucent stays so, in premultiplied alpha.

```c
static bool present(CscdPresenter* p, const CscdImage* frame) {
    drawScene();
    /* only p->damage, when p->partial: the rest of the texture is still right */
    uploadToTexture(frame, p->partial, p->damage, p->damageCount);
    drawOverScene();    /* blending ONE, ONE_MINUS_SRC_ALPHA */
    swapBuffers();
    return true;        /* false: the device is lost */
}
```

`cscdWindowOnFrame` runs before each frame, and with it set the window keeps drawing. To tell a
click on the interface from one on the scene, give the scene an element of its own, and ask
`cscdViewElementAt` or listen to its events; `cscdViewElementRect` says where it is.

The page's pixels stay the CPU's, the same everywhere, wherever they are opaque. Translucent ones
are blended over the scene by the GPU, which rounds in its own way. `filter` and `backdrop-filter`
work on what the page painted, so they do not see the scene.

## Painting on several threads

`cscdViewPaint` records the frame into a `CscdDisplayList` (the painters and the hit areas run once,
then), and the list is drawn by horizontal bands, a few for each thread, by the library's workers.
The result is the same pixels as painting on one thread: the rasterizer works each row out from
the shapes alone, never from where the band starts. What a band needs beyond its rows is made
once for all of them: a shadow that crosses several bands is blurred once (whatever its size),
and a picture drawn much smaller is reduced once; the bands that need it while it is being made
wait for it asleep. Two things make a frame paint on one thread: a blurred layer
(`filter: blur()`) or a blurred backdrop (`backdrop-filter: blur()`), which read the pixels around
each one; and a frame with little to paint (a hover, a caret), where starting threads costs more
than it saves.

`cscdSetPaintThreads(n)` sets how many threads (0, the default: as many as the process may use, at
most 8; 1: no threads), and so does `CASCADE_PAINT_THREADS` in the environment. On Linux, "may use"
is the process's affinity and its container's limit, not the machine's count.

A full repaint with the caches warm, on four cores:

| Page | 1920×1080 | 3840×2160 (2x) |
| --- | ---: | ---: |
| The gallery | 1.9 ms (3.1 on one thread) | 4.5 ms (9.4) |
| `docs/assets/src/hero.html`: large shadows, pictures drawn smaller | 23 ms (53) | 46 ms (178) |

The workers are the same threads for every loop the library runs in parallel (the bands, the first
scan of the system's fonts), named `cascade worker` for debuggers and profilers. A loop asked for
while another runs, from another thread or from inside one, runs on its own thread instead of
waiting: two windows painting at once never wait for each other.

## Hit testing

Clicks and hovers find their element from the hit areas recorded while painting, so they match
exactly what is drawn — transforms, clips and stacking included. After a partial repaint that
changed the layout, they are recorded again without painting.
