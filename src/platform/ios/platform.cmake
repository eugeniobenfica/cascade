# iOS and iPadOS: UIKit (Objective-C with ARC), Core Animation, UITextInput, VoiceOver. mainUikit.m
# holds main, which starts UIKit and calls the program's (renamed by window.h).
set(cascade_platform_name "iOS")
set(cascade_platform_tier 2)
set(cascade_platform_vulkan OFF)
set(cascade_platform_opengl OFF)

enable_language(OBJC)
set(cascade_objc ${cascade_platform_dir}/windowUikit.m ${cascade_platform_dir}/mainUikit.m)
if(CASCADE_ACCESSIBILITY)
  list(APPEND cascade_objc ${cascade_platform_dir}/a11yUikit.m)
endif()
target_sources(${BT} PRIVATE ${cascade_objc})
set_source_files_properties(${cascade_objc} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(${BT} PUBLIC "-framework UIKit" "-framework QuartzCore" "-framework CoreGraphics" "-framework Foundation")
list(APPEND cascade_pc_libs "-framework UIKit" "-framework QuartzCore" "-framework CoreGraphics" "-framework Foundation")
