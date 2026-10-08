#!/usr/bin/env bash
# Cross-build the dependencies the MLP1 toolchain sysroot does not provide.
#
#   libudev  (eudev)     -> needed to link moonlight's udev input module
#   libevdev             -> needed to link moonlight's evdev input module
#   libuuid  (util-linux)-> libgamestream unique-id generation
#   opus                 -> moonlight audio codec
#   ffmpeg (avcodec/avutil only) -> moonlight software video decode
#
# Everything installs into the toolchain sysroot, which is what a real SDK
# would ship: pkg-config and CMake then find it with no extra path plumbing.
# Static-only, so the finished binary carries no runtime dependency on them.
set -euo pipefail

export PATH=/opt/mlp1-toolchain/bin:$PATH
export HOME=/root
unset MAKEFLAGS

# Host tools, not target ones. The base image ships a cross compiler and little
# else: eudev's build runs gperf over its device-property tables, and eudev and
# util-linux both want m4/bison/flex for their generated parsers. Without these
# the first dependency fails in a fresh container, which is exactly the
# situation CI and a first-time clone are in. Install them only when missing,
# so a container that already has them costs nothing.
if ! command -v gperf >/dev/null 2>&1; then
  echo "installing host build tools (gperf, m4, bison, flex)"
  DEBIAN_FRONTEND=noninteractive apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gperf m4 bison flex pkg-config
fi
for t in gperf m4 bison flex; do
  command -v "$t" >/dev/null 2>&1 || { echo "missing host tool: $t" >&2; exit 1; }
done

CROSS=aarch64-buildroot-linux-gnu
SYSROOT=/opt/mlp1-toolchain/aarch64-buildroot-linux-gnu/sysroot
PREFIX="$SYSROOT/usr"

# RK3566: quad Cortex-A55. Use -mcpu so NEON and the crypto extensions this
# SoC has are available; anything stricter than the silicon would be speculation.
export CC=$CROSS-gcc CXX=$CROSS-g++ AR=$CROSS-ar RANLIB=$CROSS-ranlib LD=$CROSS-ld STRIP=$CROSS-strip
export CFLAGS="-O2 -pipe -mcpu=cortex-a55 -fPIC"
export CXXFLAGS="$CFLAGS"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"

SRC=/build/src
mkdir -p "$SRC" /build/logs
cd "$SRC"

VER_libevdev=1.13.3
VER_eudev=3.2.14
VER_util_linux=2.40.4
VER_opus=1.5.2
VER_expat=2.6.1
VER_curl=8.6.0
VER_ffmpeg=6.1.2

fetch() { # fetch <file> <url>
  [ -f "$1" ] || { echo "fetch $2"; curl -fsSL -m 300 -o "$1.part" "$2" && mv "$1.part" "$1"; }
}

extract() { # extract <tarball> <expected-dir>
  [ -d "$2" ] || { echo "extract $1"; tar xf "$1"; }
}

log() { echo; echo "===== $* ====="; }

CONF_BASE="--host=$CROSS --prefix=$PREFIX --disable-shared --enable-static"

# ---------------------------------------------------------------- eudev
log "eudev $VER_eudev"
fetch eudev-$VER_eudev.tar.gz \
  https://github.com/eudev-project/eudev/releases/download/v$VER_eudev/eudev-$VER_eudev.tar.gz
extract eudev-$VER_eudev.tar.gz eudev-$VER_eudev
cd eudev-$VER_eudev
if [ ! -f .built ]; then
  ./configure $CONF_BASE \
    --sbindir="$PREFIX/bin" --libdir="$PREFIX/lib" \
    --disable-selinux --disable-blkid --disable-kmod --disable-gudev \
    --disable-introspection --disable-manpages --disable-hwdb \
    --enable-split-usr=no 2>&1 | tail -5
  make -j"$(nproc)" 2>&1 | tail -5
  make install 2>&1 | tail -5
  touch .built
fi
cd "$SRC"

# ---------------------------------------------------------------- libevdev
log "libevdev $VER_libevdev"
fetch libevdev-$VER_libevdev.tar.xz \
  https://www.freedesktop.org/software/libevdev/libevdev-$VER_libevdev.tar.xz
extract libevdev-$VER_libevdev.tar.xz libevdev-$VER_libevdev
cd libevdev-$VER_libevdev
if [ ! -f .built ]; then
  ./configure $CONF_BASE --disable-documentation 2>&1 | tail -5
  make -j"$(nproc)" 2>&1 | tail -5
  make install 2>&1 | tail -5
  touch .built
fi
cd "$SRC"

# ---------------------------------------------------------------- libuuid
log "util-linux $VER_util_linux (libuuid only)"
fetch util-linux-$VER_util_linux.tar.xz \
  https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v${VER_util_linux%.*}/util-linux-$VER_util_linux.tar.xz
extract util-linux-$VER_util_linux.tar.xz util-linux-$VER_util_linux
cd util-linux-$VER_util_linux
if [ ! -f .built ]; then
  ./configure $CONF_BASE \
    --disable-all-programs --enable-libuuid \
    --without-ncurses --without-readline --without-tinfo \
    --disable-bash-completion --disable-poman 2>&1 | tail -5
  make -j"$(nproc)" 2>&1 | tail -5
  make install 2>&1 | tail -5
  touch .built
fi
cd "$SRC"

# ---------------------------------------------------------------- opus
log "opus $VER_opus"
fetch opus-$VER_opus.tar.gz https://downloads.xiph.org/releases/opus/opus-$VER_opus.tar.gz
extract opus-$VER_opus.tar.gz opus-$VER_opus
cd opus-$VER_opus
if [ ! -f .built ]; then
  ./configure $CONF_BASE --disable-doc --disable-extra-programs --enable-fixed-point=no 2>&1 | tail -5
  make -j"$(nproc)" 2>&1 | tail -5
  make install 2>&1 | tail -5
  touch .built
