# Cross-compile moonlight-embedded for the Miniloong Pocket 1 (aarch64 / RK3566)
# with the pinned mlp1-toolchain image. Only ever read inside that image, so
# every path below is a container path.
#
# CMAKE_SYSTEM_PROCESSOR=aarch64 makes upstream build its AArch64 path rather
# than a portable one; the sysroot supplies SDL2 2.28.5 (the same version the
# device ships), ALSA, OpenSSL 3 and libcurl, plus the cross-built additions
# the MLP1 SDK does not carry (libudev, libevdev, libuuid, opus, ffmpeg).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(MLP1_TOOLCHAIN_PREFIX "aarch64-buildroot-linux-gnu" CACHE STRING "Cross tool prefix")
set(MLP1_SYSROOT "/opt/mlp1-toolchain/aarch64-buildroot-linux-gnu/sysroot" CACHE PATH "Target sysroot")

set(CMAKE_C_COMPILER "${MLP1_TOOLCHAIN_PREFIX}-gcc")
set(CMAKE_CXX_COMPILER "${MLP1_TOOLCHAIN_PREFIX}-g++")
set(CMAKE_SYSROOT "${MLP1_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${MLP1_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Everything here targets the device: never let a host package satisfy a probe.
set(PKG_CONFIG_EXECUTABLE "/opt/mlp1-toolchain/bin/pkg-config" CACHE FILEPATH "")
set(PKG_CONFIG_SYSROOT_DIR "${MLP1_SYSROOT}" CACHE PATH "")
set(PKG_CONFIG_LIBDIR "${MLP1_SYSROOT}/usr/lib/pkgconfig" CACHE PATH "")
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF CACHE BOOL "")
