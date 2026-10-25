#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# One-shot: build the render harness and render all presets/examples to PNG.
#   firmware/test/host/render/render.sh [out-dir]   (default: <repo>/.tmp/render)
# Renders twice: the built-in 5x7 fallback font and, when FreeType and a Noto
# Sans CJK font are available, a 12 px CJK approximation of the embedded font.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
OUT=${1:-$REPO/.tmp/render}
BUILD=$REPO/.tmp/host-render
cmake -S "$REPO/firmware/test/host" -B "$BUILD" -DEEGO_HOST_RENDER=ON -DEEGO_HOST_SANITIZE=OFF >/dev/null
cmake --build "$BUILD" --target dashboard_render -j >/dev/null
python3 "$HERE/make_fixtures.py" "$OUT/fixtures"
"$BUILD/render/dashboard_render" "$OUT/fixtures" "$OUT" --font fallback || STATUS=$?
FONT=${EEGO_RENDER_FONT:-/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc}
if [ -f "$FONT" ]; then
  "$BUILD/render/dashboard_render" "$OUT/fixtures" "$OUT" --font "$FONT" --px "${EEGO_RENDER_PX:-12}" || STATUS=$?
fi
exit ${STATUS:-0}
