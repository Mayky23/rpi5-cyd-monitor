"""Validate every dashboard theme against its darkest and lightest surfaces."""
from pathlib import Path
import math
import re


SOURCE = Path(__file__).parents[1] / "src" / "main.cpp"
MINIMUM_RATIO = 4.5
MINIMUM_THEME_SEPARATION = 30


def rgb565(red, green, blue):
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def blend(foreground, background, alpha, steps=3):
    green = ((((foreground & 0x07E0) * alpha) + ((background & 0x07E0) * (steps - alpha))) // steps) & 0x07E0
    red = (((foreground >> 11) * alpha + (background >> 11) * (steps - alpha)) // steps) << 11
    blue = ((foreground & 31) * alpha + (background & 31) * (steps - alpha)) // steps
    return red | green | blue


def luminance(color):
    channels = ((color >> 11) / 31, ((color >> 5) & 63) / 63, (color & 31) / 31)
    linear = tuple(value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4 for value in channels)
    return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]


def contrast(first, second):
    bright, dark = sorted((luminance(first), luminance(second)), reverse=True)
    return (bright + 0.05) / (dark + 0.05)


def channels(color):
    return ((color >> 11) * 255 / 31, ((color >> 5) & 63) * 255 / 63, (color & 31) * 255 / 31)


def main():
    source = SOURCE.read_text(encoding="utf-8")
    table = source.split("static constexpr Theme THEMES[] = {", 1)[1].split("};", 1)[0]
    entries = re.findall(r'\{"([A-Z]+)",(.*?)\}\s*,?', table, re.DOTALL)
    assert entries, "No themes found"
    identities = {}
    for name, body in entries:
        triples = re.findall(r"rgb565\((\d+),(\d+),(\d+)\)", body)
        colors = [rgb565(*(int(channel) for channel in triple)) for triple in triples]
        assert len(colors) == 18, f"{name}: expected 18 colors, found {len(colors)}"
        foregrounds = (colors[7], blend(colors[8], colors[7], 2), blend(colors[10], colors[7], 2),
                       colors[12], colors[14], colors[16])
        minimum = min(contrast(foreground, background) for background in (colors[2], colors[3], colors[1])
                      for foreground in foregrounds)
        assert minimum >= MINIMUM_RATIO, f"{name}: minimum contrast is {minimum:.2f}"
        identities[name] = sum((channels(colors[index]) for index in (0, 1, 2, 3, 10)), ())
        print(f"{name}: {minimum:.2f}")
    distances = ((math.sqrt(sum((a - b) ** 2 for a, b in zip(identities[first], identities[second]))
                                / len(identities[first])), first, second)
                 for position, first in enumerate(identities) for second in tuple(identities)[position + 1:])
    minimum, first, second = min(distances)
    assert minimum >= MINIMUM_THEME_SEPARATION, f"Themes {first} and {second} are too similar ({minimum:.1f})"
    print(f"Closest themes: {first}/{second}, separation {minimum:.1f}")


if __name__ == "__main__":
    main()
