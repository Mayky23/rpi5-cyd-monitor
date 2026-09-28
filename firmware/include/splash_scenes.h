#pragma once

#include <Arduino_GFX_Library.h>
#include <algorithm>
#include <math.h>
#include "custom_splash.h"
#include "ui_type.h"

namespace Splash {
struct Palette { uint16_t bg, text, muted, accent, good, warn, bad; };
enum class Stage : uint8_t { Wifi, Api, Ready, Offline, Preview, Closing };
struct Point { float x, y; };
static constexpr uint8_t DESIGN_COUNT = 5;
static constexpr float PI_F = 3.14159265f;

class Canvas : public Arduino_Canvas {
 public:
  using Arduino_Canvas::Arduino_Canvas;
  void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override {
    if (x >= 0 && y >= 0 && x < width() && y < height())
      Arduino_Canvas::writePixelPreclipped(x, y, color);
  }
};

inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 248u) << 8u) | ((g & 252u) << 3u) | (b >> 3u);
}

inline uint16_t mix(uint16_t a, uint16_t b, float amount) {
  amount = std::max(0.0f, std::min(1.0f, amount));
  const int r = (a >> 11) + int(((b >> 11) - (a >> 11)) * amount);
  const int g = ((a >> 5) & 63) + int((int((b >> 5) & 63) - int((a >> 5) & 63)) * amount);
  const int bl = (a & 31) + int((int(b & 31) - int(a & 31)) * amount);
  return (r << 11) | (g << 5) | bl;
}

inline Palette paletteFor(uint8_t design) {
  Palette p{rgb(2,7,11), rgb(239,245,249), rgb(143,160,173), rgb(54,197,244),
            rgb(76,224,151), rgb(255,194,72), rgb(255,91,112)};
  if (design == 0) {
    p.bg = rgb(12,3,9); p.accent = rgb(211,17,83); p.good = rgb(125,181,42);
  } else if (design == 1) {
    p.bg = rgb(0,8,3); p.accent = rgb(69,242,127); p.good = p.accent;
  } else if (design == 2) {
    p.bg = rgb(1,8,18); p.accent = rgb(40,202,244); p.good = rgb(80,232,157);
  } else if (design == 3) {
    p.bg = rgb(3,5,21); p.accent = rgb(126,119,255); p.good = rgb(74,226,205);
  }
  return p;
}

class Scene {
 public:
  Scene(Arduino_GFX &target, const Palette &palette, int width, int height, int top = 0)
      : g(target), p(palette), viewportW(width), viewportH(height), stripOffset(top),
        scale(width / 320.0f), artTop((height - 28 - 156 * scale) / 2) {}

  void draw(uint8_t design, uint32_t elapsed, Stage stage) {
    time = elapsed / 1000.0f;
    state = stage;
    if (design == 4 && CustomSplash::available()) {
      CustomSplash::draw(g, viewportW, viewportH, stripOffset);
      customOverlay();
    } else {
      background(design);
      switch (design) {
        case 0: raspberry(); break;
        case 1: terminal(); break;
        case 2: cyber(); break;
        case 3: orbital(); break;
        default: customPlaceholder(); break;
      }
    }
    statusLabel();
  }

 private:
  Arduino_GFX &g;
  Palette p;
  int viewportW, viewportH, stripOffset;
  float scale, artTop, time = 0;
  Stage state = Stage::Wifi;

  int x(float value) const { return lroundf(value * scale); }
  int y(float value) const { return lroundf(artTop + value * scale) + stripOffset; }
  int size(float value) const { return std::max(1, x(value)); }
  uint16_t dim(uint16_t color, float amount) const { return mix(p.bg, color, amount); }
  float viewTop() const { return -artTop / scale; }
  float viewBottom() const { return (viewportH - artTop) / scale; }

