cmake_minimum_required(VERSION 3.16)
project(cascade VERSION 1.0.0 DESCRIPTION "HTML and CSS interfaces driven from C" LANGUAGES C)

# HTML / CSS renderer in plain C11: no third-party libraries. Vulkan and OpenGL are loaded at run
# time (vulkan-1.dll / opengl32.dll, libvulkan.so.1 / libGL.so.1, MoltenVK / OpenGL.framework), so no
# SDK is needed to build.
#
#   src/                 the engine, a folder per part: core/ dom/ style/ layout/ text/ image/ paint/
#                        view/ capi/
#   src/platform/        the native window, a module per platform (the target's, chosen below):
#     common/              what every platform shares: the frame, the presenter's choice, posting
#     gpu/                 the Vulkan and OpenGL presenters (CASCADE_VULKAN, CASCADE_OPENGL)
#     windows/ linux/ macos/ ios/ android/   each with its platform.cmake
#   cmake/               cascade_app() and the package find_package(cascade) finds
#
# Only src/, include/, cmake/ and the target's module are needed to build the library: a copy may
# leave the rest out (tools/slim.cmake makes one), and the options below follow what is there.

# ---- options -----------------------------------------------------------------------------------------

# Examples, tools and tests default on only when cascade is the top-level project and they are in
# this copy; phones run apps, not command-line programs: no tools or tests there by default.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(cascade_top ON)
else()
  set(cascade_top OFF)
