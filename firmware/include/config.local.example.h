#pragma once

// Copia este archivo como config.local.h. No publiques config.local.h.
#define WIFI_SSID "NOMBRE_DE_TU_WIFI"
#define WIFI_PASSWORD "CONTRASENA_DE_TU_WIFI"
#define API_BASE_URL "http://monitor.local:8787"
#define API_TOKEN "TOKEN_GENERADO_POR_EL_SERVIDOR"
// Solo con https://: huella SHA-256 del certificado de la API.
// #define API_CERT_SHA256 "64_CARACTERES_HEXADECIMALES"
// Hora de creación (segundos Unix); gana sobre un bloque del instalador web más antiguo.
// #define CONFIG_STAMP 0UL

// Personalización opcional:
#define DISPLAY_ROTATION 1
#define DISPLAY_INVERT_COLORS 1
#define SCREEN_BRIGHTNESS 210
#define AUTO_ROTATE_PAGE_MS 0
