# What programs made with cascade use, from the source tree (add_subdirectory) or from an installed
# cascade (find_package): cascade_app and cascade_release_link.

# cascade_release_link(<target>): release links of programs and libraries made with cascade leave
# out the parts of cascade the program does not use, and the symbols too (a third to a half
# lighter).
function(cascade_release_link target)
  set(release "$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>") # (CMake 3.16: one configuration per $<CONFIG>)
  if(MSVC)
    target_link_options(${target} PRIVATE "$<${release}:/OPT:REF>" "$<${release}:/OPT:ICF>")
  elseif(APPLE)
    target_link_options(${target} PRIVATE "$<${release}:-Wl,-dead_strip>")
  else()
    target_link_options(${target} PRIVATE "$<${release}:-Wl,--gc-sections>" "$<${release}:-s>")
  endif()
endfunction()

# cascade_app(<target> [UI <folder>]): links cascade into a program and puts its interface folder
# next to it (copied again whenever a file in it changes), so that cscdViewLoadFile("ui/index.html")
# finds it wherever the program is started from. Debug builds read the interface from the source
# folder instead, where live reload sees the files you edit. MinGW programs get their runtime linked
# in, so they also start outside the MinGW shell; MSVC's WIN32 programs start at main. On iOS the target is an app bundle (the folder goes
# into it); on Android it is the shared library cascade's activity loads, and the folder goes into
# the APK's assets when it is packaged (examples/android).
function(cascade_app target)
  cmake_parse_arguments(BA "" "UI" "" ${ARGN})
  target_link_libraries(${target} PRIVATE cascade::cascade)
  set(source_dir "${CMAKE_CURRENT_SOURCE_DIR}")
  if(BA_UI)
    get_filename_component(ui "${BA_UI}" ABSOLUTE)
    get_filename_component(source_dir "${ui}" DIRECTORY) # (the folder that holds the interface folder)
  endif()
  target_compile_definitions(${target} PRIVATE "CASCADE_APP_SOURCE_DIR=\"${source_dir}\"")
  if(MINGW)
    target_link_options(${target} PRIVATE -static)
  endif()
  cascade_release_link(${target})
  get_target_property(gui ${target} WIN32_EXECUTABLE)
  if(MSVC AND gui)
    target_link_options(${target} PRIVATE /ENTRY:mainCRTStartup) # (a WIN32 program with main, as on MinGW)
  endif()
  if(IOS)
    set_target_properties(${target} PROPERTIES MACOSX_BUNDLE ON)
  endif()
  if(BA_UI AND NOT ANDROID)
    get_filename_component(name "${ui}" NAME)
    file(GLOB_RECURSE ui_files CONFIGURE_DEPENDS "${ui}/*")
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}-${name}.stamp")
    add_custom_command(OUTPUT "${stamp}"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${ui}" "$<TARGET_FILE_DIR:${target}>/${name}"
      COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
      DEPENDS ${ui_files}
      COMMENT "Copying ${name} next to ${target}"
      VERBATIM)
    add_custom_target(${target}-${name} ALL DEPENDS "${stamp}")
    add_dependencies(${target}-${name} ${target}) # (after it: the copy goes where it is built)
  endif()
endfunction()
