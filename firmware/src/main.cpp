#include <Arduino.h>
#include <cstring>
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>
#include "config.h"

// Valores seguros para actualizar solo main.cpp sin tocar las credenciales existentes.
#ifndef PANEL_TITLE
#define PANEL_TITLE "RPI5"
#endif
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

static constexpr bool LANDSCAPE = DISPLAY_ROTATION == 1 || DISPLAY_ROTATION == 3;
static constexpr int16_t SCREEN_W = LANDSCAPE ? 320 : 240;
static constexpr int16_t SCREEN_H = LANDSCAPE ? 240 : 320;
static constexpr int16_t HEADER_H = LANDSCAPE ? 37 : 40;
static constexpr int16_t FOOTER_H = LANDSCAPE ? 26 : 28;
static constexpr int16_t MARGIN = 6;
static constexpr int16_t GAP = 5;
static constexpr int16_t CONTENT_TOP = HEADER_H + 5;
static constexpr int16_t CONTENT_BOTTOM = SCREEN_H - FOOTER_H - 5;
static constexpr int16_t CONTENT_H = CONTENT_BOTTOM - CONTENT_TOP;

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_GFX *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED, DISPLAY_ROTATION, DISPLAY_INVERT_COLORS != 0);
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
JsonDocument snapshot;

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8u) << 8u) | ((g & 0xFCu) << 3u) | (b >> 3u));
}

namespace C {
constexpr uint16_t BG = rgb565(0, 0, 0);
constexpr uint16_t TOP = rgb565(5, 8, 12);
constexpr uint16_t CARD = rgb565(14, 19, 26);
constexpr uint16_t CARD_ALT = rgb565(18, 25, 34);
constexpr uint16_t CARD_DEEP = rgb565(10, 14, 20);
constexpr uint16_t BORDER = rgb565(38, 49, 64);
constexpr uint16_t GRID = rgb565(29, 38, 50);
constexpr uint16_t TEXT = rgb565(232, 238, 246);
constexpr uint16_t MUTED = rgb565(126, 141, 160);
constexpr uint16_t DIM = rgb565(77, 91, 109);
constexpr uint16_t ACCENT = rgb565(62, 196, 255);
constexpr uint16_t ACCENT_DARK = rgb565(8, 55, 76);
constexpr uint16_t OK = rgb565(47, 211, 131);
constexpr uint16_t OK_DARK = rgb565(7, 58, 39);
constexpr uint16_t WARN = rgb565(255, 184, 77);
constexpr uint16_t WARN_DARK = rgb565(73, 45, 5);
constexpr uint16_t BAD = rgb565(255, 91, 112);
constexpr uint16_t BAD_DARK = rgb565(75, 13, 25);
constexpr uint16_t WHITE_ = 0xFFFF;
}

static const char *PAGE_NAMES[] = {PANEL_TITLE, "DISCOS", "RED", "CPU / TEMP", "PUERTOS", "DOCKER"};
static constexpr uint8_t PAGE_COUNT = sizeof(PAGE_NAMES) / sizeof(PAGE_NAMES[0]);

static uint8_t currentPage = 0;
static uint32_t lastRequest = 0;
static uint32_t nextWifiAttempt = 0;
static uint32_t reconnectDelay = 1000;
static uint32_t lastGoodSample = 0;
static uint32_t lastPageChange = 0;
static bool online = false;
static const char *statusText = "INICIO";
static String lastHeaderIdentity;
static String lastStatusRendered;
static uint32_t listSignatures[PAGE_COUNT] = {0};