fi
cd "$SRC"

# ---------------------------------------------------------------- expat
# Static only. The sysroot already ships a shared expat, but linking it would
# put libexpat.so.1 on the binary's NEEDED list and the device does not carry
# it; see the note at the top of this file about staying self-contained.
log "expat $VER_expat (static)"
fetch expat-$VER_expat.tar.xz \
  https://github.com/libexpat/libexpat/releases/download/R_${VER_expat//./_}/expat-$VER_expat.tar.xz
extract expat-$VER_expat.tar.xz expat-$VER_expat
cd expat-$VER_expat
if [ ! -f .built ]; then
  ./configure $CONF_BASE \
    --without-docbook --without-xmlwf --without-examples 2>&1 | tail -5
  make -j"$(nproc)" 2>&1 | tail -5
  make install 2>&1 | tail -5
  touch .built
fi
cd "$SRC"

# ---------------------------------------------------------------- curl
# Needed by libgamestream to talk HTTPS to the host. Static, with static
# OpenSSL, so the finished binary does not need libcurl.so on the device.
log "curl $VER_curl (static, static OpenSSL)"
fetch curl-$VER_curl.tar.xz https://curl.se/download/curl-$VER_curl.tar.xz
extract curl-$VER_curl.tar.xz curl-$VER_curl
cd curl-$VER_curl
if [ ! -f .built ]; then
  ./configure $CONF_BASE \
    --with-openssl="$SYSROOT/usr" \
    --with-zlib="$SYSROOT/usr" \
    --disable-ldap --disable-ldaps --disable-manual \
    --without-libpsl --without-nghttp2 --without-brotli --without-zstd \
    --without-libidn2 --without-librtmp --disable-sspi \
    --enable-http 2>&1 | tail -8
  make -j"$(nproc)" 2>&1 | tail -5
  make install 2>&1 | tail -5
  touch .built
fi
cd "$SRC"

# ---------------------------------------------------------------- ffmpeg
# Only libavcodec + libavutil, and only the two decoders Moonlight asks for by
# name (h264 / hevc). Rendering goes through SDL's YUV texture path, so no
# swscale, no avformat, no ffmpeg CLI.
log "ffmpeg $VER_ffmpeg (avcodec + avutil, h264/hevc only)"
fetch ffmpeg-$VER_ffmpeg.tar.xz https://ffmpeg.org/releases/ffmpeg-$VER_ffmpeg.tar.xz
extract ffmpeg-$VER_ffmpeg.tar.xz ffmpeg-$VER_ffmpeg
cd ffmpeg-$VER_ffmpeg
if [ ! -f .built ]; then
  ./configure \
    --prefix="$PREFIX" \
    --cross-prefix=$CROSS- \
    --enable-cross-compile \
    --arch=aarch64 --target-os=linux \
    --cc=$CROSS-gcc --cxx=$CROSS-g++ --ar=$CROSS-ar --ranlib=$CROSS-ranlib \
    --pkg-config=pkg-config \
    --pkg-config-flags="--define-prefix" \
    --extra-cflags="-O2 -pipe -mcpu=cortex-a55 -fPIC" \
    --disable-shared --enable-static --enable-pic \
    --disable-debug --disable-doc --disable-htmlpages --disable-manpages \
    --disable-gpl --disable-nonfree \
    --disable-everything \
    --enable-avcodec --enable-avutil \
    --enable-decoder=h264,hevc \
    --enable-parser=h264,hevc \
    --enable-pthreads --enable-neon \
    --disable-avformat --disable-avfilter --disable-avdevice \
    --disable-swresample --disable-swscale --disable-postproc \
    --disable-network --disable-programs 2>&1 | tail -25
  make -j"$(nproc)" 2>&1 | tail -15
  make install 2>&1 | tail -10
  touch .built
fi

# ffmpeg's own .pc files put -latomic in Libs, not Libs.private, so anything
# that asks pkg-config for libavcodec gets told to link the shared atomic
# library -- which the MLP1 does not ship, even though the handful of symbols
# avutil wants are just 64-bit counters a static libatomic.a answers for.
# Drop it here and let the final link take it from the archive instead.
# Idempotent, and outside the .built guard so a cache hit still applies it.
# If the .pc files are absent, make install did not land and there is nothing
# to fix up -- say so loudly instead of letting sed fail on a glob that matched
# nothing and letting a broken sysroot look like a finished one.
if ls "$PREFIX"/lib/pkgconfig/libav*.pc >/dev/null 2>&1; then
  sed -i 's/ -latomic//g' "$PREFIX"/lib/pkgconfig/libav*.pc
  echo "ffmpeg .pc Libs lines:"
  grep -h '^Libs:' "$PREFIX"/lib/pkgconfig/libav*.pc
else
  echo "no ffmpeg .pc files under $PREFIX/lib/pkgconfig -- install failed" >&2
  exit 1
fi

cd "$SRC"

log "dependency staging report"
for pc in libudev libevdev uuid opus expat libcurl libavcodec libavutil; do
  if pkg-config --exists "$pc"; then
    echo "  OK   $pc -> $(pkg-config --modversion "$pc")"
  else
    echo "  MISS $pc"
  fi
done
echo
echo "installed archives:"
ls -la "$PREFIX/lib" | grep -E "lib(udev|evdev|uuid|opus|expat|curl|avcodec|avutil)\.a" || true
echo
echo "DEPS DONE"
