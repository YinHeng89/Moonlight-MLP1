#!/usr/bin/env python3
"""Draw the Moonlight pak icon: a crescent on the pak's dark plate.

Pure standard library -- nothing in the build environment has to provide Pillow
for this, and there is no gradient stroke to turn into moire noise. Supersampled
4x and boxed down so the crescent's edge is smooth.
"""
import struct
import sys
import zlib

SIZE = 512
SS = 4  # supersample factor
SS_SIZE = SIZE * SS

BG = (16, 19, 26)
MOON = (159, 208, 255)


class Surface:
    def __init__(self, w, h, color=(0, 0, 0)):
        self.w = w
        self.h = h
        self.px = bytearray(bytes(color) * (w * h))

    def set(self, x, y, color):
        if 0 <= x < self.w and 0 <= y < self.h:
            i = (y * self.w + x) * 3
            self.px[i:i + 3] = bytes(color)

    def get(self, x, y):
        i = (y * self.w + x) * 3
        return tuple(self.px[i:i + 3])


def rounded_plate(surf, radius, color):
    """Fill a rounded square, leaving everything outside it untouched."""
    n = surf.w
    r = radius
    for y in range(n):
        for x in range(n):
            # find the nearest point inside the rounded rect for this pixel
            cx = min(max(x, r), n - 1 - r)
            cy = min(max(y, r), n - 1 - r)
            dx = x - cx
            dy = y - cy
            if dx * dx + dy * dy <= r * r:
                surf.set(x, y, color)


def crescent_mask(surf, ox, oy, orad, ix, iy, irad):
    """Set the moon colour where inside the outer circle and outside the inner."""
    for y in range(surf.h):
        for x in range(surf.w):
            if (x - ox) ** 2 + (y - oy) ** 2 <= orad * orad and \
               (x - ix) ** 2 + (y - iy) ** 2 >= irad * irad:
                surf.set(x, y, MOON)


def downsample(src, factor):
    out = Surface(src.w // factor, src.h // factor)
    for y in range(out.h):
        for x in range(out.w):
            r = g = b = 0
            for dy in range(factor):
                for dx in range(factor):
                    c = src.get(x * factor + dx, y * factor + dy)
                    r += c[0]
                    g += c[1]
                    b += c[2]
            n = factor * factor
            out.set(x, y, (r // n, g // n, b // n))
    return out


def write_png(surf, path):
    raw = bytearray()
    for y in range(surf.h):
        raw.append(0)  # filter type 0
        start = y * surf.w * 3
        raw.extend(surf.px[start:start + surf.w * 3])

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + \
            struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", surf.w, surf.h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "res/icon.png"
    plate = Surface(SS_SIZE, SS_SIZE)
    rounded_plate(plate, SS_SIZE * 96 // SIZE, BG)

    # A crescent: one disc with a slightly smaller disc cut out of its upper
    # right. Offsets are in supersampled units.
    u = SS_SIZE // SIZE
    ox, oy, orad = 256 * u, 268 * u, 138 * u
    ix, iy, irad = 306 * u, 226 * u, 122 * u
    crescent_mask(plate, ox, oy, orad, ix, iy, irad)

    write_png(downsample(plate, SS), path)
    print("wrote", path)


if __name__ == "__main__":
    main()