struct Rect {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

enum class Align : uint8_t { Left, Center, Right };

static int16_t i16Max(int16_t a, int16_t b) { return a > b ? a : b; }
static uint8_t u8Max(uint8_t a, uint8_t b) { return a > b ? a : b; }
static uint8_t u8Min(uint8_t a, uint8_t b) { return a < b ? a : b; }
static uint32_t u32Min(uint32_t a, uint32_t b) { return a < b ? a : b; }
static size_t sizeMin(size_t a, size_t b) { return a < b ? a : b; }
static size_t sizeMax(size_t a, size_t b) { return a > b ? a : b; }

static String fitText(String value, int16_t pixelWidth, uint8_t size = 1) {
  if (pixelWidth <= 0) return "";
  const int16_t charWidth = 6 * size;
  const size_t maxChars = static_cast<size_t>(pixelWidth / charWidth);
  if (value.length() <= maxChars) return value;
  if (maxChars == 0) return "";
  if (maxChars <= 3) return value.substring(0, maxChars);
  return value.substring(0, maxChars - 3) + "...";
}

static void rawText(int16_t x, int16_t y, const String &value, uint16_t color,
                    uint8_t size, uint16_t background) {
  gfx->setTextWrap(false);
  gfx->setTextSize(size);
  gfx->setTextColor(color, background);
  gfx->setCursor(x, y);
  gfx->print(value);
}

static void textBox(const Rect &r, const String &value, uint16_t color = C::TEXT,
                    uint8_t size = 1, Align align = Align::Left,
                    uint16_t background = C::CARD, int16_t pad = 0) {
  if (r.w <= 0 || r.h <= 0) return;
  gfx->fillRect(r.x, r.y, r.w, r.h, background);
  const int16_t usable = i16Max(0, static_cast<int16_t>(r.w - pad * 2));
  const String fitted = fitText(value, usable, size);
  const int16_t textW = static_cast<int16_t>(fitted.length() * 6 * size);
  int16_t x = r.x + pad;
  if (align == Align::Center) x = r.x + (r.w - textW) / 2;
  else if (align == Align::Right) x = r.x + r.w - pad - textW;
  const int16_t textH = 8 * size;
  const int16_t y = r.y + i16Max(0, static_cast<int16_t>((r.h - textH) / 2));
  rawText(x, y, fitted, color, size, background);
}

static void valueBox(const Rect &r, const String &value, uint16_t color = C::TEXT,
                     Align align = Align::Left, uint16_t background = C::CARD) {
  uint8_t size = 2;
  if (r.h < 16 || static_cast<int16_t>(value.length() * 12) > r.w - 2) size = 1;
  textBox(r, value, color, size, align, background, 1);
}

static void cardFrame(const Rect &r, const String &title = "", uint16_t accent = C::ACCENT) {
  gfx->fillRoundRect(r.x, r.y, r.w, r.h, 7, C::CARD);
  gfx->drawRoundRect(r.x, r.y, r.w, r.h, 7, C::BORDER);
  if (title.length()) {
    gfx->fillRoundRect(r.x + 7, r.y + 7, 3, 12, 1, accent);
    textBox({static_cast<int16_t>(r.x + 14), static_cast<int16_t>(r.y + 5),
             static_cast<int16_t>(r.w - 21), 16},
            title, C::MUTED, 1, Align::Left, C::CARD);
  }
}

static void divider(int16_t x, int16_t y, int16_t w) {
  gfx->drawFastHLine(x, y, w, C::GRID);
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
  gfx->fillRoundRect(r.x, r.y, r.w, r.h, i16Max(1, static_cast<int16_t>(r.h / 2)), background);
  const int16_t fill = static_cast<int16_t>((r.w * percent) / 100.0f);
  if (fill > 0) {
    const int16_t radius = i16Max(1, static_cast<int16_t>(r.h / 2));
    if (fill >= radius * 2) gfx->fillRoundRect(r.x, r.y, fill, r.h, radius, color);
    else gfx->fillRect(r.x, r.y, fill, r.h, color);
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

static uint32_t fnv1aAdd(uint32_t hash, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) {
    hash ^= static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
    hash *= 16777619u;
  }
  return hash;
}

static uint32_t storageSignature() {
  uint32_t hash = 2166136261u;
  for (JsonObject item : snapshot["storage"]["mounts"].as<JsonArray>()) {
    hash = fnv1aAdd(hash, item["mount"] | "");
    hash = fnv1aAdd(hash, static_cast<uint32_t>((item["used_bytes"] | 0ULL) >> 20));
    hash = fnv1aAdd(hash, static_cast<uint32_t>((item["percent"] | 0.0f) * 10));
  }
  return hash;
}

static uint32_t interfaceSignature() {
  uint32_t hash = 2166136261u;
  for (JsonObject item : snapshot["interfaces"].as<JsonArray>()) {
    hash = fnv1aAdd(hash, item["name"] | "");
    hash = fnv1aAdd(hash, item["ipv4"] | "");
    hash = fnv1aAdd(hash, static_cast<uint32_t>(item["up"] | false));
    hash = fnv1aAdd(hash, static_cast<uint32_t>(item["speed_mbps"] | 0));
  }
  return hash;
}

static uint32_t portSignature() {
  uint32_t hash = 2166136261u;
  for (JsonObject item : snapshot["ports"].as<JsonArray>()) {
    hash = fnv1aAdd(hash, static_cast<uint32_t>(item["port"] | 0));
    hash = fnv1aAdd(hash, item["protocol"] | "");
    hash = fnv1aAdd(hash, item["service"] | "");
    hash = fnv1aAdd(hash, item["address"] | "");
  }
  return hash;
}

static uint32_t dockerSignature() {
  uint32_t hash = 2166136261u;
  hash = fnv1aAdd(hash, static_cast<uint32_t>(snapshot["docker"]["available"] | false));
  for (JsonObject item : snapshot["docker"]["containers"].as<JsonArray>()) {
    hash = fnv1aAdd(hash, item["name"] | "");
    hash = fnv1aAdd(hash, item["image"] | "");
    hash = fnv1aAdd(hash, item["state"] | "");
  }
  return hash;
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
    textBox({static_cast<int16_t>(r.x + 8), static_cast<int16_t>(r.y + 6),
             static_cast<int16_t>(r.w - 16), 12},
            label, C::MUTED, 1, Align::Left, C::CARD);
  }
  valueBox({static_cast<int16_t>(r.x + 7), static_cast<int16_t>(r.y + 20),
            static_cast<int16_t>(r.w - 14), static_cast<int16_t>(r.h - 36)},
           value, C::TEXT, Align::Left, C::CARD);
  progressBar({static_cast<int16_t>(r.x + 7), static_cast<int16_t>(r.y + r.h - 10),
               static_cast<int16_t>(r.w - 14), 4},
              percent, accent);
}

static void drawStatCard(const Rect &r, const String &label, const String &value,
                         uint16_t accent, bool full) {
  if (full) {
    cardFrame(r);
    gfx->fillRoundRect(r.x + 7, r.y + 7, 3, 12, 1, accent);
    textBox({static_cast<int16_t>(r.x + 14), static_cast<int16_t>(r.y + 5),
             static_cast<int16_t>(r.w - 21), 16},
            label, C::MUTED, 1, Align::Left, C::CARD);
  }
  valueBox({static_cast<int16_t>(r.x + 8), static_cast<int16_t>(r.y + 21),
            static_cast<int16_t>(r.w - 16), static_cast<int16_t>(r.h - 26)},
           value, C::TEXT, Align::Left, C::CARD);
}

static void drawHeaderBase() {
  gfx->fillRect(0, 0, SCREEN_W, HEADER_H, C::TOP);
  gfx->drawFastHLine(0, HEADER_H - 1, SCREEN_W, C::BORDER);
  rawText(8, 4, PAGE_NAMES[currentPage], C::TEXT, 2, C::TOP);
}

static void drawHeaderIdentity(bool force = false) {
  const String ip = snapshot["system"]["ip"] | "sin datos";
  if (!force && ip == lastHeaderIdentity) return;
  const int16_t rightReserve = LANDSCAPE ? 71 : 64;
  textBox({8, 22, static_cast<int16_t>(SCREEN_W - 16 - rightReserve), 13}, ip,
          C::MUTED, 1, Align::Left, C::TOP);
  lastHeaderIdentity = ip;
}

static void drawStatusChip(bool force = false) {
  const String current = String(statusText) + (online ? "1" : "0");
  if (!force && current == lastStatusRendered) return;
  const int16_t w = LANDSCAPE ? 65 : 59;
  const int16_t x = SCREEN_W - w - 7;
  const uint16_t fg = online ? C::OK : (strcmp(statusText, "API") == 0 ? C::WARN : C::BAD);
  const uint16_t bg = online ? C::OK_DARK : (strcmp(statusText, "API") == 0 ? C::WARN_DARK : C::BAD_DARK);
  gfx->fillRoundRect(x, 5, w, 15, 7, bg);
  gfx->fillCircle(x + 8, 12, 3, fg);
  textBox({static_cast<int16_t>(x + 14), 6, static_cast<int16_t>(w - 17), 13},
          statusText, fg, 1, Align::Center, bg);
  lastStatusRendered = current;
}

static void drawFooter() {
  const int16_t y = SCREEN_H - FOOTER_H;
  gfx->fillRect(0, y, SCREEN_W, FOOTER_H, C::TOP);
  gfx->drawFastHLine(0, y, SCREEN_W, C::BORDER);

  const Rect left{7, static_cast<int16_t>(y + 4), 42, static_cast<int16_t>(FOOTER_H - 8)};
  const Rect right{static_cast<int16_t>(SCREEN_W - 49), static_cast<int16_t>(y + 4), 42,
                   static_cast<int16_t>(FOOTER_H - 8)};
  gfx->fillRoundRect(left.x, left.y, left.w, left.h, 5, C::CARD_ALT);
  gfx->drawRoundRect(left.x, left.y, left.w, left.h, 5, C::BORDER);
  gfx->fillRoundRect(right.x, right.y, right.w, right.h, 5, C::CARD_ALT);
  gfx->drawRoundRect(right.x, right.y, right.w, right.h, 5, C::BORDER);
  textBox({static_cast<int16_t>(left.x + 3), static_cast<int16_t>(left.y + 2),
           static_cast<int16_t>(left.w - 6), static_cast<int16_t>(left.h - 4)},
          "<", C::TEXT, 1, Align::Center, C::CARD_ALT);
  textBox({static_cast<int16_t>(right.x + 3), static_cast<int16_t>(right.y + 2),
           static_cast<int16_t>(right.w - 6), static_cast<int16_t>(right.h - 4)},
          ">", C::TEXT, 1, Align::Center, C::CARD_ALT);

  const int16_t dotsW = PAGE_COUNT * 12;
  const int16_t dotsX = (SCREEN_W - dotsW) / 2 + 6;
  const int16_t cy = y + FOOTER_H / 2;
  for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
    const int16_t cx = dotsX + i * 12;
    if (i == currentPage) gfx->fillRoundRect(cx - 4, cy - 2, 9, 5, 2, C::ACCENT);
    else gfx->fillCircle(cx, cy, 2, C::DIM);
  }
}

