#include <Arduino.h>
#include <cstring>
#include <atomic>
#include <new>
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <XPT2046_Touchscreen.h>
#include <esp_system.h>
#include <memory>
#include "config.h"
#include "runtime_config.h"
#include "splash_scenes.h"
#include "panel_data.h"
#include "ui_type.h"
#include "version.h"

// Valores seguros para actualizar solo main.cpp sin tocar las credenciales existentes.
#ifndef DISPLAY_INVERT_COLORS
#define DISPLAY_INVERT_COLORS 1
#endif

#ifndef DISPLAY_TEST_ONLY
#define DISPLAY_TEST_ONLY 0
#endif

// ESP32-2432S028R / Cheap Yellow Display
static constexpr uint8_t TFT_BL = 21;
static constexpr uint8_t TFT_CS = 15;
static constexpr uint8_t TFT_DC = 2;
static constexpr uint8_t TFT_SCK = 14;
static constexpr uint8_t TFT_MOSI = 13;
static constexpr uint8_t TFT_MISO = 12;
static constexpr uint8_t TOUCH_CS = 33;
static constexpr uint8_t TOUCH_IRQ = 36;
static constexpr uint8_t TOUCH_SCK = 25;
static constexpr uint8_t TOUCH_MOSI = 32;
static constexpr uint8_t TOUCH_MISO = 39;

static_assert(DISPLAY_ROTATION >= 0 && DISPLAY_ROTATION <= 3,
              "DISPLAY_ROTATION debe estar entre 0 y 3");

static uint8_t displayRotation = DISPLAY_ROTATION;
static bool LANDSCAPE = DISPLAY_ROTATION == 1 || DISPLAY_ROTATION == 3;
static int16_t SCREEN_W = LANDSCAPE ? 320 : 240;
static int16_t SCREEN_H = LANDSCAPE ? 240 : 320;
static constexpr int16_t HEADER_H = 36;
static constexpr int16_t FOOTER_H = 32;
static constexpr int16_t MARGIN = 6;
static constexpr int16_t GAP = 5;
static constexpr int16_t CONTENT_TOP = HEADER_H + 5;
#define CONTENT_BOTTOM static_cast<int16_t>(SCREEN_H - FOOTER_H - 5)
#define CONTENT_H static_cast<int16_t>(CONTENT_BOTTOM - CONTENT_TOP)

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_GFX *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED, DISPLAY_ROTATION, DISPLAY_INVERT_COLORS != 0);
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
JsonDocument snapshot;
JsonDocument snapshotFilter;
Preferences preferences;
static RuntimeConfig::Values netConfig;

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8u) << 8u) | ((g & 0xFCu) << 3u) | (b >> 3u));
}

struct Theme {
  const char *name;
  uint16_t bg, top, card, cardAlt, cardDeep, border, grid;
  uint16_t text, muted, dim, accent, accentDark;
  uint16_t ok, okDark, warn, warnDark, bad, badDark;
};

static constexpr Theme THEMES[] = {
  {"ORIGINAL", rgb565(3,5,7), rgb565(12,15,18), rgb565(20,24,28), rgb565(31,36,41), rgb565(9,12,15), rgb565(79,90,100), rgb565(46,55,63),
   rgb565(244,247,249), rgb565(170,184,194), rgb565(132,148,159), rgb565(67,204,255), rgb565(12,57,77),
   rgb565(78,223,159), rgb565(10,61,41), rgb565(255,192,84), rgb565(69,46,13), rgb565(255,112,130), rgb565(76,19,31)},
  {"RASPBERRY", rgb565(10,2,8), rgb565(36,8,24), rgb565(53,10,34), rgb565(73,15,45), rgb565(24,4,16), rgb565(172,45,100), rgb565(94,24,57),
   rgb565(255,244,249), rgb565(238,166,198), rgb565(193,112,150), rgb565(250,37,112), rgb565(92,4,42),
   rgb565(151,231,65), rgb565(40,65,8), rgb565(255,202,67), rgb565(91,54,5), rgb565(255,145,120), rgb565(92,20,13)},
  {"MATRIX", rgb565(0,5,1), rgb565(1,16,5), rgb565(3,28,10), rgb565(6,43,16), rgb565(0,12,4), rgb565(31,145,64), rgb565(13,80,34),
   rgb565(224,255,232), rgb565(120,219,147), rgb565(72,166,101), rgb565(47,255,100), rgb565(0,74,25),
   rgb565(50,240,212), rgb565(4,68,59), rgb565(255,221,54), rgb565(76,61,2), rgb565(255,83,111), rgb565(78,9,25)},
  {"AMBAR", rgb565(28,13,0), rgb565(59,31,2), rgb565(76,43,7), rgb565(99,61,14), rgb565(42,23,2), rgb565(210,139,28), rgb565(129,80,14),
   rgb565(255,249,224), rgb565(250,214,143), rgb565(222,169,82), rgb565(255,150,0), rgb565(111,52,0),
   rgb565(151,240,200), rgb565(19,68,48), rgb565(255,200,98), rgb565(92,28,2), rgb565(255,150,144), rgb565(92,13,8)},
  {"GRAFITO", rgb565(0,0,0), rgb565(13,14,15), rgb565(25,27,29), rgb565(41,43,46), rgb565(10,11,12), rgb565(99,103,109), rgb565(54,58,63),
   rgb565(250,250,243), rgb565(192,194,189), rgb565(149,151,146), rgb565(255,222,32), rgb565(74,62,8),
   rgb565(225,229,162), rgb565(53,56,27), rgb565(255,164,69), rgb565(79,42,10), rgb565(255,109,109), rgb565(79,21,21)},
  {"VIOLETA", rgb565(10,4,22), rgb565(28,12,51), rgb565(43,19,70), rgb565(61,28,92), rgb565(20,8,39), rgb565(137,80,204), rgb565(78,42,124),
   rgb565(253,244,255), rgb565(216,175,245), rgb565(170,126,209), rgb565(209,113,255), rgb565(69,20,112),
   rgb565(96,244,202), rgb565(7,70,52), rgb565(255,213,94), rgb565(85,54,3), rgb565(255,150,210), rgb565(91,11,52)},
  {"OCEANO", rgb565(0,4,38), rgb565(0,12,67), rgb565(0,23,91), rgb565(0,37,122), rgb565(0,9,57), rgb565(0,100,190), rgb565(0,58,135),
   rgb565(232,250,255), rgb565(139,210,232), rgb565(83,162,194), rgb565(0,235,177), rgb565(0,73,66),
   rgb565(110,242,175), rgb565(8,68,43), rgb565(255,215,102), rgb565(83,54,4), rgb565(255,145,154), rgb565(85,13,23)},
  {"CORAL", rgb565(246,218,209), rgb565(255,237,229), rgb565(255,250,246), rgb565(251,229,218), rgb565(235,201,190), rgb565(183,104,91), rgb565(221,165,151),
   rgb565(55,28,30), rgb565(112,66,67), rgb565(135,82,80), rgb565(207,57,71), rgb565(255,203,201),
   rgb565(0,117,104), rgb565(202,238,229), rgb565(151,81,0), rgb565(255,226,183), rgb565(179,26,55), rgb565(255,208,217)},
  {"AVIACION", rgb565(18,24,30), rgb565(29,42,53), rgb565(40,57,69), rgb565(53,74,87), rgb565(25,35,44), rgb565(109,145,166), rgb565(63,96,116),
   rgb565(246,250,252), rgb565(187,209,220), rgb565(138,169,185), rgb565(255,157,36), rgb565(93,45,0),
   rgb565(112,242,184), rgb565(10,69,43), rgb565(255,218,105), rgb565(91,45,2), rgb565(255,151,155), rgb565(88,11,20)},
  {"CLARO", rgb565(224,231,235), rgb565(247,250,252), rgb565(255,255,255), rgb565(235,241,244), rgb565(211,221,227), rgb565(144,160,171), rgb565(193,207,216),
   rgb565(24,35,44), rgb565(71,88,100), rgb565(88,105,116), rgb565(0,104,151), rgb565(205,231,244),
   rgb565(0,115,77), rgb565(207,238,221), rgb565(156,83,0), rgb565(250,233,197), rgb565(186,33,59), rgb565(251,217,224)},
};
static constexpr uint8_t THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);
static constexpr uint8_t THEMES_PER_PAGE = 6;

namespace C {
uint16_t BG, TOP, CARD, CARD_ALT, CARD_DEEP, BORDER, GRID;
uint16_t TEXT, VALUE, MUTED, DIM, ACCENT, ACCENT_DARK;
uint16_t OK, OK_DARK, WARN, WARN_DARK, BAD, BAD_DARK;
constexpr uint16_t WHITE_ = 0xFFFF;
}

enum Page : uint8_t {
  PAGE_TOTAL,
  PAGE_OVERVIEW,
  PAGE_STORAGE,
  PAGE_NETWORK,
  PAGE_CPU,
  PAGE_PORTS,
  PAGE_DOCKER,
  PAGE_PROXMOX,
  PAGE_PVE_PERFORMANCE,
  PAGE_PVE_GUESTS,
  PAGE_PVE_STORAGE,
  PAGE_PVE_TASKS,
  PAGE_DIAG,
  PAGE_BRIGHTNESS,
  PAGE_CUSTOMIZE,
  PAGE_SPLASH,
  PAGE_ORIENTATION,
  PAGE_ID_COUNT,
};

static const char *PAGE_NAMES[] = {"RPI5", "RENDIMIENTO", "DISCOS", "RED", "CPU / TEMP", "PUERTOS", "DOCKER", "PVE RESUMEN", "PVE RENDIM.", "PVE VM / LXC", "PVE DISCOS", "PVE TAREAS", "DIAGNOSTICO", "BRILLO", "PERSONALIZACION", "ARRANQUE", "ORIENTACION"};
static_assert(sizeof(PAGE_NAMES) / sizeof(PAGE_NAMES[0]) == PAGE_ID_COUNT, "Page names must match page ids");
#if MONITOR_PROFILE == MONITOR_PROFILE_RPI5
static constexpr Page VISIBLE_PAGES[] = {
  PAGE_TOTAL, PAGE_OVERVIEW, PAGE_STORAGE, PAGE_NETWORK, PAGE_CPU, PAGE_PORTS,
  PAGE_DOCKER, PAGE_DIAG, PAGE_BRIGHTNESS, PAGE_CUSTOMIZE, PAGE_SPLASH, PAGE_ORIENTATION
};
#elif MONITOR_PROFILE == MONITOR_PROFILE_PROXMOX
static constexpr Page VISIBLE_PAGES[] = {
  PAGE_PROXMOX, PAGE_PVE_PERFORMANCE, PAGE_PVE_GUESTS, PAGE_PVE_STORAGE,
  PAGE_PVE_TASKS, PAGE_DIAG, PAGE_BRIGHTNESS, PAGE_CUSTOMIZE, PAGE_SPLASH, PAGE_ORIENTATION
};
#else
static constexpr Page VISIBLE_PAGES[] = {
  PAGE_TOTAL, PAGE_OVERVIEW, PAGE_STORAGE, PAGE_NETWORK, PAGE_CPU, PAGE_PORTS,
  PAGE_DOCKER, PAGE_PROXMOX, PAGE_PVE_PERFORMANCE, PAGE_PVE_GUESTS,
  PAGE_PVE_STORAGE, PAGE_PVE_TASKS, PAGE_DIAG, PAGE_BRIGHTNESS, PAGE_CUSTOMIZE,
  PAGE_SPLASH, PAGE_ORIENTATION
};
#endif
static constexpr uint8_t PAGE_COUNT = sizeof(VISIBLE_PAGES) / sizeof(VISIBLE_PAGES[0]);
// Index zero retains the minimum nonzero 8-bit PWM duty for night use.
static constexpr uint8_t BRIGHTNESS_LEVELS[] = {1, 26, 51, 102, 153, 204, 255};
static constexpr uint8_t BRIGHTNESS_LABELS[] = {5, 10, 20, 40, 60, 80, 100};
static constexpr uint8_t BRIGHTNESS_LEVEL_COUNT = sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]);
static_assert(sizeof(BRIGHTNESS_LABELS) == BRIGHTNESS_LEVEL_COUNT, "Brightness labels must match PWM levels");
static const char *SPLASH_NAMES[] = {"RASPBERRY", "TERMINAL", "CYBER", "ORBITAL", "PERSONAL"};
static constexpr uint8_t SPLASH_COUNT = sizeof(SPLASH_NAMES) / sizeof(SPLASH_NAMES[0]);
static constexpr uint8_t SPLASHES_PER_PAGE = 4;
static_assert(SPLASH_COUNT == Splash::DESIGN_COUNT, "Splash names and scenes must match");

static uint8_t currentPage = VISIBLE_PAGES[0];
static uint32_t lastRequest = 0;
static uint32_t nextWifiAttempt = 0;
static uint32_t reconnectDelay = 5000;
static uint32_t lastGoodSample = 0;
static uint32_t lastPageChange = 0;
static int lastHttpCode = 0;
static String lastApiError = "Sin muestras";
static bool online = false;
static bool bootstrapping = true;
static const char *statusText = "INICIO";
static String lastHeaderIdentity;
static String lastStatusRendered;
static uint32_t listSignatures[PAGE_ID_COUNT] = {0};
static uint8_t brightnessLevel = 0;
static uint8_t themeIndex = 0;
static uint8_t splashIndex = 0;
static bool navigationOpen = false;
static bool statusDirty = false;

static void applyTheme(uint8_t index) {
  themeIndex = index < THEME_COUNT ? index : 0;
  const Theme &theme = THEMES[themeIndex];
  C::BG = theme.bg; C::TOP = theme.top; C::CARD = theme.card;
  C::CARD_ALT = theme.cardAlt; C::CARD_DEEP = theme.cardDeep;
  C::BORDER = theme.border; C::GRID = theme.grid; C::TEXT = theme.text;
  C::VALUE = Typography::blend(theme.accent, theme.text, 2);
  C::MUTED = Typography::blend(theme.muted, theme.text, 2); C::DIM = theme.dim; C::ACCENT = theme.accent;
  C::ACCENT_DARK = theme.accentDark; C::OK = theme.ok; C::OK_DARK = theme.okDark;
  C::WARN = theme.warn; C::WARN_DARK = theme.warnDark;
  C::BAD = theme.bad; C::BAD_DARK = theme.badDark;
}

static void loadTheme() {
  preferences.begin("rpi5-panel", false);
  applyTheme(preferences.getUChar("theme", 0));
  splashIndex = preferences.getUChar("splash", 0);
  if (preferences.getUChar("splashVer", 0) < 3) {
    if (splashIndex == 2) splashIndex = 1;
    else if (splashIndex == 3) splashIndex = 2;
    else if (splashIndex == 4) splashIndex = 3;
    else if (splashIndex != 0) splashIndex = 0;
    preferences.putUChar("splashVer", 3);
    preferences.putUChar("splash", splashIndex);
  }
  if (splashIndex >= SPLASH_COUNT) splashIndex = 0;
}

static void saveTheme(uint8_t index) {
  applyTheme(index);
#if !SPLASH_CAPTURE_ENABLED
  preferences.putUChar("theme", themeIndex);
#endif
}

static void saveSplash(uint8_t index) {
  splashIndex = index < SPLASH_COUNT ? index : 0;
#if !SPLASH_CAPTURE_ENABLED
  preferences.putUChar("splash", splashIndex);
  preferences.putUChar("splashVer", 3);
#endif
}

static uint8_t nearestBrightnessLevel(uint8_t duty) {
  uint8_t closest = 0;
  uint16_t bestDistance = 256;
  for (uint8_t i = 0; i < BRIGHTNESS_LEVEL_COUNT; ++i) {
    const uint16_t distance = abs(static_cast<int>(duty) - static_cast<int>(BRIGHTNESS_LEVELS[i]));
    if (distance < bestDistance) {
      bestDistance = distance;
      closest = i;
    }
  }
  return closest;
}

static void applyBrightness() {
  ledcWrite(0, BRIGHTNESS_LEVELS[brightnessLevel]);
}

static void loadBrightness() {
  const uint8_t previousLevels[] = {1, 18, 55, 130, 255};
  uint8_t duty = SCREEN_BRIGHTNESS;
  if (preferences.isKey("brightnessPwm")) {
    duty = preferences.getUChar("brightnessPwm", duty);
  } else if (preferences.isKey("brightness")) {
    const uint8_t oldIndex = preferences.getUChar("brightness", 255);
    if (oldIndex < sizeof(previousLevels)) duty = previousLevels[oldIndex];
  }
  brightnessLevel = nearestBrightnessLevel(duty);
  applyBrightness();
}

static void saveBrightness() {
  applyBrightness();
  preferences.putUChar("brightnessPwm", BRIGHTNESS_LEVELS[brightnessLevel]);
}

