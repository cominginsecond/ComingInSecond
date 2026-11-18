#!/usr/bin/env bash
# Regenerates the MetaModule PNG assets from the VCV SVGs. Needs rsvg-convert
# (MSYS2: pacman -S mingw-w64-x86_64-librsvg). MetaModule panels are 240 px tall
# for 128.5 mm, so every graphic is rendered at 240/128.5 px per mm.
set -euo pipefail
cd "$(dirname "$0")"
RES=../res
OUT=assets
mkdir -p "$OUT/keys"

PX_PER_MM=$(awk 'BEGIN { print 240 / 128.5 }')
px() { awk -v mm="$1" -v k="$PX_PER_MM" 'BEGIN { printf "%d", mm * k + 0.5 }'; }

# Faceplate: opaque background, exactly 240 px tall. The code loads res/QuantizerSequencer.svg,
# which the SDK maps to assets/QuantizerSequencer.png.
# Uses the MetaModule-only faceplate (light gray body), not the VCV panel.
rsvg-convert -h 240 -b "#d4d4d4" faceplate.svg -o "$OUT/QuantizerSequencer.png"

# Key frames, sized to the key widgets (white 9.5 x 7 mm, black 5.8 x 4.4 mm).
for k in white-off white-on; do
	rsvg-convert -w "$(px 9.5)" -h "$(px 7)" "$RES/keys/$k.svg" -o "$OUT/keys/$k.png"
done
for k in black-off black-on; do
	rsvg-convert -w "$(px 5.8)" -h "$(px 4.4)" "$RES/keys/$k.svg" -o "$OUT/keys/$k.png"
done
echo "Assets written to $OUT/"