static void drawChart(const Rect &card, bool full) {
  if (full) {
    cardFrame(card);
    gfx->fillRoundRect(card.x + 7, card.y + 7, 3, 12, 1, C::ACCENT);
    textBox({static_cast<int16_t>(card.x + 14), static_cast<int16_t>(card.y + 5),
             static_cast<int16_t>(card.w - 108), 16},
            "HISTORICO 5 MIN", C::MUTED, 1, Align::Left, C::CARD);
    textBox({static_cast<int16_t>(card.x + card.w - 84), static_cast<int16_t>(card.y + 5), 34, 14},
            "CPU", C::ACCENT, 1, Align::Right, C::CARD);
    textBox({static_cast<int16_t>(card.x + card.w - 45), static_cast<int16_t>(card.y + 5), 37, 14},
            "RAM", C::OK, 1, Align::Right, C::CARD);
  }

  const Rect plot{static_cast<int16_t>(card.x + 8), static_cast<int16_t>(card.y + 25),
                  static_cast<int16_t>(card.w - 16), static_cast<int16_t>(card.h - 33)};
  gfx->fillRect(plot.x, plot.y, plot.w, plot.h, C::CARD);
  for (uint8_t i = 1; i < 4; ++i) {
    const int16_t gy = plot.y + (plot.h * i) / 4;
    gfx->drawFastHLine(plot.x, gy, plot.w, C::GRID);
  }

  JsonArray history = snapshot["history"].as<JsonArray>();
  const size_t count = history.size();
  if (count < 2) {
    textBox(plot, "Esperando muestras...", C::DIM, 1, Align::Center, C::CARD);
    return;
  }

  const size_t maxPoints = sizeMin(count, static_cast<size_t>(plot.w));
  const size_t start = count - maxPoints;
  int16_t previousCpuX = 0, previousCpuY = 0, previousRamX = 0, previousRamY = 0;
  bool first = true;
  for (size_t index = start; index < count; ++index) {
    const size_t local = index - start;
    const int16_t x = plot.x + static_cast<int16_t>((local * (plot.w - 1)) / sizeMax(static_cast<size_t>(1), maxPoints - 1));
    const float cpu = constrain(history[index]["cpu"] | 0.0f, 0.0f, 100.0f);
    const float ram = constrain(history[index]["ram"] | 0.0f, 0.0f, 100.0f);
    const int16_t cpuY = plot.y + plot.h - 1 - static_cast<int16_t>((cpu * (plot.h - 1)) / 100.0f);
    const int16_t ramY = plot.y + plot.h - 1 - static_cast<int16_t>((ram * (plot.h - 1)) / 100.0f);
    if (!first) {
      gfx->drawLine(previousCpuX, previousCpuY, x, cpuY, C::ACCENT);
      gfx->drawLine(previousRamX, previousRamY, x, ramY, C::OK);
    }
    previousCpuX = previousRamX = x;
    previousCpuY = cpuY;
    previousRamY = ramY;
    first = false;
  }
}

static void drawCompactMetric(const Rect &r, const String &label, const String &value,
                              uint16_t accent, bool full) {
  if (full) {
    textBox({r.x, r.y, r.w, 11}, label, C::MUTED, 1, Align::Left, C::CARD);
  }
  textBox({r.x, static_cast<int16_t>(r.y + 12), r.w, static_cast<int16_t>(r.h - 12)},
          value, accent, 1, Align::Left, C::CARD);
}

