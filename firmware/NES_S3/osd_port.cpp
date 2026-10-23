/* Nofrendo OSD adapter - developed for NES ESP32-S3 v0.2.0-alpha.1.
 * Uses Nofrendo 1.2 external library, TFT_eSPI, PSRAM, Dabble state.
 * Does NOT implement console emulation: the installed Nofrendo library does.
 */
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nes_color.h"
#include "halo_fixed_palette.h"
#include "halo_savestates.h"
#include "halo_library.h"
#include "halo_audio.h"

extern "C" {
#include <noftypes.h>
#include <bitmap.h>
#include <vid_drv.h>
#include <osd.h>
#include <nofrendo.h>
#include <event.h>
#include <nofconfig.h>
#include <nes/nes.h>
#include <nes/nesstate.h>
#include <nes/nesinput.h>
#include <nes/nes_pal.h>
#include <log.h>
}
// Optional patch in Nofrendo nes_ppu.c; weak import avoids a build dependency.
extern "C" uint8_t halo_nofrendo_ppumask(void) __attribute__((weak));
extern TFT_eSPI tft;
extern volatile uint16_t gamepadMask;
extern volatile bool exitRequested;
extern volatile bool pauseMenuRequested;
extern volatile bool pauseMenuActive;
extern volatile uint16_t pauseMenuButtons;
extern volatile bool pauseWaitForRelease;
extern volatile uint32_t pauseResumeEpoch;
extern volatile uint32_t producedFrames;
extern volatile uint32_t emulatorYields;
extern volatile uint32_t emulatorStackMinFree;
extern volatile uint32_t videoTransferLastUs;
extern volatile uint32_t videoTransferMaxUs;
extern volatile bool nesSwapRedBlue;
extern volatile bool nesUseFixedPalette;
extern volatile uint8_t nesLcdAction,nesLcdPreset,nesLcdInversion,nesAspect43;
extern bool launchResumeAuto;
// Nofrendo (C) consulta este simbolo em nes.c/nofrendo.c apos patch.
extern "C" int nes_platform_exit_requested(void){
 return __atomic_load_n(&exitRequested,__ATOMIC_ACQUIRE)?1:0;
}
// Chamado pelo patch C v0.2.9.4: nao manter CPU0 ocupada por segundos
// durante fread(512KiB) a 1 MHz. IDLE0 precisa rodar para alimentar WDT.
extern "C" void nes_platform_rom_io_yield(void){
 vTaskDelay(1);
}
static uint8_t *nesFramebuffer=nullptr;
static uint16_t *rgbStripe=nullptr;
static uint16_t rgbPalette[256]={};
static uint16_t aspectMapX[320]={};
static uint32_t videoConvertLastUs=0,videoSpiLastUs=0;
static bool videoPaletteReady=false;
static uint16_t guiRgb565[64]={};
static bool splashUntilGameplay=true;
static uint32_t splashSince=0;
static void videoRebuildPalette(){
 if(!videoPaletteReady)return;
 const bool swapRB=__atomic_load_n(&nesSwapRedBlue,__ATOMIC_ACQUIRE);
 for(int i=0;i<256;i++){
   uint16_t rgb=(i<192)?haloFixedRgb565[i&63]:guiRgb565[i-192];
   rgbPalette[i]=swapRB?nesSwapRb565(rgb):rgb;
 }
 Serial.printf("[R9.1][PALETTE][LOCKED] R8_EXATA sky31=%04X orange17=%04X orange27=%04X fruit37=%04X gui192=%04X\n",
   rgbPalette[0x31],rgbPalette[0x17],rgbPalette[0x27],rgbPalette[0x37],rgbPalette[192]);
}

