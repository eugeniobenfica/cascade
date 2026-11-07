# Drawing

Most of an interface is HTML and CSS. What a page cannot describe — a timeline, a waveform, a
graph, a video viewport — is drawn by C in an element of its own.

## Painters

`cscdElementSetPainter` gives an element a function that draws its content. The page still lays the
element out: CSS decides where it is and how big, and the painter draws inside its content box,
with the origin at its top-left, in CSS pixels, clipped to the box.

```html
<div id="meter" style="height: 120px"></div>
```

```c
static void paintMeter(void* user, CscdCanvas* c, float w, float h) {
    App* app = user;
    cscdCanvasFillRect(c, cscdRect(0, 0, w, h), cscdRgb(18, 17, 24));
    const float level = currentLevel(app);                    /* 0..1 */
    CscdGradientStop stops[] = {{0, cscdRgb(124, 58, 237)}, {1, cscdRgb(232, 213, 255)}};
    CscdShader sh = cscdShaderLinear(cscdPoint(0, h), cscdPoint(0, 0), stops, 2);
    cscdCanvasFillRectShader(c, cscdRect(0, h * (1 - level), w, h * level), &sh);
    cscdShaderFini(&sh);
}

CscdPainter p = {paintMeter, app, NULL};   /* NULL: nothing to free when the painter goes */
cscdElementSetPainter(meter, p);
```

When what the painter draws changes, ask for that element alone to be painted again:

```c
cscdViewInvalidateElement(ui, meter);       /* just its box — not the whole window */
```

`<canvas>` elements take painters the same way.

## The canvas

`CscdCanvas` is a 2D painter with anti-aliasing: paths, fills and strokes, gradients, images, text,
shadows, clipping and layers. `cscdCanvasNew(&image)` makes one on an image; painters are given one.

| | |
| --- | --- |
| State | `cscdCanvasSave`, `Restore`, `Translate`, `Scale`, `Rotate`, `Concat`, `SetMatrix`, `SetGlobalAlpha` |
| Clips | `cscdCanvasClipRect`, `ClipRRect`, `ClipPath` (non-zero or even-odd) |
| Shapes | `cscdCanvasFillRect`, `FillRRect`, `FillRing`, `FillPath`, `StrokePath`, `StrokeRect`, `DrawLine`, `FillCircle` (each with a `...Shader` form) |
| Paint | `cscdShaderSolid`, `cscdShaderLinear`, `cscdShaderRadial`, conic, `cscdShaderImage`; `CscdStrokeStyle` (width, caps, joins, miter limit, dashes) |
| Images | `cscdCanvasDrawImage(c, img, dst, smooth)`, `cscdCanvasDrawImageRect(c, img, src, dst, smooth)` |
| Text | `cscdCanvasDrawText`, `cscdCanvasMeasureText`, `cscdCanvasDrawGlyphs` |
| Effects | `cscdCanvasDrawBoxShadow`, `DrawBlurredPath`, `PushLayer(opacity, blend)` / `PopLayer`, `SetLayerFilter` |

Paths take `cscdPathMoveTo`, `LineTo`, `QuadTo`, `CubicTo`, SVG arcs (`ArcTo`), canvas arcs (`Arc`),
rectangles, rounded rectangles, ellipses and other paths; `cscdParseSvgPath("M10 10 L20 20 …", &p)`
reads SVG path data. A path owns its arrays: `cscdPathFree` it.

```c
CscdPath p = {0};
cscdPathMoveTo(&p, 10, 40);
cscdPathCubicTo(&p, 40, 0, 80, 80, 110, 40);
CscdStrokeStyle st = cscdStrokeStyle(3);
st.cap = CSCD_CAP_ROUND;
cscdVecPush(&st.dash, 6.f);
cscdVecPush(&st.dash, 4.f);
CscdShader sh = cscdShaderSolid(cscdRgb(139, 92, 246));
cscdCanvasStrokePathShader(c, &p, &sh, &st);
cscdStrokeStyleFini(&st);
cscdPathFree(&p);
```

