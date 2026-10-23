#pragma once
#include <stddef.h>
#include <stdint.h>
// esp_vfs file-backed RAM cache. ROM is loaded ONLY ONCE from SD.
// Nofrendo uses normal fopen/fread/fseek via /romcache/GAME.NES.
namespace HaloRomCache {
 bool install();
 bool load(const char* path, uint32_t* payloadHash, uint32_t* readMs, int* failureErr, uint32_t* readBytes);
 void release();
 size_t size();
 const char* path();
}