// Sequencias derivadas dos arquivos ILI9341_Init.h do Bodmer/TFT_eSPI.
// APENAS alteram registros de cor/tensao; NAO reinicializam SPI ou MADCTL.
static const uint8_t gammaPosOriginal[15]={0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,0x37,0x07,0x10,0x03,0x0E,0x09,0x00};
static const uint8_t gammaNegOriginal[15]={0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F};
static const uint8_t gammaPosAlt[15]={0x0F,0x2A,0x28,0x08,0x0E,0x08,0x54,0xA9,0x43,0x0A,0x0F,0x00,0x00,0x00,0x00};
static const uint8_t gammaNegAlt[15]={0x00,0x15,0x17,0x07,0x11,0x06,0x2B,0x56,0x3C,0x05,0x10,0x0F,0x3F,0x3F,0x0F};
// Candidata ILI9341V IPS - outro modulo; teste experimental, nao calibracao universal.
static const uint8_t gammaPosIps[15]={0x0F,0x35,0x31,0x0B,0x0E,0x06,0x49,0xA7,0x33,0x07,0x0F,0x03,0x0C,0x0A,0x00};
static const uint8_t gammaNegIps[15]={0x00,0x0A,0x0F,0x04,0x11,0x08,0x36,0x58,0x4D,0x07,0x10,0x0C,0x32,0x34,0x0F};
static uint8_t lcdPresetApplied=0;
static bool lcdInversionApplied=false;
static bool aspectApplied=false;
static void lcdWriteBytes(uint8_t reg,const uint8_t *bytes,uint8_t count){
 tft.writecommand(reg);
 for(uint8_t i=0;i<count;i++)tft.writedata(bytes[i]);
}
static void lcdApplyPreset(uint8_t id){
 // Preset 0: valores defaults do ILI9341_DRIVER oficial, nao reinicia o painel.
 const uint8_t pw1=(id==2)?0x10:0x23;
 const uint8_t pw2=(id==2)?0x00:0x10;
 const uint8_t vm1[2]={(uint8_t)((id==2)?0x30:0x3E),(uint8_t)((id==2)?0x30:0x28)};
 const uint8_t vm2=(id==2)?0xB7:0x86;
 lcdWriteBytes(0xC0,&pw1,1); lcdWriteBytes(0xC1,&pw2,1);
 lcdWriteBytes(0xC5,vm1,2); lcdWriteBytes(0xC7,&vm2,1);
 const uint8_t gammaMode=1; lcdWriteBytes(0x26,&gammaMode,1);
 const uint8_t *pos=(id==1||id==2)?gammaPosAlt:(id==3)?gammaPosIps:gammaPosOriginal;
 const uint8_t *neg=(id==1||id==2)?gammaNegAlt:(id==3)?gammaNegIps:gammaNegOriginal;
 lcdWriteBytes(0xE0,pos,15);lcdWriteBytes(0xE1,neg,15);
 lcdPresetApplied=id;
 Serial.printf("[LCD][PROFILE] id=%u name=%s VCOM=0x%02X INV=%u ASPECT=%s\n",
   (unsigned)id,(id==0)?"ORIGINAL":(id==1)?"GAMMA_ALT":(id==2)?"ALT_PWR_GAMMA":"IPS_GAMMA",
   (unsigned)vm2,(unsigned)lcdInversionApplied,aspectApplied?"4:3":"NATIVE");
}
static void lcdRestore(){
 if(lcdPresetApplied!=0)lcdApplyPreset(0);
 if(lcdInversionApplied){tft.invertDisplay(false);lcdInversionApplied=false;}
 if(aspectApplied){aspectApplied=false;tft.fillScreen(TFT_BLACK);}
 __atomic_store_n(&nesLcdPreset,(uint8_t)0,__ATOMIC_RELEASE);
 __atomic_store_n(&nesLcdInversion,(uint8_t)0,__ATOMIC_RELEASE);
 __atomic_store_n(&nesAspect43,(uint8_t)0,__ATOMIC_RELEASE);
 Serial.println("[LCD][RESTORE] ORIGINAL: gamma+VCOM padrao, INV=0, NATIVE 256x240");
}
static void lcdHandleAction(){
 const uint8_t action=__atomic_exchange_n(&nesLcdAction,(uint8_t)0,__ATOMIC_ACQ_REL);
 if(!action)return;
 switch(action){
 case 1: {
   const uint8_t next=(uint8_t)((lcdPresetApplied+1)%4);
   lcdApplyPreset(next);
   __atomic_store_n(&nesLcdPreset,next,__ATOMIC_RELEASE);
   break;
 }
 case 2:
   lcdInversionApplied=!lcdInversionApplied;
   tft.invertDisplay(lcdInversionApplied);
   __atomic_store_n(&nesLcdInversion,(uint8_t)lcdInversionApplied,__ATOMIC_RELEASE);
   Serial.printf("[LCD][INVERSION] enabled=%u\n",(unsigned)lcdInversionApplied);
   break;
 case 3:
   aspectApplied=!aspectApplied;
   __atomic_store_n(&nesAspect43,(uint8_t)aspectApplied,__ATOMIC_RELEASE);
   tft.fillScreen(TFT_BLACK);
   Serial.printf("[VIDEO][ASPECT] mode=%s width=%u height=240\n",aspectApplied?"4:3":"NATIVE",(unsigned)(aspectApplied?320:256));
   break;
 case 4: lcdRestore();break;
 default:break;
 }
}

static bitmap_t *lockedBitmap=nullptr;
static uint16_t previousPadMask=0; // Reset a cada ROM; evita botoes presos entre jogos
static esp_timer_handle_t tickTimer=nullptr;
static uint64_t nesTimerPeriodUs=16666;
static bool nesTimerPaused=false;
static volatile uint32_t mapperVerboseSuppressed=0;
extern volatile uint32_t nesExpectedPayloadHash;
extern volatile bool nesExpectedPayloadReady;
extern volatile uint32_t nesPayloadValidationCalls;
volatile uint32_t videoSampleNonzeroPercent=0;
volatile uint32_t videoSampleUniqueColors=0;
volatile uint32_t videoSampleHash=0;
volatile uint32_t videoSamplesMade=0;

// Nofrendo C chama apos carregar PRG e CHR. Compara bytes REAIS da PSRAM
// com a leitura completa anterior do SD, detectando corrupcao silenciosa.
extern "C" int nes_platform_validate_rom_payload(const uint8_t *prg,unsigned prgSize,
                                                  const uint8_t *chr,unsigned chrSize){
 const uint32_t expected=__atomic_load_n(&nesExpectedPayloadHash,__ATOMIC_ACQUIRE);
 const bool ready=__atomic_load_n(&nesExpectedPayloadReady,__ATOMIC_ACQUIRE);
 uint32_t hash=2166136261UL;
 if((prgSize && !prg) || (chrSize && !chr) || !ready){
  Serial.println("[ROM][BANKS][FAIL] payload nao disponivel");return -1;
 }
 for(unsigned i=0;i<prgSize;i++){
  hash=(hash^prg[i])*16777619UL;
  if((i&0x3FFFu)==0x3FFFu)nes_platform_rom_io_yield();
 }
 for(unsigned i=0;i<chrSize;i++){
  hash=(hash^chr[i])*16777619UL;
  if((i&0x3FFFu)==0x3FFFu)nes_platform_rom_io_yield();
 }
 __atomic_add_fetch(&nesPayloadValidationCalls,1,__ATOMIC_RELEASE);
 Serial.printf("[ROM][BANKS][%s] expected=0x%08lX loaded=0x%08lX PRG=%u CHR=%u\n",
    hash==expected?"OK":"MISMATCH",(unsigned long)expected,(unsigned long)hash,prgSize,chrSize);
 return hash==expected?0:-1;
}



