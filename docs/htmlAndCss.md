# HTML and CSS

cascade follows the web's specifications where it implements them, so pages are written the
way they would be for a browser. This page lists what is there, and what is not yet.

## HTML

An HTML5 tokenizer and tree builder: implied `<html>`, `<head>` and `<body>`, optional end tags
(`p`, `li`, `td`, `option`…), void and raw-text elements, character references, implied
`<tbody>`. `<link rel="stylesheet">`, `<style>`, `<title>`, `<img>`, inline `<svg>`, `<canvas>`,
form controls, `<details>`, `<dialog>` and the `popover` attribute all work. A `media` attribute on
`<style>` or `<link>` is evaluated again whenever the view is resized.

One attribute is cascade's own: `data-focus="keyboard"`, on an element with `tabindex`, is reached by
Tab but not by a click, and Esc leaves it, for controls in a program whose keys mean something after
a click (Space to play).

## The DOM

| | |
| --- | --- |
| Finding elements | `getElementById`, `querySelector`, `querySelectorAll`, `closest`, `matches` |
| Attributes | `attr`, `setAttribute`, `removeAttribute`, `hasAttribute` |
| Classes and style | `addClass`, `removeClass`, `toggleClass`, `hasClass`, `setStyle`, `getStyle` |
| Content | `setText`, `setInnerHTML`, `innerHTML`, `outerHTML`, `appendElement` |
| Forms | `value`, `setValue`, `checked`, `setChecked`, `disabled` |
| Geometry | `cscdViewElementRect`, `boundingClientRect`, `scrollTop`, `scrollLeft`, `scrollWidth`, `scrollHeight` |
| Events | `on`, `onClick`, `addEventListener` (capture or bubble), `removeEventListeners`, `click` |

Event handling, the forms and the rest of the interface are in [The interface](interface.md).

## CSS

- **Syntax**: the full tokenizer (CSS Syntax 3), nesting, `@media` (with ranges and
  `prefers-color-scheme`), `@container` (size queries, named containers), `@import`,
  `@supports`, `@font-face` (TrueType, OpenType CFF, WOFF), `@keyframes`.
- **Selectors** (level 4): type, class, id, attribute (all operators), `:is`, `:where`, `:not`,
  `:has`, `:nth-child(An+B)` and its kin, `:hover`, `:active`, `:focus`, `:focus-visible`,
  `:focus-within`, `:checked`, `:disabled`, `:empty`, `::before`, `::after`, `::placeholder`,
  `::marker`, `::selection`; every combinator.
- **The cascade**: origins, specificity, `!important`, inheritance, `inherit` / `initial` /
  `unset`, `var()` custom properties with fallbacks.
- **Generated content**: `::before` / `::after` with strings, `attr()`, quotes, images and CSS
  counters (`counter-reset`, `counter-set`, `counter-increment`, `counter()`, `counters()` in
  any list style).
- **Fonts**: the system's fonts and `@font-face` (TrueType, OpenType, WOFF), `font-family`
  lists with fallback per character, weights, italics, `font-stretch`, and **variable fonts**
  (`fvar`, `avar`, `gvar`, `HVAR`: `font-weight` moves the weight axis, `font-stretch` the width,
  the size the optical size, `font-variation-settings` any axis; `font-optical-sizing`).
