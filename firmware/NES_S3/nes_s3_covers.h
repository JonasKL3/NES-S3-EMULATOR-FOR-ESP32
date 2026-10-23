#pragma once
#include <stdint.h>
#include <TFT_eSPI.h>
namespace NesS3Covers {
  static constexpr uint16_t WIDTH=104;
  static constexpr uint16_t HEIGHT=136;
  // Arquivos /covers/ROM_NAME.cov, cabecalho NS3C v1 + RGB565 little endian.
  bool begin();
  bool draw(TFT_eSPI &tft,const char *romPath,int x,int y);
  void drawMissing(TFT_eSPI &tft,int x,int y);
  void invalidate();
}
