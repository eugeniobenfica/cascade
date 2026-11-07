# Platforms

cascade's window is native on each system, and so is everything the window reaches: input,
scaling, the clipboard, dragging from other applications, the system's settings and services.

| | Windows | Linux, BSDs | macOS | iOS, iPadOS | Android |
| --- | --- | --- | --- | --- | --- |
| Window | Win32 | Wayland (`xdg-shell`) or X11, chosen at run time | Cocoa | UIKit (inside the safe area) | cascade's activity (Java, inside the bars, notch and keyboard) |
| Vulkan | `vulkan-1.dll` | `libvulkan.so.1` (Wayland or Xlib surface) | MoltenVK, when installed (Metal surface) | — | `libvulkan.so` (Android surface) |
| OpenGL | WGL | GLX (X11) | `NSOpenGLContext` | — | — |
| Software | GDI | `wl_shm` buffers (Wayland), `XPutImage` (X11) | Core Animation | Core Animation | `ANativeWindow_lock` |
| Scaling | per-monitor DPI | the outputs' scale (Wayland), `Xft.dpi` (X11) | Retina backing scale | screen scale | screen density |
| Text input | IMM (composition at the caret) | the keymap through xkbcommon (Wayland), XIM (X11) | `NSTextInputClient` | `UITextInput`: suggestions, corrections, dictation, marked text; hardware keys | `InputConnection`: composition, corrections, every language; hardware keys in their layout |
| On-screen keyboard | — | — | — | of the field's kind (`type`, `inputmode`, `enterkeyhint`, `autocapitalize`, `autocorrect`) | the same |
| Clipboard | text, pictures, copied files | text and `text/uri-list` (Wayland's data device); `CLIPBOARD`: text, PNG, `text/uri-list` (X11) | text, PNG and TIFF, file URLs | text, PNG (pasted pictures as PNG files) | text, pictures (`CascadeFiles`), files (copies) |
| Dragged in from other apps | OLE (`WM_DROPFILES` when OLE is unavailable): files, text | data device (Wayland), XDND (X11): files, text | files, text | files, text (`UIDropInteraction`) | files (copies), text (split screen, free-form windows) |
| Touch | pointer messages | — | — | touch; an iPad's trackpad and mouse | touch, mouse, wheel |
| Selecting text by touch | handles, the view's own menu | — | — | handles, the edit menu (`UIEditMenuInteraction`; `UIMenuController` before iOS 16), the loupe (iOS 17) | handles, the floating edit menu, the magnifier (Android 9) |
| Screen readers | UI Automation | AT-SPI (Orca), D-Bus spoken directly | NSAccessibility | VoiceOver (UIAccessibility) | TalkBack (`AccessibilityNodeProvider`) |
| The system's settings | the apps' theme, Text size, animation effects | — | the appearance, Reduce motion | the theme, Dynamic Type, Reduce Motion | the theme, font size, animation scale |
| The app's life | minimized and back | — | minimized and back | background, foreground, memory warnings | background, foreground, low memory |
| Links, files, sharing | `ShellExecute`, the Open dialog | `xdg-open`, zenity or kdialog | `NSWorkspace`, `NSOpenPanel` | `openURL`, the document picker, the share sheet | intents, the documents UI, the share sheet |
| Saving a file | the Save As dialog | zenity or kdialog | `NSSavePanel` | the document picker, exporting | the documents UI (`ACTION_CREATE_DOCUMENT`) |
| The menu bar | — (the page's) | — (the page's) | the menu bar at the top of the screen | — | — |
| Tray | the notification area | — | a status item in the menu bar | — | — |
| Vibration | — | — | — | the feedback generators | haptic feedback |

## Support tiers and modules

| Tier | Platforms | What it means |
| --- | --- | --- |
| 1 | Windows, Linux, macOS | Supported. Every change is built, tested and opened in a window on each, with each presenter. |
| 2 | iOS, Android | Experimental. Built and tested by CI on a simulator and an emulator; Android also on phones, by hand (`tools/androidDeviceTest.sh`); iOS not yet on devices. |

Each platform is a module of its own in `src/platform/<platform>/`, with a `platform.cmake` that
says what it builds and links; a build takes the target's module only, and a copy needs no other
([`tools/slim.cmake`](../tools/slim.cmake) makes one with only yours). What every window shares is
in `common/`, the GPU presenters in `gpu/`. Its own workflow tests each platform
(`.github/workflows/<platform>.yml`), run when the engine or that module changes.

Parts of the window a build can leave out:

| Option | Off |
| --- | --- |
| `CASCADE_VULKAN` | no Vulkan presenter (Windows, Linux, macOS, Android) |
| `CASCADE_OPENGL` | no OpenGL presenter (Windows, X11, macOS), nor `cscdGl*` in `gpu.h` |
| `CASCADE_ACCESSIBILITY` | no bridges to screen readers (the accessible tree stays, for a host of its own) |

The system's software path is always there, so a window always shows; asking for a presenter the
build left out falls back to the next.

## Using a release's SDK

Each release carries the SDK of every platform: the library, its headers and a CMake package.

- **Windows, Linux, macOS**: put the SDK's folder in `CMAKE_PREFIX_PATH`, then
  `find_package(cascade 1 REQUIRED)` and `cascade_app(my_app UI ui)`. The SDK also has
  `cascade_c` (the handle API as a shared library, for Python, Rust, C#) and `cascade-render`.
- **Android**: one installed tree per ABI (`arm64-v8a`, `armeabi-v7a`, `x86_64`); point
  `CMAKE_PREFIX_PATH` at the one the NDK build is for. cascade's activity is in
  `share/cascade/android/java` (`CASCADE_ANDROID_JAVA_DIR` once the package is found).
- **iOS**: `cascade.xcframework`, for devices and the simulator. Add it to the app's target with the
  UIKit, QuartzCore, CoreGraphics and Foundation frameworks; the library holds `main` (it starts
  UIKit and calls the program's, renamed by `cascade/window.h`).

## Linux: Wayland and X11

The window is a Wayland one when the session is (`WAYLAND_DISPLAY` set) and `libwayland-client`
opens, an X11 one otherwise; `CASCADE_BACKEND=x11` or `=wayland` chooses. Nothing about Wayland is
needed to build: `libwayland-client`, `libxkbcommon` and `libwayland-cursor` are opened at run time,
and xdg-shell's interfaces are written out in `src/platform/linux/windowWayland.c`. Without the Xlib
headers cascade builds with Wayland alone. The compositor draws the window's frame when it offers
`xdg-decoration` (KDE, Sway, wlroots); otherwise (GNOME) cascade draws a title bar with the close,
maximize and minimize buttons on a subsurface, and the window's edges resize it.

Screen readers see the page through AT-SPI on both: cascade asks the session bus for the
accessibility bus, embeds itself in the registry and answers Accessible, Application, Component,
Action, Text and Value from the accessible tree (`NO_AT_BRIDGE=1` turns it off).

## iOS and Android

Both start the program themselves, so `cascade/window.h` renames the program's `main` to
`cscdAppMain` there and cascade calls it once the system is ready: the same
`int main(int argc, char** argv)` runs on every platform (include `cascade/window.h` in the file
that has it). There is one window, the size of the screen: `width`, `height`, `resizable` and the
title are ignored, `cscdWindowSetFullscreen` hides the system's bars, and `cscdWindowClose` ends
the app. The page is laid out where nothing covers it (clear of the status bar, the notch, the home
indicator and the on-screen keyboard); what is around it continues the page's edges: under the
status bar the color of what the page shows at its top, above the home indicator what it shows at
its bottom (the background of a box that scrolls there, so items passing under a bar do not tint
it), and beside the page in landscape (the notch, the navigation bar) each row continues the page's
edge there. The status bar's text turns dark or light to stay readable.

A drag along a slider moves it; the page scrolls with drags mostly up or down. `touch-action`
keeps gestures from the view, as in browsers: over `touch-action: none` a drag is the page's
(`mousedown`, `mousemove`, for drawing on a canvas, a map, a game), `pan-y` leaves only scrolling up
and down to the view, `pan-x` only sideways, and pinching zooms only where `pinch-zoom` (or `auto`,
`manipulation`) allows it. On Android 10 and later, a slider, a splitter or an element whose
`touch-action` keeps sideways drags, near the screen's left or right side, is kept from the system's
back gesture.

Each touch carries the time the screen sampled it: the moves Android gathers between two events
(screens sample faster than frames come) and iOS's coalesced touches each reach the page, so a
stroke has all its points, and a fling's speed is the finger's even when the program takes the moves
late. The speed is measured over at least 8 ms.

The on-screen keyboard comes up while a text field has the focus, of the kind the field asks for:
`type=email`, `url`, `tel`, `number`, `search` and `password`, `inputmode` (`none` keeps it away),
`enterkeyhint` (the action key: `go`, `search`, `send`, `next` and `previous` move the focus,
`done` also lets the keyboard go), `autocapitalize` and `autocorrect` / `spellcheck`. The keyboard
edits the field's whole text (`cscdViewSetTextState`), so its suggestions and corrections, dictation
and input methods that compose (Chinese, Japanese, Korean, accents held on a key) work as in the
system's own fields; the part still being composed is underlined.

Text is selected by touch as in the system's own fields: a long press selects the word (the page's
`contextmenu` handler can prevent it), a double tap too, and a tap on the caret asks for the menu.
cascade draws handles at the selection's ends (or under the caret), which a finger drags, and the
system shows its edit menu beside the selection (cut, copy, paste, select all; copying a password
is never offered) and its magnifier over the handle being dragged. Typing, a key or the field
losing the focus take them away.

The system's theme, text size and "reduce motion" reach the page (`prefers-color-scheme`, the
root's default font size and the font-size keywords, `prefers-reduced-motion`), on desktops too,
unless the app chose otherwise (`cscdViewSetDarkMode`, the "animations" feature). Going to the
background stops the drawing and calls `cscdWindowOnAppEvent` (save what the user would lose: the
system may end a background app without warning), as do coming back and low memory.

**iOS** (iOS 13 or later; Xcode on a Mac, signed with your team):

```bash
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
```

then open `build-ios/cascade.xcodeproj`, pick a team for `cascade-gallery` and run it. `cascade_app()`
makes the target an app bundle with its interface folder inside; the gallery's
[Info.plist](../examples/ios/Info.plist.in) is a starting point for yours. UIKit's run loop runs inside
`cscdWindowPump`, on the main thread; while the page animates, frames follow the screen's refresh
(120 Hz on ProMotion screens: `CADisableMinimumFrameDurationOnPhone` in the Info.plist lets iPhones
go past 60). Files and text dragged in from other apps (an iPad's split screen) reach the page as
drops from the desktop's file manager do.

**Android** (Android 8 or later; the NDK, the SDK's build tools and a JDK, no Gradle): the program
is a shared library that cascade's activity (`org.cascade.CascadeActivity`, one Java file in
[src/platform/android/java](../src/platform/android/java)) loads; the activity hands the program what the
system gives only to Java (the input method, the clipboard, TalkBack, insets, intents) and the
program's `main` runs on a thread of its own, in C.
[examples/android/buildApk.sh](../examples/android/buildApk.sh) builds the gallery's APK, the Java
compiled by `javac` and `d8` from the SDK:

```bash
ANDROID_HOME=~/Android/Sdk ANDROID_NDK_HOME=~/Android/Sdk/ndk/<version> examples/android/buildApk.sh
adb install -r build-android/cascade-gallery.apk
```

Your own app needs the same [manifest](../examples/android/AndroidManifest.xml) with its library's
name (`android.app.lib_name`) and the activity's Java file among its sources (with Gradle: add
`src/platform/android/java` to the module's `java.srcDirs`; CMake names the folder
`CASCADE_ANDROID_JAVA_DIR`). The manifest's `<provider>` (`org.cascade.CascadeFiles`, its authority
the package's name followed by `.cascade.files`) lets the app copy pictures to the clipboard;
without it, pictures are not copied. Files read by relative paths (`ui/index.html`) are looked up
among the APK's assets; standard error goes to the log (`adb logcat -s cascade`); system fonts come
from `/system/fonts`; files the picker gives, the clipboard holds and other apps drop are copies in
the app's cache (`content:` URIs only, under safe names), removed the next time the app starts.
Links open only for the web, mail, phone, messages, maps and the store. The library is linked for
16 KB pages, as Google Play asks of new apps. Back takes the selection's handles away, closes what
is open over the page (a menu, a dialog, the keyboard), or else the app. Vulkan is used when the
device has it, the software path otherwise.

To test on your own device: turn on its developer options and USB or wireless debugging, and check
that `adb devices` lists it. [tools/androidDeviceTest.sh](../tools/androidDeviceTest.sh) then runs
the engine's tests on it (build them as the Android workflow does, for its ABI), installs the
gallery and drives it: start times, the presenter, taps, typing, selecting text by touch, Back, the
background and coming back, the system ending it, scrolling's frame rate, CPU and memory over a
while, crashes; it leaves a screenshot of each step in `build-android/device-test` and changes none
of the device's settings:

```bash
ANDROID_SERIAL=<the device, as adb devices names it> tools/androidDeviceTest.sh
```

The log says which presenter the gallery uses (`adb logcat -s cascade`); a Vulkan that does not work
there says why.

## Choosing a backend

```c
CscdWindowOptions opts = cscdWindowOptions();
opts.backend = CSCD_BACKEND_AUTO;                                     /* Vulkan, then OpenGL, then software */
CscdWindow* win = cscdWindowNew(&opts);
cscdWindowBackendDescription(win);                                    /* "Vulkan (NVIDIA GeForce …)" */
```

`CSCD_BACKEND_VULKAN`, `CSCD_BACKEND_OPENGL` and `CSCD_BACKEND_SOFTWARE` ask for one; when it does not work on the machine,
the window falls back to the next. Both GPU APIs are opened at run time, so an application builds
without any SDK and still starts on a machine without a GPU driver. If presenting fails later (a
lost device), the window carries on with the software path.

## The window

| | |
| --- | --- |
| `CscdWindowOptions` (`cscdWindowOptions()` for the defaults) | title, size (CSS pixels), backend, resizable, dark title bar, minimum size, `visible`, `page`, `liveReload` |
| `cscdWindowShow`, `Maximize`, `SetFullscreen`, `Resize`, `SetTitle`, `Close` | as named; `cscdWindowMaximize` also shows a window made hidden |
| `cscdWindowOnClose` | the function returns `false` to keep the window open (to ask about unsaved work) |
| `cscdWindowOnFrame` | runs once per frame before painting |
| `cscdWindowPost` | runs a function on the window's thread, from any thread |
| `cscdWindowNativeHandle`, `cscdWindowNativeDisplay` | the `HWND`, X11 window, `NSWindow*`, `UIWindow*` or the activity (a JNI global reference), and the X11 `Display*` |
| `cscdWindowDevicePixelRatio` | device pixels per CSS pixel |
| `cscdWindowOnAppEvent` | the app goes to the background, comes back, or should free memory |
| `cscdWindowOpenUrl`, `cscdWindowShareText` | a link opened by the system (browser, mail, phone); the share sheet (phones) |
| `cscdWindowPickFiles` | the system's file picker, with an `accept` list as `<input type=file>` has; the files come to a callback |
| `cscdWindowSaveFile` | the system's dialog to save a file; the callback writes it where the user chose |
| `cscdWindowSetMenuBar` | the app's menus in the system's menu bar (macOS) |
| `cscdTrayNew` | an icon in the tray (Windows, macOS), with a click action and a menu |
| `cscdWindowHide` | hides the window (Windows, X11, macOS) |
| `cscdWindowHaptic` | a short vibration that answers a touch (phones) |

The page's `<title>` becomes the window's title until `cscdWindowSetTitle` is called.

## Keyboard

On macOS, Command is reported as Ctrl and Control as Meta, so an interface's Ctrl shortcuts are
the Mac's Command ones.

## Threads and COM on Windows

Make the window on the thread that runs the interface, before anything sets that thread's COM to
multithreaded (some audio libraries do when they start): dragging from Explorer needs OLE on that
thread. When OLE is not available, dropped files still arrive through `WM_DROPFILES`, without the
hovering feedback, and cascade says so on standard error.
