#pragma once
#include <stdint.h>
namespace HaloAudio {
  // GPIO dedicados: BCLK=15, LRC=16, DIN=17. Alterar apenas se livres.
  static constexpr int BCLK=15,LRC=16,DIN=17;
  static constexpr int RATE=22020, SAMPLES=367;
  bool begin();
  void end();
  void setEnabled(bool on);
  bool enabled();
  bool initialized();
  void setVolume(int percent);
  int volume();
  void setGenerator(void (*playfunc)(void*,int));
  void frame();
  void pause(bool paused);
  uint32_t framesProduced();
  uint32_t framesDropped();
}
