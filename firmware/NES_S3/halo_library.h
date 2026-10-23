#pragma once
#include <stdint.h>
namespace HaloLibrary {
  static constexpr unsigned RECENT_MAX=10;
  uint32_t gameId(const char *sdPath);
  bool load();
  bool save();
  bool isFavorite(uint32_t id);
  void toggleFavorite(uint32_t id);
  bool hasLast();
  uint32_t lastGame();
  unsigned recentCount();
  uint32_t recent(unsigned idx);
  void opened(uint32_t id);
  int volume();
  void setVolume(int percent);
}