extern "C" void *mem_alloc(int size,bool prefer_fast_memory){
 if(size<=0)return nullptr;
 void *ptr=nullptr;
 if(prefer_fast_memory)ptr=heap_caps_malloc(size,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
 else if(psramFound())ptr=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!ptr)ptr=malloc(size);
 return ptr;
}

static int videoInit(int width,int height){
 Serial.printf("[NOF][VID] init %dx%d\n",width,height);
 if(width!=256 || height!=240)return -1; // Nofrendo NES fixo: evita OOB no blit
 if(!nesFramebuffer)nesFramebuffer=(uint8_t *)heap_caps_malloc(256*240,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!rgbStripe)rgbStripe=(uint16_t *)heap_caps_malloc(320*8*sizeof(uint16_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
 if(!rgbStripe)rgbStripe=(uint16_t *)heap_caps_malloc(320*8*sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 Serial.printf("[NOF][VID] framebuffer=%p stripe=%p\n",nesFramebuffer,rgbStripe);
 if(!nesFramebuffer || !rgbStripe){
   if(nesFramebuffer){free(nesFramebuffer);nesFramebuffer=nullptr;}
   if(rgbStripe){free(rgbStripe);rgbStripe=nullptr;}
   return -1;
 }
 __atomic_store_n(&nesLcdAction,(uint8_t)0,__ATOMIC_RELEASE);
 lcdPresetApplied=0;lcdInversionApplied=false;aspectApplied=false;
 videoPaletteReady=false;
 for(int x=0;x<320;x++)aspectMapX[x]=(uint16_t)((x*4)/5);
 __atomic_store_n(&nesLcdPreset,(uint8_t)0,__ATOMIC_RELEASE);
 __atomic_store_n(&nesLcdInversion,(uint8_t)0,__ATOMIC_RELEASE);
 __atomic_store_n(&nesAspect43,(uint8_t)0,__ATOMIC_RELEASE);
 // Splash vetorial permanece enquanto o core carrega e inicia a ROM.
 splashUntilGameplay=true;splashSince=millis();
 return 0;
}
static void videoShutdown(){
 if(lcdPresetApplied||lcdInversionApplied||aspectApplied)lcdRestore();
 __atomic_store_n(&nesLcdAction,(uint8_t)0,__ATOMIC_RELEASE);
 if(lockedBitmap)bmp_destroy(&lockedBitmap);
 if(nesFramebuffer){free(nesFramebuffer);nesFramebuffer=nullptr;}
 if(rgbStripe){free(rgbStripe);rgbStripe=nullptr;}
 videoPaletteReady=false;
}
static int videoSetMode(int width,int height){(void)width;(void)height;return 0;}
static void videoPalette(rgb_t *palette){
 if(!palette)return;
 for(int i=192;i<256;i++){
   const rgb_t &c=palette[i];
   guiRgb565[i-192]=nesRgb565((uint8_t)c.r,(uint8_t)c.g,(uint8_t)c.b);
 }
 videoPaletteReady=true;
 videoRebuildPalette();
}
static void videoClear(uint8_t color){if(nesFramebuffer)memset(nesFramebuffer,color,256*240);}
static bitmap_t *videoLock(){
 if(!nesFramebuffer)return nullptr;
 if(!lockedBitmap)lockedBitmap=bmp_createhw(nesFramebuffer,256,240,256);
 return lockedBitmap;
}
// A biblioteca Nofrendo conserva o bitmap (screen) depois de free_write:
// liberar a estrutura aqui causaria use-after-free. Liberacao so em videoShutdown.
static void videoUnlock(int num_dirties,rect_t *rects){(void)num_dirties;(void)rects;}
static void videoBlit(bitmap_t *bmp,int num_dirties,rect_t *rects){
 (void)num_dirties;(void)rects;
 if(!bmp||!rgbStripe)return;
 // No boot, nao expor quadros vazios nem mensagens de carregamento.
 // Fallback apos 4s permite jogos que iniciem com telas monocromaticas.
 if(splashUntilGameplay){
   uint8_t found[256]={};unsigned distinct=0;
   for(int yy=24;yy<224;yy+=24)for(int xx=24;xx<240;xx+=24){
      const unsigned id=bmp->line[yy][xx];if(!found[id]){found[id]=1;++distinct;}
   }
   if(distinct<3 && (uint32_t)(millis()-splashSince)<4000u)return;
   splashUntilGameplay=false;
   tft.fillScreen(TFT_BLACK);
   Serial.printf("[R12][SPLASH][END] coresDistintas=%u delayMs=%lu\n",distinct,(unsigned long)(millis()-splashSince));
 }
 // Configuracoes aplicadas apenas no boundary de frame da tarefa NES.
 lcdHandleAction();
 // Sample *raw* NES indices and converted pixels at sky-like test coordinates.
 // Labels are coordinates, NOT scene segmentation: useful for same stage A/B.
 const uint32_t frameNum=__atomic_load_n(&producedFrames,__ATOMIC_RELAXED);
 if(frameNum<3 || frameNum%90==0){
   const uint8_t x0=128,x1=64;
   const uint8_t idx0=bmp->line[50][x0],idx1=bmp->line[95][x1];
   Serial.printf("[ANEMOIA][PIX] frame=%lu idx(128,50)=%02X NES=%02X rgb565=%04X idx(64,95)=%02X NES=%02X rgb565=%04X pal=%s mask=%02X hook=%u bank=%u gray=%u\n",
     (unsigned long)frameNum,(unsigned)idx0,(unsigned)(idx0&63),(unsigned)rgbPalette[idx0],
     (unsigned)idx1,(unsigned)(idx1&63),(unsigned)rgbPalette[idx1],"ANEMOIA CEU AZUL (R8)",
     0u,0u,0u,0u);
   // A coarse 11x5 indexed-color histogram in an upper-screen rectangle.
   // It is intentionally content-agnostic; do not interpret as sky without checking scene.
   uint16_t counts[64]={};
   for(int y=40;y<=104;y+=16)for(int x=48;x<=208;x+=16)
     counts[bmp->line[y][x]&63]++;
   uint8_t top[3]={0xFF,0xFF,0xFF};
   for(int idx=0;idx<64;idx++){
     for(int j=0;j<3;j++){
       if(top[j]==0xFF || counts[idx]>counts[top[j]]){
         for(int k=2;k>j;k--)top[k]=top[k-1];
         top[j]=(uint8_t)idx;
         break;
       }
     }
   }
   Serial.printf("[ANEMOIA][REGION] frame=%lu upperRGB colors: %02X=%u, %02X=%u, %02X=%u pal=%s\n",
      (unsigned long)frameNum,(unsigned)top[0],(unsigned)counts[top[0]],
      (unsigned)top[1],(unsigned)counts[top[1]],
      (unsigned)top[2],(unsigned)counts[top[2]],"ANEMOIA CEU AZUL (R8)");
 }

 // Dados validos durante a chamada; nenhuma fila com ponteiros liberados.
 // 8 linhas por transacao para reduzir overhead do SPI.
 // Mede apenas o envio de quadro ao TFT: ajuda a identificar atraso/tearing.
 // Captura leve da imagem de entrada periodicamente, sem mexer nos pixels.
 // Unicos entre amostras espacadas mostram se o NES esta gerando somente
 // a cor de fundo ou uma cena real. Nao se deve tratar paleta cinza como erro.
 const uint32_t count=__atomic_load_n(&producedFrames,__ATOMIC_RELAXED);
 if(count<4 || (count%64)==0){
   uint8_t seen[256]={0};
   uint32_t nonZero=0, unique=0, total=0, hash=2166136261UL;
   for(int y=0;y<240;y+=8){
     const uint8_t *line=bmp->line[y];
     if(!line)break;
     for(int x=0;x<256;x+=8){
       const uint8_t pix=line[x];
       if(pix)nonZero++;
       if(!seen[pix]){seen[pix]=1;unique++;}
       hash=(hash^pix)*16777619UL;
       total++;
     }
   }
   if(total){
    __atomic_store_n(&videoSampleNonzeroPercent,100U*nonZero/total,__ATOMIC_RELAXED);
    __atomic_store_n(&videoSampleUniqueColors,unique,__ATOMIC_RELAXED);
    __atomic_store_n(&videoSampleHash,hash,__ATOMIC_RELAXED);
    __atomic_add_fetch(&videoSamplesMade,1,__ATOMIC_RELAXED);
   }
 }
 const uint32_t blitStartedUs=(uint32_t)esp_timer_get_time();
 uint32_t convertUs=0,spiUs=0;
 tft.startWrite();
 const int outW=aspectApplied?320:256;
 const int outX=aspectApplied?0:32;
 for(int y=0;y<240;y+=8){
   const uint32_t convertStart=(uint32_t)esp_timer_get_time();
   for(int dy=0;dy<8;dy++){
     const uint8_t *src=bmp->line[y+dy];
     uint16_t *dst=rgbStripe+dy*outW;
     if(aspectApplied){
       // Mapeamento nearest-neighbor deterministico: 256 -> 320, sem trocar paleta.
       for(int x=0;x<320;x++)dst[x]=rgbPalette[src[aspectMapX[x]]];
     }else{
       for(int x=0;x<256;x++)dst[x]=rgbPalette[src[x]];
     }
   }
   convertUs+=(uint32_t)esp_timer_get_time()-convertStart;
   const uint32_t sendStart=(uint32_t)esp_timer_get_time();
   tft.pushImage(outX,y,outW,8,rgbStripe);
   spiUs+=(uint32_t)esp_timer_get_time()-sendStart;
 }
 tft.endWrite();
 videoConvertLastUs=convertUs;videoSpiLastUs=spiUs;
 const uint32_t elapsedUs=(uint32_t)esp_timer_get_time()-blitStartedUs;
 __atomic_store_n(&videoTransferLastUs,elapsedUs,__ATOMIC_RELAXED);
 uint32_t maxSeen=__atomic_load_n(&videoTransferMaxUs,__ATOMIC_RELAXED);
 while(elapsedUs>maxSeen && !__atomic_compare_exchange_n(&videoTransferMaxUs,&maxSeen,elapsedUs,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
 const uint32_t totalFrames=__atomic_add_fetch(&producedFrames,1,__ATOMIC_RELAXED);
 if((totalFrames%128u)==0u)Serial.printf("[VIDEO][STAGES] frame=%lu aspect=%s pal=%s convUs=%lu pushUs=%lu totalUs=%lu\n",(unsigned long)totalFrames,aspectApplied?"4:3":"NATIVE","ANEMOIA CEU AZUL (R8)",(unsigned long)videoConvertLastUs,(unsigned long)videoSpiLastUs,(unsigned long)elapsedUs);
 // Esta pausa permite que a tarefa IDLE0 e o BLE executem mesmo ao renderizar
 // intensamente; vTaskDelay(1) realmente bloqueia por 1 tick (taskYIELD nao).
 vTaskDelay(1);
 __atomic_add_fetch(&emulatorYields,1,__ATOMIC_RELAXED);
}
static viddriver_t screenDriver={
 "ILI9341-S3",videoInit,videoShutdown,videoSetMode,videoPalette,videoClear,
 videoLock,videoUnlock,videoBlit,false
};

extern "C" void osd_getvideoinfo(vidinfo_t *info){
 info->default_width=256;
 info->default_height=240;
 info->driver=&screenDriver;
}
// DEBUG de certos mappers gera milhares de "wrote $..." por segundo.
// Serial bloqueante nessa taxa causa quedas de FPS e muda o timing do NES.
// Preserva ASSERT, avisos e inicializacao; omite apenas traços repetitivos.
static int serialLog(const char *s){
 if(!s)return 0;
 if(strncmp(s,"wrote $",7)==0 || strncmp(s,"read $",6)==0){
   __atomic_add_fetch(&mapperVerboseSuppressed,1,__ATOMIC_RELAXED);
   return 0;
 }
 Serial.print(s);
 return 0;
}
extern "C" int osd_init(void){
 previousPadMask=0;
 __atomic_store_n(&mapperVerboseSuppressed,0,__ATOMIC_RELAXED);
 nofrendo_log_chain_logfunc(serialLog);
 Serial.println("[NOF][OSD] init");
 return 0;
}
extern "C" void osd_shutdown(void){
 if(tickTimer){esp_timer_stop(tickTimer);esp_timer_delete(tickTimer);tickTimer=nullptr;}
 previousPadMask=0;
 Serial.printf("[NOF][DEBUG] linhas repetitivas suprimidas=%lu\n",(unsigned long)__atomic_load_n(&mapperVerboseSuppressed,__ATOMIC_RELAXED));
 HaloAudio::end();
 Serial.println("[NOF][OSD] shutdown");
}
extern "C" int osd_main(int argc,char *argv[]){
 if(argc<1||!argv||!argv[0])return -1;
 static char configName[]="/sd/nofrendo.cfg";
 config.filename=configName;
 Serial.printf("[NOF] main_loop(%s)\n",argv[0]);
 return main_loop(argv[0],system_autodetect);
}
static void tickTimerCB(void *arg){(void)arg;nofrendo_ticks++;}
extern "C" int osd_installtimer(int frequency,void *func,int funcsize,void *counter,int countersize){
 (void)func;(void)funcsize;(void)counter;(void)countersize;
 if(frequency<=0)return -1;
 nesTimerPeriodUs=1000000ULL/(uint32_t)frequency;
 nesTimerPaused=false;
 if(tickTimer){esp_timer_stop(tickTimer);esp_timer_delete(tickTimer);tickTimer=nullptr;}
 esp_timer_create_args_t args={};
 args.callback=tickTimerCB;args.arg=nullptr;args.dispatch_method=ESP_TIMER_TASK;args.name="nes60hz";
 if(esp_timer_create(&args,&tickTimer)!=ESP_OK)return -1;
 if(esp_timer_start_periodic(tickTimer,nesTimerPeriodUs)!=ESP_OK){
   esp_timer_delete(tickTimer);tickTimer=nullptr;return -1;
 }
 Serial.printf("[NOF][TIMER] %d Hz\n",frequency);
 return 0;
}
// Pausa cooperativa: toda a interface roda na propria tarefa NES.
// Enquanto ela aguarda, a CPU nao executa instrucoes do jogo e o BLE
// continua no loop() do Arduino, sem disputa pelo barramento SPI/TFT.
static void pauseReleaseNesButtons(){
 const int ids[8]={event_joypad1_a,event_joypad1_b,event_joypad1_select,event_joypad1_start,
    event_joypad1_up,event_joypad1_down,event_joypad1_left,event_joypad1_right};
 for(int b=0;b<8;b++){
   const uint16_t bit=(uint16_t)(1U<<b);
   if(previousPadMask&bit){event_t h=event_get(ids[b]);if(h)h(INP_STATE_BREAK);}
 }
 previousPadMask=0;
 __atomic_store_n(&gamepadMask,(uint16_t)0,__ATOMIC_RELEASE);
}
static const uint16_t MENU_PAUSE_BG=0x0000;
static const uint16_t MENU_PAUSE_PANEL=0x1082;
static const uint16_t MENU_PAUSE_SELECTED=0x0399;
static const uint8_t VIDEO_MENU_TOTAL=5;
static constexpr uint8_t SAVE_SLOT_COUNT=5;
static constexpr uint8_t SAVE_SLOT_IDS[SAVE_SLOT_COUNT]={0,1,2,3,9};
static uint8_t selectedSlot=0;
static constexpr uint8_t PAUSE_MENU_COUNT=8;
static void pauseStateStatus(const char* action,int slot,bool result){
 tft.fillRect(0,201,320,23,TFT_BLACK);
 tft.setTextSize(1);tft.setTextColor(result?TFT_GREEN:TFT_RED,TFT_BLACK);
 char slabel[8];snprintf(slabel,sizeof(slabel),"%d",slot);
 tft.setCursor(6,207);tft.printf("%s SLOT %s: %s",action,slot==9?"AUTO":slabel,result?"OK":"FALHOU, VER SERIAL");
 Serial.printf("[R11][UI] %s slot=%d %s\n",action,slot,result?"OK":"FAIL");
 delay(900);
}
static void pauseDrawMenu(bool settings,uint8_t selection){
 tft.fillScreen(MENU_PAUSE_BG);
 tft.fillRect(0,0,320,32,TFT_NAVY);
 tft.setTextSize(2);tft.setTextColor(TFT_CYAN,TFT_NAVY);
 tft.setCursor(settings?45:65,8);tft.print(settings?"VIDEO / AUDIO":"JOGO / SAVE");
 if(!settings){
   const char* labels[]={"CONTINUAR","SALVAR ESTADO","CARREGAR ESTADO","SLOT","AUTO SAVE","REINICIAR JOGO","CONFIGURACOES","SAIR DO JOGO"};
   for(uint8_t n=0;n<PAUSE_MENU_COUNT;n++){
     int y=35+21*n;
     const bool selected=(n==selection);
     const uint16_t bg=selected?MENU_PAUSE_SELECTED:MENU_PAUSE_PANEL;
     tft.fillRect(5,y,310,20,bg);
     tft.setTextColor(selected?TFT_YELLOW:TFT_WHITE,bg);tft.setTextSize(1);
     tft.setCursor(10,y+5);
     if(n==3){
       const int slot=SAVE_SLOT_IDS[selectedSlot];
       char slabel[8];snprintf(slabel,sizeof(slabel),"%d",slot);
       tft.printf("%c SLOT: %s [%s]",selected?'>':' ',slot==9?"AUTO":slabel,HaloSave::present(slot)?"SALVO":"VAZIO");
     }else if(n==4){
       tft.printf("%c AUTO SAVE: %s (120s)",selected?'>':' ',HaloSave::enabled()?"ON":"OFF");
     }else tft.printf("%c %s",selected?'>':' ',labels[n]);
   }
   tft.setTextSize(1);tft.setTextColor(TFT_LIGHTGREY,MENU_PAUSE_BG);
   tft.setCursor(5,208);tft.print("D-PAD: navegar / slot ESQ-DIR");
   tft.setCursor(5,222);tft.print("X: confirmar   B: continuar");
   return;
 }
 for(uint8_t n=0;n<VIDEO_MENU_TOTAL;n++){
   int y=43+31*n;
   const bool selected=n==selection;
   uint16_t bg=selected?MENU_PAUSE_SELECTED:MENU_PAUSE_PANEL;
   tft.fillRect(8,y,304,29,bg);tft.setTextSize(1);
   tft.setTextColor(selected?TFT_YELLOW:TFT_WHITE,bg);
   tft.setCursor(15,y+10);
   char audioLine[48];
   snprintf(audioLine,sizeof(audioLine),"VOLUME I2S: %d%%",HaloAudio::volume());
   const char* label=(n==0)?(aspectApplied?"PROPORCAO: 4:3":"PROPORCAO: NATIVA"):
                     (n==1)?(!HaloAudio::initialized()?"AUDIO I2S: FALHOU":HaloAudio::enabled()?"AUDIO MAX98357A: ON":"AUDIO MAX98357A: OFF"):
                     (n==2)?audioLine:(n==3)?"INSPECIONAR PIXEL":"VOLTAR";
   tft.printf("%c %s",selected?'>':' ',label);
 }
 tft.setTextSize(1);tft.setTextColor(TFT_LIGHTGREY,MENU_PAUSE_BG);
 tft.setCursor(12,222);tft.print("X alterar | ESQ/DIR volume/aspecto");
}
static void pauseInspectPixel(){
 if(!lockedBitmap || !rgbStripe){Serial.println("[R9][PICK][FAIL] sem framebuffer");return;}
 int x=128,y=104;
 uint16_t prior=__atomic_load_n(&pauseMenuButtons,__ATOMIC_ACQUIRE);
 Serial.println("[R9][PICK][START] D-pad mover; X capturar indice; B voltar");
 bool redraw=true;
 for(;;){
   uint16_t now=__atomic_load_n(&pauseMenuButtons,__ATOMIC_ACQUIRE);
   uint16_t press=(uint16_t)(now&~prior);prior=now;
   if(press&0x02)break;
   if(press&0x10){y=(y>4)?y-4:0;redraw=true;}
   if(press&0x20){y=(y<235)?y+4:239;redraw=true;}
   if(press&0x40){x=(x>4)?x-4:0;redraw=true;}
   if(press&0x80){x=(x<251)?x+4:255;redraw=true;}
   if(press&0x01){
     const uint8_t raw=lockedBitmap->line[y][x];
     const uint8_t id=(uint8_t)(raw&63);
     Serial.printf("[R9][PICK] x=%d y=%d raw=%02X NES=%02X RGB565=%04X PRIOR=%s\n",
       x,y,(unsigned)raw,(unsigned)id,(unsigned)rgbPalette[raw],
       (id==0x31)?"CEU31":"NES");
     redraw=true;
   }
   if(redraw){
     // O preview nao modifica o framebuffer; somente o LCD e a cruz.
     tft.fillScreen(TFT_BLACK);
     videoBlit(lockedBitmap,0,nullptr);
     int px=aspectApplied?((x*5)/4):(x+32);
     tft.drawFastHLine(px-5<0?0:px-5,y,11,TFT_YELLOW);
     tft.drawFastVLine(px,y-5<0?0:y-5,11,TFT_YELLOW);
     tft.fillRect(0,218,320,22,TFT_BLACK);
     tft.setTextSize(1);tft.setTextColor(TFT_YELLOW,TFT_BLACK);
     tft.setCursor(5,223);
     const uint8_t raw=lockedBitmap->line[y][x];
     tft.printf("X:%03d Y:%03d NES:$%02X RGB:%04X X=LOG B=SAIR",x,y,(unsigned)(raw&63),rgbPalette[raw]);
     redraw=false;
   }
   vTaskDelay(20);
 }
 Serial.println("[R9][PICK][END]");
}
static void pauseRunMenu(){
 HaloAudio::pause(true);
 __atomic_store_n(&pauseMenuRequested,false,__ATOMIC_RELEASE);
 __atomic_store_n(&pauseMenuActive,true,__ATOMIC_RELEASE);
 pauseReleaseNesButtons();
 if(tickTimer && !nesTimerPaused){
   if(esp_timer_stop(tickTimer)==ESP_OK)nesTimerPaused=true;
   else Serial.println("[PAUSE][WARN] falhou interromper timer");
 }
 bool settings=false;
 bool fullscreenPreview=false;
 uint8_t selection=0;
 uint16_t lastButtons=__atomic_load_n(&pauseMenuButtons,__ATOMIC_ACQUIRE);
 pauseDrawMenu(settings,selection);
 Serial.println("[PAUSE][OPEN] R11 Save/Load slots 0-3, AUTO 9; paleta R8 fixa");
 bool quitGame=false;
  bool resumeGame=false;
  bool restartGame=false;
 bool loadedState=false;
  while(!quitGame && !resumeGame && !restartGame && !__atomic_load_n(&exitRequested,__ATOMIC_ACQUIRE)){
   const uint16_t buttons=__atomic_load_n(&pauseMenuButtons,__ATOMIC_ACQUIRE);
   const uint16_t pressed=(uint16_t)(buttons&~lastButtons);
   lastButtons=buttons;
   bool redraw=false;
   if(fullscreenPreview){
     if(pressed&(0x01|0x02|0x08)){
       fullscreenPreview=false;
       redraw=true;
       Serial.println("[PAUSE][PREVIEW] voltar aos ajustes");
     }
   }else if(settings && (pressed&0x08)){
     // Preview do ultimo frame sem executar CPU/PPU do NES ou timer.
     fullscreenPreview=true;
     tft.fillScreen(TFT_BLACK);
     if(lockedBitmap && rgbStripe)videoBlit(lockedBitmap,0,nullptr);
     Serial.println("[PAUSE][PREVIEW] frame inteiro, NES continua pausado");
   }else if(pressed&0x10){
     const uint8_t count=settings?VIDEO_MENU_TOTAL:PAUSE_MENU_COUNT;
     selection=(uint8_t)((selection+count-1)%count);redraw=true;
   }else if(pressed&0x20){
     const uint8_t count=settings?VIDEO_MENU_TOTAL:PAUSE_MENU_COUNT;
     selection=(uint8_t)((selection+1)%count);redraw=true;
   }else if(!settings && selection==3 && (pressed&(0x40|0x80))){
     const int delta=(pressed&0x80)?1:-1;
     selectedSlot=(uint8_t)((selectedSlot+SAVE_SLOT_COUNT+delta)%SAVE_SLOT_COUNT);
     redraw=true;
   }else if(settings && (pressed&(0x40|0x80))){
     if(selection==0){__atomic_store_n(&nesLcdAction,(uint8_t)3,__ATOMIC_RELEASE);lcdHandleAction();redraw=true;}
     else if(selection==2){int v=HaloAudio::volume()+((pressed&0x80)?5:-5);HaloAudio::setVolume(v);HaloLibrary::setVolume(HaloAudio::volume());redraw=true;}
   }else if(pressed&0x02){
     if(settings){settings=false;selection=6;redraw=true;}
     else resumeGame=true;
   }else if(pressed&0x01){
     if(!settings){
       if(selection==0)resumeGame=true;
       else if(selection==1){
         const int slot=SAVE_SLOT_IDS[selectedSlot];
         const bool ok=HaloSave::save(slot);
         pauseStateStatus("SAVE",slot,ok);redraw=true;
       }else if(selection==2){
         const int slot=SAVE_SLOT_IDS[selectedSlot];
         const bool ok=HaloSave::load(slot);
         pauseStateStatus("LOAD",slot,ok);
         if(ok){loadedState=true;resumeGame=true;}else redraw=true;
       }else if(selection==3){selectedSlot=(selectedSlot+1)%SAVE_SLOT_COUNT;redraw=true;}
       else if(selection==4){HaloSave::setEnabled(!HaloSave::enabled());redraw=true;}
       else if(selection==5)restartGame=true;
       else if(selection==6){settings=true;selection=0;redraw=true;}
       else if(selection==7)quitGame=true;
     }else if(selection==4){settings=false;selection=6;redraw=true;}
     else if(selection==1){HaloAudio::setEnabled(!HaloAudio::enabled());redraw=true;}
     else if(selection==2){HaloAudio::setVolume((HaloAudio::volume()+10)%110);HaloLibrary::setVolume(HaloAudio::volume());redraw=true;}
     else if(selection==3){pauseInspectPixel();redraw=true;}
     else if(selection==0){
       __atomic_store_n(&nesLcdAction,(uint8_t)3,__ATOMIC_RELEASE);
       lcdHandleAction();redraw=true;
     }
   }
   if(redraw)pauseDrawMenu(settings,selection);
   vTaskDelay(10);
 }
  if(quitGame){
    // Save automatico na saida: ultimo progresso persistente antes do menu.
    if(HaloSave::enabled()){
      bool ok=HaloSave::save(9);
      Serial.printf("[R11][AUTO][EXIT] %s\n",ok?"OK":"FAIL");
    }
    __atomic_store_n(&exitRequested,true,__ATOMIC_RELEASE);
    Serial.println("[PAUSE][EXIT] encerramento cooperativo solicitado");
  }
  // Reset ocorre na propria tarefa do NES, depois do frame e com timer suspenso.
  // A ROM e os bancos PRG/CHR ficam carregados; nao ha SD, remount ou ESP.restart.
  // A limpeza impede mostrar o ultimo frame do jogo anterior apos o reset.
  const bool doRestart=restartGame && !__atomic_load_n(&exitRequested,__ATOMIC_ACQUIRE);
  if(doRestart){
    pauseReleaseNesButtons();
    Serial.println("[PAUSE][RESET] iniciando HARD_RESET; ROM preservada");
    nes_reset(HARD_RESET);
    HaloSave::resetAutoTimer();
    videoClear(0);
    tft.fillScreen(TFT_BLACK);
    __atomic_store_n(&producedFrames,(uint32_t)0,__ATOMIC_RELAXED);
    __atomic_store_n(&videoTransferLastUs,(uint32_t)0,__ATOMIC_RELAXED);
    __atomic_store_n(&videoTransferMaxUs,(uint32_t)0,__ATOMIC_RELAXED);
    Serial.println("[PAUSE][RESET][OK] CPU/PPU/APU/mapper reiniciados; aguardando novo frame");
  }
  // Continuar normal restaura o ultimo frame; reiniciar aguarda um novo quadro.
  // Ambos limpam completamente a tela antes de voltar ao jogo.
  if(!quitGame && !doRestart){
    tft.fillScreen(TFT_BLACK);
    if(!loadedState && lockedBitmap && rgbStripe){
     videoBlit(lockedBitmap,0,nullptr);
     Serial.printf("[PAUSE][VIDEO][RESTORED] clear=320x240 aspect=%s frame=OK\n",
                   aspectApplied?"4:3":"NATIVE");
   }else Serial.println("[PAUSE][VIDEO][RESTORED] clear=320x240 frame=WAIT_NEXT");
 }
 if(tickTimer && nesTimerPaused){
   if(esp_timer_start_periodic(tickTimer,nesTimerPeriodUs)!=ESP_OK){
     Serial.println("[PAUSE][TIMER][FAIL] solicitando saida segura");
     __atomic_store_n(&exitRequested,true,__ATOMIC_RELEASE);
   }else Serial.println("[PAUSE][TIMER] retomado");
   nesTimerPaused=false;
 }
 if(!quitGame)HaloAudio::pause(false);
 __atomic_store_n(&pauseWaitForRelease,true,__ATOMIC_RELEASE);
 pauseReleaseNesButtons();
 __atomic_store_n(&pauseMenuActive,false,__ATOMIC_RELEASE);
 if(!quitGame && !__atomic_load_n(&exitRequested,__ATOMIC_ACQUIRE))
   __atomic_add_fetch(&pauseResumeEpoch,1,__ATOMIC_RELEASE);
  Serial.println(quitGame?"[PAUSE][CLOSED] EXIT":(doRestart?"[PAUSE][CLOSED] RESTART":"[PAUSE][CLOSED] CONTINUE"));
}
extern "C" void osd_getinput(void){
 // Recuperacao do ultimo autosave feita na tarefa do nucleo, apos o carregamento.
 // Nao aplica estado de outro jogo porque HaloSave usa hash ROM completo.
 if(launchResumeAuto){
  launchResumeAuto=false;
  bool ok=HaloSave::load(9);
  Serial.printf("[R12][RESUME][AUTO] %s\n",ok?"OK":"NAO ENCONTRADO / FALHA");
 }
 if(__atomic_load_n(&pauseMenuRequested,__ATOMIC_ACQUIRE))pauseRunMenu();
 // AUTO em ponto cooperativo: video ja foi enviado, CPU/PPU nao executam
 // durante SNSS. Interromper clock NES para nao acumular frames no SD lento.
 if(!__atomic_load_n(&exitRequested,__ATOMIC_ACQUIRE) && HaloSave::autoDue() &&
    !__atomic_load_n(&pauseMenuActive,__ATOMIC_ACQUIRE)){
   bool stopped=false;
   if(tickTimer && !nesTimerPaused && esp_timer_stop(tickTimer)==ESP_OK)stopped=true;
   // Se o timer nao puder parar, nao interromper a emulacao para gravar.
   const int done=stopped?HaloSave::saveAuto():0;
   Serial.printf("[R11][AUTO][PERIODIC] %s\n",done==1?"OK":done<0?"FAIL":"SKIP");
   if(stopped && esp_timer_start_periodic(tickTimer,nesTimerPeriodUs)!=ESP_OK){
      Serial.println("[R11][AUTO][TIMER][FAIL] pedindo saida");
      __atomic_store_n(&exitRequested,true,__ATOMIC_RELEASE);
   }
 }
 const uint16_t current=__atomic_load_n(&gamepadMask,__ATOMIC_ACQUIRE);
 const uint16_t change=(uint16_t)(previousPadMask^current);
 // Botoes mapeados aos eventos do nucleo NES; observe NES A/B distintos.
 const int ids[8]={event_joypad1_a,event_joypad1_b,event_joypad1_select,event_joypad1_start,
    event_joypad1_up,event_joypad1_down,event_joypad1_left,event_joypad1_right};
 for(int b=0;b<8;b++){
   uint16_t bit=(uint16_t)(1U<<b);
   if(change&bit){event_t h=event_get(ids[b]);if(h)h((current&bit)?INP_STATE_MAKE:INP_STATE_BREAK);}
 }
 previousPadMask=current;
 // Stack high-water mark (unidade retornada pelo FreeRTOS port ESP-IDF).
 const uint32_t remaining=(uint32_t)uxTaskGetStackHighWaterMark(nullptr);
 uint32_t old=__atomic_load_n(&emulatorStackMinFree,__ATOMIC_RELAXED);
 while(remaining<old && !__atomic_compare_exchange_n(&emulatorStackMinFree,&old,remaining,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
}
extern "C" void osd_getmouse(int *x,int *y,int *button){if(x)*x=0;if(y)*y=0;if(button)*button=0;}
extern "C" void osd_fullname(char *dest,const char *source){
 if(dest&&source){strncpy(dest,source,PATH_MAX-1);dest[PATH_MAX-1]=0;}
}
extern "C" char *osd_newextension(char *name,char *ext){
 if(!name||!ext)return name;
 char *slash=strrchr(name,'/');char *dot=strrchr(name,'.');
 if(dot && (!slash || dot>slash)){
   const size_t prefix=(size_t)(dot-name);
   if(prefix+strlen(ext)<PATH_MAX)strcpy(dot,ext);
 } else if(strlen(name)+strlen(ext)<PATH_MAX)strcat(name,ext);
 return name;
}
extern "C" int osd_makesnapname(char *filename,int len){(void)filename;(void)len;return -1;}
extern "C" void osd_setsound(void (*func)(void *buffer,int size)){HaloAudio::setGenerator(func);}
extern "C" void osd_getsoundinfo(sndinfo_t *info){info->sample_rate=HaloAudio::RATE;info->bps=16;}