struct Rect {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

struct ListPager {
  Rect card{};
  size_t page = 0, pages = 1;
};
static ListPager listPagers[PAGE_ID_COUNT];

static void setDisplayGeometry(uint8_t rotation) {
  displayRotation = rotation < 4 ? rotation : DISPLAY_ROTATION;
  LANDSCAPE = (displayRotation & 1) != 0;
  SCREEN_W = LANDSCAPE ? 320 : 240;
  SCREEN_H = LANDSCAPE ? 240 : 320;
  for (uint8_t i = 0; i < PAGE_ID_COUNT; ++i) {
    listPagers[i] = ListPager{};
    listSignatures[i] = 0;
  }
  listPagers[PAGE_CUSTOMIZE].page = themeIndex / THEMES_PER_PAGE;
  listPagers[PAGE_SPLASH].page = splashIndex / SPLASHES_PER_PAGE;
  lastHeaderIdentity = "";
  lastStatusRendered = "";
}

static void applyDisplayRotation(uint8_t rotation) {
  setDisplayGeometry(rotation);
  gfx->setRotation(displayRotation);
}

enum class Align : uint8_t { Left, Center, Right };

static int16_t i16Max(int16_t a, int16_t b) { return a > b ? a : b; }
static uint8_t u8Max(uint8_t a, uint8_t b) { return a > b ? a : b; }
static uint8_t u8Min(uint8_t a, uint8_t b) { return a < b ? a : b; }
static uint32_t u32Min(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t u32Max(uint32_t a, uint32_t b) { return a > b ? a : b; }
static size_t sizeMin(size_t a, size_t b) { return a < b ? a : b; }
static size_t sizeMax(size_t a, size_t b) { return a > b ? a : b; }

static String fitText(String value, int16_t pixelWidth, uint8_t size = 1) {
  if (pixelWidth <= 0) return "";
  if (Typography::width(value, size) <= pixelWidth) return value;
  const String suffix = "...";
  while (value.length() && Typography::width(value + suffix, size) > pixelWidth)
    value.remove(value.length() - 1);
  return Typography::width(suffix, size) <= pixelWidth ? value + suffix : "";
}

static void rawText(int16_t x, int16_t y, const String &value, uint16_t color,
                    uint8_t size, uint16_t background) {
  Typography::draw(*gfx, x, y, value, color, background, size);
}

static void textBox(const Rect &r, const String &value, uint16_t color = C::TEXT,
                    uint8_t size = 1, Align align = Align::Left,
                    uint16_t background = C::CARD, int16_t pad = 0) {
  if (r.w <= 0 || r.h <= 0) return;
  if (value == "ERROR") color = C::BAD;
  gfx->fillRect(r.x, r.y, r.w, r.h, background);
  if (Typography::font(size).height > r.h) size = Typography::fitSize("", r.w, r.h, size);
  if (Typography::font(size).height > r.h) return;
  const String fitted = fitText(value, i16Max(0, r.w - pad * 2), size);
  const int16_t textW = Typography::width(fitted, size);
  int16_t x = r.x + pad;
  if (align == Align::Center) x = r.x + (r.w - textW) / 2;
  else if (align == Align::Right) x = r.x + r.w - pad - textW;
  const int16_t y = r.y + i16Max(0, (r.h - Typography::font(size).height) / 2);
  rawText(x, y, fitted, color, size, background);
}

static void valueBox(const Rect &r, const String &value, uint16_t color = C::TEXT,
                     Align align = Align::Left, uint16_t background = C::CARD) {
  const uint8_t size = Typography::fitSize(value, r.w - 2, r.h);
  textBox(r, value, value == "ERROR" ? C::BAD : color, size, align, background, 1);
}

static void cardFrame(const Rect &r, const String &title = "", uint16_t accent = C::ACCENT) {
  gfx->fillRect(r.x, r.y, r.w, r.h, C::CARD);
  if (title.length()) {
    textBox({static_cast<int16_t>(r.x + 8), static_cast<int16_t>(r.y + 3),
             static_cast<int16_t>(r.w - 16), 17}, title, C::MUTED);
    gfx->drawFastHLine(r.x + 8, r.y + 22, r.w - 16, C::BORDER);
    gfx->drawFastHLine(r.x + 8, r.y + 22, 18, accent);
  }
}

static void chevron(int16_t cx, int16_t cy, bool next, uint16_t color) {
  for (int16_t i = 0; i < 2; ++i) {
    const int16_t tip = cx + (next ? 3 : -3);
    const int16_t back = cx + (next ? -2 : 2);
    gfx->drawLine(back + i, cy - 5, tip + i, cy, color);
    gfx->drawLine(tip + i, cy, back + i, cy + 5, color);
  }
}

static void checkmark(int16_t x, int16_t y, uint16_t color) {
  for (uint8_t i = 0; i < 2; ++i) {
    gfx->drawLine(x, y + i, x + 3, y + 3 + i, color);
    gfx->drawLine(x + 3, y + 3 + i, x + 9, y - 4 + i, color);
  }
}

static void divider(int16_t x, int16_t y, int16_t w) {
  gfx->drawFastHLine(x, y, w, C::GRID);
}

static Rect listArrowRect(const Rect &card, bool next) {
  return {static_cast<int16_t>(card.x + card.w - (next ? 25 : 80)),
          static_cast<int16_t>(card.y + 1), 23, 22};
}

static size_t listStart(const Rect &card, const String &title, uint8_t pageId,
                        size_t count, size_t perPage) {
  auto &pager = listPagers[pageId];
  pager.card = card;
  pager.pages = sizeMax(1, (count + perPage - 1) / perPage);
  pager.page = sizeMin(pager.page, pager.pages - 1);
  const bool paged = pager.pages > 1;
  gfx->fillRect(card.x + 1, card.y + 1, card.w - 2, 21, C::CARD);
  textBox({static_cast<int16_t>(card.x + 8), static_cast<int16_t>(card.y + 3),
           static_cast<int16_t>(card.w - (paged ? 90 : 16)), 17}, title, C::MUTED);
  if (paged) {
    const Rect left = listArrowRect(card, false), right = listArrowRect(card, true);
    gfx->fillRect(left.x, left.y, 78, 21, C::CARD);
    chevron(left.x + left.w / 2, left.y + left.h / 2, false, C::VALUE);
    chevron(right.x + right.w / 2, right.y + right.h / 2, true, C::VALUE);
    textBox({static_cast<int16_t>(left.x + left.w), left.y, 32, left.h},
            String(pager.page + 1) + "/" + String(pager.pages), C::TEXT, 1, Align::Center);
  }
  gfx->drawFastHLine(card.x + 8, card.y + 22, card.w - 16, C::BORDER);
  gfx->drawFastHLine(card.x + 8, card.y + 22, 18, C::ACCENT);
  return pager.page * perPage;
}

static uint16_t healthColor(float value, float warn, float bad, bool highIsBad = true) {
  if (!highIsBad) return value > bad ? C::OK : (value > warn ? C::WARN : C::BAD);
  if (value >= bad) return C::BAD;
  if (value >= warn) return C::WARN;
  return C::OK;
}

static void progressBar(const Rect &r, float percent, uint16_t color,
                        uint16_t background = C::CARD_DEEP) {
  percent = constrain(percent, 0.0f, 100.0f);
  gfx->fillRect(r.x, r.y, r.w, r.h, background);
  const int16_t fill = static_cast<int16_t>((r.w * percent) / 100.0f);
  if (fill > 0) {
    gfx->fillRect(r.x, r.y, fill, r.h, color);
  }
}

static String rate(double value) {
  const char *units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
  uint8_t unit = 0;
  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    ++unit;
  }
  char out[22];
  snprintf(out, sizeof(out), value >= 100.0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
  return String(out);
}

static String bytes(double value) {
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  uint8_t unit = 0;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }
  char out[22];
  snprintf(out, sizeof(out), value >= 100.0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
  return String(out);
}

static bool rootDiskOk() {
  const char *status = snapshot["disk"]["status"] | "ok";
  return strcmp(status, "ok") == 0 && Panel::number(snapshot["disk"]["percent"]);
}

static float rootDiskPercent() {
  return rootDiskOk() ? snapshot["disk"]["percent"].as<float>() : 0.0f;
}

static String rootDiskText() {
  return rootDiskOk() ? String(rootDiskPercent(), 0) + "%" : "ERROR";
}

static uint16_t rootDiskColor() {
  return rootDiskOk() ? healthColor(rootDiskPercent(), 80, 92) : C::BAD;
}

static bool diskIoOk() {
  const char *status = snapshot["disk"]["io_status"] | "ok";
  return strcmp(status, "ok") == 0 && Panel::number(snapshot["disk"]["read_bps"]) &&
         Panel::number(snapshot["disk"]["write_bps"]);
}

static String diskRateText(const char *field) {
  return diskIoOk() ? rate(snapshot["disk"][field].as<double>()) : "ERROR";
}

static String rootFreeText() {
  return rootDiskOk() && Panel::number(snapshot["disk"]["free_bytes"])
             ? bytes(snapshot["disk"]["free_bytes"].as<double>())
             : "ERROR";
}

static String physicalDeviceKey(String device) {
  if (device.startsWith("/dev/")) device.remove(0, 5);
  if (device.startsWith("mmcblk") || device.startsWith("nvme")) {
    const int16_t partition = device.lastIndexOf('p');
    if (partition > 0) device.remove(partition);
  } else if (device.startsWith("sd") || device.startsWith("hd") ||
             device.startsWith("vd") || device.startsWith("xvd")) {
    while (device.length() && isDigit(device[device.length() - 1])) {
      device.remove(device.length() - 1);
    }
  }
  return device;
}

static String uptimeText(uint64_t value) {
  char out[22];
  const uint32_t days = value / 86400ULL;
  const uint32_t hours = (value % 86400ULL) / 3600ULL;
  const uint32_t mins = (value % 3600ULL) / 60ULL;
  if (days) snprintf(out, sizeof(out), "%lud %luh", static_cast<unsigned long>(days), static_cast<unsigned long>(hours));
  else snprintf(out, sizeof(out), "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(mins));
  return String(out);
}

static uint32_t fnv1aAdd(uint32_t hash, const char *text) {
  if (!text) return hash;
  while (*text) {
    hash ^= static_cast<uint8_t>(*text++);
    hash *= 16777619u;
  }
  return hash;
}

static uint32_t jsonSignature(JsonVariantConst value) {
  String encoded;
  serializeJson(value, encoded);
  return fnv1aAdd(2166136261u, encoded.c_str());
}

static bool collectionFailed(const char *name) {
  const JsonVariantConst ok = snapshot["collection_status"][name]["ok"];
  return ok.is<bool>() && !ok.as<bool>();
}

static uint32_t storageSignature() { return jsonSignature(snapshot["storage"]) ^ collectionFailed("block_devices"); }
static uint32_t interfaceSignature() { return jsonSignature(snapshot["interfaces"]) ^ collectionFailed("interfaces"); }
static uint32_t portSignature() { return jsonSignature(snapshot["ports"]) ^ collectionFailed("ports"); }
static uint32_t dockerSignature() { return jsonSignature(snapshot["docker"]); }
// Each Proxmox list only redraws when its own rows or the connection state change,
// not every time the node CPU/RAM history moves.
static uint32_t proxmoxSignature(const char *field) {
  const uint32_t state = fnv1aAdd(jsonSignature(snapshot["proxmox"][field]), snapshot["proxmox"]["state"] | "");
  const bool available = snapshot["proxmox"]["available"] | false;
  return state ^ (available ? 1u : 0u);
}

static Rect kpiRect(uint8_t index, uint8_t columns, int16_t y, int16_t h) {
  const int16_t totalGap = GAP * (columns - 1);
  const int16_t w = (SCREEN_W - MARGIN * 2 - totalGap) / columns;
  return {static_cast<int16_t>(MARGIN + index * (w + GAP)), y, w, h};
}

static void drawKpi(const Rect &r, const String &label, const String &value,
                    float percent, uint16_t accent, bool full) {
  if (full) {
    cardFrame(r);
    textBox({static_cast<int16_t>(r.x + 7), static_cast<int16_t>(r.y + 4),
             static_cast<int16_t>(r.w - 14), 14}, label, C::MUTED);
  }
  const uint16_t color = value == "ERROR" ? C::BAD :
                         accent == C::WARN || accent == C::BAD ? accent : C::VALUE;
  valueBox({static_cast<int16_t>(r.x + 6), static_cast<int16_t>(r.y + 19),
            static_cast<int16_t>(r.w - 12), static_cast<int16_t>(r.h - 29)}, value, color);
  progressBar({static_cast<int16_t>(r.x + 7), static_cast<int16_t>(r.y + r.h - 6),
               static_cast<int16_t>(r.w - 14), 2}, percent, accent);
}

static void drawStatCard(const Rect &r, const String &label, const String &value,
                         uint16_t accent, bool full) {
  if (full) {
    cardFrame(r);
    textBox({static_cast<int16_t>(r.x + 7), static_cast<int16_t>(r.y + 3),
             static_cast<int16_t>(r.w - 14), 15}, label, C::MUTED);
  }
  valueBox({static_cast<int16_t>(r.x + 6), static_cast<int16_t>(r.y + 21),
            static_cast<int16_t>(r.w - 12), static_cast<int16_t>(r.h - 23)},
           value, accent == C::BAD || accent == C::WARN ? accent : C::VALUE);
}

static void drawHeaderBase() {
  gfx->fillRect(0, 0, SCREEN_W, HEADER_H, C::TOP);
  gfx->drawFastHLine(0, HEADER_H - 1, SCREEN_W, C::BORDER);
  gfx->fillRect(0, 0, 3, HEADER_H - 1, C::ACCENT);
  const int16_t titleW = SCREEN_W - 87;
  const uint8_t size = Typography::fitSize(PAGE_NAMES[currentPage], titleW, 22, Typography::Heading);
  textBox({10, 1, titleW, 22}, PAGE_NAMES[currentPage], C::TEXT, size, Align::Left, C::TOP);
}

static uint8_t visiblePagePosition(uint8_t page) {
  for (uint8_t index = 0; index < PAGE_COUNT; ++index)
    if (VISIBLE_PAGES[index] == page) return index;
  return 0;
}

static void drawHeaderIdentity(bool force = false) {
  const String ip = String(snapshot["system"]["ip"] | "Sin datos") +
                    (!online && lastGoodSample ? "  /  Sin actualizar" : "");
  if (!force && ip == lastHeaderIdentity) return;
  textBox({10, 22, static_cast<int16_t>(SCREEN_W - 20), 13}, ip, C::MUTED, 1, Align::Left, C::TOP);
  lastHeaderIdentity = ip;
}

static void drawStatusChip(bool force = false) {
  const String current = String(statusText) + (online ? "1" : "0");
  if (!force && current == lastStatusRendered) return;
  const bool healthy = online && strcmp(statusText, "PARCIAL") != 0;
  const uint16_t fg = healthy ? C::OK : C::WARN;
  const int16_t x = SCREEN_W - 73;
  gfx->fillRect(x, 3, 66, 18, C::TOP);
  gfx->fillCircle(x + 5, 11, 2, fg);
  textBox({static_cast<int16_t>(x + 13), 3, 54, 18},
          online ? (healthy ? "ONLINE" : "PARCIAL") : strcmp(statusText, "WIFI") == 0 ? "SIN WIFI" : "SIN API",
          fg, 1, Align::Left, C::TOP);
  lastStatusRendered = current;
}

static Rect footerButtonRect(bool next) {
  return {static_cast<int16_t>(next ? SCREEN_W - 62 : 0),
          static_cast<int16_t>(SCREEN_H - FOOTER_H), 62, FOOTER_H};
}

static void drawFooter() {
  const int16_t y = SCREEN_H - FOOTER_H;
  gfx->fillRect(0, y, SCREEN_W, FOOTER_H, C::TOP);
  gfx->drawFastHLine(0, y, SCREEN_W, C::BORDER);
  const Rect left = footerButtonRect(false), right = footerButtonRect(true);
  chevron(left.x + left.w / 2, y + 16, false, C::VALUE);
  chevron(right.x + right.w / 2, y + 16, true, C::VALUE);
  gfx->drawFastVLine(left.w, y + 7, FOOTER_H - 14, C::GRID);
  gfx->drawFastVLine(right.x, y + 7, FOOTER_H - 14, C::GRID);
  const String position = String(visiblePagePosition(currentPage) + 1) + " / " + String(PAGE_COUNT);
  const int16_t width = Typography::width(position, 1);
  const int16_t groupX = (SCREEN_W - width - 25) / 2;
  for (uint8_t i = 0; i < 4; ++i)
    gfx->fillRect(groupX + (i % 2) * 6, y + 11 + (i / 2) * 6, 4, 4, C::MUTED);
  textBox({static_cast<int16_t>(groupX + 24), static_cast<int16_t>(y + 5),
           static_cast<int16_t>(width + 2), 23}, position, C::MUTED, 1, Align::Left, C::TOP);
}

static Rect navigationItem(uint8_t index) {
  const int16_t w = (SCREEN_W - MARGIN * 2 - GAP) / 2;
  const int16_t h = (CONTENT_H - 10) / ((PAGE_COUNT + 1) / 2);
  return {static_cast<int16_t>(MARGIN + (index % 2) * (w + GAP)),
          static_cast<int16_t>(CONTENT_TOP + (index / 2) * h), w, static_cast<int16_t>(h - 2)};
}

static void drawNavigation() {
  gfx->fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H - FOOTER_H, C::BG);
  for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
    const uint8_t page = VISIBLE_PAGES[i];
    const Rect r = navigationItem(i);
    const bool active = page == currentPage;
    const uint16_t bg = active ? C::ACCENT_DARK : C::CARD;
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 3, bg);
    const uint8_t size = Typography::fitSize(PAGE_NAMES[page], r.w - 13, r.h, Typography::Body);
    textBox({static_cast<int16_t>(r.x + 7), r.y, static_cast<int16_t>(r.w - 13), r.h},
            PAGE_NAMES[page], active ? C::VALUE : C::TEXT, size, Align::Left, bg);
  }
}

