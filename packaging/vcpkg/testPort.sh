#!/bin/sh
# Builds the vcpkg port from this checkout (in place of the release's sources) and a program that
# uses it. CI runs it; locally: VCPKG_ROOT=<a vcpkg clone> sh packaging/vcpkg/testPort.sh
set -eu
root=$(cd "$(dirname "$0")/../.." && (pwd -W 2>/dev/null || pwd)) # (on Windows a path CMake reads: C:/...)
vcpkg="${VCPKG_ROOT:?set VCPKG_ROOT to a vcpkg clone}/vcpkg"
work=$(mktemp -d)
mkdir -p "$work/ports"
cp -R "$root/packaging/vcpkg/ports/cascade" "$work/ports/"
# the sources: this checkout, not the release's
awk -v src="$root" '
  /^vcpkg_from_github\(/ { skip = 1; print "set(SOURCE_PATH \"" src "\")"; next }
  skip && /^\)/ { skip = 0; next }
  !skip' "$root/packaging/vcpkg/ports/cascade/portfile.cmake" > "$work/ports/cascade/portfile.cmake"
"$vcpkg" install cascade --overlay-ports="$work/ports" --x-install-root="$work/installed" ${VCPKG_TRIPLET:+--triplet "$VCPKG_TRIPLET"}

mkdir -p "$work/app/ui"
printf '<h1>hi</h1>' > "$work/app/ui/index.html"
cat > "$work/app/CMakeLists.txt" <<'CM'
cmake_minimum_required(VERSION 3.16)
project(app C)
find_package(cascade CONFIG REQUIRED)
add_executable(app main.c)
cascade_app(app UI ui)
CM
printf '#include "cascade/cascade.h"\nint main(void) { CscdView* v = cscdViewNew(); cscdViewFree(v); return 0; }\n' > "$work/app/main.c"
cmake -S "$work/app" -B "$work/app/build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_INSTALLED_DIR="$work/installed" \
  ${VCPKG_TRIPLET:+-DVCPKG_TARGET_TRIPLET="$VCPKG_TRIPLET"}
cmake --build "$work/app/build" --config Release
echo "vcpkg port: built and used"
