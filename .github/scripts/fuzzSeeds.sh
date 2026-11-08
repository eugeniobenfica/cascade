#!/bin/sh
# Seeds for a fuzz target's corpus: fuzzSeeds.sh <html|css|image|font|zlib> <folder>. They come from
# the repository (the tests' pages and fixtures, the docs' pictures, what crashed a target before, in
# tests/corpus/<target>) and a system font.
set -e
target=$1
dir=$2
mkdir -p "$dir"
if [ -d "tests/corpus/$target" ]; then cp "tests/corpus/$target"/* "$dir/"; fi

# the arrays of tests/testFixtures.inc whose names match $1, each as a file
fixtures() {
  python3 - "$dir" "$1" <<'EOF'
import re, sys
out, wanted = sys.argv[1], sys.argv[2]
for name, body in re.findall(r"unsigned char (\w+)\[\] = \{([0-9,]*)\}", open("tests/testFixtures.inc").read()):
    if re.match(wanted, name):
        open(f"{out}/{name}", "wb").write(bytes(int(b) for b in body.split(",")))
EOF
}

case "$target" in
html)
  cp tests/*.html examples/gallery/ui/index.html docs/assets/src/hero.html "$dir/"
  ;;
css)
  cp examples/gallery/ui/style.css "$dir/"
  printf '#a{display:grid;grid-template:1fr/1fr;color:oklch(50%% .1 30)}' > "$dir/grid.css"
  printf '@media (min-width:1px){a:has(b):not(.c)::before{content:counter(x) attr(y);transform:rotate3d(1,0,0,1rad)}}' > "$dir/selectors.css"
  printf '@keyframes f{from{opacity:0}50%%{width:calc(1px + 2%%)}}@container (width>1px){a{animation:f 1s}}' > "$dir/rules.css"
  ;;
image)
  cp docs/assets/heroAurora.png docs/assets/screens/*.png "$dir/"
  fixtures 'kJpeg'
  ;;
font)
  fixtures 'k(Pixels|Sbix|Cbdt)Font'
  cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf "$dir/"
  ;;
zlib)
  printf '\170\234\003\000\000\000\000\001' > "$dir/empty"
  python3 -c "import sys, zlib; open(sys.argv[1], 'wb').write(zlib.compress(open('README.md', 'rb').read()))" "$dir/readme"
  ;;
*)
  echo "no target $target" >&2
  exit 1
  ;;
esac