static String numberText(JsonVariantConst value, uint8_t decimals = 0, const char *unit = "") {
  return Panel::number(value) ? String(value.as<double>(), static_cast<unsigned int>(decimals)) + unit : "ERROR";
}

static String trafficText(const char *field) {
  return Panel::number(snapshot["network"][field]) ? rate(snapshot["network"][field].as<double>()) : "ERROR";
}

static void drawHistoryLane(const Rect &r, JsonArray history, const char *field,
                             const char *label, uint16_t color) {
  const auto scale = Panel::historyScale(history, field);
  const Rect plot{static_cast<int16_t>(r.x + 30), static_cast<int16_t>(r.y + 16),
                  static_cast<int16_t>(r.w - 38), static_cast<int16_t>(r.h - 20)};
  gfx->fillRect(r.x, r.y, r.w, r.h, C::CARD);
  textBox({r.x, r.y, 35, 14}, label, color);
  textBox({static_cast<int16_t>(r.x + 36), r.y, static_cast<int16_t>(r.w - 43), 14},
          String(scale.low, 0) + " - " + String(scale.high, 0) + "%", C::MUTED, 1, Align::Right);
  if (plot.h < 8) return;
  if (plot.h >= 26) {
    textBox({r.x, plot.y, 26, 12}, String(scale.high, 0), C::MUTED, 1, Align::Right);
    textBox({r.x, static_cast<int16_t>(plot.y + plot.h - 12), 26, 12}, String(scale.low, 0), C::MUTED, 1, Align::Right);
  }
  for (uint8_t i = 0; i < 3; ++i)
    gfx->drawFastHLine(plot.x, plot.y + (plot.h - 1) * i / 2, plot.w, C::GRID);
  if (history.size() < 2) {
    textBox(plot, "Sin muestras suficientes", C::MUTED, 1, Align::Center);
    return;
  }
  bool connected = false;
  int16_t previousX = 0, previousY = 0;
  const size_t first = history.size() > static_cast<size_t>(plot.w) ? history.size() - plot.w : 0;
  for (size_t i = first; i < history.size(); ++i) {
    if (!Panel::number(history[i][field])) { connected = false; continue; }
    const int16_t x = plot.x + (i - first) * (plot.w - 1) / (history.size() - first - 1);
    const float value = constrain(history[i][field].as<float>(), scale.low, scale.high);
    const int16_t y = plot.y + plot.h - 1 - (value - scale.low) * (plot.h - 1) / (scale.high - scale.low);
    if (connected) gfx->drawLine(previousX, previousY, x, y, color);
    previousX = x; previousY = y; connected = true;
  }
  if (connected) gfx->fillCircle(previousX, previousY, 2, color);
}

static void drawHistoryChart(const Rect &card, JsonArray history,
                             uint32_t intervalMs, bool full) {
  if (full) cardFrame(card, "HISTORICO");
  const uint32_t seconds = history.size() > 1 ? (history.size() - 1) * intervalMs / 1000 : 0;
  const String duration = seconds >= 60 ? String(seconds / 60) + "m " + String(seconds % 60) + "s" : String(seconds) + " s";
  textBox({static_cast<int16_t>(card.x + card.w - 67), static_cast<int16_t>(card.y + 3), 59, 17},
          duration, C::MUTED, 1, Align::Right);
  const int16_t laneH = (card.h - 27) / 2;
  drawHistoryLane({static_cast<int16_t>(card.x + 8), static_cast<int16_t>(card.y + 25),
                   static_cast<int16_t>(card.w - 16), laneH}, history, "cpu", "CPU", C::VALUE);
  drawHistoryLane({static_cast<int16_t>(card.x + 8), static_cast<int16_t>(card.y + 25 + laneH),
                   static_cast<int16_t>(card.w - 16), laneH}, history, "ram", "RAM", C::OK);
}

static void drawChart(const Rect &card, bool full) {
  drawHistoryChart(card, snapshot["history"].as<JsonArray>(),
                   snapshot["history_interval_ms"] | 5000u, full);
}

static void drawOverview(bool full) {
  const int16_t topH = 51;
  drawStatCard(kpiRect(0, 2, CONTENT_TOP, topH), "CPU",
               numberText(snapshot["cpu"]["percent"], 1, "%"), C::VALUE, full);
  drawStatCard(kpiRect(1, 2, CONTENT_TOP, topH), "MEMORIA RAM",
               numberText(snapshot["memory"]["percent"], 1, "%"), C::OK, full);
  const int16_t y = CONTENT_TOP + topH + GAP;
  drawChart({MARGIN, y, static_cast<int16_t>(SCREEN_W - MARGIN * 2),
             static_cast<int16_t>(CONTENT_BOTTOM - y)}, full);
}

static void drawStorageList(const Rect &card, bool full) {
  const uint32_t signature = storageSignature();
  if (!full && signature == listSignatures[PAGE_STORAGE]) return;
  listSignatures[PAGE_STORAGE] = signature;
  const Rect body{static_cast<int16_t>(card.x + 4), static_cast<int16_t>(card.y + 25),
                  static_cast<int16_t>(card.w - 8), static_cast<int16_t>(card.h - 29)};
  gfx->fillRect(body.x, body.y, body.w, body.h, C::CARD);
  std::vector<JsonObject> rows;
  for (JsonObject item : snapshot["storage"]["mounts"].as<JsonArray>()) {
    const String key = physicalDeviceKey(item["device"] | "");
    if (!key.length()) continue;
    auto match = std::find_if(rows.begin(), rows.end(), [&](JsonObject row) {
      return physicalDeviceKey(row["device"] | "") == key;
    });
    if (match == rows.end()) rows.push_back(item);
    else if (String(item["mount"] | "") == "/") *match = item;
  }
  const uint8_t perPage = u8Max(1, body.h / 37);
  const size_t start = listStart(card, "DISPOSITIVOS " + String(rows.size()), PAGE_STORAGE, rows.size(), perPage);
  for (size_t i = start; i < rows.size() && i < start + perPage; ++i) {
    JsonObject item = rows[i];
    const int16_t y = body.y + (i - start) * 37;
    const bool ok = String(item["status"] | "ok") == "ok" && Panel::number(item["percent"]);
    const bool unmounted = String(item["status"] | "") == "unmounted";
    const String name = physicalDeviceKey(item["device"] | "--");
    const float pct = ok ? item["percent"].as<float>() : 0;
    const uint16_t color = ok ? healthColor(pct, 80, 92) : unmounted ? C::WARN : C::BAD;
    textBox({static_cast<int16_t>(body.x + 4), y, static_cast<int16_t>(body.w - 85), 15},
            name, C::TEXT);
    textBox({static_cast<int16_t>(body.x + body.w - 82), y, 77, 15},
            ok ? String(pct, 0) + "%" : unmounted ? "SIN MONTAR" : "ERROR", color, 1, Align::Right);
    String capacity = "Lectura no disponible";
    if (ok && Panel::number(item["total_bytes"]) && Panel::number(item["used_bytes"]))
      capacity = bytes(item["used_bytes"].as<double>()) + " / " + bytes(item["total_bytes"].as<double>());
    else if (unmounted && Panel::number(item["size_bytes"])) capacity = bytes(item["size_bytes"].as<double>());
    textBox({static_cast<int16_t>(body.x + 4), static_cast<int16_t>(y + 15),
             static_cast<int16_t>(body.w - 8), 15}, capacity, C::MUTED);
    if (ok) progressBar({static_cast<int16_t>(body.x + 4), static_cast<int16_t>(y + 32),
                        static_cast<int16_t>(body.w - 8), 2}, pct, color);
    else divider(body.x + 4, y + 32, body.w - 8);
  }
  if (rows.empty()) textBox(body, collectionFailed("block_devices") ? "ERROR DE LECTURA" : "Sin dispositivos detectados",
                            collectionFailed("block_devices") ? C::BAD : C::MUTED, 1, Align::Center);
}

static void drawStorage(bool full) {
  const float disk = rootDiskPercent();
  const uint16_t diskColor = rootDiskColor();
  const uint16_t ioColor = diskIoOk() ? C::ACCENT : C::BAD;
  if (LANDSCAPE) {
    const int16_t h = 50;
    drawStatCard(kpiRect(0, 3, CONTENT_TOP, h), "LECTURA",
                 diskRateText("read_bps"), ioColor, full);
    drawStatCard(kpiRect(1, 3, CONTENT_TOP, h), "ESCRITURA",
                 diskRateText("write_bps"), diskIoOk() ? C::OK : C::BAD, full);
    drawStatCard(kpiRect(2, 3, CONTENT_TOP, h), "RAIZ LIBRE",
                 rootFreeText(), diskColor, full);
    const Rect list{MARGIN, static_cast<int16_t>(CONTENT_TOP + h + GAP),
                    static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                    static_cast<int16_t>(CONTENT_BOTTOM - CONTENT_TOP - h - GAP)};
    if (full) cardFrame(list, "ALMACENAMIENTO", C::WARN);
    drawStorageList(list, full);
  } else {
    const int16_t h = 46;
    drawStatCard(kpiRect(0, 2, CONTENT_TOP, h), "LECTURA",
                 diskRateText("read_bps"), ioColor, full);
    drawStatCard(kpiRect(1, 2, CONTENT_TOP, h), "ESCRITURA",
                 diskRateText("write_bps"), diskIoOk() ? C::OK : C::BAD, full);
    const Rect root{MARGIN, static_cast<int16_t>(CONTENT_TOP + h + GAP),
                    static_cast<int16_t>(SCREEN_W - MARGIN * 2), 42};
    if (full) cardFrame(root);
    textBox({static_cast<int16_t>(root.x + 8), static_cast<int16_t>(root.y + 5), 80, 12},
            "DISCO RAIZ", C::MUTED, 1, Align::Left, C::CARD);
    textBox({static_cast<int16_t>(root.x + 94), static_cast<int16_t>(root.y + 5),
             static_cast<int16_t>(root.w - 102), 12},
            "Libre " + rootFreeText(), rootDiskOk() ? C::VALUE : C::BAD, 1, Align::Right);
    valueBox({static_cast<int16_t>(root.x + 8), static_cast<int16_t>(root.y + 17), 55, 17},
             rootDiskText(), diskColor, Align::Left, C::CARD);
    progressBar({static_cast<int16_t>(root.x + 70), static_cast<int16_t>(root.y + 24),
                 static_cast<int16_t>(root.w - 80), 5},
                disk, diskColor);
    const Rect list{MARGIN, static_cast<int16_t>(root.y + root.h + GAP),
                    static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                    static_cast<int16_t>(CONTENT_BOTTOM - root.y - root.h - GAP)};
    if (full) cardFrame(list, "ALMACENAMIENTO", C::WARN);
    drawStorageList(list, full);
  }
}

static void drawInterfaceList(const Rect &card, bool full) {
  const uint32_t signature = interfaceSignature();
  if (!full && signature == listSignatures[PAGE_NETWORK]) return;
  listSignatures[PAGE_NETWORK] = signature;
  const Rect body{static_cast<int16_t>(card.x + 4), static_cast<int16_t>(card.y + 24),
                  static_cast<int16_t>(card.w - 8), static_cast<int16_t>(card.h - 28)};
  gfx->fillRect(body.x, body.y, body.w, body.h, C::CARD);
  const int16_t rowH = 33;
  const uint8_t maxRows = u8Max(1, static_cast<uint8_t>(body.h / rowH));
  const auto rows = Panel::interfaces(snapshot["interfaces"].as<JsonArray>());
  const size_t start = listStart(card, "REDES " + String(rows.size()), PAGE_NETWORK, rows.size(), maxRows);
  uint8_t rowIndex = 0;
  for (size_t i = start; i < rows.size() && rowIndex < maxRows; ++i) {
    JsonObject item = rows[i];
    const int16_t y = body.y + rowIndex * rowH;
    const bool up = item["up"] | false;
    const uint16_t bg = (rowIndex & 1) ? C::CARD_ALT : C::CARD;
    if (rowIndex & 1) gfx->fillRect(body.x, y, body.w, rowH, bg);
    gfx->fillCircle(body.x + 9, y + 7, 3, up ? C::OK : C::BAD);
    const String name = item["name"] | "--";
    const String displayName = item["display_name"] | "";
    textBox({static_cast<int16_t>(body.x + 17), y, static_cast<int16_t>(body.w - 85), 14},
            displayName.length() ? displayName : Panel::interfaceName(name), C::TEXT, 1, Align::Left, bg);
    textBox({static_cast<int16_t>(body.x + body.w - 70), y, 66, 14},
            up ? "ACTIVA" : "SIN ENLACE", up ? C::OK : C::BAD, 1, Align::Right, bg);
    const int kind = Panel::interfaceOrder(name);
    String speed = item["speed_mbps"].isNull() ? "Enlace s/d" : String(item["speed_mbps"].as<int>()) + " Mb/s";
    if (kind >= 4) speed = "Red virtual";
    else if (kind == 2) speed = "Tunel VPN";
    const String ip = item["ipv4"] | "--";
    const int16_t speedW = LANDSCAPE ? 92 : 70;
    textBox({static_cast<int16_t>(body.x + 5), static_cast<int16_t>(y + 15),
             static_cast<int16_t>(body.w - speedW - 8), 15},
            ip == "--" ? "Sin IP propia" : "IP " + ip, C::MUTED, 1, Align::Left, bg);
    textBox({static_cast<int16_t>(body.x + body.w - speedW), static_cast<int16_t>(y + 15),
             static_cast<int16_t>(speedW - 4), 15}, speed, C::VALUE, 1, Align::Right, bg);
    ++rowIndex;
  }
  if (rowIndex == 0) textBox(body, collectionFailed("interfaces") ? "ERROR DE LECTURA" : "Sin interfaces",
                              collectionFailed("interfaces") ? C::BAD : C::DIM, 1, Align::Center, C::CARD);
}

static void drawNetwork(bool full) {
  const int16_t h = 53;
  drawStatCard(kpiRect(0, 2, CONTENT_TOP, h), "SUBIDA",
               trafficText("tx_bps"), C::OK, full);
  drawStatCard(kpiRect(1, 2, CONTENT_TOP, h), "DESCARGA",
               trafficText("rx_bps"), C::VALUE, full);
  const Rect list{MARGIN, static_cast<int16_t>(CONTENT_TOP + h + GAP),
                  static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                  static_cast<int16_t>(CONTENT_H - h - GAP)};
  if (full) cardFrame(list, "INTERFACES");
  drawInterfaceList(list, full);
}

static void drawCoreRows(const Rect &card, bool full) {
  if (full) cardFrame(card, "NUCLEOS", C::ACCENT);
  const Rect body{static_cast<int16_t>(card.x + 7), static_cast<int16_t>(card.y + 25),
                  static_cast<int16_t>(card.w - 14), static_cast<int16_t>(card.h - 31)};
  JsonArray cores = snapshot["cpu"]["per_core"].as<JsonArray>();
  const uint8_t maxRows = u8Min(static_cast<uint8_t>(cores.size()), static_cast<uint8_t>(LANDSCAPE ? 4 : 5));
  const int16_t rowH = maxRows ? body.h / maxRows : body.h;
  for (uint8_t i = 0; i < maxRows; ++i) {
    const bool valid = Panel::number(cores[i]);
    const float value = valid ? cores[i].as<float>() : 0;
    const int16_t y = body.y + i * rowH;
    textBox({body.x, y, 34, rowH}, "CPU" + String(i), C::MUTED, 1, Align::Left, C::CARD);
    progressBar({static_cast<int16_t>(body.x + 37), static_cast<int16_t>(y + rowH / 2 - 3),
                 static_cast<int16_t>(body.w - 77), 6},
                value, healthColor(value, 75, 92));
    textBox({static_cast<int16_t>(body.x + body.w - 37), y, 37, rowH},
            valid ? String(value, 0) + "%" : "ERROR", valid ? C::TEXT : C::BAD, 1, Align::Right, C::CARD);
  }
  if (maxRows == 0) textBox(body, "Sin datos por nucleo", C::DIM, 1, Align::Center, C::CARD);
}

static void drawSensorRows(const Rect &card, bool full) {
  if (full) cardFrame(card, "SENSORES", C::WARN);
  const Rect body{static_cast<int16_t>(card.x + 5), static_cast<int16_t>(card.y + 24),
                  static_cast<int16_t>(card.w - 10), static_cast<int16_t>(card.h - 29)};
  gfx->fillRect(body.x, body.y, body.w, body.h, C::CARD);
  const int16_t rowH = LANDSCAPE ? 18 : 20;
  const uint8_t maxRows = u8Max(1, static_cast<uint8_t>(body.h / rowH));
  JsonArray sensors = snapshot["temperatures"].as<JsonArray>();
  const size_t start = listStart(card, "SONDAS", PAGE_CPU, sensors.size(), maxRows);
  uint8_t index = 0;
  for (size_t i = start; i < sensors.size() && index < maxRows; ++i) {
    JsonObject item = sensors[i];
    const int16_t y = body.y + index * rowH;
    const float value = item["current_c"] | 0.0f;
    const uint16_t bg = (index & 1) ? C::CARD_ALT : C::CARD;
    if (index & 1) gfx->fillRect(body.x, y, body.w, rowH, bg);
    textBox({static_cast<int16_t>(body.x + 4), y, static_cast<int16_t>(body.w - 58), rowH},
            Panel::sensorName(item["name"] | "", i), C::TEXT, 1, Align::Left, bg);
    textBox({static_cast<int16_t>(body.x + body.w - 54), y, 50, rowH},
            numberText(item["current_c"], 1, " C"),
            Panel::number(item["current_c"]) ? healthColor(value, 70, 80) : C::BAD, 1, Align::Right, bg);
    ++index;
  }
  if (index == 0) textBox(body, "Sin sensores", C::DIM, 1, Align::Center, C::CARD);
}

