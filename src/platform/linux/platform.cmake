# Linux and the BSDs: Wayland and X11, chosen at run time (windowLinux.c). Wayland's libraries
# (libwayland-client, libxkbcommon, libwayland-cursor) are opened at run time, so it is always built;
# X11 when the Xlib headers are there (libx11-dev on Debian and Ubuntu, libX11-devel on Fedora).
# AT-SPI (screen readers) speaks D-Bus itself, on both.
set(cascade_platform_name "Linux")
set(cascade_platform_tier 1)
set(cascade_platform_vulkan ON)
set(cascade_platform_opengl OFF) # (GLX, with X11 below)

target_sources(${BT} PRIVATE
  ${cascade_platform_dir}/windowLinux.c
  ${cascade_platform_dir}/windowWayland.c)
if(CASCADE_ACCESSIBILITY)
  target_sources(${BT} PRIVATE ${cascade_platform_dir}/a11yAtspi.c)
endif()
target_compile_definitions(${BT} PRIVATE CASCADE_WAYLAND=1)
target_link_libraries(${BT} PUBLIC ${CMAKE_DL_LIBS})
if(CMAKE_DL_LIBS)
  list(APPEND cascade_pc_libs -l${CMAKE_DL_LIBS})
endif()
find_library(CASCADE_RT_LIB rt) # (shm_open: in the C library itself since glibc 2.34)
if(CASCADE_RT_LIB)
  target_link_libraries(${BT} PUBLIC rt)
  list(APPEND cascade_pc_libs -lrt)
endif()
find_package(X11)
if(X11_FOUND)
  target_sources(${BT} PRIVATE ${cascade_platform_dir}/windowX11.c ${cascade_platform_dir}/presentX11.c)
  target_compile_definitions(${BT} PRIVATE CASCADE_X11=1)
  # libX11 only; libGL, libvulkan and libXcursor (themed cursors) are opened at run time
  target_link_libraries(${BT} PUBLIC X11::X11)
  list(APPEND cascade_pc_libs -lX11)
  set(cascade_platform_opengl ON)
  set(cascade_uses_x11 ON)
  string(APPEND cascade_platform_name " (Wayland, X11)")
else()
  string(APPEND cascade_platform_name " (Wayland)")
  message(STATUS "cascade: no Xlib headers (libx11-dev / libX11-devel): the window is Wayland's only")
endif()