  void line(float ax, float ay, float bx, float by, uint16_t color) {
    g.drawLine(x(ax), y(ay), x(bx), y(by), color);
  }
  void rect(float ax, float ay, float w, float h, uint16_t color) {
    g.fillRect(x(ax), y(ay), size(w), size(h), color);
  }
  void circle(float cx, float cy, float radius, uint16_t color, bool fill = true) {
    if (fill) g.fillCircle(x(cx), y(cy), size(radius), color);
    else g.drawCircle(x(cx), y(cy), size(radius), color);
  }
  void ellipse(float cx, float cy, float rx, float ry, uint16_t color, bool fill = true) {
    if (fill) g.fillEllipse(x(cx), y(cy), size(rx), size(ry), color);
    else g.drawEllipse(x(cx), y(cy), size(rx), size(ry), color);
  }
  void text(float ax, float ay, const char *value, uint16_t color) {
    g.setTextWrap(false); g.setTextSize(1); g.setTextColor(color);
    g.setCursor(x(ax), y(ay)); g.print(value);
  }
  template <size_t N> void poly(const Point (&points)[N], uint16_t color) {
    float top = points[0].y, bottom = top;
    for (const auto &point : points) { top = std::min(top, point.y); bottom = std::max(bottom, point.y); }
    for (int row = int(ceilf(top)); row <= int(bottom); ++row) {
      float intersections[N]; size_t count = 0;
      for (size_t i = 0, j = N - 1; i < N; j = i++) {
        const auto a = points[j], b = points[i];
        if ((a.y <= row && b.y > row) || (b.y <= row && a.y > row))
          intersections[count++] = a.x + (row - a.y) * (b.x - a.x) / (b.y - a.y);
      }
      std::sort(intersections, intersections + count);
      for (size_t i = 0; i + 1 < count; i += 2)
        line(intersections[i], row, intersections[i + 1], row, color);
    }
  }

  void background(uint8_t design) {
    const int first = std::max(0, -stripOffset);
    const int last = std::min(viewportH, int(g.height()) - stripOffset);
    for (int row = first; row < last; ++row) {
      const float position = row / float(std::max(1, viewportH - 1));
      uint16_t color = mix(p.bg, p.accent, 0.025f + sinf(position * PI_F) * 0.065f);
      if (design == 1 && row % 4 == 0) color = mix(color, p.good, 0.05f);
      g.drawFastHLine(0, row + stripOffset, viewportW, color);
    }
    for (float yy = viewTop() + 12; yy < viewBottom(); yy += 24)
      line(0, yy, 320, yy, dim(p.accent, design == 1 ? 0.12f : 0.055f));
  }

  void statusLabel() {
    const char *label = state == Stage::Ready ? "SISTEMA LISTO" :
                        state == Stage::Api ? "RECIBIENDO DATOS" :
                        state == Stage::Offline ? "SIN CONEXION" :
                        state == Stage::Preview ? "VISTA PREVIA" :
                        state == Stage::Closing ? "HASTA PRONTO" : "CONECTANDO WIFI";
    const int localY = viewportH - 27;
    const int first = std::max(0, -stripOffset), last = std::min(viewportH, int(g.height()) - stripOffset);
    if (localY < last && localY + 27 > first)
      g.fillRect(0, localY + stripOffset, viewportW, 27, rgb(0, 5, 8));
    const int xx = (viewportW - Typography::width(label, Typography::Body)) / 2;
    Typography::draw(g, xx, localY + 7 + stripOffset, label,
                     state == Stage::Ready ? p.good : p.text, rgb(0,5,8), Typography::Body);
  }

  void raspberry() {
    const float pulse = 1.0f + sinf(time * 2.2f) * 0.035f;
    const uint16_t berry = rgb(198,11,72), leaf = rgb(124,181,44), ink = rgb(0,0,0);
    for (int side : {-1, 1}) for (int row = 0; row < 4; ++row) {
      const float yy = 35 + row * 31;
      line(160 + side*48, yy, 160 + side*135, yy, dim(p.accent,.25f));
      const float travel = fmodf(time*.55f + row*.23f, 1.0f);
      rect(160 + side*(48 + travel*80), yy-1, 5, 3, p.accent);
    }
    const Point leftLeaf[]={{158,44},{140,41},{121,29},{112,13},{132,17},{134,9},{151,23},{160,39}};
    const Point rightLeaf[]={{162,44},{180,41},{199,29},{208,13},{188,17},{186,9},{169,23},{160,39}};
    poly(leftLeaf,leaf); poly(rightLeaf,leaf); ellipse(160,92,45*pulse,49*pulse,ink);
    const Point berries[][4]={
      {{142,54},{158,47},{158,68},{137,69}},{{162,47},{178,54},{183,69},{162,68}},
      {{127,72},{147,68},{151,91},{128,99}},{{151,72},{169,69},{169,94},{151,94}},
      {{173,69},{193,76},{192,99},{170,92}},{{132,101},{153,94},{158,119},{139,126}},
      {{159,97},{180,96},{181,121},{160,126}},{{145,128},{175,127},{160,145},{160,145}}};
    for (const auto &shape : berries) poly(shape,berry);
    text(18,143,"RASPBERRY PI",p.muted); text(264,143,"CORE",p.accent);
  }

