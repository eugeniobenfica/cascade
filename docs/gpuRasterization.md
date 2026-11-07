# Rasterizing on the GPU, with the same pixels everywhere

A study: whether cascade can paint frames on the GPU and keep its promise that a page shows the same
pixels on every platform, machine and presenter ([Principles](principles.md#the-same-pixels-everywhere)).
Nothing here is built yet. It says what stands in the way, a design that gets around it, what it
would cost, and the order to do it in.

## Where the time goes today

A full repaint, caches warm (cascade 1.0.0, MinGW GCC `-O2`, a 4-core x86 laptop; the frame at 1x
for 1920×1080, at 2x for 3840×2160):

| Page | 1920×1080 | 3840×2160 | 3840×2160, one thread |
| --- | ---: | ---: | ---: |
| The gallery (controls, text, borders, a few shadows) | 1.9 ms | 4.5 ms | 9.4 ms |
| `docs/assets/src/hero.html` (large blurred shadows, pictures drawn smaller) | 23 ms | 46 ms | 178 ms |

Two things follow:

- **An ordinary interface is not GPU-bound.** Fills, borders and text at 4K fit in a 120 Hz frame
  (8.3 ms) on four cores, and a window repaints only what changed.
- **Blurs are what costs**, and they grow with the area: `filter: blur()`, `backdrop-filter`, and
  large `box-shadow` blurs. (The hero page took 1,624 ms at 4K with threads before 1.0.0: every
  band blurred the large shadows and reduced the pictures again. Each is now made once for all the
  bands.)

So the GPU's case is blurs, very large surfaces (4K and 5K at 2x, several windows), the battery on
phones, and animations that would otherwise repaint every frame.

## Why a GPU does not give the same pixels

The CPU path gives the same bits everywhere because every operation in it is specified to the bit:
IEEE single precision without fused multiply-adds (`-ffp-contract=off`), cascade's own `sin`, `pow`
and `exp`, and integer pixel math with explicit rounding. On a GPU, most of that is left to the
driver:

| What | What the APIs allow |
| --- | --- |
| Float arithmetic | Shader compilers fuse `a*b+c` unless each result is marked (`precise` in GLSL, `NoContraction` in SPIR-V); denormals may be flushed to zero. |
| Division, `sqrt`, `inversesqrt` | Vulkan allows errors of 2.5 ULP for `x / y`, more for the others: each vendor rounds differently. |
| `sin`, `exp`, `pow`, `log` | Their precision is up to the implementation. |
| Rasterization and MSAA | Sample positions, the coverage of a pixel at an edge, and the anti-aliasing filter differ by vendor. |
| Texture filtering | Bilinear weights use a fixed-point fraction with as few as 4 bits (`subTexelPrecisionBits`); sRGB conversions and mipmaps differ. |
| Blending | Fixed-function blending of 8-bit formats rounds as the hardware does. |
| Order | Atomics and work in flight run in no fixed order. |

This is why cascade rejected GPU compositing before: the GPU would sample, blend and round in its
own way. But one part of every GPU *is* exact. **Integer arithmetic** on 32 bits (add, subtract,
multiply, shifts, bitwise operations, unsigned division) is specified to the bit in SPIR-V, Metal
and GLSL, and every conformant driver must give the same result. A rasterizer that uses only
integers, and none of the fixed-function stages, paints the same pixels on any GPU.

## The design: one integer rasterizer, run on the CPU and on the GPU

1. **Geometry stays on the CPU.** Layout, transforms, path flattening, stroking and text shaping
   stay where they are. Their float math is already the same everywhere. What they produce is
   rounded once into fixed point: coordinates as 24.8 integers, colors as premultiplied 8-bit,
   gradient stops and image transforms as 16.16. Every frame's input to the rasterizer is integers.

2. **The rasterizer becomes integer-only.** Exact-area coverage of an edge across a pixel is a
   piece of geometry that integers compute exactly: accumulate signed areas in 16.16 per cell, as
   the current rasterizer does with floats, then turn them into an 8-bit coverage with explicit
   rounding. Blending is the CPU's `mulPixel` (already integer). Gradients are evaluated in fixed
   point, or read from a 256-entry ramp the CPU built. Images are sampled with `texelFetch` and
   bilinear weights computed in the shader in 8.8 fixed point, never with the texture unit's
   filtering. Blurs use integer kernels: weights in 16.16 summing to exactly 65536, so the
   per-band blurs can never drift.

3. **The same kernels on both sides.** The integer rasterizer becomes *the* reference: the CPU path
   runs it as C, the GPU path runs it as compute shaders. The kernels are written once in a small
   subset common to C and GLSL (integers, fixed-size arrays, no pointers), included by both sides,
   so they cannot drift apart. The CPU path changes its pixels once, when it moves to the integer
   rasterizer (new reference images for the tests), and from then on the CPU and every GPU match.

4. **Compute only, as tiles.** The frame is cut into 16×16 tiles. The CPU (later a compute pass)
   bins the display list's commands into a list per tile. A compute workgroup per tile runs that
   list in order, one invocation per pixel, and writes the result with `imageStore` to an `r32ui`
   image. There is no rasterization stage, no blending stage and no texture filtering; nothing
   depends on order, because one invocation alone writes each pixel. This is the shape of
   Vello / piet-gpu, Spinel and Forma, with integers where they use floats.

5. **Present as today.** The `r32ui` image is the frame, which the presenter copies to the
   swapchain. Reading it back (`cscdViewPaint` into a `CscdImage`, screenshots, tests) gives the
   same bits as the CPU.

### Where it runs

| Platform | Compute API | Notes |
| --- | --- | --- |
| Windows, Linux, Android | Vulkan 1.1 compute | already loaded at run time by the presenters |
| macOS | Vulkan through MoltenVK (Metal compute) | MoltenVK translates SPIR-V; integer math stays exact in Metal |
| iOS | Metal | cascade's iOS window has no Vulkan; a Metal path, or MoltenVK linked in |
| OpenGL hosts | GL 4.3 / GLES 3.1 compute | not macOS (GL 4.1 has no compute): the CPU path there |
| Everywhere else | the CPU, the same kernels as C | the reference, and the fallback when a device cannot |

A frame never mixes the two: when the device lacks something a frame needs (a kernel did not
compile, memory ran out), that frame is painted on the CPU, and the pixels are the same either way.

### What stays on the CPU

- **Glyphs** are rasterized once into the glyph cache (already integer coverage after this change),
  then uploaded to an atlas the kernels read with `texelFetch`.
- **Painters** draw through `CscdCanvas`, which records into the display list: they run on the GPU
  path unchanged.
- **Hit testing**, which reads the display list, is unchanged.

## Proving it

- **A fuzzer of frames.** Random display lists (paths, gradients, images, blurs, clips, layers), each
  painted by the C kernels and by the compute kernels; the hashes of the two images must be equal.
  CI runs it on lavapipe (Linux, Mesa's CPU Vulkan), SwiftShader (Windows) and MoltenVK (macOS).
- **Real GPUs**, which is what matters: the same corpus on NVIDIA, AMD, Intel, Apple, Adreno and
  Mali, by hand before a release, until a device farm can run it on each change.
- **The tests' pages**, rendered on every presenter, compared to the reference PNGs.

## Cost and risks

- **Effort.** The integer rasterizer and its new reference images are the first, largest part:
  about a month. The tile binner and compute kernels for fills, borders, text and images take
  another; gradients, layers, clips, blurs and filters a third. Then the fuzzer and the devices.
- **Precision.** Coverage in 16.16 instead of a float changes edges by at most one level in 255.
  That is invisible, but it does change every reference image once.
- **The 32-bit limit.** 64-bit integers are optional on GPUs (`shaderInt64`), so every product must
  fit in 32 bits. That bounds coordinates (±8 million pixels in 24.8) and the sizes of blur
  kernels; larger blurs run as several passes.
- **Drivers.** Integer conformance is tested by the Khronos CTS, but compilers have bugs. The
  fuzzer is what finds them, and a device that fails is put on the CPU path by name.

## The order to do it in

1. **A deterministic compositor.** Layers painted on the CPU, composited by a compute shader that
   uses only integers: opacity, and translations by whole device pixels, during animations and
   scrolling. This is a few hundred lines. It removes most repaints in animations, and proves the
   integer-on-GPU approach on every driver before the large work begins.
2. **The integer rasterizer on the CPU** (new reference images, the C kernels).
3. **The compute path** for fills, borders, text and images; then gradients and clips; then layers,
   blurs and filters. Each step is a frame the CPU paints when the GPU path lacks a part.
4. **Mobile:** Metal on iOS; Android's Vulkan as on the desktops.

Each step can stop there and still have paid for itself. If step 1 makes the interfaces people
build fast enough, and the profiles show rasterization no longer leads, the rest can wait.
