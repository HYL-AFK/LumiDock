"""Convert an RGBA PNG to the LVGL RGB565A8 color-map plus alpha-map format."""

from __future__ import annotations

import struct
import sys
from pathlib import Path

from png_to_rgb565 import decode_png


def main() -> None:
    source, destination = map(Path, sys.argv[1:3])
    width, height, pixels, channels = decode_png(source)
    color_map = bytearray()
    alpha_map = bytearray()
    for offset in range(0, len(pixels), channels):
        red, green, blue = pixels[offset : offset + 3]
        alpha = pixels[offset + 3] if channels == 4 else 255
        color = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
        color_map.extend(struct.pack("<H", color))
        alpha_map.append(alpha)
    destination.write_bytes(color_map + alpha_map)
    print(f"{source.name}: {width}x{height} -> {destination.name} ({len(color_map) + len(alpha_map)} bytes)")


if __name__ == "__main__":
    main()