static void drawOverview(bool full) {
  const float cpu = snapshot["cpu"]["percent"] | 0.0f;
  const float ram = snapshot["memory"]["percent"] | 0.0f;
  const float temp = snapshot["cpu"]["temperature_c"] | 0.0f;
  const float disk = snapshot["disk"]["percent"] | 0.0f;

  if (LANDSCAPE) {
    const int16_t topH = 58;
    drawKpi(kpiRect(0, 4, CONTENT_TOP, topH), "CPU", String(cpu, 0) + "%", cpu,
            healthColor(cpu, 70, 90), full);
    drawKpi(kpiRect(1, 4, CONTENT_TOP, topH), "RAM", String(ram, 0) + "%", ram,
            healthColor(ram, 75, 90), full);
    drawKpi(kpiRect(2, 4, CONTENT_TOP, topH), "TEMP", String(temp, 1) + " C",
            constrain(temp, 0.0f, 100.0f), healthColor(temp, 70, 80), full);
    drawKpi(kpiRect(3, 4, CONTENT_TOP, topH), "DISCO", String(disk, 0) + "%", disk,
            healthColor(disk, 80, 92), full);

    const int16_t y = CONTENT_TOP + topH + GAP;
    const int16_t h = CONTENT_BOTTOM - y;
    const int16_t chartW = (SCREEN_W - MARGIN * 2 - GAP) * 2 / 3;
    const Rect chart{MARGIN, y, chartW, h};
    const Rect side{static_cast<int16_t>(MARGIN + chartW + GAP), y,
                    static_cast<int16_t>(SCREEN_W - MARGIN * 2 - GAP - chartW), h};
    drawChart(chart, full);
    if (full) cardFrame(side, "TRAFICO", C::OK);
    const int16_t innerX = side.x + 8;
    const int16_t innerW = side.w - 16;
    // El panel de trafico reserva una franja exclusiva para el uptime. Antes,
    // el valor de SUBIDA ocupaba 28 px y alcanzaba la misma zona que "UP ...",
    // provocando recortes cuando ambos se actualizaban.
    const int16_t metricH = 25;
    const int16_t firstMetricY = side.y + 27;
    const int16_t separatorY = firstMetricY + metricH + 4;
    const int16_t secondMetricY = separatorY + 5;
    const int16_t uptimeH = 13;
    const int16_t uptimeY = side.y + side.h - uptimeH - 6;

    drawCompactMetric({innerX, firstMetricY, innerW, metricH}, "DESCARGA",
                      rate(snapshot["network"]["rx_bps"] | 0.0), C::ACCENT, full);
    divider(innerX, separatorY, innerW);
    drawCompactMetric({innerX, secondMetricY, innerW,
                       static_cast<int16_t>(uptimeY - secondMetricY - 3)},
                      "SUBIDA", rate(snapshot["network"]["tx_bps"] | 0.0), C::OK, full);
    textBox({innerX, uptimeY, innerW, uptimeH},
            "UP " + uptimeText(snapshot["system"]["uptime_s"] | 0ULL), C::MUTED, 1,
            Align::Left, C::CARD);
  } else {
    const int16_t topH = 56;
    const int16_t secondY = CONTENT_TOP + topH + GAP;
    drawKpi(kpiRect(0, 2, CONTENT_TOP, topH), "CPU", String(cpu, 0) + "%", cpu,
            healthColor(cpu, 70, 90), full);
    drawKpi(kpiRect(1, 2, CONTENT_TOP, topH), "RAM", String(ram, 0) + "%", ram,
            healthColor(ram, 75, 90), full);
    drawKpi(kpiRect(0, 2, secondY, topH), "TEMP", String(temp, 1) + " C",
            constrain(temp, 0.0f, 100.0f), healthColor(temp, 70, 80), full);
    drawKpi(kpiRect(1, 2, secondY, topH), "DISCO", String(disk, 0) + "%", disk,
            healthColor(disk, 80, 92), full);

    const int16_t chartY = secondY + topH + GAP;
    const int16_t netH = 49;
    const int16_t chartH = CONTENT_BOTTOM - chartY - GAP - netH;
    const Rect chart{MARGIN, chartY, static_cast<int16_t>(SCREEN_W - MARGIN * 2), chartH};
    const Rect net{MARGIN, static_cast<int16_t>(chartY + chartH + GAP),
                   static_cast<int16_t>(SCREEN_W - MARGIN * 2), netH};
    drawChart(chart, full);
    if (full) cardFrame(net, "RED", C::OK);
    const int16_t half = (net.w - 24) / 2;
    drawCompactMetric({static_cast<int16_t>(net.x + 8), static_cast<int16_t>(net.y + 25), half, 20},
                      "RX", rate(snapshot["network"]["rx_bps"] | 0.0), C::ACCENT, full);
    drawCompactMetric({static_cast<int16_t>(net.x + 16 + half), static_cast<int16_t>(net.y + 25), half, 20},
                      "TX", rate(snapshot["network"]["tx_bps"] | 0.0), C::OK, full);
  }
}

static void drawStorageList(const Rect &card, bool full) {
  const uint32_t signature = storageSignature();
  if (!full && signature == listSignatures[1]) return;
  listSignatures[1] = signature;
  const Rect body{static_cast<int16_t>(card.x + 4), static_cast<int16_t>(card.y + 24),
                  static_cast<int16_t>(card.w - 8), static_cast<int16_t>(card.h - 28)};
  gfx->fillRect(body.x, body.y, body.w, body.h, C::CARD);
  const int16_t rowH = LANDSCAPE ? 20 : 24;
  const uint8_t maxRows = u8Max(1, static_cast<uint8_t>(body.h / rowH));
  uint8_t rowIndex = 0;
  for (JsonObject item : snapshot["storage"]["mounts"].as<JsonArray>()) {
    if (rowIndex >= maxRows) break;
    const int16_t y = body.y + rowIndex * rowH;
    if (rowIndex & 1) gfx->fillRect(body.x, y, body.w, rowH, C::CARD_ALT);
    const String mount = item["mount"] | "--";
    const float pct = item["percent"] | 0.0f;
    textBox({static_cast<int16_t>(body.x + 6), y, static_cast<int16_t>(body.w * 35 / 100), 11},
            mount, C::TEXT, 1, Align::Left, (rowIndex & 1) ? C::CARD_ALT : C::CARD);
    textBox({static_cast<int16_t>(body.x + body.w * 35 / 100), y,
             static_cast<int16_t>(body.w * 45 / 100), 11},
            bytes(item["used_bytes"] | 0.0) + " / " + bytes(item["total_bytes"] | 0.0),
            C::MUTED, 1, Align::Left, (rowIndex & 1) ? C::CARD_ALT : C::CARD);
    textBox({static_cast<int16_t>(body.x + body.w * 80 / 100), y,
             static_cast<int16_t>(body.w * 18 / 100), 11},
            String(pct, 0) + "%", healthColor(pct, 80, 92), 1, Align::Right,
            (rowIndex & 1) ? C::CARD_ALT : C::CARD);
    progressBar({static_cast<int16_t>(body.x + 6), static_cast<int16_t>(y + rowH - 6),
                 static_cast<int16_t>(body.w - 12), 3},
                pct, healthColor(pct, 80, 92), (rowIndex & 1) ? C::CARD_DEEP : C::CARD_DEEP);
    ++rowIndex;
  }
  if (rowIndex == 0) textBox(body, "Sin puntos de montaje", C::DIM, 1, Align::Center, C::CARD);
}

