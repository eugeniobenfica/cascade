# Android: the NDK, and cascade's activity in Java (java/, which the app's APK carries:
# CASCADE_ANDROID_JAVA_DIR). Vulkan is opened at run time; TalkBack goes through the activity.
set(cascade_platform_name "Android")
set(cascade_platform_tier 2)
set(cascade_platform_vulkan ON)
set(cascade_platform_opengl OFF)

target_sources(${BT} PRIVATE ${cascade_platform_dir}/windowAndroid.c)
target_link_libraries(${BT} PUBLIC android log ${CMAKE_DL_LIBS})
set_target_properties(${BT} PROPERTIES POSITION_INDEPENDENT_CODE ON) # (it goes into the app's shared library)
# what the activity calls: nothing in the app's library does, Java does
target_link_options(${BT} INTERFACE
  "-Wl,-u,Java_org_cascade_CascadeActivity_nativeCreate"
  "-Wl,-u,Java_org_cascade_CascadeActivity_nativeSurface"
  "-Wl,-u,Java_org_cascade_CascadeActivity_nativeEvent"
  "-Wl,-z,max-page-size=16384") # (16 KB pages: what Google Play asks of apps for newer devices)
list(APPEND cascade_pc_libs -landroid -llog -l${CMAKE_DL_LIBS}
  -Wl,-u,Java_org_cascade_CascadeActivity_nativeCreate -Wl,-u,Java_org_cascade_CascadeActivity_nativeSurface
  -Wl,-u,Java_org_cascade_CascadeActivity_nativeEvent -Wl,-z,max-page-size=16384)
set(CASCADE_ANDROID_JAVA_DIR "${cascade_platform_dir}/java" CACHE INTERNAL "cascade's activity (Java sources)")
