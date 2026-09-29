"""Build docs/panels.png from frames rendered by the real ESP32 display.

Capture (needs pyserial and the board flashed with SPLASH_CAPTURE_ENABLED=1):

    PLATFORMIO_BUILD_FLAGS="-D SPLASH_CAPTURE_ENABLED=1" pio run -d firmware -t upload
    python firmware/tools/build_gallery.py --capture --port COM3

Compose from frames that were captured earlier:

    python firmware/tools/build_gallery.py

Check that the published image matches this edition (used by CI):

    python firmware/tools/build_gallery.py --check

The capture uses synthetic fixture data, so nothing from a real network,
credential or server appears in the published image. Flash the normal firmware
again after capturing.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "docs/panels.png"
FRAMES = ROOT / "firmware/.pio/gallery-frames"
FONT_BOLD = ROOT / "firmware/assets/fonts/Aileron-Bold.otf"

BG = "#03070a"
BORDER = "#34505f"
TEXT = "#f1f5f8"
CYAN = "#3dc4f3"
GREEN = "#35d493"

PANEL_W, PANEL_H = 320, 240
COLUMNS = 4
MARGIN = 36
GAP_X = 18
GAP_Y = 28
HEADER = 116
FOOTER = 34

PAGE_COUNT = {"combined": 17, "rpi5": 12, "proxmox": 10}
LABEL = {"combined": "RPi5 + PROXMOX", "rpi5": "RASPBERRY PI 5", "proxmox": "PROXMOX VE"}


def font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONT_BOLD), size)


def edition_name() -> str:
    return (ROOT / "EDITION").read_text(encoding="ascii").strip()


def version() -> str:
    return (ROOT / "VERSION").read_text(encoding="ascii").strip()


def image_size(pages: int) -> tuple[int, int]:
    rows = (pages + COLUMNS - 1) // COLUMNS
    width = MARGIN * 2 + COLUMNS * PANEL_W + (COLUMNS - 1) * GAP_X
    height = HEADER + rows * PANEL_H + (rows - 1) * GAP_Y + FOOTER
    return width, height


def capture(port_name: str, frames: Path, expected: int) -> None:
    """Save one 320x240 frame per page as drawn by the firmware."""
    import serial
    from check_panels import fixture, send_json
    from check_splashes import capture_command, device_info

    frames.mkdir(parents=True, exist_ok=True)
    for stale in frames.glob("page-*.png"):
        stale.unlink()
    with serial.Serial(port_name, 460800, timeout=20) as port:
        port.dtr = False
        port.rts = True
        time.sleep(0.1)
        port.rts = False
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if b"CAPTURE READY" in port.readline():
                break
        else:
            raise SystemExit("La placa no responde: carga el firmware con SPLASH_CAPTURE_ENABLED=1")
        pages = device_info(port)["pages"]
        if pages != expected:
            raise SystemExit(f"La placa tiene {pages} paneles y esta edición espera {expected}")
        send_json(port, fixture())
        for page in range(pages):
            # page, subpage, brightness, width, theme, mode (1 = normal panel)
            width, height, colors, pixels = capture_command(port, f"P {page} 0 6 {PANEL_W} 0 1")
            if (width, height) != (PANEL_W, PANEL_H) or len(set(colors)) < 8:
                raise SystemExit(f"Captura incorrecta del panel {page + 1}")
            Image.frombytes("RGB", (width, height), bytes(pixels)).save(frames / f"page-{page:02d}.png")
    print(f"{pages} paneles capturados en {frames}")


def compose(frames: Path, expected: int) -> Image.Image:
    files = sorted(frames.glob("page-*.png"))
    if len(files) != expected:
        raise SystemExit(f"Se necesitan {expected} capturas en {frames} y hay {len(files)}; usa --capture")
    edition = edition_name()
    width, height = image_size(len(files))
    image = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(image)
    draw.text((MARGIN, 28), "CYD MONITOR", font=font(30), fill=TEXT)
    draw.text((MARGIN, 68), f"EDICION {LABEL[edition]}  ·  VERSION {version()}", font=font(13), fill=CYAN)
    draw.text((width - MARGIN, 69), f"{len(files)} PANELES", font=font(12), fill=GREEN, anchor="ra")
    for index, path in enumerate(files):
        x = MARGIN + (index % COLUMNS) * (PANEL_W + GAP_X)
        y = HEADER + (index // COLUMNS) * (PANEL_H + GAP_Y)
        image.paste(Image.open(path).convert("RGB"), (x, y))
        draw.rounded_rectangle((x - 1, y - 1, x + PANEL_W, y + PANEL_H), radius=3, outline=BORDER, width=1)
    return image


def check(expected: int) -> None:
    if not OUTPUT.exists():
        raise SystemExit("Falta docs/panels.png; ejecuta build_gallery.py --capture")
    with Image.open(OUTPUT) as published:
        size = published.size
        published.verify()
    if size != image_size(expected):
        raise SystemExit(
            f"docs/panels.png mide {size[0]}x{size[1]} y no corresponde a los {expected} paneles "
            f"de la edición {edition_name()}; ejecuta build_gallery.py --capture"
        )
    print(f"Galería {edition_name()} verificada: {OUTPUT.relative_to(ROOT)}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--capture", action="store_true", help="captura los paneles desde la placa por USB")
    parser.add_argument("--port", default="COM3", help="puerto serie de la placa (por defecto COM3)")
    parser.add_argument("--frames", type=Path, default=FRAMES, help="carpeta de capturas")
    parser.add_argument("--check", action="store_true", help="comprueba la imagen publicada")
    args = parser.parse_args()
    expected = PAGE_COUNT[edition_name()]
    if args.check:
        check(expected)
        return
    if args.capture:
        sys.path.insert(0, str(Path(__file__).parent))
        capture(args.port, args.frames, expected)
    image = compose(args.frames, expected)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    image.save(OUTPUT, optimize=True)
    print(f"Galería {edition_name()} creada: {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
