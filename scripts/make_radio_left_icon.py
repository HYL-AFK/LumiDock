"""Derive radio_next_left_icon.rgb565a8 as the exact horizontal mirror of the right icon.

Run this whenever radio_next_icon.rgb565a8 changes:

    python scripts/make_radio_left_icon.py

Why flip the committed bytes instead of rendering the mirrored SVG through a browser:
what the radio screen wants is "the icon already on the right, pointing the other way".
The committed radio_next_icon.rgb565a8 *is* what the screen shows, and flipping that
raster is exact by construction - the mirror axis falls on the pixel boundary between
column 15 and 16, so there is nothing to resample. A browser render would add a second
sampling step for no gain.

radio_next_left_icon.svg sits next to it as the vector source of truth: same path data
wrapped in transform="translate(1024,0) scale(-1,1)", which mirrors about viewBox x=512,
i.e. pixel x=16 in the 32 px render - the same axis as the flip below.

tests/test_audio_radio_and_volume_contract.ps1 re-checks the bytes of both files against
each other, so a one-sided regeneration cannot slip through.

The PNG writer is stdlib only (zlib + struct) because Pillow is not installed here; it
renders a side-by-side preview so the result can be eyeballed before it is flashed.
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "main" / "assets" / "ui"
SIZE = 32
SCALE = 6


def flip_horizontal(raw: bytes, size: int) -> bytes:
    """Mirror an RGB565A8 buffer: the RGB565 plane first, then the A8 plane."""
    plane = size * size * 2
    colors, alpha = raw[:plane], raw[plane:]
    out_colors = bytearray()
    out_alpha = bytearray()
    for y in range(size):
        row = colors[y * size * 2 : (y + 1) * size * 2]
        for x in range(size - 1, -1, -1):
            out_colors += row[x * 2 : x * 2 + 2]
        out_alpha += bytes(reversed(alpha[y * size : (y + 1) * size]))
    return bytes(out_colors) + bytes(out_alpha)


def write_png(path: Path, width: int, height: int, rows: list[bytes]) -> None:
    raw = b"".join(b"\x00" + row for row in rows)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def to_rgba(raw: bytes, size: int, background: tuple[int, int, int]) -> list[bytes]:
    """Decode RGB565A8 and composite it over a background, scaled up by SCALE."""
    plane = size * size * 2
    colors, alpha = raw[:plane], raw[plane:]
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            value = struct.unpack_from("<H", colors, (y * size + x) * 2)[0]
            r5, g6, b5 = (value >> 11) & 0x1F, (value >> 5) & 0x3F, value & 0x1F
            red, green, blue = (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)
            a = alpha[y * size + x]
            blended = tuple(
                (channel * a + back * (255 - a)) // 255
                for channel, back in zip((red, green, blue), background)
            )
            row += bytes((*blended, 255)) * SCALE
        rows.extend([bytes(row)] * SCALE)
    return rows


def main() -> None:
    right = (UI / "radio_next_icon.rgb565a8").read_bytes()
    left = flip_horizontal(right, SIZE)
    (UI / "radio_next_left_icon.rgb565a8").write_bytes(left)

    opaque = sum(1 for v in right[SIZE * SIZE * 2 :] if v == 255)
    clear = sum(1 for v in right[SIZE * SIZE * 2 :] if v == 0)
    print(f"right {len(right)} bytes, left {len(left)} bytes, size match: {len(right) == len(left)}")
    print(f"alpha: {opaque} opaque / {clear} clear / {SIZE * SIZE} total (transparency preserved)")
    # 自检：翻转两次必须回到原样，否则说明翻转本身写错了。
    print(f"flip(flip(right)) == right: {flip_horizontal(left, SIZE) == right}")

    background = (240, 240, 240)
    gap = 24 * SCALE
    width = SIZE * SCALE * 2 + gap * 3
    height = SIZE * SCALE + gap * 2
    blank = bytes((*background, 255)) * width
    rows = [blank] * gap
    right_rows = to_rgba(right, SIZE, background)
    left_rows = to_rgba(left, SIZE, background)
    for index in range(SIZE * SCALE):
        rows.append(bytes((*background, 255)) * gap + right_rows[index]
                    + bytes((*background, 255)) * gap + left_rows[index]
                    + bytes((*background, 255)) * gap)
    rows.extend([blank] * gap)

    preview = ROOT / "tmp" / "radio_icon_mirror_preview.png"
    write_png(preview, width, height, rows)
    print(f"preview: {preview} ({width}x{height}): left half = current right icon, right half = the mirror")


if __name__ == "__main__":
    main()
