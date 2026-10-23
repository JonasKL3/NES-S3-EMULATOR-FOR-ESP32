/* R12 audio: APU da Nofrendo -> PCM mono -> I2S stereo duplicado -> MAX98357A.
 * hook: fim de TODOS os frames emulados no nes.c. Nunca gerar APU em thread paralela.
 * Transmissao usa fila nao bloqueante e tarefa independente; mantem timing da CPU NES.
 */
#include "halo_audio.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>
namespace HaloAudio {
struct Packet {int16_t mono[SAMPLES];};
static QueueHandle_t queue=nullptr;
static TaskHandle_t worker=nullptr;
static bool ready=false;
static bool active=true;
static volatile bool isPaused=false;
static volatile int gain=35;
static volatile uint32_t made=0,dropped=0;
static void (*generator)(void*,int)=nullptr;
static void transmit(void*){
 Packet p; static int16_t stereo[SAMPLES*2];
 for(;;){
  if(xQueueReceive(queue,&p,portMAX_DELAY)!=pdTRUE)continue;
  if(isPaused || !active)continue;
  const int v=__atomic_load_n(&gain,__ATOMIC_RELAXED);
  for(int i=0;i<SAMPLES;i++){
   int32_t x=((int32_t)p.mono[i]*v)/100;
   // Soft clipping against 16-bit overflows (normally already in range).
   if(x>32767)x=32767;else if(x<-32768)x=-32768;
   stereo[2*i]=(int16_t)x;stereo[2*i+1]=(int16_t)x;
  }
  size_t wrote=0;
  esp_err_t r=i2s_write(I2S_NUM_0,stereo,sizeof(stereo),&wrote,pdMS_TO_TICKS(50));
  if(r!=ESP_OK||wrote!=sizeof(stereo))__atomic_add_fetch(&dropped,1,__ATOMIC_RELAXED);
 }
}
bool begin(){
 if(ready)return true;
 i2s_config_t cfg={};
 cfg.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX);
 cfg.sample_rate=RATE;
 cfg.bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT;
 cfg.channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT;
 cfg.communication_format=I2S_COMM_FORMAT_STAND_I2S;
 cfg.intr_alloc_flags=0;
 cfg.dma_buf_count=6;
 cfg.dma_buf_len=256;
 cfg.use_apll=false;
 cfg.tx_desc_auto_clear=true;
 cfg.fixed_mclk=0;
 esp_err_t r=i2s_driver_install(I2S_NUM_0,&cfg,0,nullptr);
 if(r!=ESP_OK){Serial.printf("[R12][AUDIO][FAIL] install err=%d\n",(int)r);return false;}
 i2s_pin_config_t pins={};
 pins.mck_io_num=I2S_PIN_NO_CHANGE; // MAX98357A dispensa MCLK. Nao reservar GPIO0.
 pins.bck_io_num=BCLK;pins.ws_io_num=LRC;pins.data_out_num=DIN;pins.data_in_num=I2S_PIN_NO_CHANGE;
 r=i2s_set_pin(I2S_NUM_0,&pins);
 if(r!=ESP_OK){Serial.printf("[R12][AUDIO][FAIL] pinos err=%d\n",(int)r);i2s_driver_uninstall(I2S_NUM_0);return false;}
 queue=xQueueCreate(8,sizeof(Packet));
 if(!queue){i2s_driver_uninstall(I2S_NUM_0);return false;}
 if(xTaskCreatePinnedToCore(transmit,"NES_I2S",4096,nullptr,2,&worker,1)!=pdPASS){vQueueDelete(queue);queue=nullptr;i2s_driver_uninstall(I2S_NUM_0);return false;}
 ready=true;
 Serial.printf("[R12][AUDIO][I2S] MAX98357A BCLK=%d LRC=%d DIN=%d rate=%d 16bit stereo\n",BCLK,LRC,DIN,RATE);
 return true;
}
void end(){
 // Driver compartilhado entre jogos. Mantem I2S instalado durante o menu, mas silenciado.
 pause(true);generator=nullptr;
}
void setGenerator(void(*fn)(void*,int)){generator=fn;made=0;dropped=0;if(queue)xQueueReset(queue);isPaused=false;}
void frame(){
 // Chamado SOMENTE da tarefa CPU/PPU via patch. Nao pode bloquear.
 if(!generator || !ready || !active || isPaused)return;
 Packet p;generator(p.mono,SAMPLES);
 __atomic_add_fetch(&made,1,__ATOMIC_RELAXED);
 if(ready&&active&&!isPaused&&queue){
  if(xQueueSend(queue,&p,0)!=pdTRUE){
   Packet old; xQueueReceive(queue,&old,0);
   if(xQueueSend(queue,&p,0)!=pdTRUE)__atomic_add_fetch(&dropped,1,__ATOMIC_RELAXED);
  }
 }
}
void pause(bool v){isPaused=v;if(v && queue)xQueueReset(queue);if(v&&ready)i2s_zero_dma_buffer(I2S_NUM_0);}
void setEnabled(bool v){active=v;if(!v)pause(true);else pause(false);}
bool enabled(){return active;}
bool initialized(){return ready;}
void setVolume(int percent){if(percent<0)percent=0;if(percent>100)percent=100;__atomic_store_n(&gain,percent,__ATOMIC_RELAXED);}
int volume(){return __atomic_load_n(&gain,__ATOMIC_RELAXED);}
uint32_t framesProduced(){return __atomic_load_n(&made,__ATOMIC_RELAXED);}
uint32_t framesDropped(){return __atomic_load_n(&dropped,__ATOMIC_RELAXED);}
}
extern "C" void nes_platform_audio_frame(void){HaloAudio::frame();}
