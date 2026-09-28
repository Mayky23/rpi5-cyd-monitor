"""Assemble the real USB framebuffer captures used in the README."""
from pathlib import Path
from PIL import Image


def main():
    root = Path(__file__).resolve().parents[2]
    source = root / 'firmware' / '.pio' / 'panel-captures'
    gallery = Image.new('RGB', (1008, 516), (8, 8, 8))
    for index, page in enumerate((0, 1, 2, 3, 4, 6)):
        with Image.open(source / f'page-{page}-320.png') as capture:
            assert capture.size == (320, 240)
            gallery.paste(capture, (12 + (index % 3) * 332, 12 + (index // 3) * 252))
    target = root / 'docs' / 'images' / 'panel-v8.png'
    gallery.save(target)
    print(target)


if __name__ == '__main__':
    main()
