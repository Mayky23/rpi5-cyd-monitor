#pragma once

#include <Arduino_GFX_Library.h>

#if __has_include("custom_splash.local.h")
#include "custom_splash.local.h"
#else
namespace CustomSplashAsset {
static constexpr bool AVAILABLE = false;
inline void draw(Arduino_GFX &, int, int, int) {}
}  // namespace CustomSplashAsset
#endif

namespace CustomSplash {
inline bool available() { return CustomSplashAsset::AVAILABLE; }
inline void draw(Arduino_GFX &display, int width, int height, int offset) {
  CustomSplashAsset::draw(display, width, height, offset);
}
}  // namespace CustomSplash