static void drawCpu(bool full) {
  const float cpu = snapshot["cpu"]["percent"] | 0.0f;
  const float temp = snapshot["cpu"]["temperature_c"] | 0.0f;
  if (LANDSCAPE) {
    const int16_t h = 51;
    drawStatCard(kpiRect(0, 4, CONTENT_TOP, h), "CPU",
                 numberText(snapshot["cpu"]["percent"], 0, "%"), healthColor(cpu, 70, 90), full);
    drawStatCard(kpiRect(1, 4, CONTENT_TOP, h), "TEMP",
                 numberText(snapshot["cpu"]["temperature_c"], 1, " C"), healthColor(temp, 70, 80), full);
    drawStatCard(kpiRect(2, 4, CONTENT_TOP, h), "MHz",
                 numberText(snapshot["cpu"]["frequency_mhz"]), C::VALUE, full);
    drawStatCard(kpiRect(3, 4, CONTENT_TOP, h), "CARGA 1m",
                 numberText(snapshot["cpu"]["load"][0], 2), C::VALUE, full);
    const int16_t y = CONTENT_TOP + h + GAP;
    const int16_t half = (SCREEN_W - MARGIN * 2 - GAP) / 2;
    drawCoreRows({MARGIN, y, half, static_cast<int16_t>(CONTENT_BOTTOM - y)}, full);
    drawSensorRows({static_cast<int16_t>(MARGIN + half + GAP), y, half,
                    static_cast<int16_t>(CONTENT_BOTTOM - y)}, full);
  } else {
    drawStatCard(kpiRect(0, 2, CONTENT_TOP, 51), "CPU",
                 numberText(snapshot["cpu"]["percent"], 1, "%"), healthColor(cpu, 70, 90), full);
    drawStatCard(kpiRect(1, 2, CONTENT_TOP, 51), "TEMP",
                 numberText(snapshot["cpu"]["temperature_c"], 1, " C"), healthColor(temp, 70, 80), full);
    const int16_t metaY = CONTENT_TOP + 56;
    textBox({MARGIN, metaY, static_cast<int16_t>(SCREEN_W - MARGIN * 2), 22},
            numberText(snapshot["cpu"]["frequency_mhz"], 0, " MHz") + "   Carga 1m: " +
            numberText(snapshot["cpu"]["load"][0], 2), C::MUTED, 1, Align::Center);
    const int16_t coreY = metaY + 27;
    drawCoreRows({MARGIN, coreY, static_cast<int16_t>(SCREEN_W - MARGIN * 2), 82}, full);
    const int16_t sensorY = coreY + 87;
    drawSensorRows({MARGIN, sensorY, static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                    static_cast<int16_t>(CONTENT_BOTTOM - sensorY)}, full);
  }
}

static void drawPorts(bool full) {
  const Rect card{MARGIN, CONTENT_TOP, static_cast<int16_t>(SCREEN_W - MARGIN * 2), CONTENT_H};
  if (full) cardFrame(card, "SERVICIOS EN ESCUCHA", C::ACCENT);
  const uint32_t signature = portSignature();
  if (!full && signature == listSignatures[PAGE_PORTS]) return;
  listSignatures[PAGE_PORTS] = signature;

  const Rect body{static_cast<int16_t>(card.x + 4), static_cast<int16_t>(card.y + 24),
                  static_cast<int16_t>(card.w - 8), static_cast<int16_t>(card.h - 28)};
  gfx->fillRect(body.x, body.y, body.w, body.h, C::CARD);
  const int16_t rowH = 32;
  const uint8_t maxRows = u8Max(1, static_cast<uint8_t>(body.h / rowH));
  const auto rows = Panel::ports(snapshot["ports"].as<JsonArray>());
  const size_t start = listStart(card, "SERVICIOS " + String(rows.size()), PAGE_PORTS, rows.size(), maxRows);
  uint8_t index = 0;
  for (size_t i = start; i < rows.size() && index < maxRows; ++i) {
    const auto &item = rows[i];
    const int16_t y = body.y + index * rowH;
    const uint16_t bg = (index & 1) ? C::CARD_ALT : C::CARD;
    if (index & 1) gfx->fillRect(body.x, y, body.w, rowH, bg);
    String protocol = item.protocol;
    protocol.toUpperCase();
    textBox({static_cast<int16_t>(body.x + 5), y, 65, 15},
            String(item.number) + "/" + protocol,
            C::ACCENT, 1, Align::Left, bg);
    textBox({static_cast<int16_t>(body.x + 74), y, static_cast<int16_t>(body.w - 80), 15},
            Panel::serviceName(item.service), C::TEXT, 1, Align::Left, bg);
    const String family = item.v4 && item.v6 ? "IPv4+IPv6" : (item.v6 ? "IPv6" : "IPv4");
    textBox({static_cast<int16_t>(body.x + 5), static_cast<int16_t>(y + 15),
             static_cast<int16_t>(body.w - 10), 15},
            item.address + " / " + family, C::MUTED, 1, Align::Left, bg);
    ++index;
  }
  if (index == 0) textBox(body, collectionFailed("ports") ? "ERROR DE LECTURA" : "Sin puertos disponibles",
                          collectionFailed("ports") ? C::BAD : C::DIM, 1, Align::Center, C::CARD);
}

static void drawDockerList(const Rect &card, bool full) {
  const uint32_t signature = dockerSignature();
  if (!full && signature == listSignatures[PAGE_DOCKER]) return;
  listSignatures[PAGE_DOCKER] = signature;
  const Rect body{static_cast<int16_t>(card.x + 4), static_cast<int16_t>(card.y + 24),
                  static_cast<int16_t>(card.w - 8), static_cast<int16_t>(card.h - 28)};
  gfx->fillRect(body.x, body.y, body.w, body.h, C::CARD);
  const int16_t rowH = 16;
  const uint8_t maxRows = u8Max(1, static_cast<uint8_t>(body.h / rowH));
  JsonArray containers = snapshot["docker"]["containers"].as<JsonArray>();
  const size_t start = listStart(card, "CONTENEDORES " + String(containers.size()), PAGE_DOCKER, containers.size(), maxRows);
  uint8_t index = 0;
  for (size_t i = start; i < containers.size() && index < maxRows; ++i) {
    JsonObject item = containers[i];
    const int16_t y = body.y + index * rowH;
    const String state = item["state"] | "unknown";
    const bool running = state == "running";
    const uint16_t bg = (index & 1) ? C::CARD_ALT : C::CARD;
    if (index & 1) gfx->fillRect(body.x, y, body.w, rowH, bg);
    gfx->fillCircle(body.x + 9, y + rowH / 2, 3, running ? C::OK : C::WARN);
    const int16_t stateW = 62;
    const int16_t nameW = body.w - 17 - stateW;
    textBox({static_cast<int16_t>(body.x + 17), y, nameW, rowH},
            item["name"] | "--", C::TEXT, 1, Align::Left, bg);
    const String label = running ? "ACTIVO" : state == "paused" ? "PAUSA" :
                         state == "restarting" ? "REINICIO" : state == "dead" ? "ERROR" :
                         state == "created" ? "CREADO" : state == "exited" ? "PARADO" : "S/D";
    textBox({static_cast<int16_t>(body.x + body.w - stateW), y,
             static_cast<int16_t>(stateW - 5), rowH},
            label, running ? C::OK : C::WARN, 1, Align::Right, bg);
    ++index;
  }
  if (index == 0) {
    const bool dockerAvailable = snapshot["docker"]["available"] | false;
    const String message = dockerAvailable
                               ? "No hay contenedores"
                               : String(snapshot["docker"]["error"] | "Docker no disponible");
    textBox(body, message, C::DIM, 1, Align::Center, C::CARD);
  }
}

static void drawDocker(bool full) {
  const bool available = snapshot["docker"]["available"] | false;
  const int16_t h = 48;
  drawStatCard(kpiRect(0, 3, CONTENT_TOP, h), "MOTOR", available ? "OK" : "ERROR",
               available ? C::OK : C::BAD, full);
  drawStatCard(kpiRect(1, 3, CONTENT_TOP, h), "ACTIVOS",
               available ? numberText(snapshot["docker"]["running"]) : "ERROR", C::OK, full);
  drawStatCard(kpiRect(2, 3, CONTENT_TOP, h), "TOTAL",
               available ? numberText(snapshot["docker"]["total"]) : "ERROR", C::VALUE, full);
  const Rect list{MARGIN, static_cast<int16_t>(CONTENT_TOP + h + GAP),
                  static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                  static_cast<int16_t>(CONTENT_H - h - GAP)};
  if (full) cardFrame(list, "CONTENEDORES", C::OK);
  drawDockerList(list, full);
}

static bool proxmoxAvailable() {
  return snapshot["proxmox"]["available"] | false;
}

enum class PveState : uint8_t { Online, Offline, Unconfigured, Error };

static PveState proxmoxState() {
  if (proxmoxAvailable()) return PveState::Online;
  const String state = snapshot["proxmox"]["state"] | "";
  if (state == "offline") return PveState::Offline;
  if (state == "unconfigured") return PveState::Unconfigured;
  return PveState::Error;
}

// Offline or not configured are expected situations: show "--" in amber, not a red error.
static bool proxmoxOffline() {
  const PveState state = proxmoxState();
  return state == PveState::Offline || state == PveState::Unconfigured;
}

static String proxmoxApiText() {
  switch (proxmoxState()) {
    case PveState::Online: return "OK";
    case PveState::Offline: return "OFFLINE";
    case PveState::Unconfigured: return "SIN CONFIG";
    default: return "ERROR";
  }
}

static String proxmoxUnavailableText() {
  switch (proxmoxState()) {
    case PveState::Offline: return "PROXMOX OFFLINE";
    case PveState::Unconfigured: return "PROXMOX SIN CONFIGURAR";
    default: return "ERROR DE PROXMOX";
  }
}

static JsonObject proxmoxNode() {
  JsonArray nodes = snapshot["proxmox"]["nodes"].as<JsonArray>();
  return nodes.size() ? nodes[0].as<JsonObject>() : JsonObject();
}

static void drawProxmox(bool full) {
  const bool available = proxmoxAvailable();
  const bool offline = proxmoxOffline();
  JsonObject node = proxmoxNode();
  const bool onlineNode = available && node && String(node["status"] | "") == "online";
  const bool cpuValid = onlineNode && Panel::number(node["cpu_percent"]);
  const bool ramValid = onlineNode && Panel::number(node["memory_percent"]);
  const float cpu = cpuValid ? node["cpu_percent"].as<float>() : 0;
  const float ram = ramValid ? node["memory_percent"].as<float>() : 0;
  const uint8_t columns = 2;
  const int16_t h = (CONTENT_H - GAP) / 2;
  const String labels[] = {"CPU", "RAM", "UPTIME", "API"};
  const String values[] = {
    cpuValid ? numberText(node["cpu_percent"], 0, "%") : offline ? "--" : "ERROR",
    ramValid ? numberText(node["memory_percent"], 0, "%") : offline ? "--" : "ERROR",
    onlineNode ? uptimeText(node["uptime_s"] | 0ULL) : offline ? "--" : "ERROR",
    proxmoxApiText()
  };
  const uint16_t unavailableColor = offline ? C::WARN : C::BAD;
  const uint16_t colors[] = {cpuValid ? healthColor(cpu, 70, 90) : unavailableColor,
                             ramValid ? healthColor(ram, 75, 90) : unavailableColor,
                             onlineNode ? C::VALUE : unavailableColor,
                             available ? C::OK : unavailableColor};
  for (uint8_t i = 0; i < 4; ++i) {
    const int16_t y = CONTENT_TOP + (i / columns) * (h + GAP);
    drawStatCard(kpiRect(i % columns, columns, y, h), labels[i], values[i], colors[i], full);
  }
}

static void drawPvePerformance(bool full) {
  JsonObject node = proxmoxNode();
  const bool available = proxmoxAvailable() && node;
  const bool offline = proxmoxOffline();
  const uint16_t unavailableColor = offline ? C::WARN : C::BAD;
  const int16_t topH = 51;
  drawStatCard(kpiRect(0, 2, CONTENT_TOP, topH), "CPU PVE",
               available ? numberText(node["cpu_percent"], 1, "%") : offline ? "--" : "ERROR",
               available ? C::VALUE : unavailableColor, full);
  drawStatCard(kpiRect(1, 2, CONTENT_TOP, topH), "RAM PVE",
               available ? numberText(node["memory_percent"], 1, "%") : offline ? "--" : "ERROR",
               available ? C::OK : unavailableColor, full);
  const int16_t y = CONTENT_TOP + topH + GAP;
  const Rect chart{MARGIN, y, static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                   static_cast<int16_t>(CONTENT_BOTTOM - y)};
  if (offline) {
    if (full) cardFrame(chart, "HISTORICO");
    const Rect body{static_cast<int16_t>(chart.x + 4), static_cast<int16_t>(chart.y + 24),
                    static_cast<int16_t>(chart.w - 8), static_cast<int16_t>(chart.h - 28)};
    gfx->fillRect(body.x, body.y, body.w, body.h, C::CARD);
    const String message = proxmoxUnavailableText();
    textBox(body, message, C::WARN, Typography::fitSize(message, body.w - 8, body.h, Typography::Heading),
            Align::Center, C::CARD);
  } else {
    drawHistoryChart(chart, snapshot["proxmox"]["history"].as<JsonArray>(),
                     snapshot["proxmox"]["history_interval_ms"] | 8000u, full);
  }
}

static void drawPveGuests(bool full) {
  const bool available = proxmoxAvailable();
  const bool offline = proxmoxOffline();
  const uint16_t unavailableColor = offline ? C::WARN : C::BAD;
  const int16_t h = 48;
  drawStatCard(kpiRect(0, 3, CONTENT_TOP, h), "API", proxmoxApiText(),
               available ? C::OK : unavailableColor, full);
  drawStatCard(kpiRect(1, 3, CONTENT_TOP, h), "ACTIVOS",
               available ? numberText(snapshot["proxmox"]["guests_running"]) : offline ? "--" : "ERROR",
               available ? C::OK : unavailableColor, full);
  drawStatCard(kpiRect(2, 3, CONTENT_TOP, h), "TOTAL",
               available ? numberText(snapshot["proxmox"]["guests_total"]) : offline ? "--" : "ERROR",
               available ? C::VALUE : unavailableColor, full);
  const Rect card{MARGIN, static_cast<int16_t>(CONTENT_TOP + h + GAP),
                  static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                  static_cast<int16_t>(CONTENT_H - h - GAP)};
  if (full) cardFrame(card, "MAQUINAS", C::OK);
  const uint32_t signature = proxmoxSignature("guests");
  if (!full && signature == listSignatures[PAGE_PVE_GUESTS]) return;
  listSignatures[PAGE_PVE_GUESTS] = signature;
  const Rect body{static_cast<int16_t>(card.x + 4), static_cast<int16_t>(card.y + 24),
                  static_cast<int16_t>(card.w - 8), static_cast<int16_t>(card.h - 28)};
  gfx->fillRect(body.x, body.y, body.w, body.h, C::CARD);
  JsonArray guests = snapshot["proxmox"]["guests"].as<JsonArray>();
  const int16_t rowH = 32;
  const uint8_t maxRows = u8Max(1, static_cast<uint8_t>(body.h / rowH));
  const size_t start = listStart(card, "VM / LXC " + String(guests.size()), PAGE_PVE_GUESTS,
                                 guests.size(), maxRows);
  uint8_t row = 0;
  for (size_t i = start; i < guests.size() && row < maxRows; ++i, ++row) {
    JsonObject item = guests[i];
    const int16_t y = body.y + row * rowH;
    const uint16_t bg = (row & 1) ? C::CARD_ALT : C::CARD;
    if (row & 1) gfx->fillRect(body.x, y, body.w, rowH, bg);
    const bool running = String(item["status"] | "") == "running";
    gfx->fillCircle(body.x + 8, y + 8, 3, running ? C::OK : C::WARN);
    textBox({static_cast<int16_t>(body.x + 16), y, static_cast<int16_t>(body.w - 87), 16},
            String(item["id"] | 0) + "  " + String(item["name"] | "--"), C::TEXT, 1, Align::Left, bg);
    textBox({static_cast<int16_t>(body.x + body.w - 68), y, 64, 16},
            String(item["type"] | "--") + (running ? " ON" : " OFF"),
            running ? C::OK : C::WARN, 1, Align::Right, bg);
    textBox({static_cast<int16_t>(body.x + 16), static_cast<int16_t>(y + 16),
             static_cast<int16_t>(body.w - 20), 16},
            String("CPU ") + numberText(item["cpu_percent"], 0, "%") + "   RAM " +
            numberText(item["memory_percent"], 0, "%") + "   DISCO " +
            numberText(item["disk_percent"], 0, "%"), C::MUTED, 1, Align::Left, bg);
  }
  if (!row) textBox(body, available ? "Sin maquinas" : proxmoxUnavailableText(),
                    available ? C::DIM : unavailableColor, 1, Align::Center, C::CARD);
}

