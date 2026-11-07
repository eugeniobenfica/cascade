<p align="center"><img src="assets/logo.svg" width="72" alt="cascade"></p>

# Porting a Qt / Skia application to cascade

cascade replaces three things in a Qt Widgets + Skia app: the widget tree (QWidget and layouts),
the styling (QStyle, QSS, palettes) and the 2D drawing (QPainter / SkCanvas). The application
logic stays where it is: cascade's API is C, so C++ (or any language that calls C) uses it
directly.

## Concepts side by side

| Qt / Skia | cascade |
| --- | --- |
| `QApplication` + `QMainWindow` | `CscdWindow` (owns a `CscdView`) |
| `.ui` files, widget constructors | an HTML file (`cscdViewLoadFile`) or DOM calls (`cscdElementAppendElement`) |
| `QHBoxLayout` / `QVBoxLayout` | `display: flex` with `flex-direction` |
| `QGridLayout` | `display: grid` |
| `QStackedWidget`, `QTabWidget` | toggle a `.hidden { display: none }` class |
| `QScrollArea` | `overflow: auto` |
| `QSplitter` | an element with the `splitter` attribute between flex items |
| `QSS`, `QPalette`, `QProxyStyle` | CSS, custom properties (`--accent`), `:hover`, `:checked`… |
| signals / slots (`clicked`, `valueChanged`) | `cscdElementOnClick`, `cscdElementOn(e, "input")`, `cscdElementOnChange` |
| `QLineEdit`, `QTextEdit` | `<input type="text">`, `<textarea>` |
| `QCheckBox`, `QRadioButton`, `QSlider`, `QComboBox` | `<input type="checkbox|radio|range">`, `<select>` |
| `QProgressBar` | `<progress>` |
| `QLabel` with rich text | any element with `cscdElementSetText` / `cscdElementSetInnerHTML` |
| `QTimer` | `cscdViewSetTimeout` / `cscdViewSetInterval` |
| `paintEvent` + `QPainter` / `SkCanvas` | `cscdElementSetPainter` with a `CscdPainter` (`fn(user, canvas, w, h)`) |
| `QPainterPath` / `SkPath` | `CscdPath` (`cscdPathMoveTo`, `LineTo`, `QuadTo`, `CubicTo`, `ArcTo`, `cscdParseSvgPath`) |
| `QBrush` gradients / `SkShader` | `cscdShaderLinear`, `cscdShaderRadial`, `cscdShaderImage` |
| `QPen` / `SkPaint` stroke | `CscdStrokeStyle` (width, caps, joins, dashes) |
| `QImage` / `SkImage` | `CscdImage` (premultiplied BGRA), `cscdLoadImage`, `cscdSavePNG` |
| `QFontDatabase::addApplicationFont` | `@font-face` or `cscdViewAddFont` |
| `QOpenGLWidget` / Skia GPU surface | `CscdWindow` with `CSCD_BACKEND_VULKAN` / `CSCD_BACKEND_OPENGL`, or `cscdGlUpload` into your context |

## A panel, before and after

Qt:

```cpp
auto* row = new QHBoxLayout;
auto* label = new QLabel("Opacity");
auto* slider = new QSlider(Qt::Horizontal);
slider->setRange(0, 100);
connect(slider, &QSlider::valueChanged, this, [=](int v) { layer->setOpacity(v / 100.0); });
row->addWidget(label);
row->addWidget(slider);
```

cascade:

```html
<div class="row"><label>Opacity</label><input type="range" id="opacity" min="0" max="100"></div>
```

```css
.row { display: flex; align-items: center; gap: 10px; }
.row label { width: 70px; color: var(--dim); }
```

```c
static void setOpacity(void* user, CscdSv value) {
    Layer* layer = user;
    layerSetOpacity(layer, strtof(value.ptr, NULL) / 100.f); /* (the value ends where the digits do) */
}

cscdElementOnInput(cscdViewGet(ui, CSCD_SV("#opacity")), setOpacity, layer, NULL);
```

From C++, a lambda without captures converts to the function pointer, and the user data carries
the object:

```cpp
cscdElementOnInput(cscdViewGet(ui, CSCD_SV("#opacity")),
                 [](void* user, CscdSv v) { static_cast<Layer*>(user)->setOpacity(std::stof(std::string(v.ptr, v.len)) / 100.0); },
                 layer, nullptr);
```

## Custom canvases (timelines, node graphs, viewports)

Give the element a size in CSS and a painter. The painter runs in CSS pixels with the origin at
the element's content box; the canvas already carries the device pixel ratio, so drawing stays
sharp on high-DPI screens. Call `cscdViewInvalidateElement(view, element)` when only the drawing
changed (no restyle or relayout).

```c
static void paintGraph(void* user, CscdCanvas* c, float w, float h) {
    Model* model = user;
    for (size_t i = 0; i < model->count; ++i) {
        const Node* node = &model->nodes[i];
        CscdRRect r = cscdRRect(cscdRect(node->x, node->y, 160, 60));
        for (int k = 0; k < 4; ++k) r.rx[k] = r.ry[k] = 8;
        cscdCanvasDrawBoxShadow(c, &r, 0, 4, 12, 0, cscdRgba(0, 0, 0, 80), false);
        cscdCanvasFillRRect(c, &r, cscdRgb(40, 41, 50));
        cscdCanvasDrawText(c, cscdSv(node->title), node->x + 10, node->y + 22, &model->font, cscdRgb(232, 232, 236));
    }
}

static void pick(void* user, CscdEvent* e) { modelPick(user, e->offsetX, e->offsetY); }

CscdElement* graph = cscdViewGet(ui, CSCD_SV("#graph"));
CscdPainter painter = {paintGraph, model, NULL};
cscdElementSetPainter(graph, painter);
cscdElementOn(graph, CSCD_SV("mousedown"), pick, model);
```

Mouse events carry `offsetX / offsetY` relative to the element, so hit testing inside a custom
canvas works like it did in `mousePressEvent`.

## Themes

Put the colors in custom properties and switch a class on `<body>`; every element follows,
including transitions:

```css
:root { --bg: #f3f4f7; --text: #1d1f24; --accent: #6c5ce7; }
body.dark { --bg: #16171b; --text: #e8e8ec; --accent: #8b7cf6; }
```

```c
cscdElementSetClass(cscdDocumentBody(cscdViewDocument(ui)), CSCD_SV("dark"), settings.dark);
```

## Keeping it light

- The view only restyles and relays out after the DOM changed; scrolling, the caret blink and
  `cscdViewInvalidateElement` only repaint.
- `cscdWindowRun` sleeps until there is input, a timer or an animation frame.
- Fonts are read lazily: the system font folder is indexed by name once, a face is loaded the
  first time it is used, and glyphs are cached as bitmaps.