endif()
foreach(part examples tools tests)
  if(cascade_top AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${part}" AND (part STREQUAL "examples" OR NOT (IOS OR ANDROID)))
    set(cascade_default_${part} ON)
  else()
    set(cascade_default_${part} OFF)
  endif()
endforeach()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/common")
  set(cascade_default_window ON)
else()
  set(cascade_default_window OFF) # (a copy of the engine alone)
endif()
option(CASCADE_WINDOW "Build the native window of the target's platform" ${cascade_default_window})
option(CASCADE_VULKAN "Present frames through Vulkan where the platform has it" ON)
option(CASCADE_OPENGL "Present frames through OpenGL where the platform has it" ON)
option(CASCADE_ACCESSIBILITY "Offer pages to the platform's screen readers" ON)
option(CASCADE_BUILD_EXAMPLES "Build the examples" ${cascade_default_examples})
option(CASCADE_BUILD_TOOLS "Build cascade-render (HTML to PNG)" ${cascade_default_tools})
option(CASCADE_BUILD_TESTS "Build cascade-tests" ${cascade_default_tests})
option(CASCADE_C_SHARED "Build cascade_c, a shared library of the handle API (for other languages)" OFF)
option(CASCADE_INSTALL "Install the library, its headers and its CMake package" ${cascade_top})
# Checks for development (GCC, Clang): -DCASCADE_SANITIZE=address,undefined builds the library and
# what links it with those sanitizers; CASCADE_FUZZ builds cascade-fuzz-* (tests/fuzz.c), with
# libFuzzer when the compiler is Clang, or as a program that runs the files it is given.
set(CASCADE_SANITIZE "" CACHE STRING "Sanitizers to build with (GCC, Clang): address, undefined, ...")
option(CASCADE_FUZZ "Build the cascade-fuzz-* programs" OFF)
# The library's target name, for projects that already have a target called "cascade".
set(CASCADE_TARGET cascade CACHE STRING "Name of the cascade library target")
set(BT ${CASCADE_TARGET})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(cmake/cascadeApp.cmake)

# ---- the engine ----------------------------------------------------------------------------------------

add_library(${BT} STATIC
  # core: memory, strings, containers, files, threads
  src/core/base.c src/core/files.c
  # dom: the HTML parser, the DOM, data binding
  src/dom/html.c src/dom/dom.c src/dom/bind.c
  # style: CSS syntax and selectors, the cascade, computed values
  src/style/css.c src/style/style.c src/style/styleValues.c src/style/propertiesValues.c src/style/properties.c
  src/style/cascade.c
  # layout: block, inline, flex, grid and table
  src/layout/layout.c src/layout/flexgrid.c
  # text: fonts, OpenType shaping, bidirectional text
  src/text/font.c src/text/fontOt.c src/text/fontIndic.c src/text/fontColor.c src/text/fontVar.c src/text/bidi.c
  # image: the decoders
  src/image/image.c src/image/imageVp8.c
  # paint: the canvas, the rasterizer, painting the boxes
  src/paint/canvasPath.c src/paint/canvas.c src/paint/paint.c
  # view: a page and its interaction (events, keys, touch, widgets, accessibility, the inspector)
  src/view/view.c src/view/viewLoad.c src/view/viewAnimate.c src/view/viewRestyle.c src/view/viewScroll.c
  src/view/viewEvents.c src/view/viewKeys.c src/view/widgets.c src/view/touch.c src/view/inspector.c src/view/a11y.c
  # capi: the handle API (cascadeC.h)
  src/capi/capi.c
)
add_library(cascade::cascade ALIAS ${BT})
set_target_properties(${BT} PROPERTIES EXPORT_NAME cascade)
target_include_directories(${BT}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE src) # (internal headers by their folder: "view/viewInternal.h")
# the first look at the system fonts reads them on several threads; frames paint by bands on several
find_package(Threads REQUIRED)
target_link_libraries(${BT} PUBLIC Threads::Threads)
# what cascade.pc tells programs built without CMake to link, besides the library (the platform
# modules add theirs)
set(cascade_pc_libs "")
set(cascade_pc_cflags "")
if(CMAKE_USE_PTHREADS_INIT)
  list(APPEND cascade_pc_libs -pthread)
endif()
if(UNIX)
  target_link_libraries(${BT} PUBLIC m)
  list(APPEND cascade_pc_libs -lm)
endif()
if(ANDROID)
  target_link_libraries(${BT} PUBLIC android log) # (the APK's assets, files.c; the log)
  list(APPEND cascade_pc_libs -landroid -llog)
endif()

if(MSVC)
  target_compile_options(${BT} PRIVATE /W3 /utf-8 /Gy /fp:precise) # (no fused a*b+c: the same pixels everywhere; /Gy: functions the linker can drop)
  target_compile_definitions(${BT} PRIVATE _CRT_SECURE_NO_WARNINGS) # (strcpy and its kin: bounded by hand)
else()
  # -ffp-contract=off: no a*b+c fused into one rounding (ARM compilers fuse by default, x86 ones
  # do not), so that every platform paints the same pixels
  target_compile_options(${BT} PRIVATE -Wall -Wno-parentheses -Wno-unused-function -ffp-contract=off)
  # each function in a section of its own: a program's release link drops what it does not call
  # (cascade_app)
  target_compile_options(${BT} PRIVATE -ffunction-sections -fdata-sections)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(${BT} PRIVATE -O2)
  endif()
endif()

if(CASCADE_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "cascade: CASCADE_SANITIZE is for GCC and Clang")
  endif()
  # on the library's interface: the programs that link it are built and linked with them too
  target_compile_options(${BT} PUBLIC -fsanitize=${CASCADE_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND CASCADE_SANITIZE MATCHES "undefined")
    # (an empty vector's NULL + 0 would stop every run; AddressSanitizer sees real overruns)
    target_compile_options(${BT} PUBLIC -fno-sanitize=pointer-overflow)
  endif()
  target_link_options(${BT} PUBLIC -fsanitize=${CASCADE_SANITIZE})
endif()

# ---- the window: the target platform's module ------------------------------------------------------------
# Tier 1 (Windows, Linux, macOS): supported, every change tested on each. Tier 2 (iOS, Android):
# experimental, built and tested by CI on a simulator and an emulator; Android also on phones, by hand
# (tools/androidDeviceTest.sh), iOS not yet on devices.

if(WIN32)
  set(cascade_platform windows)
elseif(ANDROID)
  set(cascade_platform android)
elseif(IOS)
  set(cascade_platform ios)
elseif(APPLE)
  set(cascade_platform macos)
elseif(UNIX)
  set(cascade_platform linux)
else()
  set(cascade_platform "")
endif()

set(cascade_window OFF)
set(cascade_uses_x11 OFF)
if(CASCADE_WINDOW AND cascade_platform)
  set(cascade_platform_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/${cascade_platform}")
  if(NOT EXISTS "${cascade_platform_dir}/platform.cmake" OR NOT EXISTS "${cascade_platform_dir}/../common")
    message(FATAL_ERROR "cascade: this copy has no window for ${cascade_platform} (src/platform/${cascade_platform}, "
      "src/platform/common): it was made for other platforms (tools/slim.cmake). Use a copy with that "
      "platform, or -DCASCADE_WINDOW=OFF for the engine alone.")
  endif()
  include(${cascade_platform_dir}/platform.cmake)
  target_sources(${BT} PRIVATE src/platform/common/window.c src/platform/common/presenter.c)
  set(cascade_backends "software")
  if(CASCADE_VULKAN AND cascade_platform_vulkan)
    target_sources(${BT} PRIVATE src/platform/gpu/vulkan.c)
    string(PREPEND cascade_backends "Vulkan, ")
  else()
    target_compile_definitions(${BT} PRIVATE CASCADE_NO_VULKAN=1)
  endif()
  if(CASCADE_OPENGL AND cascade_platform_opengl)
    target_sources(${BT} PRIVATE src/platform/gpu/opengl.c)
    string(REPLACE "software" "OpenGL, software" cascade_backends "${cascade_backends}")
  else()
    target_compile_definitions(${BT} PRIVATE CASCADE_NO_OPENGL=1)
  endif()
  if(NOT CASCADE_ACCESSIBILITY)
    target_sources(${BT} PRIVATE src/platform/common/a11yOff.c)
    target_compile_definitions(${BT} PRIVATE CASCADE_NO_ACCESSIBILITY=1)
  endif()
  target_compile_definitions(${BT} PUBLIC CASCADE_HAS_WINDOW=1)
  list(APPEND cascade_pc_cflags -DCASCADE_HAS_WINDOW=1)
  set(cascade_window ON)
  if(cascade_platform_tier EQUAL 1)
    set(cascade_tier "tier 1")
  else()
    set(cascade_tier "tier 2, experimental")
  endif()
  message(STATUS "cascade ${PROJECT_VERSION}: the window of ${cascade_platform_name} (${cascade_tier}); ${cascade_backends}")
else()
  message(STATUS "cascade ${PROJECT_VERSION}: no window (the engine alone)")
endif()

# ---- cascade_c: the handle API as a shared library ---------------------------------------------------------
# (cascade_c.dll, libcascade_c.so, libcascade_c.dylib) that Python's ctypes, C#, Go... load. It holds
# the static library; only the cscd_* functions are exported.

if(CASCADE_C_SHARED)
  set_target_properties(${BT} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(cascade_c SHARED src/capi/capi.c)
  target_link_libraries(cascade_c PRIVATE ${BT})
  target_include_directories(cascade_c PRIVATE src)
  target_compile_definitions(cascade_c PRIVATE CASCADE_C_EXPORTS)
  # libcascade_c.so.1: a new major version is a new name, so programs keep the ABI they were built on
  set_target_properties(cascade_c PROPERTIES C_VISIBILITY_PRESET hidden PREFIX ""
    VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
  cascade_release_link(cascade_c)
  if(UNIX)
    set_target_properties(cascade_c PROPERTIES PREFIX "lib")
  endif()
  if(UNIX AND NOT APPLE)
    target_link_options(cascade_c PRIVATE "-Wl,--exclude-libs,ALL") # (the library inside stays inside)
  endif()
  if(MINGW)
    target_link_options(cascade_c PRIVATE -static)
  endif()
endif()

# ---- tools, examples, tests ------------------------------------------------------------------------------

if(CASCADE_BUILD_TOOLS)
  add_executable(cascade-render tools/render.c)
  target_link_libraries(cascade-render PRIVATE ${BT})
  if(WIN32)
    target_link_libraries(cascade-render PRIVATE shell32) # (CommandLineToArgvW: the arguments as UTF-8)
  endif()
  cascade_release_link(cascade-render)
endif()

if(CASCADE_BUILD_EXAMPLES AND cascade_window)
  if(ANDROID)
    add_library(cascade-gallery SHARED examples/gallery/main.c) # (libcascade-gallery.so: examples/android packs it)
  else()
    add_executable(cascade-gallery WIN32 examples/gallery/main.c)
  endif()
  cascade_app(cascade-gallery UI examples/gallery/ui)
  if(IOS)
    set_target_properties(cascade-gallery PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/examples/ios/Info.plist.in"
      MACOSX_BUNDLE_GUI_IDENTIFIER "org.cascade.gallery"
      MACOSX_BUNDLE_BUNDLE_NAME "cascade"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
      MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
      XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.cascade.gallery"
      XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
  endif()
endif()

if(CASCADE_BUILD_TESTS)
  enable_testing()
  add_executable(cascade-tests tests/tests.c)
  target_link_libraries(cascade-tests PRIVATE ${BT})
  add_test(NAME cascade-tests COMMAND cascade-tests)
  # the handle API, from plain C99
  add_executable(cascade-capi-tests tests/capi.c)
  target_link_libraries(cascade-capi-tests PRIVATE ${BT})
  set_target_properties(cascade-capi-tests PROPERTIES C_STANDARD 99)
  add_test(NAME cascade-capi-tests COMMAND cascade-capi-tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  # random changes and input, for the sanitizers to watch
  add_executable(cascade-stress tests/stress.c)
  target_link_libraries(cascade-stress PRIVATE ${BT})
  add_test(NAME cascade-stress COMMAND cascade-stress)
  # a presenter of the app's in a real window (it opens one with CASCADE_TEST_WINDOW)
  if(cascade_window)
    add_executable(cascade-window-tests tests/window.c)
    target_link_libraries(cascade-window-tests PRIVATE ${BT})
    add_test(NAME cascade-window-tests COMMAND cascade-window-tests)
  endif()
endif()

if(CASCADE_FUZZ)
  set(cascade_fuzz_index 0)
  foreach(cascade_fuzz_target html css image font zlib)
    set(cascade_fuzz_program cascade-fuzz-${cascade_fuzz_target})
    add_executable(${cascade_fuzz_program} tests/fuzz.c)
    target_link_libraries(${cascade_fuzz_program} PRIVATE ${BT})
    target_compile_definitions(${cascade_fuzz_program} PRIVATE CASCADE_FUZZ_TARGET=${cascade_fuzz_index})
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_definitions(${cascade_fuzz_program} PRIVATE CASCADE_LIBFUZZER=1)
      target_compile_options(${cascade_fuzz_program} PRIVATE -fsanitize=fuzzer)
      target_link_options(${cascade_fuzz_program} PRIVATE -fsanitize=fuzzer)
    endif()
    math(EXPR cascade_fuzz_index "${cascade_fuzz_index} + 1")
  endforeach()
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(${BT} PRIVATE -fsanitize=fuzzer-no-link) # (coverage, for the fuzzer to follow)
  endif()
endif()

# ---- installing: the library, its headers and a CMake package ----------------------------------------------
#   cmake --install build --prefix <dir>, then find_package(cascade) with <dir> in CMAKE_PREFIX_PATH

if(CASCADE_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  set(cascade_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/cascade)
  set(cascade_java_install_dir ${CMAKE_INSTALL_DATADIR}/cascade/android/java)
  install(TARGETS ${BT} EXPORT cascadeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT cascadeTargets NAMESPACE cascade:: DESTINATION ${cascade_cmake_dir})
  configure_package_config_file(cmake/cascadeConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/cascadeConfig.cmake
    INSTALL_DESTINATION ${cascade_cmake_dir} PATH_VARS cascade_java_install_dir)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/cascadeConfigVersion.cmake COMPATIBILITY SameMajorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cascadeConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/cascadeConfigVersion.cmake
    cmake/cascadeApp.cmake DESTINATION ${cascade_cmake_dir})
  install(FILES LICENSE README.md CHANGELOG.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
  # cascade.pc, for builds without CMake (pkg-config --cflags --libs cascade): the library is
  # static, so what it links is in Libs
  list(REMOVE_DUPLICATES cascade_pc_libs)
  string(REPLACE ";" " " cascade_pc_libs "${cascade_pc_libs}")
  string(REPLACE ";" " " cascade_pc_cflags "${cascade_pc_cflags}")
  set(cascade_pc_name ${BT})
  # the prefix from where the file is (${pcfiledir}): the same file works wherever --prefix put it
  file(RELATIVE_PATH cascade_pc_up "/p/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/p")
  string(REGEX REPLACE "/$" "" cascade_pc_up "${cascade_pc_up}")
  configure_file(cmake/cascade.pc.in ${CMAKE_CURRENT_BINARY_DIR}/cascade.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cascade.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
  if(ANDROID AND cascade_window)
    install(DIRECTORY src/platform/android/java/ DESTINATION ${cascade_java_install_dir})
  endif()
  if(CASCADE_C_SHARED)
    install(TARGETS cascade_c
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  if(CASCADE_BUILD_TOOLS)
    install(TARGETS cascade-render RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()
