#!/bin/sh
# Regenerates data/screenshots/*.png (run from the repo root, needs a Wayland session).
# Needs a configured build dir: meson setup build
set -e
ninja -C build
tmp=$(mktemp -d)
mkdir -p "$tmp/c2048"
cc -O1 -o "$tmp/shot" tools/screenshot.c build/resources.c -Ibuild \
   $(pkg-config --cflags --libs libadwaita-1) -lm -Wno-unused
export XDG_DATA_HOME="$tmp"
sed 's/^difficulty=.*/difficulty=hard/' tools/screenshot-state.ini > "$tmp/c2048/state.ini"
"$tmp/shot" data/screenshots/game.png
sed 's/^difficulty=.*/difficulty=evil/' tools/screenshot-state.ini > "$tmp/c2048/state.ini"
"$tmp/shot" data/screenshots/evil.png dark
rm -rf "$tmp"
