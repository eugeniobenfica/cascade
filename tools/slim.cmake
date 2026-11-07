# A copy of cascade with only what the platforms you build for need: no other platform's module,
# and no tests, tools, examples, bindings or docs unless asked for. The copy builds the same library
# (add_subdirectory, FetchContent from your own repository, a vendored folder).
#
#   cmake -DPLATFORMS=windows -DOUT=../cascade-windows -P tools/slim.cmake
#   cmake -DPLATFORMS="desktop" -DWITH="examples;tests" -DOUT=third_party/cascade -P tools/slim.cmake
#   cmake -DPLATFORMS=none -DOUT=../cascade-engine -P tools/slim.cmake   # the engine alone, no window
#
# PLATFORMS: windows, linux, macos, ios, android, or desktop (the first three), mobile (the last two),
#   all, none. WITH: examples, tests, tools, bindings, docs. OUT must not exist yet, or be empty.

cmake_minimum_required(VERSION 3.16) # (a script sets its policies too: if(... IN_LIST ...))

if(NOT DEFINED PLATFORMS OR NOT DEFINED OUT)
  message(FATAL_ERROR "usage: cmake -DPLATFORMS=<windows;linux;macos;ios;android|desktop|mobile|all|none> "
    "[-DWITH=<examples;tests;tools;bindings;docs>] -DOUT=<folder> -P tools/slim.cmake")
endif()

get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(out "${OUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(known windows linux macos ios android)
set(platforms "")
foreach(p IN LISTS PLATFORMS)
  string(TOLOWER "${p}" p)
  if(p STREQUAL "desktop")
    list(APPEND platforms windows linux macos)
  elseif(p STREQUAL "mobile")
    list(APPEND platforms ios android)
  elseif(p STREQUAL "all")
    list(APPEND platforms ${known})
  elseif(p STREQUAL "none")
  elseif(p IN_LIST known)
    list(APPEND platforms ${p})
  else()
    message(FATAL_ERROR "slim: unknown platform \"${p}\" (${known}, desktop, mobile, all, none)")
  endif()
endforeach()
list(REMOVE_DUPLICATES platforms)

set(extras examples tests tools bindings docs)
foreach(w IN LISTS WITH)
  if(NOT w IN_LIST extras)
    message(FATAL_ERROR "slim: unknown WITH \"${w}\" (${extras})")
  endif()
endforeach()

if(EXISTS "${out}")
  file(GLOB existing "${out}/*" "${out}/.*")
  if(existing)
    message(FATAL_ERROR "slim: ${out} is not empty")
  endif()
endif()
string(FIND "${out}/" "${root}/" inside)
if(inside EQUAL 0)
  message(FATAL_ERROR "slim: ${out} is inside the source tree")
endif()

# what building and testing leaves in the tree (.gitignore)
set(junk PATTERN "bin" EXCLUDE PATTERN "obj" EXCLUDE PATTERN "__pycache__" EXCLUDE
  PATTERN "target" EXCLUDE PATTERN "Cargo.lock" EXCLUDE)

# ---- the library: always ----------------------------------------------------------------------------------

file(MAKE_DIRECTORY "${out}/src")
file(COPY "${root}/CMakeLists.txt" "${root}/LICENSE" "${root}/README.md" "${root}/CHANGELOG.md"
  "${root}/cmake" "${root}/include" DESTINATION "${out}")
file(GLOB parts LIST_DIRECTORIES true "${root}/src/*") # (the engine's folders; the platforms below)
list(FILTER parts EXCLUDE REGEX "/platform$")
file(COPY ${parts} DESTINATION "${out}/src")

if(platforms)
  file(COPY "${root}/src/platform/common" DESTINATION "${out}/src/platform")
  set(gpu_platforms ${platforms})
  list(REMOVE_ITEM gpu_platforms ios) # (iOS presents through Core Animation only)
  if(gpu_platforms)
    file(COPY "${root}/src/platform/gpu" DESTINATION "${out}/src/platform")
  endif()
  foreach(p IN LISTS platforms)
    file(COPY "${root}/src/platform/${p}" DESTINATION "${out}/src/platform")
  endforeach()
endif()

# ---- what was asked for ----------------------------------------------------------------------------------

if("examples" IN_LIST WITH)
  file(COPY "${root}/examples/gallery" DESTINATION "${out}/examples")
  foreach(p ios android) # (how to package the gallery for phones)
    if(p IN_LIST platforms)
      file(COPY "${root}/examples/${p}" DESTINATION "${out}/examples")
    endif()
  endforeach()
endif()
if("tests" IN_LIST WITH)
  file(COPY "${root}/tests" DESTINATION "${out}")
endif()
if("tools" IN_LIST WITH)
  if("android" IN_LIST platforms)
    file(COPY "${root}/tools" DESTINATION "${out}" PATTERN "slim.cmake" EXCLUDE)
  else()
    file(COPY "${root}/tools" DESTINATION "${out}" PATTERN "slim.cmake" EXCLUDE
      PATTERN "android-*" EXCLUDE)
  endif()
endif()
if("bindings" IN_LIST WITH)
  file(COPY "${root}/bindings" DESTINATION "${out}" ${junk})
endif()
if("docs" IN_LIST WITH)
  set(skip "")
  foreach(p ios android) # (screenshots of the phones left out)
    if(NOT p IN_LIST platforms)
      list(APPEND skip PATTERN "${p}-*.png" EXCLUDE)
    endif()
  endforeach()
  file(COPY "${root}/docs" DESTINATION "${out}" ${skip})
endif()

if(platforms)
  string(REPLACE ";" ", " kept "${platforms}")
else()
  set(kept "none (the engine alone)")
endif()
message(STATUS "slim: cascade for ${kept} in ${out}")
