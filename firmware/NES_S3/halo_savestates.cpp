/* HALO NES R11: SNSS per-game persistente, sem alterar a biblioteca Nofrendo.
 * Converte TEMPORARIAMENTE rominfo->filename para uma rota no FAT (/sd/save),
 * exclusivamente enquanto state_save/state_load da Nofrendo sao executados.
 * Nao altera o caminho do VFS /romcache/GAME.NES utilizado para abrir a ROM.
 * Deve ser chamado SOMENTE da tarefa de emulacao (osd_getinput / pausa).
 */
#include "halo_savestates.h"
#include <Arduino.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
extern "C" {
#include <nes/nes.h>
#include <nes/nesstate.h>
}
#ifndef HALO_SAVE_ROOT
#define HALO_SAVE_ROOT "/sd/save"
#endif
namespace HaloSave {
static char key[70]={};
static uint32_t lastAutoAt=0;
static constexpr uint32_t AUTO_INTERVAL_MS=120000u; // a cada 2 min durante gameplay
static bool autoEnabled=true;
static bool haveGame=false;
static int currentSlot=-1;
static uint32_t fnvPath(const char* s){
  uint32_t h=2166136261u;
  if(s)while(*s){ h^=(uint8_t)*s++;h*=16777619u; }
  return h;
}
static void slotName(char* buf,size_t capacity,int slot,const char* variant){
  if(!buf||!capacity)return;
  snprintf(buf,capacity,HALO_SAVE_ROOT "/%s%s.ss%d",key,variant?variant:"",slot);
}
void selectGame(const char* romPath,uint32_t romPayloadHash){
  key[0]=0; haveGame=false; currentSlot=-1;
  if(!romPath||!*romPath)return;
  const char* base=strrchr(romPath,'/');base=base?base+1:romPath;
  char slug[31]={};size_t k=0;
  for(size_t i=0;base[i]&&k<28;i++){
    char c=base[i];
    if(c=='.' && !strcasecmp(base+i,".nes"))break;
    if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'))slug[k++]=c;
    else if(k && slug[k-1]!='_')slug[k++]='_';
  }
  while(k&&slug[k-1]=='_')slug[--k]=0;
  if(!k)strcpy(slug,"ROM");
  snprintf(key,sizeof(key),"%s_%08lX_%08lX",slug,(unsigned long)fnvPath(romPath),(unsigned long)romPayloadHash);
  haveGame=true;lastAutoAt=millis();
  Serial.printf("[R11][SAVE][GAME] %s => /save/%s.ssN\n",romPath,key);
}
const char* gameKey(){return key;}
bool enabled(){return autoEnabled;}
void setEnabled(bool on){autoEnabled=on;lastAutoAt=millis();}
void resetAutoTimer(){lastAutoAt=millis();}
bool autoDue(){return haveGame && autoEnabled && (uint32_t)(millis()-lastAutoAt)>=AUTO_INTERVAL_MS;}
static bool filePresent(const char* file){struct stat st={};return stat(file,&st)==0 && S_ISREG(st.st_mode);}
static uint32_t be32(const uint8_t *d){return ((uint32_t)d[0]<<24)|((uint32_t)d[1]<<16)|((uint32_t)d[2]<<8)|d[3];}
// Checagem ANTES de chamar state_load(): os blocos SNSS tem tamanhos conhecidos.
// Impede que dados truncados ou blocos de tamanho adulterado sejam entregues
// ao carregador antigo do Nofrendo, que aplica blocos durante a leitura.
static bool validSnss(const char* filename){
  FILE* f=fopen(filename,"rb");if(!f)return false;
  bool ok=false;
  do{
    if(fseek(f,0,SEEK_END)!=0)break;
    long n=ftell(f);if(n<100||n>128*1024L||fseek(f,0,SEEK_SET)!=0)break;
    uint8_t head[8];if(fread(head,1,8,f)!=8||memcmp(head,"SNSS",4)!=0)break;
    uint32_t blocks=be32(head+4);
    if(blocks<1||blocks>10)break;
    long pos=8;bool hasBase=false;bool hasMapper=false;uint32_t last=0;
    for(uint32_t j=0;j<blocks;j++){
      uint8_t bh[12];
      if(pos+12>n||fseek(f,pos,SEEK_SET)||fread(bh,1,12,f)!=12){blocks=0;break;}
      const uint32_t len=be32(bh+8);
      if(!len||len>(uint32_t)(n-pos-12)){blocks=0;break;}
      if(!memcmp(bh,"BASR",4)){if(hasBase){blocks=0;break;}hasBase=true;}
      if(!memcmp(bh,"MPRD",4))hasMapper=true;
      // SNSS original: BASR, VRAM, SRAM, SOUN, MPRD (v1).
      if(memcmp(bh,"BASR",4)&&memcmp(bh,"VRAM",4)&&memcmp(bh,"SRAM",4)&&
         memcmp(bh,"SOUN",4)&&memcmp(bh,"MPRD",4)){blocks=0;break;}
      pos+=12+(long)len;last=j+1;
    }
    if(blocks && last==blocks && pos==n && hasBase){ok=true;}
    (void)hasMapper;
  }while(false);
  fclose(f);return ok;
}
static bool makePath(char *dst,size_t cap,int slot,const char* variant){
  if(!haveGame || !dst || slot<0 || slot>9)return false;
  slotName(dst,cap,slot,variant);
  return dst[0] && strlen(dst)<cap-1;
}
// Preserva estados invalidos para pericia; nunca apaga um .bad anterior.
static bool preserveInvalidSave(const char *path){
  struct stat src={};
  if(stat(path,&src)!=0){return errno==ENOENT;}
  if(!S_ISREG(src.st_mode))return false;
  char bad[160];
  for(unsigned i=0;i<10;i++){
    const int n=snprintf(bad,sizeof(bad),"%s.bad%u",path,i);
    if(n<=0||(size_t)n>=sizeof(bad))return false;
    struct stat st={};
    if(stat(bad,&st)==0)continue; // nao sobrescrever sobras antigas
    if(errno!=ENOENT)return false; // EIO/ENODEV: nao tocar nos arquivos
    return rename(path,bad)==0;
  }
  Serial.println("[R13.1][SAVE][PRESERVE] limite de arquivos .bad atingido");
  return false;
}
static bool recoverBackup(int slot){
  char path[128],backup[142],tmp[128];
  if(!makePath(path,sizeof(path),slot,""))return false;
  snprintf(backup,sizeof(backup),"%s.bak",path);
  slotName(tmp,sizeof(tmp),slot,"_TMP");
  if(validSnss(path))return true;
  // Ordem: backup confirmado > temporario completo. Um _TMP parcial nunca
  // deve substituir o estado conhecido bom; SNSS validado antes do rename.
  const char *candidate=nullptr;
  const char *origin=nullptr;
  if(validSnss(backup)){candidate=backup;origin="BAK";}
  else if(validSnss(tmp)){candidate=tmp;origin="TMP";}
  else return false;
  if(!preserveInvalidSave(path))return false;
  if(rename(candidate,path)!=0){
    Serial.printf("[R13.1][SAVE][RECOVER][FAIL] slot=%d origem=%s errno=%d\n",slot,origin,errno);
    return false;
  }
  const bool ok=validSnss(path);
  Serial.printf("[R13.1][SAVE][RECOVER] slot=%d origem=%s valido=%u\n",slot,origin,(unsigned)ok);
  return ok;
}
bool present(int slot){return recoverBackup(slot);}
static bool setTemporaryRomFilename(nes_t* machine,const char *path,char *prior,size_t priorCap){
  if(!machine||!machine->rominfo||!prior||!path)return false;
  if(strlen(path)>=sizeof(machine->rominfo->filename) || priorCap<sizeof(machine->rominfo->filename))return false;
  memcpy(prior,machine->rominfo->filename,sizeof(machine->rominfo->filename));
  strcpy(machine->rominfo->filename,path);
  return true;
}
static void restoreRomFilename(nes_t* machine,const char* prior){
  memcpy(machine->rominfo->filename,prior,sizeof(machine->rominfo->filename));
}
static bool selectNativeSlot(int slot){
  // 0..3 para usuario; 9 reservado ao AUTO. Nofrendo mantem o slot internamente.
  if(slot<0||slot>9)return false;
  state_setslot(slot);currentSlot=slot;
  return true;
}
static bool saveNative(int slot){
  char file[128],pathForCore[128];
  if(!makePath(file,sizeof(file),slot,"_TMP"))return false;
  // state_save substitui extensao de .nes por .ssN na biblioteca.
  snprintf(pathForCore,sizeof(pathForCore),HALO_SAVE_ROOT "/%s_TMP.nes",key);
  nes_t *machine=nes_getcontextptr();char prior[PATH_MAX+1];
  if(!setTemporaryRomFilename(machine,pathForCore,prior,sizeof(prior)))return false;
  selectNativeSlot(slot);
  const int result=state_save();
  restoreRomFilename(machine,prior);
  return result==0;
}
bool save(int slot){
  if(!haveGame||slot<0||slot>9)return false;
  char file[128],tmp[128],bak[144];
  slotName(file,sizeof(file),slot,"");slotName(tmp,sizeof(tmp),slot,"_TMP");
  snprintf(bak,sizeof(bak),"%s.bak",file);
  // Falha fechada: se diretorio /save desapareceu, nao escreve em /romcache.
  struct stat st={};if(stat(HALO_SAVE_ROOT,&st)!=0||!S_ISDIR(st.st_mode)){
    Serial.println("[R11][SAVE][FAIL] pasta /save nao esta acessivel");return false;
  }
  // Ao salvar novamente, recuperar o ultimo backup se a gravacao anterior
  // foi interrompida durante o commit (destino faltando/invalido).
  // Um stat() com EIO/ENODEV nao pode ser interpretado como "nao existe".
  // E um save corrompido sem backup nao deve ser sobrescrito silenciosamente.
  if(!recoverBackup(slot)) {
    struct stat current={};
    errno=0;
    const int rc=stat(file,&current);
    const int e=errno;
    if(rc==0 || e!=ENOENT){
      Serial.printf("[R13.1][SAVE][ABORT] destino existente invalido ou acesso SD falhou slot=%d stat=%d errno=%d\n",slot,rc,e);
      return false;
    }
  }
  // Se sobrou um _TMP completo e nao houve recuperacao, preserva-o no PC.
  // O arquivo incompleto anterior pode ser substituido somente com SD saudavel.
  errno=0;
  if(remove(tmp)!=0 && errno!=ENOENT){
    Serial.printf("[R13.1][SAVE][ABORT] nao pode limpar TMP errno=%d\n",errno);
    return false;
  }
  if(!saveNative(slot)||!validSnss(tmp)){
    const int failure=errno;
    Serial.printf("[R11][SAVE][FAIL] SNSS incompleto ou SD falhou slot=%d errno=%d\n",slot,failure);
    // Nao tentar apagar nada quando FAT sinaliza falha fisica de E/S.
    if(failure!=EIO && failure!=ENODEV && failure!=ENXIO)remove(tmp);
    return false;
  }
  struct stat current={};
  errno=0;
  const bool hasPrimary=(stat(file,&current)==0);
  if(!hasPrimary && errno!=ENOENT){
    Serial.printf("[R13.1][SAVE][ABORT] stat destino errno=%d\n",errno);
    return false;
  }
  if(hasPrimary){
    if(!S_ISREG(current.st_mode) || !validSnss(file)){
      Serial.println("[R13.1][SAVE][ABORT] estado anterior invalido - preservado");
      return false;
    }
    // Backup antigo so e removido DEPOIS de o novo temporario estar validado.
    errno=0;
    if(remove(bak)!=0 && errno!=ENOENT){
      Serial.printf("[R13.1][SAVE][ABORT] nao removeu BAK errno=%d\n",errno);
      return false;
    }
    if(rename(file,bak)!=0){
      Serial.printf("[R11][SAVE][FAIL] backup errno=%d\n",errno);
      return false;
    }
  }
  if(rename(tmp,file)!=0){
    const int e=errno;
    if(!filePresent(file)&&filePresent(bak))rename(bak,file);
    Serial.printf("[R11][SAVE][FAIL] commit errno=%d\n",e);return false;
  }
  lastAutoAt=millis();
  Serial.printf("[R11][SAVE][OK] slot=%d file=/save/%s.ss%d\n",slot,key,slot);
  return true;
}
bool load(int slot){
  if(!haveGame||slot<0||slot>9||!recoverBackup(slot)){
    Serial.printf("[R11][LOAD][FAIL] slot=%d nao existe ou invalido\n",slot);return false;
  }
  char pathForCore[128];snprintf(pathForCore,sizeof(pathForCore),HALO_SAVE_ROOT "/%s.nes",key);
  nes_t *machine=nes_getcontextptr();char prior[PATH_MAX+1];
  if(!setTemporaryRomFilename(machine,pathForCore,prior,sizeof(prior)))return false;
  selectNativeSlot(slot);
  const int result=state_load();
  restoreRomFilename(machine,prior);
  if(result!=0){Serial.printf("[R11][LOAD][FAIL] slot=%d libsnss\n",slot);return false;}
  lastAutoAt=millis();
  Serial.printf("[R11][LOAD][OK] slot=%d ROM preservada\n",slot);
  return true;
}
int saveAuto(){
  if(!autoDue())return 0;
  // Reagendar mesmo em falha evita tempestade de tentativas em SD instavel.
  lastAutoAt=millis();
  return save(9)?1:-1;
}
} // namespace
