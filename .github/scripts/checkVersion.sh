#!/bin/sh
# The version is written in one place, CMakeLists.txt's project(); everything that repeats it must
# agree: the README's version badge and download links, the CHANGELOG's entry, the Rust
# bindings, the vcpkg port, and the tag being released (its first argument, "v1.2.3", when there is one).
set -e
cd "$(dirname "$0")/../.."
version=$(sed -n 's/^project(cascade VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$version" ] || { echo "no version in CMakeLists.txt"; exit 1; }
fail=0
check() { # what, found
  if [ "$2" != "$version" ]; then echo "$1 says '$2', CMakeLists.txt says $version"; fail=1; else echo "$1: $version"; fi
}
header() { sed -n "s/^#define CSCD_VERSION_$1 \([0-9]*\).*/\1/p" include/cascade/core.h; }
check "include/cascade/core.h" "$(header MAJOR).$(header MINOR).$(header PATCH)"
check "README.md's badge" "$(sed -n 's/.*badge\/version-\([0-9.]*\)-.*/\1/p' README.md | head -n 1)"
check "CHANGELOG.md" "$(sed -n 's/^## \[\([0-9.]*\)\].*/\1/p' CHANGELOG.md | head -n 1)"
check "bindings/rust/Cargo.toml" "$(sed -n 's/^version = "\([0-9.]*\)"/\1/p' bindings/rust/Cargo.toml | head -n 1)"
check "packaging/vcpkg/ports/cascade/vcpkg.json" "$(sed -n 's/^ *"version": "\([0-9.]*\)",*/\1/p' packaging/vcpkg/ports/cascade/vcpkg.json | head -n 1)"
for link in $(grep -o 'releases/download/v[0-9.]*/cascade[a-z-]*-[0-9.]*[0-9]' README.md | sort -u); do
  check "README.md's link $link (tag)" "$(echo "$link" | sed 's|.*download/v\([0-9.]*\)/.*|\1|')"
  check "README.md's link $link (file)" "$(echo "$link" | sed 's/.*-//')"
done
for tag in $(grep -oh 'GIT_TAG v[0-9.]*[0-9]' docs/*.md | sed 's/GIT_TAG v//' | sort -u); do
  check "docs' GIT_TAG" "$tag"
done
if [ -n "$1" ]; then check "the tag" "${1#v}"; fi
exit $fail
