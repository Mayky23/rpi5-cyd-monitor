"""Create a private, responsive RGB565 splash header for the ESP32 firmware."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps


ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "firmware/include/custom_splash.local.h"
PREVIEWS = ROOT / "firmware/assets/splashes"
FONT = ROOT / "firmware/assets/fonts/Aileron-Bold.otf"


def parse_color(value: str) -> tuple[int, int, int]:
    value = value.strip().lstrip("#")
    if len(value) != 6:
        raise argparse.ArgumentTypeError("El color debe tener formato RRGGBB")
    try:
        return tuple(int(value[index:index + 2], 16) for index in (0, 2, 4))
    except ValueError as exc:
        raise argparse.ArgumentTypeError("El color contiene caracteres invalidos") from exc


def generated_artwork(title: str, subtitle: str, accent: tuple[int, int, int]) -> Image.Image:
    image = Image.new("RGB", (1200, 1200), (3, 9, 15))
    draw = ImageDraw.Draw(image)
    for y in range(image.height):
        factor = y / (image.height - 1)
        color = tuple(round(3 + (component - 3) * factor * 0.18) for component in accent)
        draw.line((0, y, image.width, y), fill=color)
    for radius, alpha in ((390, 38), (300, 54), (215, 72)):
        color = tuple(round(component * alpha / 255) for component in accent)
        draw.ellipse((600-radius, 500-radius, 600+radius, 500+radius), outline=color, width=8)
    for index in range(9):
        x = 80 + index * 132
        draw.line((x, 90, x, 910), fill=tuple(component // 5 for component in accent), width=3)
    title_font = ImageFont.truetype(str(FONT), 112)
    subtitle_font = ImageFont.truetype(str(FONT), 42)
    title_box = draw.textbbox((0, 0), title, font=title_font)
    subtitle_box = draw.textbbox((0, 0), subtitle, font=subtitle_font)
    draw.text(((1200 - (title_box[2] - title_box[0])) / 2, 430), title,
              font=title_font, fill=(245, 248, 250))
    draw.text(((1200 - (subtitle_box[2] - subtitle_box[0])) / 2, 575), subtitle,
              font=subtitle_font, fill=accent)
    return image


def prepare(image: Image.Image, size: tuple[int, int], fit: str,
            background: tuple[int, int, int]) -> Image.Image:
    image = ImageOps.exif_transpose(image).convert("RGB")
    if fit == "cover":
        result = ImageOps.fit(image, size, Image.Resampling.LANCZOS, centering=(0.5, 0.5))
    else:
        fitted = ImageOps.contain(image, size, Image.Resampling.LANCZOS)
        result = Image.new("RGB", size, background)
        result.paste(fitted, ((size[0] - fitted.width) // 2, (size[1] - fitted.height) // 2))
    return result.quantize(colors=48, method=Image.Quantize.MEDIANCUT,
                           dither=Image.Dither.NONE).convert("RGB")


def encode(image: Image.Image, prefix: str) -> str:
    runs: list[int] = []
    rows: list[int] = []
    for y in range(image.height):
        rows.append(len(runs))
        pixels = []
        for r, g, b in (image.getpixel((x, y)) for x in range(image.width)):
            pixels.append(((r & 248) << 8) | ((g & 252) << 3) | (b >> 3))
        x = 0
        while x < len(pixels):
            end = x + 1
            while end < len(pixels) and pixels[end] == pixels[x] and end - x < 65535:
                end += 1
            runs.extend((end - x, pixels[x]))
            x = end
    rows.append(len(runs))

    def array(kind: str, name: str, values: list[int]) -> str:
        lines = [", ".join(str(value) for value in values[index:index + 16])
                 for index in range(0, len(values), 16)]
        return f"static const {kind} {name}[] PROGMEM = {{\n  " + ",\n  ".join(lines) + "\n};\n"

    return array("uint32_t", f"{prefix}Rows", rows) + array("uint16_t", f"{prefix}Runs", runs)


def main() -> None:
    parser = argparse.ArgumentParser(description="Genera la pantalla PERSONAL del firmware")
    parser.add_argument("--source", type=Path, help="Imagen PNG o JPG")
    parser.add_argument("--title", default="MY MONITOR", help="Titulo para crear un diseno")
    parser.add_argument("--subtitle", default="SYSTEM DASHBOARD", help="Subtitulo del diseno")
    parser.add_argument("--accent", type=parse_color, default=parse_color("35C7F3"))
    parser.add_argument("--background", type=parse_color, default=parse_color("02070B"))
    parser.add_argument("--fit", choices=("cover", "contain"), default="cover")
    args = parser.parse_args()

    if args.source:
        artwork = Image.open(args.source)
    else:
        artwork = generated_artwork(args.title[:24], args.subtitle[:36], args.accent)

    PREVIEWS.mkdir(parents=True, exist_ok=True)
    sections = [
        "#pragma once\n#include <Arduino.h>\n#include <algorithm>\n",
        "namespace CustomSplashAsset {\nstatic constexpr bool AVAILABLE = true;\n",
    ]
    for width, height, name in ((240, 320, "Portrait"), (320, 240, "Landscape")):
        image = prepare(artwork, (width, height), args.fit, args.background)
        image.save(PREVIEWS / f"custom-preview-{width}x{height}.png")
        sections.append(encode(image, name))
    sections.append(
        "inline void draw(Arduino_GFX &display, int width, int height, int offset) {\n"
        "  const uint32_t *rows = width > height ? LandscapeRows : PortraitRows;\n"
        "  const uint16_t *runs = width > height ? LandscapeRuns : PortraitRuns;\n"
        "  const int first = std::max(0, -offset);\n"
        "  const int last = std::min(height, int(display.height()) - offset);\n"
        "  for (int y = first; y < last; ++y) {\n"
        "    int x = 0;\n"
        "    const uint32_t end = pgm_read_dword(rows + y + 1);\n"
        "    for (uint32_t i = pgm_read_dword(rows + y); i < end; i += 2) {\n"
        "      const uint16_t count = pgm_read_word(runs + i);\n"
        "      display.drawFastHLine(x, y + offset, count, pgm_read_word(runs + i + 1));\n"
        "      x += count;\n"
        "    }\n"
        "  }\n"
        "}\n}  // namespace CustomSplashAsset\n"
    )
    OUTPUT.write_text("".join(sections), encoding="ascii")
    print(f"Pantalla personalizada creada: {OUTPUT}")
    print(f"Vistas previas: {PREVIEWS / 'custom-preview-240x320.png'} y custom-preview-320x240.png")


if __name__ == "__main__":
    main()
