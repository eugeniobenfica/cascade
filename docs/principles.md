<p align="center"><img src="assets/logo.svg" width="72" alt="cascade"></p>

# Principles

What cascade holds to, and how it decides what goes in.

## Dependencies: only what cannot be done without

cascade writes what it needs itself. A library is never brought in because it would make the work
easier: the HTML parser, the cascade, the layout, OpenType shaping, the image decoders, the
rasterizer and the GPU loaders all live here. A program that uses cascade builds with a C compiler
and CMake, and carries nothing else.

A dependency comes in only when it is **necessary**: when the platform offers a capability through
that library and nothing else. Android is where this happens — some of what apps are expected to
do exists only in Google's libraries (Jetpack, Play services): push notifications, purchases
through Google Play, signing in with a Google account. None has come in yet: today cascade links no
third-party library on any platform. When it offers such a thing:

- the dependency stays in that platform's module (`src/platform/android`), never in the engine;
- it is optional where it can be: a build that does not use the capability does not carry it;
- the reason is written next to it: what it is needed for, and why the system alone cannot do it.

What the systems themselves provide is not a dependency: Win32, Cocoa, UIKit, the Android SDK and
the NDK, and the libraries opened at run time where they are found (Vulkan, OpenGL, Wayland). A
program still starts without them, on the next presenter or the next window system.

## The same pixels everywhere

Every frame is painted by cascade's own rasterizer, on the CPU, the same way on every platform
(floating point is kept from fusing operations differently on different processors); the GPU only
presents. A page looks the same on Windows, a Mac and a phone, and a test that compares pixels on
one compares them on all: `testPixels` in `tests/tests.c` holds every platform CI has to the same
hashes of the same pages (`tests/pixelsExpected.inc`), with a font and pictures of the test's own.
The pages are a sample, not every operation: they catch what moves a pixel on them, as fusing
operations does.

## Each platform in its own module

A platform's window, input, presenters and screen readers live in `src/platform/<platform>`, built
only for that target. What every window shares is in `common/`; nothing in the engine knows which
platform it runs on. Platforms come in [tiers](platforms.md#support-tiers-and-modules): tier 1 is
tested on every change; tier 2 is tested on a simulator or an emulator until it has been on
devices.

## The web's names, C's ways

Pages are written as for a browser: the same HTML, the same CSS, the same events and the same
`data-` attributes, so that what is known about the web applies. The API around them is plain C,
written one way everywhere: see [Conventions](conventions.md).

## Versions that mean something

Releases follow [Semantic Versioning](https://semver.org): a program written for 1.x compiles
against every 1.x. The version is written once (`project()` in CMakeLists.txt), and CI checks
that everything repeating it agrees.
