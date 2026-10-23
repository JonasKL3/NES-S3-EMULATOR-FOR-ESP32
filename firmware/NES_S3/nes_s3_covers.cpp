#include "nes_s3_covers.h"
#include <Arduino.h>
#include <SD.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace NesS3Covers {
static uint16_t *cachedPixels=nullptr;
static char cachedRom[245]={};
static bool cachedPresent=false;
static uint32_t requestCount=0;
static constexpr unsigned PIX_BYTES=WIDTH*HEIGHT*2;
static constexpr unsigned HEADER_BYTES=12;

static uint16_t readLE16(const uint8_t *p){return (uint16_t)p[0]|((uint16_t)p[1]<<8);}

bool begin(){
  if(!cachedPixels){
    cachedPixels=(uint16_t*)heap_caps_malloc(PIX_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!cachedPixels){
      // A capa e opcional; nunca derrubar a emulacao por falta de PSRAM.
      Serial.println("[R13][COVER][WARN] PSRAM insuficiente, mostrando placeholder");
      return false;
    }
  }
  cachedRom[0]=0;
  return true;
}
void invalidate(){cachedRom[0]=0;cachedPresent=false;}

static bool load(const char *romPath){
  char path[275];
  const char *filename=strrchr(romPath,'/');
  filename=filename?filename+1:romPath;
  size_t n=strlen(filename);
  if(n<4 || n>=240 || strcasecmp(filename+n-4,".nes")!=0)return false;
  int written=snprintf(path,sizeof(path),"/covers/%.*s.cov",(int)(n-4),filename);
  if(written<=0 || (size_t)written>=sizeof(path))return false;
  File cover=SD.open(path,FILE_READ);
  if(!cover){Serial.printf("[R13][COVER][MISS] %s\n",path);return false;}
  if(cover.size()!=HEADER_BYTES+PIX_BYTES){
    Serial.printf("[R13][COVER][INVALID] %s size=%lu\n",path,(unsigned long)cover.size());
    cover.close();return false;
  }
  uint8_t h[HEADER_BYTES];
  if(cover.read(h,HEADER_BYTES)!=HEADER_BYTES ||
     memcmp(h,"NS3C",4)!=0 || readLE16(h+4)!=WIDTH ||
     readLE16(h+6)!=HEIGHT || readLE16(h+8)!=1){
    Serial.printf("[R13][COVER][INVALID] cabecalho %s\n",path);
    cover.close();return false;
  }
  uint8_t *pixels=(uint8_t*)cachedPixels;
  unsigned done=0;
  while(done<PIX_BYTES){
    const unsigned chunk=PIX_BYTES-done>2048?2048:PIX_BYTES-done;
    const int nread=cover.read(pixels+done,chunk);
    if(nread<=0){
      Serial.printf("[R13][COVER][FAIL] lidos=%u de %u arquivo=%s\n",done,PIX_BYTES,path);
      cover.close();return false;
    }
    done+=(unsigned)nread;
  }
  cover.close();
  ++requestCount;
  Serial.printf("[R13][COVER][OK] %s bytes=%u loads=%lu\n",path,done,(unsigned long)requestCount);
  return true;
}

bool draw(TFT_eSPI &tft,const char *romPath,int x,int y){
  if(!romPath||!romPath[0])return false;
  if(!cachedPixels && !begin())return false;
  if(strcmp(romPath,cachedRom)!=0){
    // Evita o uso de ponteiros de File do SD apos fechar (SPI compartilhado).
    cachedPresent=load(romPath);
    snprintf(cachedRom,sizeof(cachedRom),"%s",romPath);
  }
  if(!cachedPresent)return false;
  // tft.setSwapBytes(true) foi configurado no pipeline R8, que ja usa RGB565.
  tft.pushImage(x,y,WIDTH,HEIGHT,cachedPixels);
  return true;
}

void drawMissing(TFT_eSPI &tft,int x,int y){
  tft.fillRoundRect(x,y,WIDTH,HEIGHT,5,0x000B);
  tft.drawRoundRect(x+5,y+5,WIDTH-10,HEIGHT-10,4,0x5AEB);
  tft.setTextSize(2);tft.setTextColor(0x03FF,0x000B);
  tft.setCursor(x+9,y+42);tft.print("NES-S3");
  tft.setTextSize(1);tft.setTextColor(TFT_LIGHTGREY,0x000B);
  tft.setCursor(x+17,y+83);tft.print("SEM CAPA");
}
}
