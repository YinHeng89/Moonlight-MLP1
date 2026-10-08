#!/usr/bin/env bash
# Probe: can cross-compiled aarch64 binaries run natively in this image?
set -uo pipefail
export PATH=/opt/mlp1-toolchain/bin:$PATH
export PKG_CONFIG_SYSROOT_DIR=/opt/mlp1-toolchain/aarch64-buildroot-linux-gnu/sysroot
export PKG_CONFIG_LIBDIR=/opt/mlp1-toolchain/aarch64-buildroot-linux-gnu/sysroot/usr/lib/pkgconfig
S=/opt/mlp1-toolchain/aarch64-buildroot-linux-gnu/sysroot
RUNNER="$S/lib/ld-linux-aarch64.so.1 --library-path $S/lib:$S/usr/lib"

cat > /tmp/t.c <<'EOF'
#include <stdio.h>
int main(void){ printf("hello native aarch64 OK\n"); return 0; }
EOF
aarch64-buildroot-linux-gnu-gcc -O2 /tmp/t.c -o /tmp/t.elf
echo "--- static-linked C test ---"
$RUNNER /tmp/t.elf

cat > /tmp/s.c <<'EOF'
#include <SDL2/SDL.h>
#include <stdio.h>
int main(void){
#ifdef SDL_VIDEO_DRIVER_WAYLAND
  printf("SDL wayland macro defined = %d\n", SDL_VIDEO_DRIVER_WAYLAND);
#else
  printf("SDL wayland macro NOT defined\n");
#endif
  printf("linked SDL header version %d.%d.%d\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
  return 0;
}
EOF
aarch64-buildroot-linux-gnu-gcc -O2 $(pkg-config --cflags sdl2) /tmp/s.c -o /tmp/s.elf $(pkg-config --libs sdl2)
echo "--- dynamic SDL2 test ---"
$RUNNER /tmp/s.elf

echo "--- pkg-config sanity ---"
for p in sdl2 SDL2_ttf libcurl openssl opus zlib libdrm expat; do
  if pkg-config --exists "$p" 2>/dev/null; then
    echo "  OK   $p -> $(pkg-config --modversion "$p")"
  else
    echo "  MISS $p"
  fi
done
