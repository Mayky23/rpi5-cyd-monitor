#pragma once
#include "ui_fonts.h"

namespace Typography {
enum Size : uint8_t { Compact = 0, Body = 1, Heading = 2, Display = 3, Medium = 4, Value = 5, SmallValue = 6 };

inline const Font &font(uint8_t size) {
  switch (size) {
    case Compact: return font10;
    case Heading: return font18;
    case Display: return font32;
    case Medium: return font13;
    case Value: return font22;
    case SmallValue: return font16;
    default: return font11;
  }
}

inline uint16_t blend(uint16_t foreground, uint16_t background, uint8_t alpha, uint8_t steps = 3) {
  const uint32_t g = ((((foreground & 0x07e0) * alpha) + ((background & 0x07e0) * (steps - alpha))) / steps) & 0x07e0;
  // Blend channels separately to prevent red/blue carry across packed RGB565 bits.
  const uint16_t red = (((foreground >> 11) * alpha + (background >> 11) * (steps - alpha)) / steps) << 11;
  const uint16_t blue = (((foreground & 31) * alpha + (background & 31) * (steps - alpha)) / steps);
  return red | g | blue;
}

inline const Glyph &glyph(const Font &f, char c) {
  return f.glyphs[(c >= 32 && c <= 126 ? c : '?') - 32];
}

inline int16_t width(const String &text, uint8_t size) {
  int16_t result = 0;
  const auto &f = font(size);
  for (size_t i = 0; i < text.length(); ++i) result += glyph(f, text[i]).width;
  return result;
}

inline uint8_t fitSize(const String &text, int16_t w, int16_t h, uint8_t maximum = Value) {
  const uint8_t sizes[] = {Display, Value, Heading, SmallValue, Medium, Body, Compact};
  for (const uint8_t size : sizes) {
    if (font(size).height <= font(maximum).height && font(size).height <= h && width(text, size) <= w)
      return size;
  }
  return Compact;
}

inline void draw(Arduino_GFX &gfx, int16_t x, int16_t y, const String &text,
                 uint16_t fg, uint16_t bg, uint8_t size) {
  const auto &f = font(size);
  const uint16_t colors[] = {bg, blend(fg, bg, 1), blend(fg, bg, 2), fg};
  for (size_t c = 0; c < text.length(); ++c) {
    const auto &g = glyph(f, text[c]);
    for (uint8_t row = 0; row < f.height; ++row) {
      uint8_t start = 0;
      while (start < g.width) {
        const uint16_t pos = row * g.width + start;
        const uint8_t level = (pgm_read_byte(f.pixels + g.offset + pos / 4) >> (6 - (pos % 4) * 2)) & 3;
        uint8_t end = start + 1;
        while (end < g.width) {
          const uint16_t next = row * g.width + end;
          if (((pgm_read_byte(f.pixels + g.offset + next / 4) >> (6 - (next % 4) * 2)) & 3) != level) break;
          ++end;
        }
        if (level) gfx.drawFastHLine(x + start, y + row, end - start, colors[level]);
        start = end;
      }
    }
    x += g.width;
  }
}
}  // namespace Typography
