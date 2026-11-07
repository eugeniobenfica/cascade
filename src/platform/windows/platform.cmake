# Windows: Win32, WGL and GDI, UI Automation. System DLLs only: the window, GDI for the software path,
# the IME, OLE for drops from other apps, the shell (links); UI Automation (and OLE automation's
# strings) for screen readers.
set(cascade_platform_name "Windows")
set(cascade_platform_tier 1)
set(cascade_platform_vulkan ON)
set(cascade_platform_opengl ON)

target_sources(${BT} PRIVATE
  ${cascade_platform_dir}/windowWin32.c
  ${cascade_platform_dir}/presentWin32.c)
target_link_libraries(${BT} PUBLIC user32 gdi32 imm32 ole32 oleaut32 shell32 uuid)
list(APPEND cascade_pc_libs -luser32 -lgdi32 -limm32 -lole32 -loleaut32 -lshell32 -luuid)
if(CASCADE_ACCESSIBILITY)
  target_sources(${BT} PRIVATE ${cascade_platform_dir}/a11yWin32.c)
  target_link_libraries(${BT} PUBLIC uiautomationcore)
  list(APPEND cascade_pc_libs -luiautomationcore)
endif()
