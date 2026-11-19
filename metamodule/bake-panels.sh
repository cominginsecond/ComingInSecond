#!/usr/bin/env bash
# Renders the MetaModule faceplate PNGs (metamodule/assets/<slug>.png) with VCV Rack, so the
# panel labels and icons that the modules draw in code are baked into the image.
#
# On the MetaModule, every widget that draws itself becomes a "display" in the module's element
# list, so the modules skip their static labels and icons there (see PanelArt in
# src/house_style.hpp). These PNGs have them baked in instead.
#
# How it works: with COMINGINSECOND_BAKE_PANELS set, each module widget keeps only its panel and
# its PanelArt. Rack's screenshot mode (-t ZOOM) then renders every module of the plugin. The
# zoom makes the panel exactly 240 px tall, the MetaModule's faceplate height.
#
# Run from the repo root after `make dist`, and whenever a panel's layout or labels change:
#   metamodule/bake-panels.sh
# Windows only for now (Rack's plugins-win-x64 folder). Set RACK_EXE to use another Rack.
set -euo pipefail
cd "$(dirname "$0")/.."

RACK_EXE="${RACK_EXE:-/c/Program Files/VCV/Rack2Pro/Rack.exe}"
OUT=metamodule/assets
SLUG=$(sed -n -E 's/^  "slug": "([^"]+)".*/\1/p' plugin.json | head -1)
# Rack panels are 380 px tall at zoom 1, and MetaModule faceplates are 240 px tall. Rack rounds
# the screenshot size up, so aim just under 240.
ZOOM=$(awk 'BEGIN { print 239.9 / 380 }')

package=$(ls dist/*.vcvplugin 2>/dev/null | head -1 || true)
if [ -z "$package" ]; then
	echo "No dist/*.vcvplugin. Run 'make dist' first." >&2
	exit 1
fi

# A throwaway Rack user folder, so no other plugins, settings or autosaved patch get involved.
user=$(mktemp -d)
trap 'rm -rf "$user"' EXIT
mkdir -p "$user/plugins-win-x64"
cp "$package" "$user/plugins-win-x64/"

COMINGINSECOND_BAKE_PANELS=1 "$RACK_EXE" -u "$(cygpath -w "$user")" -t "$ZOOM"

shots="$user/screenshots/$SLUG"
if ! ls "$shots"/*.png >/dev/null 2>&1; then
	echo "Rack wrote no screenshots to $shots. See $user/log.txt." >&2
	trap - EXIT
	exit 1
fi
for png in "$shots"/*.png; do
	cp "$png" "$OUT/$(basename "$png")"
	echo "Baked $OUT/$(basename "$png")"
done