static void drawStorage(bool full) {
  const float disk = snapshot["disk"]["percent"] | 0.0f;
  if (LANDSCAPE) {
    const int16_t h = 50;
    drawStatCard(kpiRect(0, 3, CONTENT_TOP, h), "LECTURA",
                 rate(snapshot["disk"]["read_bps"] | 0.0), C::ACCENT, full);
    drawStatCard(kpiRect(1, 3, CONTENT_TOP, h), "ESCRITURA",
                 rate(snapshot["disk"]["write_bps"] | 0.0), C::OK, full);
    drawStatCard(kpiRect(2, 3, CONTENT_TOP, h), "RAIZ LIBRE",
                 bytes(snapshot["disk"]["free_bytes"] | 0.0), healthColor(disk, 80, 92), full);
    const Rect list{MARGIN, static_cast<int16_t>(CONTENT_TOP + h + GAP),
                    static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                    static_cast<int16_t>(CONTENT_BOTTOM - CONTENT_TOP - h - GAP)};
    if (full) cardFrame(list, "ALMACENAMIENTO", C::WARN);
    drawStorageList(list, full);
  } else {
    const int16_t h = 46;
    drawStatCard(kpiRect(0, 2, CONTENT_TOP, h), "LECTURA",
                 rate(snapshot["disk"]["read_bps"] | 0.0), C::ACCENT, full);
    drawStatCard(kpiRect(1, 2, CONTENT_TOP, h), "ESCRITURA",
                 rate(snapshot["disk"]["write_bps"] | 0.0), C::OK, full);
    const Rect root{MARGIN, static_cast<int16_t>(CONTENT_TOP + h + GAP),
                    static_cast<int16_t>(SCREEN_W - MARGIN * 2), 42};
    if (full) cardFrame(root);
    textBox({static_cast<int16_t>(root.x + 8), static_cast<int16_t>(root.y + 5), 80, 12},
            "DISCO RAIZ", C::MUTED, 1, Align::Left, C::CARD);
    valueBox({static_cast<int16_t>(root.x + 8), static_cast<int16_t>(root.y + 17), 55, 17},
             String(disk, 0) + "%", healthColor(disk, 80, 92), Align::Left, C::CARD);
    progressBar({static_cast<int16_t>(root.x + 70), static_cast<int16_t>(root.y + 24),
                 static_cast<int16_t>(root.w - 80), 5},
                disk, healthColor(disk, 80, 92));
    const Rect list{MARGIN, static_cast<int16_t>(root.y + root.h + GAP),
                    static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                    static_cast<int16_t>(CONTENT_BOTTOM - root.y - root.h - GAP)};
    if (full) cardFrame(list, "ALMACENAMIENTO", C::WARN);
    drawStorageList(list, full);
  }
}

static void drawInterfaceList(const Rect &card, bool full) {
  const uint32_t signature = interfaceSignature();
  if (!full && signature == listSignatures[2]) return;
  listSignatures[2] = signature;
  const Rect body{static_cast<int16_t>(card.x + 4), static_cast<int16_t>(card.y + 24),
                  static_cast<int16_t>(card.w - 8), static_cast<int16_t>(card.h - 28)};
  gfx->fillRect(body.x, body.y, body.w, body.h, C::CARD);
  const int16_t rowH = LANDSCAPE ? 21 : 25;
  const uint8_t maxRows = u8Max(1, static_cast<uint8_t>(body.h / rowH));
  uint8_t rowIndex = 0;
  for (JsonObject item : snapshot["interfaces"].as<JsonArray>()) {
    if (rowIndex >= maxRows) break;
    const int16_t y = body.y + rowIndex * rowH;
    const bool up = item["up"] | false;
    const uint16_t bg = (rowIndex & 1) ? C::CARD_ALT : C::CARD;
    if (rowIndex & 1) gfx->fillRect(body.x, y, body.w, rowH, bg);
    gfx->fillCircle(body.x + 9, y + 7, 3, up ? C::OK : C::BAD);
    textBox({static_cast<int16_t>(body.x + 17), y, static_cast<int16_t>(body.w * 25 / 100), 13},
            item["name"] | "--", C::TEXT, 1, Align::Left, bg);
    textBox({static_cast<int16_t>(body.x + body.w * 28 / 100), y,
             static_cast<int16_t>(body.w * 45 / 100), 13},
            item["ipv4"] | "--", C::MUTED, 1, Align::Left, bg);
    String speed = item["speed_mbps"].isNull() ? "--" : String(item["speed_mbps"].as<int>()) + "M";
    textBox({static_cast<int16_t>(body.x + body.w * 74 / 100), y,
             static_cast<int16_t>(body.w * 23 / 100), 13},
            up ? speed : "CAIDA", up ? C::OK : C::BAD, 1, Align::Right, bg);
    ++rowIndex;
  }
  if (rowIndex == 0) textBox(body, "Sin interfaces", C::DIM, 1, Align::Center, C::CARD);
}

static void drawNetwork(bool full) {
  if (LANDSCAPE) {
    const int16_t h = 50;
    drawStatCard(kpiRect(0, 3, CONTENT_TOP, h), "DESCARGA",
                 rate(snapshot["network"]["rx_bps"] | 0.0), C::ACCENT, full);
    drawStatCard(kpiRect(1, 3, CONTENT_TOP, h), "SUBIDA",
                 rate(snapshot["network"]["tx_bps"] | 0.0), C::OK, full);
    drawStatCard(kpiRect(2, 3, CONTENT_TOP, h), "IP LOCAL",
                 snapshot["system"]["ip"] | "--", C::WARN, full);
    const Rect list{MARGIN, static_cast<int16_t>(CONTENT_TOP + h + GAP),
                    static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                    static_cast<int16_t>(CONTENT_BOTTOM - CONTENT_TOP - h - GAP)};
    if (full) cardFrame(list, "INTERFACES", C::ACCENT);
    drawInterfaceList(list, full);
  } else {
    const int16_t h = 46;
    drawStatCard(kpiRect(0, 2, CONTENT_TOP, h), "DESCARGA",
                 rate(snapshot["network"]["rx_bps"] | 0.0), C::ACCENT, full);
    drawStatCard(kpiRect(1, 2, CONTENT_TOP, h), "SUBIDA",
                 rate(snapshot["network"]["tx_bps"] | 0.0), C::OK, full);
    const Rect ip{MARGIN, static_cast<int16_t>(CONTENT_TOP + h + GAP),
                  static_cast<int16_t>(SCREEN_W - MARGIN * 2), 40};
    drawStatCard(ip, "IP LOCAL", snapshot["system"]["ip"] | "--", C::WARN, full);
    const Rect list{MARGIN, static_cast<int16_t>(ip.y + ip.h + GAP),
                    static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                    static_cast<int16_t>(CONTENT_BOTTOM - ip.y - ip.h - GAP)};
    if (full) cardFrame(list, "INTERFACES", C::ACCENT);
    drawInterfaceList(list, full);
  }
}