  void terminal() {
    const uint16_t green=p.accent;
    text(18,9,"MONITOR // INIT",green); line(18,25,302,25,dim(green,.4f));
    const char *command="> connect.dashboard()";
    const size_t count=std::min(size_t(time*17),strlen(command));
    char typed[28]={0}; memcpy(typed,command,count); text(18,38,typed,p.text);
    const bool wifi=state!=Stage::Wifi, ready=state==Stage::Ready;
    if(time>.4f){text(18,66,"DISPLAY",p.muted);text(166,66,"READY",green);}
    if(time>.8f){text(18,88,"WIFI LINK",p.muted);text(166,88,wifi?"LINKED":"WAIT",wifi?green:p.warn);}
    if(time>1.2f){text(18,110,"API DATA",p.muted);text(166,110,ready?"READY":"WAIT",ready?green:p.warn);}
    const char spinner[]="|/-\\"; char value[24];
    snprintf(value,sizeof(value),"%c SYSTEM / %s",spinner[int(time*8)%4],ready?"ONLINE":"BOOT");
    text(18,137,value,green);
  }

  void cyber() {
    const uint16_t blue=p.accent, green=p.good;
    const Point shield[]={{160,7},{216,28},{208,91},{191,119},{160,143},{129,119},{112,91},{104,28}};
    const Point inside[]={{160,14},{208,32},{201,87},{185,112},{160,133},{135,112},{119,87},{112,32}};
    poly(shield,blue); poly(inside,dim(blue,.10f));
    ellipse(160,64,17,21,green,false); rect(137,68,46,36,green); rect(142,73,36,26,dim(green,.12f));
    circle(160,83,5,green); rect(158,84,5,12,green);
    const float scan=30+fmodf(time*34,92); line(126,scan,194,scan,mix(blue,p.text,.35f));
    for(int i=0;i<7;++i){const float t=fmodf(time*.7f+i/7.0f,1.0f);circle(104+112*t,28+sinf(t*PI_F)*-17,2,p.text);}
    text(12,143,"SECURE LINK",p.muted); text(272,143,"OK",green);
  }

  Point orbitPoint(float angle,float tilt,float rx,float ry) {
    const float px=cosf(angle)*rx, py=sinf(angle)*ry;
    return {160+px*cosf(tilt)-py*sinf(tilt),78+px*sinf(tilt)+py*cosf(tilt)};
  }
  void orbit(float tilt,float rx,float ry,uint16_t color) {
    Point previous=orbitPoint(0,tilt,rx,ry);
    for(float angle=.08f;angle<2*PI_F+.08f;angle+=.08f){const Point next=orbitPoint(angle,tilt,rx,ry);line(previous.x,previous.y,next.x,next.y,color);previous=next;}
  }
  void orbital() {
    for(int i=0;i<38;++i) circle((i*83+11)%313,viewTop()+fmodf(i*47+7,viewBottom()-viewTop()),.7f,dim(p.text,.36f));
    orbit(-.28f,114,39,dim(p.accent,.45f));orbit(.62f,89,37,dim(p.good,.40f));
    ellipse(160,78,39,39,dim(p.accent,.32f));circle(160,78,37,p.accent,false);
    ellipse(160,78,17,36,dim(p.text,.28f),false);ellipse(160,78,36,13,dim(p.text,.28f),false);
    const Point land[]={{143,46},{164,44},{168,55},{158,62},{163,76},{153,80},{147,67},{135,63}};poly(land,dim(p.good,.8f));
    const Point sat=orbitPoint(time*.8f,-.28f,114,39);rect(sat.x-4,sat.y-4,8,8,p.text);rect(sat.x-18,sat.y-3,11,6,p.accent);rect(sat.x+7,sat.y-3,11,6,p.accent);
    text(15,143,"UPLINK",p.muted);text(267,143,"LIVE",p.good);
  }

  void customPlaceholder() {
    const float breathe=.55f+sinf(time*2.0f)*.18f;
    rect(108,30,104,76,dim(p.accent,.17f));
    line(108,30,212,30,p.accent);line(108,106,212,106,p.accent);line(108,30,108,106,p.accent);line(212,30,212,106,p.accent);
    const Point mountain[]={{118,94},{144,67},{159,83},{178,55},{202,94}};poly(mountain,dim(p.accent,breathe));circle(187,48,8,p.warn);
    text(112,118,"TU IMAGEN",p.text);text(74,139,"CREALA DESDE INSTALL.PS1",p.muted);
  }

  void customOverlay() {
    const int scanY=int(fmodf(time*28,std::max(1,viewportH-30)));
    if(scanY+stripOffset>=0&&scanY+stripOffset<g.height())g.drawFastHLine(0,scanY+stripOffset,viewportW,mix(p.accent,p.text,.35f));
  }
};
}  // namespace Splash