## Images and fonts

`CscdImage` is a block of premultiplied BGRA pixels. `cscdLoadImage` and `cscdDecodeImage` read PNG,
JPEG (baseline and progressive), GIF, WebP (lossless, and lossy with its own VP8 decoder, alpha
included) and BMP into a shared image (`cscdRelease` it); `cscdSavePNG` and `cscdEncodePNG` write
PNG. `cscdImageInit` / `cscdImageFini` make one you hold yourself.

The decoders take data from anywhere: a file that is broken or made to harm (cut short, headers
that claim gigabytes, tables past their end) gives NULL, or as much of the picture as is sound, and
is never read past. `cscdInflateZlibLimit` inflates with a cap on what it makes, for data from
elsewhere (a few kilobytes of DEFLATE can make gigabytes).

Animated GIFs and WebPs play in pages (an `<img>`, a background) as browsers play them: each frame
composed whole (disposal, blending, offsets), for as long as it says, as many times as its loop
count asks (`CSCD_FEATURE_ANIMATIONS` off holds them on their first frame). `cscdDecodeAnimatedImage`
and `cscdLoadAnimatedImage` give the frames themselves: a `CscdAnimatedImage` with each frame's
picture and duration. Lossy WebP decodes to the same pixels libwebp (and so Chrome) shows.

Pages refer to images by URL, as usual. An application can give the page images made in memory —
a thumbnail, a frame — under a URL of its choosing:

```c
cscdViewSetImage(ui, CSCD_SV("app://cover"), coverImage);   /* then <img src="app://cover"> */
```

Fonts come from `@font-face` in CSS, from `cscdViewAddFont(view, family, path, weight, italic)`, or
from the system's fonts, which are found by family name with fallback for missing characters.

## Recording drawing

A `CscdDisplayList` keeps drawing to do later. A canvas made on one records instead of drawing (and
keeps its matrix and clip as it goes, so painters work as usual); `cscdDisplayListReplay` draws it
into a canvas, and `cscdDisplayListReplayInto` into an image by bands of rows on several threads,
with the same pixels as drawing directly.

```c
CscdDisplayList* list = cscdDisplayListNew();
CscdCanvas* rec = cscdCanvasNewRecording(&image, list); /* image gives the size; its pixels are not touched */
drawScene(rec);
cscdCanvasFree(rec);
cscdDisplayListReplayInto(list, &image, 0);           /* 0: on as many threads as the machine has */
cscdDisplayListFree(list);
```

Images given to a recording canvas with `cscdCanvasDrawSharedImage` are kept as they are (a
reference); the other calls copy what they draw. `cscdCanvasNewRegion(&image, region)` draws only
inside a region of the image.

## Bringing in another renderer

Anything that can draw into a block of pixels fits in: draw into a `CscdImage`, then show it from a
painter. The pixels are premultiplied BGRA, so Skia, for example, draws into them directly:

```c
CscdImage* frame = cscdImageNew(w, h);
/* Skia (C++): SkSurfaces::WrapPixels(SkImageInfo::Make(w, h, kBGRA_8888_SkColorType,
 * kPremul_SkAlphaType), frame->pixels, w * 4) */
drawScene(frame);

static void paintViewport(void* user, CscdCanvas* c, float w, float h) {
    CscdImage* frame = user;
    cscdCanvasDrawSharedImage(c, frame, cscdRect(0, 0, frame->width, frame->height), cscdRect(0, 0, w, h), true);
}
CscdPainter p = {paintViewport, frame, cscdRelease};   /* the painter keeps the frame */
cscdElementSetPainter(viewport, p);
```

Heavy work — a 3D scene, decoded video, a composition — is best drawn on a thread of its own: it
fills a `CscdImage`, hands it to the interface's thread with `cscdWindowPost`, and the painter shows
the newest one, followed by `cscdViewInvalidateElement(ui, element)`. A viewport can draw on the
graphics card that way while the interface stays responsive.

A picture only scaled (no rotation) is drawn on a fast path, so showing a frame rendered at half
size and enlarged costs little.
