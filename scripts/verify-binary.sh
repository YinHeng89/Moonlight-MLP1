#!/usr/bin/env bash
# Check that the moonlight binary can run on the MLP1 as packaged. Runs inside
# the toolchain image (needs the cross readelf).
#
#   verify-binary.sh <binary> <device-libs.txt> <glibc-ceiling>
#
# Fails unless the binary is AArch64 and stripped, carries no RPATH/RUNPATH,
# needs no glibc symbol version above the ceiling, and every NEEDED library is
# listed in the device allowlist (libraries the MLP1 itself provides; this pak
# bundles none).
set -euo pipefail

BINARY="${1:?usage: verify-binary.sh <binary> <device-libs.txt> <glibc-ceiling>}"
ALLOWLIST="${2:?usage: verify-binary.sh <binary> <device-libs.txt> <glibc-ceiling>}"
CEILING="${3:?usage: verify-binary.sh <binary> <device-libs.txt> <glibc-ceiling>}"
READELF="${CROSS:-aarch64-buildroot-linux-gnu}-readelf"

fail() { echo "FAIL $*" >&2; exit 1; }

"$READELF" -h "$BINARY" | grep -q 'Machine:.*AArch64' || fail "not an AArch64 ELF: $BINARY"
echo "ok   AArch64 ELF"

if "$READELF" -S "$BINARY" | grep -q '\.symtab'; then
  fail "binary is not stripped"
fi
echo "ok   stripped"

if "$READELF" -d "$BINARY" | grep -E '\((RPATH|RUNPATH)\)' | grep -v '\[\]'; then
  fail "binary carries a non-empty RPATH/RUNPATH"
fi
echo "ok   no RPATH/RUNPATH"

highest="$("$READELF" -V "$BINARY" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | sort -uV | tail -n 1)"
[ -n "$highest" ] || fail "no GLIBC symbol versions found"
if [ "$(printf '%s\n%s\n' "$highest" "$CEILING" | sort -V | tail -n 1)" != "$CEILING" ]; then
  fail "needs GLIBC_$highest, device provides $CEILING"
fi
echo "ok   highest glibc symbol GLIBC_$highest (ceiling $CEILING)"

allowed="$(grep -v '^[[:space:]]*#' "$ALLOWLIST" | awk 'NF {print $1}')"
missing=0
echo "NEEDED:"
while read -r lib; do
  [ -n "$lib" ] || continue
  if printf '%s\n' "$allowed" | grep -Fxq "$lib"; then
    echo "  $lib"
  else
    echo "  $lib   <- not provided by the device" >&2
    missing=1
  fi
done < <("$READELF" -d "$BINARY" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
[ "$missing" -eq 0 ] || fail "binary needs libraries the MLP1 does not provide"
echo "ok   every NEEDED library is on the device allowlist"
