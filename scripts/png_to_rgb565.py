"""Convert non-interlaced 8-bit RGB/RGBA PNG files to little-endian RGB565."""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path


def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def decode_png(path: Path) -> tuple[int, int, bytes, int]:
    raw = path.read_bytes()
    if raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG")
    pos = 8
    width = height = color_type = bit_depth = None
    data = bytearray()
    while pos < len(raw):
        length = struct.unpack(">I", raw[pos : pos + 4])[0]
        kind = raw[pos + 4 : pos + 8]
        value = raw[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type, compression, filter_type, interlace = struct.unpack(">IIBBBBB", value)
            if bit_depth != 8 or color_type not in (2, 6) or compression or filter_type or interlace:
                raise ValueError(f"unsupported PNG format in {path}")
        elif kind == b"IDAT":
            data.extend(value)
        elif kind == b"IEND":
            break
    if width is None or height is None or color_type is None:
        raise ValueError(f"missing PNG header in {path}")
    channels = 4 if color_type == 6 else 3
    stride = width * channels
    packed = zlib.decompress(data)
    rows = bytearray(height * stride)
    previous = bytearray(stride)
    offset = 0
    for row in range(height):
        filter_type = packed[offset]
        offset += 1
        current = bytearray(packed[offset : offset + stride])
        offset += stride
        for index in range(stride):
            left = current[index - channels] if index >= channels else 0
            up = previous[index]
            up_left = previous[index - channels] if index >= channels else 0
            if filter_type == 1:
                current[index] = (current[index] + left) & 0xFF
            elif filter_type == 2:
                current[index] = (current[index] + up) & 0xFF
            elif filter_type == 3:
                current[index] = (current[index] + ((left + up) >> 1)) & 0xFF
            elif filter_type == 4:
                current[index] = (current[index] + paeth(left, up, up_left)) & 0xFF
            elif filter_type != 0:
                raise ValueError(f"unsupported PNG row filter {filter_type}")
        rows[row * stride : (row + 1) * stride] = current
        previous = current
    return width, height, bytes(rows), channels


def main() -> None:
    source, destination = map(Path, sys.argv[1:3])
    width, height, pixels, channels = decode_png(source)
    output = bytearray()
    for offset in range(0, len(pixels), channels):
        red, green, blue = pixels[offset : offset + 3]
        color = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
        output.extend(struct.pack("<H", color))
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(output)
    print(f"{source.name}: {width}x{height} -> {destination.name} ({len(output)} bytes)")


if __name__ == "__main__":
    main()
