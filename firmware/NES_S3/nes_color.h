#pragma once
/* NES ESP32-S3 color conversion. RGB565 words, not SPI byte-swapped. */
#include <stdint.h>

static inline uint16_t nesRgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3));
}

static inline uint16_t nesSwapRb565(uint16_t c) {
  return (uint16_t)(((c & 0x001F) << 11) | (c & 0x07E0) | ((c >> 11) & 0x001F));
}