static void drawPveStorage(bool full) {
  const bool available = proxmoxAvailable();
  const Rect card{MARGIN, CONTENT_TOP, static_cast<int16_t>(SCREEN_W - MARGIN * 2), CONTENT_H};
  if (full) cardFrame(card, "ALMACENAMIENTO PROXMOX", C::WARN);
  const uint32_t signature = proxmoxSignature("storage");
  if (!full && signature == listSignatures[PAGE_PVE_STORAGE]) return;
  listSignatures[PAGE_PVE_STORAGE] = signature;
  const Rect body{static_cast<int16_t>(card.x + 4), static_cast<int16_t>(card.y + 24),
                  static_cast<int16_t>(card.w - 8), static_cast<int16_t>(card.h - 28)};
  gfx->fillRect(body.x, body.y, body.w, body.h, C::CARD);
  JsonArray stores = snapshot["proxmox"]["storage"].as<JsonArray>();
  const int16_t rowH = 39;
  const uint8_t maxRows = u8Max(1, static_cast<uint8_t>(body.h / rowH));
  const size_t start = listStart(card, "DISCOS " + String(stores.size()), PAGE_PVE_STORAGE,
                                 stores.size(), maxRows);
  uint8_t row = 0;
  for (size_t i = start; i < stores.size() && row < maxRows; ++i, ++row) {
    JsonObject item = stores[i];
    const int16_t y = body.y + row * rowH;
    const uint16_t bg = (row & 1) ? C::CARD_ALT : C::CARD;
    if (row & 1) gfx->fillRect(body.x, y, body.w, rowH, bg);
    const bool ok = String(item["status"] | "") == "available";
    const bool valid = Panel::number(item["percent"]);
    const float percent = valid ? item["percent"].as<float>() : 0;
    textBox({static_cast<int16_t>(body.x + 5), y, static_cast<int16_t>(body.w - 70), 17},
            item["name"] | "--", C::TEXT, 1, Align::Left, bg);
    textBox({static_cast<int16_t>(body.x + body.w - 64), y, 59, 17},
            ok ? (valid ? String(percent, 0) + "%" : "ERROR") : "ERROR",
            ok && valid ? healthColor(percent, 80, 92) : C::BAD, 1, Align::Right, bg);
    progressBar({static_cast<int16_t>(body.x + 5), static_cast<int16_t>(y + 21),
                 static_cast<int16_t>(body.w - 10), 5}, percent,
                ok && valid ? healthColor(percent, 80, 92) : C::BAD, bg);
    textBox({static_cast<int16_t>(body.x + 5), static_cast<int16_t>(y + 27),
             static_cast<int16_t>(body.w - 10), 12},
            ok && valid ? bytes(item["used_bytes"] | 0.0) + " / " + bytes(item["total_bytes"] | 0.0)
                        : "Lectura no disponible",
            ok && valid ? C::MUTED : C::BAD, 1, Align::Left, bg);
  }
  if (!row) textBox(body, available ? "Sin almacenamientos" : proxmoxUnavailableText(),
                    available ? C::DIM : proxmoxOffline() ? C::WARN : C::BAD,
                    1, Align::Center, C::CARD);
}

static void drawPveTasks(bool full) {
  const bool available = proxmoxAvailable();
  const Rect card{MARGIN, CONTENT_TOP, static_cast<int16_t>(SCREEN_W - MARGIN * 2), CONTENT_H};
  if (full) cardFrame(card, "ACTIVIDAD PROXMOX", C::VALUE);
  const uint32_t signature = proxmoxSignature("tasks");
  if (!full && signature == listSignatures[PAGE_PVE_TASKS]) return;
  listSignatures[PAGE_PVE_TASKS] = signature;
  const Rect body{static_cast<int16_t>(card.x + 4), static_cast<int16_t>(card.y + 24),
                  static_cast<int16_t>(card.w - 8), static_cast<int16_t>(card.h - 28)};
  gfx->fillRect(body.x, body.y, body.w, body.h, C::CARD);
  JsonArray tasks = snapshot["proxmox"]["tasks"].as<JsonArray>();
  const int16_t rowH = 39;
  const uint8_t maxRows = u8Max(1, static_cast<uint8_t>(body.h / rowH));
  const int failed = snapshot["proxmox"]["tasks_failed"] | 0;
  const int cancelled = snapshot["proxmox"]["tasks_cancelled"] | 0;
  const String title = failed ? String("TAREAS ") + String(tasks.size()) + " / " +
                                  String(failed) + " ERROR"
                              : cancelled ? String("TAREAS ") + String(tasks.size()) + " / " +
                                              String(cancelled) + " AVISO"
                              : String("TAREAS ") + String(tasks.size());
  const size_t start = listStart(card, title, PAGE_PVE_TASKS, tasks.size(), maxRows);
  uint8_t row = 0;
  for (size_t i = start; i < tasks.size() && row < maxRows; ++i, ++row) {
    JsonObject item = tasks[i];
    const int16_t y = body.y + row * rowH;
    const uint16_t bg = (row & 1) ? C::CARD_ALT : C::CARD;
    if (row & 1) gfx->fillRect(body.x, y, body.w, rowH, bg);
    const String state = item["state"] | "ERROR";
    const String stateText = state == "RUNNING" ? "EN CURSO" :
                             state == "CANCELLED" ? "CANCELADA" : state;
    const uint16_t color = state == "OK" ? C::OK : state == "RUNNING" ? C::VALUE :
                           state == "CANCELLED" ? C::WARN : C::BAD;
    textBox({static_cast<int16_t>(body.x + 5), y, static_cast<int16_t>(body.w - 83), 17},
            item["label"] | "Tarea", C::TEXT, 1, Align::Left, bg);
    textBox({static_cast<int16_t>(body.x + body.w - 76), y, 71, 17},
            stateText, color, 1, Align::Right, bg);
    textBox({static_cast<int16_t>(body.x + 5), static_cast<int16_t>(y + 18),
             static_cast<int16_t>(body.w - 10), 17},
            String(item["started"] | "--") + "  " + String(item["detail"] | "Sin detalle"),
            state == "ERROR" ? C::BAD : C::MUTED, 1, Align::Left, bg);
  }
  if (!row) textBox(body, available ? "Sin tareas recientes" : proxmoxUnavailableText(),
                    available ? C::DIM : proxmoxOffline() ? C::WARN : C::BAD,
                    1, Align::Center, C::CARD);
}

static void drawNetworkMini(const Rect &r, bool full) {
  if (full) cardFrame(r, "RED");
  for (uint8_t i = 0; i < 2; ++i) {
    const bool upload = i == 0;
    const int16_t y = r.y + 27 + i * 18;
    const int16_t x = r.x + 12;
    const uint16_t color = upload ? C::OK : C::VALUE;
    gfx->fillRect(r.x + 8, y, 9, 16, C::CARD);
    gfx->drawFastVLine(x, y + 3, 10, color);
    const int16_t tip = y + (upload ? 2 : 13);
    const int16_t base = y + (upload ? 6 : 9);
    gfx->drawLine(x, tip, x - 3, base, color);
    gfx->drawLine(x, tip, x + 3, base, color);
    textBox({static_cast<int16_t>(r.x + 25), y, static_cast<int16_t>(r.w - 33), 16},
            trafficText(upload ? "tx_bps" : "rx_bps"), color, 4);
  }
}

static void drawTotal(bool full) {
  const float cpu = snapshot["cpu"]["percent"] | 0.0f;
  const float ram = snapshot["memory"]["percent"] | 0.0f;
  const float temp = snapshot["cpu"]["temperature_c"] | 0.0f;
  const int16_t columns = LANDSCAPE ? 4 : 2;
  const int16_t tileH = LANDSCAPE ? 58 : 54;
  const String labels[] = {"CPU", "RAM", "TEMP", "DISCO"};
  const String values[] = {numberText(snapshot["cpu"]["percent"], 0, "%"),
                           numberText(snapshot["memory"]["percent"], 0, "%"),
                           numberText(snapshot["cpu"]["temperature_c"], 0, " C"), rootDiskText()};
  const float percentages[] = {cpu, ram, temp, rootDiskPercent()};
  const uint16_t colors[] = {healthColor(cpu, 70, 90), healthColor(ram, 75, 90),
                             healthColor(temp, 70, 80), rootDiskColor()};
  for (uint8_t i = 0; i < 4; ++i)
    drawKpi(kpiRect(i % columns, columns, CONTENT_TOP + (i / columns) * (tileH + GAP), tileH),
            labels[i], values[i], percentages[i], colors[i], full);
  const int16_t y = CONTENT_TOP + (LANDSCAPE ? 1 : 2) * (tileH + GAP);
  const int16_t half = (SCREEN_W - MARGIN * 2 - GAP) / 2;
  const Rect net{MARGIN, y, half, 70};
  const Rect services{static_cast<int16_t>(MARGIN + half + GAP), y, half, 70};
  drawNetworkMini(net, full);
  if (full) cardFrame(services, "SERVICIOS", C::OK);
  const bool docker = snapshot["docker"]["available"] | false;
  const String active = docker && Panel::number(snapshot["docker"]["running"]) && Panel::number(snapshot["docker"]["total"])
                           ? numberText(snapshot["docker"]["running"]) + "/" + numberText(snapshot["docker"]["total"]) : "ERROR";
  textBox({static_cast<int16_t>(services.x + 8), static_cast<int16_t>(y + 27), 44, 16}, "Docker", C::MUTED);
  textBox({static_cast<int16_t>(services.x + 54), static_cast<int16_t>(y + 27),
           static_cast<int16_t>(services.w - 62), 16}, active, docker ? C::OK : C::BAD, 4, Align::Right);
  textBox({static_cast<int16_t>(services.x + 8), static_cast<int16_t>(y + 45), 44, 16}, "Puertos", C::MUTED);
  textBox({static_cast<int16_t>(services.x + 54), static_cast<int16_t>(y + 45),
           static_cast<int16_t>(services.w - 62), 16},
          !collectionFailed("ports") && snapshot["ports"].is<JsonArray>() ?
              String(Panel::ports(snapshot["ports"].as<JsonArray>()).size()) : "ERROR",
          collectionFailed("ports") ? C::BAD : C::VALUE, 4, Align::Right);
  const Rect status{MARGIN, static_cast<int16_t>(y + 75), static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                    static_cast<int16_t>(CONTENT_BOTTOM - y - 75)};
  if (full) cardFrame(status);
  textBox({static_cast<int16_t>(status.x + 7), status.y, 23, status.h}, "API", C::MUTED);
  textBox({static_cast<int16_t>(status.x + 33), status.y, 56, status.h},
          online ? (strcmp(statusText, "PARCIAL") == 0 ? "AVISO" : "OK") : "ERROR",
          !online ? C::BAD : strcmp(statusText, "PARCIAL") == 0 ? C::WARN : C::OK);
  textBox({static_cast<int16_t>(status.x + 93), status.y, static_cast<int16_t>(status.w - 101), status.h},
          Panel::number(snapshot["system"]["uptime_s"]) ?
          uptimeText(snapshot["system"]["uptime_s"].as<uint64_t>()) + " encendida" : "Sin datos",
          C::MUTED, 1, Align::Right);
}

static String wifiStateText() {
  switch (WiFi.status()) {
    case WL_CONNECTED: return "CONECTADO";
    case WL_NO_SSID_AVAIL: return "SSID NO VISIBLE";
    case WL_CONNECT_FAILED: return "CLAVE/CONEXION";
    case WL_CONNECTION_LOST: return "PERDIDA";
    case WL_DISCONNECTED: return "DESCONECTADO";
    default: return "CONECTANDO";
  }
}

static void drawDiagRow(int16_t y, int16_t h, int16_t labelW, const String &label,
                        const String &value, uint16_t color) {
  const Rect r{static_cast<int16_t>(MARGIN + 5), y,
               static_cast<int16_t>(SCREEN_W - MARGIN * 2 - 10), h};
  gfx->fillRect(r.x, r.y, r.w, r.h, C::CARD);
  textBox({static_cast<int16_t>(r.x + 3), r.y, labelW, r.h},
          label, C::MUTED, 1, Align::Left, C::CARD);
  textBox({static_cast<int16_t>(r.x + labelW + 7), r.y,
           static_cast<int16_t>(r.w - labelW - 10), r.h},
          value, color, 1, Align::Left, C::CARD);
}

static void drawDiagnostics(bool full) {
  const Rect card{MARGIN, CONTENT_TOP, static_cast<int16_t>(SCREEN_W - MARGIN * 2), CONTENT_H};
  if (full) cardFrame(card, "CONEXION Y SISTEMA");
  const int16_t startY = card.y + 26;
  const int16_t step = (card.h - 30) / 8;
  const int16_t labelW = LANDSCAPE ? 66 : 59;
  const bool wifiOk = WiFi.status() == WL_CONNECTED;
  String endpoint = netConfig.apiBase;
  endpoint.replace("http://", ""); endpoint.replace("https://", "");
  String ssid = netConfig.ssid;
  String panelIp = wifiOk ? WiFi.localIP().toString() : "--";
  String signal = wifiOk ? String(WiFi.RSSI()) + " dBm" : "--";
  String state = wifiStateText();
  String http = lastHttpCode ? String(lastHttpCode) : "--";
  String sample = lastGoodSample ? String((millis() - lastGoodSample) / 1000) + " s" : "--";
  bool wifiGood = wifiOk, signalGood = wifiOk && WiFi.RSSI() > -70;
  String apiText = lastApiError;
  bool httpGood = lastHttpCode == 200, apiGood = online;
#if SPLASH_CAPTURE_ENABLED
  // Documentation captures must never reveal the real network or server.
  ssid = "HOME-WIFI"; panelIp = "192.0.2.42"; signal = "-52 dBm"; endpoint = "192.0.2.10:8787";
  state = "CONECTADO"; http = "200"; sample = "2 s"; apiText = "OK";
  wifiGood = signalGood = httpGood = apiGood = true;
#endif
  drawDiagRow(startY, step, labelW, "WiFi", state, wifiGood ? C::OK : C::WARN);
  drawDiagRow(startY + step, step, labelW, "Red", ssid, C::TEXT);
  drawDiagRow(startY + step * 2, step, labelW, "IP panel", panelIp, C::VALUE);
  drawDiagRow(startY + step * 3, step, labelW, "Senal", signal, signalGood ? C::OK : C::WARN);
  drawDiagRow(startY + step * 4, step, labelW, "Servidor", endpoint, C::TEXT);
  drawDiagRow(startY + step * 5, step, labelW, "HTTP", http, httpGood ? C::OK : C::WARN);
  drawDiagRow(startY + step * 6, step, labelW, "Muestra", sample, apiGood ? C::OK : C::WARN);
  drawDiagRow(startY + step * 7, step, labelW, "Estado", apiText, apiGood ? C::OK : C::WARN);
}

static Rect brightnessButtonRect(bool increase) {
  const int16_t w = LANDSCAPE ? 92 : 90;
  const int16_t h = 46;
  return {static_cast<int16_t>(SCREEN_W / 2 + (increase ? 12 : -w - 12)),
          static_cast<int16_t>(CONTENT_BOTTOM - h - 10), w, h};
}

static Rect brightnessTrackRect() {
  return {static_cast<int16_t>(MARGIN + 23), static_cast<int16_t>(CONTENT_TOP + (LANDSCAPE ? 81 : 110)),
          static_cast<int16_t>(SCREEN_W - MARGIN * 2 - 46), 24};
}

static void drawBrightnessButton(const Rect &r, bool increase, bool enabled) {
  const uint16_t bg = enabled ? C::CARD_ALT : C::CARD_DEEP;
  const uint16_t fg = enabled ? C::VALUE : C::DIM;
  gfx->fillRoundRect(r.x, r.y, r.w, r.h, 4, bg);
  gfx->drawRoundRect(r.x, r.y, r.w, r.h, 4, enabled ? C::BORDER : C::GRID);
  gfx->fillRect(r.x + r.w / 2 - 8, r.y + r.h / 2 - 1, 17, 2, fg);
  if (increase) gfx->fillRect(r.x + r.w / 2 - 1, r.y + r.h / 2 - 8, 2, 17, fg);
}

static void drawBrightness(bool full) {
  const Rect card{MARGIN, CONTENT_TOP, static_cast<int16_t>(SCREEN_W - MARGIN * 2), CONTENT_H};
  if (full) cardFrame(card, "RETROILUMINACION", C::VALUE);
  const int16_t valueY = CONTENT_TOP + (LANDSCAPE ? 28 : 49);
  textBox({static_cast<int16_t>(MARGIN + 14), valueY, static_cast<int16_t>(card.w - 28), 40},
          String(BRIGHTNESS_LABELS[brightnessLevel]) + "%", C::VALUE, 3, Align::Center);
  const Rect track = brightnessTrackRect();
  gfx->fillRect(track.x - 6, track.y, track.w + 12, track.h, C::CARD);
  gfx->drawFastHLine(track.x, track.y + 12, track.w, C::BORDER);
  for (uint8_t i = 0; i < BRIGHTNESS_LEVEL_COUNT; ++i) {
    const int16_t x = track.x + i * (track.w - 1) / (BRIGHTNESS_LEVEL_COUNT - 1);
    gfx->fillCircle(x, track.y + 12, i == brightnessLevel ? 5 : 2, i <= brightnessLevel ? C::VALUE : C::MUTED);
    if (i == brightnessLevel) gfx->fillCircle(x, track.y + 12, 2, C::CARD);
  }
  drawBrightnessButton(brightnessButtonRect(false), false, brightnessLevel > 0);
  drawBrightnessButton(brightnessButtonRect(true), true, brightnessLevel < BRIGHTNESS_LEVEL_COUNT - 1);
}

