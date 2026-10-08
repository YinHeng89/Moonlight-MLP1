#!/usr/bin/env bash
# Runs inside the pinned MLP1 toolchain image. Produces a stripped, device-ready
# moonlight executable plus the verification report.
#
#   /build/moonlight        pristine source copied in from the host checkout
#   /build/ml-build         CMake tree (cached)
#   /build/out              the stripped binary and its report
set -euo pipefail

export PATH=/opt/mlp1-toolchain/bin:$PATH
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"
CROSS=aarch64-buildroot-linux-gnu
SYSROOT=/opt/mlp1-toolchain/aarch64-buildroot-linux-gnu/sysroot

SRC=/build/moonlight
BUILD=/build/ml-build
OUT=/build/out
HOST_SRC=/host/thirdparty/moonlight-embedded

log() { echo "build-moonlight: $*"; }

# Fresh copy every run so a stale generated file from a previous configure
# cannot answer a question this one asked differently.
rm -rf "$SRC"
mkdir -p "$SRC" "$OUT"
tar -C "$HOST_SRC" --exclude=.git -cf - . | tar -C "$SRC" -xf -

rm -rf "$BUILD"

# cmake/generate_version_header.cmake stamps the build by running git, and only
# inside an if(GIT_FOUND AND IS_DIRECTORY .git). The vendored tree has no .git,
# so both stamp variables would come out empty -- and configuration.h.in wraps
# GIT_COMMIT_HASH in #cmakedefine, which turns an empty value into an #undef
# that src/main.c then prints as an undeclared identifier. Supply the stamp
# ourselves from the pinned commit instead: the version string then names the
# exact upstream source this binary came from, which is more useful than
# whatever the local checkout happened to be at, and it makes the build
# independent of whether git is installed.
LOCK=/host/scripts/upstream.lock.json
SOURCE_COMMIT="$(sed -n 's/.*"source_commit": *"\([0-9a-f]*\)".*/\1/p' "$LOCK" | head -1)"
[ -n "$SOURCE_COMMIT" ] || { log "cannot read source_commit from $LOCK"; exit 1; }

log "configuring for MLP1 (SDL video/audio/input, ffmpeg software decode)"

# -lz is the one collateral the static archives do not carry themselves:
# libcurl.a decompresses gzip responses and static OpenSSL can too, and cmake's
# FindCURL hands back just the archive so nothing else would add it. The device
# ships libz.so.1, which device-libs.txt has already confirmed.
#
# OpenSSL, expat and libcurl are told to use their static archives while SDL2,
# ALSA and zlib stay shared: those three are on the device, so forcing ".a
# everywhere" would silently replace the device's own Wayland-capable SDL2 with
# the toolchain's build.
#
# ffmpeg's avutil asks for -latomic for its 64-bit counters. It is static-copied
# at the end of the link line, where it can still satisfy them, so the device
# does not need libatomic.so.1 -- which is not on the confirmed list. That goes
# in STANDARD_LIBRARIES rather than EXE_LINKER_FLAGS because the latter lands
# before the archives that need it and would be discarded unresolved.
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/host/scripts/mlp1-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_SDL=ON \
  -DENABLE_FFMPEG=ON \
  -DENABLE_X11=OFF \
  -DENABLE_PULSE=OFF \
  -DENABLE_CEC=OFF \
  -DOPENSSL_USE_STATIC_LIBS=ON \
  -DCURL_LIBRARY="$SYSROOT/usr/lib/libcurl.a" \
  -DEXPAT_LIBRARY="$SYSROOT/usr/lib/libexpat.a" \
  -DCMAKE_C_FLAGS="-O2 -pipe -mcpu=cortex-a55" \
  -DGIT_COMMIT_HASH="${SOURCE_COMMIT:0:7}" \
  -DGIT_BRANCH="mlp1" \
  -DCMAKE_EXE_LINKER_FLAGS="-lz" \
  -DCMAKE_C_STANDARD_LIBRARIES="-Wl,--push-state -Wl,-Bstatic -latomic -Wl,--pop-state" \
  2>&1 | tail -40

log "compiling"
cmake --build "$BUILD" -j"$(nproc)" 2>&1 | tail -40

BIN="$BUILD/moonlight"
[ -x "$BIN" ] || { log "no moonlight executable was produced"; exit 1; }

"$CROSS-strip" --strip-unneeded -o "$OUT/moonlight" "$BIN"
log "stripped -> $OUT/moonlight ($(stat -c %s "$OUT/moonlight") bytes)"

log "verifying the binary"
bash /host/scripts/verify-binary.sh "$OUT/moonlight" /host/pak/device-libs.txt "${GLIBC_CEILING:-2.38}" \
  | tee "$OUT/verify-binary.txt"

# The pak's own notice program: the fullscreen message that stands in for the
# terminal the MLP1 does not have, most visibly the pairing PIN. It is this
# repository's source, not upstream's, and links only SDL2 and SDL_ttf, both
# device-provided.
log "compiling the notice program"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
"$CROSS-gcc" -O2 -std=c11 -Wall -Wextra \
  $(pkg-config --cflags sdl2 SDL2_ttf) \
  -o "$OUT/moonlight-notice-raw" /host/scripts/notice.c \
  $(pkg-config --libs sdl2 SDL2_ttf)
"$CROSS-strip" --strip-unneeded -o "$OUT/moonlight-notice" "$OUT/moonlight-notice-raw"
rm -f "$OUT/moonlight-notice-raw"
log "verifying the notice program"
bash /host/scripts/verify-binary.sh "$OUT/moonlight-notice" /host/pak/device-libs.txt "${GLIBC_CEILING:-2.38}" \
  | tee "$OUT/verify-notice.txt"
