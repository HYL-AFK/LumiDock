"""Scale an RGB565 icon and make its white canvas transparent for LVGL."""

from __future__ import annotations

import struct
import sys
from pathlib import Path


def rgb565_to_rgb888(pixel: int) -> tuple[int, int, int]:
    return (
        ((pixel >> 11) & 0x1F) * 255 // 31,
        ((pixel >> 5) & 0x3F) * 255 // 63,
        (pixel & 0x1F) * 255 // 31,
    )


def main() -> None:
    source, destination, source_size, target_size = sys.argv[1:5]
    source_path = Path(source)
    destination_path = Path(destination)
    source_side = int(source_size)
    target_side = int(target_size)
    raw = source_path.read_bytes()
    expected_size = source_side * source_side * 2
    if len(raw) != expected_size:
        raise ValueError(f"{source_path} must contain {expected_size} bytes, got {len(raw)}")

    colors = bytearray()
    alpha = bytearray()
    for y in range(target_side):
        source_y = y * source_side // target_side
        for x in range(target_side):
            source_x = x * source_side // target_side
            offset = (source_y * source_side + source_x) * 2
            pixel = struct.unpack_from("<H", raw, offset)[0]
            red, green, blue = rgb565_to_rgb888(pixel)
            colors.extend(struct.pack("<H", pixel))
            alpha.append(0 if red >= 248 and green >= 248 and blue >= 248 else 255)

    destination_path.write_bytes(colors + alpha)
    print(f"{source_path.name}: {source_side}px -> {target_side}px transparent RGB565A8")


if __name__ == "__main__":
    main()