static Rect themeRowRect(uint8_t index) {
  if (index / THEMES_PER_PAGE != listPagers[PAGE_CUSTOMIZE].page) return {0, 0, 0, 0};
  index %= THEMES_PER_PAGE;
  const int16_t w = (SCREEN_W - MARGIN * 2 - 12) / 2;
  const int16_t h = (CONTENT_H - 39) / 3;
  return {static_cast<int16_t>(MARGIN + 4 + (index % 2) * (w + 4)),
          static_cast<int16_t>(CONTENT_TOP + 27 + (index / 2) * h),
          w, static_cast<int16_t>(h - 4)};
}

static void drawThemeRow(uint8_t index) {
  const Rect r = themeRowRect(index);
  const Theme &theme = THEMES[index];
  const bool selected = index == themeIndex;
  const uint16_t bg = theme.card;
  gfx->fillRoundRect(r.x, r.y, r.w, r.h, 3, bg);
  gfx->drawRoundRect(r.x, r.y, r.w, r.h, 3, selected ? theme.accent : theme.border);
  textBox({static_cast<int16_t>(r.x + 7), static_cast<int16_t>(r.y + 2),
           static_cast<int16_t>(r.w - 26), 14}, theme.name, theme.text, 1, Align::Left, bg);
  if (selected) checkmark(r.x + r.w - 18, r.y + 9, theme.accent);
  const uint16_t swatches[] = {theme.accent, theme.text, theme.ok, theme.warn, theme.bad};
  const int16_t swatchW = (r.w - 22) / 5;
  for (uint8_t i = 0; i < 5; ++i)
    gfx->fillRect(r.x + 7 + i * (swatchW + 2), r.y + r.h - 13, swatchW, 7, swatches[i]);
}

static void drawCustomize(bool full) {
  const Rect card{MARGIN, CONTENT_TOP, static_cast<int16_t>(SCREEN_W - MARGIN * 2), CONTENT_H};
  if (full) cardFrame(card);
  const size_t start = listStart(card, "TEMAS", PAGE_CUSTOMIZE, THEME_COUNT, THEMES_PER_PAGE);
  for (size_t i = start; i < sizeMin(start + THEMES_PER_PAGE, THEME_COUNT); ++i) drawThemeRow(i);
}

static Rect splashRowRect(uint8_t index) {
  if (index / SPLASHES_PER_PAGE != listPagers[PAGE_SPLASH].page) return {0, 0, 0, 0};
  index %= SPLASHES_PER_PAGE;
  const int16_t top = CONTENT_TOP + 25;
  const int16_t available = CONTENT_H - 44;
  const int16_t rowH = available / SPLASHES_PER_PAGE;
  return {static_cast<int16_t>(MARGIN + 7), static_cast<int16_t>(top + index * rowH),
          static_cast<int16_t>(SCREEN_W - MARGIN * 2 - 14), static_cast<int16_t>(rowH - 2)};
}

static void drawSplashRow(uint8_t index) {
  const Rect r = splashRowRect(index);
  const bool selected = index == splashIndex;
  const uint16_t bg = selected ? C::CARD_ALT : C::CARD;
  const uint16_t iconColor = Splash::paletteFor(index).accent;
  gfx->fillRoundRect(r.x, r.y, r.w, r.h, 4, bg);
  if (selected) gfx->drawRoundRect(r.x, r.y, r.w, r.h, 4, C::ACCENT);
  gfx->fillCircle(r.x + 12, r.y + r.h / 2, 5, iconColor);
  gfx->drawCircle(r.x + 12, r.y + r.h / 2, 7, C::BORDER);
  textBox({static_cast<int16_t>(r.x + 25), r.y, static_cast<int16_t>(r.w - 94), r.h},
          SPLASH_NAMES[index], selected ? C::VALUE : C::MUTED, 1, Align::Left, bg);
  textBox({static_cast<int16_t>(r.x + r.w - 60), r.y, 23, r.h},
          selected ? "OK" : "", C::OK, 1, Align::Center, bg);
  gfx->drawFastVLine(r.x + r.w - 33, r.y + 4, r.h - 8, C::BORDER);
  gfx->fillTriangle(r.x + r.w - 23, r.y + 6, r.x + r.w - 23, r.y + r.h - 6,
                    r.x + r.w - 12, r.y + r.h / 2, C::VALUE);
}

static void drawSplashPicker(bool full) {
  const Rect card{MARGIN, CONTENT_TOP, static_cast<int16_t>(SCREEN_W - MARGIN * 2), CONTENT_H};
  if (full) cardFrame(card);
  const size_t start = listStart(card, "ANIMACIONES", PAGE_SPLASH, SPLASH_COUNT, SPLASHES_PER_PAGE);
  for (size_t i = start; i < sizeMin(start + SPLASHES_PER_PAGE, SPLASH_COUNT); ++i) drawSplashRow(i);
}

static Rect rotationButtonRect(bool clockwise) {
  const int16_t w = (SCREEN_W - MARGIN * 2 - 20) / 2;
  return {static_cast<int16_t>(MARGIN + 6 + (clockwise ? w + 8 : 0)),
          static_cast<int16_t>(CONTENT_BOTTOM - 46), w, 36};
}

static Rect orientationChoiceRect(uint8_t index) {
  const int16_t w = (SCREEN_W - MARGIN * 2 - 20) / 2;
  const int16_t top = CONTENT_TOP + 28;
  const int16_t rowH = (rotationButtonRect(false).y - top - 6) / 2;
  return {static_cast<int16_t>(MARGIN + 6 + (index % 2) * (w + 8)),
          static_cast<int16_t>(top + (index / 2) * rowH), w, static_cast<int16_t>(rowH - 5)};
}

static void drawDegrees(int16_t x, int16_t y, uint16_t degrees, uint16_t fg, uint16_t bg, uint8_t size) {
  const String value(degrees);
  rawText(x, y, value, fg, size, bg);
  gfx->drawCircle(x + Typography::width(value, size) + 3, y + 4, 2, fg);
}

static void drawRotationArrow(int16_t cx, int16_t cy, bool clockwise, uint16_t color) {
  const int16_t direction = clockwise ? 1 : -1;
  for (uint8_t i = 1; i <= 18; ++i) {
    const float a = -PI / 2 + (i - 1) * PI / 12;
    const float b = -PI / 2 + i * PI / 12;
    gfx->drawLine(cx + direction * lroundf(cosf(a) * 9), cy + lroundf(sinf(a) * 9),
                  cx + direction * lroundf(cosf(b) * 9), cy + lroundf(sinf(b) * 9), color);
  }
  const int16_t tipX = cx - direction * 9;
  gfx->fillTriangle(tipX, cy - 3, tipX - 4, cy + 3, tipX + 4, cy + 3, color);
}

static void drawOrientation(bool full) {
  if (!full) return;
  const Rect area{MARGIN, CONTENT_TOP, static_cast<int16_t>(SCREEN_W - MARGIN * 2), CONTENT_H};
  cardFrame(area, "PANTALLA");
  textBox({static_cast<int16_t>(area.x + area.w - 96), static_cast<int16_t>(area.y + 3), 88, 17},
          LANDSCAPE ? "HORIZONTAL" : "VERTICAL", C::VALUE, 1, Align::Right);
  for (uint8_t index = 0; index < 4; ++index) {
    const Rect r = orientationChoiceRect(index);
    const bool selected = index == displayRotation;
    const uint16_t bg = selected ? C::ACCENT_DARK : C::CARD_ALT;
    const uint16_t fg = selected ? C::VALUE : C::MUTED;
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 4, bg);
    gfx->drawRoundRect(r.x, r.y, r.w, r.h, 4, selected ? C::VALUE : C::BORDER);
    const int16_t cx = r.x + 21, cy = r.y + r.h / 2;
    const int16_t iw = index & 1 ? 24 : 16, ih = index & 1 ? 16 : 24;
    gfx->drawRoundRect(cx - iw / 2, cy - ih / 2, iw, ih, 2, fg);
    if (index == 0) gfx->fillTriangle(cx, cy - 7, cx - 3, cy - 2, cx + 3, cy - 2, fg);
    else if (index == 1) gfx->fillTriangle(cx + 7, cy, cx + 2, cy - 3, cx + 2, cy + 3, fg);
    else if (index == 2) gfx->fillTriangle(cx, cy + 7, cx - 3, cy + 2, cx + 3, cy + 2, fg);
    else gfx->fillTriangle(cx - 7, cy, cx - 2, cy - 3, cx - 2, cy + 3, fg);
    drawDegrees(r.x + 43, r.y + (r.h - Typography::font(2).height) / 2, index * 90, fg, bg, 2);
    if (selected) checkmark(r.x + r.w - 16, r.y + 8, C::VALUE);
  }
  for (uint8_t i = 0; i < 2; ++i) {
    const Rect r = rotationButtonRect(i != 0);
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 4, C::CARD_ALT);
    drawRotationArrow(r.x + r.w / 2 - 18, r.y + r.h / 2, i != 0, C::VALUE);
    drawDegrees(r.x + r.w / 2 + 2, r.y + (r.h - Typography::font(4).height) / 2,
                90, C::VALUE, C::CARD_ALT, 4);
  }
}

static void renderCurrentPage(bool full) {
  statusDirty = false;
  if (full) {
    gfx->fillScreen(C::BG);
    drawHeaderBase();
    drawFooter();
    lastHeaderIdentity = "";
    lastStatusRendered = "";
    listSignatures[currentPage] = 0;
  }
  drawHeaderIdentity(full);
  drawStatusChip(full);
  if (navigationOpen) { drawNavigation(); return; }
  switch (currentPage) {
    case PAGE_TOTAL: drawTotal(full); break;
    case PAGE_OVERVIEW: drawOverview(full); break;
    case PAGE_STORAGE: drawStorage(full); break;
    case PAGE_NETWORK: drawNetwork(full); break;
    case PAGE_CPU: drawCpu(full); break;
    case PAGE_PORTS: drawPorts(full); break;
    case PAGE_DOCKER: drawDocker(full); break;
    case PAGE_PROXMOX: drawProxmox(full); break;
    case PAGE_PVE_PERFORMANCE: drawPvePerformance(full); break;
    case PAGE_PVE_GUESTS: drawPveGuests(full); break;
    case PAGE_PVE_STORAGE: drawPveStorage(full); break;
    case PAGE_PVE_TASKS: drawPveTasks(full); break;
    case PAGE_DIAG: drawDiagnostics(full); break;
    case PAGE_BRIGHTNESS: drawBrightness(full); break;
    case PAGE_CUSTOMIZE: drawCustomize(full); break;
    case PAGE_SPLASH: drawSplashPicker(full); break;
    case PAGE_ORIENTATION: drawOrientation(full); break;
  }
  if (full) lastPageChange = millis();
}

static void transformTouch(const TS_Point &raw, int16_t &x, int16_t &y) {
  const int16_t nx = constrain(map(raw.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, 239), 0, 239);
  const int16_t ny = constrain(map(raw.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, 319), 0, 319);
  switch (displayRotation) {
    case 0: x = nx;       y = ny;       break;
    case 1: x = ny;       y = 239 - nx; break;
    case 2: x = 239 - nx; y = 319 - ny; break;
    default:x = 319 - ny; y = nx;       break;
  }
}

static bool readTouch(int16_t &x, int16_t &y) {
  if (!touch.touched()) return false;
  TS_Point raw = touch.getPoint();
  transformTouch(raw, x, y);
  x = constrain(x, 0, SCREEN_W - 1);
  y = constrain(y, 0, SCREEN_H - 1);
#if TOUCH_DEBUG
  Serial.printf("raw=%d,%d mapped=%d,%d\n", raw.x, raw.y, x, y);
#endif
  return true;
}

static void changePage(int8_t direction) {
  navigationOpen = false;
  const uint8_t position = visiblePagePosition(currentPage);
  const uint8_t next = direction < 0 ? (position + PAGE_COUNT - 1) % PAGE_COUNT
                                     : (position + 1) % PAGE_COUNT;
  currentPage = VISIBLE_PAGES[next];
  renderCurrentPage(true);
}

static bool contains(const Rect &r, int16_t x, int16_t y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static bool handleOrientationTouch(int16_t x, int16_t y) {
  if (currentPage != PAGE_ORIENTATION) return false;
  uint8_t selected = displayRotation;
  bool hit = false;
  for (uint8_t i = 0; i < 4; ++i) {
    if (contains(orientationChoiceRect(i), x, y)) { selected = i; hit = true; break; }
  }
  if (contains(rotationButtonRect(false), x, y)) { selected = (displayRotation + 3) % 4; hit = true; }
  if (contains(rotationButtonRect(true), x, y)) { selected = (displayRotation + 1) % 4; hit = true; }
  if (hit && selected != displayRotation) {
    applyDisplayRotation(selected);
#if !SPLASH_CAPTURE_ENABLED
    preferences.putUChar("rotation", displayRotation);
#endif
    renderCurrentPage(true);
  }
  return hit;
}

static bool handleBrightnessTouch(int16_t x, int16_t y) {
  if (currentPage != PAGE_BRIGHTNESS) return false;
  const Rect track = brightnessTrackRect();
  if (contains(track, x, y)) {
    brightnessLevel = constrain(((x - track.x) * (BRIGHTNESS_LEVEL_COUNT - 1) + track.w / 2) / track.w,
                                0, BRIGHTNESS_LEVEL_COUNT - 1);
    saveBrightness();
    drawBrightness(false);
    return true;
  }
  if (contains(brightnessButtonRect(false), x, y) && brightnessLevel > 0) {
    --brightnessLevel;
    saveBrightness();
    drawBrightness(false);
    return true;
  }
  if (contains(brightnessButtonRect(true), x, y) && brightnessLevel < BRIGHTNESS_LEVEL_COUNT - 1) {
    ++brightnessLevel;
    saveBrightness();
    drawBrightness(false);
    return true;
  }
  return false;
}

static bool handleCustomizeTouch(int16_t x, int16_t y) {
  if (currentPage != PAGE_CUSTOMIZE) return false;
  for (uint8_t i = 0; i < THEME_COUNT; ++i) {
    if (contains(themeRowRect(i), x, y)) {
      if (i != themeIndex) {
        saveTheme(i);
        renderCurrentPage(true);
      }
      return true;
    }
  }
  return false;
}

static void previewSplash(uint8_t design);

static bool handleSplashTouch(int16_t x, int16_t y) {
  if (currentPage != PAGE_SPLASH) return false;
  for (uint8_t i = 0; i < SPLASH_COUNT; ++i) {
    if (contains(splashRowRect(i), x, y)) {
      if (x >= splashRowRect(i).x + splashRowRect(i).w - 33) {
        previewSplash(i);
      } else if (i != splashIndex) {
        saveSplash(i);
        renderCurrentPage(true);
      }
      return true;
    }
  }
  return false;
}

static bool navigationTouch(int16_t y) {
  return y >= CONTENT_BOTTOM - 8;
}

static void dispatchTouch(int16_t x, int16_t y) {
  // Footer plus its safety margin always wins over list/theme selection.
  if (navigationTouch(y)) {
    if (x < 62) changePage(-1);
    else if (x >= SCREEN_W - 62) changePage(1);
    else {
      navigationOpen = !navigationOpen;
      renderCurrentPage(true);
    }
    return;
  }
  if (navigationOpen) {
    for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
      if (contains(navigationItem(i), x, y)) {
        currentPage = VISIBLE_PAGES[i];
        navigationOpen = false;
        renderCurrentPage(true);
        break;
      }
    }
    return;
  }
  auto &pager = listPagers[currentPage];
  if (pager.pages > 1) {
    const bool previous = contains(listArrowRect(pager.card, false), x, y);
    const bool next = contains(listArrowRect(pager.card, true), x, y);
    if (previous || next) {
      pager.page = previous ? (pager.page + pager.pages - 1) % pager.pages : (pager.page + 1) % pager.pages;
      renderCurrentPage(true);
      return;
    }
  }
  if (handleBrightnessTouch(x, y)) return;
  if (handleOrientationTouch(x, y)) return;
  if (handleCustomizeTouch(x, y)) return;
  handleSplashTouch(x, y);
}

static void handleTouch() {
  static Panel::TouchLatch latch;
  int16_t x = 0, y = 0;
  if (latch.accept(readTouch(x, y), millis())) dispatchTouch(x, y);
}

static void setStatus(bool state, const char *value) {
  const bool changed = online != state || strcmp(statusText, value) != 0;
  online = state;
  statusText = value;
  if (changed) statusDirty = true;
}

