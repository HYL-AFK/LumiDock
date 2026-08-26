"""Generate RGB565 UI assets from the approved vector scenes and Lucide icon rasters."""

from __future__ import annotations

import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EDGE = Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe")


def png_to_rgb565(source: Path, destination: Path) -> None:
    subprocess.run(["python", str(ROOT / "scripts" / "png_to_rgb565.py"), str(source), str(destination)], check=True)


def render_svg(source: Path, destination: Path, width: int, height: int, transparent: bool = False) -> None:
    command = [
        str(EDGE), "--headless", "--no-sandbox", "--disable-gpu",
        f"--user-data-dir={ROOT / 'tmp' / ('edge_profile_' + source.stem)}", f"--window-size={width},{height}",
    ]
    if transparent:
        command.append("--default-background-color=00000000")
    command.extend([f"--screenshot={destination}", source.as_uri()])
    subprocess.run(command, check=True)


def main() -> None:
    ui = ROOT / "main" / "assets" / "ui"
    preview = ROOT / "tmp" / "runtime_asset_render"
    preview.mkdir(parents=True, exist_ok=True)
    main_scene = preview / "main_scene.png"
    render_svg(ui / "main_scene.svg", main_scene, 320, 170)
    png_to_rgb565(main_scene, ui / "main_scene.rgb565")
    for name in ("weather_clear_scene", "weather_cloudy_scene", "weather_fog_scene", "weather_rain_scene", "weather_snow_scene", "weather_thunder_scene"):
        png = preview / f"{name}.png"
        rgb565 = preview / f"{name}.rgb565"
        render_svg(ui / f"{name}.svg", png, 320, 170)
        png_to_rgb565(png, rgb565)
        subprocess.run(["python", str(ROOT / "scripts" / "rgb565_to_lvgl_i8.py"),
                        str(rgb565), str(ui / f"{name}.i8"), "320", "170"], check=True)


if __name__ == "__main__":
    main()
