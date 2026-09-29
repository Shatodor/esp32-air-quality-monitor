#ifndef ESP_CRC_H
#define ESP_CRC_H

#include <stdint.h>
#include <stddef.h>

uint32_t esp_crc32_le(uint32_t crc, const uint8_t *buf, size_t len);

#endif