struct ApiResponse {
  JsonDocument data;
  String error;
  int httpCode = 0;
  bool ok = false;
};
static ApiResponse pendingResponse;
enum class RequestState : uint8_t { Idle, Running, Done };
static std::atomic<RequestState> requestState{RequestState::Idle};

static bool requestSnapshot(ApiResponse &response) {
  response.ok = false;
  response.httpCode = 0;
  response.data.clear();
  if (WiFi.status() != WL_CONNECTED) {
    response.error = "WiFi sin conexion";
    return false;
  }
  const uint16_t timeout = bootstrapping ? 7000 : HTTP_TIMEOUT_MS;
  const String url = netConfig.apiBase + "/api/v2/snapshot";
  // Declared before HTTPClient so the transport outlives it.
  std::unique_ptr<WiFiClient> transport;
  if (url.startsWith("https://")) {
    auto *tls = new (std::nothrow) WiFiClientSecure();
    if (!tls) {
      response.error = "Memoria insuficiente";
      return false;
    }
    transport.reset(tls);
    tls->setInsecure();
    tls->setHandshakeTimeout((timeout + 999) / 1000);
    if (netConfig.certSha256.length()) {
      // Connect first and compare the certificate before the token is sent;
      // HTTPClient then reuses this already verified connection.
      String host;
      uint16_t port = 443;
      if (!Panel::httpsEndpoint(netConfig.apiBase, host, port)) {
        response.error = "URL API invalida";
        return false;
      }
      if (!tls->connect(host.c_str(), port, timeout)) {
        response.httpCode = HTTPC_ERROR_CONNECTION_REFUSED;
        response.error = "Servidor no accesible";
        return false;
      }
      if (!tls->verify(netConfig.certSha256.c_str(), nullptr)) {
        tls->stop();
        response.error = "Certificado API no coincide";
        return false;
      }
    }
  } else {
    transport.reset(new (std::nothrow) WiFiClient());
    if (!transport) {
      response.error = "Memoria insuficiente";
      return false;
    }
  }
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(timeout);
  http.setTimeout(timeout);
  if (!http.begin(*transport, url)) {
    response.error = "URL API invalida";
    return false;
  }
  http.addHeader("X-API-Key", netConfig.token);
  response.httpCode = http.GET();
  if (response.httpCode != HTTP_CODE_OK) {
    response.error = response.httpCode == HTTP_CODE_UNAUTHORIZED ? "Token incorrecto" :
                     response.httpCode < 0 ? "Servidor no accesible" : "HTTP " + String(response.httpCode);
    http.end();
    return false;
  }
  const DeserializationError error = deserializeJson(response.data, http.getStream(),
                                                     DeserializationOption::Filter(snapshotFilter));
  http.end();
  if (error) {
    response.error = "JSON " + String(error.c_str());
    return false;
  }
  if ((response.data["schema"] | 0) != 2) {
    response.error = "Schema incompatible";
    return false;
  }
  const auto cpu = response.data["cpu"]["percent"].as<JsonVariantConst>();
  const auto memory = response.data["memory"]["percent"].as<JsonVariantConst>();
  if (!Panel::number(cpu) || !Panel::number(memory) ||
      cpu.as<float>() < 0 || cpu.as<float>() > 100 ||
      memory.as<float>() < 0 || memory.as<float>() > 100 ||
      !response.data["system"].is<JsonObject>() ||
      !response.data["network"].is<JsonObject>() ||
      !response.data["storage"]["mounts"].is<JsonArray>() ||
      !response.data["interfaces"].is<JsonArray>() ||
      !response.data["ports"].is<JsonArray>() ||
      !response.data["docker"].is<JsonObject>() ||
      !response.data["proxmox"].is<JsonObject>() ||
      !response.data["history"].is<JsonArray>()) {
    response.error = "Respuesta incompleta";
    return false;
  }
  response.error = "OK";
  response.ok = true;
  return true;
}

static void acceptResponse(ApiResponse &response, bool render) {
  lastHttpCode = response.httpCode;
  lastApiError = response.error;
  if (response.ok) {
    bool partial = false;
    String partialSection;
    for (JsonPair section : response.data["collection_status"].as<JsonObject>()) {
      if (Panel::sectionShown(MONITOR_PROFILE, section.key().c_str()) && section.value()["ok"] == false) {
        partial = true;
        partialSection = section.key().c_str();
        break;
      }
    }
    swap(snapshot, response.data);
    lastGoodSample = millis();
    if (partial) lastApiError = "Datos parciales: " + partialSection;
    setStatus(true, partial ? "PARCIAL" : "ONLINE");
  } else {
    // Keep the last valid sample visible; never replace it with a partial JSON document.
    setStatus(false, WiFi.status() == WL_CONNECTED ? "API" : "WIFI");
  }
  response.data.clear();
#if !SPLASH_CAPTURE_ENABLED
  Serial.printf("API %s; HTTP=%d; heap=%u\n", response.ok ? "OK" : "ERROR",
                response.httpCode, ESP.getFreeHeap());
#endif
  if (render) renderCurrentPage(false);
}

static bool fetchSnapshot(bool render = true) {
  ApiResponse response;
  requestSnapshot(response);
  acceptResponse(response, render);
  return response.ok;
}

static void requestTask(void *) {
  requestSnapshot(pendingResponse);
  requestState.store(RequestState::Done, std::memory_order_release);
  vTaskDelete(nullptr);
}

static void pollApi() {
  if (requestState.load(std::memory_order_acquire) == RequestState::Done) {
    acceptResponse(pendingResponse, true);
    requestState.store(RequestState::Idle);
  }
  if (requestState.load() == RequestState::Idle && WiFi.status() == WL_CONNECTED &&
      millis() - lastRequest >= REQUEST_INTERVAL_MS) {
    lastRequest = millis();
    requestState.store(RequestState::Running);
    if (xTaskCreate(requestTask, "api-refresh", 10240, nullptr, 1, nullptr) != pdPASS) {
      requestState.store(RequestState::Idle);
      lastApiError = "Memoria insuficiente";
      setStatus(false, "API");
    }
  }
}

static void manageWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    reconnectDelay = 5000;
    return;
  }
  setStatus(false, "WIFI");
  lastApiError = "WiFi " + wifiStateText();
  if (static_cast<int32_t>(millis() - nextWifiAttempt) < 0) return;
  Serial.println("Conectando WiFi...");
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(netConfig.deviceName.c_str());
  WiFi.begin(netConfig.ssid.c_str(), netConfig.password.c_str());
  nextWifiAttempt = millis() + reconnectDelay;
  reconnectDelay = u32Min(reconnectDelay * 2, 30000u);
}

enum class BootResult : uint8_t { Running, Ready, Offline };
static std::atomic<BootResult> bootResult{BootResult::Running};
static std::atomic<Splash::Stage> bootStage{Splash::Stage::Wifi};

static void bootstrapTask(void *) {
  const uint32_t started = millis();
  bool ready = false;
  while (millis() - started < BOOTSTRAP_TIMEOUT_MS) {
    manageWifi();
    if (WiFi.status() == WL_CONNECTED) {
      bootStage.store(Splash::Stage::Api);
      if (fetchSnapshot(false)) {
        ready = true;
        break;
      }
      setStatus(false, "API");
      vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
      bootStage.store(Splash::Stage::Wifi);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
  // Publish only after the worker has finished writing the shared snapshot/status.
  bootResult.store(ready ? BootResult::Ready : BootResult::Offline, std::memory_order_release);
  vTaskDelete(nullptr);
}

static Arduino_Canvas *createSplashCanvas() {
  // Full-screen rendering in 40-row strips keeps RAM use bounded without PSRAM.
  auto *canvas = new (std::nothrow) Splash::Canvas(SCREEN_W, 40, nullptr);
  if (canvas && canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) return canvas;
  delete canvas;
  Serial.println("Splash: memoria insuficiente para el buffer");
  return nullptr;
}

static void clearSplash(uint8_t design) {
  gfx->fillScreen(Splash::paletteFor(design).bg);
}

static void renderSplashFrame(Arduino_Canvas *canvas, uint8_t design, uint32_t elapsed, Splash::Stage stage) {
  const auto palette = Splash::paletteFor(design);
  if (canvas) {
    for (int16_t row = 0; row < SCREEN_H; row += 40) {
      Splash::Scene scene(*canvas, palette, SCREEN_W, SCREEN_H, -row);
      scene.draw(design, elapsed, stage);
      gfx->draw16bitRGBBitmap(0, row, canvas->getFramebuffer(), SCREEN_W, std::min(40,SCREEN_H-row));
    }
  } else {
    Splash::Scene scene(*gfx, palette, SCREEN_W, SCREEN_H);
    scene.draw(design, elapsed, stage);
  }
}

static void drawSplash() {
  bootResult.store(BootResult::Running);
  bootStage.store(Splash::Stage::Wifi);
  auto *canvas = createSplashCanvas();
  clearSplash(splashIndex);
  renderSplashFrame(canvas, splashIndex, 0, Splash::Stage::Wifi);
  const uint32_t started = millis();
  // The network worker never draws. Only this task owns the display and its buffer.
  auto created = xTaskCreate(bootstrapTask, "boot-api", 10240, nullptr, 1, nullptr);
  if (created != pdPASS) {
    delete canvas;
    canvas = nullptr;
    created = xTaskCreate(bootstrapTask, "boot-api", 10240, nullptr, 1, nullptr);
  }
  if (created != pdPASS) {
    lastApiError = "Memoria insuficiente";
    bootResult.store(BootResult::Offline);
  }
  uint32_t frames = 0, readyAt = 0;
  while (true) {
    const uint32_t frameStarted = millis();
    const uint32_t elapsed = frameStarted - started;
    const BootResult result = bootResult.load(std::memory_order_acquire);
    Splash::Stage stage = bootStage.load();
    if (result != BootResult::Running) {
      stage = result == BootResult::Ready ? Splash::Stage::Ready : Splash::Stage::Offline;
      if (!readyAt) readyAt = frameStarted;
    }
    renderSplashFrame(canvas, splashIndex, elapsed, stage);
    ++frames;
    if (result != BootResult::Running && elapsed >= SPLASH_TIME_MS && frameStarted - readyAt >= 350) break;
    const uint32_t frameTime = millis() - frameStarted;
    if (frameTime < 50) delay(50 - frameTime);
    else delay(1);
  }
  delete canvas;
  Serial.printf("Bootstrap %s en %lu ms; frames=%lu; heap=%u\n",
                online ? "OK" : "TIMEOUT", static_cast<unsigned long>(millis() - started),
                static_cast<unsigned long>(frames), ESP.getFreeHeap());
}

static void previewSplash(uint8_t design) {
  auto *canvas = createSplashCanvas();
  clearSplash(design);
  const uint32_t started = millis();
  while (millis() - started < 4500) {
    renderSplashFrame(canvas, design, millis() - started, Splash::Stage::Preview);
    delay(40);
  }
  delete canvas;
  renderCurrentPage(true);
}

static void drawShutdownSplash() {
  if (!gfx) return;
  auto *canvas = createSplashCanvas();
  clearSplash(splashIndex);
  const uint32_t started = millis();
  while (millis() - started < SHUTDOWN_SPLASH_TIME_MS) {
    renderSplashFrame(canvas, splashIndex, millis() - started, Splash::Stage::Closing);
    delay(40);
  }
  delete canvas;
}

static void drawDisplayTest() {
  const uint16_t colors[] = {C::BAD, C::OK, C::ACCENT, C::WARN};
  const char *labels[] = {"R", "G", "B", "Y"};
  const int16_t band = SCREEN_W / 4;
  gfx->fillScreen(C::BG);
  for (uint8_t i = 0; i < 4; ++i) {
    gfx->fillRect(i * band, 0, i == 3 ? SCREEN_W - i * band : band, SCREEN_H / 2, colors[i]);
    textBox({static_cast<int16_t>(i * band), static_cast<int16_t>(SCREEN_H / 4 - 8),
             static_cast<int16_t>(i == 3 ? SCREEN_W - i * band : band), 16},
            labels[i], C::BG, 2, Align::Center, colors[i]);
  }
  textBox({8, static_cast<int16_t>(SCREEN_H / 2 + 14), static_cast<int16_t>(SCREEN_W - 16), 22},
          "CYD DISPLAY OK", C::TEXT, 2, Align::Center, C::BG);
  textBox({8, static_cast<int16_t>(SCREEN_H / 2 + 42), static_cast<int16_t>(SCREEN_W - 16), 14},
          String(SCREEN_W) + "x" + SCREEN_H + " ROT=" + displayRotation,
          C::ACCENT, 1, Align::Center, C::BG);
  textBox({8, static_cast<int16_t>(SCREEN_H / 2 + 62), static_cast<int16_t>(SCREEN_W - 16), 14},
          "TOCA LA PANTALLA", C::MUTED, 1, Align::Center, C::BG);
}

#if SPLASH_CAPTURE_ENABLED
static JsonDocument previousCaptureSnapshot;
class PanelCaptureSurface : public Arduino_GFX {
 public:
  PanelCaptureSurface(Splash::Canvas &canvas, int16_t top)
      : Arduino_GFX(SCREEN_W, SCREEN_H), target(canvas), offset(top) {}
  bool begin(int32_t = GFX_NOT_DEFINED) override { return true; }
  void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override {
    target.writePixel(x, y - offset, color);
  }
  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    target.drawFastHLine(x, y - offset, w, color);
  }
  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    target.drawFastVLine(x, y - offset, h, color);
  }
  void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    target.fillRect(x, y - offset, w, h, color);
  }
 private:
  Splash::Canvas &target;
  int16_t offset;
};

static void sendCapturePixels(const uint16_t *pixels, size_t count) {
  for (size_t i = 0; i < count;) {
    uint16_t length = 1;
    while (i + length < count && length < 65535 && pixels[i + length] == pixels[i]) ++length;
    const uint16_t run[] = {length, pixels[i]};
    Serial.write(reinterpret_cast<const uint8_t *>(run), sizeof(run));
    i += length;
  }
}

static bool checkTypographyLogic() {
  bool ok = Typography::font(Typography::Compact).height <= 12;
  ok &= Typography::fitSize("100%", 59, 25) == Typography::Value;
  ok &= Typography::fitSize("ERROR", 59, 25) == Typography::SmallValue;
  ok &= Typography::fitSize("100.0 C", 59, 25) == Typography::SmallValue;
  ok &= Typography::fitSize("CPU0", 34, 12, Typography::Body) == Typography::Compact;
  const char *values[] = {"0%", "100%", "100.0 C", "ERROR", "2400", "12.3 MB/s", "999 GB/s"};
  for (const char *value : values) {
    const uint8_t size = Typography::fitSize(value, 85, 23);
    ok &= Typography::font(size).height >= Typography::font(Typography::Medium).height;
    ok &= fitText(value, 85, size) == value;
  }
  for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
    const uint8_t page = VISIBLE_PAGES[i];
    const Rect r = navigationItem(i);
    const uint8_t size = Typography::fitSize(PAGE_NAMES[page], r.w - 13, r.h, Typography::Body);
    ok &= fitText(PAGE_NAMES[page], r.w - 13, size) == PAGE_NAMES[page];
    const uint8_t heading = Typography::fitSize(PAGE_NAMES[page], SCREEN_W - 87, 22, Typography::Heading);
    ok &= fitText(PAGE_NAMES[page], SCREEN_W - 87, heading) == PAGE_NAMES[page];
  }
  return ok;
}

