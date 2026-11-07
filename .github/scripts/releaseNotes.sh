#!/bin/sh
# The CHANGELOG's entry for a version ("1.2.3"), without its heading: the release's notes.
set -e
cd "$(dirname "$0")/../.."
awk -v v="$1" '
  /^## \[/ { if (on) exit; if (index($0, "## [" v "]") == 1) { on = 1; next } }
  on { print }
' CHANGELOG.md | sed -e '/./,$!d'