- **Shaping**: OpenType layout throughout — GSUB (single, multiple, alternate, ligature,
  contextual, chained and reverse substitutions) and GPOS (kerning, single adjustments, marks on
  bases, ligatures and marks, contextual positioning), with GDEF's classes and the lookup flags.
  `font-feature-settings`, every `font-variant-*` (`-caps` with small capitals made from capitals
  when the font has none, `-numeric`, `-ligatures`, `-position`, `-east-asian`), `font-kerning` and
  `font-synthesis`. Arabic joins; **Indic scripts** (Devanagari, Bengali, Gurmukhi, Gujarati, Oriya,
  Tamil, Telugu, Kannada, Malayalam) are reordered by syllable as the OpenType Indic model asks
  (pre-base matras, the reph, half and below-base forms, two-part vowels); right-to-left text is
  shaped in its reading order. **Color emoji** (COLR layers, CBDT and sbix pictures) with ZWJ
  sequences, skin tones, keycaps and flags (where the system's emoji font has them).
- **Values**: every length unit (including `rem`, `vw`, `vh`, `ch` and the container units
  `cqw`, `cqh`, `cqi`, `cqb`, `cqmin`, `cqmax`), `calc()`, `min()`, `max()`,
  `clamp()`; colors as names, hex, `rgb()`, `hsl()`, `hwb()`, `oklch()` and `color-mix()`.

## Layout

- **Flow**: block formatting with margin collapsing, floats and clearance; inline formatting with
  white-space modes, line breaking, `text-align` (and justify), `vertical-align`, `text-indent`,
  `text-overflow: ellipsis` and `line-clamp`, `text-align-last`; `inline-block`.
- **Flexbox**: direction, wrap, grow and shrink (with min / max freezing), `gap`, `order`, every
  alignment, auto margins.
- **Grid**: `fr`, `minmax`, `repeat(auto-fill / auto-fit)`, named lines and areas, spans,
  auto-placement, alignment, `subgrid` on columns and rows (its items size the parent's tracks).
- **Tables**: `colspan`, `rowspan`, automatic and `table-layout: fixed` column widths,
  `border-collapse`, `caption-side`.
- **Positioning**: `relative`, `absolute`, `fixed`, `sticky`; `z-index` and stacking contexts.
- **Containers**: `container-type` (`inline-size`, `size`, with size containment),
  `container-name`, `container`.
- **Sizing**: `box-sizing`, `aspect-ratio`, min and max sizes, `overflow` with scrolling and
  scrollbars; replaced elements (images, SVG, canvases, form controls).

## Painting

Backgrounds (colors, images, linear / radial / conic and repeating gradients, size, position,
repeat, clip, `background-clip: text`); borders of every style with elliptical radii;
`box-shadow` (blur, spread, inset); `outline`; `opacity`; 2D and 3D `transform` (`perspective`,
`perspective-origin`, `rotateX` / `rotateY` / `rotate3d`, `translateZ`, `matrix3d`,
`backface-visibility`; a `transform-style: preserve-3d` subtree is flattened into its parent); `filter` (blur,
grayscale, brightness, saturate, invert); `backdrop-filter` (the same functions, on what is behind); `mask-image` (gradients and images,
with `mask-size`, `mask-position`, `mask-repeat`); `mix-blend-mode`; `clip-path` basic shapes; text with
kerning (`kern` and GPOS), font fallback, synthetic bold and italic, `text-shadow`,
decorations, `-webkit-text-fill-color` and `-webkit-text-stroke`; `image-rendering: pixelated`;
`isolation`; inline SVG (paths, shapes, `use`, transforms); CSS **transitions** and
**animations**; **bidirectional text** (the Unicode bidi algorithm, UAX #9: `direction` and the
`dir` attribute, Hebrew and Arabic in English and the other way round, numbers, mirrored
brackets, the embedding and isolate controls).

## Limits

A document cannot make cascade crash or run out of memory by its shape, so some things are
bounded, as browsers bound them:

| | Limit | Beyond it |
| --- | --- | --- |
| Element nesting from HTML | 512 levels | deeper elements go side by side at the last level |
| Nested style rules and at-rules | 64 levels | dropped |
| `:is()`, `:not()`, `:where()`, `:has()` inside each other | 32 levels | the selector is invalid |
| Functions and brackets inside a value (`calc(calc(…))`) | 32 levels | the value is invalid |
| A value after `var()` substitution | 512 KiB | invalid at computed-value time |
| Grid lines an item names | 1000 | clamped |
| Roman numerals in list markers | 1 to 3999 | decimal (letters below 1, too) |

Nesting costs nothing extra either. Flex, grid and table containers lay their items out twice
(to measure them, then at the size the line or row gives them); a box that starts a formatting
context of its own remembers its layout for the rest of the pass, and is only moved when it is
laid out again with the same input, or at the very height it found for itself (unless something
inside it depends on its height being definite: percentage heights, column flex containers, grids
with rows of their own). Sixty levels of tables in tables lay out in a few milliseconds.

## Not there yet

cascade is not a browser. Today it has:

- **no JavaScript** — C (or any language that calls it) takes its place;
- **no network loading** — files, `data:` URLs and what the program's loader gives
  (`cscdAppSetResourceLoader`, which may fetch them itself);
- **no automatic hyphenation dictionaries** — `hyphens: auto` breaks at soft hyphens only, as
  `manual` does;
- **vertical text in part** — `writing-mode: vertical-rl`, `sideways-rl` and `sideways-lr` lay a
  box out in its own axes (CJK upright, other text on its side); `vertical-lr` turns the same way,
  so its lines stack from the right; `text-orientation` and `text-combine-upright` are not there;
- **no variable CFF2 outlines** (variable TrueType fonts are), and COLR's version 1 paint graphs
  show the layers of version 0 the fonts also carry;
- **accepted, without effect**: what only means something in print or to a browser's machinery
  (`orphans`, `widows`, the `break-*` properties, `will-change`, `font-display`, `text-rendering`,
  `-webkit-font-smoothing`, `overflow-anchor`, `scrollbar-gutter` with overlay scroll bars,
  `forced-color-adjust`), and `resize`, `box-decoration-break`, `text-justify`,
  `line-break` — a page using them renders, without them.

Also there: `hyphens` (soft hyphens break lines and show a hyphen), `quotes` (nested quotes
by level), `text-emphasis` (and `-style`, `-color`, `-position`), `text-underline-position`,
`zoom`, `shape-outside` (with `shape-margin`; circles, ellipses, insets, polygons and the boxes),
`contain` and `content-visibility`, `unicode-bidi` (embeddings, isolates, overrides, `plaintext`),
`writing-mode`, `empty-cells`, `scrollbar-width`, `overscroll-behavior`, `scroll-behavior` and
`color-scheme`.

## Using styles from the web

Stylesheets written for browsers mostly work as they are, especially CSS-only ones (a compiled
Tailwind build, Pico.css, a theme) since they rely on flexbox, grid, colors and spacing.

1. **Download what the page links** — the CSS, its images and fonts — into your project, and
   link them with relative paths. Links to a CDN do not load.
2. **Fonts.** `@font-face` (or `cscdViewAddFont`) takes TrueType, OpenType and WOFF files. WOFF2,
   what Google Fonts serves to browsers, is not read: download the font's TrueType files, or convert
   it once (`fonttools ttLib.woff2 decompress`).
3. **Behaviour in code.** Components whose behaviour is JavaScript (Bootstrap's dropdowns,
   modals, tabs) show their look but do not act on their own: wire them with events, or use
   cascade's own menus, dialogs and popovers.
4. **Check with `cascade-render`**, which shows at once what renders as in a browser:

   ```bash
   cascade-render page.html out.png 1200
   ```
