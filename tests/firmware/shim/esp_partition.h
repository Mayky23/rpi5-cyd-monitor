// Stub of the ESP-IDF partition API; the host tests never read real flash.
#pragma once

#include <cstddef>
#include <cstdint>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1

typedef enum { ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef enum { ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 0x82 } esp_partition_subtype_t;
typedef struct { uint32_t address; uint32_t size; } esp_partition_t;

inline const esp_partition_t *esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t, const char *) {
  return nullptr;
}
inline esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t) { return ESP_FAIL; }
