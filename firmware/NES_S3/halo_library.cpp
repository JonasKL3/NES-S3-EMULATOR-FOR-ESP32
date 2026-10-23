/* HALO R12: metadados pequenos e persistentes na pasta /save.
 * Gravacao fora da emulacao (menu); troca por temporario e backup.
 */
#include "halo_library.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#ifndef HALO_LIB_ROOT
#define HALO_LIB_ROOT "/sd/save"
#endif
namespace HaloLibrary {
static constexpr const char *PATH=HALO_LIB_ROOT "/HALO_LIB.DAT";
static constexpr const char *TEMP=HALO_LIB_ROOT "/HALO_LIB.TMP";
static constexpr const char *BACK=HALO_LIB_ROOT "/HALO_LIB.BAK";
static uint32_t fav[500]={};static unsigned count=0;
static uint32_t rec[RECENT_MAX]={};static unsigned rc=0;
static uint32_t last=0;static int vol=35;
uint32_t gameId(const char*s){uint32_t h=2166136261u;if(!s)return 0;while(*s){h^=(uint8_t)*s++;h*=16777619u;}return h? h:1;}
static bool contains(uint32_t h){for(unsigned i=0;i<count;i++)if(fav[i]==h)return true;return false;}
bool isFavorite(uint32_t h){return contains(h);}
uint32_t lastGame(){return last;}
bool hasLast(){return last!=0;}
unsigned recentCount(){return rc;}
uint32_t recent(unsigned i){return i<rc?rec[i]:0;}
int volume(){return vol;}
// Save file uses text records, verified with a trailing FNV hash.
static uint32_t step(uint32_t h,const char *s){while(*s){h=(h^(unsigned char)*s++)*16777619u;}return h;}
static bool parse(const char *p,bool apply=true){
 FILE*f=fopen(p,"rb");if(!f)return false;
 char line[92]={};uint32_t vf[500]={},vr[RECENT_MAX]={},vl=0;int vv=35;unsigned nf=0,nr=0;
 uint32_t hash=2166136261u,expected=0;bool footer=false,ok=false;
 if(!fgets(line,sizeof(line),f)||strcmp(line,"HALO_LIB_V1\n"))goto end;
 hash=step(hash,line);
 while(fgets(line,sizeof(line),f)){
  if(!strncmp(line,"CHECK ",6)){
   char extra[4];unsigned long checksum=0;if(sscanf(line+6,"%lx",&checksum)!=1)break;expected=(uint32_t)checksum;
   footer=fgets(extra,sizeof(extra),f)==nullptr;break;
  }
  hash=step(hash,line);
  unsigned long id=0;int a=0;
  if(sscanf(line,"LAST %lx",&id)==1){vl=(uint32_t)id;continue;}
  if(sscanf(line,"VOL %d",&a)==1){if(a<0||a>100)break;vv=a;continue;}
  if(sscanf(line,"FAV %lx",&id)==1){if(nf>=500)break;vf[nf++]=(uint32_t)id;continue;}
  if(sscanf(line,"REC %lx",&id)==1){if(nr>=RECENT_MAX)break;vr[nr++]=(uint32_t)id;continue;}
  break;
 }
 if(footer&&expected==hash){if(apply){memcpy(fav,vf,nf*sizeof(uint32_t));count=nf;memcpy(rec,vr,nr*sizeof(uint32_t));rc=nr;last=vl;vol=vv;}ok=true;}
end:fclose(f);return ok;
}
bool load(){
 count=rc=0;last=0;vol=35;
 if(parse(PATH)){Serial.printf("[R12][LIB][LOAD] fav=%u recent=%u last=%08lX volume=%d\n",count,rc,(unsigned long)last,vol);return true;}
 if(parse(BACK)){Serial.println("[R12][LIB][BACKUP] recuperado");return true;}
 Serial.println("[R12][LIB] indice ausente ou invalido; inicio vazio");return false;
}
bool save(){
 struct stat st={};if(stat(HALO_LIB_ROOT,&st)!=0||!S_ISDIR(st.st_mode)){Serial.println("[R12][LIB][FAIL] pasta /save ausente");return false;}
 FILE*f=fopen(TEMP,"wb");if(!f){Serial.println("[R12][LIB][FAIL] abrir TMP");return false;}
 uint32_t h=2166136261u;char s[96];
 auto emit=[&](const char*t){h=step(h,t);return fputs(t,f)>=0;};
 bool ok=emit("HALO_LIB_V1\n");
 snprintf(s,sizeof(s),"LAST %08lX\n",(unsigned long)last);ok=ok&&emit(s);
 snprintf(s,sizeof(s),"VOL %d\n",vol);ok=ok&&emit(s);
 for(unsigned i=0;ok&&i<count;i++){snprintf(s,sizeof(s),"FAV %08lX\n",(unsigned long)fav[i]);ok=emit(s);}
 for(unsigned i=0;ok&&i<rc;i++){snprintf(s,sizeof(s),"REC %08lX\n",(unsigned long)rec[i]);ok=emit(s);}
 snprintf(s,sizeof(s),"CHECK %08lX\n",(unsigned long)h);ok=ok&&fputs(s,f)>=0;
 if(fflush(f)!=0)ok=false;
 const int fd=fileno(f);if(fd>=0&&fsync(fd)!=0)Serial.println("[R12][LIB][WARN] fsync nao suportado");
 if(fclose(f)!=0)ok=false;
 if(!ok||!parse(TEMP,false)){remove(TEMP);Serial.println("[R12][LIB][FAIL] TMP nao passou verificacao");return false;}
 // Verificacao TEMP nao modifica preferencias de memoria em uso.
 // Nunca tratar EIO/ENODEV como arquivo ausente e executar rename por cima.
 struct stat mainStat={};errno=0;
 const bool hasMain=(stat(PATH,&mainStat)==0);
 if(!hasMain && errno!=ENOENT){
   Serial.printf("[R13.1][LIB][ABORT] stat principal errno=%d\n",errno);return false;
 }
 if(hasMain){
   // Nao sacrificar o ultimo BACKUP bom quando PATH esta corrompido.
   if(!S_ISREG(mainStat.st_mode) || !parse(PATH,false)){
     Serial.println("[R13.1][LIB][ABORT] principal invalido; restaure BACKUP antes de gravar");
     return false;
   }
   errno=0;
   if(remove(BACK)!=0 && errno!=ENOENT){
     Serial.printf("[R13.1][LIB][ABORT] backup nao removido errno=%d\n",errno);return false;
   }
   if(rename(PATH,BACK)!=0){
     Serial.printf("[R13.1][LIB][FAIL] copia do principal errno=%d\n",errno);return false;
   }
 }
 if(rename(TEMP,PATH)!=0){
   if(hasMain)rename(BACK,PATH);
   Serial.printf("[R12][LIB][FAIL] commit errno=%d\n",errno);return false;
 }
 Serial.printf("[R12][LIB][SAVE] favoritos=%u recentes=%u\n",count,rc);return true;
}
void toggleFavorite(uint32_t id){if(!id)return;for(unsigned i=0;i<count;i++)if(fav[i]==id){for(unsigned j=i+1;j<count;j++)fav[j-1]=fav[j];--count;save();return;}if(count<500){fav[count++]=id;save();}}
void opened(uint32_t id){if(!id)return;last=id;unsigned p=RECENT_MAX;for(unsigned i=0;i<rc;i++)if(rec[i]==id){p=i;break;}if(p<rc){for(unsigned j=p;j>0;j--)rec[j]=rec[j-1];}else{if(rc<RECENT_MAX)++rc;for(unsigned j=rc-1;j>0;j--)rec[j]=rec[j-1];}rec[0]=id;save();}
void setVolume(int v){if(v<0)v=0;if(v>100)v=100;if(v==vol)return;vol=v;save();}
} // namespace
