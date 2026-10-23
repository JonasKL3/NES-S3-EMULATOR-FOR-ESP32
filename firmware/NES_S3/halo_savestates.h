#pragma once
#include <stdint.h>
namespace HaloSave {
  void selectGame(const char* romPath, uint32_t romPayloadHash);
  bool enabled();
  void setEnabled(bool enabled);
  bool present(int slot);
  bool save(int slot);
  bool load(int slot);
  void resetAutoTimer();
  bool autoDue();
  int saveAuto();  // 1=gravado, 0=nao era hora, -1=falhou
  const char* gameKey();
}
