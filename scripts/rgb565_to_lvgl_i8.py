"""Convert an opaque little-endian RGB565 image into an LVGL I8 image map.

The generated map begins with LVGL's 256-entry ARGB8888 palette followed by
one palette index per pixel. It keeps the weather artwork in Flash
without adding an image decoder or allocating a full-screen runtime buffer.
"""

from __future__ import annotations

import struct
import sys
from collections import Counter
from pathlib import Path


Color = tuple[int, int, int]


def rgb565_to_rgb(value: int) -> Color:
    return (
        ((value >> 11) & 0x1F) * 255 // 31,
        ((value >> 5) & 0x3F) * 255 // 63,
        (value & 0x1F) * 255 // 31,
    )


def box_score(entries: list[tuple[Color, int]]) -> int:
    if len(entries) < 2:
        return 0
    channels = list(zip(*(color for color, _ in entries)))
    spread = max(max(channel) - min(channel) for channel in channels)
    return spread * sum(count for _, count in entries)


def split_box(entries: list[tuple[Color, int]]) -> tuple[list[tuple[Color, int]], list[tuple[Color, int]]]:
    channels = list(zip(*(color for color, _ in entries)))
    axis = max(range(3), key=lambda index: max(channels[index]) - min(channels[index]))
    ordered = sorted(entries, key=lambda item: item[0][axis])
    half = sum(count for _, count in ordered) / 2
    cumulative = 0
    for index, (_, count) in enumerate(ordered[:-1], start=1):
        cumulative += count
        if cumulative >= half:
            return ordered[:index], ordered[index:]
    midpoint = len(ordered) // 2
    return ordered[:midpoint], ordered[midpoint:]


def representative(entries: list[tuple[Color, int]]) -> Color:
    total = sum(count for _, count in entries)
    return tuple(sum(color[channel] * count for color, count in entries) // total for channel in range(3))


def make_palette(colors: Counter[Color]) -> list[Color]:
    boxes = [list(colors.items())]
    while len(boxes) < 256:
        index = max(range(len(boxes)), key=lambda item: box_score(boxes[item]))
        if box_score(boxes[index]) == 0:
            break
        left, right = split_box(boxes[index])
        boxes[index:index + 1] = [left, right]
    palette = [representative(entries) for entries in boxes]
    palette.extend([(0, 0, 0)] * (256 - len(palette)))
    return palette


def nearest_palette_index(color: Color, palette: list[Color]) -> int:
    return min(
        range(len(palette)),
        key=lambda index: 2 * (color[0] - palette[index][0]) ** 2
        + 4 * (color[1] - palette[index][1]) ** 2
        + (color[2] - palette[index][2]) ** 2,
    )


def main() -> None:
    if len(sys.argv) != 5:
        raise SystemExit('usage: rgb565_to_lvgl_i8.py <source> <destination> <width> <height>')
    source, destination = map(Path, sys.argv[1:3])
    width, height = map(int, sys.argv[3:5])
    raw = source.read_bytes()
    if len(raw) != width * height * 2:
        raise ValueError(f'{source} is not {width}x{height} RGB565 data')
    pixels = [rgb565_to_rgb(value) for (value,) in struct.iter_unpack('<H', raw)]
    palette = make_palette(Counter(pixels))
    cache: dict[Color, int] = {}
    indices = bytearray()
    for pixel in pixels:
        index = cache.get(pixel)
        if index is None:
            index = nearest_palette_index(pixel, palette)
            cache[pixel] = index
        indices.append(index)
    output = bytearray()
    for red, green, blue in palette:
        output.extend(struct.pack('<I', 0xFF000000 | red << 16 | green << 8 | blue))
    output.extend(indices)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(output)
    print(f'{source.name}: {width}x{height} -> {destination.name} ({len(output)} bytes)')


if __name__ == '__main__':
    main()