static void drawCoreRows(const Rect &card, bool full) {
  if (full) cardFrame(card, "NUCLEOS", C::ACCENT);
  const Rect body{static_cast<int16_t>(card.x + 7), static_cast<int16_t>(card.y + 25),
                  static_cast<int16_t>(card.w - 14), static_cast<int16_t>(card.h - 31)};
  JsonArray cores = snapshot["cpu"]["per_core"].as<JsonArray>();
  const uint8_t maxRows = u8Min(static_cast<uint8_t>(cores.size()), static_cast<uint8_t>(LANDSCAPE ? 4 : 5));
  const int16_t rowH = maxRows ? body.h / maxRows : body.h;
  for (uint8_t i = 0; i < maxRows; ++i) {
    const float value = cores[i].as<float>();
    const int16_t y = body.y + i * rowH;
    textBox({body.x, y, 34, rowH}, "CPU" + String(i), C::MUTED, 1, Align::Left, C::CARD);
    progressBar({static_cast<int16_t>(body.x + 37), static_cast<int16_t>(y + rowH / 2 - 3),
                 static_cast<int16_t>(body.w - 77), 6},
                value, healthColor(value, 75, 92));
    textBox({static_cast<int16_t>(body.x + body.w - 37), y, 37, rowH},
            String(value, 0) + "%", C::TEXT, 1, Align::Right, C::CARD);
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
  uint8_t index = 0;
  for (JsonObject item : snapshot["temperatures"].as<JsonArray>()) {
    if (index >= maxRows) break;
    const int16_t y = body.y + index * rowH;
    const float value = item["current_c"] | 0.0f;
    const uint16_t bg = (index & 1) ? C::CARD_ALT : C::CARD;
    if (index & 1) gfx->fillRect(body.x, y, body.w, rowH, bg);
    textBox({static_cast<int16_t>(body.x + 4), y, static_cast<int16_t>(body.w - 58), rowH},
            item["name"] | "sensor", C::MUTED, 1, Align::Left, bg);
    textBox({static_cast<int16_t>(body.x + body.w - 54), y, 50, rowH},
            String(value, 1) + " C", healthColor(value, 70, 80), 1, Align::Right, bg);
    ++index;
  }
  if (index == 0) textBox(body, "Sin sensores", C::DIM, 1, Align::Center, C::CARD);
}

static void drawCpu(bool full) {
  const float cpu = snapshot["cpu"]["percent"] | 0.0f;
  const float temp = snapshot["cpu"]["temperature_c"] | 0.0f;
  const float freq = snapshot["cpu"]["frequency_mhz"] | 0.0f;
  JsonArray load = snapshot["cpu"]["load"].as<JsonArray>();
  const float load1 = load.size() ? load[0].as<float>() : 0.0f;

  if (LANDSCAPE) {
    const int16_t h = 50;
    drawStatCard(kpiRect(0, 4, CONTENT_TOP, h), "CPU", String(cpu, 0) + "%",
                 healthColor(cpu, 70, 90), full);
    drawStatCard(kpiRect(1, 4, CONTENT_TOP, h), "TEMP", String(temp, 1) + " C",
                 healthColor(temp, 70, 80), full);
    drawStatCard(kpiRect(2, 4, CONTENT_TOP, h), "FREC.", String(freq, 0) + "M",
                 C::ACCENT, full);
    drawStatCard(kpiRect(3, 4, CONTENT_TOP, h), "LOAD 1M", String(load1, 2),
                 C::WARN, full);
    const int16_t y = CONTENT_TOP + h + GAP;
    const int16_t totalW = SCREEN_W - MARGIN * 2 - GAP;
    const Rect cores{MARGIN, y, static_cast<int16_t>(totalW * 58 / 100),
                     static_cast<int16_t>(CONTENT_BOTTOM - y)};
    const Rect sensors{static_cast<int16_t>(cores.x + cores.w + GAP), y,
                       static_cast<int16_t>(totalW - cores.w), cores.h};
    drawCoreRows(cores, full);
    drawSensorRows(sensors, full);
  } else {
    const int16_t h = 44;
    const int16_t secondY = CONTENT_TOP + h + GAP;
    drawStatCard(kpiRect(0, 2, CONTENT_TOP, h), "CPU", String(cpu, 0) + "%",
                 healthColor(cpu, 70, 90), full);
    drawStatCard(kpiRect(1, 2, CONTENT_TOP, h), "TEMP", String(temp, 1) + " C",
                 healthColor(temp, 70, 80), full);
    drawStatCard(kpiRect(0, 2, secondY, h), "FRECUENCIA", String(freq, 0) + " MHz",
                 C::ACCENT, full);
    drawStatCard(kpiRect(1, 2, secondY, h), "LOAD 1M", String(load1, 2), C::WARN, full);
    const int16_t coresY = secondY + h + GAP;
    const int16_t coresH = 70;
    const Rect cores{MARGIN, coresY, static_cast<int16_t>(SCREEN_W - MARGIN * 2), coresH};
    const Rect sensors{MARGIN, static_cast<int16_t>(coresY + coresH + GAP),
                       static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                       static_cast<int16_t>(CONTENT_BOTTOM - coresY - coresH - GAP)};
    drawCoreRows(cores, full);
    drawSensorRows(sensors, full);
  }
}

static void drawPorts(bool full) {
  const Rect card{MARGIN, CONTENT_TOP, static_cast<int16_t>(SCREEN_W - MARGIN * 2), CONTENT_H};
  if (full) cardFrame(card, "SERVICIOS EN ESCUCHA", C::ACCENT);
  const uint32_t signature = portSignature();
  if (!full && signature == listSignatures[4]) return;
  listSignatures[4] = signature;

  const Rect body{static_cast<int16_t>(card.x + 4), static_cast<int16_t>(card.y + 24),
                  static_cast<int16_t>(card.w - 8), static_cast<int16_t>(card.h - 28)};
  gfx->fillRect(body.x, body.y, body.w, body.h, C::CARD);
  const int16_t headerH = 16;
  gfx->fillRect(body.x, body.y, body.w, headerH, C::CARD_DEEP);
  const int16_t c1 = LANDSCAPE ? 68 : 58;
  const int16_t c3 = LANDSCAPE ? 94 : 64;
  const int16_t c2 = body.w - c1 - c3;
  textBox({static_cast<int16_t>(body.x + 5), body.y, static_cast<int16_t>(c1 - 8), headerH},
          "PUERTO", C::DIM, 1, Align::Left, C::CARD_DEEP);
  textBox({static_cast<int16_t>(body.x + c1), body.y, c2, headerH},
          "SERVICIO", C::DIM, 1, Align::Left, C::CARD_DEEP);
  textBox({static_cast<int16_t>(body.x + c1 + c2), body.y, static_cast<int16_t>(c3 - 5), headerH},
          "BIND", C::DIM, 1, Align::Right, C::CARD_DEEP);

  const int16_t rowH = LANDSCAPE ? 17 : 18;
  const uint8_t maxRows = u8Max(1, static_cast<uint8_t>((body.h - headerH) / rowH));
  uint8_t index = 0;
  for (JsonObject item : snapshot["ports"].as<JsonArray>()) {
    if (index >= maxRows) break;
    const int16_t y = body.y + headerH + index * rowH;
    const uint16_t bg = (index & 1) ? C::CARD_ALT : C::CARD;
    if (index & 1) gfx->fillRect(body.x, y, body.w, rowH, bg);
    textBox({static_cast<int16_t>(body.x + 5), y, static_cast<int16_t>(c1 - 8), rowH},
            String(item["port"] | 0) + "/" + String(item["protocol"] | "tcp"),
            C::ACCENT, 1, Align::Left, bg);
    textBox({static_cast<int16_t>(body.x + c1), y, c2, rowH},
            item["service"] | "unknown", C::TEXT, 1, Align::Left, bg);
    textBox({static_cast<int16_t>(body.x + c1 + c2), y, static_cast<int16_t>(c3 - 5), rowH},
            item["address"] | "*", C::MUTED, 1, Align::Right, bg);
    ++index;
  }
  if (index == 0) textBox({body.x, static_cast<int16_t>(body.y + headerH), body.w,
                           static_cast<int16_t>(body.h - headerH)},
                          "Sin puertos disponibles", C::DIM, 1, Align::Center, C::CARD);
}

static void drawDockerList(const Rect &card, bool full) {
  const uint32_t signature = dockerSignature();
  if (!full && signature == listSignatures[5]) return;
  listSignatures[5] = signature;
  const Rect body{static_cast<int16_t>(card.x + 4), static_cast<int16_t>(card.y + 24),
                  static_cast<int16_t>(card.w - 8), static_cast<int16_t>(card.h - 28)};
  gfx->fillRect(body.x, body.y, body.w, body.h, C::CARD);
  const int16_t rowH = LANDSCAPE ? 20 : 24;
  const uint8_t maxRows = u8Max(1, static_cast<uint8_t>(body.h / rowH));
  uint8_t index = 0;
  for (JsonObject item : snapshot["docker"]["containers"].as<JsonArray>()) {
    if (index >= maxRows) break;
    const int16_t y = body.y + index * rowH;
    const String state = item["state"] | "unknown";
    const bool running = state == "running";
    const uint16_t bg = (index & 1) ? C::CARD_ALT : C::CARD;
    if (index & 1) gfx->fillRect(body.x, y, body.w, rowH, bg);
    gfx->fillCircle(body.x + 9, y + rowH / 2, 3, running ? C::OK : C::WARN);
    const int16_t stateW = LANDSCAPE ? 58 : 44;
    const int16_t nameW = LANDSCAPE ? 95 : 73;
    textBox({static_cast<int16_t>(body.x + 17), y, nameW, rowH},
            item["name"] | "--", C::TEXT, 1, Align::Left, bg);
    textBox({static_cast<int16_t>(body.x + 17 + nameW), y,
             static_cast<int16_t>(body.w - 17 - nameW - stateW), rowH},
            item["image"] | "--", C::MUTED, 1, Align::Left, bg);
    textBox({static_cast<int16_t>(body.x + body.w - stateW), y,
             static_cast<int16_t>(stateW - 5), rowH},
            running ? "RUN" : "STOP", running ? C::OK : C::WARN, 1, Align::Right, bg);
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
  if (LANDSCAPE) {
    const int16_t h = 50;
    drawStatCard(kpiRect(0, 3, CONTENT_TOP, h), "ESTADO", available ? "DISP." : "ERROR",
                 available ? C::OK : C::BAD, full);
    drawStatCard(kpiRect(1, 3, CONTENT_TOP, h), "ACTIVOS",
                 String(snapshot["docker"]["running"] | 0), C::OK, full);
    drawStatCard(kpiRect(2, 3, CONTENT_TOP, h), "TOTAL",
                 String(snapshot["docker"]["total"] | 0), C::ACCENT, full);
    const Rect list{MARGIN, static_cast<int16_t>(CONTENT_TOP + h + GAP),
                    static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                    static_cast<int16_t>(CONTENT_BOTTOM - CONTENT_TOP - h - GAP)};
    if (full) cardFrame(list, "CONTENEDORES", C::OK);
    drawDockerList(list, full);
  } else {
    const int16_t h = 46;
    drawStatCard(kpiRect(0, 2, CONTENT_TOP, h), "ACTIVOS",
                 String(snapshot["docker"]["running"] | 0), C::OK, full);
    drawStatCard(kpiRect(1, 2, CONTENT_TOP, h), "TOTAL",
                 String(snapshot["docker"]["total"] | 0), C::ACCENT, full);
    const Rect status{MARGIN, static_cast<int16_t>(CONTENT_TOP + h + GAP),
                      static_cast<int16_t>(SCREEN_W - MARGIN * 2), 38};
    drawStatCard(status, "DOCKER", available ? "DISPONIBLE" : "NO DISPONIBLE",
                 available ? C::OK : C::BAD, full);
    const Rect list{MARGIN, static_cast<int16_t>(status.y + status.h + GAP),
                    static_cast<int16_t>(SCREEN_W - MARGIN * 2),
                    static_cast<int16_t>(CONTENT_BOTTOM - status.y - status.h - GAP)};
    if (full) cardFrame(list, "CONTENEDORES", C::OK);
    drawDockerList(list, full);
  }
}

static void renderCurrentPage(bool full) {
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
  switch (currentPage) {
    case 0: drawOverview(full); break;
    case 1: drawStorage(full); break;
    case 2: drawNetwork(full); break;
    case 3: drawCpu(full); break;
    case 4: drawPorts(full); break;
    default: drawDocker(full); break;
  }
  if (full) lastPageChange = millis();
}

static void transformTouch(const TS_Point &raw, int16_t &x, int16_t &y) {
  const int16_t nx = constrain(map(raw.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, 239), 0, 239);
  const int16_t ny = constrain(map(raw.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, 319), 0, 319);
  switch (DISPLAY_ROTATION) {
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
  if (direction < 0) currentPage = (currentPage + PAGE_COUNT - 1) % PAGE_COUNT;
  else currentPage = (currentPage + 1) % PAGE_COUNT;
  renderCurrentPage(true);
}

static void handleTouch() {
  static bool wasPressed = false;
  static uint32_t lastTouch = 0;
  int16_t x = 0, y = 0;
  const bool pressed = readTouch(x, y);
  if (pressed && !wasPressed && millis() - lastTouch > 180) {
    lastTouch = millis();
    if (y >= SCREEN_H - FOOTER_H - 4) {
      if (x < SCREEN_W / 2) changePage(-1);
      else changePage(1);
    }
  }
  wasPressed = pressed;
}

static void setStatus(bool state, const char *value) {
  const bool changed = online != state || strcmp(statusText, value) != 0;
  online = state;
  statusText = value;
  if (changed && !DISPLAY_TEST_ONLY) drawStatusChip(true);
}

static bool fetchSnapshot() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(String(API_BASE_URL) + "/api/v2/snapshot")) return false;
  http.addHeader("X-API-Key", API_TOKEN);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("API HTTP %d\n", code);
    http.end();
    return false;
  }
  snapshot.clear();
  const DeserializationError error = deserializeJson(snapshot, http.getStream());
  http.end();
  if (error) {
    Serial.printf("JSON: %s\n", error.c_str());
    return false;
  }
  if ((snapshot["schema"] | 0) != 2) {
    Serial.println("Schema API incompatible");
    return false;
  }
  setStatus(true, "ONLINE");
  lastGoodSample = millis();
  renderCurrentPage(false);
  return true;
}

