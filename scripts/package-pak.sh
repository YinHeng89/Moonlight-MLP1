#!/usr/bin/env bash
# Assemble build/package/Moonlight.pak from the built binaries and pak/ sources.
# Only what the device needs at runtime goes in: no build tree, no sources.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$REPO_ROOT/build"
PACKAGE="$BUILD/package/Moonlight.pak"
DIST="$BUILD/dist"
OUT="$BUILD/mlp1"

[ -x "$OUT/moonlight" ] || { echo "no $OUT/moonlight -- run the build first" >&2; exit 1; }
[ -x "$OUT/moonlight-notice" ] || { echo "no $OUT/moonlight-notice -- run the build first" >&2; exit 1; }
[ -x "$OUT/moonlight-keyprobe" ] || { echo "no $OUT/moonlight-keyprobe -- run the build first" >&2; exit 1; }
[ -x "$OUT/moonlight-menu" ] || { echo "no $OUT/moonlight-menu -- run the build first" >&2; exit 1; }
[ -f "$REPO_ROOT/pak/res/icon.png" ] || { echo "no icon -- run scripts/make_icon.py" >&2; exit 1; }

rm -rf "$PACKAGE"
mkdir -p "$PACKAGE/res"

cp "$REPO_ROOT/pak/pak.json" "$PACKAGE/pak.json"
cp "$REPO_ROOT/pak/launch.sh" "$PACKAGE/launch.sh"
cp "$REPO_ROOT/pak/moonlight-user.conf" "$PACKAGE/moonlight-user.conf"
cp "$REPO_ROOT/pak/res/icon.png" "$PACKAGE/res/icon.png"
cp "$REPO_ROOT/thirdparty/moonlight-embedded/third_party/SDL_GameControllerDB/gamecontrollerdb.txt" \
   "$PACKAGE/res/gamecontrollerdb.txt"
cp "$OUT/moonlight" "$PACKAGE/moonlight"
cp "$OUT/moonlight-notice" "$PACKAGE/moonlight-notice"
cp "$OUT/moonlight-keyprobe" "$PACKAGE/moonlight-keyprobe"
cp "$OUT/moonlight-menu" "$PACKAGE/moonlight-menu"
cp "$OUT/verify-binary.txt" "$PACKAGE/verify-binary.txt" 2>/dev/null || true
cp "$OUT/verify-notice.txt" "$PACKAGE/verify-notice.txt" 2>/dev/null || true
cp "$OUT/verify-keyprobe.txt" "$PACKAGE/verify-keyprobe.txt" 2>/dev/null || true
cp "$OUT/verify-menu.txt" "$PACKAGE/verify-menu.txt" 2>/dev/null || true

chmod 755 "$PACKAGE/launch.sh" "$PACKAGE/moonlight" "$PACKAGE/moonlight-notice" "$PACKAGE/moonlight-keyprobe" "$PACKAGE/moonlight-menu"

# Licence: moonlight-embedded is GPL-3.0-or-later, and the corresponding source
# for this exact binary is reproducible with the scripts in scripts/.
cp "$REPO_ROOT/thirdparty/moonlight-embedded/LICENSE" "$PACKAGE/LICENSE-moonlight-embedded.txt"

# Byte-identical mtimes so an archive built twice from the same inputs matches.
if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
  find "$PACKAGE" -print0 | xargs -0 touch -d "@$SOURCE_DATE_EPOCH"
fi

echo "packaged $PACKAGE"
du -sh "$PACKAGE"
echo
echo "contents:"
find "$PACKAGE" -type f -printf '%10s  %P\n' | sort -k2
