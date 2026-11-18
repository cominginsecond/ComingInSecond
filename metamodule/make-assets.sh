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

# Faceplates: every panel in res/ (one per module), opaque background, exactly 240 px tall.
for panel in "$RES"/*.svg; do
	name=$(basename "$panel" .svg)
	rsvg-convert -h 240 -b black "$panel" -o "$OUT/$name.png"
done

# Key frames, sized to the key widgets (white 9.5 x 7 mm, black 5.8 x 4.4 mm).
for k in white-off white-on; do
	rsvg-convert -w "$(px 9.5)" -h "$(px 7)" "$RES/keys/$k.svg" -o "$OUT/keys/$k.png"
done
for k in black-off black-on; do
	rsvg-convert -w "$(px 5.8)" -h "$(px 4.4)" "$RES/keys/$k.svg" -o "$OUT/keys/$k.png"
done

# Silver knob faces (MetaModule only), each rendered at its own size in mm. The MetaModule
# rotates these images.
mkdir -p "$OUT/knobs"
for knob in "$RES"/knobs/*.svg; do
	name=$(basename "$knob" .svg)
	mm=$(sed -n -E 's/.*<svg[^>]* width="([0-9.]+)mm".*/\1/p' "$knob" | head -1)
	rsvg-convert -w "$(px "$mm")" -h "$(px "$mm")" "$knob" -o "$OUT/knobs/$name.png"
done

# Fader tracks, each rendered at its own width x height in mm.
mkdir -p "$OUT/sliders"
for track in "$RES"/sliders/*.svg; do
	name=$(basename "$track" .svg)
	w=$(sed -n -E 's/.*<svg[^>]* width="([0-9.]+)mm".*/\1/p' "$track" | head -1)
	h=$(sed -n -E 's/.*<svg[^>]* height="([0-9.]+)mm".*/\1/p' "$track" | head -1)
	rsvg-convert -w "$(px "$w")" -h "$(px "$h")" "$track" -o "$OUT/sliders/$name.png"
done

echo "Assets written to $OUT/"
