"""Rebuild the original lab texture assets using only Python's standard library.

Runtime loads the resulting PNG files with stb_image; Python is not needed to run it.
Specular is a linear grayscale mask. Emissive contains colored luminous strips.
"""
from pathlib import Path
import math
import struct
import zlib

OUT = Path(__file__).resolve().parent / "textures"
SIZE = 256


def png(name, width, height, pixel):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.extend(max(0, min(255, round(c))) for c in pixel(x, y))
    OUT.mkdir(parents=True, exist_ok=True)
    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    data += chunk(b"IEND", b"")
    (OUT / name).write_bytes(data)


def brick(x, y):
    row = y // 64
    xx = (x + (row % 2) * 64) % 128
    if y % 64 < 5 or xx < 5:
        return (105, 101, 88)
    grain = 8 * math.sin(x * 1.7 + y * 2.3) + 5 * math.sin(x * 0.31 - y * 0.7)
    shade = 5 * math.sin((x // 128 + row) * 3.1)
    return (155 + grain + shade, 67 + grain * 0.6, 40 + grain * 0.35)


def seam(x, y):
    return x % 64 < 4 or y % 64 < 4


def strip(x, y):
    return 12 <= x % 64 < 18 and 13 <= y % 64 < 52


def metal(x, y):
    if seam(x, y):
        return (18, 24, 30)
    if strip(x, y):
        return (22, 102, 116)
    brush = 10 * math.sin(y * 2.2) + 3 * math.sin(x * 0.7 + y * 2.9)
    return (92 + brush, 110 + brush, 128 + brush)


def specular(x, y):
    # Seams and light strips are matte; only alternate panels reflect strongly.
    value = 0 if seam(x, y) or strip(x, y) else (230 if (x // 64 + y // 64) % 2 == 0 else 28)
    return (value, value, value)


def emissive(x, y):
    return (15, 190, 230) if strip(x, y) else (0, 0, 0)


def tiles(x, y):
    if seam(x, y):
        return (48, 51, 57)
    base = (224, 203, 147) if (x // 64 + y // 64) % 2 == 0 else (36, 109, 136)
    detail = 4 * math.sin(x * 0.6) * math.sin(y * 0.7)
    return tuple(v + detail for v in base)


if __name__ == "__main__":
    png("black.png", 1, 1, lambda x, y: (0, 0, 0))
    for name, pixels in [("brick", brick), ("metal", metal), ("metal_specular", specular),
                         ("metal_emissive", emissive), ("tiles", tiles)]:
        png(name + ".png", SIZE, SIZE, pixels)
    print(f"Wrote six PNG textures to {OUT}")
