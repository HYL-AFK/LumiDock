"""Apply a visible static rain layer to the 320x80 RGB565 rain scene."""

from pathlib import Path


WIDTH = 320
HEIGHT = 80
RAIN_BLUE = 0x398C


def put_pixel(data: bytearray, x: int, y: int) -> None:
    if x < 0 or x >= WIDTH or y < 0 or y >= HEIGHT:
        return
    offset = (y * WIDTH + x) * 2
    data[offset] = RAIN_BLUE & 0xFF
    data[offset + 1] = RAIN_BLUE >> 8


def main() -> None:
    output = Path(__file__).resolve().parents[1] / "main/assets/ui/weather_rain_scene.rgb565"
    data = bytearray(output.read_bytes())
    if len(data) != WIDTH * HEIGHT * 2:
        raise ValueError(f"unexpected rain-scene size: {len(data)}")
    for center_x in (55, 74, 93):
        for row in range(48, 66):
            put_pixel(data, center_x - (row - 48) // 7, row)
            put_pixel(data, center_x + 1 - (row - 48) // 7, row)
    output.write_bytes(data)


if __name__ == "__main__":
    main()