static void manageWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    reconnectDelay = 1000;
    return;
  }
  setStatus(false, "WIFI");
  if (static_cast<int32_t>(millis() - nextWifiAttempt) < 0) return;
  Serial.println("Conectando WiFi...");
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_NAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  nextWifiAttempt = millis() + reconnectDelay;
  reconnectDelay = u32Min(reconnectDelay * 2, 30000u);
}

static void drawSplash() {
  gfx->fillScreen(C::BG);
  const int16_t centerY = SCREEN_H / 2;
  gfx->fillRoundRect(SCREEN_W / 2 - 29, centerY - 45, 58, 5, 2, C::ACCENT);
  textBox({12, static_cast<int16_t>(centerY - 30), static_cast<int16_t>(SCREEN_W - 24), 24},
          "PI OPS", C::TEXT, 2, Align::Center, C::BG);
  textBox({12, static_cast<int16_t>(centerY - 4), static_cast<int16_t>(SCREEN_W - 24), 16},
          "RASPBERRY PI MONITOR", C::MUTED, 1, Align::Center, C::BG);
  textBox({12, static_cast<int16_t>(centerY + 18), static_cast<int16_t>(SCREEN_W - 24), 14},
          "v7.2  DARK PANEL", C::DIM, 1, Align::Center, C::BG);
  delay(SPLASH_TIME_MS);
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
          String(SCREEN_W) + "x" + SCREEN_H + " ROT=" + DISPLAY_ROTATION,
          C::ACCENT, 1, Align::Center, C::BG);
  textBox({8, static_cast<int16_t>(SCREEN_H / 2 + 62), static_cast<int16_t>(SCREEN_W - 16), 14},
          "TOCA LA PANTALLA", C::MUTED, 1, Align::Center, C::BG);
}

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
  Serial.begin(115200);
  Serial.printf("PI OPS v7.2 %dx%d rotation=%d invert=%d test=%d\n", SCREEN_W, SCREEN_H,
                DISPLAY_ROTATION, DISPLAY_INVERT_COLORS, DISPLAY_TEST_ONLY);

  pinMode(TFT_BL, OUTPUT);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, SCREEN_BRIGHTNESS);

  if (!gfx->begin()) Serial.println("ERROR iniciando ILI9341");
  gfx->setRotation(DISPLAY_ROTATION);

  touchSpi.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  if (!touch.begin(touchSpi)) Serial.println("ERROR iniciando XPT2046");
  touch.setRotation(0);

  if (DISPLAY_TEST_ONLY) {
    drawDisplayTest();
    return;
  }

  drawSplash();
  snapshot["system"]["hostname"] = "Raspberry Pi";
  snapshot["system"]["ip"] = "sin datos";
  renderCurrentPage(true);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  manageWifi();
}

void loop() {
  if (DISPLAY_TEST_ONLY) {
    displayTestLoop();
    return;
  }

  handleTouch();
  manageWifi();

  if (WiFi.status() == WL_CONNECTED && millis() - lastRequest >= REQUEST_INTERVAL_MS) {
    lastRequest = millis();
    if (!fetchSnapshot()) setStatus(false, "API");
  }

  if (lastGoodSample && millis() - lastGoodSample > STALE_DATA_MS && online) {
    setStatus(false, "STALE");
  }

#if AUTO_ROTATE_PAGE_MS > 0
  if (millis() - lastPageChange >= AUTO_ROTATE_PAGE_MS) {
    changePage(1);
  }
#endif
  delay(8);
}
