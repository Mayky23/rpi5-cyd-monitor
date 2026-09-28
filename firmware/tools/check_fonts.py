"""Validate the generated font geometry and the heavier numeric strokes."""
import re
import unittest

from PIL import Image, ImageDraw, ImageFont
from build_fonts import FONT_PATH, FONT_SIZES, rasterize


class FontTests(unittest.TestCase):
    def test_complete_ascii_without_clipped_ink(self):
        for size in FONT_SIZES:
            font = ImageFont.truetype(str(FONT_PATH), size)
            top = min(font.getbbox(chr(c))[1] for c in range(32, 127))
            height, glyphs = rasterize(size)
            self.assertEqual(len(glyphs), 95)
            for code, glyph in enumerate(glyphs, 32):
                with self.subTest(size=size, char=chr(code)):
                    padded = Image.new('L', (glyph.width + 8, height + 8))
                    left = min(0, font.getbbox(chr(code))[0])
                    ImageDraw.Draw(padded).text((4 - left, 4 - top), chr(code), font=font, fill=255)
                    box = padded.getbbox()
                    if code != 32:
                        self.assertIsNotNone(box)
                        self.assertGreaterEqual(box[0], 4)
                        self.assertGreaterEqual(box[1], 4)
                        self.assertLessEqual(box[2], glyph.width + 4)
                        self.assertLessEqual(box[3], height + 4)
                    self.assertEqual(glyph.tobytes(), padded.crop((4, 4, glyph.width + 4, height + 4)).tobytes())

    def test_generated_pixels_match_source(self):
        header = (FONT_PATH.parents[1].parent / 'include' / 'ui_fonts.h').read_text(encoding='ascii')
        for size in FONT_SIZES:
            height, glyphs = rasterize(size)
            array = re.search(rf'pixels{size}\[\].*?\{{(.*?)\}};', header, re.S).group(1)
            pixels = [int(item) for item in re.findall(r'\d+', array)]
            array = re.search(rf'glyphs{size}\[\].*?\{{(.*?)\}};', header, re.S).group(1)
            entries = [(int(offset), int(width)) for offset, width in re.findall(r'\{(\d+),(\d+)\}', array)]
            self.assertEqual(len(entries), 95)
            for glyph, (offset, width) in zip(glyphs, entries):
                self.assertEqual(width, glyph.width)
                expected = [(pixel * 3 + 127) // 255 for pixel in glyph.get_flattened_data()]
                actual = [(pixels[offset + i // 4] >> (6 - i % 4 * 2)) & 3 for i in range(width * height)]
                self.assertEqual(expected, actual)

    def test_bold_has_stronger_numeric_strokes(self):
        for size in (11, 18, 22):
            regular = ImageFont.load_default(size=size)
            _, glyphs = rasterize(size)
            old_ink = new_ink = 0
            for char in '0123456789%':
                canvas = Image.new('L', (size * 2, size * 2))
                ImageDraw.Draw(canvas).text((2, 0), char, font=regular, fill=255)
                old_ink += sum(canvas.get_flattened_data())
                new_ink += sum(glyphs[ord(char) - 32].get_flattened_data())
            self.assertGreater(new_ink, old_ink * 1.25)

    def test_compact_rows_and_tabular_numbers(self):
        self.assertLessEqual(rasterize(10)[0], 12)
        self.assertLessEqual(rasterize(11)[0], 13)
        self.assertLessEqual(rasterize(13)[0], 16)
        for size in FONT_SIZES:
            _, glyphs = rasterize(size)
            self.assertEqual(len({glyphs[ord(c) - 32].width for c in '0123456789'}), 1)


if __name__ == '__main__':
    unittest.main()