static float themeLuminance(uint16_t color) {
  const auto channel = [](float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
  return 0.2126f * channel((color >> 11) / 31.0f) +
         0.7152f * channel(((color >> 5) & 63) / 63.0f) + 0.0722f * channel((color & 31) / 31.0f);
}

static bool checkThemeContrast() {
  for (const auto &theme : THEMES) {
    const uint16_t colors[] = {theme.text, Typography::blend(theme.muted, theme.text, 2),
                              Typography::blend(theme.accent, theme.text, 2), theme.ok, theme.warn, theme.bad};
    for (uint16_t bg : {theme.card, theme.cardAlt, theme.top}) {
      for (uint16_t fg : colors) {
        const float a = themeLuminance(fg) + 0.05f, b = themeLuminance(bg) + 0.05f;
        if (std::max(a, b) / std::min(a, b) < 4.5f) {
          Serial.printf("ERROR contrast %s fg=%u bg=%u ratio=%.2f\n", theme.name, fg, bg, std::max(a,b)/std::min(a,b));
          return false;
        }
      }
    }
  }
  return true;
}

static bool checkPickerLogic() {
  const uint8_t originalPage = currentPage, originalTheme = themeIndex, originalSplash = splashIndex;
  const uint8_t savedTheme = preferences.getUChar("theme", 0), savedSplash = preferences.getUChar("splash", 0);
  const ListPager themePager = listPagers[PAGE_CUSTOMIZE], splashPager = listPagers[PAGE_SPLASH];
  bool ok = true;
  for (uint8_t page : {PAGE_CUSTOMIZE, PAGE_SPLASH}) {
    const uint8_t count = page == PAGE_CUSTOMIZE ? THEME_COUNT : SPLASH_COUNT;
    const uint8_t perPage = page == PAGE_CUSTOMIZE ? THEMES_PER_PAGE : SPLASHES_PER_PAGE;
    currentPage = page;
    auto &pager = listPagers[page];
    for (size_t group = 0; group < (count + perPage - 1) / perPage; ++group) {
      pager.page = group;
      renderCurrentPage(true);
      for (uint8_t i = 0; i < count; ++i) {
        const Rect r = page == PAGE_CUSTOMIZE ? themeRowRect(i) : splashRowRect(i);
        if (i / perPage != group) { ok &= r.w == 0 && r.h == 0; continue; }
        ok &= r.w > 80 && r.h >= 26 && r.y >= CONTENT_TOP + 24;
        ok &= r.y + r.h < CONTENT_BOTTOM - 8 && r.x + r.w <= SCREEN_W - MARGIN;
      }
      const Rect arrow = listArrowRect(pager.card, true);
      const uint8_t beforeTheme = themeIndex, beforeSplash = splashIndex;
      dispatchTouch(arrow.x + arrow.w / 2, arrow.y + arrow.h / 2);
      ok &= pager.page == (group + 1) % pager.pages;
      ok &= themeIndex == beforeTheme && splashIndex == beforeSplash;
    }
    pager.page = (count - 1) / perPage;
    renderCurrentPage(true);
    const Rect last = page == PAGE_CUSTOMIZE ? themeRowRect(count - 1) : splashRowRect(count - 1);
    dispatchTouch(last.x + 28, last.y + last.h / 2);
    ok &= page == PAGE_CUSTOMIZE ? themeIndex == count - 1 : splashIndex == count - 1;
    const uint8_t selectedTheme = themeIndex, selectedSplash = splashIndex;
    // Empty slots on the final selector page must not select an invisible item.
    const Rect empty = page == PAGE_CUSTOMIZE ? themeRowRect(count) : splashRowRect(count);
    dispatchTouch(empty.x + 28, empty.y + empty.h / 2);
    ok &= themeIndex == selectedTheme && splashIndex == selectedSplash && currentPage == page;
    dispatchTouch(SCREEN_W - 25, SCREEN_H - 10);
    ok &= currentPage == page + 1 && themeIndex == selectedTheme && splashIndex == selectedSplash;
  }
  ok &= preferences.getUChar("theme", 0) == savedTheme && preferences.getUChar("splash", 0) == savedSplash;
  currentPage = originalPage;
  applyTheme(originalTheme);
  splashIndex = originalSplash;
  listPagers[PAGE_CUSTOMIZE] = themePager;
  listPagers[PAGE_SPLASH] = splashPager;
  return ok;
}

static bool checkOrientationLogic() {
  const uint8_t originalRotation = displayRotation, originalPage = currentPage;
  const uint8_t savedRotation = preferences.getUChar("rotation", DISPLAY_ROTATION);
  const bool originalNavigation = navigationOpen;
  const int16_t expected[4][4][2] = {
    {{0, 0}, {239, 0}, {0, 319}, {239, 319}},
    {{0, 239}, {0, 0}, {319, 239}, {319, 0}},
    {{239, 319}, {0, 319}, {239, 0}, {0, 0}},
    {{319, 0}, {319, 239}, {0, 0}, {0, 239}},
  };
  bool ok = true;
  navigationOpen = false;
  for (uint8_t rotation = 0; rotation < 4; ++rotation) {
    applyDisplayRotation(rotation);
    ok &= checkTypographyLogic();
    if (rotation < 2) ok &= checkPickerLogic();
    ok &= gfx->width() == SCREEN_W && gfx->height() == SCREEN_H && gfx->getRotation() == rotation;
    for (uint8_t corner = 0; corner < 4; ++corner) {
      const TS_Point raw(corner & 1 ? TOUCH_MAX_X : TOUCH_MIN_X,
                         corner & 2 ? TOUCH_MAX_Y : TOUCH_MIN_Y, 1000);
      int16_t x, y;
      transformTouch(raw, x, y);
      ok &= x == expected[rotation][corner][0] && y == expected[rotation][corner][1];
    }
    for (uint8_t i = 0; i < 4; ++i) {
      const Rect r = orientationChoiceRect(i);
      ok &= r.x >= MARGIN && r.x + r.w <= SCREEN_W - MARGIN;
      ok &= r.h >= 30 && r.y >= CONTENT_TOP && r.y + r.h < rotationButtonRect(false).y;
    }
    for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
      const Rect r = navigationItem(i);
      ok &= r.y + r.h < CONTENT_BOTTOM - 8;
    }
    currentPage = PAGE_ORIENTATION;
    Rect button = rotationButtonRect(true);
    ok &= button.y + button.h < CONTENT_BOTTOM - 8;
    dispatchTouch(button.x + button.w / 2, button.y + button.h / 2);
    ok &= displayRotation == (rotation + 1) % 4;
    button = rotationButtonRect(false);
    dispatchTouch(button.x + button.w / 2, button.y + button.h / 2);
    ok &= displayRotation == rotation;
    for (uint8_t target = 0; target < 4; ++target) {
      applyDisplayRotation(rotation);
      const Rect r = orientationChoiceRect(target);
      dispatchTouch(r.x + r.w / 2, r.y + r.h / 2);
      ok &= displayRotation == target;
      ok &= gfx->width() == SCREEN_W && gfx->height() == SCREEN_H;
    }
    applyDisplayRotation(rotation);
    const uint8_t previousTheme = themeIndex, previousSplash = splashIndex;
    dispatchTouch(30, SCREEN_H - 10);
    ok &= currentPage == PAGE_SPLASH && displayRotation == rotation;
    ok &= themeIndex == previousTheme && splashIndex == previousSplash;
  }
  ok &= preferences.getUChar("rotation", DISPLAY_ROTATION) == savedRotation;
  currentPage = originalPage;
  navigationOpen = originalNavigation;
  applyDisplayRotation(originalRotation);
  return ok;
}

static void checkPanelLogic() {
  Panel::TouchLatch latch;
  bool ok = latch.accept(true, 100);
  ok &= !latch.accept(true, 500);
  ok &= !latch.accept(false, 510);
  ok &= !latch.accept(true, 530);
  latch.accept(false, 540);
  latch.accept(false, 620);
  ok &= latch.accept(true, 630);
  for (uint8_t i = 0; i < THEME_COUNT; ++i) {
    const auto r = themeRowRect(i);
    ok &= r.y + r.h < CONTENT_BOTTOM - 8;
  }
  for (uint8_t i = 0; i < SPLASH_COUNT; ++i) {
    const auto r = splashRowRect(i);
    ok &= r.y + r.h < CONTENT_BOTTOM - 8;
  }
  ok &= navigationTouch(footerButtonRect(false).y);
  ok &= navigationTouch(footerButtonRect(true).y);
  ok &= BRIGHTNESS_LEVEL_COUNT == 7 && BRIGHTNESS_LEVELS[0] == 1;
  for (uint8_t i = 0; i < BRIGHTNESS_LEVEL_COUNT; ++i)
    ok &= nearestBrightnessLevel(BRIGHTNESS_LEVELS[i]) == i;
  JsonDocument fixture;
  deserializeJson(fixture, R"([{"port":22,"protocol":"tcp","address":"0.0.0.0","service":"sshd"},
    {"port":22,"protocol":"tcp6","address":"::","service":"sshd"},
    {"port":22,"protocol":"udp","address":"0.0.0.0","service":"sshd"},
    {"port":22,"protocol":"tcp","address":"127.0.0.1","service":"sshd"},
    {"port":22,"protocol":"tcp","address":"0.0.0.0","service":"other"}])");
  const auto ports = Panel::ports(fixture.as<JsonArray>());
  ok &= ports.size() == 4 && ports[0].v4 && ports[0].v6;
  ok &= Panel::sensorName("cpu_thermal:sensor 1", 0) == "CPU";
  ok &= Panel::sensorName("rp1_adc:sensor 1", 1) == "Chip RP1";
  ok &= !Panel::number(fixture["missing"]);
  deserializeJson(fixture, "[{\"cpu\":98,\"ram\":32},{\"cpu\":100,\"ram\":33}]");
  const auto cpuScale = Panel::historyScale(fixture.as<JsonArray>(), "cpu");
  const auto ramScale = Panel::historyScale(fixture.as<JsonArray>(), "ram");
  ok &= cpuScale.high == 100 && cpuScale.low <= 98 && cpuScale.low >= 94;
  ok &= ramScale.high >= 33 && ramScale.high < 40 && ramScale.low <= 32;
  snapshot["cpu"]["percent"] = 52;
  ApiResponse failed;
  failed.error = "Test offline";
  acceptResponse(failed, false);
  ok &= snapshot["cpu"]["percent"].as<int>() == 52 && !online;
  ApiResponse valid;
  valid.ok = true;
  valid.data["cpu"]["percent"] = 17;
  acceptResponse(valid, false);
  ok &= snapshot["cpu"]["percent"].as<int>() == 17 && online;
  snapshot.clear();
  for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
    const auto r = navigationItem(i);
    ok &= r.y >= CONTENT_TOP && r.y + r.h < CONTENT_BOTTOM - 8;
  }
  ok &= checkOrientationLogic();
  ok &= checkThemeContrast();
  Serial.println(ok ? "TEST OK touch/brightness/ports/sensors/scales/snapshots/navigation/rotation/typography/pickers/contrast" : "ERROR panel logic");
}

static void captureSplashFrame() {
  if (!Serial.available()) return;
  const String command = Serial.readStringUntil('\n');
  if (command == "I") {
    String ids;
    for (uint8_t i = 0; i < PAGE_COUNT; ++i) ids += (i ? "," : "") + String(VISIBLE_PAGES[i]);
    Serial.printf("{\"pages\":%u,\"page_ids\":[%s],\"themes\":%u,\"splashes\":%u,\"themes_per_page\":%u,\"splashes_per_page\":%u}\n",
                  PAGE_COUNT, ids.c_str(), THEME_COUNT, SPLASH_COUNT, THEMES_PER_PAGE, SPLASHES_PER_PAGE);
    return;
  }
  if (command == "T") { checkPanelLogic(); return; }
  if (command.startsWith("J ")) {
    const auto error = deserializeJson(snapshot, command.substring(2));
    Serial.println(error ? "ERROR fixture JSON" : "JSON OK");
    return;
  }
  if (command.startsWith("U ")) {
    const auto error = deserializeJson(previousCaptureSnapshot, command.substring(2));
    Serial.println(error ? "ERROR fixture JSON" : "JSON OK");
    return;
  }
  unsigned page, subpage, brightness, panelWidth = SCREEN_W, panelTheme = themeIndex, mode = 1, panelRotation = 255;
  if (sscanf(command.c_str(), "P %u %u %u %u %u %u %u", &page, &subpage, &brightness,
             &panelWidth, &panelTheme, &mode, &panelRotation) >= 3) {
    if (panelRotation == 255) panelRotation = panelWidth == 320 ? 1 : 0;
    if (page >= PAGE_COUNT || brightness >= BRIGHTNESS_LEVEL_COUNT ||
        (panelWidth != 240 && panelWidth != 320) || panelTheme >= THEME_COUNT || mode > 3 ||
        panelRotation > 3 || panelWidth != (panelRotation & 1 ? 320 : 240)) {
      Serial.println("ERROR page request");
      return;
    }
    applyDisplayRotation(panelRotation);
    applyTheme(panelTheme);
    Splash::Canvas frame(SCREEN_W, 40, nullptr);
    if (!frame.begin()) { Serial.println("ERROR capture allocation"); return; }
    Arduino_GFX *display = gfx;
    currentPage = VISIBLE_PAGES[page];
    listPagers[currentPage].page = subpage;
    brightnessLevel = brightness;
    online = mode != 0;
    statusText = online ? "ONLINE" : "API";
    lastApiError = online ? "OK" : "Servidor no accesible";
    lastGoodSample = millis();
    lastHttpCode = online ? 200 : -1;
    navigationOpen = mode == 2;
    JsonDocument currentCaptureSnapshot;
    if (mode == 3) currentCaptureSnapshot = snapshot;
    Serial.printf("FRAME %u %u\n", SCREEN_W, SCREEN_H);
    for (int16_t row = 0; row < SCREEN_H; row += 40) {
      frame.fillScreen(C::BG);
      PanelCaptureSurface surface(frame, row);
      gfx = &surface;
      if (mode == 3) snapshot = previousCaptureSnapshot;
      renderCurrentPage(true);
      if (mode == 3) {
        snapshot = currentCaptureSnapshot;
        renderCurrentPage(false);
      }
      gfx = display;
      display->draw16bitRGBBitmap(0, row, frame.getFramebuffer(), SCREEN_W, 40);
      sendCapturePixels(frame.getFramebuffer(), SCREEN_W * 40);
    }
    Serial.println("\nEND");
    return;
  }
  unsigned design, elapsed, width, theme, captureRotation=255, stripHeight=40, captureStage=1;
  if (sscanf(command.c_str(), "F %u %u %u %u %u %u %u", &design, &elapsed, &width, &theme,
             &captureRotation, &stripHeight, &captureStage) < 4 ||
      design >= SPLASH_COUNT || (width != 240 && width != 320) || theme >= THEME_COUNT ||
      (stripHeight!=40 && stripHeight!=80) || captureStage>5) {
    Serial.println("ERROR invalid capture request");
    return;
  }
  const uint8_t previousTheme = themeIndex;
  applyTheme(theme);
  const int16_t height = width == 320 ? 240 : 320;
  if (captureRotation==255) captureRotation=width==320?1:0;
  if (captureRotation>3 || width!=(captureRotation&1?320:240)) { Serial.println("ERROR rotation"); return; }
  applyDisplayRotation(captureRotation);
  Splash::Canvas frame(width, stripHeight, nullptr);
  if (!frame.begin()) {
    applyTheme(previousTheme);
    Serial.println("ERROR capture allocation");
    return;
  }
  const auto palette = Splash::paletteFor(design);
  Serial.printf("FRAME %u %u\n", width, height);
  for (int16_t row = 0; row < height; row += stripHeight) {
    Splash::Scene scene(frame, palette, width, height, -row);
    scene.draw(design, elapsed, static_cast<Splash::Stage>(captureStage));
    gfx->draw16bitRGBBitmap(0, row, frame.getFramebuffer(), width, stripHeight);
    sendCapturePixels(frame.getFramebuffer(), width * stripHeight);
  }
  Serial.println("\nEND");
  applyTheme(previousTheme);
}
#endif

static void displayTestLoop() {
  int16_t x, y;
  if (readTouch(x, y)) {
    gfx->fillCircle(x, y, 3, C::WHITE_);
    textBox({8, static_cast<int16_t>(SCREEN_H - 24), static_cast<int16_t>(SCREEN_W - 16), 16},
            "TOUCH " + String(x) + "," + String(y), C::TEXT, 1, Align::Center, C::BG);
  }
  delay(10);
}

void setup() {
  Serial.begin(SPLASH_CAPTURE_ENABLED ? 460800 : 115200);
  netConfig = RuntimeConfig::load();
  Panel::buildSnapshotFilter(snapshotFilter);
  Serial.printf("Configuracion de red: %s\n", netConfig.fromFlash ? "instalador web" : "compilada");
  if (netConfig.apiBase.startsWith("https://") && !netConfig.certSha256.length())
    Serial.println("Aviso: API https sin huella SHA-256; el certificado no se comprueba");

  pinMode(TFT_BL, OUTPUT);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  loadTheme();
  setDisplayGeometry(preferences.getUChar("rotation", DISPLAY_ROTATION));
  Serial.printf("CYD Monitor %s perfil=%d %dx%d rotation=%d invert=%d test=%d\n", FIRMWARE_VERSION,
                MONITOR_PROFILE, SCREEN_W, SCREEN_H, displayRotation, DISPLAY_INVERT_COLORS, DISPLAY_TEST_ONLY);
  loadBrightness();
  Serial.printf("Ajustes: brillo PWM=%u/255; pantalla=%s\n",
                BRIGHTNESS_LEVELS[brightnessLevel], SPLASH_NAMES[splashIndex]);

  if (!gfx->begin()) Serial.println("ERROR iniciando ILI9341");
  gfx->setRotation(displayRotation);
  if (esp_register_shutdown_handler(drawShutdownSplash) != ESP_OK) {
    Serial.println("Aviso: no se pudo registrar splash de apagado");
  }

  touchSpi.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  if (!touch.begin(touchSpi)) Serial.println("ERROR iniciando XPT2046");
  touch.setRotation(0);

#if SPLASH_CAPTURE_ENABLED
  Serial.println("CAPTURE READY");
  return;
#endif

  if (DISPLAY_TEST_ONLY) {
    drawDisplayTest();
    return;
  }

  snapshot["system"]["hostname"] = "Raspberry Pi";
  snapshot["system"]["ip"] = "sin datos";
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  manageWifi();
  drawSplash();
  bootstrapping = false;
  lastRequest = millis();
  renderCurrentPage(true);
}

void loop() {
#if SPLASH_CAPTURE_ENABLED
  captureSplashFrame();
  delay(8);
  return;
#endif
  if (DISPLAY_TEST_ONLY) {
    displayTestLoop();
    return;
  }

  handleTouch();
  manageWifi();

  pollApi();

  if (lastGoodSample && millis() - lastGoodSample > STALE_DATA_MS && online) {
    setStatus(false, "STALE");
  }
  if (statusDirty) renderCurrentPage(false);

#if AUTO_ROTATE_PAGE_MS > 0
  if (millis() - lastPageChange >= AUTO_ROTATE_PAGE_MS) {
    changePage(1);
  }
#endif
  delay(8);
}
