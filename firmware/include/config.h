#pragma once

/*
 * Configuración pública y valores por defecto.
 *
 * Para usar credenciales sin publicarlas en GitHub:
 *   1. Copia config.local.example.h como config.local.h
 *   2. Edita config.local.h
 *
 * config.local.h está incluido en .gitignore.
 */
#if __has_include("config.local.h")
#include "config.local.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "TU_WIFI"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "TU_PASSWORD"
#endif
#ifndef API_BASE_URL
#define API_BASE_URL "http://rpi5.local:8787"
#endif
#ifndef API_TOKEN
#define API_TOKEN "PEGA_AQUI_EL_TOKEN"
#endif
#ifndef DEVICE_NAME
#define DEVICE_NAME "rpi-panel"
#endif

#ifndef PANEL_TITLE
#define PANEL_TITLE "RPI5"
#endif

// 1 corrige las CYD que muestran el tema oscuro como un negativo blanco/rojo.
#ifndef DISPLAY_INVERT_COLORS
#define DISPLAY_INVERT_COLORS 1
#endif

// 0 = vertical, 1 = horizontal, 2 = vertical invertida, 3 = horizontal invertida.
#ifndef DISPLAY_ROTATION
#define DISPLAY_ROTATION 1
#endif

#ifndef REQUEST_INTERVAL_MS
#define REQUEST_INTERVAL_MS 2500
#endif
#ifndef HTTP_TIMEOUT_MS
#define HTTP_TIMEOUT_MS 4000
#endif
#ifndef STALE_DATA_MS
#define STALE_DATA_MS 12000
#endif
#ifndef SCREEN_BRIGHTNESS
#define SCREEN_BRIGHTNESS 210
#endif
#ifndef AUTO_ROTATE_PAGE_MS
#define AUTO_ROTATE_PAGE_MS 0
#endif
#ifndef SPLASH_TIME_MS
#define SPLASH_TIME_MS 650
#endif

// Calibración típica del XPT2046 de la ESP32-2432S028R.
#ifndef TOUCH_MIN_X
#define TOUCH_MIN_X 200
#endif
#ifndef TOUCH_MAX_X
#define TOUCH_MAX_X 3900
#endif
#ifndef TOUCH_MIN_Y
#define TOUCH_MIN_Y 240
#endif
#ifndef TOUCH_MAX_Y
#define TOUCH_MAX_Y 3850
#endif
#ifndef TOUCH_DEBUG
#define TOUCH_DEBUG 0
#endif
