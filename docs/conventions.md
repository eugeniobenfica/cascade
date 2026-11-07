# Conventions

cascade is plain C11. The API follows a few rules everywhere, so that what one function does
tells you what the others do.

## Names

| Kind | Style | Examples |
| --- | --- | --- |
| Types | `Cscd` + PascalCase | `CscdView`, `CscdElement`, `CscdComputedStyle`, `CscdRect` |
| Functions | `cscd` + camelCase, the type first, then the verb | `cscdViewLoadFile`, `cscdElementSetText`, `cscdCanvasFillRect` |
| Constants, enum values, macros | `CSCD_` + UPPER_CASE | `CSCD_KEY_ENTER`, `CSCD_DISPLAY_FLEX`, `CSCD_SV("text")` |
| Fields | camelCase | `e->scrollTop`, `style->backgroundColor` |
| Files | camelCase | `viewInternal.h`, `fontOt.c`, `windowWin32.c`, `gettingStarted.md`, `makeTestFont.py` |

File names follow the functions: one word stays lower case (`view.c`, `dom.h`), so that the public
headers keep short names. Only names a tool or a language fixes differ: Java's classes
(`CascadeActivity.java`), `CMakeLists.txt`, `AndroidManifest.xml`, `Cargo.toml`, `README.md`... What the
build makes keeps its own style (`cascade-render`, `cascade_c`).

Internal functions (in `src/`) have no prefix and are `static`, or carry a short module prefix
(`lay`, `view`, `edit`) when several files share them. The internal headers are not installed.

## Strings

Text is UTF-8 everywhere, in two kinds (file paths too: on Windows they open through the UTF-16
calls, so names in any language work, whatever the system's code page):

- **`CscdSv`**, a borrowed view: a pointer and a length, not NUL-terminated, owning nothing. What
  functions *take*. Make one from a literal with `CSCD_SV("text")`, from a C string with
  `cscdSv(cstr)`, from a `CscdStr` with `cscdStrSv(&s)`.
- **`CscdStr`**, an owned string: always NUL-terminated (`cscdStrC(&s)` is a `const char*`, never NULL),
  freed with `cscdStrFree`. What functions *return* when the text is new; the comment says "the
  caller's".

```c
CscdStr html = cscdElementInnerHTML(e);       /* the caller's */
printf("%s\n", cscdStrC(&html));
cscdStrFree(&html);

CscdSv id = cscdElementId(e);                 /* borrowed: good while e is unchanged */
```

Functions that only look at a value return a `CscdSv` into the object (valid until it changes);
`cscdElementAttr(e, CSCD_SV("href"))` is `""` when the attribute is not there, and
`cscdElementGetAttribute` returns NULL in that case.

## Vectors and maps

`CSCD_VEC(T)` is `struct { T* items; size_t len, cap; }`. A zeroed one is empty, and the macros grow
it: `cscdVecPush(&v, x)`, `cscdVecPop(&v)`, `cscdVecLast(&v)`, `cscdVecInsert`, `cscdVecRemove`,
`cscdVecResize` (zero-fills), `cscdVecFree`. Each kind has a name (`CscdPtrVec`, `CscdFloatVec`,
`CscdStrVec`...), so that the same kind is one type everywhere.

Functions that find several things *append* to a vector you give them:

```c
CscdPtrVec rows = {0};
cscdViewQuerySelectorAll(view, CSCD_SV(".row"), &rows);
for (size_t i = 0; i < rows.len; ++i) cscdElementAddClass(rows.items[i], CSCD_SV("seen"));
cscdVecFree(&rows);
```

`CscdStrMap` (text keys) and `CscdU64Map` (numbers or pointers, `CSCD_PKEY(p)`) are open-addressing hash
maps whose values are a `CscdMapVal` union.

## Ownership

- `...New` makes an object you own; `...Free` frees it (`cscdViewNew` / `cscdViewFree`,
  `cscdWindowNew` / `cscdWindowFree`, `cscdCanvasNew` / `cscdCanvasFree`).
- Objects that are **shared** are reference counted: `cscdRetain` takes a reference, `cscdRelease`
  drops it (NULL is fine for both). Images made by `cscdImageNew` or a decoder, computed styles,
  style sheets and drag data (`CscdDataTransfer`) are shared. A function that keeps one it is given
  says so ("retains").
- Values you hold on the stack have `...Init` / `...Fini` (`cscdImageInit`, `cscdShaderFini`,
  `cscdStrokeStyleFini`), or need no cleaning when their comment says so (a solid shader).
- The DOM owns its nodes. `cscdElementRemove` takes an element out of the document and frees it;
  while an event is on its way through the document, the freeing waits until it is done, so a
  handler may remove its own element.

## Callbacks

A callback is a function, its user data, and (optionally) what frees that data when the callback
is dropped:

```c
typedef struct CscdEventHandler { CscdEventFn fn; void* user; CscdFreeFn freeUser; } CscdEventHandler;
```

`cscdElementOn(e, type, fn, user)` is the short form without a free function. Painters
(`CscdPainter`), highlighters, timers (`CscdAction`) and menu items work the same way. A free function
runs once: when the listener is removed, its element is freed, the timer is cleared or has run,
the menu is closed.

## Errors

Functions that can fail return `bool` (or NULL) and report what went wrong through the view's
warning callback (`cscdViewOnWarning`) when there is something to say. Memory allocation failing
aborts the program: no function returns half-made objects.

## Threads

Use a view, its document and its window from the thread that made them. `cscdWindowPost` is the one
call meant for other threads: it runs a function on the window's thread soon. Each view (and its
window) stands alone, so two views may live on two threads.

Inside, the library runs its parallel work (the painting bands, the first scan of the system's
fonts) on workers it keeps, and shares caches between threads (glyphs, blurred shadows, fonts).
Callbacks it calls from several threads at once say so: the program's resource loader
(`cscdAppSetResourceLoader`) is one.
