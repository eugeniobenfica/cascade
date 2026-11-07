# macOS: Cocoa (Objective-C with ARC), NSOpenGLContext, Core Animation, NSAccessibility. Metal comes in
# through MoltenVK when it is installed (Vulkan, opened at run time).
set(cascade_platform_name "macOS")
set(cascade_platform_tier 1)
set(cascade_platform_vulkan ON)
set(cascade_platform_opengl ON)

enable_language(OBJC)
set(cascade_objc ${cascade_platform_dir}/windowCocoa.m)
if(CASCADE_ACCESSIBILITY)
  list(APPEND cascade_objc ${cascade_platform_dir}/a11yCocoa.m)
endif()
target_sources(${BT} PRIVATE ${cascade_objc})
# (NSOpenGLContext is deprecated but still works everywhere)
set_source_files_properties(${cascade_objc} PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wno-deprecated-declarations")
target_link_libraries(${BT} PUBLIC "-framework AppKit" "-framework QuartzCore")
list(APPEND cascade_pc_libs "-framework AppKit" "-framework QuartzCore")
