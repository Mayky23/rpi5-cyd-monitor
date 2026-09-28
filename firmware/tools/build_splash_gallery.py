"""Build a documentation gallery from frames captured on the ESP32."""

import argparse
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).parents[2]
CAPTURES = ROOT / "firmware/.pio/splash-captures"
OUTPUT = ROOT / "docs/images"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--count", type=int, default=5)
    parser.add_argument("--timestamp", type=int, default=2300)
    args = parser.parse_args()
    frames = []
    for design in range(args.count):
        landscape = Image.open(CAPTURES / f"{design}-320-{args.timestamp}.png").convert("RGB")
        portrait = Image.open(CAPTURES / f"{design}-240-{args.timestamp}.png").convert("RGB")
        frame = Image.new("RGB", (568, 344), (5, 9, 13))
        frame.paste(landscape, (0, 40))
        frame.paste(portrait, (328, 24))
        ImageDraw.Draw(frame).text((10, 10), f"Diseno {design + 1}", fill="white")
        frames.append(frame)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    frames[0].save(OUTPUT / "splash-gallery.gif", save_all=True,
                   append_images=frames[1:], duration=850, loop=0, disposal=2)
    print(f"Generated {OUTPUT / 'splash-gallery.gif'}")


if __name__ == "__main__":
    main()
