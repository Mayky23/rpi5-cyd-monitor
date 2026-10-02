#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_partition.h>

#include "config.h"

/*
 * Configuración de red escrita desde el instalador web.
 *
 * El instalador guarda un bloque de 4 KB al principio de la partición de datos
 * "spiffs", que este firmware no utiliza para nada más:
 *
 *   0..7    "CYDCFG01"
 *   8..9    longitud del JSON (little endian)
 *   10..11  reservado (0)
 *   12..15  CRC-32 del JSON (little endian, el mismo que zlib.crc32)
 *   16..    JSON UTF-8: {"ssid","pass","api","token","name"}
 *
 * Si el bloque no existe o no es válido se usan los valores de config.h /
 * config.local.h, de modo que Install.ps1 sigue funcionando igual que antes.
 */
namespace RuntimeConfig {

struct Values {
  String ssid = WIFI_SSID;
  String password = WIFI_PASSWORD;
  String apiBase = API_BASE_URL;
  String token = API_TOKEN;
  String deviceName = DEVICE_NAME;
  bool fromFlash = false;
};

constexpr size_t SECTOR_SIZE = 4096;
constexpr size_t HEADER_SIZE = 16;
constexpr char MAGIC[8] = {'C', 'Y', 'D', 'C', 'F', 'G', '0', '1'};

inline uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

// Devuelve true y rellena `out` solo si el bloque es íntegro y sus valores son razonables.
inline bool parse(const uint8_t *sector, Values &out) {
  if (memcmp(sector, MAGIC, sizeof(MAGIC)) != 0) return false;
  const size_t length = sector[8] | (static_cast<size_t>(sector[9]) << 8);
  if (length == 0 || length > SECTOR_SIZE - HEADER_SIZE) return false;
  const uint32_t stored = sector[12] | (static_cast<uint32_t>(sector[13]) << 8) |
                          (static_cast<uint32_t>(sector[14]) << 16) | (static_cast<uint32_t>(sector[15]) << 24);
  if (crc32(sector + HEADER_SIZE, length) != stored) return false;

  JsonDocument json;
  if (deserializeJson(json, reinterpret_cast<const char *>(sector + HEADER_SIZE), length)) return false;
  String ssid = json["ssid"] | "";
  String password = json["pass"] | "";
  String api = json["api"] | "";
  String token = json["token"] | "";
  String name = json["name"] | "";
  while (api.endsWith("/")) api.remove(api.length() - 1);
  const bool validUrl = api.startsWith("http://") || api.startsWith("https://");
  if (ssid.length() < 1 || ssid.length() > 32 || password.length() > 64 || !validUrl ||
      token.length() < 1 || token.length() > 128 || name.length() > 32) {
    return false;
  }
  out.ssid = ssid;
  out.password = password;
  out.apiBase = api;
  out.token = token;
  if (name.length()) out.deviceName = name;
  out.fromFlash = true;
  return true;
}

inline Values load() {
  Values values;
  const esp_partition_t *partition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (!partition) return values;
  uint8_t *sector = static_cast<uint8_t *>(malloc(SECTOR_SIZE));
  if (!sector) return values;
  if (esp_partition_read(partition, 0, sector, SECTOR_SIZE) == ESP_OK) parse(sector, values);
  free(sector);
  return values;
}

}  // namespace RuntimeConfig
